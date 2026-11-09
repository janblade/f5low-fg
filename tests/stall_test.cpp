// Host-only stall test of the bounded waits (src/wait_bound.h): a presenter that never drains must not hold the game's Present. No GPU,
// no Windows API. tests/build_stall_test.ps1 builds it; exit code 0: all checks passed.
//
// The model is the game-side push of xefg_own.h: lock, wait for room in the presenter's queue, queue the frame. "Main" is the old wait
// (unbounded): the test proves it hangs, with a guard that gives up after a second, then shows the bounded one returns in time,
// reports pass-through, and recovers once the presenter drains.
#include "../src/wait_bound.h"
#include <atomic>
#include <chrono>
#include <cstdio>
#include <deque>
#include <thread>

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

using Clock = std::chrono::steady_clock;
static double Ms(Clock::time_point a, Clock::time_point b) { return std::chrono::duration<double, std::milli>(b - a).count(); }

struct Model
{
    std::mutex pm;
    std::condition_variable pcv;
    std::deque<int> jobs;           // frames waiting for the presenter
    unsigned appJobs = 2;           // as AppJobs
    bool quit = false;
    waitbound::Episode ep;

    // The game's push; says whether there was room and whether the frame goes through as a pass-through frame.
    waitbound::Admit Push(int frame, bool unbounded, unsigned ms)
    {
        std::unique_lock<std::mutex> l(pm);
        const waitbound::Admit a = waitbound::AdmitFrame(l, pcv, ms, unbounded, ep, [this] { return jobs.size() < appJobs || quit; });
        jobs.push_back(frame);
        return a;
    }
    // A presenter that takes one frame.
    void Drain1()
    {
        {
            std::lock_guard<std::mutex> l(pm);
            if (!jobs.empty())
                jobs.pop_front();
        }
        pcv.notify_all();
    }
};

int main()
{
    // 1. Main's wait: a presenter that never drains hangs the game thread (the guard gives up after 1 s).
    {
        Model m;
        std::atomic<bool> returned { false };
        std::thread game([&] {
            for (int f = 0; f < 5; ++f)
                m.Push(f, true, 0);
            returned = true;
        });
        const auto t0 = Clock::now();
        while (!returned && Ms(t0, Clock::now()) < 1000)
            std::this_thread::sleep_for(std::chrono::milliseconds(10));
        CHECK(!returned);                       // old behaviour: hangs
        printf("unbounded wait: %s\n", returned ? "returned (unexpected)" : "HANG (the old behaviour, as expected)");
        { // let it go so the thread can end
            std::lock_guard<std::mutex> l(m.pm);
            m.quit = true;
        }
        m.pcv.notify_all();
        game.join();
    }
    // 2. The bounded wait: the first timeout takes the normal bound, the following pushes only the short slice while the context is
    // stalled; frames beyond the queue's room pass through.
    {
        Model m;
        int passed = 0, generated = 0;
        double first = 0, restWorst = 0;
        for (int f = 0; f < 8; ++f)
        {
            const auto t0 = Clock::now();
            const bool pass = m.Push(f, false, 100).passThrough;
            const double ms = Ms(t0, Clock::now());
            if (f == 2)
                first = ms;
            else if (f > 2)
                restWorst = std::max(restWorst, ms);
            (pass ? passed : generated)++;
        }
        printf("bounded: first timeout %.1f ms, then at most %.1f ms, %d generated, %d passed through, %llu trips\n", first, restWorst, generated, passed,
               static_cast<unsigned long long>(m.ep.Trips()));
        CHECK(first >= 95 && first < 100 + 60);                 // the normal bound
        CHECK(restWorst < waitbound::kStalledGameWaitMs + 40);  // the short slice while stalled
        CHECK(waitbound::GameBound(false, 500) == 500 && waitbound::GameBound(true, 500) == waitbound::kStalledGameWaitMs);
        CHECK(waitbound::GameBound(true, 5) == 5);
        CHECK(waitbound::kGameWaitMs == 500);
        CHECK(generated == 2);                  // the queue's room (appJobs)
        CHECK(passed == 6);
        CHECK(m.ep.Stalled());
        CHECK(m.ep.Trips() == 6);
        // The stall episode logs once: only the first trip reports it.
        waitbound::Episode e2;
        CHECK(e2.Trip());
        CHECK(!e2.Trip());
        CHECK(!e2.Trip());
        // 3. Recovery: the presenter drains, frames find room again, and after kGoodToRecover of them the episode ends (once).
        int ended = 0;
        for (int f = 0; f < 60; ++f)
        {
            m.Drain1();
            m.Drain1();
            m.Drain1();                         // (the queue is over its limit by the pass-through frames)
            m.Drain1();
            m.Drain1();
            m.Drain1();
            m.Drain1();
            m.Drain1();
            const waitbound::Admit a = m.Push(100 + f, false, 100);
            if (a.room && m.ep.Good())
                ++ended;
        }
        CHECK(ended == 1);
        CHECK(!m.ep.Stalled());
    }
    // 4. A flapping stall: one good frame between timeouts does not end the episode.
    {
        waitbound::Episode e;
        e.Trip();
        for (int i = 0; i < 10; ++i)
        {
            for (unsigned k = 0; k + 1 < waitbound::kGoodToRecover; ++k)
                CHECK(!e.Good());
            e.Trip();
        }
        CHECK(e.Stalled());
    }
    // 5. A fence that never completes: WaitFor returns false within the bound (the GPU waits use the same call).
    {
        std::mutex mu;
        std::condition_variable cv;
        std::unique_lock<std::mutex> l(mu);
        const auto t0 = Clock::now();
        const bool ok = waitbound::WaitFor(l, cv, waitbound::kGpuWaitMs, [] { return false; });
        const double ms = Ms(t0, Clock::now());
        CHECK(!ok);
        CHECK(ms >= waitbound::kGpuWaitMs - 5 && ms < waitbound::kGpuWaitMs + 100);
    }
    printf(g_fail ? "stall_test: %d FAILED\n" : "stall_test: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
