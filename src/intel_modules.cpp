// intel_modules.cpp - see intel_modules.h. zstd is the vendored decompressor in third_party/zstd (BSD, built as C).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <bcrypt.h>

#define ZSTD_STATIC_LINKING_ONLY
#include "zstd.h"

#include <algorithm>
#include <chrono>
#include <cstring>

#include "intel_modules.h"

#pragma comment(lib, "bcrypt.lib")

namespace intel_modules {
namespace {

constexpr uint32_t kFrameMagic = 0xFD2FB528u;  // an ordinary zstd frame
constexpr uint32_t kDictMagic = 0xEC30A437u;   // the first word of a zstd dictionary
constexpr size_t kDictCap = size_t(16) << 20;
constexpr size_t kFrameCap = size_t(64) << 20;

struct Range {
    size_t begin, end;
};

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

// The raw data of every section that has any. Fails when the file is not a 64-bit PE image.
bool ParseSections(const uint8_t* d, size_t n, std::vector<Range>& out, std::string& why) {
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') { why = "not a PE image"; return false; }
    uint32_t pe = Rd32(d + 0x3C);
    if (pe > n || n - pe < 24 + 4 || memcmp(d + pe, "PE\0\0", 4) != 0) { why = "not a PE image"; return false; }
    uint16_t machine, nsec, optSize;
    memcpy(&machine, d + pe + 4, 2);
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&optSize, d + pe + 20, 2);
    if (machine != 0x8664) { why = "not a 64-bit PE image"; return false; }
    size_t tab = size_t(pe) + 24 + optSize;
    if (nsec == 0 || tab + size_t(nsec) * 40 > n) { why = "bad section table"; return false; }
    for (uint16_t i = 0; i < nsec; ++i) {
        const uint8_t* s = d + tab + size_t(i) * 40;
        uint32_t rawSize = Rd32(s + 16), rawPtr = Rd32(s + 20);
        if (rawSize == 0 || rawPtr >= n) continue;
        size_t end = std::min(n, size_t(rawPtr) + rawSize);
        out.push_back({rawPtr, end});
    }
    if (out.empty()) { why = "no section data"; return false; }
    return true;
}

// Every offset in [r.begin, r.end) where the 4 bytes `pat` start.
template <class F>
void ScanWord(const uint8_t* d, const Range& r, uint32_t pat, F&& hit) {
    uint8_t b[4];
    memcpy(b, &pat, 4);
    if (r.end - r.begin < 4) return;
    const uint8_t* p = d + r.begin;
    const uint8_t* last = d + r.end - 4;
    while (p <= last) {
        p = static_cast<const uint8_t*>(memchr(p, b[0], size_t(last - p) + 1));
        if (!p) return;
        if (p[1] == b[1] && p[2] == b[2] && p[3] == b[3]) hit(size_t(p - d));
        ++p;
    }
}

bool Contains(const std::vector<uint8_t>& v, const char* needle) {
    size_t n = strlen(needle);
    const uint8_t* p = v.data();
    const uint8_t* end = p + v.size();
    while (p + n <= end) {
        p = static_cast<const uint8_t*>(memchr(p, needle[0], size_t(end - p) - n + 1));
        if (!p) return false;
        if (memcmp(p, needle, n) == 0) return true;
        ++p;
    }
    return false;
}

// Decodes one frame at `src` (running to `avail` bytes) into `out`; the frame must end inside the input.
bool Inflate(ZSTD_DCtx* dctx, const uint8_t* src, size_t avail, size_t cap, std::vector<uint8_t>& out) {
    ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
    out.clear();
    out.resize(size_t(1) << 16);
    ZSTD_inBuffer in{src, avail, 0};
    ZSTD_outBuffer ob{out.data(), out.size(), 0};
    for (;;) {
        size_t r = ZSTD_decompressStream(dctx, &ob, &in);
        if (ZSTD_isError(r)) return false;
        if (r == 0) break;  // frame complete
        if (ob.pos == ob.size) {
            if (out.size() >= cap) return false;
            out.resize(std::min(cap, out.size() * 2));
            ob.dst = out.data();
            ob.size = out.size();
        } else if (in.pos == in.size) {
            return false;  // input ended inside the frame
        }
    }
    out.resize(ob.pos);
    return !out.empty();
}

struct Hasher {
    BCRYPT_ALG_HANDLE alg = nullptr;
    Hasher() {
        if (BCryptOpenAlgorithmProvider(&alg, BCRYPT_SHA256_ALGORITHM, nullptr, 0) < 0) alg = nullptr;
    }
    ~Hasher() {
        if (alg) BCryptCloseAlgorithmProvider(alg, 0);
    }
    bool operator()(const uint8_t* data, size_t n, Sha256& out) const {
        return alg && BCryptHash(alg, nullptr, 0, const_cast<PUCHAR>(data), ULONG(n), out.data(), ULONG(out.size())) >= 0;
    }
};

struct Dict {
    uint32_t id;
    ZSTD_DDict* ddict;
};

}  // namespace

bool HashBytes(const void* data, size_t n, Sha256& out) {
    Hasher hash;
    return hash(static_cast<const uint8_t*>(data), n, out);
}

std::string ToHex(const Sha256& h) {
    static const char* digits = "0123456789abcdef";
    std::string s;
    for (uint8_t b : h) { s += digits[b >> 4]; s += digits[b & 15]; }
    return s;
}

bool ParseHex(const std::string& hex, Sha256& out) {
    if (hex.size() != 64) return false;
    auto nib = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return -1;
    };
    for (size_t i = 0; i < 32; ++i) {
        int a = nib(hex[2 * i]), b = nib(hex[2 * i + 1]);
        if (a < 0 || b < 0) return false;
        out[i] = uint8_t(a * 16 + b);
    }
    return true;
}

bool Library::Open(const std::wstring& path, std::string* error) {
    modules_.clear();
    stats_ = Stats();
    std::string why;
    HANDLE f = CreateFileW(path.c_str(), GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE) {
        if (error) *error = "cannot open the file";
        return false;
    }
    LARGE_INTEGER sz{};
    std::vector<uint8_t> data;
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart > 0 && sz.QuadPart < (LONGLONG(1) << 31);
    if (ok) {
        data.resize(size_t(sz.QuadPart));
        size_t got = 0;
        while (got < data.size()) {
            DWORD n = 0;
            if (!ReadFile(f, data.data() + got, DWORD(std::min<size_t>(data.size() - got, 1u << 24)), &n, nullptr) || n == 0) break;
            got += n;
        }
        ok = got == data.size();
    }
    CloseHandle(f);
    if (!ok) {
        if (error) *error = "cannot read the file";
        return false;
    }
    return OpenMemory(data.data(), data.size(), error);
}

bool Library::OpenMemory(const uint8_t* d, size_t n, std::string* error) {
    modules_.clear();
    stats_ = Stats();
    auto t0 = std::chrono::steady_clock::now();
    auto fail = [&](const std::string& why) {
        modules_.clear();
        if (error) *error = why;
        return false;
    };
    std::vector<Range> secs;
    std::string why;
    if (!ParseSections(d, n, secs, why)) return fail(why);
    Hasher hash;
    if (!hash.alg) return fail("no SHA-256 provider");

    ZSTD_DCtx* dctx = ZSTD_createDCtx();
    if (!dctx) return fail("zstd: out of memory");
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1);

    // 1. the dictionaries: ordinary frames whose content starts with the dictionary magic
    std::vector<Dict> dicts;
    std::vector<uint8_t> buf;
    for (const Range& r : secs) {
        ScanWord(d, r, kFrameMagic, [&](size_t at) {
            if (!Inflate(dctx, d + at, r.end - at, kDictCap, buf)) return;
            if (buf.size() < 8 || Rd32(buf.data()) != kDictMagic) return;
            uint32_t id = Rd32(buf.data() + 4);
            for (const Dict& k : dicts)
                if (k.id == id) return;
            ZSTD_DDict* dd = ZSTD_createDDict(buf.data(), buf.size());
            if (dd) dicts.push_back({id, dd});
        });
    }
    stats_.dictionaries = dicts.size();

    // 2. the module frames: a descriptor byte (single segment, 4-byte dictionary id, 2- or 4-byte content size)
    // followed by the dictionary id; the frame has no magic
    ZSTD_DCtx_setParameter(dctx, ZSTD_d_format, ZSTD_f_zstd1_magicless);
    for (const Dict& dict : dicts) {
        for (const Range& r : secs) {
            ScanWord(d, r, dict.id, [&](size_t at) {
                if (at == r.begin) return;
                size_t start = at - 1;
                if (d[start] != 0x63 && d[start] != 0xA3) return;
                ZSTD_DCtx_reset(dctx, ZSTD_reset_session_only);
                if (ZSTD_isError(ZSTD_DCtx_refDDict(dctx, dict.ddict))) return;
                if (!Inflate(dctx, d + start, r.end - start, kFrameCap, buf)) return;
                ++stats_.frames;
                if (Contains(buf, "SPV_INTEL_vector_compute") || !Contains(buf, "DXIL")) return;  // not a DXIL module
                // the container without its signature: its size field (offset 24 of the container) counts the 4 bytes
                std::vector<uint8_t> box(4 + buf.size());
                memcpy(box.data(), "DXBC", 4);
                memcpy(box.data() + 4, buf.data(), buf.size());
                if (box.size() < 32 || Rd32(box.data() + 24) != box.size()) {
                    ++stats_.rejected;
                    return;
                }
                Sha256 h;
                if (!hash(box.data(), box.size(), h)) return;
                ++stats_.dxil;
                modules_.emplace(h, std::move(box));
            });
        }
    }
    ZSTD_freeDCtx(dctx);
    for (const Dict& k : dicts) ZSTD_freeDDict(k.ddict);

    stats_.distinct = modules_.size();
    stats_.decodeMs = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (modules_.empty()) return fail(dicts.empty() ? "no zstd dictionaries: not a known libxess_fg layout" : "no shader modules found");
    return true;
}

const std::vector<uint8_t>* Library::Find(const Sha256& hash) const {
    auto it = modules_.find(hash);
    return it == modules_.end() ? nullptr : &it->second;
}

const std::vector<uint8_t>* Library::FindHex(const std::string& hex) const {
    Sha256 h;
    return ParseHex(hex, h) ? Find(h) : nullptr;
}

}  // namespace intel_modules
