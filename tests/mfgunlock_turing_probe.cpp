// Ported from OptiScaler-NR tests/mfgunlock_turing_probe.cpp at 745e49431 (origin/release/0.1.36).
// Changes: include path only.
// Host-side check for the RTX 20 (sm_75) kernel path. For every CUDA fatbin container in a provider file it runs the
// same code the runtime uses (PtxTuring::BuildFromContainer) and asks this PC's CUDA driver to JIT the result. The
// text is written for ".target sm_75", so the driver rejects any instruction Turing lacks, which is the real question.
// With -numeric it also runs each rewrite construct against the original instruction on this GPU and compares results.
// usage: mfgunlock_turing_probe <nvngx_dlssg.dll | driver .bin> [-v] [-numeric] [-dump <dir>]
#include "../src/unlock/PtxTuring.h"

#define NOMINMAX
#include <windows.h>

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <fstream>
#include <iterator>
#include <random>

namespace
{
using CUresult = int;
using CUmodule = void*;
using CUfunction = void*;

struct Cuda
{
    HMODULE lib = nullptr;
    CUresult(WINAPI* cuInit)(unsigned) = nullptr;
    CUresult(WINAPI* cuDeviceGet)(int*, int) = nullptr;
    CUresult(WINAPI* cuCtxCreate)(void**, unsigned, int) = nullptr;
    CUresult(WINAPI* cuModuleLoadDataEx)(CUmodule*, const void*, unsigned, int*, void**) = nullptr;
    CUresult(WINAPI* cuModuleUnload)(CUmodule) = nullptr;
    CUresult(WINAPI* cuModuleGetFunction)(CUfunction*, CUmodule, const char*) = nullptr;
    CUresult(WINAPI* cuMemAlloc)(unsigned long long*, size_t) = nullptr;
    CUresult(WINAPI* cuMemFree)(unsigned long long) = nullptr;
    CUresult(WINAPI* cuMemcpyHtoD)(unsigned long long, const void*, size_t) = nullptr;
    CUresult(WINAPI* cuMemcpyDtoH)(void*, unsigned long long, size_t) = nullptr;
    CUresult(WINAPI* cuLaunchKernel)(CUfunction, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned, unsigned,
                                     void*, void**, void**) = nullptr;
    CUresult(WINAPI* cuCtxSynchronize)() = nullptr;

    bool Init()
    {
        lib = LoadLibraryW(L"nvcuda.dll");
        if (!lib)
            return false;
#define LOAD(name, symbol) name = reinterpret_cast<decltype(name)>(GetProcAddress(lib, symbol))
        LOAD(cuInit, "cuInit");
        LOAD(cuDeviceGet, "cuDeviceGet");
        LOAD(cuCtxCreate, "cuCtxCreate_v2");
        LOAD(cuModuleLoadDataEx, "cuModuleLoadDataEx");
        LOAD(cuModuleUnload, "cuModuleUnload");
        LOAD(cuModuleGetFunction, "cuModuleGetFunction");
        LOAD(cuMemAlloc, "cuMemAlloc_v2");
        LOAD(cuMemFree, "cuMemFree_v2");
        LOAD(cuMemcpyHtoD, "cuMemcpyHtoD_v2");
        LOAD(cuMemcpyDtoH, "cuMemcpyDtoH_v2");
        LOAD(cuLaunchKernel, "cuLaunchKernel");
        LOAD(cuCtxSynchronize, "cuCtxSynchronize");
#undef LOAD
        int device = 0;
        void* context = nullptr;
        return cuInit(0) == 0 && cuDeviceGet(&device, 0) == 0 && cuCtxCreate(&context, 0, device) == 0;
    }

    CUresult Load(const std::string& ptx, CUmodule& module, std::string& log)
    {
        char buffer[4096] = {};
        int options[] = { 5, 6 }; // error log buffer, size
        void* values[] = { buffer, reinterpret_cast<void*>(static_cast<uintptr_t>(sizeof(buffer))) };
        const CUresult result = cuModuleLoadDataEx(&module, ptx.c_str(), 2, options, values);
        log = buffer;
        return result;
    }
} cuda;

std::mt19937 rng(1);

// Runs the kernel K(a, b, out) over n words and returns out.
bool RunWords(const std::string& ptx, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b,
              std::vector<uint32_t>& out)
{
    CUmodule module = nullptr;
    std::string log;
    if (cuda.Load(ptx, module, log) != 0)
    {
        std::printf("  JIT failed: %.200s\n", log.c_str());
        return false;
    }
    CUfunction function = nullptr;
    cuda.cuModuleGetFunction(&function, module, "K");
    const size_t bytes = a.size() * 4;
    unsigned long long da = 0, db = 0, dout = 0;
    cuda.cuMemAlloc(&da, bytes);
    cuda.cuMemAlloc(&db, bytes);
    cuda.cuMemAlloc(&dout, bytes);
    cuda.cuMemcpyHtoD(da, a.data(), bytes);
    cuda.cuMemcpyHtoD(db, b.data(), bytes);
    void* args[] = { &da, &db, &dout };
    const bool ok = cuda.cuLaunchKernel(function, static_cast<unsigned>(a.size() / 256), 1, 1, 256, 1, 1, 0, nullptr,
                                        args, nullptr) == 0 &&
                    cuda.cuCtxSynchronize() == 0;
    out.assign(a.size(), 0);
    cuda.cuMemcpyDtoH(out.data(), dout, bytes);
    cuda.cuMemFree(da);
    cuda.cuMemFree(db);
    cuda.cuMemFree(dout);
    cuda.cuModuleUnload(module);
    return ok;
}

std::string Elementwise(const std::string& body)
{
    return std::string(".version 8.0\n.target sm_80\n.address_size 64\n") +
           ".visible .entry K(.param .u64 pa, .param .u64 pb, .param .u64 po){\n"
           ".reg .b32 %r<8>; .reg .b16 %rs<8>; .reg .f32 %f<4>; .reg .b64 %rd<8>; .reg .pred %p<2>;\n"
           "ld.param.u64 %rd1,[pa]; ld.param.u64 %rd2,[pb]; ld.param.u64 %rd3,[po];\n"
           "mov.u32 %r4,%tid.x; mov.u32 %r5,%ctaid.x; mov.u32 %r6,%ntid.x; mad.lo.u32 %r4,%r5,%r6,%r4; "
           "mul.wide.u32 %rd4,%r4,4;\n"
           "add.s64 %rd5,%rd1,%rd4; add.s64 %rd6,%rd2,%rd4; add.s64 %rd7,%rd3,%rd4;\n"
           "ld.global.u32 %r1,[%rd5]; ld.global.u32 %r2,[%rd6];\n" +
           body + "\nst.global.u32 [%rd7],%r3;\nret;}\n";
}

uint32_t HalfWord()
{
    // two finite fp16 values
    uint32_t word = 0;
    for (int half = 0; half < 2; ++half)
    {
        uint32_t h = rng() & 0xFFFF;
        if (((h >> 10) & 31) == 31)
            h = (h & 0x83FF) | (30u << 10);
        word |= h << (16 * half);
    }
    return word;
}

// Compares original and rewritten kernels; returns how many words differ, or -1 when something failed.
long Compare(const char* name, const std::string& body, const std::vector<uint32_t>& a, const std::vector<uint32_t>& b)
{
    const std::string original = Elementwise(body);
    const auto rewritten = MfgUnlock::PtxTuring::RewriteText(original);
    if (!rewritten.ok || rewritten.ptx == original)
    {
        std::printf("%s: rewrite did not fire (%s)\n", name, rewritten.failure.c_str());
        return -1;
    }
    std::vector<uint32_t> x, y;
    if (!RunWords(original, a, b, x) || !RunWords(rewritten.ptx, a, b, y))
        return -1;
    long differ = 0;
    for (size_t i = 0; i < x.size(); ++i)
        differ += x[i] != y[i];
    std::printf("%s: %ld of %zu words differ\n", name, differ, x.size());
    return differ;
}

bool Numeric()
{
    constexpr size_t n = 1 << 20;
    std::vector<uint32_t> a(n), b(n);
    for (size_t i = 0; i < n; ++i)
    {
        a[i] = HalfWord();
        b[i] = HalfWord();
    }
    // Every pair of the edge values, in both halves (swapped in the high one): signed zeros, the canonical NaN, two
    // non-canonical NaN payloads (quiet and signalling), the infinities, a subnormal and equal values. The
    // non-canonical NaNs exercise the both-NaN canonicalisation.
    constexpr uint16_t edges[] = { 0x0000, 0x8000, 0x7FFF, 0x7E00, 0x7C01, 0xFE01,
                                   0x3C00, 0xBC00, 0x7C00, 0xFC00, 0x0001, 0x8001 };
    size_t slot = 0;
    for (const uint16_t x : edges)
    {
        for (const uint16_t y : edges)
        {
            a[slot] = x | (static_cast<uint32_t>(y) << 16);
            b[slot] = y | (static_cast<uint32_t>(x) << 16);
            ++slot;
        }
    }
    // Mix a NaN or infinity into some otherwise-finite random words, so both-NaN and NaN/finite pairs appear widely.
    for (size_t i = slot; i < n; ++i)
    {
        if ((rng() & 7) == 0)
            a[i] = (a[i] & 0xFFFF) | (edges[rng() % std::size(edges)] << 16);
        if ((rng() & 7) == 0)
            b[i] = (b[i] & 0xFFFF) | (edges[rng() % std::size(edges)] << 16);
        if ((rng() & 7) == 0)
            a[i] = (a[i] & 0xFFFF0000) | edges[rng() % std::size(edges)];
        if ((rng() & 7) == 0)
            b[i] = (b[i] & 0xFFFF0000) | edges[rng() % std::size(edges)];
    }
    bool ok = true;
    for (const char* op : { "min", "max" })
    {
        // Signed zeros and NaN operands included, the rewrite has to match bit for bit.
        const std::string vec = std::string("{") + op + ".f16x2 %r3,%r1,%r2;\n}";
        const long vectorDiffs = Compare((std::string(op) + ".f16x2").c_str(), vec, a, b);
        ok = ok && vectorDiffs == 0;
        const std::string scalar = std::string("mov.b32 {%rs1,%rs5}, %r1; mov.b32 {%rs2,%rs5}, %r2;\n{") + op +
                                   ".f16 %rs3,%rs1,%rs2;\n}\ncvt.u32.u16 %r3, %rs3;";
        const long scalarDiffs = Compare((std::string(op) + ".f16").c_str(), scalar, a, b);
        ok = ok && scalarDiffs == 0;
        // The destination is also the first source.
        const std::string aliased = std::string("mov.b32 {%rs1,%rs5}, %r1; mov.b32 {%rs2,%rs5}, %r2;\n{") + op +
                                    ".f16 %rs1,%rs1,%rs2;\n}\ncvt.u32.u16 %r3, %rs1;";
        const long aliasedDiffs = Compare((std::string(op) + ".f16 d=a").c_str(), aliased, a, b);
        ok = ok && aliasedDiffs == 0;
    }
    std::vector<uint32_t> fa(n), fb(n);
    std::normal_distribution<float> normal;
    std::uniform_real_distribution<float> exponent(-6.f, 5.f);
    for (size_t i = 0; i < n; ++i)
    {
        const float x = normal(rng) * std::pow(10.f, exponent(rng));
        const float y = normal(rng) * std::pow(10.f, exponent(rng));
        std::memcpy(&fa[i], &x, 4);
        std::memcpy(&fb[i], &y, 4);
    }
    const long cvtDiffs = Compare("cvt.rn.f16x2.f32", "mov.b32 %f1,%r1; mov.b32 %f2,%r2;\n{ cvt.rn.f16x2.f32 %r3, %f1, %f2; }",
                                  fa, fb);
    ok = ok && cvtDiffs == 0;

    // mma: one warp, A 4 regs, B 2 regs, C 2 regs per lane
    const std::string mma =
        ".version 8.0\n.target sm_80\n.address_size 64\n"
        ".visible .entry K(.param .u64 pa, .param .u64 pb, .param .u64 pc, .param .u64 po){\n"
        ".reg .b32 %r<24>; .reg .b64 %rd<8>;\n"
        "ld.param.u64 %rd1,[pa]; ld.param.u64 %rd2,[pb]; ld.param.u64 %rd3,[pc]; ld.param.u64 %rd4,[po];\n"
        "mov.u32 %r20,%tid.x; mul.wide.u32 %rd5,%r20,16; add.s64 %rd6,%rd1,%rd5;\n"
        "ld.global.v4.u32 {%r1,%r2,%r3,%r4},[%rd6];\n"
        "mul.wide.u32 %rd5,%r20,8; add.s64 %rd6,%rd2,%rd5; ld.global.v2.u32 {%r5,%r6},[%rd6];\n"
        "add.s64 %rd6,%rd3,%rd5; ld.global.v2.u32 {%r7,%r8},[%rd6];\n"
        "mma.sync.aligned.m16n8k16.row.col.f16.f16.f16.f16 {%r9,%r10}, {%r1,%r2,%r3,%r4}, {%r5,%r6}, {%r7,%r8};\n"
        "add.s64 %rd6,%rd4,%rd5; st.global.v2.u32 [%rd6],{%r9,%r10};\nret;}\n";
    const auto rewritten = MfgUnlock::PtxTuring::RewriteText(mma);
    if (!rewritten.ok || rewritten.mma != 1 || rewritten.ptx.find("m16n8k8") == std::string::npos)
    {
        std::printf("mma: rewrite did not fire (%s)\n", rewritten.failure.c_str());
        return false;
    }
    CUmodule moduleA = nullptr, moduleB = nullptr;
    std::string log;
    if (cuda.Load(mma, moduleA, log) != 0 || cuda.Load(rewritten.ptx, moduleB, log) != 0)
    {
        std::printf("mma: JIT failed %.200s\n", log.c_str());
        return false;
    }
    CUfunction functionA = nullptr, functionB = nullptr;
    cuda.cuModuleGetFunction(&functionA, moduleA, "K");
    cuda.cuModuleGetFunction(&functionB, moduleB, "K");
    const auto halves = [&](size_t count) {
        std::vector<uint32_t> v(count);
        std::normal_distribution<float> dist(0.f, 0.5f);
        for (auto& word : v)
        {
            uint32_t packed = 0;
            for (int half = 0; half < 2; ++half)
            {
                const float f = dist(rng);
                // float -> fp16 by truncation of mantissa is enough for test inputs
                uint32_t bits;
                std::memcpy(&bits, &f, 4);
                const int exponentBits = static_cast<int>((bits >> 23) & 0xFF) - 127 + 15;
                uint32_t h = 0;
                if (exponentBits > 0 && exponentBits < 31)
                    h = ((bits >> 16) & 0x8000) | (static_cast<uint32_t>(exponentBits) << 10) | ((bits >> 13) & 0x3FF);
                packed |= h << (16 * half);
            }
            word = packed;
        }
        return v;
    };
    double worst = 0;
    long identical = 0, total = 0;
    for (int trial = 0; trial < 100; ++trial)
    {
        const auto A = halves(128), B = halves(64), C = halves(64);
        std::vector<uint32_t> results[2];
        CUfunction functions[2] = { functionA, functionB };
        for (int which = 0; which < 2; ++which)
        {
            unsigned long long dA, dB, dC, dO;
            cuda.cuMemAlloc(&dA, 512);
            cuda.cuMemAlloc(&dB, 256);
            cuda.cuMemAlloc(&dC, 256);
            cuda.cuMemAlloc(&dO, 256);
            cuda.cuMemcpyHtoD(dA, A.data(), 512);
            cuda.cuMemcpyHtoD(dB, B.data(), 256);
            cuda.cuMemcpyHtoD(dC, C.data(), 256);
            void* args[] = { &dA, &dB, &dC, &dO };
            cuda.cuLaunchKernel(functions[which], 1, 1, 1, 32, 1, 1, 0, nullptr, args, nullptr);
            cuda.cuCtxSynchronize();
            results[which].assign(64, 0);
            cuda.cuMemcpyDtoH(results[which].data(), dO, 256);
            cuda.cuMemFree(dA);
            cuda.cuMemFree(dB);
            cuda.cuMemFree(dC);
            cuda.cuMemFree(dO);
        }
        const auto toFloat = [](uint16_t h) {
            const int sign = (h >> 15) ? -1 : 1;
            const int e = (h >> 10) & 31;
            const int m = h & 0x3FF;
            if (e == 0)
                return sign * std::ldexp(static_cast<double>(m), -24);
            return sign * std::ldexp(1.0 + m / 1024.0, e - 15);
        };
        double peak = 1e-6, delta = 0;
        for (size_t i = 0; i < 64; ++i)
        {
            identical += results[0][i] == results[1][i];
            ++total;
            for (int half = 0; half < 2; ++half)
            {
                const double x = toFloat(static_cast<uint16_t>(results[0][i] >> (16 * half)));
                const double y = toFloat(static_cast<uint16_t>(results[1][i] >> (16 * half)));
                peak = std::max(peak, std::fabs(x));
                delta = std::max(delta, std::fabs(x - y));
            }
        }
        worst = std::max(worst, delta / peak);
    }
    std::printf("mma m16n8k16 -> 2x m16n8k8: %.1f%% words identical, worst error / peak = %.2e\n",
                100.0 * identical / total, worst);
    ok = ok && worst < 5e-3;
    return ok;
}
} // namespace

int main(int argc, char** argv)
{
    if (argc < 2)
        return 2;
    bool verbose = false, numeric = false;
    const char* dumpDir = nullptr;
    for (int a = 2; a < argc; ++a)
    {
        if (!std::strcmp(argv[a], "-v"))
            verbose = true;
        else if (!std::strcmp(argv[a], "-numeric"))
            numeric = true;
        else if (!std::strcmp(argv[a], "-dump") && a + 1 < argc)
            dumpDir = argv[++a];
    }
    if (!cuda.Init())
    {
        std::printf("CUDA init failed\n");
        return 3;
    }

    if (numeric)
        return Numeric() ? 0 : 1;

    std::ifstream in(argv[1], std::ios::binary);
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    unsigned containers = 0, ok = 0, failed = 0, mma = 0, minMax = 0, cvt = 0;
    for (size_t i = 0; i + 16 <= d.size();)
    {
        if (MfgUnlock::PtxTuring::detail::Load<uint32_t>(&d[i]) != MfgUnlock::PtxTuring::kFatbinMagic ||
            MfgUnlock::PtxTuring::detail::Load<uint16_t>(&d[i + 6]) != 0x10)
        {
            ++i;
            continue;
        }
        const uint64_t size = MfgUnlock::PtxTuring::detail::Load<uint64_t>(&d[i + 8]);
        if (size == 0 || size > d.size() - i - 16)
        {
            i += 4;
            continue;
        }
        const unsigned index = containers++;
        const auto built = MfgUnlock::PtxTuring::BuildFromContainer(&d[i]);
        i += 16 + static_cast<size_t>(size);
        if (!built.ok)
        {
            std::printf("c%u: build failed: %s\n", index, built.failure.c_str());
            ++failed;
            continue;
        }
        mma += built.mma;
        minMax += built.minMaxVec + built.minMaxScalar;
        cvt += built.cvt;
        if (dumpDir)
        {
            char name[MAX_PATH];
            std::snprintf(name, sizeof(name), "%s\\c%02u_sm75_rw.ptx", dumpDir, index);
            std::ofstream(name, std::ios::binary) << built.ptx;
        }
        CUmodule module = nullptr;
        std::string log;
        const CUresult result = cuda.Load(built.ptx, module, log);
        if (result == 0)
        {
            ++ok;
            cuda.cuModuleUnload(module);
            if (verbose)
                std::printf("c%u: ok (src sm_%d, mma=%u minmax=%u+%u cvt=%u)\n", index, built.sourceArch, built.mma,
                            built.minMaxVec, built.minMaxScalar, built.cvt);
        }
        else
        {
            ++failed;
            std::printf("c%u: JIT failed %d: %.300s\n", index, result, log.c_str());
        }
    }
    std::printf("containers=%u jitOk=%u failed=%u | rewrites mma=%u minmax=%u cvt=%u\n", containers, ok, failed, mma,
                minMax, cvt);
    return failed == 0 && containers != 0 ? 0 : 1;
}
