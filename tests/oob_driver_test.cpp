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
        const DriverSettings a = ResolveDriver(false, true, false, 14, false);
        Check(a.driverFg && a.parts == 15 && a.flipBurst && !a.pacingIgnored && !a.burstIgnored, "DriverPacing sets the signals (all parts) and the burst");
        const DriverSettings b = ResolveDriver(true, true, false, 14, false);
        Check(!b.driverFg && b.parts == 14 && !b.flipBurst && b.pacingIgnored, "Gate=1 vetoes DriverPacing");
        const DriverSettings c = ResolveDriver(true, false, true, 3, true);
        Check(c.driverFg && c.parts == 3 && !c.flipBurst && c.burstIgnored && !c.pacingIgnored, "Gate=1 vetoes the burst, keeps the low-level signals");
        const DriverSettings d = ResolveDriver(false, false, false, 15, false);
        Check(!d.driverFg && d.parts == 15 && !d.flipBurst && !d.pacingIgnored && !d.burstIgnored, "nothing asked, nothing changes");
        const DriverSettings e = ResolveDriver(false, false, true, 6, true);
        Check(e.driverFg && e.parts == 6 && e.flipBurst, "the low-level keys still work with DriverPacing=0");
    }
    Check(kIdSetReflexSync == 0xB9F6FAFFu && kIdSetFlipConfig == 0xF3148C42u, "nvapi ids are the public ones");
    if (failures == 0)
        std::printf("oob_driver_test PASS\n");
    return failures == 0 ? 0 : 1;
}
