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

    packbuild::IntelFileSource src(argv[1], parts);
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
    const auto& st = src.GetStats();
    printf("%d/%d uploads match the recipe's SHA-256 (%zu bytes)\n", ok, total, bytes);
    printf("prepare %.1f ms (read %.1f, weight blobs %.1f [%zu], vector runs %.1f [%zu]); decode of all uploads %.1f ms (in source %.1f)\n",
           prepMs, st.readMs, st.blobsMs, st.blobs, st.runsMs, st.runs, decMs, st.decodeMs);
    return ok == total && total > 0 ? 0 : 2;
}
