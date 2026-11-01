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
3. **Per slot, one of three routes.**
   - **native**: Intel's container runs as it is on D3D12 (the pack's `native <ptx> <dxbc> [32]` line:
     Intel's own root signature, descriptors from the manifest's binds; `32` = u0 is a raw buffer written with a
     one-texel border, which the pack pads). No translation, nothing to build.
   - **own**: a kernel of ours replaces the module (a generator in `tools/kernels`, compiled with the pack).
   - **translate**: module disassembled and turned into PTX (dxil2ptx, today Python). In pack-less mode only as a
     last resort: it needs a C++ port that reads the player's dxcompiler.
4. **Cache** the built pack per (libxess_fg SHA-256, GPU SM) (story B2 step 3).

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
| k0 | input prep once per app frame: both frames into the flow net's 2 input textures | plain | a6aefb7a (both) | native only if its consumers (k2, k3, cubins reading the textures as buffers) become native or sampling: **blocked**; own possible (small); translate works today | in game 0.12 ms per app frame translated; the same module native (MFG Unlocker's route) 0.04 | **own** kernel (small, per app frame); until then translate |
| k1 | box downscale of the previous depth to 640x360 | plain | fc68cb62 (both) | own (maths and edge rule documented by the bit-exact checks), translate; native **blocked** (consumer k18 is a cubin reading the buffer) | exact rounding matters: div.approx vs div.rn was 1 ulp off on 19% of pixels | **own** kernel or translate; low effort, tiny cost |
| k2 | flow net A stem, 18-bit clamp | clamp:18 (module ships 17) | 61e52c4b (both) | native **impossible** (module's clamp is fixed at 17); own stem kernel (needs a stem generator, family B generator is the closest); translate + clamp re-bound (today) | proven both ways 17<->18 with check_all V clamp rows | **own** (stem generator) |
| k3 | flow net B stem, 17-bit clamp | clamp:17 (module ships 18) | 654e0607 (both) | as k2 | as k2 | **own** (same generator) |
| k11 | flow net B front stage (U0 only) | plain | c68b3843 (both) | native **blocked** (outputs read by our family B cubins as buffers); own; translate | no separate timing; once per app frame | translate until an own kernel exists; role inferred from dispatch order, not verified |
| k12 | flow net B front stage (x128 in/out) | plain | 338f1957 (both) | as k11 | as k11 | as k11 |
| k17 | motion splat of the game's motion + depth, per generated frame | plain | aae5e2f6 (both) | **native works** (existing `native` line, no padding); own; translate | native == PTX 0.11 ms per pass in pack_d3d (kernel alone 0.046); outputs equal byte for byte; the cost was the clear before it, now range-cleared | **native** |
| k17 variants | same, flag variants (inverted depth / no depth cut) | plain | c0481f4f, 4847df5d, 903ffa0d (both) | as k17 | as k17 | **native** |
| k18 | per-t forward splat of the flow (atomic key buffer) | plain | 9ef8c15d (both) | native possible but **slower** (kept as PTX in the personal pack); own kernel (semantics documented: key = flag, depth, fp16 flow x t, atomic max into 2x2); translate | notes 2026-10-24: "slower native: stays PTX" (no number recorded) | **own** kernel; translate meanwhile |
| k18 variant | same, other depth sense (inv=0) | plain | bca124cb (both) | as k18 | as k18 | **own** (flag of the k18 kernel) |
| k19 | reconstruction input (U-Net input, 3 or 4 blend images) | recon (u0 store edit) | a7fc1e46 / 4ffe969e | **native works** (u0 raw, `32` pad; u1-u4 real textures the blend samples); own would be a rewrite of the gathers; translate (needs the u0 store edit) | translated 0.360 ms/pass vs native 0.249 (1440p, NBA inputs); per app frame 2.10 -> 0.94 ms; also makes the blend 0.195 -> 0.122; translation differs +-1/1023 in 0.05% of one image, native is the reference | **native** |
| k19 variants x11 | same, per init-flag set (ndc, jit, hires, inv) | recon | SDR 5a5a7b35, eae043e6, dfa6feb3, 8c7b249a, ad6437f9, 15dcfb96, 30511d38, 13637bf9, 81316214, 6c30e9aa, 6e84300e; HDR bb0e28cd, a48c8c87, d4b7977d, 31138d0d, 7e4c1e40, 643e6215, 6cb5a7ef, b0bd346d, da334ef5, 1f808ae3, 4cb7798f | as k19 | as k19 | **native** |
| k31 | blend of the generated frame (writes the redirected ring buffer) | plain | cd506291 / efc1536e | native **impossible** (writes a redirected output); own blend kernel (10 sampled inputs, SDR rgba8 / HDR rgb10a2); translate (today) | translated 0.122 ms/pass; Intel's module native 0.140 (not faster, even before the redirect) | translate; **own** later (largest of the own kernels: 4 formats/variants) |
| k31 variant | blend without premultiplied alpha | plain | 0f8619d4 / 47821c21 | as k31 | as k31 | as k31 |

Summary: 16 slots per pack run native today (12 recon, 4 splat) and stay native; 10 do not and need an own
kernel or the translation: k0, k1, k2, k3, k11, k12, k18 (+ variant), k31 (+ variant) = 8 slots with 10 modules
(the recipe's other 16 `intel_xlate` modules are the PTX twins of the native ones, kept for the CUDA path).

Open (needs GPU timing, not measured here): own vs translated for k0/k1/k11/k12 (all small, once per app frame,
total about 0.2 ms of the 1.45 ms app-frame budget), and the number behind "k18 slower native".
