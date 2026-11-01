// translate.h - turns a shader module of the player's own libxess_fg.dll into a PTX kernel, at run time.
//
// Pipeline (all of it ours): the decoded module container (intel_modules) -> the disassembly text of Microsoft's
// dxcompiler.dll (dxc_disasm.h; the player's PC gets the DLL next to f5low.dll, it is never part of this repo)
// -> Dxil2Ptx -> PTX text. Two recipe transforms work on the way: the reconstruction edit rewrites the module's
// U-Net input stores before translation, and ClampVariant re-binds the accumulator clamp of a translated kernel.
//
// Translation is instruction for instruction (one PTX register per SSA value, the descriptor-table resource model
// below) so the kernel is as fast as the offline translation it replaces: the output text is byte for byte what the
// offline tool wrote for the same input (tests/translate_test.cpp checks that).
//
// Resources go through one descriptor table (kernel parameter p_desc): slot = class * 32 + register
// (class 0 SRV, 1 UAV, 2 CBV), 48 bytes each:
//   +0 u64 pointer   +8 u64 texture object (sampled SRVs)   +16 u32 width   +20 u32 height
//   +24 u32 slices   +28 u32 bytes per texel   +32 u32 total bytes   +36 u32 row pitch (bytes)
//   +40 u64 point texture object (Load at integer coordinates)
// Textures are rows of <row pitch> bytes, slice after slice. Out-of-range loads return zero and out-of-range
// stores are dropped, as in D3D.
#pragma once

#include <string>
#include <vector>

namespace translate {

struct Options {
    // Name printed in the first line of the PTX ("from <name>"); the offline tool printed the input file name.
    std::string sourceName = "module.dxasm";
    // A register's format when it is not 32 bits per component: "S2=rgba8", "U1=rg8i" (S = SRV, U = UAV); "x128",
    // "x64", "x32" name 32-bit components with 16, 8 or 4 bytes a texel (no conversion, one vector access).
    std::vector<std::string> fmts;
    std::string target = "sm_75";  // .target of the PTX
    // The remaining options are the offline tool's debugging switches; the defaults are what the recipe uses.
    std::string entry = "dxil_main";
    bool exact = false;               // no float contraction at all
    bool noFtz = false;
    std::string contract = "single";  // backend | single | any
    std::string prefer = "first";     // first | second: which fusable multiply of an add is fused
    std::string div = "approx";       // approx | full | rn | rcp
    int rgb10Frac = 14;               // fraction bits the float -> UNORM10 store keeps before rounding
    bool dce = true;
    bool vecShared = true;            // merge groupshared loads of four aligned words into one vector load
    bool distribute = true;           // (C - x) * K -> fma(-x, K, C * K) when C * K is exact
};

// Disassembly text -> PTX. On failure returns false with the reason in *err (the module uses something the
// translator does not know; never a crash).
bool Dxil2Ptx(const std::string& disassembly, const Options& opt, std::string& ptx, std::string* err);

// The reconstruction edit: rewrites the module's two U-Net input stores (raw buffer u0) into texture stores at the
// thread's own (x, y). Disassembly text in, disassembly text out.
bool ReconEdit(const std::string& disassembly, std::string& edited, std::string* err);

// Rewrites a translated accumulator-clamped module for another clamp width (bits, e.g. 17, 18, 19, 20).
bool ClampVariant(const std::string& ptx, int bits, std::string& out, std::string* err);

// Merges runs of four aligned groupshared loads into one vector load (the last step of Dxil2Ptx; exposed for tests).
std::vector<std::string> VectorizeShared(std::vector<std::string> lines, size_t* merged, size_t* loads);

// One recipe entry: transform is "plain", "recon" or "clamp:<bits>" (recipe/i0/parts.json).
bool TranslateEntry(const std::string& disassembly, const std::string& transform, const std::vector<std::string>& fmts,
                    const std::string& sourceName, std::string& ptx, std::string* err);

}  // namespace translate
