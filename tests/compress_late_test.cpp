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

    // auto stale: only a generated frame, with the next game frame finished AND the presenter at least half a step past its slot
    CHECK(AutoStale(true, true, 5000 + MinGap(step), 5000, step));          // half a step past: late anyway
    CHECK(AutoStale(true, true, 9000, 5000, step));
    CHECK(!AutoStale(true, true, 5000 + MinGap(step) - 1, 5000, step));     // a hair late (show would put it out): not dropped
    CHECK(!AutoStale(true, true, 5000, 5000, step));                        // on time
    CHECK(!AutoStale(true, true, 100, 5000, step));                         // early
    CHECK(!AutoStale(true, false, 9000, 5000, step));                       // next frame not finished or not known
    CHECK(!AutoStale(false, true, 9000, 5000, step));                       // the real frame
    // modes: auto and early act only without flip metering and differ only in the early-out; compress and drop act on every GPU
    CHECK(LateNextFrame(kLateAuto) && LateNextFrame(kLateEarly) && !LateNextFrame(kLateShow) && !LateNextFrame(kLateDrop));
    CHECK(!LateEarlyOut(kLateAuto) && LateEarlyOut(kLateEarly) && !LateEarlyOut(kLateCompress));
    CHECK(LateActive(kLateEarly, false) && !LateActive(kLateEarly, true) && LateActive(kLateCompress, true));
    // auto is active only without flip metering; its GPU timeout drop does not need a timed batch (V-Sync included), the time rule is off
    CHECK(LateActive(kLateAuto, false) && !LateActive(kLateAuto, true));
    CHECK(DropLate(true, true, false, LateTimeRule(kLateAuto), raw - 500, raw, step));      // timeout, V-Sync (timed false in the caller)
    CHECK(DropLate(true, true, false, false, raw - 500, raw, step));
    CHECK(!DropLate(true, true, true, LateTimeRule(kLateAuto), raw + 50 * step, raw, step));
    // show does nothing
    CHECK(!LateActive(kLateShow, false) && !LateActive(kLateShow, true));

    // Every time rule above is asked with `timed` = not display-paced (V-Sync), never with the pacer's own "paced" flag: V-Sync stays out
    // of AutoStale / CompressEarly / CompressDrop even if a timer is later used under V-Sync (xefg_own.h PresentReal).
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

    // ---- a jittered game through the schedule rules (xefg_own.h PresentReal's, with its smoothing), counting what LateGen=early does (the early-out on every present; its stale count is not asserted)
    // variant 2: steady 60 fps, the presenter starts one game frame behind (the next frame is already done when the first batch starts)
    for (int variant = 0; variant < 3; ++variant)
        for (unsigned n : { 1u, 3u, 5u })
        {
            if (variant == 2 && n == 1)
                continue;
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
            unsigned stale = 0, early = 0, gens = 0, tooClose = 0, presents = 0, staleFirst5 = 0, staleAfter5 = 0, earlyFirst5 = 0, lastStaleBatch = 0;
            double lat = 0;
            if (variant == 2)
                now = ready[2];
            for (size_t f = 1; f + 2 < ready.size(); ++f)
            {
                ri.Observe(static_cast<double>(ready[f] - ready[f - 1]) / us);
                const long long iv = static_cast<long long>(ri.ms * us);
                base = NextBase(base, ready[f], 0, iv);
                const long long st = iv / (n + 1);
                now = std::max(now, ready[f]);                          // the presenter takes the frame once it is done
                const unsigned stale0 = stale, early0 = early;
                for (unsigned k = 1; k <= n + 1; ++k)
                {
                    const bool gen = k <= n;
                    const long long slot = Bound(base + static_cast<long long>(k) * st, st, lastAny);
                    const bool nextDone = now >= ready[f + 1];
                    if (gen)
                        ++gens;
                    if (AutoStale(gen, nextDone, now, slot, st))
                    {
                        ++stale;
                        lastAny = std::max(lastAny, slot);
                        continue;
                    }
                    long long at;
                    if (now >= slot)
                        at = now;
                    else if (nextDone)
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
                if (f <= 5)
                {
                    staleFirst5 += stale - stale0;
                    earlyFirst5 += early - early0;
                }
                else
                    staleAfter5 += stale - stale0;
                if (stale != stale0)
                    lastStaleBatch = static_cast<unsigned>(f);
            }
            if (variant == 2)
                printf("model behind-at-start 60 fps %ux: stale %u in the first 5 batches (last stale batch %u), %u after; %u early-outs in the first "
                       "5 batches\n", n + 1, staleFirst5, lastStaleBatch, staleAfter5, earlyFirst5);
            printf("model %s %ux: %u of %u generated frames stale, %u of %u presents released early, %u gaps under the minimum, "
                   "real frame %.2f ms after its ready stamp on average\n", ramp ? "40->60 fps" : "60 fps", n + 1, stale, gens, early, presents,
                   tooClose, lat / 5900.0);
            CHECK(variant == 2 ? staleAfter5 == 0 : true);   // from a start one frame behind it converges within a few batches
            CHECK(tooClose == 0);
        }

    // ---- LateGen=auto as shipped: plain slot waits (the show schedule), presenter overhead on every present (0-1.5 ms, an occasional 3 ms
    // stall), steady play with jitter. Stale only when the presenter is at least half a step past a slot with the next frame done.
    for (int stalls = 0; stalls < 2; ++stalls)
    for (unsigned n : { 1u, 3u, 5u })
    {
        std::mt19937 rng(777 + n);
        std::normal_distribution<double> noise(0.0, 0.10);
        std::uniform_real_distribution<double> ovh(0.0, 1000.0), pick(0.0, 1.0);
        std::vector<long long> ready;
        double t = 1e6;
        for (int f = 0; f < 6000; ++f)
        {
            ready.push_back(static_cast<long long>(t));
            t += std::max(5000.0, 16700.0 * (1.0 + noise(rng)));
        }
        ReadyInterval ri;
        long long base = 0, lastAny = 0, now = 0;
        unsigned stale = 0, gens = 0, bad = 0;
        for (size_t f = 1; f + 2 < ready.size(); ++f)
        {
            ri.Observe(static_cast<double>(ready[f] - ready[f - 1]) / 1000.0);
            const long long iv = static_cast<long long>(ri.ms * 1000.0);
            base = NextBase(base, ready[f], 0, iv);
            const long long st = iv / (n + 1);
            now = std::max(now, ready[f]);
            for (unsigned k = 1; k <= n + 1; ++k)
            {
                const bool gen = k <= n;
                const long long slot = Bound(base + static_cast<long long>(k) * st, st, lastAny);
                if (now < slot)
                    now = slot;                                                // the plain wait for the slot
                now += static_cast<long long>(ovh(rng) + (stalls && pick(rng) < 0.02 ? 3000.0 : 0.0));    // the presenter's own time
                if (gen)
                    ++gens;
                if (AutoStale(gen, now >= ready[f + 1], now, slot, st))
                {
                    ++stale;
                    bad += now < slot + MinGap(st);
                    lastAny = std::max(lastAny, slot);
                    continue;
                }
                lastAny = now;
            }
        }
        printf("model auto 60 fps, presenter overhead 0-1 ms%s %ux: %u of %u generated frames stale\n", stalls ? " plus a 3 ms stall on 2% of presents" : "", n + 1, stale, gens);
        // overhead alone never drops; a stall longer than the step puts a slot more than half a step behind, which is the rule working (counted, bounded)
        CHECK(stalls ? stale * 50 < gens : stale == 0);
        CHECK(bad == 0);
    }

    if (g_fail)
        printf("compress_late_test: %d FAILED\n", g_fail);
    else
        printf("compress_late_test: all passed\n");
    return g_fail ? 1 : 0;
}
