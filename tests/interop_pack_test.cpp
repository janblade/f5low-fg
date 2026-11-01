// interop_pack_test - the pack's CUDA graph fed and read through D3D12, as the xefg_cuda DLL will do:
// tagged inputs are copied by a D3D12 queue into shared buffers, the shared fence hands the frame to
// CUDA, the graph runs, and the output buffer is read back by D3D12 and compared byte for byte with
// the pack's expectation. No game, no swap chain; this isolates the sharing path.
//
// Usage: interop_pack_test.exe <pack dir> [--bench N]
#include "xefg_interop.h"

#include <cstdlib>

using namespace xefgio;

static bool Ok(HRESULT hr, const char* what)
{
    if (FAILED(hr))
        printf("%s failed: 0x%08lx\n", what, static_cast<unsigned long>(hr));
    return SUCCEEDED(hr);
}

static ID3D12Resource* HeapBuffer(ID3D12Device* d, D3D12_HEAP_TYPE type, size_t bytes)
{
    D3D12_HEAP_PROPERTIES hp { type };
    D3D12_RESOURCE_DESC rd {};
    rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
    rd.Width = bytes;
    rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
    rd.SampleDesc.Count = 1;
    rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
    ID3D12Resource* r = nullptr;
    const D3D12_RESOURCE_STATES st = type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST;
    d->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, nullptr, IID_PPV_ARGS(&r));
    return r;
}

int main(int argc, char** argv)
{
    if (argc < 2)
    {
        printf("usage: interop_pack_test <pack dir> [--bench N]\n");
        return 1;
    }
    int bench = 0;
    for (int i = 2; i < argc; ++i)
        if (!strcmp(argv[i], "--bench") && i + 1 < argc)
            bench = atoi(argv[++i]);

    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    ID3D12Device* dev = nullptr;
    for (UINT i = 0; !dev; ++i)
    {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 ad;
        a->GetDesc1(&ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && ad.VendorId == 0x10DE &&
            SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
            printf("adapter: %S\n", ad.Description);
        a->Release();
    }
    if (!dev)
    {
        printf("no NVIDIA D3D12 device\n");
        return 1;
    }

    Interop io;
    if (!io.Init(dev))
    {
        printf("interop: %s\n", io.error.c_str());
        return 1;
    }
    Pack pack(io.cu);
    if (!pack.Load(argv[1]))
    {
        printf("load: %s\n", pack.error.c_str());
        return 1;
    }

    static const char* const kRoles[] = { "frame_cur", "frame_prev", "motion", "depth", "noise", "output" };
    std::vector<SharedBuf> bufs;
    std::vector<int> roleRes;
    bufs.reserve(6);
    for (const char* role : kRoles)
    {
        const int ri = pack.Role(role);
        if (ri < 0)
            continue;
        SharedBuf b;
        if (!io.CreateBuffer(pack.res[ri].bytes, b))
        {
            printf("%s: %s\n", role, io.error.c_str());
            return 1;
        }
        pack.SetExternal(ri, b.ptr);
        bufs.push_back(b);
        roleRes.push_back(ri);
    }
    if (!pack.Build(false))
    {
        printf("build: %s\n", pack.error.c_str());
        return 1;
    }

    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* al = nullptr;
    ID3D12GraphicsCommandList* cl = nullptr;
    ID3D12Fence* hostFence = nullptr;
    if (!Ok(dev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q)), "queue") ||
        !Ok(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&al)), "allocator") ||
        !Ok(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, al, nullptr, IID_PPV_ARGS(&cl)), "list") ||
        !Ok(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&hostFence)), "fence"))
        return 1;
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    UINT64 hostValue = 0;
    auto hostWait = [&] {
        q->Signal(hostFence, ++hostValue);
        if (hostFence->GetCompletedValue() < hostValue)
        {
            hostFence->SetEventOnCompletion(hostValue, ev);
            WaitForSingleObject(ev, 30000);
        }
    };

    // Inputs: the pack's init data, repacked into the pitched layout, copied by D3D12 into the shared buffers.
    std::vector<ID3D12Resource*> uploads;
    for (size_t k = 0; k < roleRes.size(); ++k)
    {
        const Resource& x = pack.res[roleRes[k]];
        if (x.init.empty())
            continue;
        const auto tight = Slurp(pack.dir + "\\" + x.init);
        if (tight.size() != x.TightBytes())
        {
            printf("%s: init is %zu bytes, expected %zu\n", x.init.c_str(), tight.size(), x.TightBytes());
            return 1;
        }
        ID3D12Resource* up = HeapBuffer(dev, D3D12_HEAP_TYPE_UPLOAD, x.bytes);
        uint8_t* p = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&p));
        if (x.tex)
        {
            const size_t row = static_cast<size_t>(x.w) * x.texel;
            for (size_t y = 0; y < x.Rows(); ++y)
                memcpy(p + y * x.pitch, tight.data() + y * row, row);
        }
        else
            memcpy(p, tight.data(), tight.size());
        up->Unmap(0, nullptr);
        cl->CopyBufferRegion(bufs[k].res, 0, up, 0, x.bytes);
        uploads.push_back(up);
    }
    cl->Close();
    ID3D12CommandList* lists[] = { cl };
    q->ExecuteCommandLists(1, lists);
    hostWait();

    CUstream stream = nullptr;
    io.cu.StreamCreate(&stream, 1);
    const int out = pack.Role("output");
    size_t outBuf = 0;
    for (size_t k = 0; k < roleRes.size(); ++k)
        if (roleRes[k] == out)
            outBuf = k;
    const Resource& ox = pack.res[out];
    ID3D12Resource* rb = HeapBuffer(dev, D3D12_HEAP_TYPE_READBACK, ox.bytes);

    UINT64 fv = 0;
    // One frame: D3D12 signals, CUDA waits and runs the graph and signals, D3D12 waits.
    auto frame = [&](bool readback) -> bool {
        const UINT64 toCuda = ++fv, toD3d = ++fv;
        q->Signal(io.fence, toCuda);
        if (!io.CudaWait(stream, toCuda) || !pack.Launch(stream) || !io.CudaSignal(stream, toD3d))
        {
            printf("frame: %s %s\n", io.error.c_str(), pack.error.c_str());
            return false;
        }
        q->Wait(io.fence, toD3d);
        if (readback)
        {
            al->Reset();
            cl->Reset(al, nullptr);
            cl->CopyBufferRegion(rb, 0, bufs[outBuf].res, 0, ox.bytes);
            cl->Close();
            q->ExecuteCommandLists(1, lists);
        }
        hostWait();
        return true;
    };
    if (!frame(true))
        return 1;

    uint8_t* got = nullptr;
    rb->Map(0, nullptr, reinterpret_cast<void**>(&got));
    const auto want = Slurp(pack.dir + "\\" + pack.expect);
    const size_t row = static_cast<size_t>(ox.w) * ox.texel;
    size_t bad = 0;
    for (size_t y = 0; y < ox.Rows(); ++y)
        for (size_t i = 0; i < row; ++i)
            if (got[y * ox.pitch + i] != want[y * row + i])
                ++bad;
    rb->Unmap(0, nullptr);
    printf("output via D3D12 readback: %zu of %zu bytes differ\n", bad, want.size());
    int rc = (bad == 0 && want.size() == row * ox.Rows()) ? 0 : 1;
    printf("%s\n", rc == 0 ? "shared-buffer path matches Intel's shader chain exactly" : "MISMATCH");

    if (bench > 0 && rc == 0)
    {
        LARGE_INTEGER f, a, b;
        QueryPerformanceFrequency(&f);
        for (int i = 0; i < 10; ++i)
            frame(false);
        std::vector<double> t;
        for (int i = 0; i < bench; ++i)
        {
            QueryPerformanceCounter(&a);
            frame(false);
            QueryPerformanceCounter(&b);
            t.push_back(1000.0 * double(b.QuadPart - a.QuadPart) / double(f.QuadPart));
        }
        std::sort(t.begin(), t.end());
        printf("D3D12 -> CUDA graph -> D3D12 round trip: median %.3f ms, min %.3f ms over %d frames\n", t[t.size() / 2], t[0], bench);
    }
    return rc;
}
