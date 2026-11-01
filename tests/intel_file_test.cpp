// intel_file_test - rebuilds every weight upload of the recipe from a libxess_fg.dll FILE (src/intel_file_source.cpp) and
// checks each one against the SHA-256 the recipe lists for it (independently of the source's own check).
//
// Usage: intel_file_test.exe <libxess_fg.dll> <recipe\i0\parts.json> [--rounds N]
// Exit code: 0 all uploads match, 1 usage / unreadable, 2 at least one upload failed or does not match.
#include <windows.h>

#include <chrono>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "../src/intel_file_source.h"
#include "../src/intel_modules.h"
#include "../src/recipe_json.h"

using Clock = std::chrono::steady_clock;

int wmain(int argc, wchar_t** argv) {
    if (argc < 3) {
        printf("usage: intel_file_test <libxess_fg.dll> <recipe\\i0\\parts.json>\n");
        return 1;
    }
    std::ifstream f(argv[2], std::ios::binary);
    if (!f) {
        printf("cannot read the recipe\n");
        return 1;
    }
    const std::vector<uint8_t> parts((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    packbuild::Json root;
    if (!packbuild::ParseJson(parts, root) || !root.Get("data")) {
        printf("the recipe is not valid\n");
        return 1;
    }

    // manifest.tmpl next to parts.json
    std::wstring mpath(argv[2]);
    mpath = mpath.substr(0, mpath.find_last_of(L"\\/") + 1) + L"manifest.tmpl";
    std::ifstream mf(mpath, std::ios::binary);
    if (!mf) {
        printf("cannot read manifest.tmpl next to the recipe\n");
        return 1;
    }
    const std::vector<uint8_t> manifest((std::istreambuf_iterator<char>(mf)), std::istreambuf_iterator<char>());

    packbuild::IntelFileSource src(argv[1], parts, manifest);
    const auto t0 = Clock::now();
    if (!src.Prepare()) {
        printf("PREPARE FAILED: %s\n", src.Problem().c_str());
        return 2;
    }
    const double prepMs = std::chrono::duration<double, std::milli>(Clock::now() - t0).count();

    int total = 0, ok = 0;
    size_t bytes = 0;
    const auto t1 = Clock::now();
    for (const auto& kv : root.Get("data")->obj) {
        const packbuild::Json& d = kv.second;
        if (d.Str_("kind") != "intel_upload") continue;
        const int ord = static_cast<int>(d.Num_("ordinal"));
        ++total;
        std::vector<uint8_t> up;
        if (!src.Upload(ord, up)) {
            printf("upload %3d FAILED: %s\n", ord, src.Problem().c_str());
            continue;
        }
        up.resize(static_cast<size_t>(d.Num_("bytes")), 0);
        intel_modules::Sha256 h{};
        if (!intel_modules::HashBytes(up.data(), up.size(), h) || intel_modules::ToHex(h) != d.Str_("sha256")) {
            printf("upload %3d SHA-256 MISMATCH\n", ord);
            continue;
        }
        ++ok;
        bytes += up.size();
    }
    const double decMs = std::chrono::duration<double, std::milli>(Clock::now() - t1).count();

    // root constants: every block, hash taken with the recipe's masked dwords zeroed
    int rcTotal = 0, rcOk = 0, rcMasked = 0;
    const auto t2 = Clock::now();
    for (const auto& kv : root.Get("data")->obj) {
        const packbuild::Json& d = kv.second;
        if (d.Str_("kind") != "intel_const") continue;
        const int idx = static_cast<int>(d.Num_("capture_index"));
        ++rcTotal;
        std::vector<uint8_t> rc;
        if (!src.RootConstants(idx, rc)) {
            printf("root constants #%2d FAILED: %s\n", idx, src.Problem().c_str());
            continue;
        }
        rc.resize(static_cast<size_t>(d.Num_("dwords")) * 4, 0);
        const packbuild::Json* mask = d.Get("mask");
        if (mask) {
            ++rcMasked;
            for (const packbuild::Json& x : mask->arr) memset(rc.data() + x.num * 4, 0, 4);
        }
        intel_modules::Sha256 h{};
        if (!intel_modules::HashBytes(rc.data(), rc.size(), h) || intel_modules::ToHex(h) != d.Str_("sha256")) {
            printf("root constants #%2d SHA-256 MISMATCH\n", idx);
            continue;
        }
        ++rcOk;
    }
    const double rcMs = std::chrono::duration<double, std::milli>(Clock::now() - t2).count();
    const auto& st = src.GetStats();
    printf("%d/%d root-constant blocks match (%d exact + %d masked), %.1f ms\n", rcOk, rcTotal, rcOk - rcMasked, rcMasked, rcMs);
    printf("%d/%d uploads match the recipe's SHA-256 (%zu bytes)\n", ok, total, bytes);
    printf("prepare %.1f ms (read %.1f, weight blobs %.1f [%zu], vector runs %.1f [%zu]); decode of all uploads %.1f ms (in source %.1f)\n",
           prepMs, st.readMs, st.blobsMs, st.blobs, st.runsMs, st.runs, decMs, st.decodeMs);
    return ok == total && total > 0 && rcOk == rcTotal && rcTotal > 0 ? 0 : 2;
}
