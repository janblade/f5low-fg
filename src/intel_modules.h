// intel_modules.h - reads the shader modules out of the player's own libxess_fg.dll, at run time.
//
// The DLL is read as a FILE (never loaded). Its modules sit in the data sections as zstd frames: two zstd
// dictionaries are stored as ordinary frames, every module frame is written without the zstd magic and starts
// with its frame header descriptor followed by the id of the dictionary it was compressed with. Both are found
// by structure (the dictionary magic, then the dictionary id), so nothing here depends on where a build keeps
// them. A frame holding a DXIL module is a DXBC container without its 4-byte signature; it is restored and keyed by
// the SHA-256 of the restored container. Recipes refer to modules by that hash only.
//
// A file that is not such a DLL (other product, other layout) fails cleanly: Open returns false, Find returns null.
#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <string>
#include <vector>

namespace intel_modules {

using Sha256 = std::array<uint8_t, 32>;

bool HashBytes(const void* data, size_t n, Sha256& out);  // SHA-256 (Windows CNG)
std::string ToHex(const Sha256& h);
bool ParseHex(const std::string& hex, Sha256& out);  // 64 hex digits, either case

struct Stats {
    size_t dictionaries = 0;  // zstd dictionaries found
    size_t frames = 0;        // module frames that decoded
    size_t dxil = 0;          // of those: DXIL containers restored (equals the module count of the file)
    size_t rejected = 0;      // DXIL frames whose container size field did not match (not used)
    size_t distinct = 0;      // distinct container hashes
    double decodeMs = 0;      // wall time of Open (read + decode + hash)
};

class Library {
public:
    // Decodes `path`. On failure returns false and says why in `error` (may be null); the library stays empty.
    bool Open(const std::wstring& path, std::string* error = nullptr);
    bool OpenMemory(const uint8_t* data, size_t size, std::string* error = nullptr);

    // The restored DXBC container with this hash, or null.
    const std::vector<uint8_t>* Find(const Sha256& hash) const;
    const std::vector<uint8_t>* FindHex(const std::string& hex) const;

    const Stats& GetStats() const { return stats_; }
    const std::map<Sha256, std::vector<uint8_t>>& All() const { return modules_; }

private:
    std::map<Sha256, std::vector<uint8_t>> modules_;
    Stats stats_;
};

}  // namespace intel_modules
