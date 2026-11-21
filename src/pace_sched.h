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

#include "pace_lead.h"

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

// The least time between two presents of a batch that the schedule allows: half a step. Bound's lower limit, and the spacing LateGen=compress
// squeezes a late batch to.
inline long long MinGap(long long step) { return step / 2; }

// Half a step after the last present at the soonest, two steps after it at the latest: a frame the game handed over after a stutter has
// its schedule set from that late frame; and a late frame is followed by the next no sooner than half a step (no bunch).
inline long long Bound(long long target, long long step, long long last)
{
    return last ? std::clamp(target, last + MinGap(step), last + 2 * step) : target;
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

// LateGen= values (xefg_own.h lateMode) and what each lets PresentReal do. Show: nothing. Auto, only without flip metering: a GPU timeout
// drops, and on a timed batch a stale generated frame is not presented (AutoStale); the slot waits are as in show. Early (experiment, same
// condition): auto plus the early-out (AutoWaitOver). Drop: the timeout and the one-step time rule, on every GPU. Compress (experiment,
// every GPU): a timed batch is squeezed while the game's next frame is finished (CompressEarly / CompressAt) and only a frame a whole game
// frame behind is dropped (CompressDrop); LateTimeRule is true for it too (it uses the batch's time).
enum LateMode : unsigned char { kLateShow = 0, kLateAuto = 1, kLateDrop = 2, kLateCompress = 3, kLateEarly = 4 };
inline bool LateNextFrame(unsigned char mode) { return mode == kLateAuto || mode == kLateEarly; }      // the stale drop (and the timeout drop)
inline bool LateEarlyOut(unsigned char mode) { return mode == kLateEarly; }
inline bool LateActive(unsigned char mode, bool flipMetering) { return mode == kLateDrop || mode == kLateCompress || (LateNextFrame(mode) && !flipMetering); }
inline bool LateTimeRule(unsigned char mode) { return mode == kLateDrop || mode == kLateCompress; }

// FixedPacing= values (xefg_own.h fixedMode): who spaces a batch's frames when the game asks for V-Sync. Display: the display does (the
// frames go out as soon as they are ready). Timer: f5low waits for each frame's time itself on the plain schedule (the LateGen rules stay as
// they are: only the spacing changes). Auto (default): timer, on every GPU: the driver never meters a V-Sync batch, and the timer paced
// equal or smoother with 3-8 ms less display latency on a fixed-refresh display (measured) and fixed uneven frames on a VRR one. Only a
// present with a V-Sync interval is affected (the caller checks sync != 0); the timer uses the plain schedule only (no lead, no burst, no
// metering). Never with Gate=1 (the gate has its own pacing): display.
enum FixedMode : unsigned char { kFixedAuto = 0, kFixedDisplay = 1, kFixedTimer = 2 };
// Flip metering is off for a context (LateGen=auto's condition): the settings or the GPU say off (`flipMetering`
// false), or it was wanted but InitFlip tried (`flipTried`) and found no usable driver call (`flipFnUsable` false).
inline bool MeterOff(bool flipMetering, bool flipTried, bool flipFnUsable) { return !flipMetering || (flipTried && !flipFnUsable); }
inline bool FixedTimer(unsigned char mode, bool gate)
{
    if (gate)
        return false;
    return mode != kFixedDisplay;
}
// The pacer export's FixedPacing (include/f5low_pacer.h F5LowPacerCreateInfo::fixedPacing): the host's value (0 = f5low's own setting `own`,
// 1 display, 2 timer) over the setting; the gate still wins in FixedTimer. Values above 2 are the caller's argument error.
inline unsigned char HostFixedMode(unsigned hostField, unsigned char own)
{
    return hostField == 1 ? kFixedDisplay : hostField == 2 ? kFixedTimer : own;
}

// LateGen=drop: whether a generated frame is left out instead of shown late (xefg_own.h, PresentReal). Never the batch's real frame
// (`generated` false). A frame whose own GPU work did not finish in its wait (`gpuOk` false) is dropped. Where its time means something
// (`timed`: the schedule or the clock paces the batch - not V-Sync, not a burst or flip-metered batch, whose later presents never wait
// for their time) it is also dropped when, at the moment it would go out, it is more than one step past `rawTarget`, its time on the
// frame's own schedule before the clamp that keeps a late frame from bunching with the next (Bound) - the clamp moves the time into the
// past after a game stutter, and a frame on its new schedule is not late. Within a step it goes out as before. `enabled` false
// (LateGen=show) never drops. Ticks.
inline bool DropLate(bool enabled, bool generated, bool gpuOk, bool timed, long long now, long long rawTarget, long long step)
{
    if (!enabled || !generated)
        return false;
    if (!gpuOk)
        return true;
    return timed && now - rawTarget > step;
}

// LateGen=auto and early, where no flip metering paces the batch (xefg_own.h PresentReal), on a timed batch (not V-Sync, flip-metered, burst or on
// the clock pacer). `nextDone`: the game frame after the batch's own is finished on the GPU (false when not known). A GPU timeout drops
// with or without a timed batch (DropLate). The batch's real frame is never dropped.
// Early-out (LateGen=early only, an experiment, in xefg_own.h's WaitSlotOrNext): once the next game frame is finished, the frames still
// waiting for their slots - the real frame included - stop waiting; each goes out no sooner than MinGap after the last present that went
// out (CompressAt), so the batch never bunches.
// Stale (LateGen=auto and early): a GENERATED frame is not presented when the next game frame is finished and the presenter is at least
// MinGap (half a step) past the frame's slot when it gets to it: a frame show would put out a hair late (an allocator wait, recording, a
// preempted thread) is not dropped, which would leave a two-step hole. A frame on time is never dropped. A presenter that is behind
// (the next game frame was finished before the batch started) is not a reason of its own. Ticks.
inline bool AutoStale(bool generated, bool nextDone, long long now, long long slot, long long step)
{
    return generated && nextDone && now >= slot + MinGap(step);
}
// How long the wait for a slot spins at its end instead of sleeping (ms): the timer is set to the slot less this.
constexpr double kAutoSpinMs = 0.5;

// LateGen=compress (xefg_own.h, PresentReal): instead of dropping a late generated frame, squeeze the batch into the time left, using
// whether the game's NEXT frame is already finished on the GPU. `nextDone`: the frame after this batch's game frame is finished;
// `afterNextDone`: the one after that too. Both are read without waiting and are false when not known.

// Whether a present of a timed batch (`timed`, as in DropLate) is released without waiting for its slot: the next game frame is done, so
// holding this one for its slot only makes the picture older. Not timed (V-Sync, flip-metered or burst batch): never.
inline bool CompressEarly(bool timed, bool nextDone) { return timed && nextDone; }

// When an early release goes out (LateGen=compress, and LateGen=early's early-out): now, but no sooner than MinGap after the last present that really went out (`lastShown`, 0: none), so
// the frames of a squeezed batch keep their order and an even spacing and never bunch. Ticks.
inline long long CompressAt(long long now, long long lastShown, long long step)
{
    return lastShown ? std::max(now, lastShown + MinGap(step)) : now;
}

// Whether a generated frame is dropped with LateGen=compress. Never the real frame (`generated` false). A GPU wait that ran out (`gpuOk`
// false) drops as in LateGen=drop, timed or not. Otherwise only a frame a whole game frame behind on a timed batch: the next game frame and
// the one after it are both finished on the GPU. Merely late is squeezed, not dropped.
inline bool CompressDrop(bool generated, bool gpuOk, bool timed, bool nextDone, bool afterNextDone)
{
    if (!generated)
        return false;
    if (!gpuOk)
        return true;
    return timed && nextDone && afterNextDone;
}
// ---- the schedule arithmetic shared by the D3D12 presenter (xefg_own.h) and the host-agnostic pacer export (pace_api.h) ----------------
// One place for each formula: Own and pacecore::Pacer both call these, so a pacing fix reaches every host with the DLL. `freq` is the tick
// rate (QPC frequency) as a double; every time is in ticks.

// The ready-to-ready time of two app frames, ms.
inline double ReadyMs(long long ready, long long lastReady, double freq) { return static_cast<double>(ready - lastReady) * 1000.0 / freq; }
// The schedule's clock for the next app frame (Pick): NextBase with the interval and the lag in ticks.
inline long long NextScheduleBase(long long base, long long ready, double lagMs, double intervalMs, double freq)
{
    const long long iv = static_cast<long long>(intervalMs * freq / 1000.0);
    return NextBase(base, ready, static_cast<long long>(lagMs * freq / 1000.0), iv);
}
// One step of a batch of n generated frames + the app's own: the smoothed interval over n + 1.
inline long long StepTicks(double intervalMs, uint32_t n, double freq)
{
    return static_cast<long long>(intervalMs / static_cast<double>(n + 1) * freq / 1000.0);
}
// Frame k's time on the schedule that has no lead (PaceTarget: the generation lane's paths, GenBlock).
inline long long ScheduleTarget(long long base, double intervalMs, uint32_t n, uint32_t k, double freq, long long lastAnyPresent)
{
    const double step = intervalMs / static_cast<double>(n + 1) * freq / 1000.0;
    return Bound(base + static_cast<long long>(k * step), static_cast<long long>(step), lastAnyPresent);
}
// Frame k's time on a batch's own schedule (Target, with the lead of pace_lead.h frozen for the batch).
inline long long BatchTarget(long long base, long long lead, long long step, uint32_t k, long long lastAnyPresent)
{
    return Bound(base + pacelead::Offset(lead, step, k), step, lastAnyPresent);
}

// The app frame's time on the GPU timeline as the flip metering and the clock keep it (ClockObserve): smoothed (1/8), a delta over 1.5x left
// out at most twice in a row (one game stutter must not stretch the spacing for the frames after it), none outside 1-250 ms (set anew).
inline void ObserveBatch(double& batch, double& var, unsigned& hitchRun, unsigned& hitches, double ms)
{
    if (ms < 1 || ms > 250)
    {
        batch = var = 0;
        hitchRun = 0;
        return;
    }
    if (batch <= 0)
    {
        batch = ms;
        return;
    }
    if (ms > 1.5 * batch && hitchRun < 2)
    {
        ++hitchRun;
        ++hitches;
        return;
    }
    hitchRun = 0;
    const double d = ms - batch;
    batch += d / 8;
    var += (d * d - var) / 8;
}

// Flip metering of a batch (MeterBatch). kFlipMaxBatch: the most frames the driver meters in a batch; kFlipSamples: app frames with generation on
// before a batch is metered (3 and 8: the first batch got the start-up frames' 11-13 ms of 33; the batch time eases 1/8); kFlipGrowth: one long
// frame must not hold the whole next batch to it: up by an eighth per batch at most; kFlipRefusals: refusals in a row after which it is off.
constexpr unsigned kFlipMaxBatch = 8, kFlipSamples = 24, kFlipRefusals = 8;
constexpr double kFlipGrowth = 1.125;
// Why a batch is not metered (null: it is). `samples`: app frames with generation on in a row, including this one.
inline const char* FlipSkip(uint32_t n, bool displayPaced, bool refused, unsigned samples, double batchMs)
{
    return !n ? "not generating" : displayPaced ? "V-Sync" : refused ? "refused by the driver"
           : n + 1 > kFlipMaxBatch ? "too many frames per batch" : samples < kFlipSamples || batchMs <= 0 ? "frame time unknown" : nullptr;
}
// The batch time a metered batch of n + 1 is configured with: `prevOutputs` / `prevBatchMs` what was configured last (0: nothing).
inline double FlipBatchMs(double batchMs, uint32_t prevOutputs, double prevBatchMs, uint32_t n)
{
    return prevOutputs == n + 1 ? std::min(batchMs, prevBatchMs * kFlipGrowth) : batchMs;
}
// How early a metered frame after a batch's first is released (ticks, `spacing` the step). Auto: flipLeadUs at most 0.45 of the spacing (2500 us at
// 180 out), a set value wins.
constexpr double kFlipLeadShare = 0.45;
inline long long FlipLeadTicks(long long spacing, uint32_t flipLeadUs, bool flipLeadAuto, double freq)
{
    const long long set = static_cast<long long>(flipLeadUs * freq / 1e6);
    return flipLeadAuto && spacing > 0 ? std::min(set, static_cast<long long>(kFlipLeadShare * static_cast<double>(spacing))) : set;
}
} // namespace pacesched
