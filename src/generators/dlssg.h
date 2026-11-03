// generators/dlssg.h - Generator=dlssg: NVIDIA DLSS-G makes the generated frames behind Intel's XeFG API (docs/GENERATORS.md,
// "dlssg"). The present-sink shape: DLSS-G presents into our sink (sink/sink.h), which forwards to the real swap chain.
//
// Routing: a dlssg context is its own kind of context at the XeFG entry-point level (xefg_cuda.cpp, next to the forwarding route);
// it does not go through class Own or IPackGenerator. DlssgGenerator below is its IGenerator face: Caps() says whether it can run
// on this GPU. There is NO fallback to the own generator: when dlssg cannot start, D3D12CreateContext reports status 3 with the
// reason and returns an error.
//
// The unlock (unlock/Unlock.h): on RTX 20/30/40, OptiScaler's MFG unlock families are applied in memory to OUR copy of nvngx_dlssg and
// sl.dlss_g before slInit, so DLSS-G offers 2x-6x there too; RTX 50 and newer need nothing.
//
// DLSS-G is driven through Streamline's public API (headers: NVIDIA's public Streamline SDK), the way the spike (tests/dlssg_spike.cpp)
// proved it:
//   - one Streamline per process, a private copy under other names made by dlssg_files.h (xefg_slinterposer.dll, sl.xefg_*.dll):
//     OptiScaler hooks every module whose name ENDS in sl.interposer.dll / sl.common.dll / sl.pcl.dll / ... (suffix match), and its
//     hooks keep one set of originals per process: with our plugins under the plain names it re-pointed them at ours and the game's
//     DLSS calls landed in our Streamline (NBA 2K27: "kFeatureDLSS context is missing", OptiScaler "no upscaler"). Streamline finds
//     plugins by sl.*.dll, so the renamed ones still load. A game's own Streamline next to it is fine.
//     Manual hooking + a DXGI factory proxy (Streamline wraps our SinkFactory, not the game's v-tables); the game's device is
//     only handed over (slSetD3DDevice), never upgraded.
//   - the swap chain is made by Streamline's factory proxy over our SinkFactory in stable-buffer mode (DLSS-G takes its buffers
//     once and draws into buffer[GetCurrentBackBufferIndex()]), so every frame DLSS-G presents lands in the sink.
//   - DLSS-G needs Reflex on and PCL present markers around the game's Present (it generates nothing without them; simulation
//     and render markers and Reflex sleep are not needed): the game gets a thin wrapper (MarkerChain) that sets them.
//   - it generates only while the window is the foreground one (a game in front: yes).
// The XeFG frame id is Streamline's frame index (slGetNewFrameToken with that index).
//
// Pacing: DLSS-G paces itself (it presents from a thread of its own, with the driver's flip metering); the sink forwards each
// present to the real chain as it comes. The host's own pacer is not involved (logged once per context).
#pragma once
#include "../generator.h"
#include "../sink/sink.h"
#include "dlssg_files.h"
#include "dlssg_own.h"
#include "../unlock/Unlock.h"

#include <psapi.h>
#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include <xefg_swapchain.h>
#include <xefg_swapchain_d3d12.h>

#include <atomic>
#include <cmath>
#include <memory>
#include <mutex>
#include <string>

namespace xdlssg
{
using LogFn = void (*)(const char* fmt, ...);

// The process' Streamline: loaded and initialised once, never shut down (a game's Streamline may share the driver's NGX).
struct Streamline
{
    HMODULE m = nullptr;
    std::wstring ownNgx;     // our private nvngx_dlssg.dll (full path)
    PFun_slInit* init = nullptr;
    PFun_slIsFeatureSupported* isSupported = nullptr;
    PFun_slSetTagForFrame* setTag = nullptr;
    PFun_slSetConstants* setConstants = nullptr;
    PFun_slUpgradeInterface* upgrade = nullptr;
    PFun_slGetFeatureFunction* featureFn = nullptr;
    PFun_slGetNewFrameToken* newToken = nullptr;
    PFun_slSetD3DDevice* setDevice = nullptr;
    PFun_slDLSSGGetState* getState = nullptr;
    PFun_slDLSSGSetOptions* setOptions = nullptr;
    PFun_slReflexSetOptions* reflexOptions = nullptr;
    PFun_slPCLSetMarker* marker = nullptr;
    ID3D12Device* device = nullptr;     // the device handed over (Streamline takes one per process)
    std::string error;                  // why it is not usable ("" = usable)
    std::wstring dir;                   // the folder it was loaded from

    sl::FrameToken* Token(uint32_t id) const
    {
        sl::FrameToken* t = nullptr;
        newToken(t, &id);
        return t;
    }
};

inline LogFn g_log = nullptr;
// The caller asked xefgCudaDriverMultipliesReflexCap (xefg_cuda.cpp): an OptiScaler that never does divides a frame cap through
// NVIDIA Reflex by the multiplier while the driver multiplies it again (needs OptiScaler-NR 0.1.36 or newer)
inline std::atomic<bool> g_capAsked { false };
inline std::mutex g_slMu;

inline void SlLog(sl::LogType type, const char* msg)
{
    if (g_log && type != sl::LogType::eInfo)
        g_log("dlssg: streamline %s: %s", type == sl::LogType::eWarn ? "warning" : "error", msg);
}

// Which nvngx_dlssg is in use (dlssg_own.h has the rule). Generator=dlssg runs DLSS-G on OUR copy: it is loaded by full path before slInit
// on every tier, and after DLSS-G is up no other nvngx_dlssg* may be in the process (a game's own 310.6 under Streamline 2.11.1 got the
// driver to run DLSS-G on it: GPU fault at level load in A Plague Tale: Resonance). F5LOW_DLSSG_ALLOW_FOREIGN=1 only logs.
#pragma comment(lib, "version.lib")
inline std::wstring ModulePathOf(HMODULE m)
{
    std::wstring p(1024, L'\0');
    const DWORD n = GetModuleFileNameW(m, p.data(), static_cast<DWORD>(p.size()));
    p.resize(n < p.size() ? n : 0);
    return p;
}
inline std::vector<std::wstring> LoadedNgxModules()
{
    std::vector<std::wstring> out;
    std::vector<HMODULE> mods(1024);
    DWORD need = 0;
    if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &need))
        return out;
    if (need > mods.size() * sizeof(HMODULE))
    {
        mods.resize(need / sizeof(HMODULE));
        if (!K32EnumProcessModules(GetCurrentProcess(), mods.data(), static_cast<DWORD>(mods.size() * sizeof(HMODULE)), &need))
            return out;
    }
    for (size_t i = 0; i < need / sizeof(HMODULE) && i < mods.size(); ++i)
    {
        const std::wstring p = ModulePathOf(mods[i]);
        if (IsNgxDlssgName(p))
            out.push_back(p);
    }
    return out;
}
inline std::string FileVersionOf(const std::wstring& path)
{
    DWORD dummy = 0;
    const DWORD sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    std::vector<char> buf(sz);
    VS_FIXEDFILEINFO* fi = nullptr;
    UINT len = 0;
    if (!sz || !GetFileVersionInfoW(path.c_str(), 0, sz, buf.data()) || !VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) || !fi)
        return "version unknown";
    char v[64];
    snprintf(v, sizeof v, "%u.%u.%u.%u", HIWORD(fi->dwFileVersionMS), LOWORD(fi->dwFileVersionMS), HIWORD(fi->dwFileVersionLS), LOWORD(fi->dwFileVersionLS));
    return v;
}
inline std::string DescribeNgx(const std::wstring& path) { return Narrow(path) + ", " + FileVersionOf(path); }

inline bool g_allowForeignNgx = false;
// Our nvngx_dlssg.dll, by full path, before slInit (every tier; the unlock's own load of the same path then returns this handle). The
// reference is kept for the life of the process. A different nvngx_dlssg already in the process is logged with both paths and versions.
inline bool PreloadOwnNgx(const std::wstring& dir, std::wstring& ownPath, std::string& why)
{
    ownPath = longpath::Plain(dir + L"\\nvngx_dlssg.dll");
    const NgxVerdict before = JudgeNgx(ownPath, LoadedNgxModules());
    if (g_log)
        for (const std::wstring& f : before.foreign)
            g_log("dlssg: a different nvngx_dlssg.dll is already in the process before ours is loaded: %s (ours: %s)", DescribeNgx(f).c_str(),
                  DescribeNgx(ownPath).c_str());
    static HMODULE keep = nullptr;
    if (!keep)
        keep = LoadLibraryW(ownPath.c_str());
    if (!keep)
    {
        why = "cannot load " + Narrow(ownPath) + " (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    if (g_log)
        g_log("dlssg: our nvngx_dlssg %p loaded by full path (%s)", static_cast<void*>(keep), DescribeNgx(ownPath).c_str());
    return true;
}
// After DLSS-G is up: only our nvngx_dlssg may be in the process. On false, `why` has the reason.
inline bool CheckOwnNgxInUse(const std::wstring& ownPath, std::string& why)
{
    std::vector<std::wstring> loaded = LoadedNgxModules();
    if (const HMODULE bare = GetModuleHandleW(L"nvngx_dlssg.dll"))
        loaded.push_back(ModulePathOf(bare));       // what a bare-name load resolves to
    const NgxVerdict v = JudgeNgx(ownPath, loaded);
    if (v.Ok())
        return true;
    for (const std::wstring& f : v.foreign)
    {
        const std::string msg = "a different nvngx_dlssg.dll (" + DescribeNgx(f) + ") is loaded in the game: DLSS-G would run on it" +
                                (v.ownLoaded ? " (ours is loaded too: " : " (ours is not loaded: ") + DescribeNgx(ownPath) + ")";
        if (g_allowForeignNgx)
        {
            if (g_log)
                g_log("dlssg: %s - F5LOW_DLSSG_ALLOW_FOREIGN=1, continuing", msg.c_str());
        }
        else if (why.empty())
            why = msg;
    }
    return g_allowForeignNgx;
}

// Loads and initialises Streamline from `dir` (the renamed folder, dlssg_files.h) on the first call and hands it `dev`. Null with the
// reason in `why`. A later call with another folder keeps the first (one Streamline per process).
inline Streamline g_sl;
inline Streamline* GetLocked(const std::wstring& dir, int gpuSm, ID3D12Device* dev);
inline Streamline* Get(const std::wstring& dir, int gpuSm, ID3D12Device* dev, std::string& why)
{
    std::lock_guard<std::mutex> lk(g_slMu);
    Streamline* s = GetLocked(dir, gpuSm, dev);
    if (!s)
        why = g_sl.error.empty() ? "a second D3D12 device (Streamline keeps the first)" : g_sl.error;
    return s;
}
inline Streamline* GetLocked(const std::wstring& dir, int gpuSm, ID3D12Device* dev)
{
    Streamline& s = g_sl;
    static bool tried = false;
    if (!tried)
    {
        tried = true;
        s.dir = dir;
        // Never a plain-named plugin: OptiScaler would hook it and cross the game's Streamline with ours (see the top)
        for (const wchar_t* plain : kPlainPlugins)
            if (FileExists(dir + L"\\" + plain))
            {
                s.error = Narrow(plain) + " in the Streamline folder " + Narrow(dir) + " - plain names are never loaded";
                return nullptr;
            }
        // The unlock (unlock/Unlock.h): OptiScaler's MFG unlock families, applied in memory to OUR copy of nvngx_dlssg and sl.dlss_g, which
        // are loaded here by full path BEFORE slInit so Streamline gets the patched modules. RTX 50 and newer: nothing to patch. No
        // fallback: a build the families do not recognise fails below RTX 50 with the reason (status 3).
        {
            unlock::g_log = g_log;
            unlock::Tier tier;
            if (unlock::TierOfSm(gpuSm, tier) != 0)
            {
                s.error = "DLSS-G needs an RTX 20 or newer GeForce (this GPU is sm_" + std::to_string(gpuSm) + ")";
                return nullptr;
            }
            g_allowForeignNgx = unlock::EnvIsOne("F5LOW_DLSSG_ALLOW_FOREIGN");
            if (!PreloadOwnNgx(dir, s.ownNgx, s.error))
                return nullptr;
            const unlock::Result u = unlock::Apply(dir, tier);
            if (!u.ok)
            {
                s.error = u.reason;
                return nullptr;
            }
        }
        s.m = LoadLibraryW(longpath::Plain(dir + L"\\xefg_slinterposer.dll").c_str());
        if (!s.m)
        {
            s.error = "cannot load xefg_slinterposer.dll from " + Narrow(dir) + " (error " + std::to_string(GetLastError()) + ")";
            return nullptr;
        }
        s.init = reinterpret_cast<PFun_slInit*>(GetProcAddress(s.m, "slInit"));
        s.isSupported = reinterpret_cast<PFun_slIsFeatureSupported*>(GetProcAddress(s.m, "slIsFeatureSupported"));
        s.setTag = reinterpret_cast<PFun_slSetTagForFrame*>(GetProcAddress(s.m, "slSetTagForFrame"));
        s.setConstants = reinterpret_cast<PFun_slSetConstants*>(GetProcAddress(s.m, "slSetConstants"));
        s.upgrade = reinterpret_cast<PFun_slUpgradeInterface*>(GetProcAddress(s.m, "slUpgradeInterface"));
        s.featureFn = reinterpret_cast<PFun_slGetFeatureFunction*>(GetProcAddress(s.m, "slGetFeatureFunction"));
        s.newToken = reinterpret_cast<PFun_slGetNewFrameToken*>(GetProcAddress(s.m, "slGetNewFrameToken"));
        s.setDevice = reinterpret_cast<PFun_slSetD3DDevice*>(GetProcAddress(s.m, "slSetD3DDevice"));
        if (!s.init || !s.isSupported || !s.setTag || !s.setConstants || !s.upgrade || !s.featureFn || !s.newToken || !s.setDevice)
        {
            s.error = "xefg_slinterposer.dll lacks Streamline's API";
            return nullptr;
        }
        const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        const wchar_t* paths[] = { s.dir.c_str() };
        sl::Preferences pref {};
        pref.logLevel = sl::LogLevel::eDefault;
        pref.logMessageCallback = SlLog;
        pref.pathsToPlugins = paths;
        pref.numPathsToPlugins = 1;
        // A proxy over the factory we hand it (not its v-table patched: that would be our SinkFactory's); tags per frame token
        pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy |
                     sl::PreferenceFlags::eUseFrameBasedResourceTagging;
        pref.featuresToLoad = features;
        pref.numFeaturesToLoad = 3;
        pref.engine = sl::EngineType::eCustom;
        pref.applicationId = 0x5EC0A11D;        // any non-zero id: NGX features stay off with none
        pref.renderAPI = sl::RenderAPI::eD3D12;
        if (const sl::Result r = s.init(pref, sl::kSDKVersion); r != sl::Result::eOk)
        {
            s.error = "slInit failed (" + std::to_string(static_cast<int>(r)) + ")";
            return nullptr;
        }
    }
    if (!s.error.empty() || !s.m)
        return nullptr;
    if (!s.device)
    {
        if (const sl::Result r = s.setDevice(dev); r != sl::Result::eOk)
        {
            s.error = "slSetD3DDevice failed (" + std::to_string(static_cast<int>(r)) + ")";
            return nullptr;
        }
        s.device = dev;
        void *get = nullptr, *set = nullptr, *refl = nullptr, *mark = nullptr;
        s.featureFn(sl::kFeatureDLSS_G, "slDLSSGGetState", get);
        s.featureFn(sl::kFeatureDLSS_G, "slDLSSGSetOptions", set);
        s.featureFn(sl::kFeatureReflex, "slReflexSetOptions", refl);
        s.featureFn(sl::kFeaturePCL, "slPCLSetMarker", mark);
        s.getState = reinterpret_cast<PFun_slDLSSGGetState*>(get);
        s.setOptions = reinterpret_cast<PFun_slDLSSGSetOptions*>(set);
        s.reflexOptions = reinterpret_cast<PFun_slReflexSetOptions*>(refl);
        s.marker = reinterpret_cast<PFun_slPCLSetMarker*>(mark);
        if (!s.getState || !s.setOptions || !s.reflexOptions || !s.marker)
        {
            s.error = "DLSS-G, Reflex or PCL did not load (an old driver, a plugin Streamline refused to load, or nvngx_dlssg.dll missing)";
            return nullptr;
        }
        // Only our nvngx_dlssg may be in the process now (no silent fallback: refuse, with the reason)
        if (!CheckOwnNgxInUse(s.ownNgx, s.error))
            return nullptr;
        // DLSS-G refuses to run without Reflex (eDLSSGStatusFailReflexNotDetectedAtRuntime)
        sl::ReflexOptions ro {};
        ro.mode = sl::ReflexMode::eLowLatency;
        s.reflexOptions(ro);
    }
    else if (s.device != dev)
        return nullptr;
    return &s;
}

class Ctx;
uint32_t BeforePresent(Ctx& c);      // below: the frame's id for the markers; DLSS-G off for a frame without inputs

// What the game presents on: Streamline's chain (over our sink), with DLSS-G's present markers around each Present. Holds its
// context weakly: the game may keep the chain after Destroy (then it presents plainly).
class MarkerChain final : public scproxy::LogSwapChain
{
    Streamline* sl;
    std::weak_ptr<Ctx> ctx;

    template <class F> HRESULT Marked(F present)
    {
        std::shared_ptr<Ctx> c = ctx.lock();
        if (!c)
            return present();
        sl::FrameToken* t = sl->Token(BeforePresent(*c));
        sl->marker(sl::PCLMarker::ePresentStart, *t);
        const HRESULT h = present();
        sl->marker(sl::PCLMarker::ePresentEnd, *t);
        return h;
    }

public:
    MarkerChain(IDXGISwapChain4* slChain, Streamline* s, std::weak_ptr<Ctx> c) : scproxy::LogSwapChain(slChain), sl(s), ctx(std::move(c)) {}
    HRESULT STDMETHODCALLTYPE Present(UINT si, UINT fl) override
    {
        return Marked([&] { return r->Present(si, fl); });
    }
    HRESULT STDMETHODCALLTYPE Present1(UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* p) override
    {
        return Marked([&] { return r->Present1(si, fl, p); });
    }
};

// A typeless format as DLSS-G should read it (Streamline takes typed formats for typeless resources in DLSSGOptions)
inline DXGI_FORMAT Typed(DXGI_FORMAT f, bool depth)
{
    switch (f)
    {
    case DXGI_FORMAT_R32_TYPELESS: return DXGI_FORMAT_R32_FLOAT;
    case DXGI_FORMAT_R32G8X24_TYPELESS: return DXGI_FORMAT_R32_FLOAT_X8X24_TYPELESS;
    case DXGI_FORMAT_R24G8_TYPELESS: return DXGI_FORMAT_R24_UNORM_X8_TYPELESS;
    case DXGI_FORMAT_R16_TYPELESS: return depth ? DXGI_FORMAT_R16_UNORM : DXGI_FORMAT_R16_FLOAT;
    case DXGI_FORMAT_R16G16_TYPELESS: return DXGI_FORMAT_R16G16_FLOAT;
    case DXGI_FORMAT_R16G16B16A16_TYPELESS: return DXGI_FORMAT_R16G16B16A16_FLOAT;
    case DXGI_FORMAT_R32G32_TYPELESS: return DXGI_FORMAT_R32G32_FLOAT;
    case DXGI_FORMAT_R8G8B8A8_TYPELESS: return DXGI_FORMAT_R8G8B8A8_UNORM;
    case DXGI_FORMAT_B8G8R8A8_TYPELESS: return DXGI_FORMAT_B8G8R8A8_UNORM;
    case DXGI_FORMAT_R10G10B10A2_TYPELESS: return DXGI_FORMAT_R10G10B10A2_UNORM;
    default: return f;
    }
}

// Row-vector (D3D) 4x4 helpers for the camera constants
inline void Mul(const float* a, const float* b, float* o)
{
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            o[i * 4 + j] = a[i * 4] * b[j] + a[i * 4 + 1] * b[4 + j] + a[i * 4 + 2] * b[8 + j] + a[i * 4 + 3] * b[12 + j];
}
inline bool Inverse(const float* m, float* o)
{
    float a[4][8];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 8; ++j)
            a[i][j] = j < 4 ? m[i * 4 + j] : (j - 4 == i ? 1.f : 0.f);
    for (int c = 0; c < 4; ++c)
    {
        int p = c;
        for (int r = c + 1; r < 4; ++r)
            if (std::fabs(a[r][c]) > std::fabs(a[p][c]))
                p = r;
        if (std::fabs(a[p][c]) < 1e-20f)
            return false;
        if (p != c)
            for (int j = 0; j < 8; ++j)
                std::swap(a[p][j], a[c][j]);
        const float d = a[c][c];
        for (int j = 0; j < 8; ++j)
            a[c][j] /= d;
        for (int r = 0; r < 4; ++r)
            if (r != c && a[r][c] != 0)
            {
                const float f = a[r][c];
                for (int j = 0; j < 8; ++j)
                    a[r][j] -= f * a[c][j];
            }
    }
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j)
            o[i * 4 + j] = a[i][j + 4];
    return true;
}
inline sl::float4x4 ToSl(const float* m)
{
    sl::float4x4 r;
    for (int i = 0; i < 4; ++i)
        r.row[i] = sl::float4(m[i * 4], m[i * 4 + 1], m[i * 4 + 2], m[i * 4 + 3]);
    return r;
}

// One XeFG context run by DLSS-G.
class Ctx : public std::enable_shared_from_this<Ctx>
{
public:
    Streamline* sl = nullptr;
    ID3D12Device* dev = nullptr;                // the game's (not referenced; the context goes before the device)
    std::mutex mu;
    xsink::SinkFactory* sinkFac = nullptr;      // our reference
    MarkerChain* chain = nullptr;               // our reference; the game gets its own through GetSwapChainPtr
    std::atomic<uint32_t> frameId { 0 };
    bool enabled = false;
    uint32_t numInterp = 1, maxGen = 1;
    uint32_t initFlags = 0;
    xefg_swapchain_d3d12_init_params_t initParams {};
    uint32_t mvW = 0, mvH = 0;                  // the motion vectors' size (their tag), for Streamline's normalised scale
    float prevVP[16] {};
    bool havePrev = false;
    uint32_t lastPresented = 1;
    uint32_t fmt[4] {};                         // typed formats of MV, depth, hud-less, UI (DLSSGOptions)
    bool optsDirty = false;                     // a format changed: Apply at the next frame's constants
    // A frame presented without new constants (a game in its menu stops tagging, as Cyberpunk does) is shown alone, as Intel does:
    // DLSS-G would otherwise interpolate it from the last inputs. DLSS-G keeps its resources while off (eRetainResourcesWhenOff).
    bool inputsThisFrame = false;
    bool appliedOn = false;
    unsigned untaggedLogs = 0;
    unsigned statFrames = 0;
    const sl::ViewportHandle vp { 0 };

    ~Ctx()
    {
        if (chain)
            chain->Release();
        if (sinkFac)
            sinkFac->Release();
    }

    // DLSS-G's options (on: enabled and this frame has inputs) and the sink's burst (from enabled / count). Called with mu held.
    void Apply() { Apply(enabled && inputsThisFrame); }
    void Apply(bool on)
    {
        const uint32_t n = std::max(1u, std::min(numInterp, maxGen));
        appliedOn = on;
        sl::DLSSGOptions o {};
        o.mode = on ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
        o.flags = sl::DLSSGFlags::eRetainResourcesWhenOff;
        o.numFramesToGenerate = n;
        o.mvecBufferFormat = fmt[0];
        o.depthBufferFormat = fmt[1];
        o.hudLessBufferFormat = fmt[2];
        o.uiBufferFormat = fmt[3];
        optsDirty = false;
        const sl::Result r = sl->setOptions(vp, o);
        if (sinkFac)
            sinkFac->SetBurst(enabled ? n + 1 : 1);
        if (g_log && (on || !enabled || untaggedLogs < 5))
            g_log("dlssg: options %s, %u generated -> %d%s", on ? "on" : "off", n, static_cast<int>(r), enabled && !on ? " (a frame without inputs)" : "");
        if (enabled && !on)
            ++untaggedLogs;
    }

    xefg_swapchain_result_t InitFromDesc(HWND wnd, const DXGI_SWAP_CHAIN_DESC1* d, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, ID3D12CommandQueue* q,
                                         IDXGIFactory2* fac, const xefg_swapchain_d3d12_init_params_t* ip)
    {
        IDXGIFactory4* f4 = nullptr;
        if (!d || !q || !fac || !ip || FAILED(fac->QueryInterface(IID_PPV_ARGS(&f4))))
            return XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
        std::lock_guard<std::mutex> lk(mu);
        auto* sf = new xsink::SinkFactory(f4, true);     // DLSS-G takes its buffers once: stable buffers
        f4->Release();
        IDXGIFactory* up = sf;
        sf->AddRef();                                   // Streamline's proxy holds this one
        IDXGIFactory2* up2 = nullptr;
        if (sl->upgrade(reinterpret_cast<void**>(&up)) != sl::Result::eOk || FAILED(up->QueryInterface(IID_PPV_ARGS(&up2))))
        {
            up->Release();
            sf->Release();
            if (g_log)
                g_log("dlssg: Streamline did not take our factory");
            return XEFG_SWAPCHAIN_RESULT_ERROR_UNKNOWN;
        }
        DXGI_SWAP_CHAIN_DESC1 dd = *d;
        dd.Flags |= DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;     // DLSS-G presents with tearing allowed
        IDXGISwapChain1* sc1 = nullptr;
        IDXGISwapChain4* sc4 = nullptr;
        const HRESULT hr = up2->CreateSwapChainForHwnd(q, wnd, &dd, fs, nullptr, &sc1);
        up2->Release();
        up->Release();
        if (FAILED(hr) || !sc1 || FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc4))))
        {
            if (sc1)
                sc1->Release();
            sf->Release();
            if (g_log)
                g_log("dlssg: CreateSwapChainForHwnd through Streamline failed 0x%08lx", static_cast<unsigned long>(hr));
            return XEFG_SWAPCHAIN_RESULT_ERROR_DXGI_INVALID_CALL;
        }
        sc1->Release();
        if (!sf->HasSink() && g_log)
            g_log("dlssg: Streamline's chain is not on our sink - DLSS-G paces its own frames");
        if (chain)
            chain->Release();
        if (sinkFac)
            sinkFac->Release();
        chain = new MarkerChain(sc4, sl, weak_from_this());  // owns the QI reference
        sinkFac = sf;
        initFlags = ip->initFlags;
        initParams = *ip;
        sl::DLSSGState st {};
        sl::DLSSGOptions none {};
        if (sl->getState(vp, st, &none) == sl::Result::eOk && st.numFramesToGenerateMax)
            maxGen = st.numFramesToGenerateMax;
        initParams.maxInterpolatedFrames = maxGen;
        initParams.pApplicationSwapChain = chain;
        Apply();
        if (g_log)
        {
            g_log("dlssg: swap chain %ux%u fmt=%d buffers=%u flags=0x%x, DLSS-G makes up to %u, frames go through our sink", d->Width, d->Height,
                  static_cast<int>(d->Format), d->BufferCount, dd.Flags, maxGen);
            g_log("dlssg: DLSS-G paces its own frames (host pacer handed over)");
        }
        return XEFG_SWAPCHAIN_RESULT_SUCCESS;
    }

    xefg_swapchain_result_t Tag(ID3D12CommandList* cl, uint32_t id, const xefg_swapchain_d3d12_resource_data_t& rd)
    {
        sl::BufferType bt;
        switch (rd.type)
        {
        case XEFG_SWAPCHAIN_RES_MOTION_VECTOR: bt = sl::kBufferTypeMotionVectors; break;
        case XEFG_SWAPCHAIN_RES_DEPTH: bt = sl::kBufferTypeDepth; break;
        case XEFG_SWAPCHAIN_RES_HUDLESS_COLOR: bt = sl::kBufferTypeHUDLessColor; break;
        case XEFG_SWAPCHAIN_RES_UI: bt = sl::kBufferTypeUIColorAndAlpha; break;
        default: return XEFG_SWAPCHAIN_RESULT_SUCCESS;     // the back buffer: DLSS-G takes it from the chain
        }
        if (!rd.pResource)
            return XEFG_SWAPCHAIN_RESULT_SUCCESS;
        {
            std::lock_guard<std::mutex> lk(mu);
            if (rd.type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR)
            {
                mvW = rd.resourceSize.x;
                mvH = rd.resourceSize.y;
            }
            const int k = rd.type == XEFG_SWAPCHAIN_RES_MOTION_VECTOR ? 0 : rd.type == XEFG_SWAPCHAIN_RES_DEPTH ? 1 : rd.type == XEFG_SWAPCHAIN_RES_HUDLESS_COLOR ? 2 : 3;
            const uint32_t f = static_cast<uint32_t>(Typed(rd.pResource->GetDesc().Format, k == 1));
            if (f != fmt[k])
            {
                fmt[k] = f;
                optsDirty = true;
            }
        }
        sl::Resource res(sl::ResourceType::eTex2d, rd.pResource, nullptr, nullptr, static_cast<uint32_t>(rd.incomingState));
        const sl::Extent ext { rd.resourceBase.y, rd.resourceBase.x, rd.resourceSize.x, rd.resourceSize.y };
        const sl::ResourceTag tag(&res, bt, rd.validity == XEFG_SWAPCHAIN_RV_ONLY_NOW ? sl::ResourceLifecycle::eOnlyValidNow : sl::ResourceLifecycle::eValidUntilPresent,
                                  &ext);
        const sl::Result r = sl->setTag(*sl->Token(id), vp, &tag, 1, reinterpret_cast<sl::CommandBuffer*>(cl));
        return r == sl::Result::eOk ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    }

    // XeFG's camera (row-major, row vectors as D3D) into Streamline's constants
    xefg_swapchain_result_t Constants(uint32_t id, const xefg_swapchain_frame_constant_data_t& c)
    {
        std::lock_guard<std::mutex> lk(mu);
        frameId = id;
        inputsThisFrame = true;
        // F5LOW_SINK_STATS=1 (tests): the sink's statistics cover frame generation after its start-up
        if (enabled && xsink::g_stats && ++statFrames == 12)
            xsink::ResetGaps();
        if (optsDirty || appliedOn != enabled)
            Apply();
        sl::Constants k {};
        float vp4[16], inv[16], invV[16], c2p[16], p2c[16];
        Mul(c.viewMatrix, c.projectionMatrix, vp4);
        const bool haveInv = Inverse(vp4, inv);
        k.cameraViewToClip = ToSl(c.projectionMatrix);
        float invP[16];
        k.clipToCameraView = Inverse(c.projectionMatrix, invP) ? ToSl(invP) : ToSl(c.projectionMatrix);
        float ident[16] {};
        ident[0] = ident[5] = ident[10] = ident[15] = 1.f;
        k.clipToLensClip = ToSl(ident);
        if (haveInv && havePrev && !c.resetHistory)
        {
            Mul(inv, prevVP, c2p);
            k.clipToPrevClip = ToSl(c2p);
            k.prevClipToClip = Inverse(c2p, p2c) ? ToSl(p2c) : ToSl(ident);
        }
        else
            k.clipToPrevClip = k.prevClipToClip = ToSl(ident);
        std::copy(vp4, vp4 + 16, prevVP);
        havePrev = true;
        // The camera in world space: rows of the inverse view (right, up, forward, position)
        if (Inverse(c.viewMatrix, invV))
        {
            k.cameraRight = sl::float3(invV[0], invV[1], invV[2]);
            k.cameraUp = sl::float3(invV[4], invV[5], invV[6]);
            k.cameraFwd = sl::float3(invV[8], invV[9], invV[10]);
            k.cameraPos = sl::float3(invV[12], invV[13], invV[14]);
        }
        else
        {
            k.cameraRight = sl::float3(1, 0, 0);
            k.cameraUp = sl::float3(0, 1, 0);
            k.cameraFwd = sl::float3(0, 0, 1);
            k.cameraPos = sl::float3(0, 0, 0);
        }
        // Near / far from depth = A + B / z (A = P[2][2], B = P[3][2]); an infinite far plane is capped
        const float* p = c.projectionMatrix;
        const float A = p[10], B = p[14];
        float zn = A != 0 ? -B / A : 1e6f, zf = A != 1 ? B / (1 - A) : 1e6f;
        if (!(std::isfinite(zn) && zn > 0))
            zn = 1e6f;
        if (!(std::isfinite(zf) && zf > 0))
            zf = 1e6f;
        k.cameraNear = std::min(zn, zf);
        k.cameraFar = std::max(zn, zf);
        k.cameraFOV = p[5] != 0 ? 2.f * std::atan(1.f / p[5]) : 1.f;
        k.cameraAspectRatio = p[0] != 0 ? p[5] / p[0] : 1.f;
        k.jitterOffset = sl::float2(c.jitterOffsetX, c.jitterOffsetY);
        // XeFG's scale takes the vectors to pixels (0 = already pixels); Streamline's to [0, 1] of the vectors' size
        const float sx = c.motionVectorScaleX != 0 ? c.motionVectorScaleX : 1.f, sy = c.motionVectorScaleY != 0 ? c.motionVectorScaleY : 1.f;
        k.mvecScale = sl::float2(mvW ? sx / static_cast<float>(mvW) : 1.f, mvH ? sy / static_cast<float>(mvH) : 1.f);
        k.cameraPinholeOffset = sl::float2(0, 0);
        k.motionVectorsInvalidValue = 0.f;
        k.depthInverted = (initFlags & XEFG_SWAPCHAIN_INIT_FLAG_INVERTED_DEPTH) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        k.cameraMotionIncluded = sl::Boolean::eTrue;
        k.motionVectors3D = sl::Boolean::eFalse;
        k.reset = c.resetHistory ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        k.motionVectorsJittered = (initFlags & XEFG_SWAPCHAIN_INIT_FLAG_JITTERED_MV) ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        const sl::Result r = sl->setConstants(k, *sl->Token(id), vp);
        return r == sl::Result::eOk ? XEFG_SWAPCHAIN_RESULT_SUCCESS : XEFG_SWAPCHAIN_RESULT_ERROR_INVALID_ARGUMENT;
    }

    void Status(xefg_swapchain_present_status_t& s)
    {
        sl::DLSSGState st {};
        if (sl->getState(vp, st, nullptr) == sl::Result::eOk)
            lastPresented = std::max(1u, st.numFramesActuallyPresented);
        s.framesPresented = lastPresented;
        s.frameGenResult = XEFG_SWAPCHAIN_RESULT_SUCCESS;
        s.isFrameGenEnabled = enabled && lastPresented > 1 ? 1 : 0;
    }

    void Shutdown()
    {
        std::lock_guard<std::mutex> lk(mu);
        enabled = false;
        sl::DLSSGOptions o {};
        o.mode = sl::DLSSGMode::eOff;
        sl->setOptions(vp, o);
    }
};

// Right before the game's Present: DLSS-G off for a frame that brought no constants, on again with the next that does; the inputs
// flag starts over. Returns the frame's id for the markers.
// Every 2 s with Log=1: the game's presents per second and the driver's Reflex sleep status on the device (the frame cap check:
// OptiScaler's cap is a Reflex interval on the same device our Streamline's Reflex sets too)
struct SleepStatus
{
    uint32_t version;
    uint8_t lowLatency, fsVrr, cplVsync;
    uint32_t sleepIntervalUs;
    uint8_t useGameSleep, fsIFlip, fgMultiplier;
    uint8_t rsvd[119];
};
inline void CapCheck(Ctx& c)
{
    static uint64_t t0 = 0, n = 0;
    static int(__cdecl * getStatus)(IUnknown*, SleepStatus*) = nullptr;
    static bool looked = false;
    if (!g_log || !c.dev)
        return;
    LARGE_INTEGER f, t;
    QueryPerformanceFrequency(&f);
    QueryPerformanceCounter(&t);
    ++n;
    if (!t0)
    {
        t0 = t.QuadPart;
        n = 0;
        return;
    }
    const double s = double(t.QuadPart - t0) / f.QuadPart;
    if (s < 2.0)
        return;
    if (!looked)
    {
        looked = true;
        if (HMODULE nv = GetModuleHandleW(L"nvapi64.dll"))
            if (auto qi = reinterpret_cast<void*(__cdecl*)(uint32_t)>(GetProcAddress(nv, "nvapi_QueryInterface")))
                getStatus = reinterpret_cast<decltype(getStatus)>(qi(0xAEF96CA1u)); // NvAPI_D3D_GetSleepStatus
    }
    SleepStatus st {};
    st.version = static_cast<uint32_t>(sizeof(SleepStatus)) | (1u << 16);
    const int r = getStatus ? getStatus(c.dev, &st) : -1;
    g_log("dlssg cap check: %.1f game presents/s; driver sleep status %d: low latency %u, interval %u us, game sleep %u, "
          "FG multiplier %u",
          n / s, r, st.lowLatency, st.sleepIntervalUs, st.useGameSleep, st.fgMultiplier);
    t0 = t.QuadPart;
    n = 0;
}

inline uint32_t BeforePresent(Ctx& c)
{
    CapCheck(c);
    // Once, 600 presents in (OptiScaler-NR 0.1.36 and newer ask when they load the library)
    static unsigned presents = 0;
    if (++presents == 600 && !g_capAsked.load() && g_log)
        g_log("dlssg: WARNING the OptiScaler that loaded this DLL never asked xefgCudaDriverMultipliesReflexCap: a frame cap "
              "through NVIDIA Reflex comes out divided by the multiplier twice (3x with a cap of 90: 30 shown). Use OptiScaler-NR "
              "0.1.36 or newer, or no cap; Generator=own / intel are not affected");
    std::lock_guard<std::mutex> lk(c.mu);
    if (c.enabled && !c.inputsThisFrame && c.appliedOn)
        c.Apply(false);
    c.inputsThisFrame = false;
    return c.frameId.load();
}

// A DLSS-G context for `dev`, or null with the reason in `why`.
inline std::shared_ptr<Ctx> Create(ID3D12Device* dev, const std::wstring& dir, int gpuSm, std::string& why)
{
    Streamline* s = Get(dir, gpuSm, dev, why);
    if (!s)
        return nullptr;
    LUID luid = dev->GetAdapterLuid();
    sl::AdapterInfo ai {};
    ai.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    ai.deviceLUIDSizeInBytes = sizeof(luid);
    if (const sl::Result r = s->isSupported(sl::kFeatureDLSS_G, ai); r != sl::Result::eOk)
    {
        why = "DLSS-G is not supported on this GPU / driver (" + std::to_string(static_cast<int>(r)) + ")";
        return nullptr;
    }
    auto c = std::make_shared<Ctx>();
    c->sl = s;
    c->dev = dev;
    // The maximum now: OptiScaler reads GetProperties before the swap chain exists and offers no multiplier above 1 otherwise
    sl::DLSSGState st {};
    sl::DLSSGOptions none {};
    const sl::Result r = s->getState(c->vp, st, &none);
    if (r == sl::Result::eOk && st.numFramesToGenerateMax)
        c->maxGen = st.numFramesToGenerateMax;
    if (g_log)
        g_log("dlssg: at create, DLSS-G's maximum %u (state %d)", st.numFramesToGenerateMax, static_cast<int>(r));
    unlock::LogAfterStart();
    return c;
}

// F5LOW_SINK_STATS=1 (tests): the sink's statistics into the log, once the context is gone
inline void LogSinkStats()
{
    unlock::LogCudaTotals();
    if (!g_log || !xsink::g_stats)
        return;
    for (const std::string& t : xsink::StatsLines())
        g_log("dlssg: %s", t.c_str());
}
} // namespace xdlssg

namespace xefgown
{
// The IGenerator face of dlssg: what the host asks before it routes a context here (Caps), nothing of the per-frame
// pack surface (a present-sink generator has none; the contexts are xdlssg::Ctx, driven from the XeFG entry points).
class DlssgGenerator final : public IGenerator
{
public:
    // gpuSm: the GPU's compute capability as sm_NN (0: no CUDA device behind the adapter, i.e. not NVIDIA)
    explicit DlssgGenerator(int gpuSm)
    {
        caps.name = "DLSS-G";
        caps.presentsItself = true;
        caps.anyGpu = false;
        // 5 (6x) on every tier: native on RTX 50 (numFramesToGenerateMax 5 with NVIDIA's 310.9 files, measured on an RTX 5070 Ti), and
        // after the unlock (unlock/Unlock.h) on RTX 20/30/40. The context creation replaces it with what the real library reports.
        caps.maxGenerated = 5;
        unlock::Tier tier;
        if (gpuSm <= 0)
            Fail("DLSS-G needs an NVIDIA GPU (no CUDA device behind this adapter)");
        else if (gpuSm < 75)
            Fail("DLSS-G needs RTX 20 or newer");
        else if (unlock::TierOfSm(gpuSm, tier) != 0)
            Fail("DLSS-G is not supported on this NVIDIA GPU (sm_" + std::to_string(gpuSm) + "; GeForce RTX 20 to 50 only)");
        else
            caps.available = true;
    }
    GenCaps Caps() const override
    {
        GenCaps c = caps;
        c.reason = reason.c_str();
        return c;
    }
    // Streamline could not be set up (missing files, slInit failed ...): not available, with the reason.
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

    // Present-sink generators are not built by Own::Create's pack machinery.
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
