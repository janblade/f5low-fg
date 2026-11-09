// Bounded waits for the presenter / generator path: no wait on it may hang the game. Host-only pieces (no GPU, no Windows), so
// tests/stall_test.cpp can drive them with a presenter that never drains.
//
// The rule: every blocking wait has a bound. On a timeout the context logs once (which wait, how long), stops generating (the game's
// frames pass through without generated ones), never resets a command allocator the GPU may still use, and recovers by itself once the
// stall clears (kGoodToRecover frames in a row without a timeout).
#pragma once
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

namespace waitbound
{
// Defaults (ini: WaitBoundMs, GameWaitMs in f5low.ini).
// - GPU fence / allocator waits 500 ms: a healthy frame is 5-50 ms and a long hitch (a loading screen) a few hundred; past half a
//   second the GPU lane is stuck, and the game must not follow it. A well-known frame-generation presenter caps its CPU waits at about
//   the same.
// - The game thread's wait for room in the presenter's queue 500 ms: with AppJobs=2 the game's Present legitimately waits about one
//   batch, which at 10-15 fps or over a GPU hitch is 100 ms or more; a shorter bound would drop generation on slow games. While the
//   context is already stalled the wait is a short slice (kStalledGameWaitMs) instead, so a presenter that stays stuck costs the game
//   one slice per frame, not one bound per frame.
constexpr unsigned kGpuWaitMs = 500;
constexpr unsigned kGameWaitMs = 500;
constexpr unsigned kStalledGameWaitMs = 16;
// Frames in a row without a timeout that end a stall episode (about a third of a second at 60 fps: long enough that a flapping GPU lane
// does not turn generation on and off every frame).
constexpr unsigned kGoodToRecover = 20;
// Spare command allocators per lane, used while the lane's own allocator is still in flight after the bound.
constexpr unsigned kSpares = 4;

// One stall episode per context: Trip() on a timeout, Good() once per frame that had none.
class Episode
{
public:
    // A wait ran into its bound. True for the first trip of an episode: the caller logs it then (once, not once per frame).
    bool Trip()
    {
        trips.fetch_add(1, std::memory_order_relaxed);
        good.store(0, std::memory_order_relaxed);
        return !stalled.exchange(true, std::memory_order_acq_rel);
    }
    // A frame went by without a timeout. True when this one ended the episode: the caller logs the recovery.
    bool Good()
    {
        if (!stalled.load(std::memory_order_acquire))
            return false;
        if (good.fetch_add(1, std::memory_order_relaxed) + 1 < kGoodToRecover)
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
};

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
struct Admit
{
    bool room;          // there was room within the bound
    bool passThrough;   // this frame is shown as the game made it
    bool firstTrip;     // this frame began a stall episode: log it (once)
};
template <class Pred>
inline Admit AdmitFrame(std::unique_lock<std::mutex>& l, std::condition_variable& cv, unsigned ms, bool unbounded, Episode& ep, Pred room)
{
    const bool ok = WaitRoom(l, cv, GameBound(ep.Stalled(), ms), unbounded, room);
    const bool first = !ok && ep.Trip();
    return { ok, !ok || ep.Stalled(), first };
}
} // namespace waitbound
