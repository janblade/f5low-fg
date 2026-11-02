// scproxy.h - a logging IDXGIFactory4 / IDXGISwapChain4 pass-through, for listing every DXGI call a XeSS-FG library makes on the
// factory it is given and on the swap chain it creates from it (epic 2026-10-29-xefg-cuda-pacer-and-generators, Story 3 step 1).
// Hand repro_test a LogFactory (REPRO_LOGSC=1): InitFromSwapChainDesc then creates its swap chain through it and gets a LogSwapChain
// back, so the library's own use of the real chain is seen, not the application's. Every call is forwarded unchanged; nothing is
// faked. A QueryInterface for an IID the proxies do not know goes to the real object and is logged as "QI unknown" - from then on
// that object is no longer logged.
#pragma once
#define NOMINMAX
#include <windows.h>

#include <dxgi1_6.h>

#include <atomic>
#include <cstdarg>
#include <cstdio>
#include <map>
#include <mutex>
#include <algorithm>
#include <cmath>
#include <string>
#include <vector>

namespace scproxy
{
inline std::mutex g_mu;
inline std::map<std::string, unsigned> g_count;
inline std::map<std::string, unsigned> g_threads;   // distinct thread ids per method are summed up below
inline std::map<std::string, std::map<DWORD, unsigned>> g_tid;
inline std::map<std::string, std::pair<double, double>> g_ms;   // per timed method: total and worst ms spent inside the real call
inline std::vector<double> g_presentGaps;           // ms between the library's own Present calls (arrival on the real chain)
inline LONGLONG g_lastPresent = 0;
inline FILE* g_log = nullptr;
inline unsigned g_seq = 0;
inline bool g_done = false;                         // Summary closed the log: later calls (the teardown) are only counted
inline unsigned g_detailPerMethod = 6;              // full lines for the first N calls of each method, then only counted
inline unsigned g_burst = 0;                        // Mark(): how many calls after it are logged in full
// Off by default: nothing is logged, timed or collected (xefg_cuda.dll uses the sink in games: no scproxy.log, no growing statistics).
// The test harness (repro_test) turns it on.
inline bool g_enabled = false;

inline void Log(const char* name, const char* fmt = "", ...)
{
    if (!g_enabled)
        return;
    std::lock_guard<std::mutex> l(g_mu);
    const unsigned n = ++g_count[name];
    ++g_tid[name][GetCurrentThreadId()];
    if (!g_log && !g_done)
        fopen_s(&g_log, "scproxy.log", "w");
    const bool burst = g_burst > 0;     // after a Mark(): the next calls are logged in full, whatever their count
    if (burst)
        --g_burst;
    if (!g_log || (n > g_detailPerMethod && !burst))
        return;
    char buf[512];
    va_list ap;
    va_start(ap, fmt);
    vsnprintf(buf, sizeof(buf), fmt, ap);
    va_end(ap);
    fprintf(g_log, "%5u t%-6lu %s(%s)\n", ++g_seq, GetCurrentThreadId(), name, buf);
    fflush(g_log);
}

// The caller marks an event (REPRO_FS: entering or leaving exclusive fullscreen): a line in the log and the next 60 calls in full.
inline void Mark(const char* what)
{
    Log("MARK", "%s", what);
    std::lock_guard<std::mutex> l(g_mu);
    g_burst = 60;
}

inline void AddTime(const char* name, LONGLONG t0)
{
    if (!g_enabled)
        return;
    LARGE_INTEGER t1, f;
    QueryPerformanceCounter(&t1);
    QueryPerformanceFrequency(&f);
    const double ms = static_cast<double>(t1.QuadPart - t0) * 1000.0 / static_cast<double>(f.QuadPart);
    std::lock_guard<std::mutex> l(g_mu);
    auto& e = g_ms[name];
    e.first += ms;
    e.second = ms > e.second ? ms : e.second;
}

inline void ResetGaps()
{
    std::lock_guard<std::mutex> l(g_mu);
    g_presentGaps.clear();
}

inline void PresentGap(LONGLONG t)
{
    if (!g_enabled)
        return;
    LARGE_INTEGER f;
    QueryPerformanceFrequency(&f);
    std::lock_guard<std::mutex> l(g_mu);
    if (g_lastPresent)
        g_presentGaps.push_back(static_cast<double>(t - g_lastPresent) * 1000.0 / static_cast<double>(f.QuadPart));
    g_lastPresent = t;
}

// The statistics line of a list of values (empty string: fewer than 8). Written into buf (cut to fit).
inline std::string GapText(char* buf, size_t size, const char* what, std::vector<double> v)
{
    if (v.size() < 8)
        return {};
    double m = 0, q = 0;
    for (double x : v)
        m += x;
    m /= static_cast<double>(v.size());
    for (double x : v)
        q += (x - m) * (x - m);
    std::sort(v.begin(), v.end());
    snprintf(buf, size, "%s: %zu gaps  mean %.3f ms  sd %.3f  p01 %.3f  p50 %.3f  p99 %.3f  max %.3f", what, v.size(), m, std::sqrt(q / static_cast<double>(v.size())),
             v[v.size() / 100], v[v.size() / 2], v[(std::min)(v.size() - 1, v.size() * 99 / 100)], v.back());
    return buf;
}

inline void GapStats(FILE* out, const char* what, std::vector<double> v)
{
    char buf[512];
    if (!GapText(buf, sizeof(buf), what, std::move(v)).empty())
        fprintf(out, "%s\n", buf);
}

inline LONGLONG Qpc()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}

inline void Summary(FILE* out)
{
    std::lock_guard<std::mutex> l(g_mu);
    fprintf(out, "---- DXGI calls the library made (count, threads) ----\n");
    for (const auto& [name, n] : g_count)
    {
        fprintf(out, "%-34s %7u  threads:", name.c_str(), n);
        for (const auto& [tid, c] : g_tid[name])
            fprintf(out, " %lu x%u", tid, c);
        fprintf(out, "\n");
    }
    GapStats(out, "gaps between the library's Present calls", g_presentGaps);
    for (const auto& [name, e] : g_ms)
        fprintf(out, "time in %-28s total %9.3f ms  avg %8.4f ms  worst %8.3f ms\n", name.c_str(), e.first, e.first / g_count[name], e.second);
    if (g_log)
        fclose(g_log), g_log = nullptr;
    g_done = true;
}

class LogSwapChain : public IDXGISwapChain4
{
protected:
    IDXGISwapChain4* r;
    std::atomic<ULONG> rc { 1 };

public:
    explicit LogSwapChain(IDXGISwapChain4* real) : r(real) {}
    virtual ~LogSwapChain() = default;
    IDXGISwapChain4* Real() const { return r; }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** o) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) || id == __uuidof(IDXGIDeviceSubObject) || id == __uuidof(IDXGISwapChain) ||
            id == __uuidof(IDXGISwapChain1) || id == __uuidof(IDXGISwapChain2) || id == __uuidof(IDXGISwapChain3) || id == __uuidof(IDXGISwapChain4))
        {
            Log("SC.QueryInterface", "%08lx", id.Data1);
            *o = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        Log("SC.QueryInterface unknown", "%08lx-%04x-%04x", id.Data1, id.Data2, id.Data3);
        return r->QueryInterface(id, o);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++rc; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --rc;
        if (!n)
        {
            r->Release();
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override { Log("SC.SetPrivateData", "size %u", s); return r->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { Log("SC.SetPrivateDataInterface"); return r->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override { Log("SC.GetPrivateData"); return r->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID id, void** p) override { Log("SC.GetParent", "%08lx", id.Data1); return r->GetParent(id, p); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID id, void** p) override { Log("SC.GetDevice", "%08lx", id.Data1); return r->GetDevice(id, p); }
    HRESULT STDMETHODCALLTYPE Present(UINT si, UINT fl) override
    {
        const LONGLONG t0 = Qpc();
        const HRESULT h = r->Present(si, fl);
        AddTime("SC.Present", t0);
        PresentGap(t0);
        Log("SC.Present", "sync %u flags 0x%x -> 0x%lx", si, fl, h);
        return h;
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT b, REFIID id, void** s) override { Log("SC.GetBuffer", "buffer %u iid %08lx", b, id.Data1); return r->GetBuffer(b, id, s); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* o) override { Log("SC.SetFullscreenState", "%d", f); return r->SetFullscreenState(f, o); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override
    {
        Log("SC.GetFullscreenState");
        const LONGLONG t0 = Qpc();
        const HRESULT h = r->GetFullscreenState(f, o);
        AddTime("SC.GetFullscreenState", t0);
        return h;
    }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { Log("SC.GetDesc"); return r->GetDesc(d); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override
    {
        Log("SC.ResizeBuffers", "count %u %ux%u fmt %d flags 0x%x", c, w, h, static_cast<int>(f), fl);
        return r->ResizeBuffers(c, w, h, f, fl);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* m) override { Log("SC.ResizeTarget"); return r->ResizeTarget(m); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { Log("SC.GetContainingOutput"); return r->GetContainingOutput(o); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { Log("SC.GetFrameStatistics"); return r->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { Log("SC.GetLastPresentCount"); return r->GetLastPresentCount(c); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { Log("SC.GetDesc1"); return r->GetDesc1(d); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { Log("SC.GetFullscreenDesc"); return r->GetFullscreenDesc(d); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { Log("SC.GetHwnd"); return r->GetHwnd(h); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID id, void** p) override { Log("SC.GetCoreWindow"); return r->GetCoreWindow(id, p); }
    HRESULT STDMETHODCALLTYPE Present1(UINT si, UINT fl, const DXGI_PRESENT_PARAMETERS* p) override
    {
        const HRESULT h = r->Present1(si, fl, p);
        Log("SC.Present1", "sync %u flags 0x%x dirty %u scroll %d -> 0x%lx", si, fl, p ? p->DirtyRectsCount : 0, p && p->pScrollRect ? 1 : 0, h);
        return h;
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { Log("SC.IsTemporaryMonoSupported"); return r->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { Log("SC.GetRestrictToOutput"); return r->GetRestrictToOutput(o); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { Log("SC.SetBackgroundColor"); return r->SetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { Log("SC.GetBackgroundColor"); return r->GetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION v) override { Log("SC.SetRotation"); return r->SetRotation(v); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* v) override { Log("SC.GetRotation"); return r->GetRotation(v); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { Log("SC.SetSourceSize", "%ux%u", w, h); return r->SetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { Log("SC.GetSourceSize"); return r->GetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT m) override { Log("SC.SetMaximumFrameLatency", "%u", m); return r->SetMaximumFrameLatency(m); }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* m) override { Log("SC.GetMaximumFrameLatency"); return r->GetMaximumFrameLatency(m); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { Log("SC.GetFrameLatencyWaitableObject"); return r->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { Log("SC.SetMatrixTransform"); return r->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { Log("SC.GetMatrixTransform"); return r->GetMatrixTransform(m); }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { Log("SC.GetCurrentBackBufferIndex"); return r->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { Log("SC.CheckColorSpaceSupport", "%d", static_cast<int>(c)); return r->CheckColorSpaceSupport(c, s); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override { Log("SC.SetColorSpace1", "%d", static_cast<int>(c)); return r->SetColorSpace1(c); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* nm, IUnknown* const* q) override
    {
        Log("SC.ResizeBuffers1", "count %u %ux%u fmt %d flags 0x%x", c, w, h, static_cast<int>(f), fl);
        return r->ResizeBuffers1(c, w, h, f, fl, nm, q);
    }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* d) override { Log("SC.SetHDRMetaData", "type %d size %u", static_cast<int>(t), s); return r->SetHDRMetaData(t, s, d); }
};

// Up to IDXGIFactory7 when the real one has it: Streamline asks for IDXGIFactory7, and an interface handed on from the real factory
// would go around us (the swap chain then made on the real factory, not ours).
class LogFactory : public IDXGIFactory7
{
protected:
    IDXGIFactory4* r;
    IDXGIFactory7* r7 = nullptr;
    std::atomic<ULONG> rc { 1 };

public:
    explicit LogFactory(IDXGIFactory4* real) : r(real)
    {
        r->AddRef();
        r->QueryInterface(IID_PPV_ARGS(&r7));
    }
    virtual ~LogFactory() = default;

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID id, void** o) override
    {
        if (id == __uuidof(IUnknown) || id == __uuidof(IDXGIObject) || id == __uuidof(IDXGIFactory) || id == __uuidof(IDXGIFactory1) ||
            id == __uuidof(IDXGIFactory2) || id == __uuidof(IDXGIFactory3) || id == __uuidof(IDXGIFactory4))
        {
            Log("F.QueryInterface", "%08lx", id.Data1);
            *o = static_cast<IDXGIFactory4*>(this);
            AddRef();
            return S_OK;
        }
        if (id == __uuidof(IDXGIFactory5) || id == __uuidof(IDXGIFactory6) || id == __uuidof(IDXGIFactory7))
        {
            Log("F.QueryInterface", "%08lx%s", id.Data1, r7 ? "" : " (refused: the real factory lacks it)");
            if (!r7)
            {
                *o = nullptr;
                return E_NOINTERFACE;
            }
            *o = static_cast<IDXGIFactory7*>(this);
            AddRef();
            return S_OK;
        }
        Log("F.QueryInterface unknown", "%08lx-%04x-%04x", id.Data1, id.Data2, id.Data3);
        return r->QueryInterface(id, o);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++rc; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG n = --rc;
        if (!n)
        {
            if (r7)
                r7->Release();
            r->Release();
            delete this;
        }
        return n;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT s, const void* d) override { Log("F.SetPrivateData"); return r->SetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { Log("F.SetPrivateDataInterface"); return r->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* s, void* d) override { Log("F.GetPrivateData"); return r->GetPrivateData(g, s, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID id, void** p) override { Log("F.GetParent"); return r->GetParent(id, p); }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override { Log("F.EnumAdapters", "%u", i); return r->EnumAdapters(i, a); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND w, UINT f) override { Log("F.MakeWindowAssociation", "flags 0x%x", f); return r->MakeWindowAssociation(w, f); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* w) override { Log("F.GetWindowAssociation"); return r->GetWindowAssociation(w); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** s) override { Log("F.CreateSwapChain"); return r->CreateSwapChain(d, desc, s); }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { Log("F.CreateSoftwareAdapter"); return r->CreateSoftwareAdapter(m, a); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override { Log("F.EnumAdapters1", "%u", i); return r->EnumAdapters1(i, a); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { Log("F.IsCurrent"); return r->IsCurrent(); }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { Log("F.IsWindowedStereoEnabled"); return r->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* d, HWND w, const DXGI_SWAP_CHAIN_DESC1* desc, const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs,
                                                     IDXGIOutput* o, IDXGISwapChain1** s) override
    {
        Log("F.CreateSwapChainForHwnd", "%ux%u fmt %d buffers %u effect %d flags 0x%x scaling %d alpha %d fullscreenDesc %d", desc ? desc->Width : 0,
            desc ? desc->Height : 0, desc ? static_cast<int>(desc->Format) : -1, desc ? desc->BufferCount : 0, desc ? static_cast<int>(desc->SwapEffect) : -1,
            desc ? desc->Flags : 0, desc ? static_cast<int>(desc->Scaling) : -1, desc ? static_cast<int>(desc->AlphaMode) : -1, fs ? 1 : 0);
        IDXGISwapChain1* real = nullptr;
        const HRESULT h = r->CreateSwapChainForHwnd(d, w, desc, fs, o, &real);
        if (FAILED(h) || !real)
        {
            *s = real;
            return h;
        }
        IDXGISwapChain4* real4 = nullptr;
        if (FAILED(real->QueryInterface(IID_PPV_ARGS(&real4))))
        {
            *s = real;      // cannot wrap below IDXGISwapChain4: hand it over as it is
            return h;
        }
        real->Release();
        *s = new LogSwapChain(real4);
        return h;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* d, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* o, IDXGISwapChain1** s) override
    {
        Log("F.CreateSwapChainForCoreWindow");
        return r->CreateSwapChainForCoreWindow(d, w, desc, o, s);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE h, LUID* l) override { Log("F.GetSharedResourceAdapterLuid"); return r->GetSharedResourceAdapterLuid(h, l); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND w, UINT m, DWORD* c) override { Log("F.RegisterStereoStatusWindow"); return r->RegisterStereoStatusWindow(w, m, c); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE e, DWORD* c) override { Log("F.RegisterStereoStatusEvent"); return r->RegisterStereoStatusEvent(e, c); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override { Log("F.UnregisterStereoStatus"); r->UnregisterStereoStatus(c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND w, UINT m, DWORD* c) override { Log("F.RegisterOcclusionStatusWindow"); return r->RegisterOcclusionStatusWindow(w, m, c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE e, DWORD* c) override { Log("F.RegisterOcclusionStatusEvent"); return r->RegisterOcclusionStatusEvent(e, c); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override { Log("F.UnregisterOcclusionStatus"); r->UnregisterOcclusionStatus(c); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* d, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* o, IDXGISwapChain1** s) override
    {
        Log("F.CreateSwapChainForComposition");
        return r->CreateSwapChainForComposition(d, desc, o, s);
    }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { Log("F.GetCreationFlags"); return r->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID l, REFIID id, void** a) override { Log("F.EnumAdapterByLuid"); return r->EnumAdapterByLuid(l, id, a); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID id, void** a) override { Log("F.EnumWarpAdapter"); return r->EnumWarpAdapter(id, a); }
    // IDXGIFactory5-7: only handed out when r7 is there
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE f, void* d, UINT n) override { Log("F.CheckFeatureSupport", "%d", static_cast<int>(f)); return r7->CheckFeatureSupport(f, d, n); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByGpuPreference(UINT i, DXGI_GPU_PREFERENCE g, REFIID id, void** a) override
    {
        Log("F.EnumAdapterByGpuPreference", "%u", i);
        return r7->EnumAdapterByGpuPreference(i, g, id, a);
    }
    HRESULT STDMETHODCALLTYPE RegisterAdaptersChangedEvent(HANDLE e, DWORD* c) override { Log("F.RegisterAdaptersChangedEvent"); return r7->RegisterAdaptersChangedEvent(e, c); }
    HRESULT STDMETHODCALLTYPE UnregisterAdaptersChangedEvent(DWORD c) override { Log("F.UnregisterAdaptersChangedEvent"); return r7->UnregisterAdaptersChangedEvent(c); }
};
} // namespace scproxy
