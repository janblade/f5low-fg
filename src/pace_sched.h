// The pieces of the schedule pacer that both the own generator's D3D12 path (xefg_own.h: Pick, Target, Bound) and the sink's schedule
// mode (sink/sink.h, Generator=intel) use: the smoothed app-frame interval, the smoothed base of each app frame's batch, and the clamp
// that keeps a late frame from bunching with the next. No GPU or Windows in here. The lead rule is in pace_lead.h.
#pragma once
#include <algorithm>
#include <array>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdlib>

namespace pacesched
{
// The rate the schedule spaces generated frames by: the app frame's interval, eased by 1/10, but when the game slows for good (two
// frames in a row over 1.3x of it: a menu turning into play) it is raised at once, to the shorter of the two. Easing alone took ~15
// frames after 120 -> 28 fps, each batch presented at the old, shorter spacing and then waiting for the next real frame (NBA run U2:
// 4-5 hitches in a row; harness --fpsat 300 28). Never lowered at once: after a stall the game hands over a couple of quick catch-up
// frames, and taking those as its pace made every batch after it short (NBA run V). Three in a row under 1/1.3 of it (a real return
// to a faster pace, e.g. after a two-frame stall raised it) lower it at once, to the longest of the three; catch-up bursts are one or two.
struct ReadyInterval
{
    double ms = 16.7;                       // the smoothed interval
    double prev = 0;                        // the last app frame's ready-to-ready time
    std::array<double, 8> hist {};          // the last 8 of them, for the hitch log
    size_t at = 0;
    uint64_t run = 0;                       // samples in range since the last one out of range

    void Observe(double sample)
    {
        if (!(sample > 0.5 && sample < 200))
        {
            prev = 0;
            run = 0;                        // the history stays for the hitch log; the down rule needs three new samples
            return;
        }
        ++run;
        hist[at] = sample;
        at = (at + 1) % hist.size();
        const size_t n = hist.size();
        const double a = hist[(at + n - 1) % n], b = hist[(at + n - 2) % n], c = hist[(at + n - 3) % n];
        if (prev > 1.3 * ms && sample > 1.3 * ms)
            ms = std::min(sample, prev);
        else if (run >= 3 && std::max({ a, b, c }) * 1.3 < ms)
            ms = std::max({ a, b, c });
        else
            ms = ms * 0.9 + sample * 0.1;
        prev = sample;
    }
};

// The schedule's clock for the next app frame: set anew (to the frame's ready stamp plus the lag) when it is more than an interval
// off the last one's plus the interval (a pause, the first frame), else drawn a fifth of the way towards it - the stamps of a
// busy game wander by milliseconds, which taken as they are put the jitter into the gap between an app frame and the next generated one.
inline long long NextBase(long long base, long long ready, long long lag, long long interval)
{
    const long long want = ready + lag, due = base + interval;
    return !base || std::llabs(want - due) > interval ? want : due + (want - due) / 5;
}

// Half a step after the last present at the soonest, two steps after it at the latest: a frame the game handed over after a stutter has
// its schedule set from that late frame; and a late frame is followed by the next no sooner than half a step (no bunch).
inline long long Bound(long long target, long long step, long long last)
{
    return last ? std::clamp(target, last + step / 2, last + 2 * step) : target;
}

// The sink's choice of the application Present a burst belongs to (sink/sink.h, Plan). `stamps`: the unmatched Present times, oldest
// first (any container with front/pop_front/operator[]/size). The newest one not later than `arrived` (the burst's first frame) is taken
// and the older ones dropped: one extra stamp (a Present the library never turned into a burst) must not shift the schedule by a frame
// for good. Returns false when there is none (every stamp is later than the arrival): the arrival itself is the base then.
template <class Deque> inline bool PickStamp(Deque& stamps, long long arrived, long long& stamp)
{
    while (stamps.size() > 1 && stamps[1] <= arrived)
        stamps.pop_front();
    if (stamps.empty() || stamps.front() > arrived)
        return false;
    stamp = stamps.front();
    stamps.pop_front();
    return true;
}

// The same pairing for stamps that are more than a time: `key(entry)` is the CPU time of the game's Present (the clock the burst's
// arrival is on). The pairing is ALWAYS by that time, in Present order - never by the GPU stamp that came to belong to the entry: the
// library may hand its first generated frame to the sink before the GPU has finished the application's frame (the Present is a CPU
// call), so a GPU time can be later than the arrival and must not decide which Present a burst belongs to. The entry's GPU time, once
// there, is only the VALUE the schedule is based on (GpuStampRing).
template <class Deque, class Entry, class Key> inline bool PickStampBy(Deque& stamps, long long arrived, Entry& picked, Key key)
{
    while (stamps.size() > 1 && key(stamps[1]) <= arrived)
        stamps.pop_front();
    if (stamps.empty() || key(stamps.front()) > arrived)
        return false;
    picked = stamps.front();
    stamps.pop_front();
    return true;
}

// One application Present as the sink's schedule keeps it: the CPU time of the call, and the value of the fence signalled on the
// APPLICATION's queue right behind it (SinkFactory::SetStampQueue; not the queue the library made its chain with) (0: none was signalled -
// GPU stamps off, no application queue, or an older Present).
struct AppStamp
{
    long long cpu = 0;
    uint64_t fence = 0;
};

// The base of a batch (ticks) when the Present's GPU time may not be there (sink/sink.h, ResolveBatch). With the GPU time the usual
// NextBase, and the offset GPU time minus CPU time of the Present is tracked (smoothed by 1/8). A miss (the GPU time did not come in time)
// is based on the Present's CPU time MOVED BY that offset, through the same NextBase: the CPU time alone sits a GPU latency off the GPU
// stamps (falling back to it made the base jump there and back, two hitches per miss), and the corrected one stays continuous; it is
// drawn towards by NextBase, not set to, and after a real pause (more than an interval off) it resets, as every base does. Before the first
// GPU time, after Reset, and after kMaxMisses misses in a row (the GPU stamps are gone): the CPU time as it is.
struct GpuBase
{
    static constexpr unsigned kMaxMisses = 3;
    bool have = false;
    double offset = 0;                      // ticks, GPU minus CPU
    unsigned missRun = 0;

    void Reset() { *this = GpuBase {}; }
    long long Next(long long base, bool gpu, long long gpuT, long long cpuT, long long lag, long long interval)
    {
        if (gpu)
        {
            const double cur = static_cast<double>(gpuT - cpuT);
            offset = have ? offset + (cur - offset) / 8 : cur;
            have = true;
            missRun = 0;
            return NextBase(base, gpuT, lag, interval);
        }
        if (have && missRun < kMaxMisses)
        {
            ++missRun;
            return NextBase(base, cpuT + static_cast<long long>(offset), lag, interval);
        }
        if (have)
            ++missRun;
        return NextBase(base, cpuT, lag, interval);
    }
};

// The times the GPU reached the fence values signalled behind the game's Presents (sink/sink.h: a watcher thread takes the time at
// which the fence's completion event fired, as the own generator's watcher does for appFence, xefg_own.h StartPresenter). Values are
// consecutive (1, 2, 3 ...); the last kN are kept. Not thread-safe: the sink guards it with its mutex.
class GpuStampRing
{
    static constexpr size_t kN = 64;
    long long at[kN] {};
    uint64_t done = 0;                      // the values [1, done] have a time

public:
    // The time (QPC ticks) the GPU reached fence value `v`. Values must come in order; a skipped one just has no time.
    void Publish(uint64_t v, long long t)
    {
        if (v <= done)
            return;
        at[v % kN] = t;
        done = v;
    }
    // False: not reached yet (or forgotten, kN values back, or 0 = never signalled): the caller falls back to the CPU time.
    bool Get(uint64_t v, long long& t) const
    {
        if (!v || v > done || done - v >= kN)
            return false;
        t = at[v % kN];
        return true;
    }
};

// The schedule's lag behind the GPU timeline, as the own generator keeps it (xefg_own.h, paceLag): raised at once to the most a
// generated frame came out late on top of the lag its batch was scheduled with, never above maxLag app frames, falling by 2% per app
// frame while nothing comes out late. Under a GPU the game keeps busy the first generated frame needs that time for the library's
// work; with no lag the frames go out late and uneven. "Late" is only our own GPU wait (a frame that arrived after its time is not).
constexpr double kLagLateMs = 0.5;          // the own generator raises the lag for a frame more than this far past its time
struct Lag
{
    double ms = 0;
    // A generated frame came out `late` ms past its time; `scheduled`: the lag its batch was scheduled with; `intervalMs`: the app frame interval.
    void Late(double scheduled, double late, double intervalMs, double maxLagFrames)
    {
        if (late > kLagLateMs)
            ms = std::min(std::max(ms, scheduled + late), intervalMs * maxLagFrames);
    }
    void EndFrame() { ms *= 0.98; }         // after each batch
};

// The cap of the lead (ticks) a batch is planned with: `cap` (a step, or n steps with the wide-cap test) for the lead rule; a fixed test
// lead (F5LOW_TEST_SINK_LEAD_MS) is never cut by it - the experiment must run the lead it asks for.
inline long long LeadCap(long long cap, long long lead, bool fixedTestLead) { return fixedTestLead ? std::max(cap, lead) : cap; }

// LateGen=drop: whether a generated frame is left out instead of shown late (xefg_own.h, PresentReal). Never the batch's real frame
// (`generated` false). A frame whose own GPU work did not finish in its wait (`gpuOk` false) is dropped. With the schedule in use
// (`timed`: not display-paced) a frame is also dropped when, at the moment it would go out, it is more than one step past its time,
// or the batch's real frame is already due: showing it then would put it right in front of the real frame, or behind it in time.
// Within a step of its time it goes out as before. `enabled` false (LateGen=show) never drops. Ticks.
inline bool DropLate(bool enabled, bool generated, bool gpuOk, bool timed, long long now, long long target, long long step, long long realTarget)
{
    if (!enabled || !generated)
        return false;
    if (!gpuOk)
        return true;
    return timed && (now - target > step || now >= realTarget);
}
} // namespace pacesched
