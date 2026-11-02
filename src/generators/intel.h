// generators/intel.h - Generator=intel: Intel's libxess_fg makes the generated frames behind the XeFG API (docs/GENERATORS.md, "intel").
// The present-sink shape in "spaced" mode: the library presents into our sink (sink/sink.h), whose always-signalled latency waitable
// neutralises Intel's own pacing, so its burst arrives back to back and the sink spaces it on the real swap chain.
//
// NO copies on disk (user decision 2026-11-03): the library is the SAME libxess_fg.dll the own generator reads (RealLibraryPath in
// xefg_cuda.cpp: XEFG_CUDA_INTEL_LIBRARY, next to f5low.dll, the folder above = OptiScaler's), loaded by full path.
//   - The unlock (unlock/UnlockXeFg.h: U1-U5 in libxess_fg, XL1 in libxell) is applied right after OUR load and BEFORE the first
//     XeFG call: Intel sizes its per-generated-frame buffers from U3 when a context is created, so a late patch leaves 3-slot
//     buffers (repeated frames). So when libxess_fg.dll was ALREADY loaded in the process before we loaded it (a context may exist),
//     nothing is patched: the library runs at Intel's own maximum and the log says why.
//   - libxess_fg does not import libxell: it looks it up by name at run time (GetModuleHandleExA(0, "libxell.dll")), so it uses the
//     libxell already in the process (OptiScaler's or the game's) = the one that made the XeLL context. XL1 is patched in that module in
//     memory; it only lifts libxell's refusal of generated-frame counts above 3. If no libxell.dll is loaded when we need it, we load
//     the one next to libxess_fg by full path. If the XL1 site is not found, XL1 and the libxess_fg unlock are both skipped.
//   - A source that is f5low itself (our module, or any module exporting f5lowGetStatus) is never loaded.
// This patches inside Intel's DLLs in memory (personal work, user decision 2026-10-16); nothing is written to disk.
// There is NO fallback to the own generator: when the library cannot start the context is refused (see xefg_cuda.cpp). When the unlock
// cannot be applied (unknown build, pre-loaded, no libxell) the library runs unpatched at its own maximum, with the reason in the log.
// A failed rollback (a partially patched module) refuses the library outright.
#pragma once
#include "../generator.h"
#include "../sink/sink.h"
#include "dlssg_files.h"
#include "../unlock/UnlockXeFg.h"

#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

namespace xintel
{
using LogFn = void (*)(const char* fmt, ...);
inline LogFn g_log = nullptr;

inline std::wstring Trim(std::wstring d)
{
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/' || d.back() == L' '))
        d.pop_back();
    return d;
}

// F5LOW_INTEL_UNLOCK: 0 / off / false / no (any case) turns the unlock off (tests); anything else, or unset, leaves it on.
inline bool UnlockWanted()
{
    char v[16];
    if (!GetEnvironmentVariableA("F5LOW_INTEL_UNLOCK", v, sizeof(v)))
        return true;
    for (char* c = v; *c; ++c)
        *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
    return !(!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no"));
}

// ---- the library --------------------------------------------------------------------------------------------
struct Lib
{
    HMODULE fg = nullptr, ll = nullptr;
    std::wstring path;
    std::string how;
    std::string error;                  // not usable: why
    unlock::xefg::Result unlocked;      // ok = the six sites patched
    std::string unlockNote;             // one line for the status text
};
inline Lib g_lib;
inline std::mutex g_libMu;

// The libxess_fg at `fgPath` (what the own generator reads, `from` says where that came from), once per process. `selfPath`: our own DLL.
inline Lib* Get(const std::wstring& fgPath, const char* from, const std::wstring& selfPath, std::string& why)
{
    std::lock_guard<std::mutex> lk(g_libMu);
    Lib& l = g_lib;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        unlock::g_log = g_log;
        l.path = fgPath;
        const std::wstring p = longpath::ExtendIfLong(fgPath);
        if (fgPath.empty() || !xdlssg::FileExists(fgPath))
            l.error = "no libxess_fg.dll found (" + std::string(from) + ")";
        else if (!selfPath.empty() && _wcsicmp(fgPath.c_str(), selfPath.c_str()) == 0)
            l.error = "the libxess_fg source is f5low itself (" + xdlssg::Narrow(fgPath) + ")";
        if (l.error.empty())
        {
            // Already in the process before us? Then a context may exist and a late patch would leave 3-slot buffers.
            HMODULE pre = nullptr;
            const bool preloaded = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT, p.c_str(), &pre) && pre;
            if (preloaded)
            {
                if (GetProcAddress(pre, "f5lowGetStatus"))
                    l.error = "the libxess_fg source exports f5lowGetStatus: it is an f5low DLL, not Intel's library";
            }
            else
            {
                // Mapped without running its code, only to see its exports
                if (HMODULE probe = LoadLibraryExW(p.c_str(), nullptr, DONT_RESOLVE_DLL_REFERENCES))
                {
                    const bool isF5low = GetProcAddress(probe, "f5lowGetStatus") != nullptr;
                    FreeLibrary(probe);
                    if (isF5low)
                        l.error = "the libxess_fg source exports f5lowGetStatus: it is an f5low DLL, not Intel's library";
                }
            }
            if (l.error.empty())
            {
                l.fg = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (!l.fg)
                    l.error = "cannot load " + xdlssg::Narrow(fgPath) + " (error " + std::to_string(GetLastError()) + ")";
                else
                {
                    l.how = "libxess_fg from " + xdlssg::Narrow(fgPath) + " (" + from + "), " + (preloaded ? "already loaded in this process" : "loaded by f5low");
                    if (!UnlockWanted())
                        l.unlockNote = "unlock off (F5LOW_INTEL_UNLOCK): Intel's own maximum";
                    else if (preloaded)
                        l.unlockNote = "unlock skipped: libxess_fg.dll was already loaded in this process before f5low (a context may exist and "
                                       "patching late leaves 3-slot buffers) - Intel's own maximum";
                    else
                    {
                        // libxell: the one in the process (libxess_fg looks it up by that name), else the one next to libxess_fg
                        l.ll = GetModuleHandleW(L"libxell.dll");
                        std::string llNote = "the libxell.dll already in the process";
                        if (!l.ll)
                        {
                            std::wstring dir = fgPath;
                            dir.resize(dir.find_last_of(L'\\') == std::wstring::npos ? 0 : dir.find_last_of(L'\\'));
                            const std::wstring llPath = dir + L"\\libxell.dll";
                            if (xdlssg::FileExists(llPath))
                            {
                                l.ll = LoadLibraryExW(longpath::ExtendIfLong(llPath).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                                llNote = "libxell.dll loaded from " + xdlssg::Narrow(dir) + " (none in the process yet)";
                            }
                        }
                        if (!l.ll)
                            l.unlockNote = "unlock skipped: no libxell.dll in the process and none next to libxess_fg - Intel's own maximum";
                        else
                        {
                            if (g_log)
                                g_log("intel: XL1 patches %s in memory (it only lifts libxell's refusal of generated-frame counts above 3)", llNote.c_str());
                            l.unlocked = unlock::xefg::Apply(l.fg, l.ll, 5);
                            if (l.unlocked.fatal)
                                l.error = l.unlocked.reason + " - refusing the library";
                            else if (!l.unlocked.ok)
                                l.unlockNote = "unlock skipped: " + l.unlocked.reason + " - Intel's own maximum";
                            else
                                l.unlockNote = "unlocked in memory to 5 interpolated frames (6x)";
                        }
                    }
                }
            }
        }
    }
    if (!l.error.empty())
    {
        why = l.error;
        return nullptr;
    }
    return &l;
}

// ---- one context ---------------------------------------------------------------------------------------------
class Ctx
{
public:
    Lib* lib = nullptr;
    xefg_swapchain_handle_t h = nullptr;
    std::mutex mu;
    xsink::SinkFactory* sinkFac = nullptr;  // our reference, null until the swap chain exists
    bool enabled = false;
    uint32_t numInterp = 1;
    uint32_t maxGen = 3;
    unsigned statFrames = 0;

    ~Ctx()
    {
        if (sinkFac)
            sinkFac->Release();
    }
    // Frames per application frame the sink expects (called with mu held; the factory remembers it for a chain made later)
    void ApplyBurst()
    {
        if (sinkFac)
            sinkFac->SetBurst(enabled ? numInterp + 1 : 1);
    }
};

inline std::map<xefg_swapchain_handle_t, std::shared_ptr<Ctx>> g_ctx;
inline std::mutex g_ctxMu;
inline std::shared_ptr<Ctx> Find(xefg_swapchain_handle_t h)
{
    std::lock_guard<std::mutex> lk(g_ctxMu);
    auto it = g_ctx.find(h);
    return it == g_ctx.end() ? nullptr : it->second;
}
inline bool AnyContext()
{
    std::lock_guard<std::mutex> lk(g_ctxMu);
    return !g_ctx.empty();
}

// The library's export (the library is one per process: the caller caches the result per call site). Null if the library is not loaded
// or lacks it.
inline FARPROC Proc(const char* name)
{
    std::lock_guard<std::mutex> lk(g_libMu);
    return g_lib.fg ? GetProcAddress(g_lib.fg, name) : nullptr;
}

// F5LOW_SINK_STATS=1 (tests): the sink's statistics into the log, once the context is gone
inline void LogSinkStats()
{
    if (!g_log || !xsink::g_stats)
        return;
    for (const std::string& t : xsink::StatsLines())
        g_log("intel: %s", t.c_str());
}
} // namespace xintel

namespace xefgown
{
// The IGenerator face of intel: what the host asks before it routes a context here (Caps); the contexts are xintel::Ctx.
class IntelGenerator final : public IGenerator
{
public:
    IntelGenerator()
    {
        caps.name = "XeFG (Intel library)";
        caps.presentsItself = true;
        caps.anyGpu = true;
        caps.available = true;
        caps.maxGenerated = 3;      // replaced with what the library reports (5 after the unlock)
    }
    GenCaps Caps() const override
    {
        GenCaps c = caps;
        c.reason = reason.c_str();
        return c;
    }
    void Fail(const std::string& why)
    {
        caps.available = false;
        reason = why;
    }
    void SetMax(uint32_t n)
    {
        if (n)
            caps.maxGenerated = n;
    }
    bool Build(const GenBuild&) override { return false; }
    bool Rebuild(const xefgpack::Sizes&, const GenBuild&) override { return false; }
    void Drain() override {}
    void Stop() override {}
    void Shutdown() override {}

private:
    GenCaps caps;
    std::string reason;
};
} // namespace xefgown
