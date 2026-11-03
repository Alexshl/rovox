#!/usr/bin/env python3
# SPDX-License-Identifier: AGPL-3.0-or-later
# Copyright (C) 2026 Rovox contributors
#
# FX parity harness: renders the same input with the same FX settings in REAPER (reaper_driver.py) and in Rovox (rovox_driver.py,
# through import.rpp of the RPP + FX sidecar REAPER wrote), compares the two (compare.py) and prints a pass/fail table.
#   run.py --all                       all fixtures in fixtures/*.json
#   run.py reacomp_fast reaeq         fixtures whose name contains one of the words
#   run.py --list
#   run.py project "<file>.RPP" [--start S --end E]    whole-project comparison (REAPER renders the RPP in a new tab, Rovox import.rpp)
# Options: --out DIR (default ~/.rovox-parity), --reuse-reaper (use the REAPER renders already in DIR), --reuse-rovox.
# Needs: REAPER running with mcp_bridge_file_v2.lua, a Rovox container (see rovox_driver.py for URL/token/mounts).
# Outputs (outside git): DIR/reports/<name>.json, DIR/plots/<name>.png, DIR/summary.json, renders in DIR/work/<name>/.
import argparse
import glob
import json
import os
import sys
import traceback

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)

import compare  # noqa: E402
import gen_signals  # noqa: E402

DEFAULT_THRESHOLDS = {
    "eq": {"null_db": -60, "bands_db": 0.1},
    "delay": {"null_db": -60, "bands_db": 0.1},
    "comp": {"null_db": -45, "lufs_db": 0.1, "peak_db": 0.1, "env_db": 0.5},
    "gate": {"null_db": -45, "lufs_db": 0.1, "peak_db": 0.1, "env_db": 0.5},
    "limit": {"null_db": -45, "lufs_db": 0.1, "peak_db": 0.1, "env_db": 0.5},
    "verb": {"lufs_db": 0.3, "bands_db": 1.0, "rt60_rel": 0.10},
    "bare": {"null_db": -60, "lufs_db": 0.1, "peak_db": 0.1},
    "project": {"lufs_db": 0.2, "peak_db": 0.2, "bands_db": 0.5},
}


def load_fixtures():
    out = {}
    for p in sorted(glob.glob(os.path.join(HERE, "fixtures", "*.json"))):
        f = json.load(open(p))
        out[f["name"]] = f
    return out


def fmt(v, spec="%.2f"):
    return "-" if v is None else spec % v


def run_fixture(fx, out, reuse_reaper, reuse_rovox):
    import reaper_driver
    import rovox_driver

    work = os.path.join(out, "work", fx["name"])
    os.makedirs(work, exist_ok=True)
    sig = gen_signals.write(fx["signal"], fx["channels"], os.path.join(out, "signals"))
    name = fx["name"]
    ref_wav = os.path.join(work, name + ".wav")
    log_path = os.path.join(work, name + ".reaper.json")
    if reuse_reaper and os.path.exists(ref_wav) and os.path.exists(log_path):
        log = json.load(open(log_path, encoding="utf-8", errors="replace"))
    else:
        job = {"name": name, "out_dir": work, "signal": sig, "fx": fx.get("fx"), "params": fx.get("params", []), "config": fx.get("config"),
               "tail_ms": fx.get("tail_ms", 0), "track_vol_db": fx.get("track_vol_db"), "track_pan": fx.get("track_pan"),
               "item_vol_db": fx.get("item_vol_db"), "fade_in": fx.get("fade_in"), "fade_out": fx.get("fade_out"), "label": name,
               "item_info": fx.get("item_info")}
        log = reaper_driver.run_job(job)["log"]
    rpp = os.path.join(work, name + ".tab.RPP")
    rovox_wav = os.path.join(work, name + ".rovox.wav")
    imp = None
    if not (reuse_rovox and os.path.exists(rovox_wav)):
        imp = rovox_driver.render(rpp, log["fx"], work, name, tail_s=fx.get("tail_ms", 0) / 1000.0)["import"]
    return ref_wav, rovox_wav, sig, log, imp, {"max_lag_s": 0.05, "want_rt60": bool(fx.get("rt60"))}


def run_project(rpp, out, start, end, reuse_reaper, reuse_rovox):
    import reaper_driver
    import rovox_driver

    name = "project_" + os.path.splitext(os.path.basename(rpp))[0].replace(" ", "_")
    if start is not None:
        name += "_%g_%g" % (start, end)
    work = os.path.join(out, "work", name)
    os.makedirs(work, exist_ok=True)
    ref_wav = os.path.join(work, name + ".wav")
    log_path = os.path.join(work, name + ".reaper.json")
    if reuse_reaper and os.path.exists(ref_wav) and os.path.exists(log_path):
        log = json.load(open(log_path, encoding="utf-8", errors="replace"))
    else:
        log = reaper_driver.run_project(rpp, work, name, start, end)["log"]
    rovox_wav = os.path.join(work, name + ".rovox.wav")
    imp = None
    if not (reuse_rovox and os.path.exists(rovox_wav)):
        imp = rovox_driver.render(rpp, log["fx"], work, name, start=start, end=end)["import"]
    return name, ref_wav, rovox_wav, log, imp, {"max_lag_s": 0.5}


def evaluate(name, kind, ref_wav, rovox_wav, sig, thresholds, out, opts):
    rep = compare.compare(ref_wav, rovox_wav, sig, opts.get("max_lag_s", 0.05), opts.get("want_rt60", False))
    os.makedirs(os.path.join(out, "plots"), exist_ok=True)
    compare.plot(rep, os.path.join(out, "plots", name + ".png"), name)
    checks = compare.check(rep, thresholds)
    return rep, checks


def row(name, rep, checks, status):
    rt = [abs(v["rel_diff"]) for v in rep.get("rt60", {}).values() if v["rel_diff"] is not None]
    return "%-22s %-5s %8s %8s %7s %7s %7s %7s %7s  %s" % (
        name, status, fmt(rep["null"]["rms_dbfs"], "%.1f"), fmt(rep["null"]["peak_dbfs"], "%.1f"), fmt(rep["lufs"]["diff"], "%+.2f"),
        fmt(rep["true_peak_db"]["diff"], "%+.2f"), fmt(rep["bands"]["max_abs_db"]), fmt(rep["envelope"]["max_abs_db"]),
        fmt(max(rt) * 100 if rt else None, "%.0f%%"),
        "; ".join("%s=%s>%s" % (n, fmt(v, "%.3g"), lim) for n, v, lim, ok in checks if not ok))


HEADER = "%-22s %-5s %8s %8s %7s %7s %7s %7s %7s  %s" % ("fixture", "", "null rms", "null pk", "dLUFS", "dTP", "band", "env", "rt60", "failed checks")


def main():
    ap = argparse.ArgumentParser(description=__doc__, formatter_class=argparse.RawDescriptionHelpFormatter)
    ap.add_argument("names", nargs="*", help="fixture names (substring match), or: project FILE.RPP")
    ap.add_argument("--all", action="store_true")
    ap.add_argument("--list", action="store_true")
    ap.add_argument("--out", default=os.path.expanduser("~/.rovox-parity"))
    ap.add_argument("--start", type=float)
    ap.add_argument("--end", type=float)
    ap.add_argument("--reuse-reaper", action="store_true")
    ap.add_argument("--reuse-rovox", action="store_true")
    a = ap.parse_args()
    fixtures = load_fixtures()
    if a.list:
        for n, f in fixtures.items():
            print("%-22s %-6s %-10s %-10s %s" % (n, f["kind"], f.get("fx") or "-", f["signal"], f.get("description", "")))
        return 0
    os.makedirs(os.path.join(a.out, "reports"), exist_ok=True)
    results = []
    print(HEADER)

    def finish(name, kind, thresholds, ref_wav, rovox_wav, sig, opts, log, imp):
        rep, checks = evaluate(name, kind, ref_wav, rovox_wav, sig, thresholds, a.out, opts)
        ok = all(c[3] for c in checks) and bool(checks)
        print(row(name, rep, checks, "PASS" if ok else "FAIL"))
        doc = compare.clean(rep)
        doc.update({"name": name, "kind": kind, "thresholds": thresholds, "pass": ok,
                    "checks": [{"name": n, "value": v, "limit": lim, "pass": p} for n, v, lim, p in checks],
                    "reaper_settings": log.get("settings"), "reaper_track": log.get("track"), "rovox_import": imp})
        json.dump(doc, open(os.path.join(a.out, "reports", name + ".json"), "w"), indent=1, default=float)
        results.append({"name": name, "pass": ok, "null_rms": rep["null"]["rms_dbfs"], "lufs_diff": rep["lufs"]["diff"],
                        "tp_diff": rep["true_peak_db"]["diff"], "band_max": rep["bands"]["max_abs_db"], "env_max": rep["envelope"]["max_abs_db"],
                        "failed": [n for n, v, lim, p in checks if not p]})

    if a.names and a.names[0] == "project":
        if len(a.names) < 2:
            ap.error("project needs an .RPP path")
        try:
            name, ref_wav, rovox_wav, log, imp, opts = run_project(os.path.expanduser(a.names[1]), a.out, a.start, a.end, a.reuse_reaper, a.reuse_rovox)
            finish(name, "project", DEFAULT_THRESHOLDS["project"], ref_wav, rovox_wav, None, opts, log, imp)
        except Exception as e:
            traceback.print_exc()
            results.append({"name": "project", "pass": False, "error": str(e)})
            print("%-22s ERROR  %s" % ("project", str(e)[:300]))
    else:
        if not a.all and not a.names:
            ap.error("give fixture names or --all")
        sel = [f for n, f in fixtures.items() if a.all or any(w in n for w in a.names)]
        for fx in sel:
            th = dict(DEFAULT_THRESHOLDS[fx["kind"]])
            th.update(fx.get("thresholds", {}))
            try:
                ref_wav, rovox_wav, sig, log, imp, opts = run_fixture(fx, a.out, a.reuse_reaper, a.reuse_rovox)
                finish(fx["name"], fx["kind"], th, ref_wav, rovox_wav, sig, opts, log, imp)
            except Exception as e:
                results.append({"name": fx["name"], "pass": False, "error": str(e)})
                print("%-22s ERROR  %s" % (fx["name"], str(e).replace("\n", " ")[:400]))
    json.dump(results, open(os.path.join(a.out, "summary.json"), "w"), indent=1, default=float)
    npass = sum(1 for r in results if r["pass"])
    print("\n%d/%d passed. Reports: %s/reports, plots: %s/plots" % (npass, len(results), a.out, a.out))
    return 0 if npass == len(results) else 1


if __name__ == "__main__":
    sys.exit(main())
