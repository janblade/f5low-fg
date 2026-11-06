// Which queue gets which out of band notice, and the marker rule (src/oob_driver.h). Host only.
#include "../src/oob_driver.h"

#include <cstdio>

static int failures = 0;

static void Check(bool ok, const char* what)
{
    if (!ok)
    {
        std::printf("FAIL: %s\n", what);
        ++failures;
    }
}

int main()
{
    using namespace oobdriver;
    Check(TypeFor(true, true, true) == kLeaveAlone, "the application's queue is never marked, even when the swap chain presents on it");
    Check(TypeFor(false, true, true) == kRenderPresent, "our default queue: presents and generation");
    Check(TypeFor(false, true, false) == kPresent, "a queue with presents only");
    Check(TypeFor(false, false, true) == kRender, "the generation lane");
    Check(TypeFor(false, false, false) == kLeaveAlone, "an unused queue");
    Check(!MarkFrame(0), "no frame id, no marker");
    Check(MarkFrame(1), "a frame id gets its markers");
    Check(BurstNow(true, 2, true), "burst: a metered present after the first");
    Check(BurstNow(true, 4, true), "burst: the last present of a batch");
    Check(!BurstNow(true, 1, true), "no burst for the batch's first present");
    Check(!BurstNow(true, 2, false), "no burst when the batch is not metered");
    Check(!BurstNow(false, 2, true), "no burst when it is off");
    // Settings: Gate vetoes DriverPacing and the burst; DriverPacing sets the whole set; nothing asked changes nothing.
    {
        const DriverSettings a = ResolveDriver(false, 2, false, 14, false);
        Check(a.driverFg && a.parts == 15 && a.flipBurst && !a.pacingIgnored && !a.burstIgnored && !a.pacingAuto, "DriverPacing=2 sets the signals (all parts) and the burst, always");
        const DriverSettings a1 = ResolveDriver(false, 1, false, 14, false);
        Check(a1.driverFg && a1.parts == 15 && a1.flipBurst && a1.pacingAuto && !a1.pacingIgnored, "DriverPacing=1 sets the same, switched by the rule");
        const DriverSettings b = ResolveDriver(true, 1, false, 14, false);
        Check(!b.driverFg && b.parts == 14 && !b.flipBurst && b.pacingIgnored && !b.pacingAuto, "Gate=1 vetoes DriverPacing=1");
        const DriverSettings b2 = ResolveDriver(true, 2, false, 14, false);
        Check(!b2.driverFg && !b2.flipBurst && b2.pacingIgnored && !b2.pacingAuto, "Gate=1 vetoes DriverPacing=2");
        const DriverSettings c = ResolveDriver(true, 0, true, 3, true);
        Check(c.driverFg && c.parts == 3 && !c.flipBurst && c.burstIgnored && !c.pacingIgnored, "Gate=1 vetoes the burst, keeps the low-level signals");
        const DriverSettings d = ResolveDriver(false, 0, false, 15, false);
        Check(!d.driverFg && d.parts == 15 && !d.flipBurst && !d.pacingIgnored && !d.burstIgnored && !d.pacingAuto, "nothing asked, nothing changes");
        const DriverSettings e = ResolveDriver(false, 0, true, 6, true);
        Check(e.driverFg && e.parts == 6 && e.flipBurst && !e.pacingAuto, "the low-level keys still work with DriverPacing=0");
    }
    // PacingRule: enter at 0.75 held 2 s, leave at 0.55 held 2 s, 10 s between changes, ineligible = off at once.
    {
        PacingRule r;
        Check(!r.Update(1.8, 0, true), "high load: not on at once");
        Check(!r.Update(1.8, 1900, true), "high load held 1.9 s: not yet");
        Check(r.Update(1.8, 2000, true), "high load held 2 s: on");
        Check(r.Update(0.3, 2100, true) && r.Update(0.3, 5000, true), "low load while the state is young: stays on (dwell and hold)");
        Check(r.Update(0.3, 11999, true), "10 s dwell not over yet at 12 s after the change: stays on");
        Check(!r.Update(0.3, 12100, true), "low load held 2 s past the dwell: off");
        // hysteresis: between the thresholds nothing is held, and it resets a hold in progress
        PacingRule h;
        h.Update(0.9, 0, true);
        h.Update(0.6, 1500, true);          // between: the hold starts over
        Check(!h.Update(0.9, 2500, true), "a load between the thresholds resets the enter hold");
        Check(h.Update(0.9, 4500, true), "enter hold counted again from 2.5 s: on at 4.5 s");
        PacingRule g;
        g.Update(0.9, 0, true);
        g.Update(0.9, 2000, true);          // on at 2 s
        g.Update(0.4, 14000, true);
        g.Update(0.6, 15000, true);         // between: leave hold reset
        Check(g.Update(0.4, 16000, true), "leave hold reset by a load between the thresholds");
        Check(!g.Update(0.4, 18100, true), "leave hold of 2 s from 16 s: off");
        // a load at exactly the enter threshold counts
        PacingRule t;
        t.Update(0.75, 0, true);
        Check(t.Update(0.75, 2000, true), "load == 0.75 enters");
        // a dip between the thresholds before the hold ends cancels it
        PacingRule c;
        c.Update(0.9, 0, true);
        c.Update(0.7, 1000, true);
        Check(!c.Update(0.9, 2500, true), "a dip between the thresholds restarts the hold");
        // dwell: held long enough but too soon after the last change
        PacingRule d2;
        d2.Update(0.9, 0, true);
        d2.Update(0.9, 2000, true);         // on
        d2.Update(0.2, 3000, true);
        d2.Update(0.2, 6000, true);
        Check(d2.on, "held low for 3 s but only 4 s after the change: stays on");
        Check(!d2.Update(0.2, 12000, true), "off once the 10 s dwell is over");
        d2.Update(0.9, 12100, true);
        Check(!d2.Update(0.9, 14200, true), "not on again: 2.2 s after the off change, dwell not over");
        Check(d2.Update(0.9, 22100, true), "on again after the dwell");
        // eligibility: off at once, counted as a change, not held while ineligible
        Check(!d2.Update(0.9, 23000, false), "ineligible: off at once");
        d2.Update(0.9, 23500, true);
        Check(!d2.Update(0.9, 26000, true), "after the eligibility off: dwell of 10 s from it (not on at 26 s)");
        Check(d2.Update(0.9, 33100, true), "on after the dwell once the load was held");
        PacingRule e2;
        Check(!e2.Update(0.9, 0, false) && !e2.Update(0.9, 5000, false), "ineligible while off: stays off, no hold counted");
        e2.Update(0.9, 5100, true);
        Check(!e2.Update(0.9, 6000, true) && e2.Update(0.9, 7100, true), "the hold starts when it becomes eligible, and is not counted a change while off");
    }
    Check(kIdSetReflexSync == 0xB9F6FAFFu && kIdSetFlipConfig == 0xF3148C42u, "nvapi ids are the public ones");
    if (failures == 0)
        std::printf("oob_driver_test PASS\n");
    return failures == 0 ? 0 : 1;
}
