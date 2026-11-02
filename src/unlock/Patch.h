// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/Patch.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. 
#pragma once

#include <Windows.h>
#include <intrin.h>

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <utility>
#include <vector>

namespace MfgUnlock
{
struct PatchOperation
{
    void* address = nullptr;
    std::vector<std::byte> expected;
    std::vector<std::byte> replacement;
};

struct TransactionResult
{
    bool applied = false;
    bool rollbackComplete = true;
    size_t failedIndex = 0;
    bool failedIndexValid = false;
};

using PatchWriter = bool (*)(void*, std::span<const std::byte>, std::span<const std::byte>);

inline bool CompareExchangeBytes(void* address, std::span<const std::byte> expected,
                                 std::span<const std::byte> replacement)
{
    if (address == nullptr || expected.empty() || expected.size() != replacement.size())
        return false;

    switch (expected.size())
    {
    case 1:
    {
        const CHAR oldValue = static_cast<CHAR>(std::to_integer<uint8_t>(expected[0]));
        const CHAR newValue = static_cast<CHAR>(std::to_integer<uint8_t>(replacement[0]));
        return _InterlockedCompareExchange8(static_cast<volatile CHAR*>(address), newValue, oldValue) == oldValue;
    }
    case 2:
    {
        if (reinterpret_cast<uintptr_t>(address) % alignof(SHORT) != 0)
            return false;
        SHORT oldValue = 0;
        SHORT newValue = 0;
        std::memcpy(&oldValue, expected.data(), sizeof(oldValue));
        std::memcpy(&newValue, replacement.data(), sizeof(newValue));
        return _InterlockedCompareExchange16(static_cast<volatile SHORT*>(address), newValue, oldValue) == oldValue;
    }
    case 4:
    {
        if (reinterpret_cast<uintptr_t>(address) % alignof(LONG) != 0)
            return false;
        LONG oldValue = 0;
        LONG newValue = 0;
        std::memcpy(&oldValue, expected.data(), sizeof(oldValue));
        std::memcpy(&newValue, replacement.data(), sizeof(newValue));
        return _InterlockedCompareExchange(static_cast<volatile LONG*>(address), newValue, oldValue) == oldValue;
    }
    case 8:
    {
        if (reinterpret_cast<uintptr_t>(address) % alignof(LONG64) != 0)
            return false;
        LONG64 oldValue = 0;
        LONG64 newValue = 0;
        std::memcpy(&oldValue, expected.data(), sizeof(oldValue));
        std::memcpy(&newValue, replacement.data(), sizeof(newValue));
        return _InterlockedCompareExchange64(static_cast<volatile LONG64*>(address), newValue, oldValue) == oldValue;
    }
    default:
        return false;
    }
}

namespace detail
{
struct ProtectedRegion
{
    void* base = nullptr;
    size_t size = 0;
    DWORD originalProtection = 0;
    bool writable = false;
};

// Code is mapped execute-read; the kernel containers of nvngx_dlssg sit in a read-only, copy-on-write or writable data
// section depending on the build. Every write is a compare-exchange against the bytes the locator read.
inline bool IsPatchableProtection(DWORD protect)
{
    const DWORD base = protect & 0xff;
    return base == PAGE_EXECUTE_READ || base == PAGE_READONLY || base == PAGE_WRITECOPY || base == PAGE_READWRITE;
}

inline bool RestoreRegions(std::vector<ProtectedRegion>& regions)
{
    bool restored = true;
    for (auto it = regions.rbegin(); it != regions.rend(); ++it)
    {
        if (!it->writable)
            continue;

        DWORD ignored = 0;
        if (VirtualProtect(it->base, it->size, it->originalProtection, &ignored))
            it->writable = false;
        else
            restored = false;
    }
    return restored;
}

inline bool MakeRegionsWritable(std::vector<ProtectedRegion>& regions)
{
    bool writable = true;
    for (auto& region : regions)
    {
        if (region.writable)
            continue;

        DWORD currentProtection = 0;
        if (!VirtualProtect(region.base, region.size, PAGE_EXECUTE_READWRITE, &currentProtection))
        {
            writable = false;
            continue;
        }

        region.writable = true;
    }
    return writable;
}
} // namespace detail

inline TransactionResult ApplyTransaction(std::span<const PatchOperation> operations, PatchWriter writer = nullptr)
{
    TransactionResult result {};
    if (operations.empty())
        return result;

    std::vector<size_t> regionForOperation(operations.size(), 0);
    std::vector<detail::ProtectedRegion> regions;

    for (size_t index = 0; index < operations.size(); ++index)
    {
        const auto& operation = operations[index];
        if (operation.address == nullptr || operation.expected.empty() ||
            operation.expected.size() != operation.replacement.size() ||
            (operation.expected.size() != 1 && operation.expected.size() != 2 && operation.expected.size() != 4 &&
             operation.expected.size() != 8) ||
            std::memcmp(operation.address, operation.expected.data(), operation.expected.size()) != 0)
        {
            result.failedIndex = index;
            result.failedIndexValid = true;
            return result;
        }

        const uintptr_t start = reinterpret_cast<uintptr_t>(operation.address);
        const uintptr_t end = start + operation.expected.size();
        if (end < start)
        {
            result.failedIndex = index;
            result.failedIndexValid = true;
            return result;
        }

        for (size_t prior = 0; prior < index; ++prior)
        {
            const uintptr_t priorStart = reinterpret_cast<uintptr_t>(operations[prior].address);
            const uintptr_t priorEnd = priorStart + operations[prior].expected.size();
            if (start < priorEnd && priorStart < end)
            {
                result.failedIndex = index;
                result.failedIndexValid = true;
                return result;
            }
        }

        if (writer != nullptr)
            continue;

        MEMORY_BASIC_INFORMATION info {};
        if (VirtualQuery(operation.address, &info, sizeof(info)) != sizeof(info) || info.State != MEM_COMMIT ||
            !detail::IsPatchableProtection(info.Protect))
        {
            result.failedIndex = index;
            result.failedIndexValid = true;
            return result;
        }

        // VirtualQuery reports everything from this page to the end of the run, so regions of different patches
        // overlap. Only the patched pages are changed, and overlapping page ranges are merged into one region.
        constexpr uintptr_t pageMask = 0xFFF;
        const uintptr_t pageStart = start & ~pageMask;
        const uintptr_t pageEnd = (end + pageMask) & ~pageMask;
        const uintptr_t regionStart = reinterpret_cast<uintptr_t>(info.BaseAddress);
        const uintptr_t regionEnd = regionStart + info.RegionSize;
        if (pageEnd < end || regionEnd < regionStart || regionEnd < pageEnd)
        {
            result.failedIndex = index;
            result.failedIndexValid = true;
            return result;
        }

        bool merged = false;
        for (auto& region : regions)
        {
            const uintptr_t existingStart = reinterpret_cast<uintptr_t>(region.base);
            const uintptr_t existingEnd = existingStart + region.size;
            if (pageStart <= existingEnd && existingStart <= pageEnd && region.originalProtection == info.Protect)
            {
                const uintptr_t newStart = (std::min)(existingStart, pageStart);
                const uintptr_t newEnd = (std::max)(existingEnd, pageEnd);
                region.base = reinterpret_cast<void*>(newStart);
                region.size = newEnd - newStart;
                merged = true;
                break;
            }
        }
        if (!merged)
            regions.push_back({ reinterpret_cast<void*>(pageStart), pageEnd - pageStart, info.Protect, false });
    }

    // Merging can bridge two earlier regions, so fold until no pair touches.
    for (bool changed = true; changed;)
    {
        changed = false;
        for (size_t a = 0; a < regions.size() && !changed; ++a)
        {
            for (size_t b = a + 1; b < regions.size() && !changed; ++b)
            {
                const uintptr_t aStart = reinterpret_cast<uintptr_t>(regions[a].base);
                const uintptr_t aEnd = aStart + regions[a].size;
                const uintptr_t bStart = reinterpret_cast<uintptr_t>(regions[b].base);
                const uintptr_t bEnd = bStart + regions[b].size;
                if (aStart <= bEnd && bStart <= aEnd && regions[a].originalProtection == regions[b].originalProtection)
                {
                    const uintptr_t newStart = (std::min)(aStart, bStart);
                    regions[a].base = reinterpret_cast<void*>(newStart);
                    regions[a].size = (std::max)(aEnd, bEnd) - newStart;
                    regions.erase(regions.begin() + static_cast<std::ptrdiff_t>(b));
                    changed = true;
                }
            }
        }
    }

    if (writer == nullptr)
    {
        for (size_t index = 0; index < operations.size(); ++index)
        {
            const uintptr_t address = reinterpret_cast<uintptr_t>(operations[index].address);
            for (size_t regionIndex = 0; regionIndex < regions.size(); ++regionIndex)
            {
                const uintptr_t regionStart = reinterpret_cast<uintptr_t>(regions[regionIndex].base);
                if (address >= regionStart && address < regionStart + regions[regionIndex].size)
                    regionForOperation[index] = regionIndex;
            }
        }
    }

    for (auto& region : regions)
    {
        DWORD previousProtection = 0;
        if (!VirtualProtect(region.base, region.size, PAGE_EXECUTE_READWRITE, &previousProtection))
        {
            result.rollbackComplete = detail::RestoreRegions(regions);
            return result;
        }

        region.writable = true;
        if (!detail::IsPatchableProtection(previousProtection))
        {
            region.originalProtection = previousProtection;
            result.rollbackComplete = detail::RestoreRegions(regions);
            return result;
        }
        region.originalProtection = previousProtection;
    }

    size_t applied = 0;
    for (; applied < operations.size(); ++applied)
    {
        const auto& operation = operations[applied];
        const bool written = writer != nullptr
                                 ? writer(operation.address, operation.expected, operation.replacement)
                                 : CompareExchangeBytes(operation.address, operation.expected, operation.replacement);
        if (!written)
            break;
    }

    const bool allWritesApplied = applied == operations.size();
    if (!allWritesApplied)
    {
        result.failedIndex = applied;
        result.failedIndexValid = true;
        const size_t writtenCount = applied;
        while (applied > 0)
        {
            --applied;
            const auto& operation = operations[applied];
            const bool restored =
                writer != nullptr ? writer(operation.address, operation.replacement, operation.expected)
                                  : CompareExchangeBytes(operation.address, operation.replacement, operation.expected);
            result.rollbackComplete = result.rollbackComplete && restored;
        }
        if (writer == nullptr)
        {
            for (size_t index = 0; index < writtenCount; ++index)
                FlushInstructionCache(GetCurrentProcess(), operations[index].address,
                                      operations[index].replacement.size());
        }
    }
    else
    {
        result.applied = true;
    }

    if (!detail::RestoreRegions(regions))
    {
        if (allWritesApplied)
        {
            result.applied = false;
            result.failedIndex = operations.size();
            const bool allWritable = detail::MakeRegionsWritable(regions);
            bool bytesRestored = true;
            for (size_t index = operations.size(); index > 0; --index)
            {
                const size_t operationIndex = index - 1;
                const auto& operation = operations[operationIndex];
                const bool regionWritable = writer != nullptr || regions[regionForOperation[operationIndex]].writable;
                if (regionWritable)
                {
                    const bool restored =
                        writer != nullptr
                            ? writer(operation.address, operation.replacement, operation.expected)
                            : CompareExchangeBytes(operation.address, operation.replacement, operation.expected);
                    bytesRestored = bytesRestored && restored;
                }
                else
                {
                    bytesRestored = false;
                }
            }

            if (writer == nullptr)
            {
                for (const auto& operation : operations)
                    FlushInstructionCache(GetCurrentProcess(), operation.address, operation.replacement.size());
            }

            const bool protectionsRestored = detail::RestoreRegions(regions);
            result.rollbackComplete = result.rollbackComplete && allWritable && bytesRestored && protectionsRestored;
        }
        else
        {
            result.rollbackComplete = result.rollbackComplete && detail::RestoreRegions(regions);
        }
        return result;
    }

    if (result.applied && writer == nullptr)
        for (const auto& operation : operations)
            FlushInstructionCache(GetCurrentProcess(), operation.address, operation.replacement.size());

    return result;
}

class AppliedPatchSet
{
  public:
    TransactionResult Apply(std::span<const PatchOperation> operations, PatchWriter writer = nullptr)
    {
        if (!_operations.empty())
            return {};

        auto result = ApplyTransaction(operations, writer);
        if (result.applied || !result.rollbackComplete)
            _operations.assign(operations.begin(), operations.end());
        return result;
    }

    TransactionResult Restore(PatchWriter writer = nullptr)
    {
        if (_operations.empty())
            return {};

        std::vector<PatchOperation> inverse;
        inverse.reserve(_operations.size());
        size_t operationIndex = _operations.size();
        for (auto it = _operations.rbegin(); it != _operations.rend(); ++it)
        {
            --operationIndex;
            if (std::memcmp(it->address, it->expected.data(), it->expected.size()) == 0)
                continue;
            if (std::memcmp(it->address, it->replacement.data(), it->replacement.size()) != 0)
            {
                return { false, false, operationIndex };
            }
            inverse.push_back({ it->address, it->replacement, it->expected });
        }

        if (inverse.empty())
        {
            _operations.clear();
            return { true, true, 0 };
        }

        auto result = ApplyTransaction(inverse, writer);
        if (result.applied)
            _operations.clear();
        return result;
    }

    bool IsActive() const { return !_operations.empty(); }

    // True when the first operation's bytes are still the replacement. A module reloaded fresh at the same address
    // reads back the original bytes, so this tells a live patch from a stale bookkeeping entry. Reads under SEH is
    // the caller's job (the address may be unmapped); here the set is assumed to point at readable memory.
    bool StillApplied() const
    {
        if (_operations.empty())
            return false;
        const auto& first = _operations.front();
        return first.address != nullptr &&
               std::memcmp(first.address, first.replacement.data(), first.replacement.size()) == 0;
    }

  private:
    std::vector<PatchOperation> _operations;
};
} // namespace MfgUnlock
