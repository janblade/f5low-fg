// modules_test - decodes a libxess_fg.dll with src/intel_modules.cpp and checks that every module the recipe names
// (module_sha256 in recipe/i0/parts.json and recipe/i0/hdr/parts.json) is found by its hash.
//
// Usage: modules_test.exe <libxess_fg.dll> <recipe folder> [hashes.txt]
//   hashes.txt: written with the SHA-256 of every decoded container, sorted, one per line (to compare with another decoder)
// Exit code: 0 all found, 1 usage / decode failed, 2 a recipe module is missing.
#include <windows.h>

#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <string>

#include "../src/intel_modules.h"

static std::set<std::string> RecipeHashes(const std::string& path) {
    std::set<std::string> out;
    std::ifstream f(path, std::ios::binary);
    if (!f) return out;
    std::stringstream ss;
    ss << f.rdbuf();
    std::string s = ss.str();
    const std::string key = "\"module_sha256\"";
    for (size_t at = s.find(key); at != std::string::npos; at = s.find(key, at + key.size())) {
        size_t q = s.find('"', s.find(':', at));
        if (q == std::string::npos) break;
        out.insert(s.substr(q + 1, 64));
    }
    return out;
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: modules_test <libxess_fg.dll> <recipe folder> [hashes.txt]\n");
        return 1;
    }
    auto narrow = [](const wchar_t* w) {
        std::string s;
        for (; *w; ++w) s += char(*w);
        return s;
    };
    intel_modules::Library lib;
    std::string err;
    if (!lib.Open(argv[1], &err)) {
        printf("decode failed: %s\n", err.c_str());
        return 1;
    }
    const auto& st = lib.GetStats();
    printf("dictionaries %zu, module frames %zu, DXIL containers %zu (rejected %zu), distinct hashes %zu, decode %.0f ms\n",
           st.dictionaries, st.frames, st.dxil, st.rejected, st.distinct, st.decodeMs);

    std::string recipe = narrow(argv[2]);
    std::set<std::string> want;
    for (const char* rel : {"/i0/parts.json", "/i0/hdr/parts.json"}) {
        auto h = RecipeHashes(recipe + rel);
        printf("%s: %zu distinct module hashes\n", rel, h.size());
        want.insert(h.begin(), h.end());
    }
    size_t missing = 0;
    for (const std::string& h : want) {
        if (!lib.FindHex(h)) {
            printf("MISSING %s\n", h.substr(0, 8).c_str());
            ++missing;
        }
    }
    printf("recipe modules: %zu distinct, %zu found, %zu missing\n", want.size(), want.size() - missing, missing);

    if (argc > 3) {
        std::ofstream out(narrow(argv[3]));
        for (const auto& kv : lib.All()) out << intel_modules::ToHex(kv.first) << "\n";
    }
    // a file that is not a libxess_fg must fail cleanly
    intel_modules::Library bad;
    std::string e2;
    wchar_t self[MAX_PATH];
    GetModuleFileNameW(nullptr, self, MAX_PATH);
    bool badOk = !bad.Open(self, &e2) && !bad.Find(intel_modules::Sha256{});
    printf("non-libxess file (this exe): %s (%s)\n", badOk ? "rejected cleanly" : "NOT REJECTED", e2.c_str());
    printf(missing || !badOk ? "FAIL\n" : "PASS\n");
    return missing || !badOk ? 2 : 0;
}
