// Host-only test of the LateGen=auto next-frame rules and LateGen=compress (src/pace_sched.h): no GPU, no Windows API.
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

    // ---- compress
    CHECK(LateActive(kLateCompress, false) && LateActive(kLateCompress, true) && LateTimeRule(kLateCompress));
    // next game frame not finished: no early release, no stale drop (as show)
    CHECK(!CompressEarly(true, false));
    CHECK(!CompressDrop(true, true, true, false, false));
    CHECK(!CompressDrop(true, true, true, false, true));
    // next frame finished and on time: released early, not dropped
    CHECK(CompressEarly(true, true));
    CHECK(!CompressDrop(true, true, true, true, false));
    // not timed (V-Sync, metered, burst): never early, never dropped by staleness
    CHECK(!CompressEarly(false, true));
    CHECK(!CompressDrop(true, true, false, true, true));
    // spacing: now, but never less than MinGap after the last present that went out
    CHECK(MinGap(step) == step / 2);
    CHECK(CompressAt(5000, 0, step) == 5000);
    CHECK(CompressAt(5000, 4900, step) == 4900 + step / 2);
    CHECK(CompressAt(5000, 4000, step) == 5000);
    CHECK(CompressAt(5000, 5000 - step / 2, step) == 5000);
    CHECK(Bound(0, step, 10000) == 10000 + MinGap(step));          // Bound's lower limit is the same constant
    // several frames in a row compress evenly
    {
        long long now = 20000, last = 10000, prev = last;
        for (int k = 1; k <= 5; ++k)
        {
            CHECK(CompressEarly(true, true));
            const long long at = CompressAt(now, last, step);
            CHECK(at - prev >= MinGap(step));
            if (k > 1)
                CHECK(at - prev == MinGap(step));
            CHECK(at >= now);
            now = at;
            last = prev = at;
        }
    }
    {
        const long long a = CompressAt(9000, 1000, step);
        CHECK(a == 9000);
        CHECK(CompressAt(9000, a, step) == 9000 + MinGap(step));
    }
    // stale: dropped only with both the next frame and the one after it finished
    CHECK(CompressDrop(true, true, true, true, true));
    CHECK(!CompressDrop(true, true, true, false, true));
    // the real frame is never dropped
    CHECK(!CompressDrop(false, false, true, true, true));
    CHECK(!CompressDrop(false, true, true, true, true));
    CHECK(!CompressDrop(false, false, false, false, false));
    // a GPU wait that ran out still drops a generated frame: timed or not
    CHECK(CompressDrop(true, false, true, false, false));
    CHECK(CompressDrop(true, false, false, false, false));

    if (g_fail)
        printf("compress_late_test: %d FAILED\n", g_fail);
    else
        printf("compress_late_test: all passed\n");
    return g_fail ? 1 : 0;
}
