// Bounded waits for the presenter / generator path. Host-only pieces (no GPU, no Windows), so tests/stall_test.cpp can drive them with a
// presenter that never drains and a fence whose event fires late.
//
// What is guaranteed: the game thread's CPU waits on this path (its Present waiting for room in the presenter's queue; in the sink, its
// waits for room and for a copy) end within a bound for any finite stall or stuck GPU lane. On a timeout the context logs once (which
// wait, how long), stops generating (the game's frames pass through), never resets a command allocator the GPU may still be using, and
// generates again by itself once the stall has cleared (kGoodToRecover frames with room in a row, and at least kRecoverMs).
// What is not: while stalled the game runs at a few frames per second (its existing 200 ms retire wait plus one short slice per frame);
// a presenter thread that never returns can still stop the game's GPU queue through consumedFence / blockFence - on purpose, those
// fences are what keeps the game from overwriting a picture still to be copied. A few waits stay unbounded or long by design (see
// docs/ROBUSTNESS.md): what follows them frees or rebuilds what the GPU may be using.
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace waitbound
{
// Defaults (ini: WaitBoundMs, GameWaitMs in f5low.ini).
// - GPU fence / allocator waits 500 ms: a healthy frame is 5-50 ms and a long hitch (a loading screen) a few hundred; past half a second
//   the GPU lane is stuck, and the game must not follow it.
// - The game thread's wait for room in the presenter's queue 500 ms: with AppJobs=2 the game's Present legitimately waits about one
//   batch, which at 10-15 fps or over a GPU hitch is 100 ms or more; a shorter bound would drop generation on slow games. While the
//   context is already stalled the wait is a short slice (kStalledGameWaitMs) instead, so a presenter that stays stuck costs the game
//   one slice per frame, not one bound per frame.
// - Resize / teardown waits (kIdleWaitMs): long on purpose, what follows frees or rebuilds what the GPU may still use.
constexpr unsigned kGpuWaitMs = 500;
constexpr unsigned kGameWaitMs = 500;
constexpr unsigned kStalledGameWaitMs = 16;
constexpr unsigned kIdleWaitMs = 10000;
// Recovery: this many frames with room in a row and at least this long since the last timeout (a flapping lane does not turn generation
// on and off every few frames).
constexpr unsigned kGoodToRecover = 8;
constexpr int64_t kRecoverMs = 300;
// Spare command allocators per lane, used while the lane's own allocator is still in flight after the bound.
constexpr unsigned kSpares = 4;

inline int64_t NowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// One stall episode per context: Trip() on a timeout, Good() once per frame that had room.
class Episode
{
public:
    // A wait ran into its bound. True for the first trip of an episode: the caller logs it then (once, not once per frame).
    bool Trip(int64_t now = NowMs())
    {
        trips.fetch_add(1, std::memory_order_relaxed);
        good.store(0, std::memory_order_relaxed);
        lastTrip.store(now, std::memory_order_relaxed);
        return !stalled.exchange(true, std::memory_order_acq_rel);
    }
    // A frame had room. True when this one ended the episode: the caller logs the recovery.
    bool Good(int64_t now = NowMs())
    {
        if (!stalled.load(std::memory_order_acquire))
            return false;
        if (good.fetch_add(1, std::memory_order_relaxed) + 1 < kGoodToRecover || now - lastTrip.load(std::memory_order_relaxed) < kRecoverMs)
            return false;
        good.store(0, std::memory_order_relaxed);
        return stalled.exchange(false, std::memory_order_acq_rel);
    }
    bool Stalled() const { return stalled.load(std::memory_order_acquire); }
    uint64_t Trips() const { return trips.load(std::memory_order_relaxed); }

private:
    std::atomic<bool> stalled { false };
    std::atomic<uint32_t> good { 0 };
    std::atomic<uint64_t> trips { 0 };
    std::atomic<int64_t> lastTrip { 0 };
};

// Waits until done() >= v, at most `ms`: loops until the deadline, re-reading the fence after every wake. An event armed for an earlier
// value can fire late (SetEventOnCompletion stays registered after a timeout) and must not end this wait, nor count as a timeout. arm(v)
// registers the wake-up for v, wait(ms) sleeps on it for at most ms (it may return early). False when the deadline came first.
template <class Done, class Arm, class Wait>
inline bool WaitReached(uint64_t v, unsigned ms, Done done, Arm arm, Wait wait)
{
    const auto end = std::chrono::steady_clock::now() + std::chrono::milliseconds(ms);
    for (;;)
    {
        if (done() >= v)
            return true;
        const auto left = end - std::chrono::steady_clock::now();
        if (left <= std::chrono::steady_clock::duration::zero())
            return false;
        arm(v);
        wait(static_cast<unsigned>(std::chrono::duration_cast<std::chrono::milliseconds>(left).count() + 1));
    }
}

// cv.wait(l, pred) with a bound: false when the bound ran out with the predicate still false.
template <class Pred>
inline bool WaitFor(std::unique_lock<std::mutex>& l, std::condition_variable& cv, unsigned ms, Pred pred)
{
    return cv.wait_for(l, std::chrono::milliseconds(ms), pred);
}

// The bound the game's Present waits with: the configured one, a short slice while the context is stalled.
inline unsigned GameBound(bool stalled, unsigned ms) { return stalled ? (ms < kStalledGameWaitMs ? ms : kStalledGameWaitMs) : ms; }

// The game's Present waiting for room in the presenter's queue. `unbounded` is the old behaviour (the stall test proves it hangs).
template <class Pred>
inline bool WaitRoom(std::unique_lock<std::mutex>& l, std::condition_variable& cv, unsigned ms, bool unbounded, Pred pred)
{
    if (unbounded)
    {
        cv.wait(l, pred);
        return true;
    }
    return WaitFor(l, cv, ms, pred);
}

// The queue-depth rule of the game's Present, host-side: a frame is handed to the presenter either with room (generation as usual) or,
// after the bound, as a pass-through frame (no generation; the queue may then hold one over its limit per bound that ran out).
// While stalled the wait is the short slice and a timeout is no new trip (it neither logs nor restarts the recovery). `rebuilding`: a
// resize or rebuild is under way, which legitimately holds the presenter - ms is used as it is (the long bound), no trip, no fault.
struct Admit
{
    bool room;          // there was room within the bound
    bool passThrough;   // this frame is shown as the game made it
    bool firstTrip;     // this frame began a stall episode: log it (once)
};
template <class Pred>
inline Admit AdmitFrame(std::unique_lock<std::mutex>& l, std::condition_variable& cv, unsigned ms, bool unbounded, Episode& ep, Pred room,
                        bool rebuilding = false)
{
    const bool ok = WaitRoom(l, cv, rebuilding ? ms : GameBound(ep.Stalled(), ms), unbounded, room);
    const bool first = !ok && !rebuilding && !ep.Stalled() && ep.Trip();
    return { ok, !ok || ep.Stalled(), first };
}
} // namespace waitbound
