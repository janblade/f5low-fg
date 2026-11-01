"""Builds a graph pack from its recipe and the Intel parts on this PC (offline; B2 / B3 move this into the DLL).

  python tools/recipe/build_pack.py --dll <libxess_fg.dll> --weights <dump dir> --cap <XeFGPsoCap.log>
         --recipe recipe/i0 --out <dir> --lxw <libxess-weights checkout> [--dxc dxc.exe] [--modules <dxbc dir>]
         [--legacy-names names.json] [--work <dir>]

What each part needs (parts.json kinds, see tools/recipe/classify.py):
  own_gen        tools/kernels/<generator> with the recorded arguments
  own_tpl        tools/kernels/hlsl/<template>.hlsl through dxc, then dxil2ptx
  intel_xlate    the module (found in --dll by SHA-256) through dxil2ptx [+ clamp_variant / reconstruction edit]
  intel_native   the module's container, copied
  intel_upload   upload <ordinal> of the weight dump (--weights), padded, SHA-256 checked
  intel_const    root constants of dispatch <n> of the PSO capture (--cap), SHA-256 checked
  own_synth      the noise texture, drawn from the recorded seed
  capture_input  not written: a game fills these every frame (the DLL reads a missing one as zero)
A missing module or a hash that does not match stops the build and names the part. dxil2ptx.py, clamp_variant.py,
i0_recon.py, extract_kernels.py and restore_dxbc.py are taken from --lxw (B2 decides where they live).
--legacy-names (a local JSON from classify.py) writes the files and slot comments under the names an older pack had.
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

D_LINE = re.compile(r"^\s*[\d.]+ D (\d+) t\d+ list=\S+ (\S+) g=(\d+),(\d+),(\d+)(?: r(\d+)=(\S+))?")


def read_capture(path):
    """Root constants of each dispatch of the PSO capture, in log order: [[dword, ...], ...]."""
    out = []
    for line in open(path, encoding="ascii", errors="replace"):
        m = D_LINE.match(line)
        if m:
            vals = m.group(7)
            out.append([int(x, 16) for x in vals.split(",")] if vals else [])
    return out


def noise(seed, w, h):
    """The 32 x 32 R8 noise of tools/chain_graph.py's synthetic scene: drawn after the two frames' float noise."""
    rng = np.random.default_rng(seed)
    left = 2 * w * h * 4
    while left:
        n = min(left, 1 << 22)
        rng.normal(0, 0.01, n)
        left -= n
    return rng.integers(0, 256, (32, 32), dtype=np.uint8).tobytes()


class Builder:
    def __init__(self, a):
        self.a = a
        self.lxw = os.path.abspath(a.lxw)
        self.dxc = R.find_dxc(a.dxc)
        self.legacy = R.load_json(a.legacy_names)["packs"] if a.legacy_names else None
        self.work = a.work or tempfile.mkdtemp(prefix="f5low_build_")
        os.makedirs(self.work, exist_ok=True)
        self.made = {}                  # cache key -> built file (identical parts of the SDR and HDR packs)
        self.serial = 0

    def prepare(self):
        a = self.a
        self.mods = R.extract_modules(a.dll, self.lxw, self.work, a.modules)
        self.uploads = {}
        for n in os.listdir(a.weights):
            m = re.match(r"u(\d{4})_.*\.bin$", n)
            if m:
                self.uploads[int(m.group(1))] = os.path.join(a.weights, n)
        self.rc = read_capture(a.cap)

    def module(self, name, part):
        p = self.mods.get(part["module_sha256"])
        if not p:
            sys.exit(f"{name}: module {part['module_sha256']} not found in {self.a.dll}")
        return p

    def scratch(self):
        self.serial += 1
        d = os.path.join(self.work, f"p{self.serial:03d}")
        os.makedirs(d, exist_ok=True)
        return d

    def build_file(self, key, name, part, dst, dxasm_name):
        kind = part["kind"]
        ck = (kind, part.get("module_sha256"), part.get("transform"), tuple(part.get("fmts", ())), part.get("template"),
              tuple(part.get("defines", ())), part.get("generator"), tuple(part.get("args", ())), dxasm_name)
        if ck in self.made:
            shutil.copyfile(self.made[ck], dst)
            return
        if kind == "own_gen":
            R.generate(part["generator"], part["args"], dst)
        elif kind == "intel_native":
            shutil.copyfile(self.module(name, part), dst)
        elif kind == "own_tpl":
            d = self.scratch()
            asm = os.path.join(d, dxasm_name)
            R.compile_template(self.dxc, part["template"], part["defines"], asm)
            R.translate(self.lxw, asm, dst, part["fmts"])
        elif kind == "intel_xlate":
            d = self.scratch()
            asm = os.path.join(d, dxasm_name)
            tr = part["transform"]
            if tr == "recon":
                raw = os.path.join(d, "module.dxasm")
                R.dxasm_of(self.dxc, self.module(name, part), raw)
                R.recon_edit(self.lxw, raw, asm)
            else:
                R.dxasm_of(self.dxc, self.module(name, part), asm)
            if tr.startswith("clamp:"):
                tmp = os.path.join(d, "module.ptx")
                R.translate(self.lxw, asm, tmp, part["fmts"])
                R.clamp(self.lxw, tmp, dst, int(tr.split(":")[1]))
            else:
                R.translate(self.lxw, asm, dst, part["fmts"])
        else:
            sys.exit(f"{name}: kind {kind} cannot be built")
        self.made[ck] = dst

    def build_pack(self, rdir, out, key, main_data):
        parts = R.load_json(os.path.join(rdir, "parts.json"))
        tmpl = open(os.path.join(rdir, "manifest.tmpl"), encoding="ascii").read()
        os.makedirs(out, exist_ok=True)
        leg = (self.legacy or {}).get(key)
        names = {}
        for n, part in parts["files"].items():
            old = leg["files"][n] if leg else {}
            names[n] = old.get("file", n)
            dxn = old.get("dxasm") or (os.path.splitext(n)[0] + ".dxasm")
            self.build_file(key, n, part, os.path.join(out, names[n]), dxn)
        text = re.sub(r"@([A-Za-z0-9_]+\.(?:ptx|dxbc))", lambda m: names[m.group(1)], tmpl)
        if "@" in text:
            sys.exit(f"{rdir}: a placeholder is left in the manifest")
        if leg:
            text = "\n".join(re.sub(r"^(dispatch .*?#\s*)(\d+) (once|per-t)$",
                                    lambda m: f"{m.group(1)}{m.group(2)} {leg['slots'][m.group(2)]} {m.group(3)}", l)
                             for l in text.split("\n"))
        with open(os.path.join(out, "manifest.txt"), "w", encoding="ascii", newline="\n") as f:
            f.write(text)
        size = next(l.split() for l in text.split("\n") if l.startswith("size "))
        for rel, d in parts["data"].items():
            kind = d["kind"]
            if kind == "capture_input":
                continue
            dst = os.path.join(out, rel)
            os.makedirs(os.path.dirname(dst), exist_ok=True)
            if kind == "intel_upload":
                if d["ordinal"] not in self.uploads:
                    sys.exit(f"{rel}: no upload {d['ordinal']} in {self.a.weights}")
                up = open(self.uploads[d["ordinal"]], "rb").read()
                if len(up) != d["upload_bytes"]:
                    sys.exit(f"{rel}: upload {d['ordinal']} has {len(up)} bytes, the recipe wants {d['upload_bytes']}")
                blob = up + bytes(d["bytes"] - len(up))
            elif kind == "intel_const":
                i = d["capture_index"]
                if i >= len(self.rc):
                    sys.exit(f"{rel}: the capture has {len(self.rc)} dispatches, the recipe wants #{i}")
                blob = np.array((self.rc[i] + [0] * d["dwords"])[:d["dwords"]], np.uint32).tobytes()
            elif kind == "own_synth":
                blob = noise(d["seed"], int(size[1]), int(size[2]))
            else:
                sys.exit(f"{rel}: kind {kind} cannot be built")
            if R.sha256(blob) != d["sha256"]:
                sys.exit(f"{rel}: {kind} content does not match the recipe's SHA-256 (wrong capture or dump?)")
            with open(dst, "wb") as f:
                f.write(blob)
        return len(parts["files"]), len(parts["data"])

    def run(self):
        self.prepare()
        rdir, out = os.path.abspath(self.a.recipe), os.path.abspath(self.a.out)
        n, nd = self.build_pack(rdir, out, ".", None)
        print(f"{out}: {n} kernel files, {nd} data entries")
        for sub in sorted(os.listdir(rdir)):
            if os.path.isfile(os.path.join(rdir, sub, "parts.json")):
                n, nd = self.build_pack(os.path.join(rdir, sub), os.path.join(out, sub), sub, out)
                print(f"{os.path.join(out, sub)}: {n} kernel files, {nd} data entries")
        if not self.a.work:
            shutil.rmtree(self.work, ignore_errors=True)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--dll", required=True, help="libxess_fg.dll (read only)")
    ap.add_argument("--weights", required=True, help="weight dump folder (u0001_*.bin ...)")
    ap.add_argument("--cap", required=True, help="XeFGPsoCap log with the model's dispatches (root constants)")
    ap.add_argument("--recipe", required=True)
    ap.add_argument("--out", required=True)
    ap.add_argument("--lxw", required=True, help="libxess-weights checkout (dxil2ptx, clamp_variant, i0_recon, extract)")
    ap.add_argument("--dxc")
    ap.add_argument("--modules", help="DXBC containers of an earlier extraction of --dll (skips the decode)")
    ap.add_argument("--legacy-names")
    ap.add_argument("--work", help="keep the scratch files here")
    Builder(ap.parse_args()).run()


if __name__ == "__main__":
    main()
