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

#include <algorithm>
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

// F5LOW_INTEL_UNLOCK: 0 / off / false / no (any case) turns the unlock off (tests); anything else, unset or too long to be a value, leaves it on.
inline bool UnlockWanted()
{
    char v[16] = {};
    const DWORD n = GetEnvironmentVariableA("F5LOW_INTEL_UNLOCK", v, sizeof(v));
    if (n == 0 || n >= sizeof(v))
        return true;
    for (char* c = v; *c; ++c)
        *c = static_cast<char>(tolower(static_cast<unsigned char>(*c)));
    return !(!strcmp(v, "0") || !strcmp(v, "off") || !strcmp(v, "false") || !strcmp(v, "no"));
}

// Whether the image at `base` (mapped as it is in memory, `size` bytes) exports `name`. Bounds-checked, under SEH: a truncated file
// must not take the process down.
inline bool HasExport(const uint8_t* base, size_t size, const char* name)
{
    bool found = false;
    __try
    {
        if (size < sizeof(IMAGE_DOS_HEADER))
            return false;
        const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
        if (dos->e_magic != IMAGE_DOS_SIGNATURE || dos->e_lfanew <= 0 || static_cast<size_t>(dos->e_lfanew) + sizeof(IMAGE_NT_HEADERS64) > size)
            return false;
        const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
        if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
            return false;
        // the image size the headers claim must fit what is mapped; everything below is read inside it
        if (nt->OptionalHeader.SizeOfImage == 0 || nt->OptionalHeader.SizeOfImage > size)
            return false;
        size = nt->OptionalHeader.SizeOfImage;
        const IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        if (!dd.VirtualAddress || static_cast<size_t>(dd.VirtualAddress) + sizeof(IMAGE_EXPORT_DIRECTORY) > size)
            return false;
        const auto* ex = reinterpret_cast<const IMAGE_EXPORT_DIRECTORY*>(base + dd.VirtualAddress);
        if (ex->AddressOfNames + 4ull * ex->NumberOfNames > size)
            return false;
        const auto* names = reinterpret_cast<const DWORD*>(base + ex->AddressOfNames);
        for (DWORD i = 0; i < ex->NumberOfNames && !found; ++i)
            if (static_cast<size_t>(names[i]) + strlen(name) + 1 <= size &&       // the whole name, with its NUL, lies inside the image
                memcmp(base + names[i], name, strlen(name) + 1) == 0)
                found = true;
    }
    __except (EXCEPTION_EXECUTE_HANDLER)
    {
        return false;
    }
    return found;
}

// Whether the file at `path` is an f5low DLL (exports f5lowGetStatus): mapped as a data file / image resource, so no code of it runs and
// nothing is resolved (a mapping others might load later is not left behind).
inline bool IsF5lowFile(const std::wstring& path)
{
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_LIBRARY_AS_IMAGE_RESOURCE | LOAD_LIBRARY_AS_DATAFILE);
    if (!m)
        return false;
    const auto* base = reinterpret_cast<const uint8_t*>(reinterpret_cast<uintptr_t>(m) & ~static_cast<uintptr_t>(3));
    MEMORY_BASIC_INFORMATION mbi {};
    size_t size = 0;
    if (VirtualQuery(base, &mbi, sizeof(mbi)) == sizeof(mbi))
        size = static_cast<size_t>(mbi.RegionSize);
    const bool is = HasExport(base, size, "f5lowGetStatus");
    FreeLibrary(m);
    return is;
}

// The modules of this process (handles), for "was it loaded before us": by handle, not by path spelling (8.3 names, junctions, \\?\ ...).
inline std::vector<HMODULE> ProcessModules()
{
    std::vector<HMODULE> v(512);
    for (int tries = 0; tries < 4; ++tries)
    {
        DWORD need = 0;
        if (!EnumProcessModules(GetCurrentProcess(), v.data(), static_cast<DWORD>(v.size() * sizeof(HMODULE)), &need))
            return {};
        if (need <= v.size() * sizeof(HMODULE))
        {
            v.resize(need / sizeof(HMODULE));
            return v;
        }
        v.resize(need / sizeof(HMODULE) + 64);
    }
    return {};
}

// ---- the library --------------------------------------------------------------------------------------------
struct Lib
{
    HMODULE fg = nullptr, ll = nullptr;
    std::wstring path;
    std::string how;
    std::string error;                  // not usable: why
    unlock::xefg::Result unlocked;      // ok = the sites patched
    std::string unlockNote;             // one line for the status text
};
inline Lib g_lib;
inline std::mutex g_libMu;

// The libxess_fg at `fgPath` (what the own generator reads, `from` says where that came from), once per process. `selfPath`: our own DLL.
// realModule / realFirstLoad / realHadContext: the shared library f5low's own forwarding loaded (Real() in xefg_cuda.cpp), whether that
// load was the FIRST one in the process (no other loader had it before), and whether a context was ever made through it. Only the same
// module, first loaded by us, with no context made through it, counts as ours and is still safe to patch: if another loader had it first
// (OptiScaler, the game) it may have a live context on the same file.
inline Lib* Get(const std::wstring& fgPath, const char* from, const std::wstring& selfPath, HMODULE realModule, bool realFirstLoad, bool realHadContext,
                std::string& why)
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
        else if (IsF5lowFile(p))
            l.error = "the libxess_fg source exports f5lowGetStatus: it is an f5low DLL, not Intel's library";
        if (l.error.empty())
        {
            // Was it in the process before us? By handle: the modules of the process before the load, then the handle the load returns.
            const std::vector<HMODULE> before = ProcessModules();
            l.fg = LoadLibraryExW(p.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
            if (!l.fg)
                l.error = "cannot load " + xdlssg::Narrow(fgPath) + " (error " + std::to_string(GetLastError()) + ")";
            else if (GetProcAddress(l.fg, "f5lowGetStatus"))
                l.error = "the libxess_fg source exports f5lowGetStatus: it is an f5low DLL, not Intel's library";
            else
            {
                const bool preloaded = before.empty() || std::find(before.begin(), before.end(), l.fg) != before.end();   // no list: assume the worst
                const bool oursEarlier = preloaded && l.fg == realModule && realFirstLoad && !realHadContext;
                l.how = "libxess_fg from " + xdlssg::Narrow(fgPath) + " (" + from + "), " +
                        (!preloaded ? "loaded by f5low" : oursEarlier ? "loaded earlier by f5low's own forwarding (the first load in the process), no context made through it" : "already loaded in this process");
                if (!UnlockWanted())
                    l.unlockNote = "unlock off (F5LOW_INTEL_UNLOCK): Intel's own maximum";
                else if (preloaded && !oursEarlier)
                    l.unlockNote = "unlock skipped: libxess_fg.dll was already loaded in this process before f5low (not by f5low's forwarding, or contexts "
                                   "may exist; patching late leaves 3-slot buffers) - Intel's own maximum";
                else
                {
                    // libxell: the one in the process (libxess_fg looks it up by that name), else the one next to libxess_fg
                    // one step: the lookup and the reference we keep on the module we are about to patch (it is never unloaded, and loaded
                    // again unpatched, under us); the reference lasts for the life of the process
                    GetModuleHandleExW(0, L"libxell.dll", &l.ll);
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
                        {
                            l.unlockNote = "unlocked in memory to 5 interpolated frames (6x)";
                            // The patched module is THE module of that path: whoever loads it later in this process gets it as it is now
                            if (g_log)
                                g_log("intel: module shared: later loaders of %s in this process get the unlocked libxess_fg (reports up to 5 "
                                      "interpolated frames)", xdlssg::Narrow(fgPath).c_str());
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
class StampChain;

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
    std::atomic<bool> schedule { false };       // the sink releases on the schedule (F5LOW_SINK_PACE), so the game's Presents are stamped
    StampChain* stampChain = nullptr;           // what the game presents on in schedule mode (our reference), made at GetSwapChainPtr; under mu
    xsink::SinkFactory* stampTarget = nullptr;  // the factory the stamps go to (a plain pointer to sinkFac's object, under mu: the chain stamps with mu held)

    ~Ctx();
    // Frames per application frame the sink expects (called with mu held; the factory remembers it for a chain made later)
    void ApplyBurst()
    {
        if (sinkFac)
            sinkFac->SetBurst(enabled ? numInterp + 1 : 1);
    }
};

// What the game presents on in schedule mode: Intel's proxy chain behind a thin wrapper that stamps the time of every application
// Present for the sink (xsink::SinkFactory::AppPresent). The least invasive place that sees the game's own Present: the call itself is
// forwarded untouched, nothing else of the chain is changed (scproxy::LogSwapChain forwards every other method). Holds its context weakly:
// the game may keep the chain after Destroy (then it presents plainly).
class StampChain final : public scproxy::LogSwapChain
{
    std::weak_ptr<Ctx> ctx;
    // With the context's lock held: the target is only released/replaced under it (Destroy, a re-init), so it is alive here
    void Stamp(UINT flags)
    {
        if (flags & DXGI_PRESENT_TEST)      // a test present shows nothing: no stamp, no interval sample
            return;
        if (std::shared_ptr<Ctx> c = ctx.lock())
        {
            std::lock_guard<std::mutex> lk(c->mu);
            if (c->stampTarget)
                c->stampTarget->AppPresent(xsink::Now());
        }
    }

public:
    StampChain(IDXGISwapChain4* intelChain, std::weak_ptr<Ctx> c) : scproxy::LogSwapChain(intelChain), ctx(std::move(c)) {}
    HRESULT STDMETHODCALLTYPE Present(UINT si, UINT fl) override
    {
        Stamp(fl);
        return r->Present(si, fl);
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* p) override
    {
        Stamp(fl);
        return r->Present1(si, fl, p);
    }
};

inline Ctx::~Ctx()
{
    stampTarget = nullptr;
    if (stampChain)
        stampChain->Release();
    if (sinkFac)
        sinkFac->Release();
}

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
