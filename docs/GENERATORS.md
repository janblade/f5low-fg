# Generators

f5low.dll is a frame generation **host**. OptiScaler drives it through Intel's XeSS-FG swap chain API (the
`xefgSwapChain*` exports, headers from Intel's public SDK). Behind that API the host owns the parts every generator
shares: the swap chain the game sees, frame input staging, pacing, presentation, latency reporting (XeLL calls),
the UI/letterbox rules. A **generator** only makes the in-between frames.

Today one generator runs: **own** (XeFG Tensor). `intel`, `dlssg` and `fsr` are registered and selectable, report
"not available yet" in the log, and run `own`. This note is the paper check that the interface already fits them,
so adding one later is a backend, not a host change.

## Choosing a generator

`F5LOW_GENERATOR` (env, set by OptiScaler before each `xefgSwapChainD3D12CreateContext`), else the old name
`XEFG_CUDA_GENERATOR` (env), else `Generator=` in f5low.ini (`GeneratorSetting()` in `src/xefg_cuda.cpp`, next to the
other settings). Values, case insensitive: `own` (default), `intel`, `dlssg`, `fsr`; anything else logs
`f5low: generator "<value>" is unknown (own, intel, dlssg, fsr) - running own` and runs `own`.

A generator that is not available logs one line, `f5low: generator <name> not available yet, running own`, and the host
runs `own`. Today that is `intel`, `dlssg` and `fsr` (`src/generators/stubs.h`, `GenCaps::available = false`). Once they
exist the same line, with the reason, also covers a generator that cannot start (library missing, unknown version, init
failure, wrong GPU); on a GPU `own` cannot run on, the host then offers no frame generation. The generator that runs is
logged once at context creation: `f5low: generator own (XeFG Tensor)`.

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

Every library falls in one of two shapes. The interface carries both, so no backend needs a host change.

**Frames only** (`own`, `fsr` through its dispatch): the host calls the generator once per app frame with the
inputs and the interpolation points `t = k / (n + 1)`, and gets n frames back on the GPU. The host paces and
presents them exactly as today.

**Present sink** (`intel`, `dlssg`, and `fsr` if its swap chain turns out to be needed): the library insists on
presenting. The host gives it a **sink**, a proxy swap chain and factory of ours, and every `Present` the library
makes becomes a job for our pacer, shown on the real swap chain. The library's own pacing is neutralised by
handing it an always-signalled latency waitable, so its burst arrives back to back and our pacer spaces it.
Proven with Intel 1.3.4.95 in the harness (libxess-weights, 2026-10-29/31, sink code not yet imported): real-chain
present gaps at 6x / 30 fps sd 0.29 ms, p99 6.15 ms (Intel presenting itself: sd 2.10, p99 11.86); resize,
exclusive full screen in and out, and a menu stretch (bursts split by arrival gap) all pass.

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

The struct shape above was the target. The real interface is `src/generator.h` (`IGenerator`, `GenCaps`, `GenBuf`,
`GenSlot`): it follows the code of `own` (`src/generators/own.h`, class `OwnGenerator`), the only generator that runs.
The host (`Own` in `src/xefg_own.h`) reaches it only through that interface and owns it as a
`std::unique_ptr<IGenerator>`. What the real interface adds to the sketch: the generator hands the host its input buffers
(`Buf(GenSlot)`; the host copies the game's frame, motion and depth into them); a pass is either recorded into a command
list of the host's (`PreparePass`, `RecordPass`, `RecordPassInto`: the D3D12 path) or started on a CUDA stream that
signals a fence (`Launch`, `DoneFence`, `RingFrame`: the CUDA path); `Build` / `Rebuild` / `Shutdown` replace
`Init` / `Resize`. `AttachSink` stays a default-returning-false virtual for the present-sink backends.

## Per backend, when it is built (stories A2-A4)

- **intel**: libxess_fg from `XEFG_CUDA_INTEL_LIBRARY` (OptiScaler sets it to XeFGPath or its own copy), then our
  folder, then the folder above. Sink shape. Max = what Intel reports; our unlock never patches this copy (decided
  in OptiScaler's F5Low plan). XeLL: the library's own calls pass through.
- **dlssg**: Streamline from the player's install, loaded as a renamed private copy so the game's own Streamline
  (and its upscaler) is never touched (a build that used the original names broke the game's upscaler). Sink
  shape. Reflex required: the host keeps the driver's interval rule (the driver multiplies the Reflex interval by
  DLSS-G's multiplier; no per-rendered-frame division). Switching own <-> dlssg without restart.
- **fsr**: FidelityFX FG from the DLLs OptiScaler ships (path from OptiScaler). Frames-only shape through the
  dispatch (`outputs[]`, `numGeneratedFrames`, `frameID` +1 each frame); the sink shape only if the dispatch path
  turns out unusable on its own. Opens F5Low on AMD and Intel GPUs.

## Not in the interface on purpose

- Vulkan: the host is D3D12 (D3D11 and Vulkan games reach it through OptiScaler's bridges).
- Upscaling: generators never upscale; OptiScaler does that before the frame reaches the host.
