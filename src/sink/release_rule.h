// release_rule.h - how the present sink releases one queued frame: paced (burst spacing or the schedule) or on arrival. Pure, no Windows API
// (so tests/release_rule_test.cpp runs on the host).
//
// Two things send a frame out on arrival (after its staging copy; only the wait for its time is skipped):
//  - SinkPace=off (F5LOW_SINK_PACE=off): the pass-through mode for A/B. No burst spacing, no schedule, no stamps. On RTX 40/50 DLSS-G meters
//    its own flips through the driver, so with `off` the display hardware paces them and our timer is not a second pacer on the chain.
//  - A present that the display paces: sync interval > 0 (V-Sync) or flags other than tearing. The own generator (xefg_own.h, DisplayPaced)
//    stands aside the same way: "no clock, no spread, the gate holds nothing and the driver meters nothing; the frames go out as soon as
//    they are done". The sink mirrors that predicate here (keep the two in step). Re-timing such frames on top of V-Sync makes two pacers
//    on one chain (RDR2: OptiScaler forces V-Sync, sync interval 1).
#pragma once
#include <cstdint>

namespace xsink
{
// How a sink releases a burst (docs/GENERATORS.md, "intel"): by the burst interval it measured, on the own generator's schedule, the
// schedule where it is not worse than the burst spacing (per burst size, kAutoMaxBurst; the choice is logged), or Off: no pacing at all.
enum class PaceMode { Burst, Schedule, Auto, Off };

inline constexpr uint32_t kPresentAllowTearing = 0x00000200u;     // DXGI_PRESENT_ALLOW_TEARING

// Mirror of xefg_own.h DisplayPaced (Job::DisplayPaced): V-Sync, or any flag other than tearing
inline constexpr bool DisplayPaced(uint32_t sync, uint32_t flags)
{
    return sync != 0 || (flags & ~kPresentAllowTearing) != 0;
}

enum class Release
{
    Paced,              // the burst spacing or the schedule times it
    OnArrivalOff,       // SinkPace=off
    OnArrivalDisplay,   // the display paces this present (V-Sync / non-tearing flags)
};
inline constexpr Release ReleaseRule(uint32_t sync, uint32_t flags, PaceMode mode)
{
    if (mode == PaceMode::Off)
        return Release::OnArrivalOff;
    return DisplayPaced(sync, flags) ? Release::OnArrivalDisplay : Release::Paced;
}
} // namespace xsink
