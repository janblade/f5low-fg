// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/FamilyFatbin.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. 
#pragma once

#include "FamilyRtx40.h"
#include "Image.h"
#include "PtxTuring.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// RTX 40 midpoint fix. nvngx_dlssg carries two builds of its interpolation kernels in CUDA fatbin containers: sm_89
// (Ada) and sm_120 (Blackwell). The Ada kernel places every generated frame at the same point between the two real
// ones, so above 2X the world does not advance between generated frames. The Blackwell PTX uses nothing Ada lacks, so
// per container the Blackwell PTX is relabelled sm_89 (its image header and its ".target" line, same length) and the
// images that really were sm_89 are relabelled to an architecture that does not exist, so the driver JITs the
// Blackwell kernel when it asks for Ada. Only bytes inside the existing containers change; nothing changes length.
// A container missing either image, or whose ".target" bytes do not decode as one literal run of the (LZ4) PTX
// payload, is left alone.
namespace MfgUnlock::FamilyRtx40::Fatbin
{
inline constexpr uint32_t kArchAda = 89;
inline constexpr uint32_t kArchBlackwell = 120;
inline constexpr uint32_t kArchParked = 122; // no such shader model

inline constexpr size_t kContainerHeader = 16;
inline constexpr size_t kImagePayloadSize = 8;
inline constexpr size_t kImageArch = 28;
inline constexpr uint16_t kKindPtx = 1;

inline constexpr std::string_view kTargetFrom = ".target sm_120";
inline constexpr std::string_view kTargetTo = ".target sm_89 ";
static_assert(kTargetFrom.size() == kTargetTo.size());

// RTX 30 (Ampere, sm_86). The shipped PTX is portable, so lowering its ".target" is enough for the driver to JIT it
// for Ampere; verified for every container of every provider build with tests/mfgunlock_ptx_jit_probe. Unlike the
// Ada fix every container is needed (the network modules only ship an sm_89 PTX), so one that does not qualify makes
// the whole report incomplete.
inline constexpr uint32_t kArchAmpere = 86;
inline constexpr std::string_view kAdaTarget = ".target sm_89";
inline constexpr std::string_view kAmpereTargetFromBlackwell = ".target sm_86 ";
inline constexpr std::string_view kAmpereTargetFromAda = ".target sm_86";
static_assert(kAmpereTargetFromBlackwell.size() == kTargetFrom.size());
static_assert(kAmpereTargetFromAda.size() == kAdaTarget.size());

enum class Target
{
    Ada,
    Ampere,
};

struct Report
{
    std::vector<LocatedPatch> patches;
    unsigned containers = 0;
    unsigned rejected = 0;
    bool requireEveryContainer = false;
    std::string failure;

    bool Complete() const
    {
        return failure.empty() && containers != 0 && !patches.empty() && (!requireEveryContainer || rejected == 0) &&
               std::all_of(patches.begin(), patches.end(),
                           [](const LocatedPatch& patch) { return patch.state == LocateState::Found; });
    }
};


namespace detail
{
inline std::vector<std::byte> U32(uint32_t value)
{
    std::vector<std::byte> bytes(sizeof(value));
    std::memcpy(bytes.data(), &value, sizeof(value));
    return bytes;
}

inline std::vector<std::byte> Text(std::string_view text)
{
    std::vector<std::byte> bytes(text.size());
    std::memcpy(bytes.data(), text.data(), text.size());
    return bytes;
}

inline void Add(Report& report, std::string name, uint32_t rva, std::vector<std::byte> expected,
                std::vector<std::byte> replacement)
{
    LocatedPatch patch;
    patch.name = std::move(name);
    patch.state = LocateState::Found;
    patch.rva = rva;
    patch.expected = std::move(expected);
    patch.replacement = std::move(replacement);
    report.patches.push_back(std::move(patch));
}

inline bool EndsDirective(uint8_t next)
{
    return next == ' ' || next == '\t' || next == '\r' || next == '\n' || next == ',';
}

// Where an image header says its payload is compressed (compressed size at +16, uncompressed at +0x38, in the 0x40-byte
// header form).
inline constexpr size_t kImageCompressed = 16;
inline constexpr size_t kImageUncompressed = 0x38;
inline constexpr size_t kImageHeaderWithSizes = 0x40;

struct Payload
{
    uint32_t compressed = 0;
    uint64_t uncompressed = 0;
};

inline Payload ReadPayloadSizes(const std::byte* header, uint64_t headerSize)
{
    Payload sizes;
    if (headerSize >= kImageHeaderWithSizes)
    {
        std::memcpy(&sizes.compressed, header + kImageCompressed, sizeof(sizes.compressed));
        std::memcpy(&sizes.uncompressed, header + kImageUncompressed, sizeof(sizes.uncompressed));
    }
    return sizes;
}

// A ".target" edit writes text bytes straight into the payload. For an LZ4 payload that is only right when those bytes
// are one literal run that nothing later copies: the payload is decoded as it is and with the edit, and the edit is
// accepted only when the decoded texts differ in exactly the one directive, from -> to. A plain payload is the text.
inline bool EditKeepsText(const std::byte* payload, uint64_t payloadSize, const Payload& sizes, size_t at,
                          std::string_view from, std::string_view to)
{
    if (sizes.compressed == 0 || sizes.uncompressed == 0)
        return true;
    if (sizes.compressed > payloadSize || sizes.uncompressed > (64ull << 20) || from.size() != to.size() ||
        at + from.size() > sizes.compressed)
        return false;

    const auto* source = reinterpret_cast<const uint8_t*>(payload);
    std::vector<uint8_t> before;
    if (!PtxTuring::detail::Lz4Decompress(source, sizes.compressed, before, static_cast<size_t>(sizes.uncompressed)))
        return false;
    std::vector<uint8_t> edited(source, source + sizes.compressed);
    std::memcpy(edited.data() + at, to.data(), to.size());
    std::vector<uint8_t> after;
    if (!PtxTuring::detail::Lz4Decompress(edited.data(), edited.size(), after, before.size()))
        return false;

    size_t first = before.size();
    size_t last = 0;
    for (size_t index = 0; index < before.size(); ++index)
    {
        if (before[index] != after[index])
        {
            first = std::min(first, index);
            last = index;
        }
    }
    if (first == before.size())
        return false;
    // The differing bytes have to sit inside one copy of `from` that became `to`.
    const size_t lowest = first + 1 >= from.size() ? first + 1 - from.size() : 0;
    for (size_t start = lowest; start <= first; ++start)
    {
        if (last >= start + from.size() || start + from.size() > before.size())
            continue;
        if (std::memcmp(before.data() + start, from.data(), from.size()) == 0 &&
            std::memcmp(after.data() + start, to.data(), to.size()) == 0)
            return true;
    }
    return false;
}

// Appends the patches of one container; returns false when the container does not qualify.
inline bool Collect(Report& report, const ImageView& image, uint32_t containerRva, uint64_t fatSize)
{
    const uint64_t end = static_cast<uint64_t>(containerRva) + kContainerHeader + fatSize;
    if (end > UINT32_MAX)
        return false;

    uint32_t blackwellRva = 0;
    uint64_t blackwellHeader = 0;
    uint64_t blackwellPayload = 0;
    Payload blackwellSizes;
    unsigned blackwellCount = 0;
    std::vector<uint32_t> adaImages;

    for (uint64_t cursor = containerRva + kContainerHeader; cursor < end;)
    {
        const auto* header = image.DataAtRva(static_cast<uint32_t>(cursor), kImageArch + sizeof(uint32_t));
        if (header == nullptr)
            return false;

        uint16_t kind = 0;
        uint32_t imageHeader = 0;
        uint64_t payload = 0;
        uint32_t arch = 0;
        std::memcpy(&kind, header, sizeof(kind));
        std::memcpy(&imageHeader, header + 4, sizeof(imageHeader));
        std::memcpy(&payload, header + kImagePayloadSize, sizeof(payload));
        std::memcpy(&arch, header + kImageArch, sizeof(arch));

        const uint64_t remaining = end - cursor;
        if (imageHeader < kImageArch + sizeof(uint32_t) || imageHeader > remaining || payload == 0 ||
            payload > remaining - imageHeader)
            return false;

        if (kind == kKindPtx && arch == kArchBlackwell)
        {
            blackwellRva = static_cast<uint32_t>(cursor);
            blackwellHeader = imageHeader;
            blackwellPayload = payload;
            const auto* fullHeader = image.DataAtRva(static_cast<uint32_t>(cursor), static_cast<size_t>(imageHeader));
            if (fullHeader == nullptr)
                return false;
            blackwellSizes = ReadPayloadSizes(fullHeader, imageHeader);
            ++blackwellCount;
        }
        else if (arch == kArchAda)
        {
            adaImages.push_back(static_cast<uint32_t>(cursor));
        }
        cursor += imageHeader + payload;
    }

    if (blackwellCount != 1 || adaImages.empty())
        return false;

    const uint32_t bodyRva = static_cast<uint32_t>(blackwellRva + blackwellHeader);
    const auto* body = image.DataAtRva(bodyRva, static_cast<size_t>(blackwellPayload));
    if (body == nullptr)
        return false;

    const auto* chars = reinterpret_cast<const char*>(body);
    const std::string_view text(chars, static_cast<size_t>(blackwellPayload));
    const size_t at = text.find(kTargetFrom);
    if (at == std::string_view::npos || at + kTargetFrom.size() >= text.size() ||
        !EndsDirective(static_cast<uint8_t>(text[at + kTargetFrom.size()])) ||
        !EditKeepsText(body, blackwellPayload, blackwellSizes, at, kTargetFrom, kTargetTo))
        return false;

    const std::string tag = "container@" + std::to_string(containerRva);
    Add(report, tag + " ptx target", static_cast<uint32_t>(bodyRva + at), Text(kTargetFrom), Text(kTargetTo));
    Add(report, tag + " blackwell arch", blackwellRva + static_cast<uint32_t>(kImageArch), U32(kArchBlackwell),
        U32(kArchAda));
    for (const auto rva : adaImages)
        Add(report, tag + " ada arch", rva + static_cast<uint32_t>(kImageArch), U32(kArchAda), U32(kArchParked));
    return true;
}

// Appends the patches that make one container JIT for Ampere; returns false when the container does not qualify.
// Source PTX: the Blackwell one where there is one (it also carries the corrected interpolation), else the Ada one.
// Every other Ada or Blackwell image is relabelled to an architecture that does not exist so the driver cannot pick it.
inline bool CollectAmpere(Report& report, const ImageView& image, uint32_t containerRva, uint64_t fatSize)
{
    const uint64_t end = static_cast<uint64_t>(containerRva) + kContainerHeader + fatSize;
    if (end > UINT32_MAX)
        return false;

    struct Entry
    {
        uint32_t rva = 0;
        uint16_t kind = 0;
        uint32_t arch = 0;
        uint64_t header = 0;
        uint64_t payload = 0;
        Payload sizes;
    };
    std::vector<Entry> entries;

    for (uint64_t cursor = containerRva + kContainerHeader; cursor < end;)
    {
        const auto* header = image.DataAtRva(static_cast<uint32_t>(cursor), kImageArch + sizeof(uint32_t));
        if (header == nullptr)
            return false;

        Entry entry;
        entry.rva = static_cast<uint32_t>(cursor);
        std::memcpy(&entry.kind, header, sizeof(entry.kind));
        uint32_t imageHeader = 0;
        std::memcpy(&imageHeader, header + 4, sizeof(imageHeader));
        std::memcpy(&entry.payload, header + kImagePayloadSize, sizeof(entry.payload));
        std::memcpy(&entry.arch, header + kImageArch, sizeof(entry.arch));
        entry.header = imageHeader;

        const uint64_t remaining = end - cursor;
        if (imageHeader < kImageArch + sizeof(uint32_t) || imageHeader > remaining || entry.payload == 0 ||
            entry.payload > remaining - imageHeader)
            return false;
        // An architecture other than Ada or Blackwell (a ready-made sm_86 image, say) is not understood here.
        if (entry.arch != kArchAda && entry.arch != kArchBlackwell)
            return false;
        const auto* fullHeader = image.DataAtRva(entry.rva, static_cast<size_t>(imageHeader));
        if (fullHeader == nullptr)
            return false;
        entry.sizes = ReadPayloadSizes(fullHeader, imageHeader);
        entries.push_back(entry);
        cursor += imageHeader + entry.payload;
    }

    const Entry* source = nullptr;
    for (const uint32_t wanted : { kArchBlackwell, kArchAda })
    {
        unsigned count = 0;
        for (const auto& entry : entries)
        {
            if (entry.kind == kKindPtx && entry.arch == wanted)
            {
                source = &entry;
                ++count;
            }
        }
        if (count == 1)
            break;
        if (count > 1)
            return false;
        source = nullptr;
    }
    if (source == nullptr)
        return false;

    const bool fromBlackwell = source->arch == kArchBlackwell;
    const std::string_view from = fromBlackwell ? kTargetFrom : kAdaTarget;
    const std::string_view to = fromBlackwell ? kAmpereTargetFromBlackwell : kAmpereTargetFromAda;

    const uint32_t bodyRva = static_cast<uint32_t>(source->rva + source->header);
    const auto* body = image.DataAtRva(bodyRva, static_cast<size_t>(source->payload));
    if (body == nullptr)
        return false;

    // The text sits in the compressed stream's first literal run (checked by decoding below); a second hit would be a
    // different directive.
    const std::string_view text(reinterpret_cast<const char*>(body), static_cast<size_t>(source->payload));
    constexpr std::string_view anyTarget = ".target sm_";
    size_t hits = 0;
    size_t at = std::string_view::npos;
    for (size_t scan = 0; (scan = text.find(anyTarget, scan)) != std::string_view::npos; ++scan)
    {
        ++hits;
        at = scan;
    }
    if (hits != 1 || text.compare(at, from.size(), from) != 0 || at + from.size() >= text.size() ||
        !EndsDirective(static_cast<uint8_t>(text[at + from.size()])) ||
        !EditKeepsText(body, source->payload, source->sizes, at, from, to))
        return false;

    const std::string tag = "container@" + std::to_string(containerRva);
    Add(report, tag + " ptx target", static_cast<uint32_t>(bodyRva + at), Text(from), Text(to));
    Add(report, tag + " source arch", source->rva + static_cast<uint32_t>(kImageArch), U32(source->arch),
        U32(kArchAmpere));
    for (const auto& entry : entries)
        if (&entry != source)
            Add(report, tag + " parked arch", entry.rva + static_cast<uint32_t>(kImageArch), U32(entry.arch),
                U32(kArchParked));
    return true;
}
} // namespace detail

// Calls visit(containerRva, fatSize) for every fatbin container header in the readable, non-executable sections. A
// container's bytes are skipped when visit returns true (they are its images, not further containers).
template <typename Visit> inline void ForEachContainer(const ImageView& image, Visit&& visit)
{
    constexpr uint8_t magic[] = { 0x50, 0xED, 0x55, 0xBA };
    for (WORD index = 0; index < image.SectionCount(); ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!image.SectionAt(index, section) || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) != 0 ||
            (section.Characteristics & IMAGE_SCN_MEM_READ) == 0)
            continue;

        const uint32_t size = section.Misc.VirtualSize;
        const auto* data = image.DataAtRva(section.VirtualAddress, size);
        if (data == nullptr)
            continue;

        for (uint32_t offset = 0; offset + kContainerHeader <= size; ++offset)
        {
            if (std::memcmp(data + offset, magic, sizeof(magic)) != 0)
                continue;

            uint16_t headerSize = 0;
            uint64_t fatSize = 0;
            std::memcpy(&headerSize, data + offset + 6, sizeof(headerSize));
            std::memcpy(&fatSize, data + offset + 8, sizeof(fatSize));
            if (headerSize != kContainerHeader || fatSize == 0 || fatSize > size - offset - kContainerHeader)
                continue;

            if (visit(section.VirtualAddress + offset, fatSize))
                offset += static_cast<uint32_t>(kContainerHeader + fatSize) - 1;
        }
    }
}

// The fatbin containers of an image (header RVAs), whole and inside their section.
inline std::vector<uint32_t> ContainerRvas(const ImageView& image)
{
    std::vector<uint32_t> rvas;
    if (image.IsValid())
        ForEachContainer(image,
                         [&rvas](uint32_t rva, uint64_t)
                         {
                             rvas.push_back(rva);
                             return true;
                         });
    return rvas;
}

inline Report Locate(const ImageView& image, Target target = Target::Ada)
{
    Report report;
    report.requireEveryContainer = target == Target::Ampere;
    if (!image.IsValid())
    {
        report.failure = "invalid image";
        return report;
    }

    ForEachContainer(image,
                     [&](uint32_t containerRva, uint64_t fatSize)
                     {
                         const size_t before = report.patches.size();
                         const bool qualifies = target == Target::Ampere
                                                    ? detail::CollectAmpere(report, image, containerRva, fatSize)
                                                    : detail::Collect(report, image, containerRva, fatSize);
                         if (qualifies)
                         {
                             ++report.containers;
                             return true;
                         }
                         report.patches.resize(before);
                         ++report.rejected;
                         return false;
                     });

    if (report.containers == 0)
        report.failure = target == Target::Ampere ? "no fatbin container that can be retargeted to sm_86"
                                                  : "no fatbin container with both a Blackwell PTX and an Ada image";
    else if (report.requireEveryContainer && report.rejected != 0)
        report.failure = std::to_string(report.rejected) + " fatbin container(s) could not be retargeted to sm_86";
    return report;
}

inline std::vector<PatchOperation> BuildOperations(const Report& report, const ImageView& image)
{
    if (!report.Complete())
        return {};
    return BuildOperationsFromPatches(report.patches, image);
}
} // namespace MfgUnlock::FamilyRtx40::Fatbin
