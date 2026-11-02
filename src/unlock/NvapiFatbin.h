// unlock/NvapiFatbin.h - the kernel images of f5low's private DLSS-G, rewritten for the card (host-testable: no Windows, no GPU).
//
// DLSS-G creates its ~80 compute kernels through NVAPI (NvapiKernels.h), each from a CUDA fatbin container that holds PTX for
// Ada/Blackwell (and an Ada ELF in some). RTX 20/30 cards cannot use any of those images, so before the driver sees one the container
// is replaced by a new one with a single, uncompressed PTX image for the card:
//   Ampere (sm_86): the source PTX with its ".target" lowered to sm_86
//   Turing (sm_75): the same, run through the sm_75 rewrite (PtxTuring.h: mma k16 -> 2x k8, f16 min/max, cvt.rn.f16x2.f32)
// The container is built from the shipped one's own headers (format: the public CUDA fatbin layout PtxTuring.h already reads), with the
// compression off and the architecture field set. Our code only; nothing of the kernels is stored in this repo.
#pragma once

#include "PtxTuring.h"

#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace NvapiFatbin
{
struct Built
{
    bool ok = false;
    std::vector<uint8_t> image;      // the new container
    std::string failure;
    int sourceArch = 0;
    unsigned mma = 0, minMax = 0, cvt = 0; // Turing rewrite counters
    uint64_t sourceFlags = 0;        // the source entry's flags field (logged once)
    bool sourceCompressed = false;   // the source entry was LZ4-compressed (logged with the flags: how the flag bit was confirmed)
};

namespace detail
{
template <typename T> inline void Store(std::vector<uint8_t>& bytes, size_t at, T value)
{
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}
// The entry-flag bits that say "payload is compressed". Observed (f5low.log, "first container" line): the shipped LZ4-compressed PTX
// entries of DLSS-G 310.9 carry flags 0x2041, with 0x2000 set and 0x1000 clear, so 0x2000 is the LZ4 bit here. 0x1000 is cleared as
// well only as a precaution (the bit the public fatbin layouts use for the other codec); it was never seen set. A new entry is stored
// plain, with the compressed and uncompressed size fields 0, which is also what the driver accepted on the forced-tier runs.
constexpr uint64_t kCompressionFlags = 0x1000 | 0x2000;
} // namespace detail

// One container with one PTX image: `ptx` for architecture `arch`, from the headers of `source`.
inline std::vector<uint8_t> MakeContainer(const MfgUnlock::PtxTuring::Source& source, const std::string& ptx, uint32_t arch)
{
    using namespace detail;
    // NUL-terminated, padded to 8
    const size_t payload = (ptx.size() + 1 + 7) & ~static_cast<size_t>(7);
    std::vector<uint8_t> out(source.header.size() + source.entryHeader.size() + payload, 0);
    std::memcpy(out.data(), source.header.data(), source.header.size());
    std::memcpy(out.data() + source.header.size(), source.entryHeader.data(), source.entryHeader.size());
    Store<uint64_t>(out, 8, source.entryHeader.size() + payload); // fatSize
    const size_t e = source.header.size();
    Store<uint64_t>(out, e + 8, payload);                 // payload size
    Store<uint32_t>(out, e + 16, 0);                      // compressed size: 0 = stored plain
    Store<uint32_t>(out, e + 0x1C, arch);
    uint64_t flags = 0;
    std::memcpy(&flags, out.data() + e + 0x28, sizeof(flags));
    flags &= ~kCompressionFlags;
    Store<uint64_t>(out, e + 0x28, flags);
    Store<uint64_t>(out, e + 0x38, 0);                    // uncompressed size: none
    std::memcpy(out.data() + e + source.entryHeader.size(), ptx.data(), ptx.size());
    return out;
}

// The container for card `targetSm` (86: Ampere, 75: Turing) from the shipped container `image` of `size` bytes.
inline Built Rewrite(const uint8_t* image, size_t size, int targetSm)
{
    Built built;
    if (targetSm != 86 && targetSm != 75)
    {
        built.failure = "no rewrite for this architecture";
        return built;
    }
    MfgUnlock::PtxTuring::Source source = MfgUnlock::PtxTuring::ExtractSourcePtx(image, true, size);
    if (!source.ok)
    {
        built.failure = source.failure;
        return built;
    }
    built.sourceArch = source.sourceArch;
    built.sourceCompressed = source.sourceCompressed;
    if (source.entryHeader.size() >= 0x30)
        std::memcpy(&built.sourceFlags, source.entryHeader.data() + 0x28, sizeof(built.sourceFlags));
    if (!MfgUnlock::PtxTuring::RetargetTo(source.ptx, targetSm == 86 ? "86" : "75", built.failure))
        return built;
    if (targetSm == 75)
    {
        MfgUnlock::PtxTuring::Rewrite r = MfgUnlock::PtxTuring::RewriteText(source.ptx);
        if (!r.ok)
        {
            built.failure = r.failure;
            return built;
        }
        built.mma = r.mma;
        built.minMax = r.minMaxVec + r.minMaxScalar;
        built.cvt = r.cvt;
        source.ptx = std::move(r.ptx);
    }
    built.image = MakeContainer(source, source.ptx, static_cast<uint32_t>(targetSm));
    built.ok = true;
    return built;
}

// ---- the create call's decision flow (host-testable: the driver call and the handle check come in as callables) ------------------------
struct Attempt
{
    int status = 0;          // what the caller gets back
    bool rewritten = false;  // the driver took our image
    bool failed = false;     // the container was left as shipped (or its create failed), `why` says why
    bool retried = false;    // the real create was called a second time, with the shipped image
    std::string why;
};

// `haveMade`: the rewritten container exists (else `why` is the reason). real(true) calls the driver's create with OUR image, real(false)
// with the shipped one. snapshot()/written(before) tell whether the call wrote the output handle. The real create is called a second
// time ONLY when the first call returned an error and did not write the handle: a call that wrote a handle (even with an error) is
// never repeated, so no handle is created and lost.
template <class Real, class Snapshot, class Written>
inline Attempt CreateWithFallback(bool haveMade, std::string why, Real&& real, Snapshot&& snapshot, Written&& written)
{
    Attempt a;
    if (!haveMade)
    {
        a.failed = true;
        a.why = std::move(why);
        a.status = real(false);
        return a;
    }
    const uint64_t before = snapshot();
    a.status = real(true);
    if (a.status == 0)
    {
        a.rewritten = true;
        return a;
    }
    a.failed = true;
    a.why = "the driver refused the rewritten image (status " + std::to_string(a.status) + ")";
    if (written(before))
    {
        a.why += "; it wrote the handle, so the create was not repeated";
        return a;
    }
    a.retried = true;
    a.status = real(false);
    return a;
}
} // namespace NvapiFatbin