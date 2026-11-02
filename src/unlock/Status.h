// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/Status.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. 
#pragma once

#include <array>
#include <cstddef>
#include <format>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace MfgUnlock
{
enum class Family : size_t
{
    XeFG,
    RTX40,
    RTX2030,
    Count
};

enum class FamilyState
{
    Off,
    Waiting,
    Active,
    StoodAside,
    NotNeeded
};

struct PatchStatus
{
    std::string name;
    std::string result;
};

struct FamilyStatus
{
    FamilyState state = FamilyState::Off;
    std::wstring modulePath;
    std::string version;
    std::string sha256;
    std::vector<PatchStatus> patches;
    std::string failure;
};

inline constexpr std::array<std::string_view, static_cast<size_t>(Family::Count)> kFamilyNames = { "XeFG", "RTX 40",
                                                                                                   "RTX 20/30" };

class StatusRegistry
{
  public:
    static void Set(Family family, FamilyStatus status)
    {
        std::lock_guard lock(Mutex());
        Statuses()[static_cast<size_t>(family)] = std::move(status);
    }

    static FamilyStatus Get(Family family)
    {
        std::lock_guard lock(Mutex());
        return Statuses()[static_cast<size_t>(family)];
    }

  private:
    static std::array<FamilyStatus, static_cast<size_t>(Family::Count)>& Statuses()
    {
        static std::array<FamilyStatus, static_cast<size_t>(Family::Count)> statuses;
        return statuses;
    }

    static std::mutex& Mutex()
    {
        static std::mutex mutex;
        return mutex;
    }
};

inline std::string FormatStatus(const FamilyStatus& status)
{
    switch (status.state)
    {
    case FamilyState::Off:
        return "off";
    case FamilyState::Waiting:
        return "waiting for a provider";
    case FamilyState::Active:
    {
        size_t found = 0;
        for (const auto& patch : status.patches)
            found += patch.result == "found";
        return std::format("active: {} of {} patches", found, status.patches.size());
    }
    case FamilyState::StoodAside:
        return "stood aside: " + status.failure;
    case FamilyState::NotNeeded:
        return "not needed";
    }

    return "unknown";
}

inline std::wstring WidenAscii(std::string_view value) { return { value.begin(), value.end() }; }

inline std::wstring FormatDiagnostic(Family family, const FamilyStatus& status)
{
    const std::wstring familyName = WidenAscii(kFamilyNames[static_cast<size_t>(family)]);
    const std::wstring statusText = WidenAscii(FormatStatus(status));
    std::wstring result = std::format(L"[MFGUnlock] {}: {}; module {}; version {}; sha256 {}", familyName, statusText,
                                      status.modulePath, WidenAscii(status.version), WidenAscii(status.sha256));
    for (const auto& patch : status.patches)
        result += std::format(L"; patch {}: {}", WidenAscii(patch.name), WidenAscii(patch.result));
    return result;
}
} // namespace MfgUnlock
