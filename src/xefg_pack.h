// xefg_pack.h - runs a graph pack (tools/build_graph_pack.py) as one CUDA graph: the whole XeFG frame,
// 38 dispatches, without D3D12 or Intel's DLL. Shared by tools/cuda/pack_run.cpp (offline check) and
// the xefg_cuda replacement DLL (tools/xefg_dll).
//
// The CUDA driver is loaded at run time (nvcuda.dll), so nothing links against the toolkit.
// Resources follow dxil2ptx.py's layout: raw buffers as bytes; textures as rows of a 256-byte-aligned
// pitch, slice after slice - the layout D3D12's CopyTextureRegion writes into a buffer footprint, which
// is what lets the DLL hand frames over with one copy. Each kernel takes p_desc (unused), then one
// 48-byte p_s<slot> per bound descriptor slot and one p_cb<reg> per constant buffer, in the order its
// PTX declares them (the same packing tools/cuda/generic_check.cpp uses, which is the bit-exact truth).
//
// The graph is built explicitly (not by stream capture) so each dispatch's kernel node can be kept and
// its constants changed per frame with cuGraphExecKernelNodeSetParams.
#pragma once

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cmath>
#include <type_traits>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <sstream>
#include <string>
#include <vector>

#include "long_path.h"

namespace xefgpack
{

typedef int CUresult;
typedef int CUdevice;
typedef unsigned long long CUdeviceptr;
typedef unsigned long long CUtexObject;
typedef void* CUcontext;
typedef void* CUmodule;
typedef void* CUfunction;
typedef void* CUstream;
typedef void* CUgraph;
typedef void* CUgraphExec;
typedef void* CUgraphNode;
typedef void* CUevent;
typedef void* CUexternalMemory;
typedef void* CUexternalSemaphore;

struct ResDesc
{
    int resType;
    int pad0;
    CUdeviceptr devPtr;
    int format;
    unsigned numChannels;
    size_t width, height, pitchInBytes;
    int reserved[32 - 10];
    unsigned flags;
    int pad1;
};
static_assert(sizeof(ResDesc) == 144, "CUDA_RESOURCE_DESC layout");

struct TexDesc
{
    int addressMode[3];
    int filterMode;
    unsigned flags;
    unsigned maxAnisotropy;
    int mipmapFilterMode;
    float mipmapLevelBias, minMipmapLevelClamp, maxMipmapLevelClamp;
    float borderColor[4];
    int reserved[12];
};
static_assert(sizeof(TexDesc) == 104, "CUDA_TEXTURE_DESC layout");

struct KernelNodeParams        // CUDA_KERNEL_NODE_PARAMS (v1: cuGraphAddKernelNode without _v2)
{
    CUfunction func;
    unsigned gridX, gridY, gridZ, blockX, blockY, blockZ, shared;
    void** params;
    void** extra;
};
static_assert(sizeof(KernelNodeParams) == 56, "CUDA_KERNEL_NODE_PARAMS layout");

struct MemsetNodeParams        // CUDA_MEMSET_NODE_PARAMS
{
    CUdeviceptr dst;
    size_t pitch;
    unsigned value, elementSize;
    size_t width, height;
};
static_assert(sizeof(MemsetNodeParams) == 40, "CUDA_MEMSET_NODE_PARAMS layout");

// External memory / semaphores (cuda.h layouts; proven in tools/interop/d3d12_cuda.cpp).
enum { kExtMemD3D12Resource = 5, kExtSemD3D12Fence = 4, kExtMemDedicated = 1 };

struct ExtMemHandleDesc
{
    unsigned type;
    union
    {
        int fd;
        struct { void* handle; const void* name; } win32;
        const void* nvSciBufObject;
    } handle;
    unsigned long long size;
    unsigned flags;
    unsigned reserved[16];
};

struct ExtMemBufferDesc
{
    unsigned long long offset, size;
    unsigned flags;
    unsigned reserved[16];
};

struct ExtSemHandleDesc
{
    unsigned type;
    union
    {
        int fd;
        struct { void* handle; const void* name; } win32;
        const void* nvSciSyncObj;
    } handle;
    unsigned flags;
    unsigned reserved[16];
};

struct ExtSemSignalParams
{
    struct
    {
        struct { unsigned long long value; } fence;
        union { void* fence; unsigned long long reserved; } nvSciSync;
        struct { unsigned long long key; } keyedMutex;
        unsigned reserved[12];
    } params;
    unsigned flags;
    unsigned reserved[16];
};

struct ExtSemWaitParams
{
    struct
    {
        struct { unsigned long long value; } fence;
        union { void* fence; unsigned long long reserved; } nvSciSync;
        struct { unsigned long long key; unsigned timeoutMs; } keyedMutex;
        unsigned reserved[10];
    } params;
    unsigned flags;
    unsigned reserved[16];
};

struct Cuda
{
    HMODULE lib = nullptr;
#define XEFG_CU(ret, name, args) ret(*name) args = nullptr;
#define XEFG_CU_LIST                                                                                       \
    XEFG_CU(CUresult, Init, (unsigned))                                                                    \
    XEFG_CU(CUresult, DeviceGet, (CUdevice*, int))                                                         \
    XEFG_CU(CUresult, DeviceGetCount, (int*))                                                              \
    XEFG_CU(CUresult, DeviceGetName, (char*, int, CUdevice))                                               \
    XEFG_CU(CUresult, DeviceGetLuid, (char*, unsigned*, CUdevice))                                         \
    XEFG_CU(CUresult, DeviceGetAttribute, (int*, int, CUdevice))                                           \
    XEFG_CU(CUresult, CtxCreate, (CUcontext*, unsigned, CUdevice))                                         \
    XEFG_CU(CUresult, CtxDestroy, (CUcontext))                                                             \
    XEFG_CU(CUresult, CtxPushCurrent, (CUcontext))                                                         \
    XEFG_CU(CUresult, CtxPopCurrent, (CUcontext*))                                                         \
    XEFG_CU(CUresult, CtxSynchronize, ())                                                                  \
    XEFG_CU(CUresult, ModuleLoadDataEx, (CUmodule*, const void*, unsigned, int*, void**))                 \
    XEFG_CU(CUresult, ModuleGetFunction, (CUfunction*, CUmodule, const char*))                            \
    XEFG_CU(CUresult, ModuleUnload, (CUmodule))                                                            \
    XEFG_CU(CUresult, MemAlloc, (CUdeviceptr*, size_t))                                                    \
    XEFG_CU(CUresult, MemFree, (CUdeviceptr))                                                              \
    XEFG_CU(CUresult, MemsetD8, (CUdeviceptr, unsigned char, size_t))                                     \
    XEFG_CU(CUresult, MemcpyHtoD, (CUdeviceptr, const void*, size_t))                                     \
    XEFG_CU(CUresult, MemcpyDtoH, (void*, CUdeviceptr, size_t))                                           \
    XEFG_CU(CUresult, MemcpyDtoDAsync, (CUdeviceptr, CUdeviceptr, size_t, CUstream))                      \
    XEFG_CU(CUresult, LaunchKernel, (CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,  \
                                     unsigned, CUstream, void**, void**))                                  \
    XEFG_CU(CUresult, TexObjectCreate, (CUtexObject*, const ResDesc*, const TexDesc*, const void*))      \
    XEFG_CU(CUresult, TexObjectDestroy, (CUtexObject))                                                     \
    XEFG_CU(CUresult, StreamCreate, (CUstream*, unsigned))                                                 \
    XEFG_CU(CUresult, StreamDestroy, (CUstream))                                                           \
    XEFG_CU(CUresult, StreamSynchronize, (CUstream))                                                       \
    XEFG_CU(CUresult, EventCreate, (CUevent*, unsigned))                                                   \
    XEFG_CU(CUresult, EventRecord, (CUevent, CUstream))                                                    \
    XEFG_CU(CUresult, EventDestroy, (CUevent))                                                             \
    XEFG_CU(CUresult, EventSynchronize, (CUevent))                                                         \
    XEFG_CU(CUresult, EventElapsedTime, (float*, CUevent, CUevent))                                       \
    XEFG_CU(CUresult, LaunchHostFunc, (CUstream, void (*)(void*), void*))                                 \
    XEFG_CU(CUresult, GraphCreate, (CUgraph*, unsigned))                                                   \
    XEFG_CU(CUresult, GraphDestroy, (CUgraph))                                                             \
    XEFG_CU(CUresult, GraphAddKernelNode, (CUgraphNode*, CUgraph, const CUgraphNode*, size_t,             \
                                           const KernelNodeParams*))                                       \
    XEFG_CU(CUresult, GraphAddMemsetNode, (CUgraphNode*, CUgraph, const CUgraphNode*, size_t,             \
                                           const MemsetNodeParams*, CUcontext))                            \
    XEFG_CU(CUresult, GraphInstantiate, (CUgraphExec*, CUgraph, unsigned long long))                      \
    XEFG_CU(CUresult, GraphExecDestroy, (CUgraphExec))                                                     \
    XEFG_CU(CUresult, GraphExecKernelNodeSetParams, (CUgraphExec, CUgraphNode, const KernelNodeParams*))  \
    XEFG_CU(CUresult, GraphLaunch, (CUgraphExec, CUstream))                                                \
    XEFG_CU(CUresult, ImportExternalMemory, (CUexternalMemory*, const ExtMemHandleDesc*))                 \
    XEFG_CU(CUresult, ExternalMemoryGetMappedBuffer, (CUdeviceptr*, CUexternalMemory, const ExtMemBufferDesc*)) \
    XEFG_CU(CUresult, DestroyExternalMemory, (CUexternalMemory))                                           \
    XEFG_CU(CUresult, ImportExternalSemaphore, (CUexternalSemaphore*, const ExtSemHandleDesc*))           \
    XEFG_CU(CUresult, SignalExternalSemaphoresAsync, (const CUexternalSemaphore*, const ExtSemSignalParams*, \
                                                      unsigned, CUstream))                                 \
    XEFG_CU(CUresult, WaitExternalSemaphoresAsync, (const CUexternalSemaphore*, const ExtSemWaitParams*,  \
                                                    unsigned, CUstream))                                   \
    XEFG_CU(CUresult, DestroyExternalSemaphore, (CUexternalSemaphore))                                     \
    XEFG_CU(CUresult, LinkCreate, (unsigned, int*, void**, void**))                                        \
    XEFG_CU(CUresult, LinkAddData, (void*, int, void*, size_t, const char*, unsigned, int*, void**))      \
    XEFG_CU(CUresult, LinkComplete, (void*, void**, size_t*))                                              \
    XEFG_CU(CUresult, LinkDestroy, (void*))                                                                \
    XEFG_CU(CUresult, GetErrorName, (CUresult, const char**))
    XEFG_CU_LIST
#undef XEFG_CU

    bool Load()
    {
        if (lib)
            return true;
        lib = LoadLibraryW(L"nvcuda.dll");
        if (!lib)
            return false;
        // Driver export names: the _v2 entry points where the toolkit headers map to them.
        static const struct { const char* api; const char* exp; } kNames[] = {
            { "CtxCreate", "cuCtxCreate_v2" },           { "CtxDestroy", "cuCtxDestroy_v2" },
            { "CtxPushCurrent", "cuCtxPushCurrent_v2" }, { "CtxPopCurrent", "cuCtxPopCurrent_v2" },
            { "MemAlloc", "cuMemAlloc_v2" },             { "MemFree", "cuMemFree_v2" },
            { "MemsetD8", "cuMemsetD8_v2" },             { "MemcpyHtoD", "cuMemcpyHtoD_v2" },
            { "MemcpyDtoH", "cuMemcpyDtoH_v2" },         { "StreamDestroy", "cuStreamDestroy_v2" },
            { "MemcpyDtoDAsync", "cuMemcpyDtoDAsync_v2" },
            { "EventDestroy", "cuEventDestroy_v2" },     { "GraphInstantiate", "cuGraphInstantiateWithFlags" },
            { "LinkCreate", "cuLinkCreate_v2" },         { "LinkAddData", "cuLinkAddData_v2" },
        };
        auto exportName = [](const char* api, std::string& buf) -> const char* {
            for (const auto& n : kNames)
                if (!strcmp(n.api, api))
                    return n.exp;
            buf = std::string("cu") + api;
            return buf.c_str();
        };
        bool ok = true;
        std::string buf;
#define XEFG_CU(ret, name, args)                                                                           \
    *reinterpret_cast<FARPROC*>(&name) = GetProcAddress(lib, exportName(#name, buf));                      \
    ok = ok && name != nullptr;
        XEFG_CU_LIST
#undef XEFG_CU
        return ok;
    }
    const char* Err(CUresult r) const
    {
        const char* s = nullptr;
        if (GetErrorName && GetErrorName(r, &s) == 0 && s)
            return s;
        return "?";
    }
};

// A pack held in memory (xefg_cuda.dll's embedded pack): its files by lower-case relative path with '\' separators.
// A pack loaded from kEmbeddedDir reads from here instead of the disk.
inline const char* const kEmbeddedDir = "<embedded>";
inline std::map<std::string, std::vector<uint8_t>>& EmbeddedFiles()
{
    static std::map<std::string, std::vector<uint8_t>> files;
    return files;
}

inline std::vector<uint8_t> Slurp(const std::string& p)
{
    const std::string prefix = std::string(kEmbeddedDir) + "\\";
    if (p.compare(0, prefix.size(), prefix) == 0)
    {
        std::string rel = p.substr(prefix.size());
        for (char& c : rel)
            c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
        // a nested pack (hdr\) names the main pack's shared files as ..\data\...
        for (size_t up; (up = rel.find("\\..\\")) != std::string::npos;)
        {
            const size_t sep = up ? rel.rfind('\\', up - 1) : std::string::npos;
            const size_t from = sep == std::string::npos ? 0 : sep + 1;
            rel.erase(from, up + 4 - from);
        }
        auto& files = EmbeddedFiles();
        auto it = files.find(rel);
        return it == files.end() ? std::vector<uint8_t>() : it->second;
    }
    // a pack folder deep in an install path (the pack cache) passes MAX_PATH: open it in the extended form
    std::ifstream f;
    if (p.size() >= longpath::kPlainLimit)
        f.open(longpath::Extend(longpath::Widen(p)).c_str(), std::ios::binary);
    else
        f.open(p, std::ios::binary);
    if (!f)
        return {};
    return std::vector<uint8_t>((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
}

// Bytes per texel of a view format, as generic_check.cpp has them (0 = raw buffer view).
inline uint32_t TexelBytes(uint32_t fmt)
{
    switch (fmt)
    {
    case 2: case 3: return 16;
    case 10: return 8;
    case 16: case 17: return 8;
    case 41: case 42: return 4;
    case 28: return 4;
    case 24: return 4;
    case 34: return 4;
    case 61: return 1;
    case 52: return 2;
    default: return 0;
    }
}

// The texture unit's format and channel count for a sampled SRV (same table as generic_check.cpp).
inline bool TexFormat(uint32_t fmt, int* cuFmt, unsigned* ch)
{
    switch (fmt)
    {
    case 2: *cuFmt = 0x20; *ch = 4; return true;
    case 16: *cuFmt = 0x20; *ch = 2; return true;
    case 41: *cuFmt = 0x20; *ch = 1; return true;
    case 28: *cuFmt = 0x01; *ch = 4; return true;
    case 24: *cuFmt = 0x50; *ch = 4; return true;
    case 61: *cuFmt = 0x01; *ch = 1; return true;
    case 34: *cuFmt = 0x10; *ch = 2; return true;
    default: return false;
    }
}

// The four sizes a "pack 2" manifest's formulas are written in: output (OW, OH) and render (RW, RH).
struct Sizes
{
    uint32_t OW = 0, OH = 0, RW = 0, RH = 0;
    // The interpolation area (letterboxing: the back buffer tag's region); AW = AH = 0: the whole output.
    uint32_t AX = 0, AY = 0, AW = 0, AH = 0;
    bool operator==(const Sizes& o) const
    {
        return OW == o.OW && OH == o.OH && RW == o.RW && RH == o.RH && AX == o.AX && AY == o.AY && AW == o.AW && AH == o.AH;
    }
    bool operator!=(const Sizes& o) const { return !(*this == o); }
};

// Evaluates one formula of tools/pack_generalize.py: numbers, OW OH RW RH (AX AY AW AH: the interpolation
// area, tools/pack_letterbox.py), + - * / % (real division),
// parentheses, ceil(x) floor(x) round(x) = floor(x + 0.5), align(x, a) = ceil(x / a) * a. Double precision,
// as the generalizer checks them.
class Expr
{
public:
    static bool Eval(const std::string& e, const Sizes& s, double& out)
    {
        Expr x(e, s);
        out = x.Sum();
        return x.ok && x.p == x.e.size();
    }

private:
    Expr(const std::string& t, const Sizes& sz) : e(t), s(sz) {}
    std::string e;
    const Sizes& s;
    size_t p = 0;
    bool ok = true;

    bool Eat(char c)
    {
        if (p < e.size() && e[p] == c)
        {
            ++p;
            return true;
        }
        return false;
    }
    double Sum()
    {
        double v = Product();
        for (;;)
            if (Eat('+'))
                v += Product();
            else if (Eat('-'))
                v -= Product();
            else
                return v;
    }
    double Product()
    {
        double v = Unary();
        for (;;)
            if (Eat('*'))
                v *= Unary();
            else if (Eat('/'))
                v /= Unary();
            else if (Eat('%'))
                v = std::fmod(v, Unary());
            else
                return v;
    }
    double Unary() { return Eat('-') ? -Unary() : Atom(); }
    double Atom()
    {
        if (Eat('('))
        {
            const double v = Sum();
            ok = ok && Eat(')');
            return v;
        }
        if (p < e.size() && (isdigit(static_cast<unsigned char>(e[p])) || e[p] == '.'))
        {
            size_t n = 0;
            double v = 0;
            try
            {
                v = std::stod(e.substr(p), &n);
            }
            catch (...)         // "." alone, or out of range: a bad formula, never an exception into the game
            {
                ok = false;
                n = e.size() - p;
            }
            p += n;
            return v;
        }
        size_t q = p;
        while (q < e.size() && isalpha(static_cast<unsigned char>(e[q])))
            ++q;
        const std::string id = e.substr(p, q - p);
        p = q;
        if (id == "OW") return s.OW;
        if (id == "OH") return s.OH;
        if (id == "RW") return s.RW;
        if (id == "RH") return s.RH;
        if (id == "AX") return s.AX;
        if (id == "AY") return s.AY;
        if (id == "AW") return s.AW ? s.AW : s.OW;
        if (id == "AH") return s.AH ? s.AH : s.OH;
        if (!Eat('('))
        {
            ok = false;
            return 0;
        }
        const double a = Sum();
        double b = 1;
        if (id == "align")
        {
            ok = ok && Eat(',');
            b = Sum();
        }
        ok = ok && Eat(')');
        if (id == "ceil") return std::ceil(a);
        if (id == "floor") return std::floor(a);
        if (id == "round") return std::floor(a + 0.5);
        if (id == "align") return std::ceil(a / b) * b;
        ok = false;
        return 0;
    }
};

struct Resource
{
    std::string id;
    bool tex = false;
    uint32_t w = 0, h = 0, slices = 1, dxgi = 0, texel = 0;
    size_t pitch = 0, bytes = 0;          // bytes: the CUDA allocation (pitched for textures)
    std::string init;                     // file, relative to the pack
    CUdeviceptr ptr = 0;
    bool external = false;                // memory supplied by the caller (shared with D3D12)
    size_t Rows() const { return static_cast<size_t>(h) * slices; }
    size_t TightBytes() const { return tex ? static_cast<size_t>(w) * texel * Rows() : bytes; }
};

struct Bind
{
    int slot, res;
    uint32_t fmt;
    int byT[2] = { -1, -1 };              // the multi-frame model's blend: the frame bound while t < 0.5 and from
                                          // t >= 0.5 (res is the one bound now, Pack::SetPass picks it)
};

struct Patch
{
    int reg, dword;
    std::string name;
};

// clear <id> <byte value> [bytes]: the resource's first `bytes` (0: all of it) before the dispatch
struct Clear
{
    int res = -1;
    uint8_t value = 0;
    size_t bytes = 0;
};

struct Dispatch
{
    int kernel = 0;
    unsigned g[3] = { 1, 1, 1 };
    std::string module;                   // from the manifest's trailing comment
    std::vector<Bind> binds;
    std::map<int, std::vector<uint8_t>> cbs;
    std::vector<Patch> patches;
    std::vector<Clear> clears;
    // built
    std::vector<std::vector<uint8_t>> blobs;
    std::vector<void*> args;
    std::map<int, size_t> cbBlob;         // cb register -> index into blobs
    CUgraphNode node = nullptr;
    bool dirty = false;
    CUgraphNode nodeS = nullptr;          // the same dispatch in the head or tail graph
    bool dirtyS = false;
    struct Near { size_t bind, blob; std::vector<uint8_t> desc[2]; };
    std::vector<Near> nears;              // a two-frame bind's descriptor slot for each of its frames
};

// swaprb(u64 p, u32 words): bytes 0 and 2 of each word change places, so a B8G8R8A8 frame reads as the R8G8B8A8
// one the pack was built for (Intel's graph is the same for both, tools/XEFG_SETTINGS.md). Both runners use it.
inline constexpr char kSwapRbPtx[] = R"(.version 7.0
.target sm_75
.address_size 64
.visible .entry swaprb(.param .u64 p, .param .u32 n)
{
    .reg .pred %p;
    .reg .b32 %r<9>;
    .reg .b64 %d<4>;
    ld.param.u64 %d1, [p];
    ld.param.u32 %r1, [n];
    mov.u32 %r3, %ctaid.x;
    mov.u32 %r4, %ntid.x;
    mov.u32 %r5, %tid.x;
    mad.lo.u32 %r3, %r3, %r4, %r5;
    setp.ge.u32 %p, %r3, %r1;
    @%p ret;
    mul.wide.u32 %d2, %r3, 4;
    add.u64 %d3, %d1, %d2;
    ld.global.u32 %r6, [%d3];
    prmt.b32 %r7, %r6, 0, 0x3012;
    st.global.u32 [%d3], %r7;
    ret;
}
)";

struct Kernel
{
    std::string file, module, hash;
    unsigned block[3] = { 8, 8, 1 };
    std::vector<uint8_t> ptx;
    // The pack has no PTX file for this kernel: it is the name of a 'native' line, whose DXBC is all there is (the
    // pack-less pack, which is for the D3D12 path only). Load leaves ptx empty; the CUDA runner refuses such a pack.
    bool nativeOnly = false;
    CUmodule mod = nullptr;
    CUfunction fn = nullptr;
    struct Param { bool cb; unsigned idx, size; };
    std::vector<Param> params;            // after p_desc, in declaration order
};

// The settings a "pack 3" picks its module variants by (tools/XEFG_SETTINGS.md): the init flags and the back
// buffer. hires (motion vectors at output size) follows the sizes and is decided in Load.
struct Variant
{
    bool inv = true, ndc = false, jit = false, premul = true, hdr = false;
};

class Pack
{
public:
    std::string dir, error;
    std::vector<Resource> res;
    std::vector<Kernel> kernels;
    std::vector<Dispatch> disp;
    std::map<std::string, int> roles;     // role -> resource index
    // 'native' lines: a kernel (by PTX file) that the D3D12 runner runs as its native DXIL module (the container,
    // a file of the pack), and the UAV slots that module writes as a raw buffer with a one-texel border.
    struct NativeModule
    {
        std::string file;
        std::set<int> padded;
    };
    std::map<std::string, NativeModule> natives;
    std::string expect;

    explicit Pack(Cuda& c) : cu(c) {}
    ~Pack() { Release(); }
    Pack(const Pack&) = delete;
    Pack& operator=(const Pack&) = delete;

    int Find(const std::string& id) const
    {
        for (size_t i = 0; i < res.size(); ++i)
            if (res[i].id == id)
                return static_cast<int>(i);
        return -1;
    }
    // The bytes a clear covers: its own count, at most the resource's.
    size_t ClearBytes(const Clear& c) const
    {
        return c.bytes && c.bytes < res[c.res].bytes ? c.bytes : res[c.res].bytes;
    }

    // The architecture a PTX file targets: its ".target sm_NN" (0 when it names none).
    static int PtxSm(const std::vector<uint8_t>& ptx)
    {
        static const char kTarget[] = ".target sm_";
        const auto it = std::search(ptx.begin(), ptx.end(), kTarget, kTarget + sizeof(kTarget) - 1);
        if (it == ptx.end())
            return 0;
        int sm = 0;
        for (auto c = it + sizeof(kTarget) - 1; c != ptx.end() && *c >= '0' && *c <= '9'; ++c)
            sm = sm * 10 + (*c - '0');
        return sm;
    }
    int fellBack = 0;                     // kernels Load swapped for their fallback (deviceSm)

    // The oldest GPU (sm_NN) the pack at `dir` and its hdr\ part run on: Load's own kernels with every fallback taken
    // (deviceSm 1), each at its PTX target. kNoSm when a manifest fails to load. Reads the manifests, their constant
    // files and the PTX only; no CUDA calls.
    static constexpr int kNoSm = 1 << 30;
    static int NeedSm(Cuda& cu, const std::string& dir)
    {
        int need = 0;
        for (const std::string& d : { dir, dir + "\\hdr" })
        {
            if (d != dir && Slurp(d + "\\manifest.txt").empty())
                continue;
            Pack p(cu);
            p.deviceSm = 1;
            if (!p.Load(d))
                return kNoSm;
            for (const Kernel& k : p.kernels)
                need = std::max(need, PtxSm(k.ptx));
        }
        return need;
    }

    int Role(const char* r) const
    {
        auto it = roles.find(r);
        return it == roles.end() ? -1 : it->second;
    }

    // The sizes the pack is built for, and (pack 2) the ones it was captured at: init files and the
    // expectation only hold at the latter.
    Sizes size, captured;
    int version = 1;
    // The GPU's compute capability as sm_NN, set before Load: a kernel whose PTX targets a newer one runs its
    // "fallback <kernel> <file> <block x y z>" line's PTX instead (an RTX 20 card: our own kernels at sm_75,
    // mma.m8n8k16, in place of the sm_80 ones; older packs: Intel's translated modules). 0: no check.
    int deviceSm = 0;

    // Parses the manifest and reads every file it names. No CUDA calls. A "pack 2" manifest is evaluated for
    // `want` (its own captured size when null); a "pack 1" manifest has one size only. A "pack 3" is a pack 2
    // whose "variant <kernel> <file> <block x y z> <key>=<0|1>..." lines swap a kernel when every key matches
    // `var` (inv, ndc, jit, premul, hdr) or the sizes (hires: render size = output size); the first match wins.
    bool Load(const std::string& packDir, const Sizes* want = nullptr, const Variant& var = Variant())
    {
        std::vector<bool> swapped;
        std::map<unsigned, Kernel> fallbacks;     // kernel -> its fallback line (deviceSm)
        fellBack = 0;
        dir = packDir;
        // A number, or a {formula} of the sizes.
        auto num = [&](std::istringstream& s, auto& v) {
            std::string t;
            if (!(s >> t))
                return false;
            double d = 0;
            if (t.size() > 1 && t.front() == '{' && t.back() == '}')
            {
                if (!Expr::Eval(t.substr(1, t.size() - 2), size, d))
                {
                    Fail("bad formula " + t);
                    return false;
                }
            }
            else
            {
                try
                {
                    d = std::stod(t);
                }
                catch (...)
                {
                    Fail("bad number " + t);
                    return false;
                }
            }
            // a size or count: finite and not negative (a formula can divide by zero or go below zero)
            if (!std::isfinite(d) || d < 0 || d > 4294967295.0)
            {
                Fail("number out of range: " + t);
                return false;
            }
            v = static_cast<std::remove_reference_t<decltype(v)>>(std::floor(d + 1e-6));
            return true;
        };
        const std::vector<uint8_t> manifest = Slurp(dir + "\\manifest.txt");
        if (manifest.empty())
            return Fail("no manifest.txt in " + dir);
        std::istringstream f(std::string(manifest.begin(), manifest.end()));
        std::string line;
        Dispatch* cur = nullptr;
        int lineNo = 0;
        while (std::getline(f, line))
        {
            ++lineNo;
            std::string comment;
            if (const size_t hash = line.find('#'); hash != std::string::npos)
            {
                comment = line.substr(hash + 1);
                line.resize(hash);
            }
            std::istringstream s(line);
            std::string op;
            if (!(s >> op))
                continue;
            if (op == "pack")
            {
                s >> version;
                if (version < 1 || version > 3)
                    return Fail("unsupported pack version");
            }
            else if (op == "size")
            {
                s >> captured.OW >> captured.OH >> captured.RW >> captured.RH;
                size = want ? *want : captured;
                if (!size.OW || !size.OH || !size.RW || !size.RH)       // an area alone (--area without --size): the captured size
                    size.OW = captured.OW, size.OH = captured.OH, size.RW = captured.RW, size.RH = captured.RH;
                if (!size.AW || !size.AH)
                    size.AX = size.AY = size.AW = size.AH = 0;
                else if (size.AX + static_cast<uint64_t>(size.AW) > size.OW || size.AY + static_cast<uint64_t>(size.AH) > size.OH)
                    return Fail("interpolation area " + std::to_string(size.AX) + "," + std::to_string(size.AY) + " " + std::to_string(size.AW) +
                                "x" + std::to_string(size.AH) + " outside the " + std::to_string(size.OW) + "x" + std::to_string(size.OH) + " output");
            }
            else if (op == "resource")
            {
                Resource r;
                std::string kind;
                s >> r.id >> kind;
                if (kind == "raw")
                {
                    if (!num(s, r.bytes))
                        return Fail("line " + std::to_string(lineNo) + ": bad size");
                }
                else if (kind == "tex")
                {
                    r.tex = true;
                    if (!num(s, r.w) || !num(s, r.h) || !num(s, r.slices))
                        return Fail("line " + std::to_string(lineNo) + ": bad size");
                    s >> r.dxgi >> r.texel;
                    r.pitch = (static_cast<size_t>(r.w) * r.texel + 255) & ~size_t(255);
                    r.bytes = r.pitch * r.Rows();
                }
                else
                    return Fail("line " + std::to_string(lineNo) + ": unknown resource kind " + kind);
                std::string kw;
                if (s >> kw && kw == "init")
                    s >> r.init;
                res.push_back(r);
            }
            else if (op == "role")
            {
                std::string role, id;
                s >> role >> id;
                const int i = Find(id);
                if (i < 0)
                    return Fail("role " + role + ": unknown resource " + id);
                roles[role] = i;
            }
            else if (op == "kernel")
            {
                Kernel k;
                unsigned idx = 0;
                s >> idx >> k.file >> k.block[0] >> k.block[1] >> k.block[2];
                std::string kw;
                while (s >> kw)
                    if (kw == "hash")
                        s >> k.hash;
                // the module name is the file's suffix: k<nn>_<module>.ptx
                if (const size_t u = k.file.find('_'); u != std::string::npos)
                    k.module = k.file.substr(u + 1, k.file.size() - u - 5);
                if (idx != kernels.size())
                    return Fail("kernels out of order");
                kernels.push_back(k);
                swapped.push_back(false);
            }
            else if (op == "native")
            {
                // native <PTX file> <DXIL container> [slot,slot...]: see Pack::natives
                std::string ptx, slots;
                NativeModule m;
                s >> ptx >> m.file >> slots;
                for (size_t p = 0; p < slots.size();)
                {
                    m.padded.insert(atoi(slots.c_str() + p));
                    p = slots.find(',', p);
                    p = p == std::string::npos ? slots.size() : p + 1;
                }
                if (ptx.empty() || m.file.empty())
                    return Fail("line " + std::to_string(lineNo) + ": native <PTX file> <container> [slots]");
                natives[ptx] = m;
            }
            else if (op == "variant")
            {
                Kernel k;
                unsigned idx = 0;
                s >> idx >> k.file >> k.block[0] >> k.block[1] >> k.block[2];
                if (idx >= kernels.size())
                    return Fail("line " + std::to_string(lineNo) + ": variant of an unknown kernel");
                const bool hires = size.RW == size.OW && size.RH == size.OH;
                bool match = !swapped[idx];
                std::string kv;
                while (match && s >> kv)
                {
                    const size_t eq = kv.find('=');
                    const std::string key = kv.substr(0, eq);
                    const bool want1 = eq != std::string::npos && kv.substr(eq + 1) == "1";
                    const bool have = key == "inv" ? var.inv : key == "ndc" ? var.ndc : key == "jit" ? var.jit : key == "premul" ? var.premul
                                    : key == "hdr" ? var.hdr : key == "hires" ? hires : !want1;
                    match = have == want1;
                }
                if (match)
                {
                    if (const size_t u = k.file.find('_'); u != std::string::npos)
                        k.module = k.file.substr(u + 1, k.file.size() - u - 5);
                    kernels[idx] = k;
                    swapped[idx] = true;
                }
            }
            else if (op == "dispatch")
            {
                Dispatch d;
                s >> d.kernel;
                if (!num(s, d.g[0]) || !num(s, d.g[1]) || !num(s, d.g[2]))
                    return Fail("line " + std::to_string(lineNo) + ": bad grid");
                std::istringstream c(comment);
                int n = 0;
                c >> n >> d.module;
                if (d.kernel < 0 || d.kernel >= static_cast<int>(kernels.size()))
                    return Fail("dispatch with an unknown kernel");
                disp.push_back(d);
                cur = &disp.back();
            }
            else if (op == "bind" || op == "cb" || op == "patch" || op == "clear" || op == "set")
            {
                if (!cur)
                    return Fail(op + " before the first dispatch");
                if (op == "bind")
                {
                    Bind b {};
                    std::string id;
                    s >> b.slot >> id >> b.fmt;
                    b.res = Find(id);
                    if (b.res < 0)
                        return Fail("bind: unknown resource " + id);
                    // bind <slot> <id> <fmt> <id from t >= 0.5>: the frame nearest the generated one
                    if (std::string late; s >> late)
                    {
                        b.byT[0] = b.res;
                        b.byT[1] = Find(late);
                        if (b.byT[1] < 0)
                            return Fail("bind: unknown resource " + late);
                        // one view serves both (the D3D12 runner builds it from the first): same kind, format and size
                        const Resource &r0 = res[b.byT[0]], &r1 = res[b.byT[1]];
                        if (r0.tex != r1.tex || r0.dxgi != r1.dxgi || r0.texel != r1.texel || r0.w != r1.w || r0.h != r1.h || r0.slices != r1.slices)
                            return Fail("bind: " + id + " and " + late + " differ in kind, format or size");
                    }
                    cur->binds.push_back(b);
                }
                else if (op == "cb")
                {
                    int reg = 0;
                    std::string file;
                    s >> reg >> file;
                    auto data = Slurp(dir + "\\" + file);
                    if (data.empty())
                        return Fail("cannot read " + file);
                    cur->cbs[reg] = std::move(data);
                }
                else if (op == "set")
                {
                    // set <reg> <dword> <i|f> {formula}: a size-dependent root-constant dword.
                    int reg = 0, dword = 0;
                    std::string type, t;
                    s >> reg >> dword >> type >> t;
                    double d = 0;
                    if (t.size() < 2 || !Expr::Eval(t.substr(1, t.size() - 2), size, d) || !std::isfinite(d))
                        return Fail("line " + std::to_string(lineNo) + ": bad formula " + t);
                    if (dword < 0 || dword >= 64)
                        return Fail("line " + std::to_string(lineNo) + ": root-constant dword " + std::to_string(dword) + " out of range");
                    auto& b = cur->cbs[reg];
                    if (b.size() < (static_cast<size_t>(dword) + 1) * 4)
                        b.resize((static_cast<size_t>(dword) + 1) * 4);
                    uint32_t u;
                    if (type == "f")
                    {
                        const float fl = static_cast<float>(d);
                        memcpy(&u, &fl, 4);
                    }
                    else
                        u = static_cast<uint32_t>(static_cast<int64_t>(std::floor(d + 1e-6)));
                    memcpy(b.data() + static_cast<size_t>(dword) * 4, &u, 4);
                }
                else if (op == "patch")
                {
                    Patch p;
                    s >> p.reg >> p.dword >> p.name;
                    cur->patches.push_back(p);
                }
                else
                {
                    std::string id;
                    int v = 0;
                    s >> id >> v;
                    Clear c;
                    c.res = Find(id);
                    if (c.res < 0)
                        return Fail("clear: unknown resource " + id);
                    c.value = static_cast<uint8_t>(v);
                    if (!num(s, c.bytes) && !error.empty())
                        return false;
                    cur->clears.push_back(c);
                }
            }
            else if (op == "fallback")
            {
                unsigned idx = 0;
                Kernel fb;
                s >> idx >> fb.file >> fb.block[0] >> fb.block[1] >> fb.block[2];
                if (idx >= kernels.size() || fb.file.empty())
                    return Fail("fallback for an unknown kernel");
                if (const size_t u = fb.file.find('_'); u != std::string::npos)
                    fb.module = fb.file.substr(u + 1, fb.file.size() - u - 5);
                fallbacks[idx] = fb;
            }
            else if (op == "expect")
                s >> expect;
            else
                return Fail("line " + std::to_string(lineNo) + ": unknown directive " + op);
            if (!error.empty())
                return false;
        }
        if (version >= 2 && size != captured)
            expect.clear();         // the expectation holds at the captured size only
        for (const auto& [idx, fb] : fallbacks)
            if (deviceSm && PtxSm(Slurp(dir + "\\" + kernels[idx].file)) > deviceSm)
            {
                kernels[idx].file = fb.file;
                kernels[idx].module = fb.module;
                std::copy(fb.block, fb.block + 3, kernels[idx].block);
                ++fellBack;
            }
        for (Kernel& k : kernels)
        {
            k.ptx = Slurp(dir + "\\" + k.file);
            if (k.ptx.empty())
            {
                // the PTX twin of a native module may be left out of the pack (pack-less: the D3D12 path runs the DXBC)
                if (!natives.count(k.file))
                    return Fail("cannot read " + k.file);
                k.nativeOnly = true;
            }
            k.ptx.push_back(0);
            // Parameters after p_desc, in the order dxil2ptx.py declares them.
            const char* q = reinterpret_cast<const char*>(k.ptx.data());
            while ((q = strstr(q, ".b8 p_")) != nullptr)
            {
                unsigned idx = 0, bytes = 0;
                if (sscanf_s(q, ".b8 p_s%u[%u]", &idx, &bytes) == 2)
                    k.params.push_back({ false, idx, bytes });
                else if (sscanf_s(q, ".b8 p_cb%u[%u]", &idx, &bytes) == 2)
                    k.params.push_back({ true, idx, bytes });
                q += 6;
            }
        }
        return true;
    }

    // Fills a resource with tightly packed contents (after Build(); offline checks).
    bool Fill(int r, const std::vector<uint8_t>& tight)
    {
        return r >= 0 && r < static_cast<int>(res.size()) && tight.size() == res[r].TightBytes() && Upload(res[r], tight.data());
    }

    // Caller-owned memory for a resource (a D3D12 shared buffer mapped into CUDA). Before Build().
    bool SetExternal(int r, CUdeviceptr p)
    {
        if (r < 0 || r >= static_cast<int>(res.size()))
            return false;
        res[r].ptr = p;
        res[r].external = true;
        return true;
    }

    // Allocates and fills every resource that is not external, loads the kernels, makes the texture
    // objects and builds and instantiates the graph. Needs a current CUDA context.
    bool Build(bool uploadExternalInit = false)
    {
        CUresult r;
        for (Resource& x : res)
        {
            if (!x.external)
            {
                if ((r = cu.MemAlloc(&x.ptr, (x.bytes + 15) & ~size_t(15))))
                    return Fail(std::string("cuMemAlloc: ") + cu.Err(r));
                cu.MemsetD8(x.ptr, 0, x.bytes);
                owned.push_back(x.ptr);
            }
            if (x.init.empty() || (x.external && !uploadExternalInit))
                continue;
            auto data = Slurp(dir + "\\" + x.init);
            if (data.size() != x.TightBytes() && version >= 2 && size != captured)
                continue;           // the captured frames of a pack evaluated at another size: left zero
            if (data.empty() && std::any_of(roles.begin(), roles.end(), [&](const auto& ro) {
                    return &res[ro.second] == &x && ro.first != "noise";
                }))
                continue;           // a per-frame input whose capture is not in the pack (pack_embed.py leaves it out)
            if (data.size() != x.TightBytes())
                return Fail(x.init + ": " + std::to_string(data.size()) + " bytes, the resource needs " +
                            std::to_string(x.TightBytes()));
            if (!Upload(x, data.data()))
                return false;
        }
        CUdeviceptr dummy = 0;
        if ((r = cu.MemAlloc(&dummy, 256)))
            return Fail(std::string("cuMemAlloc: ") + cu.Err(r));
        owned.push_back(dummy);
        descTable = dummy;

        static char jitLog[16384];
        for (Kernel& k : kernels)
        {
            if (k.nativeOnly)
                return Fail(k.file + ": this pack has only the native module of this kernel (D3D12 path only)");
            int opts[] = { 5, 6 };      // CU_JIT_ERROR_LOG_BUFFER, _SIZE_BYTES
            void* vals[] = { jitLog, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(jitLog))) };
            jitLog[0] = 0;
            if ((r = cu.ModuleLoadDataEx(&k.mod, k.ptx.data(), 2, opts, vals)))
                return Fail(k.file + ": cuModuleLoadDataEx " + cu.Err(r) + " " + jitLog);
            if ((r = cu.ModuleGetFunction(&k.fn, k.mod, "dxil_main")))
                return Fail(k.file + ": cuModuleGetFunction " + cu.Err(r));
        }

        // Kernel arguments per dispatch.
        if (!PrepareArgs([this](const Bind& b, uint8_t* s) { return Descriptor(b, s); }))
            return false;
        for (Dispatch& d : disp)
        {
            d.args.clear();
            d.args.push_back(&descTable);
            for (auto& b : d.blobs)
                d.args.push_back(b.data());
        }

        // The graph: a chain in dispatch order, as D3D's UAV barriers order them.
        if ((r = cu.GraphCreate(&graph, 0)))
            return Fail(std::string("cuGraphCreate: ") + cu.Err(r));
        CUgraphNode prev = nullptr;
        for (Dispatch& d : disp)
        {
            for (const Clear& c : d.clears)
            {
                MemsetNodeParams mp {};
                mp.dst = res[c.res].ptr;
                mp.value = c.value;
                mp.elementSize = 1;
                mp.width = ClearBytes(c);
                mp.height = 1;
                mp.pitch = mp.width;
                CUgraphNode n = nullptr;
                if ((r = cu.GraphAddMemsetNode(&n, graph, prev ? &prev : nullptr, prev ? 1 : 0, &mp, CurrentCtx())))
                    return Fail(std::string("cuGraphAddMemsetNode: ") + cu.Err(r));
                prev = n;
            }
            const KernelNodeParams kp = NodeParams(d);
            if ((r = cu.GraphAddKernelNode(&d.node, graph, prev ? &prev : nullptr, prev ? 1 : 0, &kp)))
                return Fail(d.module + ": cuGraphAddKernelNode " + cu.Err(r));
            prev = d.node;
        }
        if ((r = cu.GraphInstantiate(&exec, graph, 0)))
            return Fail(std::string("cuGraphInstantiate: ") + cu.Err(r));
        if (!BuildSplit())
            return false;
        return true;
    }

    // Each dispatch's argument blobs after p_desc: its constant buffers, and one 48-byte descriptor slot per
    // bound resource, which `desc` fills (the CUDA runner here, or the D3D12 one in xefg_pack_d3d.h).
    template <class DescFn>
    bool PrepareArgs(const DescFn& desc)
    {
        for (Dispatch& d : disp)
        {
            const Kernel& k = kernels[d.kernel];
            d.blobs.clear();
            d.blobs.reserve(k.params.size());
            d.cbBlob.clear();
            d.nears.clear();
            for (const Kernel::Param& p : k.params)
            {
                std::vector<uint8_t> b(p.size, 0);
                if (p.cb)
                {
                    if (auto it = d.cbs.find(p.idx); it != d.cbs.end())
                        memcpy(b.data(), it->second.data(), std::min<size_t>(it->second.size(), b.size()));
                    d.cbBlob[p.idx] = d.blobs.size();
                }
                else
                {
                    for (size_t bi = 0; bi < d.binds.size(); ++bi)
                        if (const Bind& bd = d.binds[bi]; bd.slot == static_cast<int>(p.idx))
                        {
                            uint8_t slot[48] = {};
                            if (!desc(bd, slot))
                                return false;
                            memcpy(b.data(), slot, std::min<size_t>(48, b.size()));
                            if (bd.byT[1] < 0)
                                continue;
                            Dispatch::Near n { bi, d.blobs.size() };
                            for (int j = 0; j < 2; ++j)
                            {
                                Bind one = bd;
                                one.res = bd.byT[j];
                                uint8_t s2[48] = {};
                                if (!desc(one, s2))
                                    return false;
                                n.desc[j].assign(s2, s2 + std::min<size_t>(48, b.size()));
                            }
                            d.nears.push_back(std::move(n));
                        }
                }
                d.blobs.push_back(std::move(b));
            }
        }
        return true;
    }

    // The first dispatch with a per-pass patch: [0, split) runs once per app frame, the rest once per
    // generated frame. disp.size() when the pack has no per-pass patches.
    size_t FindSplit() const
    {
        if (!HasPassPatches())
            return disp.size();
        for (size_t i = 0; i < disp.size(); ++i)
            for (const Patch& p : disp[i].patches)
                if (p.name == "interp_t" || p.name == "flow_t" || p.name == "mv_scale_x" || p.name == "mv_scale_y" ||
                    p.name == "pass_index" || p.name == "splat_t")
                    return i;
        return disp.size();
    }

    // Overwrites dwords of a dispatch's constant buffer; applied at the next Launch().
    bool SetConstants(size_t di, int reg, const uint32_t* v, size_t first, size_t count)
    {
        if (di >= disp.size())
            return false;
        Dispatch& d = disp[di];
        auto it = d.cbBlob.find(reg);
        if (it == d.cbBlob.end())
            return false;
        std::vector<uint8_t>& b = d.blobs[it->second];
        const size_t end = std::min(b.size() / 4, first + count);
        for (size_t i = first; i < end; ++i)
        {
            uint32_t old;
            memcpy(&old, b.data() + i * 4, 4);
            if (old != v[i - first])
            {
                memcpy(b.data() + i * 4, &v[i - first], 4);
                d.dirty = true;
                d.dirtyS = true;
            }
        }
        return true;
    }

    // A named per-frame value (manifest 'patch' lines), e.g. jitter_cur_x.
    bool HasPatch(const char* name) const
    {
        for (const Dispatch& d : disp)
            for (const Patch& p : d.patches)
                if (p.name == name)
                    return true;
        return false;
    }

    void SetPatch(const char* name, float value)
    {
        uint32_t u;
        memcpy(&u, &value, 4);
        SetPatchBits(name, u);
    }
    void SetPatchBits(const char* name, uint32_t u)
    {
        for (size_t i = 0; i < disp.size(); ++i)
            for (const Patch& p : disp[i].patches)
                if (p.name == name)
                    SetConstants(i, p.reg, &u, p.dword, 1);
    }
    // The captured value of a patch (as the manifest's constants hold it, whatever has been set since), as a float.
    bool PatchValue(const char* name, float& v) const
    {
        for (const Dispatch& d : disp)
            for (const Patch& p : d.patches)
                if (p.name == name)
                    if (auto it = d.cbs.find(p.reg); it != d.cbs.end() && it->second.size() >= (p.dword + 1u) * 4)
                    {
                        memcpy(&v, it->second.data() + p.dword * 4, 4);
                        return true;
                    }
        return false;
    }

    // The motion-vector scale the per-pass constants now hold: mv_scale = scale * (1 - t) at the t in interp_t (the
    // capture's until a SetPass: 0.5 for the 2x model, the first generated frame's for the multi-frame model).
    bool MvScale(float& x, float& y) const
    {
        float t = 0.5f;
        if (!PatchValue("mv_scale_x", x) || !PatchValue("mv_scale_y", y) || (PatchValue("interp_t", t) && !(t < 1.0f)))
            return false;
        x /= 1.0f - t;
        y /= 1.0f - t;
        return true;
    }

    // One generated frame's constants, as Intel's libxess_fg sets them (tools/pack_patches.py): t = k / (n + 1)
    // from the previous frame, the flow network's quantised t, the motion-vector scale times (1 - t), and the
    // pass index; for the multi-frame model (tools/build_i0_pack.py) also t / 4 in its per-t splat and the
    // frame nearest t in its blend. A pack without a patch keeps the captured value for it.
    void SetPass(float t, uint32_t pass, float mvScaleX, float mvScaleY)
    {
        SetPatch("interp_t", t);
        SetPatchBits("flow_t", static_cast<uint32_t>(63 + static_cast<int>(roundf((t - 0.5f) * 126.0f))));
        SetPatch("mv_scale_x", mvScaleX * (1.0f - t));
        SetPatch("mv_scale_y", mvScaleY * (1.0f - t));
        SetPatchBits("pass_index", pass);
        SetPatch("splat_t", t * 0.25f);
        const int late = t >= 0.5f ? 1 : 0;
        for (Dispatch& d : disp)
            for (size_t bi = 0; bi < d.binds.size(); ++bi)
                if (Bind& b = d.binds[bi]; b.byT[1] >= 0 && b.res != b.byT[late])
                {
                    b.res = b.byT[late];
                    for (const Dispatch::Near& n : d.nears)
                        if (n.bind == bi)
                            memcpy(d.blobs[n.blob].data(), n.desc[late].data(), n.desc[late].size());
                    d.dirty = d.dirtyS = true;
                }
    }
    // N-x needs every per-pass constant; t alone (an older pack) is not enough. The multi-frame model runs
    // its flow networks once per app frame (no flow_t) and splats the flow per t instead.
    bool HasPassPatches() const
    {
        return HasPatch("interp_t") && (HasPatch("flow_t") || HasPatch("splat_t")) && HasPatch("mv_scale_x") && HasPatch("pass_index");
    }

    // The same frame as two graphs: the head (everything before the first dispatch with a per-pass patch,
    // in practice the input preparation) depends only on the app's frames, so it runs once per app frame;
    // the tail runs once per generated frame with that frame's constants, as Intel does.
    bool CanSplit() const { return execHead && execTail; }
    bool LaunchHead(CUstream s) { return LaunchPart(execHead, 0, splitAt, s); }
    bool LaunchTail(CUstream s) { return LaunchPart(execTail, splitAt, disp.size(), s); }

    // R <-> B in every texel of resource r on stream s (B8G8R8A8 back buffers; xefg_pack_d3d.h SwapRB on D3D12).
    bool SwapRB(CUstream s, int r)
    {
        CUresult e;
        if (!swapFn && ((e = cu.ModuleLoadDataEx(&swapMod, kSwapRbPtx, 0, nullptr, nullptr)) || (e = cu.ModuleGetFunction(&swapFn, swapMod, "swaprb"))))
            return Fail(std::string("swaprb: ") + cu.Err(e));
        CUdeviceptr p = res[r].ptr;
        unsigned n = static_cast<unsigned>(res[r].bytes / 4);
        void* args[] = { &p, &n };
        if ((e = cu.LaunchKernel(swapFn, (n + 255) / 256, 1, 1, 256, 1, 1, 0, s, args, nullptr)))
            return Fail(std::string("swaprb launch: ") + cu.Err(e));
        return true;
    }

    bool Launch(CUstream stream)
    {
        CUresult r;
        LARGE_INTEGER q0, q1, q2;
        QueryPerformanceCounter(&q0);
        for (Dispatch& d : disp)
            if (d.dirty)
            {
                const KernelNodeParams kp = NodeParams(d);
                if ((r = cu.GraphExecKernelNodeSetParams(exec, d.node, &kp)))
                    return Fail(d.module + ": cuGraphExecKernelNodeSetParams " + cu.Err(r));
                d.dirty = false;
                ++lastDirty;
            }
        QueryPerformanceCounter(&q1);
        if ((r = cu.GraphLaunch(exec, stream)))
            return Fail(std::string("cuGraphLaunch: ") + cu.Err(r));
        QueryPerformanceCounter(&q2);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        tSetParams += static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        tGraphLaunch += static_cast<double>(q2.QuadPart - q1.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        return true;
    }
    // Launch timing, ms summed since the caller last cleared them (diagnostics).
    double tSetParams = 0, tGraphLaunch = 0;
    unsigned lastDirty = 0;

    // Tightly packed contents of a resource (offline checks); from: a copy of it in another buffer of its layout.
    bool Download(int ri, std::vector<uint8_t>& out, CUdeviceptr from = 0)
    {
        const Resource& x = res[ri];
        std::vector<uint8_t> raw(x.bytes);
        if (CUresult r = cu.MemcpyDtoH(raw.data(), from ? from : x.ptr, x.bytes))
            return Fail(std::string("cuMemcpyDtoH: ") + cu.Err(r));
        if (!x.tex)
        {
            out = std::move(raw);
            return true;
        }
        const size_t tight = static_cast<size_t>(x.w) * x.texel;
        out.resize(tight * x.Rows());
        for (size_t y = 0; y < x.Rows(); ++y)
            memcpy(out.data() + y * tight, raw.data() + y * x.pitch, tight);
        return true;
    }

    void Release()
    {
        if (execHead)
            cu.GraphExecDestroy(execHead);
        if (execTail)
            cu.GraphExecDestroy(execTail);
        if (gHead)
            cu.GraphDestroy(gHead);
        if (gTail)
            cu.GraphDestroy(gTail);
        execHead = execTail = nullptr;
        gHead = gTail = nullptr;
        if (exec)
            cu.GraphExecDestroy(exec);
        if (graph)
            cu.GraphDestroy(graph);
        exec = nullptr;
        graph = nullptr;
        for (CUtexObject t : texObjs)
            cu.TexObjectDestroy(t);
        texObjs.clear();
        for (Kernel& k : kernels)
            if (k.mod)
            {
                cu.ModuleUnload(k.mod);
                k.mod = nullptr;
            }
        if (swapMod)
            cu.ModuleUnload(swapMod);
        swapMod = nullptr;
        swapFn = nullptr;
        for (CUdeviceptr p : owned)
            cu.MemFree(p);
        owned.clear();
    }

private:
    Cuda& cu;
    CUmodule swapMod = nullptr;
    CUfunction swapFn = nullptr;
    CUgraph graph = nullptr;
    CUgraphExec exec = nullptr;
    CUgraph gHead = nullptr, gTail = nullptr;
    CUgraphExec execHead = nullptr, execTail = nullptr;
    size_t splitAt = 0;
    CUdeviceptr descTable = 0;
    std::vector<CUdeviceptr> owned;
    std::vector<CUtexObject> texObjs;
    std::map<std::pair<int, uint32_t>, std::pair<CUtexObject, CUtexObject>> texCache;

    bool Fail(const std::string& e)
    {
        error = e;
        return false;
    }

    // One chain of dispatches [from, to) with their clears, as its own graph.
    bool BuildChain(CUgraph& g, CUgraphExec& e, size_t from, size_t to)
    {
        CUresult r;
        if ((r = cu.GraphCreate(&g, 0)))
            return Fail(std::string("cuGraphCreate: ") + cu.Err(r));
        CUgraphNode prev = nullptr;
        for (size_t i = from; i < to; ++i)
        {
            Dispatch& d = disp[i];
            for (const Clear& c : d.clears)
            {
                MemsetNodeParams mp {};
                mp.dst = res[c.res].ptr;
                mp.value = c.value;
                mp.elementSize = 1;
                mp.width = ClearBytes(c);
                mp.height = 1;
                mp.pitch = mp.width;
                CUgraphNode n = nullptr;
                if ((r = cu.GraphAddMemsetNode(&n, g, prev ? &prev : nullptr, prev ? 1 : 0, &mp, CurrentCtx())))
                    return Fail(std::string("cuGraphAddMemsetNode: ") + cu.Err(r));
                prev = n;
            }
            const KernelNodeParams kp = NodeParams(d);
            if ((r = cu.GraphAddKernelNode(&d.nodeS, g, prev ? &prev : nullptr, prev ? 1 : 0, &kp)))
                return Fail(d.module + ": cuGraphAddKernelNode " + cu.Err(r));
            d.dirtyS = false;
            prev = d.nodeS;
        }
        if ((r = cu.GraphInstantiate(&e, g, 0)))
            return Fail(std::string("cuGraphInstantiate: ") + cu.Err(r));
        return true;
    }

    bool BuildSplit()
    {
        splitAt = FindSplit();
        if (splitAt == 0 || splitAt >= disp.size())
            return true;
        return BuildChain(gHead, execHead, 0, splitAt) && BuildChain(gTail, execTail, splitAt, disp.size());
    }

    bool LaunchPart(CUgraphExec e, size_t from, size_t to, CUstream stream)
    {
        CUresult r;
        LARGE_INTEGER q0, q1, q2;
        QueryPerformanceCounter(&q0);
        for (size_t i = from; i < to; ++i)
            if (disp[i].dirtyS)
            {
                const KernelNodeParams kp = NodeParams(disp[i]);
                if ((r = cu.GraphExecKernelNodeSetParams(e, disp[i].nodeS, &kp)))
                    return Fail(disp[i].module + ": cuGraphExecKernelNodeSetParams " + cu.Err(r));
                disp[i].dirtyS = false;
                ++lastDirty;
            }
        QueryPerformanceCounter(&q1);
        if ((r = cu.GraphLaunch(e, stream)))
            return Fail(std::string("cuGraphLaunch: ") + cu.Err(r));
        QueryPerformanceCounter(&q2);
        LARGE_INTEGER f;
        QueryPerformanceFrequency(&f);
        tSetParams += static_cast<double>(q1.QuadPart - q0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        tGraphLaunch += static_cast<double>(q2.QuadPart - q1.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        return true;
    }

    CUcontext CurrentCtx()
    {
        CUcontext c = nullptr;
        if (cu.CtxPopCurrent(&c) == 0 && c)
            cu.CtxPushCurrent(c);
        return c;
    }

    bool Upload(const Resource& x, const uint8_t* tight)
    {
        CUresult r;
        if (!x.tex)
            r = cu.MemcpyHtoD(x.ptr, tight, x.bytes);
        else
        {
            const size_t row = static_cast<size_t>(x.w) * x.texel;
            std::vector<uint8_t> staged(x.bytes, 0);
            for (size_t y = 0; y < x.Rows(); ++y)
                memcpy(staged.data() + y * x.pitch, tight + y * row, row);
            r = cu.MemcpyHtoD(x.ptr, staged.data(), x.bytes);
        }
        return r == 0 || Fail(std::string("cuMemcpyHtoD: ") + cu.Err(r));
    }

    KernelNodeParams NodeParams(Dispatch& d)
    {
        const Kernel& k = kernels[d.kernel];
        KernelNodeParams kp {};
        kp.func = k.fn;
        kp.gridX = d.g[0];
        kp.gridY = d.g[1];
        kp.gridZ = d.g[2];
        kp.blockX = k.block[0];
        kp.blockY = k.block[1];
        kp.blockZ = k.block[2];
        kp.params = d.args.data();
        return kp;
    }

    // One 48-byte descriptor-table slot, exactly as generic_check.cpp fills it: u64 pointer, u64
    // linear texture object, u32 width, height, slices, bytes per texel, total bytes, pitch, u64 point
    // texture object. Sampled SRV textures get both texture objects over the pitched memory itself.
    bool Descriptor(const Bind& b, uint8_t* s)
    {
        const Resource& x = res[b.res];
        const uint32_t tb = x.tex ? (TexelBytes(b.fmt) ? TexelBytes(b.fmt) : x.texel) : 0;
        uint64_t tex = 0, texPoint = 0;
        int cuFmt = 0;
        unsigned cuCh = 0;
        if (x.tex && b.slot < 32 && x.slices == 1 && TexFormat(b.fmt, &cuFmt, &cuCh))
        {
            auto key = std::make_pair(b.res, b.fmt);
            auto it = texCache.find(key);
            if (it == texCache.end())
            {
                ResDesc rd {};
                rd.resType = 3;         // CU_RESOURCE_TYPE_PITCH2D
                rd.devPtr = x.ptr;
                rd.format = cuFmt;
                rd.numChannels = cuCh;
                rd.width = x.w;
                rd.height = x.h;
                rd.pitchInBytes = x.pitch;
                TexDesc td {};
                td.addressMode[0] = td.addressMode[1] = td.addressMode[2] = 1;   // clamp
                td.filterMode = 1;                                               // linear
                td.flags = 2;                                                    // normalised coords
                CUtexObject lin = 0, pt = 0;
                CUresult r;
                if ((r = cu.TexObjectCreate(&lin, &rd, &td, nullptr)))
                    return Fail(x.id + ": cuTexObjectCreate " + cu.Err(r));
                TexDesc tp {};
                tp.addressMode[0] = tp.addressMode[1] = tp.addressMode[2] = 3;   // border, colour 0
                if ((r = cu.TexObjectCreate(&pt, &rd, &tp, nullptr)))
                    return Fail(x.id + ": cuTexObjectCreate (point) " + cu.Err(r));
                texObjs.push_back(lin);
                texObjs.push_back(pt);
                it = texCache.emplace(key, std::make_pair(lin, pt)).first;
            }
            tex = it->second.first;
            texPoint = it->second.second;
        }
        const uint32_t dims[6] = { x.tex ? x.w : 0, x.tex ? x.h : 0, x.tex ? x.slices : 1, tb,
                                   static_cast<uint32_t>(x.bytes), static_cast<uint32_t>(x.pitch) };
        memcpy(s, &x.ptr, 8);
        memcpy(s + 8, &tex, 8);
        memcpy(s + 16, dims, 24);
        memcpy(s + 40, &texPoint, 8);
        return true;
    }
};

}  // namespace xefgpack
