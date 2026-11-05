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
    Check(!BurstNow(false, 1, false), "no burst: everything off");
    Check(kAsyncMarkerVer == (88u | (1u << 16)), "structure version");
    Check(kReflexSyncVer == (48u | (1u << 16)), "reflex sync structure version");
    if (failures == 0)
        std::printf("oob_driver_test PASS\n");
    return failures == 0 ? 0 : 1;
}
