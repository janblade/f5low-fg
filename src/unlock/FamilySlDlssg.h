// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/FamilySlDlssg.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: nothing.
// GPL-3.0 with the rest of this repo. Adapted parts are MIT (KleberMotta / Dreamt / mavismmg): see licenses/MFGUnlock_LICENSE.txt.

#pragma once

#include "FamilyRtx40.h"

// RTX 40 side of the Streamline DLSS-G plugin (sl.dlss_g), two independent derivations that must both succeed:
//  - the frame-count clamp: `mov edx, N / cmp ecx, edx / cmovb edx, ecx` becomes `cmovb edx, edx` (one ModRM byte), so
//    the count NGX reports is not lowered back to the plugin's compiled maximum;
//  - software pacing: the plugin has a fallback for a missing flip-metering DLL that logs "FG1 DLL has been detected"
//    and writes one byte of its context. That (offset, value) pair is read from the fallback itself and every other
//    byte store to the offset is pinned to the same value, so hardware flip metering cannot freeze more than 2x. The
//    member initialisation (a cleared register stored once) is left as it is, although it writes 0 (hardware flip
//    metering): that rests on the construction pattern, not on a proven order with the pinned stores. Any other store
//    that cannot be pinned in place stands the family aside.
// Adapted from KleberMotta/OptiScaler-DLSS5-MFG-RTX40 (MIT, from the RenoDX MFG Unlock addon). See
// Licenses/MFGUnlock_LICENSE.txt.
namespace MfgUnlock::FamilyRtx40::Streamline
{
inline constexpr std::string_view kFlipMarker = "FG1 DLL has been detected";
inline constexpr uint32_t kFlipWindow = 0x200;
inline constexpr uint32_t kMinField = 0x100;
inline constexpr uint32_t kMaxField = 0x20000;
inline constexpr size_t kStoreLength = 7;
inline constexpr size_t kMaxSites = 16;

struct Report
{
    std::vector<LocatedPatch> patches;
    uint8_t compiledCeiling = 0;
    uint32_t flipField = 0;
    uint8_t flipValue = 0;
    // Zero-initialising register stores to the state, left unpinned: they write 0 (hardware flip metering) and are
    // accepted by the construction pattern only, not a proven order with the pinned stores (see LocateFlip).
    unsigned initialisers = 0;
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
inline constexpr std::string_view kCeilingBytes("\xBA\x00\x00\x00\x00\x3B\xCA\x0F\x42\xD1", 10);
inline constexpr std::string_view kCeilingMask = "x???xxxxxx";
inline constexpr MaskedPattern kCeiling { kCeilingBytes, kCeilingMask };

inline bool CeilingAnchor(const ImageView& image, uint32_t rva, void*)
{
    const auto* bytes = image.DataAtRva(rva, kCeilingBytes.size());
    if (bytes == nullptr)
        return false;
    const auto count = std::to_integer<uint8_t>(bytes[1]);
    const bool zeroUpper = bytes[2] == std::byte { 0 } && bytes[3] == std::byte { 0 } && bytes[4] == std::byte { 0 };
    FunctionRange function {};
    return zeroUpper && count >= 1 && count <= 8 && image.FindFunctionRange(rva, function) && rva >= function.begin &&
           rva + kCeilingBytes.size() <= function.end;
}

// mov byte ptr [reg+disp32], x with mod = 10 and no SIB.
inline bool IsDisp32NoSib(uint8_t modrm)
{
    return modrm >= 0x80 && modrm <= 0xBF && (modrm & 7) != 4;
}

inline uint32_t ReadU32(const std::byte* bytes)
{
    uint32_t value = 0;
    std::memcpy(&value, bytes, sizeof(value));
    return value;
}

enum class Walk
{
    Start,    // the linear walk lands on rva
    NotStart, // the walk decoded cleanly past rva: rva is inside another instruction
    Unknown   // no unwind function, or the walk could not decode its way to rva (VEX, data, desync)
};

// Decodes linearly from the start of the unwind function holding rva.
inline Walk WalkToInstruction(const ImageView& image, uint32_t rva)
{
    FunctionRange function {};
    if (!image.FindFunctionRange(rva, function) || rva < function.begin || rva >= function.end)
        return Walk::Unknown;
    for (uint32_t cursor = function.begin; cursor < function.end;)
    {
        if (cursor == rva)
            return Walk::Start;
        if (cursor > rva)
            return Walk::NotStart;
        const uint32_t room = function.end - cursor;
        const size_t take = room < 15 ? room : 15;
        const auto* bytes = image.DataAtRva(cursor, take);
        if (bytes == nullptr)
            return Walk::Unknown;
        uint8_t copy[16] {};
        std::memcpy(copy, bytes, take);
        hde64s decoded {};
        hde64_disasm(copy, &decoded);
        if ((decoded.flags & F_ERROR) != 0 || decoded.len == 0 || decoded.len > take)
            return Walk::Unknown;
        cursor += decoded.len;
    }
    return Walk::Unknown;
}

// True only when rva is provably an instruction start (fail closed for the fallback's own store).
inline bool IsInstructionStart(const ImageView& image, uint32_t rva)
{
    return WalkToInstruction(image, rva) == Walk::Start;
}

inline std::string Hex(uint32_t value)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    for (int shift = 28; shift >= 0; shift -= 4)
        if (!text.empty() || (value >> shift) != 0 || shift == 0)
            text += digits[(value >> shift) & 15];
    return text;
}

inline std::string HexBytes(const ImageView& image, uint32_t rva, size_t length)
{
    constexpr char digits[] = "0123456789ABCDEF";
    std::string text;
    const auto* bytes = image.DataAtRva(rva, length);
    for (size_t index = 0; bytes != nullptr && index < length; ++index)
    {
        const auto value = std::to_integer<uint8_t>(bytes[index]);
        if (index != 0)
            text += ' ';
        text += digits[value >> 4];
        text += digits[value & 15];
    }
    return text;
}

struct ByteStore
{
    uint32_t rva = 0;
    hde64s decoded {};
    bool knownInstruction = true; // false: the function walk could not reach it, so not provably an instruction
    uint32_t width = 0;           // bytes written to the memory operand
    uint32_t disp = 0;            // the memory displacement [base + disp]
};

// Bytes an instruction writes to its memory operand, or 0 when it is not a memory write this recognises (a read, a
// register-only form, or an opcode not listed). SSE stores are over-estimated at 16 bytes, which only widens the
// range checked for coverage, so it fails closed.
inline uint32_t MemoryWriteWidth(const hde64s& d)
{
    if ((d.flags & F_MODRM) == 0 || d.modrm_mod == 3)
        return 0;
    const uint32_t wide = (d.flags & F_PREFIX_66) != 0 ? 2u : (d.rex_w != 0 ? 8u : 4u);
    const uint8_t reg = d.modrm_reg;
    if (d.opcode == 0x0F)
    {
        const uint8_t o2 = d.opcode2;
        if ((o2 >= 0x90 && o2 <= 0x9F) || o2 == 0xB0 || o2 == 0xC0)
            return 1; // setcc / cmpxchg r/m8 / xadd r/m8
        if (o2 == 0xB1 || o2 == 0xC1)
            return wide; // cmpxchg / xadd
        if (o2 == 0x11 || o2 == 0x13 || o2 == 0x17 || o2 == 0x29 || o2 == 0x2B || o2 == 0x7F || o2 == 0xD6 ||
            o2 == 0xE7)
            return 16; // movups/movlps/movhps/movaps/movntps/movdqa/movq/movntdq stores
        return 0;
    }
    switch (d.opcode)
    {
    case 0xC6:                                                                        // mov m8, imm8
    case 0x88:                                                                        // mov m8, r8
    case 0x86:                                                                        // xchg m8, r8
    case 0x00: case 0x08: case 0x10: case 0x18: case 0x20: case 0x28: case 0x30:      // alu m8, r8 (0x38 cmp reads)
    case 0xC0: case 0xD0: case 0xD2:                                                  // shift/rotate m8
        return 1;
    case 0x80:
        return reg != 7 ? 1u : 0u; // grp1 m8, imm8 (cmp reads)
    case 0xFE:
        return reg <= 1 ? 1u : 0u; // inc/dec m8
    case 0xF6:
        return (reg == 2 || reg == 3) ? 1u : 0u; // not/neg m8
    case 0xC7:                                                                        // mov m, imm
    case 0x89:                                                                        // mov m, r
    case 0x87:                                                                        // xchg m, r
    case 0x01: case 0x09: case 0x11: case 0x19: case 0x21: case 0x29: case 0x31:      // alu m, r (0x39 cmp reads)
    case 0xC1: case 0xD1: case 0xD3:                                                  // shift/rotate m
        return wide;
    case 0x81: case 0x83:
        return reg != 7 ? wide : 0u; // grp1 imm (cmp reads)
    case 0xFF:
        return reg <= 1 ? wide : 0u; // inc/dec m
    case 0xF7:
        return (reg == 2 || reg == 3) ? wide : 0u; // not/neg m
    default:
        return 0;
    }
}

// The memory-writing instruction whose disp32 sits at dispRva (mod 10, with or without SIB), with the bytes it writes.
// Reads and register forms are not reported. `back` spans the longest prefix+opcode+modrm+sib run before a disp32.
inline bool FindCoveringStore(const ImageView& image, uint32_t dispRva, ByteStore& store)
{
    for (uint32_t back = 2; back <= 11 && back <= dispRva; ++back)
    {
        const uint32_t start = dispRva - back;
        const size_t room = image.ImageSize() - start;
        const size_t take = room < 15 ? room : 15;
        const auto* bytes = image.DataAtRva(start, take);
        if (bytes == nullptr)
            continue;
        uint8_t copy[16] {};
        std::memcpy(copy, bytes, take);
        hde64s decoded {};
        hde64_disasm(copy, &decoded);
        if ((decoded.flags & F_ERROR) != 0 || decoded.len == 0 || decoded.len > take ||
            (decoded.flags & F_DISP32) == 0 || decoded.modrm_mod != 2)
            continue;
        const uint32_t immediate = (decoded.flags & F_IMM8) != 0    ? 1
                                   : (decoded.flags & F_IMM16) != 0 ? 2
                                   : (decoded.flags & F_IMM32) != 0 ? 4
                                   : (decoded.flags & F_IMM64) != 0 ? 8
                                                                    : 0;
        if (start + decoded.len - immediate - sizeof(uint32_t) != dispRva)
            continue;
        const uint32_t width = MemoryWriteWidth(decoded);
        if (width == 0)
            continue;

        // Only a clean walk past `start` rules a candidate out. When the walk cannot reach it the store may be real,
        // so it is reported as not provably an instruction, which stands the family aside.
        const Walk walk = WalkToInstruction(image, start);
        if (walk == Walk::NotStart)
            continue;
        store.rva = start;
        store.decoded = decoded;
        store.knownInstruction = walk == Walk::Start;
        store.width = width;
        store.disp = decoded.disp.disp32;
        return true;
    }
    return false;
}

// A `mov [base+field], r8` without REX (six bytes) has no in-place imm8 form. The known providers have exactly one:
// the object's member initialisation, storing a register cleared by `xor r32, r32`. It is accepted only when that is
// provable in a linear walk of its function: the xor is the last write of the register before the store, everything in
// between is a memory store, a load into another register or a nop, and no relative branch lands in between.
inline bool IsZeroInitialiser(const ImageView& image, const ByteStore& store)
{
    const auto& target = store.decoded;
    const bool rex = (target.flags & F_PREFIX_REX) != 0;
    const uint8_t source = static_cast<uint8_t>(target.modrm_reg | (target.rex_r << 3));
    FunctionRange function {};
    if (target.opcode != 0x88 || !store.knownInstruction || (!rex && source >= 4) || // ah..bh
        !image.FindFunctionRange(store.rva, function))
        return false;

    uint32_t clearedAt = 0;
    bool cleared = false;
    std::vector<uint32_t> branchTargets;
    for (uint32_t cursor = function.begin; cursor < function.end;)
    {
        const uint32_t room = function.end - cursor;
        const size_t take = room < 15 ? room : 15;
        const auto* bytes = image.DataAtRva(cursor, take);
        if (bytes == nullptr)
            return false;
        uint8_t copy[16] {};
        std::memcpy(copy, bytes, take);
        hde64s decoded {};
        hde64_disasm(copy, &decoded);
        if ((decoded.flags & F_ERROR) != 0 || decoded.len == 0 || decoded.len > take)
            return false;

        // An indirect jump (jump-table dispatch) can land anywhere in the function: the order is then unprovable.
        if (decoded.opcode == 0xFF && (decoded.modrm_reg == 4 || decoded.modrm_reg == 5))
            return false;
        uint32_t branch = 0;
        if ((decoded.flags & F_RELATIVE) != 0 && DecodeRelativeBranchTarget(image, cursor, branch))
            branchTargets.push_back(branch);

        if (cursor < store.rva)
        {
            const bool memory = (decoded.flags & F_MODRM) != 0 && decoded.modrm_mod != 3;
            const uint8_t reg = static_cast<uint8_t>(decoded.modrm_reg | (decoded.rex_r << 3));
            const uint8_t rm = static_cast<uint8_t>(decoded.modrm_rm | (decoded.rex_b << 3));
            if ((decoded.opcode == 0x31 || decoded.opcode == 0x33) && (decoded.flags & F_MODRM) != 0 && !memory &&
                reg == rm && reg == source && (decoded.flags & F_PREFIX_66) == 0)
            {
                cleared = true;
                clearedAt = cursor;
            }
            else if (cleared)
            {
                // General-purpose stores and the SSE stores (movups/movaps/movdqa/movq/movntps) of initialisers.
                const bool sseStore = decoded.opcode == 0x0F &&
                                      (decoded.opcode2 == 0x11 || decoded.opcode2 == 0x13 || decoded.opcode2 == 0x17 ||
                                       decoded.opcode2 == 0x29 || decoded.opcode2 == 0x2B || decoded.opcode2 == 0x7F ||
                                       decoded.opcode2 == 0xD6 || decoded.opcode2 == 0xE7);
                const bool memoryStore = memory && (decoded.opcode == 0x88 || decoded.opcode == 0x89 ||
                                                    decoded.opcode == 0xC6 || decoded.opcode == 0xC7 || sseStore);
                const bool otherLoad =
                    (decoded.opcode == 0x8A || decoded.opcode == 0x8B || decoded.opcode == 0x8D) && reg != source;
                // 90 is a nop only without REX (41 90 is xchg eax, r8d); 66/F3 90 are nop/pause.
                const bool nop = (decoded.opcode == 0x90 && (decoded.flags & F_PREFIX_REX) == 0) ||
                                 (decoded.opcode == 0x0F && decoded.opcode2 == 0x1F);
                if (!memoryStore && !otherLoad && !nop)
                    cleared = false;
            }
        }
        cursor += decoded.len;
    }
    if (!cleared)
        return false;
    for (const uint32_t branch : branchTargets)
        if (branch > clearedAt && branch <= store.rva)
            return false;
    return true;
}

// The bytes that make the store write `want`; empty when it already does. False when it cannot be rewritten in place:
// a store outside every unwind function, any byte write other than `mov`, an immediate that is not 0/1, or a register
// store whose immediate form is longer.
inline bool PinByteStore(const ImageView& image, const ByteStore& store, uint8_t want, std::vector<std::byte>& replacement)
{
    replacement.clear();
    const auto& decoded = store.decoded;
    const auto* bytes = image.DataAtRva(store.rva, decoded.len);
    if (!store.knownInstruction || bytes == nullptr)
        return false;

    if (decoded.opcode == 0xC6 && decoded.modrm_reg == 0)
    {
        const uint8_t value = decoded.imm.imm8;
        if (value == want)
            return true;
        if (value != 1 - want)
            return false;
        replacement.assign(bytes, bytes + decoded.len);
        replacement.back() = static_cast<std::byte>(want);
        return true;
    }

    // mov [base(+index)+disp32], r8 with a REX prefix that only names the source register (or nothing): dropping the
    // REX for the imm8 keeps the length, C6 modrm [sib] disp32 imm8.
    const bool sib = (decoded.flags & F_SIB) != 0;
    const size_t expected = 1 + 1 + 1 + (sib ? 1 : 0) + sizeof(uint32_t);
    if (decoded.opcode != 0x88 || (decoded.flags & F_PREFIX_ANY) != F_PREFIX_REX || decoded.rex_b != 0 ||
        decoded.rex_x != 0 || decoded.len != expected)
        return false;
    replacement.reserve(decoded.len);
    replacement.push_back(std::byte { 0xC6 });
    replacement.push_back(static_cast<std::byte>(0x80 | decoded.modrm_rm));
    replacement.insert(replacement.end(), bytes + 3, bytes + decoded.len); // [sib] disp32
    replacement.push_back(static_cast<std::byte>(want));
    return replacement.size() == decoded.len;
}

inline void Fail(Report& report, std::string message)
{
    if (report.failure.empty())
        report.failure = std::move(message);
}

inline LocatedPatch MakePatch(std::string name, const ImageView& image, uint32_t rva,
                              std::vector<std::byte> replacement)
{
    LocatedPatch patch;
    patch.name = std::move(name);
    patch.rva = rva;
    patch.replacement = std::move(replacement);
    const auto* bytes = image.DataAtRva(rva, patch.replacement.size());
    if (bytes == nullptr)
    {
        patch.state = LocateState::InvalidImage;
        return patch;
    }
    patch.expected.assign(bytes, bytes + patch.replacement.size());
    patch.state = LocateState::Found;
    return patch;
}

inline void LocateFlip(const ImageView& image, Report& report)
{
    const auto reference = FindUniqueStringReference(image, kFlipMarker, true);
    if (reference.state != LocateState::Found)
    {
        report.patches.push_back({ "F1 flip metering fallback", reference.state, 0, {}, {} });
        Fail(report, "F1 flip metering fallback: " + std::string(LocateStateName(reference.state)));
        return;
    }

    // The fallback's own store defines the wanted state; every store in the window must agree.
    const uint32_t room = reference.function.end - reference.instructionRva;
    const uint32_t window = std::min(room, kFlipWindow);
    bool haveWanted = false;
    bool conflict = false;
    uint32_t wantField = 0;
    uint8_t wantValue = 0;
    for (uint32_t offset = 0; offset + kStoreLength <= window; ++offset)
    {
        const auto* p = image.DataAtRva(reference.instructionRva + offset, kStoreLength);
        if (p == nullptr)
            break;
        if (p[0] != std::byte { 0xC6 } || !IsDisp32NoSib(std::to_integer<uint8_t>(p[1])) ||
            (std::to_integer<uint8_t>(p[1]) & 0x38) != 0)
            continue;
        const uint32_t field = ReadU32(p + 2);
        const auto value = std::to_integer<uint8_t>(p[6]);
        if (field <= kMinField || field >= kMaxField || value > 1 ||
            !IsInstructionStart(image, reference.instructionRva + offset))
            continue;
        if (!haveWanted)
        {
            haveWanted = true;
            wantField = field;
            wantValue = value;
        }
        else if (field != wantField || value != wantValue)
        {
            conflict = true;
        }
    }
    if (!haveWanted || conflict)
    {
        report.patches.push_back({ "F1 flip metering fallback", LocateState::AnchorFailed, 0, {}, {} });
        Fail(report, std::string("F1 flip metering fallback: ") +
                         (conflict ? "the fallback writes more than one state" : "state not readable"));
        return;
    }
    report.flipField = wantField;
    report.flipValue = wantValue;

    // Every other byte store to the field in executable code is pinned to the same value. Stores are found by their
    // displacement and decoded, so every encoding is seen; one that cannot be rewritten in place stands the family
    // aside rather than staying un-pinned behind a complete report.
    std::vector<LocatedPatch> sites;
    std::vector<uint32_t> seen;
    std::string unpinnable;
    for (WORD index = 0; index < image.SectionCount() && unpinnable.empty(); ++index)
    {
        IMAGE_SECTION_HEADER section {};
        if (!image.SectionAt(index, section) || (section.Characteristics & IMAGE_SCN_MEM_EXECUTE) == 0 ||
            (section.Characteristics & IMAGE_SCN_MEM_READ) == 0 || section.Misc.VirtualSize < sizeof(uint32_t))
            continue;
        const auto* data = image.DataAtRva(section.VirtualAddress, section.Misc.VirtualSize);
        if (data == nullptr)
            continue;
        // A store covers the state byte when its displacement is in [field - 15, field] and the bytes it writes reach
        // it. The widest store here is 16 bytes, so disp32 values that far below the field are read as candidates too.
        for (uint32_t offset = 0; offset + sizeof(uint32_t) <= section.Misc.VirtualSize; ++offset)
        {
            const uint32_t candidate = ReadU32(data + offset);
            if (candidate > wantField || wantField - candidate > 15)
                continue;
            const uint32_t dispRva = section.VirtualAddress + offset;
            ByteStore store;
            if (!FindCoveringStore(image, dispRva, store) ||
                !(store.disp <= wantField && wantField - store.disp < store.width) ||
                std::find(seen.begin(), seen.end(), store.rva) != seen.end())
                continue;
            seen.push_back(store.rva);

            // Only the exact one-byte store at the field can be pinned or kept; anything wider or offset covers the
            // state byte and cannot be rewritten in place.
            std::vector<std::byte> replacement;
            const bool exactByte = store.width == 1 && store.disp == wantField;
            if (exactByte && !PinByteStore(image, store, wantValue, replacement) && IsZeroInitialiser(image, store))
            {
                // Accepted residual: the member initialisation writes 0, which is hardware flip metering (the
                // opposite of the pinned state when that is 1). It is kept on the basis of the object-construction
                // pattern only; nothing here proves that a pinned store runs after construction and before the
                // state is read. Reported as "initialisers" in the status.
                ++report.initialisers;
                continue;
            }
            if (!exactByte || !PinByteStore(image, store, wantValue, replacement))
            {
                unpinnable = "the store at RVA 0x" + Hex(store.rva) + " (" +
                             HexBytes(image, store.rva, store.decoded.len) +
                             (store.width != 1 ? ", writes " + std::to_string(store.width) + " bytes" : "") +
                             (store.knownInstruction ? "" : ", the function walk could not reach it") +
                             ") cannot be pinned in place";
                break;
            }
            if (replacement.empty())
                continue; // already writes the wanted state
            if (sites.size() >= kMaxSites)
            {
                report.patches.push_back({ "F2 flip metering stores", LocateState::Ambiguous, 0, {}, {} });
                Fail(report, "F2 flip metering stores: too many stores to the state");
                return;
            }
            sites.push_back(MakePatch("F2 flip metering store", image, store.rva, std::move(replacement)));
        }
    }
    if (!unpinnable.empty())
    {
        report.patches.push_back({ "F2 flip metering stores", LocateState::AnchorFailed, 0, {}, {} });
        Fail(report, "F2 flip metering stores: " + unpinnable);
        return;
    }
    if (sites.empty())
    {
        report.patches.push_back({ "F2 flip metering stores", LocateState::Missing, 0, {}, {} });
        Fail(report, "F2 flip metering stores: no other store to the state can be patched");
        return;
    }
    for (size_t i = 0; i < sites.size(); ++i)
    {
        for (size_t j = i + 1; j < sites.size(); ++j)
        {
            if (sites[i].rva < sites[j].rva + sites[j].replacement.size() &&
                sites[j].rva < sites[i].rva + sites[i].replacement.size())
            {
                report.patches.push_back({ "F2 flip metering stores", LocateState::Ambiguous, 0, {}, {} });
                Fail(report, "F2 flip metering stores: stores overlap");
                return;
            }
        }
    }
    for (auto& site : sites)
        report.patches.push_back(std::move(site));
}
} // namespace detail

inline Report Locate(const ImageView& image)
{
    Report report;
    if (!image.IsValid())
    {
        report.failure = "sl.dlss_g must be a valid mapped AMD64 image";
        return report;
    }

    const auto ceiling = FindUniqueInImage(image, detail::kCeiling, detail::CeilingAnchor);
    if (ceiling.state == LocateState::Found)
    {
        const auto* bytes = image.DataAtRva(ceiling.rva, detail::kCeilingBytes.size());
        report.compiledCeiling = bytes != nullptr ? std::to_integer<uint8_t>(bytes[1]) : 0;
        report.patches.push_back(detail::MakePatch("C1 frame count clamp", image, ceiling.rva + 9,
                                                   FamilyRtx40::detail::Bytes({ 0xD2 })));
        if (report.patches.back().expected.empty() || report.patches.back().expected[0] != std::byte { 0xD1 })
            report.patches.back().state = LocateState::AnchorFailed;
    }
    else
    {
        report.patches.push_back({ "C1 frame count clamp", ceiling.state, 0, {}, {} });
    }
    if (report.patches.back().state != LocateState::Found)
        detail::Fail(report, "C1 frame count clamp: " + std::string(LocateStateName(report.patches.back().state)));

    detail::LocateFlip(image, report);
    return report;
}

inline std::vector<PatchOperation> BuildOperations(const Report& report, const ImageView& image)
{
    return report.Complete() ? BuildOperationsFromPatches(report.patches, image) : std::vector<PatchOperation> {};
}
} // namespace MfgUnlock::FamilyRtx40::Streamline
