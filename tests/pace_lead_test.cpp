// Host-only test of the PaceLead rule (src/pace_lead.h): no GPU, no Windows API. tests/build_pace_lead_test.ps1 builds it;
// CI runs it. Exit code 0: all checks passed.
#include "../src/pace_lead.h"
#include <cmath>
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
static bool Near(double a, double b) { return std::fabs(a - b) < 1e-9; }

// One app frame: freeze, a generated frame that came `late` ms late (0: none), end. Returns the lead it was frozen with.
static double Frame(pacelead::Lead& l, double stepMs, double late)
{
    const double frozen = l.Freeze(stepMs);
    l.Late(late);
    l.EndFrame(frozen, stepMs);
    return frozen;
}

int main()
{
    // (a) lead off: the offset is k steps, as before the lead existed.
    {
        const long long step = 166667;
        for (uint32_t k = 1; k <= 6; ++k)
            CHECK(pacelead::Offset(0, step, k) == static_cast<long long>(k) * step);
        // and with a lead: lead + (k - 1) steps, the lead at most a step
        CHECK(pacelead::Offset(30000, step, 1) == 30000);
        CHECK(pacelead::Offset(30000, step, 3) == 30000 + 2 * step);
        CHECK(pacelead::Offset(2 * step, step, 2) == 2 * step);
        pacelead::Lead off;
        off.automatic = false;
        CHECK(!off.On() && off.Freeze(16.7) == 0);
    }
    // (b) one late of 1.2 ms: the next lead is 3 + 1.2 + 1 = 5.2, held for the window, then eased by 1/16 of the way per frame.
    {
        pacelead::Lead l;
        CHECK(Near(Frame(l, 16.7, 0), 3.0));
        CHECK(Near(Frame(l, 16.7, 1.2), 3.0));
        CHECK(Near(l.ms, 5.2));
        for (size_t i = 1; i < pacelead::kWindow; ++i)      // the window still holds it for 127 more app frames
        {
            Frame(l, 16.7, 0);
            CHECK(Near(l.ms, 5.2));
        }
        const double before = l.ms;
        Frame(l, 16.7, 0);                                  // the 128th frame after it: gone from the window
        CHECK(Near(l.ms, before - (before - pacelead::kFloor) / 16));
        for (int i = 0; i < 400; ++i)
            Frame(l, 16.7, 0);
        CHECK(l.ms >= pacelead::kFloor && l.ms < pacelead::kFloor + 0.01);
        // under the late threshold nothing happens
        pacelead::Lead q;
        Frame(q, 16.7, 0.4);
        CHECK(Near(q.ms, 3.0));
    }
    // (c) a 10 ms late raises the lead by 3 ms at most (2 ms of late + 1 ms margin).
    {
        pacelead::Lead l;
        Frame(l, 16.7, 10.0);
        CHECK(Near(l.ms, 6.0));
        // repeated late frames climb step by step
        Frame(l, 16.7, 10.0);
        CHECK(Near(l.ms, 9.0));
    }
    // (d) the lead never exceeds the step: neither frozen nor followed.
    {
        pacelead::Lead l;
        for (int i = 0; i < 20; ++i)
            CHECK(Frame(l, 8.3, 10.0) <= 8.3);
        CHECK(l.ms <= 8.3 + 1e-9);
        pacelead::Lead s;
        CHECK(Near(s.Freeze(1.4), 1.4));                    // a step shorter than the 3 ms start
    }
    // (e) fixed mode: constant, no adaptation.
    {
        pacelead::Lead f;
        f.automatic = false;
        f.fixedMs = 5.0;
        for (int i = 0; i < 300; ++i)
            CHECK(Near(Frame(f, 16.7, i % 7 == 0 ? 8.0 : 0), 5.0));
        CHECK(Near(f.ms, 5.0));
    }
    printf(g_fail ? "pace_lead_test: %d FAILED\n" : "pace_lead_test: PASS\n", g_fail);
    return g_fail ? 1 : 0;
}
