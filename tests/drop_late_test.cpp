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
    const long long step = 1000, raw = 10000;

    // on time (before and exactly at its time): shown
    CHECK(!DropLate(true, true, true, true, raw - 500, raw, step));
    CHECK(!DropLate(true, true, true, true, raw, raw, step));
    // late within the margin (up to one step): shown
    CHECK(!DropLate(true, true, true, true, raw + 400, raw, step));
    CHECK(!DropLate(true, true, true, true, raw + step, raw, step));
    // late past one step: dropped
    CHECK(DropLate(true, true, true, true, raw + step + 1, raw, step));
    // the time is the raw one: a clamped target in the past (after a stutter) is not what is passed, so a frame on its new schedule is shown
    {
        const long long clamped = raw - 5 * step, now = raw + step / 2;
        CHECK(now - clamped > step);                                                  // the clamped time would have dropped it
        CHECK(!DropLate(true, true, true, true, now, raw, step));
    }
    // GPU wait timed out: dropped even when the time says it is early, and where the time rule is off
    CHECK(DropLate(true, true, false, true, raw - 500, raw, step));
    CHECK(DropLate(true, true, false, false, raw - 500, raw, step));
    // time rule off (V-Sync, metered or burst batch): only the wait result counts, never the time
    CHECK(!DropLate(true, true, true, false, raw + 50 * step, raw, step));
    // the real frame is never dropped, whatever the wait or time say
    CHECK(!DropLate(true, false, false, true, raw + 50 * step, raw, step));
    CHECK(!DropLate(true, false, true, true, raw + 50 * step, raw, step));
    // LateGen=show never drops
    CHECK(!DropLate(false, true, false, true, raw + 50 * step, raw, step));
    CHECK(!DropLate(false, true, true, true, raw + 50 * step, raw, step));
    CHECK(!DropLate(false, false, false, true, raw + 50 * step, raw, step));

    if (g_fail)
        printf("drop_late_test: %d FAILED\n", g_fail);
    else
        printf("drop_late_test: all passed\n");
    return g_fail ? 1 : 0;
}
