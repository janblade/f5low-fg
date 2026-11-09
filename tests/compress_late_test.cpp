// Host-only test of the LateGen=auto next-frame rules and LateGen=compress (src/pace_sched.h): no GPU, no Windows API.
// tests/build_compress_late_test.ps1 builds it. Exit code 0: all checks passed.
#include "../src/pace_sched.h"
#include <algorithm>
#include <cstdio>
#include <random>
#include <vector>

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

    // auto early-out: the wait for a slot is over at the slot, or when the next game frame is finished (generated or real frame alike)
    CHECK(AutoWaitOver(true, 100, 5000));
    CHECK(!AutoWaitOver(false, 100, 5000));                         // not finished, or not known: waits for the slot
    CHECK(AutoWaitOver(false, 5000, 5000));
    CHECK(AutoWaitOver(false, 9000, 5000));
    // auto stale: only a generated frame, with the next game frame finished AND (its slot already past, or the presenter was behind at Pick)
    CHECK(AutoStale(true, true, true, false));                      // late anyway
    CHECK(AutoStale(true, true, false, true));                      // behind when it took the batch
    CHECK(!AutoStale(true, true, false, false));                    // on time: goes out under the early-out, never dropped
    CHECK(!AutoStale(true, false, true, true));                     // next frame not finished or not known
    CHECK(!AutoStale(false, true, true, true));                     // the real frame
    // auto is active only without flip metering; its GPU timeout drop does not need a timed batch (V-Sync included), the time rule is off
    CHECK(LateActive(kLateAuto, false) && !LateActive(kLateAuto, true));
    CHECK(DropLate(true, true, false, LateTimeRule(kLateAuto), raw - 500, raw, step));      // timeout, V-Sync (timed false in the caller)
    CHECK(DropLate(true, true, false, false, raw - 500, raw, step));
    CHECK(!DropLate(true, true, true, LateTimeRule(kLateAuto), raw + 50 * step, raw, step));
    // show does nothing
    CHECK(!LateActive(kLateShow, false) && !LateActive(kLateShow, true));

    // Every time rule above is asked with `timed` = not display-paced (V-Sync), never with the pacer's own "paced" flag: V-Sync stays out
    // of AutoStale / AutoWaitOver / CompressEarly / CompressDrop even if a timer is later used under V-Sync (xefg_own.h PresentReal).
    CHECK(!CompressEarly(false, true) && !CompressDrop(true, true, false, true, true));

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

    // ---- a jittered game through the schedule rules (xefg_own.h PresentReal's, with its smoothing), counting what auto does
    for (int variant = 0; variant < 2; ++variant)
        for (unsigned n : { 1u, 3u, 5u })
        {
            const bool ramp = variant == 1;
            std::mt19937 rng(12345 + n + variant * 100);
            std::normal_distribution<double> noise(0.0, 0.10);
            const double us = 1000.0;                                   // ticks: microseconds
            std::vector<long long> ready;                               // ready stamps of the game frames
            double t = 1e6;
            for (int f = 0; f < 6000; ++f)
            {
                const double base = ramp ? (f < 600 ? 25.0 + (16.7 - 25.0) * f / 600.0 : 16.7) : 16.7;    // 40 -> 60 fps over 600 frames
                ready.push_back(static_cast<long long>(t));
                t += std::max(0.3 * base, base * (1.0 + noise(rng))) * us;
            }
            ReadyInterval ri;
            long long base = 0, lastAny = 0, lastShown = 0, now = 0;
            unsigned stale = 0, early = 0, gens = 0, tooClose = 0, presents = 0;
            double lat = 0;
            for (size_t f = 1; f + 2 < ready.size(); ++f)
            {
                ri.Observe(static_cast<double>(ready[f] - ready[f - 1]) / us);
                const long long iv = static_cast<long long>(ri.ms * us);
                base = NextBase(base, ready[f], 0, iv);
                const long long st = iv / (n + 1);
                now = std::max(now, ready[f]);                          // the presenter takes the frame once it is done
                const bool behind = ready[f + 1] <= now;
                for (unsigned k = 1; k <= n + 1; ++k)
                {
                    const bool gen = k <= n;
                    const long long slot = Bound(base + static_cast<long long>(k) * st, st, lastAny);
                    const bool nextDone = now >= ready[f + 1];
                    if (gen)
                        ++gens;
                    if (AutoStale(gen, nextDone, now >= slot, behind))
                    {
                        ++stale;
                        lastAny = std::max(lastAny, slot);
                        continue;
                    }
                    long long at;
                    if (now >= slot)
                        at = now;
                    else if (AutoWaitOver(nextDone, now, slot))
                        at = std::max(now, lastShown + MinGap(st)), ++early;
                    else if (ready[f + 1] < slot)                       // the wait ends when the next game frame does
                        at = std::max(ready[f + 1], lastShown + MinGap(st)), ++early;
                    else
                        at = slot;
                    now = at;
                    if (lastShown && at - lastShown < MinGap(st))
                        ++tooClose;
                    lastAny = lastShown = at;
                    ++presents;
                    if (k == n + 1)
                        lat += static_cast<double>(at - ready[f]) / us;
                }
            }
            printf("model %s %ux: %u of %u generated frames stale, %u of %u presents released early, %u gaps under the minimum, "
                   "real frame %.2f ms after its ready stamp on average\n", ramp ? "40->60 fps" : "60 fps", n + 1, stale, gens, early, presents,
                   tooClose, lat / 5900.0);
            CHECK(stale == 0);
            CHECK(tooClose == 0);
        }

    if (g_fail)
        printf("compress_late_test: %d FAILED\n", g_fail);
    else
        printf("compress_late_test: all passed\n");
    return g_fail ? 1 : 0;
}
