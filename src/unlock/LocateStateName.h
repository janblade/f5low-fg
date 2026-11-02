// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/FamilyXeFG.h at 745e49431 (origin/release/0.1.36): only LocateStateName.
// FamilyXeFG.h (the XeFG family, ported next to it) and FamilyRtx40.h both use this one function, through the FamilyXeFG namespace.
#pragma once

#include "Locator.h"

#include <string_view>

namespace MfgUnlock::FamilyXeFG
{
inline std::string_view LocateStateName(LocateState state)
{
    switch (state)
    {
    case LocateState::Found:
        return "found";
    case LocateState::Missing:
        return "missing";
    case LocateState::Ambiguous:
        return "ambiguous";
    case LocateState::AnchorFailed:
        return "anchor failed";
    case LocateState::NotExecutable:
        return "not executable";
    case LocateState::InvalidImage:
        return "invalid image";
    }
    return "unknown";
}
} // namespace MfgUnlock::FamilyXeFG
