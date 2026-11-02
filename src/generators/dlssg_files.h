// generators/dlssg_files.h - where the dlssg generator gets its Streamline from, and the renamed private copy it loads.
//
// The player's own NVIDIA files (OptiScaler ships them in OptiScaler\Streamline: sl.interposer.dll, sl.common.dll,
// sl.dlss_g.dll, sl.reflex.dll, sl.pcl.dll, nvngx_dlssg.dll) are COPIED, unmodified, under other names into a folder of ours.
// Why: OptiScaler hooks every module whose name ends in sl.interposer.dll / sl.common.dll / sl.pcl.dll / ... and keeps one set of
// originals per process; with our Streamline loaded under the plain names it re-pointed its hooks at ours and the game's own DLSS
// calls landed in our Streamline (NBA 2K27: "kFeatureDLSS context is missing", no upscaler). Streamline finds its plugins by the
// pattern sl.*.dll, so the renamed ones still load. Plain-named Streamline files are never loaded by this code.
//
//   plain name           renamed copy
//   sl.interposer.dll    xefg_slinterposer.dll
//   sl.common.dll        sl.xefg_common.dll
//   sl.dlss_g.dll        sl.xefg_dlss_g.dll
//   sl.reflex.dll        sl.xefg_reflex.dll
//   sl.pcl.dll           sl.xefg_pcl.dll
//   nvngx_dlssg.dll      nvngx_dlssg.dll (keeps its name: the plugin looks for it)
//
// Search order (the first folder that holds a complete set wins):
//   1. F5LOW_STREAMLINE_DIR (environment; OptiScaler passes its Streamline folder)
//   2. DlssgPath= in f5low.ini / XEFG_CUDA_DLSSGPATH (the older setting)
//   3. <this DLL's folder>\..\streamline and ..\Streamline (OptiScaler\plugins\f5low.dll -> OptiScaler\streamline)
// A folder with the plain names is copied into <this DLL's folder>\f5low_streamline\ (renamed), keyed by the SHA-256 of each
// source file in f5low_streamline\manifest.txt: a changed source is copied again, an unchanged one is not. A folder that already
// has the renamed names (and no plain-named sl.*.dll, which Streamline's plugin scan would also pick up) is used as it is.
// Copies are written under a temporary name and moved into place. Nothing is ever modified.
#pragma once

#include "../intel_modules.h"
#include "../long_path.h"

#include <windows.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

namespace xdlssg
{

struct FilePair
{
    const wchar_t* plain;
    const wchar_t* renamed;
};
inline constexpr FilePair kFiles[] = {
    { L"sl.interposer.dll", L"xefg_slinterposer.dll" }, { L"sl.common.dll", L"sl.xefg_common.dll" }, { L"sl.dlss_g.dll", L"sl.xefg_dlss_g.dll" },
    { L"sl.reflex.dll", L"sl.xefg_reflex.dll" },        { L"sl.pcl.dll", L"sl.xefg_pcl.dll" },        { L"nvngx_dlssg.dll", L"nvngx_dlssg.dll" },
};
// Plain-named Streamline plugins a folder we load from must not hold (Streamline's scan for sl.*.dll would load them, and OptiScaler hooks them)
inline constexpr const wchar_t* kPlainPlugins[] = { L"sl.common.dll", L"sl.dlss_g.dll", L"sl.reflex.dll", L"sl.pcl.dll", L"sl.dlss.dll",
                                                    L"sl.dlss_d.dll", L"sl.nis.dll", L"sl.deepdvc.dll", L"sl.directsr.dll", L"sl.imgui.dll" };

inline std::string Narrow(const std::wstring& w) { return longpath::ToUtf8(longpath::Plain(w)); }

inline bool FileExists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(longpath::ExtendIfLong(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && !(a & FILE_ATTRIBUTE_DIRECTORY);
}
inline bool DirExists(const std::wstring& p)
{
    const DWORD a = GetFileAttributesW(longpath::ExtendIfLong(p).c_str());
    return a != INVALID_FILE_ATTRIBUTES && (a & FILE_ATTRIBUTE_DIRECTORY);
}

inline bool ReadAll(const std::wstring& path, std::vector<uint8_t>& out)
{
    HANDLE f = CreateFileW(longpath::ExtendIfLong(path).c_str(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_DELETE, nullptr, OPEN_EXISTING,
                           FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
        return false;
    LARGE_INTEGER sz {};
    bool ok = GetFileSizeEx(f, &sz) && sz.QuadPart >= 0 && sz.QuadPart < (1ll << 30);
    if (ok)
    {
        out.resize(static_cast<size_t>(sz.QuadPart));
        size_t done = 0;
        while (ok && done < out.size())
        {
            DWORD got = 0;
            ok = ReadFile(f, out.data() + done, static_cast<DWORD>(std::min<size_t>(out.size() - done, 1u << 24)), &got, nullptr) && got;
            done += got;
        }
    }
    CloseHandle(f);
    return ok;
}

// Writes the bytes under a temporary name next to `path`, then moves them into place (an old file is replaced; a process that
// still has it loaded makes the move fail with a sharing violation).
inline bool WriteAtomic(const std::wstring& path, const std::vector<uint8_t>& data, DWORD* err = nullptr)
{
    const std::wstring tmp = path + L".tmp" + std::to_wstring(GetCurrentProcessId());
    const std::wstring tmpE = longpath::ExtendIfLong(tmp);
    HANDLE f = CreateFileW(tmpE.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (f == INVALID_HANDLE_VALUE)
    {
        if (err)
            *err = GetLastError();
        return false;
    }
    size_t done = 0;
    bool ok = true;
    while (ok && done < data.size())
    {
        DWORD put = 0;
        ok = WriteFile(f, data.data() + done, static_cast<DWORD>(std::min<size_t>(data.size() - done, 1u << 24)), &put, nullptr) && put;
        done += put;
    }
    ok = ok && FlushFileBuffers(f);
    if (!ok && err)
        *err = GetLastError();
    CloseHandle(f);
    if (ok && !MoveFileExW(tmpE.c_str(), longpath::ExtendIfLong(path).c_str(), MOVEFILE_REPLACE_EXISTING))
    {
        ok = false;
        if (err)
            *err = GetLastError();
    }
    if (!ok)
        DeleteFileW(tmpE.c_str());
    return ok;
}

inline std::string HashHex(const std::vector<uint8_t>& d)
{
    intel_modules::Sha256 h {};
    return intel_modules::HashBytes(d.data(), d.size(), h) ? intel_modules::ToHex(h) : std::string();
}

// What the search found.
struct Source
{
    bool ok = false;
    std::wstring dir;           // the folder to hand to Streamline (renamed files, nothing plain-named)
    std::string how;            // one log line: where from, copied or reused
    std::string why;            // not ok: why
};

enum class Kind { Missing, Plain, Renamed, Incomplete };

// A candidate folder: a complete plain set, a complete renamed set, or neither (what is missing, in `what`).
inline Kind Classify(const std::wstring& dir, std::string& what)
{
    if (!DirExists(dir))
    {
        what = "no such folder";
        return Kind::Missing;
    }
    std::string missPlain, missRenamed;
    for (const FilePair& f : kFiles)
    {
        if (!FileExists(dir + L"\\" + f.plain))
            missPlain += (missPlain.empty() ? "" : ", ") + longpath::ToUtf8(f.plain);
        if (!FileExists(dir + L"\\" + f.renamed))
            missRenamed += (missRenamed.empty() ? "" : ", ") + longpath::ToUtf8(f.renamed);
    }
    if (missRenamed.empty())
        return Kind::Renamed;
    if (missPlain.empty())
        return Kind::Plain;
    // Nothing of Streamline there at all, or a partial set (the closer of the two sets is named)
    if (!FileExists(dir + L"\\sl.interposer.dll") && !FileExists(dir + L"\\xefg_slinterposer.dll"))
    {
        what = "no Streamline there (no sl.interposer.dll)";
        return Kind::Missing;
    }
    what = "incomplete, missing " + (missPlain.size() <= missRenamed.size() ? missPlain : missRenamed);
    return Kind::Incomplete;
}

// Copies the plain set of `src` into `dst` under the renamed names unless the manifest there says it is current.
// `copied` counts the files written.
inline bool MakeCopy(const std::wstring& src, const std::wstring& dst, unsigned& copied, std::string& why)
{
    CreateDirectoryW(longpath::ExtendIfLong(dst).c_str(), nullptr);
    if (!DirExists(dst))
    {
        why = "cannot create " + Narrow(dst);
        return false;
    }
    // manifest.txt: one "<renamed name> <SHA-256 of the source file>" per line
    std::vector<uint8_t> mf;
    std::string old(ReadAll(dst + L"\\manifest.txt", mf) ? std::string(mf.begin(), mf.end()) : std::string());
    std::string manifest;
    copied = 0;
    for (const FilePair& f : kFiles)
    {
        std::vector<uint8_t> data;
        if (!ReadAll(src + L"\\" + f.plain, data))
        {
            why = "cannot read " + Narrow(src + L"\\" + f.plain);
            return false;
        }
        const std::string sha = HashHex(data);
        if (sha.empty())
        {
            why = "cannot hash " + Narrow(src + L"\\" + f.plain);
            return false;
        }
        const std::string name = longpath::ToUtf8(f.renamed);
        const std::string line = name + " " + sha;
        const std::wstring to = dst + L"\\" + f.renamed;
        bool current = false;
        for (size_t p = 0; p < old.size() && !current;)
        {
            size_t e = old.find('\n', p);
            if (e == std::string::npos)
                e = old.size();
            std::string l = old.substr(p, e - p);
            while (!l.empty() && (l.back() == '\r' || l.back() == ' '))
                l.pop_back();
            current = l == line;
            p = e + 1;
        }
        if (current)
        {
            // the manifest says so; the copy itself must still be what it was made from
            std::vector<uint8_t> have;
            current = ReadAll(to, have) && have.size() == data.size() && HashHex(have) == sha;
        }
        if (!current)
        {
            DWORD err = 0;
            if (!WriteAtomic(to, data, &err))
            {
                // a copy of the same bytes already there (loaded by another process) is as good
                std::vector<uint8_t> have;
                if (!(ReadAll(to, have) && have.size() == data.size() && HashHex(have) == sha))
                {
                    why = "cannot write " + Narrow(to) + " (error " + std::to_string(err) + ")";
                    return false;
                }
            }
            else
                ++copied;
        }
        manifest += line + "\r\n";
    }
    if (manifest != old)
    {
        const std::vector<uint8_t> m(manifest.begin(), manifest.end());
        WriteAtomic(dst + L"\\manifest.txt", m);        // a failed manifest only means the next start hashes again
    }
    return true;
}

// Finds Streamline and makes the folder to load from.
//   selfDir:   the folder of f5low.dll
//   iniPath:   DlssgPath from the environment / ini ("" = none)
inline Source Prepare(const std::wstring& selfDir, const std::wstring& envDir, const std::wstring& iniPath)
{
    Source s;
    struct Cand
    {
        std::wstring dir;
        const char* from;
    };
    std::vector<Cand> cands;
    const auto add = [&](std::wstring d, const char* from) {
        while (!d.empty() && (d.back() == L'\\' || d.back() == L'/' || d.back() == L' '))
            d.pop_back();
        if (d.empty())
            return;
        for (const Cand& c : cands)
            if (_wcsicmp(c.dir.c_str(), d.c_str()) == 0)
                return;
        cands.push_back({ std::move(d), from });
    };
    add(envDir, "F5LOW_STREAMLINE_DIR");
    add(iniPath, "DlssgPath");
    std::wstring up = selfDir;
    while (!up.empty() && up.back() == L'\\')
        up.pop_back();
    if (const size_t slash = up.find_last_of(L'\\'); slash != std::wstring::npos)
    {
        up.resize(slash);
        add(up + L"\\streamline", "the folder above this DLL");
        add(up + L"\\Streamline", "the folder above this DLL");
    }
    std::string tried;
    for (const Cand& c : cands)
    {
        std::string what;
        const Kind k = Classify(c.dir, what);
        if (k == Kind::Renamed)
        {
            // Streamline loads every sl.*.dll in the folder: a plain-named plugin next to ours would be loaded (and hooked by OptiScaler)
            std::string plain;
            for (const wchar_t* p : kPlainPlugins)
                if (FileExists(c.dir + L"\\" + p))
                    plain += (plain.empty() ? "" : ", ") + longpath::ToUtf8(p);
            if (!plain.empty())
            {
                tried += std::string(tried.empty() ? "" : "; ") + Narrow(c.dir) + " (" + c.from + "): has the renamed files but also " + plain +
                         " - Streamline would load those too";
                continue;
            }
            s.ok = true;
            s.dir = c.dir;
            s.how = "Streamline (renamed files) used as it is from " + Narrow(c.dir) + " (" + c.from + ")";
            return s;
        }
        if (k == Kind::Plain)
        {
            const std::wstring dst = selfDir + (selfDir.empty() || selfDir.back() == L'\\' ? L"" : L"\\") + L"f5low_streamline";
            unsigned copied = 0;
            std::string why;
            if (!MakeCopy(c.dir, dst, copied, why))
            {
                tried += std::string(tried.empty() ? "" : "; ") + Narrow(c.dir) + " (" + c.from + "): " + why;
                continue;
            }
            s.ok = true;
            s.dir = dst;
            s.how = "Streamline from " + Narrow(c.dir) + " (" + c.from + "): " + (copied ? std::to_string(copied) + " file(s) copied under the renamed names into "
                                                                                          : std::string("renamed copy is current in ")) +
                    Narrow(dst);
            return s;
        }
        tried += std::string(tried.empty() ? "" : "; ") + Narrow(c.dir) + " (" + c.from + "): " + what;
    }
    s.why = cands.empty() ? "no Streamline folder to look in" : "no usable Streamline folder: " + tried;
    return s;
}

} // namespace xdlssg
