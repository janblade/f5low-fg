// dxc_disasm.h - the disassembly text of a DXIL container, from Microsoft's DirectX Shader Compiler.
//
// f5low.dll loads dxcompiler.dll (the redistributable package of microsoft/DirectXShaderCompiler, pinned version in
// docs/PACKLESS.md) from its own folder at run time. It is never part of this repo: the release packaging fetches
// it. Only IDxcUtils::CreateBlobFromPinned and IDxcCompiler::Disassemble are used, declared here by hand (the
// interface layouts are the stable COM ABI of dxcapi.h), so building f5low needs no DXC header.
#pragma once

#include <cstddef>
#include <cstdint>
#include <string>

namespace dxc_disasm {

class Disassembler {
public:
    Disassembler() = default;
    ~Disassembler();
    Disassembler(const Disassembler&) = delete;
    Disassembler& operator=(const Disassembler&) = delete;

    // Loads dxcompiler.dll from `path`. False with the reason in `error` if the file is missing or is no DXC.
    bool Load(const std::wstring& path, std::string* error = nullptr);
    bool Loaded() const { return compiler_ != nullptr; }
    // "<major>.<minor>.<build>.<rev>" of the loaded DLL, empty when unknown.
    const std::string& Version() const { return version_; }

    // The disassembly text of a DXBC/DXIL container. Same text `dxc -dumpbin` prints.
    bool Disassemble(const uint8_t* container, size_t size, std::string& text, std::string* error = nullptr) const;

private:
    void* module_ = nullptr;
    void* compiler_ = nullptr;
    void* utils_ = nullptr;
    std::string version_;
};

}  // namespace dxc_disasm
