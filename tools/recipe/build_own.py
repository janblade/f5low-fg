"""Build-time half of the pack-less runtime (build.ps1): everything of the own generator's pack that is OURS.

  python tools/recipe/build_own.py --out <dir> --xlate <xlate_tool.exe> [--dxc dxc.exe] [--recipe recipe/i0]

Writes into <dir>:
  own_bundle.bin   plain-text container that build.ps1 embeds as resource 102 of f5low.dll (layout below)
  own_files.txt    "<sha256>  resource 102: <name>" for every file in the bundle, for build\\own_blobs.txt
                   (tools/audit.py --dll finds the bundle inside the DLL and accepts only files listed there)
What goes in (names relative to the recipe, '/' separators, prefix "i0/"):
  i0/manifest.tmpl, i0/parts.json, i0/hdr/...      the recipe, as it is in the repo
  i0/<file>.ptx, i0/hdr/<file>.ptx                 every `own_gen` (tools/kernels generators) and `own_tpl` (our HLSL
                                                   template through dxc, then src/translate.cpp via xlate_tool) kernel
  i0/<data path>, i0/hdr/<data path>               `own_synth` data (the noise texture, drawn from the recorded seed)
Intel parts (`intel_*` kinds) are not touched here: they are made on the player's PC from the player's libxess_fg.dll.

Container: b"F5OWN1\\n", then per file a line "<size> <name>\\n", <size> bytes, "\\n"; ends with a line "end\\n".
All text except the noise (1 KB), so a compressed or encrypted payload cannot hide in it (audit entropy check).
"""
import argparse
import os
import re
import shutil
import sys
import tempfile

import numpy as np

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recipe_lib as R  # noqa: E402


def noise(seed, w, h):
    """The 32 x 32 R8 noise of tools/chain_graph.py's synthetic scene: drawn after the two frames' float noise."""
    rng = np.random.default_rng(seed)
    left = 2 * w * h * 4
    while left:
        n = min(left, 1 << 22)
        rng.normal(0, 0.01, n)
        left -= n
    return rng.integers(0, 256, (32, 32), dtype=np.uint8).tobytes()


def build_pack(a, rdir, prefix, work, files):
    parts = R.load_json(os.path.join(rdir, "parts.json"))
    tmpl_path = os.path.join(rdir, "manifest.tmpl")
    with open(tmpl_path, "rb") as f:
        tmpl = f.read()
    with open(os.path.join(rdir, "parts.json"), "rb") as f:
        files[prefix + "manifest.tmpl"] = tmpl
        files[prefix + "parts.json"] = f.read()
    size = next(l.split() for l in tmpl.decode("ascii").split("\n") if l.startswith("size "))
    for name, part in parts["files"].items():
        kind = part["kind"]
        if kind not in ("own_gen", "own_tpl"):
            continue
        dst = os.path.join(work, prefix.replace("/", "_") + name)
        if kind == "own_gen":
            R.generate(part["generator"], part["args"], dst)
        else:
            dxasm_name = os.path.splitext(name)[0] + ".dxasm"
            asm = os.path.join(work, prefix.replace("/", "_") + dxasm_name)
            R.compile_template(a.dxc, part["template"], part["defines"], asm)
            R.run([a.xlate, asm, dst, dxasm_name] + list(part["fmts"]), f"translation of {name}")
        with open(dst, "rb") as f:
            files[prefix + name] = f.read()
    for rel, d in parts["data"].items():
        if d["kind"] != "own_synth":
            continue
        blob = noise(d["seed"], int(size[1]), int(size[2]))
        if R.sha256(blob) != d["sha256"]:
            sys.exit(f"{rel}: the noise does not match the recipe's SHA-256")
        files[prefix + rel] = blob
    for sub in sorted(os.listdir(rdir)):
        if os.path.isfile(os.path.join(rdir, sub, "parts.json")):
            build_pack(a, os.path.join(rdir, sub), prefix + sub + "/", work, files)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", required=True)
    ap.add_argument("--xlate", required=True, help="build\\xlate_tool.exe")
    ap.add_argument("--dxc")
    ap.add_argument("--recipe", default=os.path.join(R.ROOT, "recipe", "i0"))
    a = ap.parse_args()
    a.dxc = R.find_dxc(a.dxc)
    os.makedirs(a.out, exist_ok=True)
    work = tempfile.mkdtemp(prefix="f5low_own_")
    files = {}
    try:
        build_pack(a, os.path.abspath(a.recipe), "i0/", work, files)
    finally:
        shutil.rmtree(work, ignore_errors=True)
    out = bytearray(b"F5OWN1\n")
    for name in sorted(files):
        out += f"{len(files[name])} {name}\n".encode("ascii") + files[name] + b"\n"
    out += b"end\n"
    with open(os.path.join(a.out, "own_bundle.bin"), "wb") as f:
        f.write(out)
    with open(os.path.join(a.out, "own_files.txt"), "w", encoding="ascii", newline="\n") as f:
        for n in sorted(files):
            f.write(f"{R.sha256(files[n])}  resource 102: {n}\n")
    print(f"{len(files)} files, {len(out) / 1e6:.2f} MB")


if __name__ == "__main__":
    main()
