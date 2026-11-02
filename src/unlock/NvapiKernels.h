// unlock/NvapiKernels.h - the NVAPI kernel-creation hook of f5low's private DLSS-G (RTX 20/30 tiers).
//
// DLSS-G makes no CUDA calls: it creates its ~80 kernels through NVAPI (nvapi_QueryInterface id 0xAD1A677D: device, image, size,
// &handle, 0, name) and launches them as D3D12 compute work. Every image is a CUDA fatbin container with PTX for Ada/Blackwell, which an
// RTX 20/30 driver cannot use. nvapi_QueryInterface is hooked (one inline hook, unlock::detail::InstallInlineHook); when OUR
// nvngx_dlssg / sl.dlss_g ask for that id they are handed a wrapper, every other caller gets the real function. The wrapper swaps the
// container for one with a single PTX image for the card (NvapiFatbin.h) and calls the real create. A container that cannot be
// rewritten, or that the driver refuses, goes to the real create as shipped (counted, with the first reason in the log).
// RTX 40 and newer: not installed (the shipped images run there). F5LOW_NVAPI_COUNT=1 installs it count-only (tests).
#pragma once

#include "NvapiFatbin.h"
#include "Unlock.h"

#include <chrono>
#include <deque>

namespace unlock::nvapi
{
inline constexpr unsigned kIdCreateKernel = 0xAD1A677Du;

inline std::mutex g_mu;
inline int g_targetSm = 0;                   // 86, 75; 0: count only
inline HMODULE g_ours[2] = {};               // our nvngx_dlssg and sl.dlss_g (null: match by base name, count-only tests)
inline void* (WINAPI* o_queryInterface)(unsigned) = nullptr;
inline void* g_createFn = nullptr;           // the real create function
inline bool g_installed = false;
inline std::deque<std::vector<uint8_t>> g_keep; // the rewritten containers: alive for the process
struct Cached
{
    const std::vector<uint8_t>* made = nullptr; // null: left as shipped
    std::string why;                            // then why
};
inline std::map<std::pair<const void*, size_t>, Cached> g_cache; // shipped image -> rewritten
inline std::atomic<unsigned> g_seen { 0 }, g_rewritten { 0 }, g_passed { 0 }, g_failed { 0 };
inline std::atomic<uint64_t> g_ticksRewrite { 0 }, g_ticksTotal { 0 };
inline std::string g_firstFailure;
inline unsigned g_mma = 0, g_minMax = 0, g_cvt = 0;
inline unsigned g_logged = 0;                // the counts at the last summary line
inline bool g_firstLogged = false;

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

inline bool OurCaller(void* returnAddress)
{
    HMODULE caller = nullptr;
    if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                            static_cast<LPCWSTR>(returnAddress), &caller) ||
        !caller)
        return false;
    if (g_ours[0] || g_ours[1])
        return caller == g_ours[0] || caller == g_ours[1];
    const std::wstring path = GetModulePath(caller);
    const size_t slash = path.find_last_of(L"\\/");
    const wchar_t* base = path.c_str() + (slash == std::wstring::npos ? 0 : slash + 1);
    return _wcsicmp(base, L"nvngx_dlssg.dll") == 0 || _wcsicmp(base, L"sl.xefg_dlss_g.dll") == 0;
}

using CreateFn = uint64_t(WINAPI*)(uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t, uint64_t);

// The rewritten container for the shipped image, or null (left as shipped; the reason is counted).
inline const std::vector<uint8_t>* RewrittenFor(const void* image, size_t size, std::string& why)
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
    if (size < 16 || size > (64ull << 20) || !SafeCopy(image, 16, head))
        why = "image cannot be read (address " + std::to_string(reinterpret_cast<uintptr_t>(image)) + ", size " + std::to_string(size) + ")";
    else
    {
        std::memcpy(&magic, head, 4);
        std::memcpy(&fatSize, head + 8, 8);
        if (magic != MfgUnlock::PtxTuring::kFatbinMagic)
            why = "not a fatbin container";
        else if (fatSize == 0 || fatSize + 16 > size)
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
        g_mma += built.mma;
        g_minMax += built.minMax;
        g_cvt += built.cvt;
        if (!g_firstLogged)
        {
            g_firstLogged = true;
            Log("dlssg kernels: first container: source PTX sm_%d, entry flags 0x%llx, new image %zu bytes (shipped %llu)", built.sourceArch,
                static_cast<unsigned long long>(built.sourceFlags), result->size(), static_cast<unsigned long long>(fatSize + 16));
        }
    }
    g_cache[{ image, size }] = { result, why };
    return result;
}

// What DLSS-G calls instead of the driver's create.
inline uint64_t WINAPI Wrapper(uint64_t a, uint64_t b, uint64_t c, uint64_t d, uint64_t e, uint64_t f, uint64_t g, uint64_t h)
{
    const auto real = reinterpret_cast<CreateFn>(g_createFn);
    LARGE_INTEGER t0, t1, t2;
    QueryPerformanceCounter(&t0);
    ++g_seen;
    // No image at all (a probe call with a null image and size 0): not a kernel, nothing to rewrite
    if (g_targetSm == 0 || b == 0 || c == 0)
    {
        ++g_passed;
        const uint64_t status = real(a, b, c, d, e, f, g, h);
        QueryPerformanceCounter(&t1);
        g_ticksTotal += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
        return status;
    }
    std::string why;
    const std::vector<uint8_t>* made = RewrittenFor(reinterpret_cast<const void*>(b), static_cast<size_t>(c), why);
    QueryPerformanceCounter(&t1);
    g_ticksRewrite += static_cast<uint64_t>(t1.QuadPart - t0.QuadPart);
    uint64_t status = 0;
    bool failed = made == nullptr;
    if (made)
    {
        status = real(a, reinterpret_cast<uint64_t>(made->data()), made->size(), d, e, f, g, h);
        if (status != 0)
        {
            failed = true;
            why = "the driver refused the rewritten image (status " + std::to_string(static_cast<int>(status)) + ")";
        }
    }
    if (failed)
    {
        ++g_failed;
        {
            std::lock_guard lk(g_mu);
            if (g_firstFailure.empty())
                g_firstFailure = why;
        }
        Log("dlssg kernels: container left as shipped: %s", why.c_str());
        status = real(a, b, c, d, e, f, g, h);
    }
    else
        ++g_rewritten;
    QueryPerformanceCounter(&t2);
    g_ticksTotal += static_cast<uint64_t>(t2.QuadPart - t0.QuadPart);
    return status;
}

inline void* WINAPI hkQueryInterface(unsigned id)
{
    void* r = o_queryInterface(id);
    if (id == kIdCreateKernel && r && r != reinterpret_cast<void*>(&Wrapper) && OurCaller(_ReturnAddress()))
    {
        {
            std::lock_guard lk(g_mu);
            g_createFn = r;
        }
        return reinterpret_cast<void*>(&Wrapper);
    }
    return r;
}

// targetSm 86 / 75: rewrite; 0: count only. `ngx` / `plugin`: our two module handles (null: any nvngx_dlssg / sl.xefg_dlss_g).
inline bool Install(int targetSm, HMODULE ngx, HMODULE plugin, std::string& why)
{
    std::lock_guard lk(g_mu);
    if (g_installed)
        return true;
    HMODULE nvapi = LoadLibraryW(L"nvapi64.dll");
    auto* qi = nvapi ? reinterpret_cast<void*>(GetProcAddress(nvapi, "nvapi_QueryInterface")) : nullptr;
    if (!qi)
    {
        why = "nvapi64.dll has no nvapi_QueryInterface";
        return false;
    }
    g_targetSm = targetSm;
    g_ours[0] = ngx;
    g_ours[1] = plugin;
    if (!InstallInlineHook(qi, reinterpret_cast<void*>(&hkQueryInterface), reinterpret_cast<void**>(&o_queryInterface), why))
        return false;
    g_installed = true;
    return true;
}

// One line with what happened so far (again only when it changed): "dlssg kernels: ..."
inline void LogSummary(const char* tierName)
{
    if (!g_installed)
        return;
    const unsigned seen = g_seen.load();
    if (seen == 0 || seen == g_logged)
        return;
    g_logged = seen;
    std::string first;
    {
        std::lock_guard lk(g_mu);
        first = g_firstFailure;
    }
    if (g_targetSm == 0)
        Log("dlssg kernels: %s path, %u created, %u passed through (shipped images), 0 rewritten, 0 failed, took %.1f ms", tierName, seen,
            g_passed.load(), Ms(g_ticksTotal.load()));
    else
    {
        const std::string counters = g_targetSm == 75 ? " [mma " + std::to_string(g_mma) + ", min/max " + std::to_string(g_minMax) + ", cvt " + std::to_string(g_cvt) + "]" : "";
        Log("dlssg kernels: %s path, %u of %u %s, %u passed through, %u failed (%s), took %.1f ms (%.1f ms rewriting, rest = the driver's create / JIT)%s",
            tierName, g_rewritten.load(), seen, g_targetSm == 86 ? "retargeted" : "rewritten", g_passed.load(), g_failed.load(),
            first.empty() ? "none" : first.c_str(), Ms(g_ticksTotal.load()), Ms(g_ticksRewrite.load()), counters.c_str());
    }
}
} // namespace unlock::nvapi
