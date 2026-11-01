"""Classifies a graph pack's files and manifest references, and writes its recipe.

  python tools/recipe/classify.py <pack dir> --lxw <libxess-weights> [--weights <dump dir>] [--out recipe/i0]
         [--legacy-names names.json] [--dxc dxc.exe]

This is the one tool that reads a finished pack (Intel-derived, kept outside this repo) and the old build tools in
a libxess-weights checkout: it works out, per file, which of our generators / templates / which Intel module plus
transform made it, and writes only the OURS part into --out (manifest.tmpl, parts.json; nested packs under
<out>/<name>). Intel modules are named by SHA-256 of the decoded module; the position they have in one DLL, the
names the old pack gave its files and MFG Unlocker's shader names are not written. --legacy-names writes those to a
local JSON (never committed) so build_pack.py can rebuild the pack under its old names for a byte compare.

Kinds: own_gen (our generator), own_tpl (our HLSL template), intel_xlate (Intel module through dxil2ptx, optional
clamp / reconstruction edit), intel_native (Intel container copied), intel_upload (weights from the dump),
intel_const (root constants from the PSO capture), own_synth (noise texture we draw), capture_input (per-frame
inputs a game fills), orphan (a kernel file no manifest line names), other.
"""
import argparse
import collections
import os
import re
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import recipe_lib as R  # noqa: E402

TEXFMT_NAMES = None     # set from the libxess-weights checkout: DXGI format -> dxil2ptx --fmt name


def load_old_tools(lxw):
    """OURS, label, TEXFMT and declarations from the libxess-weights checkout (a dev checkout, never shipped)."""
    global TEXFMT_NAMES
    tools = os.path.join(lxw, "tools")
    sys.path.insert(0, tools)
    cwd = os.getcwd()
    os.chdir(lxw)
    try:
        import build_i0_pack as B
        import chain_graph as C
    finally:
        os.chdir(cwd)
    TEXFMT_NAMES = {k: v[1] for k, v in C.TEXFMT.items()}
    return B, C


def parse_manifest(lines):
    """resources {id: (kind, dims, init)}, roles, dispatches [{kn, comment, binds, cbs}], per-line file words."""
    res, roles, disp = {}, {}, []
    for l in lines:
        w = l.split()
        if not w:
            continue
        if w[0] == "resource":
            init = w[w.index("init") + 1] if "init" in w else None
            res[w[1]] = (w[2], w[3:w.index("init")] if init else w[3:], init)
        elif w[0] == "role":
            roles[w[1]] = w[2]
        elif w[0] == "dispatch":
            m = re.search(r"#\s*(\d+)\s+(\S+)\s+(once|per-t)\s*$", l)
            disp.append({"kn": int(w[1]), "idx": int(m.group(1)) if m else None, "tok": m.group(2) if m else None,
                         "tag": m.group(3) if m else None, "binds": [], "cbs": []})
        elif l.startswith("  bind "):
            disp[-1]["binds"].append((int(w[1]), w[2], int(w[3])))
        elif l.startswith("  cb "):
            disp[-1]["cbs"].append((int(w[1]), w[2]))
    return res, roles, disp


def first_line(path):
    with open(path, "rb") as f:
        return f.readline().decode("ascii", "replace").rstrip("\r\n")


class Pack:
    def __init__(self, folder, name, lines):
        self.dir, self.name, self.lines = folder, name, lines
        self.res, self.roles, self.disp = parse_manifest(lines)


def short(gen):
    return re.sub(r"^gen_|_ptx\.py$", "", gen)


def classify(a):
    lxw = os.path.abspath(a.lxw)
    B, C = load_old_tools(lxw)
    dxc = R.find_dxc(a.dxc)
    top = os.path.abspath(a.pack)
    packs = []
    for root, _, names in os.walk(top):
        if "manifest.txt" in names:
            packs.append(Pack(root, os.path.relpath(root, top).replace("\\", "/").replace(".", "") or "", R.read_manifest(os.path.join(root, "manifest.txt"))))
    packs.sort(key=lambda p: p.name)
    # Intel module containers of the checkout (offset names there; only their hashes are used here)
    mods = {}               # offset name (8 hex) -> sha256
    mod_dir = os.path.join(lxw, "dxbc")
    for n in os.listdir(mod_dir):
        m = re.match(r"dxil_([0-9A-F]{8})\.dxbc$", n)
        if m:
            mods[m.group(1)] = R.sha256_file(os.path.join(mod_dir, n))
    work = tempfile.mkdtemp(prefix="f5low_classify_", dir=a.scratch)
    dumpf = {}
    if a.weights:
        for n in sorted(os.listdir(a.weights)):
            m = re.match(r"u(\d{4})_.*\.bin$", n)
            if m:
                dumpf[int(m.group(1))] = os.path.join(a.weights, n)

    report = collections.OrderedDict()
    recipes, legacy = {}, {"packs": {}}
    for pk in packs:
        key = pk.name or "."
        files = {}                  # neutral file name -> part
        legacy_files = {}           # neutral file name -> {"file": old name, "dxasm": old dxasm name}
        counts = collections.Counter()
        line_counts = collections.Counter()
        names_in_use = set()

        def neutral(base):
            n = base
            assert n not in names_in_use, f"duplicate name {n}"
            names_in_use.add(n)
            return n

        # slot formats per kernel number, once the base kernels are known
        kern = {}                   # kn -> part (base kernel)
        file_part = {}              # old file name -> neutral name
        dxasm_cache = {}

        def dxasm_for(part, tag):
            """Local dxasm of an xlate / tpl part (for its register declarations)."""
            k = (part["kind"], part.get("module_sha256", ""), part.get("template", ""), tuple(part.get("defines", ())),
                 part.get("transform", ""))
            if k in dxasm_cache:
                return dxasm_cache[k]
            dst = os.path.join(work, f"{len(dxasm_cache):03d}_{tag}.dxasm")
            if part["kind"] == "own_tpl":
                R.compile_template(dxc, part["template"], part["defines"], dst)
            else:
                base = [p for p in mod_paths if mod_paths[p] == part["module_sha256"]][0]
                if part["transform"] == "recon":
                    tmp = dst + ".in"
                    R.dxasm_of(dxc, base, tmp)
                    R.recon_edit(lxw, tmp, dst)
                else:
                    R.dxasm_of(dxc, base, dst)
            dxasm_cache[k] = dst
            return dst

        mod_paths = {os.path.join(mod_dir, f"dxil_{o}.dxbc"): s for o, s in mods.items()}

        kernels = []                # (op, kn, old file, rest words)
        for l in pk.lines:
            w = l.split()
            if w and not l.startswith(" ") and w[0] in ("kernel", "fallback", "variant"):
                kernels.append((w[0], int(w[1]), w[2]))
        natives = [l.split() for l in pk.lines if l.startswith("native ")]
        native_dxbc = {w[2] for w in natives}

        # pass 1: kind and recipe of every kernel / variant / fallback file
        for op, kn, old in kernels:
            if old in file_part:
                continue
            path = os.path.join(pk.dir, old)
            fl = first_line(path)
            m_gen = re.match(r"// GENERATED by (gen_\w+\.py) (.*?) - ", fl)
            m_x = re.match(r"// GENERATED by dxil2ptx\.py from (\S+)\.dxasm - ", fl)
            m_c = re.match(r"// clamp_variant\.py \d+ -> (\d+) bits, from dxil2ptx\.py from (\S+)\.dxasm - ", fl)
            part = {}
            if m_gen:
                args = m_gen.group(2).split()
                part = {"kind": "own_gen", "generator": m_gen.group(1), "args": args}
                vals = [x for i, x in enumerate(args) if not x.startswith("--") and not (i and args[i - 1] == "--sm")]
                base = f"k{kn:02d}_{short(m_gen.group(1))}_" + "_".join(vals)
                if "--sm" in args:
                    base += "_sm" + args[args.index("--sm") + 1]
                legacy_dx = None
            elif m_x or m_c:
                name = (m_c or m_x).group(2 if m_c else 1)
                mm = re.match(r"^(?:dxil_)?([0-9A-F]{8})(t?)$", name)
                if mm:
                    sha = mods[mm.group(1)]
                    recon = mm.group(2) == "t"
                    part = {"kind": "intel_xlate", "module_sha256": sha,
                            "transform": "recon" if recon else (f"clamp:{m_c.group(1)}" if m_c else "plain")}
                    base = f"{'v' if op == 'variant' else 'k%02d' % kn}_{'r' if recon else 'x'}{sha[:8]}"
                    if m_c:
                        base += f"_c{m_c.group(1)}"
                    legacy_dx = name + ".dxasm"
                else:
                    # a template: head_2_18 / rs_0 / warp_1 (the old label)
                    ent = [(k, v) for k, v in B.OURS.items() if v[0] == "hlsl" and B.label(v) == name]
                    if not ent:
                        sys.exit(f"{old}: no template recipe for label {name}")
                    rec = ent[0][1]
                    part = {"kind": "own_tpl", "template": rec[1], "defines": list(rec[2]), "label": name}
                    base = f"k{kn:02d}_{name}"
                    legacy_dx = name + ".dxasm"
            else:
                part = {"kind": "other", "first_line": fl}
                base = f"k{kn:02d}_other"
                legacy_dx = None
            nn = neutral(base + ".ptx")
            files[nn] = part
            file_part[old] = nn
            legacy_files[nn] = {"file": old}
            if part["kind"] in ("intel_xlate", "own_tpl"):
                legacy_files[nn]["dxasm"] = legacy_dx
            if op == "kernel":
                kern[kn] = part
            counts[part["kind"]] += 1
            part.setdefault("_op", op)
        # fmts: per kernel number from the dispatches that use it, against the base kernel's declarations
        slot_fmts = {}
        for kn, part in kern.items():
            if part["kind"] not in ("intel_xlate", "own_tpl"):
                continue
            decl = C.declarations(None, dxasm_for(part, f"k{kn}"))
            fm = set()
            for d in pk.disp:
                if d["kn"] != kn:
                    continue
                for slot, rid, fmt in d["binds"]:
                    cls, reg = slot // 32, slot % 32
                    if (cls, reg) in decl and decl[(cls, reg)][0] not in (11, 12) and TEXFMT_NAMES.get(fmt):
                        fm.add(f"{'SU'[cls]}{reg}={TEXFMT_NAMES[fmt]}")
            slot_fmts[kn] = sorted(fm)
        for op, kn, old in kernels:
            part = files[file_part[old]]
            if part["kind"] in ("intel_xlate", "own_tpl"):
                part["fmts"] = slot_fmts.get(kn, [])
        # natives
        nat_part = {}
        for w in natives:
            ptx, dxb = w[1], w[2]
            if dxb in nat_part:
                continue
            sha = R.sha256_file(os.path.join(pk.dir, dxb))
            if sha not in mods.values():
                sys.exit(f"{dxb}: not one of the checkout's modules")
            nn = neutral(os.path.splitext(file_part[ptx])[0] + ".dxbc")
            files[nn] = {"kind": "intel_native", "module_sha256": sha, "_op": "native"}
            nat_part[dxb] = nn
            legacy_files[nn] = {"file": dxb}
            counts["intel_native"] += 1
        # orphans
        named = set(file_part) | native_dxbc
        orphans = sorted(n for n in os.listdir(pk.dir) if os.path.isfile(os.path.join(pk.dir, n))
                         and n != "manifest.txt" and ".bak" not in n and n not in named)
        counts["orphan"] += len(orphans)

        # data files
        data = {}
        order = []                          # resource ids in the order the graph first binds them
        for d in pk.disp:
            for _, rid, _ in d["binds"]:
                if rid not in order:
                    order.append(rid)
        used_ord = set()
        role_of = {v: k for k, v in pk.roles.items()}
        sdr_data = recipes.get(".", {}).get("data", {}) if pk.name else {}
        for rid in order + [r for r in pk.res if r not in order]:
            kind, dims, init = pk.res[rid]
            if not init:
                continue
            shared = init.startswith("../")
            relname = init[3:] if shared else init
            if relname in data or (shared and relname in sdr_data):
                continue
            p = os.path.normpath(os.path.join(pk.dir, init))
            role = role_of.get(rid)
            if role and role != "noise":
                data[relname] = {"kind": "capture_input", "role": role}
                counts["capture_input"] += 1
                continue
            if role == "noise":
                data[relname] = {"kind": "own_synth", "what": "noise", "seed": 0, "bytes": os.path.getsize(p),
                                 "sha256": R.sha256_file(p)}
                counts["own_synth"] += 1
                continue
            blob = open(p, "rb").read()
            hit = None
            for o in sorted(dumpf):
                if o in used_ord:
                    continue
                up = open(dumpf[o], "rb").read()
                if len(up) <= len(blob) and blob[:len(up)] == up and not any(blob[len(up):]):
                    hit = (o, len(up))
                    break
            if hit is None:
                data[relname] = {"kind": "other", "bytes": len(blob)}
                counts["other"] += 1
                continue
            used_ord.add(hit[0])
            data[relname] = {"kind": "intel_upload", "ordinal": hit[0], "upload_bytes": hit[1], "bytes": len(blob),
                             "sha256": R.sha256(blob)}
            counts["intel_upload"] += 1
        for i, d in enumerate(pk.disp):
            for _, f in d["cbs"]:
                shared = f.startswith("../")
                rel = f[3:] if shared else f
                if rel in data or (shared and rel in sdr_data) or not re.match(r"data/rc\d+\.bin$", rel):
                    continue
                p = os.path.normpath(os.path.join(pk.dir, f))
                blob = open(p, "rb").read()
                data[rel] = {"kind": "intel_const", "capture_index": d["idx"], "dwords": len(blob) // 4,
                             "sha256": R.sha256(blob)}
                counts["intel_const"] += 1
        # references from the manifest, by line, for the table
        for op in ("kernel", "fallback", "variant", "native"):
            line_counts[op] = sum(1 for l in pk.lines if l.split()[:1] == [op] and not l.startswith(" "))
        line_counts["resource"] = sum(1 for l in pk.lines if l.startswith("resource "))
        line_counts["resource with init"] = sum(1 for l in pk.lines if l.startswith("resource ") and " init " in l)

        # the template: generated file names as @name, the dispatch comments without the old slot names
        out = []
        by_line = {}
        for no, op, wi, old in R.manifest_files(pk.lines):
            by_line.setdefault(no, []).append((wi, old))
        for no, l in enumerate(pk.lines):
            if no in by_line:
                w = l.split()
                for wi, old in by_line[no]:
                    w[wi] = "@" + (nat_part[old] if old in nat_part else file_part[old])
                l = " ".join(w)
            m = re.match(r"^(dispatch .*?#\s*)(\d+) (\S+) (once|per-t)$", l)
            if m:
                l = f"{m.group(1)}{m.group(2)} {m.group(4)}"
            out.append(l)
        recipes[pk.name or "."] = {"tmpl": "\n".join(out), "files": files, "data": data}
        legacy["packs"][key] = {"files": legacy_files,
                                "slots": {str(d["idx"]): d["tok"] for d in pk.disp if d["tok"]}}
        report[key] = {"counts": counts, "lines": line_counts, "orphans": orphans, "files": files}
    return report, recipes, legacy


def write_recipe(recipes, out):
    for name, rc in recipes.items():
        d = out if name == "." else os.path.join(out, name)
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "manifest.tmpl"), "w", encoding="ascii", newline="\n") as f:
            f.write(rc["tmpl"])
        files = {k: {a: b for a, b in v.items() if not a.startswith("_")} for k, v in rc["files"].items()}
        R.dump_json({"version": 1, "files": files, "data": rc["data"]}, os.path.join(d, "parts.json"))


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("pack")
    ap.add_argument("--lxw", required=True)
    ap.add_argument("--weights")
    ap.add_argument("--out")
    ap.add_argument("--legacy-names")
    ap.add_argument("--dxc")
    ap.add_argument("--scratch", default=None, help="folder for temporary files (default: system temp)")
    a = ap.parse_args()
    report, recipes, legacy = classify(a)
    for key, r in report.items():
        print(f"== pack {key}")
        print("  manifest lines:", dict(r["lines"]))
        print("  files by kind :", dict(r["counts"]))
        if r["orphans"]:
            print(f"  orphans ({len(r['orphans'])}):", ", ".join(r["orphans"]))
    if a.out:
        write_recipe(recipes, a.out)
        print("recipe written to", a.out)
    if a.legacy_names:
        R.dump_json(legacy, a.legacy_names)
        print("legacy names written to", a.legacy_names)


if __name__ == "__main__":
    main()
