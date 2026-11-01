// xefg_pack_d3d.h - runs a graph pack (xefg_pack.h) inside a D3D12 command list: every dispatch is the
// pack's own kernel, compiled once to a cubin and launched through NVAPI's cubin shaders, so a frame needs
// no CUDA context, no second queue and no fence - it is recorded on the caller's list like any compute pass.
//
// Same resources and kernel arguments as the CUDA runner, with D3D12 in place of CUDA:
//  - every resource is a D3D12 buffer in the same layout (textures pitched, as a copy footprint); kernels get
//    its GPU virtual address, which is what a cubin launched in a D3D12 list addresses.
//  - an integer texel fetch reads the buffer itself (TexelLoads()).
//  - a filtered texture read samples a Texture2D shadow of its buffer, refreshed with CopyTextureRegion
//    before a dispatch samples it after a write, and handed to the kernel as an NVAPI texture handle; a
//    sampling kernel is compiled in independent texture mode and gets two sampler handles (see
//    Independent()). All handles index the runner's shader-visible heaps, which Record() binds.
//  - a clear is our own fill kernel.
// CUDA is only used while building, to compile the PTX for this GPU (cuLink); it needs a current context then.
#pragma once

#include "intel_modules.h"
#include "xefg_pack.h"

#include <d3d12.h>

#include <functional>

#include <set>

namespace xefgpack
{

// NVAPI's cubin shader entry points, by the ids of NVIDIA's public nvapi_interface.h.
struct NvApi
{
    HMODULE lib = nullptr;
    int (*CreateCubin)(ID3D12Device*, const void*, unsigned, unsigned, unsigned, unsigned, unsigned, const char*, void**) = nullptr;
    int (*LaunchCubin)(ID3D12GraphicsCommandList*, void*, unsigned, unsigned, unsigned, const void*, unsigned) = nullptr;
    int (*DestroyCubin)(ID3D12Device*, void*) = nullptr;
    struct DescriptorObject      // NVAPI_D3D12_GET_CUDA_INDEPENDENT_DESCRIPTOR_OBJECT_PARAMS
    {
        size_t structSizeIn, structSizeOut;
        ID3D12Device* device;
        int type;                // 1 texture, 2 sampler
        D3D12_CPU_DESCRIPTOR_HANDLE desc;
        uint64_t handle;
    };
    int (*GetDescriptorObject)(DescriptorObject*) = nullptr;

    // The handle a cubin uses for a descriptor of a shader-visible heap (bound on the list when it runs).
    bool Handle(ID3D12Device* dev, int type, D3D12_CPU_DESCRIPTOR_HANDLE d, uint64_t& h)
    {
        DescriptorObject o { sizeof(DescriptorObject), 0, dev, type, d, 0 };
        if (GetDescriptorObject(&o) != 0)
            return false;
        h = o.handle;
        return true;
    }

    bool Load()
    {
        if (CreateCubin)
            return true;
        lib = LoadLibraryW(L"nvapi64.dll");
        if (!lib)
            return false;
        auto qi = reinterpret_cast<void* (*)(unsigned)>(GetProcAddress(lib, "nvapi_QueryInterface"));
        if (!qi)
            return false;
        auto init = reinterpret_cast<int (*)()>(qi(0x0150e828));                   // NvAPI_Initialize
        if (!init || init() != 0)
            return false;
        *reinterpret_cast<void**>(&CreateCubin) = qi(0x3151211b);                  // NvAPI_D3D12_CreateCubinComputeShaderEx
        *reinterpret_cast<void**>(&LaunchCubin) = qi(0x5c52bb86);                  // NvAPI_D3D12_LaunchCubinShader
        *reinterpret_cast<void**>(&DestroyCubin) = qi(0x7fb785ba);                 // NvAPI_D3D12_DestroyCubinComputeShader
        *reinterpret_cast<void**>(&GetDescriptorObject) = qi(0x0ddac234);          // NvAPI_D3D12_GetCudaIndependentDescriptorObject
        return CreateCubin && LaunchCubin && DestroyCubin && GetDescriptorObject;
    }
};

class D3DPack
{
public:
    std::string error;
    // Compiled kernels (optional, owned by the caller), keyed by the CONTENT of the PTX that was compiled (CubinKey), not
    // by file name: two packs (SDR / HDR) may name different kernels alike. The PTX does not depend on the pack's size, so
    // a rebuild for another size needs no CUDA when every kernel is in here.
    std::map<std::string, std::vector<uint8_t>>* cubins = nullptr;
    // Called before the first kernel that is not in `cubins` is compiled: the caller makes a CUDA context current (it
    // may have released its own after the first build). False: no CUDA, the build fails.
    std::function<bool()> needCuda;

    // SHA-256 of a PTX text, as hex (windows CNG, intel_modules.cpp). Two different kernels must never share a key: the
    // cache hands back the other one's compiled code.
    static std::string CubinKey(const std::vector<uint8_t>& ptx)
    {
        intel_modules::Sha256 h {};
        if (!intel_modules::HashBytes(ptx.data(), ptx.size(), h))
            return {};      // no hash: the caller compiles without the cache
        return intel_modules::ToHex(h);
    }

    D3DPack(Pack& p, Cuda& c, NvApi& n) : pack(p), cu(c), nv(n) {}
    ~D3DPack() { Release(); }
    D3DPack(const D3DPack&) = delete;
    D3DPack& operator=(const D3DPack&) = delete;

    // Compiles the kernels, creates the buffers (the init files uploaded when uploadInit, or for resources the
    // caller does not fill) and the texture shadows, and packs every dispatch's arguments. Waits on `queue`.
    bool Build(ID3D12Device* device, ID3D12CommandQueue* queue, bool uploadInit = false)
    {
        dev = device;
        q = queue;
        dev->AddRef();
        q->AddRef();
        if (FAILED(dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&alloc))) ||
            FAILED(dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc, nullptr, IID_PPV_ARGS(&list))) ||
            FAILED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence))))
            return Fail("command list / fence");
        list->Close();
        event = CreateEventW(nullptr, FALSE, FALSE, nullptr);

        // the pack's native modules, unless the caller named its own or turned them off
        for (const auto& [ptx, m] : pack.natives)
            if (packNatives && !native.count(ptx))
            {
                Native& n = native[ptx];
                n.dxbc = Slurp(pack.dir + "\\" + m.file);
                n.padded = m.padded;
                if (n.dxbc.empty())
                    return Fail("cannot read " + m.file);
            }
        if (!Compile())
            return false;
        // A kernel with no PTX has no parameter declarations either: its constant buffers (the blobs SetConstants and the
        // patches write, RecordNative reads) are the root constants of its native module.
        for (size_t ki = 0; ki < pack.kernels.size(); ++ki)
            if (Kernel& k = pack.kernels[ki]; k.nativeOnly && ki < natives.size())
            {
                k.params.clear();
                for (const NativeKernel::Param& prm : natives[ki].params)
                    if (prm.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
                        k.params.push_back({ true, prm.reg, prm.count * 4 });
            }
        if (!CreateBuffers(uploadInit))
            return false;
        if (!CreateShadows())
            return false;
        if (!pack.PrepareArgs([this](const Bind& b, uint8_t* s) { return Descriptor(b, s); }))
            return Fail(pack.error);
        // Argument layout: p_desc (u64) at 0, then each blob 16-byte aligned, as the PTX declares them.
        // A sampling kernel then takes the two sampler handles (Independent()).
        for (size_t ki = 0; ki < pack.kernels.size(); ++ki)
        {
            std::vector<unsigned> off;
            unsigned o = 8;
            for (const Kernel::Param& p : pack.kernels[ki].params)
            {
                o = (o + 15) & ~15u;
                off.push_back(o);
                o += p.size;
            }
            o = (o + 7) & ~7u;
            argOffsets.push_back(off);
            samplerOffset.push_back(o);
            argBytes.push_back(indep[ki] ? o + 16 : o);
        }
        return true;
    }

    // Records dispatches [from, to) with the pack's current constants (Pack::SetPass etc.) on `cl`. The
    // caller's inputs must already be in the role buffers; the list must not be executed before the buffers
    // it reads are ready, as any D3D12 work.
    bool Record(ID3D12GraphicsCommandList* cl, size_t from, size_t to, ID3D12QueryHeap* stamps = nullptr, UINT stampBase = 0)
    {
        // stamps: a timestamp after each dispatch, at index stampBase + i + 1 (stampBase + `from` before the first;
        // profiling)
        // A record from dispatch 0 starts an app frame: the caller may have changed the inputs. A tail-only
        // record (the next generated frame) keeps the copies; writes by the dispatches mark them stale.
        if (from == 0)
            for (Shadow& s : shadows)
                s.fresh = s.own;
        if (srvHeap)
        {
            // NVAPI's texture handles index the bound shader-visible heaps.
            ID3D12DescriptorHeap* heaps[] = { srvHeap, smpHeap };
            cl->SetDescriptorHeaps(2, heaps);
        }
        std::vector<uint8_t> args;
        if (stamps)
            cl->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, stampBase + static_cast<UINT>(from));
        // A UAV barrier only where a dispatch depends on one since the last barrier: it reads or writes what
        // one wrote, or writes what one read. Profiling keeps one after every dispatch, to time each alone.
        std::set<int> wrote, read;
        auto need = [&](int r, bool writes) {
            if (wrote.count(r) || (writes && read.count(r)))
            {
                UavBarrier(cl);
                wrote.clear();
                read.clear();
            }
        };
        for (size_t i = from; i < to && i < pack.disp.size(); ++i)
        {
            const Dispatch& d = pack.disp[i];
            for (const Clear& c : d.clears)
            {
                need(c.res, true);
                Fill(cl, c.res, c.value, pack.ClearBytes(c));
                wrote.insert(c.res);
                for (Shadow& s : shadows)
                    if (s.res == c.res)
                        s.fresh = false;
            }
            for (const Bind& b : d.binds)
                need(b.res, b.slot >= 32);
            lastCopies = 0;
            RefreshShadows(cl, d);
            if (copiesAt.size() < pack.disp.size())
                copiesAt.resize(pack.disp.size());
            copiesAt[i] = lastCopies;
            const Kernel& k = pack.kernels[d.kernel];
            if (natives[d.kernel].pso)
                RecordNative(cl, i, d);
            else
            {
                args.assign(argBytes[d.kernel], 0);
                const D3D12_GPU_VIRTUAL_ADDRESS table = descTable->GetGPUVirtualAddress();
                memcpy(args.data(), &table, 8);
                for (size_t p = 0; p < d.blobs.size(); ++p)
                    memcpy(args.data() + argOffsets[d.kernel][p], d.blobs[p].data(), d.blobs[p].size());
                if (redirTo)
                    for (size_t p = 0; p < k.params.size() && p < d.blobs.size(); ++p)
                        for (const Bind& b : d.binds)
                            if (!k.params[p].cb && b.slot == static_cast<int>(k.params[p].idx) && b.res == redirRes)
                            {
                                const uint64_t va = redirTo->GetGPUVirtualAddress();      // the slot's pointer, at +0
                                memcpy(args.data() + argOffsets[d.kernel][p], &va, 8);
                            }
                if (indep[d.kernel])
                    memcpy(args.data() + samplerOffset[d.kernel], samplers, 16);
                if (int r = nv.LaunchCubin(cl, kernelShaders[d.kernel], d.g[0], d.g[1], d.g[2], args.data(), static_cast<unsigned>(args.size())))
                    return Fail(k.file + ": NvAPI_D3D12_LaunchCubinShader " + std::to_string(r));
            }
            for (const Bind& b : d.binds)
                (b.slot >= 32 ? wrote : read).insert(b.res);
            if (stamps)
            {
                UavBarrier(cl);
                wrote.clear();
                read.clear();
                cl->EndQuery(stamps, D3D12_QUERY_TYPE_TIMESTAMP, stampBase + static_cast<UINT>(i + 1));
            }
            for (const Bind& b : d.binds)
                if (b.slot >= 32)
                    for (Shadow& s : shadows)
                        if (s.res == b.res && !s.own)
                            s.fresh = false;
        }
        if (!wrote.empty())
            UavBarrier(cl);         // the caller reads the results
        return true;
    }

    ID3D12Resource* Buffer(int r) const { return r >= 0 && r < static_cast<int>(buffers.size()) ? buffers[r] : nullptr; }

    // Until Redirect(r, nullptr): the kernels that bind resource r (one only cubins use) address `to` instead, a buffer
    // of its size and layout the caller keeps in UNORDERED_ACCESS while recording - e.g. the output straight into the
    // ring buffer a generated frame is presented from, with no copy.
    bool Redirect(int r, ID3D12Resource* to)
    {
        if (to && (r < 0 || r >= static_cast<int>(buffers.size()) || ownTex.count(r) || paddedRes.count(r) ||
                   to->GetDesc().Width < pack.res[r].bytes))
            return false;
        // only cubin slot pointers follow it: a native dispatch's descriptor or a shadow copy would still use buffers[r]
        for (const Dispatch& d : pack.disp)
            for (const Bind& b : d.binds)
                if (to && b.res == r && (natives[d.kernel].pso || (b.slot < 32 && texSlots[d.kernel].count(b.slot))))
                    return false;
        redirRes = to ? r : -1;
        redirTo = to;
        return true;
    }

    // R <-> B in every texel of resource r (a pack buffer, in UNORDERED_ACCESS), then a UAV barrier.
    void SwapRB(ID3D12GraphicsCommandList* cl, int r)
    {
        struct { uint64_t p; uint32_t n; } a { Address(r), static_cast<uint32_t>(pack.res[r].bytes / 4) };
        nv.LaunchCubin(cl, swapShader, (a.n + 255) / 256, 1, 1, &a, sizeof(a));
        UavBarrier(cl);
    }

    // Fills a resource with tightly packed contents / reads one back (offline checks; both wait).
    bool Upload(int r, const std::vector<uint8_t>& tight)
    {
        const Resource& x = pack.res[r];
        if (tight.size() != x.TightBytes())
            return Fail(x.id + ": wrong upload size");
        if (ownTex.count(r) || paddedRes.count(r))
            return Fail(x.id + ": a native kernel writes it, it takes no upload");
        std::vector<uint8_t> staged(x.bytes, 0);
        Pitch(x, tight.data(), staged.data());
        ID3D12Resource* up = HostBuffer(D3D12_HEAP_TYPE_UPLOAD, x.bytes);
        if (!up)
            return Fail("upload buffer");
        void* m = nullptr;
        up->Map(0, nullptr, &m);
        memcpy(m, staged.data(), x.bytes);
        up->Unmap(0, nullptr);
        const bool ok = Submit([&](ID3D12GraphicsCommandList* cl) {
            Transition(cl, buffers[r], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_DEST);
            cl->CopyBufferRegion(buffers[r], 0, up, 0, x.bytes);
            Transition(cl, buffers[r], D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        });
        up->Release();
        for (Shadow& s : shadows)
            if (s.res == r)
                s.fresh = false;
        return ok;
    }
    bool Download(int r, std::vector<uint8_t>& out)
    {
        const Resource& x = pack.res[r];
        const size_t bytes = std::max(BufferBytes(r), x.bytes);
        ID3D12Resource* rb = HostBuffer(D3D12_HEAP_TYPE_READBACK, bytes);
        if (!rb)
            return Fail("readback buffer");
        ID3D12Resource* own = nullptr;
        for (const Shadow& s : shadows)
            if (s.own && s.res == r)
                own = s.tex;
        const bool ok = Submit([&](ID3D12GraphicsCommandList* cl) {
            if (own)
            {
                // a texture a native kernel writes: its texels as the pitched footprint
                Transition(cl, own, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_SOURCE);
                D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
                dst.pResource = rb;
                dst.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
                dst.PlacedFootprint.Footprint = { static_cast<DXGI_FORMAT>(ownTex.at(r)), x.w, x.h, 1, static_cast<UINT>(x.pitch) };
                src.pResource = own;
                src.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
                cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
                Transition(cl, own, D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
                return;
            }
            Transition(cl, buffers[r], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE);
            cl->CopyBufferRegion(rb, 0, buffers[r], 0, BufferBytes(r));
            Transition(cl, buffers[r], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
        });
        if (ok)
        {
            void* m = nullptr;
            const D3D12_RANGE all { 0, bytes };
            rb->Map(0, &all, &m);
            const uint8_t* src = static_cast<const uint8_t*>(m);
            if (!x.tex)
                out.assign(src, src + x.bytes);
            else
            {
                // pitched rows, or the interior of a bordered buffer
                const size_t tight = static_cast<size_t>(x.w) * x.texel;
                const bool pad = paddedRes.count(r) != 0;
                const size_t pitch = pad ? static_cast<size_t>(x.w + 2) * x.texel : x.pitch;
                if (pad)
                    src += pitch + x.texel;
                out.resize(tight * x.Rows());
                for (size_t y = 0; y < x.Rows(); ++y)
                    memcpy(out.data() + y * tight, src + y * pitch, tight);
            }
            const D3D12_RANGE none { 0, 0 };
            rb->Unmap(0, &none);
        }
        rb->Release();
        return ok;
    }

    // Records `rec` on the runner's own list, executes it on the queue and waits.
    template <class Fn>
    bool Submit(const Fn& rec)
    {
        alloc->Reset();
        list->Reset(alloc, nullptr);
        rec(list);
        if (FAILED(list->Close()))
            return Fail("command list Close failed (an invalid command?)");
        ID3D12CommandList* l = list;
        q->ExecuteCommandLists(1, &l);
        q->Signal(fence, ++fenceValue);
        if (fence->GetCompletedValue() < fenceValue)
        {
            fence->SetEventOnCompletion(fenceValue, event);
            if (WaitForSingleObject(event, 20000) != WAIT_OBJECT_0)
                return Fail("GPU did not finish within 20 s");
        }
        if (FAILED(dev->GetDeviceRemovedReason()))
            return Fail("device removed");
        return true;
    }

    void Release()
    {
        if (dev)
        {
            for (void* h : kernelShaders)
                if (h)
                    nv.DestroyCubin(dev, h);
            if (fillShader)
                nv.DestroyCubin(dev, fillShader);
            if (swapShader)
                nv.DestroyCubin(dev, swapShader);
        }
        kernelShaders.clear();
        fillShader = swapShader = nullptr;
        for (NativeKernel& nk : natives)
            for (IUnknown* u : std::initializer_list<IUnknown*> { nk.pso, nk.rs })
                if (u)
                    u->Release();
        natives.clear();
        if (nativeCb)
            nativeCb->Release();
        nativeCb = nullptr;
        ownTex.clear();
        paddedRes.clear();
        nativeTables.clear();
        shadowDescs = 0;
        for (Shadow& s : shadows)
            if (s.tex)
                s.tex->Release();
        shadows.clear();
        for (ID3D12Resource* b : buffers)
            if (b)
                b->Release();
        buffers.clear();
        if (bufHeap)
            bufHeap->Release();
        bufHeap = nullptr;
        for (IUnknown* u : std::initializer_list<IUnknown*> { descTable, srvHeap, smpHeap, list, alloc, fence, q, dev })
            if (u)
                u->Release();
        descTable = nullptr;
        srvHeap = smpHeap = nullptr;
        list = nullptr;
        alloc = nullptr;
        fence = nullptr;
        q = nullptr;
        dev = nullptr;
        if (event)
            CloseHandle(event);
        event = nullptr;
    }

    // Profiling (pack_d3d --prof): the texture copies before each dispatch in the last Record, and a switch that
    // skips every copy after a shadow's first (the output is then wrong; the time saved is the copies' cost).
    std::vector<int> copiesAt;
    bool skipShadowCopies = false;
    // Kernels (by file, e.g. k16_01381CB0.ptx) whose texel fetches keep the texture (a shadow copy after each write)
    // instead of reading the buffer. Set before Build.
    std::set<std::string> fetchTextures;
    // Experiment: every buffer placed in one heap (64 KB apart) instead of a committed resource each. Set before Build.
    bool oneHeap = false;
    // Experiment: kernels (by file) whose integer texel fetches read the buffer for every resource, inputs too. Set before Build.
    std::set<std::string> fetchLoads;
    // pack_d3d --regs: print each kernel's registers, shared memory and spills as the JIT reports them. Set before Build.
    bool printRegs = false;
    // Kernels (by PTX file) run as their native DXIL module instead (Intel's own container: root signature and
    // shader), compiled by the driver - the reconstruction takes 0.25 ms there vs 0.36 ms as translated PTX at
    // 2560x1440. A texture it writes lives as a texture (UAV for it, SRV for the kernels that read it: no buffer,
    // no copy). `padded`: UAV slots it writes as a raw buffer of the texture with a one-texel border (Intel's layout
    // for its CM U-Net, (w + 2) x (h + 2)): the other kernels read the interior through their slot's pointer and
    // pitch, so they need no change. A variant kernel (another PTX file) runs as PTX. Set before Build.
    struct Native
    {
        std::vector<uint8_t> dxbc;
        std::set<int> padded;
    };
    std::map<std::string, Native> native;
    // The pack's own 'native' lines (Pack::natives) go into `native` at Build; false runs those kernels as PTX.
    bool packNatives = true;

private:
    int lastCopies = 0;
    ID3D12Heap* bufHeap = nullptr;
    UINT64 heapUsed = 0;
    struct Shadow
    {
        int res = -1;
        uint32_t fmt = 0;
        ID3D12Resource* tex = nullptr;
        uint64_t handle = 0;
        bool fresh = false;
        bool readable = false;          // in NON_PIXEL_SHADER_RESOURCE (else COPY_DEST)
        bool own = false;               // the resource itself (a native kernel writes it): never copied, in
                                        // NON_PIXEL_SHADER_RESOURCE except while that kernel runs
    };
    // A native kernel: its root signature, pipeline and root parameters (descriptor table ranges, or the
    // register and count of 32-bit constants).
    struct NativeKernel
    {
        ID3D12RootSignature* rs = nullptr;
        ID3D12PipelineState* pso = nullptr;
        struct Param
        {
            D3D12_ROOT_PARAMETER_TYPE type;
            UINT reg = 0, count = 0;
            std::vector<D3D12_DESCRIPTOR_RANGE1> ranges;
        };
        std::vector<Param> params;
        std::set<int> padded;
    };
    std::vector<NativeKernel> natives;          // per kernel; no pso for a cubin
    std::map<int, uint32_t> ownTex;             // resource -> format: textures a native kernel writes
    std::set<int> paddedRes;                    // raw-with-border resources a native kernel writes
    std::vector<std::vector<UINT>> nativeTables;    // per dispatch: first descriptor of each root table (srvHeap)
    ID3D12Resource* nativeCb = nullptr;         // the native dispatches' constant buffers (upload heap)
    UINT shadowDescs = 0;
    int redirRes = -1;                          // Redirect()
    ID3D12Resource* redirTo = nullptr;
    D3D12_GPU_VIRTUAL_ADDRESS Address(int r) const
    {
        return (r == redirRes && redirTo ? redirTo : buffers[r])->GetGPUVirtualAddress();
    }

    Pack& pack;
    Cuda& cu;
    NvApi& nv;
    ID3D12Device* dev = nullptr;
    ID3D12CommandQueue* q = nullptr;
    ID3D12CommandAllocator* alloc = nullptr;
    ID3D12GraphicsCommandList* list = nullptr;
    ID3D12Fence* fence = nullptr;
    UINT64 fenceValue = 0;
    HANDLE event = nullptr;
    std::vector<ID3D12Resource*> buffers;
    ID3D12Resource* descTable = nullptr;
    std::vector<void*> kernelShaders;
    void* fillShader = nullptr;
    void* swapShader = nullptr;         // swaprb: R <-> B of every 32-bit texel (B8G8R8A8 back buffers)
    std::vector<std::vector<unsigned>> argOffsets;
    std::vector<unsigned> argBytes, samplerOffset;
    std::vector<bool> indep;            // kernel samples: compiled in independent mode, takes the samplers
    std::vector<std::set<int>> texSlots;    // per kernel: the slots it still reads through a texture
    uint64_t samplers[2] {};            // linear clamp, point border 0
    std::vector<Shadow> shadows;
    ID3D12DescriptorHeap* srvHeap = nullptr;
    ID3D12DescriptorHeap* smpHeap = nullptr;

    bool Fail(const std::string& e)
    {
        error = e;
        return false;
    }

    // PTX -> cubin for the current context's GPU (or from the cache).
    bool Cubin(const std::vector<uint8_t>& ptx, const std::string& name, std::vector<uint8_t>& out)
    {
        // `name` is for messages only: the cache is keyed by what is compiled
        const std::string key = CubinKey(ptx);
        const bool cached = cubins && !key.empty();       // no key (the hash failed): never shared
        if (cached)
            if (auto it = cubins->find(key); it != cubins->end())
            {
                out = it->second;
                return true;
            }
        if (needCuda && !needCuda())
            return Fail(name + ": no CUDA to compile it");
        if (!CompileCubin(ptx, name, out))
            return false;
        if (cached)
            (*cubins)[key] = out;
        return true;
    }
    bool CompileCubin(const std::vector<uint8_t>& ptx, const std::string& name, std::vector<uint8_t>& out)
    {
        static char log[16384], info[16384];
        log[0] = info[0] = 0;
        int opts[] = { 5, 6, 3, 4, 12 };    // CU_JIT_ERROR_LOG_BUFFER, _SIZE_BYTES, _INFO_LOG_BUFFER, _SIZE_BYTES, _LOG_VERBOSE
        void* vals[] = { log, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(log))), info,
                         reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(info))), reinterpret_cast<void*>(1) };
        void* st = nullptr;
        CUresult r;
        if ((r = cu.LinkCreate(printRegs ? 5 : 2, opts, vals, &st)))
            return Fail(name + ": cuLinkCreate " + cu.Err(r));
        void* bin = nullptr;
        size_t size = 0;
        r = cu.LinkAddData(st, 1 /* CU_JIT_INPUT_PTX */, const_cast<uint8_t*>(ptx.data()), ptx.size(), name.c_str(), 0, nullptr, nullptr);
        if (!r)
            r = cu.LinkComplete(st, &bin, &size);
        if (!r)
            out.assign(static_cast<uint8_t*>(bin), static_cast<uint8_t*>(bin) + size);
        if (printRegs)
            if (const char* u = strstr(info, "used "))
                printf("  %-28s %.*s\n", name.c_str(), static_cast<int>(strcspn(u, "\r\n")), u);
        cu.LinkDestroy(st);
        return r == 0 || Fail(name + ": cuLink " + cu.Err(r) + " " + log);
    }

    // A native kernel's root signature (its container's own) and pipeline. Root parameters: descriptor tables of
    // SRVs, UAVs and CBVs, and 32-bit constants; samplers static.
    bool CreateNative(const std::string& file, const Native& n, NativeKernel& nk)
    {
        ID3D12VersionedRootSignatureDeserializer* de = nullptr;
        const D3D12_VERSIONED_ROOT_SIGNATURE_DESC* vd = nullptr;
        if (FAILED(D3D12CreateVersionedRootSignatureDeserializer(n.dxbc.data(), n.dxbc.size(), IID_PPV_ARGS(&de))) ||
            FAILED(de->GetRootSignatureDescAtVersion(D3D_ROOT_SIGNATURE_VERSION_1_1, &vd)))
        {
            if (de)
                de->Release();
            return Fail(file + ": no root signature in its native module");
        }
        const D3D12_ROOT_SIGNATURE_DESC1& rd = vd->Desc_1_1;
        bool ok = true;
        for (UINT i = 0; i < rd.NumParameters && ok; ++i)
        {
            const D3D12_ROOT_PARAMETER1& p = rd.pParameters[i];
            NativeKernel::Param prm { p.ParameterType };
            if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
                prm.ranges.assign(p.DescriptorTable.pDescriptorRanges, p.DescriptorTable.pDescriptorRanges + p.DescriptorTable.NumDescriptorRanges);
            else if (p.ParameterType == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS)
                prm.reg = p.Constants.ShaderRegister, prm.count = p.Constants.Num32BitValues, ok &= p.Constants.RegisterSpace == 0;
            else
                ok = false;
            // NativeTables places each range's descriptors right after the previous range's
            UINT at = 0;
            for (const D3D12_DESCRIPTOR_RANGE1& r : prm.ranges)
            {
                ok &= r.RangeType != D3D12_DESCRIPTOR_RANGE_TYPE_SAMPLER && r.NumDescriptors != UINT_MAX && r.RegisterSpace == 0 &&
                      (r.OffsetInDescriptorsFromTableStart == D3D12_DESCRIPTOR_RANGE_OFFSET_APPEND || r.OffsetInDescriptorsFromTableStart == at);
                at += r.NumDescriptors;
            }
            nk.params.push_back(std::move(prm));
        }
        de->Release();
        if (!ok)
            return Fail(file + ": its native root signature has root descriptors, sampler tables, unbounded or out-of-order ranges, "
                               "or a register space other than 0");
        D3D12_COMPUTE_PIPELINE_STATE_DESC pd {};
        if (FAILED(dev->CreateRootSignature(0, n.dxbc.data(), n.dxbc.size(), IID_PPV_ARGS(&nk.rs))))
            return Fail(file + ": CreateRootSignature (native)");
        pd.pRootSignature = nk.rs;
        pd.CS = { n.dxbc.data(), n.dxbc.size() };
        if (FAILED(dev->CreateComputePipelineState(&pd, IID_PPV_ARGS(&nk.pso))))
            return Fail(file + ": CreateComputePipelineState (native)");
        nk.padded = n.padded;
        return true;
    }

    // The pack's PTX samples in unified mode (one handle for texture and sampler). In a cubin launched in a
    // D3D12 list the sampler half of such a handle is ignored: coordinates are read unnormalized, point
    // sampled. Independent mode with explicit sampler handles samples as D3D12 and CUDA do (tex_test.cpp).
    // Each tex gets the sampler of the slot its handle came from: +8 the linear one, +40 the point one.
    // A D3D12 sampler always takes normalized coordinates, so an integer texel fetch (.s32) becomes a point
    // sample at the texel's centre, ((x + 0.5) / width, (y + 0.5) / height), with the slot's own size: the
    // same texel, and the zero border outside the texture.
    static bool Independent(std::string& ptx)
    {
        const char* target = ".target sm_75";
        const size_t t = ptx.find(target);
        const size_t e = ptx.find("\n) .reqntid");
        if (t == std::string::npos || e == std::string::npos)
            return false;
        // which handle registers hold the point (+40) texture, and of which slot
        std::map<std::string, std::string> point;
        for (size_t p = ptx.find("ld.param.u64 %q"); p != std::string::npos; p = ptx.find("ld.param.u64 %q", p + 1))
        {
            const size_t comma = ptx.find(',', p), open = ptx.find('[', p), close = ptx.find(']', p);
            if (comma == std::string::npos || open == std::string::npos || close == std::string::npos)
                return false;
            if (ptx.compare(close - 3, 3, "+40") == 0)
                point[ptx.substr(p + 13, comma - p - 13)] = ptx.substr(open + 1, close - 3 - open - 1);
        }
        std::string out;
        out.reserve(ptx.size() + 4096);
        out.append(ptx, 0, t + strlen(target));
        out += ", texmode_independent";
        out.append(ptx, t + strlen(target), e - t - strlen(target));
        out += ",\n    .param .u64 p_smp_lin,\n    .param .u64 p_smp_pt";
        // the body: after its declarations, load the sampler handles
        size_t body = ptx.find("{\n", e);
        if (body == std::string::npos)
            return false;
        body += 2;
        size_t firstInstr = body;
        while (firstInstr < ptx.size())
        {
            size_t s = firstInstr;
            while (s < ptx.size() && (ptx[s] == ' ' || ptx[s] == '\t'))
                ++s;
            if (ptx.compare(s, 5, ".reg ") && ptx.compare(s, 8, ".shared ") && ptx.compare(s, 7, ".local "))
                break;
            firstInstr = ptx.find('\n', s) + 1;
        }
        out.append(ptx, e, firstInstr - e);
        out += "    .reg .b64 %smpL, %smpP;\n    ld.param.u64 %smpL, [p_smp_lin];\n    ld.param.u64 %smpP, [p_smp_pt];\n";
        size_t from = firstInstr;
        for (size_t p = ptx.find("tex.", from); p != std::string::npos; p = ptx.find("tex.", p + 1))
        {
            const size_t lineStart = ptx.rfind('\n', p) + 1, lineEnd = ptx.find('\n', p);
            const size_t open = ptx.find('[', p), comma = ptx.find(',', open);
            if (open == std::string::npos || comma == std::string::npos || lineEnd == std::string::npos)
                return false;
            const std::string reg = ptx.substr(open + 1, comma - open - 1);
            const auto pt = point.find(reg);
            const bool fetch = ptx.compare(p, 18, "tex.2d.v4.f32.s32 ") == 0;
            if (!fetch)
            {
                out.append(ptx, from, comma - from);
                out += pt != point.end() ? ", %smpP" : ", %smpL";
                from = comma;
                continue;
            }
            // tex.2d.v4.f32.s32 {d}, [%q, {%x, %y}];  ->  a point sample at the texel centre
            const size_t cOpen = ptx.find('{', comma), cComma = ptx.find(',', cOpen), cClose = ptx.find('}', cOpen);
            if (pt == point.end() || cClose > lineEnd)
                return false;
            const std::string pred = ptx.substr(lineStart, p - lineStart);       // indentation and any @%p
            const std::string x = ptx.substr(cOpen + 1, cComma - cOpen - 1), y = ptx.substr(cComma + 2, cClose - cComma - 2);
            const std::string dst = ptx.substr(p + 18, open - p - 18);           // "{d0, d1, d2, d3}, "
            const std::string& slot = pt->second;
            out.append(ptx, from, lineStart - from);
            out += "    { .reg .f32 %tu, %tv, %tw, %th; .reg .b32 %twi, %thi;\n";
            out += "    ld.param.u32 %twi, [" + slot + "+16]; ld.param.u32 %thi, [" + slot + "+20];\n";
            out += "    cvt.rn.f32.s32 %tu, " + x + "; cvt.rn.f32.s32 %tv, " + y + ";\n";
            out += "    cvt.rn.f32.u32 %tw, %twi; cvt.rn.f32.u32 %th, %thi;\n";
            out += "    add.f32 %tu, %tu, 0f3F000000; add.f32 %tv, %tv, 0f3F000000;\n";
            out += "    div.rn.f32 %tu, %tu, %tw; div.rn.f32 %tv, %tv, %th;\n";
            out += pred + "tex.2d.v4.f32.f32 " + dst + "[" + reg + ", %smpP, {%tu, %tv}];\n    }";
            from = lineEnd;
            p = lineEnd;
        }
        out.append(ptx, from, std::string::npos);
        ptx = std::move(out);
        return true;
    }

    // The handle registers of a kernel and the slot each came from: "%q12" -> {"p_s3", offset 8 or 40}.
    static std::map<std::string, std::pair<std::string, int>> Handles(const std::string& ptx)
    {
        std::map<std::string, std::pair<std::string, int>> h;
        for (size_t p = ptx.find("ld.param.u64 %q"); p != std::string::npos; p = ptx.find("ld.param.u64 %q", p + 1))
        {
            const size_t comma = ptx.find(',', p), open = ptx.find('[', p), close = ptx.find(']', p);
            if (comma == std::string::npos || open == std::string::npos || close == std::string::npos)
                break;
            for (int off : { 8, 40 })
            {
                const std::string tail = "+" + std::to_string(off);
                if (ptx.compare(close - tail.size(), tail.size(), tail) == 0)
                    h[ptx.substr(p + 13, comma - p - 13)] = { ptx.substr(open + 1, close - tail.size() - open - 1), off };
            }
        }
        return h;
    }

    // Slot number of a slot parameter name ("p_s3" -> 3), or -1.
    static int SlotOf(const std::string& param) { return param.compare(0, 3, "p_s") == 0 ? atoi(param.c_str() + 3) : -1; }

    // The slots a kernel reads through a texture instruction.
    static std::set<int> SampledSlots(const std::string& ptx)
    {
        std::set<int> s;
        const auto h = Handles(ptx);
        for (size_t p = ptx.find("tex."); p != std::string::npos; p = ptx.find("tex.", p + 1))
        {
            const size_t open = ptx.find('[', p), comma = ptx.find(',', open);
            if (open == std::string::npos || comma == std::string::npos)
                break;
            if (auto it = h.find(ptx.substr(open + 1, comma - open - 1)); it != h.end())
                s.insert(SlotOf(it->second.first));
        }
        return s;
    }

    // An integer texel fetch through the point handle (HLSL Texture2D.Load) needs no texture: it reads the
    // slot's pitched buffer (u64 pointer at +0, width +16, height +20, pitch +36) and converts the texel as
    // the texture unit would - UNORM8 c / 255, half -> float, missing channels (0, 0, 1) - with all four
    // channels 0 outside the texture, as the zero border gives. `fmt` is the view format of each slot the
    // kernel's dispatches bind (none: formats differ between dispatches, or one this has no conversion for).
    static void TexelLoads(std::string& ptx, const std::map<int, uint32_t>& fmt)
    {
        const auto h = Handles(ptx);
        const char* kFetch = "tex.2d.v4.f32.s32 ";
        std::string out;
        out.reserve(ptx.size() + 16384);
        size_t from = 0;
        for (size_t p = ptx.find(kFetch); p != std::string::npos; p = ptx.find(kFetch, p + 1))
        {
            const size_t lineStart = ptx.rfind('\n', p) + 1, lineEnd = ptx.find('\n', p);
            const size_t dOpen = p + strlen(kFetch), dClose = ptx.find('}', dOpen), open = ptx.find('[', dClose);
            const size_t comma = ptx.find(',', open), cOpen = ptx.find('{', comma), cComma = ptx.find(',', cOpen),
                         cClose = ptx.find('}', cOpen);
            if (lineEnd == std::string::npos || cClose == std::string::npos || cClose > lineEnd || ptx[dOpen] != '{')
                continue;
            const auto it = h.find(ptx.substr(open + 1, comma - open - 1));
            if (it == h.end() || it->second.second != 40)
                continue;
            const std::string& slot = it->second.first;
            const auto f = fmt.find(SlotOf(slot));
            if (f == fmt.end())
                continue;
            std::string body;
            const char* one = "0f3F800000";
            switch (f->second)
            {
            case 2:         // R32G32B32A32_FLOAT
                body = "@%xin ld.global.v4.b32 {%xc0, %xc1, %xc2, %xc3}, [%xa];\n";
                break;
            case 16:        // R32G32_FLOAT
                body = "@%xin ld.global.v2.b32 {%xc0, %xc1}, [%xa];\n    mov.b32 %xc3, " + std::string(one) + ";\n";
                break;
            case 41:        // R32_FLOAT
                body = "@%xin ld.global.b32 %xc0, [%xa];\n    mov.b32 %xc3, " + std::string(one) + ";\n";
                break;
            case 28:        // R8G8B8A8_UNORM
                body = "@%xin ld.global.b32 %xr, [%xa];\n";
                for (int c = 0; c < 4; ++c)
                    body += "    bfe.u32 %xs, %xr, " + std::to_string(8 * c) + ", 8; cvt.rn.f32.u32 %xf, %xs; div.rn.f32 %xf, %xf, 0f437F0000; mov.b32 %xc" +
                            std::to_string(c) + ", %xf;\n";
                break;
            case 24:        // R10G10B10A2_UNORM (the HDR10 frames and the multi-frame model's reconstruction images)
                body = "@%xin ld.global.b32 %xr, [%xa];\n";
                for (int c = 0; c < 4; ++c)
                    body += "    bfe.u32 %xs, %xr, " + std::to_string(10 * c) + ", " + (c < 3 ? "10" : "2") +
                            "; cvt.rn.f32.u32 %xf, %xs; div.rn.f32 %xf, %xf, " + (c < 3 ? "0f447FC000" : "0f40400000") + "; mov.b32 %xc" +
                            std::to_string(c) + ", %xf;\n";
                break;
            case 61:        // R8_UNORM
                body = "@%xin ld.global.u8 %xr, [%xa];\n    cvt.rn.f32.u32 %xf, %xr; div.rn.f32 %xf, %xf, 0f437F0000; mov.b32 %xc0, %xf;\n"
                       "    mov.b32 %xc3, " + std::string(one) + ";\n";
                break;
            case 34:        // R16G16_FLOAT
                body = "@%xin ld.global.b32 %xr, [%xa];\n    mov.b32 {%xh0, %xh1}, %xr;\n"
                       "    cvt.f32.f16 %xf, %xh0; mov.b32 %xc0, %xf; cvt.f32.f16 %xf, %xh1; mov.b32 %xc1, %xf;\n"
                       "    mov.b32 %xc3, " + std::string(one) + ";\n";
                break;
            default:
                continue;
            }
            std::vector<std::string> dst;
            for (size_t a = dOpen + 1; a < dClose;)
            {
                const size_t b = std::min(ptx.find(',', a), dClose);
                std::string r = ptx.substr(a, b - a);
                r.erase(0, r.find_first_not_of(' '));
                r.erase(r.find_last_not_of(' ') + 1);
                dst.push_back(r);
                a = b + 1;
            }
            if (dst.size() != 4)
                continue;
            std::string guard = ptx.substr(lineStart, p - lineStart);       // indentation and any @%p
            guard.erase(0, guard.find_first_not_of(" \t"));
            const std::string x = ptx.substr(cOpen + 1, cComma - cOpen - 1), y = ptx.substr(cComma + 2, cClose - cComma - 2);
            out.append(ptx, from, lineStart - from);
            out += "    { .reg .b64 %xa, %xo; .reg .b32 %xw, %xht, %xp, %xr, %xs, %xc0, %xc1, %xc2, %xc3; .reg .f32 %xf; .reg .b16 %xh0, %xh1; .reg .pred %xin;\n";
            out += "    ld.param.u32 %xw, [" + slot + "+16]; ld.param.u32 %xht, [" + slot + "+20]; ld.param.u32 %xp, [" + slot + "+36]; ld.param.u64 %xa, [" + slot + "+0];\n";
            out += "    setp.lt.u32 %xin, " + x + ", %xw; setp.lt.and.u32 %xin, " + y + ", %xht, %xin;\n";
            out += "    mul.wide.u32 %xo, " + y + ", %xp; add.u64 %xa, %xa, %xo; mul.wide.u32 %xo, " + x + ", " + std::to_string(TexelBytes(f->second)) +
                   "; add.u64 %xa, %xa, %xo;\n";
            out += "    mov.b32 %xr, 0; mov.b32 %xc0, 0; mov.b32 %xc1, 0; mov.b32 %xc2, 0; mov.b32 %xc3, 0;\n    " + body;
            for (int c = 0; c < 4; ++c)
                out += "    " + guard + "selp.b32 " + dst[c] + ", %xc" + std::to_string(c) + ", 0, %xin;\n";
            out += "    }";
            from = lineEnd;
            p = lineEnd;
        }
        out.append(ptx, from, std::string::npos);
        ptx = std::move(out);
    }

    bool Compile()
    {
        // The view format each kernel's slots are bound with, for the texel fetches to read from the buffer
        // (0 = keep the texture: formats differ between dispatches, not a plain texture, or never written by
        // a dispatch). Only a resource the graph writes needs its shadow copied per generated frame; inputs
        // are copied once per app frame, and the texture cache serves scattered fetches better than loads.
        std::set<int> written;
        for (const Dispatch& d : pack.disp)
            for (const Bind& b : d.binds)
                if (b.slot >= 32)
                    written.insert(b.res);
        // What the native kernels write: textures (kept as such) and bordered raw buffers. Nothing else may write
        // them, and a texture is read in the format it is written in.
        for (const Dispatch& d : pack.disp)
            if (auto it = native.find(pack.kernels[d.kernel].file); it != native.end())
                for (const Bind& b : d.binds)
                    if (b.slot >= 32 && pack.res[b.res].tex)
                    {
                        if (pack.res[b.res].slices != 1)
                            return Fail(pack.res[b.res].id + ": a native kernel writes a texture array");
                        if (it->second.padded.count(b.slot))
                            paddedRes.insert(b.res);
                        else
                            ownTex[b.res] = b.fmt;
                    }
        for (const Dispatch& d : pack.disp)
        {
            const bool nat = native.count(pack.kernels[d.kernel].file) != 0;
            // RecordNative puts a resource in one state for the dispatch: read (SRV) or written (UAV), not both
            for (const Bind& b : d.binds)
                if (nat && b.slot < 32 && std::any_of(d.binds.begin(), d.binds.end(), [&](const Bind& w) { return w.slot >= 32 && w.res == b.res; }))
                    return Fail(pack.res[b.res].id + ": a native kernel reads and writes it in one dispatch");
            for (const Clear& c : d.clears)
                if (ownTex.count(c.res) || paddedRes.count(c.res))
                    return Fail(pack.res[c.res].id + ": cleared, but a native kernel writes it");
            for (const Bind& b : d.binds)
                if ((ownTex.count(b.res) || paddedRes.count(b.res)) && b.slot >= 32 && !nat)
                    return Fail(pack.res[b.res].id + ": a kernel writes what a native kernel writes");
                else if (auto o = ownTex.find(b.res); o != ownTex.end() && o->second != b.fmt)
                    return Fail(pack.res[b.res].id + ": read in another format than a native kernel writes it");
        }
        std::vector<std::map<int, uint32_t>> fmts(pack.kernels.size());
        for (const Dispatch& d : pack.disp)
            for (const Bind& b : d.binds)
            {
                const Resource& x = pack.res[b.res];
                if (b.slot >= 32)
                    continue;
                const std::string& kf = pack.kernels[d.kernel].file;
                const uint32_t f = x.tex && x.slices == 1 && TexelBytes(b.fmt) == x.texel && (written.count(b.res) || fetchLoads.count(kf)) &&
                                           !fetchTextures.count(kf) && !ownTex.count(b.res) ? b.fmt : 0;
                auto [it, fresh] = fmts[d.kernel].emplace(b.slot, f);
                if (!fresh && it->second != f)
                    it->second = 0;
            }
        std::vector<uint8_t> bin;
        natives.assign(pack.kernels.size(), {});
        for (size_t ki = 0; ki < pack.kernels.size(); ++ki)
        {
            const Kernel& k = pack.kernels[ki];
            if (auto it = native.find(k.file); it != native.end())
            {
                if (!CreateNative(k.file, it->second, natives[ki]))
                    return false;
                // every texture it reads comes from a texture: a shadow, or one a native kernel writes
                std::set<int> slots;
                for (const Dispatch& d : pack.disp)
                    if (d.kernel == static_cast<int>(ki))
                        for (const Bind& b : d.binds)
                            if (b.slot < 32 && pack.res[b.res].tex)
                                slots.insert(b.slot);
                texSlots.push_back(slots);
                indep.push_back(false);
                kernelShaders.push_back(nullptr);
                continue;
            }
            if (k.nativeOnly)
                return Fail(k.file + ": the pack has only its native module, which is switched off (Native=0)");
            std::string s(reinterpret_cast<const char*>(k.ptx.data()));
            if (s.find("tex.") != std::string::npos)
                TexelLoads(s, fmts[ki]);
            texSlots.push_back(SampledSlots(s));
            // a texture a native kernel writes has no buffer behind it (BufferBytes: a 256-byte placeholder): sampled only
            for (const Dispatch& d : pack.disp)
                if (d.kernel == static_cast<int>(ki))
                    for (const Bind& b : d.binds)
                        if (b.slot < 32 && ownTex.count(b.res) && !texSlots.back().count(b.slot))
                            return Fail(k.file + ": reads " + pack.res[b.res].id + ", a native kernel's texture, other than by sampling");
            const bool samples = s.find("tex.") != std::string::npos;
            indep.push_back(samples);
            if (samples && !Independent(s))
                return Fail(k.file + ": cannot rewrite its texture sampling for independent mode");
            std::vector<uint8_t> ptx(s.begin(), s.end());
            ptx.push_back(0);
            if (!Cubin(ptx, k.file, bin))
                return false;
            void* h = nullptr;
            if (int r = nv.CreateCubin(dev, bin.data(), static_cast<unsigned>(bin.size()), k.block[0], k.block[1], k.block[2], 0, "dxil_main", &h))
                return Fail(k.file + ": NvAPI_D3D12_CreateCubinComputeShaderEx " + std::to_string(r));
            kernelShaders.push_back(h);
        }
        // fill(u64 p, u32 words, u32 value): four dwords per thread (p 16-byte aligned), the last few one by one.
        static const char kFill[] = R"(.version 7.0
.target sm_75
.address_size 64
.visible .entry fill(.param .u64 p, .param .u32 n, .param .u32 v)
{
    .reg .pred %p<3>;
    .reg .b32 %r<8>;
    .reg .b64 %d<4>;
    ld.param.u64 %d1, [p];
    ld.param.u32 %r1, [n];
    ld.param.u32 %r2, [v];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.u32 %r3, %r3, %r4, %r5;
    shl.b32 %r3, %r3, 2;
    setp.ge.u32 %p1, %r3, %r1;
    @%p1 ret;
    mul.wide.u32 %d2, %r3, 4;
    add.u64 %d3, %d1, %d2;
    add.u32 %r6, %r3, 4;
    setp.gt.u32 %p2, %r6, %r1;
    @%p2 bra TAIL;
    st.global.v4.u32 [%d3], {%r2, %r2, %r2, %r2};
    ret;
TAIL:
    st.global.u32 [%d3], %r2;
    add.u32 %r3, %r3, 1;
    add.u64 %d3, %d3, 4;
    setp.lt.u32 %p1, %r3, %r1;
    @%p1 bra TAIL;
    ret;
}
)";
        if (!Cubin(std::vector<uint8_t>(kFill, kFill + sizeof(kFill)), "fill", bin))
            return false;
        if (int r = nv.CreateCubin(dev, bin.data(), static_cast<unsigned>(bin.size()), 256, 1, 1, 0, "fill", &fillShader))
            return Fail("fill: NvAPI_D3D12_CreateCubinComputeShaderEx " + std::to_string(r));
        // swaprb (xefg_pack.h kSwapRbPtx): R <-> B of every 32-bit texel.
        if (!Cubin(std::vector<uint8_t>(kSwapRbPtx, kSwapRbPtx + sizeof(kSwapRbPtx)), "swaprb", bin))
            return false;
        if (int r = nv.CreateCubin(dev, bin.data(), static_cast<unsigned>(bin.size()), 256, 1, 1, 0, "swaprb", &swapShader))
            return Fail("swaprb: NvAPI_D3D12_CreateCubinComputeShaderEx " + std::to_string(r));
        return true;
    }

    ID3D12Resource* DeviceBuffer(size_t bytes)
    {
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<size_t>((bytes + 255) & ~size_t(255), 256);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
        ID3D12Resource* b = nullptr;
        if (bufHeap)
        {
            const UINT64 at = (heapUsed + 65535) & ~UINT64(65535);
            if (SUCCEEDED(dev->CreatePlacedResource(bufHeap, at, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b))))
            {
                heapUsed = at + rd.Width;
                return b;
            }
        }
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, D3D12_RESOURCE_STATE_COMMON, nullptr, IID_PPV_ARGS(&b));
        return b;
    }
    ID3D12Resource* HostBuffer(D3D12_HEAP_TYPE type, size_t bytes)
    {
        D3D12_HEAP_PROPERTIES hp { type };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_BUFFER;
        rd.Width = std::max<size_t>(bytes, 256);
        rd.Height = rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.SampleDesc.Count = 1;
        rd.Layout = D3D12_TEXTURE_LAYOUT_ROW_MAJOR;
        ID3D12Resource* b = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd,
                                     type == D3D12_HEAP_TYPE_UPLOAD ? D3D12_RESOURCE_STATE_GENERIC_READ : D3D12_RESOURCE_STATE_COPY_DEST,
                                     nullptr, IID_PPV_ARGS(&b));
        return b;
    }

    static void Pitch(const Resource& x, const uint8_t* tight, uint8_t* pitched)
    {
        if (!x.tex)
        {
            memcpy(pitched, tight, x.bytes);
            return;
        }
        const size_t row = static_cast<size_t>(x.w) * x.texel;
        for (size_t y = 0; y < x.Rows(); ++y)
            memcpy(pitched + y * x.pitch, tight + y * row, row);
    }

    // A resource's buffer: pitched as a copy footprint; with a one-texel border where a native kernel writes it so
    // (zeroed once: the module never writes the border); a placeholder where a native kernel writes it as a
    // texture (the texture is the resource).
    size_t BufferBytes(size_t r) const
    {
        const Resource& x = pack.res[r];
        if (paddedRes.count(static_cast<int>(r)))
            return static_cast<size_t>(x.w + 2) * (x.h + 2) * x.texel;
        if (ownTex.count(static_cast<int>(r)))
            return 256;
        return x.bytes;
    }

    // Every resource as a buffer; init files uploaded in one list. All buffers live in UNORDERED_ACCESS after.
    bool CreateBuffers(bool uploadInit)
    {
        if (oneHeap && !bufHeap)
        {
            UINT64 total = 0;
            for (const Resource& x : pack.res)
                total += ((std::max<size_t>((x.bytes + 255) & ~size_t(255), 256) + 65535) & ~UINT64(65535));
            D3D12_HEAP_DESC hd {};
            hd.SizeInBytes = (total + (4u << 20) - 1) & ~UINT64((4u << 20) - 1);
            hd.Properties.Type = D3D12_HEAP_TYPE_DEFAULT;
            hd.Alignment = D3D12_DEFAULT_MSAA_RESOURCE_PLACEMENT_ALIGNMENT;
            hd.Flags = D3D12_HEAP_FLAG_ALLOW_ONLY_BUFFERS;
            if (FAILED(dev->CreateHeap(&hd, IID_PPV_ARGS(&bufHeap))))
                bufHeap = nullptr;
            heapUsed = 0;
        }
        size_t upBytes = 0;
        std::vector<std::vector<uint8_t>> init(pack.res.size());
        for (size_t i = 0; i < pack.res.size(); ++i)
        {
            const Resource& x = pack.res[i];
            ID3D12Resource* b = DeviceBuffer(BufferBytes(i));
            if (!b)
                return Fail(x.id + ": CreateCommittedResource");
            buffers.push_back(b);
            if ((ownTex.count(static_cast<int>(i)) || paddedRes.count(static_cast<int>(i))) && !x.init.empty())
                return Fail(x.id + ": has contents, but a native kernel writes it");
            // The inputs the caller fills every frame (frames, motion, depth, UI) skip their captured contents; the
            // noise texture is constant data like the weights (it was left zero here until 2026-10-24: the D3D12 path
            // then differed from Intel and from the CUDA path, which uploads it).
            const bool role = std::any_of(pack.roles.begin(), pack.roles.end(),
                                          [&](const auto& r) { return r.second == static_cast<int>(i) && r.first != "noise"; });
            if (x.init.empty() || (role && !uploadInit))
                continue;
            auto data = Slurp(pack.dir + "\\" + x.init);
            if (data.size() != x.TightBytes() && pack.version >= 2 && pack.size != pack.captured)
                continue;           // captured frames of a pack evaluated at another size: left zero
            if (data.size() != x.TightBytes())
                return Fail(x.init + ": " + std::to_string(data.size()) + " bytes, the resource needs " + std::to_string(x.TightBytes()));
            init[i].assign(x.bytes, 0);
            Pitch(x, data.data(), init[i].data());
            upBytes += (x.bytes + 255) & ~size_t(255);
        }
        descTable = DeviceBuffer(256);
        if (!descTable)
            return Fail("descriptor table buffer");
        ID3D12Resource* up = HostBuffer(D3D12_HEAP_TYPE_UPLOAD, upBytes);
        if (!up)
            return Fail("upload buffer");
        uint8_t* m = nullptr;
        up->Map(0, nullptr, reinterpret_cast<void**>(&m));
        std::vector<size_t> at(pack.res.size(), 0);
        size_t o = 0;
        for (size_t i = 0; i < init.size(); ++i)
            if (!init[i].empty())
            {
                memcpy(m + o, init[i].data(), init[i].size());
                at[i] = o;
                o += (init[i].size() + 255) & ~size_t(255);
            }
        up->Unmap(0, nullptr);
        const bool ok = Submit([&](ID3D12GraphicsCommandList* cl) {
            std::vector<D3D12_RESOURCE_BARRIER> bs;
            for (ID3D12Resource* b : buffers)
                bs.push_back(TransitionDesc(b, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_COPY_DEST));
            cl->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
            for (size_t i = 0; i < init.size(); ++i)
                if (!init[i].empty())
                    cl->CopyBufferRegion(buffers[i], 0, up, at[i], init[i].size());
            for (auto& b : bs)
            {
                b.Transition.StateBefore = D3D12_RESOURCE_STATE_COPY_DEST;
                b.Transition.StateAfter = D3D12_RESOURCE_STATE_UNORDERED_ACCESS;
            }
            cl->ResourceBarrier(static_cast<UINT>(bs.size()), bs.data());
            // the descriptor table is never read; keep it in UAV too
            Transition(cl, descTable, D3D12_RESOURCE_STATE_COMMON, D3D12_RESOURCE_STATE_UNORDERED_ACCESS);
            for (const int r : paddedRes)
                Fill(cl, r, 0);
        });
        up->Release();
        return ok;
    }

    // A Texture2D per (resource, view format) a sampling kernel reads, with its SRV and NVAPI texture handle.
    bool CreateShadows()
    {
        std::vector<std::pair<int, uint32_t>> want;
        for (const Dispatch& d : pack.disp)
        {
            const bool nat = natives[d.kernel].pso != nullptr;
            for (const Bind& b : d.binds)
                for (const int r : { b.res, b.byT[0], b.byT[1] })     // a two-frame bind samples both
                {
                    if (r < 0)
                        continue;
                    const Resource& x = pack.res[r];
                    int f = 0;
                    unsigned ch = 0;
                    if (texSlots[d.kernel].count(b.slot) && x.tex && b.slot < 32 && x.slices == 1 && (nat || TexFormat(b.fmt, &f, &ch)) &&
                        std::find(want.begin(), want.end(), std::make_pair(r, b.fmt)) == want.end())
                        want.emplace_back(r, b.fmt);
                }
        }
        for (const auto& [r, f] : ownTex)
            if (std::find(want.begin(), want.end(), std::make_pair(r, f)) == want.end())
                want.emplace_back(r, f);
        for (const auto& w : want)
            if (paddedRes.count(w.first))
                return Fail(pack.res[w.first].id + ": read as a texture, but a native kernel writes it with a border");
        UINT nativeDescs = 0;
        for (const Dispatch& d : pack.disp)
            for (const NativeKernel::Param& p : natives[d.kernel].params)
                for (const D3D12_DESCRIPTOR_RANGE1& r : p.ranges)
                    nativeDescs += r.NumDescriptors;
        if (want.empty() && !nativeDescs)
            return true;
        shadowDescs = static_cast<UINT>(want.size());
        D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV, std::max(1u, shadowDescs + nativeDescs), D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        D3D12_DESCRIPTOR_HEAP_DESC sd { D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER, 2, D3D12_DESCRIPTOR_HEAP_FLAG_SHADER_VISIBLE };
        if (FAILED(dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&srvHeap))) || FAILED(dev->CreateDescriptorHeap(&sd, IID_PPV_ARGS(&smpHeap))))
            return Fail("descriptor heaps");
        // The CUDA runner's two texture objects: linear clamp (normalized), and point with a zero border.
        D3D12_SAMPLER_DESC smp {};
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_LINEAR;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_CLAMP;
        smp.MaxLOD = D3D12_FLOAT32_MAX;
        D3D12_CPU_DESCRIPTOR_HANDLE smpCpu = smpHeap->GetCPUDescriptorHandleForHeapStart();
        dev->CreateSampler(&smp, smpCpu);
        if (!nv.Handle(dev, 2, smpCpu, samplers[0]))
            return Fail("NvAPI_D3D12_GetCudaIndependentDescriptorObject (linear sampler)");
        smp.Filter = D3D12_FILTER_MIN_MAG_MIP_POINT;
        smp.AddressU = smp.AddressV = smp.AddressW = D3D12_TEXTURE_ADDRESS_MODE_BORDER;
        smpCpu.ptr += dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_SAMPLER);
        dev->CreateSampler(&smp, smpCpu);
        if (!nv.Handle(dev, 2, smpCpu, samplers[1]))
            return Fail("NvAPI_D3D12_GetCudaIndependentDescriptorObject (point sampler)");
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        for (size_t i = 0; i < want.size(); ++i)
        {
            const Resource& x = pack.res[want[i].first];
            Shadow s;
            s.res = want[i].first;
            s.fmt = want[i].second;
            if (TexelBytes(s.fmt) && TexelBytes(s.fmt) != x.texel)
                return Fail(x.id + ": view format of another texel size");
            D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
            D3D12_RESOURCE_DESC rd {};
            rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
            rd.Width = x.w;
            rd.Height = x.h;
            rd.DepthOrArraySize = rd.MipLevels = 1;
            rd.Format = static_cast<DXGI_FORMAT>(s.fmt);
            rd.SampleDesc.Count = 1;
            s.own = ownTex.count(s.res) != 0;
            if (s.own)
            {
                rd.Flags = D3D12_RESOURCE_FLAG_ALLOW_UNORDERED_ACCESS;
                s.fresh = s.readable = true;
            }
            if (FAILED(dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, s.own ? D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE
                                                                                           : D3D12_RESOURCE_STATE_COPY_DEST,
                                                    nullptr, IID_PPV_ARGS(&s.tex))))
                return Fail(x.id + ": shadow texture");
            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
            v.Format = rd.Format;
            v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
            v.Texture2D.MipLevels = 1;
            D3D12_CPU_DESCRIPTOR_HANDLE srv = srvHeap->GetCPUDescriptorHandleForHeapStart();
            srv.ptr += i * inc;
            dev->CreateShaderResourceView(s.tex, &v, srv);
            if (!nv.Handle(dev, 1, srv, s.handle))
                return Fail(x.id + ": NvAPI_D3D12_GetCudaIndependentDescriptorObject (texture)");
            shadows.push_back(s);
        }
        return NativeTables();
    }

    // The native dispatches' descriptor tables in srvHeap, after the shadows' SRVs, and their constant buffers
    // (contents as built: only root constants may change per pass).
    bool NativeTables()
    {
        nativeTables.assign(pack.disp.size(), {});
        size_t cbBytes = 0;
        std::map<std::pair<size_t, int>, size_t> cbAt;
        auto rootConstant = [](const NativeKernel& nk, int reg) {
            return std::any_of(nk.params.begin(), nk.params.end(), [&](const NativeKernel::Param& p) {
                return p.type == D3D12_ROOT_PARAMETER_TYPE_32BIT_CONSTANTS && p.reg == static_cast<UINT>(reg);
            });
        };
        for (size_t i = 0; i < pack.disp.size(); ++i)
        {
            const Dispatch& d = pack.disp[i];
            const NativeKernel& nk = natives[d.kernel];
            if (!nk.pso)
                continue;
            for (const Patch& p : d.patches)
                if (!rootConstant(nk, p.reg))
                    return Fail(d.module + ": a per-pass value in a constant buffer of a native dispatch");
            for (const auto& [reg, bytes] : d.cbs)
                if (!rootConstant(nk, reg))
                {
                    cbAt[{ i, reg }] = cbBytes;
                    cbBytes += (bytes.size() + 255) & ~size_t(255);
                }
        }
        if (cbBytes)
        {
            nativeCb = HostBuffer(D3D12_HEAP_TYPE_UPLOAD, cbBytes);
            uint8_t* m = nullptr;
            if (!nativeCb || FAILED(nativeCb->Map(0, nullptr, reinterpret_cast<void**>(&m))))
                return Fail("native constant buffers");
            for (const auto& [at, o] : cbAt)
            {
                const std::vector<uint8_t>& b = pack.disp[at.first].cbs.at(at.second);
                memcpy(m + o, b.data(), b.size());
            }
            nativeCb->Unmap(0, nullptr);
        }
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        UINT next = shadowDescs;
        for (size_t i = 0; i < pack.disp.size(); ++i)
        {
            const Dispatch& d = pack.disp[i];
            const NativeKernel& nk = natives[d.kernel];
            if (!nk.pso)
                continue;
            auto bound = [&](int slot) -> const Bind* {
                for (const Bind& b : d.binds)
                    if (b.slot == slot)
                        return &b;
                return nullptr;
            };
            auto own = [&](int r, uint32_t f) -> ID3D12Resource* {
                for (const Shadow& s : shadows)
                    if (s.res == r && s.fmt == f)
                        return s.tex;
                return nullptr;
            };
            for (const NativeKernel::Param& p : nk.params)
            {
                nativeTables[i].push_back(next);
                for (const D3D12_DESCRIPTOR_RANGE1& rg : p.ranges)
                    for (UINT k = 0; k < rg.NumDescriptors; ++k, ++next)
                    {
                        D3D12_CPU_DESCRIPTOR_HANDLE h = srvHeap->GetCPUDescriptorHandleForHeapStart();
                        h.ptr += static_cast<SIZE_T>(next) * inc;
                        const int reg = static_cast<int>(rg.BaseShaderRegister + k);
                        if (rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_CBV)
                        {
                            D3D12_CONSTANT_BUFFER_VIEW_DESC v {};
                            if (auto it = cbAt.find({ i, reg }); it != cbAt.end())
                            {
                                v.BufferLocation = nativeCb->GetGPUVirtualAddress() + it->second;
                                v.SizeInBytes = static_cast<UINT>((d.cbs.at(reg).size() + 255) & ~size_t(255));
                            }
                            dev->CreateConstantBufferView(&v, h);
                            continue;
                        }
                        const bool uav = rg.RangeType == D3D12_DESCRIPTOR_RANGE_TYPE_UAV;
                        const Bind* b = bound(uav ? 32 + reg : reg);
                        if (!b)
                        {
                            // in the table, not bound: a register this module does not use (one root signature
                            // serves its variants) - a null view, as the CUDA path's zero slot
                            if (uav)
                            {
                                D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
                                v.Format = DXGI_FORMAT_R32_FLOAT;
                                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                                dev->CreateUnorderedAccessView(nullptr, nullptr, &v, h);
                            }
                            else
                            {
                                D3D12_SHADER_RESOURCE_VIEW_DESC v {};
                                v.Format = DXGI_FORMAT_R32_FLOAT;
                                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                                v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                                v.Texture2D.MipLevels = 1;
                                dev->CreateShaderResourceView(nullptr, &v, h);
                            }
                            continue;
                        }
                        const Resource& x = pack.res[b->res];
                        const bool raw = !x.tex || paddedRes.count(b->res);
                        ID3D12Resource* tex = raw ? nullptr : own(b->res, b->fmt);
                        if (!raw && !tex)
                            return Fail(d.module + ": native " + x.id + " has no texture");
                        if (uav && !raw && !ownTex.count(b->res))
                            return Fail(d.module + ": native " + x.id + " written as a texture it does not own");
                        if (uav)
                        {
                            D3D12_UNORDERED_ACCESS_VIEW_DESC v {};
                            if (raw)
                            {
                                v.Format = DXGI_FORMAT_R32_TYPELESS;
                                v.ViewDimension = D3D12_UAV_DIMENSION_BUFFER;
                                v.Buffer.NumElements = static_cast<UINT>(buffers[b->res]->GetDesc().Width / 4);
                                v.Buffer.Flags = D3D12_BUFFER_UAV_FLAG_RAW;
                            }
                            else
                            {
                                v.Format = static_cast<DXGI_FORMAT>(b->fmt);
                                v.ViewDimension = D3D12_UAV_DIMENSION_TEXTURE2D;
                            }
                            dev->CreateUnorderedAccessView(raw ? buffers[b->res] : tex, nullptr, &v, h);
                        }
                        else
                        {
                            D3D12_SHADER_RESOURCE_VIEW_DESC v {};
                            v.Shader4ComponentMapping = D3D12_DEFAULT_SHADER_4_COMPONENT_MAPPING;
                            if (raw)
                            {
                                v.Format = DXGI_FORMAT_R32_TYPELESS;
                                v.ViewDimension = D3D12_SRV_DIMENSION_BUFFER;
                                v.Buffer.NumElements = static_cast<UINT>(buffers[b->res]->GetDesc().Width / 4);
                                v.Buffer.Flags = D3D12_BUFFER_SRV_FLAG_RAW;
                            }
                            else
                            {
                                v.Format = static_cast<DXGI_FORMAT>(b->fmt);
                                v.ViewDimension = D3D12_SRV_DIMENSION_TEXTURE2D;
                                v.Texture2D.MipLevels = 1;
                            }
                            dev->CreateShaderResourceView(raw ? buffers[b->res] : tex, &v, h);
                        }
                    }
            }
        }
        return true;
    }

    // The CUDA runner's 48-byte slot with D3D12 addresses: u64 pointer, u64 linear texture, u32 width, height,
    // slices, bytes per texel, total bytes, pitch, u64 point texture.
    bool Descriptor(const Bind& b, uint8_t* s)
    {
        const Resource& x = pack.res[b.res];
        const uint32_t tb = x.tex ? (TexelBytes(b.fmt) ? TexelBytes(b.fmt) : x.texel) : 0;
        uint64_t tex = 0;
        for (const Shadow& sh : shadows)
            if (sh.res == b.res && sh.fmt == b.fmt && b.slot < 32)
                tex = sh.handle;
        uint64_t ptr = buffers[b.res]->GetGPUVirtualAddress();
        size_t pitch = x.pitch;
        if (paddedRes.count(b.res))
        {
            // the interior of a bordered buffer: texel (x, y) at (x + 1, y + 1)
            pitch = static_cast<size_t>(x.w + 2) * x.texel;
            ptr += pitch + x.texel;
        }
        const uint32_t dims[6] = { x.tex ? x.w : 0, x.tex ? x.h : 0, x.tex ? x.slices : 1, tb, static_cast<uint32_t>(x.bytes),
                                   static_cast<uint32_t>(pitch) };
        memcpy(s, &ptr, 8);
        memcpy(s + 8, &tex, 8);
        memcpy(s + 16, dims, 24);
        memcpy(s + 40, &tex, 8);
        return true;
    }

    static D3D12_RESOURCE_BARRIER TransitionDesc(ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        D3D12_RESOURCE_BARRIER x {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        x.Transition.pResource = r;
        x.Transition.Subresource = D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES;
        x.Transition.StateBefore = a;
        x.Transition.StateAfter = b;
        return x;
    }
    static void Transition(ID3D12GraphicsCommandList* cl, ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b)
    {
        const D3D12_RESOURCE_BARRIER x = TransitionDesc(r, a, b);
        cl->ResourceBarrier(1, &x);
    }
    static void UavBarrier(ID3D12GraphicsCommandList* cl)
    {
        D3D12_RESOURCE_BARRIER x {};
        x.Type = D3D12_RESOURCE_BARRIER_TYPE_UAV;
        cl->ResourceBarrier(1, &x);
    }

    // A native dispatch: its raw SRVs (buffers, UNORDERED_ACCESS between dispatches) readable and the textures it
    // writes writable while it runs; root tables and constants (this pass's, as Pack::SetPass left them).
    void RecordNative(ID3D12GraphicsCommandList* cl, size_t i, const Dispatch& d)
    {
        const NativeKernel& nk = natives[d.kernel];
        std::vector<D3D12_RESOURCE_BARRIER> pre, post;
        std::set<ID3D12Resource*> seen;
        for (const Bind& b : d.binds)
        {
            const bool raw = !pack.res[b.res].tex || paddedRes.count(b.res);
            ID3D12Resource* r = nullptr;
            D3D12_RESOURCE_STATES from = D3D12_RESOURCE_STATE_UNORDERED_ACCESS, to = D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE;
            if (b.slot < 32 && raw)
                r = buffers[b.res];
            else if (b.slot >= 32 && !raw)
                for (const Shadow& s : shadows)
                    if (s.own && s.res == b.res)
                        r = s.tex, std::swap(from, to);
            if (r && seen.insert(r).second)
            {
                pre.push_back(TransitionDesc(r, from, to));
                post.push_back(TransitionDesc(r, to, from));
            }
        }
        if (!pre.empty())
            cl->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
        cl->SetComputeRootSignature(nk.rs);
        cl->SetPipelineState(nk.pso);
        const UINT inc = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_CBV_SRV_UAV);
        const D3D12_GPU_DESCRIPTOR_HANDLE base = srvHeap->GetGPUDescriptorHandleForHeapStart();
        for (size_t p = 0; p < nk.params.size(); ++p)
        {
            const NativeKernel::Param& prm = nk.params[p];
            if (prm.type == D3D12_ROOT_PARAMETER_TYPE_DESCRIPTOR_TABLE)
            {
                cl->SetComputeRootDescriptorTable(static_cast<UINT>(p), { base.ptr + static_cast<UINT64>(nativeTables[i][p]) * inc });
                continue;
            }
            std::vector<uint32_t> v(prm.count, 0);
            const int reg = static_cast<int>(prm.reg);
            if (auto it = d.cbBlob.find(reg); it != d.cbBlob.end())
                memcpy(v.data(), d.blobs[it->second].data(), std::min(d.blobs[it->second].size(), v.size() * 4));
            else if (auto c = d.cbs.find(reg); c != d.cbs.end())
                memcpy(v.data(), c->second.data(), std::min(c->second.size(), v.size() * 4));
            cl->SetComputeRoot32BitConstants(static_cast<UINT>(p), prm.count, v.data(), 0);
        }
        cl->Dispatch(d.g[0], d.g[1], d.g[2]);
        if (!post.empty())
            cl->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
    }

    void Fill(ID3D12GraphicsCommandList* cl, int r, uint8_t v, size_t bytes = 0)
    {
        if (!bytes || bytes > BufferBytes(r))
            bytes = BufferBytes(r);
        struct { uint64_t p; uint32_t n, v; } a { Address(r), static_cast<uint32_t>((bytes + 3) / 4), v * 0x01010101u };
        nv.LaunchCubin(cl, fillShader, (a.n + 1023) / 1024, 1, 1, &a, sizeof(a));
    }

    // Copies each texture this dispatch samples from its buffer when the buffer changed since the last copy.
    void RefreshShadows(ID3D12GraphicsCommandList* cl, const Dispatch& d)
    {
        std::vector<Shadow*> stale;
        for (Shadow& s : shadows)
            if (!s.fresh && !(skipShadowCopies && s.readable) && std::any_of(d.binds.begin(), d.binds.end(), [&](const Bind& b) {
                    return b.res == s.res && b.fmt == s.fmt && b.slot < 32 && texSlots[d.kernel].count(b.slot);
                }))
                stale.push_back(&s);
        lastCopies = static_cast<int>(stale.size());
        if (stale.empty())
            return;
        std::vector<D3D12_RESOURCE_BARRIER> pre, post;
        std::vector<int> bufs;
        for (Shadow* s : stale)
        {
            if (std::find(bufs.begin(), bufs.end(), s->res) == bufs.end())
            {
                bufs.push_back(s->res);
                pre.push_back(TransitionDesc(buffers[s->res], D3D12_RESOURCE_STATE_UNORDERED_ACCESS, D3D12_RESOURCE_STATE_COPY_SOURCE));
                post.push_back(TransitionDesc(buffers[s->res], D3D12_RESOURCE_STATE_COPY_SOURCE, D3D12_RESOURCE_STATE_UNORDERED_ACCESS));
            }
            if (s->readable)
                pre.push_back(TransitionDesc(s->tex, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE, D3D12_RESOURCE_STATE_COPY_DEST));
            post.push_back(TransitionDesc(s->tex, D3D12_RESOURCE_STATE_COPY_DEST, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE));
        }
        cl->ResourceBarrier(static_cast<UINT>(pre.size()), pre.data());
        for (Shadow* s : stale)
        {
            const Resource& x = pack.res[s->res];
            D3D12_TEXTURE_COPY_LOCATION dst {}, src {};
            dst.pResource = s->tex;
            dst.Type = D3D12_TEXTURE_COPY_TYPE_SUBRESOURCE_INDEX;
            src.pResource = buffers[s->res];
            src.Type = D3D12_TEXTURE_COPY_TYPE_PLACED_FOOTPRINT;
            src.PlacedFootprint.Footprint = { static_cast<DXGI_FORMAT>(s->fmt), x.w, x.h, 1, static_cast<UINT>(x.pitch) };
            cl->CopyTextureRegion(&dst, 0, 0, 0, &src, nullptr);
            s->fresh = true;
            s->readable = true;
        }
        cl->ResourceBarrier(static_cast<UINT>(post.size()), post.data());
    }
};

}  // namespace xefgpack
