// PaceLead (xefg_own.h, Target): the schedule pacer sends generated frame k at base + lead + (k - 1) steps instead of base + k
// steps. Generating takes a millisecond or two, so frame 1 need not wait a whole step. No GPU or Windows in here: the rule is
// tested on the host (tests/pace_lead_test.cpp).
#pragma once
#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>

namespace pacelead
{
constexpr double kStart = 3.0, kFloor = 3.0;    // ms: the auto lead starts at the floor and never goes below it
constexpr double kLateMs = 0.5;                 // a generated frame this far past its time (our own GPU work) is late
constexpr double kMaxRise = 2.0;                // ms of late that count: a late event raises the lead by kMaxRise + kMargin at most
constexpr double kMargin = 1.0;                 // (the game's own jitter is in `late` too: jitter2 2x swung the lead 3 -> 8.2 ms of an 8.3 ms step)
constexpr size_t kWindow = 128;                 // app frames a late frame holds the lead up for

// Frame k's offset from the schedule's base (ticks): lead 0 (off): k steps; else the lead (at most a step) + (k - 1) steps.
inline long long Offset(long long lead, long long step, uint32_t k)
{
    return (lead > 0 ? std::min(step, lead) : step) + static_cast<long long>(k - 1) * step;
}

struct Lead
{
    bool automatic = true;                      // PaceLead=auto; else fixedMs (0: off)
    double fixedMs = 0;
    double ms = 0;                              // the current lead
    double ringMax = kFloor;                    // the window's maximum, floor included (stats)
    bool On() const { return automatic || fixedMs > 0; }

    // The lead for the app frame about to be submitted, frozen for its whole batch (a lead changing in the middle of it would
    // space its frames unevenly), at most the frame's step (stepMs). 0: off.
    double Freeze(double stepMs)
    {
        if (!On())
            return 0;
        ms = automatic ? (ms > 0 ? ms : kStart) : fixedMs;
        return std::min(ms, stepMs);
    }
    // A generated frame's present came late ms after its time.
    void Late(double late)
    {
        if (late > kLateMs)
            frameLate = std::max(frameLate, late);
    }
    // After the app frame's presents (frozenMs: the lead it was frozen with; stepMs: its step). What the lead should have been
    // (0 if no frame was late) goes into a window of the last kWindow frames; the lead follows the window's maximum: up at once,
    // down by 1/16 of the way per app frame. A window, since one late frame must hold the lead for a few seconds: easing down
    // right after it walked straight into the next late frame (harness 4K 2x60: 10 of 120 late). Capped at the step.
    void EndFrame(double frozenMs, double stepMs)
    {
        if (automatic)
        {
            need[at++ % kWindow] = frameLate > 0 ? frozenMs + std::min(frameLate, kMaxRise) + kMargin : 0.0;
            n = std::min(n + 1, kWindow);
            ringMax = kFloor;
            for (size_t i = 0; i < n; ++i)
                ringMax = std::max(ringMax, need[i]);
            const double target = std::min(ringMax, stepMs);
            ms = target > ms ? target : std::max(target, ms - (ms - target) / 16);
        }
        frameLate = 0;
    }

private:
    double frameLate = 0;                       // the worst late of the batch in hand
    std::array<double, kWindow> need {};
    size_t at = 0, n = 0;
};
} // namespace pacelead
