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

// A file's key in the in-memory pack: lower case, '\' separators (xefg_pack.h VirtualKey reads it the same way).
std::string KeyOf(std::string rel) {
    for (char& c : rel) c = c == '/' ? '\\' : static_cast<char>(tolower(static_cast<unsigned char>(c)));
    return rel;
}

struct Builder {
    const Bundle& own;
    IntelDataSource& data;
    Report& rep;
    Files& out;                 // the pack, in memory: nothing is written to disk
    intel_modules::Library lib;
    std::string err;

    Builder(const Bundle& o, IntelDataSource& d, Report& r, Files& f) : own(o), data(d), rep(r), out(f) {}

    bool Fail(const std::string& what) {
        if (err.empty()) err = what;
        return false;
    }

    // `rel`: the file's path in the pack, '/' separated ("data/R7.bin", "hdr/manifest.txt")
    bool Put(const std::string& rel, const void* d, size_t n, const std::string& what) {
        const auto r = out.emplace(KeyOf(rel), std::vector<uint8_t>());
        if (!r.second) return Fail(what + ": " + rel + " is written twice");
        r.first->second.assign(static_cast<const uint8_t*>(d), static_cast<const uint8_t*>(d) + n);
        ++rep.files;
        rep.bytes += n;
        return true;
    }

    // `at`: this pack's folder inside the pack ("" for the main one, "hdr/" for the nested one)
    bool BuildOne(const std::string& prefix, const std::string& at) {
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
                if (!Put(at + n, b->data(), b->size(), n)) return false;
            } else if (kind == "intel_native") {
                const std::string mod = part.Str_("module_sha256");
                const std::vector<uint8_t>* m = lib.FindHex(mod);
                if (!m) return Fail(n + ": module " + Hex8(mod) + " not found in libxess_fg.dll");
                ++rep.native;
                if (!Put(at + n, m->data(), m->size(), n)) return false;
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
        if (!Put(at + "manifest.txt", text.data(), text.size(), "manifest.txt")) return false;

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
            if (!Put(at + rel, blob.data(), blob.size(), rel)) return false;
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
            if (!BuildOne(prefix + s + "/", at + s + "/")) return false;
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
bool BuildPack(const std::wstring& libxessPath, const Bundle& own, IntelDataSource& data, Files& out,
               Report& rep, std::string* error, const std::vector<uint8_t>* libBytes) {
    const auto t0 = Clock::now();
    rep = Report();
    out.clear();
    Builder b(own, data, rep, out);
    std::string why;
    if (libBytes) data.UseLibraryBytes(libBytes);
    if (!(libBytes ? b.lib.OpenMemory(libBytes->data(), libBytes->size(), &why) : b.lib.Open(libxessPath, &why))) {
        if (error) *error = "libxess_fg.dll (" + Narrow(libxessPath) + ") cannot be used: " + why;
        out.clear();
        return false;
    }
    rep.decodeMs = b.lib.GetStats().decodeMs;
    bool ok = b.BuildOne("i0/", "");
    if (ok) {
        // the pack has no PTX twins of its native modules: it runs on the D3D12 path only
        static const char kNote[] =
            "This pack was built on this PC from libxess_fg.dll by f5low.dll. It holds the DXBC of Intel's native modules\n"
            "but no PTX twins of them, so it runs on the D3D12 path only.\n";
        ok = b.Put(kD3d12OnlyMarker, kNote, sizeof kNote - 1, kD3d12OnlyMarker);
    }
    rep.writeMs = std::max(0.0, MsSince(t0) - rep.decodeMs - rep.weightsMs);
    if (!ok) {
        if (error) *error = b.err;
        out.clear();
    }
    return ok;
}

bool BuildInMemory(const Request& req, std::shared_ptr<const Files>& out, std::string& error, Report* repOut) {
    try {
        if (!req.own || !req.data) {
            error = !req.own ? "this f5low.dll carries no recipe" : "no data source for the weights";
            return false;
        }
        Report rep;
        auto files = std::make_shared<Files>();
        {
            // The player's libxess_fg.dll is read into memory for the build and let go of when it is done; the pack that
            // stays holds our kernels and the parts the recipe names (native modules, weights, root constants)
            std::vector<uint8_t> intel;
            if (!ReadAll(fs::path(longpath::ExtendIfLong(req.libxessPath)), intel) || intel.empty()) {
                error = "libxess_fg.dll not found or not readable (" + Narrow(req.libxessPath) + ")";
                return false;
            }
            if (req.log) req.log("building the pack in memory from libxess_fg.dll (" + std::to_string(intel.size() >> 10) + " KiB)");
            std::string why;
            if (!BuildPack(req.libxessPath, *req.own, *req.data, *files, rep, &why, &intel)) {
                error = why;
                return false;
            }
        }
        if (repOut) *repOut = rep;
        out = std::move(files);
        return true;
    } catch (const std::exception& e) {
        error = std::string("pack build failed: ") + e.what();
        return false;
    } catch (...) {
        error = "pack build failed";
        return false;
    }
}

// ---- legacy disk cache -----------------------------------------------------------------------------------------------
// Until format 3 the pack was kept on disk in <root>\<32 hex>\ (marker file `complete`). That held Intel's modules and weights
// as plain files, so it is gone: nothing Intel-derived is written to disk any more, and any such folder found is deleted.
constexpr char kMarkerMagic[] = "f5low pack cache ";

namespace {
ULONGLONG FileTimeU(const FILETIME& t) {
    ULARGE_INTEGER u;
    u.LowPart = t.dwLowDateTime;
    u.HighPart = t.dwHighDateTime;
    return u.QuadPart;
}

// The cache format a folder's marker names (0: no readable marker of ours).
int MarkerFormat(const fs::path& d) {
    std::vector<uint8_t> m;
    if (!ReadAll(d / "complete", m)) return 0;
    const std::string s(m.begin(), m.end());
    if (s.compare(0, sizeof kMarkerMagic - 1, kMarkerMagic) != 0) return 0;
    return atoi(s.c_str() + sizeof kMarkerMagic - 1);
}

// "<32 hex>.tmp<pid>_<tick>": the name an old build gave a pack folder it was still making.
bool IsOldTempName(const std::wstring& n) {
    if (n.size() <= 36 || n.compare(32, 4, L".tmp") != 0) return false;
    for (size_t i = 0; i < 32; ++i)
        if (!iswxdigit(n[i])) return false;
    return true;
}
}  // namespace

int RemoveLegacyCaches(const std::wstring& root, const std::function<void(const std::string&)>& log) {
    int removed = 0;
    try {
        const fs::path r(longpath::Extend(root));
        std::error_code ec;
        if (!fs::is_directory(r, ec)) return 0;
        FILETIME now{};
        GetSystemTimeAsFileTime(&now);
        constexpr ULONGLONG kHour = 3600ull * 10000000ull;
        const ULONGLONG t = FileTimeU(now);
        for (fs::directory_iterator it(r, ec), end; !ec && it != end; it.increment(ec)) {
            std::error_code e1;
            if (!it->is_directory(e1)) continue;
            const std::wstring name = it->path().filename().wstring();
            bool old = false;
            if (IsOldTempName(name)) {
                // a build of an older f5low.dll that never finished (or is still running in another process: it writes
                // constantly, so its newest file is recent): removed when nothing in it was written for an hour
                ULONGLONG newest = 0;
                auto stamp = [&](const fs::path& p) {
                    WIN32_FILE_ATTRIBUTE_DATA d{};
                    if (GetFileAttributesExW(p.c_str(), GetFileExInfoStandard, &d))
                        newest = std::max<ULONGLONG>(newest, FileTimeU(d.ftLastWriteTime));
                };
                stamp(it->path());
                std::error_code e2;
                for (fs::recursive_directory_iterator f(it->path(), e2), fend; !e2 && f != fend; f.increment(e2)) stamp(f->path());
                old = newest && newest + kHour < t;
            } else if (MarkerFormat(it->path()) > 0) {
                old = true;     // carries our pack marker (any format): a pack of Intel-derived files, which are no longer kept
            }
            if (!old) continue;       // anything without our marker (the cubin cache, a user's folder) is never touched
            const fs::path victim = it->path();
            fs::remove_all(victim, e1);
            const bool gone = !fs::exists(victim, e1);
            if (gone)
                ++removed;
            if (log)
                log(std::string(gone ? "removed" : "could not remove") + " the old on-disk pack cache " +
                    Narrow(longpath::Plain(victim.wstring())) + (gone ? " (the pack is built in memory now)" : ""));
        }
    } catch (...) {
    }
    return removed;
}

}  // namespace packbuild
