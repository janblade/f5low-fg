// frames_only_test - drives f5low.dll's frames-only API (include/f5low_frames.h) on a D3D12 device of its own, no swap chain, no game.
//
// A bright bar (120 px wide, full height) moves +8 px per app frame on a dark ground; motion is R16G16_FLOAT holding -8 (scale 1: this
// frame's pixels -> the previous frame's), depth R32_FLOAT constant. After the generator is built (NOT_READY until then) it checks, for
// counts 1 and 3, that the generated frames put the bar between the two app frames' positions in even steps: 2x one frame 4 px from
// each, 4x three frames 2 px apart, each within +-1 px. It also checks that consumedFence and doneFence arrive (5 s timeout each, also
// while NOT_READY and after an argument error), and that a Resize leaves it working.
//
// Usage: frames_only_test.exe <f5low.dll>
//   The player's libxess_fg.dll is found as for the XeFG API (XEFG_CUDA_INTEL_LIBRARY, else next to the f5low.dll, ...).
//   FRAMES_DEBUG=1     the D3D12 debug layer (its messages are printed; a message makes the run FAIL)
//   FRAMES_FORMAT=bgra B8G8R8A8_UNORM colour (default R8G8B8A8_UNORM)
//   FRAMES_MVFMT=n     motion vector format (a number: 34 R16G16_FLOAT default, 16 R32G32_FLOAT, 10 R16G16B16A16_FLOAT)
//   FRAMES_DEPTHFMT=56 depth in R16_UNORM (the conversion path; default 41 R32_FLOAT)
//   FRAMES_W / FRAMES_H  display size (default 1280x720)
//   F5LOW_FRAMES_QUEUE=compute  (read by the DLL) its own queue is a compute queue
// Exit code: 0 PASS, 1 FAIL, 2 setup failed.

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <f5low_frames.h>

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static ID3D12Device* g_dev;
static ID3D12CommandQueue* g_q;
static ID3D12Fence* g_idle;
static UINT64 g_idleV;
static HANDLE g_ev;
static int g_failures;

#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            ++g_failures;                                                                                             \
            printf("FAIL: " __VA_ARGS__);                                                                             \
            printf("\n");                                                                                             \
        }                                                                                                             \
    } while (0)

static int EnvInt(const char* n, int d)
{
    char b[32];
    return GetEnvironmentVariableA(n, b, sizeof b) ? atoi(b) : d;
}

static bool WaitFence(ID3D12Fence* f, UINT64 v, DWORD ms = 5000)
{
    if (f->GetCompletedValue() >= v)
        return true;
    f->SetEventOnCompletion(v, g_ev);
    return WaitForSingleObject(g_ev, ms) == WAIT_OBJECT_0;
}
static void Idle()
{
    g_q->Signal(g_idle, ++g_idleV);
    WaitFence(g_idle, g_idleV, 20000);
}

static ID3D12Resource* Buffer(size_t n, D3D12_HEAP_TYPE type)
{
    D3D12_HEAP_PROPERTIES hp { type };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = n;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&r));
    return r;
}
static ID3D12Resource* Texture(UINT w, UINT h, DXGI_FORMAT f)
{
    D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
    rd.Width = w;
    rd.Height = h;
    rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.Format = f;
    rd.SampleDesc.Count = 1;
    ID3D12Resource* r = nullptr;
    g_dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&r));
    return r;
}

static void Barrier(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    cl->ResourceBarrier(1, &br);
}

static ID3D12CommandAllocator* g_alloc;
static ID3D12GraphicsCommandList* g_cl;

// A texture filled from `data` (tight rows of `texel` bytes) on the test's own queue; returns after the queue is idle again.
static void Fill(ID3D12Resource* tex, const std::vector<uint8_t>& data, UINT w, UINT h, UINT texel, DXGI_FORMAT f, ID3D12Fence* readyFence = nullptr, UINT64 readyValue = 0)
{
    const UINT pitch = (w * texel + 255) & ~255u;
    ID3D12Resource* up = Buffer(static_cast<size_t>(pitch) * h, D3D12_HEAP_TYPE_UPLOAD);
    uint8_t* p = nullptr;
    up->Map(0, nullptr, reinterpret_cast<void**>(&p));
    for (UINT y = 0; y < h; ++y)
        memcpy(p + static_cast<size_t>(y) * pitch, data.data() + static_cast<size_t>(y) * w * texel, static_cast<size_t>(w) * texel);
    up->Unmap(0, nullptr);
    g_alloc->Reset();
    g_cl->Reset(g_alloc, nullptr);
    D3D12_TEXTURE_COPY_LOCATION d { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { up, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
    s.PlacedFootprint.Footprint = { f, w, h, 1, pitch };
    Barrier(g_cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
    g_cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    Barrier(g_cl, tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
    g_cl->Close();
    ID3D12CommandList* l = g_cl;
    g_q->ExecuteCommandLists(1, &l);
    if (readyFence)
        g_q->Signal(readyFence, readyValue);
    Idle();
    up->Release();
}

// Row-averaged brightness profile of a colour texture: the green channel (255 on the bar, 16 on the ground in RGBA and BGRA alike).
static bool Readback(ID3D12Resource* tex, UINT w, UINT h, DXGI_FORMAT f, std::vector<uint8_t>& out)
{
    const UINT pitch = (w * 4 + 255) & ~255u;
    ID3D12Resource* rb = Buffer(static_cast<size_t>(pitch) * h, D3D12_HEAP_TYPE_READBACK);
    g_alloc->Reset();
    g_cl->Reset(g_alloc, nullptr);
    D3D12_TEXTURE_COPY_LOCATION d { rb, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, s { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
    d.PlacedFootprint.Footprint = { f, w, h, 1, pitch };
    Barrier(g_cl, tex, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g_cl->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
    Barrier(g_cl, tex, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
    g_cl->Close();
    ID3D12CommandList* l = g_cl;
    g_q->ExecuteCommandLists(1, &l);
    Idle();
    uint8_t* p = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&p));
    out.resize(static_cast<size_t>(w) * h * 4);
    for (UINT y = 0; y < h; ++y)
        memcpy(out.data() + static_cast<size_t>(y) * w * 4, p + static_cast<size_t>(y) * pitch, static_cast<size_t>(w) * 4);
    rb->Unmap(0, nullptr);
    rb->Release();
    return true;
}

// The bar's left edge in a frame, from the centroid of the green channel over the rows 300..420 (centroid = left edge + 59.5 for the 120 px bar).
// Returns -1 when nothing bright is there.
static double BarLeft(const std::vector<uint8_t>& px, UINT w, UINT h)
{
    double sum = 0, wsum = 0;
    for (UINT y = h / 2 - 60; y < h / 2 + 60; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const double b = std::max(0.0, px[(static_cast<size_t>(y) * w + x) * 4 + 1] - 16.0);
            sum += b;
            wsum += b * x;
        }
    return sum < 1.0 ? -1.0 : wsum / sum - 59.5;
}

static uint16_t HalfBits(float f)
{
    // only the values used here: -8 -> 0xC800, 0 -> 0
    if (f == 0.0f)
        return 0;
    if (f == -8.0f)
        return 0xC800;
    return 0;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: frames_only_test <f5low.dll>\n");
        return 2;
    }
    char full[MAX_PATH];
    GetFullPathNameA(argv[1], MAX_PATH, full, nullptr);
    HMODULE dll = LoadLibraryA(full);
    if (!dll)
    {
        printf("cannot load %s (error %lu)\n", full, GetLastError());
        return 2;
    }
    auto Create = reinterpret_cast<decltype(&f5lowFramesCreate)>(GetProcAddress(dll, "f5lowFramesCreate"));
    auto Status = reinterpret_cast<decltype(&f5lowFramesBuildStatus)>(GetProcAddress(dll, "f5lowFramesBuildStatus"));
    auto Generate = reinterpret_cast<decltype(&f5lowFramesGenerate)>(GetProcAddress(dll, "f5lowFramesGenerate"));
    auto Resize = reinterpret_cast<decltype(&f5lowFramesResize)>(GetProcAddress(dll, "f5lowFramesResize"));
    auto Destroy = reinterpret_cast<decltype(&f5lowFramesDestroy)>(GetProcAddress(dll, "f5lowFramesDestroy"));
    auto LastError = reinterpret_cast<decltype(&f5lowFramesLastError)>(GetProcAddress(dll, "f5lowFramesLastError"));
    if (!Create || !Status || !Generate || !Resize || !Destroy || !LastError)
    {
        printf("the DLL lacks one of the six f5lowFrames* exports\n");
        return 2;
    }

    if (EnvInt("FRAMES_DEBUG", 0))
    {
        ID3D12Debug* dbg = nullptr;
        if (SUCCEEDED(D3D12GetDebugInterface(IID_PPV_ARGS(&dbg))))
        {
            dbg->EnableDebugLayer();
            dbg->Release();
        }
    }
    IDXGIFactory6* fac = nullptr;
    CreateDXGIFactory2(0, IID_PPV_ARGS(&fac));
    IDXGIAdapter1* ad = nullptr;
    for (UINT i = 0; fac && SUCCEEDED(fac->EnumAdapterByGpuPreference(i, DXGI_GPU_PREFERENCE_HIGH_PERFORMANCE, IID_PPV_ARGS(&ad))); ++i)
    {
        DXGI_ADAPTER_DESC1 d;
        ad->GetDesc1(&d);
        if (d.VendorId == 0x10DE && !(d.Flags & DXGI_ADAPTER_FLAG_SOFTWARE))
        {
            wprintf(L"adapter: %s\n", d.Description);
            break;
        }
        ad->Release();
        ad = nullptr;
    }
    if (!ad || FAILED(D3D12CreateDevice(ad, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&g_dev))))
    {
        printf("no NVIDIA D3D12 device\n");
        return 2;
    }
    ID3D12InfoQueue* iq = nullptr;
    if (EnvInt("FRAMES_DEBUG", 0) && SUCCEEDED(g_dev->QueryInterface(IID_PPV_ARGS(&iq))))
        iq->SetBreakOnSeverity(D3D12_MESSAGE_SEVERITY_ERROR, FALSE);
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    g_dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&g_q));
    g_dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&g_alloc));
    g_dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, g_alloc, nullptr, IID_PPV_ARGS(&g_cl));
    g_cl->Close();
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_idle));
    g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const UINT W = static_cast<UINT>(EnvInt("FRAMES_W", 1280)), H = static_cast<UINT>(EnvInt("FRAMES_H", 720));
    char fm[16] = {};
    const bool bgra = GetEnvironmentVariableA("FRAMES_FORMAT", fm, sizeof fm) && !_stricmp(fm, "bgra");
    const DXGI_FORMAT colourFmt = bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    const DXGI_FORMAT mvFmt = static_cast<DXGI_FORMAT>(EnvInt("FRAMES_MVFMT", DXGI_FORMAT_R16G16_FLOAT));
    printf("frames-only test: %ux%u %s, motion format %d\n", W, H, bgra ? "B8G8R8A8" : "R8G8B8A8", static_cast<int>(mvFmt));

    F5LowFramesCreateInfo ci {};
    ci.apiVersion = F5LOW_FRAMES_API_VERSION;
    ci.device = g_dev;
    ci.width = W;
    ci.height = H;
    ci.colorFormat = colourFmt;
    ci.maxGenerated = 3;
    F5LowFramesContext ctx = nullptr;
    F5LowFramesResult r = Create(&ci, &ctx);
    printf("Create -> %d (%s)\n", static_cast<int>(r), r == F5LOW_FRAMES_OK ? "ok" : LastError(nullptr));
    if (r != F5LOW_FRAMES_OK)
    {
        printf("FAIL: Create\n");
        return 1;
    }

    // inputs and targets
    ID3D12Resource* color = Texture(W, H, colourFmt);
    ID3D12Resource* motion = Texture(W, H, mvFmt);
    const DXGI_FORMAT depthFmt = static_cast<DXGI_FORMAT>(EnvInt("FRAMES_DEPTHFMT", DXGI_FORMAT_R32_FLOAT));
    ID3D12Resource* depth = Texture(W, H, depthFmt);
    ID3D12Resource* targets[3];
    for (auto& t : targets)
        t = Texture(W, H, colourFmt);
    ID3D12Fence *ready, *consumed, *done;
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ready));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumed));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&done));
    {
        // motion: (-8, 0) everywhere in the tagged format; depth 0.5
        const UINT texel = mvFmt == DXGI_FORMAT_R16G16_FLOAT ? 4 : mvFmt == DXGI_FORMAT_R32G32_FLOAT ? 8 : mvFmt == DXGI_FORMAT_R16G16B16A16_FLOAT ? 8 : 0;
        if (!texel)
        {
            printf("FRAMES_MVFMT %d is not one this test fills\n", static_cast<int>(mvFmt));
            return 2;
        }
        std::vector<uint8_t> mv(static_cast<size_t>(W) * H * texel);
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i)
        {
            if (texel == 4)
            {
                reinterpret_cast<uint16_t*>(mv.data())[i * 2] = HalfBits(-8.0f);
                reinterpret_cast<uint16_t*>(mv.data())[i * 2 + 1] = 0;
            }
            else if (mvFmt == DXGI_FORMAT_R32G32_FLOAT)
            {
                reinterpret_cast<float*>(mv.data())[i * 2] = -8.0f;
                reinterpret_cast<float*>(mv.data())[i * 2 + 1] = 0;
            }
            else
            {
                reinterpret_cast<uint16_t*>(mv.data())[i * 4] = HalfBits(-8.0f);
            }
        }
        Fill(motion, mv, W, H, texel, mvFmt);
        const UINT dtexel = depthFmt == DXGI_FORMAT_R16_UNORM ? 2 : 4;
        std::vector<uint8_t> dp(static_cast<size_t>(W) * H * dtexel);
        for (size_t i = 0; i < static_cast<size_t>(W) * H; ++i)
        {
            if (dtexel == 2)
                reinterpret_cast<uint16_t*>(dp.data())[i] = 32768;
            else
                reinterpret_cast<float*>(dp.data())[i] = 0.5f;
        }
        Fill(depth, dp, W, H, dtexel, depthFmt);
    }

    UINT64 frameNo = 0;      // app frames fed so far (the bar's position and the fence values follow it)
    auto barLeft = [](UINT64 f) { return 100.0 + 8.0 * static_cast<double>(f); };
    auto feed = [&](uint32_t count, bool wantGenerated, std::vector<double>* positions, F5LowFramesResult* res, uint32_t* generatedOut) {
        const UINT64 f = frameNo++;
        std::vector<uint8_t> px(static_cast<size_t>(W) * H * 4);
        const UINT bx = static_cast<UINT>(barLeft(f));
        for (UINT y = 0; y < H; ++y)
            for (UINT x = 0; x < W; ++x)
            {
                const bool in = x >= bx && x < bx + 120;
                uint8_t* t = &px[(static_cast<size_t>(y) * W + x) * 4];
                // yellow bar (G = 255) on a dark ground (16), the channel order of the format
                const uint8_t rr = in ? 255 : 16, gg = in ? 255 : 16, bb = in ? 0 : 16;
                t[0] = bgra ? bb : rr;
                t[1] = gg;
                t[2] = bgra ? rr : bb;
                t[3] = 255;
            }
        // the host writes the picture, then signals ready; f5low reads it after that on its own queue
        Fill(color, px, W, H, 4, colourFmt, ready, f + 1);
        F5LowFramesInput in {};
        in.color = color;
        in.motion = motion;
        in.motionWidth = W;
        in.motionHeight = H;
        in.depth = depth;
        in.depthWidth = W;
        in.depthHeight = H;
        in.motionScale[0] = in.motionScale[1] = 1.0f;
        in.cameraNear = 0.1f;
        in.cameraFar = 1000.0f;
        in.frameId = f;
        in.readyFence = ready;
        in.readyValue = f + 1;
        in.consumedFence = consumed;
        in.consumedValue = f + 1;
        F5LowFramesOutput out {};
        out.count = count;
        for (uint32_t k = 0; k < count; ++k)
            out.targets[k] = targets[k];
        out.doneFence = done;
        out.doneValue = f + 1;
        const F5LowFramesResult gr = Generate(ctx, &in, &out);
        if (res)
            *res = gr;
        if (generatedOut)
            *generatedOut = out.generated;
        CHECK(WaitFence(consumed, f + 1), "frame %llu: consumedFence did not arrive (result %d)", static_cast<unsigned long long>(f), static_cast<int>(gr));
        CHECK(WaitFence(done, f + 1), "frame %llu: doneFence did not arrive (result %d)", static_cast<unsigned long long>(f), static_cast<int>(gr));
        if (wantGenerated && positions)
        {
            CHECK(gr == F5LOW_FRAMES_OK, "frame %llu: Generate returned %d (%s)", static_cast<unsigned long long>(f), static_cast<int>(gr), LastError(ctx));
            CHECK(out.generated == count, "frame %llu: %u of %u generated", static_cast<unsigned long long>(f), out.generated, count);
            for (uint32_t k = 0; k < out.generated; ++k)
            {
                std::vector<uint8_t> rb;
                Readback(targets[k], W, H, colourFmt, rb);
                positions->push_back(BarLeft(rb, W, H));
            }
        }
    };

    // 1. NOT_READY: frames fed while the generator builds (fences must arrive), until BuildStatus says OK
    int notReady = 0;
    const DWORD t0 = GetTickCount();
    while (Status(ctx) != F5LOW_FRAMES_OK)
    {
        if (Status(ctx) < 0 || GetTickCount() - t0 > 90000)
        {
            printf("FAIL: the generator did not get built: status %d (%s)\n", static_cast<int>(Status(ctx)), LastError(ctx));
            return 1;
        }
        F5LowFramesResult gr = F5LOW_FRAMES_OK;
        uint32_t g = 99;
        feed(1, false, nullptr, &gr, &g);
        if (gr == F5LOW_FRAMES_NOT_READY)
            ++notReady;
        CHECK(gr == F5LOW_FRAMES_NOT_READY || gr == F5LOW_FRAMES_OK, "while building: Generate returned %d", static_cast<int>(gr));
        CHECK(g == 0, "while building: %u frames generated", g);
        Sleep(10);
    }
    printf("built after %lu ms, %d frames passed through as NOT_READY\n", GetTickCount() - t0, notReady);

    // 2. ~60 frames, count 1: warm-up (the first has no history: generated 0)
    for (int i = 0; i < 60; ++i)
    {
        F5LowFramesResult gr;
        uint32_t g;
        feed(1, false, nullptr, &gr, &g);
        CHECK(gr == F5LOW_FRAMES_OK, "warm-up frame %d: Generate returned %d (%s)", i, static_cast<int>(gr), LastError(ctx));
    }

    // 3. counts 1 and 3: the bar's position in each generated frame
    for (uint32_t count : { 1u, 3u })
    {
        double worst = 0;
        int checked = 0;
        for (int i = 0; i < 6; ++i)
        {
            std::vector<double> pos;
            feed(count, true, &pos, nullptr, nullptr);
            const UINT64 f = frameNo - 1;
            for (size_t k = 0; k < pos.size(); ++k)
            {
                const double want = barLeft(f - 1) + 8.0 * static_cast<double>(k + 1) / static_cast<double>(count + 1);
                const double err = pos[k] - want;
                worst = std::max(worst, std::fabs(err));
                ++checked;
                CHECK(pos[k] >= 0 && std::fabs(err) <= 1.0, "count %u frame %llu target %zu: bar at %.2f, expected %.2f (error %.2f px)", count,
                      static_cast<unsigned long long>(f), k, pos[k], want, err);
                if (i == 0)
                    printf("  count %u target %zu: bar at %.2f, expected %.2f\n", count, k, pos[k], want);
            }
            // even steps between consecutive generated frames
            for (size_t k = 1; k < pos.size(); ++k)
                CHECK(std::fabs((pos[k] - pos[k - 1]) - 8.0 / (count + 1)) <= 1.0, "count %u frame %llu: step %zu is %.2f px, expected %.2f", count,
                      static_cast<unsigned long long>(f), k, pos[k] - pos[k - 1], 8.0 / (count + 1));
        }
        printf("count %u: %d generated frames checked, worst position error %.2f px\n", count, checked, worst);
    }

    // 4. an argument error still signals both fences; a reset takes the frame as history (generated 0)
    {
        const UINT64 f = frameNo++;
        F5LowFramesInput in {};
        in.color = color;
        in.motion = motion;
        in.depth = depth;
        in.motionWidth = in.motionHeight = in.depthWidth = in.depthHeight = 0;
        in.consumedFence = consumed;
        in.consumedValue = f + 1;
        in.frameId = f;
        F5LowFramesOutput out {};
        out.count = 9;      // above maxGenerated
        out.doneFence = done;
        out.doneValue = f + 1;
        const F5LowFramesResult gr = Generate(ctx, &in, &out);
        CHECK(gr == F5LOW_FRAMES_ERROR_ARGUMENT, "count 9: Generate returned %d, expected ERROR_ARGUMENT", static_cast<int>(gr));
        CHECK(WaitFence(consumed, f + 1) && WaitFence(done, f + 1), "after the argument error the fences did not arrive");
        printf("argument error -> %d (%s), fences arrived\n", static_cast<int>(gr), LastError(ctx));
    }
    {
        // a frame with reset = 1 makes no generated frames
        F5LowFramesResult gr;
        uint32_t g = 99;
        // feed() has no reset switch: a frameId gap (skip one) counts as a reset too
        ++frameNo;
        feed(3, false, nullptr, &gr, &g);
        CHECK(gr == F5LOW_FRAMES_OK && g == 0, "a frameId gap: result %d, %u frames generated, expected OK and 0", static_cast<int>(gr), g);
        printf("frameId gap -> result %d, %u generated\n", static_cast<int>(gr), g);
        feed(1, false, nullptr, &gr, &g);
        feed(1, false, nullptr, &gr, &g);
        CHECK(g == 1, "after the gap, two frames later: %u generated, expected 1", g);
    }

    // 5. Resize to the same size and format (waits idle, rebuilds, drops the history), then it works again
    {
        r = Resize(ctx, W, H, colourFmt, 0);
        CHECK(r == F5LOW_FRAMES_OK, "Resize returned %d (%s)", static_cast<int>(r), LastError(ctx));
        F5LowFramesResult gr;
        uint32_t g = 99;
        feed(1, false, nullptr, &gr, &g);
        CHECK(gr == F5LOW_FRAMES_OK && g == 0, "first frame after Resize: result %d, %u generated, expected OK and 0", static_cast<int>(gr), g);
        std::vector<double> pos;
        feed(1, false, nullptr, &gr, &g);
        feed(1, true, &pos, nullptr, nullptr);
        const double want = barLeft(frameNo - 2) + 4.0;
        CHECK(pos.size() == 1 && std::fabs(pos[0] - want) <= 1.0, "after Resize: bar at %.2f, expected %.2f", pos.empty() ? -1.0 : pos[0], want);
        printf("after Resize: bar at %.2f, expected %.2f\n", pos.empty() ? -1.0 : pos[0], want);
    }

    Destroy(ctx);
    if (iq)
    {
        const UINT64 n = iq->GetNumStoredMessages();
        if (n)
        {
            printf("FAIL: the debug layer stored %llu messages\n", static_cast<unsigned long long>(n));
            for (UINT64 i = 0; i < n && i < 10; ++i)
            {
                SIZE_T len = 0;
                iq->GetMessage(i, nullptr, &len);
                std::vector<uint8_t> b(len);
                auto* m = reinterpret_cast<D3D12_MESSAGE*>(b.data());
                iq->GetMessage(i, m, &len);
                printf("  [%d] %s\n", static_cast<int>(m->Severity), m->pDescription);
            }
            ++g_failures;
        }
    }
    printf("%s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
