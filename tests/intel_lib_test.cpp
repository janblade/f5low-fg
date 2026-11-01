// intel_lib_test.exe <f5low.dll> - loads a pack-less f5low.dll, calls one forwarded entry point
// (xefgSwapChainGetVersion) and prints the full path of the libxess_fg.dll that got loaded behind it, or "none".
// run_intel_lib_test.ps1 lays out the folders and the environment for each case and checks the answer; one process
// per case, since xefg_cuda resolves Intel's library once per process.
// cl /nologo /std:c++20 /EHsc /W4 intel_lib_test.cpp psapi.lib
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <psapi.h>

#include <cstdio>
#include <cwchar>

int wmain(int argc, wchar_t** argv)
{
    if (argc < 2)
    {
        fwprintf(stderr, L"usage: intel_lib_test <f5low.dll>\n");
        return 2;
    }

    HMODULE cuda = LoadLibraryW(argv[1]);
    if (!cuda)
    {
        wprintf(L"error: could not load %s (%lu)\n", argv[1], GetLastError());
        return 2;
    }

    struct Version
    {
        unsigned short major, minor, patch, reserved;
    } v {};
    using GetVersion = int(__cdecl*)(Version*);
    auto getVersion = reinterpret_cast<GetVersion>(GetProcAddress(cuda, "xefgSwapChainGetVersion"));
    if (!getVersion)
    {
        wprintf(L"error: xefgSwapChainGetVersion not exported\n");
        return 2;
    }
    const int r = getVersion(&v);

    HMODULE modules[1024];
    DWORD needed = 0;
    EnumProcessModules(GetCurrentProcess(), modules, sizeof(modules), &needed);
    const wchar_t* found = L"none";
    static wchar_t path[MAX_PATH];
    for (DWORD i = 0; i < needed / sizeof(HMODULE); ++i)
    {
        if (GetModuleFileNameW(modules[i], path, MAX_PATH) && _wcslwr_s(path, MAX_PATH) == 0 &&
            wcsstr(path, L"\\libxess_fg.dll"))
        {
            found = path;
            break;
        }
    }

    wprintf(L"result=%d version=%u.%u.%u loaded=%s\n", r, v.major, v.minor, v.patch, found);
    return 0;
}
