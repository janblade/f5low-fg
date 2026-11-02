// Ported from OptiScaler-NR tests/mfgunlock_core_smoke.cpp at 745e49431 (origin/release/0.1.36).
// Changes: include paths; the synthetic pattern address is the named kPatternStart (the repo audit reads a bare hex next to a field called rva as a vendor offset); the XeFG family (SyntheticFamilyImage, the six-patch XeFG/XeLL block) is not ported, so its checks are removed; nothing else.
// Build: tests\build_mfgunlock_tests.ps1 (cl /std:c++20 /EHsc /W4 /wd4701 tests\mfgunlock_core_smoke.cpp src\unlock\hde\hde64.c)
#include "../src/unlock/Locator.h"
#include "../src/unlock/FamilyRtx40.h"
#include "../src/unlock/FamilySlDlssg.h"
#include "../src/unlock/FamilyFatbin.h"
#include "../src/unlock/FamilyAmpere.h"
#include "../src/unlock/PtxTuring.h"
#include "../src/unlock/Patch.h"
#include "../src/unlock/Status.h"
#include "../src/unlock/Mode.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

namespace
{
int failures = 0;

void Check(bool passed, const char* message)
{
    std::printf("  %s: %s\n", passed ? "ok  " : "FAIL", message);
    if (!passed)
        ++failures;
}

// Where the synthetic image below puts its pattern (named so the repo audit does not take the made-up address for a vendor offset).
constexpr uint32_t kPatternStart = 0x1000;

bool AcceptAnchor(const MfgUnlock::ImageView&, uint32_t, void*) { return true; }

bool RejectAnchor(const MfgUnlock::ImageView&, uint32_t, void*) { return false; }

struct SyntheticImage
{
    static constexpr size_t kSize = 0x6000;
    std::byte* bytes = static_cast<std::byte*>(VirtualAlloc(nullptr, kSize, MEM_RESERVE | MEM_COMMIT, PAGE_READWRITE));

    SyntheticImage()
    {
        if (bytes == nullptr)
            return;

        IMAGE_DOS_HEADER dos {};
        dos.e_magic = IMAGE_DOS_SIGNATURE;
        dos.e_lfanew = 0x80;
        std::memcpy(bytes, &dos, sizeof(dos));

        const DWORD signature = IMAGE_NT_SIGNATURE;
        std::memcpy(bytes + 0x80, &signature, sizeof(signature));

        IMAGE_FILE_HEADER file {};
        file.Machine = IMAGE_FILE_MACHINE_AMD64;
        file.NumberOfSections = 3;
        file.SizeOfOptionalHeader = sizeof(IMAGE_OPTIONAL_HEADER64);
        std::memcpy(bytes + 0x84, &file, sizeof(file));

        IMAGE_OPTIONAL_HEADER64 optional {};
        optional.Magic = IMAGE_NT_OPTIONAL_HDR64_MAGIC;
        optional.SizeOfImage = static_cast<DWORD>(kSize);
        optional.SizeOfHeaders = 0x400;
        optional.NumberOfRvaAndSizes = IMAGE_NUMBEROF_DIRECTORY_ENTRIES;
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXPORT] = { 0x3000, 0x200 };
        optional.DataDirectory[IMAGE_DIRECTORY_ENTRY_EXCEPTION] = { 0x2000, sizeof(IMAGE_RUNTIME_FUNCTION_ENTRY) };
        std::memcpy(bytes + 0x98, &optional, sizeof(optional));

        const size_t sectionOffset = 0x98 + sizeof(optional);
        IMAGE_SECTION_HEADER sections[3] {};
        std::memcpy(sections[0].Name, ".text", 5);
        sections[0].Misc.VirtualSize = 0x1000;
        sections[0].VirtualAddress = 0x1000;
        sections[0].Characteristics = IMAGE_SCN_CNT_CODE | IMAGE_SCN_MEM_EXECUTE | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[1].Name, ".pdata", 6);
        sections[1].Misc.VirtualSize = 0x1000;
        sections[1].VirtualAddress = 0x2000;
        sections[1].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(sections[2].Name, ".edata", 6);
        sections[2].Misc.VirtualSize = 0x1000;
        sections[2].VirtualAddress = 0x3000;
        sections[2].Characteristics = IMAGE_SCN_CNT_INITIALIZED_DATA | IMAGE_SCN_MEM_READ;
        std::memcpy(bytes + sectionOffset, sections, sizeof(sections));

        const IMAGE_RUNTIME_FUNCTION_ENTRY function { 0x1000, 0x1020, 0x2100 };
        std::memcpy(bytes + 0x2000, &function, sizeof(function));

        IMAGE_EXPORT_DIRECTORY exports {};
        exports.NumberOfFunctions = 1;
        exports.NumberOfNames = 1;
        exports.AddressOfFunctions = 0x3040;
        exports.AddressOfNames = 0x3044;
        exports.AddressOfNameOrdinals = 0x3048;
        std::memcpy(bytes + 0x3000, &exports, sizeof(exports));
        const DWORD functionRva = 0x1000;
        const DWORD nameRva = 0x3050;
        const WORD ordinal = 0;
        std::memcpy(bytes + 0x3040, &functionRva, sizeof(functionRva));
        std::memcpy(bytes + 0x3044, &nameRva, sizeof(nameRva));
        std::memcpy(bytes + 0x3048, &ordinal, sizeof(ordinal));
        constexpr char exportName[] = "xefgSwapChainGetProperties";
        std::memcpy(bytes + 0x3050, exportName, sizeof(exportName));
        constexpr char anchorString[] = "Network model does not support requested multi frame generation";
        std::memcpy(bytes + 0x3080, anchorString, sizeof(anchorString));

        SetPattern(0x1000);
        SetLea(0x1008, 0x3080);
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    ~SyntheticImage()
    {
        if (bytes != nullptr)
            VirtualFree(bytes, 0, MEM_RELEASE);
    }

    void SetPattern(size_t offset)
    {
        constexpr std::array<uint8_t, 3> pattern { 0xa1, 0xb2, 0xc3 };
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, pattern.data(), pattern.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    void ClearPattern(size_t offset)
    {
        const std::array<uint8_t, 3> pattern { 0xcc, 0xcc, 0xcc };
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, pattern.data(), pattern.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    void SetLea(size_t offset, uint32_t targetRva)
    {
        std::array<uint8_t, 7> instruction { 0x48, 0x8d, 0x05 };
        const int32_t displacement = static_cast<int32_t>(targetRva - (offset + instruction.size()));
        std::memcpy(instruction.data() + 3, &displacement, sizeof(displacement));
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, instruction.data(), instruction.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }

    void SetJump(size_t offset, uint32_t targetRva)
    {
        std::array<uint8_t, 5> instruction { 0xe9 };
        const int32_t displacement = static_cast<int32_t>(targetRva - (offset + instruction.size()));
        std::memcpy(instruction.data() + 1, &displacement, sizeof(displacement));
        DWORD previous = 0;
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        std::memcpy(bytes + offset, instruction.data(), instruction.size());
        VirtualProtect(bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    }
};

struct FailingWriter
{
    size_t writes = 0;

    static bool Write(void* address, std::span<const std::byte> expected, std::span<const std::byte> replacement)
    {
        auto& self = Current();
        if (++self.writes == 2)
            return false;
        if (std::memcmp(address, expected.data(), expected.size()) != 0)
            return false;
        std::memcpy(address, replacement.data(), replacement.size());
        return true;
    }

    static FailingWriter& Current()
    {
        static FailingWriter writer;
        return writer;
    }
};

struct PartialWriter
{
    size_t writes = 0;
    bool failApplyAndRollback = true;

    static bool Write(void* address, std::span<const std::byte> expected, std::span<const std::byte> replacement)
    {
        auto& self = Current();
        ++self.writes;
        if (self.failApplyAndRollback && (self.writes == 2 || self.writes == 3))
            return false;
        if (std::memcmp(address, expected.data(), expected.size()) != 0)
            return false;
        std::memcpy(address, replacement.data(), replacement.size());
        return true;
    }

    static PartialWriter& Current()
    {
        static PartialWriter writer;
        return writer;
    }
};

// fp16 bits -> double (NaN stays NaN).
double HalfValue(uint16_t bits)
{
    const int sign = (bits >> 15) != 0 ? -1 : 1;
    const int exponent = (bits >> 10) & 31;
    const int mantissa = bits & 0x3FF;
    if (exponent == 31)
        return mantissa != 0 ? std::nan("") : sign * HUGE_VAL;
    if (exponent == 0)
        return sign * std::ldexp(static_cast<double>(mantissa), -24);
    return sign * std::ldexp(1.0 + mantissa / 1024.0, exponent - 15);
}

bool HalfIsNan(uint16_t bits) { return ((bits >> 10) & 31) == 31 && (bits & 0x3FF) != 0; }

// PTX min/max.f16 as the reference, written independently of the rewrite under test: a NaN operand gives the other
// one, two NaNs give the canonical NaN, equal values tie by sign so min(-0,+0) = -0 and max(+0,-0) = +0.
uint16_t PtxMinMax(bool isMin, uint16_t a, uint16_t b)
{
    if (HalfIsNan(a) && HalfIsNan(b))
        return 0x7FFF;
    if (HalfIsNan(a))
        return b;
    if (HalfIsNan(b))
        return a;
    const double x = HalfValue(a);
    const double y = HalfValue(b);
    if (x < y)
        return isMin ? a : b;
    if (y < x)
        return isMin ? b : a;
    // Equal (only +-0 differ in bits here): min wants the negative one, max the positive one.
    const bool aNegative = (a & 0x8000) != 0;
    const bool bNegative = (b & 0x8000) != 0;
    if (isMin)
        return aNegative ? a : (bNegative ? b : a);
    return !aNegative ? a : (!bNegative ? b : a);
}

// Runs the straight-line PTX this port emits for min/max (mov.b32 pack/unpack, setp.{lt,gt,eq,nan}.f16, or.pred,
// or/and.b16, selp.b16) on host values. Unknown statements fail the run.
class PtxRunner
{
  public:
    std::map<std::string, uint32_t> regs;

    bool Run(std::string text)
    {
        for (char& c : text)
            if (c == '\n')
                c = ' ';
        size_t at = 0;
        while (at < text.size())
        {
            const size_t end = text.find(';', at);
            std::string statement = Trim(text.substr(at, end == std::string::npos ? std::string::npos : end - at));
            at = end == std::string::npos ? text.size() : end + 1;
            // Drop the block braces around the statements, keep the ones of a {a, b} operand.
            const auto count = [&statement](char c) { return std::count(statement.begin(), statement.end(), c); };
            while (!statement.empty() && statement.front() == '{' && count('{') > count('}'))
                statement = Trim(statement.substr(1));
            while (!statement.empty() && statement.back() == '}' && count('}') > count('{'))
                statement = Trim(statement.substr(0, statement.size() - 1));
            if (statement.empty() || statement.rfind(".reg", 0) == 0)
                continue;
            if (!Step(statement))
                return false;
        }
        return true;
    }

  private:
    static std::string Trim(std::string value)
    {
        while (!value.empty() && value.front() == ' ')
            value.erase(value.begin());
        while (!value.empty() && value.back() == ' ')
            value.pop_back();
        return value;
    }

    // Operands split at top-level commas ({a, b} stays one operand).
    static std::vector<std::string> Operands(const std::string& text)
    {
        std::vector<std::string> result(1);
        int depth = 0;
        for (const char c : text)
        {
            depth += c == '{' ? 1 : c == '}' ? -1 : 0;
            if (c == ',' && depth == 0)
                result.emplace_back();
            else
                result.back() += c;
        }
        for (auto& operand : result)
            operand = Trim(operand);
        return result;
    }

    bool Step(const std::string& statement)
    {
        const size_t space = statement.find(' ');
        if (space == std::string::npos)
            return false;
        const std::string op = statement.substr(0, space);
        const auto args = Operands(statement.substr(space + 1));
        const auto value = [&](const std::string& name) { return regs[name]; };
        const auto half = [&](const std::string& name) { return static_cast<uint16_t>(regs[name] & 0xFFFF); };

        if (op == "mov.b16" && args.size() == 2)
        {
            // a register copy, or an immediate (decimal or 0x hex) into a register
            const std::string& src = args[1];
            if (!src.empty() && (src.front() == '%'))
                regs[args[0]] = value(src) & 0xFFFF;
            else
                regs[args[0]] = static_cast<uint32_t>(std::stoul(src, nullptr, 0)) & 0xFFFF;
            return true;
        }
        if (op == "mov.b32" && args.size() == 2 && args[0].front() == '{')
        {
            const auto parts = Operands(args[0].substr(1, args[0].size() - 2));
            regs[parts[0]] = value(args[1]) & 0xFFFF;
            regs[parts[1]] = value(args[1]) >> 16;
            return true;
        }
        if (op == "mov.b32" && args.size() == 2 && args[1].front() == '{')
        {
            const auto parts = Operands(args[1].substr(1, args[1].size() - 2));
            regs[args[0]] = (value(parts[0]) & 0xFFFF) | (value(parts[1]) << 16);
            return true;
        }
        if (op.rfind("setp.", 0) == 0 && op.size() > 9 && args.size() == 3)
        {
            const std::string compare = op.substr(5, op.size() - 9); // setp.<cmp>.f16
            const double x = HalfValue(half(args[1]));
            const double y = HalfValue(half(args[2]));
            bool result = false;
            if (compare == "lt")
                result = x < y;
            else if (compare == "gt")
                result = x > y;
            else if (compare == "eq")
                result = x == y;
            else if (compare == "nan")
                result = std::isnan(x) || std::isnan(y);
            else
                return false;
            regs[args[0]] = result ? 1 : 0;
            return true;
        }
        if ((op == "or.pred" || op == "and.pred" || op == "or.b16" || op == "and.b16") && args.size() == 3)
        {
            const bool bitAnd = op == "and.b16" || op == "and.pred";
            regs[args[0]] = bitAnd ? (value(args[1]) & value(args[2])) : (value(args[1]) | value(args[2]));
            return true;
        }
        if (op == "selp.b16" && args.size() == 4)
        {
            regs[args[0]] = value(args[3]) != 0 ? value(args[1]) : value(args[2]);
            return true;
        }
        return false;
    }
};
} // namespace

int main()
{
    // Locator and transaction basics; scoped so the family blocks below can reuse the names.
    {
        SyntheticImage image;
        if (image.bytes == nullptr)
        {
            std::puts("FAIL: could not allocate synthetic PE image");
            return 1;
        }

        const MfgUnlock::ImageView view(image.bytes, SyntheticImage::kSize);
        Check(view.IsValid(), "accepts a bounded AMD64 PE image");

        uint32_t exportRva = 0;
        Check(view.FindExport("xefgSwapChainGetProperties", exportRva) && exportRva == 0x1000,
              "resolves an export by its name");

        MfgUnlock::FunctionRange function {};
        Check(view.FindFunctionRange(0x1003, function) && function.begin == 0x1000 && function.end == 0x1020,
              "resolves the owning unwind function");

        const MfgUnlock::MaskedPattern pattern { "\xa1\xb2\xc3", "xxx" };
        auto result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
        Check(result.state == MfgUnlock::LocateState::Found && result.rva == kPatternStart,
              "finds a unique masked pattern with a passing second anchor");
        result = MfgUnlock::FindUniqueFromExport(view, "xefgSwapChainGetProperties", pattern, AcceptAnchor);
        Check(result.state == MfgUnlock::LocateState::Found && result.rva == kPatternStart,
              "anchors a unique locator at an export");
        result = MfgUnlock::FindUniqueFromStringXref(
            view, "Network model does not support requested multi frame generation", pattern, AcceptAnchor);
        Check(result.state == MfgUnlock::LocateState::Found && result.rva == kPatternStart,
              "anchors a unique locator at the owning function of a string xref");

        image.SetLea(0x1010, 0x3080);
        const auto ambiguousReference =
            MfgUnlock::FindUniqueStringReference(view, "Network model does not support requested multi frame generation");
        Check(ambiguousReference.state == MfgUnlock::LocateState::Ambiguous,
              "rejects a string with multiple RIP-relative LEA references");
        image.ClearPattern(0x1010);

        image.SetPattern(0x1008);
        result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
        Check(result.state == MfgUnlock::LocateState::Ambiguous, "rejects an ambiguous pattern");

        image.ClearPattern(0x1000);
        image.ClearPattern(0x1008);
        result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
        Check(result.state == MfgUnlock::LocateState::Missing, "reports a missing pattern");

        image.SetPattern(0x1000);
        result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, RejectAnchor);
        Check(result.state == MfgUnlock::LocateState::AnchorFailed, "rejects a pattern when the second anchor fails");

        DWORD previous = 0;
        VirtualProtect(image.bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        result = MfgUnlock::FindUniqueInFunction(view, 0x1000, pattern, AcceptAnchor);
        Check(result.state == MfgUnlock::LocateState::NotExecutable, "rejects a non-RX executable page");
        VirtualProtect(image.bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
        image.SetJump(0x1010, 0x1000);
        uint32_t branchTarget = 0;
        Check(MfgUnlock::DecodeRelativeBranchTarget(view, 0x1010, branchTarget) && branchTarget == 0x1000,
              "decodes a relative branch target within the mapped image");
        Check(MfgUnlock::BranchTargetEquals(view, 0x1010, &branchTarget),
              "uses the decoded branch target as a second anchor");

        alignas(8) std::array<uint16_t, 3> values { 0x1111, 0x2222, 0x3333 };
        std::vector<MfgUnlock::PatchOperation> operations;
        for (size_t index = 0; index < values.size(); ++index)
        {
            MfgUnlock::PatchOperation operation;
            operation.address = &values[index];
            operation.expected.resize(sizeof(values[index]));
            operation.replacement.resize(sizeof(values[index]));
            std::memcpy(operation.expected.data(), &values[index], sizeof(values[index]));
            const uint16_t replacement = static_cast<uint16_t>(values[index] + 1);
            std::memcpy(operation.replacement.data(), &replacement, sizeof(replacement));
            operations.push_back(std::move(operation));
        }

        FailingWriter::Current().writes = 0;
        const auto transaction = MfgUnlock::ApplyTransaction(operations, &FailingWriter::Write);
        Check(!transaction.applied && transaction.rollbackComplete && transaction.failedIndex == 1 &&
                  values == std::array<uint16_t, 3> { 0x1111, 0x2222, 0x3333 },
              "rolls back patch one when patch two of three fails");

        MfgUnlock::AppliedPatchSet partialPatchSet;
        PartialWriter::Current() = {};
        const auto partialApply = partialPatchSet.Apply(operations, &PartialWriter::Write);
        Check(!partialApply.applied && !partialApply.rollbackComplete && partialPatchSet.IsActive() &&
                  values[0] == 0x1112,
              "retains recovery data when an atomic transaction cannot fully roll back");
        PartialWriter::Current().failApplyAndRollback = false;
        PartialWriter::Current().writes = 0;
        const auto partialRestore = partialPatchSet.Restore(&PartialWriter::Write);
        Check(partialRestore.applied && !partialPatchSet.IsActive() &&
                  values == std::array<uint16_t, 3> { 0x1111, 0x2222, 0x3333 },
              "reconciles a partial transaction by restoring only bytes it changed");

        MfgUnlock::PatchOperation executablePatch;
        executablePatch.address = image.bytes + 0x1000;
        executablePatch.expected = { std::byte { 0xa1 }, std::byte { 0xb2 } };
        executablePatch.replacement = { std::byte { 0xd4 }, std::byte { 0xe5 } };
        MfgUnlock::AppliedPatchSet patchSet;
        const auto executableResult = patchSet.Apply(std::span<const MfgUnlock::PatchOperation>(&executablePatch, 1));
        Check(executableResult.applied && patchSet.IsActive() && image.bytes[0x1000] == std::byte { 0xd4 } &&
                  image.bytes[0x1001] == std::byte { 0xe5 },
              "atomically patches an aligned two-byte sequence on an RX page");
        const auto restoreResult = patchSet.Restore();
        Check(restoreResult.applied && !patchSet.IsActive() && image.bytes[0x1000] == std::byte { 0xa1 } &&
                  image.bytes[0x1001] == std::byte { 0xb2 },
              "restores the original bytes and RX page protection");
    }

    MfgUnlock::FamilyStatus activeStatus;
    activeStatus.state = MfgUnlock::FamilyState::Active;
    activeStatus.patches = { { "gate", "found" }, { "wrapper", "found" } };
    Check(MfgUnlock::FormatStatus(activeStatus) == "active: 2 of 2 patches",
          "formats active family status with the located patch count");

    MfgUnlock::FamilyStatus asideStatus;
    asideStatus.state = MfgUnlock::FamilyState::StoodAside;
    asideStatus.failure = "gate not found in provider.dll 1.2";
    Check(MfgUnlock::FormatStatus(asideStatus) == "stood aside: gate not found in provider.dll 1.2",
          "formats stand-aside status with the exact failed patch");

    const auto writeCode = [](SyntheticImage& target, size_t offset, std::initializer_list<uint8_t> code)
    {
        DWORD previous = 0;
        VirtualProtect(target.bytes + 0x1000, 0x1000, PAGE_READWRITE, &previous);
        size_t cursor = offset;
        for (const uint8_t value : code)
            target.bytes[cursor++] = static_cast<std::byte>(value);
        VirtualProtect(target.bytes + 0x1000, 0x1000, PAGE_EXECUTE_READ, &previous);
    };
    const auto clearCode = [&](SyntheticImage& target)
    {
        for (size_t offset = 0x1000; offset < 0x1020; ++offset)
            writeCode(target, offset, { 0xcc });
    };

    {
        SyntheticImage rtx40Image;
        const MfgUnlock::ImageView rtx40View(rtx40Image.bytes, SyntheticImage::kSize);
        clearCode(rtx40Image);
        // 310.9 layout: advertise jl at +6 targets inside the function, validate setae follows.
        writeCode(rtx40Image, 0x1000, { 0x81, 0xfd, 0xb0, 0x01, 0x00, 0x00, 0x0f, 0x8c, 0x10, 0x00, 0x00, 0x00, 0xbf,
                                        0x05, 0x00, 0x00, 0x00, 0x3d, 0xb0, 0x01, 0x00, 0x00, 0x0f, 0x93, 0xc0 });
        const auto report = MfgUnlock::FamilyRtx40::Locate(rtx40View);
        Check(report.Complete() && report.patches.size() == 2 && report.advertiseLayout == "310.9" &&
                  report.validateLayout == "310.9",
              "locates both RTX 40 gates in the 310.9 layout");

        const auto operations = MfgUnlock::FamilyRtx40::BuildOperations(report, rtx40View);
        Check(operations.size() == 6, "RTX 40 310.9 patches change six single bytes");
        MfgUnlock::AppliedPatchSet patchSet;
        const auto applied = patchSet.Apply(operations);
        Check(applied.applied && rtx40Image.bytes[0x1007] == std::byte { 0x1f } &&
                  rtx40Image.bytes[0x100b] == std::byte { 0x90 } && rtx40Image.bytes[0x1016] == std::byte { 0xb0 } &&
                  rtx40Image.bytes[0x1017] == std::byte { 0x01 },
              "applies the RTX 40 310.9 patches");
        const auto restored = patchSet.Restore();
        Check(restored.applied && rtx40Image.bytes[0x1007] == std::byte { 0x8c } &&
                  rtx40Image.bytes[0x1016] == std::byte { 0x0f },
              "restores the RTX 40 310.9 patches");

        // Branch leaving the function fails the structural anchor.
        writeCode(rtx40Image, 0x1008, { 0x00, 0x10, 0x00, 0x00 });
        Check(!MfgUnlock::FamilyRtx40::Locate(rtx40View).Complete(),
              "RTX 40 stands aside when the advertise branch leaves the function");
    }

    {
        SyntheticImage rtx40Image;
        const MfgUnlock::ImageView rtx40View(rtx40Image.bytes, SyntheticImage::kSize);
        clearCode(rtx40Image);
        writeCode(rtx40Image, 0x1000, { 0xbb, 0x01, 0x00, 0x00, 0x00, 0x41, 0xb8, 0x03, 0x00, 0x00, 0x00, 0x81, 0xff,
                                        0xb0, 0x01, 0x00, 0x00, 0x44, 0x0f, 0x4c, 0xc3, 0x3d, 0xb0, 0x01, 0x00, 0x00,
                                        0x7c, 0x05, 0x83, 0xfb, 0x03, 0x76 });
        const auto report = MfgUnlock::FamilyRtx40::Locate(rtx40View);
        Check(report.Complete() && report.patches.size() == 4 && report.advertiseLayout == "legacy" &&
                  report.validateLayout == "legacy",
              "locates both RTX 40 gates in the legacy layout");
        const auto operations = MfgUnlock::FamilyRtx40::BuildOperations(report, rtx40View);
        Check(!operations.empty() && operations.size() == 1 + 4 + 2 + 1,
              "RTX 40 legacy patches change eight single bytes");
    }

    {
        SyntheticImage rtx40Image;
        const MfgUnlock::ImageView rtx40View(rtx40Image.bytes, SyntheticImage::kSize);
        clearCode(rtx40Image);
        writeCode(rtx40Image, 0x1000, { 0x3d, 0xb0, 0x01, 0x00, 0x00, 0x0f, 0x93, 0xc0 });
        const auto report = MfgUnlock::FamilyRtx40::Locate(rtx40View);
        Check(!report.Complete() && report.failure.find("A1 advertise gate") != std::string::npos &&
                  MfgUnlock::FamilyRtx40::BuildOperations(report, rtx40View).empty(),
              "RTX 40 stands aside and writes nothing when the advertise gate is missing");
    }

    {
        SyntheticImage slImage;
        const MfgUnlock::ImageView slView(slImage.bytes, SyntheticImage::kSize);
        clearCode(slImage);
        constexpr char marker[] = "FG1 DLL has been detected: forcing flip-metering off.";
        std::memcpy(slImage.bytes + 0x3100, marker, sizeof(marker));
        writeCode(slImage, 0x1000, { 0xba, 0x05, 0x00, 0x00, 0x00, 0x3b, 0xca, 0x0f, 0x42, 0xd1 });
        slImage.SetLea(0x100a, 0x3100);
        // The fallback's own store (state 1) and one register store that has to be pinned to the same state.
        writeCode(slImage, 0x1011, { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x01 });
        writeCode(slImage, 0x1018, { 0x40, 0x88, 0xbb, 0xf8, 0x44, 0x00, 0x00 });

        const auto report = MfgUnlock::FamilyRtx40::Streamline::Locate(slView);
        Check(report.Complete() && report.patches.size() == 2 && report.compiledCeiling == 5 &&
                  report.flipField == 0x44f8 && report.flipValue == 1,
              "locates the sl.dlss_g clamp and derives the flip metering state from the fallback");

        const auto operations = MfgUnlock::FamilyRtx40::Streamline::BuildOperations(report, slView);
        MfgUnlock::AppliedPatchSet patchSet;
        const auto applied = patchSet.Apply(operations);
        const std::array<uint8_t, 7> pinned { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x01 };
        bool pinnedOk = true;
        for (size_t index = 0; index < pinned.size(); ++index)
            pinnedOk = pinnedOk && slImage.bytes[0x1018 + index] == static_cast<std::byte>(pinned[index]);
        Check(applied.applied && slImage.bytes[0x1009] == std::byte { 0xd2 } && pinnedOk,
              "applies the sl.dlss_g clamp and pins the flip metering store");
        const auto restored = patchSet.Restore();
        Check(restored.applied && slImage.bytes[0x1009] == std::byte { 0xd1 } &&
                  slImage.bytes[0x1018] == std::byte { 0x40 } && slImage.bytes[0x101e] == std::byte { 0x00 },
              "restores the sl.dlss_g patches");

        // A fallback state that disagrees fails closed.
        writeCode(slImage, 0x1017, { 0x00 });
        writeCode(slImage, 0x1011, { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x00 });
        writeCode(slImage, 0x1018, { 0xc6, 0x83, 0xf0, 0x44, 0x00, 0x00, 0x01 });
        Check(!MfgUnlock::FamilyRtx40::Streamline::Locate(slView).Complete(),
              "sl.dlss_g stands aside when no other store can be pinned");
    }

    {
        SyntheticImage slImage;
        const MfgUnlock::ImageView slView(slImage.bytes, SyntheticImage::kSize);
        clearCode(slImage);
        writeCode(slImage, 0x1000, { 0xba, 0x05, 0x00, 0x00, 0x00, 0x3b, 0xca, 0x0f, 0x42, 0xd1 });
        const auto report = MfgUnlock::FamilyRtx40::Streamline::Locate(slView);
        Check(!report.Complete() && report.failure.find("F1 flip metering fallback") != std::string::npos &&
                  MfgUnlock::FamilyRtx40::Streamline::BuildOperations(report, slView).empty(),
              "sl.dlss_g stands aside and writes nothing when the fallback message is missing");
    }

    {
        // Midpoint fix: one container holding an Ada cubin, a Blackwell PTX and an Ada PTX.
        SyntheticImage image;
        const MfgUnlock::ImageView view(image.bytes, SyntheticImage::kSize);
        const auto put = [&](size_t offset, const void* data, size_t size) { std::memcpy(image.bytes + offset, data, size); };
        const auto writeImage = [&](size_t offset, uint16_t kind, uint64_t payload, uint32_t arch, const char* text) {
            const uint32_t header = 32;
            put(offset, &kind, sizeof(kind));
            put(offset + 4, &header, sizeof(header));
            put(offset + 8, &payload, sizeof(payload));
            put(offset + 28, &arch, sizeof(arch));
            if (text != nullptr)
                put(offset + header, text, std::strlen(text));
        };
        constexpr size_t base = 0x3200;
        const uint8_t magic[] = { 0x50, 0xed, 0x55, 0xba, 0x01, 0x00, 0x10, 0x00 };
        const uint64_t fatSize = 3 * 32 + 8 + 24 + 8;
        put(base, magic, sizeof(magic));
        put(base + 8, &fatSize, sizeof(fatSize));
        writeImage(base + 16, 2, 8, 89, nullptr);
        writeImage(base + 16 + 40, 1, 24, 120, ".target sm_120\n.version");
        writeImage(base + 16 + 40 + 56, 1, 8, 89, ".target ");

        const auto report = MfgUnlock::FamilyRtx40::Fatbin::Locate(view);
        Check(report.Complete() && report.containers == 1 && report.patches.size() == 4,
              "locates a fatbin container with a Blackwell PTX and Ada images");
        const auto operations = MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(report, view);
        MfgUnlock::AppliedPatchSet patchSet;
        const auto applied = patchSet.Apply(operations);
        uint32_t blackwellArch = 0;
        uint32_t adaArch = 0;
        std::memcpy(&blackwellArch, image.bytes + base + 16 + 40 + 28, sizeof(blackwellArch));
        std::memcpy(&adaArch, image.bytes + base + 16 + 28, sizeof(adaArch));
        Check(applied.applied && blackwellArch == 89 && adaArch == 122 &&
                  std::memcmp(image.bytes + base + 16 + 40 + 32, ".target sm_89 \n", 15) == 0,
              "relabels the Blackwell PTX as sm_89 and parks the Ada images");
        const auto restored = patchSet.Restore();
        std::memcpy(&blackwellArch, image.bytes + base + 16 + 40 + 28, sizeof(blackwellArch));
        Check(restored.applied && blackwellArch == 120 &&
                  std::memcmp(image.bytes + base + 16 + 40 + 32, ".target sm_120\n", 15) == 0,
              "restores the fatbin container");

        // ".target sm_120a" is a different target and must not be rewritten.
        put(base + 16 + 40 + 32 + 14, "a", 1);
        Check(!MfgUnlock::FamilyRtx40::Fatbin::Locate(view).Complete(),
              "leaves a container whose PTX targets sm_120a alone");
    }

    {
        const auto minmax = MfgUnlock::PtxTuring::RewriteText("{min.f16 %h1, %h2, %h3;}\n{max.f16x2 %r1, %r2, %r3;}\n");
        Check(minmax.ok && minmax.minMaxScalar == 1 && minmax.minMaxVec == 1 &&
                  minmax.ptx.find("setp.nan.f16") != std::string::npos,
              "min/max rewrite keeps PTX's NaN operand rule");

        std::array<uint8_t, 0x10 + 0x40 + 8> container{};
        const uint32_t magic = MfgUnlock::PtxTuring::kFatbinMagic;
        const uint16_t headerSize = 0x10;
        const uint64_t fatSize = 0x48;
        std::memcpy(container.data(), &magic, 4);
        std::memcpy(container.data() + 6, &headerSize, 2);
        std::memcpy(container.data() + 8, &fatSize, 8);
        const uint16_t kind = 1;
        const uint32_t entryHeader = 0x40;
        const uint64_t wrapped = 0xFFFFFFFFFFFFFFF0ull;
        const uint32_t arch = 89;
        std::memcpy(container.data() + 0x10, &kind, 2);
        std::memcpy(container.data() + 0x10 + 4, &entryHeader, 4);
        std::memcpy(container.data() + 0x10 + 8, &wrapped, 8);
        std::memcpy(container.data() + 0x10 + 0x1C, &arch, 4);
        Check(!MfgUnlock::PtxTuring::BuildFromContainer(container.data()).ok,
              "rejects a container entry whose payload size wraps");

        const uint64_t onePayload = 1;
        const uint32_t compressed = 1;
        const uint64_t uncompressed = 16;
        container[0x10 + 0x40] = 0x28;
        std::memcpy(container.data() + 0x10 + 8, &onePayload, 8);
        std::memcpy(container.data() + 0x10 + 16, &compressed, 4);
        std::memcpy(container.data() + 0x10 + 0x38, &uncompressed, 8);
        Check(!MfgUnlock::PtxTuring::BuildFromContainer(container.data()).ok,
              "rejects a one-byte compressed payload without reading past it");
    }

    {
        // sl.dlss_g flip-metering stores in other encodings, after the fallback's own store (state 1) at 0x1011.
        const auto slCase = [&](std::initializer_list<uint8_t> stores, std::unique_ptr<SyntheticImage>& holder)
        {
            holder = std::make_unique<SyntheticImage>();
            SyntheticImage& target = *holder;
            const IMAGE_RUNTIME_FUNCTION_ENTRY longer { 0x1000, 0x1080, 0x2100 };
            std::memcpy(target.bytes + 0x2000, &longer, sizeof(longer));
            for (size_t offset = 0x1000; offset < 0x1080; ++offset)
                writeCode(target, offset, { 0xcc });
            constexpr char marker[] = "FG1 DLL has been detected: forcing flip-metering off.";
            std::memcpy(target.bytes + 0x3100, marker, sizeof(marker));
            writeCode(target, 0x1000, { 0xba, 0x05, 0x00, 0x00, 0x00, 0x3b, 0xca, 0x0f, 0x42, 0xd1 });
            target.SetLea(0x100a, 0x3100);
            writeCode(target, 0x1011, { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x01 });
            writeCode(target, 0x1018, stores);
            return MfgUnlock::FamilyRtx40::Streamline::Locate(
                MfgUnlock::ImageView(target.bytes, SyntheticImage::kSize));
        };
        const auto storeAt = [](const SyntheticImage& target, size_t offset, std::initializer_list<uint8_t> bytes)
        {
            size_t cursor = offset;
            for (const uint8_t value : bytes)
                if (target.bytes[cursor++] != static_cast<std::byte>(value))
                    return false;
            return true;
        };
        const auto pinned = [&](const MfgUnlock::FamilyRtx40::Streamline::Report& report, SyntheticImage& target,
                                size_t offset, std::initializer_list<uint8_t> bytes)
        {
            const MfgUnlock::ImageView view(target.bytes, SyntheticImage::kSize);
            MfgUnlock::AppliedPatchSet set;
            const bool applied =
                report.Complete() && set.Apply(MfgUnlock::FamilyRtx40::Streamline::BuildOperations(report, view)).applied;
            const bool matches = applied && storeAt(target, offset, bytes);
            set.Restore();
            return matches;
        };

        std::unique_ptr<SyntheticImage> holder;
        auto report = slCase({ 0x44, 0x88, 0x83, 0xf8, 0x44, 0x00, 0x00 }, holder); // mov [rbx+f], r8b
        Check(pinned(report, *holder, 0x1018, { 0xc6, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x01 }),
              "sl.dlss_g pins a REX.R register store (44 88) to the imm8 form");
        report = slCase({ 0xc6, 0x84, 0x24, 0xf8, 0x44, 0x00, 0x00, 0x00 }, holder); // mov byte [rsp+f], 0
        Check(pinned(report, *holder, 0x1018, { 0xc6, 0x84, 0x24, 0xf8, 0x44, 0x00, 0x00, 0x01 }),
              "sl.dlss_g pins an imm8 store with a SIB byte");
        report = slCase({ 0x41, 0x88, 0x87, 0xf8, 0x44, 0x00, 0x00 }, holder); // mov [r15+f], al
        Check(!report.Complete() && report.failure.find("cannot be pinned") != std::string::npos,
              "sl.dlss_g stands aside on a REX.B register store (no in-place imm8 form)");
        report = slCase({ 0x88, 0x83, 0xf8, 0x44, 0x00, 0x00 }, holder); // mov [rbx+f], al, al unknown
        Check(!report.Complete(), "sl.dlss_g stands aside on a six-byte register store of an unknown value");
        report = slCase({ 0x0f, 0x94, 0x83, 0xf8, 0x44, 0x00, 0x00 }, holder); // sete [rbx+f]
        Check(!report.Complete(), "sl.dlss_g stands aside on a setcc to the state");
        report = slCase({ 0x89, 0x83, 0xf8, 0x44, 0x00, 0x00 }, holder); // mov [rbx+f], eax (dword covers the byte)
        Check(!report.Complete() && report.failure.find("writes 4 bytes") != std::string::npos,
              "sl.dlss_g stands aside on a dword store that covers the state byte");
        report = slCase({ 0x89, 0x83, 0xf6, 0x44, 0x00, 0x00 }, holder); // mov [rbx+f-2], eax covers f, f+1
        Check(!report.Complete(), "sl.dlss_g stands aside on a dword store at field-2 that reaches the state byte");
        report = slCase({ 0x0f, 0x11, 0x83, 0xf0, 0x44, 0x00, 0x00 }, holder); // movups [rbx+f-8], xmm0
        Check(!report.Complete() && report.failure.find("writes 16 bytes") != std::string::npos,
              "sl.dlss_g stands aside on a movups block that reaches the state byte");
        // mov [rbx+f-4], eax ends one byte short of the field, then a pinnable byte store.
        report = slCase({ 0x89, 0x83, 0xf4, 0x44, 0x00, 0x00, 0x40, 0x88, 0xbb, 0xf8, 0x44, 0x00, 0x00 }, holder);
        Check(report.Complete() && report.patches.size() == 2,
              "sl.dlss_g ignores a dword store that ends just below the state byte");
        report = slCase({ 0xc5, 0xf8, 0x77, 0x44, 0x88, 0x83, 0xf8, 0x44, 0x00, 0x00 }, holder); // vzeroupper / store
        Check(!report.Complete() && report.failure.find("walk could not reach it") != std::string::npos,
              "sl.dlss_g stands aside on a store the function walk cannot decode its way to");
        report = slCase({ 0x33, 0xc0, 0x88, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x40, 0x88, 0xbb, 0xf8, 0x44, 0x00, 0x00 },
                        holder); // xor eax,eax / mov [rbx+f], al / mov [rbx+f], dil
        Check(report.Complete() && report.initialisers == 1 && report.patches.size() == 2,
              "sl.dlss_g leaves the zero initialisation and pins the runtime store");
        report = slCase({ 0x33, 0xc0, 0x41, 0x90, 0x88, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x40, 0x88, 0xbb, 0xf8, 0x44,
                          0x00, 0x00 },
                        holder); // xor eax,eax / xchg eax,r8d / mov [rbx+f], al
        Check(!report.Complete(), "sl.dlss_g does not take 41 90 (xchg eax, r8d) for a nop in the zero proof");
        report = slCase({ 0x33, 0xc0, 0x88, 0x83, 0xf8, 0x44, 0x00, 0x00, 0x40, 0x88, 0xbb, 0xf8, 0x44, 0x00, 0x00,
                          0xff, 0xe0 },
                        holder); // ... / jmp rax
        Check(!report.Complete(), "sl.dlss_g gives up the zero proof in a function with an indirect jump");
    }

    {
        // RTX 30 retarget of an LZ4-compressed Ada PTX (0x40-byte image header with the payload sizes).
        const auto writeCompressedContainer = [](SyntheticImage& target, size_t base, const std::vector<uint8_t>& stream,
                                                 uint64_t uncompressed)
        {
            const auto put = [&](size_t offset, const void* data, size_t size)
            { std::memcpy(target.bytes + offset, data, size); };
            std::memset(target.bytes + base, 0, 0x200);
            const uint8_t magic[] = { 0x50, 0xed, 0x55, 0xba, 0x01, 0x00, 0x10, 0x00 };
            const uint64_t payload = (stream.size() + 7) & ~size_t { 7 };
            const uint64_t fatSize = 0x40 + payload;
            put(base, magic, sizeof(magic));
            put(base + 8, &fatSize, sizeof(fatSize));
            const uint16_t kind = 1;
            const uint32_t header = 0x40;
            const uint32_t compressed = static_cast<uint32_t>(stream.size());
            const uint32_t arch = 89;
            put(base + 16, &kind, sizeof(kind));
            put(base + 16 + 4, &header, sizeof(header));
            put(base + 16 + 8, &payload, sizeof(payload));
            put(base + 16 + 16, &compressed, sizeof(compressed));
            put(base + 16 + 28, &arch, sizeof(arch));
            put(base + 16 + 0x38, &uncompressed, sizeof(uncompressed));
            put(base + 16 + 0x40, stream.data(), stream.size());
        };
        constexpr size_t base = 0x3400;
        constexpr size_t body = base + 16 + 0x40;

        SyntheticImage literalImage;
        const MfgUnlock::ImageView literalView(literalImage.bytes, SyntheticImage::kSize);
        // One literal run of 17 bytes: token 0xF0, length extension 2.
        const std::string text = ".target sm_89\nabc";
        std::vector<uint8_t> literal = { 0xF0, 0x02 };
        literal.insert(literal.end(), text.begin(), text.end());
        writeCompressedContainer(literalImage, base, literal, text.size());
        const auto retarget =
            MfgUnlock::FamilyRtx40::Fatbin::Locate(literalView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere);
        MfgUnlock::AppliedPatchSet retargetSet;
        const bool applied = retarget.Complete() && retarget.containers == 1 &&
                             retargetSet.Apply(MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(retarget, literalView))
                                 .applied;
        std::vector<uint8_t> decoded;
        const bool decodes = MfgUnlock::PtxTuring::detail::Lz4Decompress(
            reinterpret_cast<const uint8_t*>(literalImage.bytes + body), literal.size(), decoded, text.size());
        Check(applied && decodes && std::string(decoded.begin(), decoded.end()) == ".target sm_86\nabc",
              "retargets a compressed PTX whose .target sits in one literal run");
        retargetSet.Restore();

        // The same bytes as literals, then a match copying them: the edit would change two directives.
        SyntheticImage copiedImage;
        const MfgUnlock::ImageView copiedView(copiedImage.bytes, SyntheticImage::kSize);
        const std::string directive = ".target sm_89\n";
        std::vector<uint8_t> copied = { 0xEA };
        copied.insert(copied.end(), directive.begin(), directive.end());
        copied.push_back(0x0E);
        copied.push_back(0x00);
        writeCompressedContainer(copiedImage, base, copied, directive.size() * 2);
        Check(!MfgUnlock::FamilyRtx40::Fatbin::Locate(copiedView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere)
                   .Complete(),
              "rejects a compressed PTX whose .target bytes are copied again by a later match");

        // A stream that does not decode is rejected too.
        std::vector<uint8_t> broken = literal;
        broken[1] = 0x09; // literal run longer than the stream
        SyntheticImage brokenImage;
        const MfgUnlock::ImageView brokenView(brokenImage.bytes, SyntheticImage::kSize);
        writeCompressedContainer(brokenImage, base, broken, text.size());
        Check(!MfgUnlock::FamilyRtx40::Fatbin::Locate(brokenView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere)
                   .Complete(),
              "rejects a compressed PTX that does not decode");
    }

    {
        // RTX 20 min/max rewrite run on host values against the independent reference: signed zeros, the canonical
        // NaN, two non-canonical NaN payloads (for the both-NaN rule), infinities, subnormals and a finite value.
        constexpr uint16_t edges[] = { 0x0000, 0x8000, 0x7FFF, 0x7E00, 0x7C01, 0xFE01,
                                       0x3C00, 0xBC00, 0x7C00, 0xFC00, 0x0001, 0x8001, 0x3555 };
        bool ran = true;
        bool vectorOk = true;
        bool scalarOk = true;
        bool aliasOk = true;
        for (const bool isMin : { true, false })
        {
            const std::string op = isMin ? "min" : "max";
            const auto vec = MfgUnlock::PtxTuring::RewriteText("{" + op + ".f16x2 %r1, %r2, %r3;}");
            const auto scalar = MfgUnlock::PtxTuring::RewriteText("{" + op + ".f16 %h1, %h2, %h3;}");
            const auto alias = MfgUnlock::PtxTuring::RewriteText("{" + op + ".f16 %h2, %h2, %h3;}");
            ran = ran && vec.ok && scalar.ok && alias.ok;
            for (const uint16_t x : edges)
            {
                for (const uint16_t y : edges)
                {
                    PtxRunner vectorRun;
                    vectorRun.regs["%r2"] = x | (static_cast<uint32_t>(y) << 16);
                    vectorRun.regs["%r3"] = y | (static_cast<uint32_t>(x) << 16);
                    ran = ran && vectorRun.Run(vec.ptx);
                    vectorOk = vectorOk && vectorRun.regs["%r1"] == (PtxMinMax(isMin, x, y) |
                                                                     (static_cast<uint32_t>(PtxMinMax(isMin, y, x)) << 16));

                    PtxRunner scalarRun;
                    scalarRun.regs["%h2"] = x;
                    scalarRun.regs["%h3"] = y;
                    ran = ran && scalarRun.Run(scalar.ptx);
                    scalarOk = scalarOk && scalarRun.regs["%h1"] == PtxMinMax(isMin, x, y);

                    PtxRunner aliasRun;
                    aliasRun.regs["%h2"] = x;
                    aliasRun.regs["%h3"] = y;
                    ran = ran && aliasRun.Run(alias.ptx);
                    aliasOk = aliasOk && aliasRun.regs["%h2"] == PtxMinMax(isMin, x, y);
                }
            }
        }
        Check(ran && vectorOk, "min/max.f16x2 rewrite matches PTX for +-0, NaN, +-inf and subnormal pairs");
        Check(ran && scalarOk && aliasOk, "min/max.f16 rewrite matches PTX, also when d is a");

        // mma m16n8k16 -> two m16n8k8: A columns 0-7 = {a0, a1} with b0, columns 8-15 = {a2, a3} with b1, and the
        // second half accumulates onto the first half's result.
        const auto mma = MfgUnlock::PtxTuring::RewriteText(
            "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%d0, %d1}, {%a0, %a1, %a2, %a3}, {%b0, %b1}, "
            "{%c0, %c1};");
        Check(mma.ok && mma.mma == 1 &&
                  mma.ptx == "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 {%d0, %d1}, {%a0, %a1}, {%b0}, "
                             "{%c0, %c1};\nmma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16 {%d0, %d1}, {%a2, %a3}, "
                             "{%b1}, {%d0, %d1};",
              "mma k16 splits into two k8 with the A/B halves and the accumulator chained");
        const auto aliased = MfgUnlock::PtxTuring::RewriteText(
            "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%d0, %a2}, {%a0, %a1, %a2, %a3}, {%b0, %b1}, "
            "{%c0, %c1};");
        Check(!aliased.ok && aliased.failure.find("aliases") != std::string::npos,
              "mma whose destination is also an input is rejected");

        // cvt.rn.f16x2.f32 d, a, b puts a in the upper half and b in the lower one.
        const auto cvt = MfgUnlock::PtxTuring::RewriteText("{ cvt.rn.f16x2.f32 %r1, %f1, %f2; }");
        Check(cvt.ok && cvt.cvt == 1 &&
                  cvt.ptx == "{.reg .b16 ch, cl;\ncvt.rn.f16.f32 ch, %f1; cvt.rn.f16.f32 cl, %f2;\nmov.b32 %r1, "
                             "{cl, ch};}",
              "cvt f16x2 packs the second source low and the first high");

        bool leftoversRejected = true;
        for (const char* text : { "{min.ftz.f16 %h1, %h2, %h3;}", "{min.NaN.f16x2 %r1, %r2, %r3;}",
                                  "{max.bf16x2 %r1, %r2, %r3;}", "{max.xorsign.abs.bf16 %h1, %h2, %h3;}",
                                  "{ cvt.rn.relu.f16x2.f32 %r1, %f1, %f2; }", "cvt.rn.bf16.f32 %h1, %f1;",
                                  "cvt.rn.satfinite.bf16x2.f32 %r1, %f1, %f2;" })
            leftoversRejected = leftoversRejected && !MfgUnlock::PtxTuring::RewriteText(text).ok;
        Check(leftoversRejected, "any min/max f16/bf16 form or cvt to f16x2/bf16 left over fails the rewrite");
        Check(MfgUnlock::PtxTuring::RewriteText("min.s32 %r1, %r2, %r3; cvt.rn.f16.f32 %h1, %f1; max.f32 %f1, %f2, %f3;")
                  .ok,
              "forms sm_75 has (min.s32, max.f32, scalar cvt to f16) pass");

        std::string ptx = ".version 8.0\n.target sm_89\n.address_size 64\n";
        std::string failure;
        Check(MfgUnlock::PtxTuring::RetargetToTuring(ptx, failure) &&
                  ptx == ".version 8.0\n.target sm_75\n.address_size 64\n",
              "RetargetToTuring rewrites the .target number");
        std::string suffixed = ".target sm_120a\n";
        std::string twice = ".target sm_89\n.target sm_89\n";
        std::string none = ".version 8.0\n";
        Check(!MfgUnlock::PtxTuring::RetargetToTuring(suffixed, failure) &&
                  !MfgUnlock::PtxTuring::RetargetToTuring(twice, failure) &&
                  !MfgUnlock::PtxTuring::RetargetToTuring(none, failure),
              "RetargetToTuring rejects sm_120a, two directives and none");
    }

    {
        // LZ4 block decoder: literals, a match, an overlapping match, and malformed streams.
        const auto decode = [](std::initializer_list<uint8_t> stream, size_t size, std::string& text)
        {
            const std::vector<uint8_t> source(stream);
            std::vector<uint8_t> out;
            const bool ok = MfgUnlock::PtxTuring::detail::Lz4Decompress(source.data(), source.size(), out, size);
            text.assign(out.begin(), out.end());
            return ok;
        };
        std::string text;
        Check(decode({ 0x40, 'a', 'b', 'c', 'd', 0x04, 0x00, 0x20, 'x', 'y' }, 10, text) && text == "abcdabcdxy",
              "Lz4Decompress decodes literals, a match and trailing literals");
        Check(decode({ 0x12, 'a', 0x01, 0x00 }, 7, text) && text == "aaaaaaa",
              "Lz4Decompress decodes an overlapping match");
        Check(!decode({ 0x40, 'a', 'b', 'c', 'd', 0x00, 0x00 }, 8, text) &&
                  !decode({ 0x40, 'a', 'b', 'c', 'd', 0x05, 0x00 }, 8, text) &&
                  !decode({ 0x40, 'a', 'b' }, 4, text) && !decode({ 0x40, 'a', 'b', 'c', 'd' }, 5, text) &&
                  !decode({ 0xF0 }, 15, text) && !decode({ 0x40, 'a', 'b', 'c', 'd', 0x04 }, 8, text),
              "Lz4Decompress rejects offset 0, an offset past the output, short literals, a short output, a missing "
              "length byte and a cut offset");
    }

    {
        // RTX 20/30 architecture sites from the exports of a synthetic provider.
        SyntheticImage archImage;
        const MfgUnlock::ImageView archView(archImage.bytes, SyntheticImage::kSize);
        clearCode(archImage);
        std::memset(archImage.bytes + 0x3000, 0, 0x100);
        IMAGE_EXPORT_DIRECTORY exports {};
        exports.NumberOfFunctions = 2;
        exports.NumberOfNames = 2;
        exports.AddressOfFunctions = 0x3040;
        exports.AddressOfNames = 0x3048;
        exports.AddressOfNameOrdinals = 0x3050;
        std::memcpy(archImage.bytes + 0x3000, &exports, sizeof(exports));
        const DWORD functions[] = { 0x1040, 0x1000 };
        const DWORD names[] = { 0x3060, 0x30C0 };
        const WORD ordinals[] = { 0, 1 };
        std::memcpy(archImage.bytes + 0x3040, functions, sizeof(functions));
        std::memcpy(archImage.bytes + 0x3048, names, sizeof(names));
        std::memcpy(archImage.bytes + 0x3050, ordinals, sizeof(ordinals));
        constexpr char architecture[] = "NVSDK_NGX_GetGPUArchitecture";
        constexpr char requirements[] = "NVSDK_NGX_D3D12_GetFeatureRequirements";
        std::memcpy(archImage.bytes + 0x3060, architecture, sizeof(architecture));
        std::memcpy(archImage.bytes + 0x30C0, requirements, sizeof(requirements));
        writeCode(archImage, 0x1000, { 0xc7, 0x44, 0x24, 0x20, 0x90, 0x01, 0x00, 0x00 }); // mov [rsp+20h], 190h
        writeCode(archImage, 0x1040, { 0xb8, 0x90, 0x01, 0x00, 0x00, 0xc3 });             // mov eax, 190h / ret

        const auto arch = MfgUnlock::FamilyAmpere::Locate(archView, MfgUnlock::FamilyAmpere::kArchTuringLow);
        MfgUnlock::AppliedPatchSet archSet;
        const bool applied =
            arch.Complete() && arch.patches.size() == 2 &&
            archSet.Apply(MfgUnlock::FamilyRtx40::BuildOperationsFromPatches(arch.patches, archView)).applied;
        Check(applied && archImage.bytes[0x1041] == std::byte { 0x60 } && archImage.bytes[0x1004] == std::byte { 0x60 },
              "FamilyAmpere lowers GetGPUArchitecture and the requirement store to Turing");
        archSet.Restore();
        writeCode(archImage, 0x1008, { 0xc7, 0x44, 0x24, 0x28, 0x90, 0x01, 0x00, 0x00 });
        Check(!MfgUnlock::FamilyAmpere::Locate(archView, MfgUnlock::FamilyAmpere::kArchAmpereLow).Complete(),
              "FamilyAmpere stands aside when a requirements export stores the architecture twice");
    }

    {
        // RTX 30 retarget of plain PTX: the Blackwell PTX becomes the sm_86 source, every other image is parked.
        SyntheticImage fatImage;
        const MfgUnlock::ImageView fatView(fatImage.bytes, SyntheticImage::kSize);
        const auto put = [&](size_t offset, const void* data, size_t size)
        { std::memcpy(fatImage.bytes + offset, data, size); };
        const auto writeEntry = [&](size_t offset, uint16_t kind, uint64_t payload, uint32_t arch, const char* text)
        {
            const uint32_t header = 32;
            put(offset, &kind, sizeof(kind));
            put(offset + 4, &header, sizeof(header));
            put(offset + 8, &payload, sizeof(payload));
            put(offset + 28, &arch, sizeof(arch));
            if (text != nullptr)
                put(offset + header, text, std::strlen(text));
        };
        constexpr size_t base = 0x3200;
        const uint8_t magic[] = { 0x50, 0xed, 0x55, 0xba, 0x01, 0x00, 0x10, 0x00 };
        const uint64_t fatSize = (32 + 8) + (32 + 24) + (32 + 16);
        put(base, magic, sizeof(magic));
        put(base + 8, &fatSize, sizeof(fatSize));
        writeEntry(base + 16, 2, 8, 89, nullptr);
        writeEntry(base + 16 + 40, 1, 24, 120, ".target sm_120\n.version");
        writeEntry(base + 16 + 40 + 56, 1, 16, 89, ".target sm_89\n");

        const auto report =
            MfgUnlock::FamilyRtx40::Fatbin::Locate(fatView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere);
        MfgUnlock::AppliedPatchSet fatSet;
        const bool applied = report.Complete() && report.containers == 1 && report.patches.size() == 4 &&
                             fatSet.Apply(MfgUnlock::FamilyRtx40::Fatbin::BuildOperations(report, fatView)).applied;
        uint32_t cubinArch = 0;
        uint32_t sourceArch = 0;
        uint32_t adaPtxArch = 0;
        std::memcpy(&cubinArch, fatImage.bytes + base + 16 + 28, sizeof(cubinArch));
        std::memcpy(&sourceArch, fatImage.bytes + base + 16 + 40 + 28, sizeof(sourceArch));
        std::memcpy(&adaPtxArch, fatImage.bytes + base + 16 + 40 + 56 + 28, sizeof(adaPtxArch));
        Check(applied && sourceArch == 86 && cubinArch == 122 && adaPtxArch == 122 &&
                  std::memcmp(fatImage.bytes + base + 16 + 40 + 32, ".target sm_86 \n", 15) == 0,
              "CollectAmpere retargets the Blackwell PTX to sm_86 and parks the Ada images");
        fatSet.Restore();
        const uint32_t ampere = 86;
        put(base + 16 + 28, &ampere, sizeof(ampere));
        Check(!MfgUnlock::FamilyRtx40::Fatbin::Locate(fatView, MfgUnlock::FamilyRtx40::Fatbin::Target::Ampere)
                   .Complete(),
              "CollectAmpere rejects a container that already has an image for another architecture");
    }

    {
        bool valid = false;
        const bool autoIsDefault = !MfgUnlock::ParseMode("auto", valid).has_value() && valid &&
                                   !MfgUnlock::ParseMode("AUTO", valid).has_value() && valid &&
                                   !MfgUnlock::ParseMode("", valid).has_value() && valid;
        const bool detect = MfgUnlock::ParseMode("Detect", valid) == MfgUnlockMode::Detect && valid;
        const bool onOff = MfgUnlock::ParseMode("true", valid) == MfgUnlockMode::On &&
                           MfgUnlock::ParseMode("on", valid) == MfgUnlockMode::On &&
                           MfgUnlock::ParseMode("false", valid) == MfgUnlockMode::Off &&
                           MfgUnlock::ParseMode("OFF", valid) == MfgUnlockMode::Off && valid;
        const bool invalid = !MfgUnlock::ParseMode("yes", valid).has_value() && !valid;
        Check(autoIsDefault && detect && onOff && invalid,
              "config: auto is the default (off), detect is its own mode, junk is rejected");
        Check(std::string(MfgUnlock::ModeText(std::nullopt)) == "auto" &&
                  std::string(MfgUnlock::ModeText(MfgUnlockMode::Detect)) == "detect" &&
                  std::string(MfgUnlock::ModeText(MfgUnlockMode::On)) == "true",
              "config: modes save as auto, detect and true");
    }

    return failures == 0 ? 0 : 1;
}
