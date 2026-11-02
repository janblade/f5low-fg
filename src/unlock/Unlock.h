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
//   - the CUDA module loaders are hooked with a small inline hook of our own (Detours is not used; see InstallInlineHook), and a load
//     is only rewritten when its container lies inside OUR nvngx_dlssg (or the call comes from it); every other load goes straight
//     to the real function.
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
#include "hde/hde64.h"

#include "../long_path.h"

#include <windows.h>
#include <bcrypt.h>
#include <psapi.h>
#include <tlhelp32.h>
#include <intrin.h>

#include <array>
#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#pragma comment(lib, "Bcrypt.lib")

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
// fatbin whenever one of OUR nvngx_dlssg's containers is loaded (PtxTuring.h). Everything else passes through (below).
inline std::atomic<bool> turingRewriteWanted { false };
inline std::mutex cudaRewriteMutex;
inline uintptr_t ourBase = 0, ourEnd = 0; // our nvngx_dlssg's mapped range
inline std::atomic<unsigned> cudaSeen { 0 }, cudaInOurs { 0 }, cudaRewritten { 0 }, cudaLeft { 0 }, cudaOthers { 0 };

struct CudaRewriteEntry
{
    uint64_t contentHash = 0;
    std::shared_ptr<const std::string> text; // null: not rewritable
};
inline std::map<const void*, CudaRewriteEntry> cudaRewriteCache; // keyed by image address, counts only while the bytes hash the same

using PFN_cuModuleLoadData = int(WINAPI*)(void**, const void*);
using PFN_cuModuleLoadDataEx = int(WINAPI*)(void**, const void*, unsigned, int*, void**);
using PFN_cuLibraryLoadData = int(WINAPI*)(void**, const void*, void*, void**, unsigned, void*, void**, unsigned);
inline PFN_cuModuleLoadData o_cuModuleLoadData = nullptr;
inline PFN_cuModuleLoadData o_cuModuleLoadFatBinary = nullptr;
inline PFN_cuModuleLoadDataEx o_cuModuleLoadDataEx = nullptr;
inline PFN_cuLibraryLoadData o_cuLibraryLoadData = nullptr;

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
    {
        cudaOthers.fetch_add(1, std::memory_order_relaxed);
        return { image, nullptr };
    }
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

// ---- a small x64 inline hook (what Detours does for OptiScaler) ---------------------------------------------------------------------
// The four module-load exports of nvcuda are 6-byte stubs, `jmp qword ptr [rip+disp32]`, and the CUDA runtime / the NGX core do not
// go through them: cuGetProcAddress hands out the REAL functions behind (probe: tests/unlock_hook_probe.cpp). So the stub is followed
// and the function behind it hooked, like Detours does: its first whole instructions (>= 14 bytes) are copied into a trampoline
// next to nvcuda (a RIP-relative operand is re-based; a relative branch inside the copied bytes, a ret or an undecodable
// instruction refuses the hook) and replaced by `jmp [rip+0]` to the hook. Other threads are suspended for the write. Every caller
// of the function goes through our hook, which hands anything that is not one of OUR nvngx_dlssg's containers to the trampoline.
// Returns false with the reason when a function does not have a prologue this can take (then the RTX 20 path stands aside).
inline void SuspendOtherThreads(std::vector<HANDLE>& suspended)
{
    const HANDLE snap = CreateToolhelp32Snapshot(TH32CS_SNAPTHREAD, 0);
    if (snap == INVALID_HANDLE_VALUE)
        return;
    THREADENTRY32 te { sizeof(te) };
    for (BOOL ok = Thread32First(snap, &te); ok; ok = Thread32Next(snap, &te))
    {
        if (te.th32OwnerProcessID != GetCurrentProcessId() || te.th32ThreadID == GetCurrentThreadId())
            continue;
        if (HANDLE t = OpenThread(THREAD_SUSPEND_RESUME, FALSE, te.th32ThreadID))
        {
            if (SuspendThread(t) != static_cast<DWORD>(-1))
                suspended.push_back(t);
            else
                CloseHandle(t);
        }
    }
    CloseHandle(snap);
}
inline void ResumeThreads(std::vector<HANDLE>& suspended)
{
    for (const HANDLE t : suspended)
    {
        ResumeThread(t);
        CloseHandle(t);
    }
    suspended.clear();
}

inline bool InstallInlineHook(void* function, void* hook, void** trampolineOut, std::string& why)
{
    auto* code = static_cast<uint8_t*>(function);
    constexpr size_t kJump = 14; // FF 25 00 00 00 00 <imm64>
    // Follow `jmp [rip+disp32]` stubs to the function itself
    for (int hops = 0; hops < 4 && code[0] == 0xFF && code[1] == 0x25; ++hops)
    {
        int32_t disp = 0;
        std::memcpy(&disp, code + 2, sizeof(disp));
        code = *reinterpret_cast<uint8_t**>(code + 6 + disp);
    }
    struct Copied
    {
        std::array<uint8_t, 48> bytes {};
        size_t length = 0;
        std::vector<std::pair<size_t, int64_t>> ripFixups; // offset of a disp32 in `bytes`, the absolute address it meant
    } copied;
    while (copied.length < kJump)
    {
        hde64s d {};
        const unsigned len = hde64_disasm(code + copied.length, &d);
        if (len == 0 || (d.flags & F_ERROR) != 0 || copied.length + len > copied.bytes.size())
        {
            why = "cannot decode its first instructions";
            return false;
        }
        if ((d.flags & F_RELATIVE) != 0 || d.opcode == 0xC3 || d.opcode == 0xC2 || d.opcode == 0xCC || d.opcode == 0xE9 || d.opcode == 0xEB)
        {
            why = "its first instructions hold a branch or a return";
            return false;
        }
        std::memcpy(copied.bytes.data() + copied.length, code + copied.length, len);
        if ((d.flags & F_MODRM) != 0 && d.modrm_mod == 0 && d.modrm_rm == 5)
        {
            if ((d.flags & F_DISP32) == 0 || d.p_67)
            {
                why = "a RIP-relative operand this cannot re-base";
                return false;
            }
            const size_t immBytes = (d.flags & F_IMM8) ? 1 : (d.flags & F_IMM16) ? 2 : (d.flags & F_IMM32) ? 4 : (d.flags & F_IMM64) ? 8 : 0;
            const size_t dispAt = copied.length + len - immBytes - 4;
            copied.ripFixups.emplace_back(dispAt, reinterpret_cast<int64_t>(code) + static_cast<int64_t>(copied.length + len) + static_cast<int32_t>(d.disp.disp32));
        }
        copied.length += len;
    }
    // The trampoline: within +-2 GB of the function (its RIP-relative operands keep reaching their data)
    const size_t trampSize = copied.length + kJump;
    uint8_t* tramp = nullptr;
    const auto where = reinterpret_cast<uintptr_t>(code) & ~static_cast<uintptr_t>(0xFFFF);
    for (uintptr_t step = 1; step < 0x4000 && !tramp; ++step)
        for (const int dir : { -1, 1 })
        {
            const uintptr_t at = where + dir * step * 0x10000;
            tramp = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(at), trampSize, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            if (tramp)
                break;
        }
    if (!tramp)
    {
        why = "no memory next to nvcuda for the trampoline";
        return false;
    }
    std::memcpy(tramp, copied.bytes.data(), copied.length);
    for (const auto& [at, target] : copied.ripFixups)
    {
        const int64_t disp = target - (reinterpret_cast<int64_t>(tramp) + static_cast<int64_t>(at) + 4 + 0);
        // The immediate (if any) follows the disp32, so the RIP used by the CPU is the end of the instruction; recompute from it
        size_t instrEnd = 0, pos = 0;
        while (pos <= at)
        {
            hde64s d {};
            const unsigned len = hde64_disasm(tramp + pos, &d);
            instrEnd = pos + len;
            pos += len;
        }
        const int64_t fixed = target - (reinterpret_cast<int64_t>(tramp) + static_cast<int64_t>(instrEnd));
        (void)disp;
        if (fixed < INT32_MIN || fixed > INT32_MAX)
        {
            VirtualFree(tramp, 0, MEM_RELEASE);
            why = "the trampoline cannot reach the operand of its first instructions";
            return false;
        }
        const int32_t f32 = static_cast<int32_t>(fixed);
        std::memcpy(tramp + at, &f32, sizeof(f32));
    }
    tramp[copied.length] = 0xFF;
    tramp[copied.length + 1] = 0x25;
    std::memset(tramp + copied.length + 2, 0, 4);
    const uint64_t back = reinterpret_cast<uint64_t>(code) + copied.length;
    std::memcpy(tramp + copied.length + 6, &back, 8);
    FlushInstructionCache(GetCurrentProcess(), tramp, trampSize);

    uint8_t patch[kJump] = { 0xFF, 0x25, 0, 0, 0, 0 };
    const uint64_t hookAddress = reinterpret_cast<uint64_t>(hook);
    std::memcpy(patch + 6, &hookAddress, 8);
    std::vector<HANDLE> others;
    DWORD old = 0;
    if (!VirtualProtect(code, kJump, PAGE_EXECUTE_READWRITE, &old))
    {
        VirtualFree(tramp, 0, MEM_RELEASE);
        why = "cannot unprotect the function";
        return false;
    }
    SuspendOtherThreads(others);
    std::memcpy(code, patch, kJump);
    FlushInstructionCache(GetCurrentProcess(), code, kJump);
    ResumeThreads(others);
    VirtualProtect(code, kJump, old, &old);
    *trampolineOut = tramp;
    return true;
}

inline bool HookCudaLoaders(std::string& why)
{
    HMODULE nvcuda = LoadLibraryW(L"nvcuda.dll");
    if (!nvcuda)
    {
        why = "nvcuda.dll cannot be loaded";
        return false;
    }
    // The module-load functions are placeholders until the driver is initialised (cuInit is idempotent)
    if (const auto init = reinterpret_cast<int(WINAPI*)(unsigned)>(GetProcAddress(nvcuda, "cuInit")); !init || init(0) != 0)
    {
        why = "cuInit failed";
        return false;
    }
    struct Entry
    {
        const char* name;
        void* hook;
        void** original;
    };
    const Entry entries[] = {
        { "cuModuleLoadData", reinterpret_cast<void*>(&hkCuModuleLoadData), reinterpret_cast<void**>(&o_cuModuleLoadData) },
        { "cuModuleLoadFatBinary", reinterpret_cast<void*>(&hkCuModuleLoadFatBinary), reinterpret_cast<void**>(&o_cuModuleLoadFatBinary) },
        { "cuModuleLoadDataEx", reinterpret_cast<void*>(&hkCuModuleLoadDataEx), reinterpret_cast<void**>(&o_cuModuleLoadDataEx) },
        { "cuLibraryLoadData", reinterpret_cast<void*>(&hkCuLibraryLoadData), reinterpret_cast<void**>(&o_cuLibraryLoadData) },
    };
    std::vector<void*> done; // real functions already hooked (two exports can lead to one function)
    for (const Entry& e : entries)
    {
        auto* function = reinterpret_cast<void*>(GetProcAddress(nvcuda, e.name));
        if (!function)
        {
            why = std::string("nvcuda has no ") + e.name;
            return false;
        }
        if (*e.original) // hooked by an earlier call
            continue;
        std::string one;
        if (!InstallInlineHook(function, e.hook, e.original, one))
        {
            why = std::string(e.name) + ": " + one;
            return false;
        }
    }
    return true;
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

// NvapiKernels.h (included at the end): DLSS-G's kernel creation through NVAPI, rewritten for RTX 20/30
namespace nvapi
{
inline bool Install(int targetSm, HMODULE ngx, HMODULE plugin, std::string& why);
inline void LogSummary(const char* tierName, bool final);
inline bool InstallTestE9Hook(std::string& why);
} // namespace nvapi

// F5LOW_NVAPI_COUNT=1 (tests): the NVAPI hook also on tiers that need no rewrite, counting only
inline bool NvapiCountWanted()
{
    char v[8];
    return GetEnvironmentVariableA("F5LOW_NVAPI_COUNT", v, sizeof v) != 0;
}

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
    if (char t[8]; GetEnvironmentVariableA("F5LOW_TEST_E9_QI", t, sizeof t))
        if (std::string why; !nvapi::InstallTestE9Hook(why))
            Log("unlock: TEST: the E9 hook on nvapi_QueryInterface was not placed: %s", why.c_str());
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
        if (NvapiCountWanted())
        {
            // Count-only (tests): our two files are loaded here, by full path as below, so their import tables can be reached
            std::string why;
            const HMODULE ngxModule = LoadLibraryW(longpath::Plain(dir + L"\\nvngx_dlssg.dll").c_str());
            const HMODULE plModule = LoadLibraryW(longpath::Plain(dir + L"\\sl.xefg_dlss_g.dll").c_str());
            if (!ngxModule || !nvapi::Install(0, ngxModule, plModule, why))
                Log("unlock: dlssg kernels: NVAPI count hook not installed: %s", ngxModule ? why.c_str() : "cannot load nvngx_dlssg");
        }
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
        std::string hookWhy;
        if (!HookCudaLoaders(hookWhy))
        {
            turingRewriteWanted.store(false, std::memory_order_release);
            return fail("this DLSS-G build is not recognised by the unlock: the CUDA module loaders cannot be reached (" + hookWhy + ")");
        }
        Log("unlock: RTX 20: nvcuda's module loaders (cuModuleLoadData/FatBinary/DataEx, cuLibraryLoadData) are hooked; only containers of OUR nvngx_dlssg are rewritten for sm_75, everything else passes through");
    }

    // The kernels DLSS-G creates through NVAPI (its only way: it makes no CUDA calls): RTX 30 retargeted to sm_86, RTX 20 rewritten to
    // sm_75 (NvapiKernels.h). In place BEFORE the patches, because the patched module advertises a feature whose kernels only load this way;
    // a hook that cannot be placed fails the unlock like the CUDA loaders do. F5LOW_NVAPI_DISABLE=1 (tests) leaves it out.
    {
        char off[8];
        const int target = tier == Tier::Ampere ? 86 : tier == Tier::Turing ? 75 : 0;
        if (GetEnvironmentVariableA("F5LOW_NVAPI_DISABLE", off, sizeof off))
            Log("unlock: %s: TEST: the NVAPI kernel hook is disabled (F5LOW_NVAPI_DISABLE)", TierName(tier));
        else if (target != 0 || NvapiCountWanted())
        {
            std::string why;
            if (!nvapi::Install(target, g_ngx, g_plugin, why))
            {
                turingRewriteWanted.store(false, std::memory_order_release);
                return fail("this DLSS-G build is not recognised by the unlock: the NVAPI kernel creation cannot be reached (" + why + ")");
            }
            Log("unlock: %s: NVAPI kernel creation wrapped for OUR nvngx_dlssg / sl.dlss_g only (id 0xAD1A677D)", TierName(tier));
        }
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
    nvapi::LogSummary(g_ngx ? TierName(g_result.tier) : "native", false);
    if (!g_done || !g_ngx)
        return;
    ListModules(L"nvngx_dlssg.dll", g_ngx, "DLSS-G module:");
    ListModules(L"sl.xefg_dlss_g.dll", g_plugin, "plugin module:");
    for (const wchar_t* n : { L"nvcuda.dll", L"nvapi64.dll", L"_nvngx.dll", L"nvngx.dll" })
        ListModules(n, nullptr, "driver module:");
    bool ngxStill = false, plStill = false;
    Guarded([&] {
        ngxStill = g_ngxSet.StillApplied();
        plStill = g_pluginSet.StillApplied();
    });
    Log("unlock: patches still in place: nvngx_dlssg %s, sl.dlss_g %s", ngxStill ? "yes" : "no", plStill ? "yes" : "no");
    if (turingRewriteWanted.load())
        Log("unlock: RTX 20 CUDA rewrite so far: %u module loads seen, %u from our module, %u containers rewritten to sm_75, %u left as shipped (%u loads from other callers passed through)",
            cudaSeen.load(), cudaInOurs.load(), cudaRewritten.load(), cudaLeft.load(), cudaOthers.load());
}

inline void LogCudaTotals()
{
    nvapi::LogSummary(g_ngx ? TierName(g_result.tier) : "native", true);
    if (g_done && turingRewriteWanted.load())
        Log("unlock: RTX 20 CUDA rewrite: %u module loads seen, %u from our module, %u containers rewritten to sm_75, %u left as shipped (%u loads from other callers passed through)",
            cudaSeen.load(), cudaInOurs.load(), cudaRewritten.load(), cudaLeft.load(), cudaOthers.load());
}
} // namespace unlock

#include "NvapiKernels.h"
