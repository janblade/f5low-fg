// frames_only_test - drives f5low.dll's frames-only API (include/f5low_frames.h) on a D3D12 device of its own, no swap chain, no game.
//
// A bright bar (FRAMES_BARW px wide, default 120, full height, yellow: G = 255 on a ground of 16) moves FRAMES_STEP px per app frame
// (default 8); motion is R16G16_FLOAT holding -STEP (scale 1: this frame's pixels -> the previous frame's), depth constant. When the bar
// would leave the picture the test sends a reset = 1 frame and starts again at the left. After the generator is built (NOT_READY until
// then) it checks, for counts 1 and 3, that every generated frame shows ONE full-brightness bar of the right width at the right place
// (between the two app frames, 1/(count+1) of the step apart, within +-1 px). The shape check is what tells motion compensation from a
// cross-fade: with a step larger than the bar (FRAMES_BARW=40 FRAMES_STEP=64) a blend gives two half-bright bars and fails.
//
// Further phases: an argument error, a real reset, a frameId gap, the back-pressure NOT_READY (frames submitted behind a readyFence that is
// signalled late, so the context's allocators run out; every fence value must still arrive), a mid-stream render-size change (motion /
// depth sub-rect) and depth-flag change (rebuilds), Resize to another size, and the debug layer's messages (FRAMES_DEBUG=1: a message fails).
// consumedFence / doneFence are waited for with a 5 s timeout everywhere.
//
// Usage: frames_only_test.exe <f5low.dll>
//   The player's libxess_fg.dll is found as for the XeFG API (XEFG_CUDA_INTEL_LIBRARY, else next to the f5low.dll, ...).
//   FRAMES_DEBUG=1     the D3D12 debug layer
//   FRAMES_FORMAT=bgra B8G8R8A8_UNORM colour (default R8G8B8A8_UNORM)
//   FRAMES_MVFMT=n     motion vector format (34 R16G16_FLOAT default, 16 R32G32_FLOAT, 10 R16G16B16A16_FLOAT)
//   FRAMES_DEPTHFMT=n  depth format (41 R32_FLOAT default, 56 R16_UNORM through the conversion, 40 D32_FLOAT)
//   FRAMES_W / FRAMES_H  display size (default 1280x720)
//   FRAMES_BARW / FRAMES_STEP  the bar's width and step
//   FRAMES_ZERO_MOTION=1  motion vectors 0 while the bar moves: the run must FAIL (shows the checks can fail)
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
    if (f == DXGI_FORMAT_D32_FLOAT || f == DXGI_FORMAT_D16_UNORM || f == DXGI_FORMAT_D24_UNORM_S8_UINT)
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL;
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

// A texture filled from `data` (tight rows of `texel` bytes) on the test's own queue; optionally signals a fence right after the copy.
// Returns after the test's queue is idle again.
static void Fill(ID3D12Resource* tex, const std::vector<uint8_t>& data, UINT w, UINT h, UINT texel, DXGI_FORMAT f, ID3D12Fence* signalFence = nullptr, UINT64 signalValue = 0)
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
    if (signalFence)
        g_q->Signal(signalFence, signalValue);
    Idle();
    up->Release();
}

static void Readback(ID3D12Resource* tex, UINT w, UINT h, DXGI_FORMAT f, std::vector<uint8_t>& out)
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
}

// What a generated frame shows in its middle rows: the bar's left edge (centroid of the G channel above the ground, minus half the width), the peak
// brightness, the number of runs above half-max and the width of the widest run.
struct Shape
{
    double left = -1, peak = 0;
    int runs = 0, width = 0;
};
static Shape Measure(const std::vector<uint8_t>& px, UINT w, UINT h, int barW)
{
    Shape sh;
    double sum = 0, wsum = 0;
    const UINT y0 = h / 2 - 60, y1 = h / 2 + 60;
    for (UINT y = y0; y < y1; ++y)
        for (UINT x = 0; x < w; ++x)
        {
            const double g = px[(static_cast<size_t>(y) * w + x) * 4 + 1];
            sum += std::max(0.0, g - 16.0);
            wsum += std::max(0.0, g - 16.0) * x;
            sh.peak = std::max(sh.peak, g);
        }
    if (sum >= 1.0)
        sh.left = wsum / sum - (barW - 1) * 0.5;
    const UINT y = h / 2;
    int run = 0;
    for (UINT x = 0; x <= w; ++x)
    {
        const bool on = x < w && px[(static_cast<size_t>(y) * w + x) * 4 + 1] >= 128;
        if (on)
            ++run;
        else if (run)
        {
            ++sh.runs;
            sh.width = std::max(sh.width, run);
            run = 0;
        }
    }
    return sh;
}

static uint16_t HalfBits(float f)
{
    uint32_t b;
    memcpy(&b, &f, 4);
    if ((b & 0x7fffffffu) == 0)
        return static_cast<uint16_t>(b >> 16);
    const int e = static_cast<int>((b >> 23) & 255) - 127 + 15;
    return static_cast<uint16_t>(((b >> 16) & 0x8000u) | (static_cast<uint32_t>(e) << 10) | ((b >> 13) & 0x3ffu));
}

// ---- the scene: textures of one size ----
struct Scene
{
    UINT W = 0, H = 0;
    ID3D12Resource *color = nullptr, *motion = nullptr, *depth = nullptr, *targets[3] {};
    UINT64 f0 = 0;      // the app frame this scene starts at (the bar's position follows f - f0)
    int period = 1;
};

static int g_barW, g_step;
static bool g_zeroMotion;       // FRAMES_ZERO_MOTION=1: motion = 0 while the bar still moves (the run must FAIL: proves the shape check)
static DXGI_FORMAT g_colourFmt, g_mvFmt, g_depthFmt;
static bool g_bgra;
static ID3D12Fence *g_ready, *g_consumed, *g_done;
static UINT64 g_frame;      // the next app frame id (also every fence value - 1)

static void FillMotion(Scene& S, float value)
{
    const UINT texel = g_mvFmt == DXGI_FORMAT_R16G16_FLOAT ? 4 : 8;
    std::vector<uint8_t> mv(static_cast<size_t>(S.W) * S.H * texel);
    for (size_t i = 0; i < static_cast<size_t>(S.W) * S.H; ++i)
    {
        if (g_mvFmt == DXGI_FORMAT_R16G16_FLOAT)
            reinterpret_cast<uint16_t*>(mv.data())[i * 2] = HalfBits(value);
        else if (g_mvFmt == DXGI_FORMAT_R32G32_FLOAT)
            reinterpret_cast<float*>(mv.data())[i * 2] = value;
        else
            reinterpret_cast<uint16_t*>(mv.data())[i * 4] = HalfBits(value);
    }
    Fill(S.motion, mv, S.W, S.H, texel, g_mvFmt);
}

static void MakeScene(Scene& S, UINT w, UINT h)
{
    S.W = w;
    S.H = h;
    S.color = Texture(w, h, g_colourFmt);
    S.motion = Texture(w, h, g_mvFmt);
    S.depth = Texture(w, h, g_depthFmt);
    for (auto& t : S.targets)
        t = Texture(w, h, g_colourFmt);
    FillMotion(S, g_zeroMotion ? 0.0f : -static_cast<float>(g_step));
    const UINT dtexel = g_depthFmt == DXGI_FORMAT_R16_UNORM || g_depthFmt == DXGI_FORMAT_D16_UNORM ? 2 : 4;
    std::vector<uint8_t> dp(static_cast<size_t>(w) * h * dtexel);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i)
    {
        if (dtexel == 2)
            reinterpret_cast<uint16_t*>(dp.data())[i] = 32768;
        else
            reinterpret_cast<float*>(dp.data())[i] = 0.5f;
    }
    Fill(S.depth, dp, w, h, dtexel, g_depthFmt);
    S.period = std::max(2, (static_cast<int>(w) - 200 - g_barW) / g_step);
    S.f0 = g_frame;
}
static void FreeScene(Scene& S)
{
    Idle();
    for (ID3D12Resource* r : { S.color, S.motion, S.depth, S.targets[0], S.targets[1], S.targets[2] })
        if (r)
            r->Release();
    S = {};
}

// Tolerances: 1 px / 3 px for small steps; the network narrows and shifts a bar by a pixel or four at the middle of a big step (measured with a
// 64 px step: -1.3 px, 4 px narrower), so they grow with the step. A cross-fade stays far outside them (peak ~135, two bars).
static double PosTol() { return std::max(1.0, g_step / 24.0); }
static int WidthTol() { return std::max(3, g_step / 12); }

static int BarLeft(const Scene& S, UINT64 f) { return 100 + g_step * static_cast<int>((f - S.f0) % S.period); }
static bool IsCut(const Scene& S, UINT64 f) { return f > S.f0 && (f - S.f0) % S.period == 0; }

static void PutColor(Scene& S, UINT64 f)
{
    std::vector<uint8_t> px(static_cast<size_t>(S.W) * S.H * 4);
    const UINT bx = static_cast<UINT>(BarLeft(S, f));
    for (UINT y = 0; y < S.H; ++y)
        for (UINT x = 0; x < S.W; ++x)
        {
            const bool in = x >= bx && x < bx + static_cast<UINT>(g_barW);
            uint8_t* t = &px[(static_cast<size_t>(y) * S.W + x) * 4];
            const uint8_t rr = in ? 255 : 16, gg = in ? 255 : 16, bb = in ? 0 : 16;
            t[0] = g_bgra ? bb : rr;
            t[1] = gg;
            t[2] = g_bgra ? rr : bb;
            t[3] = 255;
        }
    Fill(S.color, px, S.W, S.H, 4, g_colourFmt, g_ready, f + 1);
}

struct Opt
{
    bool reset = false;
    bool putColor = true;               // fill the colour texture first (signals g_ready at f + 1)
    ID3D12Fence* readyFence = nullptr;  // default g_ready
    UINT64 readyValue = 0;              // default f + 1
    UINT mw = 0, mh = 0, dw = 0, dh = 0;
    uint32_t inverted = 0;
    bool badCount = false;
};

static F5LowFramesContext g_ctx;
static decltype(&f5lowFramesGenerate) Generate;
static decltype(&f5lowFramesLastError) LastError;

// One app frame. Returns the result; `generated` the output's count; waits for nothing.
static F5LowFramesResult Call(Scene& S, uint32_t count, const Opt& o, uint32_t* generated, UINT64* fid = nullptr)
{
    const UINT64 f = g_frame++;
    if (fid)
        *fid = f;
    if (o.putColor)
        PutColor(S, f);
    F5LowFramesInput in {};
    in.color = S.color;
    in.motion = S.motion;
    in.motionWidth = o.mw ? o.mw : S.W;
    in.motionHeight = o.mh ? o.mh : S.H;
    in.depth = S.depth;
    in.depthWidth = o.dw ? o.dw : S.W;
    in.depthHeight = o.dh ? o.dh : S.H;
    in.motionScale[0] = in.motionScale[1] = 1.0f;
    in.cameraNear = 0.1f;
    in.cameraFar = 1000.0f;
    in.depthInverted = o.inverted;
    in.frameId = f;
    in.reset = o.reset || IsCut(S, f) ? 1 : 0;
    in.readyFence = o.readyFence ? o.readyFence : g_ready;
    in.readyValue = o.readyFence ? o.readyValue : f + 1;
    in.consumedFence = g_consumed;
    in.consumedValue = f + 1;
    F5LowFramesOutput out {};
    out.count = o.badCount ? 9 : count;
    for (uint32_t k = 0; k < 3; ++k)
        out.targets[k] = S.targets[k];
    out.doneFence = g_done;
    out.doneValue = f + 1;
    const F5LowFramesResult r = Generate(g_ctx, &in, &out);
    *generated = out.generated;
    return r;
}

static bool WaitFrame(UINT64 f, const char* what)
{
    const bool a = WaitFence(g_consumed, f + 1), b = WaitFence(g_done, f + 1);
    CHECK(a, "%s: frame %llu: consumedFence did not arrive", what, static_cast<unsigned long long>(f));
    CHECK(b, "%s: frame %llu: doneFence did not arrive", what, static_cast<unsigned long long>(f));
    return a && b;
}

// Runs the next frame and checks it. expect: 0 nothing generated, 1 `count` frames expected (checked: position, one bar of full brightness, width).
// A cut frame (the bar restarts) is sent with reset = 1 and expects 0.
static F5LowFramesResult Step(Scene& S, uint32_t count, int expect, const char* what, const Opt& o = Opt(), double* worst = nullptr)
{
    uint32_t g = 99;
    UINT64 f = 0;
    const bool cut = IsCut(S, g_frame);
    const F5LowFramesResult r = Call(S, count, o, &g, &f);
    WaitFrame(f, what);
    if (cut || expect == 0)
    {
        CHECK(r == F5LOW_FRAMES_OK && g == 0, "%s: frame %llu: result %d, %u generated, expected OK and 0 (%s)", what, static_cast<unsigned long long>(f),
              static_cast<int>(r), g, LastError(g_ctx));
        return r;
    }
    CHECK(r == F5LOW_FRAMES_OK, "%s: frame %llu: Generate returned %d (%s)", what, static_cast<unsigned long long>(f), static_cast<int>(r), LastError(g_ctx));
    if (expect == 2)        // taken, nothing compared
        return r;
    CHECK(g == count, "%s: frame %llu: %u of %u generated", what, static_cast<unsigned long long>(f), g, count);
    std::vector<double> pos;
    for (uint32_t k = 0; k < g; ++k)
    {
        std::vector<uint8_t> rb;
        Readback(S.targets[k], S.W, S.H, g_colourFmt, rb);
        const Shape sh = Measure(rb, S.W, S.H, g_barW);
        const double want = BarLeft(S, f - 1) + static_cast<double>(g_step) * (k + 1) / (count + 1);
        const double err = sh.left - want;
        if (worst)
            *worst = std::max(*worst, std::fabs(err));
        CHECK(sh.left >= 0 && std::fabs(err) <= PosTol(), "%s: count %u frame %llu target %u: bar at %.2f, expected %.2f (error %.2f px)", what, count,
              static_cast<unsigned long long>(f), k, sh.left, want, err);
        CHECK(sh.peak >= 240 && sh.runs == 1 && std::abs(sh.width - g_barW) <= WidthTol(),
              "%s: count %u frame %llu target %u: not one sharp bar (peak %.0f, %d runs, widest %d px, expected one of %d px): a blend, not motion compensation",
              what, count, static_cast<unsigned long long>(f), k, sh.peak, sh.runs, sh.width, g_barW);
        pos.push_back(sh.left);
        if (k == 0 && f == S.f0 + 40)
            printf("  %s: count %u frame %llu target 0: bar at %.2f, expected %.2f, peak %.0f, width %d\n", what, count, static_cast<unsigned long long>(f), sh.left, want, sh.peak, sh.width);
    }
    for (size_t k = 1; k < pos.size(); ++k)
        CHECK(std::fabs((pos[k] - pos[k - 1]) - static_cast<double>(g_step) / (count + 1)) <= 2 * PosTol(), "%s: count %u frame %llu: step %zu is %.2f px, expected %.2f", what,
              count, static_cast<unsigned long long>(f), k, pos[k] - pos[k - 1], static_cast<double>(g_step) / (count + 1));
    return r;
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
    Generate = reinterpret_cast<decltype(&f5lowFramesGenerate)>(GetProcAddress(dll, "f5lowFramesGenerate"));
    auto Resize = reinterpret_cast<decltype(&f5lowFramesResize)>(GetProcAddress(dll, "f5lowFramesResize"));
    auto Destroy = reinterpret_cast<decltype(&f5lowFramesDestroy)>(GetProcAddress(dll, "f5lowFramesDestroy"));
    LastError = reinterpret_cast<decltype(&f5lowFramesLastError)>(GetProcAddress(dll, "f5lowFramesLastError"));
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
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_ready));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_consumed));
    g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&g_done));
    g_ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);

    const UINT W = static_cast<UINT>(EnvInt("FRAMES_W", 1280)), H = static_cast<UINT>(EnvInt("FRAMES_H", 720));
    char fm[16] = {};
    g_bgra = GetEnvironmentVariableA("FRAMES_FORMAT", fm, sizeof fm) && !_stricmp(fm, "bgra");
    g_colourFmt = g_bgra ? DXGI_FORMAT_B8G8R8A8_UNORM : DXGI_FORMAT_R8G8B8A8_UNORM;
    g_mvFmt = static_cast<DXGI_FORMAT>(EnvInt("FRAMES_MVFMT", DXGI_FORMAT_R16G16_FLOAT));
    g_depthFmt = static_cast<DXGI_FORMAT>(EnvInt("FRAMES_DEPTHFMT", DXGI_FORMAT_R32_FLOAT));
    g_barW = EnvInt("FRAMES_BARW", 120);
    g_step = EnvInt("FRAMES_STEP", 8);
    g_zeroMotion = EnvInt("FRAMES_ZERO_MOTION", 0) != 0;
    if (g_mvFmt != DXGI_FORMAT_R16G16_FLOAT && g_mvFmt != DXGI_FORMAT_R32G32_FLOAT && g_mvFmt != DXGI_FORMAT_R16G16B16A16_FLOAT)
    {
        printf("FRAMES_MVFMT %d is not one this test fills\n", static_cast<int>(g_mvFmt));
        return 2;
    }
    printf("frames-only test: %ux%u %s, motion format %d, depth format %d, bar %d px, step %d px\n", W, H, g_bgra ? "B8G8R8A8" : "R8G8B8A8",
           static_cast<int>(g_mvFmt), static_cast<int>(g_depthFmt), g_barW, g_step);

    F5LowFramesCreateInfo ci {};
    ci.apiVersion = F5LOW_FRAMES_API_VERSION;
    ci.device = g_dev;
    ci.width = W;
    ci.height = H;
    ci.colorFormat = g_colourFmt;
    ci.maxGenerated = 3;
    F5LowFramesResult r = Create(&ci, &g_ctx);
    printf("Create -> %d (%s)\n", static_cast<int>(r), r == F5LOW_FRAMES_OK ? "ok" : LastError(nullptr));
    if (r != F5LOW_FRAMES_OK)
    {
        printf("FAIL: Create\n");
        return 1;
    }
    {
        // a render size larger than the display is refused by Create
        F5LowFramesCreateInfo bad = ci;
        bad.renderWidth = W + 16;
        bad.renderHeight = H;
        F5LowFramesContext c2 = nullptr;
        const F5LowFramesResult br = Create(&bad, &c2);
        CHECK(br == F5LOW_FRAMES_ERROR_ARGUMENT && !c2, "Create with a render size above the display size returned %d", static_cast<int>(br));
        printf("Create with render size > display -> %d (%s)\n", static_cast<int>(br), LastError(nullptr));
    }

    Scene S;
    MakeScene(S, W, H);

    // 1. NOT_READY: frames fed while the generator builds (fences must arrive), until BuildStatus says OK
    int notReady = 0;
    const DWORD t0 = GetTickCount();
    while (Status(g_ctx) != F5LOW_FRAMES_OK)
    {
        if (Status(g_ctx) < 0 || GetTickCount() - t0 > 90000)
        {
            printf("FAIL: the generator did not get built: status %d (%s)\n", static_cast<int>(Status(g_ctx)), LastError(g_ctx));
            return 1;
        }
        uint32_t g = 99;
        UINT64 f;
        const F5LowFramesResult gr = Call(S, 1, Opt(), &g, &f);
        WaitFrame(f, "building");
        notReady += gr == F5LOW_FRAMES_NOT_READY;
        CHECK(gr == F5LOW_FRAMES_NOT_READY || gr == F5LOW_FRAMES_OK, "while building: Generate returned %d", static_cast<int>(gr));
        CHECK(g == 0, "while building: %u frames generated", g);
        Sleep(10);
    }
    printf("built after %lu ms, %d frames passed through as NOT_READY\n", GetTickCount() - t0, notReady);
    S.f0 = g_frame;       // the bar starts over at the first frame after the build

    // 2. warm-up: count 1
    for (int i = 0; i < 40; ++i)
        Step(S, 1, i < 2 ? 2 : 1, "warm-up");

    // 3. counts 1 and 3: position, one sharp bar of full width
    for (uint32_t count : { 1u, 3u })
    {
        double worst = 0;
        Step(S, count, 2, "warm");      // the first frame of a count is not compared (the previous frame was another count: still fine, but unchecked)
        const int n = 30;
        for (int i = 0; i < n; ++i)
            Step(S, count, 1, "bar", Opt(), &worst);
        printf("count %u: %d frames checked, worst position error %.2f px\n", count, n, worst);
    }

    // 4. an argument error still signals both fences
    {
        Opt o;
        o.badCount = true;
        uint32_t g = 99;
        UINT64 f;
        const F5LowFramesResult gr = Call(S, 1, o, &g, &f);
        CHECK(gr == F5LOW_FRAMES_ERROR_ARGUMENT, "count 9: Generate returned %d, expected ERROR_ARGUMENT", static_cast<int>(gr));
        CHECK(WaitFrame(f, "argument error"), "fences after the argument error");
        printf("argument error -> %d (%s), fences arrived\n", static_cast<int>(gr), LastError(g_ctx));
    }
    // 5. a frameId gap takes the frame as history (the argument error took an id: that is the gap); the frame after it generates
    {
        Step(S, 3, 0, "after the gap");
        Step(S, 3, 1, "after the gap + 1");
    }
    // 6. a real reset = 1 frame (not at a cut): history only; the next frame generates again
    {
        Opt o;
        o.reset = true;
        const F5LowFramesResult gr = Step(S, 3, 0, "reset", o);
        CHECK(gr == F5LOW_FRAMES_OK, "reset frame returned %d", static_cast<int>(gr));
        double worst = 0;
        Step(S, 3, 1, "after the reset", Opt(), &worst);
        printf("reset = 1 -> OK, 0 generated; the next frame generated 3 (worst error %.2f px)\n", worst);
    }

    // 7. back-pressure: frames behind a readyFence that is signalled late hold the allocators; NOT_READY must come back without blocking, and
    // every consumed / done value must still arrive once the gate opens
    {
        ID3D12Fence* gate = nullptr;
        g_dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate));
        const UINT64 first = g_frame;
        Opt o;
        o.putColor = false;
        o.readyFence = gate;
        o.readyValue = 1;
        PutColor(S, g_frame);       // content for all of them (the picture does not matter here; ready is g_ready at first + 1, not used)
        int ok = 0, nr = 0, other = 0;
        const DWORD t1 = GetTickCount();
        for (int i = 0; i < 12; ++i)
        {
            uint32_t g = 99;
            // the colour texture is only read after the gate opens
            const F5LowFramesResult gr = Call(S, 3, o, &g);
            ok += gr == F5LOW_FRAMES_OK;
            nr += gr == F5LOW_FRAMES_NOT_READY;
            other += gr != F5LOW_FRAMES_OK && gr != F5LOW_FRAMES_NOT_READY;
            CHECK(gr == F5LOW_FRAMES_OK || gr == F5LOW_FRAMES_NOT_READY, "gated frame %d: Generate returned %d (%s)", i, static_cast<int>(gr), LastError(g_ctx));
        }
        const DWORD took = GetTickCount() - t1;
        CHECK(nr > 0, "12 gated frames in a row: no NOT_READY came back (OK %d)", ok);
        CHECK(took < 3000, "12 gated Generate calls took %lu ms: they must not block", took);
        CHECK(g_done->GetCompletedValue() < first + 1, "doneFence moved while the gate was shut");
        gate->Signal(1);
        const bool all = WaitFence(g_consumed, g_frame) && WaitFence(g_done, g_frame);
        CHECK(all, "after the gate opened the consumed / done values did not all arrive");
        printf("back-pressure: 12 gated frames in %lu ms: %d taken, %d NOT_READY, %d other; all fences arrived after the gate opened: %s\n", took, ok, nr, other, all ? "yes" : "NO");
        gate->Release();
        // the context goes on: history was dropped, then it generates again
        S.f0 = g_frame;
        Step(S, 3, 0, "after back-pressure");
        Step(S, 3, 1, "after back-pressure + 1");
    }

    // 8. pipelined: the colour is rewritten only after its consumedFence (the host's rule), several frames in flight, no waits on done
    {
        int ok = 0, nr = 0;
        Scene P;
        MakeScene(P, W, H);
        P.f0 = g_frame;
        // each frame gets its own colour texture from a ring of 4; targets are shared (only the last frame's are read back)
        ID3D12Resource* ringTex[4];
        for (auto& t : ringTex)
            t = Texture(W, H, g_colourFmt);
        ID3D12Resource* origColor = P.color;
        for (int i = 0; i < 40; ++i)
        {
            const UINT64 f = g_frame;
            P.color = ringTex[f % 4];
            if (f >= 4)
                WaitFence(g_consumed, f - 3);      // the picture of frame f - 4 was read
            uint32_t g = 99;
            const F5LowFramesResult gr = Call(P, 3, Opt(), &g);
            ok += gr == F5LOW_FRAMES_OK;
            nr += gr == F5LOW_FRAMES_NOT_READY;
            CHECK(gr == F5LOW_FRAMES_OK || gr == F5LOW_FRAMES_NOT_READY, "pipelined frame %d: Generate returned %d (%s)", i, static_cast<int>(gr), LastError(g_ctx));
        }
        CHECK(WaitFence(g_consumed, g_frame) && WaitFence(g_done, g_frame), "pipelined: fences did not arrive");
        printf("pipelined: 40 frames, 4 pictures in flight: %d OK, %d NOT_READY, all fences arrived\n", ok, nr);
        P.color = origColor;
        for (auto& t : ringTex)
            t->Release();
        FreeScene(P);
        S.f0 = g_frame;
        Step(S, 3, 2, "after pipelined");
        Step(S, 3, 2, "after pipelined + 1");
        double worst = 0;
        Step(S, 3, 1, "after pipelined + 2", Opt(), &worst);
    }

    // 9. render size and depth flag changes mid-stream: motion / depth areas of half the display size (the motion in render pixels), then back;
    // depthInverted on and off. Each change rebuilds: that frame generates nothing, the next ones do.
    {
        const UINT hw = W / 2, hh = H / 2;
        FillMotion(S, -static_cast<float>(g_step) / 2);
        Opt o;
        o.mw = o.dw = hw;
        o.mh = o.dh = hh;
        S.f0 = g_frame;
        Step(S, 3, 0, "render size change", o);
        Step(S, 3, 2, "render size change + 1", o);
        double worst = 0;
        for (int i = 0; i < 6; ++i)
            Step(S, 3, 1, "half render size", o, &worst);
        printf("render size %ux%u of %ux%u: worst position error %.2f px\n", hw, hh, W, H, worst);
        FillMotion(S, -static_cast<float>(g_step));
        S.f0 = g_frame;
        Step(S, 3, 0, "back to full render size");
        Step(S, 3, 2, "back to full render size + 1");
        o = Opt();
        o.inverted = 1;
        Step(S, 3, 0, "depth inverted on", o);
        Step(S, 3, 2, "depth inverted on + 1", o);
        worst = 0;
        for (int i = 0; i < 4; ++i)
            Step(S, 3, 1, "depth inverted", o, &worst);
        Step(S, 3, 0, "depth inverted off");
        Step(S, 3, 2, "depth inverted off + 1");
        Step(S, 3, 1, "depth inverted off + 2", Opt(), &worst);
        printf("depth flag changes: worst position error %.2f px\n", worst);
        // a render area above the display size is refused (fences still arrive)
        Opt big;
        big.mw = W + 8;
        big.mh = H;
        // the texture is only W wide, so this is "area larger than its texture": ERROR_ARGUMENT either way
        uint32_t g = 99;
        UINT64 f;
        const F5LowFramesResult gr = Call(S, 3, big, &g, &f);
        CHECK(gr == F5LOW_FRAMES_ERROR_ARGUMENT, "a motion area above the display size returned %d", static_cast<int>(gr));
        CHECK(WaitFrame(f, "oversized motion area"), "oversized motion area: fences");
    }

    // 10. Resize to another size (new textures), then it works there; a Resize to the same size is covered by the build's own sizes
    {
        const UINT W2 = W * 3 / 4 & ~1u, H2 = H * 3 / 4 & ~1u;
        FreeScene(S);
        r = Resize(g_ctx, W2, H2, g_colourFmt, 0);
        CHECK(r == F5LOW_FRAMES_OK, "Resize returned %d (%s)", static_cast<int>(r), LastError(g_ctx));
        MakeScene(S, W2, H2);
        Step(S, 3, 0, "after Resize");
        Step(S, 3, 2, "after Resize + 1");
        double worst = 0;
        for (int i = 0; i < 6; ++i)
            Step(S, 3, 1, "after Resize", Opt(), &worst);
        printf("after Resize to %ux%u: worst position error %.2f px\n", W2, H2, worst);
        // a Resize with an unusable format is an argument error and changes nothing
        r = Resize(g_ctx, W2, H2, DXGI_FORMAT_R10G10B10A2_TYPELESS, 0);
        CHECK(r == F5LOW_FRAMES_ERROR_ARGUMENT, "Resize to R10G10B10A2_TYPELESS returned %d, expected ERROR_ARGUMENT", static_cast<int>(r));
        printf("Resize to R10G10B10A2_TYPELESS -> %d (%s)\n", static_cast<int>(r), LastError(g_ctx));
        Step(S, 3, 1, "after the refused Resize");
    }

    FreeScene(S);
    Destroy(g_ctx);
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
        else
            printf("debug layer: 0 messages\n");
    }
    printf("%s (%d failures)\n", g_failures ? "FAIL" : "PASS", g_failures);
    return g_failures ? 1 : 0;
}
