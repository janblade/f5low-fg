// generators/intel.h - Generator=intel: Intel's libxess_fg makes the generated frames behind the XeFG API (docs/GENERATORS.md, "intel").
// The present-sink shape in "spaced" mode: the library presents into our sink (sink/sink.h), whose always-signalled latency waitable
// neutralises Intel's own pacing, so its burst arrives back to back and the sink spaces it on the real swap chain.
//
// A PRIVATE copy of the player's libxess_fg.dll (and libxell.dll) is used, never the one OptiScaler or the game loaded:
//   - the copies are made under other names (f5low_xefg\f5low_xefg_fg.dll / f5low_xefg_ll.dll), keyed by the SHA-256 of the sources
//     in f5low_xefg\manifest.txt (a changed source is copied again, an unchanged one is not touched), and loaded by full path, so the
//     process never shares the module with OptiScaler's XeFG output;
//   - the unlock (unlock/UnlockXeFg.h: U1-U5 in the fg copy, XL1 in the xell copy) is applied right after the load and BEFORE the
//     first xefgSwapChain* call: Intel sizes its per-generated-frame buffers from U3 when a context is created. This patches inside
//     Intel's DLLs in memory (personal work, user decision 2026-10-16); never a game's or OptiScaler's copy, nothing written to disk;
//   - libxess_fg does not import libxell: it looks it up with GetModuleHandleExA(0, "libxell.dll") and takes whatever module of
//     that name the process has (OptiScaler's, which we must not patch). We chose to redirect that ONE import (the IAT slot of the
//     private libxess_fg) to a thunk of ours that answers "libxell.dll" with the private libxell copy and passes everything else to
//     the real function. Why not rename an import: there is none to rename; why not a load-order trick: OptiScaler hooks the lookup
//     by that name and answers with its own module.
// There is NO fallback to the own generator: when the library cannot start the context is refused (see xefg_cuda.cpp).
// The unlock is on by default (F5LOW_INTEL_UNLOCK=0 turns it off, tests); when it cannot be applied (unknown build, no libxell) the
// library runs unpatched at its own maximum, with the reason in the log.
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

inline constexpr wchar_t kFgCopy[] = L"f5low_xefg_fg.dll";
inline constexpr wchar_t kLlCopy[] = L"f5low_xefg_ll.dll";
inline constexpr wchar_t kFolder[] = L"f5low_xefg";

struct Source
{
    bool ok = false;
    std::wstring fg, ll;        // the player's files (ll may be empty: no libxell found)
    std::string how;
    std::string why;
};

inline std::wstring Trim(std::wstring d)
{
    while (!d.empty() && (d.back() == L'\\' || d.back() == L'/' || d.back() == L' '))
        d.pop_back();
    return d;
}

// Where the player's libxess_fg comes from. `dirs`: candidate folders in search order with a label for the log (F5LOW_XEFG_DIR, the
// folder of XEFG_CUDA_INTEL_LIBRARY, next to f5low.dll, the folder above = OptiScaler's); `skipDir`: our own copy folder (never a source).
// libxell comes from the same folder as libxess_fg, else from the first candidate that has one.
inline Source FindSource(const std::vector<std::pair<std::wstring, const char*>>& dirs, const std::wstring& skipDir)
{
    Source s;
    std::string tried;
    for (const auto& [raw, from] : dirs)
    {
        const std::wstring d = Trim(raw);
        if (d.empty() || (!skipDir.empty() && _wcsicmp(d.c_str(), skipDir.c_str()) == 0))
            continue;
        if (!xdlssg::FileExists(d + L"\\libxess_fg.dll"))
        {
            tried += std::string(tried.empty() ? "" : "; ") + xdlssg::Narrow(d) + " (" + from + ")";
            continue;
        }
        s.fg = d + L"\\libxess_fg.dll";
        std::string llFrom = "next to it";
        if (xdlssg::FileExists(d + L"\\libxell.dll"))
            s.ll = d + L"\\libxell.dll";
        else
            for (const auto& [raw2, from2] : dirs)
            {
                const std::wstring d2 = Trim(raw2);
                if (!d2.empty() && (skipDir.empty() || _wcsicmp(d2.c_str(), skipDir.c_str()) != 0) && xdlssg::FileExists(d2 + L"\\libxell.dll"))
                {
                    s.ll = d2 + L"\\libxell.dll";
                    llFrom = from2;
                    break;
                }
            }
        s.ok = true;
        s.how = "libxess_fg from " + xdlssg::Narrow(s.fg) + " (" + from + "), libxell " +
                (s.ll.empty() ? std::string("not found") : xdlssg::Narrow(s.ll) + " (" + llFrom + ")");
        return s;
    }
    s.why = "no libxess_fg.dll found" + (tried.empty() ? std::string() : " (looked in " + tried + ")");
    return s;
}

// Copies the source file(s) into `dst` under the private names unless the manifest says the copy is current. `copied` counts the files written.
inline bool MakeCopy(const Source& src, const std::wstring& dst, unsigned& copied, std::string& why)
{
    CreateDirectoryW(longpath::ExtendIfLong(dst).c_str(), nullptr);
    if (!xdlssg::DirExists(dst))
    {
        why = "cannot create " + xdlssg::Narrow(dst);
        return false;
    }
    std::vector<uint8_t> mf;
    const std::string old(xdlssg::ReadAll(dst + L"\\manifest.txt", mf) ? std::string(mf.begin(), mf.end()) : std::string());
    std::string manifest;
    copied = 0;
    const std::pair<const std::wstring*, const wchar_t*> files[] = { { &src.fg, kFgCopy }, { &src.ll, kLlCopy } };
    for (const auto& [from, name] : files)
    {
        if (from->empty())
            continue;
        std::vector<uint8_t> data;
        if (!xdlssg::ReadAll(*from, data))
        {
            why = "cannot read " + xdlssg::Narrow(*from);
            return false;
        }
        const std::string sha = xdlssg::HashHex(data);
        if (sha.empty())
        {
            why = "cannot hash " + xdlssg::Narrow(*from);
            return false;
        }
        const std::string line = longpath::ToUtf8(name) + " " + sha;
        const std::wstring to = dst + L"\\" + name;
        bool current = false;
        for (size_t p = 0; p < old.size() && !current;)
        {
            size_t e = old.find('\n', p);
            if (e == std::string::npos)
                e = old.size();
            std::string l = old.substr(p, e - p);
            while (!l.empty() && (l.back() == '\r' || l.back() == ' '))
                l.pop_back();
            current = l == line;
            p = e + 1;
        }
        if (current)
        {
            std::vector<uint8_t> have;     // the manifest says so; the copy itself must still be what it was made from
            current = xdlssg::ReadAll(to, have) && have.size() == data.size() && xdlssg::HashHex(have) == sha;
        }
        if (!current)
        {
            DWORD err = 0;
            if (!xdlssg::WriteAtomic(to, data, &err))
            {
                std::vector<uint8_t> have;     // the same bytes already there (loaded by another process) are as good
                if (!(xdlssg::ReadAll(to, have) && have.size() == data.size() && xdlssg::HashHex(have) == sha))
                {
                    why = "cannot write " + xdlssg::Narrow(to) + " (error " + std::to_string(err) + ")";
                    return false;
                }
            }
            else
                ++copied;
        }
        manifest += line + "\r\n";
    }
    if (manifest != old)
    {
        const std::vector<uint8_t> m(manifest.begin(), manifest.end());
        xdlssg::WriteAtomic(dst + L"\\manifest.txt", m);
    }
    return true;
}

// ---- the redirected libxell lookup -------------------------------------------------------------------------
inline std::atomic<HMODULE> g_privateLl { nullptr };

inline BOOL WINAPI HookGetModuleHandleExA(DWORD flags, LPCSTR name, HMODULE* out)
{
    HMODULE ll = g_privateLl.load();
    if (ll && out && name && !(flags & GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS) && _stricmp(name, "libxell.dll") == 0)
        // the same refcount behaviour the caller asked for, on OUR copy
        return GetModuleHandleExW((flags & GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT) | GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS,
                                  reinterpret_cast<LPCWSTR>(ll), out);
    return GetModuleHandleExA(flags, name, out);
}

// Points the import `fn` of module `m` (by name, any import module) at `repl`. Returns the number of slots changed.
inline unsigned HookImport(HMODULE m, const char* fn, void* repl)
{
    const auto* base = reinterpret_cast<const uint8_t*>(m);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE)
        return 0;
    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS64*>(base + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE || nt->OptionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC)
        return 0;
    const IMAGE_DATA_DIRECTORY dd = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    const size_t imgSize = nt->OptionalHeader.SizeOfImage;
    if (!dd.VirtualAddress || dd.VirtualAddress >= imgSize)
        return 0;
    unsigned n = 0;
    for (const auto* d = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + dd.VirtualAddress); d->Name && d->FirstThunk; ++d)
    {
        const DWORD lookup = d->OriginalFirstThunk ? d->OriginalFirstThunk : d->FirstThunk;
        if (lookup >= imgSize || d->FirstThunk >= imgSize)
            break;
        const auto* ilt = reinterpret_cast<const IMAGE_THUNK_DATA64*>(base + lookup);
        auto* iat = reinterpret_cast<IMAGE_THUNK_DATA64*>(const_cast<uint8_t*>(base) + d->FirstThunk);
        for (; ilt->u1.AddressOfData; ++ilt, ++iat)
        {
            if (IMAGE_SNAP_BY_ORDINAL64(ilt->u1.Ordinal) || ilt->u1.AddressOfData >= imgSize)
                continue;
            const auto* ibn = reinterpret_cast<const IMAGE_IMPORT_BY_NAME*>(base + ilt->u1.AddressOfData);
            if (strcmp(ibn->Name, fn) != 0)
                continue;
            DWORD old = 0;
            if (!VirtualProtect(iat, sizeof(*iat), PAGE_READWRITE, &old))
                continue;
            iat->u1.Function = reinterpret_cast<ULONGLONG>(repl);
            VirtualProtect(iat, sizeof(*iat), old, &old);
            ++n;
        }
    }
    return n;
}

// ---- the private library -----------------------------------------------------------------------------------
struct Lib
{
    HMODULE fg = nullptr, ll = nullptr;
    std::wstring dir;
    std::string how;
    std::string error;                  // not usable: why
    unlock::xefg::Result unlocked;      // ok = the six sites patched
    std::string unlockNote;             // one line for the status text
};
inline Lib g_lib;
inline std::mutex g_libMu;

inline bool UnlockWanted()
{
    char v[8];
    return !(GetEnvironmentVariableA("F5LOW_INTEL_UNLOCK", v, sizeof(v)) && v[0] == '0');
}

// Loads the private copy and applies the unlock, once per process. `selfDir`: the folder of f5low.dll.
inline Lib* Get(const std::wstring& selfDir, const std::vector<std::pair<std::wstring, const char*>>& dirs, std::string& why)
{
    std::lock_guard<std::mutex> lk(g_libMu);
    Lib& l = g_lib;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        unlock::g_log = g_log;
        const std::wstring dst = Trim(selfDir) + L"\\" + kFolder;
        const Source src = FindSource(dirs, dst);
        if (!src.ok)
        {
            l.error = src.why;
        }
        else
        {
            unsigned copied = 0;
            std::string cw;
            if (!MakeCopy(src, dst, copied, cw))
                l.error = cw;
            else
            {
                l.dir = dst;
                l.how = src.how + "; " + (copied ? std::to_string(copied) + " file(s) copied into " : std::string("private copy current in ")) + xdlssg::Narrow(dst);
                // Full paths, never the names: nothing else in the process answers to these
                l.fg = LoadLibraryExW(longpath::Plain(dst + L"\\" + kFgCopy).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                if (!l.fg)
                    l.error = "cannot load the private libxess_fg copy (error " + std::to_string(GetLastError()) + ")";
                else if (!src.ll.empty())
                {
                    l.ll = LoadLibraryExW(longpath::Plain(dst + L"\\" + kLlCopy).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
                    if (!l.ll && g_log)
                        g_log("intel: cannot load the private libxell copy (error %lu)", GetLastError());
                }
            }
        }
        if (l.error.empty())
        {
            // The unlock, before any XeFG call on this module (Intel sizes its per-frame buffers from U3 at context creation)
            if (!UnlockWanted())
                l.unlockNote = "unlock off (F5LOW_INTEL_UNLOCK=0): Intel's own maximum";
            else if (!l.ll)
                l.unlockNote = "unlock not applied: no libxell found - Intel's own maximum";
            else if (HookImport(l.fg, "GetModuleHandleExA", reinterpret_cast<void*>(&HookGetModuleHandleExA)) == 0)
                l.unlockNote = "unlock not applied: the libxell lookup of this libxess_fg could not be redirected - Intel's own maximum";
            else
            {
                g_privateLl.store(l.ll);
                l.unlocked = unlock::xefg::Apply(l.fg, l.ll, 5);
                if (!l.unlocked.ok)
                {
                    g_privateLl.store(nullptr);
                    l.unlockNote = "unlock not applied: " + l.unlocked.reason + " - Intel's own maximum";
                }
                else
                    l.unlockNote = "unlocked in memory to 5 interpolated frames (6x)";
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
    // Frames per application frame the sink expects (called with mu held)
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

// The private library's export for a context's handle (null if h is not one of ours)
inline FARPROC Proc(xefg_swapchain_handle_t h, const char* name)
{
    auto c = Find(h);
    return c && c->lib && c->lib->fg ? GetProcAddress(c->lib->fg, name) : nullptr;
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
