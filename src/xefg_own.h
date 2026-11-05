// xefg_own.h - the native (non-forwarding) implementation behind f5low.dll's exports (built from xefg_cuda.cpp).
//
// One Own per xefg_swapchain handle. It runs the graph pack (tools/cuda/xefg_pack.h: the 38-dispatch XeFG
// network) on the application's frames and presents the generated frames between them: 2x to 6x
// (numInterp 1..5). Two ways to run the pack (Path):
//   d3d12  (default) inside our own command lists through NVAPI cubins (tools/cuda/xefg_pack_d3d.h): no
//          CUDA context, second CUDA queue or fence at run time. CUDA only compiles the kernels once.
//   cuda   CUDA graphs on a CUDA stream over buffers shared with D3D12 (xefg_interop.h), handed over with a
//          shared fence each way. Used when NVAPI lacks the cubin entry points.
//
// Presentation is asynchronous, as in Intel's own library: the application never waits for generated frames.
//   - The application renders into our own backbuffers (the Proxy's GetBuffer / GetCurrentBackBufferIndex);
//     the real swap chain lives on our own present queue (InitFromSwapChainDesc) or the application's
//     (InitFromSwapChain).
//   - TagFrameResource copies motion/depth (and the HUD-less colour and UI texture) into one of two staging
//     sets, so the next frame's tags cannot overwrite inputs still in use: a tag valid only now on the application's
//     own command list, one valid until the next present (as Intel, the list unused) at Present on a list of ours on
//     the application's queue.
//   - Present only signals the application's queue, makes it wait (GPU side) until the presenter has taken
//     the frame before last, and hands the frame to the presenter thread. At most two frames wait there.
//   - The presenter thread waits (GPU side) for the frame, takes its inputs, generates numInterp frames
//     at t = k / (n + 1) and presents them, paced evenly across the application's frame interval, then
//     the application's frame. cur becomes prev.
//   - CUDA path: all of a frame's passes go to the stream at once, each into a ring buffer of its own (two sets,
//     by frame parity), and the next application frame is taken as soon as it is done on the GPU and this one's
//     passes have run (HandOff, Early) - not after this one's last present - so its first generated frame does
//     not start late on a GPU the game keeps busy. Pacing (Pacer=): the clock (default) releases each frame one
//     spacing after the last, a delay behind the GPU timeline that follows when the stream finished the generated
//     frames (ClockTarget, ClockAfter); the schedule (Pacer=schedule; always on the D3D12 path) sets each app frame's
//     presents from its ready stamp on a clock of its own (Pick), behind it by as much as frames came out late (paceLag).
//     Gate=1 (clock): the clock paces on the GPU instead (ShowGated, PresentGenGated on the D3D12 path): the real swap
//     chain lives on a present lane of its own, each Present goes out at once behind a fence the release thread signals
//     at the frame's time.
// UI as Intel handles it (tools/XEFG_SETTINGS.md): with UI composition off everything is interpolated, UI
// included, and HUD-less colour / UI texture are ignored. With it on, the mode (initParams.uiMode; AUTO picks
// by what was tagged) decides: HUD-less colour is what gets interpolated (modes 3-5) and the output layer puts
// the UI back from the UI texture and/or the back buffer. A mode whose texture is missing generates nothing.
// Letterboxing: the back buffer tag's region is the interpolation area (the pack's AX AY AW AH, tools/pack_letterbox.py);
// a generated frame shows the application's frame around it, as Intel copies the borders in.
#pragma once

#include "generator.h"
#include "generators/own.h"
#include "generators/stubs.h"
#include "pace_lead.h"
#include "oob_driver.h"
#include "pace_sched.h"
#include "xefg_interop.h"
#include "xefg_convert_cs.h"
#include "xefg_formats.h"

#include <dxgi1_6.h>

#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <deque>
#include <functional>
#include <thread>
#include <map>
#include <memory>
#include <mutex>
#include <optional>

#define XEFG_SWAPCHAIN_EXPORT_API
#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>

void XLog(const char* fmt, ...);

namespace xefgown
{
// Contexts whose driver signals (DriverFg) are on: xefgCudaDriverMultipliesReflexCap answers 1 while there is one.
inline std::atomic<int> g_driverSignals { 0 };

using namespace xefgio;

// The back buffers XeFG takes: R8G8B8A8 (the pack's own), B8G8R8A8 (the same graph, red and blue swapped on
// the way in and out) and R10G10B10A2 HDR10 (the pack's hdr\ part). FP16 / scRGB Intel refuses too.
// The colour family of a 4-byte format: 1 R8G8B8A8, 2 B8G8R8A8, 3 R10G10B10A2, 0 none of them (motion vectors, depth).
inline int ColourFamily(DXGI_FORMAT f)
{
    switch (f)
    {
    case 27: case 28: case 29: return 1;
    case 87: case 90: case 91: return 2;
    case 23: case 24: case 25: return 3;
    default: return 0;
    }
}
// Motion and depth formats (xefg_formats.h: MotionTake / DepthTake, the formats Intel's library reads, measured with repro_test) that the
// pack's buffers do not hold as they are are copied raw and turned into R16G16_FLOAT / 32-bit float depth on our list (xefg_convert.hlsl,
// ConvertIn); the integer ones and anything else are refused (CopyIn).

// One tagged input's conversion for a frame (ConvertIn); kind < 0: copied as it is.
struct Conversion
{
    int32_t kind = -1;
    uint32_t w = 0, h = 0, srcPitch = 0, dstPitch = 0, stride = 0;
    uint32_t srcX = 0, srcY = 0;   // the region's offset in the raw copy (the whole texture)
    ID3D12Resource* src = nullptr; // the raw copy (stRaw; an outgrown one stays alive in stRawOld until the staging goes)
};

inline bool IsBgra(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_B8G8R8A8_UNORM || f == DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
}
inline bool IsHdr10(DXGI_FORMAT f)
{
    return f == DXGI_FORMAT_R10G10B10A2_UNORM;
}

// The other view format a swap chain buffer of this format allows (UNORM <-> SRGB), or UNKNOWN.
inline DXGI_FORMAT SrgbTwin(DXGI_FORMAT f)
{
    switch (f)
    {
    case DXGI_FORMAT_R8G8B8A8_UNORM: return DXGI_FORMAT_R8G8B8A8_UNORM_SRGB;
    case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_UNORM: return DXGI_FORMAT_B8G8R8A8_UNORM_SRGB;
    case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: return DXGI_FORMAT_B8G8R8A8_UNORM;
    default: return DXGI_FORMAT_UNKNOWN;
    }
}

class Own;

// IDXGISwapChain4 the app gets: our backbuffers and Present, everything else goes to the real swap chain.
class Proxy : public IDXGISwapChain4
{
public:
    Proxy(std::shared_ptr<Own> o, IDXGISwapChain4* inner) : own(std::move(o)), sc(inner) {}
    ~Proxy();

    // IUnknown
    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
    {
        if (!pp)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
            riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4))
        {
            *pp = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        // Anything else is the real swap chain's: an interface handed out here keeps it alive, so it is logged (a
        // reference still held at Destroy keeps the window's swap chain taken).
        const HRESULT hr = Fwd([&](IDXGISwapChain4* rsc) { return rsc->QueryInterface(riid, pp); });
        if (SUCCEEDED(hr))
            XLog("swap chain: QueryInterface {%08lX-%04X-%04X-...} handed out the real swap chain's interface", riid.Data1, riid.Data2,
                 riid.Data3);
        return hr;
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --refs;
        if (!n)
            delete this;
        return n;
    }

    // IDXGIObject
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetPrivateData(g, n, d); }); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetPrivateDataInterface(g, u); }); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetPrivateData(g, n, d); }); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** p) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetParent(r, p); }); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** p) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetDevice(r, p); }); }

    // IDXGISwapChain
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override;
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT b, REFIID r, void** p) override;
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* o) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetFullscreenState(f, o); }); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetFullscreenState(f, o); }); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetDesc(d); }); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override;
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* d) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->ResizeTarget(d); }); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetContainingOutput(o); }); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetFrameStatistics(s); }); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetLastPresentCount(c); }); }

    // IDXGISwapChain1
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetDesc1(d); }); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetFullscreenDesc(d); }); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetHwnd(h); }); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID r, void** p) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetCoreWindow(r, p); }); }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) override;
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override
    {
        IDXGISwapChain4* s = Hold();
        const BOOL b = s && s->IsTemporaryMonoSupported();
        if (s)
            s->Release();
        return b;
    }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetRestrictToOutput(o); }); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetBackgroundColor(c); }); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetBackgroundColor(c); }); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetRotation(r); }); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetRotation(r); }); }

    // IDXGISwapChain2
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetSourceSize(w, h); }); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetSourceSize(w, h); }); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT m) override;
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* m) override;
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override;
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetMatrixTransform(m); }); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->GetMatrixTransform(m); }); }

    // IDXGISwapChain3
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override;
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->CheckColorSpaceSupport(c, s); }); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetColorSpace1(c); }); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT*, IUnknown* const*) override
    {
        return ResizeBuffers(c, w, h, f, fl);
    }

    // IDXGISwapChain4
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* d) override { return Fwd([&](IDXGISwapChain4* rsc) { return rsc->SetHDRMetaData(t, s, d); }); }

    // The context is destroyed while the app still holds this swap chain (Own::Retire): let go of the real one, so
    // the window takes a new swap chain; calls that reach it from now on fail with DXGI_ERROR_INVALID_CALL.
    void LetGo()
    {
        std::lock_guard<std::mutex> l(scMu);
        if (sc)
            sc->Release();
        sc = nullptr;
    }

    std::shared_ptr<Own> own;

private:
    // The real swap chain for one forwarded call, with a reference of its own (LetGo may drop ours meanwhile).
    IDXGISwapChain4* Hold()
    {
        std::lock_guard<std::mutex> l(scMu);
        if (sc)
            sc->AddRef();
        return sc;
    }
    template <class F> HRESULT Fwd(F f)
    {
        IDXGISwapChain4* rsc = Hold();
        if (!rsc)
            return DXGI_ERROR_INVALID_CALL;
        const HRESULT hr = f(rsc);
        rsc->Release();
        return hr;
    }

    std::mutex scMu;
    IDXGISwapChain4* sc;    // the real swap chain (the reference InitFromSwapChain* handed over); null after LetGo
    std::atomic<ULONG> refs { 1 };
};

class Own : public std::enable_shared_from_this<Own>
{
public:
    // Frame constants and settings from the API.
    xefg_swapchain_frame_constant_data_t constants {};
    uint32_t initFlags = 0, maxInterp = 1, numInterp = 1;
    bool enabled = false;
    std::atomic<bool> uiComposition { false };  // SetUiCompositionState (from any of the application's threads)
    float sceneThreshold = 0.7f;        // SetSceneChangeThreshold; Intel's default
    xefg_swapchain_d3d12_init_params_t initParams {};
    std::atomic<uint32_t> framesPresented { 0 }, lastCallPresented { 0 };
    std::mutex proxyMu;                                   // proxy's hand-over between Retire and ~Proxy
    std::atomic<IDXGISwapChain4*> proxy { nullptr };      // the Proxy handed to the app (not owned)
    bool lastGenerated = false;

    // XeLL told about our frames as Intel's XeFG tells it (captured 2026-10-28 from Intel 1.3.4.95 in NBA 2K27 and GTA V):
    // SetFgEnabled with every SetEnabled; SetDisplayInfo on the first present after one; on each present while enabled
    // the frame of the game's last PRESENT_START marker, the two out-of-band markers around our submission and the
    // number of frames generated for it. XeLL's cap then counts shown frames, as with Intel's (R14). Without it (no
    // handle, no libxell loaded) XeLL counts the game's frames only, and xefgCudaXellCountsRenderedOnly says so (XL23).
    struct XellLink
    {
        void* ctx = nullptr;
        int32_t (*setFgEnabled)(void*, uint32_t, uint32_t) = nullptr;
        int32_t (*setGeneratedFramesCount)(void*, uint32_t, uint32_t) = nullptr;
        int32_t (*setDisplayInfo)(void*, void*) = nullptr;
        int32_t (*getLastPresentStartFrameId)(void*, uint32_t*) = nullptr;
        int32_t (*addMarkerData)(void*, uint32_t, uint32_t) = nullptr;
        int32_t (*setAppQueue)(void*, ID3D12CommandQueue*) = nullptr;  // optional (older libxell lacks it)
    };
    XellLink xell;                          // under mu
    std::atomic<bool> xellLinked { false };
    // XeLL takes at most 3 generated frames per rendered one (Intel's XeFG never makes more; it refuses 5 with -4,
    // repro_test 2026-10-28). Above that it is not told frame generation is on and counts the game's own frames.
    static constexpr uint32_t kXellMaxGenerated = 3;
    std::atomic<bool> xellCountsShown { false };  // XeLL told about our frames (xefgCudaXellCountsRenderedOnly: 0)
    bool xellFgOn = false;                  // under mu: what XeLL was last told by SetFgEnabled
    // Under mu: the motion vectors / depth the application tags cannot be used (a format we refuse, a size that does not fit the pack):
    // nothing is generated for them, so XeLL is told frame generation is off until usable ones arrive again. Told "on" with 0 generated
    // frames it paces the game for frames that never come (a game's frame rate dropped by about half). Decided once per frame at
    // Present (StepInputs), from what the frame's tags did: taggedMv / taggedDepth (a tag arrived), haveMv / haveDepth (one was taken).
    xefgown::InputsState inputs;
    bool taggedMv = false, taggedDepth = false;
    bool xellInputsRefused = false;         // what XellSync last used of inputs.refused
    // XeLL calls are decided under mu and sent after it; a sequence number says which came last, and the send mutex (never held
    // together with a call into libxell from under mu) drops a call that a later one has overtaken.
    uint64_t xellSeq = 0;                   // under mu
    std::mutex xellSendMu;
    uint64_t xellSentSeq = 0;               // under xellSendMu
    uint32_t xellMaxGen = 0;                // under mu: the generated frames per rendered one XeLL was told with it
    bool xellDisplayDirty = true;           // under mu: SetDisplayInfo due on the next present
    std::atomic<uint32_t> xellCountFailed { 0xffffffffu };
    std::atomic<bool> xellReported { false }, xellIdFailed { false };  // logged once each

    // A SetFgEnabled decided under mu, made after it (a call into libxell never holds our lock, as in OnPresent).
    // Intel's XeFG gives XeLL the application's queue right before every SetFgEnabled (null while it has none or the
    // context goes; Cyberpunk 2077 capture 2026-10-28): without it XeLL's cap does not count our frames. The second
    // argument of SetFgEnabled is XeLL's maxGeneratedFramesCount (its own log names it): with 1 XeLL caps 3x and 4x as
    // 2x (Cyberpunk 2077, 3x at cap 60 gave 30/90; Intel's 2x captures carried 1).
    struct XellCall
    {
        XellLink x;
        int on = -1;    // -1: nothing to tell
        ID3D12CommandQueue* queue = nullptr;    // the application's queue (held by Own for its lifetime) or null
        uint32_t maxGen = 0;                    // generated frames per rendered one while on
        uint64_t seq = 0;                       // order of decision (under mu); a call older than one already sent is dropped
        std::mutex* sendMu = nullptr;
        uint64_t* sentSeq = nullptr;
        void Send() const
        {
            if (on < 0)
                return;
            std::unique_lock<std::mutex> lk;
            if (sendMu)
            {
                lk = std::unique_lock<std::mutex>(*sendMu);
                if (seq && sentSeq && seq <= *sentSeq)
                    return;
                if (seq && sentSeq)
                    *sentSeq = seq;
            }
            if (x.setAppQueue)
                x.setAppQueue(x.ctx, queue);
            x.setFgEnabled(x.ctx, static_cast<uint32_t>(on), on ? maxGen : 0);
        }
    };

    // xefgSwapChainSetLatencyReduction. libxell.dll looked up by name with no flags, as Intel's library does: OptiScaler
    // answers only that form with its own XeLL, and a game that ships its own libxell.dll (Cyberpunk 2077) would
    // otherwise give us that copy, which never sees OptiScaler's context. Then pinned by address: the handle stays valid
    // while we hold it.
    XellCall LinkXell(void* ctx)
    {
        std::lock_guard<std::mutex> lk(mu);
        XellCall off = XellOff();   // a context linked before is told first
        xell = {};
        xellLinked = false;
        xellCountsShown = false;
        HMODULE found = nullptr, m = nullptr;
        if (!ctx || !GetModuleHandleExW(0, L"libxell.dll", &found) || !found ||
            !GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                reinterpret_cast<LPCWSTR>(found), &m) ||
            !m)
        {
            XLog("xell: %s - XeLL counts the game's frames only", ctx ? "no libxell.dll loaded" : "no context given");
            return off;
        }
        XellLink x;
        x.ctx = ctx;
        x.setFgEnabled = reinterpret_cast<decltype(x.setFgEnabled)>(GetProcAddress(m, "xellSetFgEnabled"));
        x.setGeneratedFramesCount =
            reinterpret_cast<decltype(x.setGeneratedFramesCount)>(GetProcAddress(m, "xellSetGeneratedFramesCount"));
        x.setDisplayInfo = reinterpret_cast<decltype(x.setDisplayInfo)>(GetProcAddress(m, "xellSetDisplayInfo"));
        x.getLastPresentStartFrameId =
            reinterpret_cast<decltype(x.getLastPresentStartFrameId)>(GetProcAddress(m, "xellGetLastPresentStartFrameId"));
        x.addMarkerData = reinterpret_cast<decltype(x.addMarkerData)>(GetProcAddress(m, "xellAddMarkerData"));
        x.setAppQueue = reinterpret_cast<decltype(x.setAppQueue)>(GetProcAddress(m, "xellD3D12SetAppQueue"));
        if (!x.setFgEnabled || !x.setGeneratedFramesCount || !x.setDisplayInfo || !x.getLastPresentStartFrameId ||
            !x.addMarkerData)
        {
            XLog("xell: libxell.dll lacks the frame generation calls - XeLL counts the game's frames only");
            return off;
        }
        if (off.on >= 0)
            off.Send();             // the old context, before the new one is told anything (still under mu: rare)
        xell = x;
        xellLinked = true;
        char path[MAX_PATH] = {};
        GetModuleFileNameA(m, path, MAX_PATH);
        XLog("xell: linked to %p in %s - our generated frames are reported to XeLL as Intel's XeFG does (up to %u per "
             "rendered frame)", ctx, path, kXellMaxGenerated);
        return XellSync();
    }

    // Under mu, after SetEnabled, SetNumInterpolatedFrames, an init (maxInterp) or the link: XeLL is told frame
    // generation is on while it is and the count fits, and told again when the count changes; xellCountsShown
    // follows. The caller sends it after unlocking. Intel's library also reports every present while off (count 0);
    // XeLL ignores counts then, so we only report on.
    XellCall XellSync()
    {
        const uint32_t gen = std::max(1u, std::min(numInterp, maxInterp));
        const bool fits = gen <= kXellMaxGenerated;
        xellCountsShown = xellLinked && fits;
        const bool inputsOk = !inputs.refused;
        xellInputsRefused = inputs.refused;
        const bool on = xellLinked && fits && enabled && inputsOk;
        if (!xellLinked || (on == xellFgOn && (!on || gen == xellMaxGen)))
            return {};
        xellFgOn = on;
        xellMaxGen = on ? gen : 0;
        xellDisplayDirty = true;
        XLog("xell: frame generation %s for XeLL%s", on ? "on" : "off",
             !fits && enabled ? " (more generated frames than XeLL takes: it counts the game's own)"
             : !inputsOk && enabled ? " (the tagged motion vectors / depth cannot be used: no frames are generated)" : "");
        if (on)
            XLog("xell: up to %u generated per rendered frame", gen);
        return Stamp({ xell, on ? 1 : 0, appQueue, gen });
    }

    // Under mu: a decided XeLL call gets its place in the order.
    XellCall Stamp(XellCall c)
    {
        c.seq = ++xellSeq;
        c.sendMu = &xellSendMu;
        c.sentSeq = &xellSentSeq;
        return c;
    }

    // Under mu, when the context goes (Destroy) or links anew: XeLL is not left counting frames we no longer make
    XellCall XellOff()
    {
        if (!xellLinked || !xellFgOn)
            return {};
        xellFgOn = false;
        xellMaxGen = 0;
        XLog("xell: frame generation off for XeLL (context gone)");
        return Stamp({ xell, 0 });
    }

    // The display's refresh period in ns, as Intel's SetDisplayInfo carries it (1e9 / Hz: 5555556 at 180 Hz); 0 unknown
    uint64_t RefreshNs()
    {
        IDXGIOutput* output = nullptr;
        uint32_t hz = 0;
        if (real && SUCCEEDED(real->GetContainingOutput(&output)) && output)
        {
            DXGI_OUTPUT_DESC od {};
            DEVMODEW dm {};
            dm.dmSize = sizeof(dm);
            if (SUCCEEDED(output->GetDesc(&od)) && EnumDisplaySettingsW(od.DeviceName, ENUM_CURRENT_SETTINGS, &dm))
                hz = dm.dmDisplayFrequency;
            output->Release();
        }
        return hz > 1 ? (1000000000ull + hz / 2) / hz : 0;
    }
    bool dbgTag = false, dbgOnly = false;       // debug view: corner tags on generated frames; show only generated frames
    std::string genMode = "off";                // GenQueue= in f5low.ini: off, direct or compute (see PresentGen)
    std::string genPriority;                    // GenPriority= in f5low.ini: high or normal; empty: normal with the gate, else high
    uint32_t genSpread = 2;                     // GenSpread= in f5low.ini: the D3D12 gate's passes, steps apart (0: at once; PresentGenGated)
    bool noEarly = false;                       // Early=0 in f5low.ini: never take the next frame early (Early)
    double maxLag = 0.5;                        // MaxLag= in f5low.ini: the schedule's lag at most, in app frames (paceLag)
    bool clockPacer = true;                     // Pacer= in f5low.ini: clock (ClockTarget) or schedule (PaceTarget), CUDA path
    double maxDelay = 1.5;                      // MaxDelay= in f5low.ini: the clock's delay at most, in app frames (clkTarget)
    GenSettings gs;                             // Prof=1/2 (or XEFG_CUDA_PROF), Native=0 in f5low.ini: shared with the generator
    bool gate = false;                          // Gate= in f5low.ini: pace on the GPU (ShowGated / PresentGenGated), with the clock
    bool flipMetering = true;                   // FlipMetering=0 in f5low.ini: no NVIDIA driver flip metering (Meter)
    bool oobQueue = false;                      // OobQueue=1 in f5low.ini: tell NVIDIA's driver our queues run out of band (OobNotify)
    bool oobSwitch = false;                     // OobQueue=switch: present type for the app's frame, render type for a generated one (RealPresent)
    bool flipStats = false;                     // FlipStats=1 in f5low.ini (needs Log=1): the swap chain's own flip spacing, logged every 120 app frames
    bool flipBurst = false;                     // FlipBurst=1 in f5low.ini (prototype, Gate=0): metered frames after a batch's first go out at once (PresentReal)
    bool driverFg = false;                      // DriverFg=1 in f5low.ini (prototype): the whole set of driver signals (DriverFgFrame, RealPresent)
    uint32_t driverFgParts = 15;                // DriverFgParts= (prototype, sum): 1 Reflex Sync, 2 present markers, 4 out of band markers, 8 queue notices
    uint32_t appJobs = 2;                       // AppJobs= in f5low.ini (prototype): frames that may wait for the presenter before the game's Present blocks
    bool retireWait = true;                     // RetireWait=0 in f5low.ini (prototype): the game's Present does not wait for the frame before last on the GPU
    bool oobMarkers = false;                    // OobMarkers=1 in f5low.ini: out of band present markers around our Presents (RealPresent)
    std::atomic<uint32_t> presentId { 0 };      // xefgSwapChainSetPresentId: the frame id of the game's markers, taken at its Present
    uint32_t flipLeadUs = 2500;                 // FlipLeadUs= in f5low.ini (0-8000): metered frames after the first go this early
    bool flipLeadAuto = true;                   // FlipLeadUs=auto (default): flipLeadUs, but at most kFlipLeadShare of the frame spacing
    // PaceLead in f5low.ini (Target, D3D12 path only; xefg_cuda.cpp PaceLeadSetting): generated frame k goes out at base + lead + (k - 1)
    // steps instead of base + k steps. Generating takes a millisecond or two, so the first one need not wait a whole step. auto
    // (default): the lead adapts (pace_lead.h); a number of ms: fixed (0.5-50); 0/off: one full step as before. Not on the CUDA
    // path: there paceLag already moves the schedule when frames are late, and the clock pacer has its own delay.
    bool paceLeadAuto = true;
    double paceLeadFixedMs = 0;
    // PaceLog=1: one record per present, written to the log when the presenter stops (PaceDump; tests/pacer_ab.ps1).
    bool paceLog = false;
    // The gated swap chain: room for two app frames' presents at 6x. At 7 queued the presenter blocked on the waitable object
    // until the gate let old frames out, took the next app frame late (ready stamp 37 vs 14 ms old) and CUDA started late:
    // the generated frames came 22 ms later and the clock's delay followed (harness 6x at 30 fps).
    static constexpr UINT kGateBuffers = 8, kGateLatency = 16;

    // path: "cuda" for the CUDA path, anything else the D3D12 one (see the top of this file). generator: own, intel, dlssg or
    // fsr (xefg_cuda.cpp GeneratorSetting); one that is not available yet logs one line and runs own.
    static std::shared_ptr<Own> Create(ID3D12Device* dev, const std::string& packDir, const std::string& path,
                                       const std::string& generator = "own")
    {
        auto o = std::make_shared<Own>();
        o->dev = dev;
        o->packDir = packDir;
        std::unique_ptr<IGenerator> chosen = MakeStubGenerator(generator);
        const char* id = "own";
        if (chosen && !chosen->Caps().available)
        {
            XLog("f5low: generator %s not available yet, running own", generator.c_str());
            chosen.reset();
        }
        if (chosen)
        {
            id = generator.c_str();
            o->generator = std::move(chosen);
        }
        else
            o->generator = std::make_unique<OwnGenerator>(dev, packDir, path != "cuda", o->gs);
        GenHost h;
        h.user = o.get();
        h.stampDone = &Own::StampDone;
        h.stampsWork = &Own::StampsWork;
        h.waitingFor = &Own::WaitingFor;
        o->generator->SetHost(h);
        // The per-frame path drives the pack surface (own is the only generator that has one and runs).
        o->pg = o->generator->AsPack();
        if (!o->pg)
        {
            XLog("f5low: generator %s has no pack surface - the host cannot drive it, no context", id);
            return nullptr;
        }
        XLog("f5low: generator %s (%s)", id, o->generator->Caps().name);
        return o;
    }

    // The generator's CUDA host functions and diagnostics, back in the host: when the stream reached a fence value (ClockNeed).
    static void StampDone(void* u, UINT64 v)
    {
        Own* o = static_cast<Own*>(u);
        o->doneAt[v % kStamps].store(Now(), std::memory_order_relaxed);
        o->doneVal[v % kStamps].store(v, std::memory_order_release);
    }
    static void StampsWork(void* u) { static_cast<Own*>(u)->stampsWork = true; }
    static UINT64 WaitingFor(void* u) { return static_cast<Own*>(u)->gWaitingFor.load(); }

    bool D3d() const { return pg->InCommandLists(); }

    ~Own()
    {
        XLog("context freed");  // Destroy plus the app's last Release of its swap chain: nothing of it is left
        if (driverFgCounted)
            g_driverSignals.fetch_sub(1);
        if (driverFgQueue)
            driverFgQueue->Release();
        StopDeferred();
        StopPresenter();
        generator->Stop();
        ReleaseSwapChain();
        if (latencySem)
            CloseHandle(latencySem);
        latencySem = nullptr;
        // Our tag copies run on the application's queue: done once it got past the last frame they were in.
        for (const TagSlot& t : tagSlots)
            if (t.frame && appFence)
                WaitAppFence(t.frame);
        for (auto& t : deferredTag)
            if (t.pResource)
                t.pResource->Release();
        for (TagSlot& t : tagSlots)
        {
            for (ID3D12Resource* r : t.held)
                r->Release();
            t.alloc->Release();
        }
        for (IUnknown* u : std::initializer_list<IUnknown*> { tagList })
            if (u)
                u->Release();
        if (tagEvent)
            CloseHandle(tagEvent);
        ReleaseStaging();
        ReleaseRing();
        for (IUnknown* u : std::initializer_list<IUnknown*> { appFence, consumedFence, idleFence, retireFence, tagBuf,
                                                             genList, genFence, genQueue, convRs, convPso })
            if (u)
                u->Release();
        for (auto& a : genAlloc)
            if (a)
                a->Release();
        if (genEvent)
            CloseHandle(genEvent);
        if (retireEvent)
            CloseHandle(retireEvent);
        if (gpuEvent)
            CloseHandle(gpuEvent);
        generator->Shutdown();
        if (list)
            list->Release();
        for (auto& a : alloc)
            if (a)
                a->Release();
        if (hostFence)
            hostFence->Release();
        if (keep)
            keep->Release();
        if (hold)
            hold->Release();
        if (hostEvent)
            CloseHandle(hostEvent);
        for (IUnknown* u : std::initializer_list<IUnknown*> { gateHeld[0], gateHeld[1], pqList, pqFence, gateFence, contentFence, pq,
                                                              laneCopyHeap, laneCopyReadback })
            if (u)
                u->Release();
        for (auto& a : pqAlloc)
            if (a)
                a->Release();
        if (pqEvent)
            CloseHandle(pqEvent);
        if (queue)
            queue->Release();
        if (appQueue)
            appQueue->Release();
    }

    // Loads the pack, creates the CUDA context and the shared buffers, builds the graph.
    bool EnsureBuilt()
    {
        std::lock_guard<std::mutex> lk(mu);
        // A pending resize is the presenter's to rebuild (it takes no mu): building here too would rebuild the pack
        // under its feet.
        if (built || resizePending)
            return true;
        if (failed)
            return false;
        if (rebuildOnInit)
        {
            // The presenter stopped (a swap chain released or recreated) before it got to a pending resize.
            rebuildOnInit = false;
            RebuildForSizes("at init");
            if (built)
                return true;
            // Not at those sizes: the whole build, as a context built for the first time (its sizes followed later).
        }
        if (packPoll)
        {
            // The pack is made in the background (xefg_cuda.cpp) and so is this first build, which compiles the kernels
            // (about a second): not on the application's thread. The frames pass through until it is done.
            StartDeferredBuild();
            return true;
        }
        built = Build();
        failed = !built;
        return built;
    }

    // ---- deferred first build (a pack made in the background: nothing waits for it) ----------------------------------
    // Set by the DLL before the first EnsureBuilt; empty for a pack folder or the embedded pack, which build at once.
    // packPoll: 0 the pack is still being made, 1 it is ready, 2 it failed (`why` says why).
    std::function<int(std::string* why)> packPoll;
    // With a ready pack: "" when this GPU runs it, else why not (checked here because the pack was not ready at CreateContext).
    std::function<std::string()> packFit;
    // The deferred build is over (ok: generation can start; else why not): the DLL's status.
    std::function<void(bool ok, const std::string& why)> deferredDone;
    // The context was destroyed before its deferred build made it ready: the DLL takes back the status it announced (0, building).
    std::function<void()> onAbandoned;

    // Under mu, from the first EnsureBuilt (before the presenter exists: SetQueue comes before Wrap). The build runs on a thread
    // of its own and publishes with `built`: until then no thread but that one touches the generator (the presenter skips its
    // GenScope while deferBusy, the application's threads leave everything of the pack alone while !built).
    // Everything the thread reads that the application's threads can write (initFlags, the UI mode, the composition flag, the
    // swap chain's format, the dump settings) is copied here, under mu, before it starts. The settings the generator reads
    // (GenSettings gs: path, native, prof) are fixed before the context is returned; for the built pack CreateContext makes them
    // what its pack needs (D3D12 path, native modules), so the generator never has to write them.
    // The thread has a reference on the DLL of its own (FreeLibraryAndExitThread), like the pack thread.
    void StartDeferredBuild()
    {
        if (deferStarted)
            return;
        deferStarted = true;
        deferBusy = true;
        ReadDumpEnv();
        GenBuild args = BuildArgs();        // initFlags and the swap chain's format as a build right here would have had them
        args.releaseHostBuffers = nullptr;  // the first build has nothing to release (the presenter's state is not touched)
        const BuildSnap snap { initFlags, static_cast<int>(initParams.uiMode), uiComposition.load() };
        args.packBuilt = [this, snap] { return PackBuilt(&snap); };
        args.cancelled = [this] { return deferQuit.load(); };
        HMODULE self = nullptr;
        if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&Own::DeferredEntry), &self))
            self = nullptr;
        auto* ctx = new DeferCtx { this, args, self };
        deferThread = CreateThread(nullptr, 0, &Own::DeferredEntry, ctx, 0, nullptr);
        if (!deferThread)
        {
            if (self)
                FreeLibrary(self);
            delete ctx;
            deferBusy = false;
            failed = true;
            XLog("deferred first build: the thread could not be started");
        }
    }

    struct DeferCtx { Own* o; GenBuild args; HMODULE self; };
    static DWORD WINAPI DeferredEntry(void* p)
    {
        std::unique_ptr<DeferCtx> c(static_cast<DeferCtx*>(p));
        HMODULE self = c->self;
        c->o->DeferredBuild(c->args);
        c.reset();
        if (self)
            FreeLibraryAndExitThread(self, 0);
        return 0;
    }

    void DeferredBuild(const GenBuild& args)
    {
        std::string why;
        int st;
        while ((st = packPoll(&why)) == 0 && !deferQuit)
            Sleep(2);
        bool ok = false;
        if (deferQuit)
            why = "context destroyed";
        else if (st != 1)
            ;       // the pack could not be made: `why` is the DLL's reason
        else
        {
            const std::string misfit = packFit ? packFit() : std::string();
            if (!misfit.empty())
                why = misfit;
            else if (!(ok = Build(&args)))
                why = deferQuit ? "context destroyed" : "the pack did not build for D3D12 (see the log)";
        }
        // A window between the two stores: built is true while deferBusy still is, so an application thread may tag (copies into
        // the staging buffers, under mu) and the presenter may pick the frame up usable, before this thread has left. That is
        // harmless: from here this thread touches no generator state, only the flag and the callback.
        {
            std::lock_guard<std::mutex> lk(mu);
            failed = !ok;
            built = ok && !deferQuit;
            deferReady = ok && !deferQuit;
        }
        deferBusy = false;
        if (!deferQuit && deferredDone)
            deferredDone(ok, why);
        deferFinished = true;
    }

    // Before the generator goes: stops a running deferred build at its next step (the CUDA init, one kernel's compile, one pipeline)
    // and waits for it - a context destroyed in its first second waits for one step, not the whole compile. A build that waits for
    // the pack stops at once.
    void StopDeferred()
    {
        deferQuit = true;
        if (deferThread)
        {
            WaitForSingleObject(deferThread, INFINITE);
            CloseHandle(deferThread);
            deferThread = nullptr;
        }
        if (deferStarted && !deferReady && onAbandoned)
        {
            onAbandoned();
            onAbandoned = nullptr;
        }
    }

    // The queue the real swap chain presents on, for InitFromSwapChainDesc: our own, high priority, so
    // generated frames are not queued behind the application's next frame.
    // Gate=1: the present lane instead (CreatePresentLane); our queue then does the rest of the work.
    ID3D12CommandQueue* PresentQueue()
    {
        if (!queue)
        {
            D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH };
            if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&queue))))
                return nullptr;
        }
        if (!gate || !clockPacer)
            return queue;
        if (D3d() && genMode == "off")
            genMode = "direct";             // the D3D12 path's passes must not wait at the gate (PresentGenGated)
        if (!pq && !CreatePresentLane())
        {
            XLog("gate: no present lane - pacing on the CPU");
            return queue;
        }
        return pq;
    }

    // The gated swap chain's description: more buffers and the frame-latency waitable object (kGateLatency queued presents;
    // without it a D3D12 swap chain blocks at 3).
    void GateDesc(DXGI_SWAP_CHAIN_DESC1& d) const
    {
        if (!pq && !flipBurst)
            return;
        d.BufferCount = std::max(d.BufferCount, kGateBuffers);
        d.Flags |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;
    }

    // The present lane (Gate=1, ShowGated): a direct queue of its own, high priority, its lists, its fence and the gate's.
    bool CreatePresentLane()
    {
        D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT, D3D12_COMMAND_QUEUE_PRIORITY_HIGH };
        ID3D12CommandQueue* q = nullptr;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
            return false;
        bool ok = true;
        for (auto& a : pqAlloc)
            ok = ok && SUCCEEDED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a)));
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, pqAlloc[0], nullptr, IID_PPV_ARGS(&pqList)));
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&pqFence)));
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&gateFence)));   // CUDA waits on it (GateSem)
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&contentFence)));
        if (!ok)
        {
            q->Release();
            return false;
        }
        q->SetName(L"f5low present (gated)");
        pqList->SetName(L"f5low present lane");
        pqList->Close();
        pqEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        pq = q;
        InitLaneCopyProfiling();
        XLog("gate: present lane of its own, %u buffers or more, %u presents queued at most; the release thread paces on the GPU",
             kGateBuffers, kGateLatency);
        return true;
    }

    // Takes the application's queue (and, for InitFromSwapChain, presents on it), builds, creates our lists.
    bool SetQueue(ID3D12CommandQueue* q)
    {
        if (!appQueue)
        {
            appQueue = q;
            appQueue->AddRef();
        }
        if (!queue)
        {
            queue = q;
            queue->AddRef();
        }
        if (!EnsureBuilt())
            return false;
        std::lock_guard<std::mutex> lk(mu);
        if (list)
            return true;
        for (auto& a : alloc)
            if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a))))
                return false;
        if (FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&list))))
            return false;
        list->SetName(L"f5low present list");
        list->Close();
        if (FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hostFence))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&appFence))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&consumedFence))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&idleFence))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&retireFence))))
            return false;
        hostEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        retireEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        gpuEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (D3d() && genMode != "off" && !CreateGenLane())
            XLog("present: no generation queue (%s) - generating on the present queue", genMode.c_str());
        return true;
    }

    // The generation lane (D3D12 path): a queue of its own, high priority, compute (runs alongside the game's
    // graphics work) or direct, with its own lists and fence. False leaves it unset (PresentGen is not used).
    bool CreateGenLane()
    {
        const D3D12_COMMAND_LIST_TYPE t = genMode == "direct" ? D3D12_COMMAND_LIST_TYPE_DIRECT : D3D12_COMMAND_LIST_TYPE_COMPUTE;
        // Normal priority with the gate: the frame's passes go to the lane at once, and at high priority that block (~15 ms
        // in NBA 2K27) kept the compositor off the GPU - the generated frames released during it were dropped (run G: 39% /
        // 21% of the first two, 2-3% of the rest).
        const bool high = genPriority.empty() ? !pq : genPriority != "normal";
        D3D12_COMMAND_QUEUE_DESC qd { t, high ? D3D12_COMMAND_QUEUE_PRIORITY_HIGH : D3D12_COMMAND_QUEUE_PRIORITY_NORMAL };
        ID3D12CommandQueue* q = nullptr;
        if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q))))
            return false;
        bool ok = true;
        for (auto& a : genAlloc)
            ok = ok && SUCCEEDED(dev->CreateCommandAllocator(t, IID_PPV_ARGS(&a)));
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, t, genAlloc[0], nullptr, IID_PPV_ARGS(&genList)));
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&genFence)));
        if (!ok)
        {
            q->Release();
            return false;
        }
        q->SetName(L"f5low generation");
        genList->SetName(L"f5low generation list");
        genList->Close();
        genEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        genQueue = q;
        XLog("present: generation on its own %s queue (%s priority)", t == D3D12_COMMAND_LIST_TYPE_COMPUTE ? "compute" : "direct",
             high ? "high" : "normal");
        return true;
    }

    // The proxy's first reference is the context's (Retire drops it); GetSwapChainPtr hands the app its own.
    IDXGISwapChain4* Wrap(IDXGISwapChain4* inner)
    {
        IDXGISwapChain4* p = new Proxy(shared_from_this(), inner);
        proxy = p;
        std::lock_guard<std::mutex> lk(mu);
        real = inner;
        real->AddRef();
        DXGI_SWAP_CHAIN_DESC1 d {};
        real->GetDesc1(&d);
        if (latencySem)
            CloseHandle(latencySem); // a swap chain before this one on the context (ReleaseSwapChain leaves it)
        latencySem = nullptr;
        if (d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT)
        {
            // The application waits on ours (released as the presenter takes each frame); we wait on the
            // real one before each present so the real swap chain's queue stays short.
            latencySem = CreateSemaphoreW(nullptr, static_cast<LONG>(latency), 16, nullptr);
            real->SetMaximumFrameLatency(pq || flipBurst ? kGateLatency : 2);
            realWait = real->GetFrameLatencyWaitableObject();
        }
        XLog("swap chain: the real swap chain has %u buffers, flags 0x%x (frame latency waitable object: %s), maximum frame latency set: %s, wait handle: %s",
             d.BufferCount, d.Flags, d.Flags & DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT ? "yes" : "no",
             realWait ? (pq || flipBurst ? "16" : "2") : "no", realWait ? "yes" : "no");
        if (!CreateAppBuffers())
            XLog("swap chain: could not create our backbuffers - the application will fail to render");
        StartPresenter();
        return p;
    }

    // Copies a tagged resource into this frame's staging set on the application's command list.
    xefg_swapchain_result_t Tag(ID3D12CommandList* cl, const xefg_swapchain_d3d12_resource_data_t& rd)
    {
        if (rd.type == XEFG_SWAPCHAIN_RES_BACKBUFFER)
        {
            // The interpolation area; the whole output when the region is empty, the whole output or does not fit.
            std::lock_guard<std::mutex> lk(mu);
            const uint32_t x = rd.resourceBase.x, y = rd.resourceBase.y, w = rd.resourceSize.x, h = rd.resourceSize.y;
            const bool whole = !w || !h || (x == 0 && y == 0 && w == scDesc.Width && h == scDesc.Height);
            const bool fits = x + static_cast<uint64_t>(w) <= scDesc.Width && y + static_cast<uint64_t>(h) <= scDesc.Height;
            if (!whole && !fits && !warnedArea)
            {
                XLog("tag backbuffer: region %u,%u %ux%u outside the %ux%u back buffer - using the whole frame", x, y, w, h,
                     scDesc.Width, scDesc.Height);
                warnedArea = true;
            }
            const uint32_t a[4] = { x, y, w, h };
            for (int i = 0; i < 4; ++i)
                area[i] = whole || !fits ? 0 : a[i];
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        }
        if (!rd.pResource || !built)
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        const bool isColor = rd.type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        const bool isUi = rd.type == XEFG_SWAPCHAIN_RES_UI;
        const bool isMv = rd.type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR;
        const bool isDepth = rd.type == XEFG_SWAPCHAIN_RES_DEPTH;
        if (!isColor && !isUi && !isMv && !isDepth)
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        Removed("the application's work, at a tag");
        // Valid until the next present: as Intel, the list is not used. The resource is copied at Present on a list of
        // ours on the application's queue, in the state it was tagged with. The application's list may be closed or
        // already submitted by then (Streamline's eValidUntilPresent tags), and recording into it removed the device
        // with DXGI_ERROR_INVALID_CALL (tester logs 20 + 21).
        if (rd.validity == XEFG_SWAPCHAIN_RV_UNTIL_NEXT_PRESENT)
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!built)                 // again under mu: a tag on another thread may have set the pack aside
                return XEFG_SWAPCHAIN_RESULT_SUCCESS;
            FollowSizes(rd);
            ++tagsThisFrame;
            xefg_swapchain_d3d12_resource_data_t& t = deferredTag[rd.type];
            if (t.pResource)
                t.pResource->Release();
            t = rd;
            t.pResource->AddRef();
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        }
        ID3D12GraphicsCommandList* g = nullptr;
        if (!cl || FAILED(cl->QueryInterface(IID_PPV_ARGS(&g))))
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(mu);
        if (!built)
        {
            g->Release();
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        }
        FollowSizes(rd);
        ++tagsThisFrame;
        TagCopy(g, rd);
        g->Release();
        // Success whatever the format: Intel's library takes every tag the same way (repro_test, docs\GENERATORS.md); a tag we cannot use is
        // logged by CopyIn; at Present the frame's tags decide whether frames are generated and what XeLL is told (StepInputs).
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }

    // Copies a tagged resource into this frame's staging set on g (under mu).
    void TagCopy(ID3D12GraphicsCommandList* g, const xefg_swapchain_d3d12_resource_data_t& rd)
    {
        const bool isColor = rd.type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR;
        const bool isUi = rd.type == XEFG_SWAPCHAIN_RES_UI;
        const bool isMv = rd.type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR, isDepth = rd.type == XEFG_SWAPCHAIN_RES_DEPTH;
        const uint32_t set = static_cast<uint32_t>(appFrame & 1);
        // A UI texture only counts with a pack that has the UI inputs (tools/pack_ui.py).
        if (!built || !stMv[set] || (isColor && !EnsureStaging(stHud, pg->Buf(GenSlot::Cur).bytes)) ||
            (isUi && (!pg->HasUiInputs() || !EnsureStaging(stUi, pg->Buf(GenSlot::Ui).bytes))))
            return;
        ID3D12Resource* dst = isColor ? stHud[set] : isUi ? stUi[set] : isMv ? stMv[set] : stDepth[set];
        const Resource& pr = pg->InputRes(isColor ? GenSlot::Cur : isUi ? GenSlot::Ui : isMv ? GenSlot::Motion : GenSlot::Depth);
        const bool ok = CopyIn(g, rd, dst, pr, isColor ? "hudless" : isUi ? "ui" : isMv ? "motion" : "depth", set);
        if (ok)
            (isColor ? hudless : isUi ? haveUi : isMv ? haveMv : haveDepth) = true;
        // Only noted here; Present decides (a frame may tag twice, and one tag's refusal must not outvote another's success)
        if (isMv)
            taggedMv = true;
        else if (isDepth)
            taggedDepth = true;
    }

    // One allocator of the tag copies (FlushTags): its appFence value once submitted, the resources its copies read.
    struct TagSlot
    {
        ID3D12CommandAllocator* alloc = nullptr;
        UINT64 frame = 0;
        std::vector<ID3D12Resource*> held;
    };
    static constexpr uint32_t kTagSlots = 6;

    // The frame's "until the next present" tags, copied on our tag list on the application's queue (in OnPresent, under
    // mu, before the frame's appFence signal: after the application's work for the frame, before our presenter reads them).
    void FlushTags()
    {
        bool any = false;
        for (const auto& t : deferredTag)
            any = any || t.pResource;
        if (!any)
            return;
        if (!tagList && !tagListFailed)
        {
            bool ok = (tagEvent = CreateEventW(nullptr, FALSE, FALSE, nullptr)) != nullptr && NewTagSlot() != nullptr;
            ok = ok && SUCCEEDED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, tagSlots[0].alloc, nullptr,
                                                        IID_PPV_ARGS(&tagList)));
            if (ok)
            {
                tagList->SetName(L"f5low tag copies");
                tagList->Close();
            }
            else
            {
                XLog("tag: no command list of our own for tags valid until present - those tags are ignored");
                tagListFailed = true;
            }
        }
        // An allocator the application's queue is done with, else a new one: Intel's Present does not wait for the
        // GPU, and a game whose CPU runs frames ahead of its GPU would have waited here every frame. Only with
        // kTagSlots in flight (the queue that far behind) is the oldest waited for.
        TagSlot* slot = nullptr;
        if (tagList)
        {
            const UINT64 done = appFence->GetCompletedValue();
            for (TagSlot& t : tagSlots)
                if (t.frame <= done && (!slot || t.frame < slot->frame))
                    slot = &t;
            if (!slot && tagSlots.size() < kTagSlots)
                slot = NewTagSlot();
            if (!slot)
            {
                TagSlot* oldest = &tagSlots[0];
                for (TagSlot& t : tagSlots)
                    if (t.frame < oldest->frame)
                        oldest = &t;
                if (WaitAppFence(oldest->frame))
                    slot = oldest;
            }
        }
        if (slot)
        {
            for (ID3D12Resource* r : slot->held)
                r->Release();
            slot->held.clear();
            slot->alloc->Reset();
            tagList->Reset(slot->alloc, nullptr);
        }
        for (auto& t : deferredTag)
        {
            if (!t.pResource)
                continue;
            if (slot)
            {
                TagCopy(tagList, t);
                slot->held.push_back(t.pResource); // the reference stays until the copy is done on the GPU
            }
            else
                t.pResource->Release();
            t = {};
        }
        if (!slot)
        {
            if (tagList && !warnedTagWait)
                XLog("tag: the application's queue is %u frames behind - this frame's tags are dropped", kTagSlots);
            warnedTagWait = tagList != nullptr;
            return;
        }
        Submit(appQueue, tagList, "tag copies");
        slot->frame = appFrame + 1;
    }

    // Closes and executes one of our lists. A list whose Close failed (a command the driver refused while recording)
    // would remove the device with DXGI_ERROR_INVALID_CALL once executed: it is logged and left out instead. Then
    // Removed() notes the first time the device is seen removed (tester logs 20-21 + round 3: INVALID_CALL, DRED
    // saw no unfinished list and no page fault - an API call, not the GPU).
    bool Submit(ID3D12CommandQueue* q, ID3D12GraphicsCommandList* l, const char* what)
    {
        const HRESULT hr = l->Close();
        if (FAILED(hr))
        {
            if (closeFails++ < 8)
                XLog("submit %s: Close failed 0x%08x - the list is not executed", what, static_cast<unsigned>(hr));
            Removed(what);
            return false;
        }
        ID3D12CommandList* cl = l;
        q->ExecuteCommandLists(1, &cl);
        Removed(what);
        return true;
    }
    // The first time the device is seen removed, where (after which of our submissions, or at which call).
    void Removed(const char* where)
    {
        if (removedSeen.load(std::memory_order_relaxed) || !dev)
            return;
        const HRESULT r = dev->GetDeviceRemovedReason();
        if (r != S_OK && !removedSeen.exchange(true))
            XLog("device removed: first seen after %s (reason 0x%08x)", where, static_cast<unsigned>(r));
    }

    // A new allocator in the tag pool (FlushTags), or null.
    TagSlot* NewTagSlot()
    {
        TagSlot t;
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&t.alloc))))
            return nullptr;
        tagSlots.push_back(std::move(t));
        return &tagSlots.back();
    }

    bool WaitAppFence(UINT64 v)
    {
        if (appFence->GetCompletedValue() >= v)
            return true;
        appFence->SetEventOnCompletion(v, tagEvent);
        return WaitForSingleObject(tagEvent, 2000) == WAIT_OBJECT_0;
    }

    // The application's Present: hands the frame to the presenter and returns.
    HRESULT OnPresent(UINT sync, UINT flags)
    {
        LARGE_INTEGER t0;
        QueryPerformanceCounter(&t0);
        if (flags & DXGI_PRESENT_TEST)
        {
            // A reference of our own for the call, outside mu (Retire may let go of real meanwhile).
            IDXGISwapChain4* r = nullptr;
            {
                std::lock_guard<std::mutex> lk(mu);
                if ((r = real) != nullptr)
                    r->AddRef();
            }
            if (!r)
                return DXGI_ERROR_INVALID_CALL;
            const HRESULT hr = r->Present(sync, flags);
            r->Release();
            return hr;
        }
        Removed("the application's work, at its Present");
        Job j;
        XellCall xs;                // SetFgEnabled for inputs that became unusable / usable again with this frame's tags (sent after mu)
        XellLink xr;                // XeLL report of this present (taken under mu, sent after it)
        uint32_t xrCount = 0;
        uint8_t xrDisplay[48] {};
        bool xrSend = false, xrDisplaySend = false;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (!real && latencySem)
                ReleaseSemaphore(latencySem, 1, nullptr); // retired: an app still waiting on our handle never blocks
            if (!real || appBufs.empty() || !list || !presenter.joinable())
                return real ? real->Present(sync, flags) : DXGI_ERROR_INVALID_CALL;
            FlushTags();
            // The frame's tags decide whether motion / depth are usable (once per frame, with a recovery delay): XeLL is told when it changes
            xefgown::StepInputs(inputs, taggedMv || taggedDepth, (taggedMv && !haveMv) || (taggedDepth && !haveDepth));
            if (inputs.refused != xellInputsRefused)
                xs = XellSync();
            const Resource* ox = built && pg->HasPack() ? &pg->Output() : nullptr;
            const DXGI_FORMAT bf = scDesc.Format;
            const bool fmtKnown = bf == DXGI_FORMAT_R8G8B8A8_UNORM || bf == DXGI_FORMAT_R8G8B8A8_UNORM_SRGB || IsBgra(bf) || IsHdr10(bf);
            const bool fmtOk = ox && fmtKnown && IsHdr10(bf) == pg->PackHdr() && scDesc.Width == ox->w && scDesc.Height == ox->h;
            if (enabled && built && !fmtOk && !warnedSize)
            {
                XLog("present: backbuffer %ux%u fmt=%d does not match the pack's %ux%u %s%s - passing through", scDesc.Width, scDesc.Height,
                     static_cast<int>(bf), ox ? ox->w : 0, ox ? ox->h : 0, pg->PackHdr() ? "R10G10B10A2" : "R8G8B8A8",
                     !fmtKnown ? " (a format XeFG does not take)" : "");
                warnedSize = true;
            }
            j.frame = appFrame;
            j.bb = appIndex;
            j.set = static_cast<uint32_t>(appFrame & 1);
            // Exclusive fullscreen: Intel does not interpolate there (it logs so and leaves interpolation off), the
            // frames pass through; the same here (Alt+Enter can switch without the application asking).
            BOOL fullscreen = FALSE;
            real->GetFullscreenState(&fullscreen, nullptr);
            if ((fullscreen != FALSE) != inFullscreen)
            {
                inFullscreen = fullscreen != FALSE;
                XLog("present: %s", inFullscreen ? "exclusive fullscreen - passing frames through (as Intel)" : "left exclusive fullscreen");
            }
            j.uiMode = UiMode(hudless, haveUi);
            j.usable = enabled && built && haveMv && haveDepth && fmtOk && j.uiMode > 0 && !inFullscreen;
            j.hudless = j.uiMode >= 3;
            const bool camCut = CameraCut();
            j.reset = constants.resetHistory != 0;
            j.cut = camCut && !j.reset;
            j.jitter[0] = constants.jitterOffsetX;
            j.jitter[1] = constants.jitterOffsetY;
            j.mvs[0] = constants.motionVectorScaleX;
            j.mvs[1] = constants.motionVectorScaleY;
            j.nInterp = std::max(1u, std::min(numInterp, maxInterp));
            j.passes = j.usable && pg->HasPassPatches();
            j.sync = sync;
            j.flags = flags;
            j.presentId = presentId.load(std::memory_order_relaxed);
            j.arrive = t0.QuadPart;
            j.dbgTag = dbgTag;
            j.dbgOnly = dbgOnly;
            j.conv[0] = pendConv[0];
            j.conv[1] = pendConv[1];
            pendConv[0] = pendConv[1] = {};
            haveMv = haveDepth = hudless = haveUi = false;
            taggedMv = taggedDepth = false;
            tagsThisFrame = 0;
            // What this call will put out (GetLastPresentStatus), as the presenter will decide it.
            const bool willGen = j.usable && lastUsable && !j.reset;
            lastUsable = j.usable;
            const uint32_t n = willGen ? (j.passes ? j.nInterp : 1) : 0;
            lastGenerated = willGen;
            lastCallPresented = 1 + n;
            driverFgFrames = 1 + n;
            if (xellFgOn)
            {
                xr = xell;
                xrCount = n;    // as decided for GetLastPresentStatus; the presenter's own choice (hasPrev) matches it
                xrSend = true;
                if (xellDisplayDirty)
                {
                    // Intel's 48 bytes: 1, 0, the refresh period (u64 ns), zeros, 1 at the end (captured; the meaning
                    // of the two ones is not documented)
                    const uint64_t period = RefreshNs();
                    const uint32_t one = 1;
                    memcpy(xrDisplay, &one, 4);
                    memcpy(xrDisplay + 8, &period, 8);
                    memcpy(xrDisplay + 44, &one, 4);
                    xrDisplaySend = period != 0;
                    xellDisplayDirty = period == 0;     // no output yet (or between two): tried again next present
                }
            }
            framesPresented += 1 + n;
            // GPU side: the frame is ready when the application's queue gets here; the application's next
            // frame (its tags and its backbuffer) may only start once the presenter took the frame before this.
            appQueue->Signal(appFence, appFrame + 1);
            if (appFrame >= 1)
                appQueue->Wait(consumedFence, appFrame);
            ++appFrame;
            submitted.store(appFrame);
            appIndex = (appIndex + 1) % static_cast<UINT>(appBufs.size());
            Interval(t0);
        }
        xs.Send();
        if (xrSend)
        {
            // As Intel's, inside the game's present: between its PRESENT_START and PRESENT_END markers
            if (xrDisplaySend)
                xr.setDisplayInfo(xr.ctx, xrDisplay);
            uint32_t id = 0;
            const int32_t idRc = xr.getLastPresentStartFrameId(xr.ctx, &id);
            if (idRc == 0)
            {
                // Decimal, as Intel's sends them (OptiScaler's capture, InputCommon::xefg_marker): out of band render
                // submit start and end
                xr.addMarkerData(xr.ctx, id, 80860000u);
                xr.addMarkerData(xr.ctx, id, 80860001u);
                const int32_t rc = xr.setGeneratedFramesCount(xr.ctx, id, xrCount);
                if (rc != 0 && xrCount != xellCountFailed.exchange(xrCount))
                    XLog("xell: SetGeneratedFramesCount %u refused (%d)", xrCount, rc);
                else if (rc == 0 && !xellReported.exchange(true))
                    XLog("xell: first report: frame %u, %u generated", id, xrCount);
            }
            else if (!xellIdFailed.exchange(true))
                XLog("xell: GetLastPresentStartFrameId failed (%d) - generated frames not reported this present", idRc);
        }
        if (driverFg)
            DriverFgFrame(j);
        LARGE_INTEGER t1, t2, tr;
        QueryPerformanceCounter(&t1);
        // GPU backpressure, as DXGI's own frame queue limit: the frame before last must have gone out on the GPU
        // (generated frames and all). When the game and generation together are more than the GPU can do, the
        // game slows a little instead of our frames queueing behind its work and going out in bunches.
        if (retireWait && j.frame >= 2 && retireFence->GetCompletedValue() < j.frame - 1)
        {
            retireFence->SetEventOnCompletion(j.frame - 1, retireEvent);
            WaitForSingleObject(retireEvent, 200);
        }
        QueryPerformanceCounter(&tr);
        {
            // Bounded latency: at most two frames wait for the presenter.
            std::unique_lock<std::mutex> l(pm);
            pcv.wait(l, [this] { return pjobs.size() < appJobs || pQuit; });
            pjobs.push_back(j);
        }
        pcv.notify_all();
        QueryPerformanceCounter(&t2);
        SlowAppCall(j.frame, t0.QuadPart, t1.QuadPart, tr.QuadPart, t2.QuadPart);
        lastReturn = t2.QuadPart;
        statAppWait += static_cast<double>(t2.QuadPart - t1.QuadPart) * 1000.0 / freq();
        statAppCall += static_cast<double>(t2.QuadPart - t0.QuadPart) * 1000.0 / freq();
        const HRESULT hr = lastHr;
        return FAILED(hr) ? hr : S_OK;
    }

    // Our backbuffers, as the Proxy hands them out.
    HRESULT GetAppBuffer(UINT b, REFIID r, void** p)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (b >= appBufs.size())
            return DXGI_ERROR_INVALID_CALL;
        return appBufs[b]->QueryInterface(r, p);
    }
    UINT AppIndex()
    {
        std::lock_guard<std::mutex> lk(mu);
        return appIndex;
    }

    HRESULT Resize(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!real)
            return DXGI_ERROR_INVALID_CALL;
        DrainPresenter();
        WaitIdle();
        WaitApp();
        for (ID3D12Resource* b : appBufs)
            b->Release();
        appBufs.clear();
        if (pq || (flipBurst && realWait))
        {
            c = c ? std::max(c, kGateBuffers) : c;
            fl |= DXGI_SWAP_CHAIN_FLAG_FRAME_LATENCY_WAITABLE_OBJECT;   // created with it (GateDesc): may not change
        }
        const HRESULT hr = real->ResizeBuffers(c, w, h, f, fl);
        CreateAppBuffers();
        appIndex = 0;
        hasPrev = lastUsable = false;
        for (uint32_t& a : area)
            a = 0;                  // the next back buffer tag sets it again, against the new size
        XLog("swap chain: ResizeBuffers %u %ux%u fmt=%d -> 0x%08x", c, w, h, static_cast<int>(f), static_cast<unsigned>(hr));
        return hr;
    }

    // The latency calls take mu: Retire (Destroy, on another thread) may let go of real meanwhile.
    HANDLE LatencyHandle()
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!latencyAsked.exchange(true))
            XLog("swap chain: the application asked for the frame-latency waitable object (%s)", latencySem ? "ours" : "the real one's");
        if (!latencySem)
            return real ? real->GetFrameLatencyWaitableObject() : nullptr;
        HANDLE h = nullptr;
        DuplicateHandle(GetCurrentProcess(), latencySem, GetCurrentProcess(), &h, 0, FALSE, DUPLICATE_SAME_ACCESS);
        return h;
    }
    HRESULT SetLatency(UINT m)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!latencySem)
            return real ? real->SetMaximumFrameLatency(m) : DXGI_ERROR_INVALID_CALL;
        if (m < 1 || m > 16)
            return DXGI_ERROR_INVALID_CALL;
        if (m > latency)
            ReleaseSemaphore(latencySem, static_cast<LONG>(m - latency), nullptr);
        latency = m;
        return S_OK;
    }
    HRESULT GetLatency(UINT* m)
    {
        std::lock_guard<std::mutex> lk(mu);
        if (!latencySem)
            return real ? real->GetMaximumFrameLatency(m) : DXGI_ERROR_INVALID_CALL;
        if (!m)
            return DXGI_ERROR_INVALID_CALL;
        *m = latency;
        return S_OK;
    }

    void Retire();  // xefgSwapChainDestroy: let go of the real swap chain now (after Proxy, below)

    // The app releases its swap chain: stop presenting and let go of the real one.
    void Detach()
    {
        std::lock_guard<std::mutex> lk(mu);
        StopPresenter();
        WaitIdle();
        ReleaseSwapChain();
    }

    ID3D12Device* dev = nullptr;
    std::mutex mu;

private:
    // One application frame, as the presenter gets it.
    struct Job
    {
        uint64_t frame = 0;
        UINT bb = 0;
        uint32_t set = 0;
        bool usable = false, hudless = false, reset = false, passes = false, dbgTag = false, dbgOnly = false;
        uint32_t uiMode = 1;                // the output layer's UI mode (UiMode); hudless: cur takes the HUD-less colour
        bool cut = false;                   // camera cut (CameraCut): the app's frame in place of every generated one
        float jitter[2] {}, mvs[2] {};
        uint32_t nInterp = 1;
        UINT sync = 0, flags = 0;
        uint32_t presentId = 0;             // the game's frame id (xefgSwapChainSetPresentId), for RealPresent's markers
        long long arrive = 0;               // QPC of the application's Present
        Conversion conv[2];                 // motion, depth: converted from their raw copy (ConvertIn)
    };

    // An application frame whose inputs are taken and whose generation has started (Submit), for Show.
    struct Frame
    {
        Job j;
        bool valid = false;
        uint32_t nGen = 0;
        ID3D12Resource* realSrc = nullptr;
        float jc[2] {}, jp[2] {}, mvs[2] {};
        UINT64 done[kMaxInterp] {};         // CUDA path: generated frame k is in ringC[set][k - 1] at fence value done[k - 1]
        uint32_t set = 0;                   // the ring set (frame parity)
        long long base = 0, step = 0;       // its schedule (QPC): frame k at base + lead + (k - 1) * step (Target)
        long long lead = 0;                 // PaceLead (D3D12 path): frame 1's wait after base (QPC, at most step); 0: one whole step. Frozen
        long long cstep = 0;                // the clock's spacing for its frames (QPC; ClockTarget)
        bool clock = false;                 // paced by the clock
        bool spread = false;                // passes 2.. start at releases (PresentGenGated): the clock's need from pass 1 only
        double lag = 0;                     // the lag (ms) its schedule was set with (paceLag)
        long long ready = 0;                // when it was done on the GPU (jobBase; Hitch)
        bool tookEarly = false;             // taken while the frame before it was shown (Early; Hitch)
        long long pickT = 0;                // when Pick took it, and whether it was behind then (trace)
        bool catchUp = false;
        bool handed = false;                // cur -> prev done (HandOff)
        bool held = false;                  // its own picture is in hold
        UINT64 heldAt = 0;                  // Gate=1: its own picture is in gateHeld[set] at hostFence heldAt (Submit)
    };

    static long long Now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }
    static double freq()
    {
        static const double f = [] {
            LARGE_INTEGER x;
            QueryPerformanceFrequency(&x);
            return static_cast<double>(x.QuadPart);
        }();
        return f;
    }
    // Sleeps until a QPC time: a high-resolution waitable timer for all but the last half millisecond, then a
    // spin. Sleep() alone would round up to the system timer tick (15.6 ms unless someone raised it).
    static void WaitUntil(long long target)
    {
        static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
        for (;;)
        {
            const double ms = static_cast<double>(target - Now()) * 1000.0 / freq();
            if (ms <= 0)
                return;
            if (ms > 0.6 && timer)
            {
                LARGE_INTEGER due;
                due.QuadPart = -static_cast<LONGLONG>((ms - 0.5) * 10000.0);       // relative, 100 ns units
                if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
                    WaitForSingleObject(timer, INFINITE);
                else
                    Sleep(0);
            }
            else if (ms > 2)
                Sleep(1);
            else
                YieldProcessor();
        }
    }
    // A stall on the application's side of a present (> 50 ms; its own time > 100 ms, so menus and loading under 20 fps do
    // not use up the lines): its own time before the call, our part under mu, the GPU backpressure wait, the wait for the
    // presenter queue. The frame number matches the presenter's "hitch:" lines.
    void SlowAppCall(UINT64 frame, long long t0, long long t1, long long tr, long long t2)
    {
        const auto ms = [](long long a, long long b) { return static_cast<double>(b - a) * 1000.0 / freq(); };
        const double game = lastReturn ? ms(lastReturn, t0) : 0.0, locked = ms(t0, t1), retire = ms(t1, tr), waitQ = ms(tr, t2);
        if ((game > 100 || locked > 50 || retire > 50 || waitQ > 50) && slowCalls++ < 200)
            XLog("app stall: frame %llu, the game's own time before Present %.1f ms, our part %.1f, GPU backpressure %.1f "
                 "(retired %llu), presenter queue %.1f", static_cast<unsigned long long>(frame), game, locked, retire,
                 static_cast<unsigned long long>(retireFence->GetCompletedValue()), waitQ);
    }

    // PaceLog: one record per present of a frame that generates, kept in memory (a log line each would perturb the pacing) and
    // written by PaceDump. t: the present's time (QPC), ready: the app frame's ready stamp, target: the time it was aimed at.
    enum : uint8_t { kPaceLate = 1 };
    static constexpr size_t kPaceMax = 50000;
    struct PaceRec
    {
        long long t, ready, target;
        uint64_t frame;
        uint8_t k, n, flags;
    };
    void PaceRecord(long long t, long long ready, long long target, uint64_t frame, uint32_t k, uint32_t n, uint8_t flags)
    {
        if (!paceLog || n < 2)           // n: presents in the batch (generated + the app's own)
            return;
        std::lock_guard<std::mutex> l(paceMu);
        if (paceRecs.size() < kPaceMax)
            paceRecs.push_back({ t, ready, target, frame, static_cast<uint8_t>(k), static_cast<uint8_t>(n), flags });
        else
            ++paceDropped;
    }
    // "pace: t=<ms> frame=<app frame> k=<present>/<of> ready=<ms> target=<ms> flags=<late|->": ms are QPC milliseconds.
    void PaceDump()
    {
        std::vector<PaceRec> recs;
        size_t dropped;
        {
            std::lock_guard<std::mutex> l(paceMu);
            recs.swap(paceRecs);
            dropped = std::exchange(paceDropped, size_t(0));
        }
        const double q = 1000.0 / freq();
        for (const PaceRec& r : recs)
            XLog("pace: t=%.3f frame=%llu k=%u/%u ready=%.3f target=%.3f flags=%s", r.t * q, static_cast<unsigned long long>(r.frame), r.k,
                 static_cast<unsigned>(r.n), r.ready * q, r.target * q, r.flags & kPaceLate ? "late," : "-");
        if (dropped)
            XLog("pace: %zu records not kept (the first %zu are above)", dropped, kPaceMax);
    }

    // The application's own frame time, smoothed, ms: from our Present returning to its next call. The time
    // it spends waiting in our Present is left out - pacing on it would slow the presenter, which makes the
    // application wait longer, and so on.
    void Interval(LARGE_INTEGER t)
    {
        if (lastReturn)
        {
            const double ms = static_cast<double>(t.QuadPart - lastReturn) * 1000.0 / freq();
            if (ms > 0.5 && ms < 200)
                interval = interval * 0.9 + ms * 0.1;
        }
    }

    bool Fail(const std::string& e)
    {
        XLog("build: %s", e.c_str());
        return false;
    }

    // ---- the presenter -------------------------------------------------------------------------------

    void StartPresenter()
    {
        if (presenter.joinable())
            return;
        // The watcher stamps when each application frame is done on the GPU (appFence f + 1): the presenter's
        // timeline. A thread of its own, so a busy presenter does not see frames late and pace on that.
        wQuit = false;
        watcher = std::thread([this] {
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            uint64_t f = readyCount.load();
            while (!wQuit)
            {
                const UINT64 done = appFence->GetCompletedValue();
                if (done == UINT64_MAX)
                {
                    Sleep(50);              // device removed
                    continue;
                }
                if (done < f + 1)
                {
                    // Re-read after any wake: a wake left over from an earlier registration (each 50 ms timeout
                    // registers again on the same event) used to stamp a frame that was not done yet, which put
                    // the presenter's timeline one app frame early for the rest of the run.
                    appFence->SetEventOnCompletion(f + 1, ev);
                    WaitForSingleObject(ev, 50);
                    continue;
                }
                readyAt[f % kReady] = Now();
                readyCount.store(++f);
            }
            CloseHandle(ev);
        });
        StartGate();
        pQuit = false;
        presenter = std::thread([this] {
            // As the pacers of DLSS-G, AMD's FSR FG and the unlocker run theirs: a release must not wait for the scheduler.
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            for (;;)
            {
                Job j;
                {
                    std::unique_lock<std::mutex> l(pm);
                    if (early.valid)
                        j = early.j;            // taken already (Early): this thread's own, no wait
                    else
                    {
                        pcv.wait(l, [this] { return pQuit || !pjobs.empty(); });
                        if (pQuit && pjobs.empty())
                            return;
                        j = pjobs.front();
                        pjobs.pop_front();
                    }
                    pBusy = true;
                }
                pcv.notify_all();
                if (resizePending && j.frame >= resizeFrame)
                    RebuildForSizes("presenter thread");
                Present(j);
                {
                    std::lock_guard<std::mutex> l(pm);
                    pBusy = early.valid;        // a frame taken early is still to be shown (DrainPresenter)
                }
                pcv.notify_all();
            }
        });
    }
    // Waits until the presenter has nothing left (the caller produces no new frames meanwhile).
    void DrainPresenter()
    {
        std::unique_lock<std::mutex> l(pm);
        pcv.wait(l, [this] { return (pjobs.empty() && !pBusy) || !presenter.joinable(); });
    }
    void StopPresenter()
    {
        if (!presenter.joinable())
            return;
        {
            std::lock_guard<std::mutex> l(pm);
            pQuit = true;
        }
        pcv.notify_all();
        presenter.join();
        // A resize it did not get to: done at the next init (EnsureBuilt), not left pending for another presenter.
        if (resizePending.exchange(false))
            rebuildOnInit = true;
        StopGate();
        wQuit = true;
        if (watcher.joinable())
            watcher.join();
        if (paceLog)
            PaceDump();
        // Nothing may wait on frames that will not be taken any more.
        if (consumedFence)
            consumedFence->Signal(appFrame);
        if (retireFence)
            retireFence->Signal(appFrame);
    }

    ID3D12Resource* RealBuffer()
    {
        ID3D12Resource* b = nullptr;
        real->GetBuffer(real->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&b));
        return b;
    }

    // Waits on the CPU until a fence reaches a value; false after ms (200 by default: a lost device, a stuck queue).
    bool WaitGpu(ID3D12Fence* f, UINT64 v, DWORD ms = 200)
    {
        if (f->GetCompletedValue() >= v)
            return true;
        f->SetEventOnCompletion(v, gpuEvent);
        return WaitForSingleObject(gpuEvent, ms) == WAIT_OBJECT_0;
    }

    // The first failed Present of the real swap chain, once, with what is needed to tell a lost device (and why) from a
    // plain failure, and which path (pass-through or generating, sizes against the pack) was running.
    void PresentFailed(HRESULT hr, const Job& j, uint32_t k, uint32_t n)
    {
        lastHr = hr;
        if (warnedPresent)
            return;
        warnedPresent = true;
        const HRESULT removed = dev ? dev->GetDeviceRemovedReason() : S_OK;
        const Resource* ox = built && pg->HasPack() ? &pg->Output() : nullptr;
        XLog("present: the real swap chain's Present failed: 0x%08x (device removed reason 0x%08x), frame %llu present %u of %u, %s, "
             "sync %u flags 0x%x, swap chain %ux%u fmt=%d buffers=%u, pack output %ux%u, render %ux%u, %s lane",
             static_cast<unsigned>(hr), static_cast<unsigned>(removed), static_cast<unsigned long long>(j.frame), k, n + 1,
             j.usable ? "generating" : "passing through", j.sync, j.flags, scDesc.Width, scDesc.Height, static_cast<int>(scDesc.Format),
             scDesc.BufferCount, ox ? ox->w : 0, ox ? ox->h : 0, renderSize[0], renderSize[1], pq ? "gated" : "direct");
        if (removed != S_OK)
            DumpDred();
    }

    // With Dred=1 (xefg_cuda.cpp DredOn): the command lists the GPU had not finished, the operations around where each
    // stopped, and a page fault's address with the allocations live or freed there.
    void DumpDred()
    {
        ID3D12DeviceRemovedExtendedData* dred = nullptr;
        if (FAILED(dev->QueryInterface(IID_PPV_ARGS(&dred))))
        {
            XLog("dred: not available (Dred=1 in f5low.ini turns it on)");
            return;
        }
        D3D12_DRED_AUTO_BREADCRUMBS_OUTPUT bc {};
        if (SUCCEEDED(dred->GetAutoBreadcrumbsOutput(&bc)))
        {
            int lists = 0;
            for (const D3D12_AUTO_BREADCRUMB_NODE* n = bc.pHeadAutoBreadcrumbNode; n && lists < 12; n = n->pNext)
            {
                const UINT done = n->pLastBreadcrumbValue ? *n->pLastBreadcrumbValue : 0;
                if (done >= n->BreadcrumbCount)
                    continue;
                ++lists;
                XLog("dred: list %p (%ls) on queue %p (%ls): %u of %u operations done", static_cast<const void*>(n->pCommandList),
                     n->pCommandListDebugNameW ? n->pCommandListDebugNameW : L"unnamed", static_cast<const void*>(n->pCommandQueue),
                     n->pCommandQueueDebugNameW ? n->pCommandQueueDebugNameW : L"unnamed", done, n->BreadcrumbCount);
                const UINT from = done > 6 ? done - 6 : 0, to = done + 4 < n->BreadcrumbCount ? done + 4 : n->BreadcrumbCount;
                for (UINT i = from; i < to; ++i)
                    XLog("dred:   %s op %u: %d", i == done ? ">>" : "  ", i, static_cast<int>(n->pCommandHistory[i]));
            }
            if (!lists)
                XLog("dred: every recorded command list had finished");
        }
        D3D12_DRED_PAGE_FAULT_OUTPUT pf {};
        if (SUCCEEDED(dred->GetPageFaultAllocationOutput(&pf)) && pf.PageFaultVA)
        {
            XLog("dred: page fault at GPU address 0x%llx", static_cast<unsigned long long>(pf.PageFaultVA));
            int k = 0;
            for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadExistingAllocationNode; a && k < 16; a = a->pNext, ++k)
                XLog("dred:   live there: %ls (type %d)", a->ObjectNameW ? a->ObjectNameW : L"unnamed", static_cast<int>(a->AllocationType));
            for (const D3D12_DRED_ALLOCATION_NODE* a = pf.pHeadRecentFreedAllocationNode; a && k < 32; a = a->pNext, ++k)
                XLog("dred:   freed there: %ls (type %d)", a->ObjectNameW ? a->ObjectNameW : L"unnamed", static_cast<int>(a->AllocationType));
        }
        else
            XLog("dred: no page fault recorded");
        dred->Release();
    }

    // Presents on the real swap chain. Generated frames (k = 1..n) and the application's frame (k = n + 1)
    // are spread evenly over the frame interval on the GPU's timeline, from the moment the application's
    // frame was done on the GPU (its ready stamp, smoothed: schedBase), when the app does not sync to vblank. Only the
    // GPU timeline: a base
    // taken from our own presents would let each late present push the next app frame later, and the app's
    // frame rate crept down under load. Each goes out only once its own GPU work is done: a Present never waits
    // in the swap chain's queue to flip together with the next, which is what bunched frames when the game kept
    // the GPU busy. With vblank sync the display paces them.
    // The schedule runs paceLag behind the GPU timeline: as much as our GPU work came out late lately (Show raises it
    // at once to the most a frame was late, on top of the lag it was scheduled with, and lets it fall slowly), at most
    // maxLag app frames (half; at a whole one NBA 2K27 sat at the cap). Under a GPU the game keeps busy the first
    // generated frame needs that time for the head and its pass; with no lag the frames went out late and uneven.
    // A frame behind its time is followed by the next no sooner than half a step: the presenter catches up at twice
    // the pace instead of in a bunch; and none waits more than two steps after the last present (Bound).
    // The D3D12 path records each pass just before its present: there a lag gives the passes no more time, only latency.
    // The schedule's start for each app frame is a clock of its own (Pick): the last one's plus the interval, drawn a
    // fifth of the way towards the frame's ready stamp (plus the lag) - the stamps of a GPU-bound game wander by
    // milliseconds, which taken as they are put the jitter into the gap between an app frame and the next generated one.
    long long PaceTarget(uint32_t k, uint32_t n) const
    {
        const double step = ri.ms / static_cast<double>(n + 1) * freq() / 1000.0;
        return Bound(schedBase + static_cast<long long>(k * step), static_cast<long long>(step));
    }
    // target: worked out by the caller beforehand (Show, before Early moves the timeline on), else here. Returns how
    // many ms our GPU work held the frame past its target (0 if it was done in time).
    double PresentReal(const Job& j, uint32_t k, uint32_t n, UINT sync, UINT flags, long long target = 0)
    {
        if (n > 0 && !target)
            target = PaceTarget(k, n);
        const bool paced = n > 0 && !DisplayPaced(j);
        // FlipBurst: the whole target as the lead, so WaitUntil(target - lead) = WaitUntil(0) returns at once; lastRelease below keeps the slot.
        const bool metered = Metered(j);
        const bool burst = oobdriver::BurstNow(flipBurst, k, metered);
        const long long lead = burst ? target : k > 1 && metered ? FlipLead(static_cast<long long>(ri.ms / static_cast<double>(n + 1) * freq() / 1000.0)) : 0;
        const long long t0 = Now(), before = lastAnyPresent;
        if (paced)
            WaitUntil(target - lead);
        const long long tg = Now();
        WaitGpu(hostFence, hostValue);
        const double gpuWaitMs = static_cast<double>(Now() - tg) * 1000.0 / freq();
        // Only what our own GPU work held it past its target: a frame the game handed over late is past its target too, and
        // counted whole it raised the lag after each game stutter and the next frame then slept that long (NBA 2K27).
        const double late = paced ? std::min(gpuWaitMs, std::max(0.0, static_cast<double>(Now() - target) * 1000.0 / freq())) : 0.0;
        statGpuWait += gpuWaitMs;
        const long long tw0 = Now();
        if (realWait)
            WaitForSingleObject(realWait, 100);
        const long long tw1 = Now();
        if (k == 1 && n > 0)
        {
            statFirstLate += static_cast<double>(tw1 - target) * 1000.0 / freq();
            ++statFirstN;
        }
        lastRelease = lead ? std::max(Now(), target) : Now();   // the clock's release time (ClockTarget): its slot, if let out early
        if (k == 1)
            Meter(j, n);
        Removed("our last submission, before our Present");
        const HRESULT hr = RealPresent(j, sync, flags, k <= n, k == 1);
        if (genCount > 600 && traced < 40)
        {
            ++traced;
            XLog("trace: k %u/%u catch-up %d, allocator waits %.2f ms, waited for our GPU work %.2f ms (from %.2f ms after taking the frame), "
                 "waited on the real swap chain %.2f ms, Present %.2f ms, since last present %.2f ms, "
                 "%.2f ms after the target", k, n + 1, showing ? showing->catchUp : catchUp, std::exchange(allocWaitMs, 0.0), gpuWaitMs,
                 (tg - (showing ? showing->pickT : pickT)) * 1000.0 / freq(),
                 (tw1 - tw0) * 1000.0 / freq(), (Now() - tw1) * 1000.0 / freq(),
                 (tw0 - lastAnyPresent) * 1000.0 / freq(),
                 n ? (tw0 - target) * 1000.0 / freq() : 0.0);
        }
        if (flipStats && SUCCEEDED(hr))
            FlipStatsSample();
        if (FAILED(hr))
            PresentFailed(hr, j, k, n);
        const long long t = Now();
        if (lastAnyPresent)
        {
            const double ms = static_cast<double>(t - lastAnyPresent) * 1000.0 / freq();
            statGap += ms;
            statGap2 += ms * ms;
            statGapMax = std::max(statGapMax, ms);
            ++statGaps;
        }
        // Diagnostics: a gap of more than three steps between presents, with what it was made of (Hitch).
        // A metered burst batch: its presents after the first follow at once (not a gap), and its first comes one app interval after
        // the last batch's, so that gap is judged against the app interval.
        const double stepMs = ri.ms / static_cast<double>(n + 1);
        const bool burstBatch = flipBurst && Metered(j);    // read again: Meter (k == 1) has just decided this batch
        if (sync == 0 && n > 0 && before && !(burstBatch && k > 1) &&
            static_cast<double>(t - before) * 1000.0 / freq() > 3 * (burstBatch ? ri.ms : stepMs))
        {
            if (t - hitchSecond > static_cast<long long>(freq()))
                hitchSecond = t, hitches = 0;
            if (++hitches <= 5)             // a second (with vblank sync the display spaces them: no hitches to look for)
                Hitch(k, n, before, t0, target, tg, gpuWaitMs, tw0, tw1, t, stepMs);
        }
        lastAnyPresent = t;
        if (k == n + 1)
            lastReal = t;
        if (paceLog)
            PaceRecord(t, showing ? showing->ready : jobBase, target, j.frame, k, n + 1,
                       static_cast<uint8_t>(paced && static_cast<double>(t - target) * 1000.0 / freq() > 0.5 ? kPaceLate : 0));
        return late;
    }
    void Hitch(uint32_t k, uint32_t n, long long before, long long t0, long long target, long long tg, double gpuWaitMs, long long tw0,
               long long tw1, long long t, double stepMs)
    {
        const auto ms = [](long long a, long long b) { return static_cast<double>(b - a) * 1000.0 / freq(); };
        const Frame* f = showing;
        char what[200] = "generation lane";
        if (f)
            snprintf(what, sizeof(what), "frame %llu %s, %s; schedule lag %.1f ms (set with %.1f); ready stamp %.1f ms before the present",
                     static_cast<unsigned long long>(f->j.frame), f->tookEarly ? "taken early" : "taken in turn",
                     f->handed ? (f->held ? "next frame started before this present" : "handed off") : "not handed off", paceLag, f->lag,
                     ms(f->ready, t));
        // the last app frames' ready-to-ready times, oldest first (for replaying the game's pace: xefg_host --stallat)
        char hist[8 * 8 + 1] = "";
        for (size_t i = 0, o = 0; i < ri.hist.size() && o < sizeof(hist); ++i)
            o += snprintf(hist + o, sizeof(hist) - o, i ? " %.1f" : "%.1f", ri.hist[(ri.at + i) % ri.hist.size()]);
        XLog("hitch: %.1f ms since the last present (step %.2f), k %u/%u: reached its present %.2f after it, target %+.2f after it, "
             "slept %.2f, GPU wait %.2f (%.2f late), swap chain wait %.2f, Present %.2f; %s; interval %.2f (last frames %s)",
             ms(before, t), stepMs, k, n + 1, ms(before, t0), ms(before, target), ms(t0, tg), gpuWaitMs, std::max(0.0, ms(target, tw0)),
             ms(tw0, tw1), ms(tw1, t), what, ri.ms, hist);
    }

    // One application frame on the presenter thread. Its inputs may have been taken already (Early), while the
    // frame before it waited to be presented.
    void Present(const Job& j)
    {
        // While the deferred first build runs on its own thread the generator is not ours to touch (no frame is generated: the
        // jobs are not usable); deferBusy is set before the presenter exists and cleared after `built` is published.
        std::optional<GenScope> cuda;
        if (!deferBusy.load(std::memory_order_acquire))
            cuda.emplace(*pg);
        if (j.usable && pq && D3d() && !genQueue)
        {
            static bool warned = false;
            if (!warned)
                XLog("gate: the D3D12 path has no generation lane - not generating");
            warned = true;
            Job n = j;
            n.usable = false;
            Present(n);
            return;
        }
        if (early.valid && early.j.frame == j.frame)
        {
            const Frame f = early;
            early.valid = false;
            if (pq)
                ShowGated(f);               // the swap chain's buffers only on the present lane (device removal otherwise)
            else
                Show(f);
            return;
        }
        Pick(j);
        ID3D12Resource* bb = appBufs[j.bb];
        if (j.usable && genQueue && pq)
        {
            PresentGenGated(j, bb);
            return;
        }
        if (j.usable && genQueue)
        {
            clkLast = 0;                    // the generation lane keeps the schedule
            PresentGen(j, bb);
            return;
        }
        if (!j.usable && pq)
        {
            // Not generating, gated: on the present lane (the only one that writes the swap chain's buffers), behind
            // the presents still at the gate, released at once.
            ID3D12Resource* rb = RealBuffer();
            pq->Wait(appFence, j.frame + 1);
            Frame f;
            f.j = j;
            ID3D12GraphicsCommandList* g = BeginP();
            Barrier(g, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            BeginLaneCopy(g, j.frame, 1, false);
            g->CopyResource(rb, bb);
            EndLaneCopy(g);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(g, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            EndP();
            rb->Release();
            Consumed(j, pq);
            hasPrev = false;
            GateWait(f, 1, 0);
            LanePresent(f, 1, j.sync, j.flags);
            queue->Signal(retireFence, j.frame + 1);
            Stats(j, 0);
            return;
        }
        if (!j.usable)
        {
            // Not generating: the application's frame straight to the real swap chain.
            ID3D12Resource* rb = RealBuffer();
            ID3D12GraphicsCommandList* g = Begin();
            Barrier(g, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            g->CopyResource(rb, bb);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            Barrier(g, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
            End();
            rb->Release();
            Consumed(j);
            hasPrev = false;
            PresentReal(j, 1, 0, j.sync, j.flags);
            clkLast = 0;                    // off the clock: set it anew (ClockTarget)
            paceLag *= 0.98;                // falls while nothing is generated too (Show)
            queue->Signal(retireFence, j.frame + 1);
            Stats(j, 0);
            return;
        }
        if (pq)
            ShowGated(Submit(j));
        else
            Show(Submit(j));
    }

    // Takes an application frame: waits (GPU side) for it and puts it on the presenter's timeline.
    void Pick(const Job& j)
    {
        pickT = Now();
        queue->Wait(appFence, j.frame + 1);
        // The frame's place on the GPU timeline: when it was done (the watcher's stamp). Behind (catchUp, stats and
        // trace) = the next application frame is done already.
        while (readyCount.load() <= j.frame && !pQuit)
        {
            if (!WaitGpu(appFence, j.frame + 1) || appFence->GetCompletedValue() == UINT64_MAX)
                break;                      // (UINT64_MAX: device removed, the watcher stamps nothing)
            if (readyCount.load() <= j.frame)
                Sleep(0);                   // the watcher is about to stamp it
        }
        const long long ready = readyCount.load() > j.frame && readyCount.load() - j.frame <= kReady ? readyAt[j.frame % kReady] : Now();
        if (lastReady)
        {
            const double ms = static_cast<double>(ready - lastReady) * 1000.0 / freq();
            std::lock_guard<std::mutex> l(clkMu);
            ObserveReady(ms);
            ClockObserve(ms);
        }
        lastReady = ready;
        jobBase = ready;
        // The schedule's clock (PaceTarget); set anew when it is more than an interval off (a pause, the first frame).
        const long long iv = static_cast<long long>(ri.ms * freq() / 1000.0);
        schedBase = pacesched::NextBase(schedBase, ready, static_cast<long long>(paceLag * freq() / 1000.0), iv);
        // Where the presenter stands: how old the frame's ready stamp is, and how many app frames came after it.
        statStampAge += static_cast<double>(Now() - ready) * 1000.0 / freq();
        statBehind += static_cast<double>(submitted.load() - j.frame - 1);
        statArriveToPick += static_cast<double>(pickT - j.arrive) * 1000.0 / freq();
        statArriveToReady += static_cast<double>(ready - j.arrive) * 1000.0 / freq();
        ++statJobs;
        catchUp = appFence->GetCompletedValue() >= j.frame + 2;
        statLate += catchUp;
    }

    // 1. The frame's inputs: motion and depth from its staging set; the app's frame (with UI) into cur, or into
    //    keep when cur takes the hudless colour. Then the application may reuse them.
    // 2. N-x as Intel runs it: n generated frames at t = k / (n + 1), t = 0 at the previous frame, in ascending t,
    //    each with its own t, quantised t, motion-vector scale and pass index (Pack::SetPass). A pack without those
    //    patches can only make the middle frame. The CUDA path starts them all here (RunCuda), into the ring set of
    //    the frame's parity; the D3D12 path records each just before its present (Show).
    // The frame's schedule is fixed here (Pick set the clock): taking the next frame early must not move it.
    Frame Submit(const Job& j)
    {
        Frame f;
        f.j = j;
        f.valid = true;
        f.set = static_cast<uint32_t>(j.frame & 1);
        {
            ID3D12GraphicsCommandList* g = Begin();
            f.realSrc = CopyFrame(g, j, appBufs[j.bb]);
            End();
        }
        Consumed(j);
        if (pq)
        {
            // Gated: the frame's own picture into gateHeld[set] for the present lane, which may read it until its last present;
            // gateHeld[set] and ringC[set] are rewritten only once the lane read them two app frames ago (pqRead). After
            // Consumed: the application never waits for the display.
            queue->Wait(pqFence, pqRead[f.set]);
            if (!gateHeld[f.set] && !(gateHeld[f.set] = PlainBuffer(pg->Buf(GenSlot::Cur).bytes)))
                XLog("gate: no buffer for the frame's own picture - the present lane reads it in place");
            if (gateHeld[f.set])
            {
                ID3D12GraphicsCommandList* g = Begin();
                Out(g, f.realSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
                g->CopyBufferRegion(gateHeld[f.set], 0, f.realSrc, 0, pg->Buf(GenSlot::Cur).bytes);
                Back(g, f.realSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
                End();
            }
            f.heldAt = hostValue;
        }
        const bool gen = hasPrev && !j.reset;
        f.nGen = !gen ? 0 : j.passes ? j.nInterp : 1;
        if (f.nGen && !j.cut && !D3d() && !pg->EnsureRing(f.set, f.nGen))
            f.nGen = 0;
        if (f.nGen != lastNGen)
        {
            XLog("present: %u generated frame(s) per app frame (numInterp %u, max %u, pack %s the per-pass patches)", f.nGen, numInterp,
                 maxInterp, j.passes ? "has" : "LACKS - run tools/pack_add_patches.py on it");
            lastNGen = f.nGen;
        }
        f.base = schedBase;
        f.lag = paceLag;
        f.ready = jobBase;
        f.pickT = pickT;
        f.catchUp = catchUp;
        f.step = static_cast<long long>(ri.ms / static_cast<double>(f.nGen + 1) * freq() / 1000.0);
        // GenSpread on the CUDA path (PresentGenGated has the why): pass k > 1 waits on the stream until the present spread
        // places before it is released; this frame's presents come after those of the frame being shown (gateFrameEnd: a
        // frame taken early is submitted in the middle of the last one's presents).
        const uint32_t spread = pg->HasGateFence() && genSpread && !j.cut && !DisplayPaced(j) ? genSpread : 0;
        f.spread = spread > 0;
        SetClock(f, clockPacer && !D3d() && f.nGen && !DisplayPaced(j));
        // PaceLead: only where Target paces the frame and nothing else reacts to late ones (the D3D12 path; Lead::Freeze).
        f.lead = 0;
        leadState.automatic = paceLeadAuto;
        leadState.fixedMs = paceLeadFixedMs;
        if (f.nGen && !f.clock && D3d() && !DisplayPaced(j) && leadState.On())
        {
            const double lead = leadState.Freeze(static_cast<double>(f.step) * 1000.0 / freq());
            leadMin = std::min(leadMin, lead);
            leadMax = std::max(leadMax, lead);
            ++statLeadFrames;
            f.lead = static_cast<long long>(lead * freq() / 1000.0);
        }
        // The game's motion-vector scale; the pack's own (captured) one if the game sends none.
        f.mvs[0] = j.mvs[0];
        f.mvs[1] = j.mvs[1];
        if (f.mvs[0] == 0 && f.mvs[1] == 0)
            pg->MvScale(f.mvs[0], f.mvs[1]);
        f.jc[0] = j.jitter[0];
        f.jc[1] = j.jitter[1];
        f.jp[0] = prevJitter[0];
        f.jp[1] = prevJitter[1];
        if (f.nGen && !j.cut && !D3d())
            pg->Launch(queue, fv, IsBgra(scDesc.Format), clockPacer, f.set, f.nGen, f.jc, f.jp, f.mvs, j.uiMode, f.done,
                        spread ? std::max(gateFrameEnd, gateValue.load()) : 0, spread);
        return f;
    }

    // Whether the clock paces the frame, and its spacing: a little short of the app frame's time, by its spread (as AMD's
    // pacer does): the clock must never settle slower than the frames come; the correction holds the delay (ClockAfter).
    void SetClock(Frame& f, bool on)
    {
        f.clock = on;
        if (!on)
            return;
        std::lock_guard<std::mutex> l(clkMu);
        const double batch = clkBatch > 0 ? std::max(clkBatch - 0.1 * std::sqrt(clkVar), 0.9 * clkBatch) : ri.ms;
        f.cstep = static_cast<long long>(batch * (1.0 - clkCorr) / static_cast<double>(f.nGen + 1) * freq() / 1000.0);
    }

    // Frame k's present time (PaceTarget, on the frame's own schedule).
    long long Target(const Frame& f, uint32_t k) const
    {
        return Bound(f.base + pacelead::Offset(f.lead, f.step, k), f.step);
    }
    // Half a step after the last present at the soonest, two steps after it at the latest: a frame the game handed over
    // after a stutter has its schedule set from that late frame, and it waited lag + a step more with its picture ready.
    long long Bound(long long target, long long step) const
    {
        return pacesched::Bound(target, step, lastAnyPresent);
    }

    // ---- display pacing and flip metering --------------------------------------------------------------
    // A present with V-Sync (sync interval > 0) or flags other than tearing is paced by the display: no clock, no spread, the
    // gate holds nothing and the driver meters nothing; the frames go out as soon as they are done (the unlocker stands aside
    // the same way). On the RTX 3060 test PC the gate with V-Sync stalled ~1.1 s at a time (blinking).
    static bool DisplayPaced(const Job& j) { return j.sync != 0 || (j.flags & ~static_cast<UINT>(DXGI_PRESENT_ALLOW_TEARING)) != 0; }

    // NVIDIA driver flip metering (nvapi.h, NvAPI_D3D12_SetFlipConfig, driver 570+), as DLSS-G paces on RTX 40/50: right
    // before the first Present of each app frame's batch the swap chain's queue gets the batch's frame count and time, and
    // the display hardware spaces the flips within it. Our releases after the first then go FlipLead early, so the flip is
    // placed by the hardware, not by when a thread woke. Off while display-paced, not generating, or refused 8 times in a
    // row; the driver's NO_IFLIP feedback (no independent flip, e.g. composed) leaves our pacing alone.
    struct FlipConfig                       // NV_FLIP_CONFIG_V2
    {
        UINT version, framesPerBatch, feedback, presentedPerBatch;
        float timePerBatchMs;
        UINT reserved;
    };
    static constexpr UINT kFlipConfigVer2 = static_cast<UINT>(sizeof(FlipConfig)) | (2u << 16);
    static constexpr UINT kFlipFeedbackNoIflip = 1, kFlipMaxBatch = 8, kFlipRefusals = 8, kFlipSamples = 24;  // (3 and 8: the first batch got the start-up frames' 11-13 ms of 33; clkBatch eases 1/8)
    static constexpr double kFlipGrowth = 1.125;
    using SetFlipConfigFn = int(__cdecl*)(IUnknown* queue, UINT vidPnSource, FlipConfig* config);
    using NvQueryFn = void*(__cdecl*)(unsigned);

    bool Metered(const Job& j) const { return meteredFrame.load(std::memory_order_acquire) == j.frame + 1; }
    // How early a metered frame after a batch's first is released. Auto: 2500 us at 180 out (5.56 ms spacing, 0.45 of it), less
    // as the spacing shrinks - a lead longer than the spacing queued the frames at 540+ out (harness fast6x). A set value wins.
    static constexpr double kFlipLeadShare = 0.45;
    long long FlipLead(long long spacing) const
    {
        const long long set = static_cast<long long>(flipLeadUs * freq() / 1e6);
        return flipLeadAuto && spacing > 0 ? std::min(set, static_cast<long long>(kFlipLeadShare * static_cast<double>(spacing))) : set;
    }

    // fakenvapi (OptiScaler's own, or a game-folder nvapi64.dll) answers its private interface 0x21382138; NVIDIA's does not.
    static bool AnswersFakeQuery(NvQueryFn qi)
    {
        __try
        {
            return qi(0x21382138u) != nullptr;
        }
        __except (EXCEPTION_EXECUTE_HANDLER)
        {
            return true;
        }
    }

    // NVIDIA's own nvapi64.dll, from the system folder and pinned (OptiScaler's nvapi_QueryInterface hook lives on it:
    // [NvApi] DisableFlipMetering=true hides SetFlipConfig from us too). Null, with the reason, when the adapter has no CUDA
    // device behind it (not NVIDIA) or the nvapi is a fake one.
    NvQueryFn SystemNvapi(const char*& why)
    {
        wchar_t sys[MAX_PATH] {}, path[MAX_PATH] {};
        const UINT n = GetSystemDirectoryW(sys, MAX_PATH);
        HMODULE m = nullptr, owner = nullptr;
        NvQueryFn qi = nullptr;
        if (pg->GpuSm() <= 0)
            why = "the game's adapter is not an NVIDIA GPU (no CUDA device behind it)";
        else if (!n || n >= MAX_PATH || !(m = LoadLibraryW((std::wstring(sys) + L"\\nvapi64.dll").c_str())))
            why = "no nvapi64.dll in the system folder";
        else if (!(qi = reinterpret_cast<NvQueryFn>(GetProcAddress(m, "nvapi_QueryInterface"))))
            why = "nvapi64.dll has no nvapi_QueryInterface";
        else if (const DWORD len = GetModuleFileNameW(m, path, MAX_PATH);
                 !len || len >= MAX_PATH || _wcsnicmp(path, sys, n) != 0 || path[n] != L'\\' || wcschr(path + n + 1, L'\\'))
            why = "nvapi64.dll is not the system folder's (a fake nvapi)";
        else if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                                     reinterpret_cast<LPCWSTR>(qi), &owner) || owner != m)
            why = "nvapi_QueryInterface lives in another module (a fake nvapi)";
        else if (AnswersFakeQuery(qi))
            why = "nvapi64.dll answers fakenvapi's query (a fake nvapi)";
        else if (const auto init = reinterpret_cast<int(__cdecl*)()>(qi(0x0150E828u)); !init || init() != 0)      // NvAPI_Initialize
            why = "NvAPI_Initialize failed";
        if (m)
            GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(m), &owner);
        return why ? nullptr : qi;
    }

    // Off, with the reason in the log, when there is no usable nvapi or the driver lacks SetFlipConfig (before 570).
    void InitFlip()
    {
        flipTried = true;
        const char* why = nullptr;
        if (const NvQueryFn qi = SystemNvapi(why))
            if (!(flipFn = reinterpret_cast<SetFlipConfigFn>(qi(0xF3148C42u))))                                    // NvAPI_D3D12_SetFlipConfig
                why = "no NvAPI_D3D12_SetFlipConfig: driver before 570, or OptiScaler's [NvApi] DisableFlipMetering=true";
        if (flipFn)
            XLog("flip metering: on while V-Sync is off (NVIDIA driver), frames after a batch's first released %s%u us early",
                 flipLeadAuto ? "up to " : "", flipLeadUs);
        else
            XLog("flip metering: off - %s", why);
    }

    // ---- out of band notices (oob_driver.h) -------------------------------------------------------------
    using NotifyOobFn = int(__cdecl*)(ID3D12CommandQueue* queue, int type);
    using AsyncMarkerFn = int(__cdecl*)(ID3D12CommandQueue* queue, oobdriver::AsyncMarker* marker);
    using ReflexSyncFn = int(__cdecl*)(IUnknown* device, oobdriver::ReflexSync* sync);

    void NotifyQueue(NotifyOobFn fn, ID3D12CommandQueue* q, const char* what, bool hasPresents, bool hasWork)
    {
        if (!q)
            return;
        const oobdriver::QueueType t = oobdriver::TypeFor(q == appQueue, hasPresents, hasWork);
        if (t == oobdriver::kLeaveAlone)
        {
            XLog("out of band: %s queue left alone%s", what, q == appQueue ? " (it is the application's)" : "");
            return;
        }
        const int st = fn(q, t);
        XLog("out of band: %s queue is type %d -> nvapi status %d", what, static_cast<int>(t), st);
    }

    // Once, before our first Present, on the presenting thread.
    void OobNotify()
    {
        oobTried = true;
        const char* why = nullptr;
        const NvQueryFn qi = SystemNvapi(why);
        if (!qi)
        {
            XLog("out of band: off - %s", why);
            return;
        }
        if (oobQueue || oobSwitch)
        {
            if (const auto fn = reinterpret_cast<NotifyOobFn>(qi(0x03D6E8CBu)))                                    // NvAPI_D3D12_NotifyOutOfBandCommandQueue
            {
                if (oobSwitch)
                {
                    notifyOobFn = fn;
                    return OobNotifyMarkers(qi);
                }
                NotifyQueue(fn, pq, "present lane", true, true);
                NotifyQueue(fn, queue, "own", !pq, true);
                NotifyQueue(fn, genQueue, "generation", false, true);
            }
            else
                XLog("out of band: the driver has no NvAPI_D3D12_NotifyOutOfBandCommandQueue");
        }
        OobNotifyMarkers(qi);
    }

    void OobNotifyMarkers(NvQueryFn qi)
    {
        if (oobMarkers && !(asyncMarkerFn = reinterpret_cast<AsyncMarkerFn>(qi(0x13C98F73u))))                     // NvAPI_D3D12_SetAsyncFrameMarker
            XLog("out of band: the driver has no NvAPI_D3D12_SetAsyncFrameMarker");
    }

    // OobQueue=switch: the queue's type follows the Present about to go out, the notice only when it changes.
    void OobSwitchType(ID3D12CommandQueue* q, bool generated)
    {
        const int want = generated ? oobdriver::kRender : oobdriver::kPresent;
        if (!notifyOobFn || q == appQueue || want == oobSwitchType)
            return;
        const int st = notifyOobFn(q, want);
        if (st != 0 && st != oobSwitchFailed)
        {
            oobSwitchFailed = st;
            XLog("out of band: switching the queue to type %d -> nvapi status %d", want, st);
        }
        oobSwitchType = want;
    }

    void OobMark(ID3D12CommandQueue* q, uint32_t frameId, int type)
    {
        oobdriver::AsyncMarker m {};
        m.version = oobdriver::kAsyncMarkerVer;
        m.frameId = frameId;
        m.markerType = type;
        const int st = asyncMarkerFn(q, &m);
        if (st != 0 && st != oobMarkerFailed)
        {
            oobMarkerFailed = st;
            XLog("out of band: marker %d for frame %u -> nvapi status %d", type, frameId, st);
        }
        else if (st == 0 && !oobMarked)
        {
            oobMarked = true;
            XLog("out of band: first marker %d for frame %u taken", type, frameId);
        }
    }

    // Every Present of ours that goes to the real swap chain, between its out of band markers when OobMarkers=1.
    HRESULT RealPresent(const Job& j, UINT sync, UINT flags, bool generated, bool first)
    {
        ID3D12CommandQueue* const q = pq ? pq : queue;
        if (driverFg)
        {
            const bool fgReady = (driverFgParts & 4) && driverFgReady.load(std::memory_order_acquire) && oobdriver::MarkFrame(j.presentId);
            if (fgReady && first)
                OobMark(q, j.presentId, oobdriver::kOobRenderStart);
            if (fgReady)
                OobMark(q, j.presentId, oobdriver::kOobPresentStart);
            const HRESULT fgHr = real->Present(sync, flags);
            if (fgReady)
                OobMark(q, j.presentId, oobdriver::kOobPresentEnd);
            if (fgReady && !generated)
                OobMark(q, j.presentId, oobdriver::kOobRenderEnd);
            return fgHr;
        }
        if ((oobQueue || oobSwitch || oobMarkers) && !oobTried)
            OobNotify();
        OobSwitchType(q, generated);
        const bool mark = asyncMarkerFn && q != appQueue && oobdriver::MarkFrame(j.presentId);
        if (mark)
            OobMark(q, j.presentId, oobdriver::kOobPresentStart);
        const HRESULT hr = real->Present(sync, flags);
        if (mark)
            OobMark(q, j.presentId, oobdriver::kOobPresentEnd);
        return hr;
    }

    // DriverFg=1, once, on the application's thread at its first Present: a second queue of ours, the two out of band notices,
    // the marker and Reflex Sync entry points.
    void DriverFgInit()
    {
        driverFgTried = true;
        const char* why = nullptr;
        const NvQueryFn qi = SystemNvapi(why);
        if (!qi)
        {
            XLog("driver signals: off - %s", why);
            return;
        }
        const auto notify = reinterpret_cast<NotifyOobFn>(qi(0x03D6E8CBu));                                        // NvAPI_D3D12_NotifyOutOfBandCommandQueue
        asyncMarkerFn = reinterpret_cast<AsyncMarkerFn>(qi(0x13C98F73u));                                          // NvAPI_D3D12_SetAsyncFrameMarker
        reflexSyncFn = reinterpret_cast<ReflexSyncFn>(qi(0xB9F6FAFFu));                                              // NvAPI_D3D_SetReflexSync
        D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
        ID3D12CommandQueue* q2 = nullptr;
        if (!notify || !asyncMarkerFn || !reflexSyncFn || FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q2))))
        {
            XLog("driver signals: off - notice %d, marker %d, sync %d, second queue %d", notify != nullptr, asyncMarkerFn != nullptr, reflexSyncFn != nullptr, q2 != nullptr);
            return;
        }
        q2->SetName(L"f5low driver signals");
        driverFgQueue = q2;
        if (driverFgParts & 8)
        {
            NotifyQueue(notify, q2, "second", false, true);
            NotifyQueue(notify, genQueue, "generation", false, true);
            if (pq)
                NotifyQueue(notify, queue, "own", false, true);
            ID3D12CommandQueue* const pres = pq ? pq : queue;
            if (pres != appQueue)
                XLog("driver signals: present queue type %d -> nvapi status %d", static_cast<int>(oobdriver::kRender), notify(pres, oobdriver::kRender));
            else
                XLog("driver signals: the swap chain presents on the application's queue, no notice for it");
        }
        driverFgReady.store(true, std::memory_order_release);
        if (!driverFgCounted)
        {
            driverFgCounted = true;
            g_driverSignals.fetch_add(1);
        }
        XLog("driver signals: on, parts %u (1 Reflex Sync, 2 present markers, 4 out of band markers, 8 queue notices)", driverFgParts);
    }

    // At the application's Present, in the order DLSS-G uses: the in-band present markers on the second queue, Reflex Sync.
    void DriverFgFrame(const Job& j)
    {
        if (!driverFgTried)
            DriverFgInit();
        if (!driverFgReady.load(std::memory_order_acquire) || !oobdriver::MarkFrame(j.presentId))
            return;
        if (driverFgParts & 2)
        {
            OobMark(driverFgQueue, j.presentId, oobdriver::kPresentStart);
            OobMark(driverFgQueue, j.presentId, oobdriver::kPresentEnd);
        }
        if (!(driverFgParts & 1))
            return;
        oobdriver::ReflexSync s {};
        s.version = oobdriver::kReflexSyncVer;
        s.fgMultiplier = static_cast<uint8_t>(driverFgFrames);
        const int st = reflexSyncFn(dev, &s);
        if (st != driverFgSyncStatus)
        {
            driverFgSyncStatus = st;
            XLog("driver signals: Reflex Sync multiplier %u -> nvapi status %d", driverFgFrames, st);
        }
    }

    // Before the first Present of an app frame's batch (n generated + its own), on the presenter thread.
    void Meter(const Job& j, uint32_t n)
    {
        if (!flipMetering)
            return;
        if (!flipTried)
            InitFlip();
        if (!flipFn)
            return;
        double batch;
        {
            std::lock_guard<std::mutex> l(clkMu);
            batch = clkBatch;
        }
        flipSamples = n ? flipSamples + 1 : 0;
        const char* skip = !n ? "not generating" : DisplayPaced(j) ? "V-Sync" : flipRefused ? "refused by the driver"
                         : n + 1 > kFlipMaxBatch ? "too many frames per batch" : flipSamples < kFlipSamples || batch <= 0 ? "frame time unknown"
                         : nullptr;
        if (skip)
        {
            meteredFrame.store(0, std::memory_order_release);
            if (flipOutputs)
                FlipConfigure(0, 0, skip);
            return;
        }
        // One long frame must not hold the whole next batch to it: up by an eighth per batch at most.
        if (flipOutputs == n + 1)
            batch = std::min(batch, flipBatchMs * kFlipGrowth);
        FlipConfigure(n + 1, batch, nullptr);
        meteredFrame.store(flipMeteredNow ? j.frame + 1 : 0, std::memory_order_release);
    }

    // outputs 0: off (one presented frame per batch).
    void FlipConfigure(uint32_t outputs, double batchMs, const char* why)
    {
        FlipConfig c { kFlipConfigVer2, outputs, 0, outputs ? outputs : 1u, outputs ? static_cast<float>(batchMs) : 0.0f, 0 };
        const int st = flipFn(pq ? pq : queue, 0, &c);
        ++flipCalls;
        if (st == 0)
            flipFails = 0;
        else if (outputs && ++flipFails >= kFlipRefusals)
            flipRefused = true;
        const bool metered = outputs && st == 0 && !(c.feedback & kFlipFeedbackNoIflip);
        const uint32_t configured = outputs && st == 0 ? outputs : 0;
        if ((st != flipStatus || c.feedback != flipFeedback || metered != flipMeteredNow || !configured != !flipOutputs) && flipLogs < 32)
        {
            ++flipLogs;
            XLog("flip metering: %s - %u frames per batch, %.2f ms per batch: status %d, feedback 0x%x%s%s", metered ? "on" : "off", outputs,
                 batchMs, st, c.feedback, c.feedback & kFlipFeedbackNoIflip ? " (no independent flip)" : "", why ? (std::string(", ") + why).c_str() : "");
            if (flipRefused)
                XLog("flip metering: refused %u times in a row - off for this swap chain", kFlipRefusals);
        }
        flipStatus = st;
        flipFeedback = c.feedback;
        flipMeteredNow = metered;
        flipOutputs = configured;
        flipBatchMs = configured ? batchMs : 0;
        statFlipMetered += metered;
    }

    // ---- the clock pacer (Pacer=clock, CUDA path) ----------------------------------------------------
    // A release clock instead of a schedule per app frame (studied in plans/2026-10-24-study-xefg-presenter-pacing.md;
    // the unlocker 1.7's queue and AMD's FSR FG pacer run one): each frame, generated or the app's own, goes out one
    // spacing after the last release (taken just before its Present), not at a time worked out from its app frame's
    // ready stamp - those wander by milliseconds on a GPU-bound game, and a frame its GPU work held late pushed the
    // next ones into a bunch. The clock runs clkTarget behind the ready stamps: the delay the generated frames
    // needed lately (ClockAfter, from the stream's own done times), and a correction of the spacing of at most
    // kClkMaxCorr holds it there. A frame not done by its time goes out when done and the clock carries on from it.
    static constexpr double kClkMaxCorr = 0.04, kClkGain = 0.05, kClkMargin = 2.0, kClkMarginShare = 0.36;

    // The margin on the delay the generated frames need: 2 ms at 180 out (5.56 ms spacing, 0.36 of it), less as the spacing
    // shrinks (a fixed 2 ms was a whole spacing at 480 out), never more than 2 ms. cstep: the clock's spacing, QPC ticks.
    double ClockMargin(long long cstep) const
    {
        return cstep > 0 ? std::min(kClkMargin, kClkMarginShare * static_cast<double>(cstep) * 1000.0 / freq()) : kClkMargin;
    }

    // The rate the schedule spaces generated frames by (Pick): eased by 1/10, but when the game slows for good (two frames in a
    // row over 1.3x of it: a menu turning into play) it is raised at once, to the shorter of the two. Easing alone took ~15
    // frames after 120 -> 28 fps, each batch presented at the old, shorter spacing and then waiting for the next real frame
    // (NBA run U2: 4-5 hitches in a row; harness --fpsat 300 28). Never lowered at once: after a stall the game hands over a
    // couple of quick catch-up frames, and taking those as its pace made every batch after it short (NBA run V). Three in a
    // row under 1/1.3 of it (a real return to a faster pace, e.g. after a two-frame stall raised it) lower it at once, to the
    // longest of the three; catch-up bursts are one or two frames.
    void ObserveReady(double ms) { ri.Observe(ms); }

    // The app frame's time on the GPU timeline (Pick): smoothed (1/8), a delta over 1.5x left out at most twice in a
    // row (one game stutter must not stretch the spacing for the frames after it), none outside 1-250 ms (set anew).
    void ClockObserve(double ms)
    {
        if (ms < 1 || ms > 250)
        {
            clkBatch = clkVar = 0;
            clkHitchRun = 0;
            return;
        }
        if (clkBatch <= 0)
        {
            clkBatch = ms;
            return;
        }
        if (ms > 1.5 * clkBatch && clkHitchRun < 2)
        {
            ++clkHitchRun;
            ++statClkHitches;
            return;
        }
        clkHitchRun = 0;
        const double d = ms - clkBatch;
        clkBatch += d / 8;
        clkVar += (d * d - clkVar) / 8;
    }

    // Frame k's release time: one spacing after the last release, but no sooner than the delay the generated frames
    // need (clkTarget less the margin: after a game stutter the clock had run ahead of the frames) and no later than
    // MaxDelay app frames behind the ready stamp. Set anew from the ready stamp after a pause.
    long long ClockTarget(const Frame& f, uint32_t k) const
    {
        const double q = freq() / 1000.0;
        const long long now = Now(), batch = static_cast<long long>((clkBatch > 0 ? clkBatch : ri.ms) * q);
        const long long slot = f.ready + static_cast<long long>(k - 1) * f.cstep;
        const long long soonest = slot + static_cast<long long>(std::max(0.0, clkTarget - ClockMargin(f.cstep)) * q);
        const long long latest = slot + static_cast<long long>(maxDelay * static_cast<double>(batch));
        long long t = clkLast && now - clkLast < static_cast<long long>(250 * q) ? clkLast + f.cstep : slot + static_cast<long long>(clkTarget * q);
        t = std::clamp(t, soonest, std::max(soonest, latest));
        return std::min(t, now + static_cast<long long>(100 * q));
    }

    // After an app frame's presents: the delay its generated frames needed (frame k done, less its place k - 1 spacings
    // after the first), into the target (at once up to it plus the margin, down by an eighth towards the 90th percentile
    // of the last 64 plus the margin); the delay it had (its first release after the ready stamp) sets the correction.
    void ClockAfter(const Frame& f, long long firstRelease)
    {
        const double q = freq() / 1000.0;
        const double batch = clkBatch > 0 ? clkBatch : ri.ms, limit = maxDelay * batch;
        double need = -1;
        for (uint32_t k = 1; k <= (f.spread ? 1 : f.nGen) && !f.j.cut; ++k)
        {
            const UINT64 v = f.done[k - 1];
            if (doneVal[v % kStamps].load(std::memory_order_acquire) != v)
                continue;
            const long long at = doneAt[v % kStamps].load(std::memory_order_relaxed);
            need = std::max(need, static_cast<double>(at - f.ready - static_cast<long long>(k - 1) * f.cstep) / q);
        }
        if (need >= 0)
        {
            clkNeed[clkNeedAt] = need;
            clkNeedAt = (clkNeedAt + 1) % clkNeed.size();
            clkNeedN = std::min<unsigned>(clkNeedN + 1, static_cast<unsigned>(clkNeed.size()));
            std::array<double, 64> s = clkNeed;
            const auto p = s.begin() + (clkNeedN * 9) / 10;
            std::nth_element(s.begin(), p, s.begin() + clkNeedN);
            const double margin = ClockMargin(f.cstep);
            const double floor = *p + margin;
            if (need + margin > clkTarget)
                clkTarget = need + margin;
            else if (clkTarget > floor)
                clkTarget = std::max(floor, clkTarget - std::max(0.1, (clkTarget - floor) / 8));
            statClkNeed += need;
        }
        else if (!stampsWork)
            clkTarget = 0.5 * batch;            // no done times (no host functions): half an app frame
        clkTarget = std::clamp(clkTarget, 0.0, limit);
        if (firstRelease)
        {
            clkDelay = static_cast<double>(firstRelease - f.ready) / q;
            clkCorr = std::clamp(kClkGain * std::clamp((clkDelay - clkTarget) / batch, -1.0, 1.0), -kClkMaxCorr, kClkMaxCorr);
            statClkDelay += clkDelay;
            ++statClkN;
        }
    }

    // The generated frames, then the application's frame, each at its time (PresentReal); cur -> prev.
    void Show(Frame f)
    {
        showing = &f;
        const Job& j = f.j;
        const uint32_t nGen = f.nGen;
        long long firstRelease = 0;
        const Resource& ox = pg->Output();
        for (uint32_t k = 1; k <= nGen; ++k)
        {
            // CUDA path: the next app frame starts as soon as it is done on the GPU and this one's passes have run.
            if (!D3d() && !f.handed && !DumpPending(j) && NextReady() && (j.cut || pg->DoneFence()->GetCompletedValue() >= f.done[nGen - 1]) &&
                (hold || (hold = PlainBuffer(pg->Buf(GenSlot::Cur).bytes)) != nullptr))
            {
                HandOff(f, true);
                Early();
            }
            // A camera cut: the app's frame in this place, no network.
            if (!j.cut && D3d())
            {
                // The constants are copied into the list as it is recorded (RecordPass, below).
                pg->PreparePass(k, nGen, f.jc, f.jp, f.mvs);
            }
            else if (!j.cut)
                queue->Wait(pg->DoneFence(), f.done[k - 1]);
            ID3D12Resource* frame = f.held ? hold : f.realSrc;
            ID3D12Resource* rb = RealBuffer();
            ID3D12GraphicsCommandList* g = Begin();
            if (D3d() && !j.cut)
                RecordPass(g, k, cur_i);
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            ID3D12Resource* gsrc = j.cut ? frame : D3d() ? pg->Buf(GenSlot::Output).res : pg->RingFrame(f.set, k - 1);
            Out(g, gsrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (j.cut)
                Copy(g, rb, gsrc, ox, true);
            else
                CopyGenerated(g, rb, gsrc, frame, ox);
            Back(g, gsrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (j.dbgTag)
                DrawTags(g, rb, ox);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            End();
            DumpShown(j, k, rb, f.held);
            rb->Release();
            if (!j.cut)
                DumpOnce(j, k);
            const double late = PresentReal(j, k, nGen, j.sync, j.flags & DXGI_PRESENT_ALLOW_TEARING, f.clock ? ClockTarget(f, k) : Target(f, k));
            if (f.clock)
            {
                clkLast = lastRelease;
                if (k == 1)
                    firstRelease = lastRelease;
                statClkSlips += late > 0.5;
            }
            else
            {
                if (f.lead > 0 && late > pacelead::kLateMs)
                {
                    ++statLeadLate;
                    leadState.Late(late);
                }
                if (!D3d() && late > 0.5)   // not a hair's breadth, which undid the fall below every frame
                    paceLag = std::min(std::max(paceLag, f.lag + late), ri.ms * maxLag);
            }
            if (++genCount == 1 || genCount % 600 == 0)
                XLog("present: %u frames generated", genCount);
        }

        if (f.lead > 0)
            leadState.EndFrame(static_cast<double>(f.lead) * 1000.0 / freq(), static_cast<double>(f.step) * 1000.0 / freq());

        // 3. The application's frame, and cur -> prev.
        {
            ID3D12Resource* rb = RealBuffer();
            ID3D12Resource* src = j.dbgOnly && nGen && !j.cut ? (D3d() ? pg->Buf(GenSlot::Output).res : pg->RingFrame(f.set, nGen - 1)) : f.held ? hold : f.realSrc;
            ID3D12GraphicsCommandList* g = Begin();
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            Out(g, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Copy(g, rb, src, ox, true);
            Back(g, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            End();
            DumpShown(j, nGen + 1, rb, f.held);
            rb->Release();
        }
        if (!f.handed)
            HandOff(f, false);
        // Everything of this frame that reads cur, prev or the ring is on our queue: the next frame can start
        // while this one's own frame waits for its time.
        Early();
        PresentReal(j, nGen + 1, nGen, j.sync, j.flags, f.clock ? ClockTarget(f, nGen + 1) : nGen ? Target(f, nGen + 1) : 0);
        clkLast = f.clock ? lastRelease : 0;  // a frame off the clock sets it anew
        if (f.clock)
            ClockAfter(f, firstRelease);
        paceLag *= 0.98;                    // falls by half in ~35 app frames while nothing comes out late
        queue->Signal(retireFence, j.frame + 1);
        Stats(j, nGen);
        showing = nullptr;
    }

    // ---- the gate (Gate=1, CUDA path, clock) ---------------------------------------------------------
    // The pacer on the GPU (plans/2026-10-24-xefg-swapchain-gate-pacer.md): the unlocker 1.7 gates Intel's presents with a
    // fence; here the gate is on our own present lane (pq), which does nothing but wait for each finished frame, copy it
    // into the swap chain's buffer and present it. Each Present is called at once, behind a fence wait (gateFence) the
    // release thread signals at the frame's clock time (ClockTarget) - no CPU thread sleeps between frames, and a late
    // frame holds back no work: inputs, hand-off and CUDA's start stay on our queue, which never waits for the display
    // (in the harness, gating the queue that also generated cost Intel's library 27% of the game's rate). The lane reads
    // only per-parity buffers (gateHeld[set], ringC[set]); our queue waits for it (pqRead) only before rewriting a set.

    // The generated frames, then the application's frame, each behind the gate; cur -> prev on our queue.
    void ShowGated(Frame f)
    {
        showing = &f;
        const Job& j = f.j;
        const uint32_t nGen = f.nGen;
        gateFrameEnd = gateValue.load() + nGen + 1;     // its presents' values (the next frame's spread starts after them)
        const Resource& ox = pg->Output();
        ID3D12Resource* frame = gateHeld[f.set] ? gateHeld[f.set] : f.realSrc;
        pq->Wait(hostFence, f.heldAt);
        for (uint32_t k = 1; k <= nGen; ++k)
        {
            // The next app frame starts as soon as it is done on the GPU and this one's passes have run (Show); its picture
            // goes to the other held set, so no hold copy.
            if (!f.handed && !DumpPending(j) && NextReady() && (j.cut || pg->DoneFence()->GetCompletedValue() >= f.done[nGen - 1]))
            {
                HandOff(f, false);
                Early();
            }
            if (!j.cut)
                pq->Wait(contentFence, f.done[k - 1]);
            ID3D12Resource* rb = RealBuffer();
            ID3D12GraphicsCommandList* g = BeginP();
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            if (j.cut)
            {
                BeginLaneCopy(g, j.frame, k, false);
                Copy(g, rb, frame, ox, true);
            }
            else
            {
                BeginLaneCopy(g, j.frame, k, true);
                CopyGenerated(g, rb, pg->RingFrame(f.set, k - 1), frame, ox);
            }
            EndLaneCopy(g);
            if (j.dbgTag)
                DrawTags(g, rb, ox);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            EndP();
            rb->Release();
            GateWait(f, k, j.cut ? 0 : f.done[k - 1]);
            LanePresent(f, k, j.sync, j.flags & DXGI_PRESENT_ALLOW_TEARING);
            if (++genCount == 1 || genCount % 600 == 0)
                XLog("present: %u frames generated", genCount);
        }
        {
            ID3D12Resource* rb = RealBuffer();
            ID3D12Resource* src = j.dbgOnly && nGen && !j.cut ? pg->RingFrame(f.set, nGen - 1) : frame;
            ID3D12GraphicsCommandList* g = BeginP();
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            BeginLaneCopy(g, j.frame, nGen + 1, false);
            Copy(g, rb, src, ox, true);
            EndLaneCopy(g);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            EndP();
            pqRead[f.set] = pqValue;        // the lane's last read of this set
            rb->Release();
        }
        // HandOff's GPU wait for the last pass only once that pass is done: a D3D12 wait still pending on the shared fence
        // while the stream queued its signals made nearly every app frame's cuSignalExternalSemaphoresAsync fail
        // (CUDA_ERROR_INVALID_VALUE, harness 6x at 30 fps); Show never got there before the passes were done.
        if (!f.handed && nGen && !j.cut && pg->DoneFence()->GetCompletedValue() < f.done[nGen - 1])
            WaitGpu(pg->DoneFence(), f.done[nGen - 1], 1000);
        if (!f.handed)
            HandOff(f, false);
        Early();
        GateWait(f, nGen + 1, 0);
        LanePresent(f, nGen + 1, j.sync, j.flags);
        // Retired once its own frame went out (as Show does): signalled on our queue here, the application ran a frame further
        // ahead and its frames waited 22 ms longer to be taken (harness 6x at 30 fps, same rate).
        pq->Signal(retireFence, j.frame + 1);
        Stats(j, nGen);
        showing = nullptr;
    }

    // Frame k of f: the lane waits at the gate after its copy (pqValue: the copy was just put on the lane), just before its
    // Present; the release thread gets its time and opens the gate once the copy is done. content: the fence value (io.fence)
    // the frame's picture is done at, 0 if it is done already (diagnostics).
    // The copy runs as soon as the picture is done, next to CUDA's work (copies at release time, scattered over the frame,
    // made each pass 1.3 ms slower: harness 6x at 30 fps), on contentFence - nothing of ours waits on the GPU for the CUDA
    // fence itself while the stream queues its signals (that made cuSignalExternalSemaphoresAsync fail, see ShowGated).
    // Display-paced (V-Sync): not held, the release thread never sees it.
    void GateWait(const Frame& f, uint32_t k, UINT64 content)
    {
        const UINT64 v = ++gateValue;
        if (DisplayPaced(f.j))
        {
            GateRelease(v);
            std::lock_guard<std::mutex> l(clkMu);
            clkLast = 0;                    // off the clock: set it anew (ClockTarget)
            return;
        }
        pq->Wait(gateFence, v);
        {
            std::lock_guard<std::mutex> l(gm);
            gq.push_back({ v, k, content, pqValue, f });
        }
        gcv.notify_one();
    }
    // Frame k's Present, at once (its copy is on the lane behind the gate).
    void LanePresent(const Frame& f, uint32_t k, UINT sync, UINT flags)
    {
        const long long b0 = Now();
        if (realWait)
            WaitForSingleObject(realWait, 100);
        statLaneBlock += static_cast<double>(Now() - b0) * 1000.0 / freq();
        if (k == 1)
            Meter(f.j, f.nGen);
        Removed("our last submission, before our Present (lane)");
        const long long p0 = Now();
        const HRESULT hr = RealPresent(f.j, sync, flags, k <= f.nGen, k == 1);
        // diagnostics (the test PC's ~1.1 s gate stalls with V-Sync): a Present call that blocked
        const double pms = static_cast<double>(Now() - p0) * 1000.0 / freq();
        const double wms = static_cast<double>(p0 - b0) * 1000.0 / freq();
        if ((pms > 250.0 || wms > 250.0) && slowPresentLogs < 30)
        {
            ++slowPresentLogs;
            XLog("gate: present %u of %u (sync %u, flags 0x%x) blocked: waitable %.0f ms, Present call %.0f ms (0x%08x); gate queued up to "
                 "%llu, released up to %llu; lane done %llu of %llu",
                 k, f.nGen + 1, sync, flags, wms, pms, static_cast<unsigned>(hr), static_cast<unsigned long long>(gateValue.load()),
                 static_cast<unsigned long long>(gateSig), static_cast<unsigned long long>(pqFence->GetCompletedValue()),
                 static_cast<unsigned long long>(pqValue));
        }
        if (FAILED(hr))
            PresentFailed(hr, f.j, k, f.nGen);
        lastAnyPresent = Now();
        if (k == f.nGen + 1)
            lastReal = lastAnyPresent;
    }

    // The release thread: each present in turn, at its clock time (frames off the clock at once), not before its picture
    // is done - so the clock carries on from when it really went out, as Show's did.
    void StartGate()
    {
        if (!pq || gater.joinable())
            return;
        // CUDA path, GenSpread: the stream waits on the gate fence before each later pass (RunCuda).
        if (!D3d() && genSpread && pg->WantsGateFence())
        {
            HANDLE fh = nullptr;
            if (SUCCEEDED(dev->CreateSharedHandle(gateFence, nullptr, GENERIC_ALL, nullptr, &fh)))
            {
                pg->ImportGateFence(fh);
                CloseHandle(fh);
            }
            XLog("gate: CUDA passes %s", pg->HasGateFence() ? "spread (the stream waits on the gate)" : "NOT spread: could not import the gate fence");
        }
        gQuit = false;
        // The relay. CUDA path: io.fence's progress, waited for on the CPU, into contentFence for the lane's GPU waits
        // (GateWait). D3D12 path: genFence's progress as the clock's done times (ClockAfter; the CUDA path stamps them with
        // host functions on the stream).
        ID3D12Fence* src = D3d() ? genFence : pg->DoneFence();
        if (D3d() && src)
            stampsWork = true;
        relay = std::thread([this, src] {
            if (!src)
                return;
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            UINT64 sent = src->GetCompletedValue();
            while (!gQuit)
            {
                const UINT64 done = src->GetCompletedValue();
                if (done == UINT64_MAX)
                {
                    Sleep(50);              // device removed
                    continue;
                }
                if (done > sent)
                {
                    if (D3d())
                    {
                        const long long now = Now();
                        for (UINT64 v = std::max(sent + 1, done > kStamps ? done - kStamps + 1 : 1); v <= done; ++v)
                        {
                            doneAt[v % kStamps].store(now, std::memory_order_relaxed);
                            doneVal[v % kStamps].store(v, std::memory_order_release);
                        }
                    }
                    else
                        contentFence->Signal(done);
                    sent = done;
                    continue;
                }
                src->SetEventOnCompletion(sent + 1, ev);
                WaitForSingleObject(ev, 50);
            }
            CloseHandle(ev);
        });
        gater = std::thread([this] {
            SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
            HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
            long long first = 0, last = 0;
            for (;;)
            {
                GateItem it;
                {
                    const long long i0 = Now();
                    std::unique_lock<std::mutex> l(gm);
                    gcv.wait(l, [this] { return gQuit || !gq.empty(); });
                    if (gq.empty())
                        break;
                    it = gq.front();
                    gq.pop_front();
                    // diagnostics: the release thread idle, nothing queued by the presenter for that long (FG on only)
                    const double ims = static_cast<double>(Now() - i0) * 1000.0 / freq();
                    if (ims > 250.0 && it.f.nGen && slowIdleLogs < 30)
                    {
                        ++slowIdleLogs;
                        XLog("gate: release thread idle %.0f ms before present %llu (k %u of %u): the presenter queued nothing; "
                             "lane done %llu of %llu, generation done %llu of %llu", ims, static_cast<unsigned long long>(it.v), it.k,
                             it.f.nGen + 1, static_cast<unsigned long long>(pqFence->GetCompletedValue()),
                             static_cast<unsigned long long>(pqValue), static_cast<unsigned long long>(genFence ? genFence->GetCompletedValue() : 0),
                             static_cast<unsigned long long>(genValue));
                    }
                }
                const Frame& f = it.f;
                long long slot = 0;         // its clock time; metered (Meter), let out FlipLead before it
                if (f.clock)
                {
                    {
                        std::lock_guard<std::mutex> l(clkMu);
                        slot = ClockTarget(f, it.k);
                    }
                    WaitUntil(it.k > 1 && Metered(f.j) ? slot - FlipLead(f.cstep) : slot);
                }
                // Released once its copy is done, not only its picture: a copy still waiting (for the relay, for the GPU) put
                // the present right before the next one and the compositor dropped it (NBA 2K27 5x: 30% not shown, mostly
                // the first generated frames). Late, it goes out late and the clock spaces the next one from it.
                bool slip = false;
                if (pqFence->GetCompletedValue() < it.copied)
                {
                    slip = true;
                    gWaitingFor = it.content;
                    pqFence->SetEventOnCompletion(it.copied, ev);
                    const long long w0 = Now();
                    const DWORD wr = WaitForSingleObject(ev, 1000);
                    gWaitingFor = 0;
                    // diagnostics (the test PC's ~1.1 s stalls with V-Sync): what the copy was behind when the wait ran long
                    const double wms = static_cast<double>(Now() - w0) * 1000.0 / freq();
                    if (wms > 250.0 && slowCopyLogs < 30)
                    {
                        ++slowCopyLogs;
                        UINT lastCount = 0;
                        DXGI_FRAME_STATISTICS fs {};
                        HRESULT fsr = E_FAIL;
                        if (real)
                        {
                            real->GetLastPresentCount(&lastCount);
                            fsr = real->GetFrameStatistics(&fs);
                        }
                        XLog("gate: present %llu (k %u of %u, sync %u) released %.0f ms late%s: its copy at lane value %llu, lane done %llu; "
                             "its picture at %llu, generation done %llu; released up to %llu, queued up to %llu; real swap chain: %u "
                             "presents called, statistics %s (present %u, refresh %u, sync refresh %u)",
                             static_cast<unsigned long long>(it.v), it.k, f.nGen + 1, f.j.sync, wms, wr == WAIT_TIMEOUT ? " (timed out)" : "",
                             static_cast<unsigned long long>(it.copied), static_cast<unsigned long long>(pqFence->GetCompletedValue()),
                             static_cast<unsigned long long>(it.content), static_cast<unsigned long long>(genFence ? genFence->GetCompletedValue() : 0),
                             static_cast<unsigned long long>(gateSig), static_cast<unsigned long long>(gateValue.load()), lastCount,
                             SUCCEEDED(fsr) ? "ok" : "unavailable", fs.PresentCount, fs.PresentRefreshCount, fs.SyncRefreshCount);
                    }
                }
                const long long t = Now();
                GateRelease(it.v);
                if (paceLog)
                    PaceRecord(t, f.ready, slot, f.j.frame, it.k, f.nGen + 1,
                               static_cast<uint8_t>(f.clock && static_cast<double>(t - slot) * 1000.0 / freq() > 0.5 ? kPaceLate : 0));
                std::lock_guard<std::mutex> l(clkMu);
                if (last)
                {
                    const double ms = static_cast<double>(t - last) * 1000.0 / freq();
                    statRel += ms;
                    statRel2 += ms * ms;
                    statRelMax = std::max(statRelMax, ms);
                    ++statRels;
                }
                last = t;
                statQueued += static_cast<double>(gateValue.load() - it.v);
                if (!f.clock)
                {
                    clkLast = 0;            // off the clock: set it anew (ClockTarget)
                    continue;
                }
                statClkSlips += slip;
                clkLast = std::max(t, slot);
                if (it.k == 1)
                    first = t;
                if (it.k == f.nGen + 1)
                    ClockAfter(f, std::exchange(first, 0));
            }
            CloseHandle(ev);
        });
    }
    // Releases presents up to v (never back: GateFlush may have released more).
    void GateRelease(UINT64 v)
    {
        std::lock_guard<std::mutex> l(gsm);
        if (v > gateSig)
            gateFence->Signal(gateSig = v);
    }
    // Everything at the gate goes out now (a resize, the swap chain's end); the presenter is idle.
    void GateFlush()
    {
        if (!pq)
            return;
        {
            std::lock_guard<std::mutex> l(gm);
            gq.clear();
        }
        GateRelease(gateValue.load());
    }
    void StopGate()
    {
        if (!gater.joinable())
            return;
        GateFlush();
        {
            std::lock_guard<std::mutex> l(gm);
            gQuit = true;
        }
        gcv.notify_all();
        gater.join();
        if (relay.joinable())
            relay.join();
        // Nothing on the lane may wait for frames the relay will not pass on any more: up to the last value CUDA was given.
        contentFence->Signal(std::max(contentFence->GetCompletedValue(), fv));
    }

    // The present lane's command list (Begin / End for our queue).
    ID3D12GraphicsCommandList* BeginP()
    {
        const size_t i = pqNext++ % kAllocs;
        if (pqFence->GetCompletedValue() < pqAllocFence[i])
        {
            const long long b0 = Now();
            pqFence->SetEventOnCompletion(pqAllocFence[i], pqEvent);
            WaitForSingleObject(pqEvent, 10000);
            statLaneBlock += static_cast<double>(Now() - b0) * 1000.0 / freq();
        }
        ReadLaneCopyStamp(i);
        pqAlloc[i]->Reset();
        pqList->Reset(pqAlloc[i], nullptr);
        pqCur = i;
        return pqList;
    }
    void EndP()
    {
        if (laneCopyActive)
            EndLaneCopy(pqList);
        if (laneCopyHeap && laneCopyStampedPending)
        {
            const UINT q = static_cast<UINT>(2 * pqCur);
            pqList->ResolveQueryData(laneCopyHeap, D3D12_QUERY_TYPE_TIMESTAMP, q, 2, laneCopyReadback, 16 * pqCur);
            laneCopyStamped[pqCur] = true;
            laneCopyStampedPending = false;
        }
        Submit(pq, pqList, "present lane");
        pq->Signal(pqFence, ++pqValue);
        pqAllocFence[pqCur] = pqValue;
    }

    void InitLaneCopyProfiling()
    {
        if (!gs.prof || !pq)
            return;
        D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, static_cast<UINT>(2 * kAllocs) };
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = 16 * kAllocs;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        HRESULT hr = dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&laneCopyHeap));
        if (SUCCEEDED(hr))
            hr = dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr,
                                              IID_PPV_ARGS(&laneCopyReadback));
        if (SUCCEEDED(hr))
            hr = pq->GetTimestampFrequency(&laneCopyFreq);
        if (FAILED(hr))
        {
            if (laneCopyHeap)
                laneCopyHeap->Release();
            if (laneCopyReadback)
                laneCopyReadback->Release();
            laneCopyHeap = nullptr;
            laneCopyReadback = nullptr;
            XLog("prof: could not initialize present-lane copy timestamps (0x%08x)", static_cast<unsigned>(hr));
            return;
        }
        XLog("prof: present-lane copy timestamps enabled (copy work only; Prof=%u)", gs.profDispatches ? 2 : 1);
    }

    void BeginLaneCopy(ID3D12GraphicsCommandList* g, uint64_t frame, uint32_t k, bool generated)
    {
        if (!laneCopyHeap)
            return;
        const UINT q = static_cast<UINT>(2 * pqCur);
        g->EndQuery(laneCopyHeap, D3D12_QUERY_TYPE_TIMESTAMP, q);
        laneCopyActive = true;
        laneCopyFramePending = frame;
        laneCopyKPending = k;
        laneCopyGeneratedPending = generated;
    }

    void EndLaneCopy(ID3D12GraphicsCommandList* g)
    {
        if (!laneCopyHeap || !laneCopyActive)
            return;
        const UINT q = static_cast<UINT>(2 * pqCur);
        g->EndQuery(laneCopyHeap, D3D12_QUERY_TYPE_TIMESTAMP, q + 1);
        laneCopyActive = false;
        laneCopyStampedPending = true;
        laneCopyFrame[pqCur] = laneCopyFramePending;
        laneCopyK[pqCur] = laneCopyKPending;
        laneCopyGenerated[pqCur] = laneCopyGeneratedPending;
    }

    void ReadLaneCopyStamp(size_t i)
    {
        if (!laneCopyStamped[i])
            return;
        laneCopyStamped[i] = false;
        const D3D12_RANGE r { 16 * i, 16 * i + 16 }, none { 0, 0 };
        UINT64* v = nullptr;
        const HRESULT hr = laneCopyReadback->Map(0, &r, reinterpret_cast<void**>(&v));
        if (FAILED(hr))
        {
            XLog("prof: could not read present-lane copy timestamp for app frame %llu, present %u (0x%08x)",
                 static_cast<unsigned long long>(laneCopyFrame[i]), laneCopyK[i], static_cast<unsigned>(hr));
            return;
        }
        const UINT64* t = reinterpret_cast<const UINT64*>(reinterpret_cast<const uint8_t*>(v) + 16 * i);
        const double ms = double(t[1] - t[0]) * 1000.0 / double(laneCopyFreq);
        laneCopyReadback->Unmap(0, &none);
        if (!laneCopyCount)
            laneCopyFirstFrame = laneCopyFrame[i];
        laneCopyLastFrame = laneCopyFrame[i];
        if (laneCopyGenerated[i])
        {
            laneCopyGeneratedMs += ms;
            ++laneCopyGeneratedCount;
            if (laneCopyK[i] >= 1 && laneCopyK[i] <= kMaxInterp)
            {
                laneCopyGeneratedPassMs[laneCopyK[i] - 1] += ms;
                ++laneCopyGeneratedPassCount[laneCopyK[i] - 1];
            }
        }
        else
        {
            laneCopyAppMs += ms;
            ++laneCopyAppCount;
        }
        if (++laneCopyCount == 240)
        {
            XLog("prof: present-lane copy work over app frames %llu-%llu (%u copies): generated %u avg %.4f ms [k1 %.4f, k2 %.4f, k3 %.4f, k4 %.4f, k5 %.4f], "
                 "own-frame copies %u avg %.4f ms",
                 static_cast<unsigned long long>(laneCopyFirstFrame), static_cast<unsigned long long>(laneCopyLastFrame), laneCopyCount,
                 laneCopyGeneratedCount, laneCopyGeneratedCount ? laneCopyGeneratedMs / laneCopyGeneratedCount : 0.0,
                 laneCopyGeneratedPassCount[0] ? laneCopyGeneratedPassMs[0] / laneCopyGeneratedPassCount[0] : 0.0,
                 laneCopyGeneratedPassCount[1] ? laneCopyGeneratedPassMs[1] / laneCopyGeneratedPassCount[1] : 0.0,
                 laneCopyGeneratedPassCount[2] ? laneCopyGeneratedPassMs[2] / laneCopyGeneratedPassCount[2] : 0.0,
                 laneCopyGeneratedPassCount[3] ? laneCopyGeneratedPassMs[3] / laneCopyGeneratedPassCount[3] : 0.0,
                 laneCopyGeneratedPassCount[4] ? laneCopyGeneratedPassMs[4] / laneCopyGeneratedPassCount[4] : 0.0,
                 laneCopyAppCount, laneCopyAppCount ? laneCopyAppMs / laneCopyAppCount : 0.0);
            laneCopyCount = laneCopyGeneratedCount = laneCopyAppCount = 0;
            laneCopyGeneratedMs = laneCopyAppMs = 0;
            std::fill(std::begin(laneCopyGeneratedPassCount), std::end(laneCopyGeneratedPassCount), 0);
            std::fill(std::begin(laneCopyGeneratedPassMs), std::end(laneCopyGeneratedPassMs), 0.0);
        }
    }

    // cur -> prev for the next app frame, and with it depth -> depth_prev when the pack keeps the previous frame's
    // depth (the multi-frame model's head reads it): both histories move together every app frame, camera cuts and
    // frames without generated ones included, so prev's colour and depth are always the same frame's.
    void CopyToPrev(ID3D12GraphicsCommandList* g)
    {
        for (auto [dstSlot, srcSlot] : { std::pair<GenSlot, GenSlot> { GenSlot::Prev, GenSlot::Cur }, { GenSlot::DepthPrev, GenSlot::Depth } })
        {
            const GenBuf dst = pg->Buf(dstSlot), src = pg->Buf(srcSlot);
            if (!dst.res)
                continue;
            Out(g, dst.res, D3D12_RESOURCE_STATE_COPY_DEST);
            Out(g, src.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g->CopyBufferRegion(dst.res, 0, src.res, 0, dst.bytes);
            Back(g, src.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Back(g, dst.res, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }

    // The frame is done with cur and prev: cur -> prev for the next one (after all its passes, on our queue). hold: the
    // next frame comes before this one's own present, so its picture goes into hold first (cur or keep is rewritten).
    void HandOff(Frame& f, bool keepFrame)
    {
        // CUDA path: after its last pass, which read cur and prev
        if (!D3d() && f.nGen && !f.j.cut)
            queue->Wait(pg->DoneFence(), f.done[f.nGen - 1]);
        ID3D12GraphicsCommandList* g = Begin();
        if (keepFrame)
        {
            Out(g, f.realSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g->CopyBufferRegion(hold, 0, f.realSrc, 0, pg->Buf(GenSlot::Cur).bytes);
            Back(g, f.realSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
            f.held = true;
        }
        CopyToPrev(g);
        End();
        pg->FrameHandedOff();
        hasPrev = true;
        prevJitter[0] = f.j.jitter[0];
        prevJitter[1] = f.j.jitter[1];
        f.handed = true;
    }

    // XEFG_CUDA_DUMP: this frame's first generated frame is still to be dumped with its inputs (DumpOnce), which the next
    // frame's would overwrite if it came early.
    bool DumpPending(const Job& j) const
    {
        return !dumpPath.empty() && !dumped && (dumpFrame < 0 || j.frame == static_cast<uint64_t>(dumpFrame));
    }

    // The next application frame is waiting, can generate, and is done on the GPU.
    bool NextReady()
    {
        if (genQueue || early.valid || noEarly)
            return false;
        std::lock_guard<std::mutex> l(pm);
        return !pjobs.empty() && pjobs.front().usable && appFence->GetCompletedValue() >= pjobs.front().frame + 1;
    }

    // The next application frame, when it is waiting and already done on the GPU: its inputs taken and its generated
    // frames started now (Pick, Submit), not after this frame's own present. Under a GPU the game keeps busy the first
    // generated frame otherwise had one step's time for the head and its pass and went out late; the D3D12 path takes
    // only the inputs here (it records each pass before its present). The presenter loop shows it next.
    void Early()
    {
        if (!NextReady())
            return;
        Job n;
        {
            std::lock_guard<std::mutex> l(pm);
            n = pjobs.front();
            pjobs.pop_front();
        }
        pcv.notify_all();
        Pick(n);
        early = Submit(n);
        early.tookEarly = true;
        ++statEarly;
    }

    // One application frame with the generation lane. The lane takes the inputs and runs every pass of the
    // frame at once, each into a ring buffer of its own, then cur -> prev; the present queue only waits for
    // frame k on the GPU, copies it into the swap chain's current buffer and presents it. When a present's
    // time comes its frame is long done, and its GPU work is one copy, not the whole network: the present
    // queue only gets the GPU in between the game's command lists, a compute lane runs alongside them.
    // Swap chain buffers are written only on the present queue, and only the current one (anything else is
    // device removal: debug layer id 907, DXGI_ERROR_ACCESS_DENIED).
    void PresentGen(const Job& j, ID3D12Resource* bb)
    {
        const Resource& ox = pg->Output();
        // The previous frame's copies on the present queue read cur, keep and the ring, which this one writes.
        genQueue->Wait(hostFence, hostValue);
        genQueue->Wait(appFence, j.frame + 1);
        ID3D12Resource* realSrc;
        {
            ID3D12GraphicsCommandList* g = BeginGen();
            realSrc = CopyFrame(g, j, bb);
            EndGen();
        }
        Consumed(j, genQueue);

        const bool gen = hasPrev && !j.reset;
        const uint32_t nGen = !gen ? 0 : j.passes ? j.nInterp : 1;
        if (nGen != lastNGen)
        {
            XLog("present: %u generated frame(s) per app frame (numInterp %u, max %u, pack %s the per-pass patches)", nGen, numInterp,
                 maxInterp, j.passes ? "has" : "LACKS - run tools/pack_add_patches.py on it");
            lastNGen = nGen;
        }
        float mvs[2] = { j.mvs[0], j.mvs[1] };
        if (mvs[0] == 0 && mvs[1] == 0)
            pg->MvScale(mvs[0], mvs[1]);
        UINT64 ready[kMaxInterp + 1] {};
        for (uint32_t k = 1; k <= nGen && !j.cut; ++k)
        {
            if (!ring[k - 1] && !(ring[k - 1] = PlainBuffer(pg->Buf(GenSlot::Output).bytes, true)))
                break;
            const float jc[2] = { j.jitter[0], j.jitter[1] };
            const float jp[2] = { prevJitter[0], prevJitter[1] };
            pg->PreparePass(k, nGen, jc, jp, mvs);
            ID3D12GraphicsCommandList* g = BeginGen();
            RecordPassInto(g, k, ring[k - 1]);
            ready[k - 1] = EndGen();
            DumpOnce(j, k);
        }
        {
            // cur -> prev for the next frame, once every pass read them.
            ID3D12GraphicsCommandList* g = BeginGen();
            CopyToPrev(g);
            ready[nGen] = EndGen();
        }

        for (uint32_t k = 1; k <= nGen + 1; ++k)
        {
            const bool app = k > nGen;
            // a camera cut puts the app's frame in the generated frames' places (as Intel copies it into its output)
            ID3D12Resource* src = !app ? (j.cut ? realSrc : ring[k - 1]) : j.dbgOnly && nGen && !j.cut ? ring[nGen - 1] : realSrc;
            if (!src)
                continue;
            // The app's frame after cur -> prev: both read cur, and its state must not change on two queues at once.
            queue->Wait(genFence, ready[app || j.cut ? nGen : k - 1]);
            ID3D12Resource* rb = RealBuffer();
            ID3D12GraphicsCommandList* g = Begin();
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            Out(g, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (app || j.cut)
                Copy(g, rb, src, ox, true);
            else
                CopyGenerated(g, rb, src, realSrc, ox);
            Back(g, src, D3D12_RESOURCE_STATE_COPY_SOURCE);
            if (!app && j.dbgTag)
                DrawTags(g, rb, ox);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            End();
            rb->Release();
            PresentReal(j, k, nGen, j.sync, app ? j.flags : j.flags & DXGI_PRESENT_ALLOW_TEARING);
            if (!app && (++genCount == 1 || genCount % 600 == 0))
                XLog("present: %u frames generated", genCount);
        }
        hasPrev = true;
        prevJitter[0] = j.jitter[0];
        prevJitter[1] = j.jitter[1];
        queue->Signal(retireFence, j.frame + 1);
        Stats(j, nGen);
    }

    // Gate=1 on the D3D12 path: PresentGen's generation on the generation lane, the presents behind the gate on the present
    // lane (ShowGated's rules). The CUDA path in NBA 2K27 left the compositor without the GPU for 15-20 ms each game frame
    // (30% not shown; run F on this path: 1.5%). What the present lane reads is per parity (ringG[set], gateHeld[set]); the
    // generation lane waits for it (pqRead) only before rewriting a set. The lane waits on genFence itself (no CUDA fence
    // here); the relay stamps genFence's progress as the clock's done times (StartGate).
    void PresentGenGated(const Job& j, ID3D12Resource* bb)
    {
        const Resource& ox = pg->Output();
        const uint32_t set = static_cast<uint32_t>(j.frame & 1);
        genQueue->Wait(appFence, j.frame + 1);
        ID3D12Resource* realSrc;
        {
            ID3D12GraphicsCommandList* g = BeginGen();
            realSrc = CopyFrame(g, j, bb);
            EndGen();
        }
        Consumed(j, genQueue);
        genQueue->Wait(pqFence, pqRead[set]);
        if (!gateHeld[set] && !(gateHeld[set] = PlainBuffer(pg->Buf(GenSlot::Cur).bytes)))
            XLog("gate: no buffer for the frame's own picture - the present lane reads it in place");
        ID3D12Resource* frame = gateHeld[set] ? gateHeld[set] : realSrc;
        if (gateHeld[set])
        {
            ID3D12GraphicsCommandList* g = BeginGen();
            Out(g, realSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
            g->CopyBufferRegion(gateHeld[set], 0, realSrc, 0, pg->Buf(GenSlot::Cur).bytes);
            Back(g, realSrc, D3D12_RESOURCE_STATE_COPY_SOURCE);
            EndGen();
        }
        const UINT64 heldAt = genValue;

        const bool gen = hasPrev && !j.reset;
        uint32_t nGen = !gen ? 0 : j.passes ? j.nInterp : 1;
        if (nGen != lastNGen)
        {
            XLog("present: %u generated frame(s) per app frame (numInterp %u, max %u, pack %s the per-pass patches)", nGen, numInterp,
                 maxInterp, j.passes ? "has" : "LACKS - run tools/pack_add_patches.py on it");
            lastNGen = nGen;
        }
        float mvs[2] = { j.mvs[0], j.mvs[1] };
        if (mvs[0] == 0 && mvs[1] == 0)
            pg->MvScale(mvs[0], mvs[1]);
        Frame f;
        f.j = j;
        f.valid = true;
        f.set = set;
        f.ready = jobBase;
        f.pickT = pickT;
        // Spread (GenSpread = s, 2 by default): pass k > 1 starts once the present s places before its frame is released
        // (gateValue now: this thread alone hands out the gate's values, so this frame's presents get the next ones). Submitted
        // at once, the ~15 ms block of passes kept the compositor off the GPU and the frames released during it were dropped
        // (NBA 2K27 runs G/H: 15-16%; run F, a pass just before each present: 1.5%; run I, spread: 2.8%). Pass 1 starts at
        // once and alone tells the clock its need (f.spread): the later passes' done times follow the releases, so taken as
        // need they raised the delay that starts them (run I: 45-55 ms after a hitch, falling slowly; at once: 12 ms). s = 1
        // ran away in the harness: a pass gets one step and takes longer under load.
        // Display-paced (V-Sync): no spread, the gate holds nothing (GateWait).
        const UINT64 gateBase = gateValue.load();
        const uint32_t spread = DisplayPaced(j) ? 0 : genSpread;
        for (uint32_t k = 1; k <= nGen && !j.cut; ++k)
        {
            if (!ringG[set][k - 1] && !(ringG[set][k - 1] = PlainBuffer(pg->Buf(GenSlot::Output).bytes, true)))
            {
                nGen = k - 1;               // as many as have a buffer
                break;
            }
            if (spread && k > 1 && gateBase + k > spread)
                genQueue->Wait(gateFence, gateBase + k - spread);
            const float jc[2] = { j.jitter[0], j.jitter[1] };
            const float jp[2] = { prevJitter[0], prevJitter[1] };
            pg->PreparePass(k, nGen, jc, jp, mvs);
            ID3D12GraphicsCommandList* g = BeginGen();
            RecordPassInto(g, k, ringG[set][k - 1]);
            f.done[k - 1] = EndGen();
            DumpOnce(j, k);
        }
        {
            // cur -> prev for the next frame, once every pass read them.
            ID3D12GraphicsCommandList* g = BeginGen();
            CopyToPrev(g);
            EndGen();
        }
        hasPrev = true;
        prevJitter[0] = j.jitter[0];
        prevJitter[1] = j.jitter[1];
        f.nGen = nGen;
        f.spread = spread > 0;
        SetClock(f, clockPacer && nGen && !DisplayPaced(j));

        showing = &f;
        pq->Wait(genFence, heldAt);
        for (uint32_t k = 1; k <= nGen; ++k)
        {
            if (!j.cut)
                pq->Wait(genFence, f.done[k - 1]);
            ID3D12Resource* rb = RealBuffer();
            ID3D12GraphicsCommandList* g = BeginP();
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            if (j.cut)
            {
                BeginLaneCopy(g, j.frame, k, false);
                Copy(g, rb, frame, ox, true);
            }
            else
            {
                BeginLaneCopy(g, j.frame, k, true);
                CopyGenerated(g, rb, ringG[set][k - 1], frame, ox);
            }
            EndLaneCopy(g);
            if (j.dbgTag)
                DrawTags(g, rb, ox);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            EndP();
            rb->Release();
            GateWait(f, k, j.cut ? 0 : f.done[k - 1]);
            LanePresent(f, k, j.sync, j.flags & DXGI_PRESENT_ALLOW_TEARING);
            if (++genCount == 1 || genCount % 600 == 0)
                XLog("present: %u frames generated", genCount);
        }
        {
            ID3D12Resource* rb = RealBuffer();
            ID3D12Resource* src = j.dbgOnly && nGen && !j.cut ? ringG[set][nGen - 1] : frame;
            ID3D12GraphicsCommandList* g = BeginP();
            Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_DEST);
            BeginLaneCopy(g, j.frame, nGen + 1, false);
            Copy(g, rb, src, ox, true);
            EndLaneCopy(g);
            Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_PRESENT);
            EndP();
            pqRead[set] = pqValue;
            rb->Release();
        }
        GateWait(f, nGen + 1, 0);
        LanePresent(f, nGen + 1, j.sync, j.flags);
        pq->Signal(retireFence, j.frame + 1);   // retired once its own frame went out (ShowGated)
        Stats(j, nGen);
        showing = nullptr;
    }

    ID3D12GraphicsCommandList* BeginGen()
    {
        const size_t i = genNext++ % kAllocs;
        if (genFence->GetCompletedValue() < genAllocFence[i])
        {
            const long long a0 = Now();
            genFence->SetEventOnCompletion(genAllocFence[i], genEvent);
            WaitForSingleObject(genEvent, 10000);
            // diagnostics: the presenter blocked on the generation lane (its allocators all in flight)
            const double ams = static_cast<double>(Now() - a0) * 1000.0 / freq();
            if (ams > 250.0 && slowGenLogs < 30)
            {
                ++slowGenLogs;
                XLog("gen: waited %.0f ms for a free allocator: generation done %llu of %llu, lane done %llu of %llu, gate released up to "
                     "%llu of %llu", ams, static_cast<unsigned long long>(genFence->GetCompletedValue()), static_cast<unsigned long long>(genValue),
                     static_cast<unsigned long long>(pqFence ? pqFence->GetCompletedValue() : 0), static_cast<unsigned long long>(pqValue),
                     static_cast<unsigned long long>(gateSig), static_cast<unsigned long long>(gateValue.load()));
            }
        }
        ReadStamps(kAllocs + i);
        genAlloc[i]->Reset();
        genList->Reset(genAlloc[i], nullptr);
        genCur = i;
        return genList;
    }
    // Submits the generation list; returns the fence value it is done at.
    UINT64 EndGen()
    {
        Submit(genQueue, genList, "generation list");
        genQueue->Signal(genFence, ++genValue);
        genAllocFence[genCur] = genValue;
        return genValue;
    }
    void ReleaseRing()
    {
        for (ID3D12Resource*& r : ring)
        {
            if (r)
                r->Release();
            r = nullptr;
        }
        for (auto& s2 : ringG)
            for (ID3D12Resource*& r : s2)
            {
                if (r)
                    r->Release();
                r = nullptr;
            }
    }

    // The frame's inputs are taken: the application's queue may go on (its next frame waits for this), and
    // the application's frame-latency wait is released. Signalled on the queue that took them.
    void Consumed(const Job& j, ID3D12CommandQueue* on = nullptr)
    {
        (on ? on : queue)->Signal(consumedFence, j.frame + 1);
        if (latencySem)
            ReleaseSemaphore(latencySem, 1, nullptr);
    }

    // Debug (XEFG_CUDA_DUMP): the first generated frame, tightly packed, to compare with a pack's expectation.
    // XEFG_CUDA_DUMP_FRAME=N: the first generated frame of app frame N instead (a fixed frame to compare runs by).
    void DumpOnce(const Job& j, uint32_t k)
    {
        if (dumpPath.empty() || dumped || (dumpFrame >= 0 && (j.frame != static_cast<uint64_t>(dumpFrame) || k != 1)))
            return;
        dumped = true;
        std::vector<uint8_t> px;
        WaitIdle();
        const bool ok = pg->DownloadOutput(px, static_cast<uint32_t>(j.frame & 1), k);
        // and this frame's inputs next to it (<dump>.frame_cur.bin ...), for pack_run --inputs
        for (const auto& [role, slot] : { std::pair<const char*, GenSlot> { "frame_cur", GenSlot::Cur }, { "frame_prev", GenSlot::Prev },
                                          { "motion", GenSlot::Motion }, { "depth", GenSlot::Depth }, { "depth_prev", GenSlot::DepthPrev } })
        {
            if (!pg->HasSlot(slot))
                continue;
            std::vector<uint8_t> v;
            FILE* fi = nullptr;
            if (pg->DownloadInput(slot, v) && !fopen_s(&fi, (dumpPath + "." + role + ".bin").c_str(), "wb") && fi)
            {
                fwrite(v.data(), 1, v.size(), fi);
                fclose(fi);
            }
        }
        FILE* f = nullptr;
        if (ok && !fopen_s(&f, dumpPath.c_str(), "wb") && f)
        {
            fwrite(px.data(), 1, px.size(), f);
            fclose(f);
            XLog("dumped the first generated frame of app frame %llu (%zu bytes) to %s", static_cast<unsigned long long>(j.frame), px.size(),
                 dumpPath.c_str());
        }
    }

    // Debug (XEFG_CUDA_DUMP_SHOWN with XEFG_CUDA_DUMP_FRAME=N): each picture presented for app frame N as it is in the
    // swap chain's buffer, <path>.k<k>.bin with tightly packed rows, to compare what reaches the screen between builds.
    void DumpShown(const Job& j, uint32_t k, ID3D12Resource* rb, bool held)
    {
        if (shownPath.empty() || dumpFrame < 0 || j.frame != static_cast<uint64_t>(dumpFrame))
            return;
        const D3D12_RESOURCE_DESC d = rb->GetDesc();
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT rows = 0;
        UINT64 rowBytes = 0, total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, &rows, &rowBytes, &total);
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
        D3D12_RESOURCE_DESC bd {};
        bd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        bd.Width = total;
        bd.Height = bd.DepthOrArraySize = bd.MipLevels = 1;
        bd.SampleDesc.Count = 1;
        bd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* back = nullptr;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &bd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&back))))
            return;
        ID3D12GraphicsCommandList* g = Begin();
        Barrier(g, rb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        D3D12_TEXTURE_COPY_LOCATION dst { back, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, src { rb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
        dst.PlacedFootprint = fp;
        src.SubresourceIndex = 0;
        g->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
        Barrier(g, rb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        End();
        WaitIdle();
        uint8_t* p = nullptr;
        FILE* f = nullptr;
        if (SUCCEEDED(back->Map(0, nullptr, reinterpret_cast<void**>(&p))))
        {
            if (!fopen_s(&f, (shownPath + ".k" + std::to_string(k) + ".bin").c_str(), "wb") && f)
            {
                for (UINT y = 0; y < rows; ++y)
                    fwrite(p + fp.Offset + static_cast<size_t>(y) * fp.Footprint.RowPitch, 1, static_cast<size_t>(rowBytes), f);
                fclose(f);
                XLog("dumped presented picture %u of app frame %llu (%s)", k, static_cast<unsigned long long>(j.frame),
                     held ? "its own picture from hold" : "its own picture in place");
            }
            const D3D12_RANGE none { 0, 0 };
            back->Unmap(0, &none);
        }
        back->Release();
    }

    // FlipStats=1: after each real Present, the swap chain's own frame statistics. A step of exactly one present gives the spacing of
    // two flips (SyncQPCTime delta); a larger step or a failed call (DXGI_ERROR_FRAME_STATISTICS_DISJOINT among them) only counts.
    // No GPU work, presenter thread only.
    void FlipStatsSample()
    {
        DXGI_FRAME_STATISTICS s {};
        if (!real || FAILED(real->GetFrameStatistics(&s)))
        {
            ++fsFailed;
            fsHave = false;
            return;
        }
        if (fsHave && s.PresentCount != fsLast.PresentCount)
        {
            if (s.PresentCount - fsLast.PresentCount == 1)
            {
                if (fsSteps.size() < 4096)
                    fsSteps.push_back(static_cast<double>(s.SyncQPCTime.QuadPart - fsLast.SyncQPCTime.QuadPart) * 1000.0 / freq());
            }
            else
                ++fsLarger;
        }
        fsLast = s;
        fsHave = true;
    }
    void FlipStatsLog()
    {
        std::vector<double> v;
        v.swap(fsSteps);
        std::sort(v.begin(), v.end());
        double avg = 0, sd = 0, p50 = 0, p95 = 0, p99 = 0, over = 0;
        if (!v.empty())
        {
            for (double x : v)
                avg += x;
            avg /= static_cast<double>(v.size());
            for (double x : v)
                sd += (x - avg) * (x - avg);
            sd = std::sqrt(sd / static_cast<double>(v.size()));
            const auto at = [&](double q) { return v[std::min(v.size() - 1, static_cast<size_t>(q * static_cast<double>(v.size())))]; };
            p50 = at(0.5);
            p95 = at(0.95);
            p99 = at(0.99);
            for (double x : v)
                over += x > 1.5 * p50;
            over = 100.0 * over / static_cast<double>(v.size());
        }
        XLog("stats: flip spacing %.2f ms avg, sd %.2f, p50 %.2f, p95 %.2f, p99 %.2f, %.1f%% over 1.5x p50, %u steps of 1, %u larger steps, %u calls failed",
             avg, sd, p50, p95, p99, over, static_cast<unsigned>(v.size()), fsLarger, fsFailed);
        fsLarger = fsFailed = 0;
    }

    // Every 120 app frames: how the application and the presenter are doing.
    void Stats(const Job& j, uint32_t nGen)
    {
        if (++statFrames < 120)
            return;
        const double sd = statGaps ? std::sqrt(std::max(0.0, statGap2 / statGaps - (statGap / statGaps) * (statGap / statGaps))) : 0;
        const double iv = interval;
        XLog("stats: app interval %.2f ms (%.1f fps), on the GPU %.2f ms, sync %u, %u generated per frame; app's Present %.3f ms (of it "
             "waiting for the presenter %.3f); presents every %.2f ms (sd %.2f, max %.2f; target %.2f), waited for our GPU work %.2f ms "
             "per present, %u frames started behind, %u taken early, schedule lag %.1f ms, %u CUDA handoff failures (avg of 120)",
             iv, 1000.0 / iv, ri.ms, j.sync, nGen, statAppCall / 120, statAppWait / 120, statGaps ? statGap / statGaps : 0.0, sd,
             statGapMax, ri.ms / (nGen + 1), statGaps ? statGpuWait / statGaps : 0.0, statLate, statEarly, paceLag, pg->TakeHandoffFails());
        XLog("stats: ready stamp %.1f ms old when the presenter took the frame, presenter %.2f app frames behind, first generated "
             "frame %.1f ms after its target; from the app's Present call: taken %.1f ms, ready stamp %.1f ms (avg of 120)",
             statJobs ? statStampAge / statJobs : 0.0, statJobs ? statBehind / statJobs : 0.0, statFirstN ? statFirstLate / statFirstN : 0.0,
             statJobs ? statArriveToPick / statJobs : 0.0, statJobs ? statArriveToReady / statJobs : 0.0);
        if (flipStats)
            FlipStatsLog();
        std::lock_guard<std::mutex> clk(clkMu);
        if (statRels)
        {
            const double m = statRel / statRels;
            XLog("stats: gate releases every %.2f ms (sd %.2f, max %.2f), %.1f presents behind it on release, presenter blocked "
                 "by the lane %.2f ms per app frame (avg of 120)", m, std::sqrt(std::max(0.0, statRel2 / statRels - m * m)), statRelMax,
                 statQueued / statRels, statLaneBlock / 120);
        }
        statRel = statRel2 = statRelMax = statQueued = statLaneBlock = 0;
        statRels = 0;
        if (statClkN)
            XLog("stats: clock app frame %.2f ms (sd %.2f), delay %.1f ms (target %.1f, needed %.1f), correction %+.0f ppm, %u frames "
                 "not done at their time, %u app frame times left out, done times %s (avg of 120)",
                 clkBatch, std::sqrt(clkVar), statClkDelay / statClkN, clkTarget, statClkNeed / statClkN, clkCorr * 1e6, statClkSlips,
                 statClkHitches, stampsWork ? "from the stream" : "MISSING (half an app frame)");
        statClkDelay = statClkNeed = 0;
        statClkN = statClkSlips = statClkHitches = 0;
        if (flipFn)
            XLog("stats: flip metering on for %u of 120 app frames, %.2f ms per batch, status %d, feedback 0x%x, %u calls", statFlipMetered,
                 flipBatchMs, flipStatus, flipFeedback, flipCalls);
        statFlipMetered = 0;
        if (statLeadFrames)
            XLog("stats: pace lead %.2f ms (%.2f - %.2f over %u app frames, %s, window max %.2f), %u generated frames late", leadState.ms,
                 leadMin, leadMax, statLeadFrames, leadState.automatic ? "auto" : "fixed", leadState.ringMax, statLeadLate);
        leadMin = 1e9;
        leadMax = 0;
        statLeadLate = statLeadFrames = 0;
        statFrames = statLate = statEarly = statGaps = statJobs = statFirstN = 0;
        statGap = statGap2 = statGapMax = statAppCall = statAppWait = statGpuWait = statStampAge = statBehind = statFirstLate = statArriveToPick = statArriveToReady = 0;
    }

    // ---- build ---------------------------------------------------------------------------------------

    // The generator's build: the host's part of it runs at the points the generator names (GenBuild).
    GenBuild BuildArgs()
    {
        GenBuild b;
        b.initFlags = initFlags;
        b.hdr = IsHdr10(scDesc.Format);
        b.releaseHostBuffers = [this] { ReleaseForBuild(); };
        b.packBuilt = [this] { return PackBuilt(nullptr); };
        return b;
    }

    bool Build(const GenBuild* given = nullptr)
    {
        if (!given)
            ReadDumpEnv();      // a deferred build read them before its thread started (StartDeferredBuild)
        return generator->Build(given ? *given : BuildArgs());
    }

    void ReadDumpEnv()
    {
        char env[MAX_PATH];
        if (GetEnvironmentVariableA("XEFG_CUDA_DUMP", env, sizeof(env)))
            dumpPath = env;
        if (GetEnvironmentVariableA("XEFG_CUDA_DUMP_FRAME", env, sizeof(env)))
            dumpFrame = atoi(env);
        if (GetEnvironmentVariableA("XEFG_CUDA_DUMP_SHOWN", env, sizeof(env)))
            shownPath = env;
    }

    // The old pack is gone: what was sized by it goes too (the generator's Build / Rebuild call this).
    void ReleaseForBuild()
    {
        if (keep)
            keep->Release();
        if (hold)
            hold->Release();
        keep = hold = nullptr;
        for (ID3D12Resource*& r : gateHeld)     // sized as cur (Gate=1); the present lane is idle here (WaitIdle)
        {
            if (r)
                r->Release();
            r = nullptr;
        }
        ReleaseStaging();
        ReleaseRing();
        hasPrev = lastUsable = false;
    }

    // The new pack is built: the staging sets the application's tags land in, sized by its buffers.
    // `snap`: the settings a deferred build took under mu when it started (the application's threads may change the live ones
    // meanwhile); null for a build on the caller's own thread.
    struct BuildSnap { uint32_t initFlags; int uiMode; bool uiComposition; };
    bool PackBuilt(const BuildSnap* snap)
    {
        for (int s = 0; s < 2; ++s)
        {
            stMv[s] = PlainBuffer(pg->Buf(GenSlot::Motion).bytes, true);       // UAV: ConvertIn writes them
            stDepth[s] = PlainBuffer(pg->Buf(GenSlot::Depth).bytes, true);
            if (!stMv[s] || !stDepth[s])
                return Fail("staging buffers");
        }
        const std::string mods = pg->VariantModules();
        const Sizes& z = pg->PackSize();
        XLog("built: pack %s%s, %zu dispatches, output %ux%u, render %ux%u (%s), flags 0x%x, UI mode %d composition %d%s, variant "
             "modules:%s", packDir.c_str(), pg->PackHdr() ? "\\hdr" : "", pg->DispatchCount(), z.OW, z.OH, z.RW, z.RH,
             D3d() ? "D3D12" : "CUDA", snap ? snap->initFlags : initFlags, snap ? snap->uiMode : static_cast<int>(initParams.uiMode),
             (snap ? snap->uiComposition : uiComposition.load()) ? 1 : 0,
             !pg->HasUiInputs() ? " (pack without UI inputs)" : "", mods.empty() ? " none" : mods.c_str());
        return true;
    }

    ID3D12Resource* PlainBuffer(size_t bytes, bool uav = false)
    {
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<size_t>(bytes, 256);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        ID3D12Resource* b = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b));
        return b;
    }
    // A staging pair sized like the pack resource its tag is copied for (the HUD-less colour: cur; the UI texture: uic).
    bool EnsureStaging(ID3D12Resource* (&st)[2], size_t bytes)
    {
        for (int s = 0; s < 2; ++s)
            if (!st[s])
                st[s] = PlainBuffer(bytes);
        return st[0] && st[1];
    }
    void ReleaseStaging()
    {
        for (ID3D12Resource** p : { &stMv[0], &stMv[1], &stDepth[0], &stDepth[1], &stHud[0], &stHud[1], &stUi[0], &stUi[1],
                                    &stRaw[0][0], &stRaw[0][1], &stRaw[1][0], &stRaw[1][1] })
        {
            if (*p)
                (*p)->Release();
            *p = nullptr;
        }
        for (auto& b : stRawBytes)
            b[0] = b[1] = 0;
        for (ID3D12Resource* r : stRawOld)
            r->Release();
        stRawOld.clear();
        pendConv[0] = pendConv[1] = {};
    }

    // Our backbuffers, as many as the real swap chain has, in its format and size. A swap chain lets an
    // R8G8B8A8/B8G8R8A8 buffer be viewed as UNORM or SRGB; ours allow both through relaxed format casting
    // where the runtime has it.
    bool CreateAppBuffers()
    {
        real->GetDesc1(&scDesc);
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = scDesc.Width;
        rd.Height = scDesc.Height;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = scDesc.Format;
        rd.SampleDesc.Count = 1;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET;
        if (scDesc.BufferUsage & DXGI_USAGE_UNORDERED_ACCESS)
            rd.Flags |= D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        const DXGI_FORMAT twin = SrgbTwin(scDesc.Format);
        ID3D12Device10* d10 = nullptr;
        D3D12_FEATURE_DATA_D3D12_OPTIONS12 o12 {};
        const bool cast = twin != DXGI_FORMAT_UNKNOWN && SUCCEEDED(dev->QueryInterface(IID_PPV_ARGS(&d10))) &&
                          SUCCEEDED(dev->CheckFeatureSupport(D3D12_FEATURE_D3D12_OPTIONS12, &o12, sizeof(o12))) &&
                          o12.RelaxedFormatCastingSupported;
        bool ok = true;
        for (UINT i = 0; i < std::max(1u, scDesc.BufferCount) && ok; ++i)
        {
            ID3D12Resource* b = nullptr;
            if (cast)
            {
                D3D12_RESOURCE_DESC1 r1 {};
                memcpy(&r1, &rd, sizeof(rd));
                const DXGI_FORMAT formats[1] = { twin };
                ok = SUCCEEDED(d10->CreateCommittedResource3(&hp, D3D12_HEAP_FLAG_NONE, &r1, D3D12_BARRIER_LAYOUT_COMMON, nullptr, nullptr, 1,
                                                             formats, IID_PPV_ARGS(&b)));
            }
            else
                ok = SUCCEEDED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_PRESENT, nullptr,
                                                            IID_PPV_ARGS(&b)));
            if (ok)
            {
                b->SetName(L"f5low backbuffer");
                appBufs.push_back(b);
            }
        }
        if (d10)
            d10->Release();
        XLog("swap chain: %zu backbuffers of our own, %ux%u fmt=%d%s", appBufs.size(), scDesc.Width, scDesc.Height,
             static_cast<int>(scDesc.Format), cast ? " (UNORM/SRGB views)" : "");
        return ok;
    }

    void ReleaseSwapChain()
    {
        for (ID3D12Resource* b : appBufs)
            b->Release();
        appBufs.clear();
        if (real)
            real->Release();
        real = nullptr;
        // latencySem stays until ~Own (or the next Wrap): an app may still wait on its duplicate of our handle, and
        // OnPresent keeps releasing it once there is no real swap chain.
        realWait = nullptr;         // owned by the swap chain
    }

    // Waits until our queue has finished everything (and the CUDA worker everything posted).
    void WaitIdle()
    {
        generator->Drain();
        if (pq && pqFence)
        {
            GateFlush();
            pq->Signal(pqFence, ++pqValue);
            if (pqFence->GetCompletedValue() < pqValue)
            {
                pqFence->SetEventOnCompletion(pqValue, pqEvent);
                WaitForSingleObject(pqEvent, 10000);
            }
        }
        if (genQueue)
        {
            genQueue->Signal(genFence, ++genValue);
            if (genFence->GetCompletedValue() < genValue)
            {
                genFence->SetEventOnCompletion(genValue, genEvent);
                WaitForSingleObject(genEvent, 10000);
            }
        }
        if (queue && hostFence)
        {
            queue->Signal(hostFence, ++hostValue);
            if (hostFence->GetCompletedValue() < hostValue)
            {
                hostFence->SetEventOnCompletion(hostValue, hostEvent);
                WaitForSingleObject(hostEvent, 10000);
            }
        }
    }
    // Waits until the application's queue has finished what it submitted (its tag copies into staging).
    void WaitApp()
    {
        if (!appQueue || !idleFence)
            return;
        appQueue->Signal(idleFence, ++idleValue);
        if (idleFence->GetCompletedValue() < idleValue)
        {
            idleFence->SetEventOnCompletion(idleValue, hostEvent);
            WaitForSingleObject(hostEvent, 10000);
        }
    }

    // A "pack 2" follows the game's sizes: output from the swap chain, render from the motion-vector or
    // depth region. Called on the first tag of a frame, before anything of that frame touches the buffers.
    void FollowSizes(const xefg_swapchain_d3d12_resource_data_t& rd)
    {
        if (!pg->HasPack() || pg->PackVersion() < 2)
            return;
        if (rd.type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR || rd.type == XEFG_SWAPCHAIN_RES_DEPTH)
        {
            const D3D12_RESOURCE_DESC d = rd.pResource->GetDesc();
            renderSize[0] = rd.resourceSize.x ? rd.resourceSize.x : static_cast<uint32_t>(d.Width);
            renderSize[1] = rd.resourceSize.y ? rd.resourceSize.y : d.Height;
        }
        if (!real || !renderSize[0] || tagsThisFrame)
            return;
        // The area as tagged, if it still lies inside the output (a resize may have shrunk it since), else the whole.
        const bool inside = area[2] && area[3] && area[0] + static_cast<uint64_t>(area[2]) <= scDesc.Width &&
                            area[1] + static_cast<uint64_t>(area[3]) <= scDesc.Height;
        const Sizes want { scDesc.Width, scDesc.Height, renderSize[0], renderSize[1], inside ? area[0] : 0, inside ? area[1] : 0,
                           inside ? area[2] : 0, inside ? area[3] : 0 };
        if ((want == pg->PackSize() && IsHdr10(scDesc.Format) == pg->PackHdr()) || (want == failedSize && resizeFailed))
            return;
        // As Intel, a tag never waits: the pack is set aside here and rebuilt on the presenter thread (Resize), the
        // frames passing through until then. Rebuilding in the tag call held the game's render thread ~190 ms in its
        // frame, waiting on its queue and making a CUDA context on its device when a variant was new; that was where
        // RTX 30 lost the device the moment frame generation came on (tester logs 20, 21 + Aniimo rounds 2, 3; Intel's
        // tag calls take no time).
        resizeWant = want;
        resizeFrame = appFrame;
        built = false;
        resizePending = true;
    }

    // Presenter thread, before the first frame tagged at the new sizes: nothing of ours is in flight once our queues
    // are idle (the frames before were waited for on the GPU; none since copied into the old buffers: built is false).
    // Takes no mu (see resizePending); the application's threads touch nothing of the pack while built is false.
    void RebuildForSizes(const char* where)
    {
        const Sizes want = resizeWant;
        const long long t0 = Now();
        WaitIdle();
        const bool ok = generator->Rebuild(want, BuildArgs());
        resizeFailed = !ok;
        failedSize = want;
        resizePending = false;
        built = ok;
        XLog("resize to output %ux%u, render %ux%u, interpolation area %s%u,%u %ux%u: %s in %.1f ms (%s)", want.OW,
             want.OH, want.RW, want.RH, want.AW ? "" : "(whole) ", want.AX, want.AY, want.AW ? want.AW : want.OW,
             want.AH ? want.AH : want.OH, ok ? "rebuilt" : "FAILED", static_cast<double>(Now() - t0) * 1000.0 / freq(), where);
    }

    // The output layer's UI mode for this frame, as Intel resolves it (captures in tools/XEFG_SETTINGS.md): 1 with UI
    // composition off; AUTO -> HUD-less + UI 3, HUD-less 4, UI 2, neither 1. 0 (nothing generated, as Intel does)
    // when the chosen mode's texture was not tagged. A pack without the UI inputs (tools/pack_ui.py) only has mode 1.
    uint32_t UiMode(bool hud, bool ui)
    {
        if (!uiComposition)
            return 1;
        uint32_t m = static_cast<uint32_t>(initParams.uiMode);
        if (m == XEFG_SWAPCHAIN_UI_MODE_AUTO)
            m = hud && ui ? 3 : hud ? 4 : ui ? 2 : 1;
        const bool needHud = m >= 3, needUi = m == 2 || m == 3 || m == 5;
        if (m > 5 || (needHud && !hud) || (needUi && !ui))
        {
            if (!warnedUi)
                XLog("present: UI mode %u needs %s, not tagged this frame - no generated frames (as Intel)", m,
                     m > 5 ? "a mode XeFG has" : needHud && !hud && needUi && !ui ? "HUD-less colour and a UI texture"
                                               : needHud && !hud                  ? "HUD-less colour" : "a UI texture");
            warnedUi = true;
            return 0;
        }
        if (m >= 2 && !pg->HasUiInputs())
        {
            if (!warnedUi)
                XLog("present: UI mode %u, but the pack lacks the UI inputs (tools/pack_ui.py) - interpolating everything", m);
            warnedUi = true;
            return 1;
        }
        return m;
    }

    // The frame's inputs on g: motion and depth from its staging set, the frame to interpolate into cur (the
    // HUD-less colour in modes 3-5, else the app's frame) and the output layer's back buffer (t10) and UI texture
    // (t9) for modes 2-5. Returns what to present as the application's own frame.
    ID3D12Resource* CopyFrame(ID3D12GraphicsCommandList* g, const Job& j, ID3D12Resource* bb)
    {
        const Resource& ox = pg->Output();
        ID3D12Resource* realSrc = pg->Buf(GenSlot::Cur).res;
        ConvertIn(g, j);
        Out(g, pg->Buf(GenSlot::Motion).res, D3D12_RESOURCE_STATE_COPY_DEST);
        Out(g, pg->Buf(GenSlot::Depth).res, D3D12_RESOURCE_STATE_COPY_DEST);
        g->CopyBufferRegion(pg->Buf(GenSlot::Motion).res, 0, stMv[j.set], 0, pg->Buf(GenSlot::Motion).bytes);
        g->CopyBufferRegion(pg->Buf(GenSlot::Depth).res, 0, stDepth[j.set], 0, pg->Buf(GenSlot::Depth).bytes);
        Back(g, pg->Buf(GenSlot::Depth).res, D3D12_RESOURCE_STATE_COPY_DEST);
        Back(g, pg->Buf(GenSlot::Motion).res, D3D12_RESOURCE_STATE_COPY_DEST);
        Barrier(g, bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_COPY_SOURCE);
        Out(g, pg->Buf(GenSlot::Cur).res, D3D12_RESOURCE_STATE_COPY_DEST);
        const bool bgra = IsBgra(scDesc.Format);
        if (j.hudless)
            g->CopyBufferRegion(pg->Buf(GenSlot::Cur).res, 0, stHud[j.set], 0, pg->Buf(GenSlot::Cur).bytes);
        else
            Copy(g, pg->Buf(GenSlot::Cur).res, bb, ox);
        Back(g, pg->Buf(GenSlot::Cur).res, D3D12_RESOURCE_STATE_COPY_DEST);
        if (j.uiMode >= 2)
        {
            Out(g, pg->Buf(GenSlot::BackBuffer).res, D3D12_RESOURCE_STATE_COPY_DEST);
            Copy(g, pg->Buf(GenSlot::BackBuffer).res, bb, ox);
            Back(g, pg->Buf(GenSlot::BackBuffer).res, D3D12_RESOURCE_STATE_COPY_DEST);
            if (j.uiMode != 4)
            {
                Out(g, pg->Buf(GenSlot::Ui).res, D3D12_RESOURCE_STATE_COPY_DEST);
                g->CopyBufferRegion(pg->Buf(GenSlot::Ui).res, 0, stUi[j.set], 0, pg->Buf(GenSlot::Ui).bytes);
                Back(g, pg->Buf(GenSlot::Ui).res, D3D12_RESOURCE_STATE_COPY_DEST);
            }
        }
        // The app's frame as it is, for its own present: cur unless cur took the HUD-less colour or is turned
        // round below (B8G8R8A8). Letterboxed, every generated frame takes its borders from it on the present queue
        // while the generation lane works on the pack's buffers: then a plain copy of its own (keep).
        const bool lb = Letterboxed();
        if (bgra || j.hudless || lb)
        {
            if (j.hudless && !bgra && !lb)
                realSrc = pg->Buf(GenSlot::BackBuffer).res;
            else
            {
                Copy(g, keepRes(), bb, ox);
                realSrc = keepRes();
            }
        }
        if (bgra)                   // the CUDA path turns them on its stream (Launch)
            pg->SwapInputsBgra(g, j.uiMode);
        Barrier(g, bb, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_PRESENT);
        pg->SetUiMode(j.uiMode);   // the CUDA path sets it on its own thread (Launch)
        return realSrc;
    }

    // Intel's scene change test (tools/XEFG_SETTINGS.md "Scene change"): not the images but the camera. A frame whose
    // view direction (the view matrix's third column) is turned from the previous frame's by more than the threshold
    // allows - cos(angle) < threshold - is not interpolated: Intel copies the current frame into its output instead of
    // running the network, so the frame is shown in the generated frames' places too (unlike resetHistory, which
    // presents it once). Translation and roll do not count;
    // 0 catches only turns past 90 degrees, 1 any turn. No view matrix (zeros): never.
    bool CameraCut()
    {
        const float* m = constants.viewMatrix;
        const float f[3] = { m[2], m[6], m[10] };
        const float ff = f[0] * f[0] + f[1] * f[1] + f[2] * f[2];
        bool cut = false;
        if (ff > 0 && prevForwardSet)
        {
            const float pp = prevForward[0] * prevForward[0] + prevForward[1] * prevForward[1] + prevForward[2] * prevForward[2];
            const float d = (f[0] * prevForward[0] + f[1] * prevForward[1] + f[2] * prevForward[2]) / std::sqrt(ff * pp);
            cut = d < std::clamp(sceneThreshold, 0.0f, 1.0f);
            if (cut && cameraCuts++ < 5)
                XLog("present: camera cut (view direction turned, cos %.4f < threshold %.2f) - frame %llu repeated, not interpolated", d,
                     sceneThreshold, static_cast<unsigned long long>(appFrame));
        }
        prevForwardSet = ff > 0;
        std::copy(f, f + 3, prevForward);
        return cut;
    }

    bool Letterboxed() const
    {
        if (!pg->HasPack())
            return false;
        const Sizes& z = pg->PackSize();
        return z.AW && z.AH && z.AX + static_cast<uint64_t>(z.AW) <= z.OW && z.AY + static_cast<uint64_t>(z.AH) <= z.OH;
    }

    // A generated frame (src, pack layout) into the swap chain buffer rb. Letterboxed, Intel's output outside
    // the interpolation area is the application's frame (it copies the four borders in): here the application's
    // frame (frame, the plain copy CopyFrame made) goes in first and the area of the generated one over it.
    void CopyGenerated(ID3D12GraphicsCommandList* g, ID3D12Resource* rb, ID3D12Resource* src, ID3D12Resource* frame, const Resource& ox)
    {
        if (!Letterboxed() || !frame || frame == src)
        {
            Copy(g, rb, src, ox, true);
            return;
        }
        Copy(g, rb, frame, ox, true);
        const Sizes& z = pg->PackSize();
        D3D12_TEXTURE_COPY_LOCATION d { rb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        d.SubresourceIndex = 0;
        s.PlacedFootprint.Footprint = { scDesc.Format, ox.w, ox.h, 1, static_cast<UINT>(ox.pitch) };
        const D3D12_BOX box { z.AX, z.AY, 0, z.AX + z.AW, z.AY + z.AH, 1 };
        g->CopyTextureRegion(&d, z.AX, z.AY, 0, &s, &box);
    }

    // A plain default-heap buffer for the app's frame when cur does not hold it as it is.
    ID3D12Resource* keepRes()
    {
        if (!keep)
            keep = PlainBuffer(pg->Buf(GenSlot::Cur).bytes);
        return keep;
    }

    ID3D12GraphicsCommandList* Begin()
    {
        const size_t i = next++ % kAllocs;
        if (hostFence->GetCompletedValue() < allocFence[i])
        {
            const long long w0 = Now();
            hostFence->SetEventOnCompletion(allocFence[i], hostEvent);
            WaitForSingleObject(hostEvent, 10000);
            allocWaitMs += (Now() - w0) * 1000.0 / freq();
        }
        ReadStamps(i);
        alloc[i]->Reset();
        list->Reset(alloc[i], nullptr);
        cur_i = i;
        return list;
    }
    void End()
    {
        Submit(queue, list, "present list");
        queue->Signal(hostFence, ++hostValue);
        allocFence[cur_i] = hostValue;
    }
    static void Barrier(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        GenBarrier(g, r, a, b);
    }

    // XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES: a magenta quad in each corner of a generated frame.
    void DrawTags(ID3D12GraphicsCommandList* g, ID3D12Resource* bb, const Resource& px)
    {
        constexpr UINT kQ = 64, kPitch = 256;
        if (!tagBuf)
        {
            D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_UPLOAD };
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = static_cast<UINT64>(kPitch) * kQ;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_GENERIC_READ, nullptr, IID_PPV_ARGS(&tagBuf))))
                return;
            void* p = nullptr;
            if (SUCCEEDED(tagBuf->Map(0, nullptr, &p)))
            {
                for (UINT y = 0; y < kQ; ++y)
                    for (UINT x = 0; x < kQ; ++x)
                        reinterpret_cast<uint32_t*>(static_cast<uint8_t*>(p) + y * kPitch)[x] = 0xFFFF00FFu;     // R=FF G=00 B=FF A=FF
                tagBuf->Unmap(0, nullptr);
            }
        }
        D3D12_TEXTURE_COPY_LOCATION d { bb, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { tagBuf, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        s.PlacedFootprint.Footprint = { scDesc.Format, kQ, kQ, 1, kPitch };      // magenta in 8-bit RGBA and BGRA
        const UINT xs[2] = { 0, px.w - kQ }, ys[2] = { 0, px.h - kQ };
        for (UINT yi : ys)
            for (UINT xi : xs)
                g->CopyTextureRegion(&d, xi, yi, 0, &s, nullptr);
    }

    // Texture <-> pitched buffer of the pack's layout (output size, the back buffer's format: B8G8R8A8 stays in
    // that byte order here, SwapRB turns it round for the pack).
    void Copy(ID3D12GraphicsCommandList* g, ID3D12Resource* dst, ID3D12Resource* src, const Resource& px, bool toTexture = false)
    {
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        fp.Footprint = { scDesc.Format, px.w, px.h, 1, static_cast<UINT>(px.pitch) };
        if (!toTexture)
        {
            D3D12_TEXTURE_COPY_LOCATION d { dst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT }, s { src, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX };
            d.PlacedFootprint = fp;
            s.SubresourceIndex = 0;
            g->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        }
        else
        {
            D3D12_TEXTURE_COPY_LOCATION d { dst, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, s { src, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
            d.SubresourceIndex = 0;
            s.PlacedFootprint = fp;
            g->CopyTextureRegion(&d, 0, 0, 0, &s, nullptr);
        }
    }

    bool CopyIn(ID3D12GraphicsCommandList* g, const xefg_swapchain_d3d12_resource_data_t& rd, ID3D12Resource* dst, const Resource& pr, const char* what,
                uint32_t set)
    {
        const D3D12_RESOURCE_DESC d = rd.pResource->GetDesc();
        // Motion and depth: what to do with the tagged format (xefg_formats.h): copied as it is (the pack's own layout), or copied raw into
        // stRaw and converted on our list (ConvertIn), or refused (no frames are generated while it is tagged).
        const bool isMv = rd.type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR, isDepth = rd.type == XEFG_SWAPCHAIN_RES_DEPTH;
        uint32_t kind = 0, stride = 0;
        const Take take = isMv ? MotionTake(d.Format, kind, stride) : isDepth ? DepthTake(d.Format, kind, stride) : Take::Raw;
        bool convert = (isMv || isDepth) && take == Take::Convert && pr.texel == 4 && !convFailed && !rawFailed;
        // HUD-less colour and UI texture: their region is the interpolation area (letterboxing), but the layers read
        // them at the area's offset in the whole texture, so an output-sized one is copied whole.
        const bool colour = rd.type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR || rd.type == XEFG_SWAPCHAIN_RES_UI;
        const bool full = colour && d.Width == pr.w && d.Height == pr.h;
        const uint32_t sw = full ? pr.w : rd.resourceSize.x ? rd.resourceSize.x : static_cast<uint32_t>(d.Width);
        const uint32_t sh = full ? pr.h : rd.resourceSize.y ? rd.resourceSize.y : d.Height;
        const bool whole = full || (rd.resourceBase.x == 0 && rd.resourceBase.y == 0 && sw == d.Width && sh == d.Height);
        // D3D12 copies a depth-stencil texture only whole: a 32-bit float depth region in a larger one goes the convert
        // path too, which copies the whole texture and reads the region in the shader (Kind 5, the bits as they are).
        if (!convert && isDepth && !whole && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && pr.texel == 4 &&
            take == Take::Raw && !convFailed && !rawFailed)
        {
            convert = true;
            kind = 5;
            stride = 4;
        }
        const uint32_t tb = isMv || isDepth ? (convert ? pr.texel : take == Take::Raw ? 4u : 0u) : TexelBytesOf(d.Format);
        // Depth: the pack's buffer is output-sized (a "pack 2") and takes the render-size region at its top left.
        const bool fits = rd.type == XEFG_SWAPCHAIN_RES_DEPTH ? sw <= pr.w && sh <= pr.h : sw == pr.w && sh == pr.h;
        // The region inside the tagged texture (the copy box below).
        const bool inTex = full || (rd.resourceBase.x + static_cast<uint64_t>(sw) <= d.Width && rd.resourceBase.y + static_cast<uint64_t>(sh) <= d.Height);
        // Same texel size is not enough: the HUD-less colour is the swap chain's own format family (the R <-> B swap
        // follows the swap chain), an 8-bit UI texture has its byte order. Motion vectors and depth have their format decided above.
        const int fam = ColourFamily(d.Format), scFam = ColourFamily(scDesc.Format);
        const bool family = rd.type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR ? fam == scFam
                            : rd.type == XEFG_SWAPCHAIN_RES_UI           ? fam != 0 && (scFam == 3 || fam == scFam)
                                                                         : true;
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count > 1 || !tb || tb != pr.texel || !fits || !inTex || !family)
        {
            // Logged once for each format (and kind of reason) a type is refused with, again after a tag of it was taken in between.
            const bool badFormat = (isMv || isDepth) && take == Take::No && d.Dimension == D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            // a format we convert, but the conversion cannot run (its pipeline or its raw buffer failed): not a size problem either
            const bool noConversion = (isMv || isDepth) && take == Take::Convert && !convert;
            const uint32_t key = 1u + static_cast<uint32_t>(d.Format) + (badFormat ? 0x10000u : noConversion ? 0x20000u : 0u);
            if (warnedTagKey[rd.type] != key)
            {
                const char* nm = FormatName(d.Format);
                if (badFormat)
                    XLog("tag %s: %s in DXGI format %d%s%s%s %s not supported - no frames generated while %s tagged", what,
                         isMv ? "motion vectors" : "depth", static_cast<int>(d.Format), *nm ? " (" : "", nm, *nm ? ")" : "",
                         isMv ? "are" : "is", isMv ? "they are" : "it is");
                else if (noConversion)
                    XLog("tag %s: fmt=%d %s needs the format conversion, which is not available (its pipeline or buffer could not be made) - no frames generated while it is tagged",
                         what, static_cast<int>(d.Format), nm);
                else
                    XLog("tag %s: %llux%u fmt=%d region %ux%u does not match the pack's %ux%u texel %u - ignored%s", what, d.Width,
                         d.Height, static_cast<int>(d.Format), sw, sh, pr.w, pr.h, pr.texel,
                         isMv || isDepth ? ", no frames generated while it is tagged" : "");
            }
            warnedTagKey[rd.type] = key;
            goodRun[rd.type] = 0;
            return false;
        }
        // The logged refusal is forgotten only after the type was taken in kGoodFramesToRecover different frames in a row, so tags that
        // alternate between a good and a refused format log once, not every other frame.
        if (warnedTagKey[rd.type] && goodRunFrame[rd.type] != appFrame)
        {
            goodRunFrame[rd.type] = appFrame;
            if (++goodRun[rd.type] >= xefgown::kGoodFramesToRecover)
                warnedTagKey[rd.type] = 0;
        }
        // The region may sit in a larger texture (depth); it lands at the pack's own row pitch.
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &fp, nullptr, nullptr, &total);
        fp.Offset = 0;
        fp.Footprint.Width = sw;
        fp.Footprint.Height = sh;
        fp.Footprint.RowPitch = static_cast<UINT>(pr.pitch);
        const int ci = isMv ? 0 : 1;
        if (convert)
        {
            // The whole texture at its own texel size (the footprint keeps the copyable format of plane 0, the depth
            // plane), so a depth-stencil one is copied as D3D12 allows; the shader reads the region at its offset.
            const UINT tw = static_cast<UINT>(d.Width);
            fp.Footprint.Width = tw;
            fp.Footprint.Height = d.Height;
            fp.Footprint.RowPitch = (tw * stride + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            const size_t need = static_cast<size_t>(fp.Footprint.RowPitch) * d.Height;
            if (stRawBytes[ci][set] < need)
            {
                if (stRaw[ci][set])
                    stRawOld.push_back(stRaw[ci][set]); // may still be read by a list in flight: released with the staging
                stRaw[ci][set] = PlainBuffer(need);
                stRawBytes[ci][set] = stRaw[ci][set] ? need : 0;
                if (!stRaw[ci][set])
                {
                    XLog("tag %s: no %zu-byte buffer for its raw copy - motion / depth in other formats ignored from now on", what, need);
                    rawFailed = true;
                }
            }
            if (!stRaw[ci][set] || !EnsureConvert())
                return false;
            dst = stRaw[ci][set];
            if (!warnedConv[ci])
                XLog("tag %s: fmt=%d %ux%u converted to the pack's %s on our list", what, static_cast<int>(d.Format), sw, sh,
                     isMv ? "R16G16_FLOAT" : "32-bit float depth");
            warnedConv[ci] = true;
            pendConv[ci] = { static_cast<int32_t>(kind), sw, sh, fp.Footprint.RowPitch, static_cast<uint32_t>(pr.pitch), stride,
                             rd.resourceBase.x, rd.resourceBase.y, dst };
        }
        else if (isMv || isDepth)
            pendConv[ci] = {};
        D3D12_TEXTURE_COPY_LOCATION s { rd.pResource, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, t { dst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        s.SubresourceIndex = 0;
        t.PlacedFootprint = fp;
        const D3D12_BOX box { rd.resourceBase.x, rd.resourceBase.y, 0, rd.resourceBase.x + sw, rd.resourceBase.y + sh, 1 };
        Barrier(g, rd.pResource, rd.incomingState, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g->CopyTextureRegion(&t, 0, 0, 0, &s, whole || convert ? nullptr : &box);
        Barrier(g, rd.pResource, D3D12_RESOURCE_STATE_COPY_SOURCE, rd.incomingState);
        return true;
    }

    // The conversion pipeline (xefg_convert.hlsl, root signature in its bytecode), made the first time a tag needs it.
    // On failure the tags it would have taken are ignored as before.
    bool EnsureConvert()
    {
        if (convPso)
            return true;
        if (convFailed)
            return false;
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        if (SUCCEEDED(dev->CreateRootSignature(0, g_xefgConvertCS, sizeof(g_xefgConvertCS), IID_PPV_ARGS(&convRs))))
        {
            pd.pRootSignature = convRs;
            pd.CS = { g_xefgConvertCS, sizeof(g_xefgConvertCS) };
            if (SUCCEEDED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&convPso))))
                return true;
        }
        XLog("tag: the format conversion pipeline could not be made - motion / depth in other formats ignored");
        convFailed = true;
        return false;
    }

    // The frame's motion / depth tags that came in another format (CopyIn): from their raw copy into the staging set
    // CopyFrame copies from, in the pack's layout. Buffers decay to COMMON after each list, so every list starts there.
    void ConvertIn(ID3D12GraphicsCommandList* g, const Job& j)
    {
        for (int ci = 0; ci < 2; ++ci)
        {
            const Conversion& c = j.conv[ci];
            ID3D12Resource* src = c.src;
            ID3D12Resource* dst = ci == 0 ? stMv[j.set] : stDepth[j.set];
            if (c.kind < 0 || !src || !dst || !convPso)
                continue;
            const uint32_t k[8] = { c.w, c.h, c.srcPitch, c.dstPitch, static_cast<uint32_t>(c.kind), c.stride, c.srcX, c.srcY };
            Barrier(g, src, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            Barrier(g, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            g->SetComputeRootSignature(convRs);
            g->SetPipelineState(convPso);
            g->SetComputeRoot32BitConstants(0, 8, k, 0);
            g->SetComputeRootShaderResourceView(1, src->GetGPUVirtualAddress());
            g->SetComputeRootUnorderedAccessView(2, dst->GetGPUVirtualAddress());
            g->Dispatch((c.w + 7) / 8, (c.h + 7) / 8, 1);
            Barrier(g, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE); // CopyFrame's copy next
        }
    }

    // D3D12 path: the generator's buffers rest in UNORDERED_ACCESS (xefg_pack_d3d.h), so a copy takes one out of it
    // and back. The CUDA path's shared buffers rest in COMMON and are promoted implicitly: nothing to do.
    void Out(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES s) { pg->Out(g, r, s); }
    void Back(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES s) { pg->Back(g, r, s); }

    // D3D12 path: one generated frame recorded on g by the generator (generators/own.h), with this pass's constants. GPU time
    // by timestamps (Prof) in stamp slot `slot`: the present lane's allocator index, or kAllocs + the generation lane's;
    // the queue g runs on says the timestamp frequency.
    ID3D12CommandQueue* TsQueue(size_t slot) const { return slot >= kAllocs && genQueue ? genQueue : queue; }
    bool RecordPass(ID3D12GraphicsCommandList* g, uint32_t k, size_t slot)
    {
        return pg->RecordPass(g, k, slot, TsQueue(slot), IsBgra(scDesc.Format));
    }
    // A pass on the generation lane whose frame ends up in `dst` (a UAV-capable plain buffer, COMMON between lists). With a dump
    // pending the output stays in the pack's buffer (DumpOnce reads it) and is copied.
    void RecordPassInto(ID3D12GraphicsCommandList* g, uint32_t k, ID3D12Resource* dst)
    {
        const size_t slot = kAllocs + genCur;
        pg->RecordPassInto(g, k, dst, slot, TsQueue(slot), IsBgra(scDesc.Format), !dumpPath.empty());
    }
    // The GPU time of a pass recorded with allocator i, once its list has finished (Begin waited for it).
    void ReadStamps(size_t i) { pg->ReadStamps(i); }

    uint32_t genCount = 0;
    uint32_t lastNGen = ~0u;
    std::string packDir;
    IPackGenerator* pg = nullptr;       // generator->AsPack(): the pack surface the host drives (own is the only one that runs)
    std::unique_ptr<IGenerator> generator;  // the frame generator (generators/own.h unless another is chosen and available)
    ID3D12Resource* hold = nullptr;         // an app frame's own picture once the next one rewrites cur / keep (HandOff)
    ID3D12Resource* keep = nullptr;
    ID3D12Resource* stMv[2] {};             // staging sets the application's tags land in, by frame parity
    ID3D12Resource* stDepth[2] {};
    ID3D12Resource* stHud[2] {};
    ID3D12Resource* stUi[2] {};
    ID3D12Resource* stRaw[2][2] {};         // motion, depth tags in a format ConvertIn turns into the pack's, by frame parity
    size_t stRawBytes[2][2] {};
    std::vector<ID3D12Resource*> stRawOld;  // outgrown raw buffers, released with the staging (a list may still read them)
    bool warnedConv[2] {};
    Conversion pendConv[2];                 // app thread: this frame's motion / depth conversion, handed to its Job at Present
    ID3D12RootSignature* convRs = nullptr;  // xefg_convert.hlsl
    ID3D12PipelineState* convPso = nullptr;
    bool convFailed = false;
    bool rawFailed = false;                 // a raw-copy buffer could not be made: conversions stay off (logged once)

    // Queues: ours presents (and runs the pack); the application's renders and tags.
    ID3D12CommandQueue* queue = nullptr;
    ID3D12CommandQueue* appQueue = nullptr;
    ID3D12CommandAllocator* alloc[kAllocs] {};
    UINT64 allocFence[kAllocs] {};
    ID3D12GraphicsCommandList* list = nullptr;
    // Tags valid until present (FlushTags): the frame's latest per type (a reference held), and our list for them on
    // the application's queue, its allocators a small pool (TagSlot).
    xefg_swapchain_d3d12_resource_data_t deferredTag[XEFG_SWAPCHAIN_RES_COUNT] {};
    std::vector<TagSlot> tagSlots;
    ID3D12GraphicsCommandList* tagList = nullptr;
    HANDLE tagEvent = nullptr;
    bool tagListFailed = false, warnedTagWait = false;
    std::atomic<bool> removedSeen { false };    // Removed(): logged once
    std::atomic<int> closeFails { 0 };
    ID3D12Fence* hostFence = nullptr;
    ID3D12Fence* appFence = nullptr;        // application frame f ready on its queue: f + 1
    ID3D12Fence* consumedFence = nullptr;   // presenter took frame f's inputs: f + 1
    ID3D12Fence* retireFence = nullptr;     // frame f and its generated frames done on the GPU: f + 1
    HANDLE retireEvent = nullptr;
    HANDLE gpuEvent = nullptr;              // presenter: GPU waits (WaitGpu)
    std::atomic<bool> latencyAsked { false };
    ID3D12Fence* idleFence = nullptr;       // WaitApp
    HANDLE hostEvent = nullptr;
    UINT64 hostValue = 0, idleValue = 0, fv = 0;
    size_t next = 0, cur_i = 0;

    // The generation lane (CreateGenLane, PresentGen): its queue, lists, fence and the generated frames' ring.
    ID3D12CommandQueue* genQueue = nullptr;
    ID3D12CommandAllocator* genAlloc[kAllocs] {};
    UINT64 genAllocFence[kAllocs] {};
    ID3D12GraphicsCommandList* genList = nullptr;
    ID3D12Fence* genFence = nullptr;
    UINT64 genValue = 0;
    size_t genNext = 0, genCur = 0;
    HANDLE genEvent = nullptr;
    ID3D12Resource* ring[kMaxInterp] {};
    ID3D12Resource* ringG[2][kMaxInterp] {};    // Gate=1 on the D3D12 path: generated frames by parity (PresentGenGated)

    // The swap chain: the real one, our backbuffers, the frame-latency wait.
    IDXGISwapChain4* real = nullptr;
    DXGI_SWAP_CHAIN_DESC1 scDesc {};
    std::vector<ID3D12Resource*> appBufs;
    UINT appIndex = 0;
    uint64_t appFrame = 0;
    HANDLE latencySem = nullptr, realWait = nullptr;
    UINT latency = 1;
    std::atomic<HRESULT> lastHr { S_OK };

    // The presenter thread and its queue of application frames.
    std::thread presenter;
    std::mutex pm;
    std::condition_variable pcv;
    std::deque<Job> pjobs;
    bool pBusy = false, pQuit = false;
    Frame early;                            // presenter: the next app frame, taken while this one waited (Early)

    // Set by the presenter thread after a rebuild (RebuildForSizes), read under mu by the application's threads.
    std::atomic<bool> built { false };
    bool failed = false, haveMv = false, haveDepth = false, hudless = false, haveUi = false, warnedSize = false,
         warnedPresent = false, warnedUi = false;
    bool inFullscreen = false;      // the real swap chain is in exclusive fullscreen: no generation
    bool hasPrev = false;                   // presenter: prev holds the previous app frame
    bool lastUsable = false;                // app thread: the previous frame could take part in generation
    uint32_t goodRun[8] {};
    uint64_t goodRunFrame[8] {};
    uint32_t warnedTagKey[8] {};        // per tag type: the refusal last logged (format + kind of reason), 0 after a tag was taken
    bool dumped = false;
    const Frame* showing = nullptr;         // presenter: the frame Show is presenting (Hitch)
    unsigned hitches = 0;                   // hitch lines in the second from hitchSecond (PresentReal)
    long long hitchSecond = 0;
    ID3D12Resource* tagBuf = nullptr;
    std::string dumpPath, shownPath;
    long long dumpFrame = -1;
    float prevJitter[2] {};
    uint32_t renderSize[2] {};          // the game's render size, from its motion-vector / depth regions
    uint32_t area[4] {};                // the interpolation area from the back buffer tag (x, y, w, h); 0: whole
    float prevForward[3] {};            // CameraCut: the previous frame's view direction
    bool prevForwardSet = false;
    unsigned cameraCuts = 0;                // camera cuts logged so far (the first few)
    bool warnedArea = false;
    unsigned tagsThisFrame = 0;
    Sizes failedSize;
    bool resizeFailed = false;
    // FollowSizes (under mu) -> RebuildForSizes (presenter thread, never under mu: ResizeBuffers drains the presenter
    // holding mu). resizeWant is written before resizePending is set and not again until the rebuild cleared it.
    Sizes resizeWant;
    std::atomic<UINT64> resizeFrame { 0 };
    std::atomic<bool> resizePending { false };
    bool rebuildOnInit = false;     // under mu: a resize the stopped presenter did not get to
    bool deferStarted = false;      // under mu: StartDeferredBuild ran
    std::atomic<bool> deferBusy { false };      // the deferred first build is pending or running (the presenter leaves the generator alone)
    std::atomic<bool> deferQuit { false };
    HANDLE deferThread = nullptr;
    std::atomic<bool> deferFinished { false };
    std::atomic<bool> deferReady { false };     // the deferred build ended well and the context was not destroyed

    // Pacing and stats. interval is written by the application's thread, read by the presenter.
    int slowCalls = 0;
    long long lastReturn = 0, lastReal = 0, lastAnyPresent = 0, lastRelease = 0, jobBase = 0, schedBase = 0;
    std::atomic<double> interval { 16.7 };
    double statGap = 0, statGap2 = 0, statGapMax = 0, statAppCall = 0, statAppWait = 0;
    unsigned statGaps = 0, statFrames = 0, statLate = 0, statEarly = 0, traced = 0;
    bool catchUp = false;               // presenter: the next app frame was already done on the GPU when this one started
    double allocWaitMs = 0;
    // Presenter, GPU timeline: when the app's frame was done on the GPU (QPC), and the smoothed interval between those.
    long long lastReady = 0, pickT = 0;  // pickT: when the presenter took the current app frame (trace)
    pacesched::ReadyInterval ri;            // the smoothed interval between those (ri.ms), its history for the hitch log (pace_sched.h)
    double paceLag = 0;                 // presenter: ms the schedule runs behind the GPU timeline (PaceTarget)
    // The clock pacer (ClockTarget), presenter thread. clkBatch / clkVar: the app frame's time on the GPU timeline and its
    // variance, ms; clkTarget: the delay behind the app frame's ready stamp the clock aims for, ms (clkNeed: the last 64
    // delays the generated frames needed); clkCorr: the spacing's correction towards it (fraction, +-kClkMaxCorr).
    double clkBatch = 0, clkVar = 0, clkTarget = 0, clkCorr = 0, clkDelay = 0;
    unsigned clkHitchRun = 0;
    long long clkLast = 0;              // QPC just before the last release's Present (0: set the clock anew)
    std::array<double, 64> clkNeed {};
    unsigned clkNeedN = 0, clkNeedAt = 0;
    double statClkDelay = 0, statClkNeed = 0;
    unsigned statClkN = 0, statClkSlips = 0, statClkHitches = 0;
    // Gate=1: the clock above is shared with the release thread (clkMu: clkBatch, clkVar, ri.ms, clkCorr, clkTarget,
    // clkLast and the clock's stats).
    std::mutex clkMu;
    // PaceLead (Frame::lead; pace_lead.h): the rule's state (presenter thread), and the lead's range, the app frames that used it
    // and the generated frames late per stats window.
    pacelead::Lead leadState;
    double leadMin = 1e9, leadMax = 0;
    unsigned statLeadLate = 0, statLeadFrames = 0;
    std::mutex paceMu;
    std::vector<PaceRec> paceRecs;
    size_t paceDropped = 0;
    // Flip metering (Meter, presenter thread); meteredFrame (app frame + 1 whose batch the driver meters) is read by the
    // release thread too.
    SetFlipConfigFn flipFn = nullptr;
    bool flipTried = false, flipRefused = false, flipMeteredNow = false;
    uint32_t flipOutputs = 0, flipSamples = 0, flipFails = 0, flipFeedback = 0, flipLogs = 0;
    int flipStatus = 0;
    double flipBatchMs = 0;
    unsigned flipCalls = 0, statFlipMetered = 0;
    std::atomic<UINT64> meteredFrame { 0 };
    // Gate=1 (ShowGated): the present lane, its lists and fence; the gate's fence, the presents waiting for their time.
    ID3D12CommandQueue* pq = nullptr;
    bool oobTried = false;                  // the presenting thread's, as flipTried
    bool oobMarked = false;
    int oobMarkerFailed = 0;
    AsyncMarkerFn asyncMarkerFn = nullptr;
    NotifyOobFn notifyOobFn = nullptr;      // OobQueue=switch
    ReflexSyncFn reflexSyncFn = nullptr;    // DriverFg=1, set before driverFgReady
    ID3D12CommandQueue* driverFgQueue = nullptr;
    std::atomic<bool> driverFgReady { false };
    bool driverFgCounted = false;           // raised g_driverSignals (lowered in ~Own)
    std::vector<double> fsSteps;            // FlipStats: spacings of the last window, ms
    DXGI_FRAME_STATISTICS fsLast {};
    bool fsHave = false;
    unsigned fsLarger = 0, fsFailed = 0;
    bool driverFgTried = false;             // the application's Present thread
    uint32_t driverFgFrames = 1;            // under mu, read after it on the same thread
    int driverFgSyncStatus = 0;
    int oobSwitchType = -1;
    int oobSwitchFailed = 0;
    ID3D12GraphicsCommandList* pqList = nullptr;
    ID3D12CommandAllocator* pqAlloc[kAllocs] {};
    UINT64 pqAllocFence[kAllocs] {};
    ID3D12Fence* pqFence = nullptr;
    UINT64 pqValue = 0;
    UINT64 pqRead[2] {};                    // the lane's last read of gateHeld[s] / ringC[s] at pqFence pqRead[s]
    size_t pqNext = 0, pqCur = 0;
    HANDLE pqEvent = nullptr;
    ID3D12QueryHeap* laneCopyHeap = nullptr;
    ID3D12Resource* laneCopyReadback = nullptr;
    UINT64 laneCopyFreq = 1;
    bool laneCopyActive = false, laneCopyStampedPending = false;
    bool laneCopyStamped[kAllocs] {}, laneCopyGenerated[kAllocs] {};
    uint64_t laneCopyFramePending = 0, laneCopyFrame[kAllocs] {};
    uint32_t laneCopyKPending = 0, laneCopyK[kAllocs] {};
    uint32_t laneCopyCount = 0, laneCopyGeneratedCount = 0, laneCopyAppCount = 0;
    uint32_t laneCopyGeneratedPassCount[kMaxInterp] {};
    uint64_t laneCopyFirstFrame = 0, laneCopyLastFrame = 0;
    double laneCopyGeneratedMs = 0, laneCopyAppMs = 0;
    double laneCopyGeneratedPassMs[kMaxInterp] {};
    bool laneCopyGeneratedPending = false;
    ID3D12Resource* gateHeld[2] {};             // each app frame's own picture, by parity, for the lane (Submit)
    ID3D12Fence* gateFence = nullptr;
    std::atomic<UINT64> gateValue { 0 };    // presents put behind the gate (presenter)
    UINT64 gateSig = 0;                     // released so far (gsm)
    std::mutex gsm;
    struct GateItem
    {
        UINT64 v = 0;
        uint32_t k = 0;
        UINT64 content = 0;
        UINT64 copied = 0;                  // pqFence value of its copy into the swap chain's buffer
        Frame f;
    };
    std::deque<GateItem> gq;
    std::mutex gm;
    std::condition_variable gcv;
    std::atomic<bool> gQuit { false };
    std::thread gater;
    std::thread relay;
    ID3D12Fence* contentFence = nullptr;    // io.fence's progress as the relay saw it (StartGate)
    UINT64 gateFrameEnd = 0;                // the gate value of the last present of the frame being shown (ShowGated)
    std::atomic<UINT64> gWaitingFor { 0 };  // diagnostics: the content value the release thread waits for
    int slowCopyLogs = 0;                   // diagnostics: release-thread copy waits over 250 ms logged so far (30 at most)
    int slowPresentLogs = 0, slowIdleLogs = 0, slowGenLogs = 0;     // the same for the gate's other waits
    double statRel = 0, statRel2 = 0, statRelMax = 0, statQueued = 0, statLaneBlock = 0;
    unsigned statRels = 0;
    // When each fence value the CUDA stream signals (or the generation lane's fence reaches, D3D12 path) was reached (StampDone,
    // StartGate; ClockNeed).
    std::array<std::atomic<long long>, kStamps> doneAt {};
    std::array<std::atomic<UINT64>, kStamps> doneVal {};
    std::atomic<bool> stampsWork { false };
    double statGpuWait = 0, statStampAge = 0, statBehind = 0, statFirstLate = 0, statArriveToPick = 0, statArriveToReady = 0;
    unsigned statJobs = 0, statFirstN = 0;
    std::atomic<uint64_t> submitted { 0 };  // app frames handed over so far (appFrame, readable by the presenter)
    static constexpr size_t kReady = 64;
    std::thread watcher;
    std::atomic<bool> wQuit { false };
    std::atomic<uint64_t> readyCount { 0 };  // frames [0, readyCount) have their time in readyAt
    long long readyAt[kReady] {};
};

inline Proxy::~Proxy()
{
    {
        std::lock_guard<std::mutex> l(own->proxyMu);
        IDXGISwapChain4* self = this;
        own->proxy.compare_exchange_strong(self, nullptr);
    }
    own->Detach();
    LetGo();
}

// xefgSwapChainDestroy. A game that makes a new swap chain for its window before it releases the old one gets the
// context destroyed first (OptiScaler's "Releasing old swapchain"), and a window takes one flip-model swap chain at a
// time: the real one is let go of here, not when the app releases ours, or the new one cannot be made
// (CreateSwapChainForHwnd fails for as long as the game holds the old one).
inline void Own::Retire()
{
    {
        // Anything waiting on our frame-latency handle wakes (the presenter that released it stops below).
        std::lock_guard<std::mutex> lk(mu);
        if (latencySem)
            while (ReleaseSemaphore(latencySem, 1, nullptr))
            {
            }
    }
    Detach();
    IDXGISwapChain4* p = nullptr;
    {
        std::lock_guard<std::mutex> l(proxyMu);
        p = proxy.load();
        if (p)
            static_cast<Proxy*>(p)->LetGo();
    }
    // The proxy's first reference (Wrap) is the context's: dropped here, outside proxyMu (the last Release runs
    // ~Proxy, which takes it). Held until now, nothing else can have freed it. With the app's references gone too,
    // the proxy and this context go with it.
    if (p && p->Release() > 0)
        XLog("Destroy: the application still holds its swap chain - the real one is released now, its calls fail from here");
}
inline HRESULT Proxy::Present(UINT sync, UINT flags) { return own->OnPresent(sync, flags); }
inline HRESULT Proxy::Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS*) { return own->OnPresent(sync, flags); }
inline HRESULT Proxy::GetBuffer(UINT b, REFIID r, void** p) { return own->GetAppBuffer(b, r, p); }
inline UINT Proxy::GetCurrentBackBufferIndex() { return own->AppIndex(); }
inline HRESULT Proxy::ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl) { return own->Resize(c, w, h, f, fl); }
inline HRESULT Proxy::SetMaximumFrameLatency(UINT m) { return own->SetLatency(m); }
inline HRESULT Proxy::GetMaximumFrameLatency(UINT* m) { return own->GetLatency(m); }
inline HANDLE Proxy::GetFrameLatencyWaitableObject() { return own->LatencyHandle(); }
}  // namespace xefgown
