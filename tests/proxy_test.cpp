// proxy_test - drives f5low.dll through its public API like a game would, with no game: creates a
// window and swap chain through InitFromSwapChainDesc, tags motion/depth, presents two frames whose
// contents come from a graph pack, and expects the DLL to present a generated frame in between.
// With XEFG_CUDA_DUMP and XEFG_CUDA_NOPATCH set the first generated frame is written to a file and
// compared with the pack's expectation.
//
// Usage: proxy_test.exe <f5low.dll> <pack dir>      (pack: 2560x1440 output, 1505x847 motion/depth)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#define XEFG_SWAPCHAIN_EXPORT_API
#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>

static std::vector<uint8_t> Slurp(const std::string& p)
{
    std::ifstream f(p, std::ios::binary);
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_q;
static ID3D12Fence* g_fence;
static UINT64 g_fv;
static HANDLE g_ev;

static void Idle()
{
    g_q->Signal(g_fence, ++g_fv);
    if (g_fence->GetCompletedValue() < g_fv)
    {
        g_fence->SetEventOnCompletion(g_fv, g_ev);
        WaitForSingleObject(g_ev, 30000);
    }
}

static ID3D12Resource* Buffer(D3D12_HEAP_TYPE t, size_t n, D3D12_RESOURCE_STATES st)
{
    D3D12_HEAP_PROPERTIES hp { t };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = n;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

static ID3D12Resource* Texture(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags = D3D12_RESOURCE_FLAG_NONE)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    rd.Flags = flags;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    return r;
}

// Uploads tightly packed texels (row pitch padded to 256) into subresource 0, leaving it in COMMON.
static void Fill(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, const std::vector<uint8_t>& data, UINT texel,
                 D3D12_RESOURCE_STATES from)
{
    const D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ID3D12Resource* up = Buffer(D3D12_HEAP_TYPE_UPLOAD, total, D3D12_RESOURCE_STATE_GENERIC_READ);
    uint8_t* p = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT y = 0; y < d.Height; ++y)
        memcpy(p + y * fp.Footprint.RowPitch, data.data() + static_cast<size_t>(y) * d.Width * texel, static_cast<size_t>(d.Width) * texel);
    up->Unmap(0, nullptr);
    D3D12_RESOURCE_BARRIER b {};
    b.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    b.Transition = { tex, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, from, D3D12_RESOURCE_STATE_COPY_DEST };
    cl->ResourceBarrier(1, &b);
    D3D12_TEXTURE_COPY_LOCATION dst { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, src { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.SubresourceIndex = 0;
    src.PlacedFootprint = fp;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    std::swap(b.Transition.StateBefore, b.Transition.StateAfter);
    cl->ResourceBarrier(1, &b);
    // 'up' is intentionally leaked for the life of this short test.
}

#define LOADFN(name) auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(dll, #name))

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        printf("usage: proxy_test <f5low.dll> <pack dir>\n");
        return 1;
    }
    HMODULE dll = LoadLibraryA(argv[1]);
    if (!dll)
    {
        printf("cannot load %s\n", argv[1]);
        return 1;
    }
    LOADFN(xefgSwapChainD3D12CreateContext);
    LOADFN(xefgSwapChainD3D12BuildPipelines);
    LOADFN(xefgSwapChainD3D12InitFromSwapChainDesc);
    LOADFN(xefgSwapChainD3D12GetSwapChainPtr);
    LOADFN(xefgSwapChainD3D12TagFrameResource);
    LOADFN(xefgSwapChainTagFrameConstants);
    LOADFN(xefgSwapChainSetEnabled);
    LOADFN(xefgSwapChainGetLastPresentStatus);
    LOADFN(xefgSwapChainDestroy);

    const std::string pack = argv[2];
    const auto f0 = Slurp(pack + "\\data\\R1.bin");      // frame_prev
    const auto f1 = Slurp(pack + "\\data\\R0.bin");      // frame_cur
    const auto mvData = Slurp(pack + "\\data\\R78.bin");
    const auto dData = Slurp(pack + "\\data\\R79.bin");
    if (f0.size() != 2560u * 1440 * 4 || f1.size() != f0.size() || mvData.size() != 1505u * 847 * 4 || dData.size() != mvData.size())
    {
        printf("pack data is not the 2560x1440 / 1505x847 case\n");
        return 1;
    }

    // PROXY_TEST_DEBUG=1: the D3D12 debug layer; its errors and warnings are printed at the end (exit code 2).
    char dbgEnv[4] = {};
    const bool debugLayer = GetEnvironmentVariableA("PROXY_TEST_DEBUG", dbgEnv, sizeof(dbgEnv)) && dbgEnv[0] == '1';
    if (debugLayer)
    {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
        else
            printf("debug layer not installed (Graphics Tools)\n");
    }
    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    for (UINT i = 0; !g_dev; ++i)
    {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 ad;
        a->GetDesc1(&ad);
        if (ad.VendorId == 0x10DE && !(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
            D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev));
        a->Release();
    }
    if (!g_dev)
    {
        printf("no NVIDIA D3D12 device\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    xefg_swapchain_handle_t h = nullptr;
    auto r = xefgSwapChainD3D12CreateContext(g_dev, &h);
    printf("CreateContext: %d\n", r);
    r = xefgSwapChainD3D12BuildPipelines(h, nullptr, 1, 0);
    printf("BuildPipelines: %d\n", r);
    if (r != 0)
        return 1;

    WNDCLASSA wc {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "proxy_test";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("proxy_test", "proxy_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 640, 360, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width = 2560;
    sd.Height = 1440;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    // PROXY_TEST_WAITABLE=1: the swap chain has the frame-latency waitable object, and the app waits on it before each
    // frame (with PROXY_TEST_RECREATE also on the old swap chain after Destroy: that wait must not block).
    char wtEnv[4] = {};
    const bool waitable = GetEnvironmentVariableA("PROXY_TEST_WAITABLE", wtEnv, sizeof(wtEnv)) && wtEnv[0] == '1';
    if (waitable)
        sd.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    xefg_swapchain_d3d12_init_params_t ip {};
    ip.maxInterpolatedFrames = 1;
    r = xefgSwapChainD3D12InitFromSwapChainDesc(h, wnd, &sd, nullptr, g_q, fac, &ip);
    printf("InitFromSwapChainDesc: %d\n", r);
    IDXGISwapChain4* sc = nullptr;
    r = xefgSwapChainD3D12GetSwapChainPtr(h, IID_PPV_ARGS(&sc));
    printf("GetSwapChainPtr: %d %p\n", r, static_cast<void*>(sc));
    if (r != 0 || !sc)
        return 1;
    xefgSwapChainSetEnabled(h, 1);

    // PROXY_TEST_FORMATS=wide: the same motion as R16G16B16A16_FLOAT (B and A NaN, so a leak shows) and the depth as
    // R16_UNORM, the formats the DLL converts (xefg_convert.hlsl); the generated frame should match the plain run's.
    // PROXY_TEST_FORMATS=ds: that motion, and the depth in a larger D24_UNORM_S8_UINT depth-stencil texture, tagged as a
    // region at an offset (D3D12 copies such textures only whole; run it with PROXY_TEST_DEBUG=1).
    char fmtEnv[16] = {};
    GetEnvironmentVariableA("PROXY_TEST_FORMATS", fmtEnv, sizeof(fmtEnv));
    const bool ds = !strcmp(fmtEnv, "ds");
    const bool wide = ds || !strcmp(fmtEnv, "wide");
    constexpr UINT dsW = 1600, dsH = 900, dsX = 37, dsY = 21; // the depth-stencil texture and the region's offset in it
    printf("formats: %s\n", ds     ? "motion R16G16B16A16_FLOAT, depth D24_UNORM_S8_UINT region at 37,21 of 1600x900"
                            : wide ? "motion R16G16B16A16_FLOAT, depth R16_UNORM"
                                   : "motion R16G16_FLOAT, depth R32_FLOAT");
    ID3D12Resource* mv = Texture(1505, 847, wide ? DXGI_FORMAT_R16G16B16A16_FLOAT : DXGI_FORMAT_R16G16_FLOAT);
    ID3D12Resource* depth = ds ? Texture(dsW, dsH, DXGI_FORMAT_D24_UNORM_S8_UINT, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL)
                               : Texture(1505, 847, wide ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R32_FLOAT);
    ID3D12CommandAllocator* al = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl));
    if (wide)
    {
        const size_t n = mvData.size() / 4;
        std::vector<uint8_t> mv8(n * 8), d16(n * 2);
        const uint16_t nan[2] = { 0x7E00, 0x7E00 };
        for (size_t i = 0; i < n; ++i)
        {
            memcpy(&mv8[i * 8], &mvData[i * 4], 4);
            memcpy(&mv8[i * 8 + 4], nan, 4);
            float d;
            memcpy(&d, &dData[i * 4], 4);
            const uint16_t u = static_cast<uint16_t>(std::lround(std::clamp(d, 0.0f, 1.0f) * 65535.0f));
            memcpy(&d16[i * 2], &u, 2);
        }
        Fill(cl, mv, mv8, 8, D3D12_RESOURCE_STATE_COMMON);
        if (ds)
        {
            // Plane 0 of D24S8: depth in the low 24 bits; outside the region a pattern that shows if the offset is wrong.
            std::vector<uint8_t> d24(static_cast<size_t>(dsW) * dsH * 4);
            for (UINT y = 0; y < dsH; ++y)
                for (UINT x = 0; x < dsW; ++x)
                {
                    uint32_t v = 0x00F0F0F0u;
                    if (x >= dsX && y >= dsY && x - dsX < 1505 && y - dsY < 847)
                    {
                        float d;
                        memcpy(&d, &dData[(static_cast<size_t>(y - dsY) * 1505 + (x - dsX)) * 4], 4);
                        v = static_cast<uint32_t>(std::lround(std::clamp(d, 0.0f, 1.0f) * 16777215.0));
                    }
                    memcpy(&d24[(static_cast<size_t>(y) * dsW + x) * 4], &v, 4);
                }
            Fill(cl, depth, d24, 4, D3D12_RESOURCE_STATE_COMMON);
        }
        else
            Fill(cl, depth, d16, 2, D3D12_RESOURCE_STATE_COMMON);
    }
    else
    {
        Fill(cl, mv, mvData, 4, D3D12_RESOURCE_STATE_COMMON);
        Fill(cl, depth, dData, 4, D3D12_RESOURCE_STATE_COMMON);
    }
    cl->Close();
    ID3D12CommandList* ls[] = { cl };
    g_q->ExecuteCommandLists(1, ls);
    Idle();

    xefg_swapchain_frame_constant_data_t fc {};
    for (int frame = 0; frame < 4; ++frame)
    {
        // Frames 0 and 1 carry the pack's prev and cur images; later ones repeat cur.
        const auto& img = frame == 0 ? f0 : f1;
        ID3D12Resource* bb = nullptr;
        sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
        al->Reset();
        cl->Reset(al, nullptr);
        Fill(cl, bb, img, 4, D3D12_RESOURCE_STATE_PRESENT);
        xefg_swapchain_d3d12_resource_data_t rd {};
        rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
        rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
        rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        rd.pResource = mv;
        xefgSwapChainD3D12TagFrameResource(h, cl, frame, &rd);
        rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
        rd.pResource = depth;
        if (ds)
        {
            rd.resourceBase = { dsX, dsY };
            rd.resourceSize = { 1505, 847 };
        }
        xefgSwapChainD3D12TagFrameResource(h, cl, frame, &rd);
        cl->Close();
        g_q->ExecuteCommandLists(1, ls);
        xefgSwapChainTagFrameConstants(h, frame, &fc);
        const HRESULT hr = sc->Present(0, 0);
        Idle();
        bb->Release();
        xefg_swapchain_present_status_t st {};
        xefgSwapChainGetLastPresentStatus(h, &st);
        printf("frame %d: Present 0x%08lx, presented so far %u, last frame generated %u\n", frame, static_cast<unsigned long>(hr),
               st.framesPresented, st.isFrameGenEnabled);
    }
    // PROXY_TEST_RECREATE=1: what a game does that makes a new swap chain for its window before it releases the old
    // one: OptiScaler destroys the context first, then makes a new one on the same window. The old swap chain is
    // still held, and a window takes one flip-model swap chain at a time.
    char rcEnv[4] = {};
    bool recreateFailed = false;
    if (GetEnvironmentVariableA("PROXY_TEST_RECREATE", rcEnv, sizeof(rcEnv)) && rcEnv[0] == '1')
    {
        HANDLE oldWait = waitable ? sc->GetFrameLatencyWaitableObject() : nullptr;
        xefgSwapChainDestroy(h);
        if (oldWait)
        {
            // A game thread still pacing on the old swap chain: a failing Present, then its usual wait.
            const HRESULT ph = sc->Present(0, 0);
            DWORD w = WAIT_OBJECT_0;
            for (int i = 0; i < 40 && w == WAIT_OBJECT_0; ++i)
            {
                sc->Present(0, 0);
                w = WaitForSingleObject(oldWait, 1000);
            }
            printf("old swap chain after Destroy: Present 0x%08lx, 40 waits on its latency handle %s\n",
                   static_cast<unsigned long>(ph), w == WAIT_OBJECT_0 ? "returned" : "BLOCKED");
            recreateFailed = recreateFailed || w != WAIT_OBJECT_0;
            CloseHandle(oldWait);
        }
        DXGI_SWAP_CHAIN_DESC1 od {};
        printf("old swap chain after Destroy: GetDesc1 0x%08lx (expected DXGI_ERROR_INVALID_CALL 0x887a0001)\n",
               static_cast<unsigned long>(sc->GetDesc1(&od)));
        xefg_swapchain_handle_t h2 = nullptr;
        xefgSwapChainD3D12CreateContext(g_dev, &h2);
        xefgSwapChainD3D12BuildPipelines(h2, nullptr, 1, 0);
        r = xefgSwapChainD3D12InitFromSwapChainDesc(h2, wnd, &sd, nullptr, g_q, fac, &ip);
        printf("re-created on the same window while the old swap chain is held: %d\n", r);
        IDXGISwapChain4* sc2 = nullptr;
        if (r == 0)
            xefgSwapChainD3D12GetSwapChainPtr(h2, IID_PPV_ARGS(&sc2));
        recreateFailed = recreateFailed || r != 0 || !sc2;
        if (sc2)
        {
            xefgSwapChainSetEnabled(h2, 1);
            for (int frame = 0; frame < 3; ++frame)
            {
                ID3D12Resource* bb = nullptr;
                sc2->GetBuffer(sc2->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
                al->Reset();
                cl->Reset(al, nullptr);
                Fill(cl, bb, frame == 0 ? f0 : f1, 4, D3D12_RESOURCE_STATE_PRESENT);
                xefg_swapchain_d3d12_resource_data_t rd {};
                rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
                rd.incomingState = D3D12_RESOURCE_STATE_COMMON;
                rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
                rd.pResource = mv;
                xefgSwapChainD3D12TagFrameResource(h2, cl, 10 + frame, &rd);
                rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
                rd.pResource = depth;
                if (ds)
                {
                    rd.resourceBase = { dsX, dsY };
                    rd.resourceSize = { 1505, 847 };
                }
                xefgSwapChainD3D12TagFrameResource(h2, cl, 10 + frame, &rd);
                cl->Close();
                g_q->ExecuteCommandLists(1, ls);
                xefgSwapChainTagFrameConstants(h2, 10 + frame, &fc);
                const HRESULT hr = sc2->Present(0, 0);
                Idle();
                bb->Release();
                xefg_swapchain_present_status_t st {};
                xefgSwapChainGetLastPresentStatus(h2, &st);
                printf("new swap chain frame %d: Present 0x%08lx, last frame generated %u\n", frame, static_cast<unsigned long>(hr),
                       st.isFrameGenEnabled);
                recreateFailed = recreateFailed || FAILED(hr);
            }
        }
        sc->Release(); // the game lets go of the old one only now
        if (sc2)
            sc2->Release();
        xefgSwapChainDestroy(h2);
        printf("recreate: %s\n", recreateFailed ? "FAIL" : "PASS");
    }
    else
    {
        sc->Release();
        xefgSwapChainDestroy(h);
    }
    int debugErrors = 0;
    if (ID3D12InfoQueue* iq = nullptr; debugLayer && SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&iq))))
    {
        for (UINT64 i = 0, n = iq->GetNumStoredMessages(); i < n; ++i)
        {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING)
            {
                ++debugErrors;
                printf("debug layer: %s\n", m->pDescription);
            }
        }
        printf("debug layer: %d error(s) / warning(s)\n", debugErrors);
        iq->Release();
    }
    return recreateFailed ? 3 : debugErrors ? 2 : 0;
}
