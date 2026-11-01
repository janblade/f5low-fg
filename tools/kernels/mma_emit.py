# The int8 tensor-core step our own kernels share (gen_conv_ptx.py, gen_decoder_ptx.py, gen_famb_ptx.py):
# d += a * b for M = 16 pixels, N = 8 output channels, K = 32 input channels, int32 sums.
#
# sm 80 and newer: one mma.sync.m16n8k32.s32.s8.s8.s32. sm 75 (an RTX 20, which has only the m8n8k16 int8 shape):
# four mma.m8n8k16 on the same registers - the m16n8k32 fragments are the m8n8k16 ones stacked (grp = lane/4,
# tg = lane%4): a0 / a1 = rows grp / grp+8, K 4tg..+3; a2 / a3 the same rows, K 16+4tg..; b0 / b1 = column grp,
# K 4tg.. / 16+4tg..; d0,d1 = row grp, columns 2tg, 2tg+1; d2,d3 = row grp+8. So rows grp take a0 * b0 + a2 * b1
# into d0,d1 and rows grp+8 take a1 * b0 + a3 * b1 into d2,d3: the same integers, summed in another order.


def mma_s8(w, sm, c, a, b, zero=()):
    """Emits d += a * b through w(). c: the four accumulator registers, a: the four A registers, b: the two B
    registers; zero: the K halves (0: K 0..15, 1: K 16..31) whose B is zero, which sm 75 leaves out."""
    cs = ", ".join(c)
    if sm >= 80:
        w(f"    mma.sync.aligned.m16n8k32.row.col.s32.s8.s8.s32 {{{cs}}}, {{{', '.join(a)}}}, "
          f"{{{', '.join(b)}}}, {{{cs}}};")
        return
    for h in range(2):
        if h in zero:
            continue
        for rows, ar in ((f"{c[0]}, {c[1]}", a[2 * h]), (f"{c[2]}, {c[3]}", a[2 * h + 1])):
            w(f"    mma.sync.aligned.m8n8k16.row.col.s32.s8.s8.s32 {{{rows}}}, {{{ar}}}, {{{b[h]}}}, {{{rows}}};")
