// The flow networks' 1x1 head, as a template. Instantiated at the two original shapes it must reproduce
// the original modules (net A, net B) bit for bit; at the
// multi-frame model's shape (12 outputs, one input, its own clamp) it is that model's head.
//
// One thread per input pixel (x, y); it reads texel (x, y + 1) (the one-row top pad of every family B
// activation) of t0 - and of t1 when SKIP, the second activation the 2x model adds - and dots each input
// word with weight word WSTRIDE * i + k. Accumulators start at the seed t3[k]. Epilogue: clamp to
// +-2^(CLAMP-1), * mul + (1 << (sp - 1)), >> sp, clamp to int8. Output channel k = 4c + s is byte c of
// pixel (2x + s % 2, 2y + s / 2) (a x2 depth-to-space) of a byte buffer with BPP bytes per pixel and a
// row pitch of `pitch` pixels; bytes past the last channel hold the epilogue of a zero accumulator.
// Stored only when 2y < bound - 1 (unsigned); no x bound.
//
// Defines: TEXW (uints per texel: 2 or 4), NWORDS (input words), WSTRIDE, KOUT (outputs, a multiple of
// 4), BPP (4 or 8), SKIP (0 or 1), CLAMP (bits).

Texture2DArray<vector<uint, TEXW> > t0 : register(t0);
Texture2DArray<vector<uint, TEXW> > t1 : register(t1);
StructuredBuffer<uint> t2 : register(t2);
StructuredBuffer<int4> t3 : register(t3);
RWByteAddressBuffer u0 : register(u0);

cbuffer pass_cb : register(b0)
{
    uint d0, d1, pitch, d3, d4, bound, d6, mul, shifts;
};

static const int LO = -(1 << (CLAMP - 1));
static const int HI = (1 << (CLAMP - 1)) - 1;
static const uint NCH = KOUT / 4;          // channels per sub-pixel

int Epi(int a, int rnd, uint sp)
{
    a = min(max(a, LO), HI);
    return min(max((int)(a * (int)mul + rnd) >> sp, -128), 127);
}

// Intel's root signature for these modules: the same tables, so xefg_run and the pack bind it alike.
[RootSignature("DescriptorTable(SRV(t0), SRV(t1), UAV(u0)), DescriptorTable(SRV(t2), SRV(t3)), RootConstants(num32BitConstants=9, b0)")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    int acc[KOUT];
    [unroll] for (uint k = 0; k < KOUT; k++)
        acc[k] = t3[k / 4][k % 4];
    for (uint i = 0; i < NWORDS; i++)
    {
        vector<uint, TEXW> a = t0.Load(int4(id.x, id.y + 1, i / TEXW, 0));
        uint wa = a[i % TEXW];
#if SKIP
        vector<uint, TEXW> b = t1.Load(int4(id.x, id.y + 1, i / TEXW, 0));
        uint wb = b[i % TEXW];
#endif
        [unroll] for (uint k = 0; k < KOUT; k++)
        {
            uint w = t2[WSTRIDE * i + k];
            acc[k] = dot4add_i8packed(wa, w, acc[k]);
#if SKIP
            acc[k] = dot4add_i8packed(wb, w, acc[k]);
#endif
        }
    }
    uint sp = shifts & 31;
    int rnd = (int)(1u << ((shifts + 31) & 31));
    uint zero = (uint)Epi(0, rnd, sp) & 255;
    if (id.y * 2 >= bound - 1)
        return;
    [unroll] for (uint s = 0; s < 4; s++)
    {
        uint word[2] = { 0, 0 };
        [unroll] for (uint c = 0; c < BPP; c++)
        {
            uint v = c < NCH ? (uint)Epi(acc[min(4 * c + s, KOUT - 1)], rnd, sp) & 255 : zero;
            word[c / 4] |= v << (8 * (c % 4));
        }
        uint px = (id.y * 2 + s / 2) * pitch + id.x * 2 + s % 2;
        u0.Store(px * BPP, word[0]);
        if (BPP == 8)
            u0.Store(px * BPP + 4, word[1]);
    }
}
