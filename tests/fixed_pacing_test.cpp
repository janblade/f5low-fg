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
    // FixedTimer(mode, gate): auto = timer for every V-Sync present on every GPU (the driver never meters a V-Sync batch), whatever the
    // flip metering state; display = never; timer = always; Gate=1 = never, in any mode.
    CHECK(FixedTimer(kFixedAuto, false));
    CHECK(!FixedTimer(kFixedDisplay, false));
    CHECK(FixedTimer(kFixedTimer, false));
    for (int mode = 0; mode < 3; ++mode)
        CHECK(!FixedTimer(static_cast<unsigned char>(mode), true));
    // MeterOff (LateGen=auto's condition) is not part of it any more
    CHECK(MeterOff(false, false, false) && MeterOff(false, true, true) && !MeterOff(true, false, false) && !MeterOff(true, true, true) &&
          MeterOff(true, true, false));

    if (g_fail)
    {
        printf("%d check(s) failed\n", g_fail);
        return 1;
    }
    printf("fixed_pacing_test: all checks passed\n");
    return 0;
}
