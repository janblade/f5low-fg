# Emits our own kernel for a single-source U-Net layer of the multi-frame model: a 3x3 CIN -> COUT conv, or an
# encoder (the same conv at the four sub-positions of each 2x2 block, combined by an integer max before the
# epilogue), in the pack's kernel interface (p_desc, 48-byte slots p_s0..p_s3, p_s32) so it replaces a translated
# module in a pack as it is: same slots, and each block makes the output tile of the replaced grid.
#
#   layer     what               tile     layer     what                tile
#   enc16     enc 16 -> 16 (u)   16 x 16  c128      conv 128 -> 64      16 x 16
#   enc32     enc 16 -> 32        4 x 16  c64       conv 64 -> 32 (s)   16 x 8
#   enc64     enc 32 -> 64        4 x 8   c32       conv 32 -> 16 (s)    8 x 8
#   enc128    enc 64 -> 128       8 x 4   c16       conv 16 -> 16 (s)    8 x 8
#
# The layers, as verified against the modules they replace:
#   acc[co]   = sum over taps, ci of in[cx + ex - 1][cy + ey - 1][ci] * W, the input read as zero outside its
#               texture; (cx, cy) = (x, y), or (2x + sx, 2y + sy) and acc = max over (sx, sy) for an encoder
#   (u)       = every packed input word (4 channels) + 0x80808080 as one 32-bit add, carries included, the
#               zero of an out-of-texture read too (enc16's unsigned activations)
#   out[co]   = clamp(trunc(fma(scale[co], acc, bias[co])), lo, 127): lo = -128 for (s), else 0
#   weight word = t1[((ci/16) + (co/16)*CS)*576 + (3*ey + ex)*64 + ((ci/4)%4)*16 + co%16], bytes = ci%4, CS = CIN/16
# Activations (t0 slot 0, the output slot 32) are textures of 16-channel slices: (slice * h + y) * pitch + 16x.
#
# The input tile (with its halo) is staged in shared memory; the conv runs on the tensor cores, mma.m16n8k32
# (int8: integer sums in any order, so exact) as in gen_decoder_ptx.py: M = 16 output pixels of the tile
# (row-major), N = 8 output channels, K = two taps of 16 channels (CIN 16; the 10th tap zero) or one tap's
# slice pair. A warp makes items (16 pixels, up to 32 output channels). Weights over 20 KB stay in global
# memory (read through L1), the others are copied to shared memory.
#
# --sm 75 (an RTX 20): each mma.m16n8k32 as four mma.m8n8k16 on the same registers (mma_emit.py); a half with zero
# weights (the 10th tap) is left out.
#
# Usage: python gen_conv_ptx.py <out.ptx> --layer enc16|enc32|enc64|enc128|c128|c64|c32|c16 [--sm 80|75]
import argparse

from mma_emit import mma_s8

LAYERS = {  # cin, cout, encoder, unsigned, clamp lo, tile w, tile h, threads
    "enc16": (16, 16, True, True, 0, 16, 16, 256),
    "enc32": (16, 32, True, False, 0, 4, 16, 128),
    "enc64": (32, 64, True, False, 0, 4, 8, 128),
    "enc128": (64, 128, True, False, 0, 8, 4, 256),
    "c128": (128, 64, False, False, 0, 16, 16, 256),
    "c64": (64, 32, False, False, -128, 16, 8, 256),
    "c32": (32, 16, False, False, -128, 8, 8, 128),
    "c16": (16, 16, False, False, -128, 8, 8, 128),
}
# sm_75 only (an RTX 20; the manifest's fallback line scales the grid to match, gz=): the layer's output-channel groups are
# split over ZS blocks (blockIdx.z), each with the same input tile, so a layer with few big blocks fills more multiprocessors
SM75_ZS = {"c128": 4, "enc128": 4}
SM75_NG = {"c128": 2, "enc128": 2}
SM75_NT = {"enc128": 128}
import os
for _o in os.environ.get("SM75_EXP", "").split(";"):   # EXPERIMENT
    if _o:
        _l, _kv = _o.split(":")
        for _p in _kv.split(","):
            _k, _v = _p.split("=")
            {"zs": SM75_ZS, "ng": SM75_NG, "nt": SM75_NT}[_k][_l] = int(_v)
ap = argparse.ArgumentParser()
ap.add_argument("out")
ap.add_argument("--layer", required=True, choices=sorted(LAYERS))
ap.add_argument("--sm", type=int, default=80, choices=(75, 80))
a = ap.parse_args()

CIN, COUT, ENC, UNS, LO, TW, TH, NT = LAYERS[a.layer]
if a.sm == 75:
    NT = SM75_NT.get(a.layer, NT)
CS = CIN // 16
S = 2 if ENC else 1
SW, SH = S * TW + 2, S * TH + 2     # staged input tile
NS = SW * SH
NW = NT // 32
MT = TW * TH // 16                  # m-tiles
NB = COUT // 8
NG = min(NB, 4)                     # n-blocks an item
GW = 4                              # n-blocks a group steps by in the weights / bias / output slices (32 channels)
if a.sm == 75 and a.layer in SM75_NG:
    NG = GW = SM75_NG[a.layer]
GI = NB // NG                       # n-block groups
ZS = SM75_ZS.get(a.layer, 1) if a.sm == 75 else 1
assert GI % ZS == 0
GIB = GI // ZS                      # groups a block makes
ITEMS = MT * GIB
assert ITEMS % NW == 0, (ITEMS, NW)
IPW = ITEMS // NW
WBYTES = 9 * CIN * COUT
WSH = WBYTES <= 20480
SUBS = ((0, 0), (1, 0), (0, 1), (1, 1)) if ENC else ((0, 0),)
L = []


def w(s=""):
    L.append(s)


def ksteps():
    """Per K step: two (tap, slice) halves, tap None = zero."""
    if CS == 1:
        return [((2 * st, 0), (2 * st + 1 if 2 * st + 1 < 9 else None, 0)) for st in range(5)]
    return [((tap, 2 * j), (tap, 2 * j + 1)) for tap in range(9) for j in range(CS // 2)]


def wword(tap, sl, nb):
    """Weight word of tap, slice sl, output channel 8nb (+ the group's 32gi + the lane's 16tg + grp)."""
    co = 8 * nb
    return (sl + (co // 16) * CS) * 576 + tap * 64 + co % 16


KS = ksteps()
GSTEP = (8 * GW // 16) * CS * 576       # weight words from one 32-channel group to the next

# the header names --sm only for an older GPU (the sm 80 packs of 2026-10 were written without it)
SMARG = f" --sm {a.sm}" if a.sm != 80 else ""
w(f"// GENERATED by gen_conv_ptx.py --layer {a.layer}{SMARG} - our own kernel for a {CIN} -> {COUT} U-Net "
  f"{'encoder' if ENC else 'conv'}.")
if ZS > 1:
    w(f"// GRIDSCALE 1 1 {ZS}")    # the manifest's fallback line says gz={ZS} (Pack::Load checks)
w(".version 7.0")
w(f".target sm_{a.sm}")
w(".address_size 64")
w()
w(".visible .entry dxil_main(")
w("    .param .u64 p_desc,")
for s in ("s0", "s1", "s2", "s3", "s32"):
    w(f"    .param .align 16 .b8 p_{s}[48]" + ("," if s != "s32" else ""))
w(f") .reqntid {NT}, 1, 1")
w("{")
w(f"    .shared .align 16 .b8 st[{NS * CIN}];")
if WSH:
    w(f"    .shared .align 16 .b8 wt[{WBYTES}];")
w("    .reg .pred %p, %pa, %pb, %pl;")
w("    .reg .b32 %tl, %ox0, %oy0, %ix0, %iy0, %i, %lx, %ly, %fx, %fy, %t, %sl, %j, %item, %mt, %gi;")
w("    .reg .b32 %w0, %h0, %n0, %pi0, %wo, %ho, %pio;")
w("    .reg .b32 %e, %n1, %V<4>;")
if ZS > 1:
    w("    .reg .b32 %gz;")
w("    .reg .b32 %wid, %lane, %grp, %tg, %a0, %a1, %a2, %a3, %b0, %b1, %sf, %sw, %q0, %q1, %x0, %x1, %y0, %y1;")
w("    .reg .b32 " + ", ".join(f"%c{nb}_{k}" for nb in range(NG) for k in range(4)) + ";")
if ENC:
    w("    .reg .b32 " + ", ".join(f"%m{nb}_{k}" for nb in range(NG) for k in range(4)) + ";")
w("    .reg .b16 %hv;")
w("    .reg .f32 %f, %s0, %s1, %g0, %g1;")
w("    .reg .b64 %P0, %PO, %PS, %PB, %PC, %PW, %r64;")
w()
w("    mov.u32 %tl, %tid.x;")
w("    mov.u32 %t, %ctaid.x;")
w(f"    mul.lo.u32 %ox0, %t, {TW};")
w("    mov.u32 %t, %ctaid.y;")
w(f"    mul.lo.u32 %oy0, %t, {TH};")
if ZS > 1:
    w("    mov.u32 %t, %ctaid.z;")
    w(f"    mul.lo.u32 %gz, %t, {GIB};")
w(f"    mul.lo.u32 %ix0, %ox0, {S};")
w("    add.s32 %ix0, %ix0, -1;")
w(f"    mul.lo.u32 %iy0, %oy0, {S};")
w("    add.s32 %iy0, %iy0, -1;")
w("    ld.param.u64 %P0, [p_s0+0];")
w("    ld.param.u32 %w0, [p_s0+16];")
w("    ld.param.u32 %h0, [p_s0+20];")
w("    ld.param.u32 %n0, [p_s0+24];")
w("    ld.param.u32 %pi0, [p_s0+36];")
w("    mov.u32 %sf, st;")
w("    ld.param.u64 %PW, [p_s1+0];")
if WSH:
    w("    mov.u32 %sw, wt;")
    w(f"    // the weights to shared memory: {WBYTES // 16} x 16 bytes")
    for it in range((WBYTES // 16 + NT - 1) // NT):
        w(f"    add.u32 %i, %tl, {NT * it};")
        w(f"    setp.lt.u32 %p, %i, {WBYTES // 16};")
        w("    mul.wide.u32 %r64, %i, 16;")
        w("    add.s64 %PS, %PW, %r64;")
        w("    @%p ld.global.nc.v4.b32 {%V0, %V1, %V2, %V3}, [%PS];")
        w("    mad.lo.u32 %t, %i, 16, %sw;")
        w("    @%p st.shared.v4.b32 [%t], {%V0, %V1, %V2, %V3};")
w()
w(f"    // phase 1: the input tile, staged pixel i at (ix0 + i % {SW}, iy0 + i / {SW}), {CIN} bytes each")
for it in range((NS * CS + NT - 1) // NT):
    w(f"    add.u32 %i, %tl, {NT * it};")
    w(f"    setp.lt.u32 %pl, %i, {NS * CS};")
    w(f"    @!%pl bra L{it}_END;")
    w(f"    rem.u32 %sl, %i, {CS};")
    w(f"    div.u32 %j, %i, {CS};")
    w(f"    div.u32 %ly, %j, {SW};")
    w(f"    rem.u32 %lx, %j, {SW};")
    w("    add.s32 %fx, %ix0, %lx;")
    w("    add.s32 %fy, %iy0, %ly;")
    w("    setp.lt.u32 %p, %fx, %w0;")
    w("    setp.lt.and.u32 %p, %fy, %h0, %p;")
    w("    setp.lt.and.u32 %p, %sl, %n0, %p;")
    for k in range(4):
        w(f"    mov.b32 %V{k}, 0;")
    w("    mad.lo.u32 %t, %sl, %h0, %fy;")
    w("    mul.wide.u32 %r64, %t, %pi0;")
    w("    add.s64 %PS, %P0, %r64;")
    w("    mul.wide.u32 %r64, %fx, 16;")
    w("    add.s64 %PS, %PS, %r64;")
    w("    @%p ld.global.nc.v4.b32 {%V0, %V1, %V2, %V3}, [%PS];")
    if UNS:
        for k in range(4):
            w(f"    add.s32 %V{k}, %V{k}, -2139062144;")
    w(f"    mad.lo.u32 %t, %j, {CIN}, %sf;")
    w("    mad.lo.u32 %t, %sl, 16, %t;")
    w("    st.shared.v4.b32 [%t], {%V0, %V1, %V2, %V3};")
    w(f"L{it}_END:")
w("    bar.sync 0;")
w()
w("    shr.u32 %wid, %tl, 5;")
w("    and.b32 %lane, %tl, 31;")
w("    shr.u32 %grp, %lane, 2;")
w("    and.b32 %tg, %lane, 3;")
w("    ld.param.u64 %PO, [p_s32+0];")
w("    ld.param.u32 %wo, [p_s32+16];")
w("    ld.param.u32 %ho, [p_s32+20];")
w("    ld.param.u32 %pio, [p_s32+36];")
w(f"    // phase 2: warp wid makes items wid + {NW}j (j < {IPW}): m-tile item % {MT}, output channels "
  f"32 * (item / {MT}) ..")
w("    mov.u32 %j, 0;")
w("ITEM:")
w(f"    mad.lo.u32 %item, %j, {NW}, %wid;")
w(f"    rem.u32 %mt, %item, {MT};")
w(f"    div.u32 %gi, %item, {MT};")
if ZS > 1:
    w("    add.u32 %gi, %gi, %gz;")
for px, (xr, yr, qr) in enumerate((("x0", "y0", "q0"), ("x1", "y1", "q1"))):
    w(f"    mad.lo.u32 %t, %mt, 16, %grp;")
    if px:
        w("    add.u32 %t, %t, 8;")
    w(f"    rem.u32 %{xr}, %t, {TW};")
    w(f"    div.u32 %{yr}, %t, {TW};")
    w(f"    mul.lo.u32 %t, %{yr}, {S * SW};")
    w(f"    mad.lo.u32 %t, %{xr}, {S}, %t;")
    w(f"    mad.lo.u32 %{qr}, %t, {CIN}, %sf;")
    w(f"    mad.lo.u32 %{qr}, %tg, 4, %{qr};")
w("    // this lane's weight words: + 16tg + grp, + the group's")
w("    mad.lo.u32 %t, %tg, 16, %grp;")
w(f"    mad.lo.u32 %t, %gi, {GSTEP}, %t;")
if WSH:
    w("    mad.lo.u32 %b0, %t, 4, %sw;")
else:
    w("    mul.wide.u32 %r64, %t, 4;")
    w("    add.s64 %PS, %PW, %r64;")
for si, (sx, sy) in enumerate(SUBS):
    acc = "m" if (ENC and si == 0) else "c"
    for nb in range(NG):
        for k in range(4):
            w(f"    mov.b32 %{acc}{nb}_{k}, 0;")
    for parts in KS:
        for h, (tap, sl) in enumerate(parts):
            if tap is None:
                w(f"    mov.b32 %a{2*h}, 0;")
                w(f"    mov.b32 %a{2*h+1}, 0;")
                continue
            ey, ex = tap // 3, tap % 3
            off = ((sy + ey) * SW + sx + ex) * CIN + 16 * sl
            w(f"    ld.shared.b32 %a{2*h}, [%q0+{off}];")
            w(f"    ld.shared.b32 %a{2*h+1}, [%q1+{off}];")
        for nb in range(NG):
            c = [f"%{acc}{nb}_{k}" for k in range(4)]
            regs = []
            for h, (tap, sl) in enumerate(parts):
                if tap is None:             # the 10th tap (CIN 16): zero weights
                    w("    mov.b32 %e, 0;")
                    regs.append("%e")
                    continue
                if WSH:
                    w(f"    ld.shared.b32 %{'n1' if h == 0 else 'e'}, [%b0+{4 * wword(tap, sl, nb)}];")
                    regs.append("%n1" if h == 0 else "%e")
                else:
                    w(f"    ld.global.nc.b32 %{'b0' if h == 0 else 'b1'}, [%PS+{4 * wword(tap, sl, nb)}];")
                    regs.append("%b0" if h == 0 else "%b1")
            mma_s8(w, a.sm, c, ["%a0", "%a1", "%a2", "%a3"], regs,
                   zero={h for h, (tap, _) in enumerate(parts) if tap is None})
    if ENC and si > 0:
        for nb in range(NG):
            for k in range(4):
                w(f"    max.s32 %m{nb}_{k}, %m{nb}_{k}, %c{nb}_{k};")
R = "m" if ENC else "c"
w(f"    // epilogue: clamp(trunc(fma(scale, acc, bias)), {LO}, 127); two channels a store, inside the output texture")
for px, (xr, yr, pr) in enumerate((("x0", "y0", "pa"), ("x1", "y1", "pb"))):
    w(f"    add.u32 %{xr}, %{xr}, %ox0;")
    w(f"    add.u32 %{yr}, %{yr}, %oy0;")
    w(f"    setp.lt.u32 %{pr}, %{xr}, %wo;")
    w(f"    setp.lt.and.u32 %{pr}, %{yr}, %ho, %{pr};")
w("    ld.param.u64 %PB, [p_s2+0];")
w("    ld.param.u64 %PC, [p_s3+0];")
w(f"    mul.lo.u32 %t, %gi, {32 * GW};")
w("    mad.lo.u32 %t, %tg, 8, %t;")
w("    cvt.u64.u32 %r64, %t;")
w("    add.s64 %PB, %PB, %r64;")
w("    add.s64 %PC, %PC, %r64;")
for nb in range(NG):
    w(f"    ld.global.nc.v2.f32 {{%s0, %s1}}, [%PB+{32 * nb}];")
    w(f"    ld.global.nc.v2.f32 {{%g0, %g1}}, [%PC+{32 * nb}];")
    for px, (xr, yr, pr) in enumerate((("x0", "y0", "pa"), ("x1", "y1", "pb"))):
        for k in range(2):
            w(f"    cvt.rn.f32.s32 %f, %{R}{nb}_{2 * px + k};")
            w(f"    fma.rn.ftz.f32 %f, %s{k}, %f, %g{k};")
            w("    cvt.rzi.ftz.s32.f32 %n1, %f;")
            w(f"    max.s32 %n1, %n1, {LO};")
            w(f"    min.s32 %{'e' if k == 0 else 'n1'}, %n1, 127;")
        w("    bfi.b32 %n1, %n1, %e, 8, 8;")
        w("    cvt.u16.u32 %hv, %n1;")
        w(f"    // slice 2gi + {nb // 2}, bytes {8 * (nb % 2)} + 2tg")
        w(f"    mad.lo.u32 %t, %gi, {GW // 2}, {nb // 2};")
        w(f"    mad.lo.u32 %t, %t, %ho, %{yr};")
        w("    mul.wide.u32 %r64, %t, %pio;")
        w("    add.s64 %PC, %PO, %r64;")
        w(f"    mul.wide.u32 %r64, %{xr}, 16;")
        w("    add.s64 %PC, %PC, %r64;")
        w("    shl.b32 %t, %tg, 1;")
        w("    cvt.u64.u32 %r64, %t;")
        w("    add.s64 %PC, %PC, %r64;")
        w(f"    @%{pr} st.global.u16 [%PC+{8 * (nb % 2)}], %hv;")
    w("    ld.param.u64 %PC, [p_s3+0];")
    w(f"    mul.lo.u32 %t, %gi, {32 * GW};")
    w("    mad.lo.u32 %t, %tg, 8, %t;")
    w("    cvt.u64.u32 %r64, %t;")
    w("    add.s64 %PC, %PC, %r64;")
w("    add.u32 %j, %j, 1;")
w(f"    setp.lt.u32 %p, %j, {IPW};")
w("    @%p bra ITEM;")
w("    ret;")
w("}")

open(a.out, "w", newline="\n").write("\n".join(L) + "\n")
