// xefg_unlock_test - host test of the XeFG unlock family (src/unlock/FamilyXeFG.h, UnlockXeFg.h) on the player's own Intel files, passed as
// arguments (they stay outside this repo):   xefg_unlock_test.exe <libxess_fg.dll> <libxell.dll>
// The modules are mapped without running their code (DONT_RESOLVE_DLL_REFERENCES). Checks:
//   1. every site (U1-U5, XL1) is found exactly once, and Locate changes nothing (the module is byte-identical = the "unlock off" state);
//   2. the patch plan's expected bytes are what the file holds and the replacements carry the requested count;
//   3. Apply (count 5) changes exactly the planned bytes, nothing else in either image;
//   4. a second Apply on the patched modules is refused (unknown shape), nothing further changes.
// This patches inside Intel's DLLs in memory (the test's own mapping only).
#include "../src/unlock/UnlockXeFg.h"

#include <cstdarg>
#include <cstdio>
#include <string>
#include <vector>

static std::vector<uint8_t> Snap(HMODULE m)
{
    const size_t n = unlock::detail::GetMappedImageSize(m);
    return std::vector<uint8_t>(reinterpret_cast<uint8_t*>(m), reinterpret_cast<uint8_t*>(m) + n);
}

static bool g_ok = true;
static void Check(bool c, const char* what)
{
    std::printf("%s %s\n", c ? "PASS" : "FAIL", what);
    g_ok = g_ok && c;
}

static size_t Diff(const std::vector<uint8_t>& a, const std::vector<uint8_t>& b, std::vector<size_t>& at)
{
    size_t n = 0;
    for (size_t i = 0; i < a.size() && i < b.size(); ++i)
        if (a[i] != b[i])
        {
            ++n;
            at.push_back(i);
        }
    return n;
}

int wmain(int argc, wchar_t** argv)
{
    if (argc < 3)
    {
        std::fputs("usage: xefg_unlock_test <libxess_fg.dll> <libxell.dll>\n", stderr);
        return 2;
    }
    unlock::g_log = [](const char* f, ...) {
        va_list a;
        va_start(a, f);
        std::printf("  log: ");
        std::vprintf(f, a);
        std::printf("\n");
        va_end(a);
    };
    HMODULE fg = LoadLibraryExW(argv[1], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    HMODULE ll = LoadLibraryExW(argv[2], nullptr, DONT_RESOLVE_DLL_REFERENCES);
    if (!fg || !ll)
    {
        std::fputs("cannot map the files\n", stderr);
        return 2;
    }
    const auto fg0 = Snap(fg), ll0 = Snap(ll);
    const MfgUnlock::ImageView fgi(fg, fg0.size()), lli(ll, ll0.size());

    // 1 + 2
    MfgUnlock::FamilyXeFG::Report rep;
    unlock::detail::Guarded([&] { rep = MfgUnlock::FamilyXeFG::Locate(fgi, lli, 5); });
    for (const auto& p : rep.patches)
        std::printf("  site %-28s %-9s state %-8s rva 0x%x %zu byte(s) -> %02x\n", p.name.c_str(),
                    p.module == MfgUnlock::FamilyXeFG::Module::XeFG ? "libxess_fg" : "libxell",
                    std::string(MfgUnlock::FamilyXeFG::LocateStateName(p.state)).c_str(), p.rva, p.expected.size(),
                    p.replacement.empty() ? 0 : std::to_integer<unsigned>(p.replacement[0]));
    Check(rep.Complete() && rep.patches.size() == 6, "all six sites found exactly once");
    Check(Snap(fg) == fg0 && Snap(ll) == ll0, "Locate changed nothing (unlock off = byte-identical)");
    {
        // the count is carried by U3, U4, U5 and XL1 (the replacement byte), U1 and U2 are jumps
        unsigned carry = 0;
        for (const auto& p : rep.patches)
            carry += p.replacement.size() == 1 && std::to_integer<unsigned>(p.replacement[0]) == 5;
        Check(carry == 4, "U3, U4, U5 and XL1 carry the count 5");
        MfgUnlock::FamilyXeFG::Report r4;
        unlock::detail::Guarded([&] { r4 = MfgUnlock::FamilyXeFG::Locate(fgi, lli, 4); });
        unsigned c4 = 0;
        for (const auto& p : r4.patches)
            c4 += p.replacement.size() == 1 && std::to_integer<unsigned>(p.replacement[0]) == 4;
        Check(r4.Complete() && c4 == 4, "count 4 plans the byte 4");
    }
    // 3
    const unlock::xefg::Result a = unlock::xefg::Apply(fg, ll, 5);
    Check(a.ok && a.count == 5, "Apply(5) succeeded");
    std::vector<size_t> d1, d2;
    const auto fg1 = Snap(fg), ll1 = Snap(ll);
    const size_t n1 = Diff(fg0, fg1, d1), n2 = Diff(ll0, ll1, d2);
    size_t want1 = 0, want2 = 0;
    for (const auto& p : rep.patches)
        (p.module == MfgUnlock::FamilyXeFG::Module::XeFG ? want1 : want2) += p.expected.size();
    std::printf("  changed bytes: libxess_fg %zu, libxell %zu\n", n1, n2);
    // a two-byte patch whose bytes partly equal the replacement changes fewer bytes, never more
    Check(n1 > 0 && n1 <= want1 && n2 == 1 && n2 <= want2, "only the planned bytes changed");
    bool inside = true;
    for (const size_t i : d1)
    {
        bool in = false;
        for (const auto& p : rep.patches)
            in = in || (p.module == MfgUnlock::FamilyXeFG::Module::XeFG && i >= p.rva && i < p.rva + p.expected.size());
        inside = inside && in;
    }
    for (const size_t i : d2)
    {
        bool in = false;
        for (const auto& p : rep.patches)
            in = in || (p.module == MfgUnlock::FamilyXeFG::Module::XeLL && i >= p.rva && i < p.rva + p.expected.size());
        inside = inside && in;
    }
    Check(inside, "every changed byte lies inside a planned site");
    // 4
    const unlock::xefg::Result b = unlock::xefg::Apply(fg, ll, 5);
    Check(!b.ok && Snap(fg) == fg1 && Snap(ll) == ll1, "a second Apply is refused and changes nothing");
    std::puts(g_ok ? "ALL PASS" : "FAILED");
    return g_ok ? 0 : 1;
}
