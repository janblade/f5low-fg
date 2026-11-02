// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/FamilyRtx40.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: the include of FamilyXeFG.h (the XeFG family is not ported) is LocateStateName.h, which holds the one function used from it.
// GPL-3.0 with the rest of this repo. 
#pragma once

#include "LocateStateName.h"
#include "Locator.h"
#include "Patch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <string>
#include <string_view>
#include <vector>

// RTX 40 DLSS-G multi-frame generation: the two architecture gates of nvngx_dlssg (advertise: the maximum generated
// frame count NGX publishes; validate: the capability flag and the accepted count). Both gates compare the
// architecture number against 0x1B0 (Blackwell) and exist in two code generations each. Locators are structural:
// the pattern must be unique in the executable sections and lie inside one unwind function; the 310.9 advertise
// branch must also stay inside that function.
namespace MfgUnlock::FamilyRtx40
{
using FamilyXeFG::LocateStateName;

inline constexpr uint8_t kMaxGeneratedFrames = 5;

struct LocatedPatch
{
    std::string name;
    LocateState state = LocateState::Missing;
    uint32_t rva = 0;
    std::vector<std::byte> expected;
    std::vector<std::byte> replacement;
};

struct Report
{
    std::vector<LocatedPatch> patches;
    std::string advertiseLayout;
    std::string validateLayout;
    std::string failure;

    bool Complete() const
    {
        return failure.empty() && !patches.empty() &&
               std::all_of(patches.begin(), patches.end(),
                           [](const LocatedPatch& patch) { return patch.state == LocateState::Found; });
    }
};

namespace detail
{
// mov ebx,1 / mov r8d,3 / cmp edi,0x1b0 / cmovl r8d,ebx
inline constexpr std::string_view kAdvertiseLegacyBytes("\xBB\x01\x00\x00\x00\x41\xB8\x03\x00\x00\x00\x81\xFF\xB0\x01"
                                                        "\x00\x00\x44\x0F\x4C\xC3",
                                                        21);
inline constexpr std::string_view kAdvertiseLegacyMask = "xxxxxxxxxxxxxxxxxxxxx";
// cmp ebp,0x1b0 / jl rel32 / mov edi,5
inline constexpr std::string_view kAdvertise309Bytes("\x81\xFD\xB0\x01\x00\x00\x0F\x8C\x00\x00\x00\x00\xBF\x05\x00"
                                                     "\x00\x00",
                                                     17);
inline constexpr std::string_view kAdvertise309Mask = "xxxxxxxx????xxxxx";
// cmp eax,0x1b0 / jl rel8 / cmp ebx,3 / jbe
inline constexpr std::string_view kValidateLegacyBytes("\x3D\xB0\x01\x00\x00\x7C\x00\x83\xFB\x03\x76", 11);
inline constexpr std::string_view kValidateLegacyMask = "xxxxxx?xxxx";
// cmp eax,0x1b0 / setae al
inline constexpr std::string_view kValidate309Bytes("\x3D\xB0\x01\x00\x00\x0F\x93\xC0", 8);
inline constexpr std::string_view kValidate309Mask = "xxxxxxxx";

inline constexpr MaskedPattern kAdvertiseLegacy { kAdvertiseLegacyBytes, kAdvertiseLegacyMask };
inline constexpr MaskedPattern kAdvertise309 { kAdvertise309Bytes, kAdvertise309Mask };
inline constexpr MaskedPattern kValidateLegacy { kValidateLegacyBytes, kValidateLegacyMask };
inline constexpr MaskedPattern kValidate309 { kValidate309Bytes, kValidate309Mask };

inline bool InsideSameFunction(const ImageView& image, uint32_t matchRva, size_t length)
{
    FunctionRange function {};
    return image.FindFunctionRange(matchRva, function) && matchRva >= function.begin &&
           static_cast<uint64_t>(matchRva) + length <= function.end;
}

inline bool LegacyAnchor(const ImageView& image, uint32_t matchRva, void* context)
{
    return InsideSameFunction(image, matchRva, *static_cast<const size_t*>(context));
}

// The 310.9 advertise branch is the ada path of the same function.
inline bool Advertise309Anchor(const ImageView& image, uint32_t matchRva, void*)
{
    FunctionRange function {};
    uint32_t target = 0;
    return image.FindFunctionRange(matchRva, function) && matchRva >= function.begin &&
           matchRva + kAdvertise309Bytes.size() <= function.end &&
           DecodeRelativeBranchTarget(image, matchRva + 6, target) && target >= function.begin &&
           target < function.end;
}

// The two code generations of one gate: exactly one may be present.
inline LocateResult ChooseOne(const LocateResult& first, const LocateResult& second, bool& chosenFirst)
{
    const bool firstFound = first.state == LocateState::Found;
    const bool secondFound = second.state == LocateState::Found;
    chosenFirst = firstFound;
    if (firstFound != secondFound)
        return firstFound ? first : second;
    if (firstFound || first.state == LocateState::Ambiguous || second.state == LocateState::Ambiguous)
        return { LocateState::Ambiguous, 0 };
    if (first.state == LocateState::NotExecutable || second.state == LocateState::NotExecutable)
        return { LocateState::NotExecutable, 0 };
    if (first.state == LocateState::InvalidImage || second.state == LocateState::InvalidImage)
        return { LocateState::InvalidImage, 0 };
    if (first.state == LocateState::AnchorFailed || second.state == LocateState::AnchorFailed)
        return { LocateState::AnchorFailed, 0 };
    return { LocateState::Missing, 0 };
}

inline std::vector<std::byte> Bytes(std::initializer_list<uint8_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const auto value : values)
        result.push_back(static_cast<std::byte>(value));
    return result;
}

// Reads the bytes being replaced from the image (a rel32 or rel8 is not a constant) and checks the opcode prefix.
inline void AddPatch(Report& report, std::string_view name, const ImageView& image, const LocateResult& location,
                     uint32_t offset, std::initializer_list<uint8_t> expectedPrefix, std::vector<std::byte> replacement)
{
    LocatedPatch patch;
    patch.name = name;
    patch.state = location.state;
    patch.replacement = std::move(replacement);
    if (location.state == LocateState::Found)
    {
        const uint64_t patchRva = static_cast<uint64_t>(location.rva) + offset;
        const auto* bytes = patchRva <= UINT32_MAX
                                ? image.DataAtRva(static_cast<uint32_t>(patchRva), patch.replacement.size())
                                : nullptr;
        const auto prefix = Bytes(expectedPrefix);
        if (bytes == nullptr || std::memcmp(bytes, prefix.data(), prefix.size()) != 0)
        {
            patch.state = LocateState::AnchorFailed;
        }
        else
        {
            patch.rva = static_cast<uint32_t>(patchRva);
            patch.expected.assign(bytes, bytes + patch.replacement.size());
        }
    }
    report.patches.push_back(std::move(patch));
}
} // namespace detail

inline Report Locate(const ImageView& image)
{
    Report report;
    if (!image.IsValid())
    {
        report.failure = "nvngx_dlssg must be a valid mapped AMD64 image";
        return report;
    }

    size_t advertiseLegacyLength = detail::kAdvertiseLegacyBytes.size();
    size_t validateLegacyLength = detail::kValidateLegacyBytes.size();
    bool advertiseLegacy = false;
    const auto advertise = detail::ChooseOne(
        FindUniqueInImage(image, detail::kAdvertiseLegacy, detail::LegacyAnchor, &advertiseLegacyLength),
        FindUniqueInImage(image, detail::kAdvertise309, detail::Advertise309Anchor), advertiseLegacy);
    if (advertise.state == LocateState::Found && advertiseLegacy)
    {
        report.advertiseLayout = "legacy";
        detail::AddPatch(report, "A1 advertise count", image, advertise, 7, { 0x03 },
                         detail::Bytes({ kMaxGeneratedFrames }));
        detail::AddPatch(report, "A2 advertise architecture clamp", image, advertise, 17, { 0x44, 0x0F, 0x4C, 0xC3 },
                         detail::Bytes({ 0x0F, 0x1F, 0x40, 0x00 }));
    }
    else if (advertise.state == LocateState::Found)
    {
        report.advertiseLayout = "310.9";
        detail::AddPatch(report, "A1 advertise architecture branch", image, advertise, 6, { 0x0F, 0x8C },
                         detail::Bytes({ 0x0F, 0x1F, 0x44, 0x00, 0x00, 0x90 }));
    }
    else
    {
        report.advertiseLayout = "none";
        detail::AddPatch(report, "A1 advertise gate", image, advertise, 0, { 0x00 }, {});
    }

    bool validateLegacy = false;
    const auto validate = detail::ChooseOne(
        FindUniqueInImage(image, detail::kValidateLegacy, detail::LegacyAnchor, &validateLegacyLength),
        FindUniqueInImage(image, detail::kValidate309, detail::LegacyAnchor, &validateLegacyLength), validateLegacy);
    if (validate.state == LocateState::Found && validateLegacy)
    {
        report.validateLayout = "legacy";
        detail::AddPatch(report, "V1 validate architecture branch", image, validate, 5, { 0x7C },
                         detail::Bytes({ 0x90, 0x90 }));
        detail::AddPatch(report, "V2 validate count", image, validate, 9, { 0x03 },
                         detail::Bytes({ kMaxGeneratedFrames }));
    }
    else if (validate.state == LocateState::Found)
    {
        report.validateLayout = "310.9";
        detail::AddPatch(report, "V1 validate capability flag", image, validate, 5, { 0x0F, 0x93, 0xC0 },
                         detail::Bytes({ 0xB0, 0x01, 0x90 }));
    }
    else
    {
        report.validateLayout = "none";
        detail::AddPatch(report, "V1 validate gate", image, validate, 0, { 0x00 }, {});
    }

    for (const auto& patch : report.patches)
    {
        if (patch.state != LocateState::Found)
        {
            report.failure = patch.name + ": " + std::string(LocateStateName(patch.state));
            break;
        }
    }
    return report;
}

// One byte per operation, only where the byte changes: the transaction primitive writes 1, 2, 4 or 8 aligned bytes,
// and the module is patched before NGX runs any of it.
inline std::vector<PatchOperation> BuildOperationsFromPatches(const std::vector<LocatedPatch>& patches,
                                                              const ImageView& image)
{
    std::vector<PatchOperation> operations;
    for (const auto& patch : patches)
    {
        if (patch.state != LocateState::Found || patch.expected.size() != patch.replacement.size())
            return {};
        const auto* address = image.DataAtRva(patch.rva, patch.expected.size());
        if (address == nullptr)
            return {};

        for (size_t index = 0; index < patch.expected.size(); ++index)
        {
            if (patch.expected[index] == patch.replacement[index])
                continue;
            PatchOperation operation;
            operation.address = const_cast<std::byte*>(address + index);
            operation.expected = { patch.expected[index] };
            operation.replacement = { patch.replacement[index] };
            operations.push_back(std::move(operation));
        }
    }
    return operations;
}

inline std::vector<PatchOperation> BuildOperations(const Report& report, const ImageView& image)
{
    if (!report.Complete())
        return {};
    return BuildOperationsFromPatches(report.patches, image);
}
} // namespace MfgUnlock::FamilyRtx40
