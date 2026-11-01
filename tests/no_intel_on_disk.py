"""The checker of tests/no_intel_on_disk.ps1: fails if any file under the given folders is, or holds, Intel-derived data.

  python -I tests/no_intel_on_disk.py --samples samples.txt --parts recipe/i0/parts.json --parts recipe/i0/hdr/parts.json
         [--allow-name f5low.dll --allow-name libxess_fg.dll ...] DIR [DIR ...]

What counts as Intel's data (everything f5low builds from the player's libxess_fg.dll):
  1. a DXBC container: the header 'DXBC' + 16-byte digest + version 1 + total size that fits the file, at ANY offset of any file
     (Intel's native modules are such containers; a log line that merely says "DXBC" is not a container);
  2. a file whose SHA-256 equals the hash of any Intel module or weight/constant blob the recipe lists
     (parts.json: module_sha256, and the sha256 of every intel_upload / intel_const blob) or the hash of a file of the pack
     built in memory (samples.txt, from tests/pack_samples.exe);
  3. a file holding any of the 64-byte windows of the pack's Intel-derived files (samples.txt, 8 per file spread over it,
     each with at least 16 distinct byte values) - the weights re-tiled or cut in pieces still carry such a window;
  4. a file that is not one f5low may write at all: only f5low.log (UTF-8 text) is allowed; the files named by --allow-name
     (the DLL, the player's libxess_fg.dll it was given, the test exe) are skipped; any other file fails. (This is the
     catch-all for a format the three checks above would not recognise.)
Exit code 0: clean, 1: a violation (each is printed), 2: usage.
"""
import argparse
import hashlib
import json
import os
import sys


def has_dxbc(data):
    at = data.find(b"DXBC")
    while at != -1:
        # DXBC + digest[16] + version u32 (1) + total size u32 + chunk count u32
        if at + 32 <= len(data) and data[at + 20:at + 24] == b"\x01\x00\x00\x00":
            total = int.from_bytes(data[at + 24:at + 28], "little")
            if 32 <= total <= len(data) - at:
                return True
        at = data.find(b"DXBC", at + 1)
    return False


def recipe_hashes(paths):
    out = set()

    def walk(x):
        if isinstance(x, dict):
            for k, v in x.items():
                if k in ("module_sha256", "sha256") and isinstance(v, str) and len(v) == 64:
                    out.add(v.lower())
                walk(v)
        elif isinstance(x, list):
            for v in x:
                walk(v)

    for p in paths:
        with open(p, "rb") as f:
            walk(json.load(f))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--samples", required=True)
    ap.add_argument("--parts", action="append", default=[])
    ap.add_argument("--allow-name", action="append", default=[])
    ap.add_argument("dirs", nargs="+")
    a = ap.parse_args()

    hashes = recipe_hashes(a.parts)
    windows = []
    nwin = 0
    with open(a.samples, "r") as f:
        for line in f:
            t = line.split()
            if t and t[0] == "F" and len(t) >= 4:
                hashes.add(t[3].lower())
            elif t and t[0] == "W" and len(t) >= 4:
                windows.append((t[1], bytes.fromhex(t[3])))
                nwin += 1
    if not hashes or not windows:
        print("no_intel_on_disk: the samples are empty - the check would prove nothing")
        return 2

    allow = {n.lower() for n in a.allow_name}
    scanned = 0
    bad = 0
    for d in a.dirs:
        if not os.path.isdir(d):
            continue
        for dirpath, _dirs, names in os.walk(d):
            for n in names:
                path = os.path.join(dirpath, n)
                if n.lower() in allow:
                    continue
                scanned += 1
                try:
                    with open(path, "rb") as f:
                        data = f.read()
                except OSError as e:
                    print("UNREADABLE %s: %s" % (path, e))
                    bad += 1
                    continue
                why = []
                if has_dxbc(data):
                    why.append("holds a DXBC container")
                sha = hashlib.sha256(data).hexdigest()
                if sha in hashes:
                    why.append("its SHA-256 is an Intel module / weight / constant of the recipe or the pack")
                for fname, w in windows:
                    if w in data:
                        why.append("holds a 64-byte window of the pack's %s" % fname)
                        break
                if n.lower() != "f5low.log":
                    why.append("is a file f5low should not write (only f5low.log is allowed)")
                else:
                    try:
                        data.decode("utf-8")
                    except UnicodeDecodeError:
                        why.append("f5low.log is not UTF-8 text")
                if why:
                    bad += 1
                    print("VIOLATION %s (%d bytes): %s" % (path, len(data), "; ".join(why)))
    print("no_intel_on_disk: scanned %d files in %s against %d hashes and %d windows: %s"
          % (scanned, ", ".join(a.dirs), len(hashes), nwin, "FAIL (%d)" % bad if bad else "clean"))
    return 1 if bad else 0


if __name__ == "__main__":
    sys.exit(main())
