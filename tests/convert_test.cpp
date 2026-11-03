// WARP check of xefg_convert.hlsl (xefg_own.h ConvertIn): every Kind turns a raw tag copy into the pack's layout, and the format
// table of src\xefg_formats.h (which format is taken how) on the host. Build: scripts\build_convert_test.ps1. Exit code 0 = PASS.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "xefg_convert_cs.h"
#include "../src/xefg_formats.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")

static uint16_t Half(float f) // round to nearest even, normal range only (enough for the test values)
{
    uint32_t x;
    memcpy(&x, &f, 4);
    const uint32_t sign = (x >> 16) & 0x8000u;
    const int exp = static_cast<int>((x >> 23) & 0xFF) - 127 + 15;
    uint32_t mant = x & 0x7FFFFFu;
    if (exp <= 0)
        return static_cast<uint16_t>(sign);
    uint32_t h = sign | (static_cast<uint32_t>(exp) << 10) | (mant >> 13);
    const uint32_t rest = mant & 0x1FFFu;
    if (rest > 0x1000u || (rest == 0x1000u && (h & 1u)))
        ++h;
    return static_cast<uint16_t>(h);
}

static float HalfToFloat(uint16_t h)
{
    const int sign = (h & 0x8000) ? -1 : 1, e = (h >> 10) & 31, m = h & 1023;
    if (e == 0)
        return static_cast<float>(sign) * std::ldexp(static_cast<float>(m), -24);
    if (e == 31)
        return static_cast<float>(sign) * INFINITY;
    return static_cast<float>(sign) * std::ldexp(1.0f + static_cast<float>(m) / 1024.0f, e - 15);
}
// A converted half against the exact value: 0 and +-1 exactly, anything else within the rounding of a float to a half (one ulp: the
// shader's f32tof16 may round to nearest or toward zero, and a division may be a multiplication by the reciprocal).
static bool HalfNear(uint16_t got, double want)
{
    const double g = HalfToFloat(got);
    if (want == 0.0 || std::fabs(want) == 1.0)
        return g == want;
    return std::fabs(g - want) <= std::fabs(want) / 1024.0 + 1e-7;
}

struct Gpu
{
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* a = nullptr;
    ID3D12GraphicsCommandList* g = nullptr;
    ID3D12Fence* f = nullptr;
    UINT64 v = 0;
    ID3D12RootSignature* rs = nullptr;
    ID3D12PipelineState* pso = nullptr;

    ID3D12Resource* Buf(size_t bytes, D3D12_HEAP_TYPE heap, bool uav)
    {
        D3D12_HEAP_PROPERTIES hp { heap };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = uav ? D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS : D3D12_RESOURCE_FLAG_NONE;
        const D3D12_RESOURCE_STATES s = heap == D3D12_HEAP_TYPE_UPLOAD   ? D3D12_RESOURCE_STATE_GENERIC_READ
                                        : heap == D3D12_HEAP_TYPE_READBACK ? D3D12_RESOURCE_STATE_COPY_DEST
                                                                           : D3D12_RESOURCE_STATE_COMMON;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, s, nullptr, IID_PPV_ARGS(&r));
        return r;
    }
    void Run()
    {
        g->Close();
        ID3D12CommandList* l = g;
        q->ExecuteCommandLists(1, &l);
        q->Signal(f, ++v);
        HANDLE e = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        f->SetEventOnCompletion(v, e);
        WaitForSingleObject(e, INFINITE);
        CloseHandle(e);
        a->Reset();
        g->Reset(a, nullptr);
    }
};

static void Transition(ID3D12GraphicsCommandList* g, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
{
    D3D12_RESOURCE_BARRIER br {};
    br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
    br.Transition.pResource = r;
    br.Transition.StateBefore = a;
    br.Transition.StateAfter = b;
    br.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
    g->ResourceBarrier(1, &br);
}

// Converts src (w x h texels of `stride` bytes at srcPitch) with `kind`; returns the w x h words at dstPitch.
static std::vector<uint32_t> Convert(Gpu& gpu, const std::vector<uint8_t>& src, uint32_t w, uint32_t h, uint32_t srcPitch,
                                     uint32_t kind, uint32_t stride, uint32_t dstPitch, uint32_t srcX = 0, uint32_t srcY = 0)
{
    const size_t dstBytes = static_cast<size_t>(dstPitch) * h;
    ID3D12Resource* up = gpu.Buf(src.size(), D3D12_HEAP_TYPE_UPLOAD, false);
    ID3D12Resource* raw = gpu.Buf(src.size(), D3D12_HEAP_TYPE_DEFAULT, false);
    ID3D12Resource* out = gpu.Buf(dstBytes, D3D12_HEAP_TYPE_DEFAULT, true);
    ID3D12Resource* rb = gpu.Buf(dstBytes, D3D12_HEAP_TYPE_READBACK, false);
    void* p = nullptr;
    up->Map(0, nullptr, &p);
    memcpy(p, src.data(), src.size());
    up->Unmap(0, nullptr);
    ID3D12GraphicsCommandList* g = gpu.g;
    g->CopyBufferRegion(raw, 0, up, 0, src.size());
    gpu.Run(); // as in the wrapper: the raw copy comes from another list, and every list starts with buffers in COMMON
    Transition(g, raw, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
    Transition(g, out, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
    const uint32_t k[8] = { w, h, srcPitch, dstPitch, kind, stride, srcX, srcY };
    g->SetComputeRootSignature(gpu.rs);
    g->SetPipelineState(gpu.pso);
    g->SetComputeRoot32BitConstants(0, 8, k, 0);
    g->SetComputeRootShaderResourceView(1, raw->GetGPUVirtualAddress());
    g->SetComputeRootUnorderedAccessView(2, out->GetGPUVirtualAddress());
    g->Dispatch((w + 7) / 8, (h + 7) / 8, 1);
    Transition(g, out, D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
    g->CopyBufferRegion(rb, 0, out, 0, dstBytes);
    gpu.Run();
    std::vector<uint32_t> r(static_cast<size_t>(w) * h);
    rb->Map(0, nullptr, &p);
    for (uint32_t y = 0; y < h; ++y)
        memcpy(&r[static_cast<size_t>(y) * w], static_cast<uint8_t*>(p) + static_cast<size_t>(y) * dstPitch, w * 4);
    const D3D12_RANGE none { 0, 0 };
    rb->Unmap(0, &none);
    for (ID3D12Resource* x : { up, raw, out, rb })
        x->Release();
    return r;
}

int main()
{
    Gpu gpu;
    IDXGIFactory4* fac = nullptr;
    IDXGIAdapter* warp = nullptr;
    if (FAILED(CreateDXGIFactory1(IID_PPV_ARGS(&fac))) || FAILED(fac->EnumWarpAdapter(IID_PPV_ARGS(&warp))) ||
        FAILED(D3D12CreateDevice(warp, D3D_FEATURE_LEVEL_11_0, IID_PPV_ARGS(&gpu.dev))))
    {
        printf("FAIL: no WARP device\n");
        return 1;
    }
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_COMPUTE }; // the generation lane may be a compute queue
    gpu.dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&gpu.q));
    gpu.dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_COMPUTE, IID_PPV_ARGS(&gpu.a));
    gpu.dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_COMPUTE, gpu.a, nullptr, IID_PPV_ARGS(&gpu.g));
    gpu.dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gpu.f));
    D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
    if (FAILED(gpu.dev->CreateRootSignature(0, g_xefgConvertCS, sizeof(g_xefgConvertCS), IID_PPV_ARGS(&gpu.rs))))
    {
        printf("FAIL: root signature\n");
        return 1;
    }
    pd.pRootSignature = gpu.rs;
    pd.CS = { g_xefgConvertCS, sizeof(g_xefgConvertCS) };
    if (FAILED(gpu.dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&gpu.pso))))
    {
        printf("FAIL: pipeline\n");
        return 1;
    }

    // Odd sizes: partial thread groups, and 2-byte texels on both halves of a word.
    const uint32_t w = 13, h = 5, dstPitch = 64; // dstPitch > w * 4, as the pack's own pitch can be
    int fails = 0;
    auto check = [&](const char* name, const std::vector<uint32_t>& got, auto expect) {
        int bad = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t e = expect(x, y), g = got[static_cast<size_t>(y) * w + x];
                if (e != g && bad++ < 3)
                    printf("  %s (%u,%u): got %08x want %08x\n", name, x, y, g, e);
            }
        printf("%s %s\n", bad ? "FAIL" : "ok  ", name);
        fails += bad != 0;
    };
    auto mvx = [](uint32_t x, uint32_t y) { return -3.25f + 0.5f * x - 0.125f * y; };
    auto mvy = [](uint32_t x, uint32_t y) { return 1.5f * y - 0.0625f * x; };
    auto pitchFor = [](uint32_t bytes) { return (bytes + 255u) & ~255u; };

    { // Kind 0: R16G16B16A16_FLOAT, R and G kept
        const uint32_t sp = pitchFor(w * 8);
        std::vector<uint8_t> s(static_cast<size_t>(sp) * h, 0xCD);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint16_t t[4] = { Half(mvx(x, y)), Half(mvy(x, y)), 0x3C00, 0x7BFF };
                memcpy(&s[static_cast<size_t>(y) * sp + x * 8], t, 8);
            }
        check("R16G16B16A16_FLOAT motion", Convert(gpu, s, w, h, sp, 0, 8, dstPitch),
              [&](uint32_t x, uint32_t y) { return Half(mvx(x, y)) | (static_cast<uint32_t>(Half(mvy(x, y))) << 16); });
    }
    for (uint32_t stride : { 8u, 12u, 16u }) // Kind 1: R32G32_FLOAT / R32G32B32_FLOAT / R32G32B32A32_FLOAT, to halves
    {
        const uint32_t sp = pitchFor(w * stride);
        std::vector<uint8_t> s(static_cast<size_t>(sp) * h, 0xCD);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const float t[2] = { mvx(x, y), mvy(x, y) };
                memcpy(&s[static_cast<size_t>(y) * sp + x * stride], t, 8);
            }
        check(stride == 8 ? "R32G32_FLOAT motion" : stride == 12 ? "R32G32B32_FLOAT motion" : "R32G32B32A32_FLOAT motion", Convert(gpu, s, w, h, sp, 1, stride, dstPitch),
              [&](uint32_t x, uint32_t y) { return Half(mvx(x, y)) | (static_cast<uint32_t>(Half(mvy(x, y))) << 16); });
    }
    auto close = [](uint32_t got, float want) {
        float f;
        memcpy(&f, &got, 4);
        return std::fabs(f - want) <= 1e-6f;
    };
    { // Kind 2: D24_UNORM_S8_UINT plane 0 (stencil bits above must not leak in)
        const uint32_t sp = pitchFor(w * 4);
        std::vector<uint8_t> s(static_cast<size_t>(sp) * h, 0);
        // (0,0) is the largest value, the far plane a forward-Z game clears to: exactly 1.0
        auto d24 = [](uint32_t x, uint32_t y) { return x == 0 && y == 0 ? 0xFFFFFFu : (x * 1290361u + y * 3355443u) & 0xFFFFFFu; };
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t v = d24(x, y) | 0xA5000000u;
                memcpy(&s[static_cast<size_t>(y) * sp + x * 4], &v, 4);
            }
        const auto got = Convert(gpu, s, w, h, sp, 2, 4, dstPitch);
        check("D24_UNORM depth", got, [&](uint32_t x, uint32_t y) {
            const uint32_t g = got[static_cast<size_t>(y) * w + x];
            if (d24(x, y) == 0xFFFFFFu)
                return 0x3F800000u;
            return close(g, static_cast<float>(d24(x, y)) / 16777215.0f) ? g : ~g;
        });
    }
    { // Kind 3 / 4: 16-bit UNORM and FLOAT depth
        const uint32_t sp = pitchFor(w * 2);
        auto d16 = [](uint32_t x, uint32_t y) {
            return x == 0 && y == 0 ? uint16_t(0xFFFF) : static_cast<uint16_t>((x * 4099u + y * 13001u) & 0xFFFFu);
        };
        auto f16 = [](uint32_t x, uint32_t y) { return 0.03125f * (x + 1) + 0.25f * y; };
        std::vector<uint8_t> s(static_cast<size_t>(sp) * h, 0), t(static_cast<size_t>(sp) * h, 0);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint16_t a = d16(x, y), b = Half(f16(x, y));
                memcpy(&s[static_cast<size_t>(y) * sp + x * 2], &a, 2);
                memcpy(&t[static_cast<size_t>(y) * sp + x * 2], &b, 2);
            }
        const auto got = Convert(gpu, s, w, h, sp, 3, 2, dstPitch);
        check("D16_UNORM depth", got, [&](uint32_t x, uint32_t y) {
            const uint32_t g = got[static_cast<size_t>(y) * w + x];
            if (d16(x, y) == 0xFFFF)
                return 0x3F800000u;
            return close(g, d16(x, y) / 65535.0f) ? g : ~g;
        });
        const auto gotF = Convert(gpu, t, w, h, sp, 4, 2, dstPitch);
        check("R16_FLOAT depth", gotF, [&](uint32_t x, uint32_t y) {
            const uint32_t g = gotF[static_cast<size_t>(y) * w + x];
            return close(g, f16(x, y)) ? g : ~g;
        });
    }
    { // Kind 5 at an offset: a 32-bit depth region inside a larger texture, copied whole (D3D12 copies depth-stencil
      // textures only whole); the offset rule is the same for every kind
        const uint32_t tw = w + 7, th = h + 4, ox = 5, oy = 3, sp = pitchFor(tw * 4);
        std::vector<uint8_t> s(static_cast<size_t>(sp) * th, 0);
        auto bits = [](uint32_t x, uint32_t y) { return 0x3E000000u + x * 977u + y * 131071u; };
        for (uint32_t y = 0; y < th; ++y)
            for (uint32_t x = 0; x < tw; ++x)
            {
                const uint32_t v = bits(x, y);
                memcpy(&s[static_cast<size_t>(y) * sp + x * 4], &v, 4);
            }
        check("32-bit depth region at an offset", Convert(gpu, s, w, h, sp, 5, 4, dstPitch, ox, oy),
              [&](uint32_t x, uint32_t y) { return bits(x + ox, y + oy); });
    }

    // ---- Motion kinds 6..11: normalized, 8-bit and packed formats, decoded as a typed read would, to R16G16_FLOAT ----
    // A texel's value comes from a sequence with the extremes first (the encoding's minimum, -1 / 0 / +1, maximum), then a hash; the
    // bytes of the texel the vector does not use (blue, alpha, ...) hold 0xCD so that a leak shows.
    auto hash = [](uint32_t x, uint32_t y, uint32_t c) { return (x * 2654435761u) ^ (y * 40503u + c * 977u + 12345u); };
    auto motionCase = [&](const char* name, uint32_t kind, uint32_t stride, auto encode, auto want) {
        const uint32_t sp = pitchFor(w * stride);
        std::vector<uint8_t> s(static_cast<size_t>(sp) * h, 0xCD);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
                encode(x, y, &s[static_cast<size_t>(y) * sp + x * stride]);
        const auto got = Convert(gpu, s, w, h, sp, kind, stride, dstPitch);
        int bad = 0;
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const uint32_t g = got[static_cast<size_t>(y) * w + x];
                const double wx = want(x, y, 0), wy = want(x, y, 1);
                if ((!HalfNear(static_cast<uint16_t>(g & 0xFFFF), wx) || !HalfNear(static_cast<uint16_t>(g >> 16), wy)) && bad++ < 3)
                    printf("  %s (%u,%u): got %g, %g want %g, %g\n", name, x, y, HalfToFloat(static_cast<uint16_t>(g & 0xFFFF)),
                           HalfToFloat(static_cast<uint16_t>(g >> 16)), wx, wy);
            }
        printf("%s %s\n", bad ? "FAIL" : "ok  ", name);
        fails += bad != 0;
    };
    // the value sequences: index = y * w + x, channel c
    auto u16v = [&](uint32_t x, uint32_t y, uint32_t c) -> uint32_t {
        static const uint32_t sp[] = { 0, 65535, 32768, 1, 32767, 65534 };
        const uint32_t i = y * w + x;
        return i < 6 ? sp[(i + c) % 6] : hash(x, y, c) & 0xFFFFu;
    };
    auto s16v = [&](uint32_t x, uint32_t y, uint32_t c) -> int32_t {
        static const int32_t sp[] = { -32768, -32767, 0, 32767, 1, -1, 16384, -16384 };
        const uint32_t i = y * w + x;
        return i < 8 ? sp[(i + c) % 8] : static_cast<int16_t>(hash(x, y, c) & 0xFFFFu);
    };
    auto u8v = [&](uint32_t x, uint32_t y, uint32_t c) -> uint32_t {
        static const uint32_t sp[] = { 0, 255, 128, 1, 127, 254 };
        const uint32_t i = y * w + x;
        return i < 6 ? sp[(i + c) % 6] : hash(x, y, c) & 0xFFu;
    };
    auto s8v = [&](uint32_t x, uint32_t y, uint32_t c) -> int32_t {
        static const int32_t sp[] = { -128, -127, 0, 127, 1, -1, 64, -64 };
        const uint32_t i = y * w + x;
        return i < 8 ? sp[(i + c) % 8] : static_cast<int8_t>(hash(x, y, c) & 0xFFu);
    };
    for (uint32_t stride : { 4u, 8u }) // R16G16_UNORM, R16G16B16A16_UNORM
        motionCase(stride == 4 ? "R16G16_UNORM motion" : "R16G16B16A16_UNORM motion", 6, stride,
                   [&](uint32_t x, uint32_t y, uint8_t* t) {
                       const uint16_t v[2] = { static_cast<uint16_t>(u16v(x, y, 0)), static_cast<uint16_t>(u16v(x, y, 1)) };
                       memcpy(t, v, 4);
                   },
                   [&](uint32_t x, uint32_t y, int c) { return u16v(x, y, c) / 65535.0; });
    for (uint32_t stride : { 4u, 8u }) // R16G16_SNORM, R16G16B16A16_SNORM: -32768 and -32767 are both -1
        motionCase(stride == 4 ? "R16G16_SNORM motion" : "R16G16B16A16_SNORM motion", 7, stride,
                   [&](uint32_t x, uint32_t y, uint8_t* t) {
                       const int16_t v[2] = { static_cast<int16_t>(s16v(x, y, 0)), static_cast<int16_t>(s16v(x, y, 1)) };
                       memcpy(t, v, 4);
                   },
                   [&](uint32_t x, uint32_t y, int c) { return std::fmax(s16v(x, y, c) / 32767.0, -1.0); });
    for (uint32_t stride : { 2u, 4u }) // R8G8_UNORM, R8G8B8A8_UNORM
        motionCase(stride == 2 ? "R8G8_UNORM motion" : "R8G8B8A8_UNORM motion", 8, stride,
                   [&](uint32_t x, uint32_t y, uint8_t* t) {
                       t[0] = static_cast<uint8_t>(u8v(x, y, 0));
                       t[1] = static_cast<uint8_t>(u8v(x, y, 1));
                   },
                   [&](uint32_t x, uint32_t y, int c) { return u8v(x, y, c) / 255.0; });
    for (uint32_t stride : { 2u, 4u }) // R8G8_SNORM, R8G8B8A8_SNORM
        motionCase(stride == 2 ? "R8G8_SNORM motion" : "R8G8B8A8_SNORM motion", 9, stride,
                   [&](uint32_t x, uint32_t y, uint8_t* t) {
                       t[0] = static_cast<uint8_t>(s8v(x, y, 0));
                       t[1] = static_cast<uint8_t>(s8v(x, y, 1));
                   },
                   [&](uint32_t x, uint32_t y, int c) { return std::fmax(s8v(x, y, c) / 127.0, -1.0); });
    {   // R10G10B10A2_UNORM: x and y 10 bits, the third channel and alpha must not leak in
        auto v10 = [&](uint32_t x, uint32_t y, uint32_t c) -> uint32_t {
            static const uint32_t sp[] = { 0, 1023, 512, 1, 511, 1022 };
            const uint32_t i = y * w + x;
            return i < 6 ? sp[(i + c) % 6] : hash(x, y, c) & 1023u;
        };
        motionCase("R10G10B10A2_UNORM motion", 10, 4,
                   [&](uint32_t x, uint32_t y, uint8_t* t) {
                       const uint32_t v = v10(x, y, 0) | (v10(x, y, 1) << 10) | (v10(x, y, 2) << 20) | ((x & 3u) << 30);
                       memcpy(t, &v, 4);
                   },
                   [&](uint32_t x, uint32_t y, int c) { return v10(x, y, c) / 1023.0; });
    }
    {   // R11G11B10_FLOAT: x and y are unsigned 11-bit floats (5 exponent, 6 mantissa bits); zero, a subnormal, 1, 1.5, 8, the largest
        // finite one, and infinity / NaN (read as 0)
        static const uint32_t ef[][2] = { { 0, 0 }, { 0, 1 }, { 0, 63 }, { 1, 0 }, { 15, 0 }, { 15, 32 }, { 18, 0 }, { 30, 63 }, { 31, 0 }, { 31, 5 }, { 14, 63 } };
        auto f11 = [&](uint32_t x, uint32_t y, uint32_t c) -> uint32_t {
            const uint32_t i = y * w + x;
            if (i < 11)
                return (ef[(i + c) % 11][0] << 6) | ef[(i + c) % 11][1];
            return ((hash(x, y, c) % 2047u) & 63u) | ((hash(x, y, c + 7) % 28u + 1u) << 6);   // any mantissa, exponent 1..28
        };
        auto f11v = [](uint32_t b) -> double {
            const uint32_t e = (b >> 6) & 31u, m = b & 63u;
            if (e == 31)
                return 0.0;
            return e == 0 ? m / 64.0 * std::ldexp(1.0, -14) : (1.0 + m / 64.0) * std::ldexp(1.0, static_cast<int>(e) - 15);
        };
        motionCase("R11G11B10_FLOAT motion", 11, 4,
                   [&](uint32_t x, uint32_t y, uint8_t* t) {
                       const uint32_t v = f11(x, y, 0) | (f11(x, y, 1) << 11) | ((hash(x, y, 2) & 1023u) << 22);
                       memcpy(t, &v, 4);
                   },
                   [&](uint32_t x, uint32_t y, int c) { return f11v(f11(x, y, c)); });
    }

    // ---- The format table (xefg_formats.h), on the host ----
    using xefgown::Take;
    uint32_t kd = 0, st = 0;
    auto motionIs = [&](int f, Take t, uint32_t k = 0, uint32_t s = 0) {
        const Take got = xefgown::MotionTake(static_cast<DXGI_FORMAT>(f), kd, st);
        const bool ok = got == t && (t != Take::Convert || (kd == k && st == s));
        if (!ok)
            printf("  motion format %d (%s): got take %d kind %u stride %u, want take %d kind %u stride %u\n", f, xefgown::FormatName(static_cast<DXGI_FORMAT>(f)),
                   static_cast<int>(got), kd, st, static_cast<int>(t), k, s);
        return ok;
    };
    auto depthIs = [&](int f, Take t, uint32_t k = 0, uint32_t s = 0) {
        const Take got = xefgown::DepthTake(static_cast<DXGI_FORMAT>(f), kd, st);
        const bool ok = got == t && (t != Take::Convert || (kd == k && st == s));
        if (!ok)
            printf("  depth format %d (%s): got take %d kind %u stride %u, want take %d kind %u stride %u\n", f, xefgown::FormatName(static_cast<DXGI_FORMAT>(f)),
                   static_cast<int>(got), kd, st, static_cast<int>(t), k, s);
        return ok;
    };
    bool tableOk = true;
    for (int f : { 33, 34 })
        tableOk &= motionIs(f, Take::Raw);                  // R16G16 typeless / float: the pack's own layout
    tableOk &= motionIs(9, Take::Convert, 0, 8) & motionIs(10, Take::Convert, 0, 8);
    tableOk &= motionIs(15, Take::Convert, 1, 8) & motionIs(16, Take::Convert, 1, 8);
    tableOk &= motionIs(5, Take::Convert, 1, 12) & motionIs(6, Take::Convert, 1, 12);
    tableOk &= motionIs(1, Take::Convert, 1, 16) & motionIs(2, Take::Convert, 1, 16);
    tableOk &= motionIs(35, Take::Convert, 6, 4) & motionIs(11, Take::Convert, 6, 8);       // the bug: R16G16_UNORM was copied as halves
    tableOk &= motionIs(37, Take::Convert, 7, 4) & motionIs(13, Take::Convert, 7, 8);       // the bug: R16G16_SNORM was refused
    tableOk &= motionIs(48, Take::Convert, 8, 2) & motionIs(49, Take::Convert, 8, 2) & motionIs(27, Take::Convert, 8, 4) & motionIs(28, Take::Convert, 8, 4);
    tableOk &= motionIs(51, Take::Convert, 9, 2) & motionIs(31, Take::Convert, 9, 4);
    tableOk &= motionIs(23, Take::Convert, 10, 4) & motionIs(24, Take::Convert, 10, 4) & motionIs(26, Take::Convert, 11, 4);
    // refused: integers (Intel reads them as float bit patterns), sRGB, one channel, depth, colour with an alpha-only meaning, unknown
    for (int f : { 0, 3, 4, 7, 8, 12, 14, 17, 18, 25, 29, 30, 32, 36, 38, 40, 41, 42, 43, 50, 52, 54, 55, 56, 61, 71, 87 })
        tableOk &= motionIs(f, Take::No);
    for (int f : { 19, 20, 21, 39, 40, 41 })
        tableOk &= depthIs(f, Take::Raw);
    tableOk &= depthIs(44, Take::Convert, 2, 4) & depthIs(45, Take::Convert, 2, 4) & depthIs(46, Take::Convert, 2, 4);
    tableOk &= depthIs(53, Take::Convert, 3, 2) & depthIs(55, Take::Convert, 3, 2) & depthIs(56, Take::Convert, 3, 2) & depthIs(54, Take::Convert, 4, 2);
    for (int f : { 0, 2, 28, 33, 34, 35, 37, 42, 43, 57, 58, 59, 61, 71 })
        tableOk &= depthIs(f, Take::No);
    // R16G16_UNORM is not a raw-copy format any more, and the formats that are keep their texel size
    tableOk &= xefgown::TexelBytesOf(static_cast<DXGI_FORMAT>(35)) == 0 && xefgown::TexelBytesOf(static_cast<DXGI_FORMAT>(34)) == 4 &&
               xefgown::TexelBytesOf(static_cast<DXGI_FORMAT>(28)) == 4 && xefgown::TexelBytesOf(static_cast<DXGI_FORMAT>(40)) == 4;
    printf("%s format table (xefg_formats.h)\n", tableOk ? "ok  " : "FAIL");
    fails += !tableOk;
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
