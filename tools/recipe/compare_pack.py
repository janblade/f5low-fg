"""Compares a built graph pack with a reference pack (a nested pack too).

  python tools/recipe/compare_pack.py <built> <reference> [--ignore-ptx-header] [--legacy-names names.json] [--packless]

Every file the reference's manifest names (kernels, natives, data) must exist in the built pack and be byte-identical,
and the manifests must be the same text. Not compared: the per-frame inputs a game fills (tools/pack_embed.py
capture_files), *.bak* manifest backups, and kernel files no manifest line names (orphans a tool left behind; counted
and listed). --ignore-ptx-header skips the first line of each PTX, which names the module it was translated from
(a build with neutral names writes its own); everything else must still match. --legacy-names (the local JSON
classify.py writes) is for a pack built with neutral names: its file names and slot comments are mapped back to the
reference's before comparing. Exit 1 on any difference.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recipe_lib as R  # noqa: E402

sys.path.insert(0, os.path.join(R.ROOT, "tools"))
from pack_embed import capture_files  # noqa: E402


def referenced(folder):
    """Files (relative to `folder`) the manifest of `folder` names."""
    lines = R.read_manifest(os.path.join(folder, "manifest.txt"))
    out = {n for _, _, _, n in R.manifest_files(lines)}
    for l in lines:
        for m in re.finditer(r"(?:\binit |^\s+cb \d+ )(\S+)", l):
            out.add(m.group(1))
    return {os.path.normpath(n) for n in out}


def back_to_legacy(text, leg):
    """A neutral-name manifest with the reference's file names and slot comments."""
    lines = text.split("\n")
    for no, op, wi, n in R.manifest_files(lines):
        w = lines[no].split()
        w[wi] = leg["files"][n]["file"]
        lines[no] = " ".join(w)
    return "\n".join(re.sub(r"^(dispatch .*?#\s*)(\d+) (once|per-t)$",
                            lambda m: f"{m.group(1)}{m.group(2)} {leg['slots'][m.group(2)]} {m.group(3)}", l)
                     for l in lines)


def packs(root):
    return [os.path.relpath(d, root) for d, _, names in os.walk(root) if "manifest.txt" in names]


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("built")
    ap.add_argument("ref")
    ap.add_argument("--ignore-ptx-header", action="store_true")
    ap.add_argument("--legacy-names")
    ap.add_argument("--packless", action="store_true",
                    help="built is a pack f5low.dll made on a PC (pack_build.cpp): the PTX files of 'native' lines are left out "
                         "(only the D3D12 path runs it) and the d3d12_only marker is extra; every other file must match")
    a = ap.parse_args()
    legacy = R.load_json(a.legacy_names)["packs"] if a.legacy_names else None
    skip = {os.path.normpath(p).lower() for p in capture_files(a.ref)}
    bad, same, skipped, orphans = [], 0, 0, []
    for sub in sorted(packs(a.ref)):
        rd, bd = os.path.normpath(os.path.join(a.ref, sub)), os.path.normpath(os.path.join(a.built, sub))
        if not os.path.isfile(os.path.join(bd, "manifest.txt")):
            bad.append(f"{sub}: no pack built")
            continue
        names = referenced(rd)
        if a.packless:
            # the first word after `native` is the PTX twin
            twins = set()
            for l in R.read_manifest(os.path.join(rd, "manifest.txt")):
                w = l.split("#")[0].split()
                if len(w) >= 3 and w[0] == "native":
                    twins.add(os.path.normpath(w[1]))
            names -= twins
        inv = {}
        if legacy:
            leg = legacy["." if sub == "." else sub]
            inv = {v["file"]: n for n, v in leg["files"].items()}
        for n in sorted(os.listdir(rd)):
            full = os.path.join(rd, n)
            if os.path.isfile(full) and n != "manifest.txt" and ".bak" not in n and n not in names:
                orphans.append(os.path.normpath(os.path.join(sub, n)))
        todo = ["manifest.txt"] + sorted(names)
        for rel in todo:
            ref_path = os.path.normpath(os.path.join(rd, rel))
            key = os.path.normpath(os.path.relpath(ref_path, a.ref))
            if key.lower() in skip:
                skipped += 1
                continue
            built_path = os.path.normpath(os.path.join(bd, inv.get(rel, rel)))
            if not os.path.isfile(built_path):
                bad.append(f"{key}: missing")
                continue
            x, y = open(ref_path, "rb").read(), open(built_path, "rb").read()
            if legacy and rel == "manifest.txt":
                y = back_to_legacy(y.decode("ascii"), leg).encode("ascii")
            if a.ignore_ptx_header and rel.endswith(".ptx"):
                x, y = x.split(b"\n", 1)[1], y.split(b"\n", 1)[1]
            if x == y:
                same += 1
            else:
                bad.append(f"{key}: differs ({len(x)} vs {len(y)} bytes)")
    for b in bad:
        print("DIFF", b)
    print(f"{same} files identical, {len(bad)} differences, {skipped} per-frame inputs not compared, "
          f"{len(orphans)} unreferenced reference files ignored" + (f" ({', '.join(orphans)})" if orphans else ""))
    sys.exit(1 if bad else 0)


if __name__ == "__main__":
    main()
