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
//   REPRO_PRELOAD=path  loads that libxess_fg.dll before CreateContext (intel generator: the unlock must be skipped)
//   REPRO_FRONT=1   keep the window in front (DLSS-G, Generator=dlssg, generates only for the foreground window)
//   REPRO_BAR=1    the picture is a bright-yellow bar (120 px wide, full height) moving 12 px per app frame on a dark ground, motion
//                   vectors (-8, 0) to match: with SINK_PROBE the sink reads row 540 of every frame shown, so the steps between
//                   shown frames say whether they are in order (docs/GENERATORS.md)
//   REPRO_HIST=1   after every frame with frame generation on, GetLastPresentStatus: a histogram of frames presented per app frame
//                   and the number of times the window had lost the foreground, printed at the end
//   REPRO_FPS=n     the app produces n frames per second, not as fast as the GPU lets it
//   REPRO_RAMP=a,b,secs  the app's fps ramps from a to b over secs seconds, starting with frame generation on (a before it)
//   REPRO_HITCH=frame,ms  one long app frame: ms extra before frame number `frame` of the frames on (REPRO_FPS needed)
//   REPRO_CAPSTEP=frame,fps  from frame number `frame` of the frames on the app runs at fps (a frame-cap change)
//   REPRO_JITTER=ms  every app frame leaves up to +-ms early or late (uniform, seeded: the same on every run); the mean is kept
//   REPRO_PACELOG=1  after the run, one line per app frame on: "app: frame=N t=<QPC ms before Present> ret=<QPC ms after it>"
//                   (with the DLL's PaceLog / sink pace lines tests/pacer_ab.ps1 gets latencies from them)
//   REPRO_MENU=N    two N-frame stretches (from a quarter and from five eighths of the frames on) with frame generation
//                   still enabled but no tags, no SetPresentId and no frame constants: Cyberpunk's pause menu
//   REPRO_FS=1      exclusive fullscreen from a third of the frames on until two thirds (SetFullscreenState + ResizeBuffers)
//   REPRO_RECREATE_GEN=name  with REPRO_RECREATE_AT: F5LOW_GENERATOR is set to name before the new context is made
//                   (own <-> dlssg without a restart)
//   Generator=dlssg: F5LOW_SINK_STATS=1 (the DLL logs the sink's statistics to f5low.log when the context is destroyed) and
//                   SINK_PROBE=1 (reads row 540 of every frame shown: the moving bar's steps say whether frames are in order)
// Exit code: 0 ok, 1 setup failed, 2 debug-layer messages, 3 a Present failed / the device was removed.

#include <windows.h>
#include <timeapi.h>
#include <shellapi.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
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
#pragma comment(lib, "winmm.lib")

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

// A crash inside a library under test: where (module + offset) and, for an access violation, what it touched.
static LONG WINAPI CrashReport(EXCEPTION_POINTERS* ep)
{
    const DWORD code = ep->ExceptionRecord->ExceptionCode;
    if (code != EXCEPTION_ACCESS_VIOLATION && code != EXCEPTION_STACK_OVERFLOW && code != 0xC0000094 && code != 0xC000001D)
        return EXCEPTION_CONTINUE_SEARCH;
    HMODULE m = nullptr;
    char name[MAX_PATH] = "?";
    if (GetModuleHandleExA(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           static_cast<LPCSTR>(ep->ExceptionRecord->ExceptionAddress), &m))
        GetModuleFileNameA(m, name, sizeof(name));
    printf("CRASH 0x%08lx at %p = %s + 0x%llx, thread %lu", code, ep->ExceptionRecord->ExceptionAddress, name,
           static_cast<unsigned long long>(reinterpret_cast<uintptr_t>(ep->ExceptionRecord->ExceptionAddress) - reinterpret_cast<uintptr_t>(m)),
           GetCurrentThreadId());
    if (code == EXCEPTION_ACCESS_VIOLATION && ep->ExceptionRecord->NumberParameters >= 2)
        printf(", %s address %p", ep->ExceptionRecord->ExceptionInformation[0] ? "wrote" : "read",
               reinterpret_cast<void*>(ep->ExceptionRecord->ExceptionInformation[1]));
    printf("\n");
    fflush(stdout);
    return EXCEPTION_CONTINUE_SEARCH;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0); // every line reaches the file even when a hang is killed
    AddVectoredExceptionHandler(1, CrashReport);
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
    const bool bar = EnvInt("REPRO_BAR", 0) == 1;
    const bool fsTest = EnvInt("REPRO_FS", 0) == 1;
    const int appFps = EnvInt("REPRO_FPS", 0);
    const int menuLen = EnvInt("REPRO_MENU", 0);
    // Pacing scenarios (tests/pacer_ab.ps1): the app's frame rate over the frames on. All need REPRO_FPS (the base rate).
    struct Scn
    {
        double rampA = 0, rampB = 0, rampSecs = 0;
        int hitchFrame = -1;
        double hitchMs = 0;
        int stepFrame = -1;
        double stepFps = 0;
        double jitterMs = 0;
    } scn;
    {
        char v[64] = {};
        if (GetEnvironmentVariableA("REPRO_RAMP", v, sizeof(v)))
            sscanf_s(v, "%lf,%lf,%lf", &scn.rampA, &scn.rampB, &scn.rampSecs);
        if (GetEnvironmentVariableA("REPRO_HITCH", v, sizeof(v)))
            sscanf_s(v, "%d,%lf", &scn.hitchFrame, &scn.hitchMs);
        if (GetEnvironmentVariableA("REPRO_CAPSTEP", v, sizeof(v)))
            sscanf_s(v, "%d,%lf", &scn.stepFrame, &scn.stepFps);
        if (GetEnvironmentVariableA("REPRO_JITTER", v, sizeof(v)))
            scn.jitterMs = atof(v);
    }
    const bool paceLog = EnvInt("REPRO_PACELOG", 0) == 1;
    // REPRO_TIMER1=1: the app raises the timer resolution to 1 ms (timeBeginPeriod), as most games do; without it the
    // REPRO_FPS limiter's Sleep(1) can oversleep up to 15.6 ms unless a loaded DLL raised it (DLSS-G does).
    if (EnvInt("REPRO_TIMER1", 0) == 1)
        printf("timeBeginPeriod(1): %u\n", timeBeginPeriod(1));
    struct AppRec
    {
        int frame;
        double t0, t1, waitMs = 0, idleMs = 0;
    };
    std::vector<AppRec> appRecs;
    uint32_t jitterState = 0x9E3779B9u;      // xorshift32: the same jitter on every run
    LARGE_INTEGER onStart {};                // QPC of the first frame on
    bool fsOn = false;
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

    // f5lowGetStatus (docs/GENERATORS.md, "Exports"): 0 before any context, 1 own generator, 2 forwarding to Intel's library
    auto getStatus = reinterpret_cast<int(__cdecl*)(char*, int)>(GetProcAddress(dll, "f5lowGetStatus"));
    auto printStatus = [&](const char* when) {
        char reason[256] = {};
        const int st = getStatus ? getStatus(reason, static_cast<int>(sizeof(reason))) : -1;
        printf("f5lowGetStatus %s: %d \"%s\"\n", when, st, reason);
        return st;
    };
    // REPRO_PRE_VERSION=ms: the first API call (GetVersion, as OptiScaler makes it) and then that long a pause before CreateContext:
    // the DLL's background build has its verdict by then (CreateContext takes next to nothing)
    if (EnvInt("REPRO_PRE_VERSION", -1) >= 0)
    {
        LOADFN(dll, xefgSwapChainGetVersion);
        xefg_swapchain_version_t ver {};
        printf("GetVersion: %d\n", xefgSwapChainGetVersion ? xefgSwapChainGetVersion(&ver) : -1);
        Sleep(static_cast<DWORD>(EnvInt("REPRO_PRE_VERSION", 0)));
    }
    const int stBefore = printStatus("before CreateContext");
    xefg_swapchain_handle_t h = nullptr;
    LARGE_INTEGER qf, qa, qb;
    QueryPerformanceFrequency(&qf);
    QueryPerformanceCounter(&qa);
    // REPRO_FORWARD_FIRST=1: a call on a null handle before CreateContext, which f5low forwards to the shared libxess_fg (loading it): a
    // Generator=intel run must still unlock (the module is f5low's own, no context was made through it)
    if (EnvInt("REPRO_FORWARD_FIRST", 0) == 1)
    {
        LOADFN(dll, xefgSwapChainGetPipelineBuildStatus);
        printf("forwarded call before CreateContext: %d\n", xefgSwapChainGetPipelineBuildStatus ? xefgSwapChainGetPipelineBuildStatus(nullptr) : -99);
    }
    // REPRO_PRELOAD=<libxess_fg.dll>: that library is loaded in this process BEFORE CreateContext (a Generator=intel run must then skip its
    // unlock: the library may already have contexts, see generators/intel.h)
    if (char pre[1024]; GetEnvironmentVariableA("REPRO_PRELOAD", pre, sizeof(pre)))
        printf("REPRO_PRELOAD %s: %p\n", pre, static_cast<void*>(LoadLibraryA(pre)));
    const int createRc = xefgSwapChainD3D12CreateContext(g_dev, &h);
    QueryPerformanceCounter(&qb);
    printf("CreateContext: %d (took %.1f ms on this thread)\n", createRc, 1000.0 * static_cast<double>(qb.QuadPart - qa.QuadPart) / static_cast<double>(qf.QuadPart));
    const int stAfter = printStatus("after CreateContext");
    // 0 is right while the DLL builds its frame generator in the background; 1 .. 3 the decision (own, forwarding, unavailable).
    // An older f5low.dll (e.g. a baseline to compare pictures with) has no f5lowGetStatus: nothing to check then.
    if (getStatus && (stBefore != 0 || stAfter < 0 || stAfter > 3))
    {
        printf("f5lowGetStatus check FAILED\n");
        return 1;
    }
    if (EnvInt("REPRO_STATUS_ONLY", 0) == 1)
        return 0;
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
    // REPRO_OS_ORDER=1: OptiScaler's real order - CreateContext, SetLatencyReduction, InitFromSwapChainDesc, no BuildPipelines
    // (the first build is the DLL's, started by InitFromSwapChainDesc), frames at once; the run does not wait for the generator
    const bool osOrder = EnvInt("REPRO_OS_ORDER", 0) == 1;
    if (osOrder)
        printf("SetLatencyReduction (OptiScaler order): %d\n", xefgSwapChainSetLatencyReduction(h, xell));
    QueryPerformanceCounter(&qa);
    printf("BuildPipelines: %d (took %.1f ms on this thread)\n", osOrder ? -999 : xefgSwapChainD3D12BuildPipelines(h, nullptr, 1, initFlags),
           [&] { QueryPerformanceCounter(&qb); return 1000.0 * static_cast<double>(qb.QuadPart - qa.QuadPart) / static_cast<double>(qf.QuadPart); }());
    // REPRO_DESTROY_EARLY=1: the context is destroyed while the DLL is still building (the pack, then the first build), a second
    // one is made and must come up (status 1): destroying a context during the background build is clean
    if (EnvInt("REPRO_DESTROY_EARLY", 0) == 1)
    {
        Sleep(static_cast<DWORD>(EnvInt("REPRO_DESTROY_DELAY", 0)));       // REPRO_DESTROY_DELAY=ms: into the first build too
        LARGE_INTEGER d0, d1;
        QueryPerformanceCounter(&d0);
        const int drc = xefgSwapChainDestroy(h);
        QueryPerformanceCounter(&d1);
        printf("destroy while building (after %d ms): %d, Destroy took %.1f ms\n", EnvInt("REPRO_DESTROY_DELAY", 0), drc,
               1000.0 * static_cast<double>(d1.QuadPart - d0.QuadPart) / static_cast<double>(qf.QuadPart));
        h = nullptr;
        printf("second CreateContext: %d\n", xefgSwapChainD3D12CreateContext(g_dev, &h));
        printf("second BuildPipelines: %d\n", xefgSwapChainD3D12BuildPipelines(h, nullptr, 1, initFlags));
        char reason[256] = {};
        int st = getStatus(reason, static_cast<int>(sizeof(reason)));
        for (int i = 0; i < 6000 && st == 0; ++i)
        {
            Sleep(5);
            st = getStatus(reason, static_cast<int>(sizeof(reason)));
        }
        printf("f5lowGetStatus after the second context: %d \"%s\"\n", st, reason);
        printf("destroy: %d\n", xefgSwapChainDestroy(h));
        return st == 1 ? 0 : 1;
    }
    // The DLL builds its frame generator in the background (status 0, then 1): REPRO_WAIT_READY=0 starts at once, the frames pass
    // through until it is ready; the default waits, so the pictures compared do not depend on the machine's speed.
    {
        char reason[256] = {};
        int st = getStatus ? getStatus(reason, static_cast<int>(sizeof(reason))) : -1;
        printf("f5lowGetStatus after BuildPipelines: %d \"%s\"\n", st, reason);
        QueryPerformanceCounter(&qa);
        while (st == 0 && EnvInt("REPRO_WAIT_READY", osOrder ? 0 : 1) == 1)
        {
            QueryPerformanceCounter(&qb);
            if ((qb.QuadPart - qa.QuadPart) / qf.QuadPart > 30)
                break;
            Sleep(5);
            st = getStatus ? getStatus(reason, static_cast<int>(sizeof(reason))) : -1;
        }
        QueryPerformanceCounter(&qb);
        printf("f5lowGetStatus ready: %d \"%s\" (waited %.0f ms after BuildPipelines)\n", st, reason,
               1000.0 * static_cast<double>(qb.QuadPart - qa.QuadPart) / static_cast<double>(qf.QuadPart));
        if (st == 0 && EnvInt("REPRO_WAIT_READY", osOrder ? 0 : 1) == 1)
        {
            printf("f5lowGetStatus check FAILED: the own generator did not become ready\n");
            return 1;
        }
    }

    WNDCLASSA wc {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "repro_test";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("repro_test", "repro_test", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);
    // REPRO_FRONT=1: keep the window in front (DLSS-G generates only for the foreground window); a synthetic Alt press lets a
    // background process take the focus
    const bool keepFront = EnvInt("REPRO_FRONT", 0) == 1;
    INPUT alt[2] {};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    unsigned refocus = 0;
    auto takeFront = [&] {
        if (keepFront && GetForegroundWindow() != wnd)
        {
            ++refocus;
            SendInput(2, alt, sizeof(INPUT));
            SetForegroundWindow(wnd);
        }
    };
    takeFront();
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
    Fill(cl, mv, 4, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, [bar](UINT, UINT, uint8_t* t) {
        const uint16_t v[2] = { Half(bar ? -8.0f : -4.0f), Half(0.0f) }; // a steady pan, 4 px per frame (REPRO_BAR: 8 render px = the bar's 12 output px)
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
    unsigned hist[16] {};       // REPRO_HIST: app frames by the number of frames presented for them
    xefg_swapchain_frame_constant_data_t fc {};
    fc.motionVectorScaleX = fc.motionVectorScaleY = 1.0f;
    fc.frameRenderTime = 8.0f;
    for (int frame = 0; frame < framesOff + framesOn && !failures; ++frame)
    {
        const bool on = frame >= framesOff;
        // REPRO_TOGGLE: off again on the first frame, back on 10 frames later (OptiScaler's UpdateTarget), tags only while on.
        const bool toggledOff = toggle && frame > framesOff && frame < framesOff + 10;
        const int inOn = frame - framesOff, menu1 = framesOn / 4, menu2 = 5 * framesOn / 8;
        const bool inMenu = menuLen > 0 && ((inOn >= menu1 && inOn < menu1 + menuLen) || (inOn >= menu2 && inOn < menu2 + menuLen));
        if (menuLen > 0 && (inOn == menu1 || inOn == menu2 || inOn == menu1 + menuLen || inOn == menu2 + menuLen))
            printf("frame %d: %s the menu\n", frame, inMenu ? "into" : "out of");
        if (frame == framesOff || (toggle && frame == framesOff + 10))
            printf("frame %d: SetEnabled 1: %d\n", frame, xefgSwapChainSetEnabled(h, 1));
        if (frame % 30 == 0)
        {
            for (MSG msg; PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE);)
                DispatchMessageA(&msg);
            takeFront();
        }
        LARGE_INTEGER w0, w1;
        QueryPerformanceCounter(&w0);
        if (wait)
            WaitForSingleObject(wait, 1000);
        QueryPerformanceCounter(&w1);
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
        const UINT barX = static_cast<UINT>(frame * 12) % sd.Width;
        Fill(cl, bb, 4, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_PRESENT, [frame, bar, barX](UINT x, UINT y, uint8_t* t) {
            if (bar)
            {
                const bool in = x >= barX && x < barX + 120;
                t[0] = in ? 230 : 26;
                t[1] = in ? 204 : 30;
                t[2] = in ? 51 : 38;
                t[3] = 255;
                return;
            }
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
        if (on && !toggledOff && !inMenu && !lateTag)
            tag();
        cl->Close();
        g_q->ExecuteCommandLists(1, ls);
        if (noIdle)
        {
            g_q->Signal(g_fence, ++g_fv);
            ringFence[ri] = g_fv;
            ringUploads[ri].swap(g_uploads);
        }
        if (on && !toggledOff && !inMenu && lateTag)
            tag();
        if (xellMarker)
            xellMarker(xell, static_cast<uint32_t>(frame), 3); // render submit end
        if (xefgSwapChainSetPresentId && !inMenu)
            xefgSwapChainSetPresentId(h, static_cast<uint32_t>(frame));
        if (!inMenu)
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
        const HRESULT hr = sc->Present(0, fsOn ? 0 : DXGI_PRESENT_ALLOW_TEARING);     // no tearing flag in exclusive fullscreen
        if (paceLog && on)
        {
            LARGE_INTEGER p1;
            QueryPerformanceCounter(&p1);
            const double k = 1000.0 / static_cast<double>(qf.QuadPart);
            appRecs.push_back({ frame, static_cast<double>(p0.QuadPart) * k, static_cast<double>(p1.QuadPart) * k, static_cast<double>(w1.QuadPart - w0.QuadPart) * k });
        }
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
        // REPRO_FS=1: exclusive fullscreen from a third of the frames on until two thirds (SetFullscreenState, then ResizeBuffers as a
        // game does on the resulting WM_SIZE)
        if (fsTest && (frame == framesOff + framesOn / 3 || frame == framesOff + 2 * framesOn / 3))
        {
            const bool enter = frame == framesOff + framesOn / 3;
            if (enter)
            {
                SetForegroundWindow(wnd);       // DXGI refuses (NOT_CURRENTLY_AVAILABLE) for a window that is not the foreground one
                SetFocus(wnd);
            }
            for (MSG msg; PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE);)
            {
                TranslateMessage(&msg);
                DispatchMessageA(&msg);
            }
            const HRESULT fr = sc->SetFullscreenState(enter ? TRUE : FALSE, nullptr);
            fsOn = enter && SUCCEEDED(fr);
            printf("frame %d: SetFullscreenState(%d): 0x%08lx\n", frame, enter ? 1 : 0, static_cast<unsigned long>(fr));
            const HRESULT rr = sc->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, sd.Flags);
            printf("frame %d: ResizeBuffers after it: 0x%08lx\n", frame, static_cast<unsigned long>(rr));
        }
        // REPRO_RECREATE_AT=n: a swap chain recreate at frame n (destroy the context, make a new one the way a game's resolution or
        // mode change does), while the first build may still be running
        if (EnvInt("REPRO_RECREATE_AT", -1) == frame)
        {
            Idle();
            LARGE_INTEGER c0, c1;
            QueryPerformanceCounter(&c0);
            sc->Release();
            const int drc = xefgSwapChainDestroy(h);
            QueryPerformanceCounter(&c1);
            h = nullptr;
            if (char gen[32]; GetEnvironmentVariableA("REPRO_RECREATE_GEN", gen, sizeof(gen)))
            {
                SetEnvironmentVariableA("F5LOW_GENERATOR", gen);
                printf("frame %d: F5LOW_GENERATOR=%s for the new context\n", frame, gen);
            }
            const int crc = xefgSwapChainD3D12CreateContext(g_dev, &h);
            xefgSwapChainSetLatencyReduction(h, xell);
            const int irc = xefgSwapChainD3D12InitFromSwapChainDesc(h, wnd, &sd, nullptr, g_q, fac, &ip);
            sc = nullptr;
            if (irc != 0 || xefgSwapChainD3D12GetSwapChainPtr(h, IID_PPV_ARGS(&sc)) != 0 || !sc)
            {
                printf("frame %d: recreate FAILED (%d %d)\n", frame, crc, irc);
                return 1;
            }
            wait = sc->GetFrameLatencyWaitableObject();
            xefgSwapChainSetEnabled(h, 1);
            xefgSwapChainSetNumInterpolatedFrames(h, maxInterp);
            printf("frame %d: swap chain recreated (Destroy %.1f ms)\n", frame,
                   1000.0 * static_cast<double>(c1.QuadPart - c0.QuadPart) / static_cast<double>(qf.QuadPart));
            (void)drc;
        }
        if (!noIdle)
        {
            LARGE_INTEGER i0, i1;
            QueryPerformanceCounter(&i0);
            Idle();
            QueryPerformanceCounter(&i1);
            if (paceLog && on && !appRecs.empty() && appRecs.back().frame == frame)
                appRecs.back().idleMs = static_cast<double>(i1.QuadPart - i0.QuadPart) * 1000.0 / static_cast<double>(qf.QuadPart);
        }
        if (appFps > 0)
        {
            // REPRO_FPS=n: the app produces n frames per second, not as fast as the GPU lets it. The scenarios (REPRO_RAMP, _CAPSTEP,
            // _HITCH, _JITTER) change the rate over the frames on.
            static LARGE_INTEGER due {};
            LARGE_INTEGER nowT, fq;
            QueryPerformanceFrequency(&fq);
            QueryPerformanceCounter(&nowT);
            if (on && !onStart.QuadPart)
                onStart = nowT;
            double fps = appFps;
            if (scn.rampSecs > 0)
            {
                const double s = on ? static_cast<double>(nowT.QuadPart - onStart.QuadPart) / static_cast<double>(fq.QuadPart) : 0.0;
                fps = scn.rampA + (scn.rampB - scn.rampA) * (std::min)(1.0, s / scn.rampSecs);
            }
            if (scn.stepFrame >= 0 && on && inOn >= scn.stepFrame && scn.stepFps > 0)
                fps = scn.stepFps;
            if (!due.QuadPart || nowT.QuadPart - due.QuadPart > fq.QuadPart / 4)
                due = nowT;
            due.QuadPart += static_cast<LONGLONG>(static_cast<double>(fq.QuadPart) / (std::max)(fps, 1.0));
            if (scn.hitchFrame >= 0 && on && inOn == scn.hitchFrame)
                due.QuadPart += static_cast<LONGLONG>(scn.hitchMs * static_cast<double>(fq.QuadPart) / 1000.0);
            // the jitter moves this frame's wake-up, not the schedule: no drift
            LONGLONG jit = 0;
            if (scn.jitterMs > 0 && on)
            {
                jitterState ^= jitterState << 13;
                jitterState ^= jitterState >> 17;
                jitterState ^= jitterState << 5;
                jit = static_cast<LONGLONG>((static_cast<double>(jitterState) / 4294967295.0 * 2.0 - 1.0) * scn.jitterMs * static_cast<double>(fq.QuadPart) / 1000.0);
            }
            for (;;)
            {
                QueryPerformanceCounter(&nowT);
                const double left = static_cast<double>(due.QuadPart + jit - nowT.QuadPart) * 1000.0 / static_cast<double>(fq.QuadPart);
                if (left <= 0)
                    break;
                if (left > 2.0)
                    Sleep(1);
                else
                    YieldProcessor();
            }
        }
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
        if (on && !inMenu && EnvInt("REPRO_HIST", 0) == 1)
        {
            xefg_swapchain_present_status_t ps {};
            xefgSwapChainGetLastPresentStatus(h, &ps);
            ++hist[std::min<uint32_t>(ps.framesPresented, 15)];
        }
        if (on && (frame == framesOff + 1 || frame == framesOff + framesOn - 1))
        {
            xefg_swapchain_present_status_t st {};
            xefgSwapChainGetLastPresentStatus(h, &st);
            printf("frame %d: presented %u, frame generation on %u\n", frame, st.framesPresented, st.isFrameGenEnabled);
        }
    }
    if (paceLog)
        for (const AppRec& ar : appRecs)
            printf("app: frame=%d t=%.3f ret=%.3f wait=%.3f idle=%.3f\n", ar.frame, ar.t0, ar.t1, ar.waitMs, ar.idleMs);
    if (EnvInt("REPRO_HIST", 0) == 1)
    {
        printf("frames presented per app frame (while on):");
        for (int i = 0; i < 16; ++i)
            if (hist[i])
                printf("  %d: %u", i, hist[i]);
        printf("; the window had to take the foreground back %u times\n", refocus);
    }
    printf("%d frames off, %d on: %s\n", framesOff, framesOn, failures ? "FAILED" : "no Present failure, device alive");
    {
        char reason[256] = {};
        const int st = getStatus ? getStatus(reason, static_cast<int>(sizeof(reason))) : -1;
        printf("f5lowGetStatus at the end: %d \"%s\"\n", st, reason);
    }

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
