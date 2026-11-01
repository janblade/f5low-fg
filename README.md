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
buffers, disassembly, RVAs or byte patterns. ("Byte patterns" means sequences copied from a vendor binary; the generic
instruction encodings our scans recognise, such as `mov [rsp+disp], imm`, are not.) Intel modules are referred to by
SHA-256 only. The audit in
`tools/` enforces this in CI.

## Recipe (own generator's graph pack)

`recipe/i0/` is our half of the own generator's pack: `manifest.tmpl` (the manifest, generated file names as
`@name`) and `parts.json` (how each file is made; the nested `hdr/` pack alike). `tools/kernels/` holds our
generators and HLSL templates. Intel modules appear only as SHA-256 of the decoded module, weights as upload
ordinal + size + SHA-256. `tools/recipe/build_pack.py` recombines the recipe with the Intel parts of a local
`libxess_fg.dll` and a weight capture (offline, for now); `compare_pack.py` checks a built pack against a reference;
`classify.py` derives a recipe from a finished pack (dev tool, needs the old libxess-weights checkout). Built packs
are Intel-derived and stay out of the repo.

## Licence

GPL-3.0, the same as OptiScaler (see LICENSE). Vendor libraries are never part of this repo; they are loaded
at runtime from the player's PC.
