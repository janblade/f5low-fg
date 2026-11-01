// pack_build.cpp - see pack_build.h. The Python reference of BuildPack is tools/recipe/build_pack.py (neutral names);
// tools/recipe/compare_pack.py checks that a pack built here equals its output byte for byte.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "pack_build.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <set>

#include "dxc_disasm.h"
#include "intel_modules.h"
#include "translate.h"

namespace fs = std::filesystem;

namespace packbuild {
namespace {

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

bool ReadAll(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n < 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(f) || n == 0;
}

bool WriteAll(const fs::path& p, const void* data, size_t n) {
    std::error_code ec;
    fs::create_directories(p.parent_path(), ec);
    std::ofstream f(p, std::ios::binary | std::ios::trunc);
    if (!f) return false;
    f.write(static_cast<const char*>(data), static_cast<std::streamsize>(n));
    return static_cast<bool>(f);
}

std::string Sha256Hex(const void* data, size_t n) {
    intel_modules::Sha256 h{};
    if (!intel_modules::HashBytes(data, n, h)) return {};
    return intel_modules::ToHex(h);
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

// ---- JSON (parts.json: objects, arrays, strings, integers) ---------------------------------------------------------
struct Json {
    enum Type { Null, Num, Str, Arr, Obj } type = Null;
    long long num = 0;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;  // in file order

    const Json* Get(const char* key) const {
        for (const auto& kv : obj)
            if (kv.first == key) return &kv.second;
        return nullptr;
    }
    std::string Str_(const char* key) const {
        const Json* j = Get(key);
        return j && j->type == Str ? j->str : std::string();
    }
    long long Num_(const char* key, long long def = 0) const {
        const Json* j = Get(key);
        return j && j->type == Num ? j->num : def;
    }
};

class JsonParser {
public:
    JsonParser(const char* p, size_t n) : p_(p), e_(p + n) {}
    bool Parse(Json& out) { return Value(out, 0) && (Ws(), p_ == e_); }

private:
    void Ws() {
        while (p_ < e_ && (*p_ == ' ' || *p_ == '\t' || *p_ == '\n' || *p_ == '\r')) ++p_;
    }
    bool String(std::string& s) {
        if (p_ >= e_ || *p_ != '"') return false;
        ++p_;
        while (p_ < e_ && *p_ != '"') {
            char c = *p_++;
            if (c == '\\') {
                if (p_ >= e_) return false;
                char x = *p_++;
                switch (x) {
                    case 'n': c = '\n'; break;
                    case 't': c = '\t'; break;
                    case 'r': c = '\r'; break;
                    case 'b': c = '\b'; break;
                    case 'f': c = '\f'; break;
                    case 'u': {
                        if (e_ - p_ < 4) return false;
                        unsigned v = 0;
                        for (int i = 0; i < 4; ++i) {
                            char h = *p_++;
                            v = v * 16 + (h >= 'a' ? h - 'a' + 10 : h >= 'A' ? h - 'A' + 10 : h - '0');
                        }
                        c = v < 0x80 ? static_cast<char>(v) : '?';
                        break;
                    }
                    default: c = x;  // " \ /
                }
            }
            s += c;
        }
        if (p_ >= e_) return false;
        ++p_;
        return true;
    }
    bool Value(Json& j, int depth) {
        if (depth > 16) return false;
        Ws();
        if (p_ >= e_) return false;
        if (*p_ == '{') {
            ++p_;
            j.type = Json::Obj;
            Ws();
            if (p_ < e_ && *p_ == '}') return ++p_, true;
            for (;;) {
                Ws();
                std::string k;
                if (!String(k)) return false;
                Ws();
                if (p_ >= e_ || *p_++ != ':') return false;
                Json v;
                if (!Value(v, depth + 1)) return false;
                j.obj.emplace_back(std::move(k), std::move(v));
                Ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') { ++p_; continue; }
                return *p_++ == '}';
            }
        }
        if (*p_ == '[') {
            ++p_;
            j.type = Json::Arr;
            Ws();
            if (p_ < e_ && *p_ == ']') return ++p_, true;
            for (;;) {
                Json v;
                if (!Value(v, depth + 1)) return false;
                j.arr.push_back(std::move(v));
                Ws();
                if (p_ >= e_) return false;
                if (*p_ == ',') { ++p_; continue; }
                return *p_++ == ']';
            }
        }
        if (*p_ == '"') {
            j.type = Json::Str;
            return String(j.str);
        }
        if (*p_ == '-' || (*p_ >= '0' && *p_ <= '9')) {
            const bool neg = *p_ == '-';
            if (neg) ++p_;
            if (p_ >= e_ || *p_ < '0' || *p_ > '9') return false;
            long long v = 0;
            while (p_ < e_ && *p_ >= '0' && *p_ <= '9') v = v * 10 + (*p_++ - '0');
            j.type = Json::Num;
            j.num = neg ? -v : v;
            return true;
        }
        return false;  // true/false/null/floats: not in a recipe
    }
    const char* p_;
    const char* e_;
};

bool ParseJson(const std::vector<uint8_t>& b, Json& j) {
    return JsonParser(reinterpret_cast<const char*>(b.data()), b.size()).Parse(j);
}

std::string Hex8(const std::string& h) { return h.substr(0, 8); }

// Manifest: file names written as @<name>.ptx / @<name>.dxbc in the template become the names; any other '@' is an error.
bool ResolveManifest(const std::string& tmplIn, const std::set<std::string>& names, std::string& out, std::string* err) {
    std::string tmpl;  // text-mode read of the reference tool: CR LF and lone CR become LF
    for (size_t i = 0; i < tmplIn.size(); ++i) {
        if (tmplIn[i] == '\r') {
            tmpl += '\n';
            if (i + 1 < tmplIn.size() && tmplIn[i + 1] == '\n') ++i;
        } else {
            tmpl += tmplIn[i];
        }
    }
    out.clear();
    for (size_t i = 0; i < tmpl.size(); ++i) {
        if (tmpl[i] != '@') {
            out += tmpl[i];
            continue;
        }
        size_t j = i + 1;
        while (j < tmpl.size() && (isalnum(static_cast<unsigned char>(tmpl[j])) || tmpl[j] == '_')) ++j;
        size_t k = std::string::npos;
        if (j > i + 1 && j < tmpl.size() && tmpl[j] == '.') {
            if (tmpl.compare(j + 1, 3, "ptx") == 0) k = j + 4;
            else if (tmpl.compare(j + 1, 4, "dxbc") == 0) k = j + 5;
        }
        if (k == std::string::npos) {
            if (err) *err = "a placeholder is left in the manifest";
            return false;
        }
        const std::string name = tmpl.substr(i + 1, k - i - 1);
        if (!names.count(name)) {
            if (err) *err = "the manifest names " + name + ", which the recipe does not make";
            return false;
        }
        out += name;
        i = k - 1;
    }
    return true;
}

struct Builder {
    const Bundle& own;
    IntelDataSource& data;
    const std::wstring& dxcPath;
    Report& rep;
    intel_modules::Library lib;
    dxc_disasm::Disassembler dxc;
    bool dxcTried = false;
    std::map<std::string, std::string> dis;   // module hash -> disassembly
    std::map<std::string, std::string> made;  // translation key -> PTX text
    std::string err;

    Builder(const Bundle& o, IntelDataSource& d, const std::wstring& dp, Report& r) : own(o), data(d), dxcPath(dp), rep(r) {}

    bool Fail(const std::string& what) {
        if (err.empty()) err = what;
        return false;
    }

    bool NeedDxc() {
        if (dxc.Loaded()) return true;
        if (dxcTried) return false;
        dxcTried = true;
        std::string why;
        std::error_code ec;
        if (!fs::exists(dxcPath, ec)) return Fail("dxcompiler.dll not found (" + Narrow(dxcPath) + ")");
        if (!dxc.Load(dxcPath, &why)) return Fail(why);
        rep.dxcVersion = dxc.Version();
        return true;
    }

    bool Translate(const std::string& name, const Json& part, std::string& ptx) {
        const std::string mod = part.Str_("module_sha256"), tr = part.Str_("transform");
        std::vector<std::string> fmts;
        if (const Json* f = part.Get("fmts"))
            for (const Json& x : f->arr) fmts.push_back(x.str);
        const std::string asmName = name.substr(0, name.rfind('.')) + ".dxasm";
        std::string key = mod + "|" + tr + "|" + asmName;
        for (const auto& f : fmts) key += "|" + f;
        if (auto it = made.find(key); it != made.end()) {
            ptx = it->second;
            return true;
        }
        const auto t0 = Clock::now();
        const std::vector<uint8_t>* m = lib.FindHex(mod);
        if (!m) return Fail(name + ": module " + Hex8(mod) + " not found in libxess_fg.dll");
        if (!NeedDxc()) return false;
        auto d = dis.find(mod);
        if (d == dis.end()) {
            std::string text, why;
            if (!dxc.Disassemble(m->data(), m->size(), text, &why)) return Fail(name + ": " + why);
            d = dis.emplace(mod, std::move(text)).first;
        }
        std::string why;
        if (!translate::TranslateEntry(d->second, tr, fmts, asmName, ptx, &why))
            return Fail(name + ": translation of module " + Hex8(mod) + ": " + why);
        made[key] = ptx;
        ++rep.translated;
        rep.translateMs += MsSince(t0);
        return true;
    }

    bool WriteFile_(const fs::path& p, const void* d, size_t n, const std::string& what) {
        if (!WriteAll(p, d, n)) return Fail(what + ": cannot write " + Narrow(p.wstring()));
        ++rep.files;
        return true;
    }

    bool BuildOne(const std::string& prefix, const fs::path& out) {
        auto get = [&](const std::string& n) -> const std::vector<uint8_t>* {
            auto it = own.find(prefix + n);
            return it == own.end() ? nullptr : &it->second;
        };
        const auto* pj = get("parts.json");
        const auto* tm = get("manifest.tmpl");
        if (!pj || !tm) return Fail("the embedded recipe " + prefix + " is missing (this f5low.dll was built without it)");
        Json parts;
        if (!ParseJson(*pj, parts)) return Fail(prefix + "parts.json is not valid");
        const Json* files = parts.Get("files");
        const Json* dataJ = parts.Get("data");
        if (!files || !dataJ) return Fail(prefix + "parts.json has no files/data");

        std::set<std::string> names;
        for (const auto& kv : files->obj) {
            const std::string& n = kv.first;
            const Json& part = kv.second;
            const std::string kind = part.Str_("kind");
            names.insert(n);
            std::string ptx;
            if (kind == "own_gen" || kind == "own_tpl") {
                const auto* b = get(n);
                if (!b) return Fail(n + ": our kernel is not in this f5low.dll");
                if (!WriteFile_(out / n, b->data(), b->size(), n)) return false;
            } else if (kind == "intel_native") {
                const std::string mod = part.Str_("module_sha256");
                const std::vector<uint8_t>* m = lib.FindHex(mod);
                if (!m) return Fail(n + ": module " + Hex8(mod) + " not found in libxess_fg.dll");
                ++rep.native;
                if (!WriteFile_(out / n, m->data(), m->size(), n)) return false;
            } else if (kind == "intel_xlate") {
                if (!Translate(n, part, ptx)) return false;
                if (!WriteFile_(out / n, ptx.data(), ptx.size(), n)) return false;
            } else {
                return Fail(n + ": kind " + kind + " cannot be built");
            }
        }

        std::string text, why;
        if (!ResolveManifest(std::string(tm->begin(), tm->end()), names, text, &why)) return Fail(prefix + "manifest: " + why);
        if (!WriteFile_(out / "manifest.txt", text.data(), text.size(), "manifest.txt")) return false;

        for (const auto& kv : dataJ->obj) {
            const std::string& rel = kv.first;
            const Json& d = kv.second;
            const std::string kind = d.Str_("kind");
            if (kind == "capture_input") continue;  // a game fills these every frame; the DLL reads a missing one as zero
            std::vector<uint8_t> blob;
            if (kind == "intel_upload") {
                const int ord = static_cast<int>(d.Num_("ordinal"));
                std::vector<uint8_t> up;
                if (!data.Upload(ord, up))
                    return Fail(rel + ": weight upload " + std::to_string(ord) + " is not available" +
                                (data.Problem().empty() ? "" : " (" + data.Problem() + ")"));
                if (static_cast<long long>(up.size()) != d.Num_("upload_bytes"))
                    return Fail(rel + ": upload " + std::to_string(ord) + " has " + std::to_string(up.size()) +
                                " bytes, the recipe wants " + std::to_string(d.Num_("upload_bytes")));
                blob = std::move(up);
                blob.resize(static_cast<size_t>(d.Num_("bytes")), 0);
            } else if (kind == "intel_const") {
                const int idx = static_cast<int>(d.Num_("capture_index"));
                std::vector<uint8_t> rc;
                if (!data.RootConstants(idx, rc))
                    return Fail(rel + ": root constants of dispatch #" + std::to_string(idx) + " are not available" +
                                (data.Problem().empty() ? "" : " (" + data.Problem() + ")"));
                rc.resize(static_cast<size_t>(d.Num_("dwords")) * 4, 0);
                blob = std::move(rc);
            } else if (kind == "own_synth") {
                const auto* b = get(rel);
                if (!b) return Fail(rel + ": our generated data is not in this f5low.dll");
                blob = *b;
            } else {
                return Fail(rel + ": kind " + kind + " cannot be built");
            }
            if (Sha256Hex(blob.data(), blob.size()) != d.Str_("sha256"))
                return Fail(rel + ": " + kind + " content does not match the recipe's SHA-256 (wrong source?)");
            if (!WriteFile_(out / fs::path(rel), blob.data(), blob.size(), rel)) return false;
        }

        // nested packs (hdr\): a bundle entry <prefix><sub>/parts.json
        std::set<std::string> subs;
        for (const auto& kv : own) {
            const std::string& k = kv.first;
            if (k.size() > prefix.size() + 11 && k.compare(0, prefix.size(), prefix) == 0 &&
                k.compare(k.size() - 11, 11, "/parts.json") == 0 && k.find('/', prefix.size()) == k.size() - 11)
                subs.insert(k.substr(prefix.size(), k.size() - 11 - prefix.size()));
        }
        for (const std::string& s : subs)
            if (!BuildOne(prefix + s + "/", out / s)) return false;
        return true;
    }
};

}  // namespace

// ---- bundle ----------------------------------------------------------------------------------------------------
bool ParseBundle(const uint8_t* data, size_t size, Bundle& out, std::string* error) {
    out.clear();
    auto fail = [&](const char* why) {
        if (error) *error = why;
        out.clear();
        return false;
    };
    static const char kMagic[] = "F5OWN1\n";
    size_t at = sizeof(kMagic) - 1;
    if (size < at || memcmp(data, kMagic, at) != 0) return fail("not an F5OWN1 bundle");
    for (;;) {
        size_t eol = at;
        while (eol < size && data[eol] != '\n') ++eol;
        if (eol >= size) return fail("bundle ends early");
        const std::string line(reinterpret_cast<const char*>(data) + at, eol - at);
        at = eol + 1;
        if (line == "end") return true;
        const size_t sp = line.find(' ');
        if (sp == std::string::npos || sp == 0) return fail("bad bundle record");
        const unsigned long long n = strtoull(line.c_str(), nullptr, 10);
        if (n > size - std::min(at, size) || at + n + 1 > size || data[at + n] != '\n') return fail("bundle record is cut");
        out[line.substr(sp + 1)].assign(data + at, data + at + n);
        at += static_cast<size_t>(n) + 1;
    }
}

// ---- dev data source ---------------------------------------------------------------------------------------------
DevCaptureSource::DevCaptureSource(const std::wstring& weightsDir, const std::wstring& capLog) : capLog_(capLog) {
    std::error_code ec;
    for (fs::directory_iterator it(weightsDir, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (n.size() > 9 && n[0] == L'u' && n.compare(n.size() - 4, 4, L".bin") == 0 && n[5] == L'_' &&
            iswdigit(n[1]) && iswdigit(n[2]) && iswdigit(n[3]) && iswdigit(n[4]))
            uploads_[std::stoi(n.substr(1, 4))] = it->path().wstring();
    }
    if (uploads_.empty()) problem_ = "no u<NNNN>_*.bin files in " + Narrow(weightsDir);
}

bool DevCaptureSource::Upload(int ordinal, std::vector<uint8_t>& out) {
    auto it = uploads_.find(ordinal);
    if (it == uploads_.end()) {
        if (problem_.empty()) problem_ = "no upload " + std::to_string(ordinal) + " in the weight dump";
        return false;
    }
    return ReadAll(it->second, out);
}

bool DevCaptureSource::RootConstants(int index, std::vector<uint8_t>& out) {
    if (!rcDone_) {
        rcDone_ = true;
        std::ifstream f(capLog_);
        if (!f) problem_ = "cannot read the PSO capture " + Narrow(capLog_);
        std::string line;
        // the recipe indexes the first few dozen dispatches of the capture: read the first 4096
        while (f && rc_.size() < 4096 && std::getline(f, line)) {
            // "<time> D <n> t<id> list=... <tag> g=x,y,z [r<k>=<hex dwords>]"
            std::vector<std::string> t;
            for (size_t i = 0; i < line.size();) {
                while (i < line.size() && (line[i] == ' ' || line[i] == '\t' || line[i] == '\r')) ++i;
                size_t j = i;
                while (j < line.size() && line[j] != ' ' && line[j] != '\t' && line[j] != '\r') ++j;
                if (j > i) t.push_back(line.substr(i, j - i));
                i = j;
            }
            if (t.size() < 7 || t[1] != "D" || t[3].size() < 2 || t[3][0] != 't' || t[4].compare(0, 5, "list=") != 0 ||
                t[6].compare(0, 2, "g=") != 0)
                continue;
            std::vector<uint32_t> vals;
            if (t.size() > 7 && t[7].size() > 2 && t[7][0] == 'r') {
                const size_t eq = t[7].find('=');
                if (eq != std::string::npos && eq > 1) {
                    const std::string v = t[7].substr(eq + 1);
                    for (size_t i = 0; i <= v.size();) {
                        size_t c = v.find(',', i);
                        if (c == std::string::npos) c = v.size();
                        vals.push_back(static_cast<uint32_t>(strtoul(v.substr(i, c - i).c_str(), nullptr, 16)));
                        i = c + 1;
                    }
                }
            }
            rc_.push_back(std::move(vals));
        }
    }
    if (index < 0 || static_cast<size_t>(index) >= rc_.size()) {
        if (problem_.empty()) problem_ = "the capture has " + std::to_string(rc_.size()) + " dispatches";
        return false;
    }
    const auto& v = rc_[static_cast<size_t>(index)];
    out.resize(v.size() * 4);
    if (!v.empty()) memcpy(out.data(), v.data(), out.size());
    return true;
}

// ---- BuildPack ----------------------------------------------------------------------------------------------------
bool BuildPack(const std::wstring& libxessPath, const std::wstring& dxcompilerPath, const Bundle& own, IntelDataSource& data,
               const std::wstring& outDir, Report& rep, std::string* error) {
    const auto t0 = Clock::now();
    rep = Report();
    Builder b(own, data, dxcompilerPath, rep);
    std::string why;
    if (!b.lib.Open(libxessPath, &why)) {
        if (error) *error = "libxess_fg.dll (" + Narrow(libxessPath) + ") cannot be used: " + why;
        return false;
    }
    rep.decodeMs = b.lib.GetStats().decodeMs;
    const bool ok = b.BuildOne("i0/", fs::path(outDir));
    rep.writeMs = std::max(0.0, MsSince(t0) - rep.decodeMs - rep.translateMs);
    if (!ok && error) *error = b.err;
    return ok;
}

// ---- cache --------------------------------------------------------------------------------------------------------
namespace {
size_t CountFiles(const fs::path& d) {
    size_t n = 0;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec))
        if (it->is_regular_file(ec) && it->path().filename() != "complete") ++n;
    return n;
}

bool CacheComplete(const fs::path& d) {
    std::vector<uint8_t> m;
    if (!ReadAll(d / "complete", m)) return false;
    const std::string s(m.begin(), m.end());
    size_t want = 0;
    if (sscanf_s(s.c_str(), "f5low pack cache 1\nfiles %zu", &want) != 1) return false;
    return want > 0 && CountFiles(d) == want && fs::exists(d / "manifest.txt");
}
// Folders "<key>.tmp<pid>_<tick>" of builds that never finished: removed when nothing in them was written since before
// this process started (a build still running in another process writes constantly, so its folder is newer). A
// finished pack has no ".tmp" in its name and is never touched.
void RemoveStaleTemps(const fs::path& root) {
    FILETIME created{}, ex{}, k{}, u{};
    if (!GetProcessTimes(GetCurrentProcess(), &created, &ex, &k, &u)) return;
    ULARGE_INTEGER start;
    start.LowPart = created.dwLowDateTime;
    start.HighPart = created.dwHighDateTime;
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        const std::wstring n = it->path().filename().wstring();
        if (n.find(L".tmp") == std::wstring::npos || !it->is_directory(ec)) continue;
        // newest write anywhere in the folder (a directory's own time does not change when a file inside is rewritten)
        ULONGLONG newest = 0;
        auto stamp = [&](const fs::path& p) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return;
            ULARGE_INTEGER t;
            t.LowPart = d.ftLastWriteTime.dwLowDateTime;
            t.HighPart = d.ftLastWriteTime.dwHighDateTime;
            newest = std::max<ULONGLONG>(newest, t.QuadPart);
        };
        stamp(it->path());
        std::error_code e2;
        for (fs::recursive_directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) stamp(f->path());
        if (newest && newest < start.QuadPart) fs::remove_all(it->path(), ec);
    }
}

}  // namespace

Outcome EnsurePack(const Request& req, std::wstring& dir, std::string& error, Report* repOut) {
    auto log = [&](const std::string& s) {
        if (req.log) req.log(s);
    };
    try {
        std::vector<uint8_t> intel;
        if (!ReadAll(req.libxessPath, intel) || intel.empty()) {
            error = "libxess_fg.dll not found or not readable (" + Narrow(req.libxessPath) + ")";
            return Outcome::Failed;
        }
        const std::string intelSha = Sha256Hex(intel.data(), intel.size());
        const std::string dxcVer = dxc_disasm::FileVersion(req.dxcompilerPath);
        const std::string keyText = "f5low-pack-cache-1|libxess:" + intelSha + "|recipe+kernels:" +
                                    Sha256Hex(req.bundleBytes, req.bundleSize) + "|dxcompiler:" + (dxcVer.empty() ? "none" : dxcVer);
        // not the data source: every weight and constant is checked against the recipe's SHA-256, so any source that
        // builds a pack builds the same one
        const std::string key = Sha256Hex(keyText.data(), keyText.size()).substr(0, 32);
        const fs::path root(req.cacheRoot), final_ = root / key;
        if (CacheComplete(final_)) {
            dir = final_.wstring();
            log("pack cache hit " + key + " (libxess_fg " + intelSha.substr(0, 12) + ", dxcompiler " + (dxcVer.empty() ? "none" : dxcVer) + ")");
            return Outcome::Cached;
        }
        if (!req.own || !req.data) {
            error = !req.own ? "this f5low.dll carries no recipe" : "no data source for the weights (F5LOW_DEV_WEIGHTS + F5LOW_DEV_CAP)";
            return Outcome::Failed;
        }
        std::error_code ec;
        RemoveStaleTemps(root);
        const fs::path tmp = root / (key + ".tmp" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64()));
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        if (ec) {
            error = "cannot create the cache folder " + Narrow(tmp.wstring());
            return Outcome::Failed;
        }
        log("pack cache miss " + key + ": building from libxess_fg " + intelSha.substr(0, 12) +
            " (one-time, runs on the calling thread)");
        Report rep;
        std::string why;
        if (!BuildPack(req.libxessPath, req.dxcompilerPath, *req.own, *req.data, tmp.wstring(), rep, &why)) {
            fs::remove_all(tmp, ec);
            error = why;
            return Outcome::Failed;
        }
        rep.intelSha256 = intelSha;
        const std::string marker = "f5low pack cache 1\nfiles " + std::to_string(CountFiles(tmp)) + "\n";
        if (!WriteAll(tmp / "complete", marker.data(), marker.size())) {
            fs::remove_all(tmp, ec);
            error = "cannot write into the cache folder";
            return Outcome::Failed;
        }
        // atomic publish: a folder of this key that is not complete (killed build, damaged) is replaced; a complete one a
        // second process finished first is used as it is
        if (fs::exists(final_) && !CacheComplete(final_)) fs::remove_all(final_, ec);
        fs::rename(tmp, final_, ec);
        if (ec) {
            if (CacheComplete(final_)) {
                fs::remove_all(tmp, ec);
            } else {
                fs::remove_all(tmp, ec);
                error = "cannot move the built pack into " + Narrow(final_.wstring());
                return Outcome::Failed;
            }
        }
        char buf[256];
        snprintf(buf, sizeof buf,
                 "pack built: %zu files (%zu translated, %zu native), dxcompiler %s; decode %.0f ms, translate %.0f ms, write %.0f ms",
                 rep.files, rep.translated, rep.native, rep.dxcVersion.empty() ? "-" : rep.dxcVersion.c_str(), rep.decodeMs,
                 rep.translateMs, rep.writeMs);
        log(buf);
        if (repOut) *repOut = rep;
        dir = final_.wstring();
        return Outcome::Built;
    } catch (const std::exception& e) {
        error = std::string("pack build failed: ") + e.what();
        return Outcome::Failed;
    } catch (...) {
        error = "pack build failed";
        return Outcome::Failed;
    }
}

}  // namespace packbuild
