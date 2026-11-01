# f5low-fg

Frame generation host behind OptiScaler-NR's **F5Low** FG output. Our own pacer, gate and presenter, with
pluggable frame generators:

| generator | makes the frames | GPUs |
|---|---|---|
| own (XeFG Tensor) | our CUDA/D3D12 kernels; Intel's modules and weights taken from the player's `libxess_fg.dll` | NVIDIA |
| intel | Intel's `libxess_fg.dll` | any |
| dlssg | NVIDIA DLSS-G through Streamline | NVIDIA |
| fsr | AMD FidelityFX frame generation | any |

Every vendor library is loaded at runtime from the player's PC. This repo holds only our own code.

Status: being set up (private). Layout planned:

```
src/core/        XeFG-API surface OptiScaler drives, pacer, gate, presenter, interop, log/ini/version
src/generators/  own/ intel/ dlssg/ fsr/
tools/           recipe builder, audit, harness
```

## Rule: no vendor content

Never commit a vendor binary (`libxess_fg.dll`, Streamline, `nvngx_*`, FidelityFX DLLs) or anything derived
from one: extracted or translated shaders (DXBC, DXIL, PTX made from them), zstd dictionaries, weights, captured
buffers, disassembly, RVAs or byte patterns. Intel modules are referred to by SHA-256 only. The audit in
`tools/` enforces this in CI.
