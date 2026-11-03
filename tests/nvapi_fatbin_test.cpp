// Host test (no GPU, no NVIDIA file): NvapiFatbin.h on synthetic CUDA fatbin containers - the rewrite that DLSS-G's NVAPI kernel images get
// on RTX 20/30 (src/unlock/NvapiFatbin.h). Containers are built here in the public layout: 16-byte container header, entries of a
// 0x40-byte header (kind, header size, payload, compressed size, arch at +0x1C, flags at +0x28, uncompressed size at +0x38) plus payload.
#include "../src/unlock/NvapiFatbin.h"

#include <cstdio>
#include <string>
#include <vector>

using Bytes = std::vector<uint8_t>;
using MfgUnlock::PtxTuring::kFatbinMagic;

static int g_fail = 0;
#define CHECK(c)                                                                  \
    do                                                                            \
    {                                                                             \
        if (!(c))                                                                 \
        {                                                                         \
            std::printf("FAIL line %d: %s\n", __LINE__, #c);                      \
            ++g_fail;                                                             \
        }                                                                         \
    } while (0)

template <class T> static void Put(Bytes& b, size_t at, T v) { std::memcpy(b.data() + at, &v, sizeof(T)); }
template <class T> static T Get(const Bytes& b, size_t at)
{
    T v;
    std::memcpy(&v, b.data() + at, sizeof(T));
    return v;
}

// LZ4 block holding the text as one literal run (valid LZ4: a last sequence with literals only)
static Bytes Lz4Literals(const std::string& text)
{
    Bytes out;
    size_t n = text.size();
    if (n < 15)
        out.push_back(static_cast<uint8_t>(n << 4));
    else
    {
        out.push_back(0xF0);
        size_t rest = n - 15;
        while (rest >= 255)
        {
            out.push_back(255);
            rest -= 255;
        }
        out.push_back(static_cast<uint8_t>(rest));
    }
    out.insert(out.end(), text.begin(), text.end());
    return out;
}

static Bytes Entry(uint16_t kind, uint32_t arch, const std::string& text, bool compressed)
{
    Bytes payload;
    if (compressed)
        payload = Lz4Literals(text);
    else
        payload.assign(text.begin(), text.end());
    payload.push_back(0);
    while (payload.size() % 8)
        payload.push_back(0);
    Bytes e(0x40 + payload.size(), 0);
    Put<uint16_t>(e, 0, kind);
    Put<uint16_t>(e, 2, 0x0101);
    Put<uint32_t>(e, 4, 0x40);
    Put<uint64_t>(e, 8, payload.size());
    Put<uint32_t>(e, 16, compressed ? static_cast<uint32_t>(Lz4Literals(text).size()) : 0);
    Put<uint32_t>(e, 0x1C, arch);
    Put<uint64_t>(e, 0x28, 0x1 | (compressed ? 0x1000u : 0u));
    Put<uint64_t>(e, 0x38, compressed ? text.size() : 0);
    std::memcpy(e.data() + 0x40, payload.data(), payload.size());
    return e;
}

static Bytes Container(const std::vector<Bytes>& entries)
{
    Bytes c(16, 0);
    Put<uint32_t>(c, 0, kFatbinMagic);
    Put<uint16_t>(c, 4, 1);
    Put<uint16_t>(c, 6, 0x10);
    size_t total = 0;
    for (const Bytes& e : entries)
    {
        c.insert(c.end(), e.begin(), e.end());
        total += e.size();
    }
    Put<uint64_t>(c, 8, total);
    return c;
}

static std::string Ptx(const char* target)
{
    return std::string(".version 8.7\n.target ") + target + "\n.address_size 64\n.visible .entry k()\n{\n"
           ".reg .b32 %r<16>;\n"
           "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%r1, %r2}, {%r3, %r4, %r5, %r6}, {%r7, %r8}, {%r9, %r10};\n"
           "{min.f16x2 %r11, %r12, %r13;}\n"
           "{ cvt.rn.f16x2.f32 %r14, %r15, %r1; }\n"
           "ret;\n}\n";
}

// Reads a built container back: counts and the single image
struct Parsed
{
    unsigned entries = 0, ptx = 0;
    uint32_t arch = 0, compressed = 99;
    uint64_t flags = 0, uncompressed = 99, payload = 0;
    std::string text;
    bool sizesConsistent = false;
};
static Parsed Parse(const Bytes& c)
{
    Parsed p;
    CHECK(c.size() >= 16 + 0x40);
    if (c.size() < 16 + 0x40)
        return p;
    CHECK(Get<uint32_t>(c, 0) == kFatbinMagic);
    const uint64_t fat = Get<uint64_t>(c, 8);
    p.sizesConsistent = fat + 16 == c.size();
    size_t at = 16;
    while (at + 0x40 <= c.size())
    {
        ++p.entries;
        const uint32_t hs = Get<uint32_t>(c, at + 4);
        p.payload = Get<uint64_t>(c, at + 8);
        if (Get<uint16_t>(c, at) == 1)
        {
            ++p.ptx;
            p.arch = Get<uint32_t>(c, at + 0x1C);
            p.compressed = Get<uint32_t>(c, at + 16);
            p.flags = Get<uint64_t>(c, at + 0x28);
            p.uncompressed = Get<uint64_t>(c, at + 0x38);
            p.text.assign(reinterpret_cast<const char*>(c.data() + at + hs));
        }
        at += hs + p.payload;
    }
    p.sizesConsistent = p.sizesConsistent && at == c.size() && p.payload % 8 == 0;
    return p;
}

static size_t Count(const std::string& s, const char* what)
{
    size_t n = 0;
    for (size_t at = 0; (at = s.find(what, at)) != std::string::npos; ++at)
        ++n;
    return n;
}

int main()
{
    // A: compressed Ada PTX + Ada ELF + uncompressed Blackwell PTX (the Blackwell one wins) -> Ampere
    {
        const Bytes c = Container({ Entry(1, 89, Ptx("sm_89"), true), Entry(2, 89, "ELFELFELF", false), Entry(1, 120, Ptx("sm_120"), false) });
        const NvapiFatbin::Built b = NvapiFatbin::Rewrite(c.data(), c.size(), 86);
        CHECK(b.ok || (std::printf("failure: %s\n", b.failure.c_str()), false));
        CHECK(b.sourceArch == 120);
        const Parsed p = Parse(b.image);
        CHECK(p.entries == 1 && p.ptx == 1);
        CHECK(p.arch == 86 && p.compressed == 0 && p.uncompressed == 0 && (p.flags & 0x3000) == 0 && (p.flags & 1) == 1);
        CHECK(p.sizesConsistent);
        CHECK(p.text.find(".target sm_86\n") != std::string::npos && p.text.find("sm_120") == std::string::npos);
        CHECK(p.text.find("m16n8k16") != std::string::npos); // Ampere keeps the k16 mma
        // the built container reads back as a retargeted one
        const auto again = MfgUnlock::PtxTuring::ExtractSourcePtx(b.image.data(), true, b.image.size());
        CHECK(again.ok && again.sourceArch == 86 && again.ptx == p.text.substr(0, again.ptx.size()));
        // and the strict (CUDA-path) reader refuses it: it is not a shipped container
        CHECK(!MfgUnlock::PtxTuring::ExtractSourcePtx(b.image.data(), false).ok);
    }
    // B: compressed Ada PTX only -> Turing
    {
        const Bytes c = Container({ Entry(1, 89, Ptx("sm_89"), true) });
        const NvapiFatbin::Built b = NvapiFatbin::Rewrite(c.data(), c.size(), 75);
        CHECK(b.ok || (std::printf("failure: %s\n", b.failure.c_str()), false));
        CHECK(b.sourceArch == 89);
        const Parsed p = Parse(b.image);
        CHECK(p.entries == 1 && p.ptx == 1 && p.arch == 75 && p.compressed == 0 && p.sizesConsistent);
        CHECK(p.text.find(".target sm_75\n") != std::string::npos);
        CHECK(p.text.find("m16n8k16") == std::string::npos);
        CHECK(Count(p.text, "mma.sync.aligned.m16n8k8.row.col.f16.f16.f16.f16") == 2);
        CHECK(p.text.find("min.f16x2") == std::string::npos && p.text.find("cvt.rn.f16x2.f32") == std::string::npos);
        CHECK(b.mma == 1 && b.minMax == 1 && b.cvt == 1);
    }
    // C: a container the Ampere family already retargeted in memory (arch 86 source, parked 122 images) -> Ampere
    {
        const Bytes c = Container({ Entry(1, 86, Ptx("sm_86"), true), Entry(2, 122, "ELFELFELF", false), Entry(1, 122, Ptx("sm_120"), false) });
        const NvapiFatbin::Built b = NvapiFatbin::Rewrite(c.data(), c.size(), 86);
        CHECK(b.ok || (std::printf("failure: %s\n", b.failure.c_str()), false));
        const Parsed p = Parse(b.image);
        CHECK(p.entries == 1 && p.arch == 86 && p.text.find(".target sm_86\n") != std::string::npos);
    }
    // D: failures are reported, not crashed on
    {
        Bytes c = Container({ Entry(1, 120, Ptx("sm_120"), false), Entry(1, 120, Ptx("sm_120"), false) });
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok); // two source PTX
        c = Container({ Entry(1, 89, Ptx("sm_89"), false) });
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size() - 8, 86).ok); // truncated
        Put<uint32_t>(c, 0, 0x12345678);
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok); // not a container
        Put<uint32_t>(c, 0, kFatbinMagic);
        CHECK(NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok);
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size(), 89).ok); // no rewrite for sm_89
        std::string noTarget = Ptx("sm_89");
        noTarget.replace(noTarget.find(".target"), 7, ".tarxet");
        c = Container({ Entry(1, 89, noTarget, false) });
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok);
        c = Container({ Entry(1, 89, Ptx("sm_89") + "{ max.bf16x2 %r1, %r2, %r3; }\n", false) });
        CHECK(NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok);   // Ampere has bf16 max
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size(), 75).ok);  // Turing fails closed on a form it cannot rewrite
        // a corrupt LZ4 stream
        c = Container({ Entry(1, 89, Ptx("sm_89"), true) });
        Put<uint64_t>(c, 16 + 0x38, 1u << 20); // claims a much longer text than the stream holds
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok);
    }
    // E: the strict reader (CUDA path) still takes a shipped container
    {
        const Bytes c = Container({ Entry(1, 89, Ptx("sm_89"), true), Entry(1, 120, Ptx("sm_120"), true) });
        const auto r = MfgUnlock::PtxTuring::BuildFromContainer(c.data());
        CHECK(r.ok && r.sourceArch == 120 && r.mma == 1);
    }
    // F: RetargetTo on the forms the Ampere family leaves: the padded ".target sm_86 " (it replaced ".target sm_120") and a plain one
    {
        std::string why;
        std::string padded = ".version 8.7\n.target sm_86 \n.address_size 64\n";
        CHECK(MfgUnlock::PtxTuring::RetargetTo(padded, "86", why) && padded == ".version 8.7\n.target sm_86 \n.address_size 64\n");
        std::string from120 = ".version 8.7\n.target sm_120 \n.address_size 64\n";
        CHECK(MfgUnlock::PtxTuring::RetargetTo(from120, "86", why) && from120.find(".target sm_86 \n") != std::string::npos);
        std::string to75 = ".version 8.7\n.target sm_86 \n";
        CHECK(MfgUnlock::PtxTuring::RetargetTo(to75, "75", why) && to75 == ".version 8.7\n.target sm_75 \n");
        std::string twice = ".target sm_89\n.target sm_89\n";
        CHECK(!MfgUnlock::PtxTuring::RetargetTo(twice, "86", why));
        // a padded container as the Ampere family leaves it (source PTX text with the trailing space) builds
        const Bytes c = Container({ Entry(1, 86, ".version 8.7\n.target sm_86 \n.address_size 64\n.visible .entry k()\n{\nret;\n}\n", false), Entry(2, 122, "ELF", false) });
        CHECK(NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok);
    }
    // G: a size that would overflow `fatSize + 16` is refused, not wrapped
    {
        Bytes c = Container({ Entry(1, 89, Ptx("sm_89"), false) });
        Put<uint64_t>(c, 8, ~0ull - 8); // fatSize near 2^64
        CHECK(!NvapiFatbin::Rewrite(c.data(), c.size(), 86).ok);
        c = Container({ Entry(1, 89, Ptx("sm_89"), false) });
        CHECK(!NvapiFatbin::Rewrite(c.data(), 8, 86).ok); // smaller than a container header
    }
    // H: the create flow with a fake driver call: when the repeat with the shipped image happens, and when it must not
    {
        int calls = 0, lastOurs = -1;
        uint64_t handle = 0;
        const auto snapshot = [&] { return handle; };
        const auto written = [&](uint64_t before) { return handle != before; };
        // 1. our image accepted: one call, no repeat
        auto a = NvapiFatbin::CreateWithFallback(true, "", [&](bool ours) { ++calls; lastOurs = ours; handle = 7; return 0; }, snapshot, written);
        CHECK(a.status == 0 && a.rewritten && !a.failed && !a.retried && calls == 1 && lastOurs == 1);
        // 2. no rewritten image: one call with the shipped image, counted as failed with the reason
        calls = 0;
        a = NvapiFatbin::CreateWithFallback(false, "no single source PTX", [&](bool ours) { ++calls; lastOurs = ours; return 0; }, snapshot, written);
        CHECK(a.status == 0 && !a.rewritten && a.failed && !a.retried && calls == 1 && lastOurs == 0 && a.why == "no single source PTX");
        // 3. our image refused without writing the handle: repeated once with the shipped one, whose status is returned
        calls = 0;
        handle = 0;
        a = NvapiFatbin::CreateWithFallback(true, "", [&](bool ours) { ++calls; lastOurs = ours; return ours ? -5 : 0; }, snapshot, written);
        CHECK(a.status == 0 && !a.rewritten && a.failed && a.retried && calls == 2 && lastOurs == 0 && a.why.find("-5") != std::string::npos);
        // 4. our image refused but the handle was written: NOT repeated (no handle is lost), the error goes back
        calls = 0;
        handle = 0;
        a = NvapiFatbin::CreateWithFallback(true, "", [&](bool ours) { ++calls; lastOurs = ours; handle = 9; return -5; }, snapshot, written);
        CHECK(a.status == -5 && a.failed && !a.retried && calls == 1 && lastOurs == 1);
        // 5. both refuse: the second status is returned
        calls = 0;
        handle = 0;
        a = NvapiFatbin::CreateWithFallback(true, "", [&](bool ours) { ++calls; return ours ? -5 : -7; }, snapshot, written);
        CHECK(a.status == -7 && a.failed && a.retried && calls == 2);
    }
    if (g_fail)
    {
        std::printf("nvapi_fatbin_test: %d FAILED\n", g_fail);
        return 1;
    }
    std::printf("nvapi_fatbin_test: PASS\n");
    return 0;
}
