// Host-only test: the sink's DXGI wrappers refuse Streamline's "real object" IID (src/sink/scproxy.h RefusedIid), so nothing (OptiScaler,
// Streamline) can get the real chain from behind the sink, and they answer their own interfaces themselves. No GPU, no DXGI calls:
// the wrapper is built around a null real chain and only QueryInterface paths that never touch it are used.
// tests/build_sink_iid_test.ps1 builds it; CI runs it. Exit code 0: all checks passed.
#include "../src/sink/scproxy.h"
#include <cstdio>

#pragma comment(lib, "dxgi.lib")
#pragma comment(lib, "ole32.lib")

static int g_fail = 0;
#define CHECK(c)                                                  \
    do                                                            \
    {                                                             \
        if (!(c))                                                 \
        {                                                         \
            printf("FAILED line %d: %s\n", __LINE__, #c);         \
            ++g_fail;                                             \
        }                                                         \
    } while (0)

int main()
{
    GUID sl {};
    CHECK(IIDFromString(L"{ADEC44E2-61F0-45C3-AD9F-1B37379284FF}", &sl) == S_OK);
    CHECK(scproxy::RefusedIid(sl));
    CHECK(!scproxy::RefusedIid(__uuidof(IDXGISwapChain4)));
    CHECK(!scproxy::RefusedIid(__uuidof(IUnknown)));
    GUID other = sl;
    other.Data4[7] ^= 1;
    CHECK(!scproxy::RefusedIid(other));

    // The wrapper itself: the Streamline IID is refused without asking the (here null) real chain; its own interfaces are its own.
    scproxy::LogSwapChain sc(nullptr);
    void* o = reinterpret_cast<void*>(1);
    CHECK(sc.QueryInterface(sl, &o) == E_NOINTERFACE);
    CHECK(o == nullptr);
    void* o4 = nullptr;
    CHECK(sc.QueryInterface(__uuidof(IDXGISwapChain4), &o4) == S_OK);
    CHECK(o4 == static_cast<IDXGISwapChain4*>(&sc));

    // The factory wrapper too (around a real DXGI factory: no GPU needed). A real factory answers E_NOINTERFACE as well, so only
    // the wrapper's own refusal is tested here: *o must be null and the wrapper's own interface must still be its own.
    IDXGIFactory4* real = nullptr;
    if (SUCCEEDED(CreateDXGIFactory1(IID_PPV_ARGS(&real))))
    {
        auto* f = new scproxy::LogFactory(real);
        void* fo = reinterpret_cast<void*>(1);
        CHECK(f->QueryInterface(sl, &fo) == E_NOINTERFACE);
        CHECK(fo == nullptr);
        void* f4 = nullptr;
        CHECK(f->QueryInterface(__uuidof(IDXGIFactory4), &f4) == S_OK);
        CHECK(f4 == static_cast<IDXGIFactory4*>(f));
        static_cast<IDXGIFactory4*>(f4)->Release();
        f->Release();
        real->Release();
    }
    else
        printf("sink_iid_test: no DXGI factory here, factory check skipped\n");

    if (g_fail)
    {
        printf("sink_iid_test: %d FAILED\n", g_fail);
        return 1;
    }
    printf("sink_iid_test: PASS\n");
    return 0;
}
