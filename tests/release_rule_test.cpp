// Host-only test of the sink's per-frame release rule (src/sink/release_rule.h): no GPU, no Windows API.
// tests/build_release_rule_test.ps1 builds it; CI runs it. Exit code 0: all checks passed.
#include "../src/sink/release_rule.h"
#include <cstdio>
#include <initializer_list>

static int g_fail = 0;
#define CHECK(c)                                                                                \
    do                                                                                          \
    {                                                                                           \
        if (!(c))                                                                               \
        {                                                                                       \
            printf("FAILED line %d: %s\n", __LINE__, #c);                                       \
            ++g_fail;                                                                           \
        }                                                                                       \
    } while (0)

using namespace xsink;

int main()
{
    const uint32_t tear = 0x200, other = 0x1;   // DXGI_PRESENT_ALLOW_TEARING; some other flag
    // the predicate: V-Sync or any flag but tearing
    CHECK(!DisplayPaced(0, 0));
    CHECK(!DisplayPaced(0, tear));
    CHECK(DisplayPaced(1, 0));
    CHECK(DisplayPaced(1, tear));
    CHECK(DisplayPaced(2, 0));
    CHECK(DisplayPaced(0, other));
    CHECK(DisplayPaced(0, tear | other));
    // paced modes: V-Sync-less presents are paced, display-paced ones go out on arrival
    for (PaceMode m : { PaceMode::Burst, PaceMode::Schedule, PaceMode::Auto })
    {
        CHECK(ReleaseRule(0, 0, m) == ReleaseKind::Paced);
        CHECK(ReleaseRule(0, tear, m) == ReleaseKind::Paced);
        CHECK(ReleaseRule(1, 0, m) == ReleaseKind::OnArrivalDisplay);
        CHECK(ReleaseRule(0, other, m) == ReleaseKind::OnArrivalDisplay);
    }
    // off: everything on arrival
    CHECK(ReleaseRule(0, 0, PaceMode::Off) == ReleaseKind::OnArrivalOff);
    CHECK(ReleaseRule(0, tear, PaceMode::Off) == ReleaseKind::OnArrivalOff);
    CHECK(ReleaseRule(1, 0, PaceMode::Off) == ReleaseKind::OnArrivalOff);
    if (g_fail)
        printf("%d checks failed\n", g_fail);
    else
        printf("release_rule_test: all checks passed\n");
    return g_fail ? 1 : 0;
}
