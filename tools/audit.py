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
}
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
    with open(path, "rb") as f:
        data = f.read()
    if is_pe(data):
        found.append(("magic", "PE image"))
    found += [("magic", what) for what in containers(data)]
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


def own_blob_hashes(path):
    """SHA-256 list build.ps1 writes for the shader containers it compiles in (first word per line)."""
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
    # A compressed payload (an embedded pack is LZMS, which hides every signature) reads as noise: code and data
    # sections stay well under 7.5 bits per byte in 64 KB windows.
    win = 64 * 1024
    noisy = [o for o in range(0, len(data) - win + 1, win) if entropy(data[o:o + win]) > 7.5]
    if noisy:
        found.append(f"compressed/encrypted data: {len(noisy)} x 64 KB from 0x{noisy[0]:x}")
    if b".target sm_" in data:
        names = set(RE_ENTRY.findall(data))
        if not names or names - OWN_PTX_ENTRIES:
            found.append(f"PTX text with entries {sorted(n.decode() for n in names - OWN_PTX_ENTRIES) or '?'}")
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
    }
    clean = {"b/ok.md": b"No DXBC, DXIL or XFPK packs in here.\n",
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
        with open(own_list, "w", encoding="ascii") as f:
            f.write(hashlib.sha256(dxbc).hexdigest() + "  test\n")
        ptx_own = b".target sm_75\n.visible .entry swaprb(\n"
        ptx_other = b".target sm_80\n.visible .entry k04_conv(\n"
        cases = [("own DXBC + own PTX pass", b"MZ" + dxbc + ptx_own, own_list, False),
                 ("unlisted DXBC fails", b"MZ" + dxbc, None, True),
                 ("foreign PTX entry fails", b"MZ" + ptx_own + ptx_other, own_list, True),
                 ("decoder constants (3 zstd magics) pass", b"MZ" + (bytes.fromhex("28b52ffd") + bytes(8)) * 3, own_list, False),
                 ("a zstd payload (40 frames) fails", b"MZ" + (bytes.fromhex("28b52ffd") + bytes(8)) * 40, own_list, True),
                 ("compressed payload fails",b"MZ" + os.urandom(200 * 1024), own_list, True)]
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
