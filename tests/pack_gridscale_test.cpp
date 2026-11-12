// pack_gridscale_test - the manifest's grid scale ("gx=N gy=N gz=N" on kernel, variant and fallback lines) and the
// "// GRIDSCALE x y z" line of the kernel's PTX, as Pack::Load (src/xefg_pack.h) reads them. Host only: a tiny pack
// folder is written under %TEMP%, no GPU, no CUDA.
//
//   - parse: strict (digits only, 1..64, each dimension once, no unknown token) on all three line kinds
//   - the PTX marker and the manifest must agree, both ways (marker without gz, gz without marker)
//   - a dispatch dimension that is not 1 is refused
//   - the scale is applied on kernel lines and on a taken fallback (replacing the kernel line's), not on a fallback that is not taken
//
// Usage: pack_gridscale_test.exe      (exit code 0 = all passed)
#include "../src/xefg_pack.h"

#include <cstdio>
#include <filesystem>
#include <fstream>
#include <string>

namespace fs = std::filesystem;
using namespace xefgpack;

static int g_fail = 0, g_run = 0;
static fs::path g_dir;

static void Write(const std::string& name, const std::string& text)
{
    std::ofstream(g_dir / name, std::ios::binary) << text;
}

static std::string Ptx(int sm, const std::string& marker)
{
    return (marker.empty() ? "" : "// GRIDSCALE " + marker + "\n") + ".version 7.0\n.target sm_" + std::to_string(sm) + "\n.entry dxil_main()\n{\n ret;\n}\n";
}

// Loads a one-kernel pack. `kernelOpts` / `fallbackOpts` / `variantOpts`: the rest of those lines (a fallback / variant line only
// when its argument is not "-"). `g`: the dispatch's groups. Returns the Load result; *out gets the groups afterwards.
struct Case
{
    std::string kernelOpts, fallbackOpts = "-", variantOpts = "-";
    std::string kernelMarker, fallbackMarker, variantMarker;
    int kernelSm = 80, deviceSm = 0;
    std::string g = "10 3 1";
};

static bool Run(const Case& c, unsigned (&g)[3], std::string& err, int* fellBack = nullptr)
{
    fs::remove_all(g_dir);
    fs::create_directories(g_dir);
    std::string m = "pack 3\nsize 64 64 64 64\nkernel 0 k00_a.ptx 64 1 1 " + c.kernelOpts + "\n";
    Write("k00_a.ptx", Ptx(c.kernelSm, c.kernelMarker));
    if (c.fallbackOpts != "-")
    {
        m += "fallback 0 k00_a_sm75.ptx 32 1 1 " + c.fallbackOpts + "\n";
        Write("k00_a_sm75.ptx", Ptx(75, c.fallbackMarker));
    }
    if (c.variantOpts != "-")
    {
        m += "variant 0 k00_b.ptx 64 1 1 " + c.variantOpts + "\n";
        Write("k00_b.ptx", Ptx(80, c.variantMarker));
    }
    m += "dispatch 0 " + c.g + "   # 0 once\n";
    Write("manifest.txt", m);
    Cuda cu;
    Pack p(cu);
    p.deviceSm = c.deviceSm;
    const bool ok = p.Load(g_dir.string());
    err = p.error;
    g[0] = g[1] = g[2] = 0;
    if (ok && !p.disp.empty())
        std::copy(p.disp[0].g, p.disp[0].g + 3, g);
    if (fellBack)
        *fellBack = p.fellBack;
    return ok;
}

#define CHECK(cond, what)                                                                  \
    do                                                                                     \
    {                                                                                      \
        ++g_run;                                                                           \
        if (!(cond))                                                                       \
        {                                                                                  \
            ++g_fail;                                                                      \
            printf("FAIL  %s  (%s:%d)\n", what, __FILE__, __LINE__);                       \
        }                                                                                  \
    } while (0)

static void Loads(const Case& c, const char* what, unsigned x, unsigned y, unsigned z)
{
    unsigned g[3];
    std::string err;
    const bool ok = Run(c, g, err);
    if (!ok)
        printf("      (%s: %s)\n", what, err.c_str());
    CHECK(ok && g[0] == x && g[1] == y && g[2] == z, what);
}

static void Refused(const Case& c, const char* what)
{
    unsigned g[3];
    std::string err;
    const bool ok = Run(c, g, err);
    if (ok)
        printf("      (%s: loaded as %u %u %u)\n", what, g[0], g[1], g[2]);
    CHECK(!ok && !err.empty(), what);
}

int main()
{
    g_dir = fs::temp_directory_path() / "f5low_gridscale_test";

    // --- parse and apply on a kernel line
    {
        Case c;
        c.kernelOpts = "gz=4";
        c.kernelMarker = "1 1 4";
        Loads(c, "kernel gz=4 scales z", 10, 3, 4);
        c.g = "1 3 1";
        c.kernelOpts = "gx=2 gz=3";
        c.kernelMarker = "2 1 3";
        Loads(c, "kernel gx=2 gz=3 scales x and z", 2, 3, 3);
        c.g = "10 3 1";
        c.kernelOpts = "hash abc gz=2";
        c.kernelMarker = "1 1 2";
        Loads(c, "hash and gz together", 10, 3, 2);
        c.kernelOpts = "";
        c.kernelMarker = "";
        Loads(c, "no scale, no marker: unchanged", 10, 3, 1);
        c.kernelMarker = "1 1 1";
        Loads(c, "marker 1 1 1 and no gz agree", 10, 3, 1);
    }
    // --- strict parse, on all three line kinds
    for (const char* bad : { "gz=4x", "gz=", "gz=0", "gz=65", "gz=-1", "gz=+2", "gz=2 gz=2", "gz=4294967297", "gw=2", "gz2", "G Z", "bogus=1", "gz=0x2" })
    {
        Case c;
        c.kernelMarker = "1 1 2";
        c.kernelOpts = bad;
        Refused(c, (std::string("kernel line: ") + bad).c_str());
        Case f;
        f.fallbackOpts = bad;
        f.fallbackMarker = "1 1 2";
        f.deviceSm = 75;
        f.kernelSm = 80;
        Refused(f, (std::string("fallback line: ") + bad).c_str());
        Case v;
        v.variantOpts = std::string("inv=1 ") + bad;
        v.variantMarker = "1 1 2";
        Refused(v, (std::string("variant line: ") + bad).c_str());
    }
    {
        Case v;
        v.variantOpts = "inv=2";
        Refused(v, "variant condition value other than 0/1");
        v.variantOpts = "wibble=1";
        Refused(v, "variant: unknown condition");
        v.variantOpts = "inv=0 gz=4x";      // does not match, but is still checked
        Refused(v, "variant that does not match still has its tokens checked");
    }
    // --- marker and manifest must agree, both ways
    {
        Case c;
        c.kernelOpts = "";
        c.kernelMarker = "1 1 4";
        Refused(c, "marker present, no gz");
        c.kernelOpts = "gz=4";
        c.kernelMarker = "";
        Refused(c, "gz present, no marker");
        c.kernelMarker = "1 1 2";
        Refused(c, "gz=4 but marker 2");
        c.kernelMarker = "2 1 4";
        Refused(c, "marker has an x scale the manifest lacks");
        c.kernelOpts = "";
        c.kernelMarker = "1 1 x";
        Refused(c, "garbled marker");
    }
    // --- a dimension that is not 1
    {
        Case c;
        c.kernelOpts = "gz=2";
        c.kernelMarker = "1 1 2";
        c.g = "10 3 2";
        Refused(c, "gz on a dispatch whose z is 2");
        c.kernelOpts = "gx=2";
        c.kernelMarker = "2 1 1";
        c.g = "10 3 1";
        Refused(c, "gx on a dispatch whose x is 10");
    }
    // --- fallbacks: taken (scale applied, replacing the kernel line's) and not taken (nothing applied)
    {
        Case c;
        c.fallbackOpts = "gz=2";
        c.fallbackMarker = "1 1 2";
        c.kernelSm = 80;
        c.deviceSm = 75;
        int fell = 0;
        unsigned g[3];
        std::string err;
        CHECK(Run(c, g, err, &fell) && fell == 1 && g[0] == 10 && g[1] == 3 && g[2] == 2, "fallback taken: its gz applied");
        c.deviceSm = 80;
        CHECK(Run(c, g, err, &fell) && fell == 0 && g[0] == 10 && g[1] == 3 && g[2] == 1, "fallback not taken: no scale");
        c.deviceSm = 0;
        CHECK(Run(c, g, err, &fell) && fell == 0 && g[2] == 1, "deviceSm 0: no fallback, no scale");

        // the kernel line has its own scale; the fallback replaces it when taken, the kernel's stays when not
        c.kernelOpts = "gz=4";
        c.kernelMarker = "1 1 4";
        c.deviceSm = 75;
        CHECK(Run(c, g, err, &fell) && fell == 1 && g[2] == 2, "taken fallback replaces the kernel line's scale");
        c.deviceSm = 80;
        CHECK(Run(c, g, err, &fell) && fell == 0 && g[2] == 4, "not-taken fallback leaves the kernel line's scale");

        // a taken fallback whose marker disagrees with its line, both ways
        c.kernelOpts = "";
        c.kernelMarker = "";
        c.deviceSm = 75;
        c.fallbackOpts = "gz=2";
        c.fallbackMarker = "";
        CHECK(!Run(c, g, err, &fell) && !err.empty(), "taken fallback: gz, no marker");
        c.fallbackOpts = "";
        c.fallbackMarker = "1 1 2";
        CHECK(!Run(c, g, err, &fell) && !err.empty(), "taken fallback: marker, no gz");
        // not taken: a fallback file that is never read cannot fail the load
        c.deviceSm = 80;
        CHECK(Run(c, g, err, &fell) && g[2] == 1, "fallback not taken: its marker is not looked at");
        // a fallback that is not taken still has its tokens parsed
        c.fallbackOpts = "gz=2x";
        CHECK(!Run(c, g, err, &fell) && !err.empty(), "fallback not taken: a bad token still refused");
    }
    // --- variants: the matching one's scale is applied, a non-matching one's is not
    {
        Case c;
        c.variantOpts = "inv=1 gz=2";
        c.variantMarker = "1 1 2";
        Loads(c, "matching variant: gz applied", 10, 3, 2);
        c.variantOpts = "inv=0 gz=2";
        c.variantMarker = "1 1 2";
        Loads(c, "non-matching variant: kernel line stays, no scale", 10, 3, 1);
        c.variantOpts = "inv=1 gz=2";
        c.variantMarker = "";
        Refused(c, "matching variant: gz without marker");
        c.variantOpts = "inv=1";
        c.variantMarker = "1 1 2";
        Refused(c, "matching variant: marker without gz");
    }
    // --- a scale on the kernel line carries over to a dispatch of every kernel that uses it only
    printf("%d checks, %d failed\n", g_run, g_fail);
    fs::remove_all(g_dir);
    return g_fail ? 1 : 0;
}
