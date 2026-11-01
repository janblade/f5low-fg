// xefg_interop.h - D3D12 <-> CUDA sharing for the xefg_cuda DLL (and its offline test).
//
// Findings from tools/interop/README.md: CUDA 13 has no cuGraphicsD3D12RegisterResource, but the
// external-memory API imports a D3D12 shared *committed* resource (dedicated flag) and a shared fence
// as an external semaphore. The CUDA device must be the one whose LUID is the D3D12 adapter's.
// Buffers are used rather than textures: the pack's textures are pitched rows (256-byte aligned), the
// layout CopyTextureRegion writes into a buffer footprint.
#pragma once

#include "xefg_pack.h"

#include <d3d12.h>
#include <dxgi1_4.h>

namespace xefgio
{
using namespace xefgpack;

struct SharedBuf
{
    ID3D12Resource* res = nullptr;
    HANDLE handle = nullptr;
    CUexternalMemory mem = nullptr;
    CUdeviceptr ptr = 0;
    size_t bytes = 0;
};

class Interop
{
public:
    Cuda cu;
    CUcontext ctx = nullptr;
    int sm = 0;                         // the GPU's compute capability as sm_NN (Init)
    ID3D12Device* dev = nullptr;
    ID3D12Fence* fence = nullptr;
    CUexternalSemaphore sem = nullptr;
    std::string error;

    ~Interop() { Release(); }

    // The CUDA device whose LUID is the D3D12 device's adapter's (cuInit done), or -1.
    static CUdevice FindDevice(Cuda& c, ID3D12Device* d)
    {
        const LUID luid = d->GetAdapterLuid();
        int n = 0;
        c.DeviceGetCount(&n);
        for (int i = 0; i < n; ++i)
        {
            CUdevice dv {};
            char l[8] {};
            unsigned mask = 0;
            if (c.DeviceGet(&dv, i) == 0 && c.DeviceGetLuid(l, &mask, dv) == 0 && memcmp(l, &luid, sizeof(LUID)) == 0)
                return dv;
        }
        return -1;
    }
    // A device's compute capability as sm_NN.
    static int DeviceSm(Cuda& c, CUdevice dv)
    {
        int major = 0, minor = 0;
        c.DeviceGetAttribute(&major, 75, dv);       // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MAJOR
        c.DeviceGetAttribute(&minor, 76, dv);       // CU_DEVICE_ATTRIBUTE_COMPUTE_CAPABILITY_MINOR
        return major * 10 + minor;
    }

    // Picks the CUDA device whose LUID is the D3D12 device's, makes a context, shares one fence.
    bool Init(ID3D12Device* d)
    {
        dev = d;
        if (!cu.Load())
            return Fail("nvcuda.dll missing or lacks an entry point");
        CUresult r = cu.Init(0);
        if (r)
            return Fail(std::string("cuInit: ") + cu.Err(r));
        const CUdevice found = FindDevice(cu, d);
        if (found < 0)
            return Fail("no CUDA device matches the D3D12 adapter LUID");
        sm = DeviceSm(cu, found);
        if ((r = cu.CtxCreate(&ctx, 0, found)))
            return Fail(std::string("cuCtxCreate: ") + cu.Err(r));

        HANDLE fh = nullptr;
        if (FAILED(d->CreateFence(0, D3D12_FENCE_FLAG_SHARED, IID_PPV_ARGS(&fence))) ||
            FAILED(d->CreateSharedHandle(fence, nullptr, GENERIC_ALL, nullptr, &fh)))
            return Fail("shared fence");
        ExtSemHandleDesc sd {};
        sd.type = kExtSemD3D12Fence;
        sd.handle.win32.handle = fh;
        r = cu.ImportExternalSemaphore(&sem, &sd);
        CloseHandle(fh);
        if (r)
            return Fail(std::string("cuImportExternalSemaphore: ") + cu.Err(r));
        return true;
    }

    // A committed, shared, default-heap buffer CUDA can address.
    bool CreateBuffer(size_t bytes, SharedBuf& b)
    {
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = bytes;
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_SHARED, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr,
                                                IID_PPV_ARGS(&b.res))))
            return Fail("CreateCommittedResource(shared)");
        if (FAILED(dev->CreateSharedHandle(b.res, nullptr, GENERIC_ALL, nullptr, &b.handle)))
            return Fail("CreateSharedHandle");
        // The dedicated allocation can be larger than the buffer; CUDA wants its size.
        const D3D12_RESOURCE_ALLOCATION_INFO ai = dev->GetResourceAllocationInfo(0, 1, &rd);
        ExtMemHandleDesc md {};
        md.type = kExtMemD3D12Resource;
        md.handle.win32.handle = b.handle;
        md.size = ai.SizeInBytes;
        md.flags = kExtMemDedicated;
        CUresult r = cu.ImportExternalMemory(&b.mem, &md);
        if (r)
            return Fail(std::string("cuImportExternalMemory: ") + cu.Err(r));
        ExtMemBufferDesc bd {};
        bd.size = bytes;
        if ((r = cu.ExternalMemoryGetMappedBuffer(&b.ptr, b.mem, &bd)))
            return Fail(std::string("cuExternalMemoryGetMappedBuffer: ") + cu.Err(r));
        b.bytes = bytes;
        return true;
    }

    void Destroy(SharedBuf& b)
    {
        if (b.ptr)
            cu.MemFree(b.ptr);
        if (b.mem)
            cu.DestroyExternalMemory(b.mem);
        if (b.handle)
            CloseHandle(b.handle);
        if (b.res)
            b.res->Release();
        b = SharedBuf();
    }

    // GPU-side ordering: the D3D12 queue signals Fence() at value v; CUDA waits for it, and CUDA's
    // signal at a later value is what the queue waits for. No host wait in either direction.
    bool CudaWait(CUstream s, uint64_t v)
    {
        ExtSemWaitParams p {};
        p.params.fence.value = v;
        CUresult r = cu.WaitExternalSemaphoresAsync(&sem, &p, 1, s);
        return r == 0 || Fail(std::string("cuWaitExternalSemaphoresAsync: ") + cu.Err(r));
    }
    bool CudaSignal(CUstream s, uint64_t v)
    {
        ExtSemSignalParams p {};
        p.params.fence.value = v;
        CUresult r = cu.SignalExternalSemaphoresAsync(&sem, &p, 1, s);
        return r == 0 || Fail(std::string("cuSignalExternalSemaphoresAsync: ") + cu.Err(r));
    }

    void Release()
    {
        if (sem)
            cu.DestroyExternalSemaphore(sem);
        sem = nullptr;
        if (fence)
            fence->Release();
        fence = nullptr;
        if (ctx)
            cu.CtxDestroy(ctx);
        ctx = nullptr;
    }

private:
    bool Fail(const std::string& e)
    {
        error = e;
        return false;
    }
};
}  // namespace xefgio
