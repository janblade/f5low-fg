// unlock/UnlockXeFg.h - the XeFG unlock family (FamilyXeFG.h: U1-U5 on libxess_fg, XL1 on libxell) applied in memory to f5low's PRIVATE
// copies of Intel's libxess_fg.dll and libxell.dll. This patches inside Intel's DLLs in memory (personal work, user decision
// 2026-10-16); nothing is written to disk, and a module that is not one of the two handles passed in is never touched.
//
// Order matters (evidence 2026-11-03): Intel sizes every per-generated-frame buffer from U3 (the default interpolation ceiling) when
// the context / model is CREATED, so Apply must run right after the private copy is loaded and before the first xefgSwapChain* call
// on it. All or none: nothing is written unless all six sites were found uniquely with the bytes expected; an unknown build is
// refused with the reason (which site, in which state).
#pragma once

#include "FamilyXeFG.h"
#include "Unlock.h"

namespace unlock::xefg
{
// XeFG's own ceiling: the bytes at the patched sites hold it, a count below would lower XeFG under its stock behaviour.
inline constexpr uint8_t kOriginalMax = 3;

struct Result
{
    bool ok = false;        // the six sites were patched
    uint8_t count = 0;      // interpolated frames the unlock allows (5 = 6x)
    std::string reason;     // when !ok: why (the first site that did not locate, or what was missing)
};

// fg: libxess_fg.dll, ll: libxell.dll (both mapped modules of OURS). count 1..5 (clamped to kOriginalMax..5).
inline Result Apply(HMODULE fg, HMODULE ll, int count)
{
    Result res;
    const auto fail = [&](std::string why) {
        res.reason = std::move(why);
        detail::Log("unlock: XeFG: not applied - %s", res.reason.c_str());
        return res;
    };
    if (!fg)
        return fail("no libxess_fg module");
    if (!ll)
        return fail("no libxell module (the XeLL gate cannot be patched)");
    const uint8_t n = static_cast<uint8_t>(std::clamp(count, static_cast<int>(kOriginalMax), 5));
    const MfgUnlock::ImageView fgImage(fg, detail::GetMappedImageSize(fg));
    const MfgUnlock::ImageView llImage(ll, detail::GetMappedImageSize(ll));
    MfgUnlock::FamilyXeFG::Report report;
    std::vector<MfgUnlock::PatchOperation> ops;
    if (!detail::Guarded([&] {
            report = MfgUnlock::FamilyXeFG::Locate(fgImage, llImage, n);
            if (report.Complete())
                ops = MfgUnlock::FamilyXeFG::BuildOperations(report, fgImage, llImage);
        }))
        return fail("provider memory could not be read while locating");
    for (const auto& p : report.patches)
        detail::Log("unlock: XeFG: %s (%s): %s", p.name.c_str(), p.module == MfgUnlock::FamilyXeFG::Module::XeFG ? "libxess_fg" : "libxell",
                    std::string(MfgUnlock::FamilyXeFG::LocateStateName(p.state)).c_str());
    if (!report.Complete())
        return fail("this libxess_fg / libxell build is not recognised by the unlock: " + report.failure);
    if (ops.size() != report.patches.size())
        return fail("could not build the patch transaction");
    const MfgUnlock::TransactionResult t = MfgUnlock::ApplyTransaction(ops);
    if (!t.applied)
    {
        std::string at = t.failedIndexValid && t.failedIndex < report.patches.size() ? report.patches[t.failedIndex].name : "page protection";
        return fail("patch transaction failed at " + at + (t.rollbackComplete ? "" : " (rollback incomplete)"));
    }
    res.ok = true;
    res.count = n;
    detail::Log("unlock: XeFG: applied, up to %u interpolated frames (%ux), in memory in our private libxess_fg / libxell", n, n + 1u);
    return res;
}
} // namespace unlock::xefg
