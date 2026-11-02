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
    uint64_t sourceFlags = 0;        // the source entry's flags field (logged once: the compression bits matter)
};

namespace detail
{
template <typename T> inline void Store(std::vector<uint8_t>& bytes, size_t at, T value)
{
    std::memcpy(bytes.data() + at, &value, sizeof(T));
}
// FATBIN_FLAG_COMPRESS_LZ4 / _ZSTD of the public layout: the new image is stored plain
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
} // namespace NvapiFatbin
