// generators/dlssg_own.h - which nvngx_dlssg module is ours (pure: paths in, verdict out; no Windows API, host-tested by
// tests/dlssg_own_test.cpp). Generator=dlssg must run DLSS-G on its OWN private copy (<f5low_streamline>\nvngx_dlssg.dll). A game that ships its
// own nvngx_dlssg (A Plague Tale: Resonance, 310.6 next to our 310.9) can get the driver / NGX to load that one instead, and the GPU
// then faulted at level load. The caller lists every loaded module whose base name starts with nvngx_dlssg (full paths).
#pragma once
#include <cwctype>
#include <string>
#include <vector>

namespace xdlssg
{
struct NgxVerdict
{
    bool ownLoaded = false;                // our copy is among the loaded modules
    std::vector<std::wstring> foreign;     // every other nvngx_dlssg module (paths as given), without duplicates
    bool Ok() const { return foreign.empty(); }                 // nothing foreign in the process
    bool OwnInUse() const { return ownLoaded && foreign.empty(); }
};

// Path key: lower case, one kind of slash, no \\?\ prefix
inline std::wstring NgxPathKey(const std::wstring& p)
{
    std::wstring k = p;
    if (k.rfind(L"\\\\?\\", 0) == 0 || k.rfind(L"//?/", 0) == 0)
        k.erase(0, 4);
    for (wchar_t& c : k)
    {
        if (c == L'/')
            c = L'\\';
        c = static_cast<wchar_t>(std::towlower(c));
    }
    return k;
}

// True for a base name (or a path ending in one) nvngx_dlssg*.dll
inline bool IsNgxDlssgName(const std::wstring& path)
{
    const size_t cut = path.find_last_of(L"\\/");
    const std::wstring base = NgxPathKey(cut == std::wstring::npos ? path : path.substr(cut + 1));
    return base.rfind(L"nvngx_dlssg", 0) == 0 && base.size() > 4 && base.compare(base.size() - 4, 4, L".dll") == 0;
}

// ownPath: our private nvngx_dlssg.dll; loaded: full paths of the loaded modules (anything not named nvngx_dlssg*.dll is ignored).
inline NgxVerdict JudgeNgx(const std::wstring& ownPath, const std::vector<std::wstring>& loaded)
{
    NgxVerdict v;
    const std::wstring own = NgxPathKey(ownPath);
    std::vector<std::wstring> seen;
    for (const std::wstring& p : loaded)
    {
        if (!IsNgxDlssgName(p))
            continue;
        const std::wstring k = NgxPathKey(p);
        bool dup = false;
        for (const std::wstring& s : seen)
            dup = dup || s == k;
        if (dup)
            continue;
        seen.push_back(k);
        if (k == own)
            v.ownLoaded = true;
        else
            v.foreign.push_back(p);
    }
    return v;
}
} // namespace xdlssg
