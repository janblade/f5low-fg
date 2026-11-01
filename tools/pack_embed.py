"""Packs an xefg_pack folder into one file for embedding in f5low.dll (build.ps1 -EmbedPack).

  python pack_embed.py <xefg_pack folder> <out.bin>

Layout before compression: b"XFPK", u32 version (1), u32 file count, then per file u16 name length, the name
(UTF-8, relative to the pack folder, '\\' separators, lower case), u64 size, the bytes. The whole is compressed with
Windows' Compression API (Cabinet.dll, LZMS, buffer mode), which f5low.dll decompresses with the same API, so
neither side needs another library. Windows only.
"""
import ctypes
import os
import struct
import sys
from ctypes import wintypes

COMPRESS_ALGORITHM_LZMS = 5


def compress(data: bytes) -> bytes:
    cab = ctypes.WinDLL("Cabinet.dll")
    cab.CreateCompressor.argtypes = [wintypes.DWORD, ctypes.c_void_p, ctypes.POINTER(ctypes.c_void_p)]
    cab.Compress.argtypes = [ctypes.c_void_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_void_p, ctypes.c_size_t,
                             ctypes.POINTER(ctypes.c_size_t)]
    cab.CloseCompressor.argtypes = [ctypes.c_void_p]
    h = ctypes.c_void_p()
    if not cab.CreateCompressor(COMPRESS_ALGORITHM_LZMS, None, ctypes.byref(h)):
        raise OSError(ctypes.get_last_error(), "CreateCompressor")
    try:
        src = ctypes.create_string_buffer(data, len(data))
        need = ctypes.c_size_t(0)
        cab.Compress(h, src, len(data), None, 0, ctypes.byref(need))
        out = ctypes.create_string_buffer(need.value)
        got = ctypes.c_size_t(0)
        if not cab.Compress(h, src, len(data), out, need.value, ctypes.byref(got)):
            raise OSError(ctypes.get_last_error(), "Compress")
        return out.raw[:got.value]
    finally:
        cab.CloseCompressor(h)


def capture_files(folder: str) -> set:
    """The captured contents of the per-frame inputs (frames, motion, depth: every role but the noise texture) of
    the pack and its nested packs, relative to `folder`: a game fills those every frame, so the DLL never reads
    them (xefg_pack.h leaves a missing one zero). They are most of a pack's size."""
    skip = set()
    for root, _, names in os.walk(folder):
        if "manifest.txt" not in names:
            continue
        roles, init = {}, {}
        for line in open(os.path.join(root, "manifest.txt"), encoding="utf-8"):
            w = line.split()
            if len(w) >= 3 and w[0] == "role":
                roles[w[1]] = w[2]
            elif w and w[0] == "resource" and "init" in w:
                init[w[1]] = w[w.index("init") + 1]
        for role, rid in roles.items():
            if role != "noise" and rid in init:
                full = os.path.normpath(os.path.join(root, init[rid]))
                skip.add(os.path.relpath(full, folder).replace("/", "\\").lower())
    return skip


def main(folder: str, out_path: str) -> None:
    if not os.path.isfile(os.path.join(folder, "manifest.txt")):
        sys.exit(f"no manifest.txt in {folder}")
    skip = capture_files(folder)
    files = []
    for root, _, names in os.walk(folder):
        for n in sorted(names):
            full = os.path.join(root, n)
            rel = os.path.relpath(full, folder).replace("/", "\\").lower()
            if rel in skip or ".bak" in n:
                continue            # captured inputs, manifest backups
            files.append((rel, open(full, "rb").read()))
    blob = bytearray(b"XFPK" + struct.pack("<II", 1, len(files)))
    for rel, data in files:
        name = rel.encode("utf-8")
        blob += struct.pack("<H", len(name)) + name + struct.pack("<Q", len(data)) + data
    packed = compress(bytes(blob))
    with open(out_path, "wb") as f:
        f.write(packed)
    print(f"{len(files)} files, {len(blob) / 1e6:.1f} MB -> {len(packed) / 1e6:.1f} MB ({out_path})")


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit(__doc__)
    main(sys.argv[1], sys.argv[2])
