// What the own generator tells NVIDIA's driver about its queues and presents (public nvapi.h: NvAPI_D3D12_NotifyOutOfBandCommandQueue,
// NvAPI_D3D12_SetAsyncFrameMarker), so Reflex can tell the generator's work and flips from the game's frames. Prototype, off by
// default: OobQueue=1 and OobMarkers=1 in f5low.ini (xefg_own.h, OobNotify and RealPresent). No Windows in here: tested on the host
// (tests/oob_driver_test.cpp).
#pragma once

#include <cstdint>

namespace oobdriver
{
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

// DriverPacing (xefg_own.h, PresentReal): a present after a batch's first goes out at once, on its virtual slot, when the burst is on
// and the NVIDIA driver meters the batch's flips. The batch's first present (k == 1) keeps its time.
inline bool BurstNow(bool burstOn, uint32_t k, bool metered) { return burstOn && k > 1 && metered; }
} // namespace oobdriver
