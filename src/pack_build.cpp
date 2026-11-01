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
#include <sstream>

#include "intel_modules.h"
#include "long_path.h"
#include "recipe_json.h"

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

bool WriteAll(fs::path p, const void* data, size_t n) {
    p.make_preferred();  // a "\\?\" path takes backslashes only (the recipe names files "data/R7.bin")
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

// JSON reader: recipe_json.h

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
    Report& rep;
    intel_modules::Library lib;
    std::string err;

    Builder(const Bundle& o, IntelDataSource& d, Report& r) : own(o), data(d), rep(r) {}

    bool Fail(const std::string& what) {
        if (err.empty()) err = what;
        return false;
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
                // the PTX twin of a native module: only the CUDA path runs it, and nothing is translated here. The
                // manifest still names the file; the loader runs the kernel's native module (Kernel::nativeOnly).
                ++rep.skipped;
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
                const auto tw = Clock::now();
                const bool gotUp = data.Upload(ord, up);
                rep.weightsMs += MsSince(tw);
                if (!gotUp)
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
                const auto tw = Clock::now();
                const bool gotRc = data.RootConstants(idx, rc);
                rep.weightsMs += MsSince(tw);
                if (!gotRc)
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
            // root constants the runtime patches every frame (or that are never initialised) are listed in `mask`: they
            // are written as 0, whatever the source gave, so every source builds the same bytes (and the recipe's hash
            // is over those zeros)
            if (const Json* m = d.Get("mask"))
                for (const Json& x : m->arr)
                    if (x.num >= 0 && static_cast<size_t>(x.num) * 4 + 4 <= blob.size()) memset(blob.data() + x.num * 4, 0, 4);
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
bool BuildPack(const std::wstring& libxessPath, const Bundle& own, IntelDataSource& data, const std::wstring& outDir,
               Report& rep, std::string* error, const std::vector<uint8_t>* libBytes) {
    const auto t0 = Clock::now();
    rep = Report();
    Builder b(own, data, rep);
    std::string why;
    if (libBytes) data.UseLibraryBytes(libBytes);
    if (!(libBytes ? b.lib.OpenMemory(libBytes->data(), libBytes->size(), &why) : b.lib.Open(libxessPath, &why))) {
        if (error) *error = "libxess_fg.dll (" + Narrow(libxessPath) + ") cannot be used: " + why;
        return false;
    }
    rep.decodeMs = b.lib.GetStats().decodeMs;
    bool ok = b.BuildOne("i0/", fs::path(outDir));
    if (ok) {
        // the pack has no PTX twins of its native modules: it runs on the D3D12 path only
        static const char kNote[] =
            "This pack was built on this PC from libxess_fg.dll by f5low.dll. It holds the DXBC of Intel's native modules\n"
            "but no PTX twins of them, so it runs on the D3D12 path only.\n";
        ok = b.WriteFile_(fs::path(outDir) / kD3d12OnlyMarker, kNote, sizeof kNote - 1, kD3d12OnlyMarker);
    }
    rep.writeMs = std::max(0.0, MsSince(t0) - rep.decodeMs - rep.weightsMs);
    if (!ok && error) *error = b.err;
    return ok;
}

// ---- cache --------------------------------------------------------------------------------------------------------
// How this builder lays a pack out and what it leaves out; part of the cache key and of the `complete` marker, so a builder
// that changes the layout never reuses a folder an older one made, and old folders can be told apart and pruned.
// !!! RAISE THIS on ANY change to the builder (BuildPack / Builder), to ResolveManifest, to the pack layout or to which
// files are written: a cache folder made by the old code is otherwise reused as it is. (The recipe and our kernels are in
// the key already, through the bundle's SHA-256.)
// 2: no translation, no PTX twins of native modules, a marker file. 3: the marker lists every file with its size.
constexpr int kFormatVersion = 3;
constexpr char kMarkerMagic[] = "f5low pack cache ";

namespace {
// Every regular file of the pack except the marker: relative path (forward slashes) and size.
std::vector<std::pair<std::string, uint64_t>> ListFiles(const fs::path& d) {
    std::vector<std::pair<std::string, uint64_t>> v;
    std::error_code ec;
    for (fs::recursive_directory_iterator it(d, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e2;
        if (!it->is_regular_file(e2) || it->path().filename() == "complete") continue;
        const uint64_t sz = it->file_size(e2);
        v.emplace_back(longpath::ToUtf8(it->path().lexically_relative(d).generic_wstring()), e2 ? ~0ull : sz);
    }
    std::sort(v.begin(), v.end());
    return v;
}

// "f5low pack cache <n>\nfiles <count>\n" then one "<size>\t<relative path>" line per file.
std::string MakeMarker(const fs::path& d) {
    const auto files = ListFiles(d);
    std::string m = std::string(kMarkerMagic) + std::to_string(kFormatVersion) + "\nfiles " + std::to_string(files.size()) + "\n";
    for (const auto& f : files) m += std::to_string(f.second) + "\t" + f.first + "\n";
    return m;
}

// The cache format a folder's marker names (0: no readable marker).
int MarkerFormat(const fs::path& d) {
    std::vector<uint8_t> m;
    if (!ReadAll(d / "complete", m)) return 0;
    const std::string s(m.begin(), m.end());
    if (s.compare(0, sizeof kMarkerMagic - 1, kMarkerMagic) != 0) return 0;
    return atoi(s.c_str() + sizeof kMarkerMagic - 1);
}

// The marker of this format, every file it lists present with the size it was written with, and no other file in the folder.
bool CacheComplete(const fs::path& d) {
    std::vector<uint8_t> m;
    if (!ReadAll(d / "complete", m)) return false;
    std::istringstream in(std::string(m.begin(), m.end()));
    std::string line;
    if (!std::getline(in, line) || line != kMarkerMagic + std::to_string(kFormatVersion)) return false;
    size_t want = 0;
    if (!std::getline(in, line) || sscanf_s(line.c_str(), "files %zu", &want) != 1 || want == 0) return false;
    size_t seen = 0;
    while (std::getline(in, line)) {
        const size_t tab = line.find('\t');
        if (tab == std::string::npos) return false;
        const uint64_t size = strtoull(line.substr(0, tab).c_str(), nullptr, 10);
        std::error_code ec;
        fs::path f = d / fs::path(longpath::WidenUtf8(line.substr(tab + 1)));
        f.make_preferred();     // a "\\?\" path takes backslashes only
        const uint64_t have = fs::file_size(f, ec);
        if (ec || have != size) return false;
        ++seen;
    }
    return seen == want && ListFiles(d).size() == want && fs::exists(d / "manifest.txt");
}

ULONGLONG FileTimeU(const FILETIME& t) {
    ULARGE_INTEGER u;
    u.LowPart = t.dwLowDateTime;
    u.HighPart = t.dwHighDateTime;
    return u.QuadPart;
}

// Folders "<key>.tmp<pid>_<tick>" of builds that never finished: removed when nothing in them was written since `cutoff`
// (a build still running in another process writes constantly, so its folder is newer). A finished pack has no ".tmp"
// in its name and is never touched.
void RemoveStaleTemps(const fs::path& root, ULONGLONG cutoff) {
    std::error_code ec;
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e1;
        const std::wstring n = it->path().filename().wstring();
        if (n.find(L".tmp") == std::wstring::npos || !it->is_directory(e1)) continue;
        // newest write anywhere in the folder (a directory's own time does not change when a file inside is rewritten)
        ULONGLONG newest = 0;
        auto stamp = [&](const fs::path& p) {
            WIN32_FILE_ATTRIBUTE_DATA d{};
            if (!GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d)) return;
            newest = std::max<ULONGLONG>(newest, FileTimeU(d.ftLastWriteTime));
        };
        stamp(it->path());
        std::error_code e2;
        for (fs::recursive_directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) stamp(f->path());
        if (newest && newest < cutoff) fs::remove_all(it->path(), e1);
    }
}

ULONGLONG ProcessStart() {
    FILETIME created{}, ex{}, k{}, u{};
    return GetProcessTimes(GetCurrentProcess(), &created, &ex, &k, &u) ? FileTimeU(created) : 0;
}

// After a successful build or hit: the cache folders of an OLDER format (their `complete` marker names it) and the stale
// temporary folders (nothing written for an hour) go. A folder of the current format is never touched (another process,
// or another libxess_fg.dll next to this DLL, uses it). Failures to delete are ignored.
void PruneCache(const fs::path& root) {
    std::error_code ec;
    FILETIME now{};
    GetSystemTimeAsFileTime(&now);
    constexpr ULONGLONG kHour = 3600ull * 10000000ull;
    const ULONGLONG t = FileTimeU(now);
    RemoveStaleTemps(root, t > kHour ? t - kHour : 0);
    for (fs::directory_iterator it(root, ec), end; !ec && it != end; it.increment(ec)) {
        std::error_code e1;
        if (!it->is_directory(e1) || it->path().filename().wstring().find(L".tmp") != std::wstring::npos) continue;
        const int f = MarkerFormat(it->path());
        if (f > 0 && f < kFormatVersion) fs::remove_all(it->path(), e1);
    }
}

}  // namespace

bool DiscardPack(const std::wstring& dir) {
    try {
        const fs::path p(longpath::Extend(dir));
        // only a cache folder: it sits in a folder called f5low_cache and has our marker
        if (p.parent_path().filename() != L"f5low_cache" || MarkerFormat(p) <= 0) return false;
        std::error_code ec;
        fs::remove_all(p, ec);
        return !fs::exists(p);
    } catch (...) {
        return false;
    }
}

Outcome EnsurePack(const Request& req, std::wstring& dir, std::string& error, Report* repOut) {
    auto log = [&](const std::string& s) {
        if (req.log) req.log(s);
    };
    try {
        std::vector<uint8_t> intel;
        if (!ReadAll(fs::path(longpath::ExtendIfLong(req.libxessPath)), intel) || intel.empty()) {
            error = "libxess_fg.dll not found or not readable (" + Narrow(req.libxessPath) + ")";
            return Outcome::Failed;
        }
        const std::string intelSha = Sha256Hex(intel.data(), intel.size());
        const std::string keyText = std::string("f5low-pack-cache-") + std::to_string(kFormatVersion) + "|libxess:" + intelSha +
                                    "|recipe+kernels:" + Sha256Hex(req.bundleBytes, req.bundleSize);
        // not the data source: every weight and constant is checked against the recipe's SHA-256, so any source that
        // builds a pack builds the same one
        const std::string key = Sha256Hex(keyText.data(), keyText.size()).substr(0, 32);
        // every path below is in the extended form (long_path.h): a deep install folder passes MAX_PATH
        fs::path root(longpath::Extend(req.cacheRoot));
        fs::path final_ = root / key;
        const fs::path fbRoot(req.fallbackRoot.empty() ? std::wstring() : longpath::Extend(req.fallbackRoot));
        if (CacheComplete(final_)) {
            dir = longpath::Plain(final_.wstring());
            log("pack cache hit " + key + " (libxess_fg " + intelSha.substr(0, 12) + ")");
            PruneCache(root);
            return Outcome::Cached;
        }
        // a pack an earlier run had to build in the fallback folder (the DLL's folder is not writable)
        if (!fbRoot.empty() && CacheComplete(fbRoot / key)) {
            root = fbRoot;
            final_ = root / key;
            dir = longpath::Plain(final_.wstring());
            log("pack cache hit " + key + " in " + Narrow(longpath::Plain(root.wstring())) + " (libxess_fg " +
                intelSha.substr(0, 12) + ")");
            PruneCache(root);
            return Outcome::Cached;
        }
        if (!req.own || !req.data) {
            error = !req.own ? "this f5low.dll carries no recipe" : "no data source for the weights";
            return Outcome::Failed;
        }
        std::error_code ec;
        RemoveStaleTemps(root, ProcessStart());
        const auto tempName = [&] {
            return key + ".tmp" + std::to_string(GetCurrentProcessId()) + "_" + std::to_string(GetTickCount64());
        };
        fs::path tmp = root / tempName();
        fs::remove_all(tmp, ec);
        fs::create_directories(tmp, ec);
        if (ec && !fbRoot.empty()) {
            // not writable (Program Files, a Game Pass WindowsApps install, ...): the per-user folder takes the cache
            log("cache folder " + Narrow(longpath::Plain(root.wstring())) + " is not writable (" + ec.message() +
                "): using " + Narrow(longpath::Plain(fbRoot.wstring())));
            root = fbRoot;
            final_ = root / key;
            RemoveStaleTemps(root, ProcessStart());
            tmp = root / tempName();
            fs::remove_all(tmp, ec);
            fs::create_directories(tmp, ec);
        }
        if (ec) {
            error = "cannot create the cache folder " + Narrow(longpath::Plain(tmp.wstring())) + " (" + ec.message() + ")";
            return Outcome::Failed;
        }
        log("pack cache miss " + key + ": building from libxess_fg " + intelSha.substr(0, 12) +
            " (one-time, runs on the calling thread)");
        Report rep;
        std::string why;
        if (!BuildPack(req.libxessPath, *req.own, *req.data, tmp.wstring(), rep, &why, &intel)) {
            fs::remove_all(tmp, ec);
            error = why;
            return Outcome::Failed;
        }
        rep.intelSha256 = intelSha;
        const std::string marker = MakeMarker(tmp);
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
            const std::string how = ec.message();
            if (CacheComplete(final_)) {
                fs::remove_all(tmp, ec);
            } else {
                fs::remove_all(tmp, ec);
                error = "cannot move the built pack into " + Narrow(longpath::Plain(final_.wstring())) + " (" + how + ")";
                return Outcome::Failed;
            }
        }
        char buf[256];
        snprintf(buf, sizeof buf,
                 "pack built: %zu files (%zu native modules; %zu PTX twins left out, nothing translated); decode %.0f ms, weights + "
                 "root constants %.0f ms, write %.0f ms",
                 rep.files, rep.native, rep.skipped, rep.decodeMs, rep.weightsMs, rep.writeMs);
        log(buf);
        if (repOut) *repOut = rep;
        dir = longpath::Plain(final_.wstring());
        PruneCache(root);
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
