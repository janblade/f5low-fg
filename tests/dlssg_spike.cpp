// dlssg_spike - a standalone check of the dlssg generator's ground rules (generators/dlssg.h): does NVIDIA DLSS-G, driven through
// Streamline's public API, present its frames into our sink (src/sink/sink.h)? No f5low.dll involved: this exe and the player's own
// NVIDIA files (a folder with Streamline, plain or renamed, see SPIKE_NAME) stay outside the repo.
//
// Streamline with manual hooking: our SinkFactory (over the real DXGI factory) goes through slUpgradeInterface, so the swap chain
// Streamline creates for the application sits on our sink, and every frame sl.dlss_g presents (generated and the application's)
// lands in SinkSwapChain::Present. The application side is a test pattern: a bar moving 8 px per frame, motion vectors to match.
//
// Usage: dlssg_spike.exe <folder with sl.interposer.dll, sl.common/dlss_g/reflex/pcl.dll and nvngx_dlssg.dll>
//   SPIKE_SINK=0    no sink: Streamline over a logging factory (the DXGI calls DLSS-G makes on the real chain, driver pacing)
//   SPIKE_SINK=-1   Streamline over the real factory itself (no proxy of ours at all)
//   SPIKE_FPS=n     application frames per second (default 30)
//   SPIKE_OFF=n     frames before DLSS-G is turned on (default 60)
//   SPIKE_ON=n      frames with it on (default 600)
//   SPIKE_GEN=n     generated frames per application frame (default DLSS-G's maximum)
//   SPIKE_SLLOG=1   Streamline's own log lines (warnings and errors always)
//   SPIKE_DEVUP=0   the device not upgraded (a game's device as xefg_cuda gets it), the queue made on the plain device
//   SPIKE_NAME=f    the interposer's file name in the folder (default sl.interposer.dll; a renamed copy keeps OptiScaler's hooks off it)
//   SPIKE_MARKERS=n 2 all PCL markers + Reflex sleep (default), 1 present markers only (what xefg_cuda sees), 0 none
//   SPIKE_REFLEX=0  Reflex mode off
//   SPIKE_OTHER=dir another Streamline (sl.interposer.dll in dir) loaded and initialised first with DLSS-G, as a game's own would be
// Exit code: 0 ok, 1 setup failed, 3 a Present failed / the device was removed.

#include <windows.h>
#include <d3d12.h>
#include <dxgi1_6.h>

#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <string>
#include <vector>

#include <sl.h>
#include <sl_dlss_g.h>
#include <sl_pcl.h>
#include <sl_reflex.h>

#include "../src/sink/scproxy.h"
#include "../src/sink/sink.h"

#pragma comment(lib, "d3d12.lib")
#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "user32.lib")

static int EnvInt(const char* name, int def)
{
    char v[16] = {};
    return GetEnvironmentVariableA(name, v, sizeof(v)) ? atoi(v) : def;
}

static bool g_slVerbose = false;
static void SlLog(sl::LogType type, const char* msg)
{
    if (type != sl::LogType::eInfo || g_slVerbose)
        printf("  [sl %d] %s", static_cast<int>(type), msg);
}

static sl::float4x4 Identity()
{
    sl::float4x4 m;
    for (uint32_t i = 0; i < 4; ++i)
        m.row[i] = sl::float4(i == 0 ? 1.f : 0.f, i == 1 ? 1.f : 0.f, i == 2 ? 1.f : 0.f, i == 3 ? 1.f : 0.f);
    return m;
}

int main(int argc, char** argv)
{
    setvbuf(stdout, nullptr, _IONBF, 0);
    if (argc < 2)
    {
        printf("usage: dlssg_spike <streamline folder>\n");
        return 1;
    }
    const int sinkMode = EnvInt("SPIKE_SINK", 1);
    const bool useSink = sinkMode > 0;
    const int appFps = EnvInt("SPIKE_FPS", 30), framesOff = EnvInt("SPIKE_OFF", 60), framesOn = EnvInt("SPIKE_ON", 600);
    g_slVerbose = EnvInt("SPIKE_SLLOG", 0) != 0;
    const UINT rw = 1280, rh = 720, ow = 1920, oh = 1080;

    const std::string dir = argv[1];
    const std::wstring wdir(dir.begin(), dir.end());
    const bool devUp = EnvInt("SPIKE_DEVUP", 1) != 0;
    const int markers = EnvInt("SPIKE_MARKERS", 2);
    char nameBuf[128] = "sl.interposer.dll", otherBuf[MAX_PATH] = "";
    GetEnvironmentVariableA("SPIKE_NAME", nameBuf, sizeof(nameBuf));
    GetEnvironmentVariableA("SPIKE_OTHER", otherBuf, sizeof(otherBuf));
    const std::string otherDir = otherBuf;
    const std::wstring wother(otherDir.begin(), otherDir.end());
    // The "game's" Streamline first: loaded and initialised with DLSS-G + Reflex + PCL, its own plugin folder
    if (!otherDir.empty())
    {
        HMODULE o = LoadLibraryA((otherDir + "\\sl.interposer.dll").c_str());
        auto oInit = o ? reinterpret_cast<PFun_slInit*>(GetProcAddress(o, "slInit")) : nullptr;
        const sl::Feature of[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
        const wchar_t* op[] = { wother.c_str() };
        sl::Preferences p {};
        p.logLevel = sl::LogLevel::eDefault;
        p.logMessageCallback = SlLog;
        p.pathsToPlugins = op;
        p.numPathsToPlugins = 1;
        p.featuresToLoad = of;
        p.numFeaturesToLoad = 3;
        p.applicationId = 0x5EC0A11E;
        printf("other Streamline %p, slInit: %d\n", static_cast<void*>(o), oInit ? static_cast<int>(oInit(p, sl::kSDKVersion)) : -1);
    }
    HMODULE sl = LoadLibraryA((dir + "\\" + nameBuf).c_str());
    if (!sl)
    {
        printf("cannot load %s from %s\n", nameBuf, dir.c_str());
        return 1;
    }
    printf("our Streamline %s: %p\n", nameBuf, static_cast<void*>(sl));
#define SLFN(name) auto name = reinterpret_cast<PFun_##name*>(GetProcAddress(sl, #name))
    SLFN(slInit);
    SLFN(slShutdown);
    SLFN(slIsFeatureSupported);
    SLFN(slSetTagForFrame);
    SLFN(slSetConstants);
    SLFN(slUpgradeInterface);
    SLFN(slGetFeatureFunction);
    SLFN(slGetNewFrameToken);
    SLFN(slSetD3DDevice);
#undef SLFN
    if (!slInit || !slGetFeatureFunction || !slUpgradeInterface)
    {
        printf("sl.interposer.dll lacks the API\n");
        return 1;
    }

    const sl::Feature features[] = { sl::kFeatureDLSS_G, sl::kFeatureReflex, sl::kFeaturePCL };
    const wchar_t* paths[] = { wdir.c_str() };
    sl::Preferences pref {};
    pref.logLevel = g_slVerbose ? sl::LogLevel::eVerbose : sl::LogLevel::eDefault;
    pref.logMessageCallback = SlLog;
    pref.pathsToPlugins = paths;
    pref.numPathsToPlugins = 1;
    // A proxy over the factory we hand it (not its v-table patched: that would be our SinkFactory's), tags per frame token
    pref.flags = sl::PreferenceFlags::eDisableCLStateTracking | sl::PreferenceFlags::eUseManualHooking | sl::PreferenceFlags::eUseDXGIFactoryProxy |
                 sl::PreferenceFlags::eUseFrameBasedResourceTagging;
    pref.featuresToLoad = features;
    pref.numFeaturesToLoad = 3;
    pref.engine = sl::EngineType::eCustom;
    pref.applicationId = 0x5EC0A11D;        // any non-zero id: NGX features stay off with none
    pref.renderAPI = sl::RenderAPI::eD3D12;
    printf("slInit: %d\n", static_cast<int>(slInit(pref, sl::kSDKVersion)));

    IDXGIFactory4* fac = nullptr;
    CreateDXGIFactory1(IID_PPV_ARGS(&fac));
    ID3D12Device* dev = nullptr;
    LUID luid {};
    for (UINT i = 0; !dev; ++i)
    {
        IDXGIAdapter1* a = nullptr;
        if (fac->EnumAdapters1(i, &a) != S_OK)
            break;
        DXGI_ADAPTER_DESC1 ad;
        a->GetDesc1(&ad);
        if (!(ad.Flags & DXGI_ADAPTER_FLAG_SOFTWARE) && SUCCEEDED(D3D12CreateDevice(a, D3D_FEATURE_LEVEL_12_0, IID_PPV_ARGS(&dev))))
        {
            printf("adapter: %ls\n", ad.Description);
            luid = ad.AdapterLuid;
        }
        a->Release();
    }
    if (!dev)
        return 1;
    sl::AdapterInfo ai {};
    ai.deviceLUID = reinterpret_cast<uint8_t*>(&luid);
    ai.deviceLUIDSizeInBytes = sizeof(luid);
    printf("slIsFeatureSupported DLSS-G: %d\n", static_cast<int>(slIsFeatureSupported(sl::kFeatureDLSS_G, ai)));
    printf("slSetD3DDevice: %d\n", static_cast<int>(slSetD3DDevice(dev)));

    // The application's device and queue go through Streamline (manual hooking): its proxies see the presents and the queue
    ID3D12Device* slDev = dev;
    dev->AddRef();
    if (devUp)
        printf("slUpgradeInterface device: %d\n", static_cast<int>(slUpgradeInterface(reinterpret_cast<void**>(&slDev))));
    else
        printf("device not upgraded (as in a game)\n");
    ID3D12CommandQueue* q = nullptr;
    D3D12_COMMAND_QUEUE_DESC qd { D3D12_COMMAND_LIST_TYPE_DIRECT };
    slDev->CreateCommandQueue(&qd, IID_PPV_ARGS(&q));

    using PFunGetState = PFun_slDLSSGGetState;
    using PFunSetOptions = PFun_slDLSSGSetOptions;
    void *fGet = nullptr, *fSet = nullptr, *fSleep = nullptr, *fReflexOpt = nullptr, *fMarker = nullptr;
    slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGGetState", fGet);
    slGetFeatureFunction(sl::kFeatureDLSS_G, "slDLSSGSetOptions", fSet);
    slGetFeatureFunction(sl::kFeatureReflex, "slReflexSleep", fSleep);
    slGetFeatureFunction(sl::kFeatureReflex, "slReflexSetOptions", fReflexOpt);
    slGetFeatureFunction(sl::kFeaturePCL, "slPCLSetMarker", fMarker);
    auto dlssgGetState = reinterpret_cast<PFunGetState*>(fGet);
    auto dlssgSetOptions = reinterpret_cast<PFunSetOptions*>(fSet);
    auto reflexSleep = reinterpret_cast<PFun_slReflexSleep*>(fSleep);
    auto reflexSetOptions = reinterpret_cast<PFun_slReflexSetOptions*>(fReflexOpt);
    auto pclMarker = reinterpret_cast<PFun_slPCLSetMarker*>(fMarker);
    if (!dlssgGetState || !dlssgSetOptions || !reflexSleep || !reflexSetOptions || !pclMarker)
    {
        printf("feature functions missing: getState %p setOptions %p sleep %p reflexOptions %p marker %p\n", fGet, fSet, fSleep, fReflexOpt, fMarker);
        return 1;
    }
    sl::ReflexOptions ro {};
    ro.mode = EnvInt("SPIKE_REFLEX", 1) ? sl::ReflexMode::eLowLatency : sl::ReflexMode::eOff;
    printf("slReflexSetOptions: %d\n", static_cast<int>(reflexSetOptions(ro)));

    // The factory Streamline wraps: our sink over the real one, or a logging pass-through
    scproxy::g_enabled = true;
    xsink::SinkFactory* sinkFac = useSink ? new xsink::SinkFactory(fac, true) : nullptr;     // DLSS-G takes its buffers once: stable buffers
    IDXGIFactory* slFac = sinkFac ? static_cast<IDXGIFactory*>(static_cast<IDXGIFactory4*>(sinkFac))
                        : sinkMode < 0 ? static_cast<IDXGIFactory*>(fac) : new scproxy::LogFactory(fac);
    printf("slUpgradeInterface factory (%s): %d\n", useSink ? "sink" : sinkMode < 0 ? "real" : "logging", static_cast<int>(slUpgradeInterface(reinterpret_cast<void**>(&slFac))));
    IDXGIFactory2* slFac2 = nullptr;
    slFac->QueryInterface(IID_PPV_ARGS(&slFac2));

    WNDCLASSA wc {};
    wc.lpfnWndProc = DefWindowProcA;
    wc.hInstance = GetModuleHandleA(nullptr);
    wc.lpszClassName = "dlssg_spike";
    RegisterClassA(&wc);
    HWND wnd = CreateWindowA("dlssg_spike", "dlssg_spike", WS_OVERLAPPEDWINDOW | WS_VISIBLE, 0, 0, 960, 540, nullptr, nullptr, wc.hInstance, nullptr);
    // DLSS-G generates only while its window is in front (runs with the window behind the editor: 1 frame per app frame). Windows
    // refuses SetForegroundWindow to a background process unless it just had input: a synthetic Alt press gives it that.
    INPUT alt[2] {};
    alt[0].type = alt[1].type = INPUT_KEYBOARD;
    alt[0].ki.wVk = alt[1].ki.wVk = VK_MENU;
    alt[1].ki.dwFlags = KEYEVENTF_KEYUP;
    SendInput(2, alt, sizeof(INPUT));
    SetForegroundWindow(wnd);
    BringWindowToTop(wnd);
    printf("window in front: %d\n", GetForegroundWindow() == wnd ? 1 : 0);
    DXGI_SWAP_CHAIN_DESC1 sd {};
    sd.Width = ow;
    sd.Height = oh;
    sd.Format = DXGI_FORMAT_R8G8B8A8_UNORM;
    sd.SampleDesc.Count = 1;
    sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
    sd.BufferCount = 3;
    sd.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
    sd.Flags = DXGI_SWAP_CHAIN_FLAG_ALLOW_TEARING;
    IDXGISwapChain1* sc1 = nullptr;
    HRESULT hr = slFac2 ? slFac2->CreateSwapChainForHwnd(q, wnd, &sd, nullptr, nullptr, &sc1) : E_NOINTERFACE;
    printf("CreateSwapChainForHwnd through Streamline: 0x%08lx, our sink %s\n", static_cast<unsigned long>(hr),
           sinkFac ? (sinkFac->Sink() ? "got it" : "NOT used") : "off");
    IDXGISwapChain4* sc = nullptr;
    if (FAILED(hr) || !sc1 || FAILED(sc1->QueryInterface(IID_PPV_ARGS(&sc))))
        return 1;
    sc1->Release();

    const sl::ViewportHandle vp(0);
    sl::DLSSGOptions go {};
    sl::DLSSGState gs {};
    dlssgGetState(vp, gs, &go);
    const uint32_t genMax = gs.numFramesToGenerateMax ? gs.numFramesToGenerateMax : 1;
    const uint32_t gen = EnvInt("SPIKE_GEN", 0) > 0 ? (std::min<uint32_t>)(EnvInt("SPIKE_GEN", 0), genMax) : genMax;
    printf("DLSS-G: max generated %u (using %u), status 0x%x, VSync support %d\n", gs.numFramesToGenerateMax, gen, static_cast<unsigned>(gs.status),
           static_cast<int>(gs.bIsVsyncSupportAvailable));

    // Inputs: motion R16G16_FLOAT (8 px per frame to the right, in render pixels), depth R32_FLOAT, hud-less = a copy of the back buffer
    auto tex = [&](UINT w, UINT h, DXGI_FORMAT f, D3D12_RESOURCE_FLAGS fl, D3D12_RESOURCE_STATES st, const D3D12_CLEAR_VALUE* cv) {
        D3D12_HEAP_PROPERTIES hp { D3D12_HEAP_TYPE_DEFAULT };
        D3D12_RESOURCE_DESC rd {};
        rd.Dimension = D3D12_RESOURCE_DIMENSION_TEXTURE2D;
        rd.Width = w;
        rd.Height = h;
        rd.DepthOrArraySize = rd.MipLevels = 1;
        rd.Format = f;
        rd.SampleDesc.Count = 1;
        rd.Flags = fl;
        ID3D12Resource* r = nullptr;
        dev->CreateCommittedResource(&hp, D3D12_HEAP_FLAG_NONE, &rd, st, cv, IID_PPV_ARGS(&r));
        return r;
    };
    ID3D12Resource* mv = tex(rw, rh, DXGI_FORMAT_R16G16_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
    ID3D12Resource* depth = tex(rw, rh, DXGI_FORMAT_R32_FLOAT, D3D12_RESOURCE_FLAG_ALLOW_RENDER_TARGET, D3D12_RESOURCE_STATE_RENDER_TARGET, nullptr);
    ID3D12DescriptorHeap* rtvHeap = nullptr;
    D3D12_DESCRIPTOR_HEAP_DESC hd { D3D12_DESCRIPTOR_HEAP_TYPE_RTV, 3 };
    dev->CreateDescriptorHeap(&hd, IID_PPV_ARGS(&rtvHeap));
    const UINT rtvStep = dev->GetDescriptorHandleIncrementSize(D3D12_DESCRIPTOR_HEAP_TYPE_RTV);
    D3D12_CPU_DESCRIPTOR_HANDLE rtvBb = rtvHeap->GetCPUDescriptorHandleForHeapStart(), rtvMv = rtvBb, rtvDepth = rtvBb;
    rtvMv.ptr += rtvStep;
    rtvDepth.ptr += 2 * rtvStep;
    dev->CreateRenderTargetView(mv, nullptr, rtvMv);
    dev->CreateRenderTargetView(depth, nullptr, rtvDepth);

    ID3D12CommandAllocator* alloc[3] {};
    UINT64 allocFence[3] {};
    for (auto& a : alloc)
        dev->CreateCommandAllocator(D3D12_COMMAND_LIST_TYPE_DIRECT, IID_PPV_ARGS(&a));
    ID3D12GraphicsCommandList* cl = nullptr;
    dev->CreateCommandList(0, D3D12_COMMAND_LIST_TYPE_DIRECT, alloc[0], nullptr, IID_PPV_ARGS(&cl));
    cl->Close();
    ID3D12Fence* fence = nullptr;
    UINT64 fv = 0;
    dev->CreateFence(0, D3D12_FENCE_FLAG_NONE, IID_PPV_ARGS(&fence));
    HANDLE ev = CreateEventW(nullptr, FALSE, FALSE, nullptr);
    auto barrier = [&](ID3D12Resource* r, D3D12_RESOURCE_STATES a, D3D12_RESOURCE_STATES b) {
        D3D12_RESOURCE_BARRIER br {};
        br.Type = D3D12_RESOURCE_BARRIER_TYPE_TRANSITION;
        br.Transition = { r, D3D12_RESOURCE_BARRIER_ALL_SUBRESOURCES, a, b };
        cl->ResourceBarrier(1, &br);
    };

    int failures = 0;
    uint64_t shown = 0;
    unsigned inFront = 0, behind = 0, oneInFront = 0, oneBehind = 0;     // app frames by window state, and those DLSS-G showed alone
    LARGE_INTEGER fq, due {}, now;
    QueryPerformanceFrequency(&fq);
    for (int frame = 0; frame < framesOff + framesOn && !failures; ++frame)
    {
        MSG msg;
        while (PeekMessageA(&msg, nullptr, 0, 0, PM_REMOVE))
            DispatchMessageA(&msg);
        if (frame % 30 == 0 && GetForegroundWindow() != wnd)        // something took the focus: take it back (see above)
        {
            SendInput(2, alt, sizeof(INPUT));
            SetForegroundWindow(wnd);
        }
        sl::FrameToken* tok = nullptr;
        const uint32_t fi = static_cast<uint32_t>(frame);
        slGetNewFrameToken(tok, &fi);
        if (markers >= 2)
        {
            reflexSleep(*tok);
            pclMarker(sl::PCLMarker::eSimulationStart, *tok);
            pclMarker(sl::PCLMarker::eSimulationEnd, *tok);
            pclMarker(sl::PCLMarker::eRenderSubmitStart, *tok);
        }

        const int a = frame % 3;
        if (fence->GetCompletedValue() < allocFence[a])
        {
            fence->SetEventOnCompletion(allocFence[a], ev);
            WaitForSingleObject(ev, 5000);
        }
        alloc[a]->Reset();
        cl->Reset(alloc[a], nullptr);
        ID3D12Resource* bb = nullptr;
        sc->GetBuffer(sc->GetCurrentBackBufferIndex(), IID_PPV_ARGS(&bb));
        dev->CreateRenderTargetView(bb, nullptr, rtvBb);
        barrier(bb, D3D12_RESOURCE_STATE_PRESENT, D3D12_RESOURCE_STATE_RENDER_TARGET);
        const float bg[4] = { 0.1f, 0.12f, 0.15f, 1.f }, bar[4] = { 0.9f, 0.8f, 0.2f, 1.f };
        cl->ClearRenderTargetView(rtvBb, bg, 0, nullptr);
        const LONG x = static_cast<LONG>((frame * 12) % ow);     // 12 output px = 8 render px per frame
        const D3D12_RECT barRect { x, 0, x + 120, static_cast<LONG>(oh) };
        cl->ClearRenderTargetView(rtvBb, bar, 1, &barRect);
        barrier(bb, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_PRESENT);
        // motion: (pixels this frame moved) in render pixels; DLSS-G gets mvecScale 1/size to normalise
        const float mvv[4] = { -8.f, 0.f, 0.f, 0.f }, dv[4] = { 0.5f, 0.f, 0.f, 0.f };
        cl->ClearRenderTargetView(rtvMv, mvv, 0, nullptr);
        cl->ClearRenderTargetView(rtvDepth, dv, 0, nullptr);
        barrier(mv, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        barrier(depth, D3D12_RESOURCE_STATE_RENDER_TARGET, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);

        sl::Resource rMv(sl::ResourceType::eTex2d, mv, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        sl::Resource rDepth(sl::ResourceType::eTex2d, depth, nullptr, nullptr, D3D12_RESOURCE_STATE_NON_PIXEL_SHADER_RESOURCE);
        const sl::Extent ext { 0, 0, rw, rh };
        sl::ResourceTag tags[] = { sl::ResourceTag(&rMv, sl::kBufferTypeMotionVectors, sl::ResourceLifecycle::eValidUntilPresent, &ext),
                                   sl::ResourceTag(&rDepth, sl::kBufferTypeDepth, sl::ResourceLifecycle::eValidUntilPresent, &ext) };
        const sl::Result tr = slSetTagForFrame(*tok, vp, tags, 2, cl);

        sl::Constants c {};
        c.cameraViewToClip = c.clipToCameraView = c.clipToLensClip = c.clipToPrevClip = c.prevClipToClip = Identity();
        c.jitterOffset = sl::float2(0.f, 0.f);
        c.mvecScale = sl::float2(1.f / rw, 1.f / rh);
        c.cameraPinholeOffset = sl::float2(0.f, 0.f);
        c.cameraPos = sl::float3(0.f, 0.f, 0.f);
        c.cameraUp = sl::float3(0.f, 1.f, 0.f);
        c.cameraRight = sl::float3(1.f, 0.f, 0.f);
        c.cameraFwd = sl::float3(0.f, 0.f, 1.f);
        c.cameraNear = 0.1f;
        c.cameraFar = 1000.f;
        c.cameraFOV = 1.0f;
        c.cameraAspectRatio = static_cast<float>(ow) / oh;
        c.motionVectorsInvalidValue = 0.f;
        c.depthInverted = sl::Boolean::eFalse;
        c.cameraMotionIncluded = sl::Boolean::eTrue;
        c.motionVectors3D = sl::Boolean::eFalse;
        c.reset = frame == framesOff ? sl::Boolean::eTrue : sl::Boolean::eFalse;
        const sl::Result cr = slSetConstants(c, *tok, vp);

        cl->Close();
        ID3D12CommandList* lists[] = { cl };
        q->ExecuteCommandLists(1, lists);
        q->Signal(fence, ++fv);
        allocFence[a] = fv;
        bb->Release();
        if (markers >= 2)
            pclMarker(sl::PCLMarker::eRenderSubmitEnd, *tok);

        if (frame == framesOff || frame == 0)
        {
            go.mode = frame == framesOff ? sl::DLSSGMode::eOn : sl::DLSSGMode::eOff;
            go.numFramesToGenerate = gen;
            go.numBackBuffers = sd.BufferCount;
            go.mvecDepthWidth = rw;
            go.mvecDepthHeight = rh;
            go.colorWidth = ow;
            go.colorHeight = oh;
            go.colorBufferFormat = sd.Format;
            go.mvecBufferFormat = DXGI_FORMAT_R16G16_FLOAT;
            go.depthBufferFormat = DXGI_FORMAT_R32_FLOAT;
            printf("frame %d: tags %d, constants %d, slDLSSGSetOptions %s %u: %d\n", frame, static_cast<int>(tr), static_cast<int>(cr),
                   frame ? "on" : "off", gen, static_cast<int>(dlssgSetOptions(vp, go)));
            if (sinkFac && frame == framesOff)
                sinkFac->SetBurst(gen + 1);
        }
        if (frame == framesOff + 30)
        {
            scproxy::ResetGaps();
            xsink::ResetGaps();
        }
        if (markers >= 1)
            pclMarker(sl::PCLMarker::ePresentStart, *tok);
        hr = sc->Present(0, DXGI_PRESENT_ALLOW_TEARING);
        if (markers >= 1)
            pclMarker(sl::PCLMarker::ePresentEnd, *tok);

        dlssgGetState(vp, gs, nullptr);
        if (frame > framesOff + 30)
        {
            shown += gs.numFramesActuallyPresented;
            const bool front = GetForegroundWindow() == wnd;
            (front ? inFront : behind) += 1;
            if (gs.numFramesActuallyPresented <= 1)
                (front ? oneInFront : oneBehind) += 1;
        }
        if (frame == framesOff + 5 || frame == framesOff + framesOn / 2 || frame == framesOff + framesOn - 1)
            printf("frame %d: DLSS-G presented %u, status 0x%x, window in front %d\n", frame, gs.numFramesActuallyPresented, static_cast<unsigned>(gs.status),
                   GetForegroundWindow() == wnd ? 1 : 0);

        if (appFps > 0)
        {
            QueryPerformanceCounter(&now);
            if (!due.QuadPart || now.QuadPart - due.QuadPart > fq.QuadPart / 4)
                due = now;
            due.QuadPart += fq.QuadPart / appFps;
            xsink::WaitUntil(due.QuadPart);
        }
        if (FAILED(hr) || dev->GetDeviceRemovedReason() != S_OK)
        {
            printf("frame %d: Present 0x%08lx, device removed reason 0x%08lx\n", frame, static_cast<unsigned long>(hr),
                   static_cast<unsigned long>(dev->GetDeviceRemovedReason()));
            ++failures;
        }
    }
    printf("%d frames off, %d on: %s; DLSS-G said %llu frames presented after start-up (%.2f per app frame)\n", framesOff, framesOn,
           failures ? "FAILED" : "no Present failure, device alive", static_cast<unsigned long long>(shown),
           framesOn > 31 ? static_cast<double>(shown) / (framesOn - 31) : 0.0);
    printf("app frames with the window in front %u (of them shown alone %u), behind %u (shown alone %u)\n", inFront, oneInFront, behind, oneBehind);

    go.mode = sl::DLSSGMode::eOff;
    dlssgSetOptions(vp, go);
    q->Signal(fence, ++fv);
    fence->SetEventOnCompletion(fv, ev);
    WaitForSingleObject(ev, 5000);
    sc->Release();
    scproxy::Summary(stdout);
    if (sinkFac)
        xsink::PrintStats(stdout);
    if (slShutdown)
        slShutdown();
    return failures ? 3 : 0;
}
