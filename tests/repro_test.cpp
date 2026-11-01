// repro_test - replays what the game in tester log 20 did through the XeSS-FG API (a device removed, reason
// DXGI_ERROR_INVALID_CALL, right after frame generation was turned on), against any libxess_fg-compatible DLL: ours
// or Intel's. No pack data needed; the pictures are a moving gradient.
//
// The game, from the log: a 1920x1080 R8G8B8A8_UNORM swap chain, 3 buffers, FLIP_SEQUENTIAL, waitable object +
// tearing (flags 0x840), init flags 0x1 (inverted depth); thousands of frames with frame generation off, presented
// with ALLOW_TEARING; then on, with motion R16G16_TYPELESS and depth R32G8X24_TYPELESS (depth-stencil) at 1280x720,
// both in NON_PIXEL_SHADER_RESOURCE (0x40), the most interpolated frames the DLL takes.
//
// Usage: repro_test.exe <xefg dll> [libxell.dll]     (Intel's XeFG wants XeLL; ours ignores it)
//   REPRO_DEBUG=1   the D3D12 debug layer (its errors and warnings are printed; exit code 2)
//   REPRO_HDR=1     an R10G10B10A2 swap chain (the own generator then uses the pack's hdr\ part)
//   REPRO_INITFLAGS=n  XeFG init flags (default 1 = inverted depth; 0 runs the pack's non-inverted-depth module variants)
//   REPRO_W / REPRO_H  output (back buffer) size (default 1920x1080; with REPRO_RW / REPRO_RH for the render size)
//   REPRO_OFF=N     frames before frame generation is turned on (default 120)
//   REPRO_ON=N      frames with it on (default 240)
//   REPRO_LATE_TAG=1  tag with the frame's list already closed and submitted (Streamline's eValidUntilPresent tags
//                   may come so; valid until present, the list must not be used)
//   REPRO_GBV=1     with REPRO_DEBUG: GPU-based validation too (resource states and descriptors checked on the GPU)
//   REPRO_NOIDLE=1  the GPU is not drained each frame (3 frames in flight, as a game)
//   REPRO_TOGGLE=1  as OptiScaler in tester round 2: the first frame on tags (the rebuild), then SetNumInterpolatedFrames
//                   and SetEnabled 0 before its Present, on again 10 frames later
//   REPRO_RW / REPRO_RH  render size of motion and depth (default 1280x720; round 2: 1136x640)
//   REPRO_REMOVE=1  removes the device 5 frames after frame generation is on (checks the DLL's Dred=1 log dump)
//   REPRO_CAP=fps   (with libxell) XeLL's cap as OptiScaler sets it with XeFG (Method::XellWithXefg: the shown-frame cap,
//                   R14), xellSleep at every frame start (R8); prints rendered and shown frames per second while on
//   REPRO_OPTIFIX=1 with REPRO_CAP: as OptiScaler with XL23 fixed - a DLL whose xefgCudaXellCountsRenderedOnly says 1 gets
//                   XeLL's cap per rendered frame (cap / (1 + generated)) once frame generation is on
//   REPRO_INTERP=n  interpolated frames per rendered one (default: the library's maximum)
//   REPRO_INTERP2=n switch to n halfway through the frames on
//   REPRO_RESIZEBUF=1  ResizeBuffers right after the first Present at the new render size (while the DLL's rebuild for it
//                   may still be pending), and again 3 frames later
// Exit code: 0 ok, 1 setup failed, 2 debug-layer messages, 3 a Present failed / the device was removed.

#include <windows.h>
#include <shellapi.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_q;
static ID3D12Fence* g_fence;
static UINT64 g_fv;
static HANDLE g_ev;
static std::vector<ID3D12Resource*> g_uploads; // kept until the GPU is idle

static void Idle()
{
    g_q->Signal(g_fence, ++g_fv);
    if (g_fence->GetCompletedValue() < g_fv)
    {
        g_fence->SetEventOnCompletion(g_fv, g_ev);
        WaitForSingleObject(g_ev, 10000);
    }
    for (ID3D12Resource* u : g_uploads)
        u->Release();
    g_uploads.clear();
}

static ID3D12Resource* Upload(size_t n)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_UPLOAD };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = n;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&r));
    g_uploads.push_back(r);
    return r;
}

static ID3D12Resource* Texture(UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS flags)
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

static void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
    cl->ResourceBarrier(1, &br);
}

// Writes subresource 0 (plane 0 of a depth-stencil texture) from `texel(x, y, dst)`, from state `from` to `to`.
template <class F>
static void Fill(ID3D12GraphicsCommandList* cl, ID3D12Resource* tex, UINT texelBytes, D3D12_RESOURCE_STATES from,
                 D3D12_RESOURCE_STATES to, F texel)
{
    const D3D12_RESOURCE_DESC d = tex->GetDesc();
    D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
    UINT64 total = 0;
    g_dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
    ID3D12Resource* up = Upload(total);
    uint8_t* p = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT y = 0; y < d.Height; ++y)
        for (UINT x = 0; x < d.Width; ++x)
            texel(x, y, p + y * fp.Footprint.RowPitch + x * texelBytes);
    up->Unmap(0, nullptr);
    Transition(cl, tex, from, D3D12_RESOURCE_STATE_COPY_DEST);
    D3D12_TEXTURE_COPY_LOCATION dst { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, src { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    dst.SubresourceIndex = 0;
    src.PlacedFootprint = fp;
    cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
    Transition(cl, tex, D3D12_RESOURCE_STATE_COPY_DEST, to);
}

static uint16_t Half(float f)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int e = static_cast<int>((x >> 23) & 0xFF) - 112;
    if (e <= 0)
        return static_cast<uint16_t>(sign);
    return static_cast<uint16_t>(sign | (static_cast<uint32_t>(e) << 10) | ((x & 0x7FFFFFu) >> 13));
}

static int EnvInt(const char* name, int def)
{
    char v[16] = {};
    return GetEnvironmentVariableA(name, v, sizeof(v)) ? atoi(v) : def;
}

// After a device removal: the command lists the GPU had not finished, with the operations around the last one that
// completed, and the page fault if there was one.
static void DumpDred()
{
    ID3D12DeviceRemovedExtendedData* dred = nullptr;
    if (FAILED(g_dev->QueryInterface(IID_PPV_ARGS(&dred))))
    {
        printf("DRED: not available\n");
        return;
    }
    D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc {};
    if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
    {
        int lists = 0;
        for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && lists < 8; n = n->pNext)
        {
            const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
            if (done >= n->BreadcrumbCount)
                continue; // finished
            ++lists;
            printf("DRED: list %p (%ls) on queue %p (%ls): %u of %u operations done\n", static_cast<const void*>(n->pCommandList),
                   n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"unnamed", static_cast<const void*>(n->pCommandQueue),
                   n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"unnamed", done, n->BreadcrumbCount);
            const UINT from = done > 4 ? done - 4 : 0, to = done + 4 < n->BreadcrumbCount ? done + 4 : n->BreadcrumbCount;
            for (UINT i = from; i < to; ++i)
                printf("DRED:   %s op %u: %d\n", i == done ? ">>" : "  ", i, static_cast<int>(n->pCommandHistory[i]));
        }
        if (!lists)
            printf("DRED: every recorded command list had finished\n");
    }
    D3D12_DRED_PAGE_FAULT_OUTPUT pf {};
    if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
    {
        printf("DRED: page fault at GPU address 0x%llx\n", static_cast<unsigned long long>(pf.PageFaultVA));
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a; a = a->pNext)
            printf("DRED:   live allocation there: %ls (type %d)\n", a->ObjectNameW ? a->ObjectNameW : L"unnamed",
                   static_cast<int>(a->AllocationType));
        for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a; a = a->pNext)
            printf("DRED:   recently freed there: %ls (type %d)\n", a->ObjectNameW ? a->ObjectNameW : L"unnamed",
                   static_cast<int>(a->AllocationType));
    }
    else
        printf("DRED: no page fault recorded\n");
    dred->Release();
}

static void XefgLog(const char* message, xefg_swapchain_logging_level_t level, void*)
{
    printf("  [dll %d] %s\n", static_cast<int>(level), message);
}

#define LOADFN(m, name) auto name = reinterpret_cast<decltype(&::name)>(GetProcAddress(m, #name))

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // every line reaches the file even when a hang is killed
    if (argc < 2)
    {
        printf("usage: repro_test <xefg dll> [libxell.dll]\n");
        return 1;
    }
    const bool debugLayer = EnvInt("REPRO_DEBUG", 0) == 1;
    const int framesOff = EnvInt("REPRO_OFF", 120), framesOn = EnvInt("REPRO_ON", 240);
    const bool lateTag = EnvInt("REPRO_LATE_TAG", 0) == 1;
    const bool gbv = EnvInt("REPRO_GBV", 0) == 1, noIdle = EnvInt("REPRO_NOIDLE", 0) == 1, toggle = EnvInt("REPRO_TOGGLE", 0) == 1;
    const bool removeDevice = EnvInt("REPRO_REMOVE", 0) == 1, resizeBuf = EnvInt("REPRO_RESIZEBUF", 0) == 1;
    const UINT rw = static_cast<UINT>(EnvInt("REPRO_RW", 1280)), rh = static_cast<UINT>(EnvInt("REPRO_RH", 720));
    if (debugLayer)
    {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        {
            dbg->EnableDebugLayer();
            if (ID3D12Debug1* d1 = nullptr; gbv && SUCCEEDED(dbg->QueryInterface(IID_PPV_ARGS(&d1))))
            {
                d1->SetEnableGPUBasedValidation(TRUE);
                d1->Release();
            }
            dbg->Release();
        }
        else
            printf("debug layer not installed (Windows optional feature \"Graphics Tools\")\n");
    }
    // DRED: on a device removal, which GPU operation was running in which list, and any page fault (DumpDred)
    {
        ID3D12DeviceRemovedExtendedDataSettings* dred = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dred))))
        {
            dred->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
            dred->Release();
        }
    }

    // the DLL path from the wide command line: a folder name outside the ANSI code page must reach LoadLibrary unchanged
    int wargc = 0;
    wchar_t** wargv = CommandLineToArgvW(GetCommandLineW(), &wargc);
    HMODULE dll = wargv && wargc >= 2 ? LoadLibraryW(wargv[1]) : LoadLibraryA(argv[1]);
    if (!dll)
    {
        printf("cannot load %s\n", argv[1]);
        return 1;
    }
    LOADFN(dll, xefgSwapChainD3D12CreateContext);
    LOADFN(dll, xefgSwapChainD3D12BuildPipelines);
    LOADFN(dll, xefgSwapChainD3D12InitFromSwapChainDesc);
    LOADFN(dll, xefgSwapChainD3D12GetSwapChainPtr);
    LOADFN(dll, xefgSwapChainD3D12TagFrameResource);
    LOADFN(dll, xefgSwapChainTagFrameConstants);
    LOADFN(dll, xefgSwapChainSetEnabled);
    LOADFN(dll, xefgSwapChainSetNumInterpolatedFrames);
    LOADFN(dll, xefgSwapChainGetProperties);
    LOADFN(dll, xefgSwapChainGetLastPresentStatus);
    LOADFN(dll, xefgSwapChainSetLoggingCallback);
    LOADFN(dll, xefgSwapChainSetLatencyReduction);
    LOADFN(dll, xefgSwapChainSetPresentId);
    LOADFN(dll, xefgSwapChainDestroy);

    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    for (UINT i = 0; !g_dev; ++i)
    {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 ad;
        a->GetDesc1(&ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev));
            if (g_dev)
                printf("adapter: %ls\n", ad.Description);
        }
        a->Release();
    }
    if (!g_dev)
    {
        printf("no D3D12 device\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_fence));
    g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    xefg_swapchain_handle_t h = nullptr;
    printf("CreateContext: %d\n", xefgSwapChainD3D12CreateContext(g_dev, &h));
    if (!h)
        return 1;
    xefgSwapChainSetLoggingCallback(h, XEFG_SWAPCHAIN_LOGGING_LEVEL_WARNING, XefgLog, nullptr);

    // XeLL, as OptiScaler hands it over (Intel's library wants it; ours ignores it)
    using PFN_xellD3D12CreateContext = int(__cdecl*)(ID3D12Device*, void**);
    using PFN_xellAddMarkerData = int(__cdecl*)(void*, uint32_t, int);
    void* xell = nullptr;
    PFN_xellAddMarkerData xellMarker = nullptr;
    int(__cdecl* xellSleepFn)(void*, uint32_t) = nullptr;
    const int capFps = EnvInt("REPRO_CAP", 0);
    int(__cdecl* xellSetSleep)(void*, const uint32_t*) = nullptr;
    auto countsRenderedOnly = reinterpret_cast<uint32_t (*)()>(GetProcAddress(dll, "xefgCudaXellCountsRenderedOnly"));
    const bool optiFix = EnvInt("REPRO_OPTIFIX", 0) == 1;
    if (argc >= 3)
    {
        if (HMODULE xm = LoadLibraryA(argv[2]))
        {
            auto create = reinterpret_cast<PFN_xellD3D12CreateContext>(GetProcAddress(xm, "xellD3D12CreateContext"));
            auto sleepMode = reinterpret_cast<int(__cdecl*)(void*, const uint32_t*)>(GetProcAddress(xm, "xellSetSleepMode"));
            xellSetSleep = sleepMode;
            xellMarker = reinterpret_cast<PFN_xellAddMarkerData>(GetProcAddress(xm, "xellAddMarkerData"));
            if (capFps > 0)
                xellSleepFn = reinterpret_cast<int(__cdecl*)(void*, uint32_t)>(GetProcAddress(xm, "xellSleep"));
            if (create && create(g_dev, &xell) == 0 && xell)
            {
                // xell_sleep_params_t: minimumIntervalUs, then bLowLatencyMode in bit 0 (Intel's XeFG refuses to turn on without it)
                const uint32_t params[2] = { capFps > 0 ? 1000000u / static_cast<uint32_t>(capFps) : 0u, 1u };
                printf("xellSetSleepMode low latency: %d\n", sleepMode ? sleepMode(xell, params) : -1);
                printf("SetLatencyReduction (XeLL): %d\n", xefgSwapChainSetLatencyReduction(h, xell));
            }
        }
        else
            printf("cannot load %s\n", argv[2]);
    }

    // REPRO_INITFLAGS=n: other init flags (0 = depth not inverted: the pack's other flag-set modules run)
    const uint32_t initFlags = static_cast<uint32_t>(EnvInt("REPRO_INITFLAGS", XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH)); // the game's 0x1
    printf("BuildPipelines: %d\n", xefgSwapChainD3D12BuildPipelines(h, nullptr, 1, initFlags));

    WNDCLASSA wc {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "repro_test";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("repro_test", "repro_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width = static_cast<UINT>(EnvInt("REPRO_W", 1920));     // REPRO_W / REPRO_H: the output (back buffer) size
    sd.Height = static_cast<UINT>(EnvInt("REPRO_H", 1080));
    sd.Format = EnvInt("REPRO_HDR", 0) == 1 ? DXGI_FORMAT_R10G10B10A2_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_SEQUENTIAL;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT | DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING; // 0x840
    xefg_swapchain_d3d12_init_params_t ip {};
    ip.initFlags = initFlags;
    ip.maxInterpolatedFrames = XEFG_SWAPCHAIN_USE_MAX_SUPPORTED_INTERPOLATED_FRAMES;
    auto r = xefgSwapChainD3D12InitFromSwapChainDesc(h, wnd, &sd, nullptr, g_q, fac, &ip);
    printf("InitFromSwapChainDesc %ux%u 3 buffers FLIP_SEQUENTIAL flags 0x840: %d\n", sd.Width, sd.Height, r);
    IDXGISwapChain4* sc = nullptr;
    if (r != 0 || xefgSwapChainD3D12GetSwapChainPtr(h, IID_PPV_ARGS(&sc)) != 0 || !sc)
        return 1;
    xefg_swapchain_properties_t props {};
    xefgSwapChainGetProperties(h, &props);
    // REPRO_INTERP=n: that many interpolated frames (default the library's maximum)
    const uint32_t libMax = props.maxSupportedInterpolations ? props.maxSupportedInterpolations : 1;
    const uint32_t maxInterp = EnvInt("REPRO_INTERP", 0) > 0 ? std::min<uint32_t>(EnvInt("REPRO_INTERP", 0), libMax) : libMax;
    // REPRO_INTERP2=n: switch to n interpolated frames halfway through the frames on (the cap line then covers the second half)
    const uint32_t interp2 = EnvInt("REPRO_INTERP2", 0) > 0 ? std::min<uint32_t>(EnvInt("REPRO_INTERP2", 0), libMax) : 0;
    printf("max interpolated frames: %u\n", maxInterp);
    HANDLE wait = sc->GetFrameLatencyWaitableObject();

    // The game's inputs: motion R16G16_TYPELESS, depth R32G8X24_TYPELESS (depth-stencil), 1280x720, in
    // PIXEL_SHADER_RESOURCE when tagged
    ID3D12Resource* mv = Texture(rw, rh, DXGI_FORMAT_R16G16_TYPELESS, D3D12_RESOURCE_FLAG_NONE);
    ID3D12Resource* depth = Texture(rw, rh, DXGI_FORMAT_R32G8X24_TYPELESS, D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL);
    ID3D12CommandAllocator* al = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl));
    ID3D12CommandList* ls[] = { cl };
    Fill(cl, mv, 4, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, [](UINT, UINT, uint8_t* t) {
        const uint16_t v[2] = { Half(-4.0f), Half(0.0f) }; // a steady pan, 4 px per frame
        memcpy(t, v, 4);
    });
    Fill(cl, depth, 4, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, [rw](UINT x, UINT, uint8_t* t) {
        const float d = 0.1f + 0.8f * static_cast<float>(x) / static_cast<float>(rw);
        memcpy(t, &d, 4);
    });
    cl->Close();
    g_q->ExecuteCommandLists(1, ls);
    Idle();

    // REPRO_NOIDLE: one allocator per frame in flight, its fence value and the uploads its list reads.
    ID3D12CommandAllocator* ring[3] = { al, nullptr, nullptr };
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ring[1]));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&ring[2]));
    UINT64 ringFence[3] {};
    std::vector<ID3D12Resource*> ringUploads[3];
    int failures = 0;
    xefg_swapchain_frame_constant_data_t fc {};
    fc.motionVectorScaleX = fc.motionVectorScaleY = 1.0f;
    fc.frameRenderTime = 8.0f;
    for (int frame = 0; frame < framesOff + framesOn && !failures; ++frame)
    {
        const bool on = frame >= framesOff;
        // REPRO_TOGGLE: off again on the first frame, back on 10 frames later (OptiScaler's UpdateTarget), tags only while on.
        const bool toggledOff = toggle && frame > framesOff && frame < framesOff + 10;
        if (frame == framesOff || (toggle && frame == framesOff + 10))
            printf("frame %d: SetEnabled 1: %d\n", frame, xefgSwapChainSetEnabled(h, 1));
        if (wait)
            WaitForSingleObject(wait, 1000);
        if (xellSleepFn)
            xellSleepFn(xell, static_cast<uint32_t>(frame));
        if (xellMarker)
        {
            xellMarker(xell, static_cast<uint32_t>(frame), 0); // simulation start
            xellMarker(xell, static_cast<uint32_t>(frame), 1);
            xellMarker(xell, static_cast<uint32_t>(frame), 2); // render submit start
        }
        ID3D12Resource* bb = nullptr;
        sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
        const int ri = frame % 3;
        if (noIdle)
        {
            if (g_fence->GetCompletedValue() < ringFence[ri])
            {
                g_fence->SetEventOnCompletion(ringFence[ri], g_ev);
                WaitForSingleObject(g_ev, 10000);
            }
            for (ID3D12Resource* u : ringUploads[ri])
                u->Release();
            ringUploads[ri].clear();
        }
        ID3D12CommandAllocator* fa = noIdle ? ring[ri] : al;
        fa->Reset();
        cl->Reset(fa, nullptr);
        Fill(cl, bb, 4, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, [frame](UINT x, UINT y, uint8_t* t) {
            const UINT s = x + static_cast<UINT>(frame) * 4;
            t[0] = static_cast<uint8_t>(s);
            t[1] = static_cast<uint8_t>(y);
            t[2] = static_cast<uint8_t>((s >> 3) ^ (y >> 3));
            t[3] = 255;
        });
        // How long each call takes around the switch on (frames framesOff - 1 .. framesOff + 3): where a DLL blocks.
        const bool timeIt = frame >= framesOff - 1 && frame <= framesOff + 3;
        auto ms = [](LARGE_INTEGER a) {
            LARGE_INTEGER b, f;
            QueryPerformanceCounter(&b);
            QueryPerformanceFrequency(&f);
            return static_cast<double>(b.QuadPart - a.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        };
        auto tag = [&] {
            LARGE_INTEGER t0;
            QueryPerformanceCounter(&t0);
            xefg_swapchain_d3d12_resource_data_t rd {};
            rd.validity = XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT;
            rd.incomingState = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE; // 0x40, as in the log
            rd.resourceSize = { rw, rh };
            rd.type = XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
            rd.pResource = mv;
            xefgSwapChainD3D12TagFrameResource(h, cl, static_cast<uint32_t>(frame), &rd);
            rd.type = XEFG_SWAPCHAIN_RES_DEPTH;
            rd.pResource = depth;
            xefgSwapChainD3D12TagFrameResource(h, cl, static_cast<uint32_t>(frame), &rd);
            if (timeIt)
                printf("frame %d: tags took %.2f ms\n", frame, ms(t0));
        };
        if (on && !toggledOff && !lateTag)
            tag();
        cl->Close();
        g_q->ExecuteCommandLists(1, ls);
        if (noIdle)
        {
            g_q->Signal(g_fence, ++g_fv);
            ringFence[ri] = g_fv;
            ringUploads[ri].swap(g_uploads);
        }
        if (on && !toggledOff && lateTag)
            tag();
        if (xellMarker)
            xellMarker(xell, static_cast<uint32_t>(frame), 3); // render submit end
        if (xefgSwapChainSetPresentId)
            xefgSwapChainSetPresentId(h, static_cast<uint32_t>(frame));
        xefgSwapChainTagFrameConstants(h, static_cast<uint32_t>(frame), &fc);
        if (frame == framesOff)
            printf("frame %d: SetNumInterpolatedFrames %u: %d\n", frame, maxInterp, xefgSwapChainSetNumInterpolatedFrames(h, maxInterp));
        if (frame == framesOff + framesOn / 2 && interp2 > 0)
        {
            printf("frame %d: SetNumInterpolatedFrames %u (halfway): %d\n", frame, interp2, xefgSwapChainSetNumInterpolatedFrames(h, interp2));
            const uint32_t rendered = countsRenderedOnly ? countsRenderedOnly() : 0;
            printf("frame %d: xefgCudaXellCountsRenderedOnly: %u\n", frame, rendered);
            if (capFps > 0 && xellSetSleep && optiFix)
            {
                // As OptiScaler, which reads the export each present: the shown cap, or per rendered frame
                const uint32_t params[2] = { static_cast<uint32_t>(1000000.0 * (rendered ? 1 + interp2 : 1) / capFps + 0.5), 1u };
                printf("frame %d: XeLL cap %u us: %d\n", frame, params[0], xellSetSleep(xell, params));
            }
        }
        if (frame == framesOff && capFps > 0 && xellSetSleep)
        {
            const uint32_t rendered = countsRenderedOnly ? countsRenderedOnly() : 0;
            printf("frame %d: xefgCudaXellCountsRenderedOnly %s: %u\n", frame, countsRenderedOnly ? "exported" : "not exported", rendered);
            if (optiFix && rendered)
            {
                const uint32_t params[2] = { static_cast<uint32_t>(1000000.0 * (1 + maxInterp) / capFps + 0.5), 1u };
                printf("frame %d: XeLL cap per rendered frame, %u us: %d\n", frame, params[0], xellSetSleep(xell, params));
            }
        }
        if (toggle && frame == framesOff)
            printf("frame %d: SetEnabled 0 before its Present (OptiScaler): %d\n", frame, xefgSwapChainSetEnabled(h, 0));
        if (xellMarker)
            xellMarker(xell, static_cast<uint32_t>(frame), 4); // present start
        if (removeDevice && frame == framesOff + 5)
            if (ID3D12Device5* d5 = nullptr; SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&d5))))
            {
                printf("frame %d: RemoveDevice\n", frame);
                d5->RemoveDevice();
                d5->Release();
            }
        LARGE_INTEGER p0;
        QueryPerformanceCounter(&p0);
        const HRESULT hr = sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        if (timeIt)
            printf("frame %d: Present took %.2f ms\n", frame, ms(p0));
        if (xellMarker)
            xellMarker(xell, static_cast<uint32_t>(frame), 5);
        bb->Release();
        if (resizeBuf && (frame == framesOff || frame == framesOff + 3))
        {
            LARGE_INTEGER r0;
            QueryPerformanceCounter(&r0);
            const HRESULT rr = sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, sd.Flags);
            printf("frame %d: ResizeBuffers 0x%08lx, %.2f ms\n", frame, static_cast<unsigned long>(rr), ms(r0));
        }
        if (!noIdle)
            Idle();
        const HRESULT removed = g_dev->GetDeviceRemovedReason();
        if (FAILED(hr) || removed != S_OK)
        {
            printf("frame %d: Present 0x%08lx, device removed reason 0x%08lx\n", frame, static_cast<unsigned long>(hr),
                   static_cast<unsigned long>(removed));
            ++failures;
            if (removed != S_OK)
                DumpDred();
        }
        // REPRO_CAP: rendered and shown frames per second over the frames on (the first 60 left out)
        static LARGE_INTEGER capT0;
        static uint64_t shown = 0;
        const int capStart = interp2 ? framesOff + framesOn / 2 + 30 : framesOff + 60; // after any switch has settled
        if (capFps > 0 && on && frame >= capStart)
        {
            xefg_swapchain_present_status_t ps {};
            xefgSwapChainGetLastPresentStatus(h, &ps);
            if (frame == capStart)
                QueryPerformanceCounter(&capT0);
            else
                shown += ps.framesPresented;
            if (frame == framesOff + framesOn - 1)
            {
                const double s = ms(capT0) / 1000.0, rendered = (framesOff + framesOn - 1 - capStart) / s;
                printf("cap %d fps (XeLL, shown frames): %.1f rendered fps, %.1f shown fps over %.1f s\n", capFps, rendered,
                       static_cast<double>(shown) / s, s);
            }
        }
        if (on && (frame == framesOff + 1 || frame == framesOff + framesOn - 1))
        {
            xefg_swapchain_present_status_t st {};
            xefgSwapChainGetLastPresentStatus(h, &st);
            printf("frame %d: presented %u, frame generation on %u\n", frame, st.framesPresented, st.isFrameGenEnabled);
        }
    }
    printf("%d frames off, %d on: %s\n", framesOff, framesOn, failures ? "FAILED" : "no Present failure, device alive");

    xefgSwapChainSetEnabled(h, 0);
    sc->Release();
    xefgSwapChainDestroy(h);
    int debugErrors = 0;
    if (ID3D12InfoQueue* iq = nullptr; debugLayer && SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&iq))))
    {
        for (UINT64 i = 0, n = iq->GetNumStoredMessages(); i < n; ++i)
        {
            SIZE_T len = 0;
            iq->GetMessage(i, nullptr, &len);
            std::vector<uint8_t> buf(len);
            auto* m = reinterpret_cast<D3D12_MESSAGE*>(buf.data());
            if (SUCCEEDED(iq->GetMessage(i, m, &len)) && m->Severity <= D3D12_MESSAGE_SEVERITY_WARNING && debugErrors++ < 20)
                printf("debug layer: %s\n", m->pDescription);
        }
        printf("debug layer: %d error(s) / warning(s)\n", debugErrors);
        iq->Release();
    }
    return failures ? 3 : debugErrors ? 2 : 0;
}
