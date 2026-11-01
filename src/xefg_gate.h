// xefg_gate.h - a frame-generation pacer on the swap chain (plans/2026-10-24-xefg-swapchain-gate-pacer.md, S1).
//
// The frame-generation library (Intel's libxess_fg, or ours) creates its real swap chain through the factory it is
// given. Wrapping that factory wraps the swap chain, and each Present then goes through a gate on the GPU:
//   - before the Present, on the swap chain's queue: Signal(ready, n) - the frame's work done - and Wait(gate, n):
//     the GPU holds the Present (and whatever the library queues there after it) until the gate opens;
//   - the release thread opens the gate for each Present in turn at its time on a clock (CPU Signal of the gate).
// The library's own thread never waits for us: its frame generation keeps running ahead (the unlocker 1.7 gates
// Intel's presents the same way, inside Intel's code; here nothing of the library is touched, any version works).
// More buffers (Buffers=) let it run ahead by more than the 3 most libraries ask for.
//
// The clock needs no knowledge of which frames are generated: every Present is released one spacing after the last,
// never before its frame is done. Spacing: how fast frames become ready (their ready times over the last 32, a gap
// over 8 spacings - a game stutter - left out). The frames that wait least at the gate (10th percentile of the last
// 32) are held at a small margin by a correction of the spacing of at most 4%: more wait, release faster; frames
// late, release slower (fast towards faster, slow towards slower). Never more than MaxDelay behind its ready time.
#pragma once

#define NOMINMAX
#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cmath>
#include <condition_variable>
#include <cstdarg>
#include <cstdio>
#include <deque>
#include <mutex>
#include <thread>

namespace gate
{
struct Options
{
    bool on = true;
    UINT buffers = 8;                   // the swap chain's buffers at least (0: as asked)
    UINT latency = 7;                   // its maximum frame latency at least (0: as asked)
    double maxDelayMs = 60;
    double marginMs = 1.0;              // the wait at the gate the least-waiting frames are held at
    FILE* log = nullptr;
};

inline long long Now()
{
    LARGE_INTEGER t;
    QueryPerformanceCounter(&t);
    return t.QuadPart;
}
inline double Freq()
{
    static const double f = [] {
        LARGE_INTEGER x;
        QueryPerformanceFrequency(&x);
        return static_cast<double>(x.QuadPart);
    }();
    return f;
}
inline double Ms(long long q) { return static_cast<double>(q) * 1000.0 / Freq(); }
inline long long Q(double ms) { return static_cast<long long>(ms * Freq() / 1000.0); }

inline void WaitUntil(long long target)
{
    static thread_local HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, CREATE_WAITABLE_TIMER_HIGH_RESOLUTION, TIMER_ALL_ACCESS);
    for (;;)
    {
        const double ms = Ms(target - Now());
        if (ms <= 0)
            return;
        if (ms > 0.6 && timer)
        {
            LARGE_INTEGER due;
            due.QuadPart = -static_cast<LONGLONG>((ms - 0.5) * 10000.0);
            if (SetWaitableTimerEx(timer, &due, 0, nullptr, nullptr, nullptr, 0))
                WaitForSingleObject(timer, INFINITE);
            else
                Sleep(0);
        }
        else
            YieldProcessor();
    }
}

class Pacer
{
public:
    // work: the library's queue (its frames are rendered there); present: the swap chain's own queue (ours).
    Pacer(ID3D12CommandQueue* work, ID3D12CommandQueue* present, const Options& o) : w(work), q(present), opt(o)
    {
        w->AddRef();
        q->AddRef();
        ID3D12Device* dev = nullptr;
        if (SUCCEEDED(q->GetDevice(IID_PPV_ARGS(&dev))))
        {
            ok = SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&ready))) &&
                 SUCCEEDED(dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&gate)));
            dev->Release();
        }
        if (ok)
            th = std::thread([this] { Run(); });
        Log("gate: library queue %p, present queue %p, %s", static_cast<void*>(w), static_cast<void*>(q),
            ok ? "fences ready" : "NO fences - presents pass through");
    }
    ~Pacer()
    {
        {
            std::lock_guard<std::mutex> l(m);
            quit = true;
        }
        cv.notify_all();
        if (th.joinable())
            th.join();
        if (gate)
        {
            gate->Signal(n);            // nothing may stay held
            gate->Release();
        }
        if (ready)
            ready->Release();
        q->Release();
        w->Release();
    }
    // Before the library's Present (its thread): the frame's work is done when its queue gets here (ready), and the
    // present queue waits for that and for the gate. (Work and present queue are the same today, see Factory.)
    bool Submit()
    {
        std::lock_guard<std::mutex> l(m);
        ++n;
        if (!ok || !opt.on)
            return false;
        w->Signal(ready, n);
        if (w != q)
            q->Wait(ready, n);
        q->Wait(gate, n);
        items.push_back(n);
        cv.notify_one();
        return true;
    }
    void Log(const char* fmt, ...)
    {
        if (!opt.log)
            return;
        va_list a;
        va_start(a, fmt);
        vfprintf(opt.log, fmt, a);
        va_end(a);
        fputc('\n', opt.log);
        fflush(opt.log);
    }

private:
    void Run()
    {
        SetThreadPriority(GetCurrentThread(), THREAD_PRIORITY_HIGHEST);
        HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
        constexpr double kLead = 0.2;   // ms the gate opens before the release time (the GPU's way to the flip)
        for (;;)
        {
            UINT64 v;
            {
                std::unique_lock<std::mutex> l(m);
                cv.wait(l, [this] { return quit || !items.empty(); });
                if (quit)
                    break;
                v = items.front();
                items.pop_front();
            }
            if (ready->GetCompletedValue() < v)
            {
                ready->SetEventOnCompletion(v, ev);
                WaitForSingleObject(ev, 250);
            }
            const long long rd = Now();
            if (ready->GetCompletedValue() < v)
            {
                gate->Signal(v);        // stuck or removed: let it through
                last = 0;
                continue;
            }
            Observe(rd);
            long long t = rd;
            if (spacing > 0 && last && rd - last < Q(250))
            {
                const long long due = last + Q(spacing * (1.0 - corr));
                late += due < rd;
                t = std::clamp(due, rd, rd + Q(opt.maxDelayMs));
            }
            WaitUntil(t - Q(kLead));
            gate->Signal(v);
            const long long rel = std::max(Now() + Q(kLead), t);
            Slack(Ms(rel - rd));
            if (last)
            {
                const double g = Ms(rel - last);
                sum += g;
                sum2 += g * g;
                gmax = std::max(gmax, g);
                ++gaps;
            }
            last = rel;
            if (++released % 600 == 0)
            {
                const double mean = gaps ? sum / gaps : 0;
                Log("gate: spacing %.2f ms, releases every %.2f ms (sd %.2f, max %.2f), wait at the gate p10 %.2f / p50 %.2f ms, "
                    "correction %+.0f ppm, %u late, %u stutters left out (last 600)",
                    spacing, mean, gaps ? std::sqrt(std::max(0.0, sum2 / gaps - mean * mean)) : 0.0, gmax, p10, p50, corr * 1e6, late,
                    stutters);
                sum = sum2 = gmax = 0;
                gaps = late = stutters = 0;
            }
        }
        CloseHandle(ev);
    }
    // How fast frames become ready: the mean delta over the last 32, without stutters.
    void Observe(long long rd)
    {
        if (lastReady)
        {
            const double d = Ms(rd - lastReady);
            if (d > 250)
            {
                dn = 0;
                spacing = 0;
            }
            else if (spacing > 0 && d > 8 * spacing)
                ++stutters;
            else
            {
                deltas[di] = d;
                di = (di + 1) % deltas.size();
                dn = std::min<unsigned>(dn + 1, static_cast<unsigned>(deltas.size()));
                double s = 0;
                for (unsigned i = 0; i < dn; ++i)
                    s += deltas[i];
                if (dn >= 8)
                    spacing = s / dn;
            }
        }
        lastReady = rd;
    }
    // The wait at the gate of the last 32 frames; its 10th percentile held at the margin.
    void Slack(double wait)
    {
        slack[si] = wait;
        si = (si + 1) % slack.size();
        sn = std::min<unsigned>(sn + 1, static_cast<unsigned>(slack.size()));
        if (sn < 8 || spacing <= 0)
            return;
        std::array<double, 32> s = slack;
        std::sort(s.begin(), s.begin() + sn);
        p10 = s[sn / 10];
        p50 = s[sn / 2];
        // More wait than the margin: release faster (positive = shorter spacing), up to 15%; less: slower, up to 4%.
        // Fast towards faster: with the swap chain full the library makes frames only as fast as they are released,
        // so the ready times measure our own rate, and only the wait at the gate shows it is too slow.
        corr = std::clamp(0.1 * (p10 - opt.marginMs) / spacing, -0.04, 0.15);
    }

    ID3D12CommandQueue* w;
    ID3D12CommandQueue* q;
    Options opt;
    ID3D12Fence* ready = nullptr;
    ID3D12Fence* gate = nullptr;
    bool ok = false;
    UINT64 n = 0;
    std::mutex m;
    std::condition_variable cv;
    std::deque<UINT64> items;
    bool quit = false;
    std::thread th;
    // release thread
    long long last = 0, lastReady = 0;
    double spacing = 0, corr = 0, p10 = 0, p50 = 0;
    std::array<double, 32> deltas {}, slack {};
    unsigned dn = 0, di = 0, sn = 0, si = 0;
    unsigned released = 0, late = 0, stutters = 0, gaps = 0;
    double sum = 0, sum2 = 0, gmax = 0;
};

// The library's real swap chain: everything forwarded, each Present through the gate.
class Chain : public IDXGISwapChain4
{
public:
    Chain(IDXGISwapChain4* inner, ID3D12CommandQueue* work, ID3D12CommandQueue* present, const Options& o)
        : sc(inner), opt(o), pacer(work, present, o)
    {
        if (opt.latency)
            sc->SetMaximumFrameLatency(opt.latency);
    }
    ~Chain() { sc->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
    {
        if (!pp)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIDeviceSubObject) ||
            riid == __uuidof(IDXGISwapChain) || riid == __uuidof(IDXGISwapChain1) || riid == __uuidof(IDXGISwapChain2) ||
            riid == __uuidof(IDXGISwapChain3) || riid == __uuidof(IDXGISwapChain4))
        {
            *pp = static_cast<IDXGISwapChain4*>(this);
            AddRef();
            return S_OK;
        }
        return sc->QueryInterface(riid, pp);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs;
        if (!r)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override { return sc->SetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return sc->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override { return sc->GetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** p) override { return sc->GetParent(r, p); }
    HRESULT STDMETHODCALLTYPE GetDevice(REFIID r, void** p) override { return sc->GetDevice(r, p); }
    HRESULT STDMETHODCALLTYPE Present(UINT sync, UINT flags) override
    {
        if (sync == 0 && !(flags & DXGI_PRESENT_TEST))
            pacer.Submit();
        return sc->Present(sync, flags);
    }
    HRESULT STDMETHODCALLTYPE GetBuffer(UINT b, REFIID r, void** p) override { return sc->GetBuffer(b, r, p); }
    HRESULT STDMETHODCALLTYPE SetFullscreenState(BOOL f, IDXGIOutput* o) override { return sc->SetFullscreenState(f, o); }
    HRESULT STDMETHODCALLTYPE GetFullscreenState(BOOL* f, IDXGIOutput** o) override { return sc->GetFullscreenState(f, o); }
    HRESULT STDMETHODCALLTYPE GetDesc(DXGI_SWAP_CHAIN_DESC* d) override { return sc->GetDesc(d); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl) override
    {
        return sc->ResizeBuffers(c && opt.buffers ? std::max(c, opt.buffers) : c, w, h, f, fl);
    }
    HRESULT STDMETHODCALLTYPE ResizeTarget(const DXGI_MODE_DESC* d) override { return sc->ResizeTarget(d); }
    HRESULT STDMETHODCALLTYPE GetContainingOutput(IDXGIOutput** o) override { return sc->GetContainingOutput(o); }
    HRESULT STDMETHODCALLTYPE GetFrameStatistics(DXGI_FRAME_STATISTICS* s) override { return sc->GetFrameStatistics(s); }
    HRESULT STDMETHODCALLTYPE GetLastPresentCount(UINT* c) override { return sc->GetLastPresentCount(c); }
    HRESULT STDMETHODCALLTYPE GetDesc1(DXGI_SWAP_CHAIN_DESC1* d) override { return sc->GetDesc1(d); }
    HRESULT STDMETHODCALLTYPE GetFullscreenDesc(DXGI_SWAP_CHAIN_FULLSCREEN_DESC* d) override { return sc->GetFullscreenDesc(d); }
    HRESULT STDMETHODCALLTYPE GetHwnd(HWND* h) override { return sc->GetHwnd(h); }
    HRESULT STDMETHODCALLTYPE GetCoreWindow(REFIID r, void** p) override { return sc->GetCoreWindow(r, p); }
    HRESULT STDMETHODCALLTYPE Present1(UINT sync, UINT flags, const DXGI_PRESENT_PARAMETERS* pp) override
    {
        if (sync == 0 && !(flags & DXGI_PRESENT_TEST))
            pacer.Submit();
        return sc->Present1(sync, flags, pp);
    }
    BOOL STDMETHODCALLTYPE IsTemporaryMonoSupported() override { return sc->IsTemporaryMonoSupported(); }
    HRESULT STDMETHODCALLTYPE GetRestrictToOutput(IDXGIOutput** o) override { return sc->GetRestrictToOutput(o); }
    HRESULT STDMETHODCALLTYPE SetBackgroundColor(const DXGI_RGBA* c) override { return sc->SetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE GetBackgroundColor(DXGI_RGBA* c) override { return sc->GetBackgroundColor(c); }
    HRESULT STDMETHODCALLTYPE SetRotation(DXGI_MODE_ROTATION r) override { return sc->SetRotation(r); }
    HRESULT STDMETHODCALLTYPE GetRotation(DXGI_MODE_ROTATION* r) override { return sc->GetRotation(r); }
    HRESULT STDMETHODCALLTYPE SetSourceSize(UINT w, UINT h) override { return sc->SetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE GetSourceSize(UINT* w, UINT* h) override { return sc->GetSourceSize(w, h); }
    HRESULT STDMETHODCALLTYPE SetMaximumFrameLatency(UINT m) override
    {
        return sc->SetMaximumFrameLatency(opt.latency ? std::max(m, opt.latency) : m);
    }
    HRESULT STDMETHODCALLTYPE GetMaximumFrameLatency(UINT* m) override { return sc->GetMaximumFrameLatency(m); }
    HANDLE STDMETHODCALLTYPE GetFrameLatencyWaitableObject() override { return sc->GetFrameLatencyWaitableObject(); }
    HRESULT STDMETHODCALLTYPE SetMatrixTransform(const DXGI_MATRIX_3X2_F* m) override { return sc->SetMatrixTransform(m); }
    HRESULT STDMETHODCALLTYPE GetMatrixTransform(DXGI_MATRIX_3X2_F* m) override { return sc->GetMatrixTransform(m); }
    UINT STDMETHODCALLTYPE GetCurrentBackBufferIndex() override { return sc->GetCurrentBackBufferIndex(); }
    HRESULT STDMETHODCALLTYPE CheckColorSpaceSupport(DXGI_COLOR_SPACE_TYPE c, UINT* s) override { return sc->CheckColorSpaceSupport(c, s); }
    HRESULT STDMETHODCALLTYPE SetColorSpace1(DXGI_COLOR_SPACE_TYPE c) override { return sc->SetColorSpace1(c); }
    HRESULT STDMETHODCALLTYPE ResizeBuffers1(UINT c, UINT w, UINT h, DXGI_FORMAT f, UINT fl, const UINT* mask, IUnknown* const* qs) override
    {
        return sc->ResizeBuffers1(c, w, h, f, fl, mask, qs);
    }
    HRESULT STDMETHODCALLTYPE SetHDRMetaData(DXGI_HDR_METADATA_TYPE t, UINT s, void* d) override { return sc->SetHDRMetaData(t, s, d); }

private:
    IDXGISwapChain4* sc;
    Options opt;
    Pacer pacer;
    std::atomic<ULONG> refs { 1 };
};

// The factory handed to the library: its swap chains for a window come back wrapped, with more buffers.
class Factory : public IDXGIFactory5
{
public:
    Factory(IDXGIFactory5* inner, const Options& o) : f(inner), opt(o) {}
    ~Factory() { f->Release(); }

    HRESULT STDMETHODCALLTYPE QueryInterface(REFIID riid, void** pp) override
    {
        if (!pp)
            return E_POINTER;
        if (riid == __uuidof(IUnknown) || riid == __uuidof(IDXGIObject) || riid == __uuidof(IDXGIFactory) ||
            riid == __uuidof(IDXGIFactory1) || riid == __uuidof(IDXGIFactory2) || riid == __uuidof(IDXGIFactory3) ||
            riid == __uuidof(IDXGIFactory4) || riid == __uuidof(IDXGIFactory5))
        {
            *pp = static_cast<IDXGIFactory5*>(this);
            AddRef();
            return S_OK;
        }
        return f->QueryInterface(riid, pp);
    }
    ULONG STDMETHODCALLTYPE AddRef() override { return ++refs; }
    ULONG STDMETHODCALLTYPE Release() override
    {
        const ULONG r = --refs;
        if (!r)
            delete this;
        return r;
    }
    HRESULT STDMETHODCALLTYPE SetPrivateData(REFGUID g, UINT n, const void* d) override { return f->SetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE SetPrivateDataInterface(REFGUID g, const IUnknown* u) override { return f->SetPrivateDataInterface(g, u); }
    HRESULT STDMETHODCALLTYPE GetPrivateData(REFGUID g, UINT* n, void* d) override { return f->GetPrivateData(g, n, d); }
    HRESULT STDMETHODCALLTYPE GetParent(REFIID r, void** p) override { return f->GetParent(r, p); }
    HRESULT STDMETHODCALLTYPE EnumAdapters(UINT i, IDXGIAdapter** a) override { return f->EnumAdapters(i, a); }
    HRESULT STDMETHODCALLTYPE MakeWindowAssociation(HWND h, UINT fl) override { return f->MakeWindowAssociation(h, fl); }
    HRESULT STDMETHODCALLTYPE GetWindowAssociation(HWND* h) override { return f->GetWindowAssociation(h); }
    HRESULT STDMETHODCALLTYPE CreateSwapChain(IUnknown* d, DXGI_SWAP_CHAIN_DESC* desc, IDXGISwapChain** sc) override
    {
        return f->CreateSwapChain(d, desc, sc);
    }
    HRESULT STDMETHODCALLTYPE CreateSoftwareAdapter(HMODULE m, IDXGIAdapter** a) override { return f->CreateSoftwareAdapter(m, a); }
    HRESULT STDMETHODCALLTYPE EnumAdapters1(UINT i, IDXGIAdapter1** a) override { return f->EnumAdapters1(i, a); }
    BOOL STDMETHODCALLTYPE IsCurrent() override { return f->IsCurrent(); }
    BOOL STDMETHODCALLTYPE IsWindowedStereoEnabled() override { return f->IsWindowedStereoEnabled(); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForHwnd(IUnknown* d, HWND h, const DXGI_SWAP_CHAIN_DESC1* desc,
                                                     const DXGI_SWAP_CHAIN_FULLSCREEN_DESC* fs, IDXGIOutput* o, IDXGISwapChain1** out) override
    {
        ID3D12CommandQueue* q = nullptr;
        if (!desc || !out || FAILED(d->QueryInterface(IID_PPV_ARGS(&q))))
            return f->CreateSwapChainForHwnd(d, h, desc, fs, o, out);
        // The swap chain stays on the library's queue: D3D12 lets only the swap chain's own queue write its buffers
        // (a present queue of our own: device removed at the first Present, harness 2026-10-24). So the gate holds
        // that queue, and with it whatever the library runs there after the Present (Intel's own generation).
        ID3D12CommandQueue* pq = q;
        pq->AddRef();
        DXGI_SWAP_CHAIN_DESC1 dd = *desc;
        if (opt.buffers && dd.BufferCount < opt.buffers)
            dd.BufferCount = opt.buffers;
        IDXGISwapChain1* sc1 = nullptr;
        HRESULT hr = f->CreateSwapChainForHwnd(pq, h, &dd, fs, o, &sc1);
        if (FAILED(hr) && dd.BufferCount != desc->BufferCount)
        {
            dd.BufferCount = desc->BufferCount;  // as asked, then
            hr = f->CreateSwapChainForHwnd(pq, h, &dd, fs, o, &sc1);
        }
        IDXGISwapChain4* sc4 = nullptr;
        if (FAILED(hr) || FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc4))))
        {
            if (sc1)
                *out = sc1;
            q->Release();
            pq->Release();
            return hr;
        }
        sc1->Release();
        if (opt.log)
        {
            fprintf(opt.log, "gate: swap chain %ux%u, %u buffers (asked %u), flags 0x%x\n", dd.Width, dd.Height, dd.BufferCount,
                    desc->BufferCount, dd.Flags);
            fflush(opt.log);
        }
        *out = new Chain(sc4, q, pq, opt);
        q->Release();
        pq->Release();
        return hr;
    }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForCoreWindow(IUnknown* d, IUnknown* w, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* o,
                                                           IDXGISwapChain1** sc) override
    {
        return f->CreateSwapChainForCoreWindow(d, w, desc, o, sc);
    }
    HRESULT STDMETHODCALLTYPE GetSharedResourceAdapterLuid(HANDLE r, LUID* l) override { return f->GetSharedResourceAdapterLuid(r, l); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusWindow(HWND h, UINT m, DWORD* c) override { return f->RegisterStereoStatusWindow(h, m, c); }
    HRESULT STDMETHODCALLTYPE RegisterStereoStatusEvent(HANDLE e, DWORD* c) override { return f->RegisterStereoStatusEvent(e, c); }
    void STDMETHODCALLTYPE UnregisterStereoStatus(DWORD c) override { f->UnregisterStereoStatus(c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusWindow(HWND h, UINT m, DWORD* c) override { return f->RegisterOcclusionStatusWindow(h, m, c); }
    HRESULT STDMETHODCALLTYPE RegisterOcclusionStatusEvent(HANDLE e, DWORD* c) override { return f->RegisterOcclusionStatusEvent(e, c); }
    void STDMETHODCALLTYPE UnregisterOcclusionStatus(DWORD c) override { f->UnregisterOcclusionStatus(c); }
    HRESULT STDMETHODCALLTYPE CreateSwapChainForComposition(IUnknown* d, const DXGI_SWAP_CHAIN_DESC1* desc, IDXGIOutput* o,
                                                            IDXGISwapChain1** sc) override
    {
        return f->CreateSwapChainForComposition(d, desc, o, sc);
    }
    UINT STDMETHODCALLTYPE GetCreationFlags() override { return f->GetCreationFlags(); }
    HRESULT STDMETHODCALLTYPE EnumAdapterByLuid(LUID l, REFIID r, void** a) override { return f->EnumAdapterByLuid(l, r, a); }
    HRESULT STDMETHODCALLTYPE EnumWarpAdapter(REFIID r, void** a) override { return f->EnumWarpAdapter(r, a); }
    HRESULT STDMETHODCALLTYPE CheckFeatureSupport(DXGI_FEATURE fe, void* d, UINT s) override { return f->CheckFeatureSupport(fe, d, s); }

private:
    IDXGIFactory5* f;
    Options opt;
    std::atomic<ULONG> refs { 1 };
};
} // namespace gate
