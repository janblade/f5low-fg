// unlock/NvapiKernels.h - the NVAPI kernel-creation wrapper of f5low's private DLSS-G (RTX 20/30 tiers).
//
// DLSS-G makes no CUDA calls: it creates its ~80 kernels through NVAPI (nvapi_QueryInterface id 0xAD1A677D: device, image, size,
// &handle, 0, name) and launches them as D3D12 compute work. Every image is a CUDA fatbin container with PTX for Ada/Blackwell, which an
// RTX 20/30 driver cannot use.
//
// Scope: NOTHING global is hooked. Our private nvngx_dlssg.dll and sl.dlss_g.dll import GetProcAddress from kernel32 (read from their
// import tables: header data only), and they ask it for "nvapi_QueryInterface". The GetProcAddress slot in the import address table of
// THOSE TWO MODULES is pointed at hkGetProcAddress, which answers that one name with QueryFilter and forwards every other lookup. QueryFilter
// returns the real answer for every id except 0xAD1A677D, where it returns Wrapper. So no other caller (the game, OptiScaler, another
// DLSS-G copy) is touched, nvapi_QueryInterface itself is never patched (OptiScaler Detours-hooks it, which a code patch of ours would
// clash with), and no caller check is needed. This code never loads nvapi64.dll itself: the module handle is the one DLSS-G passes.
//
// The wrapper swaps the container for one with a single PTX image for the card (NvapiFatbin.h) and calls the real create. A container
// that cannot be rewritten, or that the driver refuses, goes to the real create as shipped (counted, with the first reason in the log).
// RTX 40 and newer: not installed (the shipped images run there). F5LOW_NVAPI_COUNT=1 installs it count-only (tests).
#pragma once

#include "NvapiFatbin.h"
#include "Unlock.h"

#include <deque>

namespace unlock::nvapi
{
inline constexpr unsigned kIdCreateKernel = 0xAD1A677Du;

inline std::mutex g_mu;                      // guards the counters, the cache and the container list
inline int g_targetSm = 0;                   // 86, 75; 0: count only
inline std::atomic<void*> g_realQi { nullptr };     // the real nvapi_QueryInterface (what DLSS-G's GetProcAddress lookup returned)
inline std::atomic<void*> g_createFn { nullptr };   // the real create function
inline bool g_installed = false;
inline std::atomic<bool> g_reachedLogged { false };
inline std::atomic<unsigned> g_qiLookups { 0 };      // lookups of "nvapi_QueryInterface" by our modules
inline std::deque<std::vector<uint8_t>> g_keep;      // the rewritten containers: alive for the process
struct Cached
{
    const std::vector<uint8_t>* made = nullptr;      // null: left as shipped
    std::string why;                                 // then why
};
inline std::map<std::pair<const void*, uint32_t>, Cached> g_cache; // shipped image -> rewritten
struct Counters // all under g_mu
{
    unsigned seen = 0, rewritten = 0, passed = 0, failed = 0, retried = 0;
    uint64_t ticksRewrite = 0, ticksTotal = 0;
    unsigned mma = 0, minMax = 0, cvt = 0;
    std::string firstFailure;
    bool firstLogged = false, failureLogged = false;
    unsigned lastLogged = ~0u;
};
inline Counters g_c;

inline double Ms(uint64_t ticks)
{
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    return 1000.0 * static_cast<double>(ticks) / static_cast<double>(f.QuadPart);
}

// Copies n bytes of the caller's memory; false when it cannot be read (SEH: no object with a destructor in this frame).
inline bool SafeCopy(const void* from, size_t n, uint8_t* to)
{
    __try
    {
        std::memcpy(to, from, n);
        return true;
    }
    __except (AccessViolationFilter(GetExceptionCode()))
    {
        return false;
    }
}

// The create function, as DLSS-G calls it: 8 argument words are forwarded. Only the first six mean something (device, image, size, &handle,
// 0, name); the callee decides how many it reads, so the words 5 to 8 are read from the caller's stack too (always mapped: it is the
// caller's own frame and the home area above it) and passed on unchanged. The result is an NvAPI_Status (int). The size is a 32-bit
// value as far as anything shows (image sizes are ~40 KB, and the upper half of the register is not defined for a 32-bit argument), so
// it is taken as uint32_t.
using CreateFn = int(WINAPI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// The rewritten container for the shipped image, or null (left as shipped; `why` says why).
inline const std::vector<uint8_t>* RewrittenFor(const void* image, uint32_t size, std::string& why)
{
    {
        std::lock_guard lk(g_mu);
        const auto found = g_cache.find({ image, size });
        if (found != g_cache.end())
        {
            why = found->second.why;
            return found->second.made;
        }
    }
    NvapiFatbin::Built built;
    uint8_t head[16] = {};
    uint64_t fatSize = 0;
    uint32_t magic = 0;
    if (size < 16 || size > (64u << 20) || !SafeCopy(image, 16, head))
        why = "image cannot be read (address " + std::to_string(reinterpret_cast<uintptr_t>(image)) + ", size " + std::to_string(size) + ")";
    else
    {
        std::memcpy(&magic, head, 4);
        std::memcpy(&fatSize, head + 8, 8);
        if (magic != MfgUnlock::PtxTuring::kFatbinMagic)
            why = "not a fatbin container";
        else if (fatSize == 0 || fatSize > size - 16) // size >= 16 here: no overflow
            why = "implausible fatbin size";
    }
    if (why.empty())
    {
        std::vector<uint8_t> copy(static_cast<size_t>(fatSize) + 16);
        if (!SafeCopy(image, copy.size(), copy.data()))
            why = "image cannot be read";
        else
        {
            built = NvapiFatbin::Rewrite(copy.data(), copy.size(), g_targetSm);
            if (!built.ok)
                why = built.failure;
        }
    }
    std::lock_guard lk(g_mu);
    const std::vector<uint8_t>* result = nullptr;
    if (built.ok)
    {
        g_keep.push_back(std::move(built.image));
        result = &g_keep.back();
        g_c.mma += built.mma;
        g_c.minMax += built.minMax;
        g_c.cvt += built.cvt;
        if (!g_c.firstLogged)
        {
            g_c.firstLogged = true;
            Log("dlssg kernels: first container: source PTX sm_%d (%s in the shipped image), entry flags 0x%llx, new image %zu bytes (shipped %llu)",
                built.sourceArch, built.sourceCompressed ? "LZ4-compressed" : "stored plain", static_cast<unsigned long long>(built.sourceFlags),
                result->size(), static_cast<unsigned long long>(fatSize + 16));
        }
    }
    g_cache[{ image, size }] = { result, why };
    return result;
}

// What DLSS-G calls instead of the driver's create.
inline int WINAPI Wrapper(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    const auto real = reinterpret_cast<CreateFn>(g_createFn.load());
    if (!g_reachedLogged.exchange(true)) // here, not in the GetProcAddress hook: that one can run under the loader lock
        Log("dlssg kernels: nvapi kernel create reached our filter (target %s)", g_targetSm == 86 ? "sm_86" : g_targetSm == 75 ? "sm_75" : "count only");
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    const uint32_t size = static_cast<uint32_t>(c);
    // No image at all (a probe call with a null image and size 0), or count-only: nothing to rewrite
    if (g_targetSm == 0 || b == 0 || size == 0)
    {
        const int status = real(a, b, c, d, e, f, g, h);
        QueryPerformanceCounter(&t1);
        std::lock_guard lk(g_mu);
        ++g_c.seen;
        ++g_c.passed;
        g_c.ticksTotal += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        return status;
    }
    std::string why;
    const std::vector<uint8_t>* made = RewrittenFor(reinterpret_cast<const void*>(b), size, why);
    QueryPerformanceCounter(&t1);
    // d is the output handle (8 bytes read to tell whether a call wrote it; unreadable = assume it did, so nothing is repeated)
    bool readable = true;
    const NvapiFatbin::Attempt attempt = NvapiFatbin::CreateWithFallback(
        made != nullptr, why,
        [&](bool ours) {
            return ours ? real(a, reinterpret_cast<uint64_t>(made->data()), static_cast<uint32_t>(made->size()), d, e, f, g, h)
                        : real(a, b, c, d, e, f, g, h);
        },
        [&]() -> uint64_t {
            uint64_t v = 0;
            readable = d != 0 && SafeCopy(reinterpret_cast<const void*>(d), sizeof(v), reinterpret_cast<uint8_t*>(&v));
            return v;
        },
        [&](uint64_t before) {
            uint64_t v = 0;
            return !readable || !SafeCopy(reinterpret_cast<const void*>(d), sizeof(v), reinterpret_cast<uint8_t*>(&v)) || v != before;
        });
    QueryPerformanceCounter(&t2);
    std::lock_guard lk(g_mu);
    ++g_c.seen;
    g_c.ticksRewrite += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
    g_c.ticksTotal += static_cast<uint64_t>(t2.QuadPart - t0.QuadPart);
    if (attempt.rewritten)
        ++g_c.rewritten;
    if (attempt.retried)
        ++g_c.retried;
    if (attempt.failed)
    {
        ++g_c.failed;
        if (g_c.firstFailure.empty())
            g_c.firstFailure = attempt.why;
        if (!g_c.failureLogged) // the first reason only; the counts are in the summary line
        {
            g_c.failureLogged = true;
            Log("dlssg kernels: first container left as shipped: %s", attempt.why.c_str());
        }
    }
    return attempt.status;
}

// What our modules get for "nvapi_QueryInterface": the real function's answer, except for the kernel-creation id.
inline void* WINAPI QueryFilter(unsigned id)
{
    const auto real = reinterpret_cast<void*(WINAPI*)(unsigned)>(g_realQi.load());
    void* r = real ? real(id) : nullptr;
    if (id == kIdCreateKernel && r)
    {
        g_createFn = r;
        return reinterpret_cast<void*>(&Wrapper);
    }
    return r;
}

// GetProcAddress as our two modules see it (their import slots point here).
inline FARPROC WINAPI hkGetProcAddress(HMODULE module, LPCSTR name)
{
    FARPROC r = ::GetProcAddress(module, name);
    if (r && reinterpret_cast<uintptr_t>(name) > 0xFFFF && std::strcmp(name, "nvapi_QueryInterface") == 0)
    {
        g_realQi = reinterpret_cast<void*>(r);
        ++g_qiLookups;
        return reinterpret_cast<FARPROC>(&QueryFilter);
    }
    return r;
}

// Points every "GetProcAddress" slot in the import table of `module` at the hook; returns how many (0: none, or the table is not readable).
inline int PatchGetProcAddressSlots(HMODULE module)
{
    int patched = 0;
    Guarded([&] {
        auto* base = reinterpret_cast<uint8_t*>(module);
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE)
            return;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE)
            return;
        const IMAGE_DATA_DIRECTORY& dir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
        if (dir.VirtualAddress == 0)
            return;
        for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dir.VirtualAddress); d->Name != 0; ++d)
        {
            const auto* names = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + (d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk));
            auto* slots = reinterpret_cast<IMAGE_THUNK_DATA64*>(base + d->FirstThunk);
            for (; names->u1.AddressOfData != 0; ++names, ++slots)
            {
                if (IMAGE_SNAP_BY_ORDINAL64(names->u1.Ordinal))
                    continue;
                const auto* byName = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + names->u1.AddressOfData);
                if (std::strcmp(byName->Name, "GetProcAddress") != 0)
                    continue;
                DWORD old = 0;
                if (!VirtualProtect(slots, sizeof(*slots), PAGE_READWRITE, &old))
                    continue;
                slots->u1.Function = reinterpret_cast<ULONGLONG>(&hkGetProcAddress);
                VirtualProtect(slots, sizeof(*slots), old, &old);
                ++patched;
            }
        }
    });
    return patched;
}

// targetSm 86 / 75: rewrite; 0: count only. `ngx` / `plugin`: our two module handles. Fails (with the reason) when nvngx_dlssg's import
// table has no GetProcAddress slot to point at.
inline bool Install(int targetSm, HMODULE ngx, HMODULE plugin, std::string& why)
{
    std::lock_guard lk(g_mu);
    if (g_installed)
        return true;
    if (!ngx)
    {
        why = "no module to hook";
        return false;
    }
    // Our slots point into this DLL and the DLSS-G modules may outlive it: pin it
    HMODULE self = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN, reinterpret_cast<LPCWSTR>(&hkGetProcAddress), &self);
    // nvngx_dlssg first; sl.dlss_g and the target only after that slot worked (a failed install leaves nothing half-hooked)
    const int inNgx = PatchGetProcAddressSlots(ngx);
    if (inNgx == 0)
    {
        why = "the DLSS-G module does not import GetProcAddress from a table this can read";
        return false;
    }
    g_targetSm = targetSm;
    const int inPlugin = plugin ? PatchGetProcAddressSlots(plugin) : 0;
    Log("dlssg kernels: GetProcAddress slots of our DLSS-G modules hooked: nvngx_dlssg %d, sl.dlss_g %d (no global hook)", inNgx, inPlugin);
    g_installed = true;
    return true;
}

// After Streamline and DLSS-G started: a tier that needs the rewrite whose modules have not asked for nvapi_QueryInterface through the slot
// (they may do it later, or reach it another way)
inline void WarnIfBypassed(const char* tierName)
{
    if (g_installed && g_targetSm != 0 && g_qiLookups.load() == 0)
        Log("dlssg kernels: WARNING %s path: DLSS-G has not asked through our import slot yet - kernels may load as shipped", tierName);
}

// One line with what happened so far (again only when it changed): "dlssg kernels: ..."; `final` (the context is gone) also says so when
// DLSS-G created nothing.
inline void LogSummary(const char* tierName, bool final)
{
    std::lock_guard lk(g_mu);
    if (!g_installed)
        return;
    const Counters c = g_c;
    if (c.seen == 0)
    {
        if (final)
            Log("dlssg kernels: %s path, hook installed, 0 kernel creations seen (our modules looked up nvapi_QueryInterface %u times)", tierName,
                g_qiLookups.load());
        return;
    }
    if (c.seen == g_c.lastLogged)
        return;
    g_c.lastLogged = c.seen;
    if (g_targetSm == 0)
        Log("dlssg kernels: %s path, %u created, %u passed through (shipped images), 0 rewritten, 0 failed, took %.1f ms", tierName, c.seen, c.passed,
            Ms(c.ticksTotal));
    else
    {
        const std::string counters = g_targetSm == 75 ? " [mma " + std::to_string(c.mma) + ", min/max " + std::to_string(c.minMax) + ", cvt " + std::to_string(c.cvt) + "]" : "";
        Log("dlssg kernels: %s path, %u of %u %s, %u passed through, %u failed (%s), %u repeated with the shipped image, took %.1f ms (%.1f ms rewriting, rest = the driver's create / JIT)%s",
            tierName, c.rewritten, c.seen, g_targetSm == 86 ? "retargeted" : "rewritten", c.passed, c.failed,
            c.firstFailure.empty() ? "none" : c.firstFailure.c_str(), c.retried, Ms(c.ticksTotal), Ms(c.ticksRewrite), counters.c_str());
    }
}

// ---- test only: a Detours-style hook (E9 rel32 at the entry) already on nvapi_QueryInterface, to show that nothing here depends on its first bytes ----
inline int g_e9Calls = 0;
inline void* (WINAPI* o_e9Original)(unsigned) = nullptr;
inline void* WINAPI E9Hook(unsigned id)
{
    ++g_e9Calls;
    return o_e9Original(id);
}
// F5LOW_TEST_E9_QI=1 (tests): hooks nvapi_QueryInterface the way Detours does (a `jmp rel32` at the entry to a stub that jumps to the hook).
inline bool InstallTestE9Hook(std::string& why)
{
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    auto* qi = nvapi ? reinterpret_cast<uint8_t*>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
    if (!qi)
    {
        why = "nvapi64.dll has no nvapi_QueryInterface";
        return false;
    }
    if (!InstallInlineHook(qi, reinterpret_cast<void*>(&E9Hook), reinterpret_cast<void**>(&o_e9Original), why))
        return false;
    // The inline hook leaves FF 25 ... at the entry; a Detours hook leaves E9 rel32 to a stub near the function
    uint8_t* stub = nullptr;
    const auto where = reinterpret_cast<uintptr_t>(qi) & ~static_cast<uintptr_t>(0xFFFF);
    for (uintptr_t step = 1; step < 0x4000 && !stub; ++step)
        for (const int dir : { -1, 1 })
        {
            stub = static_cast<uint8_t*>(VirtualAlloc(reinterpret_cast<void*>(where + dir * step * 0x10000), 32, MEM_RESERVE | MEM_COMMIT, PAGE_EXECUTE_READWRITE));
            if (stub)
                break;
        }
    if (!stub)
    {
        why = "no memory next to nvapi for the stub";
        return false;
    }
    const uint64_t hookAddress = reinterpret_cast<uint64_t>(&E9Hook);
    stub[0] = 0xFF;
    stub[1] = 0x25;
    std::memset(stub + 2, 0, 4);
    std::memcpy(stub + 6, &hookAddress, 8);
    const int64_t rel = reinterpret_cast<int64_t>(stub) - (reinterpret_cast<int64_t>(qi) + 5);
    if (rel < INT32_MIN || rel > INT32_MAX)
    {
        why = "the stub is out of reach";
        return false;
    }
    const int32_t rel32 = static_cast<int32_t>(rel);
    DWORD old = 0;
    if (!VirtualProtect(qi, 16, PAGE_EXECUTE_READWRITE, &old))
    {
        why = "cannot unprotect nvapi_QueryInterface";
        return false;
    }
    qi[0] = 0xE9;
    std::memcpy(qi + 1, &rel32, 4);
    FlushInstructionCache(GetCurrentProcess(), qi, 16);
    VirtualProtect(qi, 16, old, &old);
    Log("dlssg kernels: TEST: nvapi_QueryInterface now starts with E9 (a Detours-style hook), stub %p", static_cast<void*>(stub));
    return true;
}
} // namespace unlock::nvapi
