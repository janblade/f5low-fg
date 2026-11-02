// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/FamilyXeFG.h at origin/release/0.1.36 (our own code there, GPL-3.0 here with the rest of this repo).
// Kept identical to the original except where listed here: LocateStateName lives in LocateStateName.h (the ported FamilyRtx40.h uses it
// from there), so this header includes that file instead of defining it again.
// What it finds in Intel's libxess_fg.dll (U1-U5) and libxell.dll (XL1): the gates that keep XeSS frame generation at 3 generated frames
// per rendered frame (see Locate). Structural byte shapes and unwind-function anchors only, no Intel code or addresses; applied by
// UnlockXeFg.h to f5low's PRIVATE copies, in memory, never to a game's or OptiScaler's modules.
#pragma once

#include "LocateStateName.h"
#include "Locator.h"
#include "Patch.h"

#include <algorithm>
#include <array>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <initializer_list>
#include <span>
#include <string>
#include <string_view>
#include <vector>

namespace MfgUnlock::FamilyXeFG
{
enum class Module
{
    XeFG,
    XeLL
};

struct LocatedPatch
{
    std::string name;
    Module module = Module::XeFG;
    LocateState state = LocateState::Missing;
    uint32_t rva = 0;
    std::vector<std::byte> expected;
    std::vector<std::byte> replacement;
};

struct Report
{
    std::vector<LocatedPatch> patches;
    std::string failure;

    bool Complete() const
    {
        return failure.empty() && patches.size() == 6 &&
               std::all_of(patches.begin(), patches.end(),
                           [](const LocatedPatch& patch) { return patch.state == LocateState::Found; });
    }
};

namespace detail
{
inline constexpr char kU1Bytes[] =
    "\xE8\x00\x00\x00\x00\x84\xC0\x0F\x85\x00\x00\x00\x00\x83\xFB\x01\x0F\x86\x00\x00\x00\x00";
inline constexpr std::string_view kU1PatternBytes(kU1Bytes, sizeof(kU1Bytes) - 1);
inline constexpr std::string_view kU1PatternMask = "x????xxxx????xxxxx????";
static_assert(kU1PatternBytes.size() == kU1PatternMask.size());
inline constexpr MaskedPattern kU1Pattern { kU1PatternBytes, kU1PatternMask };

inline constexpr char kU2OldBytes[] =
    "\x80\x79\x4D\x00\x74\x09\x80\x79\x64\x00\x74\x03\xB0\x01\xC3\x32\xC0\xC3";
inline constexpr std::string_view kU2OldPatternBytes(kU2OldBytes, sizeof(kU2OldBytes) - 1);
inline constexpr MaskedPattern kU2OldPattern { kU2OldPatternBytes, "xxxxxxxxxxxxxxxxxx" };

inline constexpr char kU2NewBytes[] =
    "\x80\x79\x6E\x00\x74\x0C\x80\xB9\x84\x00\x00\x00\x00\x74\x03\xB0\x01\xC3\x32\xC0\xC3";
inline constexpr std::string_view kU2NewPatternBytes(kU2NewBytes, sizeof(kU2NewBytes) - 1);
inline constexpr MaskedPattern kU2NewPattern { kU2NewPatternBytes, "xxxxxxxxxxxxxxxxxxxxx" };

inline constexpr char kU3Bytes[] =
    "\x48\x89\x5C\x24\x08\x57\x48\x83\xEC\x20\x48\x8B\xF9\xBB\x03\x00\x00\x00"
    "\xE8\x00\x00\x00\x00\x84\xC0\x74\x0D\x48\x8B\xCF\xE8\x00\x00\x00\x00"
    "\x3B\xC3\x0F\x47\xD8\x8B\xC3";
inline constexpr std::string_view kU3PatternBytes(kU3Bytes, sizeof(kU3Bytes) - 1);
inline constexpr std::string_view kU3PatternMask = "xxxxxxxxxxxxxxxxxxx????xxxxxxxx????xxxxxxx";
static_assert(kU3PatternBytes.size() == 42);
static_assert(kU3PatternBytes.size() == kU3PatternMask.size());
inline constexpr MaskedPattern kU3Pattern { kU3PatternBytes, kU3PatternMask };

inline constexpr char kU4OldBytes[] =
    "\x48\x8B\x80\x98\x00\x00\x00\xFF\x15\x00\x00\x00\x00\x84\xC0\x75\x11"
    "\xC7\x87\x6C\x01\x00\x00\x01\x00\x00\x00\xC6\x87\x68\x01\x00\x00\x01";
inline constexpr std::string_view kU4OldPatternBytes(kU4OldBytes, sizeof(kU4OldBytes) - 1);
inline constexpr std::string_view kU4OldPatternMask = "xxxxxxxxx????xxxxxxxxxxxxxxxxxxxxx";
static_assert(kU4OldPatternBytes.size() == kU4OldPatternMask.size());
inline constexpr MaskedPattern kU4OldPattern { kU4OldPatternBytes, kU4OldPatternMask };

inline constexpr char kU4NewBytes[] =
    "\x8B\x80\x98\x00\x00\x00\xFF\x15\x00\x00\x00\x00\x84\xC0\x75\x11"
    "\xC7\x87\x8C\x01\x00\x00\x01\x00\x00\x00\xC6\x87\x88\x01\x00\x00\x01";
inline constexpr std::string_view kU4NewPatternBytes(kU4NewBytes, sizeof(kU4NewBytes) - 1);
inline constexpr std::string_view kU4NewPatternMask = "xxxxxxxx????xxxxxxxxxxxxxxxxxxxxx";
static_assert(kU4NewPatternBytes.size() == kU4NewPatternMask.size());
inline constexpr MaskedPattern kU4NewPattern { kU4NewPatternBytes, kU4NewPatternMask };

inline constexpr char kU5Bytes[] = "\xB8\x01\x00\x00\x00\x89\x47\x20";
inline constexpr std::string_view kU5PatternBytes(kU5Bytes, sizeof(kU5Bytes) - 1);
inline constexpr MaskedPattern kU5Pattern { kU5PatternBytes, "xxxxxxxx" };

inline constexpr char kXellBytes[] = "\x83\xFB\x03\x76\x07";
inline constexpr std::string_view kXellPatternBytes(kXellBytes, sizeof(kXellBytes) - 1);
inline constexpr MaskedPattern kXellPattern { kXellPatternBytes, "xxxxx" };

inline constexpr std::string_view kBooleanReturnSequence("\xB0\x01\xC3\x32\xC0\xC3", 6);

inline std::vector<std::byte> Bytes(std::initializer_list<uint8_t> values)
{
    std::vector<std::byte> result;
    result.reserve(values.size());
    for (const uint8_t value : values)
        result.push_back(static_cast<std::byte>(value));
    return result;
}

inline bool U1Anchor(const ImageView& image, uint32_t matchRva, void*)
{
    uint32_t firstExit = 0;
    uint32_t secondExit = 0;
    if (!DecodeRelativeBranchTarget(image, matchRva + 7, firstExit) ||
        !DecodeRelativeBranchTarget(image, matchRva + 16, secondExit) || firstExit != secondExit)
        return false;

    uint32_t predicateThunk = 0;
    uint32_t predicateRva = 0;
    if (!DecodeRelativeBranchTarget(image, matchRva, predicateThunk) ||
        !FollowRelativeJumpChain(image, predicateThunk, predicateRva))
        return false;

    const auto* predicate = image.DataAtRva(predicateRva, 64);
    if (predicate == nullptr)
        return false;
    return std::search(predicate, predicate + 64,
                       reinterpret_cast<const std::byte*>(kBooleanReturnSequence.data()),
                       reinterpret_cast<const std::byte*>(kBooleanReturnSequence.data() +
                                                           kBooleanReturnSequence.size())) != predicate + 64;
}

inline bool HasCallTo(const ImageView& image, uint32_t candidateRva, void* context)
{
    if (context == nullptr)
        return false;

    const auto expectedTarget = *static_cast<const uint32_t*>(context);
    FunctionRange function {};
    if (!image.FindFunctionRange(candidateRva, function) ||
        !image.IsExecutableReadOnly(function.begin, function.end - function.begin))
        return false;

    uint32_t current = function.begin;
    while (current < function.end)
    {
        std::array<uint8_t, 16> bytes {};
        const size_t available = std::min(bytes.size(), static_cast<size_t>(function.end - current));
        const auto* source = image.DataAtRva(current, available);
        if (source == nullptr || available == 0)
            return false;
        std::memcpy(bytes.data(), source, available);

        hde64s decoded {};
        hde64_disasm(bytes.data(), &decoded);
        if (decoded.len == 0 || decoded.len > available || (decoded.flags & F_ERROR) != 0)
            return false;

        if (decoded.opcode == 0xE8)
        {
            uint32_t target = 0;
            uint32_t implementation = 0;
            if (DecodeRelativeBranchTarget(image, current, target) &&
                FollowRelativeJumpChain(image, target, implementation) && implementation == expectedTarget)
                return true;
        }

        current += decoded.len;
    }
    return false;
}

inline void AddPatch(Report& report, std::string_view name, Module module, const ImageView& image,
                     LocateResult location, uint32_t patchOffset, std::vector<std::byte> expected,
                     std::vector<std::byte> replacement)
{
    LocatedPatch patch;
    patch.name = name;
    patch.module = module;
    patch.state = location.state;
    patch.expected = std::move(expected);
    patch.replacement = std::move(replacement);
    if (location.state == LocateState::Found)
    {
        const uint64_t patchRva = static_cast<uint64_t>(location.rva) + patchOffset;
        const auto* bytes = patchRva <= UINT32_MAX
                                ? image.DataAtRva(static_cast<uint32_t>(patchRva), patch.expected.size())
                                : nullptr;
        if (bytes == nullptr || patch.expected.empty() ||
            std::memcmp(bytes, patch.expected.data(), patch.expected.size()) != 0 ||
            (patch.expected.size() == 2 && reinterpret_cast<uintptr_t>(bytes) % alignof(SHORT) != 0))
        {
            patch.state = LocateState::AnchorFailed;
        }
        else
        {
            patch.rva = static_cast<uint32_t>(patchRva);
        }
    }
    report.patches.push_back(std::move(patch));
}

inline LocateResult FindU4(const ImageView& image, uint32_t& patchOffset)
{
    const auto oldLayout = FindUniqueInImage(image, kU4OldPattern);
    const auto newLayout = FindUniqueInImage(image, kU4NewPattern);
    const bool oldFound = oldLayout.state == LocateState::Found;
    const bool newFound = newLayout.state == LocateState::Found;
    if (oldFound == newFound)
    {
        if (oldFound)
            return { LocateState::Ambiguous, 0 };
        if (oldLayout.state == LocateState::Ambiguous || newLayout.state == LocateState::Ambiguous)
            return { LocateState::Ambiguous, 0 };
        if (oldLayout.state == LocateState::NotExecutable || newLayout.state == LocateState::NotExecutable)
            return { LocateState::NotExecutable, 0 };
        return { oldLayout.state == LocateState::InvalidImage || newLayout.state == LocateState::InvalidImage
                     ? LocateState::InvalidImage
                     : LocateState::Missing,
                 0 };
    }

    patchOffset = oldFound ? 23 : 22;
    return oldFound ? oldLayout : newLayout;
}
} // namespace detail

inline Report Locate(const ImageView& xeFgImage, const ImageView& xeLlImage, uint8_t maxInterpolations)
{
    Report report;
    if (!xeFgImage.IsValid() || !xeLlImage.IsValid())
    {
        report.failure = "XeFG and XeLL must both be valid mapped AMD64 images";
        return report;
    }
    if (maxInterpolations < 1 || maxInterpolations > 5)
    {
        report.failure = "MaxInterpolations must be between 1 and 5";
        return report;
    }

    const auto countByte = static_cast<uint8_t>(maxInterpolations);
    const auto countBytes = detail::Bytes({ countByte });

    const auto u1 = FindUniqueFromStringXref(xeFgImage,
                                            "XeLL version {}.{}.{} is too old to support multi-frame generation, "
                                            "falling back to 2x",
                                            detail::kU1Pattern, detail::U1Anchor);
    // The `jne` (predicate: XeLL accepted) and the `jbe` both exit to "return the requested count"; falling through
    // is the "too old, falling back to 2x" path. The anchored predicate returns 0 or 1, so SF is always clear and
    // JNS is always taken: the requested count is kept. (JS would force the 2x fallback.)
    detail::AddPatch(report, "U1 XeLL-version fallback", Module::XeFG, xeFgImage, u1, 8,
                     detail::Bytes({ 0x85 }), detail::Bytes({ 0x89 }));

    const auto u2Old = FindUniqueInImage(xeFgImage, detail::kU2OldPattern);
    const auto u2New = FindUniqueInImage(xeFgImage, detail::kU2NewPattern);
    const bool u2OldFound = u2Old.state == LocateState::Found;
    const bool u2NewFound = u2New.state == LocateState::Found;
    LocateResult u2 {};
    uint32_t u2PatchOffset = 4;
    std::vector<std::byte> u2Expected;
    std::vector<std::byte> u2Replacement;
    if (u2OldFound != u2NewFound)
    {
        u2 = u2OldFound ? u2Old : u2New;
        u2Expected = detail::Bytes({ 0x74, static_cast<uint8_t>(u2OldFound ? 0x09 : 0x0C) });
        u2Replacement = detail::Bytes({ 0xEB, static_cast<uint8_t>(u2OldFound ? 0x06 : 0x09) });
    }
    else if (u2OldFound)
    {
        u2 = { LocateState::Ambiguous, 0 };
        u2Expected = detail::Bytes({ 0x74, 0x09 });
        u2Replacement = detail::Bytes({ 0xEB, 0x06 });
    }
    else
    {
        u2 = { u2Old.state == LocateState::Ambiguous || u2New.state == LocateState::Ambiguous
                   ? LocateState::Ambiguous
                   : (u2Old.state == LocateState::NotExecutable || u2New.state == LocateState::NotExecutable
                          ? LocateState::NotExecutable
                          : LocateState::Missing),
               0 };
        u2Expected = detail::Bytes({ 0x74, 0x09 });
        u2Replacement = detail::Bytes({ 0xEB, 0x06 });
    }
    detail::AddPatch(report, "U2 MFG-allowed leaf", Module::XeFG, xeFgImage, u2, u2PatchOffset,
                     std::move(u2Expected), std::move(u2Replacement));

    const auto u3 = FindUniqueInImage(xeFgImage, detail::kU3Pattern);
    uint32_t defaultCeilingRva = u3.rva;
    detail::AddPatch(report, "U3 default ceiling", Module::XeFG, xeFgImage, u3, 14, detail::Bytes({ 0x03 }),
                     countBytes);

    uint32_t u4PatchOffset = 0;
    const auto u4 = detail::FindU4(xeFgImage, u4PatchOffset);
    detail::AddPatch(report, "U4 MFG count pin", Module::XeFG, xeFgImage, u4, u4PatchOffset,
                     detail::Bytes({ 0x01 }), countBytes);

    const auto u5 = FindUniqueInImage(xeFgImage, detail::kU5Pattern, detail::HasCallTo, &defaultCeilingRva);
    detail::AddPatch(report, "U5 reported maximum fallback", Module::XeFG, xeFgImage, u5, 1,
                     detail::Bytes({ 0x01 }), countBytes);

    const auto xell = FindUniqueFromExportThunk(xeLlImage, "xellSetGeneratedFramesCount", detail::kXellPattern);
    detail::AddPatch(report, "XL1 XeLL frame-count gate", Module::XeLL, xeLlImage, xell, 2,
                     detail::Bytes({ 0x03 }), countBytes);

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

inline std::vector<PatchOperation> BuildOperations(const Report& report, const ImageView& xeFgImage,
                                                   const ImageView& xeLlImage)
{
    if (!report.Complete())
        return {};

    std::vector<PatchOperation> operations;
    operations.reserve(report.patches.size());
    for (const auto& patch : report.patches)
    {
        const ImageView& image = patch.module == Module::XeFG ? xeFgImage : xeLlImage;
        const auto* address = image.DataAtRva(patch.rva, patch.expected.size());
        if (address == nullptr)
            return {};

        PatchOperation operation;
        operation.address = const_cast<std::byte*>(address);
        operation.expected = patch.expected;
        operation.replacement = patch.replacement;
        operations.push_back(std::move(operation));
    }
    return operations;
}
} // namespace MfgUnlock::FamilyXeFG
