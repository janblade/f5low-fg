// Host-only test of the schedule pieces shared by the own generator and the sink (src/pace_sched.h, src/pace_lead.h Offset): no GPU, no
// Windows API. tests/build_pace_sched_test.ps1 builds it; CI runs it. Exit code 0: all checks passed.
#include "../src/pace_lead.h"
#include "../src/pace_sched.h"
#include <cmath>
#include <cstdio>
#include <deque>

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
static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

int main()
{
    // ReadyInterval: eased by 1/10
    {
        pacesched::ReadyInterval r;
        r.ms = 16.7;
        r.Observe(16.7);
        CHECK(Near(r.ms, 16.7));
        r.Observe(26.7);                                     // one slow sample: eased, not followed
        CHECK(Near(r.ms, 16.7 * 0.9 + 26.7 * 0.1));
    }
    // up rule: two samples in a row over 1.3x -> raised at once to the shorter of the two
    {
        pacesched::ReadyInterval r;
        r.ms = 16.0;
        r.Observe(16.0);
        r.Observe(40.0);                                     // 1 sample: eased only
        CHECK(r.ms < 20.0);
        r.Observe(35.0);                                     // 2nd in a row over 1.3x: min(35, 40)
        CHECK(Near(r.ms, 35.0));
    }
    // down rule: three samples under 1/1.3 of it -> lowered at once to the longest of the three
    {
        pacesched::ReadyInterval r;
        r.ms = 33.0;
        for (int i = 0; i < 3; ++i)
            r.Observe(33.0);                                 // run >= 3, steady
        r.Observe(16.0);
        r.Observe(16.0);
        CHECK(r.ms > 25.0);                                  // two fast samples: not yet
        r.Observe(15.0);
        CHECK(Near(r.ms, 16.0));                             // the longest of the last three
    }
    // a catch-up (one or two quick frames after a stall) does not lower it
    {
        pacesched::ReadyInterval r;
        r.ms = 33.0;
        for (int i = 0; i < 5; ++i)
            r.Observe(33.0);
        r.Observe(5.0);
        r.Observe(5.0);
        CHECK(r.ms > 25.0);
        r.Observe(33.0);
        CHECK(r.ms > 25.0);
    }
    // out of range (a pause, 0): the run restarts, the interval is kept
    {
        pacesched::ReadyInterval r;
        r.ms = 20.0;
        r.Observe(20.0);
        r.Observe(500.0);
        CHECK(Near(r.ms, 20.0) && r.run == 0 && r.prev == 0);
        r.Observe(0.1);
        CHECK(Near(r.ms, 20.0));
    }
    // NextBase: set anew at the first frame and when more than an interval off, else a fifth of the way
    {
        CHECK(pacesched::NextBase(0, 1000, 5, 100) == 1005);
        const long long b = 1000, iv = 100;
        CHECK(pacesched::NextBase(b, 1100, 0, iv) == 1100);                       // exactly due
        CHECK(pacesched::NextBase(b, 1150, 0, iv) == 1100 + (1150 - 1100) / 5);   // 50 late: a fifth of it
        CHECK(pacesched::NextBase(b, 1300, 0, iv) == 1300);                       // more than an interval off: anew
        CHECK(pacesched::NextBase(b, 800, 0, iv) == 800);
    }
    // Bound: half a step after the last present at the soonest, two steps at the latest
    {
        CHECK(pacesched::Bound(50, 10, 0) == 50);                // no last present: as it is
        CHECK(pacesched::Bound(101, 10, 100) == 105);            // too soon: last + step / 2
        CHECK(pacesched::Bound(110, 10, 100) == 110);            // in range
        CHECK(pacesched::Bound(500, 10, 100) == 120);            // too late: last + 2 steps
        // A burst release (DriverPacing): the presents after a batch's first go out at once, so measured from the time they went out the
        // next batch's first present (slot 100 + 4 * 10 = 140 after four presents) would be bounded from a last present at ~100; from the slot of
        // the last release (140) it keeps the interval: target 150 stays, and a late target is limited from the slot.
        CHECK(pacesched::Bound(150, 10, 140) == 150);
        CHECK(pacesched::Bound(150, 10, 100) == 120);            // bounded from the real time of the early release: pulled in by 30
        CHECK(pacesched::Bound(300, 10, 140) == 160);
    }
    // Offset with and without a cap
    {
        using pacelead::Offset;
        CHECK(Offset(0, 100, 1) == 100 && Offset(0, 100, 3) == 300);       // lead off: k steps
        CHECK(Offset(30, 100, 1) == 30 && Offset(30, 100, 3) == 230);      // lead + (k - 1) steps
        CHECK(Offset(250, 100, 1) == 100);                                 // a lead over a step is cut to it
        CHECK(Offset(250, 100, 1, 500) == 250);                            // ... unless the cap is wider
        CHECK(Offset(900, 100, 2, 500) == 600);                            // ... and is cut to the cap
    }
    // LeadCap: the lead rule is capped, a fixed test lead is never cut (S3)
    {
        CHECK(pacesched::LeadCap(100, 250, false) == 100);
        CHECK(pacesched::LeadCap(100, 250, true) == 250);
        CHECK(pacesched::LeadCap(300, 250, true) == 300);
        CHECK(pacelead::Offset(250, 100, 1, pacesched::LeadCap(100, 250, true)) == 250);   // lead 6 ms at a 2.8 ms step really is 6 ms
        CHECK(pacelead::Offset(250, 100, 1, pacesched::LeadCap(100, 250, false)) == 100);
    }
    // PickStamp: the newest stamp not later than the arrival (S2)
    {
        std::deque<long long> s { 100, 200, 300 };
        long long st = 0;
        CHECK(pacesched::PickStamp(s, 250, st) && st == 200);   // 100 dropped, 200 taken, 300 stays for the next burst
        CHECK(s.size() == 1 && s.front() == 300);
        CHECK(!pacesched::PickStamp(s, 250, st));               // the only one left is later than the arrival: none
        CHECK(s.size() == 1);
        CHECK(pacesched::PickStamp(s, 400, st) && st == 300 && s.empty());
        CHECK(!pacesched::PickStamp(s, 400, st));               // none at all
        // one extra stamp (a Present that never became a burst) does not shift later bursts
        std::deque<long long> e { 100, 133, 166 };              // 100 was extra
        CHECK(pacesched::PickStamp(e, 170, st) && st == 166);   // the newest one before the arrival
        e.push_back(199);
        CHECK(pacesched::PickStamp(e, 205, st) && st == 199);   // in step again
        // a TEST present never gets this far: the stamp chain does not stamp it (xintel::StampChain), so no stamp for it exists here
    }
    // PickStampBy: the same pairing on AppStamp entries, by the CPU time of the Present only - a GPU time never decides it
    {
        std::deque<pacesched::AppStamp> s { { 100, 1 }, { 200, 2 }, { 300, 3 } };
        const auto cpu = [](const pacesched::AppStamp& a) { return a.cpu; };
        pacesched::AppStamp st;
        CHECK(pacesched::PickStampBy(s, 250, st, cpu) && st.cpu == 200 && st.fence == 2);
        CHECK(s.size() == 1 && s.front().fence == 3);
        CHECK(!pacesched::PickStampBy(s, 250, st, cpu));
        CHECK(pacesched::PickStampBy(s, 400, st, cpu) && st.fence == 3 && s.empty());
        // the GPU time of Present 2 (say 260) is later than the burst's arrival (250): the burst still belongs to Present 2
        pacesched::GpuStampRing g;
        g.Publish(1, 140);
        long long t = 0;
        std::deque<pacesched::AppStamp> p { { 100, 1 }, { 200, 2 }, { 300, 3 } };
        CHECK(pacesched::PickStampBy(p, 250, st, cpu) && st.fence == 2);
        CHECK(!g.Get(st.fence, t));                              // its GPU time is not there: the caller uses st.cpu
        g.Publish(2, 260);
        CHECK(g.Get(st.fence, t) && t == 260);                   // later: the GPU time is the value (260 > the arrival 250 is fine)
    }
    // GpuStampRing: consecutive values, the last 64 kept, 0 = never signalled
    {
        pacesched::GpuStampRing g;
        long long t = 0;
        CHECK(!g.Get(0, t));
        CHECK(!g.Get(1, t));
        g.Publish(1, 1000);
        g.Publish(2, 2000);
        CHECK(g.Get(1, t) && t == 1000);
        CHECK(g.Get(2, t) && t == 2000);
        CHECK(!g.Get(3, t));
        g.Publish(2, 9999);                                      // a repeat or an older value changes nothing
        CHECK(g.Get(2, t) && t == 2000);
        for (uint64_t v = 3; v <= 100; ++v)
            g.Publish(v, static_cast<long long>(v) * 1000);
        CHECK(g.Get(100, t) && t == 100000);
        CHECK(g.Get(37, t) && t == 37000);                       // 64 back from 100: still there
        CHECK(!g.Get(36, t));                                    // 65 back: forgotten, the CPU time is used
        CHECK(!g.Get(101, t));
    }
    // Lag: raised at once to scheduled + late (capped at maxLag intervals), falls 2 % per batch, small lates do not count
    {
        pacesched::Lag lag;
        lag.Late(0, 0.3, 16.0, 0.5);                             // under kLagLateMs: nothing
        CHECK(Near(lag.ms, 0));
        lag.Late(0, 2.0, 16.0, 0.5);
        CHECK(Near(lag.ms, 2.0));
        lag.Late(0, 1.0, 16.0, 0.5);                             // less than the lag now: never lowered by a late frame
        CHECK(Near(lag.ms, 2.0));
        lag.Late(2.0, 1.5, 16.0, 0.5);                           // late on top of the lag the batch was scheduled with: 3.5
        CHECK(Near(lag.ms, 3.5));
        lag.Late(3.5, 20.0, 16.0, 0.5);                          // capped at half an interval
        CHECK(Near(lag.ms, 8.0));
        lag.EndFrame();
        CHECK(Near(lag.ms, 8.0 * 0.98));
        for (int i = 0; i < 34; ++i)
            lag.EndFrame();
        CHECK(lag.ms < 4.1 && lag.ms > 3.8);                     // about half in ~35 batches
        pacesched::Lag none;
        none.Late(0, 5.0, 16.0, 0.0);                            // MaxLag=0: no lag
        CHECK(Near(none.ms, 0));
        // the base with a lag: the stamp plus the lag
        CHECK(pacesched::NextBase(0, 1000, 80, 1600) == 1080);
    }
    // GpuBase: GPU stamps track the offset to the CPU time; a miss stays continuous, a pause resets, no GPU stamp yet = CPU time
    {
        const long long iv = 1600, off = 700;                   // the GPU stamp trails the CPU time of its Present by 700 ticks
        pacesched::GpuBase gb;
        // first: no GPU stamp yet - the CPU time as it is
        long long base = gb.Next(0, false, 0, 1000, 0, iv);
        CHECK(base == 1000 && !gb.have);
        gb.Reset();
        // first GPU stamp: set anew, offset learned
        base = gb.Next(0, true, 1000 + off, 1000, 0, iv);
        CHECK(base == 1000 + off && gb.have && Near(gb.offset, off));
        // steady state with exact stamps: stays on the grid
        for (int i = 1; i <= 5; ++i)
            base = gb.Next(base, true, 1000 + off + i * iv, 1000 + i * iv, 0, iv);
        CHECK(base == 1000 + off + 5 * iv);
        // a miss in steady state: the CPU time moved by the offset = the same grid point, no jump (a raw CPU time would be 700 early)
        const long long miss = gb.Next(base, false, 0, 1000 + 6 * iv, 0, iv);
        CHECK(miss == 1000 + off + 6 * iv);
        CHECK(gb.missRun == 1);
        // a GPU stamp ends the run of misses
        base = gb.Next(miss, true, 1000 + off + 7 * iv, 1000 + 7 * iv, 0, iv);
        CHECK(gb.missRun == 0 && base == 1000 + off + 7 * iv);
        // a miss after a long pause (more than an interval off): reset to the corrected CPU time, not the stale predicted base
        const long long later = 1000 + 40 * iv;
        CHECK(gb.Next(base, false, 0, later, 0, iv) == later + off);
        // three misses in a row keep the offset, the fourth takes the CPU time as it is
        gb.Reset();
        base = gb.Next(0, true, 5000 + off, 5000, 0, iv);
        for (int i = 1; i <= 3; ++i)
            base = gb.Next(base, false, 0, 5000 + i * iv, 0, iv);
        CHECK(base == 5000 + off + 3 * iv);
        base = gb.Next(base, false, 0, 5000 + 4 * iv, 0, iv);   // 4th: CPU time as it is, drawn a fifth of the way from the grid
        CHECK(base == (5000 + off + 4 * iv) + (5000 + 4 * iv - (5000 + off + 4 * iv)) / 5);
        // the offset is smoothed
        gb.Reset();
        gb.Next(0, true, 1000 + 800, 1000, 0, iv);
        gb.Next(1000 + 800, true, 1000 + iv + 800 + 80, 1000 + iv, 0, iv);
        CHECK(Near(gb.offset, 800 + 80.0 / 8));
    }
    if (g_fail)
    {
        printf("pace_sched_test: %d FAILED\n", g_fail);
        return 1;
    }
    printf("pace_sched_test: PASS\n");
    return 0;
}
