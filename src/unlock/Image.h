// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/Image.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. 
#pragma once

#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <Windows.h>

#include <cstddef>
#include <cstdint>
#include <cstring>
#include <string_view>

namespace MfgUnlock
{
struct FunctionRange
{
    uint32_t begin = 0;
    uint32_t end = 0;
};

class ImageView
{
  public:
    ImageView(const void* image, size_t mappedSize)
        : _base(static_cast<const std::byte*>(image)), _mappedSize(mappedSize)
    {
        if (_base == nullptr || _mappedSize < sizeof(IMAGE_DOS_HEADER))
            return;

        IMAGE_DOS_HEADER dos {};
        std::memcpy(&dos, _base, sizeof(dos));
        if (dos.e_magic != IMAGE_DOS_SIGNATURE || dos.e_lfanew <= 0)
            return;

        const size_t ntOffset = static_cast<size_t>(dos.e_lfanew);
        constexpr size_t ntFixedSize = sizeof(DWORD) + sizeof(IMAGE_FILE_HEADER);
        if (!Contains(ntOffset, ntFixedSize))
            return;

        DWORD signature = 0;
        std::memcpy(&signature, _base + ntOffset, sizeof(signature));
        if (signature != IMAGE_NT_SIGNATURE)
            return;

        std::memcpy(&_fileHeader, _base + ntOffset + sizeof(DWORD), sizeof(_fileHeader));
        if (_fileHeader.Machine != IMAGE_FILE_MACHINE_AMD64 ||
            _fileHeader.SizeOfOptionalHeader < sizeof(IMAGE_OPTIONAL_HEADER64))
            return;

        const size_t optionalOffset = ntOffset + ntFixedSize;
        if (!Contains(optionalOffset, sizeof(IMAGE_OPTIONAL_HEADER64)))
            return;

        std::memcpy(&_optionalHeader, _base + optionalOffset, sizeof(_optionalHeader));
        if (_optionalHeader.Magic != IMAGE_NT_OPTIONAL_HDR64_MAGIC || _optionalHeader.SizeOfImage == 0 ||
            _optionalHeader.SizeOfImage > _mappedSize || _optionalHeader.SizeOfHeaders > _optionalHeader.SizeOfImage ||
            _optionalHeader.NumberOfRvaAndSizes > IMAGE_NUMBEROF_DIRECTORY_ENTRIES)
            return;

        _imageSize = _optionalHeader.SizeOfImage;
        _sectionsOffset = optionalOffset + _fileHeader.SizeOfOptionalHeader;
        const size_t sectionBytes = static_cast<size_t>(_fileHeader.NumberOfSections) * sizeof(IMAGE_SECTION_HEADER);
        if (!RvaContains(static_cast<uint32_t>(_sectionsOffset), sectionBytes))
            return;

        _valid = true;
    }

    bool IsValid() const { return _valid; }

    const std::byte* Base() const { return _base; }

    size_t ImageSize() const { return _imageSize; }

    WORD SectionCount() const { return _valid ? _fileHeader.NumberOfSections : 0; }

    bool ReadAtRva(uint32_t rva, void* destination, size_t size) const
    {
        if (!_valid || destination == nullptr || !RvaContains(rva, size))
            return false;

        std::memcpy(destination, _base + rva, size);
        return true;
    }

    const std::byte* DataAtRva(uint32_t rva, size_t size) const
    {
        return _valid && RvaContains(rva, size) ? _base + rva : nullptr;
    }

    bool SectionAt(WORD index, IMAGE_SECTION_HEADER& section) const
    {
        if (!_valid || index >= _fileHeader.NumberOfSections)
            return false;

        const size_t offset = _sectionsOffset + static_cast<size_t>(index) * sizeof(section);
        std::memcpy(&section, _base + offset, sizeof(section));
        return RvaContains(section.VirtualAddress, section.Misc.VirtualSize);
    }

    bool FindExport(std::string_view name, uint32_t& functionRva) const
    {
        functionRva = 0;
        if (!_valid || name.empty() || _optionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXPORT)
            return false;

        const auto& directory = _optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT];
        IMAGE_EXPORT_DIRECTORY exports {};
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(exports) ||
            !ReadAtRva(directory.VirtualAddress, &exports, sizeof(exports)) || exports.NumberOfNames == 0 ||
            exports.NumberOfNames > exports.NumberOfFunctions || exports.NumberOfNames > _imageSize / sizeof(DWORD))
            return false;

        for (DWORD index = 0; index < exports.NumberOfNames; ++index)
        {
            DWORD nameRva = 0;
            WORD ordinal = 0;
            DWORD targetRva = 0;
            const uint64_t nameEntry =
                static_cast<uint64_t>(exports.AddressOfNames) + static_cast<uint64_t>(index) * sizeof(DWORD);
            const uint64_t ordinalEntry =
                static_cast<uint64_t>(exports.AddressOfNameOrdinals) + static_cast<uint64_t>(index) * sizeof(WORD);
            if (nameEntry > UINT32_MAX || ordinalEntry > UINT32_MAX ||
                !ReadAtRva(static_cast<uint32_t>(nameEntry), &nameRva, sizeof(nameRva)) ||
                !ReadAtRva(static_cast<uint32_t>(ordinalEntry), &ordinal, sizeof(ordinal)) ||
                ordinal >= exports.NumberOfFunctions)
                return false;

            const uint64_t functionEntry =
                static_cast<uint64_t>(exports.AddressOfFunctions) + static_cast<uint64_t>(ordinal) * sizeof(DWORD);
            if (functionEntry > UINT32_MAX ||
                !ReadAtRva(static_cast<uint32_t>(functionEntry), &targetRva, sizeof(targetRva)))
                return false;

            if (!RvaContains(nameRva, name.size() + 1))
                continue;

            const char* candidate = reinterpret_cast<const char*>(_base + nameRva);
            if (std::memcmp(candidate, name.data(), name.size()) == 0 && candidate[name.size()] == '\0')
            {
                const uint64_t exportEnd = static_cast<uint64_t>(directory.VirtualAddress) + directory.Size;
                if (targetRva == 0 || !RvaContains(targetRva, 1) ||
                    (targetRva >= directory.VirtualAddress && targetRva < exportEnd))
                    return false;

                functionRva = targetRva;
                return true;
            }
        }

        return false;
    }

    bool FindFunctionRange(uint32_t rva, FunctionRange& range) const
    {
        range = {};
        const size_t count = RuntimeFunctionCount();
        if (count == 0)
            return false;

        for (size_t index = 0; index < count; ++index)
        {
            IMAGE_RUNTIME_FUNCTION_ENTRY entry {};
            if (!RuntimeFunctionAt(index, entry))
                return false;

            if (rva >= entry.BeginAddress && rva < entry.EndAddress)
            {
                if (range.begin != 0)
                    return false;
                range = { entry.BeginAddress, entry.EndAddress };
            }
        }

        return range.begin != 0;
    }

    size_t RuntimeFunctionCount() const
    {
        if (!_valid || _optionalHeader.NumberOfRvaAndSizes <= IMAGE_DIRECTORY_ENTRY_EXCEPTION)
            return 0;

        const auto& directory = _optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        if (directory.VirtualAddress == 0 || directory.Size < sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) ||
            directory.Size % sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) != 0 ||
            !RvaContains(directory.VirtualAddress, directory.Size))
            return 0;

        return directory.Size / sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
    }

    bool RuntimeFunctionAt(size_t index, IMAGE_RUNTIME_FUNCTION_ENTRY& entry) const
    {
        const size_t count = RuntimeFunctionCount();
        if (index >= count)
            return false;

        const auto& directory = _optionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION];
        const uint64_t entryRva = static_cast<uint64_t>(directory.VirtualAddress) +
                                  static_cast<uint64_t>(index) * sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY);
        if (entryRva > UINT32_MAX || !ReadAtRva(static_cast<uint32_t>(entryRva), &entry, sizeof(entry)) ||
            entry.BeginAddress >= entry.EndAddress ||
            !RvaContains(entry.BeginAddress, entry.EndAddress - entry.BeginAddress))
            return false;

        return true;
    }

    bool IsExecutableReadOnly(uint32_t rva, size_t size) const
    {
        if (!_valid || size == 0 || !RvaContains(rva, size))
            return false;

        const uint64_t end = static_cast<uint64_t>(rva) + size;
        bool inExecutableSection = false;
        for (WORD index = 0; index < _fileHeader.NumberOfSections; ++index)
        {
            IMAGE_SECTION_HEADER section {};
            if (!SectionAt(index, section))
                return false;

            const uint64_t sectionEnd = static_cast<uint64_t>(section.VirtualAddress) + section.Misc.VirtualSize;
            if (rva >= section.VirtualAddress && end <= sectionEnd)
            {
                inExecutableSection = (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 &&
                                      (section.Characteristics & IMAGE_SCN_MEM_READ) != 0 &&
                                      (section.Characteristics & IMAGE_SCN_MEM_WRITE) == 0;
                break;
            }
        }

        if (!inExecutableSection)
            return false;

        uintptr_t current = reinterpret_cast<uintptr_t>(_base + rva);
        const uintptr_t last = current + size;
        if (last < current)
            return false;

        while (current < last)
        {
            MEMORY_BASIC_INFORMATION info {};
            if (VirtualQuery(reinterpret_cast<const void*>(current), &info, sizeof(info)) != sizeof(info) ||
                info.State != MEM_COMMIT || (info.Protect & 0xff) != PAGE_EXECUTE_READ ||
                (info.Protect & (PAGE_GUARD | PAGE_NOCACHE | PAGE_WRITECOMBINE)) != 0)
                return false;

            const uintptr_t regionEnd = reinterpret_cast<uintptr_t>(info.BaseAddress) + info.RegionSize;
            if (regionEnd <= current)
                return false;
            current = regionEnd < last ? regionEnd : last;
        }

        return true;
    }

  private:
    bool Contains(size_t offset, size_t size) const { return offset <= _mappedSize && size <= _mappedSize - offset; }

    bool RvaContains(uint32_t rva, size_t size) const
    {
        return static_cast<size_t>(rva) <= _imageSize && size <= _imageSize - static_cast<size_t>(rva);
    }

    const std::byte* _base = nullptr;
    size_t _mappedSize = 0;
    size_t _imageSize = 0;
    size_t _sectionsOffset = 0;
    IMAGE_FILE_HEADER _fileHeader {};
    IMAGE_OPTIONAL_HEADER64 _optionalHeader {};
    bool _valid = false;
};
} // namespace MfgUnlock
