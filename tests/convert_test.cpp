// WARP check of xefg_convert.hlsl (xefg_own.h ConvertIn): every Kind turns a raw tag copy into the pack's layout.
// Build: build_convert_test.ps1. Exit code 0 = PASS.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <vector>

#include "xefg_convert_cs.h"

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
    for (uint32_t stride : { 8u, 16u }) // Kind 1: R32G32_FLOAT / R32G32B32A32_FLOAT, to halves
    {
        const uint32_t sp = pitchFor(w * stride);
        std::vector<uint8_t> s(static_cast<size_t>(sp) * h, 0xCD);
        for (uint32_t y = 0; y < h; ++y)
            for (uint32_t x = 0; x < w; ++x)
            {
                const float t[2] = { mvx(x, y), mvy(x, y) };
                memcpy(&s[static_cast<size_t>(y) * sp + x * stride], t, 8);
            }
        check(stride == 8 ? "R32G32_FLOAT motion" : "R32G32B32A32_FLOAT motion", Convert(gpu, s, w, h, sp, 1, stride, dstPitch),
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
    printf(fails ? "FAIL (%d)\n" : "PASS\n", fails);
    return fails ? 1 : 0;
}
