# Generators

f5low.dll is a frame generation **host**. OptiScaler drives it through Intel's XeSS-FG swap chain API (the
`xefgSwapChain*` exports, headers from Intel's public SDK). Behind that API the host owns the parts every generator
shares: the swap chain the game sees, frame input staging, pacing, presentation, latency reporting (XeLL calls),
the UI/letterbox rules. A **generator** only makes the in-between frames.

Two generators run: **own** (XeFG Tensor) and **dlssg** (NVIDIA DLSS-G through a private copy of the player's Streamline,
RTX 20 and newer, 2x to 6x on every tier: native on RTX 50, unlocked in memory on RTX 20/30/40, see "dlssg" below). `intel` and `fsr`
are registered and selectable, report "not available yet" in the log, and run `own`; their design is below (see "What is ready").

## Choosing a generator

`F5LOW_GENERATOR` (env, set by OptiScaler before each `xefgSwapChainD3D12CreateContext`), else the old name
`XEFG_CUDA_GENERATOR` (env), else `Generator=` in f5low.ini (`GeneratorSetting()` in `src/xefg_cuda.cpp`, next to the
other settings). Values, case insensitive: `own` (default), `intel`, `dlssg`, `fsr`; anything else logs
`f5low: generator "<value>" is unknown (own, intel, dlssg, fsr) - running own` and runs `own`.

A generator that is not available logs one line and the host runs `own`: `f5low: generator <name> not available yet, running own`
for `intel` and `fsr` (`src/generators/stubs.h`, `GenCaps::available = false`). **`dlssg` never falls back**: when the player picks it and it
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

- Ready: `own` behind `IGenerator` / `IPackGenerator`; the selection (above); `dlssg` (present sink, see below); `intel` and `fsr`
  registered as "not available yet" (`GenCaps` only) and falling back to `own`.
- Not ready: frames-only backends (`fsr`). The host only knows pack-layout buffers; a backend that takes and returns textures
  needs a texture in/out path in `CopyFrame` / `Show` / `PresentGen` next to them, and a `Generate(GenInputs, t[], out[])`
  entry on `IGenerator`.
- Ready for dlssg, not for intel: the present-sink route. A dlssg context is a separate kind of context at the XeFG entry points of
  `src/xefg_cuda.cpp` (next to the forwarding-to-Intel route: `FindDlssg(h)` in every entry point), not a `class Own` /
  `IPackGenerator`; `own` runs through exactly the code it ran through before. `intel` will reuse `src/sink/` and the same kind of
  entry-point branch, with its own sink mode (spaced bursts); its design is the per-backend note below (story A2).
- Not ready: the frames-only shape (`fsr`), see above. Stories A3-A4 will shape it.

## Per backend, when it is built (stories A2-A4)

- **intel**: libxess_fg from `XEFG_CUDA_INTEL_LIBRARY` (OptiScaler sets it to XeFGPath or its own copy), then our
  folder, then the folder above. Sink shape. Max = what Intel reports; our unlock never patches this copy (decided
  in OptiScaler's F5Low plan). XeLL: the library's own calls pass through.
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
