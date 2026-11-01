// pack_build.h - builds the own generator's graph pack on the player's PC (story B2 of the pack-less epic).
//
// What goes in:
//   * OUR part, embedded in f5low.dll at build time (resource 102, tools/recipe/build_own.py): the recipe
//     (manifest.tmpl + parts.json, SDR and hdr\), the kernels we generate (own_gen / own_tpl) and the noise texture.
//   * Intel's part from the player's PC: the shader modules of their libxess_fg.dll (intel_modules, found by SHA-256):
//     the 16 modules per pack that run natively on D3D12 are copied as DXBC; weights and root constants come from an
//     IntelDataSource (below). Nothing is translated: the pack is for the D3D12 path only (the PTX twins of the native
//     modules, which only the CUDA path would run, are left out; see kD3d12OnlyMarker).
// What comes out: a pack folder in the layout xefg_pack.h reads (manifest.txt, kernels, data\), written like
// tools/recipe/build_pack.py does in its neutral-name mode, minus the PTX twins and plus the marker file.
//
// Nothing Intel-derived is ever part of f5low.dll or the repo; the built pack lives in the cache folder
// (<DLL dir>\f5low_cache\<key>\) on the player's PC only.
#pragma once

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace packbuild {

// A file in the pack's root folder: the pack was built on this PC and has no PTX file for the kernels that run as a native
// module, so it runs on the D3D12 path only (xefg_pack.h Kernel::nativeOnly). Explicit packs (a pack folder, the
// embedded pack) have no marker and keep both paths.
inline constexpr const char* kD3d12OnlyMarker = "d3d12_only";

// The weights and root constants the recipe refers to (parts.json: intel_upload by ordinal, intel_const by dispatch
// index). The builder checks every blob against the recipe's SHA-256, so a wrong source fails by naming the part.
// IntelFileSource (intel_file_source.h) rebuilds every weight upload and every root-constant block from the player's
// libxess_fg.dll file; DevCaptureSource below reads an offline capture (development only).
class IntelDataSource {
public:
    virtual ~IntelDataSource() = default;
    // Short identifier of the source kind, for the log ("dev", "file"). Not part of the cache key.
    virtual std::string Id() const = 0;
    // BuildPack has already read the libxess_fg file: a source that reads it too may use these bytes (valid for the
    // whole build) instead of reading it again.
    virtual void UseLibraryBytes(const std::vector<uint8_t>*) {}
    // The weight upload with this ordinal (recipe `ordinal`), its exact bytes before padding.
    virtual bool Upload(int ordinal, std::vector<uint8_t>& out) = 0;
    // The root constants of dispatch `index` (recipe `capture_index`): little-endian dwords as bytes.
    virtual bool RootConstants(int index, std::vector<uint8_t>& out) = 0;
    // Why the last call failed / what is missing, for the log.
    virtual std::string Problem() const { return {}; }
};

// Offline capture: a weight dump folder (u0001_*.bin ...) and the PSO capture log with the model's dispatches
// ("D <n> t<id> list=... g=x,y,z r<k>=<hex dwords>"), the files tools/recipe/build_pack.py reads.
class DevCaptureSource : public IntelDataSource {
public:
    DevCaptureSource(const std::wstring& weightsDir, const std::wstring& capLog);
    // Root constants only (the weights come from elsewhere): Upload() then always fails.
    explicit DevCaptureSource(const std::wstring& capLog) : capLog_(capLog) {}
    std::string Id() const override { return "dev"; }
    bool Upload(int ordinal, std::vector<uint8_t>& out) override;
    bool RootConstants(int index, std::vector<uint8_t>& out) override;
    std::string Problem() const override { return problem_; }

private:
    std::map<int, std::wstring> uploads_;
    std::wstring capLog_;
    std::vector<std::vector<uint32_t>> rc_;  // loaded on first use, as far as the recipe asks
    bool rcDone_ = false;
    std::string problem_;
};

// The embedded bundle of build_own.py: name -> bytes ("i0/manifest.tmpl", "i0/hdr/k01_....ptx", ...).
using Bundle = std::map<std::string, std::vector<uint8_t>>;
bool ParseBundle(const uint8_t* data, size_t size, Bundle& out, std::string* error);

struct Report {
    double decodeMs = 0;      // libxess_fg: read, hash, decode its modules
    double weightsMs = 0;     // weights and root constants from the data source (derived, hash-checked, written)
    double writeMs = 0;       // everything else: our kernels, native modules, manifests, files on disk
    size_t native = 0;        // intel_native modules copied
    size_t skipped = 0;       // intel_xlate entries (PTX twins of native modules) left out
    size_t files = 0;         // files written
    std::string intelSha256;  // of the libxess_fg file
};

// Builds the pack into `outDir` (created; existing files are overwritten). False with `error` naming what is missing
// or does not match (a module the build does not know, a weight with the wrong SHA-256, ...).
// `libBytes`: the libxess_fg file when the caller has read it already (used instead of reading it again).
bool BuildPack(const std::wstring& libxessPath, const Bundle& own, IntelDataSource& data, const std::wstring& outDir,
               Report& rep, std::string* error, const std::vector<uint8_t>* libBytes = nullptr);

// The cache: `<cacheRoot>\<key>\` holds a finished pack and a `complete` marker.
struct Request {
    std::wstring libxessPath;
    std::wstring cacheRoot;                 // <DLL dir>\f5low_cache
    std::wstring fallbackRoot;              // optional: used when cacheRoot cannot be written (%LOCALAPPDATA%\f5low\f5low_cache)
    const Bundle* own = nullptr;
    const uint8_t* bundleBytes = nullptr;   // the embedded bundle as it is (cache key)
    size_t bundleSize = 0;
    IntelDataSource* data = nullptr;
    std::function<void(const std::string&)> log;
};

enum class Outcome { Cached, Built, Failed };

// Returns Cached (folder reused, no work), Built (made now) or Failed (`error` says what is missing). `dir` is the
// pack folder on success (an ordinary path, not the \\?\ form). The key is the SHA-256 of the libxess_fg file, the bundle
// (recipe + our kernels) and kFormatVersion (pack_build.cpp; raise it on any builder change), the version of how this builder lays a pack out. Every file operation works on
// paths longer than MAX_PATH. Stale ".tmp" folders of killed builds are removed before a build.
Outcome EnsurePack(const Request& req, std::wstring& dir, std::string& error, Report* rep = nullptr);

// Deletes a cache folder that EnsurePack handed out (it failed to load later). Only a folder inside a "f5low_cache" folder
// that carries our `complete` marker is deleted. False when it was not deleted.
bool DiscardPack(const std::wstring& dir);

}  // namespace packbuild
