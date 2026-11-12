"""Turing (sm_75) occupancy table and the test file for XEFG_CUDA_TURING_OCC (tests only, never shipped).

  python tools/turing_occ.py --ptx <dir with the *_sm75.ptx> --ptxas <ptxas.exe> [--local-sm 120] [--grids1080 f --grids1440 f]
                             [--pad-out turing_pad.txt]

Per kernel: threads, registers and static shared memory as ptxas reports them for sm_75 (the real Turing compile) and for the
local GPU (the driver JITs the same PTX for it), blocks per multiprocessor on a Turing card and on the local one, and the shared
memory to add per launch (dynamic) so the local GPU fits no more blocks per multiprocessor than Turing does.
--grids*: the "grid ..." lines the DLL prints with XEFG_CUDA_DUMPGRIDS=1 (tests\repro_test.exe at 1080p / 1440p); adds the
blocks of the kernel's largest dispatch and its waves on a 2060 (30 multiprocessors), 2070 (36) and 2070 Super (40).
Turing: 1024 threads, 32 warps, 16 blocks, 65536 registers (allocated per warp in units of 256), 64 KB shared (units of 256 B).
Local sm_120: 1536 threads, 48 warps, 24 blocks, 65536 registers, 100 KB shared (units of 128 B) + 1 KB reserved per block.
"""
import argparse
import glob
import math
import os
import re
import subprocess

TUR = dict(warps=32, blocks=16, regs=65536, smem=65536, unit=256, reserve=0)
LOC = dict(warps=48, blocks=24, regs=65536, smem=102400, unit=128, reserve=1024)
SMS = (30, 36, 40)


def up(x, u):
    return -(-x // u) * u


def blocks_per_sm(g, threads, regs, smem):
    w = -(-threads // 32)
    byregs = g["regs"] // (w * up(regs * 32, 256))
    bysmem = g["smem"] // (up(smem, g["unit"]) + g["reserve"]) if smem or g["reserve"] else 99
    return min(g["blocks"], g["warps"] // w, byregs, bysmem), (g["warps"] // w, byregs, bysmem)


def ptxas(exe, ptx, sm, out):
    r = subprocess.run([exe, f"-arch=sm_{sm}", "-v", ptx, "-o", out], capture_output=True, text=True)
    m = re.search(r"Used (\d+) registers.*?(\d+) bytes smem", r.stderr + r.stdout)
    if not m:
        raise SystemExit(f"{ptx}: ptxas sm_{sm}: {r.stderr}")
    return int(m.group(1)), int(m.group(2))


def reqntid(path):
    m = re.search(r"\.reqntid (\d+), (\d+), (\d+)", open(path).read())
    return (int(m.group(1)) * int(m.group(2)) * int(m.group(3))) if m else 64


def grids(path):
    best = {}
    if path:
        for l in open(path):
            f = l.split()
            if f and f[0] == "grid":
                n = int(f[3]) * int(f[4]) * int(f[5])
                best[f[2]] = max(best.get(f[2], 0), n)
    return best


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--ptx", required=True)
    ap.add_argument("--ptxas", required=True)
    ap.add_argument("--local-sm", type=int, default=120)
    ap.add_argument("--grids1080")
    ap.add_argument("--grids1440")
    ap.add_argument("--pad-out")
    ap.add_argument("--tmp", default=os.environ.get("TEMP", "."))
    a = ap.parse_args()
    g80, g44 = grids(a.grids1080), grids(a.grids1440)
    pads = {}
    print(f"{'kernel':34}{'thr':>5}{'reg75':>6}{'smem75':>8}{'T':>3}{'occ%':>5}{'  limit(w/r/s)':>15}"
          f"{'reg120':>7}{'sm120':>7}{'L':>3}{'pad':>7}{'blk1080':>8}{'blk1440':>8}   waves 1080 on 30/36/40")
    for f in sorted(glob.glob(os.path.join(a.ptx, "*_sm75.ptx"))):
        name = os.path.basename(f)
        thr = reqntid(f)
        r75, s75 = ptxas(a.ptxas, f, 75, os.path.join(a.tmp, "o75.cubin"))
        rl, sl = ptxas(a.ptxas, f, a.local_sm, os.path.join(a.tmp, "ol.cubin"))
        t, lim = blocks_per_sm(TUR, thr, r75, s75)
        loc, _ = blocks_per_sm(LOC, thr, rl, sl)
        occ = 100 * t * (thr // 32) // 32
        pad = 0
        if loc > t:
            tot = (LOC["smem"] // t - LOC["reserve"]) // LOC["unit"] * LOC["unit"]
            tot = min(tot, 101376)
            pad = max(tot - sl, 0)
            pads[name] = pad
        b80, b44 = g80.get(name, 0), g44.get(name, 0)
        waves = "/".join(f"{b80 / (n * t):.2f}" for n in SMS) if b80 else "-"
        print(f"{name:34}{thr:>5}{r75:>6}{s75:>8}{t:>3}{occ:>5}{'  %d/%d/%d' % lim:>15}{rl:>7}{sl:>7}{loc:>3}{pad:>7}{b80:>8}{b44:>8}   {waves}")
    if a.pad_out:
        with open(a.pad_out, "w") as o:
            for k, v in sorted(pads.items()):
                o.write(f"{k} {v}\n")


main()
