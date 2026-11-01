// translate_test - translates every intel_xlate entry of the recipe (recipe/i0/parts.json and recipe/i0/hdr/parts.json)
// from a player's libxess_fg.dll in this process and compares the PTX byte for byte with the reference the offline
// Python tools wrote for the same module and options.
//
// Usage: translate_test.exe <libxess_fg.dll> <recipe folder> <reference folder> <dxcompiler.dll>
//   reference folder: sdr\<part file name>, hdr\<part file name> (the PTX) and dxasm\<module sha256>.dxasm (what
//   `dxc -dumpbin` printed for the module); written outside the repo by the reference script.
// Checks, per entry: (1) the disassembly text from dxcompiler.dll in this process equals the reference dxasm,
// (2) the PTX translated from that text equals the reference PTX. Prints the time of both steps.
// Exit code: 0 all identical, 1 usage / setup failed, 2 a difference.
#define NOMINMAX
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <map>
#include <memory>
#include <sstream>
#include <string>
#include <vector>

#include "../src/dxc_disasm.h"
#include "../src/intel_modules.h"
#include "../src/translate.h"

namespace {

// A just-enough JSON reader for the recipe files (objects, arrays, strings, numbers, literals).
struct Json {
    enum Kind { Null, Str, Arr, Obj, Other } kind = Null;
    std::string str;
    std::vector<Json> arr;
    std::vector<std::pair<std::string, Json>> obj;
    const Json* Get(const std::string& k) const {
        for (const auto& kv : obj)
            if (kv.first == k) return &kv.second;
        return nullptr;
    }
};

struct Reader {
    const std::string& s;
    size_t p = 0;
    void Ws() {
        while (p < s.size() && (s[p] == ' ' || s[p] == '\n' || s[p] == '\r' || s[p] == '\t')) ++p;
    }
    std::string String() {
        std::string out;
        ++p;  // opening quote
        while (p < s.size() && s[p] != '"') {
            if (s[p] == '\\') ++p;
            out += s[p++];
        }
        ++p;
        return out;
    }
    Json Value() {
        Ws();
        Json j;
        if (s[p] == '{') {
            j.kind = Json::Obj;
            ++p;
            for (;;) {
                Ws();
                if (s[p] == '}') {
                    ++p;
                    break;
                }
                if (s[p] == ',') {
                    ++p;
                    continue;
                }
                std::string k = String();
                Ws();
                ++p;  // colon
                j.obj.push_back({k, Value()});
            }
        } else if (s[p] == '[') {
            j.kind = Json::Arr;
            ++p;
            for (;;) {
                Ws();
                if (s[p] == ']') {
                    ++p;
                    break;
                }
                if (s[p] == ',') {
                    ++p;
                    continue;
                }
                j.arr.push_back(Value());
            }
        } else if (s[p] == '"') {
            j.kind = Json::Str;
            j.str = String();
        } else {
            j.kind = Json::Other;
            while (p < s.size() && s[p] != ',' && s[p] != '}' && s[p] != ']') ++p;
        }
        return j;
    }
};

bool ReadFile(const std::string& path, std::string& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    std::stringstream ss;
    ss << f.rdbuf();
    out = ss.str();
    return true;
}

std::wstring Wide(const std::string& s) { return std::wstring(s.begin(), s.end()); }
std::string Narrow(const wchar_t* w) {
    std::string s;
    for (; *w; ++w) s += char(*w);
    return s;
}

double Ms(std::chrono::steady_clock::time_point a, std::chrono::steady_clock::time_point b) {
    return std::chrono::duration<double, std::milli>(b - a).count();
}

// Where two texts first differ, for the report.
std::string FirstDiff(const std::string& a, const std::string& b) {
    size_t n = std::min(a.size(), b.size()), i = 0;
    while (i < n && a[i] == b[i]) ++i;
    size_t line = 1;
    for (size_t k = 0; k < i; ++k) line += a[k] == '\n';
    size_t ls = a.rfind('\n', i ? i - 1 : 0);
    ls = ls == std::string::npos ? 0 : ls + 1;
    size_t le = a.find('\n', i);
    size_t ms = b.rfind('\n', i ? i - 1 : 0);
    ms = ms == std::string::npos ? 0 : ms + 1;
    size_t me = b.find('\n', i);
    return "line " + std::to_string(line) + " (sizes " + std::to_string(a.size()) + " vs " + std::to_string(b.size()) + ")\n  got:  " +
           a.substr(ls, le == std::string::npos ? std::string::npos : le - ls) + "\n  want: " +
           b.substr(ms, me == std::string::npos ? std::string::npos : me - ms);
}

}  // namespace

int wmain(int argc, wchar_t** argv) {
    if (argc < 5) {
        printf("usage: translate_test <libxess_fg.dll> <recipe folder> <reference folder> <dxcompiler.dll>\n");
        return 1;
    }
    std::string recipe = Narrow(argv[2]), ref = Narrow(argv[3]);
    intel_modules::Library lib;
    std::string err;
    if (!lib.Open(argv[1], &err)) {
        printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    dxc_disasm::Disassembler dxc;
    if (!dxc.Load(argv[4], &err)) {
        printf("dxcompiler: %s\n", err.c_str());
        return 1;
    }
    printf("libxess_fg: %zu distinct modules (decode %.0f ms); dxcompiler %s\n", lib.GetStats().distinct, lib.GetStats().decodeMs,
           dxc.Version().c_str());

    size_t entries = 0, badDis = 0, badPtx = 0, failed = 0;
    double disMs = 0, trMs = 0;
    size_t ptxBytes = 0;
    struct Pack {
        const char* sub;
        const char* json;
    };
    for (const Pack& pk : {Pack{"sdr", "/i0/parts.json"}, Pack{"hdr", "/i0/hdr/parts.json"}}) {
        std::string text;
        if (!ReadFile(recipe + pk.json, text)) {
            printf("cannot read %s%s\n", recipe.c_str(), pk.json);
            return 1;
        }
        Reader rd{text};
        Json root = rd.Value();
        const Json* files = root.Get("files");
        if (!files) return 1;
        for (const auto& [name, part] : files->obj) {
            const Json* kind = part.Get("kind");
            if (!kind || kind->str != "intel_xlate") continue;
            std::string hash = part.Get("module_sha256")->str, transform = part.Get("transform")->str;
            std::vector<std::string> fmts;
            for (const Json& f : part.Get("fmts")->arr) fmts.push_back(f.str);
            const std::vector<uint8_t>* mod = lib.FindHex(hash);
            ++entries;
            if (!mod) {
                printf("MISSING %s/%s: module %s not in the DLL\n", pk.sub, name.c_str(), hash.substr(0, 8).c_str());
                ++failed;
                continue;
            }
            auto t0 = std::chrono::steady_clock::now();
            std::string dis;
            if (!dxc.Disassemble(mod->data(), mod->size(), dis, &err)) {
                printf("FAIL %s/%s: %s\n", pk.sub, name.c_str(), err.c_str());
                ++failed;
                continue;
            }
            auto t1 = std::chrono::steady_clock::now();
            std::string ptx;
            std::string asmName = name.substr(0, name.rfind('.')) + ".dxasm";
            bool ok = translate::TranslateEntry(dis, transform, fmts, asmName, ptx, &err);
            auto t2 = std::chrono::steady_clock::now();
            disMs += Ms(t0, t1);
            trMs += Ms(t1, t2);
            if (!ok) {
                printf("FAIL %s/%s: translation: %s\n", pk.sub, name.c_str(), err.c_str());
                ++failed;
                continue;
            }
            ptxBytes += ptx.size();
            std::string wantAsm, wantPtx;
            if (!ReadFile(ref + "/dxasm/" + hash + ".dxasm", wantAsm) || !ReadFile(ref + "/" + pk.sub + "/" + name, wantPtx)) {
                printf("FAIL %s/%s: no reference file\n", pk.sub, name.c_str());
                ++failed;
                continue;
            }
            // `dxc -dumpbin` writes through a text-mode stdout: its file has CR LF line ends, the in-process text LF only.
            std::string wantLf;
            for (size_t i = 0; i < wantAsm.size(); ++i)
                if (!(wantAsm[i] == '\r' && i + 1 < wantAsm.size() && wantAsm[i + 1] == '\n')) wantLf += wantAsm[i];
            // ... and prints one more line end after the text (the in-process text ends at the last line's end).
            if (wantLf.size() == dis.size() + 1 && wantLf.back() == '\n') wantLf.pop_back();
            bool disSame = dis == wantLf;
            bool ptxSame = ptx == wantPtx;
            badDis += !disSame;
            badPtx += !ptxSame;
            printf("%s %s/%-34s %-9s dis %s ptx %s  (%zu bytes)\n", disSame && ptxSame ? "ok  " : "DIFF", pk.sub, name.c_str(),
                   transform.c_str(), disSame ? "same" : "DIFFERS", ptxSame ? "identical" : "DIFFERS", ptx.size());
            if (!disSame) printf("  disassembly: %s\n", FirstDiff(dis, wantLf).c_str());
            if (!ptxSame) printf("  ptx: %s\n", FirstDiff(ptx, wantPtx).c_str());
        }
    }
    printf("\n%zu entries: disassembly differs %zu, PTX differs %zu, failed %zu\n", entries, badDis, badPtx, failed);
    printf("time: disassembly %.1f ms, translation %.1f ms (%.1f ms per entry), %zu PTX bytes\n", disMs, trMs, entries ? trMs / entries : 0.0, ptxBytes);
    return (badDis || badPtx || failed) ? 2 : 0;
}
