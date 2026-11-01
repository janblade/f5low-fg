// intel_file_source.h - the weight uploads of the multi-frame model, rebuilt from the player's own libxess_fg.dll FILE.
//
// The DLL is read as a file, never loaded and never run. Two kinds of data are in it:
//   * the int8 convolution weights as plain bytes, one blob per layer, in the layout the model was trained in;
//   * the per-channel scale and bias vectors, as byte-wise immediate stores in its start-up code (one
//     "mov byte [rsp+disp32], imm8" per data byte, consecutive displacements = one vector).
// What the recipe (recipe/i0/parts.json, key "decode", ours) carries is only OUR description plus hashes: per weight blob
// its length, SHA-256 and a short probe tag; per upload which layer family it is, its shape and tiling, which vector run
// it takes and in which form. No Intel bytes are stored and no address, RVA or file offset appears anywhere: blobs are
// found by their hash, runs by the generic scan of the store encoding, and every rebuilt upload is checked against the
// SHA-256 the recipe lists for it. What the code does embed are interoperability facts, listed in docs/PACKLESS.md:
// the descriptor word order and count of a layer, the call-argument window they are stored in, the function-size
// filter of the layer-table scan, where the root-constant dwords go and the (sn<<8)|sp packing, and the generic x86
// encodings (mov byte/dword [rsp+disp], imm; lea reg,[rip+disp]; call) the scans recognise.
//
// The root constants of the 44 dispatches are derived too: the size words from the manifest's `set` formulas, the
// per-layer words (mulPos, shifts, channel chunk) from the layer table of the same DLL (found by a generic scan, no
// addresses), the rest from plain numbers of our description. Words the runtime patches every frame or that are never
// initialised are written 0 and left out of the recipe's hash (its "mask"). SetRootConstantSource() installs an explicit
// override (a PSO capture, development only).
#pragma once

#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "pack_build.h"

namespace packbuild {

class IntelFileSource : public IntelDataSource {
public:
    // `partsJson` = the bytes of the recipe's parts.json, `manifestTmpl` = its manifest.tmpl (size formulas of the root
    // constants); both are the embedded "i0/parts.json" and "i0/manifest.tmpl".
    IntelFileSource(const std::wstring& libxessPath, const std::vector<uint8_t>& partsJson, const std::vector<uint8_t>& manifestTmpl);
    ~IntelFileSource() override;
    IntelFileSource(const IntelFileSource&) = delete;
    IntelFileSource& operator=(const IntelFileSource&) = delete;
    // The caller has already read the file: use its bytes instead of reading it again (must outlive this source).
    void UseLibraryBytes(const std::vector<uint8_t>* bytes) override;

    std::string Id() const override { return "file"; }
    bool Upload(int ordinal, std::vector<uint8_t>& out) override;
    bool RootConstants(int index, std::vector<uint8_t>& out) override;
    std::string Problem() const override { return problem_; }

    // Overrides the derived root constants with another source (development; not owned; may be null).
    void SetRootConstantSource(IntelDataSource* s) { rootSource_ = s; }

    // Reads the file and finds the weight blobs and the vector runs now (otherwise on the first Upload). False says why in
    // Problem(). Idempotent.
    bool Prepare();

    struct Stats {
        double readMs = 0;     // file read
        double blobsMs = 0;    // locating the weight blobs (probe scan + SHA-256 of each candidate)
        double runsMs = 0;     // scan for the immediate-store runs
        double decodeMs = 0;   // Upload() calls so far: tiling, widening, folding, SHA-256 check
        double rcMs = 0;       // RootConstants() calls so far (layer-table scan on the first)
        size_t blobs = 0, runs = 0, uploads = 0, rcBlocks = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
    IntelDataSource* rootSource_ = nullptr;
    std::wstring path_;
    std::vector<uint8_t> parts_;
    std::string problem_;
    bool prepared_ = false, ok_ = false;
    Stats stats_;
};

}  // namespace packbuild
