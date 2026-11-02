"""No-vendor-content audit for f5low-fg (README rule). Read-only.

  python tools/audit.py                  audit the repo's tracked files (git ls-files), exit 1 on a finding
  python tools/audit.py --tree <dir>     audit every file under <dir>
  python tools/audit.py --dll <file>     audit a built f5low.dll: no embedded pack, shader container or zstd data
  python tools/audit.py --self-test      plants each kind of finding in a temp folder and checks it is caught

Flags, per file:
  ext       a vendor/derived file type (.dll .dxbc .ptx .bin ...)
  magic     a container signature anywhere in the file: DXBC (with its header, not the bare word), zstd
            frame/dictionary, PE image, ELF, CUDA fatbin, our embedded pack (XFPK version 1)
  ptx       PTX text (".target sm_" with ".entry"); our own kernels are generated at build time, never committed
  modid     a bare 8-digit upper-case hex token starting with 0 (how Intel module offsets are named, e.g. 01326A40)
  rva       a hex number on a line that says RVA / offset into the vendor image
  bytes     a run of 24+ byte literals (0x.., 0x.., ...): a byte pattern
  entropy   a file over 64 KB at more than 7.5 bits per byte (compressed or encrypted data)
  nvidia    NVIDIA module content: a file named like one of NVIDIA's modules (nvngx_*, sl.*.dll, the renamed Streamline copies
            of the dlssg generator), a CUDA fatbin (magic 0xBA55ED50, or the fatbin wrapper 0x466243B1), an .nv_fatb PE
            section or the nvFatbin API's name inside a non-text file. The dlssg generator copies the player's own NVIDIA files
            at run time; none of them (and nothing derived from them) is ever committed or compiled in.

A finding the rule allows anyway goes in tools/audit_allow.txt as "<path>:<check>  <reason>"; keep it short.
"""
import argparse
import math
import os
import re
import subprocess
import sys
import tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ALLOW_FILE = os.path.join(ROOT, "tools", "audit_allow.txt")

BAD_EXT = {".dll", ".exe", ".lib", ".exp", ".pdb", ".sys", ".dxbc", ".dxil", ".dxasm", ".cso", ".bin", ".ptx",
           ".cubin", ".fatbin", ".zst", ".zstd", ".dict", ".npy", ".npz", ".pt", ".onnx"}

MAGICS = {
    bytes.fromhex("28b52ffd"): "zstd frame",
    bytes.fromhex("37a430ec"): "zstd dictionary",
    bytes.fromhex("7f454c46"): "ELF image",
    bytes.fromhex("50ed55ba"): "CUDA fatbin",
    b"XFPK" + bytes.fromhex("01000000"): "embedded pack",
    bytes.fromhex("b1436246"): "CUDA fatbin wrapper",
}
# NVIDIA module content (the "nvidia" check): file names, and section / API names inside binary files
RE_NV_NAME = re.compile(r"(^|/)(nvngx_[^/]*|nvngx\.[^/]*|sl\.[^/]*\.dll|sl\.xefg_[^/]*|xefg_slinterposer\.dll|nvapi64?\.dll|nvcuda\.dll)$", re.I)
NV_BIN_MARKERS = (b".nv_fatb", b"nvFatbin", b"__nv_fatbin", b".nv.info")
# DXBC: the signature, a 16-byte digest, then version 1.0 (the bare word is fine in docs)
RE_DXBC = re.compile(b"DXBC.{16}" + re.escape(bytes.fromhex("01000000")), re.S)

TEXT_EXT = {".py", ".cpp", ".c", ".h", ".hpp", ".cu", ".cuh", ".hlsl", ".hlsli", ".ps1", ".md", ".txt", ".ini",
            ".yml", ".yaml", ".json", ".cmake", ".bat", ".cmd", ""}

RE_MODID = re.compile(r"(?<![0-9A-Za-z_])0[0-9A-F]{7}(?![0-9A-Za-z_])")
RE_RVA = re.compile(r"\b(rva|RVA|image offset|file offset)\b.*\b0x[0-9a-fA-F]{4,}\b"
                    r"|\b0x[0-9a-fA-F]{4,}\b.*\b(rva|RVA)\b")
RE_BYTES = re.compile(r"(?:0x[0-9a-fA-F]{2}\s*,\s*){24,}")
RE_PTX = (re.compile(r"^\s*\.target\s+sm_\d+", re.M), re.compile(r"^\s*(\.visible\s+)?\.entry\b", re.M))


def entropy(data: bytes) -> float:
    if not data:
        return 0.0
    counts = [0] * 256
    for b in data:
        counts[b] += 1
    n = len(data)
    return -sum(c / n * math.log2(c / n) for c in counts if c)


def is_pe(data: bytes) -> bool:
    if data[:2] != b"MZ" or len(data) < 0x40:
        return False
    off = int.from_bytes(data[0x3C:0x40], "little")
    return data[off:off + 4] == b"PE" + bytes(2)


def containers(data: bytes):
    found = [what for sig, what in MAGICS.items() if sig in data]
    if RE_DXBC.search(data):
        found.append("DXBC container")
    return found


def check_file(path: str, rel: str):
    found = []
    ext = os.path.splitext(rel)[1].lower()
    if ext in BAD_EXT:
        found.append(("ext", f"file type {ext}"))
    if RE_NV_NAME.search(rel.replace("\\", "/")):
        found.append(("nvidia", "file named like an NVIDIA module"))
    with open(path, "rb") as f:
        data = f.read()
    if is_pe(data):
        found.append(("magic", "PE image"))
    found += [("magic", what) for what in containers(data)]
    if ext not in TEXT_EXT:
        found += [("nvidia", f"{m.decode()} in a binary file") for m in NV_BIN_MARKERS if m in data]
    if len(data) > 64 * 1024:
        e = entropy(data)
        if e > 7.5:
            found.append(("entropy", f"{e:.2f} bits/byte"))
    if ext in TEXT_EXT:
        text = data.decode("utf-8", errors="replace")
        if all(r.search(text) for r in RE_PTX):
            found.append(("ptx", "PTX kernel text"))
        for no, line in enumerate(text.splitlines(), 1):
            m = RE_MODID.search(line)
            if m:
                found.append(("modid", f"line {no}: {m.group(0)}"))
            if RE_RVA.search(line):
                found.append(("rva", f"line {no}"))
            if RE_BYTES.search(line):
                found.append(("bytes", f"line {no}"))
    return found


def load_allow():
    allow = set()
    if os.path.exists(ALLOW_FILE):
        for line in open(ALLOW_FILE, encoding="utf-8"):
            w = line.split()
            if w and not w[0].startswith("#"):
                allow.add(w[0].replace("\\", "/"))
    return allow


def audit_files(files, base):
    allow = load_allow()
    bad = 0
    for rel in files:
        rel = rel.replace("\\", "/")
        if rel in ("tools/audit.py", "tools/audit_allow.txt"):
            continue  # holds the signatures it looks for
        for check, what in check_file(os.path.join(base, rel), rel):
            if f"{rel}:{check}" in allow:
                continue
            print(f"FAIL {rel}: {check}: {what}")
            bad += 1
    return bad


def tracked_files():
    out = subprocess.run(["git", "-C", ROOT, "ls-files", "-z"], capture_output=True, check=True).stdout
    return [p for p in out.decode("utf-8").split("\0") if p]


def tree_files(folder):
    return [os.path.relpath(os.path.join(r, n), folder) for r, _, ns in os.walk(folder) for n in ns
            if ".git" not in os.path.relpath(r, folder).split(os.sep)]


# Occurrences inside a built DLL that are the run-time decoder's own constants, not data (see audit_dll).
DLL_TOLERATED = {"zstd frame": 8, "zstd dictionary": 8, "DXIL": 3}

# PTX kernels we write by hand in the runtime sources (xefg_pack.h, xefg_pack_d3d.h); any other entry is not ours.
OWN_PTX_ENTRIES = {b"swaprb", b"fill"}
RE_ENTRY = re.compile(rb"\.entry\s+([A-Za-z_][A-Za-z0-9_]*)")


BUNDLE_MAGIC = b"F5OWN1\n"


def parse_bundle(data, at):
    """The bundle of tools/recipe/build_own.py at `at`: ("<size> <name>\\n", bytes, "\\n")* then "end\\n".
    Returns (end offset, [(name, bytes)]), or (0, []) if what follows is not a complete bundle."""
    pos = at + len(BUNDLE_MAGIC)
    records = []
    while True:
        eol = data.find(b"\n", pos, pos + 600)
        if eol < 0:
            return 0, []
        line = data[pos:eol]
        pos = eol + 1
        if line == b"end":
            return pos, records
        size, _, name = line.partition(b" ")
        if not size.isdigit() or not name:
            return 0, []
        n = int(size)
        if pos + n + 1 > len(data) or data[pos + n:pos + n + 1] != b"\n":
            return 0, []
        records.append((name.decode("ascii", "replace"), data[pos:pos + n]))
        pos += n + 1


def own_blob_hashes(path):
    """SHA-256 list build.ps1 writes (first word per line): the shader containers it compiles in and every file of the
    bundle it embeds (resource 102)."""
    if not path or not os.path.exists(path):
        return set()
    return {line.split()[0].lower() for line in open(path, encoding="ascii") if line.strip()}


def audit_dll(path, own_blobs=None):
    import hashlib
    with open(path, "rb") as f:
        data = f.read()
    own = own_blob_hashes(own_blobs)
    # The DLL decodes the player's libxess_fg at run time (src/intel_modules.cpp + vendored zstd): its code holds the
    # zstd frame/dictionary magic constants and looks for the word DXIL in decoded data. A few occurrences of those are
    # that code; a payload would carry one per frame (and show up as noise below).
    found = []
    for sig, what in MAGICS.items():
        n = data.count(sig)
        if n > DLL_TOLERATED.get(what, 0):
            found.append(what + (f" (x{n})" if n > 1 else ""))
    for m in RE_DXBC.finditer(data):
        size = int.from_bytes(data[m.start() + 24:m.start() + 28], "little")
        blob = data[m.start():m.start() + size]
        if hashlib.sha256(blob).hexdigest() not in own:
            found.append(f"DXBC container at 0x{m.start():x} ({size} bytes), not one we compile")
    if data.count(b"DXIL") > DLL_TOLERATED["DXIL"]:
        found.append("DXIL part")
    for m in NV_BIN_MARKERS:
        if m in data:
            found.append(f"NVIDIA {m.decode()} (fatbin section / API name)")
    # A compressed payload (an embedded pack is LZMS, which hides every signature) reads as noise: code and data
    # sections stay well under 7.5 bits per byte in 64 KB windows.
    win = 64 * 1024
    noisy = [o for o in range(0, len(data) - win + 1, win) if entropy(data[o:o + win]) > 7.5]
    if noisy:
        found.append(f"compressed/encrypted data: {len(noisy)} x 64 KB from 0x{noisy[0]:x}")
    # Resource 102 (build_own.py's bundle) is located by its structure; every file in it must be listed by hash in
    # own_blobs.txt (what our build put in). Its bytes are then set aside: PTX text anywhere else in the DLL may only
    # be the hand-written utility kernels.
    rest = bytearray(data)
    at = data.find(BUNDLE_MAGIC)
    while at >= 0:
        end, records = parse_bundle(data, at)
        if end:
            for name, blob in records:
                if hashlib.sha256(blob).hexdigest() not in own:
                    found.append(f"bundle file {name} ({len(blob)} bytes) not built by us (not in own_blobs.txt)")
            rest[at:end] = bytes(end - at)
            at = data.find(BUNDLE_MAGIC, end)
        else:  # the same word as a constant in the DLL's code, not a bundle
            at = data.find(BUNDLE_MAGIC, at + 1)
    if b".target sm_" in rest:
        names = set(RE_ENTRY.findall(bytes(rest)))
        if not names or names - OWN_PTX_ENTRIES:
            found.append(f"PTX text outside our bundle with entries {sorted(n.decode() for n in names - OWN_PTX_ENTRIES) or '?'}")
    for what in found:
        print(f"FAIL {path}: {what} inside the DLL")
    return len(found)


def self_test():
    pe = b"MZ" + bytes(0x3A) + (0x40).to_bytes(4, "little") + b"PE" + bytes(2)
    plants = {
        "a/libxess_fg.dll": (pe, {"ext", "magic"}),
        "a/mod.txt": (b"xx DXBC" + bytes(range(16)) + bytes.fromhex("01000000") + b" yy", {"magic"}),
        "a/frame.dat": (bytes(1) + bytes.fromhex("28b52ffd") + bytes(1), {"magic"}),
        "a/dict.dat": (bytes.fromhex("37a430ec"), {"magic"}),
        "a/k.cu": (b".version 8.0\n.target sm_80\n.visible .entry k(\n", {"ptx"}),
        "a/w.bin": (b"12", {"ext"}),
        "a/names.py": (b"MODS = ['01326A40']\n", {"modid"}),
        "a/sites.h": (b"constexpr uint32_t kSite = 0x18C469; // RVA of the ceiling check\n", {"rva"}),
        "a/pat.h": (b"const unsigned char p[] = {" + b"0x90, " * 30 + b"};\n", {"bytes"}),
        "a/blob.dat": (os.urandom(80 * 1024), {"entropy"}),
        # NVIDIA content (planted files, never real ones)
        "a/nvngx_dlssg.dll": (b"MZ", {"ext", "nvidia"}),
        "a/streamline/sl.dlss_g.dll": (b"MZ", {"ext", "nvidia"}),
        "a/streamline/sl.xefg_pcl.dll": (b"MZ", {"ext", "nvidia"}),
        "a/nvngx_note.txt": (b"just a name", {"nvidia"}),
        "a/fatbin.dat": (b"\x00\x00" + bytes.fromhex("50ed55ba") + b"\x00\x00", {"magic"}),
        "a/wrapper.dat": (b"\x00" + bytes.fromhex("b1436246") + b"\x00", {"magic"}),
        "a/sec.dat": (b"\x00\x01.nv_fatb\x00\x01", {"nvidia"}),
        "a/api.dat": (b"\x00nvFatbinCreate\x00", {"nvidia"}),
    }
    clean = {"b/ok.md": b"No DXBC, DXIL or XFPK packs in here. The dlssg generator copies nvngx_dlssg.dll and sl.dlss_g.dll at run time.\n"
                        b"A text file may name .nv_fatb and nvFatbin without being one.\n",
             "b/dlssg_files.h": b"constexpr const wchar_t* k = L\"nvngx_dlssg.dll\";\n",
             "b/ok.cpp": b"// SHA-256 e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855\n"
                         b"uint32_t mask = 0x80808080; int x = 0x10;\n"}
    ok = True
    with tempfile.TemporaryDirectory() as tmp:
        for rel, data in [(r, d) for r, (d, _) in plants.items()] + list(clean.items()):
            os.makedirs(os.path.join(tmp, os.path.dirname(rel)), exist_ok=True)
            with open(os.path.join(tmp, rel), "wb") as f:
                f.write(data)
        for rel, (_, want) in plants.items():
            got = {c for c, _ in check_file(os.path.join(tmp, rel), rel)}
            miss = want - got
            print(f"{'ok  ' if not miss else 'MISS'} {rel}: want {sorted(want)} got {sorted(got)}")
            ok &= not miss
        for rel in clean:
            got = check_file(os.path.join(tmp, rel), rel)
            print(f"{'ok  ' if not got else 'FALSE'} {rel}: clean, got {got}")
            ok &= not got
        dll = os.path.join(tmp, "a", "f5low_test.dll")
        with open(dll, "wb") as f:
            f.write(b"MZ.." + b"XFPK" + bytes.fromhex("01000000") + b"..")
        caught = audit_dll(dll) > 0
        print(f"{'ok  ' if caught else 'MISS'} --dll catches an embedded pack")
        ok &= caught
        import hashlib
        dxbc = b"DXBC" + bytes(16) + bytes.fromhex("01000000") + (40).to_bytes(4, "little") + bytes(12)
        own_list = os.path.join(tmp, "own_blobs.txt")
        ptx_own = b".target sm_75\n.visible .entry swaprb(\n"
        ptx_other = b".target sm_80\n.visible .entry k04_conv(\n"
        ptx_gen = b".target sm_80\n.visible .entry dxil_main(\n// ours\n"          # listed
        ptx_intel = b".target sm_80\n.visible .entry dxil_main(\n// translated\n"  # same entry name, not listed
        with open(own_list, "w", encoding="ascii") as f:
            f.write(hashlib.sha256(dxbc).hexdigest() + "  test\n")
            f.write(hashlib.sha256(ptx_gen).hexdigest() + "  listed ptx\n")

        def bundle(*files):
            out = b"F5OWN1\n"
            for i, d in enumerate(files):
                out += f"{len(d)} i0/f{i}.ptx\n".encode() + d + b"\n"
            return out + b"end\n"
        cases = [("own DXBC + own PTX pass", b"MZ" + dxbc + ptx_own, own_list, False),
                 ("unlisted DXBC fails", b"MZ" + dxbc, None, True),
                 ("foreign PTX entry fails", b"MZ" + ptx_own + ptx_other, own_list, True),
                 ("listed PTX inside the bundle passes", b"MZ" + ptx_own + bundle(ptx_gen, ptx_gen), own_list, False),
                 ("unlisted PTX (same entry name) inside the bundle fails",
                  b"MZ" + bundle(ptx_gen, ptx_intel), own_list, True),
                 ("listed PTX outside the bundle fails", b"MZ" + ptx_gen + bundle(ptx_gen), own_list, True),
                 ("the bundle magic as a code constant is no bundle", b"MZ" + b"F5OWN1\n\0\0" + ptx_own, own_list, False),
                 ("decoder constants (3 zstd magics) pass", b"MZ" + (bytes.fromhex("28b52ffd") + bytes(8)) * 3, own_list, False),
                 ("a zstd payload (40 frames) fails", b"MZ" + (bytes.fromhex("28b52ffd") + bytes(8)) * 40, own_list, True),
                 ("compressed payload fails",b"MZ" + os.urandom(200 * 1024), own_list, True),
                 ("a CUDA fatbin fails", b"MZ" + b"\x00" + bytes.fromhex("50ed55ba") + b"\x00", own_list, True),
                 ("a CUDA fatbin wrapper fails", b"MZ" + b"\x00" + bytes.fromhex("b1436246") + b"\x00", own_list, True),
                 ("an .nv_fatb section fails", b"MZ" + b"\x00.nv_fatb\x00", own_list, True),
                 ("the nvFatbin API name fails", b"MZ" + b"\x00nvFatbinAddPTX\x00", own_list, True),
                 ("a DLL with only wide file names (nvngx_dlssg.dll) passes", b"MZ" + "nvngx_dlssg.dll".encode("utf-16-le"), own_list, False)]
        for name, data, lst, want_fail in cases:
            with open(dll, "wb") as f:
                f.write(data)
            failed = audit_dll(dll, lst) > 0
            good = failed == want_fail
            print(f"{'ok  ' if good else 'MISS'} --dll {name}")
            ok &= good
    print("SELF TEST", "PASS" if ok else "FAIL")
    return 0 if ok else 1


def main():
    ap = argparse.ArgumentParser()
    g = ap.add_mutually_exclusive_group()
    g.add_argument("--tree")
    g.add_argument("--dll")
    g.add_argument("--self-test", action="store_true")
    ap.add_argument("--own-blobs", help="with --dll: build.ps1's own_blobs.txt (default: next to the DLL)")
    a = ap.parse_args()
    if a.self_test:
        return self_test()
    if a.dll:
        bad = audit_dll(a.dll, a.own_blobs or os.path.join(os.path.dirname(os.path.abspath(a.dll)), "own_blobs.txt"))
    elif a.tree:
        bad = audit_files(tree_files(a.tree), a.tree)
    else:
        bad = audit_files(tracked_files(), ROOT)
    print("AUDIT", "PASS" if not bad else f"FAIL ({bad})")
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
