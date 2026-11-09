// Host-only test of the FixedPacing=auto|display|timer decision (src/pace_sched.h FixedTimer): no GPU, no Windows API.
// tests/build_fixed_pacing_test.ps1 builds it. Exit code 0: all checks passed.
#include "../src/pace_sched.h"
#include <cstdio>

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

int main()
{
    using namespace pacesched;
    // FixedTimer(mode, gate, flipMetering (as the GPU / settings resolved it), flipTried (InitFlip ran), flipFnUsable (it found the driver call))

    // auto: timer where flip metering is off for this context (the condition LateGen=auto uses: meterOff)
    CHECK(FixedTimer(kFixedAuto, false, false, false, false));      // metering off by GPU or setting (RTX 20/30/40, FlipMetering=0): timer from the start
    CHECK(FixedTimer(kFixedAuto, false, false, true, false));       // (InitFlip never runs then, or ran and found nothing)
    CHECK(!FixedTimer(kFixedAuto, false, true, false, false));      // wants metering, InitFlip not run yet: counts as metering on -> display
    CHECK(!FixedTimer(kFixedAuto, false, true, true, true));        // the driver meters -> display
    CHECK(FixedTimer(kFixedAuto, false, true, true, false));        // wanted but the driver path is unusable (InitFlip tried, no call) -> timer
    // the same condition as PresentReal's meterOff (MeterOff, the one function both use)
    CHECK(MeterOff(false, false, false) && MeterOff(false, true, true) && !MeterOff(true, false, false) && !MeterOff(true, true, true) &&
          MeterOff(true, true, false));
    for (int m = 0; m < 2; ++m)
        for (int t = 0; t < 2; ++t)
            for (int f = 0; f < 2; ++f)
                CHECK(FixedTimer(kFixedAuto, false, m != 0, t != 0, f != 0) == MeterOff(m != 0, t != 0, f != 0));
    // display and timer are fixed whatever the driver does
    for (int m = 0; m < 2; ++m)
        for (int t = 0; t < 2; ++t)
            for (int f = 0; f < 2; ++f)
            {
                CHECK(!FixedTimer(kFixedDisplay, false, m != 0, t != 0, f != 0));
                CHECK(FixedTimer(kFixedTimer, false, m != 0, t != 0, f != 0));
            }
    // Gate=1: never the timer, in any mode
    for (int mode = 0; mode < 3; ++mode)
        for (int m = 0; m < 2; ++m)
            for (int t = 0; t < 2; ++t)
                for (int f = 0; f < 2; ++f)
                    CHECK(!FixedTimer(static_cast<unsigned char>(mode), true, m != 0, t != 0, f != 0));

    if (g_fail)
    {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("fixed_pacing_test: all checks passed\n");
    return 0;
}
