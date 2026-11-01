// intel_file_source.cpp - see intel_file_source.h. docs/PACKLESS.md has the overview of what is rebuilt and how.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include "intel_file_source.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <fstream>
#include <map>

#include "intel_modules.h"
#include "recipe_json.h"

namespace packbuild {
namespace {

using Clock = std::chrono::steady_clock;
double MsSince(Clock::time_point t0) { return std::chrono::duration<double, std::milli>(Clock::now() - t0).count(); }

// The encoding of a byte-wise immediate store to the stack frame, "mov byte [rsp+disp32], imm8": C6 84 24 <disp32> <imm8>,
// 8 bytes per data byte. A vector is a run of such stores whose displacements are consecutive.
constexpr uint8_t kStoreOp0 = 0xC6, kStoreOp1 = 0x84, kStoreOp2 = 0x24;
constexpr size_t kStoreLen = 8;
// The probe tag of a weight blob: the high half of (8 bytes of the blob) * this odd constant. A short tag is only a filter;
// every candidate is confirmed by the SHA-256 of the whole blob.
constexpr uint64_t kProbeMul = 0x9E3779B97F4A7C15ull;
// The folded bias of a layer (float32): aux * 128.0f * scale + bias, aux = the sum of the layer's int8 source weights.
constexpr float kFoldGain = 128.0f;

uint32_t Rd32(const uint8_t* p) {
    uint32_t v;
    memcpy(&v, p, 4);
    return v;
}

std::string Narrow(const std::wstring& w) {
    std::string s;
    for (wchar_t c : w) s += c < 0x80 ? static_cast<char>(c) : '?';
    return s;
}

bool ReadFileAll(const std::wstring& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    const std::streamsize n = f.tellg();
    if (n <= 0) return false;
    out.resize(static_cast<size_t>(n));
    f.seekg(0);
    f.read(reinterpret_cast<char*>(out.data()), n);
    return static_cast<bool>(f);
}

struct Range {
    size_t begin, end;
};

// The raw data of the executable sections of a 64-bit PE image.
bool ExecSections(const uint8_t* d, size_t n, std::vector<Range>& out) {
    if (n < 0x40 || d[0] != 'M' || d[1] != 'Z') return false;
    const uint32_t pe = Rd32(d + 0x3C);
    if (pe > n || n - pe < 28 || memcmp(d + pe, "PE\0\0", 4) != 0) return false;
    uint16_t nsec, optSize;
    memcpy(&nsec, d + pe + 6, 2);
    memcpy(&optSize, d + pe + 20, 2);
    const size_t tab = size_t(pe) + 24 + optSize;
    if (nsec == 0 || tab + size_t(nsec) * 40 > n) return false;
    for (uint16_t i = 0; i < nsec; ++i) {
        const uint8_t* s = d + tab + size_t(i) * 40;
        const uint32_t rawSize = Rd32(s + 16), rawPtr = Rd32(s + 20), flags = Rd32(s + 36);
        if (!(flags & 0x20000000u) || rawSize == 0 || rawPtr >= n) continue;  // IMAGE_SCN_MEM_EXECUTE
        out.push_back({rawPtr, std::min(n, size_t(rawPtr) + rawSize)});
    }
    return true;
}

float RdF(const uint8_t* p) {
    float v;
    memcpy(&v, p, 4);
    return v;
}

struct Blob {
    size_t len = 0;
    intel_modules::Sha256 sha{};
    size_t probe = 0;
    uint32_t tag = 0;
    int firstUser = 0;           // an upload that uses it, for messages
    long long start = -1;        // position in the file once found
};

struct Layer {
    enum Kind { Conv, Head, Raw, Vector, Fold } kind = Raw;
    int blob = -1;
    long long co = 0, ci = 0, cop = 0, cip = 0, ct = 0, cit = 0;
    bool tDC = false;                  // conv tile order: false = [A][B][C][tap][D][o][i], true = [A][B][tap][D][C][o][i]
    long long run = -1;
    std::string form;                  // vector: "f32", "i16" (widen to int32), "pad512" (widen if 16-bit, pad to 512 B)
    long long biasRun = -1, scaleRun = -1;
    int weights = 0;                   // fold: the upload whose weights give aux
    // from the recipe's data entry of this upload
    size_t uploadBytes = 0, bytes = 0;
    intel_modules::Sha256 sha{};
};

}  // namespace

struct IntelFileSource::Impl {
    std::vector<uint8_t> file;
    std::vector<Blob> blobs;
    std::vector<size_t> runStart, runLen;  // runs of immediate stores: into `runBytes`
    std::vector<uint8_t> runBytes;
    std::map<int, Layer> layers;
    std::string err;

    bool Fail(const std::string& s) {
        if (err.empty()) err = s;
        return false;
    }

    bool LoadRecipe(const std::vector<uint8_t>& parts) {
        Json root;
        if (!ParseJson(parts, root)) return Fail("the embedded recipe is not valid JSON");
        const Json* dec = root.Get("decode");
        const Json* data = root.Get("data");
        if (!dec || !data) return Fail("the embedded recipe has no weight decode description (older f5low.dll recipe)");
        const Json* bl = dec->Get("blobs");
        const Json* up = dec->Get("uploads");
        if (!bl || !up || bl->type != Json::Arr || up->type != Json::Obj) return Fail("the decode description is malformed");
        for (const Json& b : bl->arr) {
            Blob x;
            x.len = static_cast<size_t>(b.Num_("len"));
            x.probe = static_cast<size_t>(b.Num_("probe"));
            x.tag = static_cast<uint32_t>(b.Num_("tag"));
            if (!x.len || x.probe + 8 > x.len || !intel_modules::ParseHex(b.Str_("sha256"), x.sha))
                return Fail("the decode description has a bad blob entry");
            blobs.push_back(x);
        }
        std::map<int, const Json*> dataByOrd;
        for (const auto& kv : data->obj)
            if (kv.second.Str_("kind") == "intel_upload") dataByOrd[static_cast<int>(kv.second.Num_("ordinal"))] = &kv.second;
        for (const auto& kv : up->obj) {
            const int ord = atoi(kv.first.c_str());
            const Json& e = kv.second;
            Layer L;
            const std::string k = e.Str_("kind");
            if (k == "conv") L.kind = Layer::Conv;
            else if (k == "head") L.kind = Layer::Head;
            else if (k == "raw") L.kind = Layer::Raw;
            else if (k == "vector") L.kind = Layer::Vector;
            else if (k == "fold") L.kind = Layer::Fold;
            else return Fail("upload " + std::to_string(ord) + ": unknown kind " + k);
            L.blob = static_cast<int>(e.Num_("blob", -1));
            L.co = e.Num_("co");
            L.ci = e.Num_("ci");
            L.cop = e.Num_("cop");
            L.cip = e.Num_("cip");
            L.ct = e.Num_("ct");
            L.cit = e.Num_("cit");
            L.tDC = e.Str_("order") == "tDC";
            L.run = e.Num_("run", -1);
            L.form = e.Str_("form");
            L.biasRun = e.Num_("bias_run", -1);
            L.scaleRun = e.Num_("scale_run", -1);
            L.weights = static_cast<int>(e.Num_("weights"));
            auto d = dataByOrd.find(ord);
            if (d == dataByOrd.end()) return Fail("upload " + std::to_string(ord) + " has no entry in the recipe's data");
            L.uploadBytes = static_cast<size_t>(d->second->Num_("upload_bytes"));
            L.bytes = static_cast<size_t>(d->second->Num_("bytes"));
            if (!intel_modules::ParseHex(d->second->Str_("sha256"), L.sha)) return Fail("upload " + std::to_string(ord) + ": bad SHA-256 in the recipe");
            if (L.blob >= 0) {
                if (static_cast<size_t>(L.blob) >= blobs.size()) return Fail("upload " + std::to_string(ord) + ": blob index out of range");
                if (!blobs[L.blob].firstUser) blobs[L.blob].firstUser = ord;
            }
            layers[ord] = L;
        }
        return true;
    }

    // Every weight blob is found by its probe tag, then confirmed by the SHA-256 of the whole blob. The blobs lie back to
    // back in the file in layer order, but nothing here relies on that or on where they are: a copy shifted by a
    // few bytes (or a second copy of the same weights) cannot pass the hash unless its bytes are the same.
    bool LocateBlobs() {
        const size_t n = file.size();
        std::vector<uint8_t> bitmap(8192, 0);
        for (const Blob& b : blobs) bitmap[(b.tag >> 16) >> 3] |= uint8_t(1u << ((b.tag >> 16) & 7));
        size_t left = blobs.size();
        const uint8_t* d = file.data();
        for (size_t i = 0; i + 8 <= n && left; ++i) {
            uint64_t v;
            memcpy(&v, d + i, 8);
            const uint32_t h = static_cast<uint32_t>((v * kProbeMul) >> 32);
            const uint32_t slot = h >> 16;
            if (!(bitmap[slot >> 3] & (1u << (slot & 7)))) continue;
            for (Blob& b : blobs) {
                if (b.tag != h || b.start >= 0 || i < b.probe) continue;
                const size_t s = i - b.probe;
                if (s + b.len > n) continue;
                intel_modules::Sha256 got{};
                if (!intel_modules::HashBytes(d + s, b.len, got) || got != b.sha) continue;
                b.start = static_cast<long long>(s);
                --left;
            }
        }
        for (const Blob& b : blobs)
            if (b.start < 0)
                return Fail("weight blob of upload " + std::to_string(b.firstUser) + " (" + std::to_string(b.len) +
                            " bytes) is not in this libxess_fg.dll");
        return true;
    }

    // Runs of byte-wise immediate stores in the executable sections, in file order. A run goes on while the next 8 bytes
    // are another store whose displacement is one more than the last one.
    void ScanRuns() {
        std::vector<Range> secs;
        if (!ExecSections(file.data(), file.size(), secs)) return;
        const uint8_t* d = file.data();
        for (const Range& r : secs) {
            bool open = false;
            int32_t next = 0;
            auto close = [&] {
                if (!open) return;
                runLen.back() = runBytes.size() - runStart.back();
                open = false;
            };
            size_t i = r.begin;
            while (r.end >= 8 && i < r.end - kStoreLen) {
                if (d[i] == kStoreOp0 && d[i + 1] == kStoreOp1 && d[i + 2] == kStoreOp2) {
                    int32_t disp;
                    memcpy(&disp, d + i + 3, 4);
                    if (open && disp == next) {
                        runBytes.push_back(d[i + 7]);
                        ++next;
                    } else {
                        close();
                        runStart.push_back(runBytes.size());
                        runLen.push_back(0);
                        runBytes.push_back(d[i + 7]);
                        open = true;
                        next = disp + 1;
                    }
                    i += kStoreLen;
                    continue;
                }
                close();
                // skip ahead to the next possible opcode byte
                const void* q = memchr(d + i + 1, kStoreOp0, r.end - kStoreLen - i - 1);
                i = q ? static_cast<size_t>(static_cast<const uint8_t*>(q) - d) : r.end;
            }
            close();
        }
    }

    const uint8_t* BlobData(int b) const { return file.data() + blobs[static_cast<size_t>(b)].start; }

    bool Run(long long r, const uint8_t*& p, size_t& n, const std::string& who) {
        if (r < 0 || static_cast<size_t>(r) >= runStart.size()) return Fail(who + ": the file has no vector run " + std::to_string(r) + " (only " + std::to_string(runStart.size()) + ")");
        p = runBytes.data() + runStart[static_cast<size_t>(r)];
        n = runLen[static_cast<size_t>(r)];
        return true;
    }

    bool Decode(int ord, const Layer& L, std::vector<uint8_t>& out) {
        const std::string who = "upload " + std::to_string(ord);
        out.clear();
        switch (L.kind) {
            case Layer::Raw: {
                const Blob& b = blobs[static_cast<size_t>(L.blob)];
                out.assign(BlobData(L.blob), BlobData(L.blob) + b.len);
                return true;
            }
            case Layer::Conv: {
                const Blob& b = blobs[static_cast<size_t>(L.blob)];
                if (L.ct <= 0 || L.cit <= 0 || L.co <= 0 || L.ci <= 0 || L.cop % L.ct || L.cip % L.cit || L.cop % 4 || L.cip % 4 ||
                    L.co > L.cop || L.ci > L.cip || b.len != size_t(L.co * L.ci * 9))
                    return Fail(who + ": the conv description does not fit its blob");
                const uint8_t* src = BlobData(L.blob);
                out.resize(size_t(L.cop * L.cip * 9));
                size_t w = 0;
                const long long A = L.cop / L.ct, B = L.cip / L.cit, C = L.ct / 4, D = L.cit / 4;
                auto at = [&](long long a, long long bb, long long c, long long t, long long dd, long long o, long long i) -> uint8_t {
                    const long long co = a * L.ct + c * 4 + o, ci = bb * L.cit + dd * 4 + i;
                    return (co < L.co && ci < L.ci) ? src[(co * L.ci + ci) * 9 + t] : uint8_t(0);
                };
                for (long long a = 0; a < A; ++a)
                    for (long long bb = 0; bb < B; ++bb) {
                        if (!L.tDC) {
                            for (long long c = 0; c < C; ++c)
                                for (long long t = 0; t < 9; ++t)
                                    for (long long dd = 0; dd < D; ++dd)
                                        for (long long o = 0; o < 4; ++o)
                                            for (long long i = 0; i < 4; ++i) out[w++] = at(a, bb, c, t, dd, o, i);
                        } else {
                            for (long long t = 0; t < 9; ++t)
                                for (long long dd = 0; dd < D; ++dd)
                                    for (long long c = 0; c < C; ++c)
                                        for (long long o = 0; o < 4; ++o)
                                            for (long long i = 0; i < 4; ++i) out[w++] = at(a, bb, c, t, dd, o, i);
                        }
                    }
                return true;
            }
            case Layer::Head: {
                // 1x1 layer: [cin/4][cout][4]. The padded columns of a row read on into the next row (the flat index is not
                // masked); past the end of the vector they read zero.
                const Blob& b = blobs[static_cast<size_t>(L.blob)];
                if (L.co <= 0 || L.ci <= 0 || L.cip < L.ci || L.cip % 4 || b.len != size_t(L.co * L.ci))
                    return Fail(who + ": the head description does not fit its blob");
                const uint8_t* src = BlobData(L.blob);
                out.resize(size_t(L.co * L.cip));
                size_t w = 0;
                for (long long r = 0; r < L.cip / 4; ++r)
                    for (long long c = 0; c < L.co; ++c)
                        for (long long j = 0; j < 4; ++j) {
                            const long long idx = c * L.ci + r * 4 + j;
                            out[w++] = idx < static_cast<long long>(b.len) ? src[idx] : uint8_t(0);
                        }
                return true;
            }
            case Layer::Vector: {
                const uint8_t* p;
                size_t n;
                if (!Run(L.run, p, n, who)) return false;
                if (L.form == "f32") {
                    out.assign(p, p + n);
                    return true;
                }
                if (L.form == "i16" || L.form == "pad512") {
                    // 16-bit values widened to 32-bit; the stem vectors come as 32-bit (448 B) or 16-bit (224 B) and are
                    // zero-padded to 512 B
                    const bool wide = L.form == "i16" || n == 224;
                    if (wide) {
                        if (n % 2) return Fail(who + ": run " + std::to_string(L.run) + " has an odd length");
                        out.resize(n * 2);
                        for (size_t i = 0; i < n / 2; ++i) {
                            int16_t v;
                            memcpy(&v, p + 2 * i, 2);
                            const int32_t w = v;
                            memcpy(out.data() + 4 * i, &w, 4);
                        }
                    } else {
                        out.assign(p, p + n);
                    }
                    if (L.form == "pad512") {
                        if (out.size() > 512) return Fail(who + ": run " + std::to_string(L.run) + " is longer than 512 bytes");
                        out.resize(512, 0);
                    }
                    return true;
                }
                return Fail(who + ": unknown vector form " + L.form);
            }
            case Layer::Fold: {
                auto wl = layers.find(L.weights);
                if (wl == layers.end() || (wl->second.kind != Layer::Conv && wl->second.kind != Layer::Head))
                    return Fail(who + ": its weights (upload " + std::to_string(L.weights) + ") are not a conv or head layer");
                const Layer& W = wl->second;
                const uint8_t *bp, *sp;
                size_t bn, sn;
                if (!Run(L.biasRun, bp, bn, who) || !Run(L.scaleRun, sp, sn, who)) return false;
                const long long co = W.co;
                if (bn != size_t(co) * 4 || sn != size_t(co) * 4) return Fail(who + ": the bias/scale runs do not fit the layer's " + std::to_string(co) + " channels");
                const uint8_t* src = BlobData(W.blob);
                const long long len = static_cast<long long>(blobs[static_cast<size_t>(W.blob)].len);
                out.resize(size_t(co) * 4);
                for (long long c = 0; c < co; ++c) {
                    long long sum = 0;  // int8 weights of the channel over the PADDED input channels (head: reads on, as the tiling does)
                    if (W.kind == Layer::Conv) {
                        for (long long k = 0; k < W.ci * 9; ++k) sum += static_cast<int8_t>(src[c * W.ci * 9 + k]);
                    } else {
                        for (long long x = 0; x < W.cip; ++x) {
                            const long long idx = c * W.ci + x;
                            if (idx < len) sum += static_cast<int8_t>(src[idx]);
                        }
                    }
                    // each step rounded to float32 on its own (no fused multiply-add)
                    volatile float aux = static_cast<float>(sum);
                    volatile float scaled = aux * kFoldGain;
                    volatile float prod = scaled * RdF(sp + 4 * c);
                    volatile float res = prod + RdF(bp + 4 * c);
                    const float r = res;
                    memcpy(out.data() + 4 * c, &r, 4);
                }
                return true;
            }
        }
        return false;
    }
};

IntelFileSource::IntelFileSource(const std::wstring& libxessPath, const std::vector<uint8_t>& partsJson)
    : impl_(new Impl), path_(libxessPath), parts_(partsJson) {}

IntelFileSource::~IntelFileSource() { delete impl_; }

bool IntelFileSource::Prepare() {
    if (prepared_) return ok_;
    prepared_ = true;
    Impl& m = *impl_;
    auto t = Clock::now();
    if (!m.LoadRecipe(parts_)) {
        problem_ = m.err;
        return false;
    }
    if (!ReadFileAll(path_, m.file)) {
        problem_ = "cannot read " + Narrow(path_);
        return false;
    }
    stats_.readMs = MsSince(t);
    t = Clock::now();
    if (!m.LocateBlobs()) {
        problem_ = m.err + " (" + Narrow(path_) + ")";
        return false;
    }
    stats_.blobsMs = MsSince(t);
    t = Clock::now();
    m.ScanRuns();
    stats_.runsMs = MsSince(t);
    stats_.blobs = m.blobs.size();
    stats_.runs = m.runStart.size();
    ok_ = true;
    return true;
}

bool IntelFileSource::Upload(int ordinal, std::vector<uint8_t>& out) {
    if (!Prepare()) return false;
    Impl& m = *impl_;
    auto it = m.layers.find(ordinal);
    if (it == m.layers.end()) {
        problem_ = "the recipe has no decode description for upload " + std::to_string(ordinal);
        return false;
    }
    const auto t0 = Clock::now();
    m.err.clear();
    const Layer& L = it->second;
    if (!m.Decode(ordinal, L, out)) {
        problem_ = m.err;
        return false;
    }
    if (out.size() != L.uploadBytes) {
        problem_ = "upload " + std::to_string(ordinal) + " decodes to " + std::to_string(out.size()) + " bytes, the recipe wants " + std::to_string(L.uploadBytes);
        return false;
    }
    // the same check the builder makes: the recipe's SHA-256 is of the upload padded with zeros to `bytes`
    std::vector<uint8_t> padded(out);
    padded.resize(L.bytes, 0);
    intel_modules::Sha256 h{};
    if (!intel_modules::HashBytes(padded.data(), padded.size(), h) || h != L.sha) {
        problem_ = "upload " + std::to_string(ordinal) + " rebuilt from " + Narrow(path_) + " does not match the recipe's SHA-256 (a libxess_fg build this recipe does not know?)";
        return false;
    }
    ++stats_.uploads;
    stats_.decodeMs += MsSince(t0);
    return true;
}

bool IntelFileSource::RootConstants(int index, std::vector<uint8_t>& out) {
    // Not derived from the file yet. Until it is, a development capture can stand in (F5LOW_DEV_CAP).
    if (rootSource_) {
        if (rootSource_->RootConstants(index, out)) return true;
        problem_ = rootSource_->Problem();
        return false;
    }
    problem_ = "the dispatch root constants are not derived from libxess_fg.dll yet (development: set F5LOW_DEV_CAP to a PSO capture log)";
    return false;
}

}  // namespace packbuild
