// generators/own.h - the own generator ("XeFG Tensor"): the graph pack (xefg_pack.h: the 38-dispatch XeFG network) and
// everything that runs it, moved out of the host (xefg_own.h, class Own) unchanged. Two ways to run the pack (Path):
//   d3d12  (default) inside the host's command lists through NVAPI cubins (xefg_pack_d3d.h): no CUDA context, second CUDA
//          queue or fence at run time. CUDA only compiles the kernels once.
//   cuda   CUDA graphs on a CUDA stream over buffers shared with D3D12 (xefg_interop.h), handed over with a shared fence
//          each way. Used when NVAPI lacks the cubin entry points.
// The host copies the game's inputs into Buf(...) and asks for passes; this class owns the pack, the CUDA interop, the
// shared buffers, the ring of generated frames (CUDA path), the split point and the GPU timing of the passes.
#pragma once

#include "../generator.h"
#include "../long_path.h"
#include "../pack_build.h"
#include "../xefg_interop.h"
#include "../xefg_pack_d3d.h"

#include <array>
#include <atomic>
#include <condition_variable>
#include <deque>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <thread>

#define XEFG_SWAPCHAIN_EXPORT_API
#include <xefg_swapchain.h>

void XLog(const char* fmt, ...);

namespace xefgown
{
using namespace xefgio;

// A pack built on the player's PC from libxess_fg.dll carries the d3d12_only marker (pack_build.h): Intel's native modules
// without PTX twins, so it runs on the D3D12 path only. `dir` is UTF-8.
inline bool IsD3dOnlyPack(const std::string& dir)
{
    if (std::string key; xefgpack::IsBuiltPath(dir + "\\" + packbuild::kD3d12OnlyMarker, &key))
    {
        const auto files = xefgpack::BuiltFiles();     // the pack built in memory
        return files && files->count(key) != 0;
    }
    return GetFileAttributesW(longpath::ExtendIfLong(longpath::WidenUtf8(dir + "\\" + packbuild::kD3d12OnlyMarker)).c_str()) !=
           INVALID_FILE_ATTRIBUTES;
}

class OwnGenerator : public IGenerator, public IPackGenerator
{
public:
    OwnGenerator(ID3D12Device* d, std::string dir, bool d3d, GenSettings& s)
        : dev(d), packDir(std::move(dir)), d3dPath(d3d), cfg(s)
    {
    }
    ~OwnGenerator() override { Shutdown(); }

    IPackGenerator* AsPack() override { return this; }
    void SetHost(const GenHost& h) override { host = h; }

    GenCaps Caps() const override
    {
        GenCaps c;
        c.name = "XeFG Tensor";
        c.available = true;
        c.reason = "";
        c.maxGenerated = kMaxInterp;
        c.presentsItself = false;
        c.anyGpu = false;       // NVIDIA: the pack's kernels are CUDA / NVAPI cubins
        return c;
    }

    // Loads the pack, creates the CUDA context and the shared buffers, builds the graph.
    bool Build(const GenBuild& gb) override
    {
        char env[MAX_PATH];
        noPatch = GetEnvironmentVariableA("XEFG_CUDA_NOPATCH", env, sizeof(env)) != 0;
        if (GetEnvironmentVariableA("XEFG_CUDA_PROF", env, sizeof(env)) && env[0] == '0')
            cfg.prof = false;
        if (gb.cancelled && gb.cancelled())
            return Fail("cancelled");
        if (!io.Init(dev))
            return Fail("interop: " + io.error);
        io.cu.CtxPopCurrent(nullptr);         // Init left the context current on this thread
        if (gb.cancelled && gb.cancelled())
            return Fail("cancelled");
        // A pack built on this PC from libxess_fg.dll (pack-less) holds Intel's native modules only: it runs on the D3D12 path.
        d3dOnly = IsD3dOnlyPack(packDir);
        if (d3dOnly && !d3dPath)
        {
            XLog("build: the pack built from libxess_fg.dll runs on the D3D12 path only - Path=cuda is not used");
            d3dPath = true;
        }
        if (d3dOnly && !cfg.native)
        {
            // its native modules have no PTX twin to run instead
            XLog("build: the pack built from libxess_fg.dll runs Intel's native modules - Native=0 is not used");
            cfg.native = true;
        }
        if (d3dPath && !nv.Load())
        {
            if (d3dOnly)
                return Fail("the pack built from libxess_fg.dll needs the D3D12 path, and NVAPI has no D3D12 cubin shaders here");
            XLog("build: NVAPI has no D3D12 cubin shaders here - using the CUDA path");
            d3dPath = false;
        }
        if (d3dPath)
        {
            // Our own queue for the build's uploads: the application's one may not be known yet.
            D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
            if (FAILED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&setupQueue))))
            {
                if (d3dOnly)
                    return Fail("the pack built from libxess_fg.dll needs the D3D12 path, and no command queue could be made");
                d3dPath = false;
            }
        }
        bool ok;
        {
            GenScope cuda(*this);
            ok = BuildPack(nullptr, gb);
            if (!ok && d3dPath && !d3dOnly)
            {
                XLog("build: the D3D12 path failed - using the CUDA path");
                d3dPath = false;
                ok = BuildPack(nullptr, gb);
            }
            if (ok && !d3dPath)
                io.cu.StreamCreate(&stream, 1);
        }
        if (ok && d3dPath)
        {
            // The kernels are compiled and cached for rebuilds at other sizes: nothing at run time needs CUDA.
            io.Release();
            XLog("build: running in our own command lists (D3D12 + NVAPI cubins); CUDA context released");
        }
        return ok;
    }

    // The pack for other sizes (the host has waited for its queues): replaces the one built before.
    bool Rebuild(const Sizes& want, const GenBuild& gb) override
    {
        GenScope cuda(*this);
        return BuildPack(&want, gb);
    }

    void Drain() override
    {
        std::unique_lock<std::mutex> l(wm);
        wcv.wait(l, [this] { return jobsDone == jobsPosted; });
    }

    void Stop() override
    {
        if (worker.joinable())
        {
            {
                std::lock_guard<std::mutex> l(wm);
                quitWorker = true;
            }
            wcv.notify_all();
            worker.join();
        }
    }

    void Shutdown() override
    {
        if (shutDone)
            return;
        shutDone = true;
        Stop();
        dpack.reset();
        for (IUnknown* u : std::initializer_list<IUnknown*> { setupQueue, stampHeap, stampRb, dispHeap, dispRb })
            if (u)
                u->Release();
        setupQueue = nullptr;
        stampHeap = nullptr;
        stampRb = nullptr;
        dispHeap = nullptr;
        dispRb = nullptr;
        if (io.ctx)
        {
            GenScope s(*this);
            if (gateSem)
                io.cu.DestroyExternalSemaphore(gateSem);
            gateSem = nullptr;
            // Prof's events (Launch creates them on first use)
            for (CUevent e : { evW, evH })
                if (e)
                    io.cu.EventDestroy(e);
            for (const CUevent* set : { evT, evG, evS })
                for (size_t i = 0; i < kMaxInterp; ++i)
                    if (set[i])
                        io.cu.EventDestroy(set[i]);
            if (stream)
            {
                io.cu.StreamSynchronize(stream);    // a queued host function (Stamp) must not run after we are gone
                io.cu.StreamDestroy(stream);
            }
            pack.reset();
            ReleaseShared();
        }
        else
        {
            pack.reset();
            ReleaseShared();
        }
    }

    // The CUDA context current on this thread (none once the D3D12 path released it).
    bool PushContext() override { return io.ctx && io.cu.CtxPushCurrent(io.ctx) == 0; }
    void PopContext() override { io.cu.CtxPopCurrent(nullptr); }

    bool HasPack() const override { return pack != nullptr; }
    const Sizes& PackSize() const override { return pack->size; }
    uint32_t PackVersion() const override { return static_cast<uint32_t>(pack->version); }
    bool PackHdr() const override { return packHdr; }
    bool HasPassPatches() const override { return pack->HasPassPatches(); }
    bool HasUiInputs() const override { return rUi >= 0; }
    const Resource& Output() const override { return pack->res[rOut]; }
    const Resource& InputRes(GenSlot s) const override { return pack->res[Role(s)]; }
    bool HasSlot(GenSlot s) const override { return Role(s) >= 0; }
    GenBuf Buf(GenSlot s) const override
    {
        const SharedBuf& b = Shared(s);
        return { b.res, b.bytes };
    }
    size_t DispatchCount() const override { return pack->disp.size(); }
    std::string VariantModules() const override
    {
        std::string mods;
        for (const Kernel& k : pack->kernels)
            if (k.file.rfind("v_", 0) == 0)
                mods += " " + k.module;
        return mods;
    }
    bool InCommandLists() const override { return d3dPath; }
    int GpuSm() const override { return io.sm; }

    // D3D12 path: the pack's buffers rest in UNORDERED_ACCESS (xefg_pack_d3d.h), so a copy takes one out of it
    // and back. The CUDA path's shared buffers rest in COMMON and are promoted implicitly: nothing to do.
    bool PackBuf(ID3D12Resource* r) const
    {
        return d3dPath && r && (r == cur.res || r == prev.res || r == mv.res || r == depth.res || r == depthPrev.res || r == out.res || r == uic.res ||
                                r == bbc.res);
    }
    void Out(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES s) override
    {
        if (PackBuf(r))
            GenBarrier(g, r, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, s);
    }
    void Back(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES s) override
    {
        if (PackBuf(r))
            GenBarrier(g, r, s, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    }

    void MvScale(float& x, float& y) override { pack->MvScale(x, y); }
    void SwapInputsBgra(ID3D12GraphicsCommandList* g, uint32_t uiMode) override
    {
        if (!dpack)                 // the CUDA path turns them on its stream (Launch)
            return;
        dpack->SwapRB(g, rCur);
        if (uiMode >= 2)
            dpack->SwapRB(g, rBb);
        if (uiMode == 2 || uiMode == 3 || uiMode == 5)
            dpack->SwapRB(g, rUi);
    }
    void SetUiMode(uint32_t uiMode) override
    {
        if (rUi >= 0 && dpack)      // the CUDA path sets it on its own thread (Launch)
            pack->SetPatchBits("ui_mode", uiMode);
    }
    void FrameHandedOff() override
    {
        prevSwapped = curSwapped;
        curSwapped = false;
    }

    // The constants of pass k of n, copied into the list as it is recorded (RecordPass).
    void PreparePass(uint32_t k, uint32_t n, const float jc[2], const float jp[2], const float mvs[2]) override
    {
        if (!noPatch)
            SetJitter(jc, jp);
        pack->SetPass(static_cast<float>(k) / static_cast<float>(n + 1), k - 1, mvs[0], mvs[1]);
    }

    bool EnsureRing(uint32_t set, uint32_t n) override { return EnsureRingC(set, n); }
    ID3D12Resource* RingFrame(uint32_t set, uint32_t k) const override { return ringC[set][k].res; }
    ID3D12Fence* DoneFence() const override { return io.fence; }
    bool WantsGateFence() const override { return !d3dPath && io.ctx && !gateSem; }
    void ImportGateFence(HANDLE sharedFence) override
    {
        GenScope cuda(*this);
        ExtSemHandleDesc sd {};
        sd.type = kExtSemD3D12Fence;
        sd.handle.win32.handle = sharedFence;
        if (io.cu.ImportExternalSemaphore(&gateSem, &sd))
            gateSem = nullptr;
    }
    bool HasGateFence() const override { return gateSem != nullptr; }
    unsigned TakeHandoffFails() override { return handoffFails.exchange(0); }

    bool DownloadInput(GenSlot s, std::vector<uint8_t>& v) override
    {
        if (d3dPath)
            return dpack->Download(Role(s), v);
        GenScope c(*this);
        return pack->Download(Role(s), v);
    }
    // the CUDA path's generated frame k is in its ring buffer (the pack's output holds the last pass)
    bool DownloadOutput(std::vector<uint8_t>& v, uint32_t set, uint32_t k) override
    {
        if (d3dPath)
            return dpack->Download(rOut, v);
        GenScope c(*this);
        return pack->Download(rOut, v, ringC[set][k - 1].ptr);
    }

    // ---- the CUDA path: all of an app frame's passes on the stream at once ----------------------------------

    // CUDA path: all nGen generated frames of an app frame on the stream at once, each copied into its own ring
    // buffer (ringC) and handed over with the shared fence at its own value (done[k - 1]); our queue waits for
    // each, GPU side, just before its copy. Pass k + 1 runs while frame k waits for its present time, so a late
    // present no longer holds back the passes after it.
    // B8G8R8A8: the inputs turned to R8G8B8A8 on the stream before the first pass (as CopyFrame does on D3D12), the
    // output back after each. prev is cur of the last frame: turned already unless that frame ran no pass (the first
    // frame, a reset or a camera cut), then it is turned here too.
    void Launch(ID3D12CommandQueue* queue, UINT64& fv, bool bgra, bool stamps, uint32_t set, uint32_t nGen, const float jc[2],
                const float jp[2], const float mvs[2], uint32_t uiMode, UINT64 done[], UINT64 gateBase, uint32_t spread) override
    {
        const SharedBuf(&into)[kMaxInterp] = ringC[set];
        const UINT64 toCuda = ++fv;
        std::array<UINT64, kMaxInterp> sig {};
        std::array<CUdeviceptr, kMaxInterp> dst {};
        for (uint32_t k = 0; k < nGen; ++k)
        {
            done[k] = sig[k] = ++fv;
            dst[k] = into[k].ptr;
        }
        queue->Signal(io.fence, toCuda);
        const float c0 = jc[0], c1 = jc[1], p0 = jp[0], p1 = jp[1], m0 = mvs[0], m1 = mvs[1];
        const bool swapPrev = bgra && !prevSwapped;
        if (bgra)
            curSwapped = true;
        Post([this, toCuda, sig, dst, nGen, c0, c1, p0, p1, m0, m1, bgra, stamps, swapPrev, uiMode, gateBase, spread] {
            if (rUi >= 0)
                pack->SetPatchBits("ui_mode", uiMode);   // the pack's constants are this thread's on the CUDA path
            bool w = io.CudaWait(stream, toCuda);
            if (bgra)
            {
                // prev as well when the presenter saw no pass last frame, or when last frame's swap of cur did not run
                // (a failed handoff): this thread knows the latter, the presenter only the former.
                const bool doPrev = swapPrev || !lastCurSwapped;
                const bool curOk = w && pack->SwapRB(stream, rCur);
                lastCurSwapped = curOk;
                w = curOk && (!doPrev || pack->SwapRB(stream, rPrev)) && (uiMode < 2 || pack->SwapRB(stream, rBb)) &&
                    ((uiMode != 2 && uiMode != 3 && uiMode != 5) || pack->SwapRB(stream, rUi));
            }
            if (cfg.prof && !evW)
            {
                io.cu.EventCreate(&evW, 0);
                io.cu.EventCreate(&evH, 0);
                for (CUevent& e : evT)
                    io.cu.EventCreate(&e, 0);
                for (CUevent& e : evG)
                    io.cu.EventCreate(&e, 0);
                for (CUevent& e : evS)
                    io.cu.EventCreate(&e, 0);
            }
            const bool timed = cfg.prof && w;
            if (timed)
                io.cu.EventRecord(evW, stream);
            if (!noPatch)
            {
                const float a[2] = { c0, c1 }, b[2] = { p0, p1 };
                SetJitter(a, b);
            }
            uint32_t ran = 0;
            for (uint32_t k = 1; k <= nGen; ++k)
            {
                if (spread && k > 1 && gateBase + k > spread)
                {
                    ExtSemWaitParams gp {};
                    gp.params.fence.value = gateBase + k - spread;
                    w = w && io.cu.WaitExternalSemaphoresAsync(&gateSem, &gp, 1, stream) == 0;
                }
                const float tInterp = static_cast<float>(k) / static_cast<float>(nGen + 1);
                pack->SetPass(tInterp, k - 1, m0, m1);
                // The head (input preparation) depends only on the app's frames: once per app frame; the tail per generated frame.
                LARGE_INTEGER cq0, cq1, cqf;
                QueryPerformanceCounter(&cq0);
                w = w && (!pack->CanSplit() ? pack->Launch(stream) : (((k > 1 || pack->LaunchHead(stream)) && (!timed || k > 1 || io.cu.EventRecord(evH, stream) == 0)) && pack->LaunchTail(stream)));
                QueryPerformanceCounter(&cq1);
                QueryPerformanceFrequency(&cqf);
                if (timed)
                    cpuLaunch += static_cast<double>(cq1.QuadPart - cq0.QuadPart) * 1000.0 / static_cast<double>(cqf.QuadPart);
                else if (!cfg.prof)
                    pack->tSetParams = pack->tGraphLaunch = 0, pack->lastDirty = 0;     // no prof line reads them
                if (timed && w)
                    io.cu.EventRecord(evG[k - 1], stream);
                w = w && (!bgra || pack->SwapRB(stream, rOut));
                if (timed && w)
                    io.cu.EventRecord(evS[k - 1], stream);
                w = w && io.cu.MemcpyDtoDAsync(dst[k - 1], out.ptr, out.bytes, stream) == 0;
                if (timed && w)
                    io.cu.EventRecord(evT[k - 1], stream);
                w = w && io.CudaSignal(stream, sig[k - 1]);
                if (w && stamps)
                {
                    // When the stream got here: the generated frame's done time for the clock (ClockNeed).
                    HostStamp& h = hostStamp[sig[k - 1] % kStamps];
                    h.o = this;
                    h.v = sig[k - 1];
                    if (io.cu.LaunchHostFunc(stream, &Stamp, &h) == 0 && host.stampsWork)
                        host.stampsWork(host.user);
                }
                ran += w;
            }
            if (ran < nGen)
            {
                // Release the D3D queue's waits so the app does not hang: once the stream is idle and our queue has
                // signalled toCuda (it may still wait for the app's frame; the stream need not have waited for it, after a
                // failed wait or a sticky error), so that no signal still to come can set the fence back below them.
                io.cu.StreamSynchronize(stream);
                if (io.fence->GetCompletedValue() < toCuda)
                {
                    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
                    if (ev && SUCCEEDED(io.fence->SetEventOnCompletion(toCuda, ev)))
                        WaitForSingleObject(ev, 2000);
                    if (ev)
                        CloseHandle(ev);
                }
                const UINT64 doneBefore = io.fence->GetCompletedValue();
                io.fence->Signal(sig[nGen - 1]);
                if (++failLogged <= 12)
                    XLog("present: handoff failure %u: pass %u of %u, signal %llu (toCuda %llu), fence was %llu, release thread waiting for %llu: %s",
                         failLogged, ran + 1, nGen, sig[ran < nGen ? ran : nGen - 1], toCuda, doneBefore, host.waitingFor ? host.waitingFor(host.user) : 0, io.error.c_str());
                ++handoffFails;
                if (!cudaFailed.exchange(true))
                    XLog("present: CUDA handoff failed (pass %u of %u, toCuda=%llu, fence=%llu): %s %s", ran + 1, nGen, toCuda,
                         io.fence->GetCompletedValue(), io.error.c_str(), pack->error.c_str());
            }
            if (timed && ran)
            {
                // GPU time of the head and each tail as the stream saw it (includes any time the GPU spent on the game).
                io.cu.EventSynchronize(evT[ran - 1]);
                const bool split = pack->CanSplit();
                float a = 0;
                if (split)
                {
                    io.cu.EventElapsedTime(&a, evW, evH);
                    gpuHead += a;
                    ++nHead;
                }
                for (uint32_t k = 0; k < ran; ++k)
                {
                    float b = 0;
                    io.cu.EventElapsedTime(&b, k ? evT[k - 1] : split ? evH : evW, evT[k]);
                    gpuTail += b;
                    float g = 0, s = 0, c = 0;
                    io.cu.EventElapsedTime(&g, k ? evT[k - 1] : split ? evH : evW, evG[k]);
                    io.cu.EventElapsedTime(&s, evG[k], evS[k]);
                    io.cu.EventElapsedTime(&c, evS[k], evT[k]);
                    gpuGraph += g, gpuSwap += s, gpuCopy += c;
                    ++nTail;
                }
                // after the whole run, so the window's launch times and its tails are the same runs'
                if (nTail >= 240)
                {
                    XLog("prof: GPU per run, head %.2f ms (x%u), tail %.2f ms (x%u) [graph %.2f, swap %.2f, copy %.2f], CPU launch %.2f ms per pass (SetParams %.2f, GraphLaunch %.2f, %u nodes set per run)",
                         nHead ? gpuHead / nHead : 0.0, nHead, gpuTail / nTail, nTail, gpuGraph / nTail, gpuSwap / nTail, gpuCopy / nTail,
                         cpuLaunch / nTail, pack->tSetParams / (nTail + nHead), pack->tGraphLaunch / (nTail + nHead),
                         pack->lastDirty / (nTail + nHead));
                    pack->tSetParams = pack->tGraphLaunch = 0, pack->lastDirty = 0;
                    gpuGraph = gpuSwap = gpuCopy = cpuLaunch = 0;
                    gpuHead = gpuTail = 0;
                    nHead = nTail = 0;
                }
            }
        });
    }

    // ---- the D3D12 path: a pass recorded into the host's list -----------------------------------------------

    // D3D12 path: one generated frame recorded on g, with this pass's constants. The head (input preparation)
    // only on the first pass of an app frame, as on the CUDA path. GPU time by timestamps (prof) in stamp slot
    // `slot`: the present lane's allocator index, or kAllocs + the generation lane's.
    // A pass on the generation lane whose frame ends up in `dst` (a UAV-capable plain buffer, COMMON between lists): the
    // blend writes it there itself (D3DPack::Redirect), which saves copying the 2560x1440 output into it every generated
    // frame. With a dump pending the output stays in the pack's buffer (DumpOnce reads it) and is copied.
    void RecordPassInto(ID3D12GraphicsCommandList* g, uint32_t k, ID3D12Resource* dst, size_t slot, ID3D12CommandQueue* tsQueue,
                        bool bgra, bool dumpPending) override
    {
        static bool warned = false;
        if (!warned && !dumpPending && !(dst->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS))
        {
            XLog("present: ring buffer without UAV access - copying the output into it");
            warned = true;
        }
        if (!dumpPending && (dst->GetDesc().Flags & D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS) && dpack->Redirect(rOut, dst))
        {
            GenBarrier(g, dst, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            RecordPass(g, k, slot, tsQueue, bgra);
            dpack->Redirect(rOut, nullptr);
            GenBarrier(g, dst, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COMMON);
            return;
        }
        RecordPass(g, k, slot, tsQueue, bgra);
        Out(g, out.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
        g->CopyBufferRegion(dst, 0, out.res, 0, out.bytes);
        Back(g, out.res, D3D12_RESOURCE_STATE_COPY_SOURCE);
    }

    bool RecordPass(ID3D12GraphicsCommandList* g, uint32_t k, size_t slot, ID3D12CommandQueue* tsQueue, bool bgra) override
    {
        const size_t n = pack->disp.size();
        const bool split = splitAt > 0 && splitAt < n;
        const size_t from = split && k > 1 ? splitAt : 0;
        if (cfg.prof && !stampHeap)
        {
            D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, static_cast<UINT>(3 * kSlots) };
            D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 24 * kSlots;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&stampHeap))) ||
                FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&stampRb))))
                cfg.prof = false;
            tsQueue->GetTimestampFrequency(&tsFreq);
        }
        // Prof=2: also a timestamp after every dispatch (each then alone behind a UAV barrier), summed per dispatch
        // as XeFGPsoCap sums Intel's - the like-for-like figure for MFG Unlocker's runs.
        if (cfg.prof && cfg.profDispatches && !dispHeap)
        {
            D3D12_QUERY_HEAP_DESC qd { D3D12_QUERY_HEAP_TYPE_TIMESTAMP, static_cast<UINT>((n + 1) * kSlots) };
            D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_READBACK };
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
            rd.Width = 8 * (n + 1) * kSlots;
            rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.SampleDesc.Count = 1;
            rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
            if (FAILED(dev->CreateQueryHeap(&qd, IID_PPV_ARGS(&dispHeap))) ||
                FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COPY_DEST, nullptr, IID_PPV_ARGS(&dispRb))))
                cfg.profDispatches = false;
            dispMs.assign(n, 0.0);
        }
        ID3D12QueryHeap* ds = cfg.prof && cfg.profDispatches ? dispHeap : nullptr;
        const UINT d0 = static_cast<UINT>((n + 1) * slot);
        const UINT q0 = static_cast<UINT>(3 * slot);
        if (cfg.prof)
            g->EndQuery(stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q0);
        bool ok = dpack->Record(g, from, split && from == 0 ? splitAt : from, ds, d0);
        if (cfg.prof)
            g->EndQuery(stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 1);
        ok = ok && dpack->Record(g, split && from == 0 ? splitAt : from, n, ds, d0);
        if (cfg.prof)
        {
            g->EndQuery(stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q0 + 2);
            g->ResolveQueryData(stampHeap, D3D12_QUERY_TYPE_TIMESTAMP, q0, 3, stampRb, 24 * slot);
            if (ds)
                g->ResolveQueryData(dispHeap, D3D12_QUERY_TYPE_TIMESTAMP, d0 + static_cast<UINT>(from), static_cast<UINT>(n - from + 1),
                                    dispRb, 8 * (d0 + from));
            stamped[slot] = true;
            stampedHead[slot] = split && from == 0;
            stampedFrom[slot] = from;
        }
        if (ok && bgra)
            dpack->SwapRB(g, rOut);        // the generated frame back into the B8G8R8A8 byte order
        if (!ok && !cudaFailed.exchange(true))
            XLog("present: recording the pack failed: %s", dpack->error.c_str());
        return ok;
    }
    // The GPU time of a pass recorded with allocator i, once its list has finished (Begin waited for it).
    void ReadStamps(size_t i) override
    {
        if (!stamped[i])
            return;
        stamped[i] = false;
        const D3D12_RANGE r { 24 * i, 24 * i + 24 }, none { 0, 0 };
        UINT64* v = nullptr;
        if (FAILED(stampRb->Map(0, &r, reinterpret_cast<void**>(&v))))
            return;
        const UINT64* s = reinterpret_cast<const UINT64*>(reinterpret_cast<const uint8_t*>(v) + 24 * i);
        const double head = double(s[1] - s[0]) * 1000.0 / double(tsFreq), tail = double(s[2] - s[1]) * 1000.0 / double(tsFreq);
        stampRb->Unmap(0, &none);
        const size_t n = dispMs.size();
        if (cfg.profDispatches && dispRb && n)
        {
            const size_t b0 = (n + 1) * i, from = stampedFrom[i];
            const D3D12_RANGE dr { 8 * (b0 + from), 8 * (b0 + n + 1) };
            UINT64* dv = nullptr;
            if (SUCCEEDED(dispRb->Map(0, &dr, reinterpret_cast<void**>(&dv))))
            {
                const UINT64* t = reinterpret_cast<const UINT64*>(reinterpret_cast<const uint8_t*>(dv) + 8 * b0);
                for (size_t k = from; k < n; ++k)
                    dispMs[k] += double(t[k + 1] - t[k]) * 1000.0 / double(tsFreq);
                dispRb->Unmap(0, &none);
                if (from == 0)
                    ++dispApp;
            }
        }
        if (stampedHead[i])
        {
            gpuHead += head;
            ++nHead;
        }
        gpuTail += tail;
        if (++nTail % 240 == 0)
        {
            XLog("prof: GPU per run, head %.2f ms (x%u), tail %.2f ms (x%u) [d3d12]", nHead ? gpuHead / nHead : 0.0, nHead, gpuTail / nTail, nTail);
            if (cfg.profDispatches && dispApp)
            {
                // Sums of each dispatch's own time (XeFGPsoCap's measure): per app frame, the head's, a tail's.
                double all = 0, hd = 0;
                for (size_t k = 0; k < n; ++k)
                {
                    all += dispMs[k];
                    if (k < splitAt)
                        hd += dispMs[k];
                }
                XLog("prof: dispatch sums %.3f ms per app frame: head %.3f, tail %.3f (x%.1f) over %u app frames [d3d12, a barrier "
                     "after each dispatch]", all / dispApp, hd / dispApp, nTail ? (all - hd) / nTail : 0.0, double(nTail) / dispApp, dispApp);
                for (size_t k = 0; k < n; ++k)
                    XLog("prof:   %2zu %-26s %-24s %.4f ms per app frame", k, pack->disp[k].module.c_str(),
                         pack->kernels[pack->disp[k].kernel].file.c_str(), dispMs[k] / dispApp);
                dispMs.assign(n, 0.0);
                dispApp = 0;
            }
            gpuHead = gpuTail = 0;
            nHead = nTail = 0;
        }
    }

private:
    int Role(GenSlot s) const
    {
        switch (s)
        {
        case GenSlot::Cur: return rCur;
        case GenSlot::Prev: return rPrev;
        case GenSlot::Motion: return rMv;
        case GenSlot::Depth: return rDepth;
        case GenSlot::DepthPrev: return rDepthPrev;
        case GenSlot::Output: return rOut;
        case GenSlot::Ui: return rUi;
        case GenSlot::BackBuffer: return rBb;
        }
        return -1;
    }
    const SharedBuf& Shared(GenSlot s) const
    {
        switch (s)
        {
        case GenSlot::Cur: return cur;
        case GenSlot::Prev: return prev;
        case GenSlot::Motion: return mv;
        case GenSlot::Depth: return depth;
        case GenSlot::DepthPrev: return depthPrev;
        case GenSlot::Output: return out;
        case GenSlot::Ui: return uic;
        case GenSlot::BackBuffer: return bbc;
        }
        return cur;
    }

    bool Fail(const std::string& e)
    {
        XLog("build: %s", e.c_str());
        return false;
    }

    // CUDA path: cuGraphLaunch blocks for ~20 ms under game GPU load, so CUDA submission runs on its own
    // thread. The D3D queue only waits on the fence, so it does not need the submission to have happened yet.
    void Post(std::function<void()> job)
    {
        std::lock_guard<std::mutex> l(wm);
        if (!worker.joinable())
            worker = std::thread([this] {
                for (;;)
                {
                    std::function<void()> j;
                    {
                        std::unique_lock<std::mutex> l2(wm);
                        wcv.wait(l2, [this] { return quitWorker || !jobs.empty(); });
                        if (jobs.empty())
                            return;
                        j = std::move(jobs.front());
                        jobs.pop_front();
                    }
                    {
                        GenScope s(*this);
                        j();
                    }
                    {
                        std::lock_guard<std::mutex> l2(wm);
                        ++jobsDone;
                    }
                    wcv.notify_all();
                }
            });
        jobs.push_back(std::move(job));
        ++jobsPosted;
        wcv.notify_all();
    }


    void SetJitter(const float jc[2], const float jp[2])
    {
        pack->SetPatch("jitter_cur_x", jc[0]);
        pack->SetPatch("jitter_cur_y", jc[1]);
        pack->SetPatch("jitter_prev_x", jp[0]);
        pack->SetPatch("jitter_prev_y", jp[1]);
    }

    // A CUDA host function after a pass's fence signal: when the stream got there.
    static void Stamp(void* p)
    {
        const HostStamp* h = static_cast<const HostStamp*>(p);
        h->o->host.stampDone(h->o->host.user, h->v);
    }

    // The pack's buffers: the CUDA path's shared ones are ours (CUDA context current), the D3D12 runner's go with it.
    void ReleaseShared()
    {
        for (SharedBuf* b : { &cur, &prev, &mv, &depth, &depthPrev, &out, &uic, &bbc })
            if (b->mem)
                io.Destroy(*b);
            else
                *b = SharedBuf();
        for (auto& set : ringC)
            for (SharedBuf& b : set)
                io.Destroy(b);
    }
    // CUDA path: ring buffers for nGen generated frames, the output's size; false (logged once) if one cannot be made.
    bool EnsureRingC(uint32_t set, uint32_t nGen)
    {
        for (uint32_t k = 0; k < nGen; ++k)
            if (!ringC[set][k].ptr && !io.CreateBuffer(out.bytes, ringC[set][k]))
            {
                io.Destroy(ringC[set][k]);
                if (!warnedRing)
                    XLog("present: no buffer for generated frame %u (%s) - presenting the app's frames only", k + 1, io.error.c_str());
                warnedRing = true;
                return false;
            }
        return true;
    }

    // Loads the pack for `want` (a "pack 2" at any size; null: its captured size), creates the shared
    // buffers, the staging sets and builds the graph. Replaces any pack built before, so nothing may still be
    // using it (DrainPresenter, WaitIdle, WaitApp). Needs the CUDA context current.
    bool BuildPack(const Sizes* want, const GenBuild& gb)
    {
        dpack.reset();
        pack.reset();
        ReleaseShared();
        if (gb.releaseHostBuffers)
            gb.releaseHostBuffers();
        // Prof=2's per-dispatch stamps are laid out by the old pack's dispatch count (RecordPass makes them anew)
        if (dispHeap)
            dispHeap->Release();
        if (dispRb)
            dispRb->Release();
        dispHeap = nullptr;
        dispRb = nullptr;
        dispMs.clear();
        std::fill(std::begin(stamped), std::end(stamped), false);
        pack = std::make_unique<Pack>(io.cu);
        // Kernels newer than this GPU run their fallback (an RTX 20: our own kernels at sm_75). XEFG_CUDA_SM=NN pretends
        // another GPU (testing).
        {
            char sm[16];
            pack->deviceSm = GetEnvironmentVariableA("XEFG_CUDA_SM", sm, sizeof(sm)) ? atoi(sm) : io.sm;
        }
        // The init flags pick the pack's module variants (inverted depth, NDC / jittered motion vectors, UI
        // texture premultiplied); the sizes pick high- or low-res motion vectors inside Load.
        Variant var;
        var.inv = (gb.initFlags & XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH) != 0;
        var.ndc = (gb.initFlags & XEFG_SWAPCHAIN_INIT_FLAG_USE_NDC_VELOCITY) != 0;
        var.jit = (gb.initFlags & XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV) != 0;
        var.premul = (gb.initFlags & XEFG_SWAPCHAIN_INIT_FLAG_UITEXTURE_NOT_PREMUL_ALPHA) == 0;
        // An HDR10 back buffer takes the pack's hdr\ part (its own frame formats and modules).
        var.hdr = gb.hdr;
        pack->allowNativeOnly = d3dOnly;
        if (!pack->Load(var.hdr ? packDir + "\\hdr" : packDir, want, var))
            return Fail(std::string("pack") + (var.hdr ? " (HDR10 part)" : "") + ": " + pack->error);
        if (pack->fellBack)
            XLog("build: this GPU is sm_%d - %d kernels run their fallback (their sm_75 version)", pack->deviceSm, pack->fellBack);
        packHdr = var.hdr;
        // A variant this run has not compiled yet (a resize to render = output size) needs CUDA again for a
        // moment, after the first build released it.
        bool tempCuda = false;
        rCur = pack->Role("frame_cur");
        rPrev = pack->Role("frame_prev");
        rMv = pack->Role("motion");
        rDepth = pack->Role("depth");
        rOut = pack->Role("output");
        rUi = pack->Role("ui");                 // the UI composition inputs, optional (tools/pack_ui.py)
        rBb = pack->Role("backbuffer");
        if (rUi < 0 || rBb < 0)
            rUi = rBb = -1;
        rDepthPrev = pack->Role("depth_prev");    // the previous app frame's depth, optional (the multi-frame model)
        if (rCur < 0 || rPrev < 0 || rMv < 0 || rDepth < 0 || rOut < 0)
            return Fail("pack lacks a frame_cur/frame_prev/motion/depth/output role");
        if (rDepthPrev >= 0 && pack->res[rDepthPrev].bytes != pack->res[rDepth].bytes)
            return Fail("pack's depth_prev and depth differ in size");
        std::vector<std::pair<SharedBuf*, int>> map = { { &cur, rCur }, { &prev, rPrev }, { &mv, rMv }, { &depth, rDepth }, { &out, rOut } };
        if (rDepthPrev >= 0)
            map.push_back({ &depthPrev, rDepthPrev });
        if (rUi >= 0)
            map.insert(map.end(), { { &uic, rUi }, { &bbc, rBb } });
        splitAt = pack->FindSplit();
        if (d3dPath)
        {
            dpack = std::make_unique<D3DPack>(*pack, io.cu, nv);
            dpack->cubins = &cubins;
            dpack->cancel = gb.cancelled;
            // a kernel not compiled yet (a variant a resize brings in) needs CUDA again for a moment, after the first build
            // released it; the cache is keyed by the PTX content, so only the compile decides
            dpack->needCuda = [&]() {
                if (io.ctx)
                    return true;
                if (!io.Init(dev))
                {
                    XLog("interop (compiling a kernel): %s", io.error.c_str());
                    return false;
                }
                tempCuda = true;
                return true;
            };
            dpack->packNatives = cfg.native;
            const bool built3d = dpack->Build(dev, setupQueue, false);
            dpack->cancel = nullptr;           // it refers to the caller's state
            dpack->needCuda = nullptr;          // it refers to this call's locals
            if (tempCuda)
            {
                io.cu.CtxPopCurrent(nullptr);         // Init left the context current on this thread
                io.Release();
            }
            if (!built3d)
            {
                const std::string e = dpack->error;
                dpack.reset();
                return Fail("pack build (D3D12): " + e);
            }
            for (const auto& [b, r] : map)
            {
                b->res = dpack->Buffer(r);
                b->bytes = pack->res[r].bytes;
            }
        }
        else
        {
            for (const auto& [b, r] : map)
            {
                if (!io.CreateBuffer(pack->res[r].bytes, *b))
                    return Fail(pack->res[r].id + ": " + io.error);
                pack->SetExternal(r, b->ptr);
            }
            if (!pack->Build(false))
                return Fail("pack build: " + pack->error);
        }
        if (gb.packBuilt && !gb.packBuilt())
            return false;
        return true;
    }


    ID3D12Device* dev = nullptr;
    std::string packDir;
    Interop io;
    bool d3dPath = true;
    bool d3dOnly = false;               // the pack is the one built from libxess_fg.dll: no PTX twins, D3D12 path only
    GenSettings& cfg;                  // Prof= (or XEFG_CUDA_PROF), Native= in f5low.ini, shared with the host
    GenHost host;
    bool shutDone = false;
    NvApi nv;
    std::unique_ptr<D3DPack> dpack;     // D3D12 path: the runner (references *pack, so it goes first)
    std::map<std::string, std::vector<uint8_t>> cubins;
    ID3D12CommandQueue* setupQueue = nullptr;
    size_t splitAt = 0;
    ID3D12QueryHeap* stampHeap = nullptr;
    ID3D12Resource* stampRb = nullptr;
    UINT64 tsFreq = 1;
    bool stamped[kSlots] {}, stampedHead[kSlots] {};
    size_t stampedFrom[kSlots] {};
    ID3D12QueryHeap* dispHeap = nullptr;        // Prof=2: a timestamp after each dispatch, n + 1 per slot
    ID3D12Resource* dispRb = nullptr;
    std::vector<double> dispMs;                 // per dispatch, summed since the last log
    uint32_t dispApp = 0;                       // app frames (head runs) in those sums
    std::unique_ptr<Pack> pack;
    int rCur = -1, rPrev = -1, rMv = -1, rDepth = -1, rDepthPrev = -1, rOut = -1, rUi = -1, rBb = -1;
    bool curSwapped = false, prevSwapped = false;   // CUDA path, B8G8R8A8: cur / prev already turned to R8G8B8A8 (Launch)
    bool lastCurSwapped = true;                     // the CUDA worker's own: its last swap of cur ran
    SharedBuf cur, prev, mv, depth, depthPrev, out;
    SharedBuf uic, bbc;                     // the output layer's UI texture (t9) and back buffer (t10), modes 2-5
    SharedBuf ringC[2][kMaxInterp];         // CUDA path: each generated frame of an app frame (Launch), by frame parity
    CUstream stream = nullptr;
    bool packHdr = false;                   // the pack built is the hdr part (R10G10B10A2 frames)
    bool warnedRing = false;
    bool noPatch = false;
    CUevent evW = nullptr, evH = nullptr, evT[kMaxInterp] {}, evG[kMaxInterp] {}, evS[kMaxInterp] {};
    double gpuGraph = 0, gpuSwap = 0, gpuCopy = 0, cpuLaunch = 0;
    double gpuHead = 0, gpuTail = 0;
    unsigned nHead = 0, nTail = 0;
    CUexternalSemaphore gateSem = nullptr;  // CUDA path, GenSpread: the host's gate fence as the stream sees it (Launch)
    std::atomic<bool> cudaFailed { false };
    unsigned failLogged = 0;
    std::atomic<unsigned> handoffFails { 0 };
    // CUDA path: when each fence value the stream signals was reached (a host function after the signal; the host's ClockNeed).
    struct HostStamp
    {
        OwnGenerator* o = nullptr;
        UINT64 v = 0;
    };
    std::array<HostStamp, kStamps> hostStamp {};
    // CUDA path: cuGraphLaunch blocks for ~20 ms under game GPU load, so CUDA submission runs on its own thread.
    std::thread worker;
    std::mutex wm;
    std::condition_variable wcv;
    std::deque<std::function<void()>> jobs;
    bool quitWorker = false;
    unsigned long long jobsPosted = 0, jobsDone = 0;
};

} // namespace xefgown
