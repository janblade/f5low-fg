// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/Mode.h at 745e49431 (origin/release/0.1.36). Kept identical to the original.
// f5low does not use the modes at run time (its tier comes from the GPU); the ported core smoke test exercises the parser, so the file stays.
#pragma once

#include <optional>
#include <string_view>

// [MFGUnlock] values. As everywhere else in the ini, `auto` (or a missing key) means the default, which is off;
// `detect` applies a family only where its own GPU check says it is wanted.
enum class MfgUnlockMode
{
    Detect,
    Off,
    On
};

namespace MfgUnlock
{
// nullopt: the default. valid turns false for text that is not a mode (that also reads as the default).
inline std::optional<MfgUnlockMode> ParseMode(std::string_view text, bool& valid)
{
    const auto equals = [text](std::string_view word)
    {
        if (text.size() != word.size())
            return false;
        for (size_t index = 0; index < text.size(); ++index)
        {
            const char c =
                text[index] >= 'A' && text[index] <= 'Z' ? static_cast<char>(text[index] - 'A' + 'a') : text[index];
            if (c != word[index])
                return false;
        }
        return true;
    };

    valid = true;
    if (text.empty() || equals("auto"))
        return std::nullopt;
    if (equals("detect"))
        return MfgUnlockMode::Detect;
    if (equals("false") || equals("off"))
        return MfgUnlockMode::Off;
    if (equals("true") || equals("on"))
        return MfgUnlockMode::On;
    valid = false;
    return std::nullopt;
}

// What a family does when its key is missing, `auto` or not a mode: nothing.
inline constexpr MfgUnlockMode kDefaultMode = MfgUnlockMode::Off;

inline MfgUnlockMode EffectiveMode(std::optional<MfgUnlockMode> mode) { return mode.value_or(kDefaultMode); }

// The text saved for a value: nullopt (the default) is saved as auto.
inline const char* ModeText(std::optional<MfgUnlockMode> mode)
{
    if (!mode.has_value())
        return "auto";
    switch (mode.value())
    {
    case MfgUnlockMode::Detect:
        return "detect";
    case MfgUnlockMode::On:
        return "true";
    case MfgUnlockMode::Off:
        break;
    }
    return "false";
}
} // namespace MfgUnlock
