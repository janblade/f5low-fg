// generator.h - what the host (xefg_own.h, class Own) asks of a frame generator. docs/GENERATORS.md has the design.
//
// The host owns everything every generator shares: the swap chain the game sees, staging of the tagged inputs, the
// presenter thread, pacing, the gate, XeLL reporting, presentation. A generator makes the in-between frames. Today one
// runs (generators/own.h, "XeFG Tensor"); generators/stubs.h registers intel, dlssg and fsr as "not available yet".
//
// Two classes. IGenerator is the narrow contract every backend implements: what it is (Caps), whether the host can use
// it, build / rebuild / stop. IPackGenerator is the own generator's surface and only its: the host copies the game's frame,
// motion and depth into the pack's buffers (Buf) and drives passes, recorded into a command list of the host's (the D3D12
// path) or started on a CUDA stream that signals a fence (the CUDA path: Launch, DoneFence, RingFrame). The host reaches it
// through IGenerator::AsPack(). A frames-only (fsr) or present-sink (intel, dlssg) backend needs host work first
// (docs/GENERATORS.md). Virtual calls are per app frame or per pass; nothing inside the pack runners is virtual.
#pragma once

#include "xefg_pack.h"

#include <d3d12.h>
#include <dxgi.h>

#include <cstdint>
#include <functional>
#include <string>
#include <vector>

namespace xefgown
{

// The most generated frames per app frame (6x). Only t differs between them.
constexpr uint32_t kMaxInterp = 5;
// Allocators per lane; timestamp slots (the present lane's, then the generation lane's).
constexpr size_t kAllocs = 16;
constexpr size_t kSlots = 2 * kAllocs;
// CUDA path: when each fence value the stream signals was reached (ClockNeed) is kept for this many values.
constexpr size_t kStamps = 64;

inline void GenBarrier(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    if (a == b)
        return;
    D3D12_RESOURCE_BARRIER br {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g->ResourceBarrier(1, &br);
}

struct GenCaps
{
    const char* name = "";          // log and overlay
    bool available = false;         // false: the host runs own instead (reason says why)
    const char* reason = "";
    uint32_t maxGenerated = 0;      // highest n this backend can do on this GPU
    bool presentsItself = false;    // true = present-sink shape
    bool anyGpu = false;            // false = NVIDIA only
};

// Settings the host reads from the ini and the generator reads (and may switch off when it cannot do them): shared
// by reference, so both see a change.
struct GenSettings
{
    bool prof = false;              // Prof=1 (or XEFG_CUDA_PROF=1): per-run GPU timing
    bool profDispatches = false;    // Prof=2: per-dispatch sums too (D3D12 path)
    bool native = true;             // Native=0: the pack's native modules run as PTX (D3D12 path)
};

// The buffers a generator takes its inputs in (and keeps its output in): the host copies into them.
enum class GenSlot
{
    Cur,            // the frame to interpolate (the app's frame, or the HUD-less colour)
    Prev,           // the frame before it
    Motion,
    Depth,
    DepthPrev,      // optional (the multi-frame model)
    Output,         // the last generated frame (D3D12 path: where it lands before the copy out)
    Ui,             // the output layer's UI texture, optional
    BackBuffer,     // the output layer's back buffer, optional
};
struct GenBuf
{
    ID3D12Resource* res = nullptr;
    size_t bytes = 0;
};

// What the host gives a generator to call back: CUDA path only (the stream's host functions).
struct GenHost
{
    void* user = nullptr;
    void (*stampDone)(void* user, UINT64 fenceValue) = nullptr;     // the stream reached this fence value
    void (*stampsWork)(void* user) = nullptr;                       // a host function could be queued: done times are real
    UINT64 (*waitingFor)(void* user) = nullptr;                     // diagnostics: the value the release thread waits for
};

// One build or rebuild: the host's part of it runs at the points the generator names, as it did when both were one.
struct GenBuild
{
    uint32_t initFlags = 0;                         // XEFG_SWAPCHAIN_INIT_FLAG_*: pick the pack's module variants
    bool hdr = false;                               // the swap chain is R10G10B10A2 (HDR10)
    std::function<void()> releaseHostBuffers;       // the old pack is gone: the host lets go of what was sized by it
    std::function<bool()> cancelled;                // true: the context is going away - stop at the next step (a build thread)
    std::function<bool()> packBuilt;                // the new pack is built: the host makes its staging; false fails the build
};

class IPackGenerator;

// The contract every backend implements.
class IGenerator
{
public:
    virtual ~IGenerator() = default;
    virtual GenCaps Caps() const = 0;
    // The host's callbacks (CUDA host functions, diagnostics); set once, before the first Build.
    virtual void SetHost(const GenHost& h) { (void)h; }
    // Present-sink backends: handed the host's sink factory instead of the real one; frames come back through the sink.
    virtual bool AttachSink(IDXGIFactory* sinkFactory) { (void)sinkFactory; return false; }
    // The pack surface (below) if this generator has one; null otherwise. Only the own generator has it today.
    virtual IPackGenerator* AsPack() { return nullptr; }

    virtual bool Build(const GenBuild& b) = 0;                                  // the first build
    virtual bool Rebuild(const xefgpack::Sizes& want, const GenBuild& b) = 0;   // other sizes; nothing may be using the old one
    virtual void Drain() = 0;                                                   // all submission posted so far has run
    virtual void Stop() = 0;                                                    // no more submission (before the host lets go of its queues)
    virtual void Shutdown() = 0;                                                // release everything
};

// The own generator's pack surface: input buffers the host copies into, passes the host drives.
class IPackGenerator
{
public:
    virtual ~IPackGenerator() = default;
    // The generator's thread-bound state (the CUDA context) current on this thread for a GenScope.
    virtual bool PushContext() = 0;
    virtual void PopContext() = 0;

    // ---- what is built ----------------------------------------------------------------------------------
    virtual bool HasPack() const = 0;
    virtual const xefgpack::Sizes& PackSize() const = 0;
    virtual uint32_t PackVersion() const = 0;
    virtual bool PackHdr() const = 0;                                   // the pack built is the HDR10 part
    virtual bool HasPassPatches() const = 0;                            // n frames per app frame (t, pass index patches)
    virtual bool HasUiInputs() const = 0;
    virtual const xefgpack::Resource& Output() const = 0;               // layout of a generated frame
    virtual const xefgpack::Resource& InputRes(GenSlot s) const = 0;    // layout of an input
    virtual bool HasSlot(GenSlot s) const = 0;
    virtual GenBuf Buf(GenSlot s) const = 0;
    virtual size_t DispatchCount() const = 0;
    virtual std::string VariantModules() const = 0;                     // " name name ..." of the variant modules in the pack
    virtual bool InCommandLists() const = 0;                            // true: passes are recorded into the host's lists
    virtual int GpuSm() const = 0;                                      // the GPU's compute capability as sm_NN; 0 none

    // ---- per app frame ----------------------------------------------------------------------------------
    // The generator's buffers rest in a state of its own (D3D12 path: UNORDERED_ACCESS): a copy takes one out and back.
    virtual void Out(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES s) = 0;
    virtual void Back(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES s) = 0;
    virtual void MvScale(float& x, float& y) = 0;                       // the pack's own motion-vector scale
    // D3D12 path, after the inputs were copied in: B8G8R8A8 inputs turned round; the output layer's UI mode.
    virtual void SwapInputsBgra(ID3D12GraphicsCommandList* g, uint32_t uiMode) = 0;
    virtual void SetUiMode(uint32_t uiMode) = 0;
    virtual void FrameHandedOff() = 0;                                  // cur -> prev was queued

    // ---- per pass ---------------------------------------------------------------------------------------
    // D3D12 path: constants for pass k of n (jitter, t = k / (n + 1), motion-vector scale), then the pass into g.
    virtual void PreparePass(uint32_t k, uint32_t n, const float jc[2], const float jp[2], const float mvs[2]) = 0;
    // TargetFps (docs/TARGETFPS.md): the same at any t in (0, 1), pass = the pass's index within its app frame (0, 1, ... in
    // order). false: this generator cannot, and the fixed k / (n + 1) layout stays.
    virtual bool PreparePassAt(float t, uint32_t pass, const float jc[2], const float jp[2], const float mvs[2])
    {
        (void)t, (void)pass, (void)jc, (void)jp, (void)mvs;
        return false;
    }
    virtual bool PassAtSupported() const { return false; }
    // slot: the timestamp slot (the present lane's allocator index, or kAllocs + the generation lane's); tsQueue: the queue g runs on.
    virtual bool RecordPass(ID3D12GraphicsCommandList* g, uint32_t k, size_t slot, ID3D12CommandQueue* tsQueue, bool bgra) = 0;
    // The same, the frame landing in dst (a UAV-capable plain buffer, COMMON between lists) instead of Output().
    virtual void RecordPassInto(ID3D12GraphicsCommandList* g, uint32_t k, ID3D12Resource* dst, size_t slot,
                                ID3D12CommandQueue* tsQueue, bool bgra, bool dumpPending) = 0;
    virtual void ReadStamps(size_t slot) = 0;                           // the GPU time of a pass recorded in slot, once done

    // ---- CUDA path --------------------------------------------------------------------------------------
    virtual bool EnsureRing(uint32_t set, uint32_t n) = 0;              // buffers for n generated frames of a frame parity
    virtual ID3D12Resource* RingFrame(uint32_t set, uint32_t k) const = 0;
    virtual ID3D12Fence* DoneFence() const = 0;                         // signalled at done[k] when generated frame k is ready
    // All n passes of an app frame on the stream at once; done[k] is the fence value frame k is ready at. fv: the host's
    // counter of shared fence values. stamps: queue host functions that tell the host when the stream got there.
    virtual void Launch(ID3D12CommandQueue* queue, UINT64& fv, bool bgra, bool stamps, uint32_t set, uint32_t n,
                        const float jc[2], const float jp[2], const float mvs[2], uint32_t uiMode, UINT64 done[], UINT64 gateBase,
                        uint32_t spread) = 0;
    virtual bool WantsGateFence() const = 0;                            // a gate fence could still be imported
    virtual void ImportGateFence(HANDLE sharedFence) = 0;
    virtual bool HasGateFence() const = 0;
    virtual unsigned TakeHandoffFails() = 0;                            // CUDA hand-over failures since the last call

    // ---- debug (XEFG_CUDA_DUMP) -------------------------------------------------------------------------
    virtual bool DownloadInput(GenSlot s, std::vector<uint8_t>& v) = 0;
    virtual bool DownloadOutput(std::vector<uint8_t>& v, uint32_t set, uint32_t k) = 0;
};

// The generator's context current on this thread for the scope (none once the D3D12 path released CUDA).
class GenScope
{
public:
    explicit GenScope(IPackGenerator& g) : gen(g), pushed(g.PushContext()) {}
    ~GenScope()
    {
        if (pushed)
            gen.PopContext();
    }
    GenScope(const GenScope&) = delete;
    GenScope& operator=(const GenScope&) = delete;

private:
    IPackGenerator& gen;
    bool pushed;
};

} // namespace xefgown
