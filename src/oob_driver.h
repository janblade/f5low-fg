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

// When DriverPacing=1 pays (xefg_own.h, PacingFrame). Measured in one game (NBA 2K27) with PresentMon: with the GPU at its limit the signals
// and the burst cut the game's sim-to-screen time by about half; with the GPU idle part of the time (a frame cap) they gain nothing and
// space the frames less evenly. So they run only while the GPU is the limit.
// load: the GPU's share of the app interval, smoothed: how long after the game's Present call its GPU work finished, over the app's
// interval. It is above 1 when the game is waiting for the GPU, and the signals themselves bring it down (1.85 -> 0.84 at 4x in the game),
// so the leave threshold sits well below the enter one. The rule holds each side for a while and each state for a dwell time.
inline constexpr double kPacingEnterLoad = 0.75, kPacingLeaveLoad = 0.55;
inline constexpr double kPacingHoldMs = 2000.0, kPacingDwellMs = 10000.0;
struct PacingRule
{
    bool on = false;
    double aboveMs = -1;                // when load rose to the enter threshold (and stayed there); -1: not above
    double belowMs = -1;                // when load fell to the leave threshold; -1: not below
    double lastChangeMs = -1e9;

    // Returns the new state. eligible false (V-Sync or a display paced game, no frames generated, no flip metering for the batch or
    // none to be had, a swap chain the game made) turns it off at once, and that counts as a change; while off nothing is held.
    bool Update(double load, double nowMs, bool eligible)
    {
        if (!eligible)
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
};

// The real swap chain's frame latency tokens (the waitable object, limit 2 at all times) under the burst. A burst present takes its token
// without blocking; when none is there it goes out anyway and its retirement still releases one, a token the chain would hand out on top
// of the limit. TokenDebt counts those presents and pays them back: a burst present takes a spare token without blocking while it owes,
// a present outside a burst waits for one (one extra blocking wait, the caller gives up and Forgive()s on a timeout: nothing is in flight
// to wait for). Probed on a host swap chain (flag, limit 2): without it the surplus grew to 21 tokens over three bursts, with it 2.
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
};

// The settings after Gate, DriverPacing and the low-level keys are resolved (xefg_cuda.cpp). Gate=1 paces on the GPU through a present
// lane of its own, so it vetoes both the burst and DriverPacing. DriverPacing=1 or 2 sets the driver signals (all four parts) and the
// burst; with 1 they run only while PacingRule says so (pacingAuto), with 2 always.
struct DriverSettings
{
    bool driverFg;
    uint32_t parts;
    bool flipBurst;
    bool pacingIgnored;     // DriverPacing was asked for and Gate=1 vetoed it
    bool burstIgnored;      // FlipBurst was asked for and Gate=1 vetoed it
    bool pacingAuto;        // DriverPacing=1: the signals and the burst start off and PacingRule switches them
};
inline DriverSettings ResolveDriver(bool gate, int driverPacing, bool driverFg, uint32_t parts, bool flipBurst)
{
    DriverSettings r { driverFg, parts, flipBurst, false, false, false };
    if (gate)
    {
        r.burstIgnored = flipBurst;
        r.flipBurst = false;
        r.pacingIgnored = driverPacing != 0;
        return r;
    }
    if (driverPacing != 0)
    {
        r.driverFg = true;
        r.parts = 15;
        r.flipBurst = true;             // the 8-buffer chain and the metering stay for both modes
        r.pacingAuto = driverPacing == 1;
    }
    return r;
}
} // namespace oobdriver
