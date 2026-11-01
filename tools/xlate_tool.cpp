// xlate_tool - build-time helper of build.ps1: turns the disassembly of one of OUR OWN HLSL templates (dxc output)
// into PTX with src/translate.cpp. It never sees an Intel module, and f5low.dll does not contain the translator.
//
//   xlate_tool <disassembly file> <out.ptx> <name in the PTX header> [fmt ...]
#include <cstdio>
#include <fstream>
#include <iterator>
#include <string>
#include <vector>

#include "translate.h"

int main(int argc, char** argv) {
    if (argc < 4) {
        fputs("usage: xlate_tool <disassembly> <out.ptx> <source name> [fmt ...]\n", stderr);
        return 2;
    }
    std::ifstream in(argv[1], std::ios::binary);
    if (!in) {
        fprintf(stderr, "xlate_tool: cannot read %s\n", argv[1]);
        return 1;
    }
    std::string raw((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>()), text;
    for (size_t i = 0; i < raw.size(); ++i)  // dxc.exe writes CR LF line ends; the translator reads LF text
        if (!(raw[i] == '\r' && i + 1 < raw.size() && raw[i + 1] == '\n')) text += raw[i];
    std::vector<std::string> fmts(argv + 4, argv + argc);
    std::string ptx, err;
    if (!translate::TranslateEntry(text, "plain", fmts, argv[3], ptx, &err)) {
        fprintf(stderr, "xlate_tool: %s: %s\n", argv[1], err.c_str());
        return 1;
    }
    std::ofstream out(argv[2], std::ios::binary);
    out.write(ptx.data(), static_cast<std::streamsize>(ptx.size()));
    return out ? 0 : 1;
}
