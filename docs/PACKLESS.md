# Pack-less runtime: where each Intel-derived slot comes from

The own generator (`Generator=` F5Low / xefg_cuda) needs, besides our kernels and the model weights, 26 kernels per
pack (SDR and `hdr\`) that are made from Intel's shader modules. The release must not carry those, so f5low.dll
gets them from the player's own `libxess_fg.dll` when it first needs them.

## Pipeline

1. **Decode** (`src/intel_modules.cpp`, vendored zstd decompressor in `third_party/zstd`). The DLL is read as a
   file, never loaded. The two zstd dictionaries and the magicless module frames are found from zstd structure
   (the dictionary magic, then the dictionary id); each DXIL frame is restored to a DXBC container. About 45 ms
   for the 186 containers (144 distinct) of libxess_fg 1.3.1.78 and 1.3.4.95. A file that is not such a DLL fails
   cleanly (`Open` returns false, a log line says why).
2. **Find by hash.** The recipe (`recipe/i0/parts.json`, `recipe/i0/hdr/parts.json`) names a module only by the
   SHA-256 of its restored container, never by position. All 40 distinct modules the recipe needs are in both
   builds. A build where one is missing is not an i0 build we know: the own generator reports itself unavailable.
3. **Per slot, one of two routes** (own kernels are a later speed-up, see below).
   - **native** (16 modules per pack): Intel's container runs as it is on D3D12 (the pack's
     `native <ptx> <dxbc> [32]` line: Intel's own root signature, descriptors from the manifest's binds; `32` = u0 is
     a raw buffer written with a one-texel border, which the pack pads). Nothing to build.
   - **translate in-process** (the 10 modules / 8 slots that cannot run native, plus the PTX twins of the native
     ones for the CUDA path = 26 `intel_xlate` entries per pack): `src/dxc_disasm.cpp` asks `dxcompiler.dll` for the
     module's disassembly text, `src/translate.cpp` turns the text into PTX, on the player's PC, with the same
     rules and the same output as the offline tool (see below). Performance is the current pack's, because the
     kernels are the same text.
4. **Build and cache** (`src/pack_build.cpp`, story B2 step 3). `BuildPack` writes a pack folder equal byte for byte
   to `tools/recipe/build_pack.py`'s neutral-name output (`tools/recipe/compare_pack.py`: 460 files, 0 differences);
   `EnsurePack` keeps it in `<DLL dir>\f5low_cache\<key>\`. The key is the SHA-256 of (libxess_fg file, the embedded
   recipe + our kernels, dxcompiler file version; not the data source, every blob is checked against the recipe's
   SHA-256 so any source builds the same pack). The pack is built in a temporary folder next to
   it, a `complete` marker with the file count is written last and the folder is renamed in one step; a folder
   without a matching marker (killed build, damaged) is replaced.

### What is embedded in f5low.dll (ours only)

`build.ps1` runs `tools/recipe/build_own.py`: our generators (`tools/kernels`) and our HLSL templates (dxc at BUILD
time, then `src/translate.cpp` through `tools/xlate_tool.cpp`) produce the `own_gen` / `own_tpl` PTX of both packs,
and the noise texture is drawn from the recorded seed. They go with the recipe into one plain-text resource (102,
format in `build_own.py`). `build\own_blobs.txt` lists the SHA-256 of every file in it. `tools/audit.py --dll`
finds the container inside the DLL by its structure and fails any file not listed; PTX text anywhere else in the DLL
fails unless it is one of the two hand-written utility kernels. So Intel-translated PTX can never ride along. Build needs `dxc.exe` (`-Dxc`, `XEFG_DXC`, the WinGet DirectX.ShaderCompiler package or the Windows SDK) and
Python with numpy.

### Loading order (`src/xefg_cuda.cpp`, `PackDir`)

`XEFG_CUDA_PACK` / `xefg_pack\` next to the DLL, then a pack embedded in the DLL (personal builds, resource 101),
then the cache, then a build now. A build needs the player's `libxess_fg.dll` (`XEFG_CUDA_INTEL_LIBRARY` or the usual
lookup), `dxcompiler.dll` next to f5low.dll (`F5LOW_DXCOMPILER`) and a data source (`IntelDataSource`, `pack_build.h`):
the 106 weight uploads and the 44 root-constant blocks come from `IntelFileSource` (next section), no environment
variable needed. Development overrides only: `F5LOW_DEV_CAP` (a PSO capture log) replaces the derived root constants,
`F5LOW_DEV_WEIGHTS` (a weight dump folder, with `F5LOW_DEV_CAP`) replaces everything. Every blob is checked against the
recipe's SHA-256. If anything is missing the log says so in ONE line ("own generator
unavailable: ...") and the DLL forwards to libxess_fg.dll as before.

The build runs synchronously at the first `PackDir()` call (the first `xefgSwapChainGetVersion` /
`D3D12CreateContext` on the game's thread): about 2.1 s once (decode 46 ms, translate 1.45 s, write 0.55 s on the
development PC), logged with its phases. A background build while Intel's library serves the first seconds is a
later improvement. Stale `*.tmp*` folders of killed builds are removed at the start of the next build.

### The weights (`src/intel_file_source.h/.cpp`, story B3 step 2)

`IntelFileSource` rebuilds all 106 weight uploads of the multi-frame model (the `intel_upload` entries) from the
`libxess_fg.dll` FILE: the library is never loaded or run, nothing is patched, no GPU is involved.

- **Weights.** The int8 convolution weights are plain bytes in the file, one blob per layer. The recipe's `decode` key
  (ours) lists each blob by length, SHA-256 and a short probe tag; the source scans the file for the probe tags and
  confirms a candidate by the SHA-256 of the whole blob (a copy shifted by a byte, or a second copy of the same weights,
  cannot pass unless its bytes are equal). The tiling per layer family is ours: the conv layers are re-tiled with a
  `[A][B][C][tap][D][o][i]` (net A with a 8x16 tile, net B 16x16) or `[A][B][tap][D][C][o][i]` (U-Net, 16x16) digit
  order, small stems padded to a 128/32 tile, 1x1 heads as `[cin/4][cout][4]` (a padded column reads on into the next
  row), one small layer is copied as it is.
- **Scale and bias.** The per-channel vectors are not data in the file but byte-wise immediate stores in start-up code.
  The generic scan (`mov byte [rsp+disp32], imm8`, consecutive displacements = one vector, in file order) finds the
  same runs, in the same order, in 1.3.4.95 and 1.3.1.78. The recipe says per upload which run it takes and in which
  form: as is (float32 or int32), 16-bit widened to int32, a stem vector widened and zero-padded to 512 bytes, or a
  folded bias `float32(aux * 128 * scale + bias)` where `aux` is the sum of the layer's int8 weights.
- **Check.** Every rebuilt upload is compared with the SHA-256 the recipe lists for it before it is used; a mismatch
  names the upload. A libxess_fg whose blobs are not in the recipe fails with the first missing blob.
- **What is and is not in the repo.** No Intel bytes (not even a few: the recipe holds lengths, SHA-256 and a hash tag
  of 8 probe bytes), and no address, RVA or file offset. The code does embed interoperability facts, all of them
  about how the data is laid out, none copied from the binary as bytes:
  the number and order of a layer descriptor's words (12; kind, cout, cin, clamp bits, sn, sp, mulPos, then 7, 1, 1, 1);
  the call-argument window of the builder's frame they are stored in (displacements 0x30 to 0x98, word order = highest
  first); the function-size filter of the layer-table scan (over 40000 bytes); that the model's table is the one with
  36 layers whose first layer has 2 input channels; which root-constant dwords the layer words go to (conv blocks:
  channel chunk, mulPos, `(sn<<8)|sp`; head blocks: mulPos, sp) and the plain numbers in `decode.rc` (each with a
  short meaning); the weight tilings; the fold gain 128; and the generic x86 encodings the scans recognise
  (`mov byte [rsp+disp32], imm8`, `mov dword [rsp+disp], imm32`, `lea reg,[rip+disp32]`, `call`). The layer
  "i<digits>" strings are only used as a delimiter by the scan; blocks refer to a layer by its position in the table.
- **Cost and test.** About 30 ms to read the file, find the 36 blobs and the 219 runs, 10 ms to decode all 106
  uploads. `tests/intel_file_test.cpp` (`tests/build_intel_file_test.ps1`) rebuilds and hash-checks all 106 from a
  libxess_fg.dll: 106/106 on 1.3.4.95 and on 1.3.1.78.
- **Root constants** (`RootConstants()`, the 44 `intel_const` blocks, no capture). Three sources:
  the size words come from the `set` formulas of `manifest.tmpl` evaluated at the size the recipe was made at
  (`decode.rc.size`); the per-layer words (multiplier position, shifts) come from the layer table of the same DLL, found by
  a generic scan (every function of the exception directory over 40000 bytes, runs of exactly 12 immediate 32-bit
  stores into the call-argument area of the frame closed by a call, named by the following "i<digits>" string; the
  table with 36 layers whose first layer has 2 input channels is the model's); the rest are plain numbers of our
  description (zeros, 25, 32, channel chunks 2 and 4, the floats 0.25, 0.5, 2, 4, and 1), kept in the recipe with the
  block-to-layer map. Words the runtime patches every frame (scale, jitter, interpolation time, pass index) or that
  Intel's code never initialises are written 0 and listed in the entry's `mask`; for those 16 blocks the recipe's SHA-256
  is taken with the masked dwords zeroed (28 blocks are hashed exactly). The pack builder (and `build_pack.py`) writes
  those dwords as 0 whatever the source gave, so every source builds the same bytes.
  The manifest's `patch` lines write the real values every frame.

### The translator (`src/translate.h/.cpp`)

`Dxil2Ptx` (disassembly text -> PTX), `ReconEdit` (the reconstruction input edit, before translation),
`ClampVariant` (re-binds the 17/18-bit accumulator clamp of a translated kernel), `VectorizeShared` (groupshared
loads of four aligned words become one vector load) and `TranslateEntry`, which runs one recipe entry's transform
(`plain`, `recon`, `clamp:<bits>`) with its `fmts`. It is a port of our own offline tool; its comments keep the
reasons for the float contraction rules (every fmul with a single fadd/fsub use is fused into it, first operand
first; everything else `.rn`) and the other bit-exact choices. A module that uses something the translator does not
know fails with a message, never a crash; the slot is then unavailable and the own generator reports it.

`tests/translate_test.cpp` (build: `tests/build_translate_test.ps1`) translates every `intel_xlate` entry of both
recipe packs from a libxess_fg.dll in-process and compares byte for byte with the offline tool's PTX for the same
module and options: 52 entries, identical on libxess_fg 1.3.4.95 and 1.3.1.78. Translating all 52 takes about 1.25 s
(24 ms each) plus 0.2 s for the disassembly, once per (libxess_fg, GPU) before the cache exists.

### dxcompiler.dll

f5low.dll loads `dxcompiler.dll` from its own folder (`plugins\`) when it has to translate; it is never in this repo
and not linked at build time (the two COM interfaces used are declared in `src/dxc_disasm.h`). The release
packaging fetches it.

- **Pinned version: DirectX Shader Compiler 1.9.2602.17 (21d28f727)**, x64 `dxcompiler.dll` of the
  microsoft/DirectXShaderCompiler redistributable package. This is the build that wrote the disassembly the offline
  pack was made from; `IDxcCompiler::Disassemble` in-process returns the same text as `dxc -dumpbin` (equal except
  that dxc.exe writes CR LF line ends and one extra line end at the end of the file; the translator reads LF text and
  ignores blank lines, so the PTX is identical). A different dxcompiler may print other text; the cache key carries
  the version and the test above must pass before a new one is pinned.
- `dxil.dll` is not needed (disassembly does not validate).
- **Licence**: the compiler source is under the LLVM (University of Illinois/NCSA) licence with MIT parts; the
  redistributable package ships these as `LICENSE-LLVM.txt` / `LICENSE-MIT.txt` and its binaries under Microsoft's
  "DirectX Shader Compiler" software licence terms (`LICENSE-MS.txt`), which allow distributing the listed DLLs
  with an application provided distributors and end users agree to terms that protect Microsoft at least as much and
  the distributor indemnifies Microsoft for claims arising from the application (not from the unmodified DLL).
  Bundling needs that clause read and accepted; nothing is bundled yet. Alternatives: the player's PC often has a
  `dxcompiler.dll` already (game folders, Windows SDK), which would need the version check above.

## Own kernels (later speed-ups)

The translation route is the baseline. Own kernels replace single slots where they are measurably faster, one at a
time, each proven against the translated PTX with the check_all tolerance: input prep k0 (0.12 -> about 0.04 ms per
app frame), the blend k31 (largest), k1, k2/k3 (stem generator), k11/k12, k18. Until a slot has one, it stays
translated.

Native on D3D12 has rules (`D3DPack` refuses the pack otherwise), and they decide most of the table:

- A native kernel's texture outputs have no buffer behind them: a PTX/cubin kernel may only **sample** them.
  A native slot therefore needs its consumers to be native or sampling.
- A native kernel cannot write a resource that is **redirected** (the blend writes each generated frame straight
  into its ring buffer) and cannot take **per-pass patches** in its constant buffer (root constants only).
- A native kernel cannot read and write one resource in a dispatch.
- Native is D3D12 only. The CUDA-graph path has no DXBC route, so with native slots in use it is off, and in
  pack-less mode it stays off until the native slots have own kernels too (epic decision 3).

## Slot options (D3D12 path)

Slots are the manifest's kernel numbers; both packs share them (the `hdr\` pack has other formats, other
recon/blend modules, the same rest). Module names are SHA-256 prefixes (8 hex digits).

| Slot | What it does | Transform | Module (SDR / HDR) | Options on D3D12 | Known facts | Recommendation |
|---|---|---|---|---|---|---|
| k0 | input prep once per app frame: both frames into the flow net's 2 input textures | **own** (`gen_prep_ptx.py`, story B2b round 1) | a6aefb7a (both) | own kernel, runs on D3D12 and CUDA; native stays blocked by its consumers (k2, k3, cubins reading the textures as buffers) | bit-exact against the module on 9 cases (odd sizes, rect, 2560x1440, 1505x847); 0.022 ms at 2560x1440 vs 0.0345 translated and 0.022 native | done, no translation |
| k1 | box downscale of the previous depth to 640x360 | **own** (`gen_depth_down_ptx.py`, story B2b round 1) | fc68cb62 (both) | own kernel; native **blocked** (consumer k18 is a cubin reading the buffer) | exact rounding matters: div.approx vs div.rn was 1 ulp off on 19% of pixels; bit-exact on 10 cases; 0.0113 ms (2560x1440 -> 640x360) vs 0.017 translated and 0.024 native | done, no translation |
| k2 | net A stem: 4 -> 128 channels, stride 2, 18-bit clamp | **own** (`gen_stem_ptx.py --stem a`, story B2b round 2) | 61e52c4b (both) | own kernel (the module's clamp is fixed at 17, the pack runs 18); native impossible | bit-exact against the pack's former translated+re-bounded kernel on 19 cases per slot (real weights, saturating, odd sizes 65x33 .. 2560x1440, crop 0/1, texture and limit edges); 0.0024 ms at 2560x1440 vs 0.0025 | done, no translation |
| k3 | net A 112 -> 224 channels, stride 2, 17-bit clamp | **own** (`gen_s2_ptx.py --shape a112`, story B2b round 2) | 654e0607 (both) | as k2 | as k2; 0.0105 ms vs 0.0109 | done, no translation |
| k11 | net B stem: 8 -> 32 channels from the half-float flow and the warp, stride 2 | **own** (`gen_stem_ptx.py --stem b`, story B2b round 2) | c68b3843 (both) | own kernel; native blocked (outputs read by our family B kernels as buffers) | bit-exact against the module on the same kind of cases (flow buffer slack, unaligned 12-byte pixels); 0.0108 ms vs 0.0118 | done, no translation |
| k12 | net B 32 -> 64 channels, stride 2 | **own** (`gen_s2_ptx.py --shape b32`, story B2b round 2) | 338f1957 (both) | as k11 | as k11; 0.0121 ms vs 0.0130 | done, no translation |
| k17 | motion splat of the game's motion + depth, per generated frame | plain | aae5e2f6 (both) | **native works** (existing `native` line, no padding); own; translate | native == PTX 0.11 ms per pass in pack_d3d (kernel alone 0.046); outputs equal byte for byte; the cost was the clear before it, now range-cleared | **native** |
| k17 variants | same, flag variants (inverted depth / no depth cut) | plain | c0481f4f, 4847df5d, 903ffa0d (both) | as k17 | as k17 | **native** |
| k18 | per-t forward splat of the flow (atomic key buffer), once per generated frame | **own** (`gen_splat_t_ptx.py`, story B2b round 3) | 9ef8c15d (both) | own kernel (key = flag, depth, fp16-style packing of flow x t, atomic max into the 2x2 targets, through a 16x16 groupshared tile); native possible but not faster | bit-exact against the module on 17 cases per variant (real-size 640x360 at t = 0.2 .. 0.8, 1505x847, 1920x1080, 2560x1440, odd sizes and pitches, heavy collisions, equal depth, scale overflow/underflow); 0.0271 ms (640x360) vs 0.0277 translated and 0.0278 native; 0.275 vs 0.276 translated and 0.260 native at 2560x1440 (atomic-bound; the tile is worth 2x over direct atomics) | done, no translation |
| k18 variant | same, key on 1 - depth (the non-inverted-depth flag set) | **own** (`gen_splat_t_ptx.py --invert-depth`) | bca124cb (both) | as k18 | as k18 | done, no translation |
| k19 | reconstruction input (U-Net input, 3 or 4 blend images) | recon (u0 store edit) | a7fc1e46 / 4ffe969e | **native works** (u0 raw, `32` pad; u1-u4 real textures the blend samples); own would be a rewrite of the gathers; translate (needs the u0 store edit) | translated 0.360 ms/pass vs native 0.249 (1440p, NBA inputs); per app frame 2.10 -> 0.94 ms; also makes the blend 0.195 -> 0.122; translation differs +-1/1023 in 0.05% of one image, native is the reference | **native** |
| k19 variants x11 | same, per init-flag set (ndc, jit, hires, inv) | recon | SDR 5a5a7b35, eae043e6, dfa6feb3, 8c7b249a, ad6437f9, 15dcfb96, 30511d38, 13637bf9, 81316214, 6c30e9aa, 6e84300e; HDR bb0e28cd, a48c8c87, d4b7977d, 31138d0d, 7e4c1e40, 643e6215, 6cb5a7ef, b0bd346d, da334ef5, 1f808ae3, 4cb7798f | as k19 | as k19 | **native** |
| k31 | blend of the generated frame (writes the redirected ring buffer) | **own** (`gen_blend_ptx.py [--hdr]`, story B2b round 4) | cd506291 / efc1536e | own kernel (native impossible: it writes a redirected output) | bit-exact against Intel's module on 24 cases per variant (UI modes 0 and 2..6, debug frame on/off with frame counters, 1x1 .. 2560x1440, odd sizes, per-texture origins, short weight / scale / bias buffers, small U-Net texture, saturated sigmoids); D3D12 pack 0.100 ms per app frame vs 0.101 translated (Intel's module native 0.140); 54 registers like the translated kernel (the reads issued before the weights: 81 registers, 0.128 ms) | done, no translation |
| k31 variant | blend with straight (not premultiplied) UI alpha | **own** (`gen_blend_ptx.py [--hdr] --premul 0`) | 0f8619d4 / 47821c21 | as k31 | as k31 | done, no translation |

Kernels of the SDR and the HDR pack that differ must have different file names: the D3D12 runner keeps one compiled kernel per file name for the whole process (`cubins`), so `k31_blend_sdr.ptx` / `k31_blend_hdr.ptx` (and their `v_...straight` variants) are named apart; the slots whose code is the same in both packs share a name on purpose.

Summary: 16 slots per pack run native today (12 recon, 4 splat) and stay native; 10 do not and need an own
kernel or the translation: k0, k1, k2, k3, k11, k12 now have one (B2b rounds 1-2); k18 (+ variant) in round 3 and k31 (+ variant, SDR and HDR) in round 4: none of the ten is translated any more
(the recipe's other 16 `intel_xlate` modules are the PTX twins of the native ones, kept for the CUDA path).

Open (needs GPU timing, not measured here): none for k0 .. k18 (measured in the table above).

## libxess_fg builds checked (story B4, 2026-11-01)

Every distinct libxess_fg.dll found on the development PC (SHA-256 first 8 hex; `tests/modules_test`, `tests/intel_file_test`
with `recipe`, `-XessInc` headers of OptiScaler-NR). translate_test was run on no new build (only builds with all modules found qualify, and
the two that do were covered in B2/B3).

| Version | sha8 | Size | Link time (UTC) | Modules found | Weight uploads / root constants | Result |
|---|---|---|---|---|---|---|
| 1.1.0.19 | D82D9ADA | 45,557,608 | 2025-03-13 | decode fails (no zstd dictionaries) | not found | unsupported |
| 1.2.0.87 | 7FA2116E | 45,541,752 | 2025-07-18 | decode fails | not found | unsupported |
| 1.2.2.118 | E042A111 | 47,119,216 | 2025-10-21 | decode fails | not found | unsupported |
| 1.3.1.78 | EC5E0C65 | 22,957,432 | 2026-03-31 | 40/40 (144 distinct) | 106/106, 44/44 | works |
| 1.3.4.95 | 752B1491 | 22,866,056 | 2026-08-31 | 40/40 (144 distinct) | 106/106, 44/44 | works |
| 1.3.4.95 (one byte-patched variant of 752B1491, same size) | 5E1ACFFE | 22,866,056 | 2026-08-31 | 40/40 | 106/106, 44/44 | works |

Why the 1.1 to 1.2 builds fail: they hold no zstd frames and no dictionaries (the 1.3 layout compresses the shader modules), no
DXBC/DXIL container text at all, 42 MB of `.rdata` instead of 20 MB, and no multi-frame-generation strings; the model names are the
`xefg_model_franky*` set (lite 6conv/8conv). They are 2x-only builds without the i0 model. Two other files named libxess_fg.dll on the PC
are OptiScaler's own wrapper builds, not Intel's; they are rejected cleanly.
