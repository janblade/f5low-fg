# Generators

f5low.dll is a frame generation **host**. OptiScaler drives it through Intel's XeSS-FG swap chain API (the
`xefgSwapChain*` exports, headers from Intel's public SDK). Behind that API the host owns the parts every generator
shares: the swap chain the game sees, frame input staging, pacing, presentation, latency reporting (XeLL calls),
the UI/letterbox rules. A **generator** only makes the in-between frames.

Three generators run: **own** (XeFG Tensor), **dlssg** (NVIDIA DLSS-G through a private copy of the player's Streamline,
RTX 20 and newer, 2x to 6x on every tier: native on RTX 50, unlocked in memory on RTX 20/30/40, see "dlssg" below) and **intel**
(Intel's libxess_fg from a private copy, unlocked in memory to 6x, see "intel" below). `fsr` is registered and selectable, reports
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
| Paces itself | no (host) | yes, on the chain's latency waitable | yes (driver flip metering, Reflex) | yes when its swap chain is used; no when we dispatch |
| Max generated frames | pack/recipe (5) | `xefgSwapChainGetProperties` (3 native on 1.3.4.95) | `DLSSGState::numFramesToGenerateMax` (1-5 by GPU) | `numGeneratedFrames` into `outputs[4]` (version dependent) |
| Latency partner | XeLL calls by host | XeLL (its own) | Reflex (required) | Anti-Lag 2 optional |
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

- **intel** (built, `src/generators/intel.h`, `src/unlock/FamilyXeFG.h` + `UnlockXeFg.h`): Intel's libxess_fg makes the frames, our sink
  paces them. Present-sink shape, spaced mode. Any D3D12 GPU Intel's library runs on.
  - *Routing.* `Generator=intel` in `D3D12CreateContext` builds an `xintel::Ctx` (not `Own`). The handle is the library's own handle; every XeFG
    entry point of `xefg_cuda.cpp` calls the PRIVATE library for such a handle (`FORWARDH`), the other handles still go to the shared
    `Real()` library. **No fallback**: when the library cannot start (no libxess_fg found, copy or load failed, CreateContext refused),
    `f5low: generator intel not available (<reason>) - frame generation is off (no fallback to own)`, `f5lowGetStatus` 3
    `intel unavailable: <reason>`, `XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE`.
  - *Library.* No copies on disk. It is the SAME libxess_fg.dll the own generator reads (`RealLibraryPath` in `xefg_cuda.cpp`:
    `XEFG_CUDA_INTEL_LIBRARY`, next to f5low.dll, the folder above = OptiScaler's), loaded by full path and logged. A source that is f5low
    itself (our own path, or a module exporting `f5lowGetStatus`) is never loaded.
  - *Unlock.* `unlock/FamilyXeFG.h` (OptiScaler-NR's family, U1-U5 on libxess_fg, XL1 on libxell, structural locators) applied by
    `unlock/UnlockXeFg.h` right after f5low loaded the library and BEFORE the first XeFG call: Intel sizes every per-generated-frame buffer
    from U3 (its default interpolation ceiling) when a context is created, so a late patch leaves 3-slot buffers (evidence in the ai-os
    2026-11-03 study). If libxess_fg.dll was ALREADY loaded in the process before f5low (a context may exist), nothing is patched: Intel's
    own maximum, with the reason in the log. All or none; an unknown build is refused with the failing site and the library then runs
    unpatched at its own maximum (on an RTX 5070 Ti with 1.3.4.95 and libxell 1.3.2.10 that is 2x: GetProperties says 1; with the
    unlock it says 5 = 6x). A failed rollback (a partially patched module) refuses the library outright: status 3. The count is 3 to 5.
    `F5LOW_INTEL_UNLOCK=0|off|false|no` turns it off (tests); `F5LOW_TEST_XEFG_SKIP=U4` leaves one site unpatched (tests). This patches
    inside Intel's DLLs in memory; nothing is written to disk.
  - *libxell.* libxess_fg does not import libxell: it calls `GetModuleHandleExA(0, "libxell.dll")` at run time, so it uses the libxell the
    process already has (OptiScaler's or the game's) = the one that made the XeLL context OptiScaler hands over. XL1 is patched in that
    module in memory (one log line says so; it only lifts libxell's refusal of generated-frame counts above 3). If no libxell.dll is loaded
    when the context is created, the one next to libxess_fg is loaded by full path. If the XL1 site is not found (or a libxell was patched
    before), XL1 and the libxess_fg unlock are skipped together. Intel reports the generated frames to XeLL itself every frame, so
    `xefgCudaXellCountsRenderedOnly` stays 0.
  - *Sink.* `InitFromSwapChainDesc` hands Intel a `SinkFactory` (spaced mode); `SetEnabled` / `SetNumInterpolatedFrames` set the sink's burst
    (generated + 1; the factory remembers it, so a re-created chain starts with it). `InitFromSwapChain` (a chain the game made) cannot be sunk: the library presents on it itself (logged).
  - *Pacing today.* The sink's burst spacing (the same as dlssg's). Not built: driving the release times from the own generator's schedule
    (frame k of an app frame at base + lead + (k - 1) * step, `PaceLead` auto, `pace_lead.h`). It would take: (1) a wrapper chain on the game's
    side (like dlssg's `MarkerChain`) that stamps the app Present time and the app interval; (2) a schedule entry in the sink replacing its
    burst-interval spacing (`sTicks`): release time of burst frame j = stamp + lead + j * interval / (n + 1), reset on resize / menu / n change;
    (3) a feed for `PaceLead`: the sink knows when each staging copy finished (its fence), so "late" = copy done after the planned release,
    the same event Own gives it; (4) a harness A/B (`pacer_ab.ps1 -Gens own,intel`, which today only knows own and dlssg).
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
  - *Pacing.* The host's own pacer (`Own`: clock, gate, flip metering) is not involved. DLSS-G paces its own frames (it presents from a
    thread of its own, with the driver's flip metering) and the sink queues each present and shows it on the real chain, spacing a full
    burst by the smoothed burst interval (`SetBurst(n + 1)` from the multiplier). Logged once per context: `dlssg: DLSS-G paces its own
    frames (host pacer handed over)`.
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
- `uint32_t xefgCudaDriverMultipliesReflexCap()` - OptiScaler's Reflex frame-cap fix asks once when it loads the library: 1 while a
  dlssg context runs (the NVIDIA driver multiplies a Reflex interval by DLSS-G's multiplier itself, so the caller gives Reflex the
  shown-frame cap undivided), else 0. A dlssg run whose OptiScaler never asked logs a warning after 600 presents.
