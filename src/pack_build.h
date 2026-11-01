// pack_build.h - builds the own generator's graph pack on the player's PC (story B2 of the pack-less epic).
//
// What goes in:
//   * OUR part, embedded in f5low.dll at build time (resource 102, tools/recipe/build_own.py): the recipe
//     (manifest.tmpl + parts.json, SDR and hdr\), the kernels we generate (own_gen / own_tpl) and the noise texture.
//   * Intel's part from the player's PC: the shader modules of their libxess_fg.dll (intel_modules, found by SHA-256):
//     16 run natively as DXBC (copied), 10 modules / 26 entries are translated in-process (dxc_disasm + translate,
//     needs the player's dxcompiler.dll); weights and root constants from an IntelDataSource (below).
// What comes out: a pack folder in the layout xefg_pack.h reads (manifest.txt, kernels, data\), written like
// tools/recipe/build_pack.py does in its neutral-name mode, byte for byte.
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

// The weights and root constants the recipe refers to (parts.json: intel_upload by ordinal, intel_const by dispatch
// index). The builder checks every blob against the recipe's SHA-256, so a wrong source fails by naming the part.
// IntelFileSource (intel_file_source.h) rebuilds every weight upload from the player's libxess_fg.dll file;
// DevCaptureSource below reads an offline capture (development only, and the stand-in for the root constants until
// they are derived too).
class IntelDataSource {
public:
    virtual ~IntelDataSource() = default;
    // Short identifier of the source kind, for the log ("dev", later "decoder-1"). Not part of the cache key.
    virtual std::string Id() const = 0;
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
    double translateMs = 0;   // dxcompiler disassembly + translation of the intel_xlate entries
    double writeMs = 0;       // everything else: own kernels, native modules, data, manifests, files on disk
    size_t translated = 0;    // intel_xlate entries translated (identical ones made once)
    size_t native = 0;        // intel_native modules copied
    size_t files = 0;         // files written
    std::string dxcVersion;
    std::string intelSha256;  // of the libxess_fg file
};

// Builds the pack into `outDir` (created; existing files are overwritten). False with `error` naming what is missing
// or does not match (a module the build does not know, a weight with the wrong SHA-256, no dxcompiler, ...).
bool BuildPack(const std::wstring& libxessPath, const std::wstring& dxcompilerPath, const Bundle& own,
               IntelDataSource& data, const std::wstring& outDir, Report& rep, std::string* error);

// The cache: `<cacheRoot>\<key>\` holds a finished pack and a `complete` marker.
struct Request {
    std::wstring libxessPath;
    std::wstring dxcompilerPath;
    std::wstring cacheRoot;                 // <DLL dir>\f5low_cache
    const Bundle* own = nullptr;
    const uint8_t* bundleBytes = nullptr;   // the embedded bundle as it is (cache key)
    size_t bundleSize = 0;
    IntelDataSource* data = nullptr;
    std::function<void(const std::string&)> log;
};

enum class Outcome { Cached, Built, Failed };

// Returns Cached (folder reused, no work), Built (made now) or Failed (`error` says what is missing). `dir` is the
// pack folder on success. The key is the SHA-256 of the libxess_fg file, the bundle (recipe + our kernels) and the
// dxcompiler version. Stale ".tmp" folders of killed builds are removed before a build.
Outcome EnsurePack(const Request& req, std::wstring& dir, std::string& error, Report* rep = nullptr);

}  // namespace packbuild
