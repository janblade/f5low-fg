// Host-only test of the nvngx_dlssg ownership rule (src/generators/dlssg_own.h): no GPU, no NVIDIA file. tests/build_dlssg_own_test.ps1
// builds it; CI runs it. Exit code 0: all checks passed.
#include "../src/generators/dlssg_own.h"
#include <cstdio>

static int g_fail = 0;
#define CHECK(c)                                                    \
    do                                                              \
    {                                                               \
        if (!(c))                                                   \
        {                                                           \
            printf("FAILED line %d: %s\n", __LINE__, #c);           \
            ++g_fail;                                               \
        }                                                           \
    } while (0)

int main()
{
    using namespace xdlssg;
    const std::wstring own = L"C:\\Game\\OptiScaler\\plugins\\f5low_streamline\\nvngx_dlssg.dll";
    const std::wstring game = L"C:\\Game\\Binaries\\Win64\\nvngx_dlssg.dll";

    // only ours: fine
    NgxVerdict v = JudgeNgx(own, { own });
    CHECK(v.ownLoaded && v.Ok() && v.OwnInUse());
    // path spelling does not matter (case, slashes, \\?\ prefix, duplicates)
    v = JudgeNgx(own, { L"\\\\?\\c:/game/optiscaler/PLUGINS/f5low_streamline/NVNGX_DLSSG.DLL", own });
    CHECK(v.ownLoaded && v.Ok());
    // only the game's: foreign, ours not loaded
    v = JudgeNgx(own, { game });
    CHECK(!v.ownLoaded && !v.Ok() && !v.OwnInUse() && v.foreign.size() == 1 && v.foreign[0] == game);
    // both: refused too, the foreign one is named
    v = JudgeNgx(own, { own, game });
    CHECK(v.ownLoaded && !v.Ok() && !v.OwnInUse() && v.foreign.size() == 1 && v.foreign[0] == game);
    // nothing loaded (before our preload)
    v = JudgeNgx(own, {});
    CHECK(!v.ownLoaded && v.Ok() && !v.OwnInUse());
    // other modules in the list are ignored; a longer name starting nvngx_dlssg counts
    v = JudgeNgx(own, { L"C:\\Game\\nvngx_dlss.dll", L"C:\\Game\\sl.dlss_g.dll", own });
    CHECK(v.OwnInUse());
    v = JudgeNgx(own, { own, L"C:\\Game\\nvngx_dlssg_310.dll" });
    CHECK(!v.Ok());
    // same file name in a sibling folder is foreign
    v = JudgeNgx(own, { L"C:\\Game\\OptiScaler\\plugins\\f5low_streamline2\\nvngx_dlssg.dll", own });
    CHECK(!v.Ok() && v.foreign.size() == 1);
    CHECK(IsNgxDlssgName(L"nvngx_dlssg.dll") && !IsNgxDlssgName(L"nvngx_dlssg.dll.off") && !IsNgxDlssgName(L"nvngx_dlss.dll"));

    if (g_fail)
        return 1;
    printf("dlssg_own_test: all passed\n");
    return 0;
}
