// The displacement warp ahead of net B, as a template. With COPY0 0 and FLOW_BPP 16 it must reproduce
// the original module bit for bit; with COPY0 1 and FLOW_BPP 8 it is the multi-frame
// model's warp, which has one flow: the previous frame's channel is passed through and only the current
// frame's is warped.
//
// Per pixel (x, y), channel c of t0 (int2 texels, the two frames' luma) displaced by (dx, dy) = halfs
// 2c, 2c + 1 of t1 (FLOW_BPP bytes per pixel, pixel (x, y + 1), pitch c0.z) - with COPY0, channel 1 by
// halfs 0, 1 and channel 0 not at all:
//   x0 = clamp(x + floor(dx), 0, W - 1), x1 = clamp(x + 1 + floor(dx), 0, W - 1), y likewise; f = frac
//   a = s10 fx + s00 (1 - fx); r = ((s01 (1 - fx) - a) + s11 fx) fy + a
//   out byte c = clamp(trunc(r), -128, 127); 4 bytes per pixel at row y + 1, inside (W, H) only.
//
// Defines: FLOW_BPP (16 or 8), COPY0 (0 or 1).

Texture2D<int2> t0 : register(t0);
ByteAddressBuffer t1 : register(t1);
RWByteAddressBuffer u0 : register(u0);

cbuffer RootConstants : register(b0) { uint4 c0; };

int Warp(uint2 id, uint ch, float dx, float dy)
{
    int ix = (int)floor(dx), iy = (int)floor(dy);
    float fx = frac(dx), fy = frac(dy);
    int xa = min(max(ix + (int)id.x, 0), (int)c0.x - 1);
    int xb = min(max((int)id.x + 1 + ix, 0), (int)c0.x - 1);
    int ya = min(max(iy + (int)id.y, 0), (int)c0.y - 1);
    int yb = min(max((int)id.y + 1 + iy, 0), (int)c0.y - 1);
    float s00 = (float)t0.Load(int3(xa, ya, 0))[ch];
    float s10 = (float)t0.Load(int3(xb, ya, 0))[ch];
    float s01 = (float)t0.Load(int3(xa, yb, 0))[ch];
    float s11 = (float)t0.Load(int3(xb, yb, 0))[ch];
    float a = s10 * fx + s00 * (1.0f - fx);
    float t = (s01 * (1.0f - fx) - a) + s11 * fx;
    float r = t * fy + a;
    return min(max((int)r, -128), 127);
}

[RootSignature("DescriptorTable(SRV(t0), SRV(t1), UAV(u0)), RootConstants(num32BitConstants=4, b0)")]
[numthreads(8, 8, 1)]
void CSMain(uint3 id : SV_DispatchThreadID)
{
    uint base = (c0.z * (id.y + 1) + id.x) * FLOW_BPP;
    uint w0 = t1.Load(base);
#if COPY0
    int v0 = min(max(t0.Load(int3(id.xy, 0)).x, -128), 127);
    int v1 = Warp(id.xy, 1, f16tof32(w0), f16tof32(w0 >> 16));
#else
    uint w1 = t1.Load(base + 4);
    int v0 = Warp(id.xy, 0, f16tof32(w0), f16tof32(w0 >> 16));
    int v1 = Warp(id.xy, 1, f16tof32(w1), f16tof32(w1 >> 16));
#endif
    if (id.x >= c0.x || id.y >= c0.y)
        return;
    u0.Store((c0.z * (id.y + 1) + id.x) * 4, ((uint)v0 & 255) | (((uint)v1 & 255) << 8));
}
