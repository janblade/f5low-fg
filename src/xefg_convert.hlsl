// Tagged inputs in a format XeSS-FG takes but the pack's buffer does not hold as it is (xefg_own.h, ConvertIn; which formats
// and kinds: xefg_formats.h, measured against Intel's library). Intel's guide: motion vectors "R16G16_FLOAT or similar", depth
// "any depth format". The tag is copied as it is into a plain buffer on the application's list; this turns it into the pack's
// layout on ours: motion R16G16_FLOAT, depth 32-bit float, one texel per thread, at the pack's row pitch. A normalized format
// is decoded as a typed read would (UNORM to [0, 1], SNORM to [-1, 1]); the motion scale the application tags multiplies the
// result later, as with Intel (a game with UNORM / SNORM motion sets it to the range its encoding spans).
//
// Built by build.ps1: fxc /T cs_5_0 /E main /Fh xefg_convert_cs.h /Vn g_xefgConvertCS (root signature embedded).

#define RS "RootConstants(num32BitConstants=8, b0), SRV(t0), UAV(u0)"

cbuffer C : register(b0)
{
    uint W, H;           // the region, texels
    uint SrcPitch;       // bytes per row of the raw copy
    uint DstPitch;       // bytes per row of the pack's buffer
    uint Kind;           // 0 motion, first 4 bytes (R16G16B16A16_FLOAT); 1 motion, two floats (R32G32 / R32G32B32A32_FLOAT);
                         // 2 depth 24-bit UNORM in 32 bits; 3 depth 16-bit UNORM; 4 depth 16-bit float; 5 32 bits as they are
                         // (a 32-bit float depth-stencil region: copied whole, since D3D12 copies those only whole);
                         // motion: 6 two 16-bit UNORM; 7 two 16-bit SNORM; 8 two 8-bit UNORM; 9 two 8-bit SNORM;
                         // 10 R10G10B10A2_UNORM (x, y); 11 R11G11B10_FLOAT (x, y)
    uint Stride;         // bytes per texel of the raw copy
    uint SrcX, SrcY;     // the region's offset in the raw copy (the whole texture is copied)
};

ByteAddressBuffer Src : register(t0);
RWByteAddressBuffer Dst : register(u0);

// One 11-bit unsigned float of R11G11B10_FLOAT (5 exponent bits, 6 mantissa bits) as a float, built from its bits (exact, no exp2).
// Infinity and NaN (never a motion vector) read as 0.
float Float11(uint v)
{
    const uint e = (v >> 6) & 31u, m = v & 63u;
    if (e == 0)
        return (float) m * 9.5367431640625e-07;      // m / 64 * 2^-14
    if (e == 31)
        return 0.0;
    return asfloat(((e + 112u) << 23) | (m << 17));
}

// The extremes exactly: a division may be compiled as a multiply by the reciprocal, which lands a hair away from +-1.0
float Unorm(uint v, uint top)
{
    return v >= top ? 1.0 : (float) v / (float) top;
}

float Snorm(int v, int top)
{
    return v >= top ? 1.0 : v <= -top ? -1.0 : (float) v / (float) top;
}

[RootSignature(RS)]
[numthreads(8, 8, 1)]
void main(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= W || id.y >= H)
        return;
    const uint off = (id.y + SrcY) * SrcPitch + (id.x + SrcX) * Stride;
    // 16-bit texels sit at a 2-byte offset half the time; loads are 4-byte aligned.
    const uint half16 = (Src.Load(off & ~3u) >> ((off & 2u) * 8u)) & 0xFFFFu;
    uint v;
    if (Kind == 0)
        v = Src.Load(off);
    else if (Kind == 1)
    {
        const float2 f = asfloat(Src.Load2(off));
        v = f32tof16(f.x) | (f32tof16(f.y) << 16);
    }
    else if (Kind == 2)
    {
        // The largest value is exactly 1.0 (the far plane a forward-Z game clears to); the division may be compiled as
        // a multiply by the reciprocal, which lands a hair below it.
        const uint d = Src.Load(off) & 0xFFFFFFu;
        v = asuint(d == 0xFFFFFFu ? 1.0 : (float) d / 16777215.0);
    }
    else if (Kind == 3)
        v = asuint(half16 == 0xFFFFu ? 1.0 : (float) half16 / 65535.0);
    else if (Kind == 4)
        v = asuint(f16tof32(half16));
    else if (Kind >= 6)
    {
        float2 f;
        if (Kind == 6)
        {
            const uint w = Src.Load(off);
            f = float2(Unorm(w & 0xFFFFu, 65535u), Unorm(w >> 16, 65535u));
        }
        else if (Kind == 7)
        {
            const uint w = Src.Load(off);
            f = float2(Snorm((int) (w << 16) >> 16, 32767), Snorm((int) w >> 16, 32767));
        }
        else if (Kind == 8)
            f = float2(Unorm(half16 & 0xFFu, 255u), Unorm(half16 >> 8, 255u));
        else if (Kind == 9)
            f = float2(Snorm((int) (half16 << 24) >> 24, 127), Snorm((int) (half16 << 16) >> 24, 127));
        else if (Kind == 10)
        {
            const uint w = Src.Load(off);
            f = float2(Unorm(w & 1023u, 1023u), Unorm((w >> 10) & 1023u, 1023u));
        }
        else
        {
            const uint w = Src.Load(off);
            f = float2(Float11(w & 2047u), Float11((w >> 11) & 2047u));
        }
        v = f32tof16(f.x) | (f32tof16(f.y) << 16);
    }
    else
        v = Src.Load(off);
    Dst.Store(id.y * DstPitch + id.x * 4, v);
}
