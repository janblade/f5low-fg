// pack_samples - the reference for tests/no_intel_on_disk.ps1: builds the pack in memory exactly as f5low.dll does (BuildInMemory
// with the recipe taken from the f5low.dll's resource 102 and the weights from the libxess_fg.dll FILE) and writes down, for
// every file of it that is NOT our own (not byte-identical to an entry of the DLL's recipe bundle - so Intel's native modules,
// the weights, the root constants), its size, its SHA-256 and up to 8 windows of 64 bytes spread over the file. The checker
// then looks for those in every file f5low wrote: a file with the same hash, or containing any of the windows, is Intel's data
// on disk. TEST TOOL ONLY: it runs outside f5low.dll, and its output goes to a scratch folder the test deletes.
//
// Usage: pack_samples.exe <libxess_fg.dll> <f5low.dll> <samples.txt> [plant_dir]
//   plant_dir: also writes ONE Intel-derived file (a native module) and one weight file into it, whole - the checker's
//   self-test (a scan of that folder must fail).
// Exit code: 0 ok, 1 usage / unreadable, 2 build failed.
#include <windows.h>

#include <cstdio>
#include <set>
#include <string>
#include <vector>

#include "../src/intel_file_source.h"
#include "../src/intel_modules.h"
#include "../src/pack_build.h"

static std::string Sha(const std::vector<uint8_t>& b) {
    intel_modules::Sha256 h{};
    return intel_modules::HashBytes(b.data(), b.size(), h) ? intel_modules::ToHex(h) : std::string();
}

int wmain(int argc, wchar_t** argv) {
    if (argc < 4) {
        printf("usage: pack_samples <libxess_fg.dll> <f5low.dll> <samples.txt> [plant_dir]\n");
        return 1;
    }
    HMODULE m = LoadLibraryExW(argv[2], nullptr, LOAD_LIBRARY_AS_DATAFILE);
    HRSRC res = m ? FindResourceW(m, MAKEINTRESOURCEW(102), MAKEINTRESOURCEW(10)) : nullptr;
    const uint8_t* bytes = res ? static_cast<const uint8_t*>(LockResource(LoadResource(m, res))) : nullptr;
    if (!bytes) {
        printf("no recipe (resource 102) in %ls\n", argv[2]);
        return 1;
    }
    packbuild::Bundle bundle;
    std::string err;
    if (!packbuild::ParseBundle(bytes, SizeofResource(m, res), bundle, &err)) {
        printf("bundle: %s\n", err.c_str());
        return 1;
    }
    const auto parts = bundle.find("i0/parts.json");
    const auto tmpl = bundle.find("i0/manifest.tmpl");
    if (parts == bundle.end() || tmpl == bundle.end()) {
        printf("the bundle has no i0 recipe\n");
        return 1;
    }
    packbuild::IntelFileSource src(argv[1], parts->second, tmpl->second);
    packbuild::Request req;
    req.libxessPath = argv[1];
    req.own = &bundle;
    req.data = &src;
    std::shared_ptr<const packbuild::Files> files;
    packbuild::Report rep;
    if (!packbuild::BuildInMemory(req, files, err, &rep)) {
        printf("BUILD FAILED: %s\n", err.c_str());
        return 2;
    }
    std::set<std::string> own;
    for (const auto& kv : bundle) own.insert(Sha(kv.second));

    FILE* out = nullptr;
    if (_wfopen_s(&out, argv[3], L"w") || !out) {
        printf("cannot write %ls\n", argv[3]);
        return 1;
    }
    size_t intelFiles = 0, windows = 0;
    bool plantedModule = false, plantedWeight = false;
    for (const auto& kv : *files) {
        const std::string sha = Sha(kv.second);
        if (own.count(sha)) continue;   // ours: a kernel, a manifest, the noise texture, the marker note is ours too (written below)
        if (kv.first == "d3d12_only") continue;
        ++intelFiles;
        fprintf(out, "F %s %zu %s\n", kv.first.c_str(), kv.second.size(), sha.c_str());
        const std::vector<uint8_t>& d = kv.second;
        // up to 8 windows of 64 bytes at spread offsets; one with fewer than 16 distinct byte values is no evidence of anything
        for (int k = 0; k < 8 && d.size() >= 64; ++k) {
            const size_t span = d.size() - 64;
            for (int probe = 0; probe < 16; ++probe) {
                const size_t at = (span * (static_cast<size_t>(k) * 16 + probe) / (8 * 16)) % (span + 1);
                bool seen[256] = {};
                int distinct = 0;
                for (size_t i = 0; i < 64; ++i)
                    if (!seen[d[at + i]]) seen[d[at + i]] = true, ++distinct;
                if (distinct < 16) continue;
                fprintf(out, "W %s %zu ", kv.first.c_str(), at);
                for (size_t i = 0; i < 64; ++i) fprintf(out, "%02x", d[at + i]);
                fprintf(out, "\n");
                ++windows;
                break;
            }
        }
        if (argc >= 5) {
            const bool isDx = d.size() > 4 && memcmp(d.data(), "DXBC", 4) == 0;
            if ((isDx && !plantedModule) || (!isDx && !plantedWeight && d.size() > 4096)) {
                const std::wstring dir(argv[4]);
                CreateDirectoryW(dir.c_str(), nullptr);
                FILE* f = nullptr;
                if (!_wfopen_s(&f, (dir + (isDx ? L"\\planted_module.bin" : L"\\planted_weight.bin")).c_str(), L"wb") && f) {
                    fwrite(d.data(), 1, d.size(), f);
                    fclose(f);
                }
                (isDx ? plantedModule : plantedWeight) = true;
            }
        }
    }
    fclose(out);
    printf("pack_samples: %zu files in the pack (%.1f MB), %zu Intel-derived, %zu windows written to %ls\n", rep.files,
           static_cast<double>(rep.bytes) / 1e6, intelFiles, windows, argv[3]);
    return 0;
}
