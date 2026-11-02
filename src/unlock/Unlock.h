// unlock/Unlock.h - OptiScaler's MFG unlock families applied to f5low's PRIVATE copy of nvngx_dlssg and sl.dlss_g, in memory.
//
// The glue is adapted from OptiScaler-NR OptiScaler/framegen/mfgunlock/MfgUnlock.cpp at 745e49431 (origin/release/0.1.36): the
// locate/apply order (LocateRtx40, CheckTuringKernels), the tier choice (ChooseRtx40Tier without the config modes), the CUDA
// module-load rewrite for RTX 20 and the SEH guards come from there. What differs, on purpose:
//   - OptiScaler watches every module load in the game's process and patches whatever DLSS-G modules show up. f5low never does:
//     it loads two files of its own by full path (<f5low_streamline>\nvngx_dlssg.dll and sl.xefg_dlss_g.dll, the renamed copy
//     made by generators/dlssg_files.h) BEFORE slInit, patches those module handles and nothing else, and logs the handles. The
//     game's own DLSS-G modules (any other handle of the same base name) are only listed in the log, never touched.
//   - the GPU comes from the caller (compute capability sm_NN, F5LOW_TEST_GPU_SM in tests) instead of IdentifyGpu/NVAPI:
//     sm >= 100 (RTX 50 and newer) patches nothing, 89 Ada, 80..88 Ampere, 75..79 Turing.
//   - the CUDA module loaders are not detoured process wide. nvngx_dlssg resolves nvcuda's entry points with GetProcAddress, so
//     the kernel32!GetProcAddress slot in THAT module's import table is pointed at a function of ours, which hands the four
//     module-load functions (directly, or through cuGetProcAddress) back as our hooks. Nobody else's calls are ever seen.
//   - no config/menu: tiers are not modes; there is no off switch (the unlock exists only for this private copy).
//   - all-or-none: both modules are located first; nothing is written unless every family of the tier located; the two patch
//     sets are applied one after the other and the first is restored if the second fails.
// The families (FamilyRtx40, FamilyAmpere, FamilyFatbin, FamilySlDlssg, PtxTuring, Locator, Image, Patch) are the ported headers
// next to this file, identical to OptiScaler's (see their headers).
#pragma once

#include "FamilyAmpere.h"
#include "FamilyFatbin.h"
#include "FamilyRtx40.h"
#include "FamilySlDlssg.h"
#include "Image.h"
#include "Locator.h"
#include "Patch.h"
#include "Provider.h"
#include "PtxTuring.h"
#include "Status.h"

#include "../long_path.h"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace unlock
{
using LogFn = void (*)(const char* fmt, ...);
inline LogFn g_log = nullptr;

enum class Tier
{
    None,   // RTX 50 and newer: DLSS-G generates up to 5 natively, nothing is patched
    Ada,    // sm_89
    Ampere, // sm_80..88
    Turing  // sm_75..79
};

inline const char* TierName(Tier t)
{
    switch (t)
    {
    case Tier::None: return "RTX 50 or newer";
    case Tier::Ada: return "RTX 40";
    case Tier::Ampere: return "RTX 30";
    case Tier::Turing: return "RTX 20";
    }
    return "?";
}

// The tier of a GPU of compute capability sm_NN. -1: not a GPU DLSS-G runs on (below sm_75, or 90..99 = datacentre Hopper, which
// OptiScaler's tier choice leaves alone as well).
inline int TierOfSm(int sm, Tier& tier)
{
    if (sm >= 100) { tier = Tier::None; return 0; }
    if (sm == 89) { tier = Tier::Ada; return 0; }
    if (sm >= 80 && sm < 89) { tier = Tier::Ampere; return 0; }
    if (sm >= 75 && sm < 80) { tier = Tier::Turing; return 0; }
    return -1;
}

struct Result
{
    bool ok = false;          // the unlock is in place (or not needed on this tier)
    bool nothingPatched = false;
    std::string reason;       // when !ok: "this DLSS-G build is not recognised by the unlock: <family>" etc.
    Tier tier = Tier::None;
    HMODULE ngx = nullptr, plugin = nullptr; // our two module handles (null on RTX 50)
};

namespace detail
{
template <class... A> inline void Log(const char* fmt, A... a)
{
    if (g_log)
        g_log(fmt, a...);
}
inline void LogText(const char* prefix, const std::wstring& w)
{
    if (g_log)
        g_log("%s%s", prefix, longpath::ToUtf8(w).c_str());
}

inline std::wstring GetModulePath(HMODULE module)
{
    if (module == nullptr)
        return {};
    std::array<wchar_t, 32768> path {};
    const DWORD length = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
    if (length == 0 || length >= path.size())
        return {};
    return { path.data(), length };
}

inline size_t GetMappedImageSize(HMODULE module)
{
    if (module == nullptr)
        return 0;
    constexpr size_t kMaximumImageSize = 512 * 1024 * 1024;
    const uintptr_t base = reinterpret_cast<uintptr_t>(module);
    uintptr_t cursor = base;
    size_t mappedSize = 0;
    while (cursor >= base && cursor - base < kMaximumImageSize)
    {
        MEMORY_BASIC_INFORMATION region {};
        if (VirtualQuery(reinterpret_cast<const void*>(cursor), &region, sizeof(region)) != sizeof(region) ||
            region.AllocationBase != module || region.Type != MEM_IMAGE)
            break;
        const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(region.BaseAddress) + region.RegionSize;
        if (regionEnd <= cursor || regionEnd < base)
            return 0;
        mappedSize = static_cast<size_t>(regionEnd - base);
        cursor = regionEnd;
    }
    return mappedSize;
}

// Handles only a bad read of mapped memory; everything else keeps unwinding so real faults are not swallowed.
inline int AccessViolationFilter(unsigned code)
{
    return code == EXCEPTION_ACCESS_VIOLATION || code == EXCEPTION_IN_PAGE_ERROR ? EXCEPTION_EXECUTE_HANDLER : EXCEPTION_CONTINUE_SEARCH;
}

// Locators read live module memory. A page that is gone makes the family stand aside, not take the game down, so each top-level
// locate runs under SEH. __try allows no C++ object with a destructor in its frame, hence the plain callback.
inline bool RunGuarded(void (*locate)(void*), void* context)
{
    __try
    {
        locate(context);
        return true;
    }
    __except (AccessViolationFilter(GetExceptionCode()))
    {
        return false;
    }
}
template <typename Locate> inline bool Guarded(Locate&& locate)
{
    using Callable = std::remove_reference_t<Locate>;
    return RunGuarded([](void* context) { (*static_cast<Callable*>(context))(); }, const_cast<void*>(static_cast<const void*>(&locate)));
}

inline std::string HashFileSha256(const std::wstring& path)
{
    HANDLE file = CreateFileW(longpath::ExtendIfLong(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
                              nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (file == INVALID_HANDLE_VALUE)
        return {};
    BCRYPT_ALG_HANDLE algorithm = nullptr;
    if (BCryptOpenAlgorithmProvider(&algorithm, BCRYPT_SHA256_ALGORITHM, nullptr, 0) != 0)
    {
        CloseHandle(file);
        return {};
    }
    BCRYPT_HASH_HANDLE hash = nullptr;
    if (BCryptCreateHash(algorithm, &hash, nullptr, 0, nullptr, 0, 0) != 0)
    {
        BCryptCloseAlgorithmProvider(algorithm, 0);
        CloseHandle(file);
        return {};
    }
    std::vector<UCHAR> buffer(1 << 16);
    bool ok = true;
    DWORD read = 0;
    while (true)
    {
        if (!ReadFile(file, buffer.data(), static_cast<DWORD>(buffer.size()), &read, nullptr))
        {
            ok = false;
            break;
        }
        if (read == 0)
            break;
        if (BCryptHashData(hash, buffer.data(), read, 0) != 0)
        {
            ok = false;
            break;
        }
    }
    std::array<UCHAR, 32> digest {};
    ok = ok && BCryptFinishHash(hash, digest.data(), static_cast<ULONG>(digest.size()), 0) == 0;
    BCryptDestroyHash(hash);
    BCryptCloseAlgorithmProvider(algorithm, 0);
    CloseHandle(file);
    if (!ok)
        return {};
    std::string result;
    static const char* hex = "0123456789abcdef";
    for (const UCHAR b : digest)
    {
        result += hex[b >> 4];
        result += hex[b & 15];
    }
    return result;
}

// ---- the RTX 20 CUDA rewrite (OptiScaler MfgUnlock.cpp: SubstituteCudaImage and its hooks) -------------------------------------
// RTX 20: nvngx_dlssg's kernels are PTX for newer cards. The driver is handed rewritten sm_75 PTX text instead of the shipped
// fatbin whenever one of OUR nvngx_dlssg's containers is loaded (PtxTuring.h). The hooks reach only that module (below).
inline std::atomic<bool> turingRewriteWanted { false };
inline std::mutex cudaRewriteMutex;
inline uintptr_t ourBase = 0, ourEnd = 0; // our nvngx_dlssg's mapped range
inline std::atomic<unsigned> cudaSeen { 0 }, cudaInOurs { 0 }, cudaRewritten { 0 }, cudaLeft { 0 }, cudaViaProc { 0 }, cudaViaGet { 0 };

struct CudaRewriteEntry
{
    uint64_t contentHash = 0;
    std::shared_ptr<const std::string> text; // null: not rewritable
};
inline std::map<const void*, CudaRewriteEntry> cudaRewriteCache; // keyed by image address, counts only while the bytes hash the same

using PFN_cuModuleLoadData = int(WINAPI*)(void**, const void*);
using PFN_cuModuleLoadDataEx = int(WINAPI*)(void**, const void*, unsigned, int*, void**);
using PFN_cuLibraryLoadData = int(WINAPI*)(void**, const void*, void*, void**, unsigned, void*, void**, unsigned);
using PFN_cuGetProcAddress = int(WINAPI*)(const char*, void**, int, unsigned long long);
using PFN_cuGetProcAddress2 = int(WINAPI*)(const char*, void**, int, unsigned long long, void*);
inline PFN_cuModuleLoadData o_cuModuleLoadData = nullptr;
inline PFN_cuModuleLoadData o_cuModuleLoadFatBinary = nullptr;
inline PFN_cuModuleLoadDataEx o_cuModuleLoadDataEx = nullptr;
inline PFN_cuLibraryLoadData o_cuLibraryLoadData = nullptr;
inline PFN_cuGetProcAddress o_cuGetProcAddress = nullptr;
inline PFN_cuGetProcAddress2 o_cuGetProcAddress2 = nullptr;

inline bool ReadMagic(const void* image, uint32_t& magic)
{
    __try
    {
        magic = *static_cast<const volatile uint32_t*>(image);
        return true;
    }
    __except (AccessViolationFilter(GetExceptionCode()))
    {
        return false;
    }
}

struct CudaSubstitute
{
    const void* image;
    std::shared_ptr<const std::string> keepAlive; // owns the rewritten text for the duration of the load call
};

// FNV-1a over the container bytes (resolving a fatbinary wrapper), 0 when it cannot be read.
inline uint64_t ContainerHash(const void* image)
{
    __try
    {
        auto container = static_cast<const uint8_t*>(image);
        if (*reinterpret_cast<const uint32_t*>(container) == MfgUnlock::PtxTuring::kFatbinWrapperMagic)
            container = *reinterpret_cast<const uint8_t* const*>(container + 8);
        if (container == nullptr || *reinterpret_cast<const uint32_t*>(container) != MfgUnlock::PtxTuring::kFatbinMagic)
            return 0;
        const uint64_t fatSize = *reinterpret_cast<const uint64_t*>(container + 8);
        if (fatSize == 0 || fatSize > (256ull << 20))
            return 0;
        uint64_t hash = 1469598103934665603ull;
        for (uint64_t i = 0; i < fatSize + 0x10; ++i)
            hash = (hash ^ container[i]) * 1099511628211ull;
        return hash;
    }
    __except (AccessViolationFilter(GetExceptionCode()))
    {
        return 0;
    }
}

inline bool InOurModule(const void* p)
{
    const auto a = reinterpret_cast<uintptr_t>(p);
    return ourBase && a >= ourBase && a < ourEnd;
}

// `caller` is the return address of the load call: the calls reach these hooks only through the slot / function pointers handed to
// our nvngx_dlssg, so it is that module's code; the check is kept as a second filter and for the log.
inline CudaSubstitute SubstituteCudaImage(const void* image, const void* caller)
{
    if (image == nullptr || !turingRewriteWanted.load(std::memory_order_acquire))
        return { image, nullptr };
    const unsigned n = cudaSeen.fetch_add(1, std::memory_order_relaxed);
    const bool ours = InOurModule(caller) || InOurModule(image);
    if (n < 3)
        Log("unlock: RTX 20: CUDA module load seen (image %p, caller %s, container %s our nvngx_dlssg)", image,
            InOurModule(caller) ? "in" : "outside", InOurModule(image) ? "inside" : "outside");
    if (!ours)
        return { image, nullptr };
    cudaInOurs.fetch_add(1, std::memory_order_relaxed);

    uint32_t magic = 0;
    if (!ReadMagic(image, magic) || (magic != MfgUnlock::PtxTuring::kFatbinMagic && magic != MfgUnlock::PtxTuring::kFatbinWrapperMagic))
        return { image, nullptr };
    // 0: the container could not be read in full. BuildFromContainer is not guarded, so leave such an image alone.
    const uint64_t hash = ContainerHash(image);
    if (hash == 0)
        return { image, nullptr };
    std::lock_guard lock(cudaRewriteMutex);
    auto found = cudaRewriteCache.find(image);
    if (found == cudaRewriteCache.end() || found->second.contentHash != hash)
    {
        CudaRewriteEntry entry;
        entry.contentHash = hash;
        const auto built = MfgUnlock::PtxTuring::BuildFromContainer(static_cast<const uint8_t*>(image));
        if (built.ok)
            entry.text = std::make_shared<const std::string>(built.ptx);
        else
            Log("unlock: RTX 20: kernel image left as is: %s", built.failure.c_str());
        found = cudaRewriteCache.insert_or_assign(image, std::move(entry)).first;
    }
    if (!found->second.text)
    {
        cudaLeft.fetch_add(1, std::memory_order_relaxed);
        return { image, nullptr };
    }
    cudaRewritten.fetch_add(1, std::memory_order_relaxed);
    return { found->second.text->c_str(), found->second.text };
}

inline int WINAPI hkCuModuleLoadData(void** module, const void* image)
{
    const auto s = SubstituteCudaImage(image, _ReturnAddress());
    return o_cuModuleLoadData(module, s.image);
}
inline int WINAPI hkCuModuleLoadFatBinary(void** module, const void* image)
{
    const auto s = SubstituteCudaImage(image, _ReturnAddress());
    return o_cuModuleLoadFatBinary(module, s.image);
}
inline int WINAPI hkCuModuleLoadDataEx(void** module, const void* image, unsigned numOptions, int* options, void** values)
{
    const auto s = SubstituteCudaImage(image, _ReturnAddress());
    return o_cuModuleLoadDataEx(module, s.image, numOptions, options, values);
}
inline int WINAPI hkCuLibraryLoadData(void** library, const void* image, void* jitOptions, void** jitValues, unsigned numJitOptions,
                                      void* libraryOptions, void** libraryValues, unsigned numLibraryOptions)
{
    const auto s = SubstituteCudaImage(image, _ReturnAddress());
    return o_cuLibraryLoadData(library, s.image, jitOptions, jitValues, numJitOptions, libraryOptions, libraryValues, numLibraryOptions);
}

// A driver entry point by name -> our hook for the four module loaders (the real one is remembered), else the pointer as it is.
inline void* HookFor(const char* name, void* real)
{
    if (!name || !real)
        return real;
    const auto is = [&](const char* n) { return strcmp(name, n) == 0; };
    if (is("cuModuleLoadData")) { o_cuModuleLoadData = reinterpret_cast<PFN_cuModuleLoadData>(real); return reinterpret_cast<void*>(&hkCuModuleLoadData); }
    if (is("cuModuleLoadFatBinary")) { o_cuModuleLoadFatBinary = reinterpret_cast<PFN_cuModuleLoadData>(real); return reinterpret_cast<void*>(&hkCuModuleLoadFatBinary); }
    if (is("cuModuleLoadDataEx")) { o_cuModuleLoadDataEx = reinterpret_cast<PFN_cuModuleLoadDataEx>(real); return reinterpret_cast<void*>(&hkCuModuleLoadDataEx); }
    if (is("cuLibraryLoadData")) { o_cuLibraryLoadData = reinterpret_cast<PFN_cuLibraryLoadData>(real); return reinterpret_cast<void*>(&hkCuLibraryLoadData); }
    return real;
}

inline int WINAPI hkCuGetProcAddress(const char* symbol, void** pfn, int version, unsigned long long flags)
{
    const int r = o_cuGetProcAddress(symbol, pfn, version, flags);
    if (r == 0 && pfn && *pfn)
        if (void* h = HookFor(symbol, *pfn); h != *pfn)
        {
            cudaViaGet.fetch_add(1, std::memory_order_relaxed);
            *pfn = h;
        }
    return r;
}
inline int WINAPI hkCuGetProcAddress2(const char* symbol, void** pfn, int version, unsigned long long flags, void* status)
{
    const int r = o_cuGetProcAddress2(symbol, pfn, version, flags, status);
    if (r == 0 && pfn && *pfn)
        if (void* h = HookFor(symbol, *pfn); h != *pfn)
        {
            cudaViaGet.fetch_add(1, std::memory_order_relaxed);
            *pfn = h;
        }
    return r;
}

// What nvngx_dlssg's kernel32!GetProcAddress slot points at while the rewrite is wanted.
inline FARPROC(WINAPI* o_GetProcAddress)(HMODULE, LPCSTR) = nullptr;
inline FARPROC WINAPI hkGetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC real = o_GetProcAddress(module, name);
    if (!real || reinterpret_cast<uintptr_t>(name) < 0x10000 || !turingRewriteWanted.load(std::memory_order_acquire))
        return real;
    if (strcmp(name, "cuGetProcAddress") == 0)
    {
        o_cuGetProcAddress = reinterpret_cast<PFN_cuGetProcAddress>(real);
        return reinterpret_cast<FARPROC>(&hkCuGetProcAddress);
    }
    if (strcmp(name, "cuGetProcAddress_v2") == 0)
    {
        o_cuGetProcAddress2 = reinterpret_cast<PFN_cuGetProcAddress2>(real);
        return reinterpret_cast<FARPROC>(&hkCuGetProcAddress2);
    }
    void* h = HookFor(name, reinterpret_cast<void*>(real));
    if (h != reinterpret_cast<void*>(real))
        cudaViaProc.fetch_add(1, std::memory_order_relaxed);
    return reinterpret_cast<FARPROC>(h);
}

// Points the kernel32!GetProcAddress import slot of `module` at hkGetProcAddress. False when the module has no such import.
inline bool HookGetProcAddressImport(HMODULE module)
{
    const auto* base = reinterpret_cast<const uint8_t*>(module);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    const auto& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (!dir.VirtualAddress)
        return false;
    for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name; ++d)
    {
        if (_stricmp(reinterpret_cast<const char*>(base + d->Name), "KERNEL32.dll") != 0)
            continue;
        const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
        auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(const_cast<uint8_t*>(base) + d->FirstThunk);
        for (size_t i = 0; names[i].u1.AddressOfData; ++i)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(names[i].u1.Ordinal))
                continue;
            const auto* by = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names[i].u1.AddressOfData);
            if (strcmp(by->Name, "GetProcAddress") != 0)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(&slots[i], sizeof(void*), PAGE_READWRITE, &old))
                return false;
            if (!o_GetProcAddress)
                o_GetProcAddress = reinterpret_cast<FARPROC(WINAPI*)(HMODULE, LPCSTR)>(slots[i].u1.Function);
            slots[i].u1.Function = reinterpret_cast<ULONGLONG>(&hkGetProcAddress);
            VirtualProtect(&slots[i], sizeof(void*), old, &old);
            return true;
        }
    }
    return false;
}

// RTX 20: every kernel container has to come out of the sm_75 rewrite now, at locate time. A container the rewrite cannot take would
// reach the driver as shipped at load time and fail there, after the module already advertised MFG.
inline bool CheckTuringKernels(const MfgUnlock::ImageView& image, std::string& failure)
{
    const auto containers = MfgUnlock::FamilyRtx40::Fatbin::ContainerRvas(image);
    if (containers.empty())
    {
        failure = "no kernel containers";
        return false;
    }
    for (const uint32_t rva : containers)
    {
        const auto* container = image.DataAtRva(rva, MfgUnlock::FamilyRtx40::Fatbin::kContainerHeader);
        const auto built = MfgUnlock::PtxTuring::BuildFromContainer(reinterpret_cast<const uint8_t*>(container));
        if (!built.ok)
        {
            failure = "container@" + std::to_string(rva) + " cannot be rewritten for sm_75: " + built.failure;
            return false;
        }
    }
    return true;
}

// Locates every site of one provider: the sl.dlss_g pair, or the nvngx_dlssg gates plus, by tier, the midpoint fix (Ada) or the
// architecture sites and kernels (RTX 20/30). Reads live module memory; run it through Guarded. (OptiScaler's LocateRtx40.)
inline void LocateRtx40(const MfgUnlock::ImageView& image, bool streamlinePlugin, Tier tier, MfgUnlock::FamilyRtx40::Report& report,
                        std::vector<MfgUnlock::PatchOperation>& operations, std::string& layouts)
{
    if (streamlinePlugin)
    {
        const auto slReport = MfgUnlock::FamilyRtx40::Streamline::Locate(image);
        report.patches = slReport.patches;
        report.failure = slReport.failure;
        if (slReport.Complete())
            operations = MfgUnlock::FamilyRtx40::Streamline::BuildOperations(slReport, image);
        layouts = "sl.dlss_g compiled ceiling=" + std::to_string(slReport.compiledCeiling) + " flip field=" + std::to_string(slReport.flipField);
        if (slReport.initialisers != 0)
            layouts += " initialisers=" + std::to_string(slReport.initialisers) +
                       " (left unpinned: the constructor stores 0 = hardware flip metering; accepted by the construction pattern, not a "
                       "proven order with the pinned stores)";
        return;
    }

    report = MfgUnlock::FamilyRtx40::Locate(image);
    layouts = "advertise=" + report.advertiseLayout + " validate=" + report.validateLayout;
    if (!report.Complete())
        return;
    operations = MfgUnlock::FamilyRtx40::BuildOperations(report, image);

    if (tier == Tier::Ampere || tier == Tier::Turing)
    {
        // RTX 30: every kernel retargeted to sm_86 in place. RTX 20: the kernels are rewritten into PTX text at load time (CUDA
        // hook), so the fatbin is only checked here (shape and rewrite). Either way the architecture is reported as the card's
        // own and there is no partial set.
        const bool turing = tier == Tier::Turing;
        const char* label = turing ? "RTX 20" : "RTX 30";
        const auto arch = MfgUnlock::FamilyAmpere::Locate(image, turing ? MfgUnlock::FamilyAmpere::kArchTuringLow : MfgUnlock::FamilyAmpere::kArchAmpereLow);
        const auto kernels = MfgUnlock::FamilyRtx40::Fatbin::Locate(image, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere);
        report.patches.insert(report.patches.end(), arch.patches.begin(), arch.patches.end());
        std::string kernelFailure;
        if (!arch.Complete())
            report.failure = arch.failure.empty() ? std::string(label) + " architecture sites incomplete" : arch.failure;
        else if (!kernels.Complete())
            report.failure = std::string(label) + " kernel check: " + kernels.failure;
        else if (turing && !CheckTuringKernels(image, kernelFailure))
            report.failure = std::string(label) + " kernel check: " + kernelFailure;
        else
        {
            auto archOps = MfgUnlock::FamilyRtx40::BuildOperationsFromPatches(arch.patches, image);
            operations.insert(operations.end(), archOps.begin(), archOps.end());
            if (turing)
                layouts += " sm_75 kernels=" + std::to_string(kernels.containers) + " containers (PTX rewrite)";
            else
            {
                auto kernelOps = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(kernels, image);
                operations.insert(operations.end(), kernelOps.begin(), kernelOps.end());
                layouts += " sm_86 kernels=" + std::to_string(kernels.containers) + " containers";
            }
        }
        return;
    }

    // The midpoint fix is added when this build has its kernel containers; the gates work without it.
    const auto fatbin = MfgUnlock::FamilyRtx40::Fatbin::Locate(image);
    auto midpoint = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(fatbin, image);
    if (!operations.empty() && !midpoint.empty())
    {
        operations.insert(operations.end(), std::make_move_iterator(midpoint.begin()), std::make_move_iterator(midpoint.end()));
        layouts += " midpoint=" + std::to_string(fatbin.containers) + " containers";
    }
    else
        layouts += " midpoint=not applied (" + fatbin.failure + ")";
}

struct Located
{
    HMODULE module = nullptr;
    bool streamline = false;
    MfgUnlock::FamilyRtx40::Report report;
    std::vector<MfgUnlock::PatchOperation> operations;
    std::string layouts;
    std::string failure; // empty: located completely
};

inline Located LocateModule(HMODULE module, bool streamline, Tier tier, const char* what)
{
    Located l;
    l.module = module;
    l.streamline = streamline;
    const size_t mapped = GetMappedImageSize(module);
    if (mapped == 0)
    {
        l.failure = std::string(what) + " is not a mapped image";
        return l;
    }
    const MfgUnlock::ImageView image(module, mapped);
    const bool readable = Guarded([&] { LocateRtx40(image, streamline, tier, l.report, l.operations, l.layouts); });
    if (!readable)
    {
        l.report = {};
        l.operations.clear();
        l.failure = std::string(what) + ": module memory could not be read while locating";
    }
    else if (!l.report.Complete())
        l.failure = std::string(what) + ": " + (l.report.failure.empty() ? "incomplete" : l.report.failure);
    else if (l.operations.empty())
        l.failure = std::string(what) + ": could not build the patch transaction";
    return l;
}

inline void LogLocated(const Located& l, const char* what)
{
    for (const auto& p : l.report.patches)
        Log("unlock:   %s patch %s: %s", what, p.name.c_str(), std::string(MfgUnlock::FamilyXeFG::LocateStateName(p.state)).c_str());
    Log("unlock:   %s layouts: %s; %zu byte writes", what, l.layouts.c_str(), l.operations.size());
}

// Every loaded module with the base name `name`: handle and path, and whether it is one of ours.
inline void ListModules(const wchar_t* name, HMODULE ours, const char* label)
{
    HMODULE all[2048];
    DWORD needed = 0;
    if (!EnumProcessModules(GetCurrentProcess(), all, sizeof(all), &needed))
        return;
    for (DWORD i = 0; i < needed / sizeof(HMODULE) && i < 2048; ++i)
    {
        const std::wstring p = GetModulePath(all[i]);
        const size_t slash = p.find_last_of(L"\\/");
        const std::wstring base = slash == std::wstring::npos ? p : p.substr(slash + 1);
        if (_wcsicmp(base.c_str(), name) != 0)
            continue;
        Log("unlock: %s %p %s%s", label, static_cast<void*>(all[i]), longpath::ToUtf8(p).c_str(),
            all[i] == ours ? "  <- ours (patched)" : "  <- not ours (never touched)");
    }
}

} // namespace detail
using namespace detail;

inline std::mutex g_mu;
inline Result g_result;
inline bool g_done = false;
inline MfgUnlock::AppliedPatchSet g_ngxSet, g_pluginSet;
inline HMODULE g_ngx = nullptr, g_plugin = nullptr;

// Loads our two modules from `dir` (the renamed folder), locates and applies the families of `tier` in them. Once per process: a
// second call returns the first result. Call BEFORE slInit.
inline Result Apply(const std::wstring& dir, Tier tier)
{
    std::lock_guard lk(g_mu);
    if (g_done)
        return g_result;
    g_done = true;
    Result r;
    r.tier = tier;
    const auto fail = [&](const std::string& why) {
        r.ok = false;
        r.reason = why;
        g_result = r;
        Log("unlock: %s: %s", TierName(tier), why.c_str());
        return r;
    };
    if (tier == Tier::None)
    {
        r.ok = true;
        r.nothingPatched = true;
        g_result = r;
        Log("unlock: %s: nothing to patch (DLSS-G generates up to 5 natively on this card)", TierName(tier));
        return r;
    }

    // Our two files, by full path, before Streamline loads anything: Streamline's own load of the same path then gets these handles
    // (checked in the log below), and the game's DLSS-G modules (other paths) are never involved.
    const std::wstring ngxPath = longpath::Plain(dir + L"\\nvngx_dlssg.dll");
    const std::wstring plPath = longpath::Plain(dir + L"\\sl.xefg_dlss_g.dll");
    g_ngx = LoadLibraryW(ngxPath.c_str());
    if (!g_ngx)
        return fail("cannot load " + longpath::ToUtf8(ngxPath) + " (error " + std::to_string(GetLastError()) + ")");
    g_plugin = LoadLibraryW(plPath.c_str());
    if (!g_plugin)
        return fail("cannot load " + longpath::ToUtf8(plPath) + " (error " + std::to_string(GetLastError()) + ")");
    r.ngx = g_ngx;
    r.plugin = g_plugin;
    ourBase = reinterpret_cast<uintptr_t>(g_ngx);
    ourEnd = ourBase + GetMappedImageSize(g_ngx);
    Log("unlock: %s: our modules loaded: nvngx_dlssg %p (sha256 %s), sl.xefg_dlss_g %p (sha256 %s)", TierName(tier), static_cast<void*>(g_ngx),
        HashFileSha256(ngxPath).c_str(), static_cast<void*>(g_plugin), HashFileSha256(plPath).c_str());

    // Locate everything first (reads only); nothing is written unless every family located.
    Located ngx = LocateModule(g_ngx, false, tier, "nvngx_dlssg");
    Located plugin = LocateModule(g_plugin, true, tier, "sl.dlss_g");
    LogLocated(ngx, "nvngx_dlssg");
    LogLocated(plugin, "sl.dlss_g");
    if (!ngx.failure.empty() || !plugin.failure.empty())
    {
        const std::string family = !ngx.failure.empty() ? ngx.failure : plugin.failure;
        return fail("this DLSS-G build is not recognised by the unlock: " + family);
    }

    if (tier == Tier::Turing)
    {
        // The CUDA rewrite is enabled before the patches are applied: the patched module advertises a feature whose kernels can only
        // load rewritten. The hooks pass everything through while the rewrite is not wanted.
        turingRewriteWanted.store(true, std::memory_order_release);
        if (!HookGetProcAddressImport(g_ngx))
        {
            turingRewriteWanted.store(false, std::memory_order_release);
            return fail("this DLSS-G build is not recognised by the unlock: its CUDA loader cannot be reached (no GetProcAddress import)");
        }
        Log("unlock: RTX 20: kernel32!GetProcAddress of OUR nvngx_dlssg points at the unlock; the CUDA module loaders are handed back as hooks (kernels are rewritten for sm_75)");
    }

    const auto ngxApplied = g_ngxSet.Apply(ngx.operations);
    if (!ngxApplied.applied)
    {
        if (g_ngxSet.IsActive())
            g_ngxSet.Restore();
        turingRewriteWanted.store(false, std::memory_order_release);
        return fail("patch transaction failed in nvngx_dlssg");
    }
    const auto plApplied = g_pluginSet.Apply(plugin.operations);
    if (!plApplied.applied)
    {
        if (g_pluginSet.IsActive())
            g_pluginSet.Restore();
        g_ngxSet.Restore();
        turingRewriteWanted.store(false, std::memory_order_release);
        return fail("patch transaction failed in sl.dlss_g (nvngx_dlssg restored)");
    }

    r.ok = true;
    g_result = r;
    // The status lines, in OptiScaler's format
    MfgUnlock::FamilyStatus st;
    st.state = MfgUnlock::FamilyState::Active;
    st.modulePath = GetModulePath(g_ngx);
    st.sha256 = HashFileSha256(ngxPath);
    st.failure = ngx.layouts;
    for (const auto& p : ngx.report.patches)
        st.patches.push_back({ p.name, "found" });
    const MfgUnlock::Family family = tier == Tier::Ada ? MfgUnlock::Family::RTX40 : MfgUnlock::Family::RTX2030;
    MfgUnlock::StatusRegistry::Set(family, st);
    LogText("unlock: ", MfgUnlock::FormatDiagnostic(family, st));
    Log("unlock: %s: APPLIED to our copy only: nvngx_dlssg %p (%zu byte writes) and sl.dlss_g %p (%zu byte writes)", TierName(tier),
        static_cast<void*>(g_ngx), ngx.operations.size(), static_cast<void*>(g_plugin), plugin.operations.size());
    return r;
}

// After Streamline and DLSS-G started: which handles the modules of those names have now, and whether our patches are still in place.
inline void LogAfterStart()
{
    std::lock_guard lk(g_mu);
    if (!g_done || !g_ngx)
        return;
    ListModules(L"nvngx_dlssg.dll", g_ngx, "DLSS-G module:");
    ListModules(L"sl.xefg_dlss_g.dll", g_plugin, "plugin module:");
    bool ngxStill = false, plStill = false;
    Guarded([&] {
        ngxStill = g_ngxSet.StillApplied();
        plStill = g_pluginSet.StillApplied();
    });
    Log("unlock: patches still in place: nvngx_dlssg %s, sl.dlss_g %s", ngxStill ? "yes" : "no", plStill ? "yes" : "no");
    if (turingRewriteWanted.load())
        Log("unlock: RTX 20 CUDA rewrite so far: %u module loads seen, %u from our module, %u containers rewritten to sm_75, %u left as shipped (hooks handed out: %u by name, %u through cuGetProcAddress)",
            cudaSeen.load(), cudaInOurs.load(), cudaRewritten.load(), cudaLeft.load(), cudaViaProc.load(), cudaViaGet.load());
}

inline void LogCudaTotals()
{
    if (g_done && turingRewriteWanted.load())
        Log("unlock: RTX 20 CUDA rewrite: %u module loads seen, %u from our module, %u containers rewritten to sm_75, %u left as shipped (hooks handed out: %u by name, %u through cuGetProcAddress)",
            cudaSeen.load(), cudaInOurs.load(), cudaRewritten.load(), cudaLeft.load(), cudaViaProc.load(), cudaViaGet.load());
}
} // namespace unlock
