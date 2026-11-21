// pace_api.h - pacecore::Pacer: the host-agnostic pacer behind include/f5low_pacer.h. No GPU or Windows in here (tested on the host,
// tests/pacer_api_test.cpp). It keeps the state the D3D12 presenter keeps for pacing (xefg_own.h: ri, schedBase, leadState, lastAnyPresent,
// clkBatch, the flip metering and driver pacing state) and runs it through the SAME functions (pace_sched.h: ReadyInterval, NextScheduleBase,
// StepTicks, BatchTarget, ObserveBatch, FlipSkip, FlipBatchMs, FlipLeadTicks, AutoStale; pace_lead.h: Lead; oob_driver.h: FlipMeteringOn,
// ResolveDriver, PacingRule, LoadShare, LoadEma, BurstNow), so what a batch's times are is decided in one place.
//
// Mapping to the presenter (xefg_own.h):
//   FrameArrived     = Pick (ReadyInterval, ClockObserve, NextBase, load share, PacingFrame) + Submit's batch plan (step, lead freeze) +
//                      MeterBatch's decision (FlipSkip, FlipBatchMs).
//   Target           = Target(f, k) (Bound(base + Offset(lead, step, k))) + PresentReal's lead / burst / wait rules for the time to release at.
//   Stale            = PresentReal's LateGen=auto stale rule (AutoStale), without flip metering.
//   Presented        = PresentReal's tail (lastAnyPresent = lastShown = t; a dropped frame keeps its slot) + Show's lead lateness (Lead::Late).
//   BatchDone        = Show's Lead::EndFrame.
// The paceLag of the presenter is 0 on D3D12 (it is raised on the CUDA path only), so it is not here.
#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

#include <f5low_pacer.h>

#include "oob_driver.h"
#include "pace_lead.h"
#include "pace_sched.h"
#include "timer_raise.h"

namespace pacecore
{
struct Config
{
    double ticksPerSecond = 1e9;
    int gpuSm = 0;
    uint32_t maxGenerated = 5;
    bool vrr = false;
    double refreshHz = 0;
    // the settings of the D3D12 context (xefg_cuda.cpp reads them the same way)
    const char* flipMetering = "auto";      // FlipMetering= (XEFG_CUDA_FLIPMETERING)
    int driverPacing = -1;                  // DriverPacing=: -1 absent (automatic: the host routes the markers), 0, 1, 2
    bool flipBurst = false;                 // FlipBurst=1
    bool paceLeadAuto = true;               // PaceLead=auto, else paceLeadFixedMs (0: off)
    double paceLeadFixedMs = 0;
    uint32_t flipLeadUs = 2500;             // FlipLeadUs=
    bool flipLeadAuto = true;
    bool fixedDisplay = false;              // FixedPacing=display (default timer: a V-Sync batch is paced by the plain schedule)
};

class Pacer
{
public:
    explicit Pacer(const Config& c) : cfg(c)
    {
        freq = cfg.ticksPerSecond > 0 ? cfg.ticksPerSecond : 1e9;
        metering = oobdriver::FlipMeteringOn(cfg.flipMetering, cfg.gpuSm);
        drv = oobdriver::ResolveDriver(false, cfg.driverPacing, false, 15, cfg.flipBurst, true, cfg.gpuSm > 0, metering);
        pacingActive = !drv.pacingAuto;                 // automatic starts off: the rule switches it
        lead.automatic = cfg.paceLeadAuto;
        lead.fixedMs = cfg.paceLeadFixedMs;
    }

    void FrameArrived(const F5LowPacerFrame& f, F5LowPacerBatch& b)
    {
        std::lock_guard<std::mutex> l(mu);
        const int64_t ready = f.readyTicks ? f.readyTicks : f.arriveTicks;
        const uint32_t n = std::min<uint32_t>(f.generated, std::min(cfg.maxGenerated, F5LOW_PACER_MAX_GENERATED));
        const bool displayPaced = f.displayPaced != 0;
        // Pick
        if (lastReady)
        {
            const double ms = pacesched::ReadyMs(ready, lastReady, freq);
            ri.Observe(ms);
            pacesched::ObserveBatch(clkBatch, clkVar, clkHitchRun, clkHitches, ms);
        }
        lastReady = ready;
        schedBase = pacesched::NextScheduleBase(schedBase, ready, 0.0, ri.ms, freq);
        if (const double share = oobdriver::LoadShare(pacesched::ReadyMs(ready, f.arriveTicks, freq), ri.ms); share >= 0.0)
            loadEma = oobdriver::LoadEma(loadEma, loadSamples, share);
        PacingFrame(f.arriveTicks, displayPaced, n);
        // the batch's plan (Submit): step and the lead frozen for the batch
        b = {};
        b.generated = n;
        b.displayPaced = displayPaced;
        b.baseTicks = schedBase;
        b.stepTicks = pacesched::StepTicks(ri.ms, n, freq);
        if (n && !displayPaced && lead.On())
        {
            const double stepMs = static_cast<double>(b.stepTicks) * 1000.0 / freq;
            b.leadTicks = static_cast<int64_t>(lead.Freeze(stepMs) * freq / 1000.0);
        }
        // MeterBatch's decision
        samples = n ? samples + 1 : 0;
        const bool allowed = metering && !meteringRefused;
        const char* skip = pacesched::FlipSkip(n, displayPaced, meteringRefused, samples, clkBatch);
        if (allowed && !skip)
        {
            b.meterWanted = 1;
            b.numFramesPerBatch = n + 1;
            b.batchMs = pacesched::FlipBatchMs(clkBatch, outputs, outputsBatchMs, n);
            outputs = n + 1;
            outputsBatchMs = b.batchMs;
            b.metered = 1;
            meteredNow = true;
        }
        else
        {
            outputs = 0;
            outputsBatchMs = 0;
            meteredNow = false;
        }
        lastN = n;
        b.serial = ++batches;
        batchLeadMs = static_cast<double>(b.leadTicks) * 1000.0 / freq;
    }

    void Target(const F5LowPacerBatch& b, uint32_t k, F5LowPacerTarget& out)
    {
        std::lock_guard<std::mutex> l(mu);
        out = {};
        const uint32_t n = b.generated;
        if (!n || k < 1 || k > n + 1)
            return;
        out.targetTicks = pacesched::BatchTarget(b.baseTicks, b.leadTicks, b.stepTicks, k, lastAny);
        const bool metered = b.metered != 0;
        const bool burst = oobdriver::BurstNow(Burst(), k, metered);
        const int64_t flipLead = !burst && k > 1 && metered
                                     ? pacesched::FlipLeadTicks(pacesched::StepTicks(ri.ms, n, freq), cfg.flipLeadUs, cfg.flipLeadAuto, freq)
                                     : 0;
        // PresentReal: paced = n > 0 && (!DisplayPaced || (V-Sync and FixedPacing=timer)); the burst is never waited for
        const bool paced = !b.displayPaced || (b.displayPaced && !cfg.fixedDisplay);
        out.flags = (burst ? F5LOW_PACER_TARGET_BURST : 0u) | (metered ? F5LOW_PACER_TARGET_METERED : 0u) | (paced ? F5LOW_PACER_TARGET_PACED : 0u);
        out.releaseTicks = paced && !burst ? out.targetTicks - flipLead : 0;
    }

    int Stale(const F5LowPacerBatch& b, uint32_t k, int64_t now, bool nextDone)
    {
        std::lock_guard<std::mutex> l(mu);
        const uint32_t n = b.generated;
        if (!n || k < 1 || k > n)
            return 0;
        // LateGen=auto: only without flip metering (MeterOff), on a timed batch (not V-Sync, not metered, not the burst)
        if (metering && !meteringRefused)
            return 0;
        if (b.displayPaced || b.metered)
            return 0;
        const int64_t slot = pacesched::BatchTarget(b.baseTicks, b.leadTicks, b.stepTicks, k, lastAny);
        return pacesched::AutoStale(true, nextDone, now, slot, b.stepTicks) ? 1 : 0;
    }

    void MeterResult(F5LowPacerBatch& b, int status, bool noIflip)
    {
        std::lock_guard<std::mutex> l(mu);
        if (!b.meterWanted)
            return;
        if (status == 0)
            fails = 0;
        else if (++fails >= pacesched::kFlipRefusals)
            meteringRefused = true;
        b.metered = status == 0 && !noIflip ? 1 : 0;
        meteredNow = b.metered != 0;
        if (status != 0)
        {
            outputs = 0;
            outputsBatchMs = 0;
        }
    }

    void Presented(const F5LowPacerBatch& b, uint32_t k, const F5LowPacerPresented& w)
    {
        std::lock_guard<std::mutex> l(mu);
        const uint32_t n = b.generated;
        const int64_t target = n && k >= 1 && k <= n + 1 ? pacesched::BatchTarget(b.baseTicks, b.leadTicks, b.stepTicks, k, lastAny) : 0;
        if (w.flags & F5LOW_PACER_PRESENT_DROPPED)
        {
            // a dropped frame's slot stays the schedule's: later frames are not moved earlier (PresentReal's drop paths)
            lastAny = std::max(lastAny, target);
            return;
        }
        // Show: a generated frame whose own GPU work held it past its time makes the lead longer
        if (n && k <= n && b.leadTicks > 0 && w.gpuDoneTicks > 0 && target)
        {
            const double gpuWait = std::max(0.0, static_cast<double>(w.gpuDoneTicks - target) * 1000.0 / freq);
            const double sinceTarget = std::max(0.0, static_cast<double>(w.presentTicks - target) * 1000.0 / freq);
            const double late = std::min(gpuWait, sinceTarget);
            lead.Late(late);
        }
        lastAny = w.presentTicks;
    }

    void BatchDone(const F5LowPacerBatch& b)
    {
        std::lock_guard<std::mutex> l(mu);
        if (b.leadTicks > 0)
            lead.EndFrame(static_cast<double>(b.leadTicks) * 1000.0 / freq, static_cast<double>(b.stepTicks) * 1000.0 / freq);
    }

    void Info(F5LowPacerInfo& o)
    {
        std::lock_guard<std::mutex> l(mu);
        o = {};
        o.flipMetering = metering && !meteringRefused;
        o.burstCapable = drv.flipBurst;
        o.burstNow = Burst();
        o.pacingMode = drv.mode;
        o.meteringRefused = meteringRefused;
        o.intervalMs = ri.ms;
        o.batchTimeMs = clkBatch;
        o.leadMs = lead.ms;
        o.loadShare = loadEma;
        o.batches = batches;
    }

    bool Metering() const { return metering; }
    std::mutex timerMu;                     // for pacewait::RaiseTimer
    timerraise::Hold timerHold;

private:
    // The burst (xefg_own.h Burst): capable, the pacing active, and for DriverPacing=2 the load rule's want.
    bool Burst() const { return drv.flipBurst && pacingActive && (drv.mode != 2 || drv.pacingAuto || burstWanted); }
    // PacingFrame: the load rule decides the automatic mode (1: pacingActive) or the burst alone (2). No arming dwell: that is for the cap export
    // and Reflex Sync of the D3D12 context.
    void PacingFrame(int64_t arrive, bool displayPaced, uint32_t n)
    {
        if (!drv.flipBurst)
            return;
        const bool hard = !metering || meteringRefused || displayPaced || !n;
        const bool transient = !meteredNow || lastN == ~0u || lastN == 0;
        const oobdriver::Eligibility kind = hard ? oobdriver::Eligibility::kHard : transient ? oobdriver::Eligibility::kTransient : oobdriver::Eligibility::kEligible;
        const double nowMs = static_cast<double>(arrive) * 1000.0 / freq;
        if (drv.mode == 2 && !drv.pacingAuto)
            burstWanted = burstRule.Update(loadEma, nowMs, kind);
        else if (drv.pacingAuto)
            pacingActive = pacingRule.Update(loadEma, nowMs, kind);
    }

    Config cfg;
    double freq = 1e9;
    bool metering = false;
    oobdriver::DriverSettings drv {};
    std::mutex mu;
    pacesched::ReadyInterval ri;
    pacelead::Lead lead;
    int64_t lastReady = 0, schedBase = 0, lastAny = 0;
    double clkBatch = 0, clkVar = 0;
    unsigned clkHitchRun = 0, clkHitches = 0;
    double loadEma = 0;
    unsigned loadSamples = 0;
    unsigned samples = 0, fails = 0;
    uint32_t outputs = 0, lastN = ~0u;
    double outputsBatchMs = 0, batchLeadMs = 0;
    bool meteringRefused = false, meteredNow = false;
    bool pacingActive = true, burstWanted = false;
    oobdriver::PacingRule pacingRule, burstRule;
    uint64_t batches = 0;
};
} // namespace pacecore
