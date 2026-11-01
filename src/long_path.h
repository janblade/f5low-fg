// long_path.h - file paths longer than MAX_PATH (260). The Win32 and std::filesystem calls fail on them unless the path
// is in the extended form "\\?\C:\..." (no "." or ".." parts, backslashes only). The pack cache lives under the DLL's
// folder (<DLL dir>\f5low_cache\<32 hex>\hdr\data\...), so a deep install folder pushes its files past the limit.
#pragma once

#include <windows.h>

#include <string>

namespace longpath
{

// Paths shorter than this need no extension (MAX_PATH minus room for what a caller appends).
inline constexpr size_t kPlainLimit = 240;

// Pack folders are held in std::string as UTF-8, whatever the system code page: a folder name with characters outside
// the ANSI code page (Cyrillic on a Western system) must reach the file APIs unchanged. These two convert at the edges.
inline std::wstring WidenUtf8(const std::string& s)
{
    if (s.empty())
        return {};
    const int n = MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), nullptr, 0);
    std::wstring w(static_cast<size_t>(n > 0 ? n : 0), L'\0');
    if (n > 0)
        MultiByteToWideChar(CP_UTF8, 0, s.data(), static_cast<int>(s.size()), w.data(), n);
    return w;
}

inline std::string ToUtf8(const std::wstring& w)
{
    if (w.empty())
        return {};
    const int n = WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), nullptr, 0, nullptr, nullptr);
    std::string s(static_cast<size_t>(n > 0 ? n : 0), '\0');
    if (n > 0)
        WideCharToMultiByte(CP_UTF8, 0, w.data(), static_cast<int>(w.size()), s.data(), n, nullptr, nullptr);
    return s;
}

// The absolute, normalised form of `p` that file APIs take at any length ("\\?\C:\...", "\\?\UNC\server\share\...").
// Already extended paths, device paths and paths that cannot be made absolute come back unchanged.
inline std::wstring Extend(const std::wstring& p)
{
    if (p.compare(0, 2, L"\\\\") == 0 && p.size() > 2 && (p[2] == L'?' || p[2] == L'.'))
        return p;
    DWORD n = GetFullPathNameW(p.c_str(), 0, nullptr, nullptr);
    if (!n)
        return p;
    std::wstring full(n, L'\0');
    n = GetFullPathNameW(p.c_str(), n, full.data(), nullptr);
    if (!n || n >= full.size())
        return p;
    full.resize(n);
    if (full.compare(0, 2, L"\\\\") == 0)
        return L"\\\\?\\UNC\\" + full.substr(2);
    if (full.size() >= 3 && full[1] == L':' && full[2] == L'\\')
        return L"\\\\?\\" + full;
    return p;
}

// The extended form when the path is long, else the path as it is.
inline std::wstring ExtendIfLong(const std::wstring& p) { return p.size() >= kPlainLimit ? Extend(p) : p; }

// The inverse for display and for handing a folder on: without the "\\?\" prefix (UNC kept as "\\server\share").
inline std::wstring Plain(const std::wstring& p)
{
    if (p.compare(0, 8, L"\\\\?\\UNC\\") == 0)
        return L"\\\\" + p.substr(8);
    if (p.compare(0, 4, L"\\\\?\\") == 0)
        return p.substr(4);
    return p;
}

}  // namespace longpath
