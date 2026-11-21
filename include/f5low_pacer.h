// f5low_pacer.h - the pacer of f5low.dll's own generator as a host-agnostic, CPU-only API (no D3D12, DXGI or Vulkan types).
//
// The D3D12 presenter of f5low (src/xefg_own.h) decides WHEN each generated frame and the app's own frame of a batch are presented from a
// handful of numbers: the app frames' ready times, the smoothed interval, the schedule's base, the lead, the last present's time, and the
// flip metering / driver pacing rules (src/pace_sched.h, pace_lead.h, oob_driver.h). A host that presents itself (OptiScaler's Vulkan
// output with f5low_frames.h) calls this API instead of keeping its own spacing, so the SAME formulas and tuning run, and a later fix in
// f5low reaches it with a DLL update. The pacer only computes times; the host sleeps (f5lowPacerWaitUntil is the same wait the
// presenter uses) and presents.
//
// Clock: every time is a signed 64-bit tick count of one monotonic clock the host chooses (ticksPerSecond; 0 = Windows QPC, which
// f5lowPacerNowTicks reads). All times given to the pacer must be on that clock.
//
// Threading: every call is serialised inside (one lock); the intended use is one producer (the game thread, which only has to note
// the arrival time of its Present) and one consumer (the present worker), which makes all the calls below for one batch in order:
//   FrameArrived
//   [MeterResult]               after the host configured the driver's metering for the batch (when meterWanted); it may also follow the
//                               batch's first present, as on D3D12 (the driver is configured right before the first Present)
//   for k = 1 .. generated + 1: Target -> [Stale] -> sleep until releaseTicks -> wait for the frame's GPU work -> present -> Presented
//   BatchDone
// With generated == 0 the batch is the app's own frame: FrameArrived, Target (returns 0: present at once), Presented, BatchDone.
// Destroy only after every other call has returned and no thread will make one (it frees the pacer). Every call is safe against an
// exception inside (it returns F5LOW_PACER_ERROR_INTERNAL); a null pacer / pointer is F5LOW_PACER_ERROR_ARGUMENT.
//
// Versioning: each struct starts with (or, for CreateInfo, has after apiVersion) `structSize`, the host's sizeof of it. f5low accepts a
// size at least as large as the one it knows (a newer host) and reads the fields it knows; a smaller one is an argument error. Output
// structs (Batch, Target, Info) are written only up to the version-1 size and keep the host's structSize. The host sets structSize on
// every struct it passes or receives into.
//
// What is NOT here (it needs the host's GPU or swap chain): waiting for the GPU work of a frame, the swap chain's frame latency token,
// the generation queue's waits (GenBlock), the driver calls themselves (the host sets VK_NV_present_metering from `numFramesPerBatch`
// and `batchMs`), XeLL / Reflex. docs/GENERATORS.md "Frames only API" lists the mapping and the gaps.
#pragma once

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C"
{
#endif

#define F5LOW_PACER_API_VERSION 1u
#define F5LOW_PACER_MAX_GENERATED 5u

// Results (int32_t)
#define F5LOW_PACER_OK 0
#define F5LOW_PACER_ERROR_ARGUMENT (-1) // a null pointer, a bad version or structSize, a field out of range
#define F5LOW_PACER_ERROR_INTERNAL (-5) // an exception inside f5low

#ifdef F5LOW_FRAMES_EXPORTS
#define F5LOW_PACER_API __declspec(dllexport)
#else
#define F5LOW_PACER_API __declspec(dllimport)
#endif

typedef struct F5LowPacer_T* F5LowPacer;

// F5LowPacerCreateInfo::flags
#define F5LOW_PACER_FLAG_VRR 0x1u // the display is variable refresh (informational: the D3D12 presenter's pacing does not read it either)
// The host can configure the driver's flip metering (Vulkan: VK_NV_present_metering is enabled on the device and the feature is on). Without it
// the pacer never asks for metering, whatever the GPU is (D3D12: no flip metering function = no metering). Default off.
#define F5LOW_PACER_FLAG_HOST_CAN_METER 0x4u
// The host's swap chain can run the burst (D3D12: a swap chain made for it with headroom, on its own present queue): the presents of a metered
// batch after the first are not waited for, the driver spaces their flips. Default off: without it a metered batch's frames after the first are
// released FlipLeadUs early instead (the plain metered pacing). Also needs DriverPacing / FlipBurst to allow the burst, as on D3D12.
#define F5LOW_PACER_FLAG_BURST_CAPABLE 0x2u

typedef struct F5LowPacerCreateInfo
{
    uint32_t apiVersion;      // F5LOW_PACER_API_VERSION
    uint32_t structSize;      // sizeof(F5LowPacerCreateInfo)
    uint32_t flags;           // F5LOW_PACER_FLAG_*
    int64_t ticksPerSecond;   // the host's clock rate; 0: Windows QPC (f5lowPacerNowTicks)
    int32_t gpuSm;            // the NVIDIA GPU's compute capability as NN of sm_NN (RTX 50: 100 and up), 0 not NVIDIA / unknown; f5lowFramesGpuSm gives it
    uint32_t maxGenerated;    // 1..F5LOW_PACER_MAX_GENERATED
    double refreshHz;         // the display's refresh rate, 0 unknown (informational, see the VRR flag)
    // ---- added after the first release of this struct (a host built against the older header passes the smaller structSize, which is
    // accepted: the two fields below are then 0). Who spaces a batch whose F5LowPacerFrame::displayPaced is 1 (V-Sync / FIFO), as D3D12's
    // FixedPacing (pace_sched.h FixedTimer):
    uint32_t fixedPacing;     // 0: f5low's own setting (XEFG_CUDA_FIXEDPACING / f5low.ini FixedPacing, auto = timer), 1: display (the batch
                              // goes out back to back, F5LOW_PACER_TARGET_PACED is not set), 2: timer (each frame waits for its time).
                              // Anything above 2 is an argument error. MAILBOX / IMMEDIATE batches (displayPaced 0) are always timed.
    uint32_t gate;            // 1: the host's own latency gate paces the game (D3D12 Gate=1): display, whatever fixedPacing or the setting say
} F5LowPacerCreateInfo;
// structSize of the first release of F5LowPacerCreateInfo (everything up to refreshHz); f5low accepts it and anything larger
#define F5LOW_PACER_CREATEINFO_SIZE_V1 offsetof(F5LowPacerCreateInfo, fixedPacing)

// One app frame arriving at the present worker.
typedef struct F5LowPacerFrame
{
    uint32_t structSize;      // sizeof(F5LowPacerFrame)
    int64_t arriveTicks;      // when the game's present call came (CPU)
    int64_t readyTicks;       // when the game's GPU work for the frame was done (the D3D12 "ready stamp"); 0 unknown: arriveTicks is used
    uint32_t generated;       // generated frames this batch will have (0: the app's frame alone, nothing generated)
    uint32_t displayPaced;    // 1: the game presents with V-Sync (the display spaces the frames; no lead, no metering, no burst)
} F5LowPacerFrame;

// A batch's plan, frozen at FrameArrived. The host keeps it and passes it back; the fields are read-only for the host except through
// f5lowPacerMeterResult.
typedef struct F5LowPacerBatch
{
    uint32_t structSize;      // the host's sizeof(F5LowPacerBatch), set before f5lowPacerFrameArrived
    uint32_t generated;       // n: the generated frames; the batch has n + 1 presents, the last one the app's own
    uint32_t displayPaced;
    int64_t baseTicks;        // the schedule's base for this frame
    int64_t stepTicks;        // the spacing of the batch: smoothed interval / (n + 1)
    int64_t leadTicks;        // the lead frozen for this batch (0: off); the first generated frame goes this early on the schedule
    uint32_t meterWanted;     // 1: configure the driver's flip metering for this batch with numFramesPerBatch (and batchMs). 0: this batch is not
                              // metered; if the batch before was, tell the driver one frame per batch (numFramesPerBatch 1) or stop chaining the config
    uint32_t metered;         // 1: this batch is metered. It is 1 whenever meterWanted is, and stays 1 unless f5lowPacerMeterResult reports a refusal or
                              // no independent flip (D3D12 semantics: the driver is assumed to take the configuration). A host that configures the
                              // driver must call f5lowPacerMeterResult; without the call the pacer meters on faith.
    uint32_t numFramesPerBatch; // n + 1, for the metering configuration
    double batchMs;           // the batch time f5low's D3D12 presenter configures (the GPU timeline's app frame time, smoothed, growth-limited by 1/8 per
                              // batch). Informational on Vulkan: the spacing comes from releaseTicks, VkSetPresentConfigNV has no time field
    uint64_t serial;          // this batch's number (1, 2, ...)
} F5LowPacerBatch;

// F5LowPacerTarget::flags
#define F5LOW_PACER_TARGET_BURST 0x1u   // metered and the burst is on: present as soon as the frame's GPU work is done, do not wait for releaseTicks
#define F5LOW_PACER_TARGET_METERED 0x2u // the batch is flip metered (the hardware spaces the flips)
#define F5LOW_PACER_TARGET_PACED 0x4u   // releaseTicks is a time to wait for

typedef struct F5LowPacerTarget
{
    uint32_t structSize;      // the host's sizeof(F5LowPacerTarget)
    int64_t targetTicks;      // frame k's slot on the schedule (0: the app's frame alone, present at once)
    int64_t releaseTicks;     // sleep until this, then wait for the frame's GPU work, then present; 0: do not sleep (burst, or not paced)
    uint32_t flags;           // F5LOW_PACER_TARGET_*
} F5LowPacerTarget;

// F5LowPacerPresented::flags
#define F5LOW_PACER_PRESENT_DROPPED 0x1u // the frame was left out (not presented): the slot is kept for the spacing of the later ones

typedef struct F5LowPacerPresented
{
    uint32_t structSize;      // sizeof(F5LowPacerPresented)
    int64_t presentTicks;     // when the present call returned (the D3D12 presenter's `lastShown`); for a dropped frame the time it was left out
    int64_t gpuDoneTicks;     // when the frame's own GPU work was done; 0 unknown. Past the target = our GPU work held the frame (raises the lead)
    uint32_t flags;           // F5LOW_PACER_PRESENT_*
} F5LowPacerPresented;

typedef struct F5LowPacerInfo
{
    uint32_t structSize;      // the host's sizeof(F5LowPacerInfo)
    uint32_t flipMetering;    // the flip metering decision for this GPU and settings (RTX 50 = on with FlipMetering=auto; 0/1 forces)
    uint32_t burstCapable;    // the host said it can (F5LOW_PACER_FLAG_BURST_CAPABLE) and the DriverPacing / FlipBurst settings allow the burst
    uint32_t pacingActive;    // the DriverPacing=1 load rule's state (on: the GPU is the limit); always 1 when the mode is not automatic
    uint32_t burstNow;        // the burst is on now (capable, pacing active and, for DriverPacing=2, the load rule's want)
    int32_t pacingMode;       // the resolved DriverPacing: 0 off, 1 automatic, 2 always
    uint32_t meteringRefused; // the driver refused the metering config kFlipRefusals times in a row: off for this context
    double intervalMs;        // the smoothed app frame interval
    double batchTimeMs;       // the metering batch time
    double leadMs;            // the lead as it stands
    double loadShare;         // the GPU's share of the app interval, smoothed
    uint64_t batches;
} F5LowPacerInfo;

// ---- the pacer ----

// 0 on success. A failed call leaves *out null.
F5LOW_PACER_API int32_t f5lowPacerCreate(const F5LowPacerCreateInfo* info, F5LowPacer* out);
F5LOW_PACER_API void f5lowPacerDestroy(F5LowPacer p); // releases the timer raise if the host left it on

// An app frame took its turn at the present worker: Pick + the batch's plan. Fills *batch.
F5LOW_PACER_API int32_t f5lowPacerFrameArrived(F5LowPacer p, const F5LowPacerFrame* frame, F5LowPacerBatch* batch);

// Frame k of the batch (1 .. generated for the generated frames, generated + 1 for the app's own). Uses the last present's time, so call it
// just before waiting for frame k, after frame k - 1 was reported with f5lowPacerPresented.
F5LOW_PACER_API int32_t f5lowPacerTarget(F5LowPacer p, const F5LowPacerBatch* batch, uint32_t k, F5LowPacerTarget* out);

// Generated frame k is stale (leave it out): the game's NEXT frame is already finished on the GPU (nextGameFrameDone) and `nowTicks` is at least
// half a step past the frame's slot. Only where the D3D12 presenter's LateGen=auto is active: metering off for the context (flipMetering 0 in
// the info: the GPU, the settings, the host's flag or 8 refusals) and the batch not metered, not V-Sync; the app's own frame is never stale.
// Returns 1 for stale, 0 for not, negative for an error.
F5LOW_PACER_API int32_t f5lowPacerStale(F5LowPacer p, const F5LowPacerBatch* batch, uint32_t k, int64_t nowTicks, uint32_t nextGameFrameDone);

// The driver's answer to the batch's metering configuration: status 0 = accepted; noIndependentFlip 1 = the driver reported no independent flip
// (composed): then the batch is not metered. Updates batch->metered; after 8 refusals in a row metering is off for the context.
// Vulkan: status = 0 when the host's VkSetPresentConfigNV was chained and the driver reported no feedback problem (presentConfigFeedback == 0);
// any other status (a failed call, a stripped extension, presentConfigFeedback != 0) is a refusal, and 8 in a row turn metering off.
F5LOW_PACER_API int32_t f5lowPacerMeterResult(F5LowPacer p, F5LowPacerBatch* batch, int32_t status, uint32_t noIndependentFlip);

// Frame k's present returned (or it was dropped).
F5LOW_PACER_API int32_t f5lowPacerPresented(F5LowPacer p, const F5LowPacerBatch* batch, uint32_t k, const F5LowPacerPresented* what);

// The batch is over (every present reported): the lead learns from it. The load rule that switches the burst (PacingFrame) treats a batch with no
// generated frames, V-Sync, no metering or a refusal as a hard departure (the burst goes off at once); a refilling history only counts after about 1 s.
F5LOW_PACER_API int32_t f5lowPacerBatchDone(F5LowPacer p, const F5LowPacerBatch* batch);

F5LOW_PACER_API int32_t f5lowPacerGetInfo(F5LowPacer p, F5LowPacerInfo* out);

// ---- the same timing helpers the D3D12 presenter uses ----
// The level of this API in the DLL: 1 = the first release, 2 = F5LowPacerCreateInfo::fixedPacing and ::gate are applied. A DLL without
// this export is level 1 (it accepts the larger struct and ignores the two fields: V-Sync batches are timed).
F5LOW_PACER_API uint32_t f5lowPacerApiLevel(void);
F5LOW_PACER_API int64_t f5lowPacerNowTicks(void);      // QPC
F5LOW_PACER_API int64_t f5lowPacerTicksPerSecond(void);
// Sleeps until a QPC time: a high-resolution timer for all but the last half millisecond, then a spin. QPC clock only.
F5LOW_PACER_API void f5lowPacerWaitUntil(int64_t qpcTicks);
// The 1 ms system timer while the host generates (on = 1) and its release (on = 0), per pacer; the presenter does this while it generates.
F5LOW_PACER_API void f5lowPacerTimerRaise(F5LowPacer p, uint32_t on);

#ifdef __cplusplus
}
#endif
