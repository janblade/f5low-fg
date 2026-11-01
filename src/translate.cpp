// translate.cpp - see translate.h. Our own translator from DXIL disassembly text to PTX; the text it reads is the
// disassembly dxcompiler.dll writes (LLVM IR flavour, one instruction a line).
//
// The text is parsed with plain string code on the hot path (an instruction is a handful of tokens) and with
// std::regex only for the rare, irregular lines (metadata, groupshared pointers). Every PTX line is built in the
// same order as the offline tool built it: temporaries are numbered in creation order, so the order of the calls
// that create them below is part of the output.
#include "translate.h"

#include <algorithm>
#include <bit>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <format>
#include <map>
#include <optional>
#include <regex>
#include <set>
#include <stdexcept>
#include <unordered_map>
#include <unordered_set>

namespace translate {
namespace {

struct Err : std::runtime_error {
    using std::runtime_error::runtime_error;
};
[[noreturn]] void die(const std::string& m) { throw Err(m); }

bool IsSpace(char c) { return c == ' ' || c == '\t' || c == '\n' || c == '\r' || c == '\f' || c == '\v'; }
bool IsDigit(char c) { return c >= '0' && c <= '9'; }
bool IsWord(char c) { return IsDigit(c) || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || c == '_'; }

std::string Strip(const std::string& s) {
    size_t b = 0, e = s.size();
    while (b < e && IsSpace(s[b])) ++b;
    while (e > b && IsSpace(s[e - 1])) --e;
    return s.substr(b, e - b);
}
bool Starts(const std::string& s, const char* p) { return s.compare(0, std::strlen(p), p) == 0; }
bool Contains(const std::string& s, const char* p) { return s.find(p) != std::string::npos; }
bool EndsWith(const std::string& s, const char* p) {
    size_t n = std::strlen(p);
    return s.size() >= n && s.compare(s.size() - n, n, p) == 0;
}

std::vector<std::string> SplitChar(const std::string& s, char c) {
    std::vector<std::string> out;
    size_t b = 0;
    for (;;) {
        size_t e = s.find(c, b);
        if (e == std::string::npos) {
            out.push_back(s.substr(b));
            return out;
        }
        out.push_back(s.substr(b, e - b));
        b = e + 1;
    }
}

// Python's int(): optional sign, digits.
long long ToInt(const std::string& tok0) {
    std::string t = Strip(tok0);
    size_t i = 0;
    bool neg = false;
    if (i < t.size() && (t[i] == '-' || t[i] == '+')) neg = t[i++] == '-';
    if (i >= t.size()) die("not an integer: '" + tok0 + "'");
    long long v = 0;
    for (; i < t.size(); ++i) {
        if (!IsDigit(t[i])) die("not an integer: '" + tok0 + "'");
        v = v * 10 + (t[i] - '0');
    }
    return neg ? -v : v;
}

// Digits starting at `pos` (at least one): returns the end, or pos when there are none.
size_t DigitsEnd(const std::string& s, size_t pos) {
    size_t e = pos;
    while (e < s.size() && IsDigit(s[e])) ++e;
    return e;
}

std::vector<std::string> SplitLines(const std::string& text) {
    std::vector<std::string> out;
    size_t b = 0;
    while (b < text.size()) {
        size_t e = text.find('\n', b);
        size_t stop = e == std::string::npos ? text.size() : e;
        size_t cut = stop;
        if (cut > b && text[cut - 1] == '\r') --cut;
        out.push_back(text.substr(b, cut - b));
        if (e == std::string::npos) break;
        b = e + 1;
    }
    return out;
}

std::string Join(const std::vector<std::string>& v, const char* sep) {
    std::string out;
    for (size_t i = 0; i < v.size(); ++i) {
        if (i) out += sep;
        out += v[i];
    }
    return out;
}

uint32_t F32Bits(float f) {
    uint32_t u;
    std::memcpy(&u, &f, 4);
    return u;
}
float BitsF32(uint32_t u) {
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// An LLVM float literal (decimal or 64-bit hex) as a PTX f32 immediate.
std::string HexFloat(const std::string& tok) {
    double v;
    if (Starts(tok, "0x")) {
        uint64_t q = std::strtoull(tok.c_str() + 2, nullptr, 16);
        std::memcpy(&v, &q, 8);
    } else {
        v = std::strtod(tok.c_str(), nullptr);
    }
    return std::format("0f{:08X}", F32Bits(static_cast<float>(v)));
}

using Block = std::vector<std::string>;

// ", !tbaa !12" style alias metadata, no meaning here
std::string DropAliasMetadata(const std::string& s) {
    if (!Contains(s, ", !")) return s;
    static const char* kinds[] = {"tbaa", "range", "noalias", "alias.scope"};
    std::string out;
    size_t i = 0;
    while (i < s.size()) {
        if (s.compare(i, 3, ", !") == 0) {
            bool hit = false;
            for (const char* k : kinds) {
                size_t kl = std::strlen(k);
                if (s.compare(i + 3, kl, k) == 0 && s.compare(i + 3 + kl, 2, " !") == 0) {
                    size_t e = DigitsEnd(s, i + 3 + kl + 2);
                    if (e > i + 3 + kl + 2) {
                        i = e;
                        hit = true;
                    }
                    break;
                }
            }
            if (hit) continue;
        }
        out += s[i++];
    }
    return out;
}

// br i1 C, label %X, label %Y  /  br label %X
bool ParseCondBr(const std::string& t, std::string& c, std::string& x, std::string& y) {
    if (!Starts(t, "br i1 ")) return false;
    size_t e = t.find(' ', 6);
    if (e == std::string::npos || e < 8 || t[e - 1] != ',') return false;
    c = t.substr(6, e - 1 - 6);
    if (t.compare(e, 8, " label %") != 0) return false;
    size_t p = e + 8, q = DigitsEnd(t, p);
    if (q == p) return false;
    x = t.substr(p, q - p);
    if (t.compare(q, 9, ", label %") != 0) return false;
    p = q + 9;
    q = DigitsEnd(t, p);
    if (q == p || q != t.size()) return false;
    y = t.substr(p, q - p);
    return true;
}
bool ParseBr(const std::string& t, std::string& x) {
    if (!Starts(t, "br label %")) return false;
    size_t p = 10, q = DigitsEnd(t, p);
    if (q == p || q != t.size()) return false;
    x = t.substr(p, q - p);
    return true;
}

// "%12 = rest": returns the name and the position of the rest
bool ParseAssign(const std::string& s, std::string& n, std::string& rhs) {
    if (s.empty() || s[0] != '%') return false;
    size_t q = DigitsEnd(s, 1);
    if (q == 1 || s.compare(q, 3, " = ") != 0) return false;
    n = s.substr(1, q - 1);
    rhs = s.substr(q + 3);
    return true;
}

// Every %<digits> in the text of an instruction (the right-hand side of an assignment, or the whole line).
template <class F>
void ForEachNumberedRef(const std::string& s, F&& f) {
    std::string n, rhs;
    size_t start = 0;
    if (ParseAssign(s, n, rhs)) start = s.find('=') + 1;
    for (size_t i = start; i < s.size(); ++i) {
        if (s[i] != '%') continue;
        size_t e = DigitsEnd(s, i + 1);
        if (e > i + 1) {
            f(s.substr(i + 1, e - i - 1));
            i = e - 1;
        }
    }
}

// the identifier the string starts with
std::string LeadWord(const std::string& s) {
    size_t e = 0;
    while (e < s.size() && IsWord(s[e])) ++e;
    return s.substr(0, e);
}

// ---- the translator ---------------------------------------------------------------------------------

enum class PT { Handle, Struct, Local, Pred, B32, Alias };

struct Phi {
    std::string name, type;
    std::vector<std::pair<std::string, std::string>> inc;  // value, predecessor block
};

class Translator {
public:
    Translator(const std::vector<std::string>& text, const Options& o) : text_(text), o_(o) {}
    std::string Run();

private:
    // options derived
    const std::vector<std::string>& text_;
    const Options& o_;
    std::string ftz_;
    std::string rnd_;
    std::optional<std::unordered_set<std::string>> fuse_;
    bool fusePolicy_ = false;
    bool distribute_ = false;
    int nt_[3] = {8, 8, 1};

    std::map<std::string, std::pair<std::string, int>> shared_;  // LLVM global -> (PTX name, element count)
    std::vector<std::string> sharedOrder_;
    std::unordered_map<std::string, Block> blocks_;
    std::vector<std::string> order_;
    std::unordered_map<std::string, PT> ptype_;
    std::map<std::pair<int, int>, std::pair<int, int>> meta_;
    std::map<std::pair<int, int>, bool> elemFloat_;
    std::map<int, int> cbsize_;
    std::unordered_map<std::string, std::pair<int, int>> handles_;
    std::unordered_map<std::string, std::string> alias_;
    std::unordered_map<std::string, int> uses_;
    std::map<std::pair<int, int>, std::string> formats_;
    std::unordered_map<std::string, std::pair<std::string, std::string>> fmuls_, csubs_;
    std::set<int> slotsUsed_;
    std::map<std::string, int> locals_;  // alloca value -> element count (kept in creation order below)
    std::vector<std::string> localsOrder_;
    std::unordered_map<std::string, std::string> space_;
    std::unordered_map<std::string, std::vector<Phi>> phis_;
    std::vector<std::string> L_;
    int tmpCount_ = 0, t64_ = 0;
    std::string ldop_ = "ld.global";

    void W(std::string s) { L_.push_back(std::move(s)); }
    std::string Tmp() { return "%t" + std::to_string(++tmpCount_); }
    std::string Tmp64() { return "%q" + std::to_string(++t64_); }

    void ParseModule();
    void IfConvert();
    void ParseMetadata();
    void DeadCode();
    void Types();
    std::string Val(std::string tok, const char* ty = "i32");
    std::string V(const std::string& tok, const char* ty = "i32");
    std::string Desc(const std::string& h, int off, const char* kind = "u32");
    void LdopFor(const std::string& h);
    void BoundedLoad(const std::vector<std::string>& dst, const std::string& base64, const std::string& offs32,
                     const std::string& limit32, bool aligned);
    void LoadFormatted(const std::string& n, const std::string& f, const std::string& a64, const std::string& p);
    void StoreFormatted(const std::string& f, const std::string& a64, const std::vector<std::string>& vals, int mask);
    void EmitCall(const std::string& n, bool hasN, const std::string& rhs);
    void EmitBody();
    std::vector<std::string> EdgeCopies(const std::string& src, const std::string& dst);
    std::string SelPred(const std::string& x);
    std::pair<std::string, std::string> Pointer(const std::string& tok);
    std::string FmtOf(const std::string& h) {
        auto it = formats_.find(handles_.at(Strip(h).substr(1)));
        return it == formats_.end() ? std::string() : it->second;
    }
    std::pair<int, int> Handle(const std::string& h) {
        auto it = handles_.find(Strip(h).substr(1));
        if (it == handles_.end()) die("unknown handle " + h);
        return it->second;
    }
    std::pair<int, int> MetaOf(std::pair<int, int> key, std::pair<int, int> dflt) {
        auto it = meta_.find(key);
        return it == meta_.end() ? dflt : it->second;
    }
    std::vector<std::string> Declarations();
    std::string Finish();
    std::vector<std::pair<std::string, std::string>> Args(const std::string& argstr);
};

void Translator::ParseModule() {
    // numthreads: the first NumThreads=(x,y,z) anywhere in the text
    for (const std::string& line : text_) {
        size_t at = line.find("NumThreads=(");
        while (at != std::string::npos) {
            size_t p = at + 12;
            int vals[3];
            bool ok = true;
            for (int k = 0; k < 3 && ok; ++k) {
                size_t e = DigitsEnd(line, p);
                if (e == p) {
                    ok = false;
                    break;
                }
                vals[k] = std::atoi(line.substr(p, e - p).c_str());
                p = e;
                if (k < 2) {
                    if (p < line.size() && line[p] == ',')
                        ++p;
                    else
                        ok = false;
                } else if (!(p < line.size() && line[p] == ')')) {
                    ok = false;
                }
            }
            if (ok) {
                nt_[0] = vals[0];
                nt_[1] = vals[1];
                nt_[2] = vals[2];
                goto numthreads_done;
            }
            at = line.find("NumThreads=(", at + 1);
        }
    }
numthreads_done:

    // groupshared arrays
    static const std::regex reShared(R"((@\S+) = (?:external )?addrspace\(3\) global \[(\d+) x (?:float|i32)\].*)");
    for (const std::string& line : text_) {
        if (line.empty() || line[0] != '@') continue;
        std::smatch m;
        if (std::regex_match(line, m, reShared)) {
            std::string name = "sm" + std::to_string(shared_.size());
            shared_[m[1].str()] = {name, std::atoi(m[2].str().c_str())};
            sharedOrder_.push_back(m[1].str());
        }
    }

    // the function body into blocks
    bool inside = false;
    std::string cur;
    for (const std::string& line : text_) {
        if (Starts(line, "define void @")) {
            size_t p = 13, e = p;
            while (e < line.size() && IsWord(line[e])) ++e;
            if (e > p && line.compare(e, 2, "()") == 0) {
                inside = true;
                cur = "0";
                blocks_[cur] = {};
                order_ = {cur};
                continue;
            }
        }
        if (!inside) continue;
        if (!line.empty() && line[0] == '}') break;
        if (Starts(line, "; <label>:")) {
            size_t e = DigitsEnd(line, 10);
            if (e > 10) {
                cur = line.substr(10, e - 10);
                blocks_[cur] = {};
                order_.push_back(cur);
                continue;
            }
        }
        std::string st = Strip(line);
        std::string s;
        if (!Starts(st, ";")) {
            size_t semi = line.find(';');
            s = Strip(semi == std::string::npos ? line : line.substr(0, semi));
        }
        s = DropAliasMetadata(s);
        if (!s.empty()) blocks_[cur].push_back(s);
    }
}

// A small arm without side effects that reads memory ("if (inside) sample; else 0") is executed unconditionally and
// its merge phis become selects. ptxas schedules within a basic block only, so an arm left as a branch keeps every
// texture fetch / load waiting on its own branch; once converted the loads can be issued early and overlap. Exact: a
// select returns what the phi did, and every read is safe to speculate (bounded loads return 0 out of range,
// samplers clamp). Arms with exp/log are left alone - they are the expensive optional paths (the PQ encoding) a
// uniform branch should keep skipping.
void Translator::IfConvert() {
    constexpr size_t kIfConvMax = 48;
    auto succ = [&](const std::string& b) {
        const Block& bl = blocks_[b];
        std::string t = bl.empty() ? std::string() : bl.back();
        std::string c, x, y;
        std::vector<std::string> r;
        if (ParseCondBr(t, c, x, y)) {
            r = {x, y};
        } else if (ParseBr(t, x)) {
            r = {x};
        }
        return r;
    };
    auto convertible = [&](const std::string& b) {
        const Block& bl = blocks_[b];
        if (b == "0" || bl.size() - 1 > kIfConvMax) return false;
        std::string tmp;
        if (!ParseBr(bl.back(), tmp)) return false;
        bool reads = false;
        for (size_t i = 0; i + 1 < bl.size(); ++i) {
            const std::string& s = bl[i];
            if (Starts(s, "store ") || Starts(s, "call void") || Starts(s, "phi") || Contains(s, " = phi ") ||
                Contains(s, "atomicrmw") || Contains(s, "@dx.op.atomicBinOp") || Contains(s, "@dx.op.barrier"))
                return false;
            if (Contains(s, "@dx.op.unary.f32(i32 21,") || Contains(s, "@dx.op.unary.f32(i32 23,")) return false;
            for (size_t at = s.find("@dx.op."); at != std::string::npos; at = s.find("@dx.op.", at + 1)) {
                std::string w = LeadWord(s.substr(at + 7));
                if (w == "sampleLevel" || w == "textureLoad" || w == "rawBufferLoad" || w == "cbufferLoadLegacy")
                    reads = true;
            }
        }
        return reads;
    };

    for (bool changed = true; changed;) {
        changed = false;
        std::unordered_map<std::string, std::vector<std::string>> preds;
        for (const auto& b : order_) preds[b];
        for (const auto& b : order_)
            for (const auto& s : succ(b)) preds[s].push_back(b);
        for (size_t ai = 0; ai < order_.size(); ++ai) {
            const std::string a = order_[ai];
            std::string t = blocks_[a].empty() ? std::string() : blocks_[a].back();
            std::string c, x, y;
            if (!ParseCondBr(t, c, x, y)) continue;
            auto isArm = [&](const std::string& z) { return preds[z].size() == 1 && convertible(z); };
            bool xa = isArm(x), ya = isArm(y);
            auto sorted2 = [](std::vector<std::string> v) {
                std::sort(v.begin(), v.end());
                return v;
            };
            std::string merge;
            std::map<std::string, std::string> armOf;
            if (xa && ya && succ(x) == succ(y) && sorted2(preds[succ(x)[0]]) == sorted2({x, y})) {
                merge = succ(x)[0];  // diamond
                armOf = {{x, x}, {y, y}};
            } else if (xa && succ(x) == std::vector<std::string>{y} && sorted2(preds[y]) == sorted2({a, x})) {
                merge = y;  // triangle, arm on true
                armOf = {{x, x}, {y, a}};
            } else if (ya && succ(y) == std::vector<std::string>{x} && sorted2(preds[x]) == sorted2({a, y})) {
                merge = x;  // triangle, arm on false
                armOf = {{x, a}, {y, y}};
            }
            if (merge.empty()) continue;
            // the arm bodies move into a, then the merge's phis become selects on c
            Block body(blocks_[a].begin(), blocks_[a].end() - 1);
            for (const std::string& z : {x, y})
                if (armOf[z] == z) body.insert(body.end(), blocks_[z].begin(), blocks_[z].end() - 1);
            Block newM;
            static const std::regex rePhiVal(R"(\[ ([^,]+), %(\d+) \])");
            for (const std::string& s : blocks_[merge]) {
                std::string n, rhs;
                if (!ParseAssign(s, n, rhs) || !Starts(rhs, "phi ")) {
                    newM.push_back(s);
                    continue;
                }
                std::string rest = rhs.substr(4);
                size_t sp = rest.find(' ');
                if (sp == std::string::npos || sp == 0) {
                    newM.push_back(s);
                    continue;
                }
                std::string ty = rest.substr(0, sp), tail = rest.substr(sp + 1);
                std::map<std::string, std::string> inc;
                for (auto it = std::sregex_iterator(tail.begin(), tail.end(), rePhiVal); it != std::sregex_iterator(); ++it)
                    inc[(*it)[2].str()] = Strip((*it)[1].str());
                if (!inc.count(armOf[x]) || !inc.count(armOf[y])) die("phi without the arm's value");
                newM.push_back(std::format("%{} = select i1 {}, {} {}, {} {}", n, c, ty, inc[armOf[x]], ty, inc[armOf[y]]));
            }
            // the merge is now reached from a alone: fold it in
            body.insert(body.end(), newM.begin(), newM.end());
            blocks_[a] = body;
            for (const std::string& z : {x, y})
                if (armOf[z] == z) {
                    order_.erase(std::find(order_.begin(), order_.end(), z));
                    blocks_.erase(z);
                }
            order_.erase(std::find(order_.begin(), order_.end(), merge));
            blocks_.erase(merge);
            // phis elsewhere naming the merge as a predecessor now come from a
            const std::regex reFix("(\\[ [^,]+, )%" + merge + " \\]");
            for (const auto& b2 : order_)
                for (std::string& s : blocks_[b2])
                    if (Contains(s, " phi ")) s = std::regex_replace(s, reFix, "$1%" + a + " ]");
            changed = true;
            break;
        }
    }
}

void Translator::ParseMetadata() {
    // Resource metadata: (class, register) -> (kind, structure stride). Class 0 SRV, 1 UAV; kind 11 is a
    // ByteAddressBuffer, 12 a StructuredBuffer (rawBufferLoad then takes an element index and a byte offset within
    // the element). Constant buffer register -> size in bytes (root constants included), from the same metadata.
    static const std::regex reCb(
        R"re(!\d+ = !\{i32 \d+, %"?([^*]+?)"?\* undef, !"[^"]*", i32 \d+, i32 (\d+), i32 \d+, i32 (\d+), null\})re");
    static const std::regex reRes(
        R"re(!\d+ = !\{i32 \d+, %"?([^*]+?)"?\* undef, !"[^"]*", i32 \d+, i32 (\d+), i32 \d+, i32 (\d+)(.*)\})re");
    static const std::regex reRef(R"(!(\d+)\s*)");
    std::vector<const std::string*> md;
    for (const std::string& line : text_)
        if (!line.empty() && line[0] == '!' && Contains(line, "undef")) md.push_back(&line);
    for (const std::string* lp : md) {
        std::smatch m;
        if (std::regex_match(*lp, m, reCb)) {
            std::string ty = m[1].str();
            if (!Contains(ty, "Sampler") && !Contains(ty, "Buffer") && !Contains(ty, "Texture"))
                cbsize_[std::atoi(m[2].str().c_str())] = std::atoi(m[3].str().c_str());
        }
    }
    for (const std::string* lp : md) {
        std::smatch m;
        if (!std::regex_match(*lp, m, reRes)) continue;
        std::string ty = m[1].str(), rest = m[4].str();
        int lb = std::atoi(m[2].str().c_str()), kind = std::atoi(m[3].str().c_str());
        int cls = Contains(ty, "RW") ? 1 : 0;
        if (!Contains(ty, "Buffer") && !Contains(ty, "Texture")) continue;  // constant buffers and samplers
        int stride = 0;
        // ref: "!N" at the very end of `rest`
        size_t bang = rest.rfind('!');
        if (kind == 12 && bang != std::string::npos) {
            size_t e = DigitsEnd(rest, bang + 1);
            std::string tail = rest.substr(e);
            bool endOk = e > bang + 1 && std::all_of(tail.begin(), tail.end(), [](char c) { return IsSpace(c); });
            if (endOk) {
                std::string key = "!" + rest.substr(bang + 1, e - bang - 1) + " = !{i32 1, i32 ";
                for (const std::string& l2 : text_) {
                    if (Starts(l2, key.c_str()) && EndsWith(l2, "}")) {
                        std::string num = l2.substr(key.size(), l2.size() - key.size() - 1);
                        if (!num.empty() && DigitsEnd(num, 0) == num.size()) {
                            stride = std::atoi(num.c_str());
                            break;
                        }
                    }
                }
            }
        }
        meta_[{cls, lb}] = {kind, stride};
        elemFloat_[{cls, lb}] = Contains(ty, "float");
    }
}

// Dead code goes first, as the driver drops it before contracting: a value used only by dead code is single-use to
// the driver (the HDR10 reconstruction stores a constant alpha but keeps its computation in the DXIL). Only
// side-effect-free instructions are removed.
void Translator::DeadCode() {
    static const std::unordered_set<std::string> pureOp = {
        "fadd", "fsub", "fmul", "fdiv", "frem", "fcmp", "icmp", "select", "add", "sub", "mul", "and", "or", "xor", "shl",
        "lshr", "ashr", "udiv", "urem", "sdiv", "srem", "bitcast", "sitofp", "uitofp", "fptosi", "fptoui", "fpext",
        "fptrunc", "zext", "sext", "trunc", "extractvalue", "phi"};
    static const std::unordered_set<std::string> pureCall = {
        "unary", "binary", "tertiary", "quaternary", "dot2", "dot3", "dot4", "isSpecialFloat", "legacyF16ToF32",
        "legacyF32ToF16", "cbufferLoadLegacy", "bufferLoad", "rawBufferLoad", "textureLoad", "createHandle",
        "threadId", "groupId", "threadIdInGroup", "flattenedThreadIdInGroup"};
    auto isPureCall = [&](const std::string& s) {
        for (size_t at = s.find("@dx.op."); at != std::string::npos; at = s.find("@dx.op.", at + 1)) {
            std::string w = LeadWord(s.substr(at + 7));
            if (at + 7 + w.size() >= s.size() || s[at + 7 + w.size()] != '.') continue;
            if (pureCall.count(w) || Starts(w, "sample") || Starts(w, "textureGather")) return true;
        }
        return false;
    };
    while (o_.dce) {
        std::unordered_map<std::string, int> live;
        for (const auto& b : order_)
            for (const std::string& s : blocks_[b]) ForEachNumberedRef(s, [&](const std::string& u) { ++live[u]; });
        int removed = 0;
        for (const auto& b : order_) {
            Block keep;
            for (const std::string& s : blocks_[b]) {
                std::string n, rhs;
                if (ParseAssign(s, n, rhs) && !live.count(n) &&
                    (pureOp.count(LeadWord(rhs)) || (Starts(rhs, "call ") && isPureCall(rhs)))) {
                    ++removed;
                    continue;
                }
                keep.push_back(s);
            }
            blocks_[b] = std::move(keep);
        }
        if (!removed) break;
    }
    for (const auto& b : order_)
        for (const std::string& s : blocks_[b]) ForEachNumberedRef(s, [&](const std::string& u) { ++uses_[u]; });
}

std::vector<std::pair<std::string, std::string>> Translator::Args(const std::string& argstr) {
    std::vector<std::pair<std::string, std::string>> a;
    for (const std::string& part : SplitChar(argstr, ',')) {
        std::string t = Strip(part);
        size_t sp = t.rfind(' ');
        if (sp == std::string::npos) die("cannot split argument '" + t + "'");
        a.push_back({t.substr(0, sp), t.substr(sp + 1)});
    }
    return a;
}

void Translator::Types() {
    for (const auto& b : order_) {
        for (const std::string& s : blocks_[b]) {
            std::string n, rhs;
            if (!ParseAssign(s, n, rhs)) continue;
            if (Contains(rhs, "@dx.op.createHandle(")) {
                size_t lp = rhs.find('('), rp = rhs.rfind(')');
                auto a = Args(rhs.substr(lp + 1, rp - lp - 1));
                if (a.size() < 4) die("createHandle arguments");
                int cls = (int)ToInt(a[1].second), idx = (int)ToInt(a[3].second);
                handles_[n] = {cls, idx};
                ptype_[n] = PT::Handle;
            } else if (Contains(rhs, "@dx.op.createHandleFromBinding")) {
                // Shader model 6.6: ResBind {lower, upper, space, class}; the index is the register itself.
                static const std::regex reBind(R"(%dx\.types\.ResBind (\{[^}]*\}|zeroinitializer), i32 (\d+))");
                std::smatch m;
                if (!std::regex_search(rhs, m, reBind)) die("createHandleFromBinding");
                int cls = 0;
                if (m[1].str() != "zeroinitializer") {
                    static const std::regex reI8(R"(i8 (\d+))");
                    std::smatch m8;
                    std::string g = m[1].str();
                    if (!std::regex_search(g, m8, reI8)) die("ResBind class");
                    cls = std::atoi(m8[1].str().c_str());
                }
                handles_[n] = {cls, std::atoi(m[2].str().c_str())};
                ptype_[n] = PT::Handle;
            } else if (Contains(rhs, "@dx.op.annotateHandle")) {
                // The same binding; its properties carry the kind (low byte) and a structure's stride.
                static const std::regex reAnn(
                    R"(%dx\.types\.Handle %(\d+), %dx\.types\.ResourceProperties (\{[^}]*\}|zeroinitializer))");
                std::smatch m;
                if (!std::regex_search(rhs, m, reAnn)) die("annotateHandle");
                handles_[n] = handles_.at(m[1].str());
                ptype_[n] = PT::Handle;
                std::vector<long long> props;
                std::string g = m[2].str();
                static const std::regex reI32(R"(i32 (-?\d+))");
                for (auto it = std::sregex_iterator(g.begin(), g.end(), reI32); it != std::sregex_iterator(); ++it)
                    props.push_back(std::atoll((*it)[1].str().c_str()));
                if (props.empty()) props = {0, 0};
                int kind = (int)(props[0] & 0xFF);
                if (kind == 11 || kind == 12) {
                    if (props.size() < 2) die("resource properties");
                    meta_.emplace(handles_[n], std::make_pair(kind, kind == 12 ? (int)props[1] : 0));
                }
            } else if (Starts(rhs, "call %dx.types.ResRet") || Starts(rhs, "call %dx.types.CBufRet") ||
                       Starts(rhs, "call %dx.types.fouri32")) {
                ptype_[n] = PT::Struct;
            } else if (Starts(rhs, "alloca ")) {
                ptype_[n] = PT::Local;
            } else if (Starts(rhs, "call i1 ") || Starts(rhs, "icmp") || Starts(rhs, "fcmp") || Starts(rhs, "and i1") ||
                       Starts(rhs, "or i1") || Starts(rhs, "xor i1") || Starts(rhs, "phi i1") ||
                       [&] {
                           if (!Starts(rhs, "select i1 ")) return false;
                           size_t e = rhs.find(' ', 10);
                           return e != std::string::npos && e >= 12 && rhs[e - 1] == ',' && rhs.compare(e, 5, " i1 ") == 0;
                       }()) {
                ptype_[n] = PT::Pred;
            } else if (Starts(rhs, "extractvalue")) {
                ptype_[n] = PT::Alias;
            } else {
                ptype_[n] = PT::B32;
            }
        }
    }
}

std::string Translator::Val(std::string tok, const char* ty) {
    tok = Strip(tok);
    if (!tok.empty() && tok[0] == '%') {
        std::string n = tok.substr(1);
        auto it = ptype_.find(n);
        return (it != ptype_.end() && it->second == PT::Pred ? "%p" : "%r") + n;
    }
    bool isFloat = std::strcmp(ty, "float") == 0;
    if (tok == "undef") return isFloat ? "0f00000000" : "0";
    if (tok == "true") return "1";
    if (tok == "false") return "0";
    if (isFloat) return HexFloat(tok);
    return std::to_string(ToInt(tok));
}
std::string Translator::V(const std::string& tok0, const char* ty) {
    std::string tok = Strip(tok0);
    if (!tok.empty() && tok[0] == '%') {
        auto it = alias_.find(tok.substr(1));
        if (it != alias_.end()) return it->second;
    }
    return Val(tok, ty);
}

// One field of a resource's descriptor. Each slot the kernel uses is a 48-byte kernel parameter (p_s<slot>, same
// layout as a descriptor-table slot), so the read comes from the constant bank rather than global memory - where
// D3D's descriptors effectively live too.
std::string Translator::Desc(const std::string& h, int off, const char* kind) {
    auto [cls, reg] = Handle(h);
    int slot = cls * 32 + reg;
    slotsUsed_.insert(slot);
    std::string r = std::strcmp(kind, "u32") == 0 ? Tmp() : Tmp64();
    W(std::format("    ld.param.{} {}, [p_s{}+{}];", kind, r, slot, off));
    return r;
}

// The load instruction for the resource being read: SRVs are read-only for the whole dispatch, so their loads take
// the non-coherent read-only path (ld.global.nc), as D3D's SRV reads do; UAV reads stay coherent.
void Translator::LdopFor(const std::string& h) { ldop_ = Handle(h).first == 0 ? "ld.global.nc" : "ld.global"; }

// dst_k = in range ? [base + offs + 4k] : 0 - one range check on the last element.
void Translator::BoundedLoad(const std::vector<std::string>& dst, const std::string& base64, const std::string& offs32,
                             const std::string& limit32, bool aligned) {
    const std::string p = "%pt";
    std::string last = Tmp();
    W(std::format("    add.s32 {}, {}, {};", last, offs32, 4 * dst.size()));
    W(std::format("    setp.le.u32 {}, {}, {};", p, last, limit32));
    std::string a = Tmp64();
    W(std::format("    cvt.u64.u32 {}, {};", a, offs32));
    W(std::format("    add.s64 {}, {}, {};", a, base64, a));
    for (const auto& d : dst) W(std::format("    mov.b32 {}, 0;", d));
    size_t n = dst.size();
    std::string regs = Join(dst, ", ");
    if ((n == 2 || n == 4) && aligned) {
        W(std::format("    @{} {}.v{}.u32 {{{}}}, [{}];", p, ldop_, n, regs, a));
        return;
    }
    if (n == 2 || n == 4) {
        // Aligned (always, for 16-byte structures): one vector load; otherwise word by word. Both predicated, no
        // branch: a branch would end the basic block, and ptxas schedules (hoists loads, overlaps their latency)
        // only within one.
        std::string al = Tmp();
        W(std::format("    and.b32 {}, {}, {};", al, offs32, 4 * n - 1));
        W("    { .reg .pred %va, %vu;");
        W(std::format("    setp.eq.and.u32 %va, {}, 0, {};", al, p));
        W(std::format("    setp.ne.and.u32 %vu, {}, 0, {};", al, p));
        W(std::format("    @%va {}.v{}.u32 {{{}}}, [{}];", ldop_, n, regs, a));
        for (size_t k = 0; k < n; ++k) W(std::format("    @%vu {}.u32 {}, [{}+{}];", ldop_, dst[k], a, 4 * k));
        W("    }");
        return;
    }
    for (size_t k = 0; k < n; ++k) W(std::format("    @{} {}.u32 {}, [{}+{}];", p, ldop_, dst[k], a, 4 * k));
}

// Component registers %r{n}_0..3 from one texel of a non-32-bit format; out of range gives 0 (alpha 1 for a format
// without one, as D3D's defaults for missing components).
void Translator::LoadFormatted(const std::string& n, const std::string& f, const std::string& a64, const std::string& p) {
    const std::string one = "0f3F800000";
    std::string raw = Tmp();
    W(std::format("    mov.b32 {}, 0;", raw));
    if (f == "rgba8" || f == "rg16f" || f == "rgb10a2")
        W(std::format("    @{} {}.u32 {}, [{}];", p, ldop_, raw, a64));
    else if (f == "r8")
        W(std::format("    @{} {}.u8 {}, [{}];", p, ldop_, raw, a64));
    else if (f == "rg8i")
        W(std::format("    @{} {}.u16 {}, [{}];", p, ldop_, raw, a64));
    else
        die("format " + f + " not handled");
    for (int k = 0; k < 4; ++k) {
        std::string d = std::format("%r{}_{}", n, k);
        if ((f == "rgba8" || f == "r8") && (f == "rgba8" || k == 0)) {
            std::string t1 = Tmp();
            W(std::format("    bfe.u32 {}, {}, {}, 8;", t1, raw, 8 * k));
            W(std::format("    cvt.rn.f32.u32 {}, {};", d, t1));
            // c / 255 correctly rounded, without div.rn's slow path: q = c * (1/255), then one Newton step on the
            // exact residual. Equal to div.rn for all 256 byte values.
            std::string t2 = Tmp(), t3 = Tmp();
            W(std::format("    mul.rn.f32 {}, {}, 0f3B808081;", t2, d));
            W(std::format("    neg.f32 {}, {};", t3, t2));
            W(std::format("    fma.rn.f32 {}, {}, 0f437F0000, {};", t3, t3, d));
            W(std::format("    fma.rn.f32 {}, {}, 0f3B808081, {};", d, t3, t2));
        } else if (f == "rgb10a2") {
            // c / 1023 (c / 3 for alpha), correctly rounded as the texture unit converts
            std::string t1 = Tmp();
            W(std::format("    bfe.u32 {}, {}, {}, {};", t1, raw, 10 * k, k < 3 ? 10 : 2));
            W(std::format("    cvt.rn.f32.u32 {}, {};", d, t1));
            W(std::format("    div.rn.f32 {}, {}, {};", d, d, k < 3 ? "0f447FC000" : "0f40400000"));
        } else if (f == "rg16f" && k < 2) {
            if (k == 0)
                W(std::format("    {{ .reg .b16 %hh; cvt.u16.u32 %hh, {}; cvt.f32.f16 {}, %hh; }}", raw, d));
            else
                W(std::format("    {{ .reg .b16 %hh; .reg .b32 %hx; shr.u32 %hx, {}, 16; cvt.u16.u32 %hh, %hx; "
                              "cvt.f32.f16 {}, %hh; }}",
                              raw, d));
        } else if (f == "rg8i" && k < 2) {
            W(std::format("    bfe.s32 {}, {}, {}, 8;", d, raw, 8 * k));
        } else {
            W(std::format("    mov.b32 {}, {};", d, (k == 3 && f != "rg8i") ? one : std::string("0")));
            if (k == 3 && f == "rg8i") W(std::format("    mov.b32 {}, 1;", d));
        }
        if (f == "rgba8" || f == "r8" || f == "rg16f" || f == "rgb10a2")
            W(std::format("    @!{} mov.b32 {}, 0;", p, d));  // out of range reads zero, alpha included
    }
}

// One texel of a non-32-bit format from float (UNORM, half) or integer (SINT) components.
void Translator::StoreFormatted(const std::string& f, const std::string& a64, const std::vector<std::string>& vals, int mask) {
    const std::string ftz = ftz_;
    if (f == "rgba8" || f == "r8") {
        int n = f == "rgba8" ? 4 : 1;
        bool packed = f == "rgba8" && (mask & 15) == 15;
        std::string word = Tmp();
        if (packed) W(std::format("    mov.b32 {}, 0;", word));
        for (int k = 0; k < n; ++k) {
            if (!(mask & (1 << k))) continue;
            // The hardware's float -> UNORM8, found against Intel's shader on 87k stores: saturate, truncate to 12
            // fraction bits, then (v * 255 + 2047) >> 12. round(x * 255) misses about 3% of bytes (fractions just
            // above .5 round down).
            std::string t1 = Tmp(), t2 = Tmp();
            W(std::format("    cvt{}.sat.f32.f32 {}, {};", ftz, t1, vals[k]));
            W(std::format("    mul.rn{}.f32 {}, {}, 0f45800000;", ftz, t1, t1));
            W(std::format("    cvt.rmi.u32.f32 {}, {};", t2, t1));
            W(std::format("    mad.lo.u32 {}, {}, 255, 2047;", t2, t2));
            W(std::format("    shr.u32 {}, {}, 12;", t2, t2));
            if (packed)
                W(std::format("    bfi.b32 {}, {}, {}, {}, 8;", word, t2, word, 8 * k));
            else
                W(std::format("    @%pt st.global.u8 [{}+{}], {};", a64, k, t2));
        }
        if (packed) W(std::format("    @%pt st.global.b32 [{}], {};", a64, word));  // one 32-bit store per texel
    } else if (f == "rgb10a2") {
        // float -> UNORM10 / UNORM2 the way the 8-bit store goes (saturate, truncate to rgb10Frac fraction bits,
        // then (v * max + half) >> bits); the texel is read back first when not all four are written.
        std::string word = Tmp();
        W(std::format("    mov.b32 {}, 0;", word));
        if ((mask & 15) != 15) W(std::format("    @%pt ld.global.u32 {}, [{}];", word, a64));
        for (int k = 0; k < 4; ++k) {
            if (!(mask & (1 << k))) continue;
            int bits = k < 3 ? 10 : 2, mx = k < 3 ? 1023 : 3;
            int fb = o_.rgb10Frac;
            std::string t1 = Tmp(), t2 = Tmp();
            W(std::format("    cvt{}.sat.f32.f32 {}, {};", ftz, t1, vals[k]));
            W(std::format("    mul.rn{}.f32 {}, {}, 0f{:08X};", ftz, t1, t1, (127u + fb) << 23));
            W(std::format("    cvt.rmi.u32.f32 {}, {};", t2, t1));
            W(std::format("    mad.lo.u32 {}, {}, {}, {};", t2, t2, mx, (1 << (fb - 1)) - 1));
            W(std::format("    shr.u32 {}, {}, {};", t2, t2, fb));
            W(std::format("    bfi.b32 {}, {}, {}, {}, {};", word, t2, word, 10 * k, bits));
        }
        W(std::format("    @%pt st.global.b32 [{}], {};", a64, word));
    } else if (f == "rg8i") {
        std::string word = Tmp();
        W(std::format("    mov.b32 {}, 0;", word));
        for (int k = 0; k < 2; ++k) {
            if (!(mask & (1 << k))) continue;
            std::string t1 = Tmp();
            W(std::format("    max.s32 {}, {}, -128;", t1, vals[k]));
            W(std::format("    min.s32 {}, {}, 127;", t1, t1));
            if ((mask & 3) == 3)
                W(std::format("    bfi.b32 {}, {}, {}, {}, 8;", word, t1, word, 8 * k));
            else
                W(std::format("    @%pt st.global.u8 [{}+{}], {};", a64, k, t1));
        }
        if ((mask & 3) == 3) W(std::format("    @%pt st.global.b16 [{}], {};", a64, word));
    } else if (f == "rg16f") {
        for (int k = 0; k < 2; ++k) {
            if (!(mask & (1 << k))) continue;
            W(std::format("    {{ .reg .b16 %hh; cvt.rn.f16.f32 %hh, {}; @%pt st.global.b16 [{}+{}], %hh; }}", vals[k], a64,
                          2 * k));
        }
    } else {
        die("format " + f + " not handled");
    }
}

const char* Unary(int opc) {
    switch (opc) {
        case 6: return "abs{ftz}.f32";
        case 7: return "cvt{ftz}.sat.f32.f32";
        case 21: return "ex2.approx{ftz}.f32";
        case 23: return "lg2.approx{ftz}.f32";
        case 24: return "sqrt.approx{ftz}.f32";
        case 25: return "rsqrt.approx{ftz}.f32";
        case 26: return "cvt.rni{ftz}.f32.f32";
        case 27: return "cvt.rmi{ftz}.f32.f32";
        case 28: return "cvt.rpi{ftz}.f32.f32";
        case 29: return "cvt.rzi{ftz}.f32.f32";
        case 12: return "cos.approx{ftz}.f32";
        case 13: return "sin.approx{ftz}.f32";
        default: return nullptr;
    }
}
const char* Binary(int opc) {
    switch (opc) {
        case 35: return "max{ftz}.f32";
        case 36: return "min{ftz}.f32";
        case 37: return "max.s32";
        case 38: return "min.s32";
        case 39: return "max.u32";
        case 40: return "min.u32";
        default: return nullptr;
    }
}
std::string SubstFtz(const char* tpl, const std::string& ftz) {
    std::string s = tpl;
    size_t at = s.find("{ftz}");
    if (at != std::string::npos) s.replace(at, 5, ftz);
    return s;
}
const char* IcmpName(const std::string& cc) {
    static const std::map<std::string, const char*> m = {
        {"eq", "eq.s32"}, {"ne", "ne.s32"}, {"ult", "lo.u32"}, {"ule", "ls.u32"}, {"ugt", "hi.u32"},
        {"uge", "hs.u32"}, {"slt", "lt.s32"}, {"sle", "le.s32"}, {"sgt", "gt.s32"}, {"sge", "ge.s32"}};
    auto it = m.find(cc);
    if (it == m.end()) die("icmp " + cc);
    return it->second;
}
const char* FcmpName(const std::string& cc) {
    static const std::map<std::string, const char*> m = {
        {"oeq", "eq"}, {"one", "ne"}, {"olt", "lt"}, {"ole", "le"}, {"ogt", "gt"}, {"oge", "ge"}, {"ord", "num"},
        {"ueq", "equ"}, {"une", "neu"}, {"ult", "ltu"}, {"ule", "leu"}, {"ugt", "gtu"}, {"uge", "geu"}, {"uno", "nan"}};
    auto it = m.find(cc);
    if (it == m.end()) die("fcmp " + cc);
    return it->second;
}
const char* IntBin(const std::string& op) {
    static const std::map<std::string, const char*> m = {
        {"add", "add.s32"}, {"sub", "sub.s32"}, {"mul", "mul.lo.s32"}, {"shl", "shl.b32"}, {"lshr", "shr.u32"},
        {"ashr", "shr.s32"}, {"and", "and.b32"}, {"or", "or.b32"}, {"xor", "xor.b32"}, {"udiv", "div.u32"},
        {"urem", "rem.u32"}, {"sdiv", "div.s32"}, {"srem", "rem.s32"}};
    return m.at(op);
}

void Translator::EmitCall(const std::string& n, bool hasN, const std::string& rhs) {
    // call <ret type> @dx.op.<op>[.<suffix>](<args>)
    size_t at = rhs.find(" @dx.op.");
    if (!Starts(rhs, "call ") || at == std::string::npos || rhs.empty() || rhs.back() != ')') die("cannot parse call: " + rhs);
    size_t pn = at + 8, e = pn;
    while (e < rhs.size() && IsWord(rhs[e])) ++e;
    std::string op = rhs.substr(pn, e - pn), suffix;
    bool hasSuffix = false;
    if (e < rhs.size() && rhs[e] == '.') {
        size_t s2 = e + 1, e2 = s2;
        while (e2 < rhs.size() && IsWord(rhs[e2])) ++e2;
        if (e2 > s2 && e2 < rhs.size() && rhs[e2] == '(') {
            suffix = rhs.substr(s2, e2 - s2);
            hasSuffix = true;
            e = e2;
        }
    }
    if (op.empty() || e >= rhs.size() || rhs[e] != '(') die("cannot parse call: " + rhs);
    std::string argstr = rhs.substr(e + 1, rhs.size() - e - 2);
    auto a = Args(argstr);
    auto A = [&](size_t i) -> const std::string& {
        if (i >= a.size()) die("call " + op + ": missing argument " + std::to_string(i));
        return a[i].second;
    };
    long long opc = ToInt(A(0));
    std::string d = hasN ? "%r" + n : std::string();
    const std::string ftz = ftz_;
    const bool isF32 = hasSuffix && suffix == "f32";
    (void)isF32;

    if (op == "createHandle" || op == "createHandleFromBinding" || op == "annotateHandle") return;
    if (op == "isSpecialFloat") {
        std::string x = V(A(1), "float");
        const char* test = opc == 8 ? "notanumber" : opc == 9 ? "infinite" : opc == 10 ? "finite" : opc == 11 ? "normal" : nullptr;
        if (!test) die("isSpecialFloat opcode " + std::to_string(opc) + " not handled");
        W(std::format("    testp.{}.f32 %p{}, {};", test, n, x));
        return;
    }
    if (op == "pack4x8") {
        int mode = (int)ToInt(A(1));
        std::string acc = d;
        W(std::format("    mov.b32 {}, 0;", acc));
        for (int k = 0; k < 4; ++k) {
            std::string t1 = Tmp();
            W(std::format("    mov.b32 {}, {};", t1, V(A(2 + k))));
            if (mode == 1) {  // u8clamp
                W(std::format("    max.s32 {}, {}, 0;", t1, t1));
                W(std::format("    min.s32 {}, {}, 255;", t1, t1));
            } else if (mode == 2) {  // s8clamp
                W(std::format("    max.s32 {}, {}, -128;", t1, t1));
                W(std::format("    min.s32 {}, {}, 127;", t1, t1));
            }
            W(std::format("    bfi.b32 {}, {}, {}, {}, 8;", acc, t1, acc, 8 * k));
        }
        return;
    }
    if (op == "unpack4x8") {
        bool sgn = ToInt(A(1)) == 1;
        std::string x = V(A(2));
        for (int k = 0; k < 4; ++k) W(std::format("    bfe.{}32 %r{}_{}, {}, {}, 8;", sgn ? "s" : "u", n, k, x, 8 * k));
        return;
    }
    if (op == "rawBufferStore") {
        std::string h = A(1);
        std::string base = Desc(h, 0, "u64");
        std::string size = Desc(h, 32);
        auto [kind, stride] = MetaOf(Handle(h), {11, 0});
        std::string addr = Tmp();
        W(std::format("    mov.b32 {}, {};", addr, V(A(2))));
        if (kind == 12) {
            W(std::format("    mul.lo.s32 {}, {}, {};", addr, addr, stride));
            if (A(3) != "undef") W(std::format("    add.s32 {}, {}, {};", addr, addr, V(A(3))));
        }
        int mask = (int)ToInt(A(8));
        std::string a64 = Tmp64();
        W(std::format("    cvt.u64.u32 {}, {};", a64, addr));
        W(std::format("    add.s64 {}, {}, {};", a64, base, a64));
        for (int k = 0; k < 4; ++k) {
            if (!(mask & (1 << k))) continue;
            std::string lim = Tmp(), src = Tmp();
            W(std::format("    add.s32 {}, {}, {};", lim, addr, 4 * k + 4));
            W(std::format("    setp.le.u32 %pu, {}, {};", lim, size));
            W(std::format("    mov.b32 {}, {};", src, V(A(4 + k), isF32 ? "float" : "i32")));
            W(std::format("    @%pu st.global.b32 [{}+{}], {};", a64, 4 * k, src));
        }
        return;
    }
    if (op == "atomicBinOp") {
        std::string h = A(1);
        static const char* aops[] = {"add.u32", "and.b32", "or.b32", "xor.b32", "min.s32", "max.s32", "min.u32", "max.u32", "exch.b32"};
        long long ai = ToInt(A(2));
        if (ai < 0 || ai > 8) die("atomicBinOp operation");
        std::string aop = aops[ai];
        std::string base = Desc(h, 0, "u64");
        std::string size = Desc(h, 32);
        if (MetaOf(Handle(h), {11, 0}).first != 11) die("atomicBinOp on a non-raw resource not handled");
        std::string addr = Tmp(), lim = Tmp(), src = Tmp();
        W(std::format("    mov.b32 {}, {};", addr, V(A(3))));
        W(std::format("    add.s32 {}, {}, 4;", lim, addr));
        W(std::format("    setp.le.u32 %pu, {}, {};", lim, size));
        W(std::format("    mov.b32 {}, {};", src, V(A(6))));
        std::string a64 = Tmp64();
        W(std::format("    cvt.u64.u32 {}, {};", a64, addr));
        W(std::format("    add.s64 {}, {}, {};", a64, base, a64));
        std::string dd = d.empty() ? Tmp() : d;
        W(std::format("    mov.b32 {}, 0;", dd));
        W(std::format("    @%pu atom.global.{} {}, [{}], {};", aop, dd, a64, src));
        return;
    }
    if (op == "barrier") {
        W("    bar.sync 0;");
        return;
    }
    if (op == "threadId" || op == "groupId" || op == "threadIdInGroup") {
        char c = "xyz"[ToInt(A(1))];
        if (op == "threadId") {
            std::string t1 = Tmp(), t2 = Tmp();
            W(std::format("    mov.u32 {}, %ctaid.{};", t1, c));
            W(std::format("    mov.u32 {}, %ntid.{};", t2, c));
            W(std::format("    mov.u32 {}, %tid.{};", d, c));
            W(std::format("    mad.lo.s32 {}, {}, {}, {};", d, t1, t2, d));
        } else {
            W(std::format("    mov.u32 {}, %{}.{};", d, op == "groupId" ? "ctaid" : "tid", c));
        }
        return;
    }
    if (op == "flattenedThreadIdInGroup") {
        W(std::format("    mov.u32 {}, %tid.x;", d));
        std::string t1 = Tmp();
        W(std::format("    mov.u32 {}, %tid.y;", t1));
        W(std::format("    mad.lo.s32 {}, {}, {}, {};", d, t1, nt_[0], d));
        W(std::format("    mov.u32 {}, %tid.z;", t1));
        W(std::format("    mul.lo.s32 {}, {}, {};", t1, t1, nt_[0] * nt_[1]));
        W(std::format("    add.s32 {}, {}, {};", d, d, t1));
        return;
    }
    if (op == "unary") {
        const char* f = Unary((int)opc);
        std::string x = V(A(1), "float");
        if (opc == 22) {  // Frc: x - floor(x)
            std::string t1 = Tmp();
            W(std::format("    cvt.rmi{}.f32.f32 {}, {};", ftz, t1, x));
            W(std::format("    sub.rn{}.f32 {}, {}, {};", ftz, d, x, t1));
            return;
        }
        if (!f) die("unary opcode " + std::to_string(opc) + " not handled");
        W(std::format("    {} {}, {};", SubstFtz(f, ftz), d, x));
        return;
    }
    if (op == "binary") {
        const char* f = Binary((int)opc);
        if (!f) die("binary opcode " + std::to_string(opc) + " not handled");
        const char* ty = isF32 ? "float" : "i32";
        std::string x = V(A(1), ty), y = V(A(2), ty);
        W(std::format("    {} {}, {}, {};", SubstFtz(f, ftz), d, x, y));
        return;
    }
    if (op == "dot4AddPacked") {
        std::vector<std::string> ops;
        for (int k : {2, 3, 1}) {
            std::string o = V(A(k));
            if (o.empty() || o[0] != '%') {
                std::string t1 = Tmp();
                W(std::format("    mov.b32 {}, {};", t1, o));
                o = t1;
            }
            ops.push_back(o);
        }
        W(std::format("    dp4a.s32.s32 {}, {}, {}, {};", d, ops[0], ops[1], ops[2]));
        return;
    }
    if (op == "legacyF16ToF32") {
        std::string h = Tmp(), t1 = Tmp();
        (void)h;
        W(std::format("    and.b32 {}, {}, 65535;", t1, V(A(1))));
        W(std::format("    {{ .reg .b16 %hh; cvt.u16.u32 %hh, {}; cvt.f32.f16 {}, %hh; }}", t1, d));
        return;
    }
    if (op == "legacyF32ToF16") {
        W(std::format("    {{ .reg .b16 %hh; cvt.rn.f16.f32 %hh, {}; cvt.u32.u16 {}, %hh; }}", V(A(1), "float"), d));
        return;
    }
    if (op == "cbufferLoadLegacy") {
        // Constant buffers are kernel parameters (p_cb<reg>), which the GPU serves from its constant bank, as D3D
        // does with cbuffers: a constant row index is a fixed offset, free when uniform.
        int cb = Handle(A(1)).second;
        int size = cbsize_.emplace(cb, 4096).first->second;
        int rows = (size + 15) / 16;
        std::vector<std::string> regs;
        for (int k = 0; k < 4; ++k) regs.push_back(std::format("%r{}_{}", n, k));
        std::string idx = V(A(2));
        if (idx.empty() || idx[0] != '%') {
            if (ToInt(idx) < rows) {
                W(std::format("    ld.param.v4.b32 {{{}}}, [p_cb{}+{}];", Join(regs, ", "), cb, 16 * ToInt(idx)));
            } else {
                for (const auto& r : regs) W(std::format("    mov.b32 {}, 0;", r));
            }
            return;
        }
        // A row index in a register: the parameter's address, range-checked (out of range reads 0).
        std::string off = Tmp(), a64 = Tmp64();
        W(std::format("    min.u32 {}, {}, {};", off, idx, rows - 1));
        W(std::format("    mul.lo.s32 {}, {}, 16;", off, off));
        W(std::format("    mov.u64 {}, p_cb{};", a64, cb));
        W(std::format("    {{ .reg .u64 %o; cvt.u64.u32 %o, {}; add.s64 {}, {}, %o; }}", off, a64, a64));
        W(std::format("    ld.param.v4.b32 {{{}}}, [{}];", Join(regs, ", "), a64));
        W(std::format("    setp.ge.u32 %pu, {}, {};", idx, rows));
        for (const auto& r : regs) W(std::format("    @%pu mov.b32 {}, 0;", r));
        return;
    }
    if (op == "rawBufferLoad") {
        LdopFor(A(1));
        std::string base = Desc(A(1), 0, "u64");
        std::string size = Desc(A(1), 32);
        int mask = (int)ToInt(A(4));
        int cnt = (int)std::bit_width((unsigned)mask);
        std::vector<std::string> regs;
        for (int k = 0; k < cnt; ++k) regs.push_back(std::format("%r{}_{}", n, k));
        auto [kind, stride] = MetaOf(Handle(A(1)), {11, 0});
        std::string addr = V(A(2));
        if (kind == 12) {
            std::string t1 = Tmp();
            W(std::format("    mul.lo.s32 {}, {}, {};", t1, addr, stride));
            if (A(3) != "undef") W(std::format("    add.s32 {}, {}, {};", t1, t1, V(A(3))));
            addr = t1;
        } else if (addr.empty() || addr[0] != '%') {
            std::string t1 = Tmp();
            W(std::format("    mov.b32 {}, {};", t1, addr));
            addr = t1;
        }
        bool aligned = kind == 12 && (cnt == 2 || cnt == 4) && stride % (4 * cnt) == 0 && (A(3) == "undef" || A(3) == "0");
        BoundedLoad(regs, base, addr, size, aligned);
        for (int k = cnt; k < 4; ++k) W(std::format("    mov.b32 %r{}_{}, 0;", n, k));
        return;
    }
    if (op == "textureLoad") {
        LdopFor(A(1));
        std::string h = A(1);
        auto [cls, reg] = Handle(h);
        std::string f = FmtOf(h);
        if (cls == 0 && MetaOf({0, reg}, {0, 0}).first == 2 && (A(2) == "0" || A(2) == "undef") &&
            (f == "rgba8" || f == "r8" || f == "rg16f" || f == "rgb10a2" ||
             (f == "x32" && elemFloat_.count({0, reg}) && elemFloat_[{0, reg}]))) {
            // Load() on a float-readable Texture2D: one texture-unit fetch at integer coordinates (descriptor
            // +40: point, unnormalised, border zero), which addresses, range-checks and converts the format in
            // hardware as D3D's Load does.
            std::string pt = Desc(h, 40, "u64");
            std::vector<std::string> xy;
            for (int ci : {3, 4}) {
                std::string c2 = V(A(ci));
                if (c2.empty() || c2[0] != '%') {
                    std::string t1 = Tmp();
                    W(std::format("    mov.b32 {}, {};", t1, c2));
                    c2 = t1;
                }
                xy.push_back(c2);
            }
            std::vector<std::string> regs;
            for (int k = 0; k < 4; ++k) regs.push_back(std::format("%r{}_{}", n, k));
            W(std::format("    tex.2d.v4.f32.s32 {{{}}}, [{}, {{{}, {}}}];", Join(regs, ", "), pt, xy[0], xy[1]));
            return;
        }
        std::string base = Desc(h, 0, "u64");
        std::string W_ = Desc(h, 16), H_ = Desc(h, 20), S_ = Desc(h, 24), tb = Desc(h, 28);
        std::string x = V(A(3)), y = V(A(4));
        std::string z = A(5) != "undef" ? V(A(5)) : std::string("0");
        std::string p = "%pt", idx = Tmp();
        W(std::format("    setp.lt.u32 {}, {}, {};", p, x, W_));
        W(std::format("    setp.lt.and.u32 {}, {}, {}, {};", p, y, H_, p));
        W(std::format("    setp.lt.and.u32 {}, {}, {}, {};", p, z, S_, p));
        // row-pitched: (slice * height + row) * pitch + x * texel bytes, the pitch at descriptor +36
        std::string rp = Desc(h, 36), t2 = Tmp();
        W(std::format("    mad.lo.s32 {}, {}, {}, {};", idx, z, H_, y));
        W(std::format("    mul.lo.s32 {}, {}, {};", t2, x, tb));
        W(std::format("    mad.lo.s32 {}, {}, {}, {};", idx, idx, rp, t2));
        std::string a64 = Tmp64();
        W(std::format("    cvt.u64.u32 {}, {};", a64, idx));
        W(std::format("    add.s64 {}, {}, {};", a64, base, a64));
        std::string fm = FmtOf(h);
        if (fm == "x128" || fm == "x64" || fm == "x32") {
            // 32-bit components, texel size known: one vector load, missing components 0
            int cnt = fm == "x128" ? 4 : fm == "x64" ? 2 : 1;
            for (int k = 0; k < 4; ++k) W(std::format("    mov.b32 %r{}_{}, 0;", n, k));
            std::vector<std::string> regs;
            for (int k = 0; k < cnt; ++k) regs.push_back(std::format("%r{}_{}", n, k));
            const char* vec = cnt == 4 ? ".v4" : cnt == 2 ? ".v2" : "";
            W(std::format("    @{} {}{}.b32 {}, [{}];", p, ldop_, vec, cnt > 1 ? "{" + Join(regs, ", ") + "}" : regs[0], a64));
            return;
        }
        if (!fm.empty()) {
            LoadFormatted(n, fm, a64, p);
            return;
        }
        std::string tbp = "%pu";
        for (int k = 0; k < 4; ++k) W(std::format("    mov.b32 %r{}_{}, 0;", n, k));
        // 16- and 8-byte texels in one vector load; anything else component by component (component k exists when
        // the texel has 4(k+1) bytes). Predicated, no branch (see BoundedLoad).
        W("    { .reg .pred %v4, %v2, %vs;");
        W(std::format("    setp.eq.and.u32 %v4, {}, 16, {};", tb, p));
        W(std::format("    setp.eq.and.u32 %v2, {}, 8, {};", tb, p));
        W("    or.pred %vs, %v4, %v2;");
        W("    not.pred %vs, %vs;");
        W(std::format("    and.pred %vs, %vs, {};", p));
        W(std::format("    @%v4 {}.v4.b32 {{%r{}_0, %r{}_1, %r{}_2, %r{}_3}}, [{}];", ldop_, n, n, n, n, a64));
        W(std::format("    @%v2 {}.v2.b32 {{%r{}_0, %r{}_1}}, [{}];", ldop_, n, n, a64));
        W(std::format("    setp.gt.and.u32 {}, {}, 0, %vs;", tbp, tb));
        W(std::format("    @{} {}.b32 %r{}_0, [{}];", tbp, ldop_, n, a64));
        for (int k = 1; k < 4; ++k) {
            W(std::format("    setp.gt.and.u32 {}, {}, {}, %vs;", tbp, tb, 4 * k));
            W(std::format("    @{} {}.b32 %r{}_{}, [{}+{}];", tbp, ldop_, n, k, a64, 4 * k));
        }
        W("    }");
        return;
    }
    if (op == "sampleLevel") {
        std::string h = A(1);
        std::string tex = Desc(h, 8, "u64");
        std::string u = V(A(3), "float"), vv = V(A(4), "float");
        std::string lod = V(A(10), "float");
        std::vector<std::string> regs;
        for (int k = 0; k < 4; ++k) regs.push_back(std::format("%r{}_{}", n, k));
        W(std::format("    tex.level.2d.v4.f32.f32 {{{}}}, [{}, {{{}, {}}}], {};", Join(regs, ", "), tex, u, vv, lod));
        return;
    }
    if (op == "textureStore") {
        std::string h = A(1);
        std::string base = Desc(h, 0, "u64");
        std::string W_ = Desc(h, 16), H_ = Desc(h, 20), S_ = Desc(h, 24), tb = Desc(h, 28);
        (void)S_;
        std::string x = V(A(2)), y = V(A(3));
        std::string z = A(4) != "undef" ? V(A(4)) : std::string("0");
        const char* ty = isF32 ? "float" : "i32";
        std::vector<std::string> vals;
        for (int k = 0; k < 4; ++k) vals.push_back(V(A(5 + k), ty));
        int mask = (int)ToInt(A(9));
        std::string p = "%pt", idx = Tmp();
        W(std::format("    setp.lt.u32 {}, {}, {};", p, x, W_));
        W(std::format("    setp.lt.and.u32 {}, {}, {}, {};", p, y, H_, p));
        W(std::format("    setp.lt.and.u32 {}, {}, {}, {};", p, z, S_, p));
        // row-pitched: (slice * height + row) * pitch + x * texel bytes, the pitch at descriptor +36
        std::string rp = Desc(h, 36), t2 = Tmp();
        W(std::format("    mad.lo.s32 {}, {}, {}, {};", idx, z, H_, y));
        W(std::format("    mul.lo.s32 {}, {}, {};", t2, x, tb));
        W(std::format("    mad.lo.s32 {}, {}, {}, {};", idx, idx, rp, t2));
        std::string a64 = Tmp64();
        W(std::format("    cvt.u64.u32 {}, {};", a64, idx));
        W(std::format("    add.s64 {}, {}, {};", a64, base, a64));
        std::string fm = FmtOf(h);
        auto regOperand = [&](const std::string& src) {
            if (!src.empty() && src[0] == '%') return src;
            std::string t1 = Tmp();
            W(std::format("    mov.b32 {}, {};", t1, src));
            return t1;
        };
        if (fm == "x128" || fm == "x64" || fm == "x32") {
            int cnt = fm == "x128" ? 4 : fm == "x64" ? 2 : 1;
            std::vector<std::string> srcs;
            for (int k = 0; k < cnt; ++k) srcs.push_back(regOperand(vals[k]));
            if ((mask & ((1 << cnt) - 1)) == (1 << cnt) - 1) {
                const char* vec = cnt == 4 ? ".v4" : cnt == 2 ? ".v2" : "";
                W(std::format("    @{} st.global{}.b32 [{}], {};", p, vec, a64, cnt > 1 ? "{" + Join(srcs, ", ") + "}" : srcs[0]));
            } else {
                for (int k = 0; k < cnt; ++k)
                    if (mask & (1 << k)) W(std::format("    @{} st.global.b32 [{}+{}], {};", p, a64, 4 * k, srcs[k]));
            }
            return;
        }
        if (!fm.empty()) {
            std::vector<std::string> regs;
            for (int k = 0; k < 4; ++k) regs.push_back(regOperand(vals[k]));
            StoreFormatted(fm, a64, regs, mask);
            return;
        }
        std::vector<std::string> srcs;
        for (int k = 0; k < 4; ++k) srcs.push_back(regOperand(vals[k]));
        // A whole 16- or 8-byte texel in one vector store, else component by component. Predicated, no branch (see
        // BoundedLoad).
        W("    { .reg .pred %vs;");
        W(std::format("    mov.pred %vs, {};", p));
        if (mask == 15) {
            W(std::format("    setp.eq.and.u32 %pu, {}, 16, {};", tb, p));
            W(std::format("    @%pu st.global.v4.b32 [{}], {{{}}};", a64, Join(srcs, ", ")));
            W(std::format("    setp.ne.and.u32 %vs, {}, 16, %vs;", tb));
        }
        if ((mask & 3) == 3) {
            W(std::format("    setp.eq.and.u32 %pu, {}, 8, %vs;", tb));
            W(std::format("    @%pu st.global.v2.b32 [{}], {{{}, {}}};", a64, srcs[0], srcs[1]));
            W(std::format("    setp.ne.and.u32 %vs, {}, 8, %vs;", tb));
        }
        for (int k = 0; k < 4; ++k) {
            if (!(mask & (1 << k))) continue;
            W(std::format("    setp.gt.and.u32 %pu, {}, {}, %vs;", tb, 4 * k));
            W(std::format("    @%pu st.global.b32 [{}+{}], {};", a64, 4 * k, srcs[k]));
        }
        W("    }");
        return;
    }
    die("dx.op " + op + " not handled");
}

// A load/store/atomic pointer operand: a pointer value, or a constant getelementptr written inline into the
// instruction. Returns (state space, address operand).
std::pair<std::string, std::string> Translator::Pointer(const std::string& tok0) {
    std::string tok = Strip(tok0);
    static const std::regex reGep(
        R"(getelementptr (?:inbounds )?\(\[\d+ x \w+\], \[\d+ x \w+\]( addrspace\(3\))?\* (\S+), i32 0, i32 (\d+)\))");
    if (Starts(tok, "getelementptr")) {
        std::smatch m;
        if (std::regex_match(tok, m, reGep)) {
            int k = std::atoi(m[3].str().c_str());
            if (m[1].matched) {
                auto it = shared_.find(m[2].str());
                if (it == shared_.end()) die("unknown shared array " + m[2].str());
                return {"shared", std::format("{}+{}", it->second.first, 4 * k)};
            }
            return {"local", std::format("la{}+{}", m[2].str().substr(1), 4 * k)};
        }
    }
    if (!tok.empty() && tok[0] == '%') {
        auto sp = space_.find(tok.substr(1));
        if (sp != space_.end()) return {sp->second, V(tok)};
        if (locals_.count(tok.substr(1))) return {"local", "la" + tok.substr(1)};
    }
    if (!tok.empty() && tok[0] == '@') {
        auto it = shared_.find(tok);
        if (it != shared_.end()) return {"shared", it->second.first};
    }
    die("pointer operand not handled: " + tok);
}

std::string Translator::SelPred(const std::string& x) {
    if (Starts(x, "%p")) {
        std::string t1 = Tmp();
        W(std::format("    selp.u32 {}, 1, 0, {};", t1, x));
        return t1;
    }
    return x;
}

// Copies for the phis of dst on the edge from src, through temporaries.
std::vector<std::string> Translator::EdgeCopies(const std::string& src, const std::string& dst) {
    std::vector<std::string> out;
    auto it = phis_.find(dst);
    if (it == phis_.end()) return out;
    struct TempCopy {
        std::string n, ty, t1;
    };
    std::vector<TempCopy> temps;
    for (const Phi& ph : it->second) {
        for (const auto& [value, pb] : ph.inc) {
            if (pb != src) continue;
            std::string t1;
            if (ph.type == "i1") {
                t1 = "%ptmp" + ph.name;
                std::string sp = SelPred(V(value, "i1"));
                out.push_back(std::format("    setp.ne.u32 {}, {}, 0;", t1, sp));
            } else {
                t1 = "%c" + ph.name;
                out.push_back(std::format("    mov.b32 {}, {};", t1, V(value, ph.type == "float" ? "float" : "i32")));
            }
            temps.push_back({ph.name, ph.type, t1});
        }
    }
    for (const auto& t : temps)
        out.push_back(t.ty == "i1" ? std::format("    mov.pred %p{}, {};", t.n, t.t1) : std::format("    mov.b32 %r{}, {};", t.n, t.t1));
    return out;
}

void Translator::EmitBody() {
    // phis by block
    for (const auto& b : order_) {
        for (const std::string& s : blocks_[b]) {
            std::string n, rhs;
            if (!ParseAssign(s, n, rhs) || !Starts(rhs, "phi ")) continue;
            std::string rest = rhs.substr(4);
            size_t sp = rest.find(' ');
            if (sp == std::string::npos || sp == 0) continue;
            Phi ph;
            ph.name = n;
            ph.type = rest.substr(0, sp);
            std::string tail = rest.substr(sp + 1);
            // findall of "[ <value>, %<block> ]"
            for (size_t i = 0; i < tail.size();) {
                size_t lb = tail.find("[ ", i);
                if (lb == std::string::npos) break;
                size_t comma = tail.find(',', lb + 2);
                if (comma == std::string::npos) break;
                bool ok = comma > lb + 2 && tail.compare(comma, 3, ", %") == 0;
                size_t de = ok ? DigitsEnd(tail, comma + 3) : 0;
                ok = ok && de > comma + 3 && tail.compare(de, 2, " ]") == 0;
                if (ok) {
                    ph.inc.push_back({tail.substr(lb + 2, comma - lb - 2), tail.substr(comma + 3, de - comma - 3)});
                    i = de + 2;
                } else {
                    i = lb + 1;
                }
            }
            phis_[b].push_back(std::move(ph));
        }
    }

    static const std::regex reGepI(
        R"(getelementptr (?:inbounds )?\[\d+ x \w+\], \[\d+ x \w+\]( addrspace\(3\))?\* (\S+), i32 0, i32 (\S+))");
    static const std::regex reLoad(R"(load (float|i32), (?:float|i32)( addrspace\(3\))?\* (.+), align \d+)");
    static const std::regex reAtomic(R"(atomicrmw (\w+) i32 addrspace\(3\)\* (.+), i32 (\S+) seq_cst)");
    static const std::regex reStore(R"(store (float|i32) (\S+), (?:float|i32)( addrspace\(3\))?\* (.+), align \d+)");
    static const std::regex reAlloca(R"(alloca \[(\d+) x (?:i32|float)\].*)");

    for (const auto& b : order_) {
        W("L" + b + ":");
        for (const std::string& s : blocks_[b]) {
            std::string n, rhs;
            if (ParseAssign(s, n, rhs)) {
                std::string d = "%r" + n;
                if (Starts(rhs, "phi ")) continue;
                if (Starts(rhs, "call ")) {
                    EmitCall(n, true, rhs);
                    continue;
                }
                if (Starts(rhs, "alloca ")) {
                    std::smatch m;
                    if (!std::regex_match(rhs, m, reAlloca)) die("alloca not handled: " + rhs);
                    if (!locals_.count(n)) localsOrder_.push_back(n);
                    locals_[n] = std::atoi(m[1].str().c_str());
                    continue;
                }
                if (Starts(rhs, "getelementptr")) {
                    std::smatch m;
                    if (!std::regex_match(rhs, m, reGepI)) die("instruction not handled: " + s);
                    std::string t1 = Tmp();
                    if (m[1].matched) {
                        auto it = shared_.find(m[2].str());
                        if (it == shared_.end()) die("unknown shared array");
                        W(std::format("    mov.u32 {}, {};", t1, it->second.first));
                        space_[n] = "shared";
                    } else {
                        W(std::format("    mov.u32 {}, la{};", t1, m[2].str().substr(1)));
                        space_[n] = "local";
                    }
                    W(std::format("    mad.lo.s32 {}, {}, 4, {};", d, V(m[3].str()), t1));
                    continue;
                }
                if (Starts(rhs, "load ")) {
                    std::smatch m;
                    if (!std::regex_match(rhs, m, reLoad)) die("instruction not handled: " + s);
                    auto [sp, ptr] = Pointer(m[3].str());
                    W(std::format("    ld.{}.b32 {}, [{}];", sp, d, ptr));
                    continue;
                }
                if (Starts(rhs, "atomicrmw ")) {
                    std::smatch m;
                    if (!std::regex_match(rhs, m, reAtomic)) die("instruction not handled: " + s);
                    static const std::map<std::string, const char*> aops = {
                        {"add", "add.u32"}, {"and", "and.b32"}, {"or", "or.b32"}, {"xor", "xor.b32"}, {"max", "max.s32"},
                        {"min", "min.s32"}, {"umax", "max.u32"}, {"umin", "min.u32"}, {"xchg", "exch.b32"}};
                    auto ao = aops.find(m[1].str());
                    if (ao == aops.end()) die("atomicrmw " + m[1].str());
                    auto [sp, ptr] = Pointer(m[2].str());
                    std::string x = V(m[3].str());
                    if (x.empty() || x[0] != '%') {
                        std::string t1 = Tmp();
                        W(std::format("    mov.b32 {}, {};", t1, x));
                        x = t1;
                    }
                    W(std::format("    atom.shared.{} {}, [{}], {};", ao->second, d, ptr, x));
                    continue;
                }
                if (Starts(rhs, "extractvalue ")) {
                    // extractvalue <type> %N, K
                    size_t sp = rhs.find(' ', 13);
                    if (sp != std::string::npos && sp > 13 && sp + 1 < rhs.size() && rhs[sp + 1] == '%') {
                        size_t e = DigitsEnd(rhs, sp + 2);
                        if (e > sp + 2 && rhs.compare(e, 2, ", ") == 0) {
                            size_t e2 = DigitsEnd(rhs, e + 2);
                            if (e2 > e + 2) {
                                alias_[n] = std::format("%r{}_{}", rhs.substr(sp + 2, e - sp - 2), rhs.substr(e + 2, e2 - e - 2));
                                continue;
                            }
                        }
                    }
                    die("instruction not handled: " + s);
                }
                std::vector<std::string> t = SplitChar(rhs, ' ');
                const std::string& op = t[0];
                // fadd/fsub/fmul/fdiv/frem [fast-math flags] float X, Y
                if (op == "fadd" || op == "fsub" || op == "fmul" || op == "fdiv" || op == "frem") {
                    size_t i = 1;
                    while (i < t.size() && (t[i] == "fast" || t[i] == "nnan" || t[i] == "ninf" || t[i] == "nsz" || t[i] == "arcp")) ++i;
                    if (i + 3 != t.size() || t[i] != "float" || t[i + 1].size() < 2 || t[i + 1].back() != ',') die("instruction not handled: " + s);
                    std::string x = V(t[i + 1].substr(0, t[i + 1].size() - 1), "float"), y = V(t[i + 2], "float");
                    if (op == "fdiv") {
                        if (o_.div == "rcp") {
                            // x / y as rcp(y), times x unless x is 1
                            if (x == "0f3F800000") {
                                W(std::format("    rcp.approx{}.f32 {}, {};", ftz_, d, y));
                            } else {
                                std::string t1 = Tmp();
                                W(std::format("    rcp.approx{}.f32 {}, {};", ftz_, t1, y));
                                W(std::format("    mul{}{}.f32 {}, {}, {};", rnd_, ftz_, d, x, t1));
                            }
                        } else if (o_.div == "approx") {
                            W(std::format("    div.approx{}.f32 {}, {}, {};", ftz_, d, x, y));
                        } else if (o_.div == "full") {
                            W(std::format("    div.full{}.f32 {}, {}, {};", ftz_, d, x, y));
                        } else {
                            W(std::format("    div.rn{}.f32 {}, {}, {};", ftz_, d, x, y));
                        }
                    } else {
                        bool startsPct = x.size() > 1 && x[0] == '%' && x[1] == 'r';
                        if (op == "fsub" && distribute_ && Starts(x, "0f") && uses_[n] == 1) csubs_[n] = {x, y};
                        if (op == "fmul" && distribute_) {
                            std::string sub, k;
                            if (startsPct && csubs_.count(x.substr(2))) {
                                sub = x;
                                k = y;
                            } else {
                                sub = y;
                                k = x;
                            }
                            if (sub.size() > 1 && sub[0] == '%' && sub[1] == 'r' && csubs_.count(sub.substr(2)) && Starts(k, "0f")) {
                                auto [c, sx] = csubs_[sub.substr(2)];
                                double cf = BitsF32((uint32_t)std::strtoul(c.c_str() + 2, nullptr, 16));
                                double kf = BitsF32((uint32_t)std::strtoul(k.c_str() + 2, nullptr, 16));
                                double prod = cf * kf;
                                float ck = static_cast<float>(prod);
                                if (static_cast<double>(ck) == prod) {
                                    std::string t1 = Tmp();
                                    W(std::format("    neg{}.f32 {}, {};", ftz_, t1, sx));
                                    W(std::format("    fma.rn{}.f32 {}, {}, {}, 0f{:08X};", ftz_, d, t1, k, F32Bits(ck)));
                                    continue;
                                }
                            }
                        }
                        if (op == "fmul") fmuls_[n] = {x, y};
                        bool fx = fuse_.has_value() && (op == "fadd" || op == "fsub");
                        auto fusable = [&](const std::string& z) {
                            return fx && z.size() > 1 && z[0] == '%' && z[1] == 'r' && fuse_->count(z.substr(2)) && fmuls_.count(z.substr(2));
                        };
                        bool mx = fusable(x), my = fusable(y);
                        if (mx && my) {
                            if (o_.prefer == "first")
                                my = false;
                            else
                                mx = false;
                        }
                        if (my) {  // x +- a*b
                            auto [ma, mb] = fmuls_[y.substr(2)];
                            if (op == "fsub") {
                                std::string t1 = Tmp();
                                W(std::format("    neg{}.f32 {}, {};", ftz_, t1, ma));
                                ma = t1;
                            }
                            W(std::format("    fma.rn{}.f32 {}, {}, {}, {};", ftz_, d, ma, mb, x));
                        } else if (mx) {  // a*b +- y
                            auto [ma, mb] = fmuls_[x.substr(2)];
                            std::string c = y;
                            if (op == "fsub") {
                                c = Tmp();
                                W(std::format("    neg{}.f32 {}, {};", ftz_, c, y));
                            }
                            W(std::format("    fma.rn{}.f32 {}, {}, {}, {};", ftz_, d, ma, mb, c));
                        } else {
                            W(std::format("    {}{}{}.f32 {}, {}, {};", op.substr(1), rnd_, ftz_, d, x, y));
                        }
                    }
                    continue;
                }
                // add/sub/... [nuw nsw exact] i32|i1 X, Y
                static const std::unordered_set<std::string> ibin = {"add", "sub", "mul", "shl", "lshr", "ashr", "and", "or", "xor", "udiv", "urem", "sdiv", "srem"};
                if (ibin.count(op)) {
                    size_t i = 1;
                    while (i < t.size() && (t[i] == "nuw" || t[i] == "nsw" || t[i] == "exact")) ++i;
                    if (i + 3 != t.size() || (t[i] != "i32" && t[i] != "i1") || t[i + 1].size() < 2 || t[i + 1].back() != ',') die("instruction not handled: " + s);
                    std::string ty = t[i], x = t[i + 1].substr(0, t[i + 1].size() - 1), y = t[i + 2];
                    if (ty == "i1") {
                        const char* pm = op == "and" ? "and.pred" : op == "or" ? "or.pred" : op == "xor" ? "xor.pred" : nullptr;
                        if (!pm) die("i1 operation " + op);
                        std::string xs = V(x, "i1"), ys = V(y, "i1");
                        for (const auto& q : {xs, ys})
                            if (!Starts(q, "%p")) die("i1 literal operand not handled: " + s);
                        W(std::format("    {} %p{}, {}, {};", pm, n, xs, ys));
                    } else {
                        std::string xs = V(x), ys = V(y);
                        W(std::format("    {} {}, {}, {};", IntBin(op), d, xs, ys));
                    }
                    continue;
                }
                if (op == "fptosi" || op == "fptoui" || op == "sitofp" || op == "uitofp" || op == "bitcast" || op == "zext" ||
                    op == "sext" || op == "trunc") {
                    if (t.size() != 5 || t[3] != "to") die("instruction not handled: " + s);
                    const std::string& fty = t[1];
                    const std::string& x = t[2];
                    if (op == "fptosi") {
                        W(std::format("    cvt.rzi{}.s32.f32 {}, {};", ftz_, d, V(x, "float")));
                    } else if (op == "fptoui") {
                        W(std::format("    cvt.rzi{}.u32.f32 {}, {};", ftz_, d, V(x, "float")));
                    } else if (op == "sitofp") {
                        W(std::format("    cvt.rn.f32.s32 {}, {};", d, V(x)));
                    } else if (op == "uitofp") {
                        W(std::format("    cvt.rn.f32.u32 {}, {};", d, V(x)));
                    } else if (op == "bitcast") {
                        W(std::format("    mov.b32 {}, {};", d, V(x, fty == "float" ? "float" : "i32")));
                    } else if (fty == "i1") {
                        W(std::format("    selp.u32 {}, {}, 0, {};", d, op == "sext" ? "-1" : "1", V(x, "i1")));
                    } else {
                        die("cast not handled: " + s);
                    }
                    continue;
                }
                if (op == "icmp") {
                    if (t.size() != 5 || t[2] != "i32" || t[3].size() < 2 || t[3].back() != ',') die("instruction not handled: " + s);
                    std::string x = t[3].substr(0, t[3].size() - 1);
                    W(std::format("    setp.{} %p{}, {}, {};", IcmpName(t[1]), n, V(x), V(t[4])));
                    continue;
                }
                if (op == "fcmp") {
                    size_t i = 1;
                    if (i < t.size() && t[i] == "fast") ++i;
                    if (i + 4 != t.size() || t[i + 1] != "float" || t[i + 2].size() < 2 || t[i + 2].back() != ',') die("instruction not handled: " + s);
                    std::string x = t[i + 2].substr(0, t[i + 2].size() - 1);
                    W(std::format("    setp.{}{}.f32 %p{}, {}, {};", FcmpName(t[i]), ftz_, n, V(x, "float"), V(t[i + 3], "float")));
                    continue;
                }
                if (op == "select") {
                    // select i1 C, T X, T Y
                    if (t.size() != 7 || t[1] != "i1" || t[2].size() < 2 || t[2].back() != ',' || t[4].back() != ',') die("instruction not handled: " + s);
                    std::string c = V(t[2].substr(0, t[2].size() - 1), "i1");
                    const std::string& t1 = t[3];
                    std::string x = t[4].substr(0, t[4].size() - 1), y = t[6];
                    const char* ty = t1 == "float" ? "float" : "i32";
                    if (t1 == "i1") {
                        std::string xs = V(x, "i1"), ys = V(y, "i1");
                        W(std::format("    {{ .reg .pred %a, %b; and.pred %a, {}, {}; not.pred %b, {}; and.pred %b, %b, {}; or.pred %p{}, %a, %b; }}",
                                      c, xs, c, ys, n));
                    } else {
                        std::string xs = V(x, ty), ys = V(y, ty);
                        W(std::format("    selp.b32 {}, {}, {}, {};", d, xs, ys, c));
                    }
                    continue;
                }
                die("instruction not handled: " + s);
            }
            if (Starts(s, "store ")) {
                std::smatch m;
                if (std::regex_match(s, m, reStore)) {
                    std::string ty = m[1].str();
                    std::string x = V(m[2].str(), ty == "float" ? "float" : "i32");
                    if (x.empty() || x[0] != '%') {
                        std::string t1 = Tmp();
                        W(std::format("    mov.b32 {}, {};", t1, x));
                        x = t1;
                    }
                    auto [sp, ptr] = Pointer(m[4].str());
                    W(std::format("    st.{}.b32 [{}], {};", sp, ptr, x));
                    continue;
                }
            }
            if (Starts(s, "call void")) {
                EmitCall("", false, s);
                continue;
            }
            std::string c, x, y;
            if (ParseCondBr(s, c, x, y)) {
                std::string cv = V(c, "i1");
                W(std::format("    @{} bra E{}_{};", cv, b, x));
                auto ef = EdgeCopies(b, y);
                L_.insert(L_.end(), ef.begin(), ef.end());
                W("    bra L" + y + ";");
                W(std::format("E{}_{}:", b, x));
                auto et = EdgeCopies(b, x);
                L_.insert(L_.end(), et.begin(), et.end());
                W("    bra L" + x + ";");
                continue;
            }
            if (ParseBr(s, x)) {
                auto e = EdgeCopies(b, x);
                L_.insert(L_.end(), e.begin(), e.end());
                W("    bra L" + x + ";");
                continue;
            }
            if (s == "ret void") {
                W("    ret;");
                continue;
            }
            die("line not handled: " + s);
        }
    }
}

// The registers a PTX line uses, by the pattern each declaration list is made from.
void ScanRegs(const std::string& line, std::set<std::string>& regs32, std::set<std::string>& preds, std::set<std::string>& q64) {
    for (size_t i = 0; i < line.size(); ++i) {
        if (line[i] != '%') continue;
        size_t p = i + 1;
        if (p >= line.size()) break;
        char c = line[p];
        auto boundary = [&](size_t e) { return e >= line.size() || !IsWord(line[e]); };
        if (c == 'r') {  // r\d+(_\d)?
            size_t e = DigitsEnd(line, p + 1);
            if (e > p + 1) {
                if (e + 1 < line.size() && line[e] == '_' && IsDigit(line[e + 1]) && boundary(e + 2)) {
                    regs32.insert(line.substr(p, e + 2 - p));
                } else if (boundary(e)) {
                    regs32.insert(line.substr(p, e - p));
                }
            }
        } else if (c == 't' || c == 'c') {  // t\d+ | c\d+
            size_t e = DigitsEnd(line, p + 1);
            if (e > p + 1 && boundary(e)) regs32.insert(line.substr(p, e - p));
        } else if (c == 'p') {  // p\d+ | ptmp\d+
            size_t e = DigitsEnd(line, p + 1);
            if (e > p + 1 && boundary(e)) {
                preds.insert(line.substr(p, e - p));
            } else if (line.compare(p, 4, "ptmp") == 0) {
                size_t e2 = DigitsEnd(line, p + 4);
                if (e2 > p + 4 && boundary(e2)) preds.insert(line.substr(p, e2 - p));
            }
        } else if (c == 'q') {
            size_t e = DigitsEnd(line, p + 1);
            if (e > p + 1 && boundary(e)) q64.insert(line.substr(p, e - p));
        }
    }
}

std::string Translator::Finish() {
    std::set<std::string> regs32, preds, q64;
    for (const auto& line : L_) ScanRegs(line, regs32, preds, q64);

    std::vector<std::string> out;
    out.push_back("// GENERATED by dxil2ptx.py from " + o_.sourceName + " - do not edit by hand.");
    out.push_back(std::format("// numthreads {} x {} x {}; flags: exact={} ftz={} div={}", nt_[0], nt_[1], nt_[2],
                              o_.exact ? "True" : "False", o_.noFtz ? "False" : "True", o_.div));
    out.push_back(".version 7.0");
    out.push_back(".target " + o_.target);
    out.push_back(".address_size 64");
    out.push_back("");
    out.push_back(".visible .entry " + o_.entry + "(");
    out.push_back("    .param .u64 p_desc");
    // Then one parameter per descriptor slot used (slot order), and one per constant buffer the kernel reads
    // (register order).
    for (int slot : slotsUsed_) {
        out.back() += ",";
        out.push_back(std::format("    .param .align 16 .b8 p_s{}[48]", slot));
    }
    std::set<int> cbUsed;
    for (const auto& [name, h] : handles_)
        if (h.first == 2 && cbsize_.count(h.second)) cbUsed.insert(h.second);
    for (int reg : cbUsed) {
        out.back() += ",";
        out.push_back(std::format("    .param .align 16 .b8 p_cb{}[{}]", reg, (cbsize_[reg] + 15) / 16 * 16));
    }
    out.push_back(std::format(") .reqntid {}, {}, {}", nt_[0], nt_[1], nt_[2]));
    out.push_back("{");
    for (const auto& g : sharedOrder_) out.push_back(std::format("    .shared .align 4 .b32 {}[{}];", shared_[g].first, shared_[g].second));
    for (const auto& l : localsOrder_) out.push_back(std::format("    .local .align 4 .b32 la{}[{}];", l, locals_[l]));
    out.push_back("    .reg .pred %pt, %pu;");
    auto chunks = [&](const char* prefix, std::vector<std::string> names) {
        for (size_t c = 0; c < names.size(); c += 40) {
            std::vector<std::string> part;
            for (size_t i = c; i < std::min(names.size(), c + 40); ++i) part.push_back("%" + names[i]);
            out.push_back(std::string(prefix) + Join(part, ", ") + ";");
        }
    };
    chunks("    .reg .pred ", {preds.begin(), preds.end()});
    chunks("    .reg .b32 ", {regs32.begin(), regs32.end()});
    std::vector<std::string> q(q64.begin(), q64.end());
    q.push_back("desc");
    chunks("    .reg .b64 ", q);
    out.insert(out.end(), L_.begin(), L_.end());
    out.push_back("}");
    if (o_.vecShared) out = VectorizeShared(std::move(out), nullptr, nullptr);
    std::string text = Join(out, "\n");
    text += "\n";
    return text;
}

std::string Translator::Run() {
    ftz_ = o_.noFtz ? "" : ".ftz";
    // Float contraction: every fmul whose single use is an fadd/fsub is fused into it, the add's first operand tried
    // first (the rule the driver's compiler follows; found by search against Intel's shaders). Every other float op
    // carries .rn so ptxas cannot contract on its own - left alone (contract backend) it skips some of these and
    // fuses others. exact disables contraction entirely (negative controls).
    if (o_.contract != "backend") fusePolicy_ = true;
    rnd_ = (o_.exact || fusePolicy_) ? ".rn" : "";
    if (o_.exact) fusePolicy_ = false;
    distribute_ = o_.distribute && !o_.exact && fusePolicy_;
    for (const std::string& spec : o_.fmts) {
        size_t eq = spec.find('=');
        if (eq == std::string::npos || eq < 2) die("bad format spec " + spec);
        formats_[{spec[0] == 'S' ? 0 : 1, std::atoi(spec.substr(1, eq - 1).c_str())}] = spec.substr(eq + 1);
    }
    ParseModule();
    if (order_.empty()) die("no function body");
    IfConvert();
    ParseMetadata();
    DeadCode();
    if (fusePolicy_) {
        // a second rule (the splat for non-inverted depth: (1 - depth) * 32767): a single-use constant-minus-x times a
        // constant is distributed into fma(-x, K, C*K) when C*K is exact
        std::unordered_set<std::string> f;
        for (const auto& b : order_)
            for (const std::string& s : blocks_[b]) {
                std::string n, rhs;
                if (ParseAssign(s, n, rhs) && Starts(rhs, "fmul") && (o_.contract == "any" || uses_[n] == 1)) f.insert(n);
            }
        fuse_ = std::move(f);
    }
    Types();
    EmitBody();
    return Finish();
}

}  // namespace

bool Dxil2Ptx(const std::string& disassembly, const Options& opt, std::string& ptx, std::string* err) {
    try {
        std::vector<std::string> lines = SplitLines(disassembly);
        Translator t(lines, opt);
        ptx = t.Run();
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

// ---- vectorize shared loads ----------------------------------------------------------------------------
// Merges runs of four ld.shared.b32 whose addresses are provably a, a+4, a+8, a+12 with a 16-byte aligned into one
// ld.shared.v4.b32, after declaring the shared arrays .align 16 (Dxil2Ptx's last step).
//
// Why: the dp4a convolutions read their weights and activations from groupshared memory four words at a time. The
// translation emits them one word per load, and ptxas cannot merge them since it cannot prove the alignment. The
// driver's own DXIL compiler does, so the original shader beat ours on every such layer.
//
// How: within a basic block every 32-bit register is tracked as a linear form over opaque atoms (mov, add, sub,
// mul.lo, mad.lo and shl with a constant); a shared array's symbol is an atom aligned to 16. The merged load takes
// the first load's place and the other three are dropped. Only merged when no store, barrier, label or branch lies
// between and the three later destinations are not touched in between.
namespace {

struct Lin {
    std::map<std::string, long long> m;
    long long c = 0;
};
Lin LinAdd(const Lin& x, const Lin& y, long long s = 1) {
    Lin d = x;
    for (const auto& [k, v] : y.m) d.m[k] += s * v;
    for (auto it = d.m.begin(); it != d.m.end();) it = it->second == 0 ? d.m.erase(it) : std::next(it);
    d.c = x.c + s * y.c;
    return d;
}
Lin LinScale(const Lin& x, long long c) {
    Lin d;
    for (const auto& [k, v] : x.m)
        if (v * c) d.m[k] = v * c;
    d.c = x.c * c;
    return d;
}
bool LinConst(const Lin& x) { return x.m.empty(); }
bool LinAligned16(const Lin& x) {
    if (x.c % 16 != 0) return false;
    for (const auto& [k, v] : x.m) {
        if (Starts(k, "sym:") ? v != 1 : v % 16 != 0) return false;
    }
    return true;
}
bool FullInt(const std::string& s) {
    size_t i = (!s.empty() && s[0] == '-') ? 1 : 0;
    return i < s.size() && DigitsEnd(s, i) == s.size();
}

struct Instr {
    std::string op;
    std::vector<std::string> a;
};
// ^\s*(?:@!?%\w+\s+)?([a-z][\w.]*)\s+(.*?);\s*$
bool ParseInstr(const std::string& l, Instr& out) {
    size_t i = 0;
    while (i < l.size() && IsSpace(l[i])) ++i;
    if (i < l.size() && l[i] == '@') {
        size_t j = i + 1;
        if (j < l.size() && l[j] == '!') ++j;
        if (j < l.size() && l[j] == '%') {
            size_t k = j + 1;
            while (k < l.size() && IsWord(l[k])) ++k;
            size_t m = k;
            while (m < l.size() && IsSpace(l[m])) ++m;
            if (k > j + 1 && m > k) i = m;
        }
    }
    if (i >= l.size() || !(l[i] >= 'a' && l[i] <= 'z')) return false;
    size_t e = i + 1;
    while (e < l.size() && (IsWord(l[e]) || l[e] == '.')) ++e;
    size_t m = e;
    while (m < l.size() && IsSpace(l[m])) ++m;
    if (m == e) return false;
    size_t end = l.size();
    while (end > m && IsSpace(l[end - 1])) --end;
    if (end <= m || l[end - 1] != ';') return false;
    out.op = l.substr(i, e - i);
    std::string body = l.substr(m, end - 1 - m);
    out.a.clear();
    for (const auto& x : SplitChar(body, ',')) out.a.push_back(Strip(x));
    return true;
}
bool FullMatchDigits(const std::string& s) { return !s.empty() && DigitsEnd(s, 0) == s.size(); }
bool IsLabelLine(const std::string& s) {  // ^[\w$]+:\s*$
    size_t i = 0;
    while (i < s.size() && (IsWord(s[i]) || s[i] == '$')) ++i;
    if (i == 0 || i >= s.size() || s[i] != ':') return false;
    for (size_t k = i + 1; k < s.size(); ++k)
        if (!IsSpace(s[k])) return false;
    return true;
}
bool HasWordBoundaryAfter(const std::string& line, const std::string& name) {
    for (size_t at = line.find(name); at != std::string::npos; at = line.find(name, at + 1)) {
        size_t e = at + name.size();
        if (e >= line.size() || !IsWord(line[e])) return true;
    }
    return false;
}

}  // namespace

std::vector<std::string> VectorizeShared(std::vector<std::string> lines, size_t* mergedOut, size_t* loadsOut) {
    for (auto& l : lines) {
        size_t at = l.find(".shared .align 4 ");
        if (at != std::string::npos) l.replace(at, 17, ".shared .align 16 ");
    }
    std::set<std::string> syms;
    for (const auto& l : lines) {
        size_t at = l.find(".shared .align 16 .b32 ");
        if (at == std::string::npos) continue;
        size_t p = at + 23, e = p;
        while (e < l.size() && IsWord(l[e])) ++e;
        if (e > p && e < l.size() && l[e] == '[') syms.insert(l.substr(p, e - p));
    }
    std::map<std::string, Lin> env;
    int fresh = 0;
    auto val = [&](const std::string& t0) -> Lin {
        std::string tok = Strip(t0);
        if (FullInt(tok)) return Lin{{}, std::atoll(tok.c_str())};
        if (syms.count(tok)) return Lin{{{"sym:" + tok, 1}}, 0};
        auto it = env.find(tok);
        if (it != env.end()) return it->second;
        return Lin{{{"reg:" + tok, 1}}, 0};
    };
    auto opaque = [&](const std::string& dst) { env[dst] = Lin{{{"op" + std::to_string(++fresh), 1}}, 0}; };

    struct Info {
        int kind = 0;  // 0 other, 1 barrier, 2 lds
        std::string dst;
        std::optional<Lin> addr;
    };
    std::vector<Info> info;
    for (const auto& l : lines) {
        std::string s = Strip(l);
        if (s.empty() || Starts(s, "//")) {
            info.push_back({});
            continue;
        }
        if (IsLabelLine(s) || Starts(s, "{") || Starts(s, "}")) {
            env.clear();
            info.push_back({1});
            continue;
        }
        Instr in;
        if (!ParseInstr(l, in)) {
            info.push_back({});
            continue;
        }
        const std::string& op = in.op;
        auto& a = in.a;
        bool pred = Starts(s, "@");
        if (op == "ld.shared.b32" && !pred && a.size() == 2) {
            // \[(%?\w+)(?:\+(-?\d+))?\]
            std::optional<Lin> addr;
            const std::string& t = a[1];
            if (t.size() >= 3 && t.front() == '[' && t.back() == ']') {
                std::string inner = t.substr(1, t.size() - 2);
                size_t i = 0;
                if (i < inner.size() && inner[i] == '%') ++i;
                size_t e = i;
                while (e < inner.size() && IsWord(inner[e])) ++e;
                if (e > i) {
                    std::string base = inner.substr(0, e);
                    long long off = 0;
                    bool ok = true;
                    if (e < inner.size()) {
                        if (inner[e] == '+' && FullInt(inner.substr(e + 1)))
                            off = std::atoll(inner.c_str() + e + 1);
                        else
                            ok = false;
                    }
                    if (ok) addr = LinAdd(val(base), Lin{{}, off});
                }
            }
            opaque(a[0]);
            info.push_back({2, a[0], addr});
            continue;
        }
        if (Starts(op, "st.shared") || Starts(op, "atom.shared") || Starts(op, "bar") || Starts(op, "bra") ||
            Starts(op, "ret") || Starts(op, "exit") || Starts(op, "call")) {
            if (Starts(op, "bra")) env.clear();
            info.push_back({1});
            continue;
        }
        std::string dst = (!a.empty() && Starts(a[0], "%")) ? a[0] : std::string();
        if (!dst.empty()) {
            if (pred) {
                opaque(dst);
            } else if (Starts(op, "mov.") && a.size() == 2) {
                env[dst] = val(a[1]);
            } else if (op == "add.s32" || op == "add.u32") {
                env[dst] = LinAdd(val(a[1]), val(a[2]));
            } else if (op == "sub.s32" || op == "sub.u32") {
                env[dst] = LinAdd(val(a[1]), val(a[2]), -1);
            } else if (op == "mul.lo.s32" || op == "mul.lo.u32") {
                Lin x = val(a[1]), y = val(a[2]);
                if (LinConst(y))
                    env[dst] = LinScale(x, y.c);
                else if (LinConst(x))
                    env[dst] = LinScale(y, x.c);
                else
                    opaque(dst);
            } else if (op == "mad.lo.s32" || op == "mad.lo.u32") {
                Lin x = val(a[1]), y = val(a[2]), z = val(a[3]);
                if (LinConst(y))
                    env[dst] = LinAdd(LinScale(x, y.c), z);
                else if (LinConst(x))
                    env[dst] = LinAdd(LinScale(y, x.c), z);
                else
                    opaque(dst);
            } else if (op == "shl.b32" && a.size() > 2 && FullMatchDigits(a[2])) {
                env[dst] = LinScale(val(a[1]), 1LL << std::atoi(a[2].c_str()));
            } else {
                opaque(dst);
            }
        }
        info.push_back({0, dst});
    }

    std::set<size_t> drop;
    std::map<size_t, std::string> replace;
    size_t merged = 0;
    for (size_t i = 0; i < lines.size(); ++i) {
        if (info[i].kind != 2 || drop.count(i) || !info[i].addr || !LinAligned16(*info[i].addr)) continue;
        const Lin a0 = *info[i].addr;
        std::vector<size_t> group = {i};
        bool ok = true;
        for (size_t j = i + 1; j < lines.size(); ++j) {
            if (group.size() == 4) break;
            if (info[j].kind == 1) {
                ok = false;
                break;
            }
            if (info[j].kind == 2 && info[j].addr) {
                Lin d = LinAdd(*info[j].addr, a0, -1);
                if (LinConst(d) && d.c == 4 * (long long)group.size()) {
                    group.push_back(j);
                } else if (LinConst(d) && d.c >= 0 && d.c < 16) {
                    ok = false;
                    break;
                }
            }
        }
        if (!ok || group.size() < 4) continue;
        std::vector<std::string> dsts;
        for (size_t g : group) dsts.push_back(info[g].dst);
        bool touched = false;
        for (int k = 1; k < 4 && !touched; ++k)
            for (size_t t = group[0] + 1; t < group[k]; ++t) {
                if (std::find(group.begin(), group.end(), t) != group.end()) continue;
                if (HasWordBoundaryAfter(lines[t], dsts[k])) {
                    touched = true;
                    break;
                }
            }
        if (touched) continue;
        size_t ind = 0;
        while (ind < lines[i].size() && IsSpace(lines[i][ind])) ++ind;
        size_t lb = lines[i].find('['), rb = lines[i].find(']', lb == std::string::npos ? 0 : lb);
        replace[i] = std::format("{}ld.shared.v4.b32 {{{}}}, [{}];", lines[i].substr(0, ind), Join(dsts, ", "),
                                 lines[i].substr(lb + 1, rb - lb - 1));
        for (size_t k = 1; k < group.size(); ++k) drop.insert(group[k]);
        ++merged;
    }
    size_t total = 0;
    for (const auto& x : info) total += x.kind == 2;
    if (mergedOut) *mergedOut = merged;
    if (loadsOut) *loadsOut = total;
    std::vector<std::string> out;
    for (size_t k = 0; k < lines.size(); ++k) {
        if (drop.count(k)) continue;
        auto it = replace.find(k);
        out.push_back(it != replace.end() ? it->second : lines[k]);
    }
    return out;
}

// ---- the reconstruction edit ----------------------------------------------------------------------------
// The multi-frame model's reconstruction input is shipped as DXIL that writes the U-Net input for a different
// U-Net: a raw buffer with a one-pixel border, 16 bytes at pixel (x + 1, y + 1). The DXIL U-Net reads a
// Texture2DArray<uint4> at (x, y). The edit rewrites the module's two U-Net-input stores (the values, and the zeros
// of threads outside the border rect) into texture stores at the thread's own (x, y); nothing else changes.
bool ReconEdit(const std::string& disassembly, std::string& edited, std::string* err) {
    try {
        std::vector<std::string> lines = SplitLines(disassembly);
        std::string h, x, y;
        static const std::regex reU0(R"(%(\d+) = call %dx\.types\.Handle @dx\.op\.createHandle\(i32 57, i8 1, i32 \d+, i32 0, i1 false\))");
        static const std::regex reTx(R"(%(\d+) = call i32 @dx\.op\.threadId\.i32\(i32 93, i32 0\))");
        static const std::regex reTy(R"(%(\d+) = call i32 @dx\.op\.threadId\.i32\(i32 93, i32 1\))");
        auto firstMatch = [&](const std::regex& re, std::string& out) {
            for (const auto& l : lines) {
                if (!Contains(l, "@dx.op.")) continue;
                std::smatch m;
                if (std::regex_search(l, m, re)) {
                    out = m[1].str();
                    return true;
                }
            }
            return false;
        };
        if (!(firstMatch(reU0, h) && firstMatch(reTx, x) && firstMatch(reTy, y))) die("no u0 handle or thread ids");
        const std::regex reStore("call void @dx\\.op\\.rawBufferStore\\.i32\\(i32 140, %dx\\.types\\.Handle %" + h +
                                 ", i32 [^,]+, i32 undef, (i32 [^,]+, i32 [^,]+, i32 [^,]+, i32 [^,]+), i8 15, i32 4\\)");
        int n = 0;
        for (auto& l : lines) {
            if (!Contains(l, "rawBufferStore.i32(i32 140")) continue;
            std::string res;
            auto begin = l.cbegin();
            std::smatch m;
            while (std::regex_search(begin, l.cend(), m, reStore)) {
                res.append(begin, m[0].first);
                res += std::format("call void @dx.op.textureStore.i32(i32 67, %dx.types.Handle %{}, i32 %{}, i32 %{}, i32 0, {}, i8 15)", h, x, y, m[1].str());
                ++n;
                begin = m[0].second;
            }
            res.append(begin, l.cend());
            l = res;
        }
        if (n != 2) die("expected 2 U-Net input stores, found " + std::to_string(n));
        const std::string leftover = "rawBufferStore.i32(i32 140, %dx.types.Handle %" + h + ",";
        for (const auto& l : lines)
            if (Contains(l, leftover.c_str())) die("another raw store to u0 is left");
        bool haveDecl = false;
        for (const auto& l : lines) haveDecl = haveDecl || Contains(l, "declare void @dx.op.textureStore.i32");
        if (!haveDecl) {
            for (size_t i = 0; i < lines.size(); ++i) {
                size_t at = lines[i].find("declare void @dx.op.rawBufferStore.i32");
                if (at == std::string::npos) continue;
                std::string ins = "declare void @dx.op.textureStore.i32(i32, %dx.types.Handle, i32, i32, i32, i32, i32, i32, i32, i8) #1";
                std::string rest = lines[i].substr(at);
                std::string head = lines[i].substr(0, at);
                lines[i] = head + ins;
                lines.insert(lines.begin() + i + 1, {"", rest});
                break;
            }
        }
        // u0's metadata: RWByteAddressBuffer (kind 11) -> RWTexture2DArray<uint4> (kind 7, element u32)
        const std::string mdLine = "= !{i32 0, %struct.RWByteAddressBuffer* undef, !\"\", i32 0, i32 0, i32 1, i32 11, i1 false, i1 false, i1 false, null}";
        int mdAt = -1;
        std::string mdName;
        for (size_t i = 0; i < lines.size() && mdAt < 0; ++i) {
            const std::string& l = lines[i];
            if (l.empty() || l[0] != '!') continue;
            size_t e = DigitsEnd(l, 1);
            if (e > 1 && l.compare(e, 1, " ") == 0 && l.compare(e + 1, mdLine.size(), mdLine) == 0 && l.size() == e + 1 + mdLine.size()) {
                mdAt = (int)i;
                mdName = l.substr(0, e);
            }
        }
        if (mdAt < 0) die("u0's metadata is not a RWByteAddressBuffer at u0");
        std::string ref;
        for (const auto& l : lines) {
            if (!l.empty() && l[0] == '!') {
                size_t e = DigitsEnd(l, 1);
                if (e > 1 && l.substr(e) == " = !{i32 0, i32 5}") {
                    ref = l.substr(0, e);
                    break;
                }
            }
        }
        if (ref.empty()) {
            int top = 0;
            for (const auto& l : lines) {
                if (l.empty() || l[0] != '!') continue;
                size_t e = DigitsEnd(l, 1);
                if (e > 1 && l.compare(e, 3, " = ") == 0) top = std::max(top, std::atoi(l.substr(1, e - 1).c_str()));
            }
            ref = "!" + std::to_string(top + 1);
            while (!lines.empty() && lines.back().empty()) lines.pop_back();
            lines.push_back(ref + " = !{i32 0, i32 5}");
        }
        lines[mdAt] = mdName + " = !{i32 0, %\"class.RWTexture2DArray<vector<unsigned int, 4> >\"* undef, !\"\", i32 0, i32 0, i32 1, i32 7, i1 false, i1 false, i1 false, " + ref + "}";
        edited = Join(lines, "\n") + "\n";
        return true;
    } catch (const std::exception& e) {
        if (err) *err = e.what();
        return false;
    }
}

// ---- clamp variant ------------------------------------------------------------------------------------------
// Rewrites a translated accumulator-clamped module for another clamp: the module's max.s32 / min.s32 pairs (four per
// accumulator set) against -2^(b-1) / 2^(b-1)-1 become the new width's bounds. The modules bake the width in (17 or
// 18 bits); the multi-frame model also has 19- and 20-bit layers, for which there is no module. Re-binding 17 -> 18
// reproduces the 18-bit module's kernel bit for bit, and the reverse (the offline checks prove it).
namespace {
// "<mnemonic> %r<d>, %r<d>, <number>;" occurrences: (position of the number, its length, the value)
struct ClampHit {
    size_t pos, len;
    long long value;
};
std::vector<ClampHit> FindClamps(const std::string& text, const char* mnemonic) {
    std::vector<ClampHit> hits;
    std::string key = std::string(mnemonic) + " %r";
    for (size_t at = text.find(key); at != std::string::npos; at = text.find(key, at + 1)) {
        size_t p = at + key.size(), e = DigitsEnd(text, p);
        if (e == p || text.compare(e, 4, ", %r") != 0) continue;
        p = e + 4;
        e = DigitsEnd(text, p);
        if (e == p || text.compare(e, 2, ", ") != 0) continue;
        p = e + 2;
        size_t q = p;
        if (q < text.size() && text[q] == '-') ++q;
        e = DigitsEnd(text, q);
        if (e == q || e >= text.size() || text[e] != ';') continue;
        hits.push_back({p, e - p, std::atoll(text.substr(p, e - p).c_str())});
    }
    return hits;
}
}  // namespace

bool ClampVariant(const std::string& ptx, int bits, std::string& out, std::string* err) {
    std::set<long long> found;
    auto mins = FindClamps(ptx, "min.s32");
    for (const auto& h : mins)
        if (h.value >= 0) found.insert(h.value);
    std::vector<long long> old;
    for (long long v : found)
        if (v >= 65535 && ((v + 1) & v) == 0) old.push_back(v);
    if (old.size() != 1) {
        if (err) *err = "expected one clamp width, found " + std::to_string(old.size()) + " upper bounds";
        return false;
    }
    long long hi = old[0], lo = -(hi + 1);
    auto maxs = FindClamps(ptx, "max.s32");
    size_t nHi = 0, nLo = 0;
    for (const auto& h : mins) nHi += h.value == hi;
    for (const auto& h : maxs) nLo += h.value == lo;
    if (nHi == 0 || nHi != nLo || nHi % 4) {
        if (err) *err = "expected the clamp as max/min pairs in fours, found " + std::to_string(nLo) + " + " + std::to_string(nHi);
        return false;
    }
    long long nhi = (1LL << (bits - 1)) - 1, nlo = -(1LL << (bits - 1));
    std::vector<std::pair<ClampHit, long long>> edits;
    for (const auto& h : mins)
        if (h.value == hi) edits.push_back({h, nhi});
    for (const auto& h : maxs)
        if (h.value == lo) edits.push_back({h, nlo});
    std::sort(edits.begin(), edits.end(), [](const auto& a, const auto& b) { return a.first.pos < b.first.pos; });
    std::string text;
    size_t at = 0;
    for (const auto& [h, nv] : edits) {
        text.append(ptx, at, h.pos - at);
        text += std::to_string(nv);
        at = h.pos + h.len;
    }
    text.append(ptx, at, std::string::npos);
    const std::string head = "// GENERATED by dxil2ptx.py";
    size_t hp = text.find(head);
    if (hp != std::string::npos)
        text.replace(hp, head.size(), std::format("// clamp_variant.py {} -> {} bits, from dxil2ptx.py", (int)std::bit_width((unsigned long long)hi) + 1, bits));
    out = std::move(text);
    return true;
}

bool TranslateEntry(const std::string& disassembly, const std::string& transform, const std::vector<std::string>& fmts,
                    const std::string& sourceName, std::string& ptx, std::string* err) {
    Options o;
    o.sourceName = sourceName;
    o.fmts = fmts;
    std::string asmText = disassembly;
    if (transform == "recon") {
        if (!ReconEdit(disassembly, asmText, err)) return false;
    } else if (transform != "plain" && !Starts(transform, "clamp:")) {
        if (err) *err = "unknown transform " + transform;
        return false;
    }
    std::string out;
    if (!Dxil2Ptx(asmText, o, out, err)) return false;
    if (Starts(transform, "clamp:")) return ClampVariant(out, std::atoi(transform.c_str() + 6), ptx, err);
    ptx = std::move(out);
    return true;
}

}  // namespace translate
