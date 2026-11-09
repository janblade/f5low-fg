// Host-only test of the LateGen=drop decision (src/pace_sched.h, DropLate): no GPU, no Windows API.
// tests/build_drop_late_test.ps1 builds it. Exit code 0: all checks passed.
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
    using pacesched::DropLate;
    const long long step = 1000, target = 10000;
    // frame k of 3 generated: the real frame is due (3 + 1 - k) steps after its time
    const long long real = target + 3 * step;

    // on time (before and exactly at its time): shown
    CHECK(!DropLate(true, true, true, true, target - 500, target, step, real));
    CHECK(!DropLate(true, true, true, true, target, target, step, real));
    // late within the margin (up to one step): shown
    CHECK(!DropLate(true, true, true, true, target + 400, target, step, real));
    CHECK(!DropLate(true, true, true, true, target + step, target, step, real));
    // late past one step: dropped
    CHECK(DropLate(true, true, true, true, target + step + 1, target, step, real));
    // the real frame is already due: dropped (the last generated frame: due one step after its own time)
    CHECK(DropLate(true, true, true, true, target + step, target, step, target + step));
    CHECK(DropLate(true, true, true, true, real, target, step, real));
    // GPU wait timed out: dropped even when the time says it is early
    CHECK(DropLate(true, true, false, true, target - 500, target, step, real));
    CHECK(DropLate(true, true, false, false, target - 500, target, step, real));     // display-paced too
    // display-paced (no schedule): only the wait result counts, never the time
    CHECK(!DropLate(true, true, true, false, target + 50 * step, target, step, real));
    // the real frame is never dropped, whatever the wait or time say
    CHECK(!DropLate(true, false, false, true, target + 50 * step, target, step, real));
    CHECK(!DropLate(true, false, true, true, target + 50 * step, target, step, real));
    // LateGen=show never drops
    CHECK(!DropLate(false, true, false, true, target + 50 * step, target, step, real));
    CHECK(!DropLate(false, true, true, true, real + 50 * step, target, step, real));
    CHECK(!DropLate(false, false, false, true, real + 50 * step, target, step, real));

    if (g_fail)
        printf("drop_late_test: %d FAILED\n", g_fail);
    else
        printf("drop_late_test: all passed\n");
    return g_fail ? 1 : 0;
}
