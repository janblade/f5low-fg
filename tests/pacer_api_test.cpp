// pacer_api_test - the host-agnostic pacer (src/pace_api.h, exported as include/f5low_pacer.h) on synthetic timelines, no GPU, no DLL needed.
//
// 1. For each timeline (steady 60 fps, +-20% jitter, a 100 ms stall, a 30 -> 90 fps change, a GPU-late generated frame) at 2X..6X the
//    pacer's targets are compared with a model of the D3D12 presenter that calls the same pacesched / pacelead functions the way xefg_own.h
//    does (Pick, Submit, Show, PresentReal's tail): the two must agree to the tick, for every frame of every batch.
// 2. Properties of those targets: monotonic, evenly spaced in steady state, and no generated frame later than the next real frame (steady).
// 3. The flip metering decision (RTX 50 = on, older = off, the settings override) and the metering / burst plan on a metered GPU.
// 4. With a DLL argument: the same through the exported C functions.
//
// Usage: pacer_api_test.exe [f5low.dll]   Exit code: 0 PASS, 1 FAIL.

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <string>
#include <vector>

#include "pace_api.h"

static int g_fail;
#define CHECK(cond, ...)                                                                                              \
    do                                                                                                                \
    {                                                                                                                 \
        if (!(cond))                                                                                                  \
        {                                                                                                             \
            if (++g_fail <= 40)                                                                                       \
            {                                                                                                         \
                printf("FAIL: " __VA_ARGS__);                                                                         \
                printf("\n");                                                                                         \
            }                                                                                                         \
        }                                                                                                             \
    } while (0)

static constexpr double kFreq = 10000000.0;     // 10 MHz, like QPC
static int64_t Ticks(double ms) { return static_cast<int64_t>(ms * kFreq / 1000.0); }
static double Ms(int64_t t) { return static_cast<double>(t) * 1000.0 / kFreq; }

// The D3D12 presenter's pacing as xefg_own.h runs it, written against the shared functions only.
struct Presenter
{
    pacesched::ReadyInterval ri;
    pacelead::Lead lead;
    int64_t lastReady = 0, schedBase = 0, lastAny = 0;
    struct Batch
    {
        int64_t base = 0, step = 0, leadT = 0;
        uint32_t n = 0;
    };
    Presenter(bool leadAuto, double leadFixed)
    {
        lead.automatic = leadAuto;
        lead.fixedMs = leadFixed;
    }
    Batch Pick(int64_t ready, uint32_t n, bool displayPaced)       // Pick + Submit
    {
        if (lastReady)
            ri.Observe(pacesched::ReadyMs(ready, lastReady, kFreq));
        lastReady = ready;
        schedBase = pacesched::NextScheduleBase(schedBase, ready, 0.0, ri.ms, kFreq);
        Batch b;
        b.n = n;
        b.base = schedBase;
        b.step = pacesched::StepTicks(ri.ms, n, kFreq);
        if (n && !displayPaced && lead.On())
            b.leadT = static_cast<int64_t>(lead.Freeze(Ms(b.step)) * kFreq / 1000.0);
        return b;
    }
    int64_t Target(const Batch& b, uint32_t k) const { return b.n ? pacesched::BatchTarget(b.base, b.leadT, b.step, k, lastAny) : 0; }
    void Presented(const Batch& b, uint32_t k, int64_t t, int64_t gpuDone)
    {
        const int64_t target = Target(b, k);
        if (b.n && k <= b.n && b.leadT > 0 && gpuDone > 0 && target)
            lead.Late(std::min(std::max(0.0, Ms(gpuDone - target)), std::max(0.0, Ms(t - target))));
        lastAny = t;
    }
    void Done(const Batch& b)
    {
        if (b.leadT > 0)
            lead.EndFrame(Ms(b.leadT), Ms(b.step));
    }
};

struct Timeline
{
    const char* name;
    std::vector<double> ready;      // ms, the app frames' GPU-ready times
    bool steady;                    // steady state checks apply
    double lateGpuMs;               // > 0: every 7th generated frame's GPU work is done this late past its target
};

static std::vector<double> Make(const char* kind, int frames, uint32_t seed)
{
    std::vector<double> r;
    double t = 1000.0;
    uint32_t x = seed;
    auto rnd = [&] { x = x * 1664525u + 1013904223u; return (x >> 8) / 16777216.0; };
    for (int i = 0; i < frames; ++i)
    {
        double dt = 1000.0 / 60.0;
        if (!strcmp(kind, "jitter"))
            dt *= 1.0 + 0.4 * (rnd() - 0.5);
        else if (!strcmp(kind, "stall"))
            dt = i == frames / 2 ? 100.0 : dt;
        else if (!strcmp(kind, "change"))
            dt = i < frames / 2 ? 1000.0 / 30.0 : 1000.0 / 90.0;
        t += dt;
        r.push_back(t);
    }
    return r;
}

static pacecore::Config Cfg(int sm, uint32_t maxGen)
{
    pacecore::Config c;
    c.ticksPerSecond = kFreq;
    c.gpuSm = sm;
    c.maxGenerated = maxGen;
    return c;
}

int main(int argc, char** argv)
{
    const Timeline tls[] = {
        { "steady 60", Make("steady", 400, 1), true, 0 },
        { "jitter +-20%", Make("jitter", 400, 7), false, 0 },
        { "100 ms stall", Make("stall", 400, 1), false, 0 },
        { "30 -> 90 fps", Make("change", 400, 1), false, 0 },
        { "GPU late", Make("steady", 400, 1), false, 2.5 },
    };
    for (const Timeline& tl : tls)
        for (uint32_t gen = 1; gen <= 5; ++gen)
        {
            // an RTX 40 (no metering): the lead schedule; the metered GPU is checked below
            pacecore::Pacer pacer(Cfg(89, 5));
            Presenter ref(true, 0);
            int64_t prevPresent = 0;
            int64_t maxOvershoot = 0;
            std::vector<int64_t> allPresents;
            uint32_t lateSeq = 0;
            for (size_t i = 0; i < tl.ready.size(); ++i)
            {
                const int64_t ready = Ticks(tl.ready[i]);
                F5LowPacerFrame f {};
                f.arriveTicks = ready - Ticks(1.0);
                f.readyTicks = ready;
                f.generated = gen;
                F5LowPacerBatch b {};
                pacer.FrameArrived(f, b);
                const Presenter::Batch rb = ref.Pick(ready, gen, false);
                CHECK(b.generated == gen && b.baseTicks == rb.base && b.stepTicks == rb.step && b.leadTicks == rb.leadT,
                      "%s %ux: batch plan differs (base %lld/%lld step %lld/%lld lead %lld/%lld)", tl.name, gen + 1, (long long)b.baseTicks, (long long)rb.base,
                      (long long)b.stepTicks, (long long)rb.step, (long long)b.leadTicks, (long long)rb.leadT);
                int64_t prevTarget = 0;
                for (uint32_t k = 1; k <= gen + 1; ++k)
                {
                    F5LowPacerTarget t {};
                    pacer.Target(b, k, t);
                    const int64_t want = ref.Target(rb, k);
                    CHECK(t.targetTicks == want, "%s %ux frame %zu k %u: target %lld, the presenter model says %lld", tl.name, gen + 1, i, k, (long long)t.targetTicks,
                          (long long)want);
                    CHECK(!(t.flags & (F5LOW_PACER_TARGET_BURST | F5LOW_PACER_TARGET_METERED)), "%s: flags 0x%x on a GPU without flip metering", tl.name, t.flags);
                    CHECK(t.releaseTicks == t.targetTicks, "%s: release %lld is not the target %lld (no flip lead without metering)", tl.name, (long long)t.releaseTicks, (long long)t.targetTicks);
                    CHECK(t.targetTicks >= prevTarget, "%s %ux frame %zu: target %u goes back (%lld < %lld)", tl.name, gen + 1, i, k, (long long)t.targetTicks, (long long)prevTarget);
                    prevTarget = t.targetTicks;
                    // the host sleeps to the release time, then presents once the frame's work is done
                    int64_t at = std::max(t.releaseTicks, ready);
                    int64_t gpuDone = 0;
                    if (tl.lateGpuMs > 0 && k <= gen && (++lateSeq % 7) == 0)
                    {
                        gpuDone = t.targetTicks + Ticks(tl.lateGpuMs);
                        at = std::max(at, gpuDone);
                    }
                    at = std::max(at, prevPresent + 1);
                    F5LowPacerPresented w {};
                    w.presentTicks = at;
                    w.gpuDoneTicks = gpuDone;
                    pacer.Presented(b, k, w);
                    ref.Presented(rb, k, at, gpuDone);
                    prevPresent = at;
                    allPresents.push_back(at);
                    if (k == gen + 1 && i + 1 < tl.ready.size() && (!tl.steady || i > tl.ready.size() / 2))   // steady: after the interval estimate settled
                        maxOvershoot = std::max<int64_t>(maxOvershoot, t.targetTicks - Ticks(tl.ready[i + 1]));
                }
                pacer.BatchDone(b);
                ref.Done(rb);
            }
            // steady state: evenly spaced (every gap within 15% of the step) over the second half; no real frame later than the next game frame
            if (tl.steady)
            {
                const double step = 1000.0 / 60.0 / (gen + 1);
                double worst = 0;
                for (size_t i = allPresents.size() / 2; i + 1 < allPresents.size(); ++i)
                    worst = std::max(worst, std::fabs(Ms(allPresents[i + 1] - allPresents[i]) - step) / step);
                CHECK(worst < 0.15, "%s %ux: the gaps between presents are up to %.0f%% off the step %.2f ms", tl.name, gen + 1, worst * 100, step);
                CHECK(maxOvershoot <= Ticks(0.05), "%s %ux: the real frame is %.2f ms later than the next game frame", tl.name, gen + 1, Ms(maxOvershoot));
                if (gen == 1 || gen == 5)
                    printf("  %-14s %ux: worst gap error %.1f%% of the %.2f ms step, real frame vs next game frame %+.2f ms\n", tl.name, gen + 1, worst * 100, step, Ms(maxOvershoot));
            }
            else if (gen == 1 || gen == 5)
                printf("  %-14s %ux: matches the presenter model on %zu presents (real frame vs next game frame, worst %+.2f ms)\n", tl.name, gen + 1, allPresents.size(),
                       Ms(maxOvershoot));
            // monotonic presents
            for (size_t i = 1; i < allPresents.size(); ++i)
                CHECK(allPresents[i] > allPresents[i - 1], "%s %ux: presents not increasing at %zu", tl.name, gen + 1, i);
        }

    // the 30 -> 90 fps change settles on the new step: the last batch's step is 1/90 s over n + 1
    {
        pacecore::Pacer p(Cfg(89, 5));
        const std::vector<double> r = Make("change", 400, 1);
        F5LowPacerBatch b {};
        for (double ms : r)
        {
            F5LowPacerFrame f {};
            f.arriveTicks = f.readyTicks = Ticks(ms);
            f.generated = 3;
            p.FrameArrived(f, b);
            for (uint32_t k = 1; k <= 4; ++k)
            {
                F5LowPacerTarget t {};
                p.Target(b, k, t);
                F5LowPacerPresented w {};
                w.presentTicks = std::max(t.targetTicks, Ticks(ms)) + k;
                p.Presented(b, k, w);
            }
            p.BatchDone(b);
        }
        const double stepMs = Ms(b.stepTicks), want = 1000.0 / 90.0 / 4;
        CHECK(std::fabs(stepMs - want) < 0.1, "30 -> 90 fps: the last step is %.2f ms, expected %.2f", stepMs, want);
        printf("  30 -> 90 fps: last step %.2f ms (expected %.2f)\n", stepMs, want);
    }

    // the flip metering decision and the metered plan
    {
        pacecore::Config c = Cfg(120, 5);
        pacecore::Pacer rtx50(c);
        pacecore::Pacer rtx40(Cfg(89, 5));
        c.flipMetering = "0";
        pacecore::Pacer off(c);
        c = Cfg(75, 5);
        c.flipMetering = "1";
        pacecore::Pacer forced(c);
        F5LowPacerInfo i50, i40, ioff, iforced;
        rtx50.Info(i50);
        rtx40.Info(i40);
        off.Info(ioff);
        forced.Info(iforced);
        CHECK(i50.flipMetering == 1 && i40.flipMetering == 0 && ioff.flipMetering == 0 && iforced.flipMetering == 1,
              "flip metering decision: sm_120 %u, sm_89 %u, sm_120 with FlipMetering=0 %u, sm_75 with FlipMetering=1 %u", i50.flipMetering, i40.flipMetering,
              ioff.flipMetering, iforced.flipMetering);
        printf("  flip metering: sm_120 auto %u, sm_89 auto %u, sm_120 FlipMetering=0 %u, sm_75 FlipMetering=1 %u\n", i50.flipMetering, i40.flipMetering, ioff.flipMetering,
               iforced.flipMetering);
        // metered plan on RTX 50: after kFlipSamples frames a 3x batch is metered (4 frames per batch, the batch time), V-Sync is not
        int metered = 0, wanted0 = 0;
        F5LowPacerBatch b {};
        double t = 1000;
        for (int i = 0; i < 60; ++i)
        {
            t += 1000.0 / 60.0;
            F5LowPacerFrame f {};
            f.arriveTicks = f.readyTicks = Ticks(t);
            f.generated = 3;
            rtx50.FrameArrived(f, b);
            wanted0 += i < 20 && b.meterWanted;
            metered += b.meterWanted != 0;
            F5LowPacerTarget tg {};
            rtx50.Target(b, 2, tg);
            if (i >= 40)
            {
                CHECK(b.meterWanted && b.numFramesPerBatch == 4 && std::fabs(b.batchMs - 1000.0 / 60.0) < 0.5, "metered batch: wanted %u, %u frames, %.2f ms", b.meterWanted,
                      b.numFramesPerBatch, b.batchMs);
                CHECK(tg.flags & F5LOW_PACER_TARGET_METERED, "metered batch: target flags 0x%x", tg.flags);
                CHECK(tg.releaseTicks < tg.targetTicks || (tg.flags & F5LOW_PACER_TARGET_BURST), "metered batch: frame 2 is not released early (release %lld target %lld)",
                      (long long)tg.releaseTicks, (long long)tg.targetTicks);
            }
            for (uint32_t k = 1; k <= 4; ++k)
            {
                F5LowPacerPresented w {};
                w.presentTicks = Ticks(t) + k * 1000;
                rtx50.Presented(b, k, w);
            }
            rtx50.BatchDone(b);
        }
        CHECK(wanted0 == 0, "metering asked for in the first 20 frames (%d), before the frame time is known", wanted0);
        printf("  RTX 50 3x: metering asked for %d of 60 batches (none in the first 20); 4 frames per batch, %.2f ms\n", metered, b.batchMs);
        // refused 8 times in a row: off
        for (int i = 0; i < 8; ++i)
        {
            F5LowPacerFrame f {};
            t += 1000.0 / 60.0;
            f.arriveTicks = f.readyTicks = Ticks(t);
            f.generated = 3;
            rtx50.FrameArrived(f, b);
            rtx50.MeterResult(b, -1, 0);
        }
        F5LowPacerInfo ir;
        rtx50.Info(ir);
        CHECK(ir.meteringRefused == 1 && ir.flipMetering == 0, "after 8 refusals: refused %u, metering %u", ir.meteringRefused, ir.flipMetering);
        // V-Sync batch: not metered, no lead, still paced by the plain schedule
        pacecore::Pacer vs(Cfg(120, 5));
        for (int i = 0; i < 40; ++i)
        {
            F5LowPacerFrame f {};
            f.arriveTicks = f.readyTicks = Ticks(1000 + i * 16.6667);
            f.generated = 2;
            f.displayPaced = 1;
            vs.FrameArrived(f, b);
        }
        F5LowPacerTarget tg {};
        vs.Target(b, 1, tg);
        CHECK(!b.meterWanted && b.leadTicks == 0 && (tg.flags & F5LOW_PACER_TARGET_PACED) && tg.releaseTicks == tg.targetTicks, "V-Sync batch: metered %u lead %lld flags 0x%x", b.meterWanted,
              (long long)b.leadTicks, tg.flags);
        // the app's frame alone: present at once
        F5LowPacerFrame f1 {};
        f1.arriveTicks = f1.readyTicks = Ticks(5000);
        vs.FrameArrived(f1, b);
        vs.Target(b, 1, tg);
        CHECK(tg.targetTicks == 0 && tg.releaseTicks == 0, "generated 0: target %lld release %lld, expected 0 (present at once)", (long long)tg.targetTicks, (long long)tg.releaseTicks);
        // stale: only without metering
        pacecore::Pacer s(Cfg(89, 5));
        F5LowPacerFrame f2 {};
        f2.arriveTicks = f2.readyTicks = Ticks(1000);
        f2.generated = 1;
        s.FrameArrived(f2, b);
        F5LowPacerTarget t1 {};
        s.Target(b, 1, t1);
        CHECK(s.Stale(b, 1, t1.targetTicks + b.stepTicks, true) == 1 && s.Stale(b, 1, t1.targetTicks + b.stepTicks, false) == 0 && s.Stale(b, 1, t1.targetTicks, true) == 0 &&
                  s.Stale(b, 2, t1.targetTicks + 10 * b.stepTicks, true) == 0,
              "stale rule");
        rtx50.FrameArrived(f2, b);
        CHECK(rtx50.Stale(b, 1, t1.targetTicks + 10 * b.stepTicks, true) == 0, "stale on a metering GPU");
    }

    // through the DLL's exports
    if (argc > 1)
    {
        HMODULE dll = LoadLibraryA(argv[1]);
        if (!dll)
        {
            printf("cannot load %s\n", argv[1]);
            return 1;
        }
        auto Create = reinterpret_cast<decltype(&f5lowPacerCreate)>(GetProcAddress(dll, "f5lowPacerCreate"));
        auto Destroy = reinterpret_cast<decltype(&f5lowPacerDestroy)>(GetProcAddress(dll, "f5lowPacerDestroy"));
        auto Arrived = reinterpret_cast<decltype(&f5lowPacerFrameArrived)>(GetProcAddress(dll, "f5lowPacerFrameArrived"));
        auto Target = reinterpret_cast<decltype(&f5lowPacerTarget)>(GetProcAddress(dll, "f5lowPacerTarget"));
        auto Presented = reinterpret_cast<decltype(&f5lowPacerPresented)>(GetProcAddress(dll, "f5lowPacerPresented"));
        auto Done = reinterpret_cast<decltype(&f5lowPacerBatchDone)>(GetProcAddress(dll, "f5lowPacerBatchDone"));
        auto Info = reinterpret_cast<decltype(&f5lowPacerGetInfo)>(GetProcAddress(dll, "f5lowPacerGetInfo"));
        auto Now = reinterpret_cast<decltype(&f5lowPacerNowTicks)>(GetProcAddress(dll, "f5lowPacerNowTicks"));
        auto Freq = reinterpret_cast<decltype(&f5lowPacerTicksPerSecond)>(GetProcAddress(dll, "f5lowPacerTicksPerSecond"));
        auto Wait = reinterpret_cast<decltype(&f5lowPacerWaitUntil)>(GetProcAddress(dll, "f5lowPacerWaitUntil"));
        auto Raise = reinterpret_cast<decltype(&f5lowPacerTimerRaise)>(GetProcAddress(dll, "f5lowPacerTimerRaise"));
        if (!Create || !Destroy || !Arrived || !Target || !Presented || !Done || !Info || !Now || !Freq || !Wait || !Raise)
        {
            printf("FAIL: the DLL lacks a f5lowPacer* export\n");
            return 1;
        }
        F5LowPacerCreateInfo ci {};
        ci.apiVersion = F5LOW_PACER_API_VERSION;
        ci.ticksPerSecond = static_cast<int64_t>(kFreq);
        ci.gpuSm = 120;
        ci.maxGenerated = 5;
        F5LowPacer p = nullptr;
        CHECK(Create(&ci, &p) == 0 && p, "f5lowPacerCreate");
        pacecore::Pacer local(Cfg(120, 5));       // same settings (defaults): the export must give the same targets as the header-only class
        F5LowPacerInfo inf {};
        Info(p, &inf);
        CHECK(inf.flipMetering == 1, "exported pacer on sm_120: flipMetering %u", inf.flipMetering);
        const std::vector<double> r = Make("jitter", 300, 3);
        int same = 0, total = 0;
        for (double ms : r)
        {
            F5LowPacerFrame f {};
            f.arriveTicks = f.readyTicks = Ticks(ms);
            f.generated = 3;
            F5LowPacerBatch b1 {}, b2 {};
            Arrived(p, &f, &b1);
            local.FrameArrived(f, b2);
            for (uint32_t k = 1; k <= 4; ++k)
            {
                F5LowPacerTarget t1 {}, t2 {};
                Target(p, &b1, k, &t1);
                local.Target(b2, k, t2);
                ++total;
                same += t1.targetTicks == t2.targetTicks && t1.releaseTicks == t2.releaseTicks && t1.flags == t2.flags;
                F5LowPacerPresented w {};
                w.presentTicks = std::max(t1.releaseTicks, Ticks(ms)) + k;
                Presented(p, &b1, k, &w);
                local.Presented(b2, k, w);
            }
            Done(p, &b1);
            local.BatchDone(b2);
        }
        CHECK(same == total, "exported pacer: %d of %d targets equal the header-only pacer's", same, total);
        printf("  exports: %d of %d targets identical to the header-only pacer (same code, same settings)\n", same, total);
        const int64_t t0 = Now(), want = t0 + Freq() / 100;     // 10 ms
        Wait(want);
        CHECK(Now() >= want && Now() < want + Freq() / 50, "f5lowPacerWaitUntil returned at %lld ticks past the target", (long long)(Now() - want));
        Raise(p, 1);
        Raise(p, 0);
        Destroy(p);
    }

    printf("%s (%d failures)\n", g_fail ? "pacer_api_test: FAIL" : "pacer_api_test: PASS", g_fail);
    return g_fail ? 1 : 0;
}
