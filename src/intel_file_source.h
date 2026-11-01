// intel_file_source.h - the weight uploads of the multi-frame model, rebuilt from the player's own libxess_fg.dll FILE.
//
// The DLL is read as a file, never loaded and never run. Two kinds of data are in it:
//   * the int8 convolution weights as plain bytes, one blob per layer, in the layout the model was trained in;
//   * the per-channel scale and bias vectors, as byte-wise immediate stores in its start-up code (one
//     "mov byte [rsp+disp32], imm8" per data byte, consecutive displacements = one vector).
// What the recipe (recipe/i0/parts.json, key "decode", ours) carries is only OUR description plus hashes: per weight blob
// its length, SHA-256 and a short probe tag; per upload which layer family it is, its shape and tiling, which vector run
// it takes and in which form. Nothing of Intel's is stored: blobs are found by their hash, runs by the generic scan of
// the store pattern, and every rebuilt upload is checked against the SHA-256 the recipe lists for it.
//
// The root constants of the dispatches are not derived yet: RootConstants() asks an optional fallback source
// (DevCaptureSource when F5LOW_DEV_CAP is set) and otherwise fails with one clear line. A derived implementation drops in
// through SetRootConstantSource() or by replacing RootConstants().
#pragma once

#include <cstdint>
#include <string>
#include <vector>

#include "pack_build.h"

namespace packbuild {

class IntelFileSource : public IntelDataSource {
public:
    // `partsJson` = the bytes of the recipe's parts.json (the embedded "i0/parts.json").
    IntelFileSource(const std::wstring& libxessPath, const std::vector<uint8_t>& partsJson);
    ~IntelFileSource() override;

    std::string Id() const override { return "file"; }
    bool Upload(int ordinal, std::vector<uint8_t>& out) override;
    bool RootConstants(int index, std::vector<uint8_t>& out) override;
    std::string Problem() const override { return problem_; }

    // Where the root constants come from until they are derived from the file (not owned; may be null).
    void SetRootConstantSource(IntelDataSource* s) { rootSource_ = s; }

    // Reads the file and finds the weight blobs and the vector runs now (otherwise on the first Upload). False says why in
    // Problem(). Idempotent.
    bool Prepare();

    struct Stats {
        double readMs = 0;     // file read
        double blobsMs = 0;    // locating the weight blobs (probe scan + SHA-256 of each candidate)
        double runsMs = 0;     // scan for the immediate-store runs
        double decodeMs = 0;   // Upload() calls so far: tiling, widening, folding, SHA-256 check
        size_t blobs = 0, runs = 0, uploads = 0;
    };
    const Stats& GetStats() const { return stats_; }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    IntelDataSource* rootSource_ = nullptr;
    std::wstring path_;
    std::vector<uint8_t> parts_;
    std::string problem_;
    bool prepared_ = false, ok_ = false;
    Stats stats_;
};

}  // namespace packbuild
