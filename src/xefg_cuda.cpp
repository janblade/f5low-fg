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
#include "xefg_version.h"
#include <xefg_swapchain_debug.h>

namespace
{
HMODULE g_real = nullptr;
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
std::string g_builtFailReason;      // why RunBuiltPack made no pack (under g_statusLock)
std::mutex g_statusLock;

void SetStatus(int state, const std::string& reason)
{
    std::lock_guard<std::mutex> lk(g_statusLock);
    g_statusState = state;
    g_statusReason = reason;
}

void SetBuiltFail(const std::string& reason)
{
    std::lock_guard<std::mutex> lk(g_statusLock);
    g_builtFailReason = reason;
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

// The pack-less route (story B2): our recipe and kernels are built into this DLL (resource 102, tools/recipe/build_own.py),
// the Intel parts come from the player's libxess_fg.dll on first use and the finished pack is cached in
// <DLL dir>\f5low_cache\<key>\. Nothing is translated and no dxcompiler.dll is needed: the pack holds Intel's native
// modules as they are and runs on the D3D12 path only (pack_build.h). The weights and root constants are rebuilt from the
// libxess_fg.dll file (intel_file_source); F5LOW_DEV_CAP (PSO capture log) replaces the derived root constants (development).
// Runs once, at the first PackDir() call that finds no other pack; the result is kept.
// The folder is UTF-8 (long_path.h). `fromCache`: EnsurePack found it finished (a damaged one is deleted and made again,
// RepairBuiltPack). All callers hold g_builtLock.
std::mutex g_builtLock;
std::string g_builtDir;
bool g_builtFromCache = false;
bool g_builtRan = false;

void RunBuiltPack()
{
    std::string& result = g_builtDir;
    {
        HMODULE self = nullptr;
        GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                           reinterpret_cast<LPCWSTR>(&RunBuiltPack), &self);
        HRSRC res = FindResourceW(self, MAKEINTRESOURCEW(102), MAKEINTRESOURCEW(10));
        const uint8_t* bytes = res ? static_cast<const uint8_t*>(LockResource(LoadResource(self, res))) : nullptr;
        const size_t bytesSize = res ? SizeofResource(self, res) : 0;
        static packbuild::Bundle bundle;
        bundle.clear();
        std::string err;
        if (!bytes || !packbuild::ParseBundle(bytes, bytesSize, bundle, &err))
        {
            Log("own generator unavailable: no pack found and this DLL carries no recipe (%s)", err.empty() ? "no resource" : err.c_str());
            SetBuiltFail("own generator unavailable: no pack found and this DLL carries no recipe (" + (err.empty() ? std::string("no resource") : err) + ")");
            return;
        }
        const std::wstring dir(g_dir);
        const char* from = "";
        packbuild::Request req;
        req.libxessPath = RealLibraryPath(&from);
        req.cacheRoot = dir + L"\\f5low_cache";
        // not writable: %LOCALAPPDATA%\f5low\f5low_cache (the folder name stays: DiscardPack checks it)
        if (const std::wstring fb = FallbackDir(); !fb.empty())
            req.fallbackRoot = fb + L"\\f5low_cache";
        req.own = &bundle;
        req.bundleBytes = bytes;
        req.bundleSize = bytesSize;
        wchar_t w[MAX_PATH] = {}, c[MAX_PATH] = {};
        // Weights and root constants: rebuilt from the player's libxess_fg.dll file (IntelFileSource). Development
        // overrides only: F5LOW_DEV_CAP (a PSO capture) replaces the derived root constants, and F5LOW_DEV_WEIGHTS (a weight
        // dump folder, with F5LOW_DEV_CAP) replaces everything.
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
        const ULONGLONG t0 = GetTickCount64();
        std::wstring packDir;
        const packbuild::Outcome o = packbuild::EnsurePack(req, packDir, err);
        if (o == packbuild::Outcome::Failed)
        {
            Log("own generator unavailable: %s", err.c_str());      // the one line: what is missing
            SetBuiltFail("own generator unavailable: " + err);
            return;
        }
        for (const std::string& l : lines)
            Log("%s", l.c_str());
        if (o == packbuild::Outcome::Built && fileSource)
        {
            const auto& st = fileSource->GetStats();
            Log("weights from libxess_fg.dll: %zu uploads rebuilt and hash-checked (read %.0f ms, %zu weight blobs located %.0f ms, %zu vector runs %.0f ms, decode %.0f ms); root constants: %zu blocks derived and hash-checked (%.0f ms)",
                st.uploads, st.readMs, st.blobs, st.blobsMs, st.runs, st.runsMs, st.decodeMs, st.rcBlocks, st.rcMs);
        }
        if (o == packbuild::Outcome::Built)
            Log("pack build took %llu ms on the calling thread (one-time; later runs use the cache)", GetTickCount64() - t0);
        g_builtFromCache = o == packbuild::Outcome::Cached;
        result = longpath::ToUtf8(packDir);
    }
}

std::string BuiltPackDir()
{
    std::lock_guard<std::mutex> lk(g_builtLock);
    if (!g_builtRan)
    {
        g_builtRan = true;
        RunBuiltPack();
    }
    return g_builtDir;
}

// A pack taken from the cache that failed to load or to build for D3D12 (a file changed or cut after the marker was
// written): its folder is deleted and the pack made again, once per process. Returns the new folder ("" when it is not the
// cached pack, was repaired before, or cannot be made).
std::string RepairBuiltPack(const std::string& bad)
{
    std::lock_guard<std::mutex> lk(g_builtLock);
    static bool tried = false;
    if (tried || !g_builtFromCache || bad != g_builtDir)
        return {};
    tried = true;
    Log("the cached pack %s failed - deleting it and building the pack again", bad.c_str());
    packbuild::DiscardPack(longpath::WidenUtf8(bad));
    g_builtDir.clear();
    g_builtFromCache = false;
    RunBuiltPack();
    return g_builtDir;
}

// Where the pack comes from, first that exists: a pack folder next to this DLL (or XEFG_CUDA_PACK) - so a new pack can be
// tried without rebuilding - then the pack embedded in this DLL (personal builds), then the cache / a build from the
// player's libxess_fg.dll (BuiltPackDir).
std::string PackDir()
{
    char env[MAX_PATH];
    if (GetEnvironmentVariableA("XEFG_CUDA_FORWARD", env, sizeof(env)))
        return {};
    // UTF-8 from here on (long_path.h): the environment and the DLL's folder are read as wide strings
    std::wstring wdir;
    if (const DWORD n = GetEnvironmentVariableW(L"XEFG_CUDA_PACK", nullptr, 0))
    {
        wdir.resize(n);
        wdir.resize(GetEnvironmentVariableW(L"XEFG_CUDA_PACK", wdir.data(), n));
    }
    else
        wdir = std::wstring(g_dir) + L"\\xefg_pack";
    std::string dir = longpath::ToUtf8(wdir);
    if (GetFileAttributesW(longpath::ExtendIfLong(wdir + L"\\manifest.txt").c_str()) != INVALID_FILE_ATTRIBUTES)
        return dir;
    if (LoadEmbeddedPack())
        return std::string(xefgpack::kEmbeddedDir);
    return BuiltPackDir();
}

// Whether the pack runs on the GPU behind a D3D12 device: `have` its compute capability as sm_NN (0: no CUDA driver,
// or no CUDA device behind that adapter - another vendor's GPU), `need` the oldest one the pack runs on, fallbacks
// taken (Pack::NeedSm; Pack::kNoSm when it does not load). Never throws: it runs inside a C API entry point.
void PackFit(ID3D12Device* d, const std::string& pack, int& need, int& have)
{
    need = have = 0;
    try
    {
        xefgpack::Cuda cu;
        if (!cu.Load() || cu.Init(0))
            return;
        const xefgpack::CUdevice dv = xefgio::Interop::FindDevice(cu, d);
        if (dv < 0)
            return;
        have = xefgio::Interop::DeviceSm(cu, dv);
        need = xefgpack::Pack::NeedSm(cu, pack, xefgown::IsD3dOnlyPack(pack));
    }
    catch (...)
    {
        need = xefgpack::Pack::kNoSm;
    }
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
std::string GeneratorSetting()
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

std::shared_ptr<xefgown::Own> FindOwn(xefg_swapchain_handle_t h)
{
    std::lock_guard<std::mutex> lk(g_ownLock);
    auto it = g_own.find(h);
    return it == g_own.end() ? nullptr : it->second;
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
        g_real = LoadLibraryExW(longpath::ExtendIfLong(path).c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
        Log("real library %S (%s): %p", path.c_str(), from, static_cast<void*>(g_real));
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
    if (!PackDir().empty())
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
    if (FindOwn(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = {};
        p->maxSupportedInterpolations = xefgown::kMaxInterp;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainGetProperties, h, p);
    if (p)
        Log("GetProperties h=%p -> %d descriptors=%u bufHeap=%llu texHeap=%llu cb=%llu maxInterp=%u", static_cast<void*>(h), r,
            p->requiredDescriptorCount, p->tempBufferHeapSize, p->tempTextureHeapSize, p->constantBufferSize,
            p->maxSupportedInterpolations);
    return r;
}

xefg_swapchain_result_t xefgSwapChainTagFrameConstants(xefg_swapchain_handle_t h, uint32_t id, const xefg_swapchain_frame_constant_data_t* c)
{
    SlowApi slow("xefgSwapChainTagFrameConstants");
    if (auto o = FindOwn(h))
    {
        if (!c)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(o->mu);
        o->constants = *c;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainTagFrameConstants, h, id, c);
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
    if (auto o = FindOwn(h))
    {
        xefgown::Own::XellCall c;
        {
            std::lock_guard<std::mutex> lk(o->mu);
            o->enabled = e != 0;
            c = o->XellSync();
        }
        c.Send();
        Log("SetEnabled %u (own)", e);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainSetEnabled, h, e);
    Log("SetEnabled %u -> %d", e, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetPresentId(xefg_swapchain_handle_t h, uint32_t id)
{
    SlowApi slow("xefgSwapChainSetPresentId");
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARD(xefgSwapChainSetPresentId, h, id);
    Log("SetPresentId %u -> %d", id, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainGetLastPresentStatus(xefg_swapchain_handle_t h, xefg_swapchain_present_status_t* s)
{
    SlowApi slow("xefgSwapChainGetLastPresentStatus");
    if (auto o = FindOwn(h))
    {
        if (!s)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        s->framesPresented = o->lastCallPresented;
        s->frameGenResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
        s->isFrameGenEnabled = o->lastGenerated ? 1 : 0;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainGetLastPresentStatus, h, s);
    if (s)
        Log("GetLastPresentStatus -> %d presented=%u enabled=%u", r, s->framesPresented, s->isFrameGenEnabled);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetLoggingCallback(xefg_swapchain_handle_t h, xefg_swapchain_logging_level_t lvl,
                                                        xefg_swapchain_app_log_callback_t cb, void* ud)
{
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARD(xefgSwapChainSetLoggingCallback, h, lvl, cb, ud);
    Log("SetLoggingCallback level=%d -> %d", static_cast<int>(lvl), r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainDestroy(xefg_swapchain_handle_t h)
{
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
    FORWARD(xefgSwapChainDestroy, h);
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
    return 0u;
}

// Not Intel's API: for OptiScaler. What f5low decided for the most recent context: 0 not decided yet, 1 own generator,
// 2 forwarding to Intel's libxess_fg, 3 unavailable. `reason` (if non-null and reasonSize > 0) gets a NUL-terminated
// one-line UTF-8 text, cut to fit. Thread-safe, callable at any time.
extern "C" __declspec(dllexport) int __cdecl f5lowGetStatus(char* reason, int reasonSize)
{
    std::lock_guard<std::mutex> lk(g_statusLock);
    if (reason && reasonSize > 0)
    {
        std::string t = g_statusState == 0 ? "not decided yet (no context created)" : g_statusReason;
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
    if (auto o = FindOwn(h))
    {
        o->LinkXell(xell).Send();
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainSetLatencyReduction, h, xell);
    Log("SetLatencyReduction xell=%p -> %d", xell, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetSceneChangeThreshold(xefg_swapchain_handle_t h, float t)
{
    SlowApi slow("xefgSwapChainSetSceneChangeThreshold");
    if (auto o = FindOwn(h))
    {
        o->sceneThreshold = t;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainSetSceneChangeThreshold, h, t);
    Log("SetSceneChangeThreshold %.4f -> %d", t, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainGetPipelineBuildStatus(xefg_swapchain_handle_t h)
{
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARD(xefgSwapChainGetPipelineBuildStatus, h);
    Log("GetPipelineBuildStatus -> %d", r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetNumInterpolatedFrames(xefg_swapchain_handle_t h, uint32_t n)
{
    SlowApi slow("xefgSwapChainSetNumInterpolatedFrames");
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
    FORWARD(xefgSwapChainSetNumInterpolatedFrames, h, n);
    Log("SetNumInterpolatedFrames %u -> %d", n, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainSetUiCompositionState(xefg_swapchain_handle_t h, xefg_swapchain_ui_composition_state_t s)
{
    SlowApi slow("xefgSwapChainSetUiCompositionState");
    if (auto o = FindOwn(h))
    {
        o->uiComposition = s == XEFG_SWAPCHAIN_UI_COMPOSITION_STATE_ENABLED;
        Log("SetUiCompositionState %d (own)", static_cast<int>(s));
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainSetUiCompositionState, h, s);
    Log("SetUiCompositionState %d -> %d", static_cast<int>(s), r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainEnableDebugFeature(xefg_swapchain_handle_t h, xefg_swapchain_debug_feature_t f, uint32_t e, void* arg)
{
    SlowApi slow("xefgSwapChainEnableDebugFeature");
    if (auto o = FindOwn(h))
    {
        Log("EnableDebugFeature %d enable=%u (own)", static_cast<int>(f), e);
        if (f == XEFG_SWAPCHAIN_DEBUG_FEATURE_TAG_INTERPOLATED_FRAMES)
            o->dbgTag = e != 0;
        else if (f == XEFG_SWAPCHAIN_DEBUG_FEATURE_SHOW_ONLY_INTERPOLATION)
            o->dbgOnly = e != 0;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;       // PRESENT_FAILED_INTERPOLATION: nothing fails here
    }
    FORWARD(xefgSwapChainEnableDebugFeature, h, f, e, arg);
    Log("EnableDebugFeature %d enable=%u -> %d", static_cast<int>(f), e, r);
    return r;
}

// ---- D3D12 -----------------------------------------------------------------------------------------
xefg_swapchain_result_t xefgSwapChainD3D12CreateContext(ID3D12Device* d, xefg_swapchain_handle_t* ph)
{
    std::string pack = PackDir();
    std::string fwdReason;      // why this context forwards (f5lowGetStatus)
    {
        char fenv[8];
        if (pack.empty())
        {
            if (GetEnvironmentVariableA("XEFG_CUDA_FORWARD", fenv, sizeof(fenv)))
                fwdReason = "forwarding to Intel XeFG: XEFG_CUDA_FORWARD is set";
            else
            {
                std::lock_guard<std::mutex> lk(g_statusLock);
                fwdReason = g_builtFailReason.empty() ? "own generator unavailable: no pack" : g_builtFailReason;
                fwdReason += " - forwarding to Intel XeFG";
            }
        }
    }
    int need = 0, have = 0;
    if (!pack.empty() && d && ph)
    {
        // A GPU the pack's kernels cannot run on (an older architecture, or not NVIDIA) gets Intel's library from the start:
        // found only at the build, the pack's failure left frame generation off on our own swap chain.
        PackFit(d, pack, need, have);
        if (need == xefgpack::Pack::kNoSm)
        {
            // a cached pack that does not load: make it again (once) and look again
            const std::string fresh = RepairBuiltPack(pack);
            if (!fresh.empty())
            {
                pack = fresh;
                PackFit(d, pack, need, have);
            }
        }
        if (have < need || !have)
        {
            Log("D3D12CreateContext: %s - forwarding to libxess_fg.dll", !have ? "no CUDA device behind this adapter"
                : need == xefgpack::Pack::kNoSm ? "the pack does not load"
                : ("this GPU is sm_" + std::to_string(have) + ", the pack's kernels need sm_" + std::to_string(need)).c_str());
            fwdReason = std::string("forwarding to Intel XeFG: ") +
                        (!have ? "no CUDA device behind this adapter"
                         : need == xefgpack::Pack::kNoSm ? "the pack does not load"
                         : "this GPU is sm_" + std::to_string(have) + ", the pack's kernels need sm_" + std::to_string(need));
            pack.clear();
        }
    }
    if (!pack.empty() && d && ph)
    {
        const std::string path = PathSetting();
        xefgown::g_repairBuiltPack = &RepairBuiltPack;
        auto o = xefgown::Own::Create(d, pack, path, GeneratorSetting());
        if (!o)
        {
            SetStatus(3, "own generator unavailable: the context could not be created");
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
        }
        o->genMode = GenQueueSetting();
        o->genPriority = GenPrioritySetting();
        o->genSpread = GenSpreadSetting();
        o->noEarly = !EarlySetting();
        o->maxLag = MaxLagSetting();
        o->clockPacer = ClockSetting();
        o->maxDelay = MaxDelaySetting();
        o->gate = GateSetting();
        // FlipMetering=0 (XEFG_CUDA_FLIPMETERING=0): no NVIDIA driver flip metering (xefg_own.h, Meter).
        o->flipMetering = Setting("XEFG_CUDA_FLIPMETERING", "FlipMetering", "1") != "0";
        // FlipLeadUs= (XEFG_CUDA_FLIPLEADUS): how early metered frames after a batch's first are released, 0-8000 us. auto (default):
        // 2500 us, less at a high rate - never more than 0.45 of the frame spacing (xefg_own.h, FlipLead).
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
        o->gs.native = Setting("XEFG_CUDA_NATIVE", "Native", "1") != "0";
        *ph = reinterpret_cast<xefg_swapchain_handle_t>(o.get());
        {
            std::lock_guard<std::mutex> lk(g_ownLock);
            g_own[*ph] = o;
        }
        {
            bool cached;
            {
                std::lock_guard<std::mutex> bl(g_builtLock);
                cached = g_builtFromCache;
            }
            const char* src = pack == xefgpack::kEmbeddedDir ? "embedded pack" : g_builtDir == pack ? (cached ? "pack built on this PC, cache hit" : "pack built on this PC") : "pack folder";
            SetStatus(1, std::string("own generator (") + src + "), D3D12");
        }
        Log("D3D12CreateContext device=%p -> own h=%p, pack %s (needs sm_%d, GPU sm_%d), path %s, early %d, pacer %s%s (CUDA path), max lag "
            "%.2f, max delay %.2f app frames",
            static_cast<void*>(d), static_cast<void*>(*ph), pack.c_str(), need, have, path.c_str(), o->noEarly ? 0 : 1,
            o->clockPacer ? "clock" : "schedule", o->gate ? " + gate" : "", o->maxLag, o->maxDelay);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    const std::string fwdText = fwdReason.empty() ? "forwarding to Intel XeFG" : fwdReason;
    if (!Real("xefgSwapChainD3D12CreateContext"))
        SetStatus(3, fwdReason.empty() ? "libxess_fg.dll not found, nothing to forward to"
                                       : fwdReason + "; libxess_fg.dll not found");
    else if (d && ph)
        SetStatus(2, fwdText);
    FORWARD(xefgSwapChainD3D12CreateContext, d, ph);
    if (r != XEFG_SWAPCHAIN_RESULT_SUCCESS && d && ph && Real("xefgSwapChainD3D12CreateContext"))
        SetStatus(3, "Intel XeFG CreateContext failed (" + std::to_string(static_cast<int>(r)) + ") after hand-over: " + fwdText);
    Log("D3D12CreateContext device=%p -> %d h=%p", static_cast<void*>(d), r, ph ? static_cast<void*>(*ph) : nullptr);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12BuildPipelines(xefg_swapchain_handle_t h, ID3D12PipelineLibrary* lib, uint8_t blocking,
                                                         uint32_t flags)
{
    if (auto o = FindOwn(h))
    {
        o->initFlags = flags;
        const bool ok = o->EnsureBuilt();
        Log("D3D12BuildPipelines flags=0x%x -> %s", flags, ok ? "built" : "FAILED");
        return ok ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_UNSUPPORTED_DEVICE;
    }
    FORWARD(xefgSwapChainD3D12BuildPipelines, h, lib, blocking, flags);
    Log("D3D12BuildPipelines lib=%p blocking=%u flags=0x%x -> %d", static_cast<void*>(lib), blocking, flags, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12GetProperties(xefg_swapchain_handle_t h, const xefg_swapchain_d3d12_init_params_t* ip,
                                                        uint32_t w, uint32_t ht, DXGI_FORMAT f, xefg_swapchain_properties_t* p)
{
    if (FindOwn(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = {};
        p->maxSupportedInterpolations = xefgown::kMaxInterp;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainD3D12GetProperties, h, ip, w, ht, f, p);
    Log("D3D12GetProperties %ux%u fmt=%d -> %d descriptors=%u bufHeap=%llu texHeap=%llu maxInterp=%u", w, ht, static_cast<int>(f), r,
        p ? p->requiredDescriptorCount : 0, p ? p->tempBufferHeapSize : 0, p ? p->tempTextureHeapSize : 0,
        p ? p->maxSupportedInterpolations : 0);
    LogInitParams("params", ip);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12InitFromSwapChain(xefg_swapchain_handle_t h, ID3D12CommandQueue* q,
                                                            const xefg_swapchain_d3d12_init_params_t* ip)
{
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
    FORWARD(xefgSwapChainD3D12InitFromSwapChain, h, q, ip);
    Log("D3D12InitFromSwapChain queue=%p -> %d", static_cast<void*>(q), r);
    LogInitParams("params", ip);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12InitFromSwapChainDesc(xefg_swapchain_handle_t h, HWND wnd, const DXGI_SWAP_CHAIN_DESC1* d,
                                                                const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, ID3D12CommandQueue* q,
                                                                IDXGIFactory2* fac, const xefg_swapchain_d3d12_init_params_t* ip)
{
    if (auto o = FindOwn(h))
    {
        if (!q || !d || !fac || !ip)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        // The real swap chain presents on our own queue, so generated frames are not queued behind the app's work.
        IDXGISwapChain1* sc1 = nullptr;
        IDXGISwapChain4* sc4 = nullptr;
        ID3D12CommandQueue* pq = o->PresentQueue();
        DXGI_SWAP_CHAIN_DESC1 dd = *d;
        o->GateDesc(dd);                // Gate=1: more buffers, the waitable object
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
        o->Wrap(sc4);
        Log("D3D12InitFromSwapChainDesc %ux%u fmt=%d buffers=%u effect=%d flags=0x%x queue=%p initFlags=0x%x (own)", d->Width, d->Height,
            static_cast<int>(d->Format), d->BufferCount, static_cast<int>(d->SwapEffect), d->Flags, static_cast<void*>(q), ip->initFlags);
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainD3D12InitFromSwapChainDesc, h, wnd, d, fs, q, fac, ip);
    if (d)
        Log("D3D12InitFromSwapChainDesc %ux%u fmt=%d buffers=%u effect=%d flags=0x%x windowed=%d queue=%p -> %d", d->Width, d->Height,
            static_cast<int>(d->Format), d->BufferCount, static_cast<int>(d->SwapEffect), d->Flags, fs ? fs->Windowed : -1,
            static_cast<void*>(q), r);
    LogInitParams("params", ip);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12GetSwapChainPtr(xefg_swapchain_handle_t h, REFIID riid, void** pp)
{
    if (auto o = FindOwn(h))
    {
        std::lock_guard<std::mutex> lk(o->proxyMu); // a Destroy on another thread may drop the proxy meanwhile (Retire)
        IDXGISwapChain4* p = o->proxy;
        if (!p || !pp)
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNINITIALIZED;
        return SUCCEEDED(p->QueryInterface(riid, pp)) ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    }
    FORWARD(xefgSwapChainD3D12GetSwapChainPtr, h, riid, pp);
    Log("D3D12GetSwapChainPtr -> %d sc=%p", r, pp ? *pp : nullptr);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12TagFrameResource(xefg_swapchain_handle_t h, ID3D12CommandList* cl, uint32_t id,
                                                           const xefg_swapchain_d3d12_resource_data_t* rd)
{
    SlowApi slow("xefgSwapChainD3D12TagFrameResource");
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
    FORWARD(xefgSwapChainD3D12TagFrameResource, h, cl, id, rd);
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
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARD(xefgSwapChainD3D12SetDescriptorHeap, h, heap, off);
    Log("D3D12SetDescriptorHeap heap=%p offset=%u -> %d", static_cast<void*>(heap), off, r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12UpdateExternalHeapOnResize(xefg_swapchain_handle_t h, ID3D12Heap* bh, uint64_t bo,
                                                                     ID3D12Heap* th, uint64_t to)
{
    if (FindOwn(h))
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    FORWARD(xefgSwapChainD3D12UpdateExternalHeapOnResize, h, bh, bo, th, to);
    Log("D3D12UpdateExternalHeapOnResize -> %d", r);
    return r;
}

xefg_swapchain_result_t xefgSwapChainD3D12GetInitializationParameters(xefg_swapchain_handle_t h, xefg_swapchain_d3d12_init_params_t* p)
{
    if (auto o = FindOwn(h))
    {
        if (!p)
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        *p = o->initParams;
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }
    FORWARD(xefgSwapChainD3D12GetInitializationParameters, h, p);
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
