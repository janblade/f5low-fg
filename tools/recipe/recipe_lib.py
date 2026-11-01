"""Shared pieces of the recipe tools (classify.py, build_pack.py, compare_pack.py).

A graph pack is a folder with manifest.txt, kernel files and data. Its recipe (recipe/<name>/) is the manifest with
every generated file name written as @<file name> (manifest.tmpl) and parts.json, which says how each file is made.
Intel modules are named by the SHA-256 of the decoded module (the DXBC container restore_dxbc.py makes), never by
position in a DLL.
"""
import hashlib
import json
import os
import re
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.dirname(os.path.dirname(HERE))
KERNELS = os.path.join(ROOT, "tools", "kernels")
PY = sys.executable

# manifest lines that name a generated file: op -> index of the file words
FILE_WORDS = {"kernel": (2,), "fallback": (2,), "variant": (2,), "native": (1, 2)}


def sha256(data):
    return hashlib.sha256(data).hexdigest()


def sha256_file(path):
    with open(path, "rb") as f:
        return sha256(f.read())


def run(cmd, what):
    r = subprocess.run(cmd, capture_output=True, text=True)
    if r.returncode != 0:
        sys.exit(f"{what} failed ({os.path.basename(cmd[0])}): {(r.stdout + r.stderr)[-400:]}")
    return r


def find_dxc(given=None):
    cands = [given, os.environ.get("XEFG_DXC"),
             os.path.join(os.environ.get("LOCALAPPDATA", ""), "Microsoft", "WinGet", "Packages",
                          "Microsoft.DirectX.ShaderCompiler_Microsoft.Winget.Source_8wekyb3d8bbwe", "bin", "x64",
                          "dxc.exe")]
    for c in cands:
        if c and os.path.isfile(c):
            return c
    sys.exit("dxc.exe not found: pass --dxc or set XEFG_DXC")


def lxw_tool(lxw, rel):
    p = os.path.join(lxw, *rel.split("/"))
    if not os.path.isfile(p):
        sys.exit(f"{p}: not found (--lxw must be a libxess-weights checkout)")
    return p


def extract_modules(dll, lxw, work, reuse=None):
    """{sha256 of the decoded module: path of its DXBC container} for every DXIL module in `dll`. `reuse`: a folder
    of containers from an earlier extraction of the same DLL (skips the slow decode)."""
    dxbc = reuse
    if not dxbc:
        frames, dxbc = os.path.join(work, "frames"), os.path.join(work, "dxbc")
        run([PY, lxw_tool(lxw, "tools/extract_kernels.py"), dll, frames], "module extraction")
        run([PY, lxw_tool(lxw, "tools/restore_dxbc.py"), frames, dxbc], "module restore")
    out = {}
    for n in sorted(os.listdir(dxbc)):
        if n.endswith(".dxbc"):
            out.setdefault(sha256_file(os.path.join(dxbc, n)), os.path.join(dxbc, n))
    return out


def dxasm_of(dxc, container, dst):
    """The disassembly dxil2ptx reads: `dxc -dumpbin` of the container."""
    r = subprocess.run([dxc, "-dumpbin", container], capture_output=True)
    if r.returncode != 0:
        sys.exit(f"dxc -dumpbin {container} failed: {r.stderr[-300:]}")
    with open(dst, "wb") as f:
        f.write(r.stdout)


def translate(lxw, dxasm, ptx, fmts):
    run([PY, lxw_tool(lxw, "tools/cuda/dxil2ptx.py"), dxasm, ptx] + [f"--fmt={f}" for f in fmts], "translation")


def clamp(lxw, src, dst, bits):
    run([PY, lxw_tool(lxw, "tools/cuda/clamp_variant.py"), src, dst, str(bits)], "clamp variant")


def recon_edit(lxw, src, dst):
    run([PY, lxw_tool(lxw, "tools/cuda/i0_recon.py"), "edit", src, dst], "reconstruction edit")


def generate(generator, args, ptx):
    run([PY, os.path.join(KERNELS, generator), ptx] + list(args), generator)


def compile_template(dxc, template, defines, dxasm):
    """Our HLSL template at `defines` through dxc; writes its disassembly to `dxasm`."""
    cmd = [dxc, "-T", "cs_6_4", "-E", "CSMain"] + [f"-D{d}" for d in defines] + \
          [os.path.join(KERNELS, "hlsl", f"{template}.hlsl"), "-Fo", dxasm[:-6] + ".dxbc", "-Fc", dxasm]
    run(cmd, f"template {template}")


def read_manifest(path):
    """Manifest lines (no line ends); a pack with CRLF is refused: every writer here uses LF."""
    raw = open(path, "rb").read()
    if b"\r" in raw:
        sys.exit(f"{path}: CR in a manifest")
    return raw.decode("ascii").split("\n")


def manifest_files(lines):
    """[(line number, op, word index, file name)] for every generated file a manifest names."""
    out = []
    for no, l in enumerate(lines):
        w = l.split()
        if w and not l.startswith(" ") and w[0] in FILE_WORDS:
            out += [(no, w[0], i, w[i]) for i in FILE_WORDS[w[0]]]
    return out


def load_json(path):
    with open(path, encoding="utf-8") as f:
        return json.load(f)


def dump_json(obj, path):
    """Indented JSON with the short lists (arguments, formats) kept on one line."""
    text = json.dumps(obj, indent=1)
    text = re.sub(r'\[\s+((?:"[^"\n]*"|-?\d+)(?:,\s+(?:"[^"\n]*"|-?\d+))*)\s+\]',
                  lambda m: "[" + re.sub(r",\s+", ", ", m.group(1)) + "]", text)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text + "\n")
