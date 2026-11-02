"""Pacer A/B analysis (tests/pacer_ab.ps1 runs the harness, this reads what it left).

  python -I tests/pacer_stats.py run <rundir> --gen own|dlssg --mult N [--skip 60] [--meta key=value ...]
      one JSON line on stdout: gap and latency numbers of one run.  <rundir> holds
        f5low.log        the DLL's log (own generator: "pace:" lines from PaceLog=1; dlssg: "sink: pace t=" lines and the
                         sink's gaps / probe lines, with F5LOW_SINK_STATS=1 and XEFG_CUDA_PACELOG=1)
        repro.out.txt    repro_test's stdout (REPRO_PACELOG=1: "app: frame= t= ret=" per app frame)
        presentmon.csv   optional, PresentMon 2.x --v2_metrics of the repro_test process
  python -I tests/pacer_stats.py report <outdir>
      reads <outdir>/results.jsonl, writes results.csv and results.md (one table per generator and scenario).

Own generator, from the "pace:" records (t = the present's time, QPC ms; ready = the app frame's ready stamp):
  gaps      = time between consecutive presents (dropped ones are not presents), app frames after --skip, three last left out
  lat_last  = per app frame: time its LAST present (the app's own frame) went out - its ready stamp
  lat_first = per app frame: time its FIRST presented generated frame went out - its ready stamp
dlssg: the sink lists only present times. Gaps are exact; latency is approximate - presents are grouped in bursts of `mult` and
  matched to the app frames' Present calls (REPRO_PACELOG), the shift chosen so no latency is negative and the mean is smallest,
  and counted from the app's Present CALL, not the ready stamp (a few ms more than the own generator's numbers).
"""
import csv
import json
import math
import re
import sys
from pathlib import Path


def pct(v, p):
    if not v:
        return 0.0
    s = sorted(v)
    return s[min(len(s) - 1, int(p * len(s)))]


def mean(v):
    return sum(v) / len(v) if v else 0.0


def sd(v):
    m = mean(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / len(v)) if v else 0.0


def gap_stats(gaps):
    m = mean(gaps)
    return {"gap_n": len(gaps), "gap_mean": m, "gap_sd": sd(gaps), "gap_p99": pct(gaps, 0.99), "gap_max": max(gaps) if gaps else 0.0,
            "hitch2x": sum(1 for g in gaps if g > 2 * m) if m else 0}


PACE = re.compile(r"pace: t=([\d.]+) frame=(\d+) k=(\d+)/(\d+) ready=([\d.]+) target=([\d.]+) flags=(\S*)")
SINK_PACE = re.compile(r"sink: pace t=([\d.]+)")
APP = re.compile(r"^app: frame=(\d+) t=([\d.]+) ret=([\d.]+)", re.M)
SINK_GAPS = re.compile(r"sink: gaps between the real chain's Present calls: (\d+) gaps\s+mean ([\d.]+)\s+ms\s+sd ([\d.]+)\s+p01 ([\d.]+)\s+p50 ([\d.]+)\s+p99 ([\d.]+)\s+max ([\d.]+)")
SINK_PROBE = re.compile(r"sink probe: .*")


def own_metrics(text, skip):
    recs = [(float(m[1]), int(m[2]), int(m[3]), int(m[4]), float(m[5]), float(m[6]), m[7]) for m in PACE.finditer(text)]
    if not recs:
        return {"notes": "no pace: lines (PaceLog=1 and XEFG_CUDA_LOG=1; did the own generator run?)"}
    frames = sorted({r[1] for r in recs})
    keep = set(frames[skip:-3]) if len(frames) > skip + 3 else set(frames)
    shown = sorted((r for r in recs if "drop" not in r[6] and r[1] in keep), key=lambda r: r[0])
    gaps = [b[0] - a[0] for a, b in zip(shown, shown[1:])]
    out = gap_stats(gaps)
    by = {}
    for r in recs:
        by.setdefault(r[1], []).append(r)
    last, first = [], []
    for f, rs in by.items():
        if f not in keep:
            continue
        ok = [r for r in rs if "drop" not in r[6]]
        total = rs[0][3]
        real = [r for r in ok if r[2] == total]
        gen = sorted((r for r in ok if r[2] < total), key=lambda r: r[2])
        if real:
            last.append(real[0][0] - real[0][4])
        if gen:
            first.append(gen[0][0] - gen[0][4])
    out.update({"lat_last_mean": mean(last), "lat_last_p99": pct(last, 0.99), "lat_first_mean": mean(first), "lat_first_p99": pct(first, 0.99)})
    kr = [r for r in recs if r[1] in keep]
    out["cuts"] = sum(1 for r in kr if "cut" in r[6])
    out["drops"] = sum(1 for r in kr if "drop" in r[6])
    out["late_pct"] = 100.0 * sum(1 for r in kr if "late" in r[6] and "drop" not in r[6]) / max(1, len(kr))
    out["frames"] = len(keep)
    return out


def dlssg_metrics(text, app_text, mult, skip):
    times = [float(m[1]) for m in SINK_PACE.finditer(text)]
    out = {}
    g = SINK_GAPS.search(text)
    if g:
        out["sink_gaps"] = {"n": int(g[1]), "mean": float(g[2]), "sd": float(g[3]), "p99": float(g[6]), "max": float(g[7])}
    probe = SINK_PROBE.search(text)
    out["probe"] = probe.group(0)[len("sink probe: "):] if probe else ""
    apps = [(int(m[1]), float(m[2])) for m in APP.finditer(app_text)]
    if not times:
        out["notes"] = "no 'sink: pace' lines (F5LOW_SINK_STATS=1, XEFG_CUDA_PACELOG=1; did Generator=dlssg run?)"
        return out
    if not apps:
        out["notes"] = "no 'app:' lines (REPRO_PACELOG=1): latency not computed"
    t0 = apps[min(skip, len(apps) - 1)][1] if apps else times[0]
    sel = [t for t in times if t >= t0 - 1.0]
    out.update(gap_stats([b - a for a, b in zip(sel, sel[1:])]))
    if apps:
        groups = [sel[i:i + mult] for i in range(0, len(sel) - mult + 1, mult)]
        best = None
        for d in range(-2, 4):
            lat_last, lat_first = [], []
            for gi, grp in enumerate(groups[:-3]):
                ai = min(skip, len(apps) - 1) + gi + d
                if not 0 <= ai < len(apps):
                    continue
                lat_last.append(grp[-1] - apps[ai][1])
                lat_first.append(grp[0] - apps[ai][1])
            if lat_last and min(lat_last) > -1.0 and (best is None or mean(lat_last) < mean(best[0])):
                best = (lat_last, lat_first, d)
        if best:
            out.update({"lat_last_mean": mean(best[0]), "lat_last_p99": pct(best[0], 0.99), "lat_first_mean": mean(best[1]),
                        "lat_first_p99": pct(best[1], 0.99)})
            out["notes"] = f"dlssg latency approximate (burst shift {best[2]:+d}, from the app's Present call)"
    out["cuts"] = out["drops"] = 0
    return out


def pm_metrics(path, skip_rows):
    try:
        with open(path, newline="") as f:
            rows = list(csv.DictReader(f))
    except OSError:
        return {}
    col = next((c for c in ("MsBetweenDisplayChange", "DisplayedTime") if rows and c in rows[0]), None)
    if col is None:
        return {"pm_notes": "no display column"}
    rows = rows[skip_rows:-10] if len(rows) > skip_rows + 10 else rows
    vals, hidden = [], 0
    for r in rows:
        try:
            x = float(r[col])
        except (ValueError, TypeError):
            x = 0.0
        if x > 0:
            vals.append(x)
        else:
            hidden += 1
    if not vals:
        return {"pm_notes": "no displayed frames"}
    return {"pm_n": len(vals), "pm_mean": mean(vals), "pm_sd": sd(vals), "pm_p99": pct(vals, 0.99), "pm_max": max(vals),
            "pm_shown_pct": 100.0 * len(vals) / (len(vals) + hidden)}


def cmd_run(argv):
    d = Path(argv[0])
    opt = {"gen": "own", "mult": 2, "skip": 60}
    meta = {}
    i = 1
    while i < len(argv):
        if argv[i] in ("--gen", "--mult", "--skip"):
            opt[argv[i][2:]] = argv[i + 1]
            i += 2
        elif argv[i] == "--meta":
            k, _, v = argv[i + 1].partition("=")
            meta[k] = v
            i += 2
        else:
            i += 1
    mult, skip = int(opt["mult"]), int(opt["skip"])
    log = (d / "f5low.log").read_text(errors="replace") if (d / "f5low.log").exists() else ""
    app = (d / "repro.out.txt").read_text(errors="replace") if (d / "repro.out.txt").exists() else ""
    out = dict(meta)
    out.update({"gen": opt["gen"], "mult": mult})
    out.update(own_metrics(log, skip) if opt["gen"] == "own" else dlssg_metrics(log, app, mult, skip))
    if (d / "presentmon.csv").exists():
        out.update(pm_metrics(d / "presentmon.csv", skip * mult))
    print(json.dumps(out))


COLS = ["gen", "scenario", "mult", "config", "exit", "gap_n", "gap_mean", "gap_sd", "gap_p99", "gap_max", "hitch2x", "lat_last_mean",
        "lat_last_p99", "lat_first_mean", "lat_first_p99", "cuts", "drops", "late_pct", "pm_n", "pm_mean", "pm_sd", "pm_p99", "pm_max",
        "pm_shown_pct", "probe", "notes"]


def f(v, n=2):
    return "" if v is None or v == "" else (f"{v:.{n}f}" if isinstance(v, float) else str(v))


def cmd_report(argv):
    d = Path(argv[0])
    rows = [json.loads(line) for line in (d / "results.jsonl").read_text().splitlines() if line.strip()]
    with open(d / "results.csv", "w", newline="") as fh:
        w = csv.writer(fh)
        w.writerow(COLS)
        for r in rows:
            w.writerow([r.get(c, "") for c in COLS])
    md = ["# Pacer A/B", "", "Gaps and latencies in ms. lat_last: the app's own frame presented - its ready stamp; lat_first: the first generated "
          "frame. Delta columns are against the `off` row of the same generator, scenario and multiplier. PresentMon columns (pm_) are "
          "the display-change gaps of the real chain when PresentMon was available.", ""]
    keys = sorted({(r["gen"], r["scenario"]) for r in rows}, key=lambda k: (k[0], k[1]))
    for gen, sc in keys:
        md += [f"## {gen} - {sc}", "",
               "| mult | config | gap sd | p99 | max | >2x | d sd | lat last | d last | lat 1st | d 1st | cuts | drops | late% | pm sd | pm p99 | shown% | notes |",
               "|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|---|"]
        sub = [r for r in rows if r["gen"] == gen and r["scenario"] == sc]
        for r in sorted(sub, key=lambda r: (r["mult"], r["config"] != "off", r["config"])):
            base = next((b for b in sub if b["mult"] == r["mult"] and b["config"] == "off"), None)

            def dl(key):
                if base is None or r is base or key not in r or key not in base:
                    return ""
                return f"{r[key] - base[key]:+.2f}"

            md.append("| {}x | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} | {} |".format(
                r["mult"], r["config"], f(r.get("gap_sd")), f(r.get("gap_p99")), f(r.get("gap_max")), f(r.get("hitch2x"), 0),
                dl("gap_sd"), f(r.get("lat_last_mean")), dl("lat_last_mean"), f(r.get("lat_first_mean")), dl("lat_first_mean"),
                f(r.get("cuts"), 0), f(r.get("drops"), 0), f(r.get("late_pct"), 1), f(r.get("pm_sd")), f(r.get("pm_p99")),
                f(r.get("pm_shown_pct"), 1), r.get("notes", "") if int(r.get("exit") or 0) == 0 else "exit " + str(r["exit"]) + " " + r.get("notes", "")))
        md.append("")
    (d / "results.md").write_text("\n".join(md), encoding="utf-8")
    print(f"{len(rows)} runs -> {d / 'results.csv'}, {d / 'results.md'}")


if __name__ == "__main__":
    if len(sys.argv) < 3 or sys.argv[1] not in ("run", "report"):
        sys.exit(__doc__)
    (cmd_run if sys.argv[1] == "run" else cmd_report)(sys.argv[2:])
