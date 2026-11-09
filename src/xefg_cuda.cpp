// xefg_cuda.cpp, built as f5low.dll (OptiScaler's F5Low frame generation) - our replacement for libxess_fg.dll,
// implementing XeSS-FG's public swap chain API (Intel's SDK headers, included from the OptiScaler checkout).
//
// Stage 1 (this file): every entry point is forwarded to the real libxess_fg.dll (found by RealLibraryPath),
// and every call is logged with its arguments to f5low.log. That proves the DLL loads in place of
// Intel's (OptiScaler.ini: XeFGPath=<full path to f5low.dll>) and records the API inputs -
// frame constants, tagged resources, settings - next to what XeFG does with them, which is what the
// CUDA path needs to compute the network's constants itself. Later stages replace the forwarding.
//
// Use only in games without anti-cheat.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <compressapi.h>
#include <psapi.h>

#include <atomic>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <string>
#include <condition_variable>
#include <mutex>
#include <vector>

#define XEFG_SWAPCHAIN_EXPORT_API
#include "intel_file_source.h"
#include "pack_build.h"
#include "xefg_own.h"
#include "generators/dlssg.h"
#include "generators/intel.h"
#include "xefg_version.h"
#include <xefg_swapchain_debug.h>

namespace
{
HMODULE g_real = nullptr;
std::atomic<bool> g_realFirstLoad { false };    // f5low's load of the shared library (Real()) was the first one in the process (no other loader had it)
std::atomic<bool> g_realContext { false };     // a context was made through g_real (the shared library; generators/intel.h asks before it patches)
FILE* g_log = nullptr;
std::mutex g_logLock;           // opening and the line count; never held for file I/O after the first open
std::mutex g_fileLock;          // the writer thread's (and detach's) file writes
std::mutex g_logQLock;
std::condition_variable g_logCv;
std::string g_logQ;             // formatted lines not yet written
LARGE_INTEGER g_t0, g_freq;
long g_lines = 0;
constexpr long kMaxLines = 400000;
std::wstring g_dir;   // this DLL's folder, any length (set in DllMain, which does no file I/O)
std::wstring g_self;  // this DLL's full path

// %LOCALAPPDATA%\f5low: where f5low writes when its own folder cannot be written (a Game Pass / WindowsApps install, a
// Program Files folder without user rights). Empty when LOCALAPPDATA is not set. The folder is not created here.
std::wstring FallbackDir()
{
    const DWORD n = GetEnvironmentVariableW(L"LOCALAPPDATA", nullptr, 0);
    if (n < 2)
        return {};
    std::wstring base(n, L'\0');
    base.resize(GetEnvironmentVariableW(L"LOCALAPPDATA", base.data(), n));
    if (base.empty())
        return {};
    return base + L"\\f5low";
}

// A setting from f5low.ini ([f5low]) next to this DLL. Without one, xefg_cuda.ini ([xefg_cuda]) from before the DLL
// was renamed to f5low.dll, so an install keeps its settings.
std::wstring IniString(const wchar_t* key, const wchar_t* def)
{
    const std::wstring& dir = g_dir;
    std::wstring file = longpath::ExtendIfLong(dir + L"\\f5low.ini");
    const wchar_t* section = L"f5low";
    if (GetFileAttributesW(file.c_str()) == INVALID_FILE_ATTRIBUTES &&
        GetFileAttributesW(longpath::ExtendIfLong(dir + L"\\xefg_cuda.ini").c_str()) != INVALID_FILE_ATTRIBUTES)
    {
        file = longpath::ExtendIfLong(dir + L"\\xefg_cuda.ini");
        section = L"xefg_cuda";
    }
    wchar_t v[64] = {};
    GetPrivateProfileStringW(section, key, def, v, 64, file.c_str());
    return v;
}

// f5low.log next to this DLL, only with XEFG_CUDA_LOG=1 or Log=1 in f5low.ini (IniString) (off by default: no file
// is created). Opened with the first line.
bool OpenLog()
{
    static bool tried = false;
    if (tried)
        return g_log != nullptr;
    tried = true;
    wchar_t v[16] = {};
    if (!GetEnvironmentVariableW(L"XEFG_CUDA_LOG", v, 16))
        wcsncpy_s(v, IniString(L"Log", L"0").c_str(), _TRUNCATE);
    if (!(v[0] == L'1' || _wcsicmp(v, L"true") == 0 || _wcsicmp(v, L"on") == 0))
        return false;
    _wfopen_s(&g_log, longpath::ExtendIfLong(g_dir + L"\\f5low.log").c_str(), L"w");
    if (!g_log)
    {
        // the DLL's folder is not writable: %LOCALAPPDATA%\f5low\f5low.log instead
        const std::wstring fb = FallbackDir();
        if (!fb.empty())
        {
            CreateDirectoryW(longpath::ExtendIfLong(fb).c_str(), nullptr);
            _wfopen_s(&g_log, longpath::ExtendIfLong(fb + L"\\f5low.log").c_str(), L"w");
            if (g_log)
                fprintf(g_log, "f5low: %ls is not writable: this log is %ls\\f5low.log\n", g_dir.c_str(), fb.c_str());
        }
    }
    if (g_log)
        setvbuf(g_log, nullptr, _IONBF, 0);  // written in batches anyway; nothing left for the CRT to flush at exit
    if (g_log)
        fputs("f5low: native with a pack (xefg_pack\\ next to this DLL, XEFG_CUDA_PACK, or the one built into this "
              "DLL), else forwarding to libxess_fg.dll\n"
              "f5low: version " XEFG_CUDA_VERSION_TEXT ", build " __DATE__ " " __TIME__
              " (V-Sync stands aside, flip metering)\n",
              g_log);
    if (g_log)
    {
        // The file is written by a thread of its own: a write or flush that blocks (a busy disk, a scanner opening the
        // growing file) stalled the game's thread for up to a second in GTA V when it was done in the call. The thread
        // pins this DLL, so it never runs after an unload (with Log on, the DLL stays loaded until the process ends).
        HMODULE self = nullptr;
        const bool pinned = GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_PIN,
                                               reinterpret_cast<LPCWSTR>(&OpenLog), &self) != FALSE;
        const HANDLE writer = !pinned ? nullptr : CreateThread(
            nullptr, 0,
            [](LPVOID) -> DWORD {
                for (;;)
                {
                    std::string out;
                    {
                        std::unique_lock<std::mutex> l(g_logQLock);
                        g_logCv.wait(l, [] { return !g_logQ.empty(); });
                        out.swap(g_logQ);
                    }
                    std::lock_guard<std::mutex> f(g_fileLock);
                    if (!g_log)
                        return 0;
                    fwrite(out.data(), 1, out.size(), g_log);
                    fflush(g_log);  // each batch at once: a process that ends without DLL detach keeps nearly all
                }
            },
            nullptr, 0, nullptr);
        if (writer)
            CloseHandle(writer);
        else
        {
            fclose(g_log);  // no thread (or no pin to keep its code loaded): no log rather than lines piling up
            g_log = nullptr;
        }
    }
    return g_log != nullptr;
}

void LogV(const char* fmt, va_list a)
{
    bool capReached = false;
    {
        std::lock_guard<std::mutex> lk(g_logLock);
        if (!OpenLog() || g_lines > kMaxLines)
            return;
        capReached = ++g_lines > kMaxLines;
    }
    // Formatted here, written by the log thread (OpenLog): the caller is often the game's thread
    std::string line;
    if (capReached)
        line = "-- line cap reached --\n";
    else
    {
        LARGE_INTEGER t;
        QueryPerformanceCounter(&t);
        char head[48];
        const int hn = snprintf(head, sizeof(head), "%10.3f t%lu ",
                                1000.0 * static_cast<double>(t.QuadPart - g_t0.QuadPart) / static_cast<double>(g_freq.QuadPart),
                                GetCurrentThreadId());
        char body[1024];
        va_list a2;
        va_copy(a2, a);
        const int bn = vsnprintf(body, sizeof(body), fmt, a2);
        va_end(a2);
        line.assign(head, hn > 0 ? static_cast<size_t>(hn) : 0);
        if (bn >= static_cast<int>(sizeof(body)))
        {
            std::string big(static_cast<size_t>(bn) + 1, '\0');
            vsnprintf(big.data(), big.size(), fmt, a);
            line.append(big.data(), static_cast<size_t>(bn));
        }
        else if (bn > 0)
            line.append(body, static_cast<size_t>(bn));
        line += '\n';
    }
    {
        std::lock_guard<std::mutex> q(g_logQLock);
        g_logQ += line;
    }
    g_logCv.notify_one();
}

void Log(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    LogV(fmt, a);
    va_end(a);
}

// Native mode: a graph pack next to this DLL (xefg_pack\manifest.txt) or named by XEFG_CUDA_PACK, unless
// XEFG_CUDA_FORWARD is set. Otherwise every call forwards to Intel's library, as in stage 1.
std::map<xefg_swapchain_handle_t, std::shared_ptr<xefgown::Own>> g_own;
std::mutex g_ownLock;

// What f5lowGetStatus() reports (the most recent context's decision): 0 not decided, 1 own generator, 2 forwarding to
// Intel's libxess_fg, 3 unavailable. Own lock: callable from any thread, also before any context.
int g_statusState = 0;
std::string g_statusReason;
std::mutex g_statusLock;

// Each SetStatus is a new decision (the most recent context's): the serial lets a context that decides later (its deferred first
// build) tell whether a newer context has decided since.
uint64_t g_statusSerial = 0;
uint64_t SetStatus(int state, const std::string& reason)
{
    std::lock_guard<std::mutex> lk(g_statusLock);
    g_statusState = state;
    g_statusReason = reason;
    return ++g_statusSerial;
}

void SetStatusIf(uint64_t serial, int state, const std::string& reason)
{
    std::lock_guard<std::mutex> lk(g_statusLock);
    if (g_statusSerial != serial)
        return;
    g_statusState = state;
    g_statusReason = reason;
}

// The pack built into this DLL (build.ps1 -EmbedPack): resource 101 (RCDATA), compressed with the Compression API
// (LZMS), layout as tools/xefg_dll/pack_embed.py writes it. Unpacked once into xefgpack::EmbeddedFiles().
bool LoadEmbeddedPack()
{
    static std::once_flag once;
    static bool ok = false;
    std::call_once(once, [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&LoadEmbeddedPack), &self);
        HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(101), MAKEINTRESOURCEW(10));     // 10 = RT_RCDATA
        if (!res)
            return;
        const void* src = LockResource(LoadResource(self, res));
        const DWORD srcSize = SizeofResource(self, res);
        DECOMPRESSOR_HANDLE d = nullptr;
        if (!src || !CreateDecompressor(COMPRESS_ALGORITHM_LZMS, nullptr, &d))
        {
            Log("embedded pack: cannot open (%lu)", GetLastError());
            return;
        }
        SIZE_T need = 0;
        Decompress(d, src, srcSize, nullptr, 0, &need);
        std::vector<uint8_t> raw(need);
        const bool unpacked = need && Decompress(d, src, srcSize, raw.data(), need, &need);
        CloseDecompressor(d);
        if (!unpacked)
        {
            Log("embedded pack: decompression failed (%lu)", GetLastError());
            return;
        }
        // "XFPK", u32 version 1, u32 count, then u16 name length, name, u64 size, bytes per file.
        size_t at = 0;
        auto take = [&](void* out, size_t n) {
            if (at + n > raw.size())
                return false;
            memcpy(out, raw.data() + at, n);
            at += n;
            return true;
        };
        char magic[4] = {};
        uint32_t version = 0, count = 0;
        if (!take(magic, 4) || memcmp(magic, "XFPK", 4) != 0 || !take(&version, 4) || version != 1 || !take(&count, 4))
        {
            Log("embedded pack: not an XFPK v1 archive");
            return;
        }
        auto& files = xefgpack::EmbeddedFiles();
        for (uint32_t i = 0; i < count; ++i)
        {
            uint16_t nameLen = 0;
            uint64_t size = 0;
            std::string name;
            if (!take(&nameLen, 2))
                break;
            name.resize(nameLen);
            if (!take(name.data(), nameLen) || !take(&size, 8) || at + size > raw.size())
                break;
            files[name].assign(raw.data() + at, raw.data() + at + size);
            at += size;
        }
        ok = files.size() == count && files.count("manifest.txt");
        Log("embedded pack: %u files, %.1f MB from %.1f MB%s", count, raw.size() / 1e6, srcSize / 1e6, ok ? "" : " - INCOMPLETE, not used");
    });
    return ok;
}

std::wstring RealLibraryPath(const char** from);

// The pack-less route: our recipe and kernels are built into this DLL (resource 102, tools/recipe/build_own.py), the Intel parts
// come from the player's libxess_fg.dll. The pack is made IN MEMORY, on a background thread, at every start of the process
// (src/pack_build.cpp): nothing Intel-derived is written to any file. Nothing is translated and no dxcompiler.dll is needed:
// the pack holds Intel's native modules as they are and runs on the D3D12 path only (pack_build.h). The weights and root
// constants are rebuilt from the libxess_fg.dll file (intel_file_source); F5LOW_DEV_CAP (PSO capture log) replaces the derived
// root constants (development).
//
// States (g_packState, under g_packMu; g_packStateA mirrors it for polling):
//   Idle -> Building (StartPackBuild: the first API call that finds no other pack) -> Ready | Failed.
// Nothing waits for the build except xefgSwapChainD3D12CreateContext, for at most kCreateWaitMs; a context made while the pack is
// still being built has its first Build deferred (xefg_own.h, Own::StartDeferredBuild) and passes the frames through until then.
enum PackState { kPackIdle = 0, kPackBuilding, kPackReady, kPackFailed };
std::mutex g_packMu;
std::condition_variable g_packCv;
int g_packState = kPackIdle;                    // under g_packMu
std::atomic<int> g_packStateA { kPackIdle };    // the same, for polling
std::string g_packFail;                         // why it failed (under g_packMu)
LARGE_INTEGER g_packT0 {}, g_packT1 {};         // started / finished (under g_packMu)
constexpr DWORD kCreateWaitMs = 300;            // the longest D3D12CreateContext waits for the VERDICT on the libxess_fg (hard cap)
// The early verdict of the build (pack_build.h BuildPack): 0 pending, 1 this libxess_fg works, 2 it does not (g_verdictWhy). Under
// g_packMu, mirrored for polling. Always set before the pack thread ends.
int g_verdict = 0;
std::string g_verdictWhy;

void SetVerdict(bool ok, const std::string& why)
{
    {
        std::lock_guard<std::mutex> lk(g_packMu);
        if (g_verdict != 0)
            return;
        g_verdict = ok ? 1 : 2;
        g_verdictWhy = why;
    }
    g_packCv.notify_all();
}

// Our recipe and kernels (resource 102), parsed once; read-only afterwards. Null with `err` when this DLL carries none.
const packbuild::Bundle* SharedBundle(std::string* err)
{
    static std::once_flag once;
    static packbuild::Bundle bundle;
    static std::string fail;
    static bool ok = false;
    std::call_once(once, [] {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&SharedBundle), &self);
        HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(102), MAKEINTRESOURCEW(10));
        const uint8_t* bytes = res ? static_cast<const uint8_t*>(LockResource(LoadResource(self, res))) : nullptr;
        const size_t size = res ? SizeofResource(self, res) : 0;
        std::string why;
        ok = bytes && packbuild::ParseBundle(bytes, size, bundle, &why);
        if (!ok)
            fail = "own generator unavailable: no pack found and this DLL carries no recipe (" + (why.empty() ? std::string("no resource") : why) + ")";
    });
    if (!ok && err)
        *err = fail;
    return ok ? &bundle : nullptr;
}

// The oldest GPU (sm_NN) the built pack runs on, from OUR kernels alone (the native modules are DXBC, no PTX): the highest
// ".target sm_NN" over the kernels, each at its fallback's target where the manifest names one (an RTX 20: sm_75). 0 when it cannot
// be told. Needs no libxess_fg, so CreateContext can decide a too old GPU without waiting for the pack.
int BundleNeedSm()
{
    const packbuild::Bundle* b = SharedBundle(nullptr);
    if (!b)
        return 0;
    int need = 0;
    for (const char* pre : { "i0/", "i0/hdr/" })
    {
        const auto m = b->find(std::string(pre) + "manifest.tmpl");
        if (m == b->end())
            continue;
        const std::string t(m->second.begin(), m->second.end());
        std::map<int, std::string> file;       // kernel -> the file that runs on the oldest GPU
        for (size_t at = 0; at < t.size();)
        {
            size_t eol = t.find('\n', at);
            if (eol == std::string::npos)
                eol = t.size();
            char op[16] = {}, name[256] = {};
            int idx = 0;
            if (sscanf_s(t.substr(at, eol - at).c_str(), "%15s %d @%255s", op, 16, &idx, name, 256) == 3 &&
                (!strcmp(op, "kernel") || !strcmp(op, "fallback")))
                file[idx] = name;       // a fallback line follows its kernel line
            at = eol + 1;
        }
        for (const auto& kv : file)
        {
            const auto f = b->find(std::string(pre) + kv.second);
            if (f != b->end())
                need = std::max(need, xefgpack::Pack::PtxSm(f->second));
        }
    }
    return need;
}

double MsSinceStart(const LARGE_INTEGER& t)
{
    return 1000.0 * static_cast<double>(t.QuadPart - g_packT0.QuadPart) / static_cast<double>(g_freq.QuadPart);
}

// Builds the pack from the player's libxess_fg.dll; on the pack thread only. Logs the one failure line.
bool RunBuiltPack(std::shared_ptr<const packbuild::Files>& files, std::string& err)
{
    std::string why;
    const packbuild::Bundle* shared = SharedBundle(&why);
    if (!shared)
    {
        err = why;
        Log("%s", err.c_str());
        return false;
    }
    const packbuild::Bundle& bundle = *shared;
    const char* from = "";
    packbuild::Request req;
    req.libxessPath = RealLibraryPath(&from);
    req.own = &bundle;
    req.verdict = [](bool ok, const std::string& w) {
        SetVerdict(ok, ok ? std::string() : "own generator unavailable: " + w);
        if (!ok)
            Log("verdict: this libxess_fg.dll cannot be used: %s", w.c_str());
    };
    wchar_t w[MAX_PATH] = {}, c[MAX_PATH] = {};
    // Weights and root constants: rebuilt from the player's libxess_fg.dll file (IntelFileSource). Development overrides only:
    // F5LOW_DEV_CAP (a PSO capture) replaces the derived root constants, and F5LOW_DEV_WEIGHTS (a weight dump folder, with
    // F5LOW_DEV_CAP) replaces everything.
    std::unique_ptr<packbuild::IntelDataSource> source;
    std::unique_ptr<packbuild::DevCaptureSource> capSource;
    packbuild::IntelFileSource* fileSource = nullptr;
    const bool haveCap = GetEnvironmentVariableW(L"F5LOW_DEV_CAP", c, MAX_PATH) != 0;
    if (haveCap && GetEnvironmentVariableW(L"F5LOW_DEV_WEIGHTS", w, MAX_PATH))
        source = std::make_unique<packbuild::DevCaptureSource>(w, c);
    else
    {
        const auto it = bundle.find("i0/parts.json");
        const auto mt = bundle.find("i0/manifest.tmpl");
        if (it != bundle.end() && mt != bundle.end())
        {
            auto file = std::make_unique<packbuild::IntelFileSource>(req.libxessPath, it->second, mt->second);
            fileSource = file.get();
            if (haveCap)
            {
                capSource = std::make_unique<packbuild::DevCaptureSource>(std::wstring(c));
                file->SetRootConstantSource(capSource.get());
            }
            source = std::move(file);
        }
    }
    req.data = source.get();
    std::vector<std::string> lines;
    req.log = [&](const std::string& l) { lines.push_back(l); };
    packbuild::Report rep;
    PROCESS_MEMORY_COUNTERS_EX before {};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&before), sizeof before);
    const LARGE_INTEGER t0 = [] { LARGE_INTEGER t; QueryPerformanceCounter(&t); return t; }();
    if (!packbuild::BuildInMemory(req, files, why, &rep))
    {
        err = "own generator unavailable: " + why;
        Log("%s", err.c_str());      // the one line: what is missing
        return false;
    }
    PROCESS_MEMORY_COUNTERS_EX after {};
    K32GetProcessMemoryInfo(GetCurrentProcess(), reinterpret_cast<PROCESS_MEMORY_COUNTERS*>(&after), sizeof after);
    LARGE_INTEGER t1;
    QueryPerformanceCounter(&t1);
    for (const std::string& l : lines)
        Log("%s", l.c_str());
    Log("pack built in memory (nothing written to disk, format %d): %zu files, %.1f MB kept; decode %.0f ms, weights + root "
        "constants %.0f ms, assembly %.0f ms, %.0f ms in all on the pack thread",
        packbuild::kFormatVersion, rep.files, static_cast<double>(rep.bytes) / 1e6, rep.decodeMs, rep.weightsMs, rep.writeMs,
        1000.0 * static_cast<double>(t1.QuadPart - t0.QuadPart) / static_cast<double>(g_freq.QuadPart));
    if (fileSource)
    {
        const auto& st = fileSource->GetStats();
        Log("weights from libxess_fg.dll: %zu uploads rebuilt and hash-checked (read %.0f ms, %zu weight blobs located %.0f ms, %zu vector runs %.0f ms, decode %.0f ms); root constants: %zu blocks derived and hash-checked (%.0f ms)",
            st.uploads, st.readMs, st.blobs, st.blobsMs, st.runs, st.runsMs, st.decodeMs, st.rcBlocks, st.rcMs);
    }
    Log("memory: the pack thread's peak was %.1f MB above where the process stood (peak commit %.1f MB, now %.1f MB, pack kept %.1f MB)",
        after.PeakPagefileUsage > before.PrivateUsage ? static_cast<double>(after.PeakPagefileUsage - before.PrivateUsage) / 1e6 : 0.0,
        static_cast<double>(after.PeakPagefileUsage) / 1e6, static_cast<double>(after.PrivateUsage) / 1e6, static_cast<double>(rep.bytes) / 1e6);
    return true;
}

// The pack thread: builds, publishes the files (xefgpack::SetBuiltFiles, immutable from then on), flips the state, then removes the
// old on-disk caches of earlier versions. It holds a reference on this DLL, so it never runs after an unload.
DWORD WINAPI PackBuildThread(void* mod)
{
    {
        std::shared_ptr<const packbuild::Files> files;
        std::string err;
        const bool ok = RunBuiltPack(files, err);
        SetVerdict(ok, err);        // a no-op when the build gave its verdict already
        if (ok)
            xefgpack::SetBuiltFiles(std::const_pointer_cast<const xefgpack::PackFiles>(files));
        files.reset();      // the loader's copy (BuiltFiles) is the one that stays
        {
            std::lock_guard<std::mutex> lk(g_packMu);
            QueryPerformanceCounter(&g_packT1);
            g_packFail = err;
            g_packState = ok ? kPackReady : kPackFailed;
            g_packStateA.store(g_packState, std::memory_order_release);
        }
        g_packCv.notify_all();
        Log("pack %s %.1f ms after the first API call", ok ? "ready" : "FAILED", [] { std::lock_guard<std::mutex> lk(g_packMu); return MsSinceStart(g_packT1); }());
        // The pack used to be kept on disk (Intel's modules and weights as plain files): delete what an earlier version left.
        std::vector<std::wstring> roots { g_dir + L"\\f5low_cache" };
        if (const std::wstring fb = FallbackDir(); !fb.empty())
            roots.push_back(fb + L"\\f5low_cache");
        for (const std::wstring& r : roots)
            packbuild::RemoveLegacyCaches(r, [](const std::string& l) { Log("%s", l.c_str()); });
    }
    FreeLibraryAndExitThread(static_cast<HMODULE>(mod), 0);
}

// Starts the build once (idempotent). Never blocks; nothing is read on the caller's thread.
void StartPackBuild()
{
    std::lock_guard<std::mutex> lk(g_packMu);
    if (g_packState != kPackIdle)
        return;
    g_packState = kPackBuilding;
    g_packStateA.store(kPackBuilding, std::memory_order_release);
    QueryPerformanceCounter(&g_packT0);
    HMODULE self = nullptr;
    HANDLE t = nullptr;
    // a reference on this DLL for the thread (released by FreeLibraryAndExitThread)
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, reinterpret_cast<LPCWSTR>(&StartPackBuild), &self))
        t = CreateThread(nullptr, 0, PackBuildThread, self, 0, nullptr);
    if (!t)
    {
        if (self)
            FreeLibrary(self);
        g_packFail = "own generator unavailable: the pack thread could not be started";
        g_packState = kPackFailed;
        g_packStateA.store(kPackFailed, std::memory_order_release);
        Log("%s", g_packFail.c_str());
        return;
    }
    CloseHandle(t);
    Log("pack: building in the background");
}

// The pack's state now, as Own::packPoll wants it: 0 still being built, 1 ready, 2 failed (`why` the reason of the failure).
int PackPoll(std::string* why)
{
    const int s = g_packStateA.load(std::memory_order_acquire);
    if (s == kPackFailed && why)
    {
        std::lock_guard<std::mutex> lk(g_packMu);
        *why = g_packFail;
    }
    return s == kPackReady ? 1 : s == kPackFailed ? 2 : 0;
}

// Waits for the build's verdict on the libxess_fg for at most `ms`; returns 0 pending, 1 works, 2 does not (`why` the reason).
int VerdictWait(DWORD ms, std::string* why)
{
    std::unique_lock<std::mutex> l(g_packMu);
    g_packCv.wait_for(l, std::chrono::milliseconds(ms), [] { return g_verdict != 0; });
    if (g_verdict == 2 && why)
        *why = g_verdictWhy;
    return g_verdict;
}

// Waits for the build for at most `ms`; returns the state then.
int PackWait(DWORD ms)
{
    std::unique_lock<std::mutex> l(g_packMu);
    g_packCv.wait_for(l, std::chrono::milliseconds(ms), [] { return g_packState != kPackBuilding; });
    return g_packState;
}

// Where an explicit pack comes from, first that exists: a pack folder next to this DLL (or XEFG_CUDA_PACK) - so a new pack can be
// tried without rebuilding - then the pack embedded in this DLL (personal builds). Those are read from disk / the resource
// as they are. Empty: none; the pack is then built from the player's libxess_fg.dll (kBuiltDir).
std::string ExplicitPackDir()
{
    // UTF-8 from here on (long_path.h): the environment and the DLL's folder are read as wide strings
    std::wstring wdir;
    if (const DWORD n = GetEnvironmentVariableW(L"XEFG_CUDA_PACK", nullptr, 0))
    {
        wdir.resize(n);
        wdir.resize(GetEnvironmentVariableW(L"XEFG_CUDA_PACK", wdir.data(), n));
    }
    else
        wdir = std::wstring(g_dir) + L"\\xefg_pack";
    if (GetFileAttributesW(longpath::ExtendIfLong(wdir + L"\\manifest.txt").c_str()) != INVALID_FILE_ATTRIBUTES)
        return longpath::ToUtf8(wdir);
    if (LoadEmbeddedPack())
        return std::string(xefgpack::kEmbeddedDir);
    return {};
}

// Which pack this process uses.
struct PackRoute
{
    bool forward = false;       // XEFG_CUDA_FORWARD: Intel's library does everything
    std::string dir;            // an explicit pack (folder / embedded), else kBuiltDir
    bool built() const { return !forward && dir == xefgpack::kBuiltDir; }
};
PackRoute ResolvePack()
{
    PackRoute r;
    char env[MAX_PATH];
    if (GetEnvironmentVariableA("XEFG_CUDA_FORWARD", env, sizeof(env)))
    {
        r.forward = true;
        return r;
    }
    r.dir = ExplicitPackDir();
    if (r.dir.empty())
        r.dir = xefgpack::kBuiltDir;
    return r;
}

// The GPU behind `d`: its compute capability as sm_NN (0: no CUDA driver, or no CUDA device behind that adapter - another
// vendor's GPU). Never throws: it runs inside a C API entry point.
int GpuSm(ID3D12Device* d)
{
    // F5LOW_TEST_GPU_SM=NN: pretends another GPU (tests only)
    if (char t[16]; GetEnvironmentVariableA("F5LOW_TEST_GPU_SM", t, sizeof t))
        return atoi(t);
    try
    {
        xefgpack::Cuda cu;
        if (!cu.Load() || cu.Init(0))
            return 0;
        const xefgpack::CUdevice dv = xefgio::Interop::FindDevice(cu, d);
        return dv < 0 ? 0 : xefgio::Interop::DeviceSm(cu, dv);
    }
    catch (...)
    {
        return 0;
    }
}

// The oldest GPU the pack runs on, sm_NN (Pack::NeedSm; Pack::kNoSm when it does not load). Reads the pack's manifests and PTX only
// (no CUDA calls). Never throws.
int PackNeedSm(const std::string& pack)
{
    try
    {
        xefgpack::Cuda cu;      // not loaded: NeedSm makes no CUDA call
        return xefgpack::Pack::NeedSm(cu, pack, xefgown::IsD3dOnlyPack(pack));
    }
    catch (...)
    {
        return xefgpack::Pack::kNoSm;
    }
}

// "" when a GPU of `have` (sm_NN) runs the pack, else why not (the text of the log and the status).
std::string FitReason(int have, int need)
{
    if (!have)
        return "no CUDA device behind this adapter";
    if (need == xefgpack::Pack::kNoSm)
        return "the pack does not load";
    if (have < need)
        return "this GPU is sm_" + std::to_string(have) + ", the pack's kernels need sm_" + std::to_string(need);
    return {};
}

// How the pack runs (xefg_own.h): "d3d12" (default) or "cuda". The CUDA kernels cost ~30% less GPU per
// generated frame, but on a GPU the game keeps busy the CUDA stream gets no priority over the game's work
// and generated frames finish late (our D3D12 present queue is high priority). XEFG_CUDA_PATH, else
// Path= in f5low.ini next to this DLL (IniString).
std::string Setting(const char* env, const char* key, const char* def)
{
    char v[64] = {};
    std::string s;
    if (GetEnvironmentVariableA(env, v, sizeof(v)))
        s = v;
    else
    {
        // Keys, defaults and values are plain ASCII
        const std::wstring value = IniString(std::wstring(key, key + strlen(key)).c_str(),
                                             std::wstring(def, def + strlen(def)).c_str());
        for (const wchar_t c : value)
            s += static_cast<char>(c);
    }
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string PathSetting() { return Setting("XEFG_CUDA_PATH", "Path", "d3d12"); }

// Which generator makes the in-between frames (generator.h, docs/GENERATORS.md): own (default), intel, dlssg or fsr, case
// insensitive. F5LOW_GENERATOR, else XEFG_CUDA_GENERATOR, else Generator= in f5low.ini. Anything else runs own, with a log line;
// so do the ones that are not built yet (xefg_own.h, Own::Create).
std::string GeneratorRaw()
{
    char v[64] = {};
    std::string s;
    if (GetEnvironmentVariableA("F5LOW_GENERATOR", v, sizeof(v)) || GetEnvironmentVariableA("XEFG_CUDA_GENERATOR", v, sizeof(v)))
        s = v;
    else
        s = Setting("F5LOW_GENERATOR", "Generator", "own");     // neither is set: the ini key
    s.erase(0, s.find_first_not_of(" \t\r\n"));
    s.erase(s.find_last_not_of(" \t\r\n") + 1);
    for (char& c : s)
        c = static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return s;
}
std::string GeneratorSetting()
{
    const std::string s = GeneratorRaw();
    if (s == "own" || s == "intel" || s == "dlssg" || s == "fsr")
        return s;
    Log("f5low: generator \"%s\" is unknown (own, intel, dlssg, fsr) - running own", s.c_str());
    return "own";
}

// Where the D3D12 path generates (xefg_own.h, PresentGen): "off" (default: on the present queue, each frame
// just before its present), "direct" (a second graphics queue of its own) or "compute" (async compute; under
// a heavy pixel-shader load it gets few SMs and runs slower: harness 6x tail 4.2 -> 5.8 ms, every frame
// late). In the harness direct paced no better than off (6x sd 2.7-3.6 vs 1.0-1.5 ms; the hand-over between
// the queues costs ~1 ms); the game, where our GPU work waits 5-10 ms, decides. XEFG_CUDA_GENQUEUE, else
// GenQueue= in f5low.ini.
std::string GenQueueSetting() { return Setting("XEFG_CUDA_GENQUEUE", "GenQueue", "off"); }
// The generation lane's queue priority: "high" or "normal"; unset: normal with Gate=1 (the compositor must get the GPU
// between our passes), else high. XEFG_CUDA_GENPRIORITY, else GenPriority= in f5low.ini.
std::string GenPrioritySetting() { return Setting("XEFG_CUDA_GENPRIORITY", "GenPriority", ""); }
// With Gate=1 on the D3D12 path: pass k starts once the present s places before its frame went out (GenSpread = s, 2 by
// default, at most 5), or all of an app frame's passes at once (0). XEFG_CUDA_GENSPREAD, else GenSpread= in f5low.ini.
uint32_t GenSpreadSetting()
{
    const int v = atoi(Setting("XEFG_CUDA_GENSPREAD", "GenSpread", "2").c_str());
    return static_cast<uint32_t>(std::clamp(v, 0, 5));
}

// Pacing under a GPU the game keeps busy (xefg_own.h, Early and PaceTarget). Early=0 (or off/false/no) never takes the
// next app frame before this one's last present: on the CUDA path it starts no generation early, on the D3D12 path it
// takes no inputs early. MaxLag= how far, in app frames, the CUDA path's schedule may run behind the GPU timeline (0.5
// default, 0 none, at most 1). XEFG_CUDA_EARLY / XEFG_CUDA_MAXLAG, else f5low.ini.
bool EarlySetting()
{
    const std::string v = Setting("XEFG_CUDA_EARLY", "Early", "1");
    return v != "0" && v != "off" && v != "false" && v != "no";
}
double MaxLagSetting()
{
    const double v = atof(Setting("XEFG_CUDA_MAXLAG", "MaxLag", "0.5").c_str());
    return std::isfinite(v) ? std::clamp(v, 0.0, 1.0) : 0.5;
}
// The CUDA path's pacer (xefg_own.h, ClockTarget): "clock" (default) releases each frame one spacing after the last,
// a delay behind the GPU that follows how late generated frames come out; "schedule" is the per-app-frame schedule
// (PaceTarget, MaxLag). MaxDelay= the clock's delay at most, in app frames (1.5 default, 0.25-3). The D3D12 path always
// uses the schedule. XEFG_CUDA_PACER / XEFG_CUDA_MAXDELAY, else f5low.ini.
bool ClockSetting() { return Setting("XEFG_CUDA_PACER", "Pacer", "clock") != "schedule"; }
// Gate=1 (or on/true/yes): the CUDA path's clock paces on the GPU (xefg_own.h, ShowGated): the real swap chain on a present
// lane of its own with 8 buffers, each Present at once behind a fence the release thread signals at the frame's time. Only
// with the clock and a swap chain we create (InitFromSwapChainDesc). Off by default. XEFG_CUDA_GATE, else f5low.ini.
bool GateSetting()
{
    const std::string v = Setting("XEFG_CUDA_GATE", "Gate", "0");
    return v == "1" || v == "on" || v == "true" || v == "yes";
}
double MaxDelaySetting()
{
    const double v = atof(Setting("XEFG_CUDA_MAXDELAY", "MaxDelay", "1.5").c_str());
    return std::isfinite(v) ? std::clamp(v, 0.25, 3.0) : 1.5;
}

// F5LOW_SINK_PACE (else SinkPace= in f5low.ini): how the intel generator's sink releases a burst. "auto" (default): the schedule for the
// burst sizes where it is not worse than the burst spacing (sink/sink.h kAutoMaxBurst), else the burst spacing, the choice logged;
// "schedule": always on the own generator's D3D12 schedule from the game's stamped Presents, with PaceLead; "burst": always by the burst
// interval it measured; "off": pass-through, every frame goes out on arrival (after its copy), no burst spacing, no schedule, no stamps (an A/B
// switch: on RTX 40/50 DLSS-G meters its own flips through the driver, so `off` lets that pace them). In every mode a present with V-Sync (sync
// interval > 0) or flags other than tearing is display-paced and goes out on arrival (sink/release_rule.h). Tests only: F5LOW_TEST_SINK_LEADCAP=wide (the lead may reach n steps, not one), F5LOW_TEST_SINK_LEAD_MS=<ms> (a
// fixed lead, not clamped to a step).
// In schedule mode the sink bases each batch on the moment the application's frame was done on the GPU (a fence signalled behind its
// Present), as the own generator does, not on the CPU time of the Present: SinkGpuStamps=0 (F5LOW_SINK_GPUSTAMPS=0) takes the CPU
// times (the A/B switch). The schedule also runs behind the GPU timeline by as much as generated frames came out late, at most MaxLag
// app frames (MaxLag=, XEFG_CUDA_MAXLAG, default 0.5, 0 = none; the same setting as the own generator's).
double PaceLeadSetting(bool* autoLead);
xsink::PaceCfg SinkPaceCfg()
{
    xsink::PaceCfg c;
    const std::string m = Setting("F5LOW_SINK_PACE", "SinkPace", "auto");
    c.mode = m == "burst" ? xsink::PaceMode::Burst : m == "schedule" ? xsink::PaceMode::Schedule : m == "off" ? xsink::PaceMode::Off : xsink::PaceMode::Auto;
    c.leadFixedMs = PaceLeadSetting(&c.leadAuto);
    c.maxLag = MaxLagSetting();
    {
        const std::string g = Setting("F5LOW_SINK_GPUSTAMPS", "SinkGpuStamps", "1");
        c.gpuStamps = g != "0" && g != "off" && g != "false" && g != "no";
    }
    // Test switches: environment only, never the ini (a stray ini line must not change a player's pacing)
    char tv[32] = {};
    c.wideCap = GetEnvironmentVariableA("F5LOW_TEST_SINK_LEADCAP", tv, sizeof(tv)) > 0 && !strcmp(tv, "wide");
    tv[0] = 0;
    const double t = GetEnvironmentVariableA("F5LOW_TEST_SINK_LEAD_MS", tv, sizeof(tv)) > 0 && GetEnvironmentVariableA("F5LOW_TEST_SINK_LEAD_MS", tv, sizeof(tv)) < sizeof(tv) ? atof(tv) : 0.0;
    c.testLeadMs = std::isfinite(t) && t > 0 ? t : 0;
    return c;
}

// PaceLog=1 (XEFG_CUDA_PACELOG, else f5low.ini): a record per present in the log when the presenter stops (needs Log=1; tests/pacer_ab.ps1).
bool PaceFlag(const char* env, const char* key)
{
    const std::string v = Setting(env, key, "0");
    return v == "1" || v == "on" || v == "true" || v == "yes";
}

// PaceLead= (XEFG_CUDA_PACELEAD): the D3D12 path's schedule sends generated frame 1 this long after the app frame is ready, not
// a whole step after it (xefg_own.h, Target; pace_lead.h). auto (default): starts at 3 ms and follows how long generating takes;
// a number of ms (0.5-50): fixed; 0 or off: a whole step. Not on the CUDA path (paceLag and the clock pacer already react to
// late frames there). Returns the fixed ms (0: off); auto sets *autoLead.
double PaceLeadSetting(bool* autoLead)
{
    const std::string v = Setting("XEFG_CUDA_PACELEAD", "PaceLead", "auto");
    *autoLead = v == "auto" || v.empty();
    if (*autoLead || v == "off")
        return 0;
    const double ms = atof(v.c_str());
    return ms > 0 && std::isfinite(ms) ? std::clamp(ms, 0.5, 50.0) : 0;
}

std::shared_ptr<xefgown::Own> FindOwn(xefg_swapchain_handle_t h)
{
    std::lock_guard<std::mutex> lk(g_ownLock);
    auto it = g_own.find(h);
    return it == g_own.end() ? nullptr : it->second;
}

// Generator=dlssg contexts (generators/dlssg.h); the handle is the context's address.
std::map<xefg_swapchain_handle_t, std::shared_ptr<xdlssg::Ctx>> g_dlssg;
std::mutex g_dlssgLock;
std::shared_ptr<xdlssg::Ctx> FindDlssg(xefg_swapchain_handle_t h)
{
    std::lock_guard<std::mutex> lk(g_dlssgLock);
    auto it = g_dlssg.find(h);
    return it == g_dlssg.end() ? nullptr : it->second;
}
// DlssgPath= in f5low.ini, else XEFG_CUDA_DLSSGPATH (a folder with Streamline; generators/dlssg_files.h has the search order). "" = none.
std::wstring DlssgPathSetting()
{
    wchar_t v[MAX_PATH] = {};
    if (!GetEnvironmentVariableW(L"XEFG_CUDA_DLSSGPATH", v, MAX_PATH))
    {
        const std::wstring ini = g_dir + L"\\f5low.ini";
        GetPrivateProfileStringW(L"f5low", L"DlssgPath", L"", v, MAX_PATH, ini.c_str());
        if (!v[0])
            GetPrivateProfileStringW(L"xefg_cuda", L"DlssgPath", L"", v, MAX_PATH, (g_dir + L"\\xefg_cuda.ini").c_str());
    }
    return v;
}
// F5LOW_STREAMLINE_DIR: the Streamline folder OptiScaler passes ("" = not set).
std::wstring StreamlineDirEnv()
{
    wchar_t v[MAX_PATH] = {};
    GetEnvironmentVariableW(L"F5LOW_STREAMLINE_DIR", v, MAX_PATH);
    return v;
}

// The real library: Intel's libxess_fg.dll, loaded on first use. Where from, first one that exists:
//   1. XEFG_CUDA_INTEL_LIBRARY - a full path; OptiScaler's F5Low output passes the copy its XeFG output uses
//      (OptiScaler\libxess_fg.dll or XeFGPath), so one copy serves both
//   2. next to this DLL - the kits, and every install from before (1)
//   3. the folder above this DLL - this DLL in OptiScaler\plugins, Intel's in the OptiScaler folder
std::wstring RealLibraryPath(const char** from)
{
    const std::wstring& self = g_self;      // any length (DllMain)

    const auto exists = [](const std::wstring& p)
    {
        const DWORD a = GetFileAttributesW(longpath::ExtendIfLong(p).c_str());
        return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
    };

    std::wstring env;
    if (const DWORD need = GetEnvironmentVariableW(L"XEFG_CUDA_INTEL_LIBRARY", nullptr, 0))
    {
        env.resize(need);
        env.resize(GetEnvironmentVariableW(L"XEFG_CUDA_INTEL_LIBRARY", env.data(), need));
    }
    // Never ourselves: a path that names this DLL would load it as its own fallback
    if (!env.empty() && _wcsicmp(env.c_str(), self.c_str()) != 0 && exists(env))
    {
        *from = "XEFG_CUDA_INTEL_LIBRARY";
        return env;
    }

    std::wstring folder(self);
    folder.resize(folder.find_last_of(L'\\') + 1);
    if (const std::wstring own = folder + L"libxess_fg.dll"; exists(own))
    {
        *from = "next to this DLL";
        return own;
    }

    std::wstring up = folder.substr(0, folder.size() - 1);
    if (const auto slash = up.find_last_of(L'\\'); slash != std::wstring::npos)
    {
        up = up.substr(0, slash + 1) + L"libxess_fg.dll";
        if (exists(up))
        {
            *from = "the folder above this DLL";
            return up;
        }
    }

    *from = "none found";
    return folder + L"libxess_fg.dll";
}

FARPROC Real(const char* name)
{
    static std::once_flag once;
    std::call_once(once, [] {
        const char* from = "";
        const std::wstring path = RealLibraryPath(&from);
        // The full path: a libxess_fg.dll the game loaded from its own folder is not taken by name instead
        // The process' modules before the load: was it ours that brought the library in? (Generator=intel patches it only then.)
        const std::vector<HMODULE> before = xintel::ProcessModules();
        g_real = LoadLibraryExW(longpath::ExtendIfLong(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        g_realFirstLoad = g_real && !before.empty() && std::find(before.begin(), before.end(), g_real) == before.end();
        Log("real library %S (%s): %p%s", path.c_str(), from, static_cast<void*>(g_real), g_realFirstLoad ? "" : " (already loaded by someone else, or unknown)");
    });
    FARPROC p = g_real ? GetProcAddress(g_real, name) : nullptr;
    if (!p)
        Log("** %s not found in the real library", name);
    return p;
}

#define FORWARD(name, ...)                                                                         \
    static auto fn = reinterpret_cast<decltype(&name)>(Real(#name));                                \
    if (!fn)                                                                                        \
        return XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;                                           \
    const xefg_swapchain_result_t r = fn(__VA_ARGS__)

// The same for a call on a context handle (the handle parameter is always called h): a Generator=intel context goes to the library f5low
// loaded for it (generators/intel.h), looked up once per call site; a proc it lacks is an error, never a fall-back to the shared real
// library. Every other handle goes to the shared real library as above (cached per export, as before).
#define FORWARDH(name, ...)                                                                        \
    decltype(&name) fn = nullptr;                                                                   \
    if (xintel::Find(h))                                                                            \
    {                                                                                               \
        static auto fnIntel = reinterpret_cast<decltype(&name)>(xintel::Proc(#name));               \
        if (!fnIntel)                                                                               \
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;                                       \
        fn = fnIntel;                                                                               \
    }                                                                                               \
    else                                                                                            \
    {                                                                                               \
        static auto fnReal = reinterpret_cast<decltype(&name)>(Real(#name));                        \
        if (!fnReal)                                                                                \
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;                                       \
        fn = fnReal;                                                                                \
    }                                                                                               \
    const xefg_swapchain_result_t r = fn(__VA_ARGS__)

const char* ResName(int t)
{
    static const char* const n[] = { "HUDLESS_COLOR", "DEPTH", "MOTION_VECTOR", "UI", "BACKBUFFER" };
    return t >= 0 && t < 5 ? n[t] : "?";
}

void LogInitParams(const char* what, const xefg_swapchain_d3d12_init_params_t* p)
{
    if (!p)
    {
        Log("  %s: null", what);
        return;
    }
    Log("  %s: appSwapChain=%p flags=0x%x maxInterpolated=%u nodes=%u/%u bufHeap=%p+%llu texHeap=%p+%llu lib=%p uiMode=%d", what,
        static_cast<void*>(p->pApplicationSwapChain), p->initFlags, p->maxInterpolatedFrames, p->creationNodeMask,
        p->visibleNodeMask, static_cast<void*>(p->pTempBufferHeap), p->bufferHeapOffset, static_cast<void*>(p->pTempTextureHeap),
        p->textureHeapOffset, static_cast<void*>(p->pPipelineLibrary), static_cast<int>(p->uiMode));
}
}  // namespace

void XLog(const char* fmt, ...)
{
    va_list a;
    va_start(a, fmt);
    LogV(fmt, a);
    va_end(a);
}

extern "C" {

// ---- common ----------------------------------------------------------------------------------------
// A call from the application that takes over 50 ms (the lock included), logged by name: where a game-thread
// stall sits when the presenter's "app stall" lines put it before our Present.
struct SlowApi
{
    const char* name;
    LARGE_INTEGER t0;
    explicit SlowApi(const char* n) : name(n) { QueryPerformanceCounter(&t0); }
    ~SlowApi()
    {
        static std::atomic<int> logs { 0 };
        LARGE_INTEGER t1, f;
        QueryPerformanceCounter(&t1);
        QueryPerformanceFrequency(&f);
        const double ms = static_cast<double>(t1.QuadPart - t0.QuadPart) * 1000.0 / static_cast<double>(f.QuadPart);
        if (ms > 50 && logs++ < 200)
            XLog("slow call: %s took %.1f ms", name, ms);
    }
};

xefg_swapchain_result_t xefgSwapChainGetVersion(xefg_swapchain_version_t* v)
{
    // The first API call starts the pack's build in the background (it never waits for it). Ours answers unless the build has
    // failed already; Intel's library then answers.
    const PackRoute route = ResolvePack();
    const std::string gen = GeneratorRaw();
    if (route.built() && gen != "intel")
        StartPackBuild();
    if (!route.forward && (gen == "dlssg" || gen == "intel" || !(route.built() && g_packStateA.load(std::memory_order_acquire) == kPackFailed)))
    {
        if (!v)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *v = { 1, 3, 1, 0 };
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainGetVersion, v);
    Log("GetVersion -> %d %u.%u.%u", r, v ? v->major : 0, v ? v->minor : 0, v ? v->patch : 0);
    return r;
}

xefg_swapchain_result_t xefgSwapChainGetProperties(xefg_swapchain_handle_t h, xefg_swapchain_properties_t* p)
{
    if (auto c = FindDlssg(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = {};
        p->maxSupportedInterpolations = c->maxGen;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (FindOwn(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = {};
        p->maxSupportedInterpolations = xefgown::kMaxInterp;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainGetProperties, h, p);
    if (p)
        Log("GetProperties h=%p -> %d descriptors=%u bufHeap=%llu texHeap=%llu cb=%llu maxInterp=%u", static_cast<void*>(h), r,
            p->requiredDescriptorCount, p->tempBufferHeapSize, p->tempTextureHeapSize, p->constantBufferSize,
            p->maxSupportedInterpolations);
    return r;
}

xefg_swapchain_result_t xefgSwapChainTagFrameConstants(xefg_swapchain_handle_t h, uint32_t id, const xefg_swapchain_frame_constant_data_t* c)
{
    SlowApi slow("xefgSwapChainTagFrameConstants");
    if (auto ic = xintel::Find(h))
    {
        // F5LOW_SINK_STATS=1 (tests): the sink's statistics cover frame generation after its start-up (as for dlssg)
        std::lock_guard<std::mutex> lk(ic->mu);
        if (ic->enabled && xsink::g_stats && ++ic->statFrames == 12)
            xsink::ResetGaps();
    }
    if (auto dg = FindDlssg(h))
        return c ? dg->Constants(id, *c) : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    if (auto o = FindOwn(h))
    {
        if (!c)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(o->mu);
        o->constants = *c;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainTagFrameConstants, h, id, c);
    if (c)
    {
        Log("TagFrameConstants id=%u -> %d jitter=%.6f,%.6f mvScale=%.6f,%.6f reset=%u renderTime=%.3f", id, r, c->jitterOffsetX,
            c->jitterOffsetY, c->motionVectorScaleX, c->motionVectorScaleY, c->resetHistory, c->frameRenderTime);
        const float* v = c->viewMatrix;
        const float* pm = c->projectionMatrix;
        Log("  view %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", v[0], v[1], v[2], v[3], v[4], v[5], v[6], v[7], v[8], v[9],
            v[10], v[11], v[12], v[13], v[14], v[15]);
        Log("  proj %g %g %g %g | %g %g %g %g | %g %g %g %g | %g %g %g %g", pm[0], pm[1], pm[2], pm[3], pm[4], pm[5], pm[6], pm[7],
            pm[8], pm[9], pm[10], pm[11], pm[12], pm[13], pm[14], pm[15]);
    }
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetEnabled(xefg_swapchain_handle_t h, uint32_t e)
{
    SlowApi slow("xefgSwapChainSetEnabled");
    if (auto c = FindDlssg(h))
    {
        std::lock_guard<std::mutex> lk(c->mu);
        c->enabled = e != 0;
        c->Apply();
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (auto ic = xintel::Find(h))
    {
        FORWARDH(xefgSwapChainSetEnabled, h, e);
        if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            std::lock_guard<std::mutex> lk(ic->mu);
            ic->enabled = e != 0;
            ic->ApplyBurst();       // only once the library took it
        }
        Log("SetEnabled %u -> %d (intel)", e, r);
        return r;
    }
    if (auto o = FindOwn(h))
    {
        xefgown::Own::XellCall c;
        {
            std::lock_guard<std::mutex> lk(o->mu);
            o->enabled = e != 0;
            c = o->XellSync();
        }
        c.Send();
        o->SetTimerRaised(e != 0);
        Log("SetEnabled %u (own)", e);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainSetEnabled, h, e);
    Log("SetEnabled %u -> %d", e, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetPresentId(xefg_swapchain_handle_t h, uint32_t id)
{
    SlowApi slow("xefgSwapChainSetPresentId");
    if (auto c = FindDlssg(h))
    {
        c->frameId = id;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (auto o = FindOwn(h))
    {
        o->presentId.store(id, std::memory_order_relaxed);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainSetPresentId, h, id);
    Log("SetPresentId %u -> %d", id, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainGetLastPresentStatus(xefg_swapchain_handle_t h, xefg_swapchain_present_status_t* s)
{
    SlowApi slow("xefgSwapChainGetLastPresentStatus");
    if (auto c = FindDlssg(h))
    {
        if (!s)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        c->Status(*s);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (auto o = FindOwn(h))
    {
        if (!s)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        s->framesPresented = o->lastCallPresented;
        s->frameGenResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
        s->isFrameGenEnabled = o->lastGenerated ? 1 : 0;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainGetLastPresentStatus, h, s);
    if (s)
        Log("GetLastPresentStatus -> %d presented=%u enabled=%u", r, s->framesPresented, s->isFrameGenEnabled);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetLoggingCallback(xefg_swapchain_handle_t h, xefg_swapchain_logging_level_t lvl,
                                                        xefg_swapchain_app_log_callback_t cb, void* ud)
{
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARDH(xefgSwapChainSetLoggingCallback, h, lvl, cb, ud);
    Log("SetLoggingCallback level=%d -> %d", static_cast<int>(lvl), r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainDestroy(xefg_swapchain_handle_t h)
{
    {
        std::shared_ptr<xdlssg::Ctx> c;
        {
            std::lock_guard<std::mutex> lk(g_dlssgLock);
            if (auto it = g_dlssg.find(h); it != g_dlssg.end())
            {
                c = std::move(it->second);
                g_dlssg.erase(it);
            }
        }
        if (c)
        {
            Log("Destroy h=%p (dlssg)", static_cast<void*>(h));
            c->Shutdown();
            c.reset();
            xdlssg::LogSinkStats();
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        }
    }
    if (auto ic = xintel::Find(h))
    {
        {
            // before the library lets go of its chain: no more stamps, and our wrapper chain (a reference on Intel's) goes first
            std::lock_guard<std::mutex> lk(ic->mu);
            ic->stampTarget = nullptr;
            if (ic->stampChain)
            {
                ic->stampChain->Release();
                ic->stampChain = nullptr;
            }
        }
        FORWARDH(xefgSwapChainDestroy, h);
        {
            std::lock_guard<std::mutex> lk(xintel::g_ctxMu);
            xintel::g_ctx.erase(h);
        }
        Log("Destroy h=%p -> %d (intel)", static_cast<void*>(h), r);
        ic.reset();         // the library let go of its swap chain (and so the sink); now our factory reference goes
        xintel::LogSinkStats();
        return r;
    }
    std::shared_ptr<xefgown::Own> o;
    {
        std::lock_guard<std::mutex> lk(g_ownLock);
        auto it = g_own.find(h);
        if (it != g_own.end())
        {
            o = std::move(it->second);
            g_own.erase(it);
        }
    }
    if (o)
    {
        Log("Destroy h=%p (own)", static_cast<void*>(h));
        xefgown::Own::XellCall c;
        {
            std::lock_guard<std::mutex> lk(o->mu);
            c = o->XellOff();
        }
        c.Send();
        o->Retire(); // outside g_ownLock: it waits for our GPU work
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainDestroy, h);
    Log("Destroy h=%p -> %d", static_cast<void*>(h), r);
    return r;
}

// Not Intel's API: for OptiScaler (its XeFG/XeLL audit XL23). Intel's library tells XeLL about the frames it generates,
// so XeLL's cap counts shown frames; ours does the same once linked to XeLL (Own::LinkXell), up to 3 generated frames
// per rendered one (XeLL's limit). 1 while a context of ours could not link or makes more (XeLL then caps the game's
// own frames and the caller divides a shown-frame cap by the frames shown per rendered one); 0 otherwise, and when
// forwarding to Intel's library.
__declspec(dllexport) uint32_t xefgCudaXellCountsRenderedOnly()
{
    std::lock_guard<std::mutex> lk(g_ownLock);
    for (const auto& o : g_own)
        if (!o.second->xellCountsShown)
            return 1u;
    // DLSS-G tells XeLL nothing either (it runs Reflex in its own Streamline)
    std::lock_guard<std::mutex> lk2(g_dlssgLock);
    return g_dlssg.empty() ? 0u : 1u;
}

// Not Intel's API: for OptiScaler, right after it loads this DLL when the generator is DLSS-G. Loads OUR private nvngx_dlssg.dll before the
// game's Streamline asks NGX for DLSS-G (NGX marks DLSS-G unavailable process-wide when the only copies it can find are blocked).
// Resolves the Streamline folder as the generator does, makes/refreshes plugins\f5low_streamline, loads our nvngx_dlssg by full path and
// keeps the handle. No slInit, no D3D. May run under the loader lock: only file I/O and LoadLibrary here, no threads, no COM, no waits on
// other threads. Idempotent, thread-safe. Returns 1 when our copy is loaded (or already was), 0 on failure (logged), also when a foreign
// nvngx_dlssg is already in the process (unless F5LOW_DLSSG_ALLOW_FOREIGN=1). The context-time load reuses the same handle.
// NOT patched here: the MFG unlock (RTX 20-40) needs the GPU tier, which comes from a CUDA device behind the game's D3D12 device (cuInit,
// adapter match), not available this early or under the loader lock. It is applied at context time. So on RTX 20-40 the game's own
// DLSS-G must stay off while F5Low DLSS-G is on: the game's Streamline could use this module before it is patched.
extern "C" __declspec(dllexport) uint32_t __cdecl f5lowPrepareDlssg()
{
    static std::mutex mu;
    static bool done = false;
    static uint32_t result = 0;
    std::lock_guard<std::mutex> lk(mu);
    if (done && result)
        return result;
    done = true;
    xdlssg::g_log = XLog;
    xdlssg::g_allowForeignNgx = unlock::EnvIsOne("F5LOW_DLSSG_ALLOW_FOREIGN");
    const xdlssg::Source src = xdlssg::Prepare(g_dir, StreamlineDirEnv(), DlssgPathSetting());
    if (!src.ok)
    {
        Log("dlssg: prepare: %s", src.why.c_str());
        return result = 0;
    }
    Log("dlssg: prepare: %s", src.how.c_str());
    std::wstring ownPath;
    std::string why;
    if (!xdlssg::PreloadOwnNgx(src.dir, ownPath, why))
    {
        Log("dlssg: prepare: %s", why.c_str());
        return result = 0;
    }
    return result = 1;
}

// A foreign nvngx_dlssg found after the context exists (dlssg.h Ctx::NgxCheck): frame generation is off, f5lowGetStatus says 3 with the reason
// The sink's pace config for the dlssg generator, set at load (f5lowPrepareDlssg may never run: older OptiScaler, a harness)
static const bool g_paceCfgHook = (xdlssg::g_paceCfg = SinkPaceCfg, true);
static const bool g_ngxRefusedHook = (xdlssg::g_onRefused = [](const std::string& why) { SetStatus(3, "dlssg unavailable: " + why); }, true);

// Not Intel's API: the host (OptiScaler 0.1.37 and newer) calls this once when it loads the library, before it makes a context, to say
// it hands the async frame markers of the game's own Reflex calls on correctly while DriverPacing's signals run. A DriverPacing key that
// is absent then means automatic (1); without the call it means off. Read at each context's creation (D3D12CreateContext).
static std::atomic<bool> g_hostRoutesMarkers { false };
extern "C" __declspec(dllexport) void __cdecl f5lowHostRoutesMarkers()
{
    g_hostRoutesMarkers.store(true, std::memory_order_release);
}

// Not Intel's API: for OptiScaler's Reflex frame-cap fix (OptiScaler-NR 0.1.36 and newer ask at each cap decision).
// 1 while a Generator=dlssg context runs, or an own-generator context is about to send or holds the frame generation multiplier for the
// driver (NvAPI_D3D_SetReflexSync): DriverPacing=2 or DriverFg with part 1 raise it just before the first send (and take it back if that
// is refused), DriverPacing=1 from the arming of the signals until their disarming is over. The NVIDIA driver then multiplies a Reflex interval on the device by
// that multiplier itself (seen in one game, NBA 2K27: 33333 us read back as 99999 us at 3x), so the caller gives real Reflex the
// shown-frame cap undivided.
__declspec(dllexport) uint32_t xefgCudaDriverMultipliesReflexCap()
{
    xdlssg::g_capAsked.store(true);
    std::lock_guard<std::mutex> lk(g_dlssgLock);
    return g_dlssg.empty() && xefgown::g_driverSignals.load() == 0 ? 0u : 1u;
}

// Not Intel's API: for OptiScaler. What f5low decided for the most recent context: 0 not decided yet, 1 own generator,
// 2 forwarding to Intel's libxess_fg, 3 unavailable. `reason` (if non-null and reasonSize > 0) gets a NUL-terminated
// one-line UTF-8 text, cut to fit. Thread-safe, callable at any time.
extern "C" __declspec(dllexport) int __cdecl f5lowGetStatus(char* reason, int reasonSize)
{
    std::lock_guard<std::mutex> lk(g_statusLock);
    if (reason && reasonSize > 0)
    {
        std::string t = g_statusState == 0 && g_statusReason.empty() ? "not decided yet (no context created)" : g_statusReason;
        size_t n = std::min(t.size(), static_cast<size_t>(reasonSize - 1));
        // do not cut a UTF-8 sequence in the middle
        while (n > 0 && n < t.size() && (static_cast<unsigned char>(t[n]) & 0xC0) == 0x80)
            --n;
        memcpy(reason, t.data(), n);
        reason[n] = 0;
    }
    return g_statusState;
}

xefg_swapchain_result_t xefgSwapChainSetLatencyReduction(xefg_swapchain_handle_t h, void* xell)
{
    SlowApi slow("xefgSwapChainSetLatencyReduction");
    if (FindDlssg(h))
    {
        Log("SetLatencyReduction xell=%p (dlssg: DLSS-G runs Reflex in its own Streamline)", xell);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (auto o = FindOwn(h))
    {
        o->LinkXell(xell).Send();
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainSetLatencyReduction, h, xell);
    Log("SetLatencyReduction xell=%p -> %d", xell, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetSceneChangeThreshold(xefg_swapchain_handle_t h, float t)
{
    SlowApi slow("xefgSwapChainSetSceneChangeThreshold");
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (auto o = FindOwn(h))
    {
        o->sceneThreshold = t;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainSetSceneChangeThreshold, h, t);
    Log("SetSceneChangeThreshold %.4f -> %d", t, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainGetPipelineBuildStatus(xefg_swapchain_handle_t h)
{
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARDH(xefgSwapChainGetPipelineBuildStatus, h);
    Log("GetPipelineBuildStatus -> %d", r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetNumInterpolatedFrames(xefg_swapchain_handle_t h, uint32_t n)
{
    SlowApi slow("xefgSwapChainSetNumInterpolatedFrames");
    if (auto c = FindDlssg(h))
    {
        std::lock_guard<std::mutex> lk(c->mu);
        if (n < 1 || n > c->maxGen)
        {
            Log("SetNumInterpolatedFrames %u: DLSS-G takes 1..%u", n, c->maxGen);
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        }
        c->numInterp = n;
        c->Apply();
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (auto ic = xintel::Find(h))
    {
        FORWARDH(xefgSwapChainSetNumInterpolatedFrames, h, n);
        if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            std::lock_guard<std::mutex> lk(ic->mu);
            ic->numInterp = n;
            ic->ApplyBurst();
        }
        Log("SetNumInterpolatedFrames %u -> %d (intel, max %u)", n, r, ic->maxGen);
        return r;
    }
    if (auto o = FindOwn(h))
    {
        if (n < 1 || n > xefgown::kMaxInterp)
        {
            Log("SetNumInterpolatedFrames %u: only 1..%u is supported", n, xefgown::kMaxInterp);
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        }
        Log("SetNumInterpolatedFrames %u (own, max %u, pack %s)", n, o->maxInterp, "see present log");
        xefgown::Own::XellCall c;
        {
            std::lock_guard<std::mutex> lk(o->mu);
            o->numInterp = n;
            c = o->XellSync();
        }
        c.Send();
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainSetNumInterpolatedFrames, h, n);
    Log("SetNumInterpolatedFrames %u -> %d", n, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetUiCompositionState(xefg_swapchain_handle_t h, xefg_swapchain_ui_composition_state_t s)
{
    SlowApi slow("xefgSwapChainSetUiCompositionState");
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (auto o = FindOwn(h))
    {
        o->uiComposition = s == XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED;
        Log("SetUiCompositionState %d (own)", static_cast<int>(s));
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainSetUiCompositionState, h, s);
    Log("SetUiCompositionState %d -> %d", static_cast<int>(s), r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainEnableDebugFeature(xefg_swapchain_handle_t h, xefg_swapchain_debug_feature_t f, uint32_t e, void* arg)
{
    SlowApi slow("xefgSwapChainEnableDebugFeature");
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (auto o = FindOwn(h))
    {
        Log("EnableDebugFeature %d enable=%u (own)", static_cast<int>(f), e);
        if (f == XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES)
            o->dbgTag = e != 0;
        else if (f == XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION)
            o->dbgOnly = e != 0;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;       // PRESENT_FAILED_INTERPOLATION: nothing fails here
    }
    FORWARDH(xefgSwapChainEnableDebugFeature, h, f, e, arg);
    Log("EnableDebugFeature %d enable=%u -> %d", static_cast<int>(f), e, r);
    return r;
}

// ---- D3D12 -----------------------------------------------------------------------------------------
xefg_swapchain_result_t xefgSwapChainD3D12CreateContext(ID3D12Device* d, xefg_swapchain_handle_t* ph)
{
    const auto callStart = std::chrono::steady_clock::now();
    const PackRoute route = ResolvePack();
    // Generator=dlssg (generators/dlssg.h): DLSS-G through a private renamed Streamline, presenting into our sink. DlssgGenerator::Caps
    // decides: no NVIDIA GPU, below RTX 20, Streamline missing, the unlock not recognising this DLSS-G build or slInit failing ->
    // one log line, status 3 with the reason, and an error from CreateContext. There is NO fallback to the own generator (user
    // 2026-11-02: when the player picks DLSS-G it runs or frame generation is off with the reason shown; switching to XeFG Tensor
    // is the player's choice, never automatic). XEFG_CUDA_FORWARD wins over everything (Intel's library does it all).
    const bool wantDlssg = !route.forward && d && ph && GeneratorRaw() == "dlssg";
    if (wantDlssg)
    {
        xdlssg::g_log = XLog;
        xsink::g_stats = xsink::EnvSet("F5LOW_SINK_STATS");     // tests: the sink's statistics into the log
        const int gpuSm = GpuSm(d);
        xefgown::DlssgGenerator dg(gpuSm);
        std::wstring dir;
        if (dg.Caps().available)
        {
            const xdlssg::Source src = xdlssg::Prepare(g_dir, StreamlineDirEnv(), DlssgPathSetting());
            if (src.ok)
            {
                Log("dlssg: %s", src.how.c_str());
                dir = src.dir;
            }
            else
                dg.Fail(src.why);
        }
        std::shared_ptr<xdlssg::Ctx> c;
        if (dg.Caps().available)
        {
            std::string why;
            c = xdlssg::Create(d, dir, gpuSm, why);
            if (c)
                dg.SetMax(c->maxGen);
            else
                dg.Fail(why);
        }
        if (c)
        {
            *ph = reinterpret_cast<xefg_swapchain_handle_t>(c.get());
            {
                std::lock_guard<std::mutex> lk(g_dlssgLock);
                g_dlssg[*ph] = c;
            }
            SetStatus(1, "dlssg generator (DLSS-G through a private Streamline copy, up to " + std::to_string(dg.Caps().maxGenerated) + " generated), D3D12");
            Log("f5low: generator dlssg (%s)", dg.Caps().name);
            Log("D3D12CreateContext device=%p -> Generator=dlssg: DLSS-G h=%p, Streamline from %S, DLSS-G makes the frames, our sink paces them (SinkPace)", static_cast<void*>(d),
                static_cast<void*>(*ph), c->sl->dir.c_str());
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        }
        // No own context is made. UNSUPPORTED_DEVICE is the XeFG API's "this GPU/driver cannot do it" and is what OptiScaler already
        // treats as "frame generation unavailable"; the exact reason travels in f5lowGetStatus (status 3).
        const std::string reason = std::string("dlssg unavailable: ") + dg.Caps().reason;
        SetStatus(3, reason);
        Log("f5low: generator dlssg not available (%s) - frame generation is off (no fallback to own)", dg.Caps().reason);
        return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
    }
    // Generator=intel (generators/intel.h): Intel's libxess_fg (the one the own generator reads), unlocked in memory, presenting into our sink in
    // spaced mode. NO fallback to the own generator (same rule as dlssg): when the library cannot start, frame generation is off with
    // the reason in f5lowGetStatus (3) and an error from CreateContext.
    if (!route.forward && d && ph && GeneratorRaw() == "intel")
    {
        xintel::g_log = XLog;
        xsink::g_stats = xsink::EnvSet("F5LOW_SINK_STATS");     // tests: the sink's statistics into the log
        xsink::g_log = XLog;
        xefgown::IntelGenerator ig;
        std::string why;
        std::shared_ptr<xintel::Ctx> c;
        const char* srcFrom = "";
        const std::wstring srcPath = RealLibraryPath(&srcFrom);     // exactly where the own generator reads libxess_fg from
        if (xintel::Lib* lib = xintel::Get(srcPath, srcFrom, g_self, g_real, g_realFirstLoad.load(), g_realContext.load(), why))
        {
            Log("intel: %s", lib->how.c_str());
            Log("intel: %s", lib->unlockNote.c_str());
            auto create = reinterpret_cast<decltype(&xefgSwapChainD3D12CreateContext)>(GetProcAddress(lib->fg, "xefgSwapChainD3D12CreateContext"));
            auto props = reinterpret_cast<decltype(&xefgSwapChainGetProperties)>(GetProcAddress(lib->fg, "xefgSwapChainGetProperties"));
            xefg_swapchain_handle_t nh = nullptr;
            xefg_swapchain_result_t cr = XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;
            if (!create || !props)
                why = "the libxess_fg lacks the XeFG API";
            else if ((cr = create(d, &nh)) != XEFG_SWAPCHAIN_RESULT_SUCCESS || !nh)
                why = "the library's CreateContext failed (" + std::to_string(static_cast<int>(cr)) + ")";
            else
            {
                c = std::make_shared<xintel::Ctx>();
                c->lib = lib;
                c->h = nh;
                xefg_swapchain_properties_t pr {};
                if (props(nh, &pr) == XEFG_SWAPCHAIN_RESULT_SUCCESS && pr.maxSupportedInterpolations)
                    c->maxGen = pr.maxSupportedInterpolations;
                ig.SetMax(c->maxGen);
                std::lock_guard<std::mutex> lk(xintel::g_ctxMu);
                xintel::g_ctx[nh] = c;
            }
        }
        if (c)
        {
            *ph = c->h;
            SetStatus(1, "intel generator (XeFG from libxess_fg, up to " + std::to_string(c->maxGen) + " generated; " + c->lib->unlockNote + "), D3D12");
            Log("f5low: generator intel (%s)", ig.Caps().name);
            Log("D3D12CreateContext device=%p -> Generator=intel: h=%p, the library makes up to %u generated frames, the sink paces its burst (spaced)",
                static_cast<void*>(d), static_cast<void*>(*ph), c->maxGen);
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        }
        ig.Fail(why);
        SetStatus(3, "intel unavailable: " + why);
        Log("f5low: generator intel not available (%s) - frame generation is off (no fallback to own)", why.c_str());
        return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
    }
    std::string pack = route.forward ? std::string() : route.dir;
    std::string fwdReason;      // why this context forwards (f5lowGetStatus)
    int need = 0, have = 0;
    bool deferred = false;      // the pack is made in the background and so is this context's first build (xefg_own.h)
    if (route.forward)
        fwdReason = "forwarding to Intel XeFG: XEFG_CUDA_FORWARD is set";
    else if (route.built() && d && ph)
    {
        // The pack is built in memory on a thread of its own (started by the first API call, else here). Two things are decided
        // here, neither needing the finished pack:
        //  - the GPU: no CUDA device behind the adapter, or older than our own kernels need (BundleNeedSm, from the DLL's own
        //    PTX) -> Intel's library at once;
        //  - the libxess_fg: the build's early VERDICT (decoded, every module found, the weights located: ~60-90 ms into the
        //    build), waited for at most kCreateWaitMs counted from the start of this call. A no -> Intel's library.
        // Only a verdict that has not come by then leaves us making our own context (the frames pass through until the pack is
        // ready; a failure after that ends in status 3, the context being ours).
        StartPackBuild();
        have = GpuSm(d);
        need = BundleNeedSm();
        std::string misfit;
        std::string vwhy;
        int verdict = 0;
        if (!have || (need && have < need))
        {
            misfit = FitReason(have, need);
            fwdReason = "forwarding to Intel XeFG: " + misfit;
            Log("D3D12CreateContext: %s - forwarding to libxess_fg.dll", misfit.c_str());
            pack.clear();
        }
        else
        {
            // the cap counts from the start of this call (the GPU check above has used part of it)
            const auto used = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - callStart).count();
            verdict = VerdictWait(used >= kCreateWaitMs ? 0 : static_cast<DWORD>(kCreateWaitMs - used), &vwhy);
            const int st = g_packStateA.load(std::memory_order_acquire);
            if (verdict == 2)
            {
                fwdReason = (vwhy.empty() ? std::string("own generator unavailable: no pack") : vwhy) + " - forwarding to Intel XeFG";
                pack.clear();
            }
            else if (st == kPackReady && !(misfit = FitReason(have, need = PackNeedSm(pack))).empty())
            {
                fwdReason = "forwarding to Intel XeFG: " + misfit;
                Log("D3D12CreateContext: %s - forwarding to libxess_fg.dll", misfit.c_str());
                pack.clear();
            }
            else
            {
                deferred = true;
                if (verdict == 0)
                    Log("D3D12CreateContext: no verdict on the libxess_fg.dll after %lu ms - making our own context, the frames pass through until the pack is ready", kCreateWaitMs);
            }
        }
    }
    else if (!pack.empty() && d && ph)
    {
        // An explicit pack (folder, embedded): a GPU its kernels cannot run on (an older architecture, or not NVIDIA) gets
        // Intel's library from the start: found only at the build, the pack's failure left frame generation off on our own
        // swap chain.
        need = PackNeedSm(pack);
        have = GpuSm(d);
        if (const std::string misfit = FitReason(have, need); !misfit.empty())
        {
            Log("D3D12CreateContext: %s - forwarding to libxess_fg.dll", misfit.c_str());
            fwdReason = "forwarding to Intel XeFG: " + misfit;
            pack.clear();
        }
    }
    if (!pack.empty() && d && ph)
    {
        // a pack built on this PC runs on the D3D12 path only (the generator says so once, at its build)
        const std::string path = route.built() ? std::string("d3d12") : PathSetting();
        auto o = xefgown::Own::Create(d, pack, path, GeneratorSetting());
        if (!o)
        {
            SetStatus(3, "own generator unavailable: the context could not be created");
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
        }
        o->genMode = GenQueueSetting();
        o->genPriority = GenPrioritySetting();
        o->presentNormal = Setting("XEFG_CUDA_PRESENTPRIORITY", "PresentPriority", "high") == "normal";
        o->genSpread = GenSpreadSetting();
        o->noEarly = !EarlySetting();
        o->maxLag = MaxLagSetting();
        o->clockPacer = ClockSetting();
        o->maxDelay = MaxDelaySetting();
        o->gate = GateSetting();
        o->paceLog = PaceFlag("XEFG_CUDA_PACELOG", "PaceLog") && OpenLog();     // nothing to record where nothing is logged
        o->paceLeadFixedMs = PaceLeadSetting(&o->paceLeadAuto);
        // FlipMetering= (XEFG_CUDA_FLIPMETERING): NVIDIA driver flip metering (xefg_own.h, InitFlip, Meter). "0" off, "1" on any GPU (tests),
        // the default "auto" only where the display hardware meters flips (oob_driver.h, FlipMeteringOn: RTX 50 / sm_100 and newer).
        // Decided here for the settings below (DriverPacing, the burst); InitFlip checks it again on the generator's GPU.
        o->flipMeteringSetting = Setting("XEFG_CUDA_FLIPMETERING", "FlipMetering", "auto");
        {
            bool known = true;
            oobdriver::FlipMeteringForced(o->flipMeteringSetting.c_str(), &known);
            if (!known)
                XLog("warning: FlipMetering=%s is not auto, 0 or 1 (or on/off, true/false, yes/no) - taken as auto", o->flipMeteringSetting.c_str());
        }
        o->flipMetering = oobdriver::FlipMeteringOn(o->flipMeteringSetting.c_str(), have);
        if (!o->flipMetering)      // Meter then never reaches InitFlip, so the reason is logged here
        {
            if (oobdriver::FlipMeteringForced(o->flipMeteringSetting.c_str()) == 0)
                XLog("flip metering: off - FlipMetering=%s in the settings", o->flipMeteringSetting.c_str());
            else if (have <= 0)
                XLog("flip metering: off - no NVIDIA compute device for this adapter");
            else
                XLog("flip metering: off - this GPU (sm_%d) has no hardware flip metering (GeForce RTX 50 and newer), f5low times every frame itself; FlipMetering=1 forces it", have);
        }
        // GenBlock (XEFG_CUDA_GENBLOCK): as DLSS-G on D3D12, a generation thread makes each frame's generated frames at once on the
        // generation lane as the frame arrives, and the game's queue waits for them (xefg_own.h StartGenAhead): its next frame never
        // runs ahead of our passes, and a present never waits for a pass (on GPUs that switch to our queue slowly - RTX 20 in a game -
        // each pass otherwise waited behind the game's next frame). "auto" (default): on where f5low times every frame itself (no
        // hardware flip metering: RTX 20/30/40, or FlipMetering=0). Not yet with metering: NBA 2K27 on an RTX 5070 Ti with the
        // automatic driver pacing then sat at 14 fps in some runs (27.7 in others); with DriverPacing=0 it ran 30 fps every run.
        // "1" on, "0" off. D3D12 path with our own swap chain and Gate=0 only (the stats line says whether it is on).
        {
            const std::string gb = Setting("XEFG_CUDA_GENBLOCK", "GenBlock", "auto");
            const bool on = gb == "1" || gb == "on" || gb == "true" || gb == "yes";
            const bool off = gb == "0" || gb == "off" || gb == "false" || gb == "no";
            if (!on && !off && gb != "auto")
                XLog("warning: GenBlock=%s is not auto, 0 or 1 (or on/off, true/false, yes/no) - taken as auto", gb.c_str());
            o->genBlock = on || (!off && !o->flipMetering);
            // What a context then does (on, or off and why) is logged when its presenter starts (xefg_own.h StartGenAhead).
            XLog("GenBlock=%s: %s", gb.c_str(), o->genBlock ? "wanted" : off ? "off" : "off (flip metering is on for this GPU)");
        }
        // LateGen (XEFG_CUDA_LATEGEN): "auto" (default): where f5low times every frame itself (flip metering off for this context: RTX
        // 20/30/40, or FlipMetering=0) a generated frame whose own GPU work did not finish in its wait is left out, and on a timed batch (not
        // V-Sync) so is one that is stale (the game's next frame is finished and its slot is past); with flip metering on it is "show".
        // "show": every generated frame is presented, late or not. Experiments: "early" = auto plus the early-out (a slot wait, the real
        // frame's too, ends once the game's next frame is finished; in this presenter that is often already so, so it is not a sign of
        // lateness); "compress": while the game's next frame is finished a timed batch's presents go out at half-step spacing instead of at
        // their slots, and only a frame a whole game frame behind (the next two frames finished) is left out; "drop": also leaves out a
        // frame more than one step past its time (not on a metered, burst or V-Sync batch: there only a GPU timeout drops). The real
        // frame is always shown. Paths that present through PresentReal (xefg_own.h; not Gate=1).
        {
            std::string lg = Setting("XEFG_CUDA_LATEGEN", "LateGen", "auto");
            if (lg != "show" && lg != "drop" && lg != "auto" && lg != "compress" && lg != "early")
            {
                XLog("warning: LateGen=%s is not auto, show, drop, compress or early - taken as auto", lg.c_str());
                lg = "auto";
            }
            o->lateMode = lg == "show" ? 0 : lg == "drop" ? 2 : lg == "compress" ? 3 : lg == "early" ? 4 : 1;
            XLog("LateGen=%s: %s", lg.c_str(),
                 o->lateMode == 2 ? "generated frames that time out or are over a step late are left out"
                 : o->lateMode == 3 ? "a late batch is squeezed; only a frame a whole game frame behind is left out"
                 : o->lateMode == 0 ? "every generated frame is shown"
                 : o->flipMetering ? "same as show while flip metering is on for this GPU (if the driver path turns out unusable it is logged and the rules apply)"
                 : o->lateMode == 4 ? "a generated frame that timed out or is stale is left out, and slot waits end when the game's next frame is done (flip metering is off)"
                                    : "a generated frame that timed out or is stale is left out (flip metering is off)");
        }
        // FixedPacing (XEFG_CUDA_FIXEDPACING): "auto" (default), "display" or "timer": who spaces the frames when the game asks for V-Sync
        // (xefg_own.h fixedMode, pace_sched.h FixedTimer). timer = f5low waits for each frame's time on the plain schedule; display = the
        // display does. auto = timer where flip metering is off for this context (the condition LateGen=auto uses), display where the
        // context uses flip metering (the driver meters nothing under V-Sync itself); InitFlip logs again if it turns out unusable. Real V-Sync only
        // (sync interval >= 1). Not with Gate=1.
        {
            std::string fp = Setting("XEFG_CUDA_FIXEDPACING", "FixedPacing", "auto");
            if (fp != "auto" && fp != "display" && fp != "timer")
            {
                XLog("warning: FixedPacing=%s is not auto, display or timer - taken as auto", fp.c_str());
                fp = "auto";
            }
            o->fixedMode = fp == "timer" ? pacesched::kFixedTimer : fp == "display" ? pacesched::kFixedDisplay : pacesched::kFixedAuto;
            if (o->gate)
            {
                if (o->fixedMode == pacesched::kFixedTimer)
                    XLog("FixedPacing=timer is not used with Gate=1 (the gate paces by itself) - display");
                else if (o->fixedMode == pacesched::kFixedAuto)
                    XLog("FixedPacing=auto: display (Gate=1 paces by itself)");
            }
            else
                XLog("FixedPacing=%s: %s", fp.c_str(),
                     o->fixedMode == pacesched::kFixedTimer ? "each frame waits for its time with V-Sync too (the plain schedule)"
                     : o->fixedMode == pacesched::kFixedDisplay ? "with V-Sync the display spaces the frames"
                     : !o->flipMetering ? "timer: flip metering is off for this GPU, f5low times every frame itself with V-Sync too"
                                        : "display for now (this context uses flip metering; timer if the driver path turns out unusable, logged then)");
        }
        // OobQueue=1 / OobMarkers=1 (XEFG_CUDA_OOBQUEUE, XEFG_CUDA_OOBMARKERS): low-level, off by default (oob_driver.h).
        const std::string oobQueue = Setting("XEFG_CUDA_OOBQUEUE", "OobQueue", "0");
        o->oobQueue = oobQueue == "1";
        o->oobSwitch = oobQueue == "switch";
        // DriverFg=1 / DriverFgParts= / FlipBurst=1 (XEFG_CUDA_DRIVERFG, XEFG_CUDA_DRIVERFGPARTS, XEFG_CUDA_FLIPBURST): low-level, off by
        // default. DriverPacing (XEFG_CUDA_DRIVERPACING) sets the whole set: the driver signals (all parts) and the burst; 1 runs it
        // only while the GPU is the limit (automatic, xefg_own.h PacingFrame), 2 always, 0 off. The key absent: automatic when the host said
        // it routes the markers (f5lowHostRoutesMarkers), else off. Gate=1 and a GPU that is not NVIDIA's veto it all (oob_driver.h ResolveDriver).
        {
            // The key absent (no ini line, no environment variable) is -1: automatic when the host routes the markers, else off.
            const std::string pacingText = Setting("XEFG_CUDA_DRIVERPACING", "DriverPacing", "");
            const int pacing = pacingText.empty() ? -1 : std::clamp(atoi(pacingText.c_str()), 0, 2);
            const bool hostRoutes = g_hostRoutesMarkers.load(std::memory_order_acquire);
            // NVIDIA: `have` is the CUDA device behind this adapter (GpuSm, matched by LUID): every context that reaches here has one
            // (a GPU without goes to Intel's library above), so the GPU is read the way the generator reads it, with no DXGI of our own.
            const bool nvidia = have > 0;
            const oobdriver::DriverSettings r = oobdriver::ResolveDriver(
                o->gate, pacing, Setting("XEFG_CUDA_DRIVERFG", "DriverFg", "0") == "1",
                static_cast<uint32_t>(atoi(Setting("XEFG_CUDA_DRIVERFGPARTS", "DriverFgParts", "15").c_str())),
                Setting("XEFG_CUDA_FLIPBURST", "FlipBurst", "0") == "1", hostRoutes, nvidia, o->flipMetering);
            o->driverFg = r.driverFg;
            o->driverFgParts = r.parts;
            o->flipBurst = r.flipBurst;
            o->pacingMode = static_cast<uint32_t>(r.mode);
            o->pacingAuto = r.pacingAuto;
            o->pacingActive.store(!r.pacingAuto);       // DriverPacing=1 starts off: nothing is sent until the rule turns it on
            // PacingFlipSec=N (XEFG_CUDA_PACINGFLIPSEC): test only, flips the requested state every N seconds, ignoring the rule.
            o->pacingFlipSec = r.pacingAuto ? std::max(0.0, atof(Setting("XEFG_CUDA_PACINGFLIPSEC", "PacingFlipSec", "0").c_str())) : 0.0;
            // The resolved mode and why, once per context.
            {
                const char* why = "";
                switch (r.why)
                {
                case oobdriver::PacingWhy::kExplicitOff: why = "DriverPacing=0 in the settings"; break;
                case oobdriver::PacingWhy::kExplicit: why = "DriverPacing set in the settings"; break;
                case oobdriver::PacingWhy::kDefaultAuto: why = "no DriverPacing key (the default) and the host routes the frame markers"; break;
                case oobdriver::PacingWhy::kDefaultOffOldHost: why = "no DriverPacing key (the default), but this host did not say it routes the frame markers (an OptiScaler before 0.1.37)"; break;
                case oobdriver::PacingWhy::kGateVeto: why = "Gate=1 vetoes it"; break;
                case oobdriver::PacingWhy::kNotNvidia: why = "no NVIDIA compute device for this adapter"; break;
                case oobdriver::PacingWhy::kNoMetering: why = "no flip metering (a GPU before GeForce RTX 50, or FlipMetering=0): f5low times every frame itself"; break;
                }
                XLog("DriverPacing resolved: mode %d (%s) - %s", r.mode, r.mode == 0 ? "off" : r.mode == 1 ? "automatic" : "always on", why);
            }
            if (r.nvidiaOff || (!nvidia && (o->oobQueue || o->oobSwitch)))
            {
                // every low-level driver key follows (OobMarkers is read just below and cleared there too)
                o->oobQueue = false;
                o->oobSwitch = false;
                XLog("driver signals, burst and out of band notices off: no NVIDIA compute device for this adapter - the swap chain stays the plain one");
            }
            static std::atomic<bool> oldHostWarned { false };       // once per process, not per context
            if (r.oldHostWarn && !oldHostWarned.exchange(true))
                XLog("warning: DriverPacing=%d is set, but this host did not say it routes the frame markers: an OptiScaler before 0.1.37 takes them for the game's own Reflex calls", pacing);
            const char* vetoBy = o->gate ? "Gate=1 is on" : "no flip metering (a GPU before GeForce RTX 50, or FlipMetering=0)";
            if (r.pacingIgnored && !r.nvidiaOff)
                XLog("DriverPacing=%d ignored: %s", pacing, vetoBy);
            if (r.burstIgnored)
                XLog("FlipBurst=1 ignored: %s", vetoBy);
            if (r.pacingAuto)
                XLog("DriverPacing=1: automatic - driver signals (all parts) and the burst run only while the GPU is the limit (load >= 0.75 for 2 s on, <= 0.55 for 2 s off)%s",
                     o->pacingFlipSec > 0 ? ", PacingFlipSec set: the rule is replaced by a flip every N seconds (test)" : "");
            else if (r.mode == 2)
                XLog("DriverPacing=2: driver signals (all parts) always; the burst (a metered batch's presents after the first released at once) while the GPU is the limit (load >= 0.75 for 2 s on, <= 0.55 for 2 s off)");
            else if (o->flipBurst)
                XLog("FlipBurst=1: a metered batch's presents after the first are released at once");
        }
        // AppJobs= / RetireWait=0 (XEFG_CUDA_APPJOBS, XEFG_CUDA_RETIREWAIT): low-level, where the game's Present blocks (xefg_own.h, OnPresent).
        o->appJobs = std::clamp(static_cast<uint32_t>(atoi(Setting("XEFG_CUDA_APPJOBS", "AppJobs", "2").c_str())), 1u, 6u);
        o->retireWait = Setting("XEFG_CUDA_RETIREWAIT", "RetireWait", "1") != "0";
        // WaitBoundMs= / GameWaitMs= (XEFG_CUDA_WAITBOUNDMS, XEFG_CUDA_GAMEWAITMS): the bounds of the presenter path's waits (wait_bound.h): GPU fence
        // and allocator waits (default 500 ms), the game's Present waiting for room in the presenter's queue (default 500 ms).
        // (not a number, 0 or negative: the default, not a clamp end)
        const auto boundMs = [&](const char* env, const char* key, const char* defText, int def, int lo, int hi) {
            const int v = atoi(Setting(env, key, defText).c_str());
            return static_cast<unsigned>(v <= 0 ? def : std::clamp(v, lo, hi));
        };
        o->waitBoundMs = boundMs("XEFG_CUDA_WAITBOUNDMS", "WaitBoundMs", "500", 500, 50, 5000);
        // Diagnostics (xefg_own.h, TestStall): TestStallFrame=<app frame, 0 = off>, TestStallMs=<ms>, TestStallKind=presenter|genlane|presentlane.
        o->testStallFrame = static_cast<uint64_t>(std::max(0LL, atoll(Setting("XEFG_CUDA_TESTSTALLFRAME", "TestStallFrame", "0").c_str())));
        o->testStallMs = std::clamp(static_cast<unsigned>(atoi(Setting("XEFG_CUDA_TESTSTALLMS", "TestStallMs", "1500").c_str())), 50u, 20000u);
        {
            const std::string k = Setting("XEFG_CUDA_TESTSTALLKIND", "TestStallKind", "presenter");
            o->testStallKind = k == "genlane" ? 1 : k == "presentlane" ? 2 : 0;
        }
        // Diagnostics (xefg_own.h, Submit): XEFG_CUDA_TESTCLOSEFAIL=<n> (or TestCloseFail= in f5low.ini): the n-th Close of one of our lists counts as
        // failed, to run the failed-list path (generation off until the swap chain is rebuilt) in a game. Debug only.
        o->testCloseFail = static_cast<unsigned>(std::max(0, atoi(Setting("XEFG_CUDA_TESTCLOSEFAIL", "TestCloseFail", "0").c_str())));
        if (o->testCloseFail)
            XLog("WARNING: XEFG_CUDA_TESTCLOSEFAIL=%u - Close number %u of our lists will be treated as FAILED (debug only; generation then goes off)",
                 o->testCloseFail, o->testCloseFail);
        o->gameWaitMs = boundMs("XEFG_CUDA_GAMEWAITMS", "GameWaitMs", "500", 500, 10, 2000);
        o->oobMarkers = have > 0 && Setting("XEFG_CUDA_OOBMARKERS", "OobMarkers", "0") == "1";      // (no NVIDIA compute device: off with the rest)
        // FlipLeadUs= (XEFG_CUDA_FLIPLEADUS): how early metered frames after a batch's first are released, 0-8000 us. auto (default):
        // 2500 us, less at a high rate - never more than 0.45 of the frame spacing (xefg_own.h, FlipLead). Not used while the burst is on.
        std::string lead = Setting("XEFG_CUDA_FLIPLEADUS", "FlipLeadUs", "auto");
        lead.erase(0, lead.find_first_not_of(" \t\r\n"));
        lead.erase(lead.find_last_not_of(" \t\r\n") + 1);
        std::transform(lead.begin(), lead.end(), lead.begin(), [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
        o->flipLeadAuto = lead.empty() || lead == "auto";
        if (!o->flipLeadAuto)
            o->flipLeadUs = static_cast<uint32_t>(std::clamp(atoi(lead.c_str()), 0, 8000));
        // Prof= in f5low.ini (XEFG_CUDA_PROF): 0 no GPU timing (default), 1 per run, 2 per dispatch too.
        const std::string prof = Setting("XEFG_CUDA_PROF", "Prof", "0");
        o->gs.prof = prof != "0";
        o->gs.profDispatches = prof == "2";
        // Native= (XEFG_CUDA_NATIVE): 0 runs the pack's native modules (the reconstruction) as PTX on D3D12 too.
        // (the pack built on this PC runs Intel's native modules whatever Native= says: set here so the generator, which would
        // otherwise write it on the build thread, never has to)
        o->gs.native = route.built() || Setting("XEFG_CUDA_NATIVE", "Native", "1") != "0";
        *ph = reinterpret_cast<xefg_swapchain_handle_t>(o.get());
        {
            std::lock_guard<std::mutex> lk(g_ownLock);
            g_own[*ph] = o;
        }
        {
            const char* src = pack == xefgpack::kEmbeddedDir ? "embedded pack" : pack == xefgpack::kBuiltDir ? "pack built in memory" : "pack folder";
            if (deferred)
            {
                // status 0 until the first build is done on its own thread; then 1 (or 3: a late failure, no way back to Intel's)
                const uint64_t serial = SetStatus(0, "building its frame generator in the background");
                const std::string from = src;
                o->packPoll = &PackPoll;
                o->packFit = [have] { return FitReason(have, PackNeedSm(xefgpack::kBuiltDir)); };
                o->onAbandoned = [serial] { SetStatusIf(serial, 0, "no context (destroyed before its generator was ready)"); };
                o->deferredDone = [serial, from](bool ok, const std::string& why) {
                    if (ok)
                        Log("deferred first build: ready, generation can start");
                    else
                        Log("deferred first build: FAILED - passing frames through: %s", why.c_str());
                    if (ok)
                        SetStatusIf(serial, 1, "own generator (" + from + "), D3D12");
                    else
                        SetStatusIf(serial, 3, (why.compare(0, 25, "own generator unavailable") == 0 ? why : "own generator unavailable: " + why));
                };
            }
            else
                SetStatus(1, std::string("own generator (") + src + "), D3D12");
        }
        Log("D3D12CreateContext device=%p -> own h=%p, pack %s (needs sm_%d, GPU sm_%d), path %s, early %d, pacer %s%s (CUDA path), max lag "
            "%.2f, max delay %.2f app frames",
            static_cast<void*>(d), static_cast<void*>(*ph), pack.c_str(), need, have, path.c_str(), o->noEarly ? 0 : 1,
            o->clockPacer ? "clock" : "schedule", o->gate ? " + gate" : "", o->maxLag, o->maxDelay);
        const std::string leadMode = o->paceLeadAuto ? std::string("auto (3.0 ms start)")
                                     : o->paceLeadFixedMs > 0 ? "fixed " + std::to_string(o->paceLeadFixedMs).substr(0, 5) + " ms" : std::string("off");
        Log("pace lead: %s%s", leadMode.c_str(), path == "d3d12" || leadMode == "off" ? "" : " (inactive: CUDA path)");
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    const std::string fwdText = fwdReason.empty() ? std::string("forwarding to Intel XeFG") : fwdReason;
    if (!Real("xefgSwapChainD3D12CreateContext"))
        SetStatus(3, fwdReason.empty() ? std::string("libxess_fg.dll not found, nothing to forward to")
                                       : fwdReason + "; libxess_fg.dll not found");
    else if (d && ph)
        SetStatus(2, fwdText);
    FORWARD(xefgSwapChainD3D12CreateContext, d, ph);
    if (r == XEFG_SWAPCHAIN_RESULT_SUCCESS && ph && *ph)
        g_realContext = true;
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS && d && ph && Real("xefgSwapChainD3D12CreateContext"))
        SetStatus(3, "Intel XeFG CreateContext failed (" + std::to_string(static_cast<int>(r)) + ") after hand-over: " + fwdText);
    Log("D3D12CreateContext device=%p -> %d h=%p", static_cast<void*>(d), r, ph ? static_cast<void*>(*ph) : nullptr);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12BuildPipelines(xefg_swapchain_handle_t h, ID3D12PipelineLibrary* lib, uint8_t blocking,
                                                         uint32_t flags)
{
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (auto o = FindOwn(h))
    {
        o->initFlags = flags;
        const bool ok = o->EnsureBuilt();
        Log("D3D12BuildPipelines flags=0x%x -> %s", flags, ok ? "built" : "FAILED");
        return ok ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
    }
    FORWARDH(xefgSwapChainD3D12BuildPipelines, h, lib, blocking, flags);
    Log("D3D12BuildPipelines lib=%p blocking=%u flags=0x%x -> %d", static_cast<void*>(lib), blocking, flags, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12GetProperties(xefg_swapchain_handle_t h, const xefg_swapchain_d3d12_init_params_t* ip,
                                                        uint32_t w, uint32_t ht, DXGI_FORMAT f, xefg_swapchain_properties_t* p)
{
    if (auto c = FindDlssg(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = {};
        p->maxSupportedInterpolations = c->maxGen;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (FindOwn(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = {};
        p->maxSupportedInterpolations = xefgown::kMaxInterp;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainD3D12GetProperties, h, ip, w, ht, f, p);
    Log("D3D12GetProperties %ux%u fmt=%d -> %d descriptors=%u bufHeap=%llu texHeap=%llu maxInterp=%u", w, ht, static_cast<int>(f), r,
        p ? p->requiredDescriptorCount : 0, p ? p->tempBufferHeapSize : 0, p ? p->tempTextureHeapSize : 0,
        p ? p->maxSupportedInterpolations : 0);
    LogInitParams("params", ip);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12InitFromSwapChain(xefg_swapchain_handle_t h, ID3D12CommandQueue* q,
                                                            const xefg_swapchain_d3d12_init_params_t* ip)
{
    if (FindDlssg(h))
    {
        Log("D3D12InitFromSwapChain: Generator=dlssg needs InitFromSwapChainDesc (Streamline makes the swap chain)");
        return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    }
    if (auto o = FindOwn(h))
    {
        IDXGISwapChain4* app = nullptr;
        if (!q || !ip || !ip->pApplicationSwapChain || FAILED(ip->pApplicationSwapChain->QueryInterface(IID_PPV_ARGS(&app))))
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        o->initFlags = ip->initFlags;
        o->initParams = *ip;
        {
            xefgown::Own::XellCall c;
            {
                std::lock_guard<std::mutex> lk(o->mu);
                o->maxInterp = std::max(1u, std::min<uint32_t>(ip->maxInterpolatedFrames, xefgown::kMaxInterp));
                c = o->XellSync();  // a count set before init may not fit any more (or fit now)
            }
            c.Send();
        }
        o->initParams.maxInterpolatedFrames = o->maxInterp;
        if (!o->SetQueue(q))
        {
            app->Release();
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
        }
        o->Wrap(app);       // owns the QI reference
        Log("D3D12InitFromSwapChain queue=%p flags=0x%x (own)", static_cast<void*>(q), ip->initFlags);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainD3D12InitFromSwapChain, h, q, ip);
    Log("D3D12InitFromSwapChain queue=%p -> %d%s", static_cast<void*>(q), r, xintel::Find(h) ? " (intel: the game's own chain, no sink - the library paces itself)" : "");
    LogInitParams("params", ip);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12InitFromSwapChainDesc(xefg_swapchain_handle_t h, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, ID3D12CommandQueue* q,
                                                                IDXGIFactory2* fac, const xefg_swapchain_d3d12_init_params_t* ip)
{
    if (auto c = FindDlssg(h))
        return c->InitFromDesc(wnd, d, fs, q, fac, ip);
    if (auto ic = xintel::Find(h))
    {
        // The library creates its swap chain through OUR factory (the sink, spaced mode): its presents land in the sink
        IDXGIFactory4* f4 = nullptr;
        if (!d || !q || !fac || !ip || FAILED(fac->QueryInterface(IID_PPV_ARGS(&f4))))
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        auto* sf = new xsink::SinkFactory(f4, false);
        f4->Release();
        sf->SetStampQueue(q);       // the application's queue (Intel presents on a queue of its own): the sink's GPU stamps are signalled on it
        {
            xsink::PaceCfg pc = SinkPaceCfg();
            if (pc.mode == xsink::PaceMode::Off)
            {
                // Off is for dlssg (DLSS-G's own flip metering paces there); Intel presents its bursts back to back and has nothing that spaces them
                pc.mode = xsink::PaceMode::Burst;
                Log("intel: SinkPace=off is for dlssg: intel uses burst spacing");
            }
            ic->schedule = pc.mode != xsink::PaceMode::Burst && pc.mode != xsink::PaceMode::Off;       // the game's Presents are stamped unless the burst spacing is forced or the pacing is off
            sf->SetPace(pc);
        }
        IDXGIFactory2* sf2 = nullptr;
        if (FAILED(sf->QueryInterface(IID_PPV_ARGS(&sf2))))
        {
            sf->Release();
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN;
        }
        FORWARDH(xefgSwapChainD3D12InitFromSwapChainDesc, h, wnd, d, fs, q, sf2, ip);
        sf2->Release();
        Log("D3D12InitFromSwapChainDesc %ux%u fmt=%d buffers=%u effect=%d flags=0x%x queue=%p -> %d (intel, through the sink factory%s)", d->Width, d->Height,
            static_cast<int>(d->Format), d->BufferCount, static_cast<int>(d->SwapEffect), d->Flags, static_cast<void*>(q), r, sf->HasSink() ? ", sink made" : ", no sink yet");
        LogInitParams("params", ip);
        if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS)
        {
            sf->Release();
            return r;
        }
        std::lock_guard<std::mutex> lk(ic->mu);
        // a re-init: the chain the game holds wraps the OLD Intel chain; it is dropped here and made anew at GetSwapChainPtr
        if (ic->stampChain)
        {
            ic->stampChain->Release();
            ic->stampChain = nullptr;
        }
        xsink::SinkFactory* old = ic->sinkFac;
        ic->sinkFac = sf;
        ic->stampTarget = sf;           // the new target first, then the old factory goes
        if (old)
            old->Release();
        ic->ApplyBurst();
        Log("intel: sink pacing %s", !ic->schedule ? "burst (forced: the sink's measured burst interval)"
                                      : "auto (the game's Presents are stamped; own D3D12 schedule + PaceLead where it is not worse than the burst spacing) or forced schedule");
        return r;
    }
    if (auto o = FindOwn(h))
    {
        if (!q || !d || !fac || !ip)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        // The real swap chain presents on our own queue, so generated frames are not queued behind the app's work.
        IDXGISwapChain1* sc1 = nullptr;
        IDXGISwapChain4* sc4 = nullptr;
        ID3D12CommandQueue* pq = o->PresentQueue();
        DXGI_SWAP_CHAIN_DESC1 dd = *d;
        o->GateDesc(dd);                // Gate=1 or the burst: more buffers, the waitable object (the game still sees what it asked for)
        const HRESULT hr = pq ? fac->CreateSwapChainForHwnd(pq, wnd, &dd, fs, nullptr, &sc1) : E_FAIL;
        if (FAILED(hr) || FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc4))))
        {
            if (sc1)
                sc1->Release();
            // 0x80070005 (E_ACCESSDENIED): the window still has a swap chain
            Log("D3D12InitFromSwapChainDesc: CreateSwapChainForHwnd failed 0x%08lx (%ux%u fmt=%d buffers=%u flags=0x%x%s)",
                static_cast<unsigned long>(hr), dd.Width, dd.Height, static_cast<int>(dd.Format), dd.BufferCount, dd.Flags,
                pq ? "" : ", no present queue");
            return XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL;
        }
        sc1->Release();
        o->initFlags = ip->initFlags;
        o->initParams = *ip;
        {
            xefgown::Own::XellCall c;
            {
                std::lock_guard<std::mutex> lk(o->mu);
                o->maxInterp = std::max(1u, std::min<uint32_t>(ip->maxInterpolatedFrames, xefgown::kMaxInterp));
                c = o->XellSync();  // a count set before init may not fit any more (or fit now)
            }
            c.Send();
        }
        o->initParams.maxInterpolatedFrames = o->maxInterp;
        o->initParams.pApplicationSwapChain = sc4;
        if (!o->SetQueue(q))
        {
            sc4->Release();
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
        }
        o->NoteRequested(d->BufferCount, d->Flags);     // before Wrap: what the game asked for is what it is shown
        o->Wrap(sc4);
        Log("D3D12InitFromSwapChainDesc %ux%u fmt=%d buffers=%u effect=%d flags=0x%x queue=%p initFlags=0x%x (own)", d->Width, d->Height,
            static_cast<int>(d->Format), d->BufferCount, static_cast<int>(d->SwapEffect), d->Flags, static_cast<void*>(q), ip->initFlags);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainD3D12InitFromSwapChainDesc, h, wnd, d, fs, q, fac, ip);
    if (d)
        Log("D3D12InitFromSwapChainDesc %ux%u fmt=%d buffers=%u effect=%d flags=0x%x windowed=%d queue=%p -> %d", d->Width, d->Height,
            static_cast<int>(d->Format), d->BufferCount, static_cast<int>(d->SwapEffect), d->Flags, fs ? fs->Windowed : -1,
            static_cast<void*>(q), r);
    LogInitParams("params", ip);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12GetSwapChainPtr(xefg_swapchain_handle_t h, REFIID riid, void** pp)
{
    if (auto c = FindDlssg(h))
    {
        std::lock_guard<std::mutex> lk(c->mu);
        if (!c->chain || !pp)
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;
        return SUCCEEDED(c->chain->QueryInterface(riid, pp)) ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    }
    if (auto o = FindOwn(h))
    {
        std::lock_guard<std::mutex> lk(o->proxyMu); // a Destroy on another thread may drop the proxy meanwhile (Retire)
        IDXGISwapChain4* p = o->proxy;
        if (!p || !pp)
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;
        return SUCCEEDED(p->QueryInterface(riid, pp)) ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    }
    if (auto ic = xintel::Find(h); ic && ic->schedule && pp)
    {
        // Schedule mode: the game gets a thin wrapper over Intel's chain that stamps its Presents (xintel::StampChain)
        std::lock_guard<std::mutex> lk(ic->mu);
        if (!ic->stampChain)
        {
            IDXGISwapChain4* sc4 = nullptr;
            FORWARDH(xefgSwapChainD3D12GetSwapChainPtr, h, IID_PPV_ARGS(&sc4));
            if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS || !sc4)
            {
                if (sc4)
                    sc4->Release();
                return r != XEFG_SWAPCHAIN_RESULT_SUCCESS ? r : XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;
            }
            ic->stampChain = new xintel::StampChain(sc4, ic);       // owns the reference
        }
        return SUCCEEDED(ic->stampChain->QueryInterface(riid, pp)) ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    }
    FORWARDH(xefgSwapChainD3D12GetSwapChainPtr, h, riid, pp);
    Log("D3D12GetSwapChainPtr -> %d sc=%p", r, pp ? *pp : nullptr);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12TagFrameResource(xefg_swapchain_handle_t h, ID3D12CommandList* cl, uint32_t id,
                                                           const xefg_swapchain_d3d12_resource_data_t* rd)
{
    SlowApi slow("xefgSwapChainD3D12TagFrameResource");
    if (auto c = FindDlssg(h))
        return rd ? c->Tag(cl, id, *rd) : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    if (auto o = FindOwn(h))
    {
        if (!rd)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        static long n = 0;
        if (n++ < 12)
        {
            D3D12_RESOURCE_DESC desc {};
            if (rd->pResource)
                desc = rd->pResource->GetDesc();
            Log("TagFrameResource id=%u %s base=%u,%u size=%ux%u (%llux%u fmt=%d) state=0x%x (own)", id, ResName(rd->type),
                rd->resourceBase.x, rd->resourceBase.y, rd->resourceSize.x, rd->resourceSize.y, desc.Width, desc.Height,
                static_cast<int>(desc.Format), rd->incomingState);
        }
        return o->Tag(cl, *rd);
    }
    FORWARDH(xefgSwapChainD3D12TagFrameResource, h, cl, id, rd);
    if (rd)
    {
        D3D12_RESOURCE_DESC desc {};
        if (rd->pResource)
            desc = rd->pResource->GetDesc();
        Log("TagFrameResource id=%u %s validity=%d base=%u,%u size=%ux%u res=%p (%llux%u fmt=%d) state=0x%x list=%p -> %d", id,
            ResName(rd->type), static_cast<int>(rd->validity), rd->resourceBase.x, rd->resourceBase.y, rd->resourceSize.x,
            rd->resourceSize.y, static_cast<void*>(rd->pResource), desc.Width, desc.Height, static_cast<int>(desc.Format),
            rd->incomingState, static_cast<void*>(cl), r);
    }
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12SetDescriptorHeap(xefg_swapchain_handle_t h, ID3D12DescriptorHeap* heap, uint32_t off)
{
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARDH(xefgSwapChainD3D12SetDescriptorHeap, h, heap, off);
    Log("D3D12SetDescriptorHeap heap=%p offset=%u -> %d", static_cast<void*>(heap), off, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12UpdateExternalHeapOnResize(xefg_swapchain_handle_t h, ID3D12Heap* bh, uint64_t bo,
                                                                     ID3D12Heap* th, uint64_t to)
{
    if (FindDlssg(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARDH(xefgSwapChainD3D12UpdateExternalHeapOnResize, h, bh, bo, th, to);
    Log("D3D12UpdateExternalHeapOnResize -> %d", r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12GetInitializationParameters(xefg_swapchain_handle_t h, xefg_swapchain_d3d12_init_params_t* p)
{
    if (auto c = FindDlssg(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(c->mu);
        *p = c->initParams;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    if (auto o = FindOwn(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = o->initParams;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARDH(xefgSwapChainD3D12GetInitializationParameters, h, p);
    Log("D3D12GetInitializationParameters -> %d", r);
    LogInitParams("params", p);
    return r;
}

}  // extern "C"

// Dred=1 in f5low.ini (or XEFG_CUDA_DRED=1), diagnostics: D3D12's crash breadcrumbs and page-fault report, written
// to the log on the first failed Present (Own::PresentFailed). They must be on before the game creates its device:
// OptiScaler loads us about a second before that, and this thread (DllMain may not read files) turns them on.
DWORD WINAPI DredOn(void*)
{
    wchar_t v[16] = {};
    if (!GetEnvironmentVariableW(L"XEFG_CUDA_DRED", v, 16))
        wcsncpy_s(v, IniString(L"Dred", L"0").c_str(), _TRUNCATE);
    if (!(v[0] == L'1' || _wcsicmp(v, L"true") == 0 || _wcsicmp(v, L"on") == 0))
        return 0;
    using GetDebugFn = HRESULT(WINAPI*)(REFIID, void**);
    HMODULE d3d = LoadLibraryW(L"d3d12.dll");
    auto get = d3d ? reinterpret_cast<GetDebugFn>(GetProcAddress(d3d, "D3D12GetDebugInterface")) : nullptr;
    ID3D12DeviceRemovedExtendedDataSettings* s = nullptr;
    if (get && SUCCEEDED(get(IID_PPV_ARGS(&s))))
    {
        s->SetAutoBreadcrumbsEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s->SetPageFaultEnablement(D3D12_DRED_ENABLEMENT_FORCED_ON);
        s->Release();
        Log("dred: on (breadcrumbs + page faults, for devices created from now on)");
    }
    else
        Log("dred: could not be turned on");
    return 0;
}

BOOL WINAPI DllMain(HINSTANCE self, DWORD reason, LPVOID)
{
    if (reason == DLL_PROCESS_ATTACH)
    {
        DisableThreadLibraryCalls(self);
        QueryPerformanceFrequency(&g_freq);
        QueryPerformanceCounter(&g_t0);
        {
            // any length: grow the buffer until the name fits
            std::wstring path(MAX_PATH, L'\0');
            for (;;)
            {
                const DWORD n = GetModuleFileNameW(self, path.data(), static_cast<DWORD>(path.size()));
                if (n == 0)
                {
                    path.clear();
                    break;
                }
                if (n < path.size())
                {
                    path.resize(n);
                    break;
                }
                if (path.size() >= 32768)
                {
                    path.clear();
                    break;
                }
                path.resize(path.size() * 2);
            }
            path = longpath::Plain(path);       // a DLL loaded as "\\?\C:\..." (a long path): the ordinary form, as callers expect
            g_self = path;
            const size_t cut = path.find_last_of(L'\\');
            g_dir = cut == std::wstring::npos ? path : path.substr(0, cut);
        }
        if (HANDLE t = CreateThread(nullptr, 0, DredOn, nullptr, 0, nullptr))
            CloseHandle(t);
    }
    else if (reason == DLL_PROCESS_DETACH && g_log)
    {
        // Only at process exit (the log thread pins this DLL): that thread is already gone, maybe killed inside a write
        // with our lock or the FILE's held. So the locks are only tried and the FILE is used without its own; what the
        // thread did not get to is written here, or left when a lock is held.
        std::unique_lock<std::mutex> f(g_fileLock, std::try_to_lock);
        std::unique_lock<std::mutex> q(g_logQLock, std::try_to_lock);
        if (f.owns_lock() && q.owns_lock())
        {
            _fwrite_nolock(g_logQ.data(), 1, g_logQ.size(), g_log);
            _fflush_nolock(g_log);
            g_logQ.clear();
        }
    }
    return TRUE;
}
