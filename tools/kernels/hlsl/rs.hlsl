// The flow path's bilinear feature resamplers, as one template. Instantiated at the original shapes it must
// reproduce the original modules (MODE 0, MODE 1 with scale 4, MODE 2) bit for bit; at the multi-frame model's shapes (3 channels, one flow) it is that
// model's resampler.
//
//   MODE 0  q: int8 source (t0, SRC_BPP bytes per pixel), dequantised: channels < NSCALED times the scale
//   MODE 1  h: fp16 source at (x + offX, y + offY), channels < NSCALED times the scale; stored at row y + 1
//   MODE 2  a: int8 source as q, channels < NSCALED times the scale, plus the fp16 t1 (ADD_BPP bytes per pixel,
//           pixel (x, y + 1)) added per channel; only NOUT channels stored
//
// Per output pixel (x, y), all in float32, as the shaders order it (the driver fuses each multiply that
// feeds an add, so the order is kept for that, not for the arithmetic's sake):
//   u = max(sx * (x - (cw - 1) * 0.5), 0), x0 = floor(u), x1 = x0 + 1, each clamped to [0, srcW - 1]
//   s = s00 w00, + s10 w10, + s01 w01, + s11 w11
// Out: NOUT halfs through the shaders' software f32 -> f16, DST_BPP bytes per pixel, pitch in pixels;
// only for x < dstW and y < dstH. Loads happen for every thread, as in the shaders.
//
// Defines: MODE, NSCALED, NOUT, SRC_BPP, DST_BPP, ADD_BPP (MODE 2), SCALE_BITS (the scale's float32 bits).

ByteAddressBuffer t0 : register(t0);
#if MODE == 2
ByteAddressBuffer t1 : register(t1);
#endif
RWByteAddressBuffer u0 : register(u0);

#if MODE == 1
cbuffer RootConstants : register(b0) { uint4 c0, c1; float4 c2; float2 c3; };
#else
cbuffer RootConstants : register(b0) { uint4 c0, c1; float4 c2; uint c3; };
#endif

uint ToHalf(float f)
{
    uint b = asuint(f) + 4096;
    uint e = (b >> 23) & 255, m = b & 8388607;
    uint h = (b >> 16) & 32768;
    if (e > 112) h |= ((((b >> 23) << 10) + 16384) & 31744) | (m >> 13);
    if (e - 102 < 11) h |= (((m + 8384512) >> ((125 - (b >> 23)) & 31)) + 1) >> 1;
    if (e > 143) h |= 32767;
    return h;
}

float Int8(uint word, uint byte)
{
    return (float)((int)(word << (24 - 8 * byte)) >> 24);
}

float Half(uint word, uint which)
{
    return f16tof32(which ? word >> 16 : word);
}

#if MODE == 1
#define SX c2.z
#define SY c2.w
#define CW c3.x
#define CH c3.y
#else
#define SX c2.x
#define SY c2.y
#define CW c2.z
#define CH c2.w
#endif

// Intel's root signatures for these modules (the multi-frame model's variants keep them).
#if MODE == 0
#define RS "DescriptorTable(SRV(t0), UAV(u0)), RootConstants(num32BitConstants=13, b0)"
#elif MODE == 1
#define RS "DescriptorTable(SRV(t0), UAV(u0)), RootConstants(num32BitConstants=14, b0)"
#else
#define RS "DescriptorTable(SRV(t0), SRV(t1), UAV(u0)), RootConstants(num32BitConstants=13, b0)"
#endif

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    float u = max(SX * ((float)id.x - (CW + -1.0f) * 0.5f), 0.0f);
    float v = max(SY * ((float)id.y - (CH + -1.0f) * 0.5f), 0.0f);
    int x0 = (int)floor(u), y0 = (int)floor(v);
    int x1 = x0 + 1, y1 = y0 + 1;
    uint xa = min((uint)max(x0, 0), c0.z - 1), ya = min((uint)max(y0, 0), c0.w - 1);
    uint xb = min((uint)max(x1, 0), c0.z - 1), yb = min((uint)max(y1, 0), c0.w - 1);
#if MODE == 1
    xa += asuint(c2.x); xb += asuint(c2.x); ya += asuint(c2.y); yb += asuint(c2.y);
#endif
    float ex = (float)x1 - u, ey = (float)y1 - v, fx = u - (float)x0, fy = v - (float)y0;
    float w00 = ey * ex, w10 = ey * fx, w01 = ex * fy, w11 = fy * fx;

    // words 0..(SRC_BPP / 4 - 1) of the four taps
    uint4 a[SRC_BPP / 4];
    [unroll] for (uint k = 0; k < SRC_BPP / 4; k++)
    {
        a[k].x = t0.Load((c0.x * ya + xa) * SRC_BPP + 4 * k);
        a[k].y = t0.Load((c0.x * ya + xb) * SRC_BPP + 4 * k);
        a[k].z = t0.Load((c0.x * yb + xa) * SRC_BPP + 4 * k);
        a[k].w = t0.Load((c0.x * yb + xb) * SRC_BPP + 4 * k);
    }
#if MODE == 2
    uint add[ADD_BPP / 4];
    [unroll] for (uint q = 0; q < ADD_BPP / 4; q++)
        add[q] = t1.Load((c3 * (id.y + 1) + id.x) * ADD_BPP + 4 * q);
#endif
    if (id.x >= c1.z || id.y >= c1.w)
        return;

    uint outw[(NOUT + 1) / 2];
    [unroll] for (uint i = 0; i < (NOUT + 1) / 2; i++)
        outw[i] = 0;
    [unroll] for (uint c = 0; c < NOUT; c++)
    {
#if MODE == 1
        uint wd = c / 2, hi = c % 2;
        float s = Half(a[wd].x, hi) * w00;
        s = Half(a[wd].y, hi) * w10 + s;
        s = s + Half(a[wd].z, hi) * w01;
        s = s + Half(a[wd].w, hi) * w11;
#else
        uint wd = c / 4, by = c % 4;
        float s = Int8(a[wd].x, by) * w00;
        s = Int8(a[wd].y, by) * w10 + s;
        s = s + Int8(a[wd].z, by) * w01;
        s = s + Int8(a[wd].w, by) * w11;
#endif
        if (c < NSCALED)
            s = s * asfloat((uint)SCALE_BITS);
#if MODE == 2
        s = Half(add[c / 2], c % 2) + s;
#endif
        outw[c / 2] |= ToHalf(s) << (16 * (c % 2));
    }
#if MODE == 1
    uint row = id.y + 1;
#else
    uint row = id.y;
#endif
    [unroll] for (uint j = 0; j < (NOUT + 1) / 2; j++)
        u0.Store((c1.x * row + id.x) * DST_BPP + 4 * j, outw[j]);
}
