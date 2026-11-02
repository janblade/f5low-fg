// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/FamilyAmpere.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. 
#pragma once

#include "FamilyRtx40.h"
#include "Image.h"

#include <algorithm>
#include <array>
#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// RTX 20/30 host side. nvngx_dlssg names the architecture it was built for (0x190, Ada) in two places that the
// rest of the stack reads: the constant NVSDK_NGX_GetGPUArchitecture returns, and the minimum architecture each
// NVSDK_NGX_*_GetFeatureRequirements stores in the requirements it reports. Lowering that number to Ampere (0x170) or
// Turing (0x160) lets the loader and the game see the feature as available on those cards. The sites are found from the
// exports, so a new build only needs the same shape, not the same addresses. The advertise/validate gates and the clamp
// and flip patches are the RTX 40 ones (FamilyRtx40) and do not depend on the GPU.
namespace MfgUnlock::FamilyAmpere
{
inline constexpr uint8_t kArchAdaLow = 0x90;    // 0x190
inline constexpr uint8_t kArchAmpereLow = 0x70; // 0x170
inline constexpr uint8_t kArchTuringLow = 0x60; // 0x160

struct Report
{
    std::vector<FamilyRtx40::LocatedPatch> patches;
    std::string failure;

    bool Complete() const
    {
        return failure.empty() && !patches.empty() &&
               std::all_of(patches.begin(), patches.end(),
                           [](const FamilyRtx40::LocatedPatch& patch) { return patch.state == LocateState::Found; });
    }
};

namespace detail
{
inline void Add(Report& report, std::string name, const ImageView& image, uint32_t rva, uint8_t replacement)
{
    FamilyRtx40::LocatedPatch patch;
    patch.name = std::move(name);
    const auto* byte = image.DataAtRva(rva, 1);
    if (byte == nullptr || static_cast<uint8_t>(*byte) != kArchAdaLow)
    {
        patch.state = LocateState::AnchorFailed;
    }
    else
    {
        patch.state = LocateState::Found;
        patch.rva = rva;
        patch.expected = { *byte };
        patch.replacement = { static_cast<std::byte>(replacement) };
    }
    report.patches.push_back(std::move(patch));
}

// mov dword ptr [rsp+disp8],0x190 (C7 44 24 xx 90 01 00 00): the minimum architecture stored into the requirements.
inline bool FindRequirementStores(const ImageView& image, uint32_t exportRva, std::vector<uint32_t>& sites)
{
    FunctionRange range;
    if (!image.FindFunctionRange(exportRva, range) || range.begin != exportRva || range.end <= range.begin)
        return false;
    const auto* bytes = image.DataAtRva(range.begin, range.end - range.begin);
    if (bytes == nullptr)
        return false;

    constexpr std::array<uint8_t, 3> head = { 0xC7, 0x44, 0x24 };
    constexpr std::array<uint8_t, 4> tail = { 0x90, 0x01, 0x00, 0x00 };
    const size_t size = range.end - range.begin;
    for (size_t at = 0; at + head.size() + 1 + tail.size() <= size; ++at)
    {
        const auto* candidate = reinterpret_cast<const uint8_t*>(bytes + at);
        if (std::memcmp(candidate, head.data(), head.size()) == 0 &&
            std::memcmp(candidate + head.size() + 1, tail.data(), tail.size()) == 0)
            sites.push_back(range.begin + static_cast<uint32_t>(at + head.size() + 1));
    }
    return true;
}
} // namespace detail

// archLow is the low byte of the architecture to report (kArchAmpereLow or kArchTuringLow).
inline Report Locate(const ImageView& image, uint8_t archLow)
{
    Report report;
    if (!image.IsValid())
    {
        report.failure = "nvngx_dlssg must be a valid mapped AMD64 image";
        return report;
    }

    uint32_t architectureRva = 0;
    if (!image.FindExport("NVSDK_NGX_GetGPUArchitecture", architectureRva))
    {
        report.failure = "NVSDK_NGX_GetGPUArchitecture export not found";
        return report;
    }
    // mov eax,0x190 / ret
    constexpr std::array<uint8_t, 6> architectureBody = { 0xB8, 0x90, 0x01, 0x00, 0x00, 0xC3 };
    const auto* body = image.DataAtRva(architectureRva, architectureBody.size());
    if (body == nullptr || std::memcmp(body, architectureBody.data(), architectureBody.size()) != 0)
    {
        report.failure = "NVSDK_NGX_GetGPUArchitecture is not a constant return of 0x190";
        return report;
    }
    detail::Add(report, "H1 GetGPUArchitecture", image, architectureRva + 1, archLow);

    constexpr std::array<std::string_view, 3> requirementExports = { "NVSDK_NGX_D3D11_GetFeatureRequirements",
                                                                     "NVSDK_NGX_D3D12_GetFeatureRequirements",
                                                                     "NVSDK_NGX_VULKAN_GetFeatureRequirements" };
    unsigned found = 0;
    for (const auto name : requirementExports)
    {
        uint32_t exportRva = 0;
        if (!image.FindExport(name, exportRva))
            continue;

        std::vector<uint32_t> sites;
        if (!detail::FindRequirementStores(image, exportRva, sites) || sites.size() != 1)
        {
            report.failure = std::string(name) + ": expected exactly one minimum-architecture store, found " +
                             std::to_string(sites.size());
            return report;
        }
        detail::Add(report, "H2 " + std::string(name.substr(10)), image, sites.front(), archLow);
        ++found;
    }
    if (found == 0)
        report.failure = "no GetFeatureRequirements export found";
    return report;
}
} // namespace MfgUnlock::FamilyAmpere
