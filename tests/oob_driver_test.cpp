// Which queue gets which out of band notice, and the marker rule (src/oob_driver.h). Host only.
#include "../src/oob_driver.h"

#include <cstdio>
#include <initializer_list>

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
    Check(!FlipMeteringOn("auto", 75), "flip metering auto: off on an RTX 20 (sm_75), no hardware metering");
    Check(!FlipMeteringOn("auto", 86) && !FlipMeteringOn("auto", 89), "flip metering auto: off on RTX 30/40");
    Check(FlipMeteringOn("auto", 120) && FlipMeteringOn("auto", 100), "flip metering auto: on from Blackwell (sm_100, sm_120)");
    Check(!FlipMeteringOn("auto", 0), "flip metering auto: off when the GPU is unknown");
    Check(FlipMeteringOn("1", 75), "FlipMetering=1 forces it on any GPU");
    Check(!FlipMeteringOn("0", 120), "FlipMetering=0 turns it off on Blackwell too");
    Check(FlipMeteringOn(nullptr, 120) && !FlipMeteringOn("", 75), "no setting: the GPU decides");
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
    // The default and the host handshake: DriverPacing absent (-1) is automatic when the host says it routes the markers, else off; 0 is off;
    // 1 and 2 are honoured as asked, with a warning on a host that did not say; Gate=1 vetoes; a GPU that is not NVIDIA's turns it all off.
    {
        // ResolveDriver(gate, driverPacing, driverFg, parts, flipBurst, hostRoutes, nvidia)
        const DriverSettings a = ResolveDriver(false, -1, false, 15, false, true, true);
        Check(a.mode == 1 && a.pacingAuto && a.driverFg && a.flipBurst && a.parts == 15 && a.why == PacingWhy::kDefaultAuto && !a.oldHostWarn,
              "key absent, host routes the markers: automatic");
        const DriverSettings b = ResolveDriver(false, -1, false, 15, false, false, true);
        Check(b.mode == 0 && !b.pacingAuto && !b.driverFg && !b.flipBurst && b.why == PacingWhy::kDefaultOffOldHost && !b.oldHostWarn,
              "key absent, old host: off, the chain is the plain one, no warning (nothing was asked)");
        for (const bool host : { true, false })
        {
            const DriverSettings z = ResolveDriver(false, 0, false, 15, false, host, true);
            Check(z.mode == 0 && !z.driverFg && !z.flipBurst && !z.pacingAuto && z.why == PacingWhy::kExplicitOff && !z.oldHostWarn,
                  "DriverPacing=0: off with or without the handshake");
            const DriverSettings o = ResolveDriver(false, 1, false, 15, false, host, true);
            Check(o.mode == 1 && o.pacingAuto && o.driverFg && o.flipBurst && o.why == PacingWhy::kExplicit && o.oldHostWarn == !host,
                  "DriverPacing=1: honoured, warned about on an old host only");
            const DriverSettings t = ResolveDriver(false, 2, false, 15, false, host, true);
            Check(t.mode == 2 && !t.pacingAuto && t.driverFg && t.flipBurst && t.why == PacingWhy::kExplicit && t.oldHostWarn == !host,
                  "DriverPacing=2: honoured, warned about on an old host only");
        }
        // Gate=1 vetoes the default, and the explicit modes, with or without the handshake
        for (const bool host : { true, false })
        {
            const DriverSettings g0 = ResolveDriver(true, -1, false, 15, false, host, true);
            Check(g0.mode == 0 && !g0.flipBurst && !g0.driverFg && !g0.pacingAuto && !g0.pacingIgnored && (host ? g0.why == PacingWhy::kGateVeto : g0.why == PacingWhy::kDefaultOffOldHost),
                  "Gate=1, key absent: off, nothing was asked, not 'ignored'");
            const DriverSettings g1 = ResolveDriver(true, 1, false, 15, false, host, true);
            Check(g1.mode == 0 && !g1.flipBurst && !g1.pacingAuto && g1.pacingIgnored && g1.why == PacingWhy::kGateVeto, "Gate=1 vetoes DriverPacing=1");
            const DriverSettings g2 = ResolveDriver(true, 2, false, 15, false, host, true);
            Check(g2.mode == 0 && !g2.flipBurst && g2.pacingIgnored, "Gate=1 vetoes DriverPacing=2");
        }
        // no NVIDIA compute device for the adapter: everything off, whatever was asked, the low-level keys included
        for (const int ask : { -1, 0, 1, 2 })
            for (const bool host : { true, false })
            {
                const DriverSettings n = ResolveDriver(false, ask, true, 3, true, host, false);
                Check(n.mode == 0 && !n.driverFg && !n.flipBurst && !n.pacingAuto && n.nvidiaOff && n.why == PacingWhy::kNotNvidia && n.pacingIgnored == (ask > 0),
                      "not NVIDIA: signals, burst and mode off, even the low-level keys, explicit 1 and 2 included");
            }
        const DriverSettings nq = ResolveDriver(false, 0, false, 15, false, true, false);
        Check(nq.mode == 0 && !nq.nvidiaOff && nq.why == PacingWhy::kExplicitOff, "not NVIDIA and nothing asked: nothing to report");
        const DriverSettings nb = ResolveDriver(false, -1, false, 15, false, false, false);
        Check(nb.mode == 0 && !nb.nvidiaOff && nb.why == PacingWhy::kDefaultOffOldHost, "not NVIDIA, old host, key absent: nothing was asked, nothing to report");
        const DriverSettings ng = ResolveDriver(true, 2, true, 3, true, true, false);
        Check(!ng.driverFg && !ng.flipBurst && ng.mode == 0 && ng.nvidiaOff, "not NVIDIA wins over Gate=1: low-level driverFg off too");
        // the low-level keys with DriverPacing=0 on NVIDIA still work, on any host
        const DriverSettings ll = ResolveDriver(false, 0, true, 6, true, false, true);
        Check(ll.driverFg && ll.parts == 6 && ll.flipBurst && ll.mode == 0 && !ll.pacingAuto, "the low-level keys with DriverPacing=0 work as before");
    }
    // PacingRule: enter at 0.75 held 2 s, leave at 0.55 held 2 s, 10 s between changes, a hard cause = off at once, a transient cause only
    // after 1 s without a break, a gap over 0.5 s between updates restarts the holds. Hold() updates every 100 ms from a to b.
    {
        auto Hold = [](PacingRule& r, double load, double a, double b, Eligibility e = Eligibility::kEligible) {
            bool s = r.on;
            for (double t = a; t <= b + 1e-9; t += 100)
                s = r.Update(load, t, e);
            return s;
        };
        PacingRule r;
        Check(!Hold(r, 1.8, 0, 1900), "high load held 1.9 s: not yet");
        Check(r.Update(1.8, 2000, true), "high load held 2 s: on");
        Check(Hold(r, 0.3, 2100, 11900), "low load while the state is young: stays on (dwell 10 s)");
        Check(!r.Update(0.3, 12000, true), "low load held 2 s past the dwell: off");
        // hysteresis: between the thresholds nothing is held, and it resets a hold in progress
        PacingRule h;
        Hold(h, 0.9, 0, 1500);
        h.Update(0.6, 1600, true);          // between: the hold starts over
        Check(!Hold(h, 0.9, 1700, 3600), "a load between the thresholds resets the enter hold");
        Check(h.Update(0.9, 3700, true), "enter hold counted again from 1.7 s: on at 3.7 s");
        PacingRule g;
        Hold(g, 0.9, 0, 2000);              // on at 2 s
        Hold(g, 0.6, 2100, 13900);          // between the thresholds: nothing is held, it stays on
        Hold(g, 0.4, 14000, 14500);
        g.Update(0.6, 14600, true);         // between: the leave hold starts over
        Check(Hold(g, 0.4, 14700, 16500), "leave hold reset by a load between the thresholds");
        Check(!g.Update(0.4, 16700, true), "leave hold of 2 s from 14.7 s: off at 16.7 s");
        // a load at exactly the enter threshold counts
        PacingRule t;
        Check(Hold(t, 0.75, 0, 2000), "load == 0.75 enters");
        // a dip between the thresholds before the hold ends cancels it
        PacingRule c;
        Hold(c, 0.9, 0, 900);
        c.Update(0.7, 1000, true);
        Check(!Hold(c, 0.9, 1100, 2900), "a dip between the thresholds restarts the hold");
        // dwell: held long enough but too soon after the last change
        PacingRule d2;
        Hold(d2, 0.9, 0, 2000);             // on
        Check(Hold(d2, 0.2, 2100, 11900), "held low for long, but under 10 s after the change: stays on");
        Check(!d2.Update(0.2, 12000, true), "off once the 10 s dwell is over");
        Check(!Hold(d2, 0.9, 12100, 21900), "high again but the dwell after the off change is not over: stays off");
        Check(d2.Update(0.9, 22000, true), "on again after the dwell");
        // a hard cause: off at once, counted as a change, not held while it lasts
        Check(!d2.Update(0.9, 22100, Eligibility::kHard), "hard cause: off at once");
        Hold(d2, 0.9, 22200, 25900);
        Check(!d2.on, "after the hard off: the 10 s dwell from it (not on at 25.9 s)");
        Check(Hold(d2, 0.9, 26000, 32100), "on after the dwell once the load was held");
        PacingRule e2;
        Check(!Hold(e2, 0.9, 0, 5000, Eligibility::kHard), "hard cause while off: stays off, no hold counted");
        Hold(e2, 0.9, 5100, 6900);
        Check(!e2.on && e2.Update(0.9, 7100, true), "the hold starts when it becomes eligible, and is not a change while off");
        // a transient cause: the state stays for 1 s, then it counts as a hard one
        PacingRule x;
        Hold(x, 0.9, 0, 2000);              // on
        Check(Hold(x, 0.9, 11000, 11900, Eligibility::kTransient), "a transient cause under 1 s: stays on");
        Check(Hold(x, 0.9, 12000, 12000), "an eligible update ends the run");
        Check(Hold(x, 0.9, 12100, 12900, Eligibility::kTransient), "a second run starts from zero: stays on");
        Check(!Hold(x, 0.9, 13000, 13100, Eligibility::kTransient), "a transient cause for 1 s without a break: off like a hard one");
        PacingRule y;
        Hold(y, 0.9, 0, 2000);
        Hold(y, 0.9, 5000, 5400, Eligibility::kTransient);
        Hold(y, 0.9, 5500, 5500);
        Hold(y, 0.9, 5600, 6400, Eligibility::kTransient);
        Check(y.on, "transient runs broken by an eligible frame do not add up");
        // a transient cause resets what was being held, and counts as a change only once it counts
        PacingRule z;
        Hold(z, 0.9, 0, 1500);
        Hold(z, 0.9, 1600, 1700, Eligibility::kTransient);
        Check(!Hold(z, 0.9, 1800, 3600), "a transient cause restarts the enter hold");
        Check(z.Update(0.9, 3800, true), "and then it enters 2 s after it");
        // a gap of over 0.5 s between updates restarts the holds
        PacingRule k;
        Hold(k, 0.9, 0, 1900);
        Check(!k.Update(0.9, 3000, true), "gap of 1.1 s: the hold starts over, not on at 3 s");
        Check(!Hold(k, 0.9, 3100, 4900), "still held only from 3 s");
        Check(k.Update(0.9, 5000, true), "on 2 s after the restart");
        PacingRule k2;
        Hold(k2, 0.9, 0, 2000);             // on
        Hold(k2, 0.3, 2100, 11500);
        k2.Update(0.3, 12500, true);        // a gap: the leave hold restarts from here
        Check(k2.on, "after a gap the leave hold has to be counted again");
        Check(Hold(k2, 0.3, 12600, 14400), "held 1.9 s since the gap: stays on");
        Check(!k2.Update(0.3, 14500, true), "off 2 s after the gap");
        // the load of one frame is clamped to 3, and an unusable interval gives -1
        Check(LoadShare(5000, 10) == 3.0 && LoadShare(20, 10) == 2.0 && LoadShare(5, 10) == 0.5, "load share: ms over the interval, at most 3");
        Check(LoadShare(-4, 10) == 0.0 && LoadShare(10, 0) < 0 && LoadShare(10, 0.4) < 0, "load share: never negative, -1 without an interval");
    }
    // PacingMachine: the order of the actions for each path.
    {
        PacingMachine m;
        uint64_t f = 0;
        PacingActions a = m.Step(false, f++);
        Check(!a.raiseExport && !a.signalsOn && !a.signalsOff && !a.sendOne && !a.lowerExport && m.state == PacingState::kOff, "off, nothing wanted: nothing to do");
        a = m.Step(true, f++);
        Check(a.raiseExport && !a.signalsOn && !a.sendOne && m.state == PacingState::kArming, "off -> arming: the export goes up first, no signals");
        for (uint64_t i = 1; i < kPacingSettleFrames; ++i)
        {
            a = m.Step(true, f++);
            Check(!a.signalsOn && !a.raiseExport && m.state == PacingState::kArming, "arming: the signals wait for the export to be read");
        }
        a = m.Step(true, f++);
        Check(a.signalsOn && !a.raiseExport && !a.lowerExport && m.state == PacingState::kOn, "arming -> on after 30 frames: signals on");
        a = m.Step(true, f++);
        Check(!a.signalsOn && !a.signalsOff && !a.sendOne && m.state == PacingState::kOn, "on, still wanted: nothing to do");
        a = m.Step(false, f++);
        Check(a.signalsOff && a.sendOne && !a.lowerExport && m.state == PacingState::kDisarming, "on -> disarming: signals off and the 1 sent at once, the export stays");
        m.SendResult(true);
        for (uint64_t i = 1; i < kPacingSettleFrames; ++i)
        {
            a = m.Step(false, f++);
            Check(!a.lowerExport && !a.sendOne && m.state == PacingState::kDisarming, "disarming: the export stays up for 30 frames");
        }
        a = m.Step(false, f++);
        Check(a.lowerExport && !a.sendOne && !a.gaveUp && m.state == PacingState::kOff, "disarming -> off: the export comes down after the 30 frames");
        // cancelled arming: only the export comes down, nothing was sent
        PacingMachine c;
        f = 0;
        c.Step(true, f++);
        c.Step(true, f++);
        a = c.Step(false, f++);
        Check(a.lowerExport && !a.sendOne && !a.signalsOff && !a.signalsOn && c.state == PacingState::kOff, "cancelled arming: the export comes down, nothing else");
        // disarming back to on: the export is still up, the signals start again, no lowering
        PacingMachine d;
        f = 0;
        d.Step(true, f++);
        for (uint64_t i = 0; i < kPacingSettleFrames; ++i)
            d.Step(true, f++);
        d.Step(false, f++);
        d.SendResult(true);
        d.Step(false, f++);
        a = d.Step(true, f++);
        Check(a.signalsOn && !a.raiseExport && !a.lowerExport && d.state == PacingState::kOn, "disarming -> on: signals on again, the export untouched");
        // a refused 1: sent again every frame, the export stays up until it is taken
        PacingMachine r;
        f = 0;
        r.Step(true, f++);
        for (uint64_t i = 0; i < kPacingSettleFrames; ++i)
            r.Step(true, f++);
        a = r.Step(false, f++);
        Check(a.sendOne && a.signalsOff, "refusal path: the 1 is sent");
        r.SendResult(false);
        for (int i = 0; i < 40; ++i)
        {
            a = r.Step(false, f++);
            Check(a.sendOne && !a.lowerExport && r.state == PacingState::kDisarming, "refused 1: sent again, the export stays up past the 30 frames");
            r.SendResult(false);
        }
        r.SendResult(true);
        a = r.Step(false, f++);
        Check(a.lowerExport && !a.sendOne && !a.gaveUp && r.state == PacingState::kOff, "the 1 taken after the 30 frames: the export comes down");
        // refused for good: after the give-up count the export comes down anyway
        PacingMachine g;
        f = 0;
        g.Step(true, f++);
        for (uint64_t i = 0; i < kPacingSettleFrames; ++i)
            g.Step(true, f++);
        a = g.Step(false, f++);
        g.SendResult(false);
        bool gave = false;
        for (uint64_t i = 0; i < kPacingGiveUpRefusals + 5 && !gave; ++i)
        {
            a = g.Step(false, f++);
            gave = a.gaveUp;
            if (a.sendOne)
                g.SendResult(false);
        }
        Check(gave && a.lowerExport && !a.sendOne && g.state == PacingState::kOff && g.refused == kPacingGiveUpRefusals, "120 refusals: the export comes down anyway, said plainly (gaveUp)");
        // a want that returns while the 1 is still refused: back on, nothing pending
        PacingMachine w;
        f = 0;
        w.Step(true, f++);
        for (uint64_t i = 0; i < kPacingSettleFrames; ++i)
            w.Step(true, f++);
        w.Step(false, f++);
        w.SendResult(false);
        a = w.Step(true, f++);
        Check(a.signalsOn && !w.sendPending && w.state == PacingState::kOn, "disarming with a refused 1, want returns: on again");
    }
    // TokenDebt: misses add, a taken token repays one, never negative, a timeout forgives.
    {
        TokenDebt d;
        Check(!d.Owes() && !d.Repay(true), "no debt: a token repays nothing");
        d.Missed();
        d.Missed();
        d.Missed();
        Check(d.owed == 3 && d.Owes(), "three presents without a token owe three");
        Check(!d.Repay(false) && d.owed == 3, "no token came: still owed");
        Check(d.Repay(true) && d.owed == 2, "a token repays one");
        while (d.Owes() && d.Repay(true))
        {
        }
        Check(d.owed == 0 && !d.Repay(true) && d.owed == 0, "paid back, and never below zero");
        d.Missed();
        d.Missed();
        d.Forgive();
        Check(d.owed == 0 && !d.Owes(), "a timeout forgives it all");
        // the extra blocking wait of a present outside a burst: a token repays one and the own wait follows; a timeout forgives everything
        // and the own wait is skipped (so no debt of its own: 100 ms a present, not 200)
        TokenDebt p;
        p.Missed();
        p.Missed();
        Check(p.PaidBack(true) && p.owed == 1, "extra wait got a token: one repaid, the own wait follows");
        Check(!p.PaidBack(false) && p.owed == 0, "extra wait timed out: forgiven, the own wait is skipped");
        Check(!p.Owes(), "after the timeout nothing is owed and no miss was added");
    }
    Check(kIdSetReflexSync == 0xB9F6FAFFu && kIdSetFlipConfig == 0xF3148C42u, "nvapi ids are the public ones");
    if (failures == 0)
        std::printf("oob_driver_test PASS\n");
    return failures == 0 ? 0 : 1;
}
