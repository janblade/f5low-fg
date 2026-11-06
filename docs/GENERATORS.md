# Generators

f5low.dll is a frame generation **host**. OptiScaler drives it through Intel's XeSS-FG swap chain API (the
`xefgSwapChain*` exports, headers from Intel's public SDK). Behind that API the host owns the parts every generator
shares: the swap chain the game sees, frame input staging, pacing, presentation, latency reporting (XeLL calls),
the UI/letterbox rules. A **generator** only makes the in-between frames.

Three generators run: **own** (XeFG Tensor), **dlssg** (NVIDIA DLSS-G through a private copy of the player's Streamline,
RTX 20 and newer, 2x to 6x on every tier: native on RTX 50, unlocked in memory on RTX 20/30/40, see "dlssg" below) and **intel**
(Intel's libxess_fg, the one the own generator reads, unlocked in memory to 6x, see "intel" below). `fsr` is registered and selectable, reports
"not available yet" in the log, and runs `own`; its design is below (see "What is ready").

## Choosing a generator

`F5LOW_GENERATOR` (env, set by OptiScaler before each `xefgSwapChainD3D12CreateContext`), else the old name
`XEFG_CUDA_GENERATOR` (env), else `Generator=` in f5low.ini (`GeneratorSetting()` in `src/xefg_cuda.cpp`, next to the
other settings). Values, case insensitive: `own` (default), `intel`, `dlssg`, `fsr`; anything else logs
`f5low: generator "<value>" is unknown (own, intel, dlssg, fsr) - running own` and runs `own`.

A generator that is not available logs one line and the host runs `own`: `f5low: generator <name> not available yet, running own`
for `fsr` (`src/generators/stubs.h`, `GenCaps::available = false`). **`dlssg` and `intel` never fall back**: when the player picks `dlssg` and it
cannot start (no NVIDIA GPU, below RTX 20, no usable Streamline, a DLSS-G build the unlock does not recognise, slInit failing),
`f5low: generator dlssg not available (<reason>) - frame generation is off (no fallback to own)` is logged, `f5lowGetStatus` says 3 with
`dlssg unavailable: <reason>`, `xefgSwapChainD3D12CreateContext` returns `XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE` and no own context is
made. Switching to XeFG Tensor is the player's choice, never automatic. The generator that runs is logged once at context creation:
`f5low: generator own (XeFG Tensor)` or `f5low: generator dlssg (DLSS-G)`.

## What each vendor library owns

| | own | intel (libxess_fg) | dlssg (Streamline DLSS-G) | fsr (FidelityFX FG) |
|---|---|---|---|---|
| Makes frames from | our recipe/pack | its own network | NVIDIA driver/model | its own optical flow + interpolation |
| Gets inputs by | host hands colour/motion/depth | `xefgSwapChainTagFrameResource` | `slSetTagForFrame` | prepare + dispatch descs |
| Can return frames without presenting | yes | no: presents on its swap chain | no: presents on its swap chain proxy | **yes**: `ffxDispatchDescFrameGeneration` writes `outputs[]` on a command list we give it |
| Paces itself | no (host; with `DriverPacing=1` the NVIDIA driver spaces the flips) | yes, on the chain's latency waitable | yes (driver flip metering, Reflex) | yes when its swap chain is used; no when we dispatch |
| Max generated frames | pack/recipe (5) | `xefgSwapChainGetProperties` (3 native on 1.3.4.95) | `DLSSGState::numFramesToGenerateMax` (1-5 by GPU) | `numGeneratedFrames` into `outputs[4]` (version dependent) |
| Latency partner | XeLL calls by host (`DriverPacing=1`: Reflex Sync with the frame generation multiplier as well) | XeLL (its own) | Reflex (required) | Anti-Lag 2 optional |
| Frame id rule | host | host passes ids | `FrameToken` | `frameID` must step by exactly 1 or it resets |
| GPU | NVIDIA (CUDA / NVAPI cubins) | any D3D12 GPU | NVIDIA RTX 40+ (multi-frame 50+) | any |

## Two backend shapes

Every library falls in one of two shapes. The code carries the present sink (dlssg); the frames-only shape is not built.

**Frames only** (`own`, `fsr` through its dispatch): the host calls the generator once per app frame with the
inputs and the interpolation points `t = k / (n + 1)`, and gets n frames back on the GPU. The host paces and
presents them exactly as today.

**Present sink** (`dlssg` today; `intel`, and `fsr` if its swap chain turns out to be needed): the library insists on
presenting. The host gives it a **sink** (`src/sink/sink.h`, `scproxy.h`), a proxy swap chain and factory of ours, and every
`Present` the library makes is queued and shown on the real swap chain by the sink's own thread. The sink has two modes:
spaced (Intel: the library's own pacing is neutralised by handing it an always-signalled latency waitable, so its burst arrives
back to back and the sink spaces it) and stable buffers (DLSS-G: it takes its buffers once and draws into
`buffer[GetCurrentBackBufferIndex()]`; `GetBuffer(i)` is staging texture i and Present returns when the next index's texture is
out of the queue and copied). Proven with Intel 1.3.4.95 in the harness (libxess-weights, 2026-10-29/31): real-chain
present gaps at 6x / 30 fps sd 0.29 ms, p99 6.15 ms (Intel presenting itself: sd 2.10, p99 11.86); resize,
exclusive full screen in and out, and a menu stretch (bursts split by arrival gap) all pass. The dlssg numbers are under "dlssg" below.

```cpp
struct GenInputs {            // one app frame, already staged by the host (valid until Release)
    ID3D12Resource* colour;   // HUD-less colour when UI composition picks it, else the back buffer
    ID3D12Resource* motion;
    ID3D12Resource* depth;
    ID3D12Resource* ui;       // may be null
    D3D12_RECT      area;     // interpolation area (letterbox)
    uint64_t        frameId;  // steps by exactly one; a jump = reset
    bool            reset;    // camera cut
    const float*    constants;// motion scale, jitter, camera matrices as the API tagged them
};

struct GenCaps {
    uint32_t maxGenerated;    // highest n this backend can do on this GPU
    bool     presentsItself;  // true = present-sink shape
    bool     anyGpu;          // false = NVIDIA only (greys F5Low's entry in OptiScaler)
    const char* name;         // log and overlay
};

class IGenerator {
public:
    virtual ~IGenerator() = default;
    // Frames-only and sink both: create on the game's device and queue, for this output size and format.
    virtual bool     Init(ID3D12Device* dev, ID3D12CommandQueue* queue, uint32_t w, uint32_t h, DXGI_FORMAT fmt,
                          uint32_t maxGenerated) = 0;
    virtual GenCaps  Caps() const = 0;
    // Frames only: record n frames at t[i] into out[i] on cl (host fences and presents them).
    virtual bool     Generate(ID3D12GraphicsCommandList* cl, const GenInputs& in, const float* t, uint32_t n,
                              ID3D12Resource* const* out) { return false; }
    // Sink: the backend is handed the host's sink factory instead of the real one, and the host's tag/present
    // calls are forwarded to the library; frames come back through the sink.
    virtual bool     AttachSink(IDXGIFactory* sinkFactory) { return false; }
    virtual void     SetGenerated(uint32_t n) = 0;   // multiplier change, no restart
    virtual void     Resize(uint32_t w, uint32_t h, DXGI_FORMAT fmt) = 0;
    virtual void     Shutdown() = 0;
};
```

The struct above is the early sketch, not the code. The code is `src/generator.h`, two classes:

- `IGenerator`: the narrow contract every backend implements. `Caps()` (`GenCaps`: name, `available`, reason, max generated
  frames, NVIDIA only or any GPU), `SetHost`, `AttachSink` (returns false unless a present-sink backend takes it),
  `Build` / `Rebuild` / `Drain` / `Stop` / `Shutdown`, and `AsPack()`, which returns the pack surface or null.
- `IPackGenerator`: the own generator's surface and only its (`src/generators/own.h`, class `OwnGenerator`, which
  implements both). The host copies the game's frame, motion and depth into the pack's buffers (`Buf(GenSlot)`) and drives
  passes: recorded into a command list of the host's (`PreparePass`, `RecordPass`, `RecordPassInto`: the D3D12 path) or
  started on a CUDA stream that signals a fence (`Launch`, `DoneFence`, `RingFrame`: the CUDA path).

The host (`Own` in `src/xefg_own.h`) owns a `std::unique_ptr<IGenerator>`, caches `AsPack()` once at `Create` and drives only
that. A generator without a pack surface cannot be driven yet, so `Create` refuses it.

## What is ready

- Ready: `own` behind `IGenerator` / `IPackGenerator`; the selection (above); `dlssg` and `intel` (present sink, see below); `fsr`
  registered as "not available yet" (`GenCaps` only) and falling back to `own`.
- Not ready: frames-only backends (`fsr`). The host only knows pack-layout buffers; a backend that takes and returns textures
  needs a texture in/out path in `CopyFrame` / `Show` / `PresentGen` next to them, and a `Generate(GenInputs, t[], out[])`
  entry on `IGenerator`.
- Ready for dlssg and intel: the present-sink route. A dlssg context is a separate kind of context at the XeFG entry points of
  `src/xefg_cuda.cpp` (next to the forwarding-to-Intel route: `FindDlssg(h)` in every entry point), not a `class Own` /
  `IPackGenerator`; `own` runs through exactly the code it ran through before. `intel` reuses `src/sink/` and the same kind of
  entry-point branch, with its own sink mode (spaced bursts); see "intel" below.
- Not ready: the frames-only shape (`fsr`), see above. Stories A3-A4 will shape it.

## Per backend, when it is built (stories A2-A4)

- **own** (`src/xefg_own.h`, D3D12 path): `DriverPacing=` in f5low.ini (`XEFG_CUDA_DRIVERPACING`): `0` off (**default**), `1` automatic, `2` always on; ignored with `Gate=1`, NVIDIA GPUs
  only, needs OptiScaler 0.1.37 or newer (older versions take the async frame markers for the game's own Reflex calls; 0.1.37 also reads `xefgCudaDriverMultipliesReflexCap` to give Reflex the shown-frame cap undivided). It
  turns on the driver signals (NvAPI_D3D_SetReflexSync with the frame generation multiplier, in-band present markers on a second queue, out of
  band markers and queue notices) and the burst: while the driver flip-meters a batch, the presents after the batch's first are released
  at once on their virtual slots and the driver spaces the flips (the real swap chain then has 8 buffers and a frame latency of 2 at all times;
  the burst's presents take their latency tokens without blocking and pay back the ones they lacked; the game still sees the buffer count it asked for). Measured in one game so far (NBA 2K27). The low-level keys
  still work with `DriverPacing=0`: `DriverFg=1`, `DriverFgParts=` (1 Reflex Sync, 2 present markers, 4 out of band markers, 8 queue
  notices), `FlipBurst=1`, `OobQueue=`, `OobMarkers=1`, `AppJobs=`, `RetireWait=`, `FlipMetering=`, `FlipLeadUs=` (not used while the burst
  is on).
  `DriverPacing=2` runs the set always (while the batch is metered). `DriverPacing=1` runs it only while the GPU is the limit: the presenter
  measures the GPU's share of the app interval (how long after the game's Present call its GPU work finished, over the app interval,
  smoothed by 1/8; logged every 120 app frames as `GPU load`), turns the set on after the load has been 0.75 or more for 2 s and off after
  0.55 or less for 2 s, at most once per 10 s; V-Sync, no generated frames, no flip metering or a swap chain the game made turn it off at
  once. Measured in one game so far (NBA 2K27, PresentMon): at 4x with the GPU at its limit the set cuts sim-to-screen from about
  95-110 ms to 37-55 ms; at 6x with a 120 fps cap (the GPU idle part of the time) it gains nothing and spaces the frames less evenly.
  Note that `DriverPacing=1` used to mean "always on" and is now the automatic mode; `2` is what `1` meant before.
  In mode 1 nothing is sent while it is off: a capped session whose load stays under the enter threshold never sends a signal (it
  behaves as `DriverPacing=0` apart from the real swap chain, which has the 8 buffers and the waitable object from the start). Once the
  set has been armed, the queue notices stay as sent. A switch goes in an order where a frame is never held longer than it should be:
  on - the cap export (`xefgCudaDriverMultipliesReflexCap`) goes up first, the signals follow 30 app frames later; off - the signals and
  the burst stop and Reflex Sync gets multiplier 1 at once, the export goes down after 30 app frames and after the driver took the 1
  (a refused 1 is sent again each frame; after 120 refusals the export comes down anyway and the log says so). Eight refused Reflex
  Syncs in a row turn the automatic mode off for the session. A cause that makes the set ineligible (V-Sync, frame generation off, a
  swap chain the game made) acts at once; a history reset, the metering refilling or a one-batch dropout only after about 1 s without a
  break. In modes 2 and with the low-level keys the export goes up just before the first Reflex Sync with the multiplier, and comes
  down again if the driver refuses that send. `PacingFlipSec=N` (`XEFG_CUDA_PACINGFLIPSEC`, test only) flips the requested state every N seconds whatever the load.
- **intel** (built, `src/generators/intel.h`, `src/unlock/FamilyXeFG.h` + `UnlockXeFg.h`): Intel's libxess_fg makes the frames, our sink
  paces them. Present-sink shape, spaced mode. Any D3D12 GPU Intel's library runs on.
  - *Routing.* `Generator=intel` in `D3D12CreateContext` builds an `xintel::Ctx` (not `Own`). The handle is the library's own handle; every XeFG
    entry point of `xefg_cuda.cpp` calls the library f5low loaded for it for such a handle (`FORWARDH`; a proc it lacks is an error, never a fall-back), the other handles still go to the shared
    `Real()` library. **No fallback**: when the library cannot start (no libxess_fg found, the load failed, a failed rollback of the unlock, CreateContext refused),
    `f5low: generator intel not available (<reason>) - frame generation is off (no fallback to own)`, `f5lowGetStatus` 3
    `intel unavailable: <reason>`, `XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE`.
  - *Library.* No copies on disk. It is the SAME libxess_fg.dll the own generator reads (`RealLibraryPath` in `xefg_cuda.cpp`:
    `XEFG_CUDA_INTEL_LIBRARY`, next to f5low.dll, the folder above = OptiScaler's), loaded by full path and logged. A source that is f5low
    itself (our own path, or a module exporting `f5lowGetStatus`) is never loaded.
  - *Unlock.* `unlock/FamilyXeFG.h` (OptiScaler-NR's family, U1-U5 on libxess_fg, XL1 on libxell, structural locators) applied by
    `unlock/UnlockXeFg.h` right after f5low loaded the library and BEFORE the first XeFG call: Intel sizes every per-generated-frame buffer
    from U3 (its default interpolation ceiling) when a context is created, so a late patch leaves 3-slot buffers (evidence in the ai-os
    2026-11-03 study). Whether libxess_fg.dll was ALREADY loaded in the process before f5low is decided by handle (the process' module
    list before the load against the handle the load returns: 8.3 names, junctions and `\\?\` prefixes do not matter); then a context may exist
    and nothing is patched: Intel's own maximum, with the reason in the log. One exception: the module f5low's own forwarding (`Real()`)
    loaded itself, when no context was ever made through it, counts as f5low's own and is patched. **The patched module is shared:** it is THE
    module of that path, so a later loader in the process (the player switching OptiScaler's FG output to XeFG at run time) gets a library that
    reports 6x; the log line "module shared" says so when the unlock applies. All or none; an unknown build is refused with the failing site and the library then runs
    unpatched at its own maximum (on an RTX 5070 Ti with 1.3.4.95 and libxell 1.3.2.10 that is 2x: GetProperties says 1; with the
    unlock it says 5 = 6x). A failed rollback (a partially patched module) refuses the library outright: status 3. The count is 3 to 5.
    `F5LOW_INTEL_UNLOCK=0|off|false|no` turns it off (tests); `F5LOW_TEST_XEFG_SKIP=U4` leaves one site unpatched (tests). This patches
    inside Intel's DLLs in memory; nothing is written to disk.
  - *libxell.* libxess_fg does not import libxell: it calls `GetModuleHandleExA(0, "libxell.dll")` at run time, so it uses the libxell the
    process already has (OptiScaler's or the game's) = the one that made the XeLL context OptiScaler hands over. XL1 is patched in that
    module in memory (one log line says so; it only lifts libxell's refusal of generated-frame counts above 3; f5low takes a reference on it so
    it is not unloaded and loaded again unpatched). XL1 already holding exactly the byte this family writes (another component, e.g.
    OptiScaler's own MFG unlock on the shared libxell, lifted it) counts as applied ("already lifted by another component"); the same is
    never accepted for libxess_fg U1-U5. If no libxell.dll is loaded
    when the context is created, the one next to libxess_fg is loaded by full path. If the XL1 site is not found (or the libxell holds another count
    before), XL1 and the libxess_fg unlock are skipped together. Intel reports the generated frames to XeLL itself every frame, so
    `xefgCudaXellCountsRenderedOnly` stays 0.
  - *Sink.* `InitFromSwapChainDesc` hands Intel a `SinkFactory` (spaced mode); `SetEnabled` / `SetNumInterpolatedFrames` set the sink's burst
    (generated + 1; the factory remembers it, so a re-created chain starts with it). `InitFromSwapChain` (a chain the game made) cannot be sunk: the library presents on it itself (logged).
  - *Pacing policy.* `F5LOW_SINK_PACE=auto` (default; `SinkPace=` in f5low.ini), `schedule`, `burst` or `off`. Auto picks per burst size (logged:
    "N frames per application frame: pacing by ..."): the schedule at 2x, the burst spacing from 3x. Why (harness A/B, 1.3.4.95, RTX 5070 Ti,
    20 s): at 2x the schedule removes the burst-start jitter (2x/30 gap sd 0.69 -> 0.24, pm_sd 2.0 -> 0.5, late <= 1 %). From 3x the first
    generated frame of every burst is late (~3 ms, about one late frame per batch, 12-20 % of generated frames at 4x/6x at 60 fps) and gap sd /
    pm_sd are worse than the burst spacing. A wider lead cap (n steps; the lead rule reached 13-17 ms) did NOT remove it, and
    Intel's frames all arrive within ~1.5 ms of the game's Present, so the cause is probably the first frame's GPU completion behind the whole
    burst's work, not a short lead. Fixed leads (not clamped to a step any more; harness 2026-11-03, 20 s, d7387f9) did not remove it either: lead 6 ms / lead 9 ms
    at 4x/60 give late 1.1 % / 27 % with gap sd 1.14 / 0.79 (burst 0.19) and at 6x/60 late 15 % / 27 % with gap sd 1.5 (burst 0.13); jitter2 is
    the same picture (6x late 11 % / 23 %, gap sd 1.5 against 0.70). Only lead 6 at 4x/60 reaches late ~1 %, with a gap sd still six times
    the burst spacing's. A bigger lead costs latency and, past about one frame time, raises the late share (the frames wait behind the burst). Tests: `F5LOW_TEST_SINK_LEADCAP=wide`, `F5LOW_TEST_SINK_LEAD_MS=<ms>`, `SINK_TRACE3`; the sink log lists
    arrival offsets and late frames per burst frame j.
  - *Display-paced presents and `off`.* A present with V-Sync (sync interval > 0) or flags other than tearing is paced by the display, and the
    sink stands aside as the own generator does (`DisplayPaced` in xefg_own.h; `sink/release_rule.h` mirrors it): such a frame goes out as soon
    as its staging copy is queued, with no wait for a time, and it does not feed the lead rule (never "late"), the burst interval, the spacing
    or the schedule (no stamps are used while it lasts, and the stamp queue is capped at 8). When the state changes (V-Sync on or off) the
    burst and schedule state is reset and learned again; one log line per change (`sink: display-paced (V-Sync): frames go out as they come`,
    at most one per second). `SinkPace=off` does the same for every frame, V-Sync or not: pass-through, no burst spacing, no schedule, no
    stamps. It is the A/B switch for DLSS-G: on RTX 40/50 DLSS-G sets the driver's flip metering itself, so with `off` the display
    hardware spaces the flips and our timer is not a second pacer on the same chain. `off` is meant for `dlssg`: for `intel` it is mapped to `burst` (logged), because Intel presents its bursts back to back and nothing else spaces them.
    A `DXGI_PRESENT_TEST` present is forwarded to the real chain and never queued. A mixed stream (generated frames sync 0, the real frame
    sync 1) does not flip the state per frame: the new state must hold for burst + 1 frames in a row. Default stays `auto` until a game A/B decides.
  - *Schedule mode.* **schedule**: the sink releases a burst on the own
    generator's D3D12 schedule instead of its measured burst interval. The game side's Present is stamped: `GetSwapChainPtr` returns Intel's
    proxy chain behind a thin wrapper (`xintel::StampChain`; the call is forwarded untouched, every other method passes through; it holds its
    context weakly). This was the least invasive place that sees the game's own Present, and the stamp is the game's own timing, free of
    Intel's variable generation time. The stamps give the smoothed app interval (`pacesched::ReadyInterval`, the own pacer's rule) and each
    batch's base (`pacesched::NextBase`); the first frame of a burst (the sink's arrival-gap split) takes the newest stamp not later than its arrival (`pacesched::PickStamp`; older
    ones are dropped, so an extra stamp cannot shift the schedule). Frame j of a batch (generated 1..n, then the game's frame) is
    released at `base + pacelead::Offset(lead, step, j + 1)`, step = interval / (n + 1), the lead frozen per batch (`pacelead::Lead`). The
    Present waits for the frame's staging copy first (as the own generator's `PresentReal`): a generated frame whose copy ran past its time
    is "late" and feeds `Lead::Late`/`EndFrame` exactly as Own does. A frame past its time is presented on arrival and followed no
    sooner than half a step after the last (`pacesched::Bound`; no bunching). First frames (fewer than 6 stamps), no wrapper (the game's own
    chain from `InitFromSwapChain`), FG off (burst 1) and a changed burst size or resized chain fall back to / restart from the burst
    spacing; a short burst (a menu) is released on its planned time for frame 0 and as it comes after. dlssg is unchanged (burst spacing).
    The pieces shared with the own generator are in `src/pace_sched.h` (and `pace_lead.h`). Base: the app frame's GPU-ready stamp as in Own (`SinkGpuStamps=1`,
    default; `0` takes the CPU time of the Present for an A/B). `AppPresent` signals a fence of the sink's own on the application's queue (the
    queue the host gave `InitFromDesc`, passed with `SinkFactory::SetStampQueue`, as Own's `appFence`; NOT the queue the library made its
    chain with: Intel presents on a queue of its own, where a signal would only mean "Intel finished the frame before"; both queues are
    logged once per chain, `sink: chain made on library queue ...`; no application queue or another device: CPU stamps, counted; a
    queue `Signal`, nothing waits on the game's thread) and a
    watcher thread (as Own's) takes the time the fence completed. The pairing of a burst with a Present stays by the CPU time of the Present
    (`pacesched::PickStampBy`; a GPU time can be later than the burst's arrival, because the library hands over its first generated frame
    before the GPU finished the game's frame), and the base is read when the pacer thread reaches the batch's first frame (`ResolveBatch`),
    waiting at most two app frame intervals (4..50 ms) for a GPU time that is not there yet. No GPU time (device removed, the wait
    ran out): counted as a miss ("schedule bases" line of the sink statistics, with the lag and how far the GPU stamp trails the Present), and
    the base is drawn (`NextBase`) towards the Present's CPU time moved by the smoothed GPU-minus-CPU offset of the GPU-stamped batches (`pacesched::GpuBase`), not the raw CPU time, which sits a GPU latency off the GPU
    stamps and made the base jump there and back; the CPU time is the base only before the first GPU stamp, after a reset, with no
    application queue / `SinkGpuStamps=0`, or after 3 misses in a row. The watcher sleeps until a value is signalled. Precision: the time is a thread's wake-up after the fence completed (tens of microseconds to ~1 ms late), and a
    GPU more than a frame behind stamps several frames at one wake-up (those are late, never early). Lag (`pacesched::Lag`, `MaxLag=`,
    default 0.5 app frames, 0 = none): the base runs behind the GPU stamp by as much as generated frames came out late lately, as Own's
    `paceLag` (raised at once to the most a frame was late on top of the lag its batch was scheduled with, 0.98 per batch down, capped at
    MaxLag app frames). Lead and lag: base = stamp + lag, frame j at base + Offset(lead, step, j + 1); the lag moves the batch later, the lead
    earlier by (step - lead). Own uses the lead on its D3D12 path and the lag on its CUDA path, never both; the sink has both, the lead
    first and the lag as overflow: a late frame raises the lag only when `PaceLead` is off or the batch's lead was already at its cap (a
    step), because raising both for one late frame overshot by 1-3 ms. The lag falls 2 % per arriving batch and is reset with the
    schedule (`MaxLag=0` or `PaceLead=off` take either out). The defaults (`SinkGpuStamps` on, this lag rule) are not game-tested yet: a game
    A/B is pending. `kAutoMaxBurst` is unchanged: `SinkPace=schedule` at 3x-6x uses
    the GPU stamps and the lag, the A/B decides whether Auto may follow. Only our own GPU wait counts as
    late for the lead (a frame that arrived after its time is presented on arrival and does not raise it). After a late frame the lead
    stays raised for up to 128 application frames (`pacelead::kWindow`) and eases down by 1/16 per frame, so a menu (short bursts) that
    follows keeps the higher lead's latency for a few seconds. A Present with `DXGI_PRESENT_TEST` is neither stamped nor sampled.
  - *Tests.* `tests/build_mfgunlock_tests.ps1` builds `xefg_unlock_test.exe <libxess_fg.dll> <libxell.dll>` (sites found once, Locate changes
    nothing, Apply changes exactly the planned bytes); the harness: `repro_test` with `F5LOW_GENERATOR=intel REPRO_INTERP=n XEFG_CUDA_INTEL_LIBRARY=<OptiScaler-style folder>\libxess_fg.dll F5LOW_SINK_STATS=1 SINK_PROBE=1 REPRO_BAR=1 REPRO_FRONT=1 XEFG_CUDA_LOG=1` and Intel's libxell.dll as the second argument
    (Intel's library wants an XeLL context before the swap chain is made); `SINK_PROBE_DUMP=1` also logs the bar's x per frame shown.
- **dlssg** (built, `src/generators/dlssg.h`, `dlssg_files.h`): NVIDIA DLSS-G through Streamline's public API. Sink shape.
  - *Routing.* `Generator=dlssg` in `D3D12CreateContext` builds a `DlssgGenerator` (an `IGenerator`: `Caps()` = name, available,
    reason, `maxGenerated` (5 on every tier), `presentsItself = true`, `anyGpu = false`). Not available below RTX 20 (sm < 75) or without an
    NVIDIA GPU (`DLSS-G needs RTX 20 or newer`), without a usable Streamline, when the unlock does not recognise the DLSS-G build, or when
    `slInit` / DLSS-G fail to load: one log line, the reason in `f5lowGetStatus` (status 3) and an error from CreateContext; nothing else
    runs. Otherwise the context is an `xdlssg::Ctx` (not `Own`); `f5lowGetStatus`
    says 1 with `dlssg generator (...)`. `XEFG_CUDA_FORWARD` wins over everything. `xefgSwapChainGetProperties` reports
    `numFramesToGenerateMax` as read from Streamline at create (5 on an RTX 5070 Ti with NVIDIA 310.9: native, nothing patched; 5 after the
    unlock on the other tiers).
  - *Streamline.* One per process, loaded from a private renamed copy so the game's own Streamline and its upscaler are never
    touched (a build with the original names broke the game's upscaler in NBA 2K27: OptiScaler hooks every module named like
    `sl.interposer.dll` / `sl.common.dll` / ..., one set of originals per process). Search order, first folder with a complete set:
    `F5LOW_STREAMLINE_DIR` (OptiScaler passes its Streamline folder), `DlssgPath=` (f5low.ini) / `XEFG_CUDA_DLSSGPATH`,
    `<f5low.dll folder>\..\streamline` and `..\Streamline` (`OptiScaler\plugins\f5low.dll` -> `OptiScaler\streamline`). A folder with
    the plain names (`sl.interposer.dll`, `sl.common.dll`, `sl.dlss_g.dll`, `sl.reflex.dll`, `sl.pcl.dll`, `nvngx_dlssg.dll`) is copied
    byte for byte under the renamed names (`xefg_slinterposer.dll`, `sl.xefg_common.dll`, `sl.xefg_dlss_g.dll`, `sl.xefg_reflex.dll`,
    `sl.xefg_pcl.dll`, `nvngx_dlssg.dll` unchanged) into `<f5low.dll folder>\f5low_streamline\`, with a manifest of the SHA-256 of
    each source: a changed source is copied again, a deleted or altered copy is made again, an unchanged one is not touched; files are
    written under a temporary name and moved into place. A folder that already has the renamed files is used as it is, but only if
    no plain-named `sl.*.dll` sits next to them (Streamline's plugin scan would load it). Plain-named files are never loaded.
  - *Own nvngx_dlssg only.* DLSS-G must run on the private `nvngx_dlssg.dll` in `f5low_streamline\`, never on a game's own copy (a game
    shipping Streamline 2.11.1 with nvngx_dlssg 310.6 had the GPU fault at level load when DLSS-G ran on it). On every GPU tier f5low
    loads its copy by full path before `slInit` (a different nvngx_dlssg already in the process is logged with both paths and versions).
    After DLSS-G is up it checks that no other `nvngx_dlssg*` module is loaded and that a bare-name lookup resolves to ours; otherwise
    `dlssg` is refused like any other dlssg error (status 3, no fallback): `a different nvngx_dlssg.dll (<path>, <version>) is loaded in the
    game: DLSS-G would run on it`. A driver-downloaded copy under `NGX\models\dlssg\` counts as foreign too. The check also runs when DLSS-G is first switched on and
    a few presents later (NGX may load its module late); a foreign copy found then switches DLSS-G off, with the reason in the log and
    `f5lowGetStatus`. A foreign copy already in the process before ours loads is refused before `slInit`. `F5LOW_DLSSG_ALLOW_FOREIGN=1`
    (environment only; OptiScaler sets it when the player keeps the game's DLSS-G) turns the refusal into a log line. OptiScaler only
    allows the private copy under `<plugins>\f5low_streamline`, so a Streamline folder used as-is through `F5LOW_STREAMLINE_DIR` or
    `DlssgPath` outside it gets blocked when OptiScaler's F5Low DLSS-G block is on. Rule:
    `src/generators/dlssg_own.h`, test `tests\dlssg_own_test.cpp`.
  - *Early load.* OptiScaler calls `uint32_t f5lowPrepareDlssg()` (cdecl export) right after loading f5low.dll when the generator is DLSS-G:
    it makes the private copy and loads our `nvngx_dlssg.dll` before the game's Streamline asks NGX (NGX marks DLSS-G unavailable
    process-wide otherwise). File I/O and LoadLibrary only, safe under the loader lock; returns 1 when loaded, 0 (logged) otherwise. The
    module is NOT unlock-patched there (the RTX 20-40 tier needs the CUDA device behind the game's D3D12 device, known only at context
    time), so on RTX 20-40 the game's own DLSS-G must stay off while F5Low DLSS-G is on.
  - *Pacing.* The host's own pacer (`Own`: clock, gate, flip metering) is not involved. DLSS-G makes the frames (it presents from a
    thread of its own, with the driver's flip metering) and our sink paces them per `SinkPace` (as for `intel`): the game's Presents
    are stamped (the thin wrapper chain), and with `auto` the frames go out on the schedule at 2x (`kAutoMaxBurst`) and by the smoothed
    burst interval above; `schedule` / `burst` force one. `SetBurst(n + 1)` follows what DLSS-G really runs: a frame without inputs is
    burst 1 (out on arrival). Logged once per context: `dlssg: DLSS-G makes the frames, our sink paces them (SinkPace auto|schedule|burst|off)`.
  - *Latency.* Reflex is required and PCL present markers around the game's `Present` (a thin wrapper chain sets them). The driver
    multiplies a Reflex interval by DLSS-G's multiplier, so a shown-frame cap through Reflex must not be divided again: OptiScaler
    asks `xefgCudaDriverMultipliesReflexCap()` (below). XeLL is not used by this generator.
  - *Inputs.* Motion vectors, depth, HUD-less colour and UI are tagged (typeless formats as typed ones in the DLSS-G options), XeFG's
    camera becomes `sl::Constants`, the XeFG frame id is Streamline's frame index. A frame presented without new constants (a game
    in its menu) is shown alone (`eRetainResourcesWhenOff`). DLSS-G generates only while its window is the foreground one.
  - *Switching.* `own` <-> `dlssg` without a restart: destroy the context, change `F5LOW_GENERATOR` / the ini, create again
    (`repro_test` `REPRO_RECREATE_AT` + `REPRO_RECREATE_GEN`); Streamline stays loaded for the life of the process.
  - *Tests.* `tests\repro_test.cpp` with `REPRO_BAR=1 REPRO_FRONT=1 REPRO_FPS=30 F5LOW_SINK_STATS=1 SINK_PROBE=1 XEFG_CUDA_LOG=1` (the
    DLL logs the sink's gaps, copy time and the moving bar's steps to f5low.log when the context is destroyed); `tests\dlssg_spike.cpp`
    (`build_dlssg_spike.ps1`) is the same check without the DLL. The exe and the NVIDIA files stay outside the repo.
  - *The unlock (RTX 20/30/40, `src/unlock/`).* OptiScaler's built-in MFG unlock, ported (the headers are the ones of OptiScaler-NR
    `OptiScaler/framegen/mfgunlock` at 745e49431, each file says what differs) and applied to f5low's PRIVATE copy only, in memory, before
    `slInit`: f5low loads `<f5low_streamline>\nvngx_dlssg.dll` and `sl.xefg_dlss_g.dll` itself, by full path, and patches those two module
    handles; Streamline's own load of the same paths gets the same handles (logged). The game's own DLSS-G modules (any other handle of
    those base names) are listed in the log and never touched. Nothing is written to disk. The tier comes from the GPU's compute capability
    (`F5LOW_TEST_GPU_SM=NN` in tests picks any tier on any card, and forces the families the way OptiScaler's RTX 20/30 "On" mode does):

    | GPU | tier | what is patched in our copy |
    |---|---|---|
    | RTX 50 and newer (sm >= 100) | none | nothing: DLSS-G generates up to 5 natively |
    | RTX 40 (sm 89) | Ada | the advertise and validate gates of nvngx_dlssg, the sl.dlss_g frame-count clamp and flip metering, the Ada midpoint fix in the kernel containers |
    | RTX 30 (sm 80-88) | Ampere | the Ada set minus the midpoint fix, plus the architecture the module reports (`NVSDK_NGX_GetGPUArchitecture` and the three `*_GetFeatureRequirements` minimums) and every kernel container retargeted to sm_86 |
    | RTX 20 (sm 75-79) | Turing | the Ampere architecture sites with Turing's value, and the kernels rewritten to sm_75 PTX text as they are loaded (CUDA module-load hook, below) |

    All sites are found structurally (unwind functions, export names, instruction shapes; no addresses, no NVIDIA bytes in this repo) and
    the transaction is all or none across both modules: if a family does not locate (an unknown nvngx_dlssg or sl.dlss_g build), nothing is
    written and the context fails with `this DLSS-G build is not recognised by the unlock: <family>` (status 3, no fallback); on RTX 50
    nothing is located or patched at all. After the unlock `numFramesToGenerateMax` is 5 (6x) on every tier. Log lines start with `unlock:`.
    The RTX 20 hook is a small inline hook of our own on nvcuda's `cuModuleLoadData`, `cuModuleLoadFatBinary`, `cuModuleLoadDataEx` and
    `cuLibraryLoadData` (it follows nvcuda's export stubs to the functions behind them, which is what `cuGetProcAddress` hands out); it
    rewrites a load only when the container lies inside OUR nvngx_dlssg, everything else passes through to the real function. It is
    refused (status 3) on a driver whose functions have a prologue it cannot copy. Tests: `tests/build_mfgunlock_tests.ps1`
    (`mfgunlock_core_smoke` on synthetic images, `mfgunlock_check`, `mfgunlock_fatbin_refs`, `mfgunlock_turing_probe` and `unlock_hook_probe` on
    the player's own `nvngx_dlssg.dll` / `sl.dlss_g.dll`, passed as arguments and never copied into the repo).
  - *The NVAPI kernel path (RTX 20/30, `src/unlock/NvapiKernels.h`, `NvapiFatbin.h`).* DLSS-G makes no CUDA calls: it creates its ~80 kernels
    through NVAPI (`nvapi_QueryInterface` id 0xAD1A677D, device + image + size) and launches them as D3D12 compute work. Every image is a CUDA
    fatbin container with PTX for Ada/Blackwell, which an RTX 20/30 driver cannot use (the CUDA module-load hook above never fires). Nothing
    global is hooked: our private nvngx_dlssg and sl.dlss_g import `GetProcAddress` from kernel32 (read from their import tables), and the
    slot of THOSE TWO modules is pointed at a function that answers the name `nvapi_QueryInterface` with a filter; the filter returns the real
    answer for every id except 0xAD1A677D, where it returns a wrapper. So nvapi_QueryInterface itself is never patched (OptiScaler Detours-hooks
    it: a code patch of ours would clash), no other caller is touched, and no caller check is needed. The wrapper replaces the container by a
    new one with a single, uncompressed PTX image for the card, built from the shipped one's own headers: RTX 30 the source PTX with
    `.target sm_86`, RTX 20 the same through the sm_75 rewrite of `PtxTuring.h`. The source is the Blackwell PTX when the container has one,
    else the Ada PTX, or the sm_86 PTX the Ampere family already retargeted in memory. A container that cannot be rewritten, or that the driver
    refuses, goes to the real create as shipped (counted; the first reason is logged); the create is repeated only when the first call
    returned an error without writing the output handle. The new containers stay alive for the process. The hook is placed before the patches
    are applied and a failure to place it fails the unlock like the CUDA loaders do (status 3); f5low.dll is pinned. RTX 40 and newer: no hook
    (`F5LOW_NVAPI_COUNT=1` installs it count-only, for tests; `F5LOW_NVAPI_DISABLE=1` leaves it out, `F5LOW_TEST_E9_QI=1` first puts a
    Detours-style E9 hook on nvapi_QueryInterface). One log line at the end: `dlssg kernels: <tier> path, N of M retargeted|rewritten, K passed
    through, F failed (<first reason>), ..., took X ms` (the driver's JIT happens inside the create, so X is mostly JIT: a second start is
    faster through the driver's cache), or `hook installed, 0 kernel creations seen`. Test without a GPU: `tests/build_nvapi_fatbin_test.ps1`
    (synthetic containers, the create flow with a fake driver call).
  - *Credits.* The sl.dlss_g frame-count clamp and software-pacing approach is adapted from KleberMotta/OptiScaler-DLSS5-MFG-RTX40 (MIT, from
    the RenoDX MFG Unlock addon by Dreamt and mavismmg): the notice is in `licenses/MFGUnlock_LICENSE.txt`.
  - *Later.* Story 2b: kernels tuned for RTX 20/30; story 3: tester proof on real RTX 40/30/20 cards; story 4: OptiScaler side (generator
    choice, Reflex cap, Streamline hand-over). Nothing NVIDIA-derived is ever written to disk or committed (`tools/audit.py` has an `nvidia`
    rule).
- **fsr**: FidelityFX FG from the DLLs OptiScaler ships (path from OptiScaler). Frames-only shape through the
  dispatch (`outputs[]`, `numGeneratedFrames`, `frameID` +1 each frame); the sink shape only if the dispatch path
  turns out unusable on its own. Opens F5Low on AMD and Intel GPUs.

## Not in the interface on purpose

- Vulkan: the host is D3D12 (D3D11 and Vulkan games reach it through OptiScaler's bridges).
- Upscaling: generators never upscale; OptiScaler does that before the frame reaches the host.

## Motion and depth formats

Which DXGI formats the tagged motion vectors and depth may have, **measured** against Intel's library (libxess_fg 1.3.4.95 with
libxell 1.3.2.10, through f5low's `intel` generator), not read from its guide ("R16G16_FLOAT or similar", "any depth format").
A game log (Enshrouded through OptiScaler's Vulkan bridge) tagged R16G16_SNORM: Intel generated, the own generator
ignored the tag, generated nothing and still told XeLL "frame generation on" (the game's frame rate fell by about half).

*Probe.* `tests\repro_test.cpp` with `REPRO_MVFMT=<DXGI format number>` / `REPRO_DEPTHFMT=<n>`: the texture is made and filled in
that format, the value encoded the way a typed read decodes it. The moving bar of `REPRO_BAR=1` has motion -8 px, so the encoded
sample times the motion scale tagged in the frame constants is -8: floats hold -8 (scale 1), a normalized format holds +-0.5 with
scale 16 (negative for the unsigned ones, which cannot hold a minus; `REPRO_MVSCALE=8` makes it +-1.0, exact in every encoding), an
integer format holds -8 (or 8 with scale -1). Run: `F5LOW_GENERATOR=intel XEFG_CUDA_INTEL_LIBRARY=<libxess_fg.dll> REPRO_FRONT=1
REPRO_BAR=1 REPRO_OFF=60 REPRO_ON=200 REPRO_INTERP=2 REPRO_HIST=1 REPRO_LOGLEVEL=0 F5LOW_SINK_STATS=1 SINK_PROBE=1 XEFG_CUDA_LOG=1
repro_test.exe f5low.dll libxell.dll` (no PresentMon). What it records: Intel's `TagFrameResource` result, everything its logging
callback says at debug level, `GetLastPresentStatus` (frames presented, generation on) and the sink probe's steps between the shown
frames: 4 px per shown frame everywhere in the baseline run (repro_test's own inputs: motion R16G16_TYPELESS (33) holding half floats,
depth R32G8X24_TYPELESS (19)), so any other step list means the generated frames sit in
the wrong place. `REPRO_NOFILL=1` makes a texture in any format number and never fills it: what the tag accepts, no picture.

*What Intel does.* It never refuses a format: `TagFrameResource` returned 0 for every one below (also BC1, sRGB, B8G8R8A8 as motion; integer
and colour formats as depth), and its log said nothing but "Reset called" / "Required descriptor size". It reads the texture
through a typed view as float, so what a float view decodes works with the application's motion scale: typeless as float, UNORM to
[0, 1], SNORM to [-1, 1]. The integer formats are accepted too, but their integers are read as float bit patterns: wrong motion.

| Motion format (number) | Intel | Own generator now |
|---|---|---|
| R16G16_TYPELESS / FLOAT (33, 34) | generates, steps 4 px | copied as it is (the pack's layout) |
| R16G16_UNORM (35) | generates, steps 4 px | converted (was: copied as if half floats = wrong motion) |
| R16G16_SNORM (37) | generates, steps 4 px | converted (was: ignored, nothing generated) |
| R16G16B16A16_TYPELESS / FLOAT / UNORM / SNORM (9, 10, 11, 13) | generates, steps 4 px | converted |
| R32G32_TYPELESS / FLOAT, R32G32B32_TYPELESS / FLOAT, R32G32B32A32_TYPELESS / FLOAT (15, 16, 5, 6, 1, 2) | generates, steps 4 px | converted |
| R8G8_TYPELESS / UNORM / SNORM (48, 49, 51) | generates, steps 4 px | converted |
| R8G8B8A8_TYPELESS / UNORM / SNORM (27, 28, 31) | generates, steps 4 px | converted |
| R10G10B10A2_TYPELESS / UNORM (23, 24) | generates, steps 4 px | converted |
| R11G11B10_FLOAT (26) | generates, steps 4 px | converted |
| R32_FLOAT, R16_FLOAT, R8_UNORM (41, 54, 61): one channel, x only | generates (y reads 0, the probe's motion is horizontal) | refused (a vector needs two channels) |
| R16G16_UINT, R32G32_UINT, R16G16B16A16_UINT, R8G8_UINT (36, 17, 12, 50; the other UINT formats were not probed) | accepts, generated frames not moved (steps 3 / 4 / 5 px) | refused |
| R16G16_SINT, R32G32_SINT, R16G16B16A16_SINT, R8G8_SINT (38, 18, 14, 52; the other SINT formats were not probed) | accepts, garbage motion (the bar is missing from most shown frames) | refused |
| R8G8B8A8_UNORM_SRGB, B8G8R8A8, BC1 (29, 87, 71) | accepts (no picture checked) | refused |

| Depth format (number) | Intel | Own generator now |
|---|---|---|
| D32_FLOAT, R32_FLOAT, R32_TYPELESS (40, 41, 39) | generates | copied as it is |
| R32G8X24_TYPELESS, D32_FLOAT_S8X24_UINT (19, 20) | generates | plane 0 copied as it is (a region in a larger one: whole copy, read at its offset) |
| R24G8_TYPELESS, D24_UNORM_S8_UINT (44, 45) | generates | plane 0 converted (24-bit UNORM to float) |
| R16_TYPELESS, D16_UNORM, R16_UNORM (53, 55, 56) | generates | converted (16-bit UNORM to float) |
| R16_FLOAT (54) | generates | converted |
| R32_UINT, R32_SINT, R16_UINT (42, 43, 57) | accepts | refused |

The probe's picture does not depend on the depth (the bar on a flat ground has no disocclusion to fill), so for depth "generates" is
the tag and the status, not a check of the decoding; the own generator's depth conversions are checked against exact values in
`tests\convert_test.cpp` and give the same generated frames as the 32-bit float run in `tests\format_check.ps1`.

*What f5low does.* `src\xefg_formats.h` is the table (host code, tested in `tests\convert_test.cpp`), `src\xefg_convert.hlsl` the
conversion (a normalized format is decoded as a typed read would, to the pack's R16G16_FLOAT; the motion scale the application tags
multiplies it afterwards, as with Intel). A format it refuses is **logged once per format change** ("tag motion: motion vectors in
DXGI format 36 (R16G16_UINT) are not supported - no frames generated while they are tagged"; the same for a texture whose size or
region does not fit the pack), `TagFrameResource` still returns success (as Intel's does for everything), the present reports
`framesPresented` 1 and `isFrameGenEnabled` 0, and XeLL is told `SetFgEnabled(0)` until a tag it can use arrives (then on again; the
log says so). Pacing is not touched. Run `tests\format_check.ps1 -IntelLib <libxess_fg.dll>` (GPU) to check all of this: the generated frame of
every taken format is bit for bit the baseline's (motion R16G16_TYPELESS, depth R32G8X24_TYPELESS); a frame's tags decide once per frame at
Present, a refused state ends after two usable frames, and the warning is logged once per format change (`StepInputs` in `src\xefg_formats.h`).

## Exports that are not Intel's API

- `int __cdecl f5lowGetStatus(char* reason, int reasonSize)` - what the host decided for the most recent context
  (`xefgSwapChainD3D12CreateContext`). Returns `0` not decided yet (no context created) or the own generator is still being built in the background (reason
  "building its frame generator in the background": the frames pass through, `1` follows by itself), `1` a generator of ours runs (the own
  generator, or `dlssg generator (...)` in the reason), `1` also when `Generator=dlssg` could not run and `own` does (the reason then
  starts `dlssg unavailable: <why> - `),
  `2` handed over to Intel's `libxess_fg` (forwarding), `3` unavailable (own generator failed and no `libxess_fg.dll`
  to forward to). `reason` (may be null; used when `reasonSize > 0`) receives a NUL-terminated one-line UTF-8 text, cut to
  fit, for example `own generator (pack built in memory), D3D12` or
  `own generator unavailable: <what is missing> - forwarding to Intel XeFG`. Thread-safe, callable before any context;
  the state stays after the last context is destroyed.
- `uint32_t xefgCudaXellCountsRenderedOnly()` - XeLL cap accounting (see `src/xefg_cuda.cpp`); 1 also while a dlssg context runs.
- `uint32_t xefgCudaDriverMultipliesReflexCap()` - OptiScaler's Reflex frame-cap fix asks at each cap decision: 1 while a
  dlssg context runs, or an own context is about to send or holds the frame generation multiplier for the driver (`DriverPacing=2` or
  `DriverFg`: from just before the first Reflex Sync; `DriverPacing=1`: from the arming of the signals to the end of their disarming)
  - the NVIDIA driver then multiplies a Reflex interval by that multiplier itself (seen in one game), so the caller gives Reflex the
  shown-frame cap undivided - else 0. A dlssg run whose OptiScaler never asked logs a warning after 600 presents.
