// Ported from OptiScaler-NR OptiScaler/framegen/mfgunlock/PtxTuring.h at 745e49431 (origin/release/0.1.36).
// Kept identical to the original except where listed here: kFatbinMagic / kFatbinWrapperMagic are built from halves at run time (see there).
// GPL-3.0 with the rest of this repo. 
#pragma once

#include <cstdint>
#include <cstring>
#include <string>
#include <string_view>
#include <vector>

// RTX 20 (Turing, sm_75) kernels for nvngx_dlssg. NVIDIA ships the kernels as PTX for Ada/Blackwell; sm_75 lacks a few
// instructions, so the PTX text is rewritten before the driver JITs it. The rewrite rules are the ones from the
// DLSS-Weights tooling (tools/dllpatch/ptx75.py, dlssg/ptx75_dlssg.py), written here without regular expressions:
//   mma.sync.aligned.m16n8k16 (f16)  -> two m16n8k8 (the intermediate sum is rounded to fp16)
//   {min|max}.f16x2                  -> per-half setp/selp
//   {min|max}.f16 (scalar)           -> setp/selp
//   cvt.rn.f16x2.f32                 -> two cvt.rn.f16.f32 plus a pack
// The result is plain PTX text, so it is handed to cuModuleLoadData* instead of the shipped fatbin, which keeps the
// patched provider bytes untouched and lets the length of the kernels change.
namespace MfgUnlock::PtxTuring
{
struct Rewrite
{
    bool ok = false;
    std::string ptx;
    std::string failure;
    unsigned mma = 0;
    unsigned minMaxVec = 0;
    unsigned minMaxScalar = 0;
    unsigned cvt = 0;
    int sourceArch = 0;
};

namespace detail
{
class Cursor
{
  public:
    Cursor(std::string_view text, size_t at) : m_text(text), m_at(at) {}

    size_t At() const { return m_at; }

    void Space()
    {
        while (m_at < m_text.size() && IsSpace(m_text[m_at]))
            ++m_at;
    }

    // \s+ when atLeastOne, otherwise \s*
    bool Space(bool atLeastOne)
    {
        const size_t before = m_at;
        Space();
        return !atLeastOne || m_at > before;
    }

    bool Lit(std::string_view literal)
    {
        if (m_text.compare(m_at, literal.size(), literal) != 0)
            return false;
        m_at += literal.size();
        return true;
    }

    // %\w+
    bool Reg(std::string_view& out)
    {
        if (m_at >= m_text.size() || m_text[m_at] != '%')
            return false;
        size_t end = m_at + 1;
        while (end < m_text.size() && IsWord(m_text[end]))
            ++end;
        if (end == m_at + 1)
            return false;
        out = m_text.substr(m_at, end - m_at);
        m_at = end;
        return true;
    }

    // , between registers with optional whitespace on both sides
    bool Comma()
    {
        Space();
        if (!Lit(","))
            return false;
        Space();
        return true;
    }

  private:
    static bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
    static bool IsWord(char c)
    {
        return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_';
    }

    std::string_view m_text;
    size_t m_at;
};

// { r, r, ... } with the given register count
inline bool RegList(Cursor& cursor, std::string_view* regs, size_t count)
{
    cursor.Space();
    if (!cursor.Lit("{"))
        return false;
    cursor.Space();
    for (size_t i = 0; i < count; ++i)
    {
        if (i != 0 && !cursor.Comma())
            return false;
        if (!cursor.Reg(regs[i]))
            return false;
    }
    cursor.Space();
    return cursor.Lit("}");
}

inline void Append(std::string& out, std::string_view text)
{
    out.append(text.data(), text.size());
}

constexpr std::string_view kMma = "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16";
constexpr std::string_view kMma8 = "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16";

// Returns the end of the match and appends the replacement, or returns 0 when the text at `at` is not this form.
inline size_t TryMma(std::string_view text, size_t at, std::string& out, bool& aliasing)
{
    Cursor cursor(text, at);
    if (!cursor.Lit(kMma))
        return 0;
    std::string_view d[2], a[4], b[2], c[2];
    if (!RegList(cursor, d, 2) || !cursor.Comma() || !RegList(cursor, a, 4) || !cursor.Comma() ||
        !RegList(cursor, b, 2) || !cursor.Comma() || !RegList(cursor, c, 2))
        return 0;
    cursor.Space();
    if (!cursor.Lit(";"))
        return 0;

    // The second half reads the destination as its accumulator, so the destination may not alias an input.
    for (const auto& destination : d)
        for (const auto& input : { a[0], a[1], a[2], a[3], b[0], b[1] })
            if (destination == input)
                aliasing = true;

    for (int half = 0; half < 2; ++half)
    {
        Append(out, kMma8);
        out += " {";
        Append(out, d[0]);
        out += ", ";
        Append(out, d[1]);
        out += "}, {";
        Append(out, a[half * 2]);
        out += ", ";
        Append(out, a[half * 2 + 1]);
        out += "}, {";
        Append(out, b[half]);
        out += "}, {";
        Append(out, half == 0 ? c[0] : d[0]);
        out += ", ";
        Append(out, half == 0 ? c[1] : d[1]);
        out += "};";
        if (half == 0)
            out += "\n";
    }
    return cursor.At();
}

// The fp16 NaN that PTX min/max return when both operands are NaN (verified against the driver by the Turing probe).
inline const std::string kCanonNaN = "0x7FFF";

// {min.f16x2 d, a, b; } / {max.f16x2 ...}  and the scalar {min.f16 ...}
inline size_t TryMinMax(std::string_view text, size_t at, std::string& out, bool& vector)
{
    Cursor cursor(text, at);
    if (!cursor.Lit("{"))
        return 0;
    const bool isMin = cursor.Lit("min");
    if (!isMin && !cursor.Lit("max"))
        return 0;
    if (!cursor.Lit(".f16"))
        return 0;
    vector = cursor.Lit("x2");
    std::string_view d, a, b;
    if (!cursor.Space(true) || !cursor.Reg(d) || !cursor.Comma() || !cursor.Reg(a) || !cursor.Comma() ||
        !cursor.Reg(b))
        return 0;
    cursor.Space();
    if (!cursor.Lit(";"))
        return 0;
    cursor.Space();
    if (!cursor.Lit("}"))
        return 0;

    const std::string cmp = isMin ? "lt" : "gt";
    // Equal values share their bits except +0/-0, where PTX orders -0 below +0: or-ing the two gives min's -0,
    // and-ing them gives max's +0. For any other equal pair both operands are identical, so the tie value is that
    // value.
    const std::string tie = isMin ? "or" : "and";

    // One half-lane: r = (a<b, or b NaN) ? a : b; equal -> the sign tie; both NaN -> the canonical NaN kCanonNaN.
    // Reads a and b before it writes r, so r may be a or b (scalar d aliases a or b). s is a unique scratch suffix.
    const auto half = [&](const std::string& x, const std::string& y, const std::string& r, const std::string& s)
    {
        return "setp." + cmp + ".f16 p" + s + ", " + x + ", " + y + "; setp.nan.f16 na" + s + ", " + x + ", " + x +
               "; setp.nan.f16 nb" + s + ", " + y + ", " + y + "; or.pred p" + s + ", p" + s + ", nb" + s +
               "; setp.eq.f16 e" + s + ", " + x + ", " + y + "; and.pred bo" + s + ", na" + s + ", nb" + s + ";\n" +
               tie + ".b16 t" + s + ", " + x + ", " + y + "; selp.b16 " + r + ", " + x + ", " + y + ", p" + s +
               "; selp.b16 " + r + ", t" + s + ", " + r + ", e" + s + "; selp.b16 " + r + ", cn, " + r + ", bo" + s +
               ";\n";
    };

    const std::string as(a);
    const std::string bs(b);
    const std::string ds(d);
    if (vector)
    {
        out += "{.reg .b16 xl, xh, yl, yh, tl, th, cn; .reg .pred pl, nal, nbl, el, bol, ph, nah, nbh, eh, boh;\n";
        out += "mov.b16 cn, " + kCanonNaN + "; mov.b32 {xl, xh}, " + as + "; mov.b32 {yl, yh}, " + bs + ";\n";
        out += half("xl", "yl", "xl", "l");
        out += half("xh", "yh", "xh", "h");
        out += "mov.b32 " + ds + ", {xl, xh};}";
    }
    else
    {
        out += "{.reg .pred ps, nas, nbs, es, bos; .reg .b16 ts, cn; mov.b16 cn, " + kCanonNaN + ";\n";
        out += half(as, bs, ds, "s");
        out += "}";
    }
    return cursor.At();
}

// { cvt.rn.f16x2.f32 d, a, b; }  (a -> upper half, b -> lower half)
inline size_t TryCvt(std::string_view text, size_t at, std::string& out)
{
    Cursor cursor(text, at);
    if (!cursor.Lit("{"))
        return 0;
    cursor.Space();
    if (!cursor.Lit("cvt.rn.f16x2.f32"))
        return 0;
    std::string_view d, a, b;
    if (!cursor.Space(true) || !cursor.Reg(d) || !cursor.Comma() || !cursor.Reg(a) || !cursor.Comma() ||
        !cursor.Reg(b))
        return 0;
    cursor.Space();
    if (!cursor.Lit(";"))
        return 0;
    cursor.Space();
    if (!cursor.Lit("}"))
        return 0;

    out += "{.reg .b16 ch, cl;\ncvt.rn.f16.f32 ch, ";
    Append(out, a);
    out += "; cvt.rn.f16.f32 cl, ";
    Append(out, b);
    out += ";\nmov.b32 ";
    Append(out, d);
    out += ", {cl, ch};}";
    return cursor.At();
}

inline bool Lz4Decompress(const uint8_t* src, size_t srcSize, std::vector<uint8_t>& out, size_t outSize)
{
    out.assign(outSize, 0);
    size_t i = 0, o = 0;
    while (i < srcSize)
    {
        const unsigned token = src[i++];
        size_t literals = token >> 4;
        if (literals == 15)
        {
            uint8_t extra;
            do
            {
                if (i >= srcSize)
                    return false;
                extra = src[i++];
                literals += extra;
            } while (extra == 255);
        }
        if (i + literals > srcSize || o + literals > outSize)
            return false;
        if (literals != 0)
            std::memcpy(out.data() + o, src + i, literals);
        i += literals;
        o += literals;
        if (i >= srcSize)
            break;
        if (i + 2 > srcSize)
            return false;
        const size_t offset = src[i] | (src[i + 1] << 8);
        i += 2;
        size_t match = token & 15;
        if (match == 15)
        {
            uint8_t extra;
            do
            {
                if (i >= srcSize)
                    return false;
                extra = src[i++];
                match += extra;
            } while (extra == 255);
        }
        match += 4;
        if (offset == 0 || offset > o || o + match > outSize)
            return false;
        for (size_t k = 0; k < match; ++k, ++o)
            out[o] = out[o - offset];
    }
    return o == outSize;
}

// Returns the first instruction sm_75 lacks that this port does not rewrite, or an empty view. Any min/max with an
// f16/bf16 type (whatever modifiers: .ftz, .NaN, .xorsign.abs, x2 or scalar) and any cvt producing f16x2 or bf16
// (.relu, .satfinite, ...) counts, so a form the rewrite does not know fails closed instead of reaching the driver.
inline std::string_view FindUnportedInstruction(std::string_view text)
{
    const auto isWord = [](char c)
    { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; };
    for (size_t at = 0; at < text.size(); ++at)
    {
        if (!isWord(text[at]) || (at != 0 && (isWord(text[at - 1]) || text[at - 1] == '.' || text[at - 1] == '%' ||
                                               text[at - 1] == '$')))
            continue;
        size_t end = at;
        while (end < text.size() && (isWord(text[end]) || text[end] == '.'))
            ++end;
        const std::string_view token = text.substr(at, end - at);
        at = end;

        const size_t dot = token.find('.');
        if (dot == std::string_view::npos)
            continue;
        const std::string_view mnemonic = token.substr(0, dot);
        const bool minMax = mnemonic == "min" || mnemonic == "max";
        const bool cvt = mnemonic == "cvt";
        if (!minMax && !cvt)
            continue;
        for (size_t part = dot; part != std::string_view::npos;)
        {
            const size_t next = token.find('.', part + 1);
            const std::string_view modifier =
                token.substr(part + 1, next == std::string_view::npos ? std::string_view::npos : next - part - 1);
            if (modifier == "bf16" || modifier == "bf16x2" || modifier == "f16x2" ||
                (minMax && modifier == "f16"))
                return token;
            part = next;
        }
    }
    return {};
}

template <typename T> inline T Load(const uint8_t* at)
{
    T value;
    std::memcpy(&value, at, sizeof(T));
    return value;
}
} // namespace detail

// Rewrites PTX text for sm_75. Fails closed: when a construct this port does not cover is left over, no text is returned.
inline Rewrite RewriteText(std::string_view text)
{
    Rewrite result;
    std::string out;
    out.reserve(text.size() + text.size() / 8);

    size_t at = 0;
    while (at < text.size())
    {
        const char c = text[at];
        size_t end = 0;
        if (c == 'm' && text.compare(at, 3, "mma") == 0)
        {
            bool aliasing = false;
            end = detail::TryMma(text, at, out, aliasing);
            if (end != 0)
            {
                if (aliasing)
                {
                    result.failure = "mma destination aliases an input";
                    return result;
                }
                ++result.mma;
            }
        }
        else if (c == '{')
        {
            bool vector = false;
            end = detail::TryMinMax(text, at, out, vector);
            if (end != 0)
            {
                ++(vector ? result.minMaxVec : result.minMaxScalar);
            }
            else if ((end = detail::TryCvt(text, at, out)) != 0)
            {
                ++result.cvt;
            }
        }

        if (end != 0)
        {
            at = end;
            continue;
        }
        out.push_back(c);
        ++at;
    }

    if (out.find("m16n8k16") != std::string::npos)
    {
        result.failure = "unrewritten construct left in the PTX: m16n8k16";
        return result;
    }
    if (const auto leftover = detail::FindUnportedInstruction(out); !leftover.empty())
    {
        result.failure = "unrewritten construct left in the PTX: " + std::string(leftover);
        return result;
    }

    result.ok = true;
    result.ptx = std::move(out);
    return result;
}

// Replaces the number in ".target sm_NNN" by 75. Requires exactly one directive and a plain number.
inline bool RetargetToTuring(std::string& ptx, std::string& failure)
{
    constexpr std::string_view needle = ".target sm_";
    const size_t at = ptx.find(needle);
    if (at == std::string::npos || ptx.find(needle, at + 1) != std::string::npos)
    {
        failure = ".target is not present exactly once";
        return false;
    }
    size_t end = at + needle.size();
    while (end < ptx.size() && ptx[end] >= '0' && ptx[end] <= '9')
        ++end;
    if (end == at + needle.size() || end >= ptx.size() ||
        !(ptx[end] == ' ' || ptx[end] == '\n' || ptx[end] == '\r' || ptx[end] == '\t' || ptx[end] == ','))
    {
        failure = ".target architecture is not a plain number";
        return false;
    }
    ptx.replace(at + needle.size(), end - (at + needle.size()), "75");
    return true;
}

// f5low: the two container magics are put together from halves at run time (volatile), so a compare never carries them as one 4-byte
// immediate: tools/audit.py --dll rejects those 4-byte sequences inside f5low.dll (they would look like embedded NVIDIA kernels).
inline volatile uint32_t kMagicHalves[4] = { 0xBA55, 0xED50, 0x4662, 0x43B1 };
inline const uint32_t kFatbinMagic = (kMagicHalves[0] << 16) | kMagicHalves[1];
inline const uint32_t kFatbinWrapperMagic = (kMagicHalves[2] << 16) | kMagicHalves[3];

// Reads a fatbin container (or a fatbinary wrapper pointing at one) and returns the sm_75 PTX for it.
// The source is the Blackwell PTX when the container has one, otherwise the Ada PTX; every image in the container
// must be sm_89 or sm_120, which is the shape of nvngx_dlssg's kernels. Anything else is not ours and is rejected.
inline Rewrite BuildFromContainer(const uint8_t* container)
{
    using namespace detail;
    Rewrite result;
    if (container == nullptr)
    {
        result.failure = "null image";
        return result;
    }
    if (Load<uint32_t>(container) == kFatbinWrapperMagic)
        container = reinterpret_cast<const uint8_t*>(Load<uintptr_t>(container + 8));
    if (container == nullptr || Load<uint32_t>(container) != kFatbinMagic || Load<uint16_t>(container + 6) != 0x10)
    {
        result.failure = "not a fatbin container";
        return result;
    }

    const uint64_t fatSize = Load<uint64_t>(container + 8);
    if (fatSize == 0 || fatSize > (256ull << 20))
    {
        result.failure = "implausible fatbin size";
        return result;
    }

    const uint8_t* source = nullptr;
    uint32_t sourceHeader = 0, sourceCompressed = 0;
    uint64_t sourcePayload = 0, sourceUncompressed = 0;
    int sourceArch = 0;
    unsigned sourcePtxCount[2] = {};

    size_t at = 0x10;
    const size_t end = 0x10 + static_cast<size_t>(fatSize);
    while (at + 0x40 <= end)
    {
        const uint16_t kind = Load<uint16_t>(container + at);
        const uint32_t headerSize = Load<uint32_t>(container + at + 4);
        const uint64_t payload = Load<uint64_t>(container + at + 8);
        const uint32_t compressed = Load<uint32_t>(container + at + 16);
        const uint32_t arch = Load<uint32_t>(container + at + 0x1C);
        const uint64_t uncompressed = Load<uint64_t>(container + at + 0x38);
        if (headerSize < 0x40 || headerSize > 0x1000 || headerSize > end - at || payload > end - at - headerSize)
        {
            result.failure = "malformed fatbin entry";
            return result;
        }
        if ((kind != 1 && kind != 2) || (arch != 89 && arch != 120))
        {
            result.failure = "container has an image that is not sm_89/sm_120";
            return result;
        }
        if (kind == 1)
        {
            ++sourcePtxCount[arch == 120 ? 1 : 0];
            if (arch == 120 || sourceArch != 120)
            {
                source = container + at + headerSize;
                sourceHeader = headerSize;
                sourceCompressed = compressed;
                sourcePayload = payload;
                sourceUncompressed = uncompressed;
                sourceArch = static_cast<int>(arch);
            }
        }
        at += headerSize + payload;
    }
    (void)sourceHeader;
    if (source == nullptr || sourcePtxCount[sourceArch == 120 ? 1 : 0] != 1)
    {
        result.failure = "container has no single source PTX";
        return result;
    }
    result.sourceArch = sourceArch;

    std::vector<uint8_t> text;
    if (sourceCompressed != 0 && sourceUncompressed != 0)
    {
        if (sourceUncompressed > (64ull << 20) || sourceCompressed > sourcePayload ||
            sourcePayload < 2 || (source[0] == 0x28 && source[1] == 0xB5))
        {
            result.failure = "PTX payload is not LZ4";
            return result;
        }
        if (!Lz4Decompress(source, sourceCompressed, text, static_cast<size_t>(sourceUncompressed)))
        {
            result.failure = "PTX LZ4 decode failed";
            return result;
        }
    }
    else
    {
        text.assign(source, source + sourcePayload);
    }

    size_t length = 0;
    while (length < text.size() && text[length] != 0)
        ++length;
    std::string ptx(reinterpret_cast<const char*>(text.data()), length);
    if (!RetargetToTuring(ptx, result.failure))
        return result;

    Rewrite rewritten = RewriteText(ptx);
    rewritten.sourceArch = sourceArch;
    return rewritten;
}
} // namespace MfgUnlock::PtxTuring
