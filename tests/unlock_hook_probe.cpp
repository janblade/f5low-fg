// Probe for the RTX 20 CUDA module-load hook of src/unlock/Unlock.h, without DLSS-G: maps the player's nvngx_dlssg.dll (path = argument;
// nothing is run from it), installs the same hooks f5low does (HookCudaLoaders) and then loads
//   1. a tiny PTX module of our own through the nvcuda exports  -> must pass straight through (not one of the DLL's containers), and
//   2. every CUDA fatbin container of the DLL, through the same exports -> every one must be rewritten to sm_75 PTX by the hook and
//      accepted by this PC's driver (JIT),
//   3. the same through the pointer cuGetProcAddress hands out (what the CUDA runtime uses; it bypasses the export stub).
// Prints the hook's counters. Exit 0 when all containers loaded and were rewritten.
// usage: unlock_hook_probe <nvngx_dlssg.dll>
#include "../src/unlock/Unlock.h"

#include <cstdio>

namespace
{
using CUresult = int;
using FnInit = CUresult(WINAPI*)(unsigned);
using FnDeviceGet = CUresult(WINAPI*)(int*, int);
using FnCtxCreate = CUresult(WINAPI*)(void**, unsigned, int);
using FnLoad = CUresult(WINAPI*)(void**, const void*);
using FnUnload = CUresult(WINAPI*)(void*);

void LogLine(const char* fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    std::vprintf(fmt, ap);
    std::printf("\n");
    va_end(ap);
}
} // namespace

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        std::fputs("usage: unlock_hook_probe <nvngx_dlssg.dll>\n", stderr);
        return 2;
    }
    unlock::g_log = LogLine;
    HMODULE ngx = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE cuda = LoadLibraryW(L"nvcuda.dll");
    if (!ngx || !cuda)
    {
        std::fputs("cannot load the DLL or nvcuda\n", stderr);
        return 2;
    }
    const auto init = reinterpret_cast<FnInit>(GetProcAddress(cuda, "cuInit"));
    const auto devGet = reinterpret_cast<FnDeviceGet>(GetProcAddress(cuda, "cuDeviceGet"));
    const auto ctxCreate = reinterpret_cast<FnCtxCreate>(GetProcAddress(cuda, "cuCtxCreate_v2"));
    const auto unload = reinterpret_cast<FnUnload>(GetProcAddress(cuda, "cuModuleUnload"));
    int dev = 0;
    void* ctx = nullptr;
    if (init(0) != 0 || devGet(&dev, 0) != 0 || ctxCreate(&ctx, 0, dev) != 0)
    {
        std::fputs("no CUDA device\n", stderr);
        return 2;
    }

    unlock::ourBase = reinterpret_cast<uintptr_t>(ngx);
    unlock::ourEnd = unlock::ourBase + unlock::GetMappedImageSize(ngx);
    unlock::turingRewriteWanted.store(true);
    std::string why;
    if (!unlock::HookCudaLoaders(why))
    {
        std::printf("HOOK FAILED: %s\n", why.c_str());
        return 1;
    }
    std::puts("hooked: the four module-load functions of nvcuda (behind their export stubs) are hooked");

    // Where does cuGetProcAddress (what the CUDA runtime uses) point for the four names: at the export stub (then the hooked slot is
    // behind it) or straight at the real function (then it bypasses the slot)? Printed so a run on a new driver shows which.
    using FnGetProc = CUresult(WINAPI*)(const char*, void**, int, unsigned long long);
    FnLoad viaGetLoad = nullptr;
    if (const auto gp = reinterpret_cast<FnGetProc>(GetProcAddress(cuda, "cuGetProcAddress")))
        for (const char* n : { "cuModuleLoadData", "cuModuleLoadFatBinary", "cuModuleLoadDataEx", "cuLibraryLoadData" })
        {
            void* viaGet = nullptr;
            const CUresult r = gp(n, &viaGet, 12000, 0);
            std::printf("cuGetProcAddress(%s) -> %d, %s the export stub\n", n, r, viaGet == reinterpret_cast<void*>(GetProcAddress(cuda, n)) ? "is" : "is NOT");
            if (std::strcmp(n, "cuModuleLoadData") == 0)
                viaGetLoad = reinterpret_cast<FnLoad>(viaGet);
        }

    // Looked up AFTER hooking, as a caller of the export would
    const auto loadData = reinterpret_cast<FnLoad>(GetProcAddress(cuda, "cuModuleLoadData"));
    static const char tiny[] = ".version 7.0\n.target sm_52\n.address_size 64\n.visible .entry k()\n{\n ret;\n}\n";
    void* mod = nullptr;
    const CUresult r1 = loadData(&mod, tiny);
    if (mod)
        unload(mod);
    std::printf("tiny foreign PTX through the hooked export: result %d; hook saw %u, from our module %u, passed through %u\n", r1,
                unlock::cudaSeen.load(), unlock::cudaInOurs.load(), unlock::cudaOthers.load());
    bool ok = r1 == 0 && unlock::cudaInOurs.load() == 0 && unlock::cudaOthers.load() == 1;

    const MfgUnlock::ImageView image(ngx, unlock::GetMappedImageSize(ngx));
    const auto containers = MfgUnlock::FamilyRtx40::Fatbin::ContainerRvas(image);
    unsigned loaded = 0, failed = 0;
    for (const uint32_t rva : containers)
    {
        const auto* data = image.DataAtRva(rva, MfgUnlock::FamilyRtx40::Fatbin::kContainerHeader);
        void* m = nullptr;
        const CUresult r = loadData(&m, data);
        if (r == 0)
        {
            ++loaded;
            unload(m);
        }
        else
            ++failed;
    }
    std::printf("DLL containers through the hooked export: %zu containers, %u loaded by the driver, %u failed; hook saw %u, from our module %u, "
                "rewritten to sm_75 %u, left as shipped %u\n",
                containers.size(), loaded, failed, unlock::cudaSeen.load(), unlock::cudaInOurs.load(), unlock::cudaRewritten.load(),
                unlock::cudaLeft.load());
    ok = ok && !containers.empty() && failed == 0 && unlock::cudaRewritten.load() == containers.size() && unlock::cudaLeft.load() == 0;
    // 3. the pointer the CUDA runtime gets from cuGetProcAddress
    if (viaGetLoad && !containers.empty())
    {
        const unsigned before = unlock::cudaRewritten.load();
        void* m = nullptr;
        const auto* data = image.DataAtRva(containers.front(), MfgUnlock::FamilyRtx40::Fatbin::kContainerHeader);
        const CUresult r = viaGetLoad(&m, data);
        if (m)
            unload(m);
        std::printf("a container through the cuGetProcAddress pointer: result %d, rewritten by the hook: %s\n", r, unlock::cudaRewritten.load() == before + 1 ? "yes" : "NO");
        ok = ok && r == 0 && unlock::cudaRewritten.load() == before + 1;
    }
    else
        ok = false;
    std::puts(ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
