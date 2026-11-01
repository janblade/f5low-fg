# f5low-fg

Frame generation host behind OptiScaler-NR's **F5Low** FG output. Our own pacer, gate and presenter, with
pluggable frame generators:

| generator | makes the frames | GPUs |
|---|---|---|
| own (XeFG Tensor) | our kernels (D3D12 command lists + NVAPI cubins); Intel's native modules, weights and root constants read from the player's `libxess_fg.dll` on first use (no translation, no `dxcompiler.dll`) | NVIDIA |
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
ordinal + size + SHA-256. At run time `f5low.dll` rebuilds the pack from the player's `libxess_fg.dll` (decode, find
modules by hash, weights and root constants from the file) and caches it in `f5low_cache\` next to the DLL (`%LOCALAPPDATA%\f5low\f5low_cache\` when that folder is not writable); the
pack-less pack runs on the D3D12 path only (`docs/PACKLESS.md`). `tools/recipe/build_pack.py` recombines the recipe with
the Intel parts of a local `libxess_fg.dll` and a weight capture offline (a dev pack with PTX twins, both paths);
`compare_pack.py` checks a built pack against a reference (`--packless` for the runtime one);
`classify.py` derives a recipe from a finished pack (dev tool, needs the old libxess-weights checkout). Built packs
are Intel-derived and stay out of the repo.

## Licence

GPL-3.0, the same as OptiScaler (see LICENSE), with an additional permission under section 7
(LICENSE-EXCEPTION.md): f5low may be combined and conveyed with Intel's XeSS libraries (`libxess_fg.dll`,
`libxell.dll`, `libxess.dll`) and NVIDIA / AMD vendor runtime libraries without those libraries falling under the GPL.
Vendor libraries are never part of this repo; they are loaded at runtime from the player's PC and keep their own
licences. `tools/make_kit.ps1` builds the release kit (f5low.dll + Intel's `libxess_fg.dll` + both licences).

## Exports

Besides Intel's `xefgSwapChain*` API: `f5lowGetStatus` (which generator runs and why) and
`xefgCudaXellCountsRenderedOnly`; see docs/GENERATORS.md.
