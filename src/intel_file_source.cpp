// intel_file_source.cpp - see intel_file_source.h. docs/PACKLESS.md has the overview of what is rebuilt and how.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "intel_file_source.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <fstream>
#include <map>

#include "intel_modules.h"
#include "recipe_json.h"

namespace packbuild {
namespace {

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// The encoding of a byte-wise immediate store to the stack frame, "mov byte [rsp+disp32], imm8": C6 84 24 <disp32> <imm8>,
// 8 bytes per data byte. A vector is a run of such stores whose displacements are consecutive.
constexpr uint8_t kStoreOp0 = 0xC6, kStoreOp1 = 0x84, kStoreOp2 = 0x24;
constexpr size_t kStoreLen = 8;
// The probe tag of a weight blob: the high half of a 64-bit finalizer hash (murmur3 fmix64) of 8 bytes of the blob, so the
// recipe holds a hash, never content. A short tag is only a filter; every candidate is confirmed by the SHA-256 of the
// whole blob.
inline uint32_t ProbeTag(uint64_t k) {
    k ^= k >> 33;
    k *= 0xff51afd7ed558ccdull;
    k ^= k >> 33;
    k *= 0xc4ceb9fe1a85ec53ull;
    k ^= k >> 33;
    return static_cast<uint32_t>(k >> 32);
}
// The folded bias of a layer (float32): aux * 128.0f * scale + bias, aux = the sum of the layer's int8 source weights.
constexpr float kFoldGain = 128.0f;

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(f);
}

struct Range {
    size_t begin, end;
};

// The raw data of the executable sections of a 64-bit PE image.
bool ExecSections(const uint8_t* d, size_t n, std::vector<Range>& out) {
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
    const uint32_t pe = Rd32(d + 0x3C);
    if (pe > n || n - pe < 28 || memcmp(d + pe, "PE\0\0", 4) != 0) return false;
    uint16_t nsec, optSize;
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&optSize, d + pe + 20, 2);
    const size_t tab = size_t(pe) + 24 + optSize;
    if (nsec == 0 || tab + size_t(nsec) * 40 > n) return false;
    for (uint16_t i = 0; i < nsec; ++i) {
        const uint8_t* s = d + tab + size_t(i) * 40;
        const uint32_t rawSize = Rd32(s + 16), rawPtr = Rd32(s + 20), flags = Rd32(s + 36);
        if (!(flags & 0x20000000u) || rawSize == 0 || rawPtr >= n) continue;  // IMAGE_SCN_MEM_EXECUTE
        out.push_back({rawPtr, std::min(n, size_t(rawPtr) + rawSize)});
    }
    return true;
}

float RdF(const uint8_t* p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}

struct Blob {
    size_t len = 0;
    intel_modules::Sha256 sha{};
    size_t probe = 0;
    uint32_t tag = 0;
    int firstUser = 0;           // an upload that uses it, for messages
    long long start = -1;        // position in the file once found
    int attempts = 0;            // SHA-256 checks spent on candidates (capped)
};

// The size formulas of the manifest's `set` lines: + - * / % ( ), numbers, OW OH RW RH AX AY AW AH, align(x,a) ceil(x) floor(x).
// Everything in double, as the reference tool does; `i` results truncate, `f` results round to float32.
class Expr {
public:
    Expr(const std::string& s, const double* vars) : s_(s), v_(vars) {}
    bool Eval(double& out) {
        ok_ = true;
        out = Sum();
        Ws();
        return ok_ && p_ == s_.size() && std::isfinite(out);
    }

private:
    void Ws() {
        while (p_ < s_.size() && s_[p_] == ' ') ++p_;
    }
    double Sum() {
        double a = Prod();
        for (;;) {
            Ws();
            if (p_ < s_.size() && (s_[p_] == '+' || s_[p_] == '-')) {
                const char op = s_[p_++];
                const double b = Prod();
                a = op == '+' ? a + b : a - b;
            } else {
                return a;
            }
        }
    }
    double Prod() {
        double a = Unary();
        for (;;) {
            Ws();
            if (p_ < s_.size() && (s_[p_] == '*' || s_[p_] == '/' || s_[p_] == '%')) {
                const char op = s_[p_++];
                const double b = Unary();
                if (op == '*') a *= b;
                else if (b == 0) ok_ = false;
                else if (op == '/') a /= b;
                else a = std::fmod(a, b);
            } else {
                return a;
            }
        }
    }
    double Unary() {
        Ws();
        if (p_ < s_.size() && s_[p_] == '-') {
            ++p_;
            return -Unary();
        }
        return Primary();
    }
    double Primary() {
        Ws();
        if (p_ >= s_.size()) { ok_ = false; return 0; }
        if (s_[p_] == '(') {
            ++p_;
            const double a = Sum();
            Ws();
            if (p_ < s_.size() && s_[p_] == ')') ++p_;
            else ok_ = false;
            return a;
        }
        if (isdigit(static_cast<unsigned char>(s_[p_])) || s_[p_] == '.') {
            char* e = nullptr;
            const double a = strtod(s_.c_str() + p_, &e);
            p_ = static_cast<size_t>(e - s_.c_str());
            return a;
        }
        size_t q = p_;
        while (q < s_.size() && isalnum(static_cast<unsigned char>(s_[q]))) ++q;
        const std::string name = s_.substr(p_, q - p_);
        p_ = q;
        static const char* kVars[] = {"OW", "OH", "RW", "RH", "AX", "AY", "AW", "AH"};
        for (int i = 0; i < 8; ++i)
            if (name == kVars[i]) return v_[i];
        Ws();
        if (p_ < s_.size() && s_[p_] == '(') {
            ++p_;
            const double a = Sum();
            double b = 0;
            Ws();
            if (p_ < s_.size() && s_[p_] == ',') {
                ++p_;
                b = Sum();
            }
            Ws();
            if (p_ < s_.size() && s_[p_] == ')') ++p_;
            else ok_ = false;
            if (name == "ceil") return std::ceil(a);
            if (name == "floor") return std::floor(a);
            if (name == "align") {
                const long long n = static_cast<long long>(a), m = static_cast<long long>(b);
                if (m <= 0 || n < 0) { ok_ = false; return 0; }
                return static_cast<double>((n + m - 1) / m * m);
            }
        }
        ok_ = false;
        return 0;
    }
    const std::string& s_;
    const double* v_;
    size_t p_ = 0;
    bool ok_ = true;
};

struct LayerRec {
    long long kind = 0, cout = 0, cin = 0, clamp = 0, sn = 0, sp = 0, mulPos = 0;
};

struct Layer {
    enum Kind { Conv, Head, Raw, Vector, Fold } kind = Raw;
    int blob = -1;
    long long co = 0, ci = 0, cop = 0, cip = 0, ct = 0, cit = 0;
    bool tDC = false;                  // conv tile order: false = [A][B][C][tap][D][o][i], true = [A][B][tap][D][C][o][i]
    long long run = -1;
    std::string form;                  // vector: "f32", "widen16" (widen to int32), "pad512" (widen if 16-bit, pad to 512 B)
    long long biasRun = -1, scaleRun = -1;
    int weights = 0;                   // fold: the upload whose weights give aux
    // from the recipe's data entry of this upload
    size_t uploadBytes = 0, bytes = 0;
    intel_modules::Sha256 sha{};
};

}  // namespace

struct IntelFileSource::Impl {
    std::vector<uint8_t> own;                   // the file as read here ...
    const std::vector<uint8_t>* ext = nullptr;  // ... or the bytes the caller already has
    const std::vector<uint8_t>& F() const { return ext ? *ext : own; }
    std::vector<Blob> blobs;
    std::vector<size_t> runStart, runLen;  // runs of immediate stores: into `runBytes`
    std::vector<uint8_t> runBytes;
    std::map<int, Layer> layers;
    std::string err;

    // ---- root constants
    struct ConstEntry {
        size_t dwords = 0;
        std::vector<int> mask;  // dwords the runtime patches every frame or that are never initialised: written 0, not hashed
        intel_modules::Sha256 sha{};
    };
    struct RcBlock {
        int layer = -1;  // position in the model's layer table (file order); -1 = no layer words
        bool head = false;
        std::map<int, uint32_t> plain;  // dwords that are plain numbers
    };
    struct SetRule {
        int dword;
        char kind;
        std::string expr;
    };
    std::string manifest;
    std::map<int, ConstEntry> consts;
    std::map<int, RcBlock> rcBlocks;
    std::map<int, std::vector<SetRule>> sets;  // by block (capture index)
    double sizeVars[8] = {};                   // OW OH RW RH AX AY AW AH
    long long rcChunk = 32, rcLayers = 0, rcFirstCin = 0;
    bool haveRc = false, tableDone = false, tableOk = false;
    std::string tableErr;
    std::vector<LayerRec> table;  // the model's layer descriptors in file order

    bool Fail(const std::string& s) {
        if (err.empty()) err = s;
        return false;
    }

    bool LoadRecipe(const std::vector<uint8_t>& parts) {
        Json root;
        if (!ParseJson(parts, root)) return Fail("the embedded recipe is not valid JSON");
        const Json* dec = root.Get("decode");
        const Json* data = root.Get("data");
        if (!dec || !data) return Fail("the embedded recipe has no weight decode description (older f5low.dll recipe)");
        const Json* bl = dec->Get("blobs");
        const Json* up = dec->Get("uploads");
        if (!bl || !up || bl->type != Json::Arr || up->type != Json::Obj) return Fail("the decode description is malformed");
        for (const Json& b : bl->arr) {
            Blob x;
            x.len = static_cast<size_t>(b.Num_("len"));
            x.probe = static_cast<size_t>(b.Num_("probe"));
            x.tag = static_cast<uint32_t>(b.Num_("tag"));
            if (!x.len || x.probe + 8 > x.len || !intel_modules::ParseHex(b.Str_("sha256"), x.sha))
                return Fail("the decode description has a bad blob entry");
            blobs.push_back(x);
        }
        std::map<int, const Json*> dataByOrd;
        for (const auto& kv : data->obj)
            if (kv.second.Str_("kind") == "intel_upload") dataByOrd[static_cast<int>(kv.second.Num_("ordinal"))] = &kv.second;
        for (const auto& kv : up->obj) {
            const int ord = atoi(kv.first.c_str());
            const Json& e = kv.second;
            Layer L;
            const std::string k = e.Str_("kind");
            if (k == "conv") L.kind = Layer::Conv;
            else if (k == "head") L.kind = Layer::Head;
            else if (k == "raw") L.kind = Layer::Raw;
            else if (k == "vector") L.kind = Layer::Vector;
            else if (k == "fold") L.kind = Layer::Fold;
            else return Fail("upload " + std::to_string(ord) + ": unknown kind " + k);
            L.blob = static_cast<int>(e.Num_("blob", -1));
            L.co = e.Num_("co");
            L.ci = e.Num_("ci");
            L.cop = e.Num_("cop");
            L.cip = e.Num_("cip");
            L.ct = e.Num_("ct");
            L.cit = e.Num_("cit");
            L.tDC = e.Str_("order") == "tDC";
            L.run = e.Num_("run", -1);
            L.form = e.Str_("form");
            L.biasRun = e.Num_("bias_run", -1);
            L.scaleRun = e.Num_("scale_run", -1);
            L.weights = static_cast<int>(e.Num_("weights"));
            auto d = dataByOrd.find(ord);
            if (d == dataByOrd.end()) return Fail("upload " + std::to_string(ord) + " has no entry in the recipe's data");
            L.uploadBytes = static_cast<size_t>(d->second->Num_("upload_bytes"));
            L.bytes = static_cast<size_t>(d->second->Num_("bytes"));
            if (!intel_modules::ParseHex(d->second->Str_("sha256"), L.sha)) return Fail("upload " + std::to_string(ord) + ": bad SHA-256 in the recipe");
            if (L.blob >= 0) {
                if (static_cast<size_t>(L.blob) >= blobs.size()) return Fail("upload " + std::to_string(ord) + ": blob index out of range");
                if (!blobs[L.blob].firstUser) blobs[L.blob].firstUser = ord;
            }
            layers[ord] = L;
        }
        // root constants: the blocks' hashes (masked where dwords are patched at run time) and our description
        for (const auto& kv : data->obj) {
            if (kv.second.Str_("kind") != "intel_const") continue;
            ConstEntry c;
            c.dwords = static_cast<size_t>(kv.second.Num_("dwords"));
            if (const Json* m = kv.second.Get("mask"))
                for (const Json& x : m->arr) c.mask.push_back(static_cast<int>(x.num));
            if (!intel_modules::ParseHex(kv.second.Str_("sha256"), c.sha)) return Fail("a root-constant block has a bad SHA-256 in the recipe");
            consts[static_cast<int>(kv.second.Num_("capture_index"))] = c;
        }
        if (const Json* rc = dec->Get("rc")) {
            const Json* sz = rc->Get("size");
            const Json* bl2 = rc->Get("blocks");
            if (!sz || sz->arr.size() != 4 || !bl2) return Fail("the root-constant description is malformed");
            sizeVars[0] = double(sz->arr[0].num);
            sizeVars[1] = double(sz->arr[1].num);
            sizeVars[2] = double(sz->arr[2].num);
            sizeVars[3] = double(sz->arr[3].num);
            sizeVars[4] = sizeVars[5] = 0;
            sizeVars[6] = sizeVars[0];
            sizeVars[7] = sizeVars[1];
            rcChunk = rc->Num_("chunk", 32);
            rcLayers = rc->Num_("layers");
            rcFirstCin = rc->Num_("first_cin");
            for (const auto& kv : bl2->obj) {
                RcBlock b;
                b.layer = static_cast<int>(kv.second.Num_("layer", -1));
                b.head = kv.second.Str_("form") == "head";
                // a plain number is {"v": value, "what": short meaning}; the meaning is for readers only
                if (const Json* pl = kv.second.Get("plain"))
                    for (const auto& d : pl->obj) b.plain[atoi(d.first.c_str())] = static_cast<uint32_t>(d.second.Num_("v"));
                rcBlocks[atoi(kv.first.c_str())] = b;
            }
            haveRc = true;
        }
        return true;
    }

    // The manifest's `cb <slot> data/rcNN.bin` binds and the `set <slot> <dword> <i|f> {formula}` lines that refer to a
    // slot. A `dispatch` line starts a new dispatch, so the slots are bound afresh.
    void ParseManifest() {
        std::map<int, int> slotFile;
        size_t at = 0;
        while (at < manifest.size()) {
            size_t eol = manifest.find('\n', at);
            if (eol == std::string::npos) eol = manifest.size();
            std::string line = manifest.substr(at, eol - at);
            at = eol + 1;
            while (!line.empty() && (line.back() == '\r' || line.back() == ' ')) line.pop_back();
            int slot, n, dw;
            char kind;
            if (line.compare(0, 9, "dispatch ") == 0) {
                slotFile.clear();
            } else if (sscanf_s(line.c_str(), " cb %d data/rc%d.bin", &slot, &n) == 2) {
                slotFile[slot] = n;
            } else if (sscanf_s(line.c_str(), " set %d %d %c {", &slot, &dw, &kind, 1) == 3) {
                const auto f = slotFile.find(slot);
                const size_t l = line.find('{'), r = line.rfind('}');
                if (f != slotFile.end() && l != std::string::npos && r != std::string::npos && r > l)
                    sets[f->second].push_back({dw, kind, line.substr(l + 1, r - l - 1)});
            }
        }
    }

    // The per-layer descriptor table, found without addresses: every function of the exception directory over 40000
    // bytes is walked for runs of exactly 12 immediate 32-bit stores into the call-argument area of the stack frame
    // (the descriptor words), closed by a call, and named by the "iNN" string a following lea refers to. The table of
    // the builder with `rcLayers` layers whose first layer has `rcFirstCin` input channels is the one we want.
    bool ScanLayerTable() {
        if (tableDone) return tableOk || Fail(tableErr);
        tableDone = true;
        const uint8_t* d = F().data();
        const size_t n = F().size();
        if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return Fail("not a PE image");
        const uint32_t pe = Rd32(d + 0x3C);
        if (pe > n || n - pe < 24 + 112 + 8 * 4) return Fail("not a PE image");
        uint16_t magic;
        memcpy(&magic, d + pe + 24, 2);
        if (magic != 0x20B) return Fail("not a 64-bit PE image");
        uint16_t nsec, optSize;
        memcpy(&nsec, d + pe + 6, 2);
        memcpy(&optSize, d + pe + 20, 2);
        const size_t tab = size_t(pe) + 24 + optSize;
        if (tab + size_t(nsec) * 40 > n) return Fail("bad section table");
        struct Sec {
            uint32_t va, vsize, raw, rawSize;
            bool exec;
        };
        std::vector<Sec> secs;
        for (uint16_t i = 0; i < nsec; ++i) {
            const uint8_t* s = d + tab + size_t(i) * 40;
            secs.push_back({Rd32(s + 12), Rd32(s + 8), Rd32(s + 20), Rd32(s + 16), (Rd32(s + 36) & 0x20000000u) != 0});
        }
        auto toOff = [&](uint32_t rva, size_t& off) {
            for (const Sec& s : secs)
                if (rva >= s.va && rva - s.va < s.rawSize && size_t(s.raw) + (rva - s.va) < n) {
                    off = size_t(s.raw) + (rva - s.va);
                    return true;
                }
            return false;
        };
        const uint32_t pdataRva = Rd32(d + pe + 24 + 112 + 3 * 8), pdataSize = Rd32(d + pe + 24 + 112 + 3 * 8 + 4);
        size_t pOff;
        if (!pdataRva || !toOff(pdataRva, pOff) || pOff + pdataSize > n) return Fail("this libxess_fg.dll has no function table");
        // The descriptor words sit in the call-argument window of the builder's frame at these displacements, highest
        // first = word order: [0] 0, [1] kind, [2] cout, [3] cin, [4] clamp bits, [5] sn, [6] sp, [7] mulPos, [8..11] 7,1,1,1.
        static const int kWordDisp[12] = {0x98, 0x90, 0x80, 0x78, 0x70, 0x68, 0x60, 0x58, 0x48, 0x40, 0x38, 0x30};
        std::vector<std::vector<LayerRec>> tables;
        for (size_t e = pOff; e + 12 <= pOff + pdataSize; e += 12) {
            const uint32_t b = Rd32(d + e), en = Rd32(d + e + 4);
            size_t fo;
            if (en <= b || en - b < 40000 || !toOff(b, fo) || fo + (en - b) > n) continue;
            const uint8_t* f = d + fo;
            const size_t len = en - b;
            std::vector<LayerRec> layersHere;
            uint32_t byDisp[14] = {};  // store value at argument-window displacement 0x30 + 8*k
            bool seen[14] = {};
            bool dup = false;
            int nst = 0;
            bool pending = false;
            LayerRec pend;
            size_t i = 0;
            while (i + 11 <= len) {
                const uint8_t* p = f + i;
                // mov dword [rsp+disp8/disp32], imm32
                if (p[0] == 0xC7 && p[2] == 0x24 && (p[1] == 0x44 || p[1] == 0x84)) {
                    int32_t disp;
                    uint32_t imm;
                    size_t sl;
                    if (p[1] == 0x44) {
                        disp = static_cast<int8_t>(p[3]);
                        memcpy(&imm, p + 4, 4);
                        sl = 8;
                    } else {
                        memcpy(&disp, p + 3, 4);
                        memcpy(&imm, p + 7, 4);
                        sl = 11;
                    }
                    if (disp >= 0x30 && disp <= 0x98 && !(disp & 7)) {  // the call-argument area
                        const int k = (disp - 0x30) / 8;
                        dup = dup || seen[k];
                        seen[k] = true;
                        byDisp[k] = imm;
                        ++nst;
                    }
                    i += sl;
                    continue;
                }
                // lea reg, [rip+disp32] that points at a layer's "i<digits>" string: only a delimiter, the name is not kept
                if (pending && (p[0] == 0x48 || p[0] == 0x4C) && p[1] == 0x8D && (p[2] & 0xC7) == 0x05) {
                    int32_t disp;
                    memcpy(&disp, p + 3, 4);
                    size_t so;
                    if (toOff(static_cast<uint32_t>(int64_t(b) + int64_t(i) + 7 + disp), so)) {
                        const char* s = reinterpret_cast<const char*>(d + so);
                        size_t k = 0;
                        while (so + k < n && k < 12 && s[k]) ++k;
                        if (k >= 2 && k < 12 && s[0] == 'i') {
                            size_t j = 1;
                            while (j < k && isdigit(static_cast<unsigned char>(s[j]))) ++j;
                            if (j == k) {
                                layersHere.push_back(pend);
                                pending = false;
                                i += 7;
                                continue;
                            }
                        }
                    }
                }
                // a call closes the run: exactly 12 distinct argument-area stores make a descriptor
                // (an E8 byte inside another instruction's displacement is ruled out by its target: it must be code)
                bool isCall = false;
                if (p[0] == 0xE8) {
                    int32_t rel;
                    memcpy(&rel, p + 1, 4);
                    const int64_t t = int64_t(b) + int64_t(i) + 5 + rel;
                    for (const Sec& s : secs)
                        if (s.exec && t >= s.va && t < int64_t(s.va) + s.vsize) isCall = true;
                } else if (p[0] == 0xFF && p[1] == 0x15) {
                    int32_t rel;
                    memcpy(&rel, p + 2, 4);
                    size_t tmp;
                    isCall = toOff(static_cast<uint32_t>(int64_t(b) + int64_t(i) + 6 + rel), tmp);
                }
                if (isCall) {
                    if (nst == 12 && !dup) {
                        bool all = true;
                        uint32_t w[12];
                        for (int a = 0; a < 12; ++a) {
                            const int k = (kWordDisp[a] - 0x30) / 8;
                            all = all && seen[k];
                            w[a] = byDisp[k];
                        }
                        if (all) {
                            pend.kind = w[1];
                            pend.cout = w[2];
                            pend.cin = w[3];
                            pend.clamp = w[4];
                            pend.sn = w[5];
                            pend.sp = w[6];
                            pend.mulPos = w[7];
                            pending = true;
                        }
                    }
                    nst = 0;
                    dup = false;
                    memset(seen, 0, sizeof seen);
                    i += p[0] == 0xE8 ? 5 : 6;
                    continue;
                }
                ++i;
            }
            if (!layersHere.empty()) tables.push_back(std::move(layersHere));
        }
        int hits = 0;
        for (const auto& t : tables) {
            if (static_cast<long long>(t.size()) == rcLayers && t[0].cin == rcFirstCin) {
                ++hits;
                table = t;
            }
        }
        if (hits != 1) {
            tableErr = "the layer table of the multi-frame model was not found in this libxess_fg.dll (" + std::to_string(tables.size()) + " builders, " + std::to_string(hits) + " fit:";
            for (const auto& t : tables) tableErr += " " + std::to_string(t.size()) + "/" + std::to_string(t[0].cin);
            tableErr += ")";
            return Fail(tableErr);
        }
        tableOk = true;
        return true;
    }

    bool RootBlock(int index, std::vector<uint8_t>& out) {
        const std::string who = "root constants of dispatch #" + std::to_string(index);
        auto ce = consts.find(index);
        if (!haveRc || ce == consts.end()) return Fail(who + ": the recipe has no description");
        if (!ScanLayerTable()) return false;
        if (sets.empty()) ParseManifest();
        const ConstEntry& c = ce->second;
        std::vector<uint32_t> dw(c.dwords, 0u);
        auto put = [&](long long k, uint32_t v) {
            if (k >= 0 && static_cast<size_t>(k) < dw.size()) dw[static_cast<size_t>(k)] = v;
        };
        auto st = sets.find(index);
        if (st != sets.end())
            for (const SetRule& r : st->second) {
                std::string e = r.expr;
                double v;
                if (!Expr(e, sizeVars).Eval(v)) return Fail(who + ": cannot evaluate {" + r.expr + "}");
                if (r.kind == 'f') {
                    const float fv = static_cast<float>(v);
                    uint32_t bits;
                    memcpy(&bits, &fv, 4);
                    put(r.dword, bits);
                } else {
                    // the runtime's rounding of a size (xefg_pack.h): floor(v + 1e-6), finite, not negative
                    if (v < 0 || v > 4294967295.0) return Fail(who + ": {" + r.expr + "} is out of range");
                    put(r.dword, static_cast<uint32_t>(static_cast<int64_t>(std::floor(v + 1e-6))));
                }
            }
        auto bk = rcBlocks.find(index);
        if (bk != rcBlocks.end()) {
            const RcBlock& b = bk->second;
            if (b.layer >= 0) {
                if (static_cast<size_t>(b.layer) >= table.size()) return Fail(who + ": layer " + std::to_string(b.layer) + " is not in this libxess_fg.dll's table");
                const LayerRec& L = table[static_cast<size_t>(b.layer)];
                if (b.head) {
                    put(7, static_cast<uint32_t>(L.mulPos));
                    put(8, static_cast<uint32_t>(L.sp));
                } else {
                    if (!b.plain.count(8)) put(8, static_cast<uint32_t>(std::min(rcChunk, L.cin)));
                    put(9, static_cast<uint32_t>(L.mulPos));
                    put(10, static_cast<uint32_t>((L.sn << 8) | L.sp));
                }
            }
            for (const auto& kv : b.plain) put(kv.first, kv.second);
        }
        for (int m : c.mask) put(m, 0);
        out.resize(dw.size() * 4);
        if (!dw.empty()) memcpy(out.data(), dw.data(), out.size());
        intel_modules::Sha256 h{};
        if (!intel_modules::HashBytes(out.data(), out.size(), h) || h != c.sha)
            return Fail(who + " derived from the libxess_fg.dll do not match the recipe's SHA-256");
        return true;
    }

    // Every weight blob is found by its probe tag, then confirmed by the SHA-256 of the whole blob. The blobs lie back to
    // back in the file in layer order, but nothing here relies on that or on where they are: a copy shifted by a
    // few bytes (or a second copy of the same weights) cannot pass the hash unless its bytes are the same.
    bool LocateBlobs() {
        const size_t n = F().size();
        std::vector<uint8_t> bitmap(8192, 0);
        for (const Blob& b : blobs) bitmap[(b.tag >> 16) >> 3] |= uint8_t(1u << ((b.tag >> 16) & 7));
        size_t left = blobs.size();
        const uint8_t* d = F().data();
        for (size_t i = 0; i + 8 <= n && left; ++i) {
            uint64_t v;
            memcpy(&v, d + i, 8);
            const uint32_t h = ProbeTag(v);
            const uint32_t slot = h >> 16;
            if (!(bitmap[slot >> 3] & (1u << (slot & 7)))) continue;
            for (Blob& b : blobs) {
                if (b.tag != h || b.start >= 0 || i < b.probe) continue;
                const size_t s = i - b.probe;
                if (s + b.len > n || b.attempts >= 16) continue;  // a probe window that is not distinctive cannot flood the scan
                ++b.attempts;
                intel_modules::Sha256 got{};
                if (!intel_modules::HashBytes(d + s, b.len, got) || got != b.sha) continue;
                b.start = static_cast<long long>(s);
                --left;
            }
        }
        for (const Blob& b : blobs)
            if (b.start < 0)
                return Fail("weight blob of upload " + std::to_string(b.firstUser) + " (" + std::to_string(b.len) +
                            " bytes) is not in this libxess_fg.dll");
        return true;
    }

    // Runs of byte-wise immediate stores in the executable sections, in file order. A run goes on while the next 8 bytes
    // are another store whose displacement is one more than the last one.
    void ScanRuns() {
        std::vector<Range> secs;
        if (!ExecSections(F().data(), F().size(), secs)) return;
        const uint8_t* d = F().data();
        for (const Range& r : secs) {
            bool open = false;
            int32_t next = 0;
            auto close = [&] {
                if (!open) return;
                runLen.back() = runBytes.size() - runStart.back();
                open = false;
            };
            size_t i = r.begin;
            while (r.end >= 8 && i < r.end - kStoreLen) {
                if (d[i] == kStoreOp0 && d[i + 1] == kStoreOp1 && d[i + 2] == kStoreOp2) {
                    int32_t disp;
                    memcpy(&disp, d + i + 3, 4);
                    if (open && disp == next) {
                        runBytes.push_back(d[i + 7]);
                        ++next;
                    } else {
                        close();
                        runStart.push_back(runBytes.size());
                        runLen.push_back(0);
                        runBytes.push_back(d[i + 7]);
                        open = true;
                        next = disp + 1;
                    }
                    i += kStoreLen;
                    continue;
                }
                close();
                // skip ahead to the next possible opcode byte
                const void* q = memchr(d + i + 1, kStoreOp0, r.end - kStoreLen - i - 1);
                i = q ? static_cast<size_t>(static_cast<const uint8_t*>(q) - d) : r.end;
            }
            close();
        }
    }

    const uint8_t* BlobData(int b) const { return F().data() + blobs[static_cast<size_t>(b)].start; }

    bool Run(long long r, const uint8_t*& p, size_t& n, const std::string& who) {
        if (r < 0 || static_cast<size_t>(r) >= runStart.size()) return Fail(who + ": the file has no vector run " + std::to_string(r) + " (only " + std::to_string(runStart.size()) + ")");
        p = runBytes.data() + runStart[static_cast<size_t>(r)];
        n = runLen[static_cast<size_t>(r)];
        return true;
    }

    bool Decode(int ord, const Layer& L, std::vector<uint8_t>& out) {
        const std::string who = "upload " + std::to_string(ord);
        out.clear();
        switch (L.kind) {
            case Layer::Raw: {
                const Blob& b = blobs[static_cast<size_t>(L.blob)];
                out.assign(BlobData(L.blob), BlobData(L.blob) + b.len);
                return true;
            }
            case Layer::Conv: {
                const Blob& b = blobs[static_cast<size_t>(L.blob)];
                if (L.ct <= 0 || L.cit <= 0 || L.co <= 0 || L.ci <= 0 || L.cop % L.ct || L.cip % L.cit || L.cop % 4 || L.cip % 4 ||
                    L.co > L.cop || L.ci > L.cip || b.len != size_t(L.co * L.ci * 9))
                    return Fail(who + ": the conv description does not fit its blob");
                const uint8_t* src = BlobData(L.blob);
                out.resize(size_t(L.cop * L.cip * 9));
                size_t w = 0;
                const long long A = L.cop / L.ct, B = L.cip / L.cit, C = L.ct / 4, D = L.cit / 4;
                auto at = [&](long long a, long long bb, long long c, long long t, long long dd, long long o, long long i) -> uint8_t {
                    const long long co = a * L.ct + c * 4 + o, ci = bb * L.cit + dd * 4 + i;
                    return (co < L.co && ci < L.ci) ? src[(co * L.ci + ci) * 9 + t] : uint8_t(0);
                };
                for (long long a = 0; a < A; ++a)
                    for (long long bb = 0; bb < B; ++bb) {
                        if (!L.tDC) {
                            for (long long c = 0; c < C; ++c)
                                for (long long t = 0; t < 9; ++t)
                                    for (long long dd = 0; dd < D; ++dd)
                                        for (long long o = 0; o < 4; ++o)
                                            for (long long i = 0; i < 4; ++i) out[w++] = at(a, bb, c, t, dd, o, i);
                        } else {
                            for (long long t = 0; t < 9; ++t)
                                for (long long dd = 0; dd < D; ++dd)
                                    for (long long c = 0; c < C; ++c)
                                        for (long long o = 0; o < 4; ++o)
                                            for (long long i = 0; i < 4; ++i) out[w++] = at(a, bb, c, t, dd, o, i);
                        }
                    }
                return true;
            }
            case Layer::Head: {
                // 1x1 layer: [cin/4][cout][4]. The padded columns of a row read on into the next row (the flat index is not
                // masked); past the end of the vector they read zero.
                const Blob& b = blobs[static_cast<size_t>(L.blob)];
                if (L.co <= 0 || L.ci <= 0 || L.cip < L.ci || L.cip % 4 || b.len != size_t(L.co * L.ci))
                    return Fail(who + ": the head description does not fit its blob");
                const uint8_t* src = BlobData(L.blob);
                out.resize(size_t(L.co * L.cip));
                size_t w = 0;
                for (long long r = 0; r < L.cip / 4; ++r)
                    for (long long c = 0; c < L.co; ++c)
                        for (long long j = 0; j < 4; ++j) {
                            const long long idx = c * L.ci + r * 4 + j;
                            out[w++] = idx < static_cast<long long>(b.len) ? src[idx] : uint8_t(0);
                        }
                return true;
            }
            case Layer::Vector: {
                const uint8_t* p;
                size_t n;
                if (!Run(L.run, p, n, who)) return false;
                if (L.form == "f32") {
                    out.assign(p, p + n);
                    return true;
                }
                if (L.form == "widen16" || L.form == "pad512") {
                    // 16-bit values widened to 32-bit; the stem vectors come as 32-bit (448 B) or 16-bit (224 B) and are
                    // zero-padded to 512 B
                    const bool wide = L.form == "widen16" || n == 224;
                    if (wide) {
                        if (n % 2) return Fail(who + ": run " + std::to_string(L.run) + " has an odd length");
                        out.resize(n * 2);
                        for (size_t i = 0; i < n / 2; ++i) {
                            int16_t v;
                            memcpy(&v, p + 2 * i, 2);
                            const int32_t w = v;
                            memcpy(out.data() + 4 * i, &w, 4);
                        }
                    } else {
                        out.assign(p, p + n);
                    }
                    if (L.form == "pad512") {
                        if (out.size() > 512) return Fail(who + ": run " + std::to_string(L.run) + " is longer than 512 bytes");
                        out.resize(512, 0);
                    }
                    return true;
                }
                return Fail(who + ": unknown vector form " + L.form);
            }
            case Layer::Fold: {
                auto wl = layers.find(L.weights);
                if (wl == layers.end() || (wl->second.kind != Layer::Conv && wl->second.kind != Layer::Head))
                    return Fail(who + ": its weights (upload " + std::to_string(L.weights) + ") are not a conv or head layer");
                const Layer& W = wl->second;
                const uint8_t *bp, *sp;
                size_t bn, sn;
                if (!Run(L.biasRun, bp, bn, who) || !Run(L.scaleRun, sp, sn, who)) return false;
                const long long co = W.co;
                if (bn != size_t(co) * 4 || sn != size_t(co) * 4) return Fail(who + ": the bias/scale runs do not fit the layer's " + std::to_string(co) + " channels");
                const uint8_t* src = BlobData(W.blob);
                const long long len = static_cast<long long>(blobs[static_cast<size_t>(W.blob)].len);
                out.resize(size_t(co) * 4);
                for (long long c = 0; c < co; ++c) {
                    long long sum = 0;  // int8 weights of the channel over the PADDED input channels (head: reads on, as the tiling does)
                    if (W.kind == Layer::Conv) {
                        for (long long k = 0; k < W.ci * 9; ++k) sum += static_cast<int8_t>(src[c * W.ci * 9 + k]);
                    } else {
                        for (long long x = 0; x < W.cip; ++x) {
                            const long long idx = c * W.ci + x;
                            if (idx < len) sum += static_cast<int8_t>(src[idx]);
                        }
                    }
                    // each step rounded to float32 on its own (no fused multiply-add)
                    volatile float aux = static_cast<float>(sum);
                    volatile float scaled = aux * kFoldGain;
                    volatile float prod = scaled * RdF(sp + 4 * c);
                    volatile float res = prod + RdF(bp + 4 * c);
                    const float r = res;
                    memcpy(out.data() + 4 * c, &r, 4);
                }
                return true;
            }
        }
        return false;
    }
};

IntelFileSource::IntelFileSource(const std::wstring& libxessPath, const std::vector<uint8_t>& partsJson,
                                 const std::vector<uint8_t>& manifestTmpl)
    : impl_(new Impl), path_(libxessPath), parts_(partsJson) {
    impl_->manifest.assign(manifestTmpl.begin(), manifestTmpl.end());
}

IntelFileSource::~IntelFileSource() = default;

void IntelFileSource::UseLibraryBytes(const std::vector<uint8_t>* bytes) {
    if (!prepared_) impl_->ext = bytes;
}

bool IntelFileSource::Prepare() {
    if (prepared_) return ok_;
    prepared_ = true;
    Impl& m = *impl_;
    auto t = Clock::now();
    if (!m.LoadRecipe(parts_)) {
        problem_ = m.err;
        return false;
    }
    if (!m.ext && !ReadFileAll(path_, m.own)) {
        problem_ = "cannot read " + Narrow(path_);
        return false;
    }
    stats_.readMs = MsSince(t);
    t = Clock::now();
    if (!m.LocateBlobs()) {
        problem_ = m.err + " (" + Narrow(path_) + ")";
        return false;
    }
    stats_.blobsMs = MsSince(t);
    t = Clock::now();
    m.ScanRuns();
    stats_.runsMs = MsSince(t);
    stats_.blobs = m.blobs.size();
    stats_.runs = m.runStart.size();
    ok_ = true;
    return true;
}

bool IntelFileSource::Upload(int ordinal, std::vector<uint8_t>& out) {
    if (!Prepare()) return false;
    Impl& m = *impl_;
    auto it = m.layers.find(ordinal);
    if (it == m.layers.end()) {
        problem_ = "the recipe has no decode description for upload " + std::to_string(ordinal);
        return false;
    }
    const auto t0 = Clock::now();
    m.err.clear();
    const Layer& L = it->second;
    if (!m.Decode(ordinal, L, out)) {
        problem_ = m.err;
        return false;
    }
    if (out.size() != L.uploadBytes) {
        problem_ = "upload " + std::to_string(ordinal) + " decodes to " + std::to_string(out.size()) + " bytes, the recipe wants " + std::to_string(L.uploadBytes);
        return false;
    }
    // the same check the builder makes: the recipe's SHA-256 is of the upload padded with zeros to `bytes`
    std::vector<uint8_t> padded(out);
    padded.resize(L.bytes, 0);
    intel_modules::Sha256 h{};
    if (!intel_modules::HashBytes(padded.data(), padded.size(), h) || h != L.sha) {
        problem_ = "upload " + std::to_string(ordinal) + " rebuilt from " + Narrow(path_) + " does not match the recipe's SHA-256 (a libxess_fg build this recipe does not know?)";
        return false;
    }
    ++stats_.uploads;
    stats_.decodeMs += MsSince(t0);
    return true;
}

bool IntelFileSource::RootConstants(int index, std::vector<uint8_t>& out) {
    // An explicit development override (a PSO capture, F5LOW_DEV_CAP) wins; otherwise the blocks are derived from the file.
    if (rootSource_) {
        if (rootSource_->RootConstants(index, out)) return true;
        problem_ = rootSource_->Problem();
        return false;
    }
    if (!Prepare()) return false;
    const auto t0 = Clock::now();
    impl_->err.clear();
    const bool ok = impl_->RootBlock(index, out);
    stats_.rcMs += MsSince(t0);
    if (!ok) {
        problem_ = impl_->err;
        return false;
    }
    ++stats_.rcBlocks;
    return true;
}

}  // namespace packbuild
