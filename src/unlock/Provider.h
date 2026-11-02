// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/Provider.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. Adapted parts are MIT (KleberMotta / Dreamt / mavismmg): see licenses/MFGUnlock_LICENSE.txt.

#pragma once

#include "Image.h"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace MfgUnlock::Provider
{
// Adapted from KleberMotta/OptiScaler-DLSS5-MFG-RTX40 74c3bac (MIT); see Licenses/MFGUnlock_LICENSE.txt.
inline constexpr std::string_view kNgxMarker = "DLSSG.MultiFrameCountMax";

inline std::wstring NormalisePath(std::wstring_view path)
{
    std::wstring result;
    result.reserve(path.size());

    for (wchar_t character : path)
    {
        if (character == L'/')
            character = L'\\';
        else if (character >= L'A' && character <= L'Z')
            character = static_cast<wchar_t>(character - L'A' + L'a');

        if (character == L'\\' && !result.empty() && result.back() == L'\\')
            continue;
        result.push_back(character);
    }

    return result;
}

inline bool IsDlssgPath(std::wstring_view path)
{
    const std::wstring normalised = NormalisePath(path);
    const std::wstring_view view = normalised;
    const size_t slash = view.find_last_of(L'\\');
    const std::wstring_view filename = slash == std::wstring_view::npos ? view : view.substr(slash + 1);
    return filename == L"nvngx_dlssg.dll" || view.find(L"\\models\\dlssg\\") != std::wstring_view::npos;
}

inline bool IsStreamlineDlssgPath(std::wstring_view path)
{
    const std::wstring normalised = NormalisePath(path);
    const std::wstring_view view = normalised;
    const size_t slash = view.find_last_of(L'\\');
    const std::wstring_view filename = slash == std::wstring_view::npos ? view : view.substr(slash + 1);
    return filename == L"sl.dlss_g.dll";
}

inline bool HasNgxMarker(const ImageView& image)
{
    if (!image.IsValid())
        return false;

    for (WORD index = 0; index < image.SectionCount(); ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!image.SectionAt(index, section) || (section.Characteristics & IMAGE_SCN_MEM_READ) == 0 ||
            section.Misc.VirtualSize < kNgxMarker.size())
            continue;

        const auto* begin = reinterpret_cast<const char*>(image.Base() + section.VirtualAddress);
        if (std::search(begin, begin + section.Misc.VirtualSize, kNgxMarker.begin(), kNgxMarker.end()) !=
            begin + section.Misc.VirtualSize)
            return true;
    }

    return false;
}
} // namespace MfgUnlock::Provider
