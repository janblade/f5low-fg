// What the own generator tells NVIDIA's driver about its queues and presents (public nvapi.h: NvAPI_D3D12_NotifyOutOfBandCommandQueue,
// NvAPI_D3D12_SetAsyncFrameMarker, NvAPI_D3D_SetReflexSync), so Reflex can tell the generator's work and flips from the game's frames.
// Off by default; DriverPacing=2 in f5low.ini turns the set on for good (xefg_own.h, DriverFgInit, DriverFgFrame and RealPresent),
// DriverPacing=1 lets PacingRule below decide while the game runs, the low-level keys OobQueue, OobMarkers, DriverFg and DriverFgParts do
// it piece by piece. No Windows in here: tested on the host (tests/oob_driver_test.cpp).
#pragma once

#include <cstdint>

namespace oobdriver
{
// The NVAPI interface ids (public nvapi.h, nvapi_interface.h) the own generator looks up through nvapi_QueryInterface, each once per use.
inline constexpr uint32_t kIdSetFlipConfig = 0xF3148C42u;                  // NvAPI_D3D12_SetFlipConfig
inline constexpr uint32_t kIdNotifyOutOfBandCommandQueue = 0x03D6E8CBu;    // NvAPI_D3D12_NotifyOutOfBandCommandQueue
inline constexpr uint32_t kIdSetAsyncFrameMarker = 0x13C98F73u;            // NvAPI_D3D12_SetAsyncFrameMarker
inline constexpr uint32_t kIdSetReflexSync = 0xB9F6FAFFu;                  // NvAPI_D3D_SetReflexSync

// NV_OUT_OF_BAND_CQ_TYPE
enum QueueType : int
{
    kRender = 0,
    kPresent = 1,
    kIgnore = 2,
    kRenderPresent = 3,
    kLeaveAlone = -1,       // not a value of the API: no notice for this queue
};

// NV_LATENCY_MARKER_TYPE
inline constexpr int kPresentStart = 4, kPresentEnd = 5, kOobRenderStart = 9, kOobRenderEnd = 10, kOobPresentStart = 11, kOobPresentEnd = 12;

// NV_SET_REFLEX_SYNC_PARAMS_V1: DriverFg only fills the frame generation multiplier
struct ReflexSync
{
    uint32_t version;
    uint32_t flags;
    uint32_t vblankIntervalUs;
    int32_t timeInQueueUs;
    uint32_t timeInQueueUsTarget;
    uint8_t fgMultiplier;
    uint8_t reserved[27];
};
static_assert(sizeof(ReflexSync) == 48, "layout of NV_SET_REFLEX_SYNC_PARAMS_V1");
inline constexpr uint32_t kReflexSyncVer = static_cast<uint32_t>(sizeof(ReflexSync)) | (1u << 16);

// NV_ASYNC_FRAME_MARKER_PARAMS_V1
struct AsyncMarker
{
    uint32_t version;
    uint64_t frameId;
    int markerType;
    uint64_t presentFrameId;
    uint8_t vendorInternal;
    uint8_t reserved[55];
};
static_assert(sizeof(AsyncMarker) == 88, "layout of NV_ASYNC_FRAME_MARKER_PARAMS_V1");
inline constexpr uint32_t kAsyncMarkerVer = static_cast<uint32_t>(sizeof(AsyncMarker)) | (1u << 16);

// A queue of ours. The application's queue never gets a notice: its frames are the in-band work the driver paces.
// hasPresents: the real swap chain presents here. hasWork: generation or copies run here.
inline QueueType TypeFor(bool isApplicationQueue, bool hasPresents, bool hasWork)
{
    if (isApplicationQueue)
        return kLeaveAlone;
    if (hasPresents)
        return hasWork ? kRenderPresent : kPresent;
    return hasWork ? kRender : kLeaveAlone;
}

// Frame id 0 is what the application's Present carries when nobody set one: no marker then.
inline bool MarkFrame(uint32_t frameId) { return frameId != 0; }

// DriverPacing (xefg_own.h, PresentReal): a present after a batch's first goes out at once (it is not waited for, only for its own GPU
// work) when the burst is on and the NVIDIA driver meters the batch's flips. The batch's first present (k == 1) keeps its time. The
// schedule's Bound measures from the end of the last burst and clamps the next batch's first present to between half a step and two
// steps after it: at 3x or more it normally goes about two steps after the burst's end, or when it is ready if that is later, not on the
// smoothed schedule. Holding it to the schedule was measured worse in one game.
inline bool BurstNow(bool burstOn, uint32_t k, bool metered) { return burstOn && k > 1 && metered; }

// Whether the presenter asks the NVIDIA driver to meter a batch's flips (xefg_own.h, InitFlip). The spacing is done in the display
// hardware of GeForce RTX 50 (Blackwell, sm_100 and newer); on older GPUs the driver takes the call (status 0) but nothing spaces the
// flips, so frames released early or in a burst reach the screen together. An RTX 2070 SUPER at 6x (tester logs, 2026-11-07/08) showed
// the batch's presents 12-15 ms apart in spread with DriverPacing=2 and 52-65 ms gaps between batches. FlipMetering= in f5low.ini:
// "0"/"off"/"false"/"no" off, "1"/"on"/"true"/"yes" on whatever the GPU (for tests), "auto" (the default) on from sm_100. Any other
// value counts as auto (the caller logs it). Case and spaces around the value don't matter. gpuSm: sm_NN, 0 or less unknown.
inline constexpr int kFlipMeteringMinSm = 100;
// -1 auto, 0 off, 1 on. *known = false for a value that is none of the words above (it is taken as auto).
inline int FlipMeteringForced(const char* setting, bool* known = nullptr)
{
    char v[8] = {};
    size_t n = 0;
    bool tooLong = false;
    if (setting)
    {
        while (*setting == ' ' || *setting == '\t')
            ++setting;
        for (; *setting; ++setting)
        {
            if (n + 1 >= sizeof(v))
            {
                tooLong = true;
                break;
            }
            v[n++] = (*setting >= 'A' && *setting <= 'Z') ? static_cast<char>(*setting - 'A' + 'a') : *setting;
        }
        while (n && (v[n - 1] == ' ' || v[n - 1] == '\t' || v[n - 1] == '\r' || v[n - 1] == '\n'))
            v[--n] = '\0';
    }
    auto is = [&](const char* s) {
        if (tooLong)
            return false;
        size_t i = 0;
        for (; s[i] && v[i] == s[i]; ++i) {}
        return s[i] == '\0' && v[i] == '\0';
    };
    if (known)
        *known = true;
    if (is("0") || is("off") || is("false") || is("no"))
        return 0;
    if (is("1") || is("on") || is("true") || is("yes"))
        return 1;
    if (known)
        *known = !tooLong && (n == 0 || is("auto"));
    return -1;
}
inline bool FlipMeteringOn(const char* setting, int gpuSm)
{
    const int forced = FlipMeteringForced(setting);
    return forced >= 0 ? forced == 1 : gpuSm >= kFlipMeteringMinSm;
}

// When DriverPacing=1 pays (xefg_own.h, PacingFrame). Measured in one game (NBA 2K27) with PresentMon: with the GPU at its limit the signals
// and the burst cut the game's sim-to-screen time by about half; with the GPU idle part of the time (a frame cap) they gain nothing and
// space the frames less evenly. So they run only while the GPU is the limit.
// load: the GPU's share of the app interval, smoothed: how long after the game's Present call its GPU work finished, over the app's
// interval. It is above 1 when the game is waiting for the GPU, and the signals themselves bring it down (1.85 -> 0.84 at 4x in the game),
// so the leave threshold sits well below the enter one. The rule holds each side for a while and each state for a dwell time.
inline constexpr double kPacingEnterLoad = 0.75, kPacingLeaveLoad = 0.55;
inline constexpr double kPacingHoldMs = 2000.0, kPacingDwellMs = 10000.0;
inline constexpr double kPacingTransientMs = 1000.0;    // a transient cause must last this long, without a break, to count
inline constexpr double kPacingGapMs = 500.0;           // no update for this long: the holds start over
inline constexpr double kPacingMaxShare = 3.0;          // one frame's load share is clamped to this (a stall must not move the average far)

// The GPU's share of the app interval for one frame: its GPU work's arrival-to-ready time over the interval, between 0 and 3. -1: no
// usable interval yet.
inline double LoadShare(double arriveToReadyMs, double intervalMs)
{
    if (!(intervalMs > 0.5))
        return -1.0;
    const double s = (arriveToReadyMs > 0.0 ? arriveToReadyMs : 0.0) / intervalMs;
    return s < kPacingMaxShare ? s : kPacingMaxShare;
}

// Why the set may not run now. kHard: V-Sync or a display paced game, frame generation off, a swap chain the game made, no flip metering to
// be had - it goes off at once. kTransient: a history reset, the metering refilling, a one-batch dropout - it only counts once it has
// lasted kPacingTransientMs without a break (a few frames of it must not drop the set and then hold it off for the dwell time).
enum class Eligibility : int { kEligible, kTransient, kHard };
struct PacingRule
{
    bool on = false;
    double aboveMs = -1;                // when load rose to the enter threshold (and stayed there); -1: not above
    double belowMs = -1;                // when load fell to the leave threshold; -1: not below
    double lastChangeMs = -1e9;
    double transientMs = -1;            // when the current run of transient causes began; -1: none
    double lastUpdateMs = -1e9;

    // Returns the new state. A hard cause turns it off at once, and that counts as a change; while off nothing is held. An update more than
    // kPacingGapMs after the last one starts the holds over (the frames in between were not seen).
    bool Update(double load, double nowMs, Eligibility e)
    {
        if (nowMs - lastUpdateMs > kPacingGapMs)
            aboveMs = belowMs = transientMs = -1;
        lastUpdateMs = nowMs;
        if (e == Eligibility::kTransient)
        {
            if (transientMs < 0)
                transientMs = nowMs;
            if (nowMs - transientMs >= kPacingTransientMs)
                e = Eligibility::kHard;
            else
            {
                aboveMs = belowMs = -1;     // nothing is held while it lasts; the state stays
                return on;
            }
        }
        else
            transientMs = -1;
        if (e == Eligibility::kHard)
        {
            if (on)
            {
                on = false;
                lastChangeMs = nowMs;
            }
            aboveMs = belowMs = -1;
            return on;
        }
        if (load >= kPacingEnterLoad)
        {
            belowMs = -1;
            if (aboveMs < 0)
                aboveMs = nowMs;
        }
        else if (load <= kPacingLeaveLoad)
        {
            aboveMs = -1;
            if (belowMs < 0)
                belowMs = nowMs;
        }
        else
            aboveMs = belowMs = -1;     // between the thresholds: neither side is being held
        const bool dwelled = nowMs - lastChangeMs >= kPacingDwellMs;
        if (!on && aboveMs >= 0 && nowMs - aboveMs >= kPacingHoldMs && dwelled)
        {
            on = true;
            lastChangeMs = nowMs;
            aboveMs = belowMs = -1;
        }
        else if (on && belowMs >= 0 && nowMs - belowMs >= kPacingHoldMs && dwelled)
        {
            on = false;
            lastChangeMs = nowMs;
            aboveMs = belowMs = -1;
        }
        return on;
    }
    bool Update(double load, double nowMs, bool eligible) { return Update(load, nowMs, eligible ? Eligibility::kEligible : Eligibility::kHard); }
};

// The order of a switch of the driver signals (xefg_own.h, PacingStep carries out the actions), once per app frame:
//   off -> arming: the cap export goes up first (OptiScaler then gives Reflex the shown-frame cap undivided; nothing is sent to the driver
//     yet, so the game runs faster than its cap, never slower); arming -> on after kPacingSettleFrames app frames: the markers, the burst and
//     Reflex Sync with the frame multiplier start. Arming -> off (the want went away): the export comes down again.
//   on -> disarming: the signals stop and Reflex Sync gets multiplier 1 at once; the export comes down only after kPacingSettleFrames app
//     frames and after the driver took the 1. A refused 1 is sent again each frame; after kPacingGiveUpRefusals refusals the export comes
//     down anyway (gaveUp: the state cannot be repaired by waiting). Disarming -> on (the want came back): the export is still up.
// The caller reports each send's outcome with SendResult.
enum class PacingState : int { kOff, kArming, kOn, kDisarming };
inline constexpr uint64_t kPacingSettleFrames = 30, kPacingGiveUpRefusals = 120;
inline constexpr uint32_t kPacingRefusalsMax = 8;       // consecutive refused Reflex Syncs after which automatic pacing gives up
struct PacingActions
{
    bool raiseExport = false, signalsOn = false, signalsOff = false, sendOne = false, lowerExport = false, gaveUp = false;
};
struct PacingMachine
{
    PacingState state = PacingState::kOff;
    uint64_t at = 0, refused = 0;
    bool sendPending = false;
    PacingActions Step(bool want, uint64_t frames)
    {
        PacingActions a;
        switch (state)
        {
        case PacingState::kOff:
            if (want)
            {
                state = PacingState::kArming;
                at = frames;
                a.raiseExport = true;
            }
            break;
        case PacingState::kArming:
            if (!want)
            {
                state = PacingState::kOff;
                a.lowerExport = true;
            }
            else if (frames - at >= kPacingSettleFrames)
            {
                state = PacingState::kOn;
                a.signalsOn = true;
            }
            break;
        case PacingState::kOn:
            if (!want)
            {
                state = PacingState::kDisarming;
                at = frames;
                refused = 0;
                sendPending = true;
                a.signalsOff = true;
                a.sendOne = true;
            }
            break;
        case PacingState::kDisarming:
            if (want)
            {
                state = PacingState::kOn;
                sendPending = false;
                a.signalsOn = true;
            }
            else if (sendPending)
            {
                if (refused >= kPacingGiveUpRefusals)
                {
                    state = PacingState::kOff;
                    sendPending = false;
                    a.lowerExport = true;
                    a.gaveUp = true;
                }
                else
                    a.sendOne = true;
            }
            else if (frames - at >= kPacingSettleFrames)
            {
                state = PacingState::kOff;
                a.lowerExport = true;
            }
            break;
        }
        return a;
    }
    void SendResult(bool ok)
    {
        if (ok)
            sendPending = false;
        else
            ++refused;
    }
};

// The real swap chain's frame latency tokens (the waitable object, limit 2 at all times) under the burst. A burst present takes its token
// without blocking; when none is there it goes out anyway and its retirement still releases one, a token the chain would hand out on top
// of the limit. TokenDebt counts those presents and pays them back: a burst present takes a spare token without blocking while it owes,
// a present outside a burst waits for one (one extra blocking wait; on a timeout nothing is in flight to wait for, so the debt is
// forgiven and the present's own wait is skipped too: a chain whose presents never retire costs 100 ms a present, not 200). Probed on a
// host swap chain (flag, limit 2): without it the surplus grew to 21 tokens over three bursts, with it 2.
// Presenter thread only. Not negative, ever.
struct TokenDebt
{
    uint32_t owed = 0;
    void Missed() { ++owed; }                       // a present went out without a token (wait(0) failed, or a blocking wait timed out)
    bool Owes() const { return owed > 0; }
    // A token was taken while owing (tookToken: the wait succeeded): one less owed. False when nothing was owed or no token came.
    bool Repay(bool tookToken)
    {
        if (!tookToken || owed == 0)
            return false;
        --owed;
        return true;
    }
    void Forgive() { owed = 0; }                    // a timeout while paying back, or the chain's handle is new or gone
    // The extra blocking wait of a present outside a burst, while owing: success repays one; a timeout forgives it all. Returns whether the
    // present's own blocking wait follows (not after a timeout: it would only wait the 100 ms again, and add a debt of its own).
    bool PaidBack(bool tookToken)
    {
        if (tookToken)
        {
            Repay(true);
            return true;
        }
        Forgive();
        return false;
    }
};

// The settings after the host handshake, the GPU vendor, Gate, DriverPacing and the low-level keys are resolved (xefg_cuda.cpp, once per
// context, before the swap chain is made). driverPacing: -1 when the key is absent (no ini key, no environment variable), else 0, 1 or 2.
//   absent: automatic (1) when the host says it routes these markers (hostRoutes, the export f5lowHostRoutesMarkers), else off: an
//     OptiScaler without that routing takes the async frame markers for the game's own Reflex calls.
//   0 off. 1 automatic, 2 always on: honoured as asked; on a host that did not say it routes the markers it is warned about (oldHostWarn).
//   No NVIDIA compute device for the adapter (nvidia false; xefg_cuda.cpp passes have > 0, the CUDA device matched by LUID): the signals, the burst and the mode are all off,
//     explicit 1 and 2 and the low-level keys included, so the chain is the plain one of today.
//   Gate=1 paces on the GPU through a present lane of its own, so it vetoes both the burst and DriverPacing.
//   No flip metering (metering false: FlipMeteringOn says no, as on GPUs before RTX 50): DriverPacing and FlipBurst are off, explicit 1
//     and 2 included. The burst leans on metering, and the driver signals tell Reflex the driver spaces the generated flips, which
//     nothing does there; the presenter's own clock times every frame instead. (NBA 2K27, FlipMetering=0 with DriverPacing=2: the
//     Reflex Sync multiplier flapped 1 <-> 6 and the on-screen spread went from 1.75 to 2.39 ms.) The low-level DriverFg key stays.
// DriverPacing 1 or 2 sets the driver signals (all four parts) and the burst; with 1 they run only while PacingRule says so (pacingAuto),
// with 2 always.
enum class PacingWhy : int { kExplicitOff, kExplicit, kDefaultAuto, kDefaultOffOldHost, kGateVeto, kNotNvidia, kNoMetering };
struct DriverSettings
{
    bool driverFg;
    uint32_t parts;
    bool flipBurst;
    bool pacingIgnored;     // DriverPacing 1 or 2 was asked for (explicitly) and Gate=1 or a non-NVIDIA GPU vetoed it
    bool burstIgnored;      // FlipBurst was asked for and Gate=1 vetoed it
    bool pacingAuto;        // mode 1: the signals and the burst start off and PacingRule switches them
    int mode = 0;           // the resolved DriverPacing: 0, 1 or 2
    PacingWhy why = PacingWhy::kExplicitOff;
    bool oldHostWarn = false;       // explicit 1 or 2, and the host did not say it routes the markers
    bool nvidiaOff = false;         // something was asked for (a mode or a low-level key) and the GPU is not NVIDIA
};
inline DriverSettings ResolveDriver(bool gate, int driverPacing, bool driverFg, uint32_t parts, bool flipBurst, bool hostRoutes = true,
                                    bool nvidia = true, bool metering = true)
{
    DriverSettings r { driverFg, parts, flipBurst, false, false, false };
    const bool absent = driverPacing < 0;
    const int ask = absent ? (hostRoutes ? 1 : 0) : driverPacing;
    r.why = absent ? (hostRoutes ? PacingWhy::kDefaultAuto : PacingWhy::kDefaultOffOldHost)
                   : (driverPacing == 0 ? PacingWhy::kExplicitOff : PacingWhy::kExplicit);
    if (!nvidia)
    {
        r.nvidiaOff = ask != 0 || driverFg || flipBurst;
        r.pacingIgnored = !absent && driverPacing > 0;
        r.burstIgnored = false;
        r.driverFg = false;
        r.flipBurst = false;
        r.mode = 0;
        if (r.nvidiaOff)
            r.why = PacingWhy::kNotNvidia;
        return r;
    }
    if (gate)
    {
        r.burstIgnored = flipBurst;
        r.flipBurst = false;
        r.pacingIgnored = !absent && driverPacing > 0;
        if (ask != 0)
            r.why = PacingWhy::kGateVeto;
        return r;
    }
    if (!metering)
    {
        r.burstIgnored = flipBurst;
        r.flipBurst = false;
        r.pacingIgnored = !absent && driverPacing > 0;
        if (ask != 0)
            r.why = PacingWhy::kNoMetering;
        return r;
    }
    if (ask != 0)
    {
        r.driverFg = true;
        r.parts = 15;
        r.flipBurst = true;             // the 8-buffer chain and the metering stay for both modes
        r.pacingAuto = ask == 1;
        r.oldHostWarn = !absent && !hostRoutes;
    }
    r.mode = ask;
    return r;
}
} // namespace oobdriver
