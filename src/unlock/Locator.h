// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/Locator.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. 
#pragma once

#include "Image.h"

#include "hde/hde64.h"

#include <algorithm>
#include <array>
#include <bit>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace MfgUnlock
{
enum class LocateState
{
    Found,
    Missing,
    Ambiguous,
    AnchorFailed,
    NotExecutable,
    InvalidImage
};

struct MaskedPattern
{
    std::string_view bytes;
    std::string_view mask;
};

struct LocateResult
{
    LocateState state = LocateState::Missing;
    uint32_t rva = 0;
};

using SecondAnchor = bool (*)(const ImageView&, uint32_t, void*);

struct StringReference
{
    LocateState state = LocateState::Missing;
    uint32_t instructionRva = 0;
    FunctionRange function {};
};

inline bool PatternMatches(const ImageView& image, uint32_t rva, const MaskedPattern& pattern)
{
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size())
        return false;

    const std::byte* bytes = image.DataAtRva(rva, pattern.bytes.size());
    if (bytes == nullptr)
        return false;

    for (size_t index = 0; index < pattern.bytes.size(); ++index)
    {
        if (pattern.mask[index] == 'x')
        {
            if (bytes[index] != static_cast<std::byte>(static_cast<uint8_t>(pattern.bytes[index])))
                return false;
        }
        else if (pattern.mask[index] != '?')
        {
            return false;
        }
    }

    return true;
}

inline LocateResult FindUniqueInFunction(const ImageView& image, uint32_t anchorRva, const MaskedPattern& pattern,
                                         SecondAnchor secondAnchor = nullptr, void* anchorContext = nullptr)
{
    if (!image.IsValid())
        return { LocateState::InvalidImage, 0 };
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size())
        return { LocateState::Missing, 0 };

    FunctionRange function {};
    if (!image.FindFunctionRange(anchorRva, function))
        return { LocateState::Missing, 0 };

    if (pattern.bytes.size() > function.end - function.begin)
        return { LocateState::Missing, 0 };
    if (!image.IsExecutableReadOnly(function.begin, function.end - function.begin))
        return { LocateState::NotExecutable, 0 };

    uint32_t match = 0;
    size_t count = 0;
    const uint32_t lastStart = function.end - static_cast<uint32_t>(pattern.bytes.size());
    for (uint64_t candidate = function.begin; candidate <= lastStart; ++candidate)
    {
        if (PatternMatches(image, static_cast<uint32_t>(candidate), pattern))
        {
            match = static_cast<uint32_t>(candidate);
            if (++count > 1)
                return { LocateState::Ambiguous, 0 };
        }
    }

    if (count == 0)
        return { LocateState::Missing, 0 };
    if (secondAnchor != nullptr && !secondAnchor(image, match, anchorContext))
        return { LocateState::AnchorFailed, match };

    return { LocateState::Found, match };
}

inline LocateResult FindUniqueFromExport(const ImageView& image, std::string_view exportName,
                                         const MaskedPattern& pattern, SecondAnchor secondAnchor = nullptr,
                                         void* anchorContext = nullptr)
{
    uint32_t exportRva = 0;
    if (!image.FindExport(exportName, exportRva))
        return { LocateState::Missing, 0 };
    return FindUniqueInFunction(image, exportRva, pattern, secondAnchor, anchorContext);
}

inline LocateResult FindUniqueInImage(const ImageView& image, const MaskedPattern& pattern,
                                      SecondAnchor secondAnchor = nullptr, void* anchorContext = nullptr)
{
    if (!image.IsValid())
        return { LocateState::InvalidImage, 0 };
    if (pattern.bytes.empty() || pattern.bytes.size() != pattern.mask.size())
        return { LocateState::Missing, 0 };

    uint32_t match = 0;
    size_t count = 0;
    bool hadExecutableSection = false;
    for (WORD index = 0; index < image.SectionCount(); ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!image.SectionAt(index, section))
            return { LocateState::InvalidImage, 0 };
        if ((section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            (section.Characteristics & IMAGE_SCN_MEM_READ) == 0)
            continue;
        if (section.Misc.VirtualSize == 0)
            continue;

        hadExecutableSection = true;
        if (!image.IsExecutableReadOnly(section.VirtualAddress, section.Misc.VirtualSize))
            return { LocateState::NotExecutable, 0 };
        if (pattern.bytes.size() > section.Misc.VirtualSize)
            continue;

        const uint32_t lastStart =
            section.VirtualAddress + section.Misc.VirtualSize - static_cast<uint32_t>(pattern.bytes.size());
        for (uint64_t candidate = section.VirtualAddress; candidate <= lastStart; ++candidate)
        {
            if (PatternMatches(image, static_cast<uint32_t>(candidate), pattern))
            {
                match = static_cast<uint32_t>(candidate);
                if (++count > 1)
                    return { LocateState::Ambiguous, 0 };
            }
        }
    }

    if (!hadExecutableSection)
        return { LocateState::NotExecutable, 0 };
    if (count == 0)
        return { LocateState::Missing, 0 };
    if (secondAnchor != nullptr && !secondAnchor(image, match, anchorContext))
        return { LocateState::AnchorFailed, match };

    return { LocateState::Found, match };
}

// With prefix set the text may continue after `value` (the string must still be unique).
inline LocateResult FindUniqueStringRva(const ImageView& image, std::string_view value, bool prefix = false)
{
    if (!image.IsValid() || value.empty())
        return { LocateState::InvalidImage, 0 };

    uint32_t stringRva = 0;
    size_t count = 0;
    for (WORD index = 0; index < image.SectionCount(); ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!image.SectionAt(index, section))
            return { LocateState::InvalidImage, 0 };
        if ((section.Characteristics & IMAGE_SCN_MEM_READ) == 0 || value.size() + 1 > section.Misc.VirtualSize)
            continue;

        const auto* begin =
            reinterpret_cast<const char*>(image.DataAtRva(section.VirtualAddress, section.Misc.VirtualSize));
        if (begin == nullptr)
            return { LocateState::InvalidImage, 0 };

        const char* end = begin + section.Misc.VirtualSize;
        const char* candidate = begin;
        while ((candidate = std::search(candidate, end, value.begin(), value.end())) != end)
        {
            const size_t offset = static_cast<size_t>(candidate - begin);
            if (offset + value.size() < section.Misc.VirtualSize && (prefix || candidate[value.size()] == '\0'))
            {
                stringRva = section.VirtualAddress + static_cast<uint32_t>(offset);
                if (++count > 1)
                    return { LocateState::Ambiguous, 0 };
            }
            ++candidate;
        }
    }

    return count == 1 ? LocateResult { LocateState::Found, stringRva } : LocateResult { LocateState::Missing, 0 };
}

inline bool DecodeRelativeBranchTarget(const ImageView& image, uint32_t instructionRva, uint32_t& targetRva)
{
    targetRva = 0;
    const std::byte* instruction = image.DataAtRva(instructionRva, 1);
    if (instruction == nullptr)
        return false;

    std::array<uint8_t, 16> bytes {};
    const size_t available = std::min(bytes.size(), image.ImageSize() - instructionRva);
    std::memcpy(bytes.data(), instruction, available);

    hde64s decoded {};
    hde64_disasm(bytes.data(), &decoded);
    if (decoded.len == 0 || decoded.len > available || (decoded.flags & F_ERROR) != 0 ||
        (decoded.flags & F_RELATIVE) == 0)
        return false;

    int64_t displacement = 0;
    if (decoded.opcode == 0xe8 || decoded.opcode == 0xe9)
    {
        if ((decoded.flags & F_IMM32) == 0)
            return false;
        displacement = std::bit_cast<int32_t>(decoded.imm.imm32);
    }
    else if (decoded.opcode == 0xeb)
    {
        if ((decoded.flags & F_IMM8) == 0)
            return false;
        displacement = std::bit_cast<int8_t>(decoded.imm.imm8);
    }
    else if (decoded.opcode == 0x0f && decoded.opcode2 >= 0x80 && decoded.opcode2 <= 0x8f &&
             (decoded.flags & F_IMM32) != 0)
    {
        displacement = std::bit_cast<int32_t>(decoded.imm.imm32);
    }
    else
    {
        return false;
    }

    const int64_t target = static_cast<int64_t>(instructionRva) + decoded.len + displacement;
    if (target < 0 || static_cast<uint64_t>(target) >= image.ImageSize())
        return false;

    targetRva = static_cast<uint32_t>(target);
    return true;
}

inline bool BranchTargetEquals(const ImageView& image, uint32_t instructionRva, void* context)
{
    if (context == nullptr)
        return false;

    uint32_t target = 0;
    return DecodeRelativeBranchTarget(image, instructionRva, target) &&
           target == *static_cast<const uint32_t*>(context);
}

inline bool FollowRelativeJumpChain(const ImageView& image, uint32_t sourceRva, uint32_t& targetRva)
{
    targetRva = 0;
    if (!image.IsValid())
        return false;

    std::array<uint32_t, 8> visited {};
    size_t visitedCount = 0;
    uint32_t current = sourceRva;
    while (true)
    {
        if (!image.IsExecutableReadOnly(current, 1))
            return false;
        for (size_t index = 0; index < visitedCount; ++index)
            if (visited[index] == current)
                return false;
        if (visitedCount == visited.size())
            return false;
        visited[visitedCount++] = current;

        const auto* instruction = image.DataAtRva(current, 1);
        if (instruction == nullptr)
            return false;
        if (std::to_integer<uint8_t>(*instruction) != 0xe9)
        {
            targetRva = current;
            return true;
        }

        uint32_t next = 0;
        if (!DecodeRelativeBranchTarget(image, current, next))
            return false;
        current = next;
    }
}

inline LocateResult FindUniqueFromExportThunk(const ImageView& image, std::string_view exportName,
                                              const MaskedPattern& pattern, SecondAnchor secondAnchor = nullptr,
                                              void* anchorContext = nullptr)
{
    uint32_t exportRva = 0;
    if (!image.FindExport(exportName, exportRva))
        return { LocateState::Missing, 0 };

    uint32_t functionRva = 0;
    if (!FollowRelativeJumpChain(image, exportRva, functionRva))
        return { LocateState::AnchorFailed, exportRva };

    return FindUniqueInFunction(image, functionRva, pattern, secondAnchor, anchorContext);
}

inline StringReference FindUniqueStringReference(const ImageView& image, std::string_view value, bool prefix = false)
{
    const auto string = FindUniqueStringRva(image, value, prefix);
    if (string.state != LocateState::Found)
        return { string.state, 0, {} };

    StringReference result;
    size_t referenceCount = 0;
    for (size_t index = 0; index < image.RuntimeFunctionCount(); ++index)
    {
        IMAGE_RUNTIME_FUNCTION_ENTRY entry {};
        if (!image.RuntimeFunctionAt(index, entry))
            return { LocateState::InvalidImage, 0, {} };

        const uint32_t size = entry.EndAddress - entry.BeginAddress;
        if (!image.IsExecutableReadOnly(entry.BeginAddress, size))
            continue;

        for (uint64_t candidate = entry.BeginAddress; candidate < entry.EndAddress; ++candidate)
        {
            const uint32_t rva = static_cast<uint32_t>(candidate);
            const std::byte* data = image.DataAtRva(rva, 1);
            if (data == nullptr)
                return { LocateState::InvalidImage, 0, {} };
            const uint8_t first = std::to_integer<uint8_t>(*data);
            if (first != 0x8d && !(first >= 0x40 && first <= 0x4f))
                continue;
            if (first == 0x8d && rva > entry.BeginAddress)
            {
                const auto* previous = image.DataAtRva(rva - 1, 1);
                if (previous != nullptr)
                {
                    const uint8_t rex = std::to_integer<uint8_t>(*previous);
                    if (rex >= 0x40 && rex <= 0x4f)
                        continue;
                }
            }

            const size_t available = std::min<size_t>(16, entry.EndAddress - rva);
            std::array<uint8_t, 16> bytes {};
            const std::byte* instruction = image.DataAtRva(rva, available);
            if (instruction == nullptr)
                return { LocateState::InvalidImage, 0, {} };
            std::memcpy(bytes.data(), instruction, available);

            hde64s decoded {};
            hde64_disasm(bytes.data(), &decoded);
            if (decoded.len == 0 || decoded.len > available || (decoded.flags & F_ERROR) != 0 ||
                decoded.opcode != 0x8d || decoded.modrm_mod != 0 || decoded.modrm_rm != 5 ||
                (decoded.flags & F_DISP32) == 0 || (decoded.flags & F_PREFIX_67) != 0)
                continue;

            const int64_t displacement = std::bit_cast<int32_t>(decoded.disp.disp32);
            const int64_t target = static_cast<int64_t>(rva) + decoded.len + displacement;
            if (target != string.rva)
                continue;

            if (++referenceCount > 1)
                return { LocateState::Ambiguous, 0, {} };
            result.instructionRva = rva;
            result.function = { entry.BeginAddress, entry.EndAddress };
        }
    }

    if (referenceCount == 0)
        return { LocateState::Missing, 0, {} };
    result.state = LocateState::Found;
    return result;
}

inline LocateResult FindUniqueFromStringXref(const ImageView& image, std::string_view value,
                                             const MaskedPattern& pattern, SecondAnchor secondAnchor = nullptr,
                                             void* anchorContext = nullptr)
{
    const auto reference = FindUniqueStringReference(image, value);
    if (reference.state != LocateState::Found)
        return { reference.state, reference.instructionRva };

    return FindUniqueInFunction(image, reference.instructionRva, pattern, secondAnchor, anchorContext);
}
} // namespace MfgUnlock
