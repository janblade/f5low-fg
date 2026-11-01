// dxc_disasm.cpp - see dxc_disasm.h.
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <unknwn.h>

#include "dxc_disasm.h"

#include <cstdio>

#pragma comment(lib, "version.lib")

namespace dxc_disasm {
namespace {

// The parts of dxcapi.h used here. Only the order of the virtual methods matters (the COM ABI); methods that are
// never called are declared with placeholder parameters.
struct IBlob : IUnknown {
    virtual LPVOID STDMETHODCALLTYPE GetBufferPointer() = 0;
    virtual SIZE_T STDMETHODCALLTYPE GetBufferSize() = 0;
};
struct IBlobEncoding : IBlob {
    virtual HRESULT STDMETHODCALLTYPE GetEncoding(BOOL* known, UINT32* codePage) = 0;
};
struct IUtils : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE CreateBlobFromBlob(IBlob* blob, UINT32 offset, UINT32 length, IBlobEncoding** out) = 0;
    virtual HRESULT STDMETHODCALLTYPE CreateBlobFromPinned(LPCVOID data, UINT32 size, UINT32 codePage, IBlobEncoding** out) = 0;
};
struct ICompiler : IUnknown {
    virtual HRESULT STDMETHODCALLTYPE Compile(void*, void*, void*, void*, void*, UINT32, void*, UINT32, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Preprocess(void*, void*, void*, UINT32, void*, UINT32, void*, void*) = 0;
    virtual HRESULT STDMETHODCALLTYPE Disassemble(IBlob* source, IBlobEncoding** disassembly) = 0;
};

const GUID kClsidCompiler = {0x73e22d93, 0xe6ce, 0x47f3, {0xb5, 0xbf, 0xf0, 0x66, 0x4f, 0x39, 0xc1, 0xb0}};
const GUID kClsidUtils = {0x6245d6af, 0x66e0, 0x48fd, {0x80, 0xb4, 0x4d, 0x27, 0x17, 0x96, 0x74, 0x8c}};
const GUID kIidCompiler = {0x8c210bf3, 0x011f, 0x4422, {0x8d, 0x70, 0x6f, 0x9a, 0xcb, 0x8d, 0xb6, 0x17}};
const GUID kIidUtils = {0x4605c4cb, 0x2019, 0x492a, {0xad, 0xa4, 0x65, 0xf2, 0x0b, 0xb7, 0xd6, 0x7f}};

using CreateInstanceProc = HRESULT(WINAPI*)(REFCLSID, REFIID, LPVOID*);

void Fail(std::string* error, const std::string& msg) {
    if (error) *error = msg;
}

}  // namespace

Disassembler::~Disassembler() {
    if (compiler_) static_cast<ICompiler*>(compiler_)->Release();
    if (utils_) static_cast<IUtils*>(utils_)->Release();
    if (module_) FreeLibrary(static_cast<HMODULE>(module_));
}

bool Disassembler::Load(const std::wstring& path, std::string* error) {
    if (compiler_) return true;
    HMODULE m = LoadLibraryExW(path.c_str(), nullptr, LOAD_WITH_ALTERED_SEARCH_PATH);
    if (!m) {
        Fail(error, "dxcompiler.dll not loadable (error " + std::to_string(GetLastError()) + ")");
        return false;
    }
    auto create = reinterpret_cast<CreateInstanceProc>(GetProcAddress(m, "DxcCreateInstance"));
    ICompiler* comp = nullptr;
    IUtils* utils = nullptr;
    if (!create || FAILED(create(kClsidCompiler, kIidCompiler, reinterpret_cast<LPVOID*>(&comp))) ||
        FAILED(create(kClsidUtils, kIidUtils, reinterpret_cast<LPVOID*>(&utils)))) {
        if (comp) comp->Release();
        FreeLibrary(m);
        Fail(error, "the DLL is no DirectX Shader Compiler (DxcCreateInstance failed)");
        return false;
    }
    module_ = m;
    compiler_ = comp;
    utils_ = utils;
    // file version, for the log
    DWORD dummy = 0, sz = GetFileVersionInfoSizeW(path.c_str(), &dummy);
    if (sz) {
        std::string buf(sz, '\0');
        VS_FIXEDFILEINFO* fi = nullptr;
        UINT len = 0;
        if (GetFileVersionInfoW(path.c_str(), 0, sz, buf.data()) && VerQueryValueW(buf.data(), L"\\", reinterpret_cast<LPVOID*>(&fi), &len) && fi)
            version_ = std::to_string(HIWORD(fi->dwFileVersionMS)) + "." + std::to_string(LOWORD(fi->dwFileVersionMS)) + "." +
                       std::to_string(HIWORD(fi->dwFileVersionLS)) + "." + std::to_string(LOWORD(fi->dwFileVersionLS));
    }
    return true;
}

bool Disassembler::Disassemble(const uint8_t* container, size_t size, std::string& text, std::string* error) const {
    if (!compiler_) {
        Fail(error, "dxcompiler not loaded");
        return false;
    }
    IBlobEncoding* src = nullptr;
    if (FAILED(static_cast<IUtils*>(utils_)->CreateBlobFromPinned(container, static_cast<UINT32>(size), 0, &src)) || !src) {
        Fail(error, "CreateBlobFromPinned failed");
        return false;
    }
    IBlobEncoding* out = nullptr;
    HRESULT hr = static_cast<ICompiler*>(compiler_)->Disassemble(src, &out);
    src->Release();
    if (FAILED(hr) || !out) {
        char code[32];
        snprintf(code, sizeof code, "0x%08lX", static_cast<unsigned long>(hr));
        Fail(error, std::string("Disassemble failed (") + code + ")");
        return false;
    }
    const char* p = static_cast<const char*>(out->GetBufferPointer());
    size_t n = out->GetBufferSize();
    while (n && p[n - 1] == '\0') --n;
    text.assign(p, n);
    out->Release();
    return true;
}

}  // namespace dxc_disasm
