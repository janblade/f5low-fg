// sl_unwrap.h - the swap chain under a Streamline proxy.
//
// A game that ships its own Streamline (sl.interposer.dll) hands the host a DXGI factory wrapped by Streamline, and a swap chain made
// through that factory is Streamline's proxy of it. Streamline's present hook runs for every Present on that proxy. Our presenter
// threads (the own generator's presenter, the sink's pacer) present from threads of their own, while the game presents on ITS chain
// (also a Streamline proxy, on the game's thread): Streamline's hook then runs on two threads at once, and that code is not thread
// safe - in The Last of Us Part I it freed the same per-frame block twice (heap corruption 0xC0000374, the game gone without a trace).
// Streamline must see the game's one Present per frame, not ours. So our threads present on the chain under the proxy, which
// Streamline hands out for its base-interface IID (the same one OptiScaler uses, IFGFeature::CheckForRealObject).
//
// The proxy itself is kept for as long as the chain is ours (it is what the window's swap chain is to Streamline): release it after
// the chain under it. f5low's own Streamline (generators/dlssg.h, DLSS-G) is a different case and is NOT unwrapped: there Streamline
// is the frame generator and must see our presents.
#pragma once

#include <dxgi1_6.h>

namespace slunwrap
{
// Streamline's base-interface IID: QueryInterface on a Streamline proxy gives the interface it wraps (AddRef'd)
inline const GUID kBaseInterface = { 0xADEC44E2, 0x61F0, 0x45C3, { 0xAD, 0x9F, 0x1B, 0x37, 0x37, 0x92, 0x84, 0xFF } };

// sc: a chain we made (one reference, ours). A Streamline proxy: sc becomes the chain under it (one reference, ours) and the proxy's
// reference moves to *proxy (release it after sc). Anything else: sc and *proxy (null) stay as they are. True when unwrapped.
inline bool Unwrap(IDXGISwapChain4*& sc, IUnknown** proxy)
{
    *proxy = nullptr;
    if (!sc)
        return false;
    IUnknown* base = nullptr;
    if (FAILED(sc->QueryInterface(kBaseInterface, reinterpret_cast<void**>(&base))) || !base)
        return false;
    IDXGISwapChain4* base4 = nullptr;
    const bool ok = SUCCEEDED(base->QueryInterface(IID_PPV_ARGS(&base4))) && base4 && base4 != sc;
    base->Release();
    if (!ok)
    {
        if (base4)
            base4->Release();
        return false;
    }
    *proxy = sc;
    sc = base4;
    return true;
}
} // namespace slunwrap
