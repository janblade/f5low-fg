// frames_only.h - the "frames only" API of f5low.dll (include/f5low_frames.h): the own generator ("XeFG Tensor") without a swap chain.
//
// A host (OptiScaler's Vulkan output, a test) owns presenting and pacing. This context turns two app frames into 1..5 generated
// frames on the host's D3D12 device, on a queue of its own, between fences the host gives (docs/GENERATORS.md "Frames only API").
// It reuses the generator (generators/own.h, OwnGenerator: the pack, the cubin passes) and the recording helpers the host class
// (xefg_own.h, Own) shares (CopyPitched, RecordCopyToPrev, MakeConvertPipeline, RecordConvert). Nothing of the swap chain side of
// Own (Proxy, presenter, gate, XeLL, pacing) is used.
//
// Per app frame, all on one queue and in this order: wait for the host's readyFence; list 1 copies the inputs into the pack's buffers
// (motion and depth through the format conversion when they are not the pack's layout), turns B8G8R8A8 round; consumedFence is
// signalled; one list per generated frame (the pass, the frame copied into the host's target texture); doneFence; cur -> prev.
// The pack is built in the background from the player's libxess_fg.dll as for the XeFG API; until then Generate only passes the
// fences through (NOT_READY).
#pragma once

#include "xefg_own.h"

#include <f5low_frames.h>

namespace xefgown
{

class FramesCtx
{
public:
    struct Params
    {
        ID3D12Device* dev = nullptr;
        uint32_t w = 0, h = 0, rw = 0, rh = 0;
        DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
        uint32_t maxGen = 0;
        uint32_t initFlags = 0;                        // XEFG_SWAPCHAIN_INIT_FLAG_*
        std::string packDir;
        std::function<int(std::string*)> packPoll;     // 0 still being made, 1 ready, 2 failed (xefg_cuda.cpp PackPoll)
        std::function<std::string()> packFit;          // with a ready pack: "" when this GPU runs it, else why not
    };

    static bool ValidFormat(DXGI_FORMAT f) { return ColourFamily(f) != 0; }

    // The arguments of Create, checked without the GPU. "" when fine.
    static std::string CheckCreate(const F5LowFramesCreateInfo& i)
    {
        if (i.apiVersion != F5LOW_FRAMES_API_VERSION)
            return "apiVersion " + std::to_string(i.apiVersion) + " is not " + std::to_string(F5LOW_FRAMES_API_VERSION);
        if (!i.device)
            return "no device";
        if (i.width < 64 || i.height < 64 || i.width > 16384 || i.height > 16384)
            return "size " + std::to_string(i.width) + "x" + std::to_string(i.height) + " is outside 64..16384";
        if (!ValidFormat(i.colorFormat))
            return "colorFormat " + std::to_string(static_cast<int>(i.colorFormat)) + " is not R8G8B8A8, B8G8R8A8 or R10G10B10A2";
        if (!i.maxGenerated || i.maxGenerated > F5LOW_FRAMES_MAX_GENERATED)
            return "maxGenerated " + std::to_string(i.maxGenerated) + " is outside 1.." + std::to_string(F5LOW_FRAMES_MAX_GENERATED);
        if (((i.flags & F5LOW_FRAMES_FLAG_HDR10) != 0) != IsHdr10(i.colorFormat))
            return "F5LOW_FRAMES_FLAG_HDR10 and colorFormat R10G10B10A2_UNORM go together";
        if ((i.renderWidth != 0) != (i.renderHeight != 0) || i.renderWidth > 16384 || i.renderHeight > 16384)
            return "render size " + std::to_string(i.renderWidth) + "x" + std::to_string(i.renderHeight) + " is not both zero or both in range";
        return {};
    }

    explicit FramesCtx(Params p) : prm(std::move(p))
    {
        dev = prm.dev;
        dev->AddRef();
        W = prm.w;
        H = prm.h;
        RW = prm.rw ? prm.rw : prm.w;
        RH = prm.rh ? prm.rh : prm.h;
        fmt = prm.fmt;
        hdr = IsHdr10(fmt);
        initFlags = prm.initFlags;
        maxGen = prm.maxGen;
    }
    ~FramesCtx() { Shutdown(); }
    FramesCtx(const FramesCtx&) = delete;
    FramesCtx& operator=(const FramesCtx&) = delete;

    // The queue, its lists and fence, then the background build. False: lastErr says why (a D3D12 call failed).
    bool Start()
    {
        char qv[16] = {};
        const bool compute = GetEnvironmentVariableA("F5LOW_FRAMES_QUEUE", qv, sizeof qv) && !_stricmp(qv, "compute");
        const D3D12_COMMAND_LIST_TYPE t = compute ? D3D12_COMMAND_LIST_TYPE_COMPUTE : D3D12_COMMAND_LIST_TYPE_DIRECT;
        listType = t;
        D3D12_COMMAND_QUEUE_DESC qd { t, D3D12_COMMAND_QUEUE_PRIORITY_HIGH };
        bool ok = SUCCEEDED(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)));
        for (auto& a : alloc)
            ok = ok && SUCCEEDED(dev->CreateCommandAllocator(t, IID_PPV_ARGS(&a)));
        ok = ok && SUCCEEDED(dev->CreateCommandList(0, t, alloc[0], nullptr, IID_PPV_ARGS(&list)));
        ok = ok && SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence)));
        ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        if (!ok || !ev)
        {
            lastErr = "the context's command queue, lists or fence could not be made";
            return false;
        }
        q->SetName(L"f5low frames-only");
        list->SetName(L"f5low frames-only list");
        list->Close();
        gen = std::make_unique<OwnGenerator>(dev, prm.packDir, true, gs);
        pg = gen->AsPack();
        XLog("frames: context %ux%u fmt=%d%s, render %ux%u, up to %u generated, init flags 0x%x, own %s queue", W, H, static_cast<int>(fmt),
             hdr ? " (HDR10)" : "", RW, RH, maxGen, initFlags, compute ? "compute" : "direct");
        thread = CreateThread(nullptr, 0, &FramesCtx::BuildEntry, this, 0, nullptr);
        if (!thread)
        {
            lastErr = "the build thread could not be started";
            return false;
        }
        return true;
    }

    F5LowFramesResult BuildStatus()
    {
        const int s = state.load(std::memory_order_acquire);
        return s == kReady ? F5LOW_FRAMES_OK : s == kBuilding ? F5LOW_FRAMES_NOT_READY : failCode;
    }
    const char* LastError() const { return lastErr.c_str(); }

    F5LowFramesResult Generate(const F5LowFramesInput* in, F5LowFramesOutput* out)
    {
        if (!in || !out)
            return Err(F5LOW_FRAMES_ERROR_ARGUMENT, "null input or output");
        if (!in->consumedFence || !out->doneFence)
            return Err(F5LOW_FRAMES_ERROR_ARGUMENT, "consumedFence and doneFence are required");
        out->generated = 0;
        // ---- what can be told without the GPU; any error from here on still signals the fences (Bail) ----
        if (out->count > maxGen)
            return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, "count " + std::to_string(out->count) + " is above maxGenerated " + std::to_string(maxGen));
        for (uint32_t k = 0; k < out->count; ++k)
            if (!out->targets[k])
                return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, "targets[" + std::to_string(k) + "] is null");
        if (!in->color || !in->motion || !in->depth)
            return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, "color, motion and depth are required");
        std::string why;
        if (!CheckColour(in->color, "color", why) || (in->hudless && !CheckColour(in->hudless, "hudless", why)))
            return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, why);
        for (uint32_t k = 0; k < out->count; ++k)
            if (!CheckColour(out->targets[k], "target", why))
                return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, why);
        const D3D12_RESOURCE_DESC md = in->motion->GetDesc(), dd = in->depth->GetDesc();
        uint32_t mKind = 0, mStride = 0, dKind = 0, dStride = 0;
        const Take mTake = MotionTake(md.Format, mKind, mStride), dTake = DepthTake(dd.Format, dKind, dStride);
        if (md.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || md.SampleDesc.Count > 1 || dd.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D ||
            dd.SampleDesc.Count > 1)
            return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, "motion and depth are single-sample 2D textures");
        if (mTake == Take::No)
            return Bail(F5LOW_FRAMES_ERROR_UNSUPPORTED, in, out, std::string("motion format ") + std::to_string(static_cast<int>(md.Format)) + " " + FormatName(md.Format) + " is not supported");
        if (dTake == Take::No)
            return Bail(F5LOW_FRAMES_ERROR_UNSUPPORTED, in, out, std::string("depth format ") + std::to_string(static_cast<int>(dd.Format)) + " " + FormatName(dd.Format) + " is not supported");
        const uint32_t mw = in->motionWidth ? in->motionWidth : static_cast<uint32_t>(md.Width), mh = in->motionHeight ? in->motionHeight : md.Height;
        const uint32_t dw = in->depthWidth ? in->depthWidth : static_cast<uint32_t>(dd.Width), dh = in->depthHeight ? in->depthHeight : dd.Height;
        if (mw > md.Width || mh > md.Height || dw > dd.Width || dh > dd.Height || mw > 16384 || mh > 16384)
            return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, "motion or depth area is larger than its texture");
        if (dw > W || dh > H)
            return Bail(F5LOW_FRAMES_ERROR_ARGUMENT, in, out, "depth area is larger than the display size");

        // ---- not built yet / failed: the fences go through, nothing else ----
        const int s = state.load(std::memory_order_acquire);
        if (s == kBuilding)
        {
            hasPrev = false;
            SignalOnly(in, out);
            return F5LOW_FRAMES_NOT_READY;
        }
        if (s == kFailed)
        {
            SignalOnly(in, out);
            return failCode;
        }
        if (dev->GetDeviceRemovedReason() != S_OK)
            return Bail(F5LOW_FRAMES_ERROR_DEVICE, in, out, "the device was removed");

        // ---- the generator's variant and sizes: another one rebuilds it (waits for the GPU) ----
        uint32_t want = initFlags & ~(XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH | XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV);
        want |= (in->depthInverted ? XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH : 0) | (in->motionJittered ? XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV : 0);
        bool rebuilt = false;
        if (want != initFlags || mw != RW || mh != RH)
        {
            XLog("frames: rebuilding for render %ux%u flags 0x%x (was %ux%u, 0x%x)", mw, mh, want, RW, RH, initFlags);
            const uint32_t oldFlags = initFlags, oldRW = RW, oldRH = RH;
            initFlags = want;
            RW = mw;
            RH = mh;
            if (!Rebuild())
            {
                initFlags = oldFlags;
                RW = oldRW;
                RH = oldRH;
                state.store(kFailed, std::memory_order_release);
                failCode = F5LOW_FRAMES_ERROR_BUILD;
                SignalOnly(in, out);
                return Err(F5LOW_FRAMES_ERROR_BUILD, "the generator could not be rebuilt: " + lastErr);
            }
            rebuilt = true;
        }

        // ---- plan the inputs (a raw buffer may have to grow) ----
        const xefgpack::Resource& mr = pg->InputRes(GenSlot::Motion);
        const xefgpack::Resource& dr = pg->InputRes(GenSlot::Depth);
        Plan mp, dp;
        if (!MakePlan(mp, in->motion, md, mw, mh, mr, mTake, mKind, mStride, true, why) || !MakePlan(dp, in->depth, dd, dw, dh, dr, dTake, dKind, dStride, false, why))
            return Bail(F5LOW_FRAMES_ERROR_UNSUPPORTED, in, out, why);
        if (mp.convert || dp.convert)
        {
            if (!convPso && !convFailed && !MakeConvertPipeline(dev, convRs, convPso))
            {
                convFailed = true;
                XLog("frames: the format conversion pipeline could not be made");
            }
            if (convFailed)
                return Bail(F5LOW_FRAMES_ERROR_UNSUPPORTED, in, out, "motion or depth needs the format conversion, which could not be made");
        }
        const bool bgra = IsBgra(fmt);
        const uint32_t uiMode = in->hudless && pg->HasUiInputs() ? 4u : 1u;
        if (in->hudless && !pg->HasUiInputs() && !warnedHud)
        {
            XLog("frames: hudless given, but the pack lacks the UI inputs - interpolating the finished picture");
            warnedHud = true;
        }
        const bool follows = hasPrev && !rebuilt && !in->reset && in->frameId == lastFrame + 1;
        uint32_t nGen = !follows ? 0u : pg->HasPassPatches() ? out->count : std::min(out->count, 1u);
        for (uint32_t k = 1; k <= nGen; ++k)
            if (!ring[k - 1] && !(ring[k - 1] = PlainBuffer(pg->Buf(GenSlot::Output).bytes, true)))
            {
                nGen = k - 1;
                break;
            }
        ReleaseGraveyard();

        // ---- list 1: the inputs into the pack's buffers ----
        if (in->readyFence)
            q->Wait(in->readyFence, in->readyValue);
        ID3D12GraphicsCommandList* g = Begin();
        if (!g)
            return Bail(F5LOW_FRAMES_ERROR_DEVICE, in, out, "the GPU did not finish an earlier list in time");
        const xefgpack::Resource& ox = pg->Output();
        std::vector<ID3D12Resource*> texs { in->color, in->motion, in->depth };
        if (in->hudless)
            texs.push_back(in->hudless);
        Transition(g, texs, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
        pg->Out(g, pg->Buf(GenSlot::Cur).res, D3D12_RESOURCE_STATE_COPY_DEST);
        CopyPitched(g, fmt, pg->Buf(GenSlot::Cur).res, uiMode == 4 ? in->hudless : in->color, ox, false);
        pg->Back(g, pg->Buf(GenSlot::Cur).res, D3D12_RESOURCE_STATE_COPY_DEST);
        if (uiMode == 4)
        {
            pg->Out(g, pg->Buf(GenSlot::BackBuffer).res, D3D12_RESOURCE_STATE_COPY_DEST);
            CopyPitched(g, fmt, pg->Buf(GenSlot::BackBuffer).res, in->color, ox, false);
            pg->Back(g, pg->Buf(GenSlot::BackBuffer).res, D3D12_RESOURCE_STATE_COPY_DEST);
        }
        RecordInput(g, mp, in->motion, GenSlot::Motion, 0);
        RecordInput(g, dp, in->depth, GenSlot::Depth, 1);
        if (bgra)
            pg->SwapInputsBgra(g, uiMode);
        pg->SetUiMode(uiMode);
        Transition(g, texs, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
        if (!End())
            return Bail(F5LOW_FRAMES_ERROR_DEVICE, in, out, "a command list did not close");
        q->Signal(in->consumedFence, in->consumedValue);

        // ---- the generated frames ----
        float mvs[2] = { in->motionScale[0], in->motionScale[1] };
        if (mvs[0] == 0 && mvs[1] == 0)
            pg->MvScale(mvs[0], mvs[1]);
        const float jc[2] = { in->jitter[0], in->jitter[1] }, jp[2] = { prevJitter[0], prevJitter[1] };
        uint32_t made = 0;
        for (uint32_t k = 1; k <= nGen; ++k)
        {
            pg->PreparePass(k, nGen, jc, jp, mvs);
            g = Begin();
            if (!g)
                break;
            ID3D12Resource* target = out->targets[k - 1];
            pg->RecordPassInto(g, k, ring[k - 1], cur, q, bgra, false);
            Barrier(g, ring[k - 1], D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_SOURCE);
            Barrier(g, target, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            CopyPitched(g, fmt, target, ring[k - 1], ox, true);
            Barrier(g, target, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_COMMON);
            Barrier(g, ring[k - 1], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_COMMON);
            if (!End())
                break;
            ++made;
        }
        if (made < nGen && !warnedShort)
        {
            XLog("frames: only %u of %u generated frames could be made (a list failed)", made, nGen);
            warnedShort = true;
        }
        q->Signal(out->doneFence, out->doneValue);
        out->generated = made;

        // ---- cur -> prev for the next frame ----
        g = Begin();
        if (g)
        {
            RecordCopyToPrev(*pg, g);
            End();
            pg->FrameHandedOff();
        }
        hasPrev = g != nullptr;
        prevJitter[0] = in->jitter[0];
        prevJitter[1] = in->jitter[1];
        lastFrame = in->frameId;
        if (made && (++frames == 1 || frames % 600 == 0))
            XLog("frames: %llu frames generated for %llu app frames", static_cast<unsigned long long>(framesMade += made), static_cast<unsigned long long>(frames));
        else
            framesMade += made;
        return F5LOW_FRAMES_OK;
    }

    // New colour size or format: waits idle, drops the history, rebuilds the generator at the new size.
    F5LowFramesResult Resize(uint32_t w, uint32_t h, DXGI_FORMAT f, uint32_t flags)
    {
        F5LowFramesCreateInfo ci {};
        ci.apiVersion = F5LOW_FRAMES_API_VERSION;
        ci.device = dev;
        ci.width = w;
        ci.height = h;
        ci.colorFormat = f;
        ci.maxGenerated = maxGen;
        ci.flags = flags;
        if (const std::string e = CheckCreate(ci); !e.empty())
            return Err(F5LOW_FRAMES_ERROR_ARGUMENT, e);
        JoinBuild();
        if (state.load() == kFailed)
            return failCode;
        WaitIdle();
        const uint32_t oW = W, oH = H, oRW = RW, oRH = RH;
        const DXGI_FORMAT oF = fmt;
        const bool oHdr = hdr;
        const bool sameRender = RW == W && RH == H;
        W = w;
        H = h;
        fmt = f;
        hdr = IsHdr10(f);
        if (sameRender)
        {
            RW = w;
            RH = h;
        }
        if (!Rebuild())
        {
            W = oW;
            H = oH;
            RW = oRW;
            RH = oRH;
            fmt = oF;
            hdr = oHdr;
            state.store(kFailed, std::memory_order_release);
            failCode = F5LOW_FRAMES_ERROR_BUILD;
            return Err(F5LOW_FRAMES_ERROR_BUILD, "the generator could not be rebuilt: " + lastErr);
        }
        return F5LOW_FRAMES_OK;
    }

    void Shutdown()
    {
        if (shutDone)
            return;
        shutDone = true;
        quit = true;
        JoinBuild();
        if (q)
            WaitIdle();
        if (gen)
            gen->Shutdown();
        gen.reset();
        pg = nullptr;
        ReleaseStaging();
        for (IUnknown* u : std::initializer_list<IUnknown*> { convPso, convRs, list, fence, q })
            if (u)
                u->Release();
        for (auto& a : alloc)
            if (a)
                a->Release();
        convPso = nullptr;
        convRs = nullptr;
        list = nullptr;
        fence = nullptr;
        q = nullptr;
        if (ev)
            CloseHandle(ev);
        ev = nullptr;
        if (dev)
            dev->Release();
        dev = nullptr;
    }

private:
    enum { kBuilding = 0, kReady = 1, kFailed = 2 };

    // One motion / depth input's route (CopyIn in xefg_own.h decides the same things for a tag).
    struct Plan
    {
        bool convert = false;
        Conversion conv;
        D3D12_PLACED_SUBRESOURCE_FOOTPRINT fp {};   // the copy's footprint (into the pack's buffer, or into the raw buffer when converting)
        bool whole = true;
        D3D12_BOX box {};
        ID3D12Resource* raw = nullptr;
    };

    F5LowFramesResult Err(F5LowFramesResult r, const std::string& why)
    {
        if (why != lastErr)
            XLog("frames: %s", why.c_str());
        lastErr = why;
        return r;
    }
    // An error after the pointers were checked: the fences still go through.
    F5LowFramesResult Bail(F5LowFramesResult r, const F5LowFramesInput* in, F5LowFramesOutput* out, const std::string& why)
    {
        SignalOnly(in, out);
        return Err(r, why);
    }
    void SignalOnly(const F5LowFramesInput* in, F5LowFramesOutput* out)
    {
        if (!q)
            return;
        if (in->readyFence)
            q->Wait(in->readyFence, in->readyValue);
        q->Signal(in->consumedFence, in->consumedValue);
        q->Signal(out->doneFence, out->doneValue);
        out->generated = 0;
    }

    bool CheckColour(ID3D12Resource* r, const char* what, std::string& why)
    {
        const D3D12_RESOURCE_DESC d = r->GetDesc();
        if (d.Dimension != D3D12_RESOURCE_DIMENSION_TEXTURE2D || d.SampleDesc.Count > 1 || d.Width != W || d.Height != H ||
            ColourFamily(d.Format) != ColourFamily(fmt))
        {
            why = std::string(what) + " is " + std::to_string(d.Width) + "x" + std::to_string(d.Height) + " fmt " + std::to_string(static_cast<int>(d.Format)) +
                  ", the context takes a single-sample " + std::to_string(W) + "x" + std::to_string(H) + " texture of the colour format's family";
            return false;
        }
        return true;
    }

    bool MakePlan(Plan& p, ID3D12Resource* tex, const D3D12_RESOURCE_DESC& d, uint32_t sw, uint32_t sh, const xefgpack::Resource& pr, Take take, uint32_t kind,
                  uint32_t stride, bool isMv, std::string& why)
    {
        bool convert = take == Take::Convert && pr.texel == 4;
        p.whole = sw == d.Width && sh == d.Height;
        // a depth-stencil texture is copied only whole: a region of one goes the convert path (Kind 5, the bits as they are)
        if (!convert && !isMv && !p.whole && (d.Flags & D3D12_RESOURCE_FLAG_ALLOW_DEPTH_STENCIL) && pr.texel == 4 && take == Take::Raw)
        {
            convert = true;
            kind = 5;
            stride = 4;
        }
        const uint32_t tb = convert ? pr.texel : take == Take::Raw ? 4u : 0u;
        const bool fits = isMv ? sw == pr.w && sh == pr.h : sw <= pr.w && sh <= pr.h;
        const char* what = isMv ? "motion" : "depth";
        if (!tb || tb != pr.texel || !fits)
        {
            why = std::string(what) + " area " + std::to_string(sw) + "x" + std::to_string(sh) + " fmt " + std::to_string(static_cast<int>(d.Format)) +
                  " does not fit the generator's " + std::to_string(pr.w) + "x" + std::to_string(pr.h) + " texel " + std::to_string(pr.texel);
            return false;
        }
        UINT64 total = 0;
        dev->GetCopyableFootprints(&d, 0, 1, 0, &p.fp, nullptr, nullptr, &total);
        p.fp.Offset = 0;
        p.fp.Footprint.Width = sw;
        p.fp.Footprint.Height = sh;
        p.fp.Footprint.RowPitch = static_cast<UINT>(pr.pitch);
        p.box = { 0, 0, 0, sw, sh, 1 };
        p.convert = convert;
        if (convert)
        {
            // the whole texture at its own texel size into the raw buffer; the shader reads the region at its offset (0, 0 here)
            const UINT tw = static_cast<UINT>(d.Width);
            p.fp.Footprint.Width = tw;
            p.fp.Footprint.Height = d.Height;
            p.fp.Footprint.RowPitch = (tw * stride + D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1) & ~(D3D12_TEXTURE_DATA_PITCH_ALIGNMENT - 1);
            const size_t need = static_cast<size_t>(p.fp.Footprint.RowPitch) * d.Height;
            const int ci = isMv ? 0 : 1;
            if (rawBytes[ci] < need)
            {
                if (raw[ci])
                    graveyard.push_back({ fv, raw[ci] });       // a list in flight may still read it
                raw[ci] = PlainBuffer(need);
                rawBytes[ci] = raw[ci] ? need : 0;
            }
            if (!raw[ci])
            {
                why = std::string("no ") + std::to_string(need) + "-byte buffer for the raw copy of " + what;
                return false;
            }
            p.raw = raw[ci];
            p.conv = { static_cast<int32_t>(kind), sw, sh, p.fp.Footprint.RowPitch, static_cast<uint32_t>(pr.pitch), stride, 0, 0, raw[ci] };
        }
        (void)tex;
        return true;
    }

    // The planned copy of motion / depth into the pack's buffer (list 1; the texture is in COPY_SOURCE already).
    void RecordInput(ID3D12GraphicsCommandList* g, const Plan& p, ID3D12Resource* tex, GenSlot slot, int)
    {
        ID3D12Resource* dst = pg->Buf(slot).res;
        D3D12_TEXTURE_COPY_LOCATION s { tex, D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX }, t { p.convert ? p.raw : dst, D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT };
        s.SubresourceIndex = 0;
        t.PlacedFootprint = p.fp;
        if (p.convert)
        {
            Barrier(g, p.raw, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST);
            g->CopyTextureRegion(&t, 0, 0, 0, &s, nullptr);
            Barrier(g, p.raw, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
            RecordConvert(g, convRs, convPso, p.conv, p.raw, dst);      // dst rests in UNORDERED_ACCESS (the pack's own state)
            D3D12_RESOURCE_BARRIER u {};
            u.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
            u.UAV.pResource = dst;
            g->ResourceBarrier(1, &u);
            Barrier(g, p.raw, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COMMON);
        }
        else
        {
            pg->Out(g, dst, D3D12_RESOURCE_STATE_COPY_DEST);
            g->CopyTextureRegion(&t, 0, 0, 0, &s, p.whole ? nullptr : &p.box);
            pg->Back(g, dst, D3D12_RESOURCE_STATE_COPY_DEST);
        }
    }

    static void Barrier(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) { GenBarrier(g, r, a, b); }
    static void Transition(ID3D12GraphicsCommandList* g, const std::vector<ID3D12Resource*>& rs, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        std::vector<ID3D12Resource*> done;
        for (ID3D12Resource* r : rs)       // one barrier per resource (a texture given twice is moved once)
        {
            if (std::find(done.begin(), done.end(), r) != done.end())
                continue;
            done.push_back(r);
            GenBarrier(g, r, a, b);
        }
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

    // One of our lists, opened; null when its allocator's last use did not finish within the bound.
    ID3D12GraphicsCommandList* Begin()
    {
        const size_t i = next++ % kAllocs;
        if (fence->GetCompletedValue() < allocFence[i] && !WaitFence(allocFence[i], 5000))
        {
            lastErr = "the GPU did not finish an earlier list in 5 s";
            XLog("frames: %s", lastErr.c_str());
            return nullptr;
        }
        pg->ReadStamps(i);
        alloc[i]->Reset();
        list->Reset(alloc[i], nullptr);
        cur = i;
        return list;
    }
    // false: the list failed to close and was not executed
    bool End()
    {
        if (FAILED(list->Close()))
        {
            lastErr = "a command list did not close";
            XLog("frames: %s", lastErr.c_str());
            return false;
        }
        ID3D12CommandList* l = list;
        q->ExecuteCommandLists(1, &l);
        q->Signal(fence, ++fv);
        allocFence[cur] = fv;
        return true;
    }
    bool WaitFence(UINT64 v, DWORD ms)
    {
        if (fence->GetCompletedValue() >= v)
            return true;
        fence->SetEventOnCompletion(v, ev);
        return WaitForSingleObject(ev, ms) == WAIT_OBJECT_0;
    }
    void WaitIdle()
    {
        if (!q || !fence)
            return;
        q->Signal(fence, ++fv);
        if (!WaitFence(fv, 10000))
            XLog("frames: the GPU did not go idle in 10 s");
    }
    void ReleaseGraveyard()
    {
        const UINT64 done = fence->GetCompletedValue();
        for (size_t i = 0; i < graveyard.size();)
            if (graveyard[i].first <= done)
            {
                graveyard[i].second->Release();
                graveyard.erase(graveyard.begin() + static_cast<std::ptrdiff_t>(i));
            }
            else
                ++i;
    }

    // What the pack's sizes were made for goes (the old pack is gone, the GPU is idle): the generator calls this at a build.
    void ReleaseStaging()
    {
        for (ID3D12Resource*& r : ring)
        {
            if (r)
                r->Release();
            r = nullptr;
        }
        for (int i = 0; i < 2; ++i)
        {
            if (raw[i])
                raw[i]->Release();
            raw[i] = nullptr;
            rawBytes[i] = 0;
        }
        for (auto& g : graveyard)
            g.second->Release();
        graveyard.clear();
        hasPrev = false;
    }

    GenBuild BuildArgs()
    {
        GenBuild b;
        b.initFlags = initFlags;
        b.hdr = hdr;
        b.releaseHostBuffers = [this] { ReleaseStaging(); };
        b.cancelled = [this] { return quit.load(); };
        b.packBuilt = [this] { return PackBuilt(); };
        return b;
    }
    bool PackBuilt()
    {
        const xefgpack::Sizes& z = pg->PackSize();
        if (z.OW != W || z.OH != H)
        {
            lastErr = "the pack was built for " + std::to_string(z.OW) + "x" + std::to_string(z.OH) + ", not " + std::to_string(W) + "x" + std::to_string(H);
            XLog("frames: %s", lastErr.c_str());
            return false;
        }
        XLog("frames: built: pack %s%s, %zu dispatches, output %ux%u, render %ux%u, flags 0x%x, UI inputs %s, per-pass patches %s", prm.packDir.c_str(),
             pg->PackHdr() ? "\\hdr" : "", pg->DispatchCount(), z.OW, z.OH, z.RW, z.RH, initFlags, pg->HasUiInputs() ? "yes" : "no",
             pg->HasPassPatches() ? "yes" : "NO (one generated frame only)");
        return true;
    }

    // The generator for the sizes / flags / format now set (WaitIdle first: nothing may be using the old pack). History dropped.
    bool Rebuild()
    {
        JoinBuild();
        WaitIdle();
        const xefgpack::Sizes want { W, H, RW, RH, 0, 0, 0, 0 };
        const bool ok = gen->Rebuild(want, BuildArgs());
        hasPrev = false;
        if (!ok)
        {
            if (lastErr.empty())
                lastErr = "the pack did not build (see the log)";
            return false;
        }
        return true;
    }

    static DWORD WINAPI BuildEntry(void* p)
    {
        static_cast<FramesCtx*>(p)->BuildThread();
        return 0;
    }
    void BuildThread()
    {
        const long long t0 = Now();
        std::string why;
        int st;
        while ((st = prm.packPoll ? prm.packPoll(&why) : 1) == 0 && !quit)
            Sleep(2);
        bool ok = false;
        F5LowFramesResult code = F5LOW_FRAMES_ERROR_BUILD;
        if (quit)
            why = "context destroyed";
        else if (st != 1)
            ;       // the pack could not be made: why is the DLL's reason
        else if (const std::string misfit = prm.packFit ? prm.packFit() : std::string(); !misfit.empty())
        {
            why = misfit;
            code = F5LOW_FRAMES_ERROR_UNSUPPORTED;
        }
        else
        {
            GenBuild args = BuildArgs();
            const xefgpack::Sizes want { W, H, RW, RH, 0, 0, 0, 0 };
            args.firstSizes = &want;
            if (!(ok = gen->Build(args)))
                why = quit ? "context destroyed" : lastErr.empty() ? "the pack did not build for D3D12 (see the log)" : lastErr;
        }
        if (ok)
            XLog("frames: generator ready %.0f ms after the context was made", static_cast<double>(Now() - t0) * 1000.0 / Freq());
        else
        {
            XLog("frames: generator FAILED: %s", why.c_str());
            lastErr = why;
            failCode = code;
        }
        state.store(ok ? kReady : kFailed, std::memory_order_release);
    }
    void JoinBuild()
    {
        if (thread)
        {
            WaitForSingleObject(thread, INFINITE);
            CloseHandle(thread);
            thread = nullptr;
        }
    }
    static long long Now()
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        return t.QuadPart;
    }
    static double Freq()
    {
        static const double f = [] { LARGE_INTEGER x; QueryPerformanceFrequency(&x); return static_cast<double>(x.QuadPart); }();
        return f;
    }

    Params prm;
    ID3D12Device* dev = nullptr;
    uint32_t W = 0, H = 0, RW = 0, RH = 0, maxGen = 0, initFlags = 0;
    DXGI_FORMAT fmt = DXGI_FORMAT_UNKNOWN;
    bool hdr = false;
    GenSettings gs;
    std::unique_ptr<OwnGenerator> gen;
    IPackGenerator* pg = nullptr;
    std::atomic<int> state { kBuilding };
    F5LowFramesResult failCode = F5LOW_FRAMES_ERROR_BUILD;     // written before state = kFailed
    std::string lastErr;
    std::atomic<bool> quit { false };
    HANDLE thread = nullptr;
    bool shutDone = false;

    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_LIST_TYPE listType = D3D12_COMMAND_LIST_TYPE_DIRECT;
    ID3D12CommandAllocator* alloc[kAllocs] {};
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    HANDLE ev = nullptr;
    UINT64 fv = 0, allocFence[kAllocs] {};
    size_t next = 0, cur = 0;

    ID3D12Resource* ring[kMaxInterp] {};        // one generated frame each, in the pack's layout (plain buffers, COMMON between lists)
    ID3D12Resource* raw[2] {};                  // motion, depth: the raw copy a conversion reads (COMMON between lists)
    size_t rawBytes[2] {};
    std::vector<std::pair<UINT64, ID3D12Resource*>> graveyard;     // outgrown raw buffers, released once the queue passed their fence value
    ID3D12RootSignature* convRs = nullptr;
    ID3D12PipelineState* convPso = nullptr;
    bool convFailed = false;

    bool hasPrev = false;
    uint64_t lastFrame = 0, frames = 0, framesMade = 0;
    float prevJitter[2] {};
    bool warnedHud = false, warnedShort = false;
};

} // namespace xefgown
