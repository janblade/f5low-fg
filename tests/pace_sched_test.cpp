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
    if (g_fail)
    {
        printf("pace_sched_test: %d FAILED\n", g_fail);
        return 1;
    }
    printf("pace_sched_test: PASS\n");
    return 0;
}
