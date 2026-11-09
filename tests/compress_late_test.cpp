// Host-only test of the LateGen=auto next-frame rules and (below) LateGen=compress (src/pace_sched.h): no GPU, no Windows API.
// tests/build_compress_late_test.ps1 builds it. Exit code 0: all checks passed.
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
    const long long step = 1000, raw = 10000;

    // auto stale: the next game frame is finished, on a timed generated frame only
    CHECK(AutoStale(true, true, true));
    CHECK(!AutoStale(true, true, false));                           // not finished, or not known
    CHECK(!AutoStale(false, true, true));                           // the real frame
    CHECK(!AutoStale(true, false, true));                           // metered, burst or V-Sync
    // auto early-out: the slot wait ends at the slot, or at once when the next game frame is finished (generated, timed)
    CHECK(AutoWaitOver(true, true, true, 100, 5000));
    CHECK(!AutoWaitOver(true, true, false, 100, 5000));
    CHECK(AutoWaitOver(true, true, false, 5000, 5000));
    CHECK(!AutoWaitOver(false, true, true, 100, 5000));             // the real frame waits for its slot
    CHECK(!AutoWaitOver(true, false, true, 100, 5000));             // not timed: no early-out
    CHECK(AutoWaitOver(true, false, false, 9000, 5000));            // a slot in the past is over
    // auto is active only without flip metering; its GPU timeout drop does not need a timed batch (V-Sync included), the time rule is off
    CHECK(LateActive(kLateAuto, false) && !LateActive(kLateAuto, true));
    CHECK(DropLate(true, true, false, LateTimeRule(kLateAuto), raw - 500, raw, step));      // timeout, V-Sync (timed false in the caller)
    CHECK(DropLate(true, true, false, false, raw - 500, raw, step));
    CHECK(!DropLate(true, true, true, LateTimeRule(kLateAuto), raw + 50 * step, raw, step));
    // show does nothing
    CHECK(!LateActive(kLateShow, false) && !LateActive(kLateShow, true));

    if (g_fail)
        printf("compress_late_test: %d FAILED\n", g_fail);
    else
        printf("compress_late_test: all passed\n");
    return g_fail ? 1 : 0;
}
