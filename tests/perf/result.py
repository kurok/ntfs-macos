#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""result.py - turn the output of bench.py into a result document (ntfsmac-test-result/1), P-* cases (T-M5-07).

Usage: result.py --bench FILE --env A|B --out DIR [--repo DIR] [--started TS] [--bench-rc N] [--meta K=V]
       result.py selftest

Cases: P-RUN-01 (the run finished: every phase of every repetition, no error; carries the parameters),
P-SEQ-01/02 sequential write/read, P-RND-01/02 random write/read, P-SMALL-01..04 small files create, stat, read,
delete, P-DIR-01.. directory listing and listing plus stat per entry count (two cases per count), P-RES-01
worker (and macOS FSKit module) CPU seconds and peak RSS of every phase. A case passes when its phase finished in
every repetition with verified data; the numbers are in the note after `data=` as JSON (the report generator
reads them from there). A phase that did not finish is a fail, never a missing case.
Exit: 0 every case passed, 1 a case failed, 2 usage or setup error.
"""
import argparse
import json
import os
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, os.path.join(REPO, "tests", "harness"))

FIXED = [("P-SEQ-01", "seq_write", "sequential write"), ("P-SEQ-02", "seq_read", "sequential read (cold)"),
         ("P-RND-01", "rnd_write", "random 4 KiB write"), ("P-RND-02", "rnd_read", "random 4 KiB read (cold)"),
         ("P-SMALL-01", "small_create", "small files create+write"), ("P-SMALL-02", "small_stat", "small files stat"),
         ("P-SMALL-03", "small_read", "small files read (cold)"), ("P-SMALL-04", "small_delete", "small files delete")]


def case_list(doc):
    out = list(FIXED)
    for k, n in enumerate(doc["params"]["dir_entries"]):
        out.append(("P-DIR-%02d" % (2 * k + 1), "dir_%d_list" % n, "directory of %d entries, readdir (cold)" % n))
        out.append(("P-DIR-%02d" % (2 * k + 2), "dir_%d_stat" % n, "directory of %d entries, readdir + stat (cold)" % n))
    k = 0
    for t in doc.get("raw_targets", []):
        for ph, what in (("seq_write", "sequential write"), ("seq_read", "sequential read"),
                         ("rnd_write", "random 4 KiB write"), ("rnd_read", "random 4 KiB read")):
            k += 1
            out.append(("P-RAW-%02d" % k, "raw_%s_%s" % (t, ph), "baseline %s, %s" % (t, what)))
    return out


def build_cases(doc, rc=0):
    reps = doc["params"]["reps"]
    cases = []
    ph = doc["phases"]
    for cid, name, what in case_list(doc):
        p = ph.get(name)
        if p is None or p.get("n") != reps:
            n = 0 if p is None else p.get("n")
            err = "; ".join((p or {}).get("errors", [])[:2]) or "no result"
            cases.append({"id": cid, "status": "fail", "duration_s": 0.0, "evidence": ["bench.json"],
                          "note": "%s: only %s of %d repetitions finished (%s)" % (what, n, reps, err)})
            continue
        d = {"phase": name, "unit": p["unit"], "median": p["median"], "min": p["min"], "max": p["max"], "n": p["n"],
             "count": p["count"], "secs_median": p["secs_median"], "fsync_s": p.get("fsync_s"),
             "coarse_cpu": p["secs_median"] < 0.5}
        for kk in ("worker_cpu_s", "module_cpu_s", "helper_cpu_s", "sys_cpu_s"):
            d[kk] = p.get(kk)
        txt = "%s: median %s %s (min %s, max %s, n=%d), %.3f s per run" % (what, p["median"], p["unit"], p["min"], p["max"],
                                                                         p["n"], p["secs_median"])
        cases.append({"id": cid, "status": "pass", "duration_s": round(p["secs_median"] * p["n"], 3),
                      "evidence": ["bench.json"], "note": txt + "; data=" + json.dumps(d, separators=(",", ":"))})
    res = {}
    missing = []
    for _cid, name, _w in case_list(doc):
        if name.startswith("raw_"):
            continue
        p = ph.get(name) or {}
        r = {k: p.get(k) for k in ("worker_cpu_s", "worker_rss_kib", "module_cpu_s", "module_rss_kib", "helper_cpu_s",
                                   "helper_rss_kib", "sys_cpu_s", "secs_median")}
        res[name] = r
        if r["worker_cpu_s"] is None or r["worker_rss_kib"] is None:
            missing.append(name)
    what = "worker CPU seconds (user+sys, median of runs) and peak RSS per phase"
    if doc["platform"]["system"] != "Linux":
        what += " and the same for the macFUSE FSKit module processes"
    cases.append({"id": "P-RES-01", "status": "fail" if missing else "pass", "duration_s": 0.0, "evidence": ["bench.json"],
                  "note": what + "; " + ("no worker sample for: " + ", ".join(missing) if missing else "recorded for every phase")
                  + "; data=" + json.dumps(res, separators=(",", ":"))})
    bad = list(doc.get("errors", []))
    if rc not in (0, 1):
        bad.append("the benchmark ended with status %d" % rc)
    if not doc.get("complete", True):
        bad.append("the result is partial: the run did not finish (deadline, kill or stop at the wall limit)")
    run = {"raw_targets": doc.get("raw_targets", []), "raw_skipped": doc.get("raw_skipped", []), "params": doc["params"], "platform": doc["platform"], "meta": doc.get("meta", {}), "wall_s": doc.get("wall_s"),
           "label": doc.get("label", "")}
    cases.append({"id": "P-RUN-01", "status": "fail" if bad else "pass", "duration_s": float(doc.get("wall_s") or 0),
                  "evidence": ["bench.json"],
                  "note": ("; ".join(bad[:4]) if bad else "every phase of every repetition finished with verified data")
                  + "; data=" + json.dumps(run, separators=(",", ":"))})
    for i, (nm, why) in enumerate(doc.get("raw_skipped", [])):
        cases.append({"id": "P-RAW-" + "".join(c for c in nm.upper() if c.isalnum()), "status": "skip", "duration_s": 0.0,
                      "evidence": ["bench.json"], "note": "baseline target %s not available: %s" % (nm, why or "no reason given")})
    return cases


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--bench")
    ap.add_argument("--env")
    ap.add_argument("--out")
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--started")
    ap.add_argument("--bench-rc", type=int, default=0)
    ap.add_argument("--meta", action="append", default=[], help="KEY=VALUE added to the run's metadata")
    a = ap.parse_args(argv)
    if not (a.bench and a.env and a.out) or a.env not in ("A", "B"):
        ap.error("--bench, --env A|B and --out are required")
    import report
    import inputs
    with open(a.bench, "r", encoding="utf-8") as f:
        doc = json.load(f)
    os.makedirs(a.out, exist_ok=True)
    dest = os.path.join(a.out, "bench.json")
    if os.path.abspath(a.bench) != os.path.abspath(dest):
        with open(dest, "w", encoding="utf-8") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
    doc.setdefault("meta", {}).update(dict(m.split("=", 1) for m in a.meta if "=" in m))
    cases = build_cases(doc, a.bench_rc)
    inputs.annotate(cases, a.out)
    meta = doc["meta"]
    sha, dirty = report.git_info(a.repo)
    now = report.utc_now()
    r = report.build_result(sha, dirty, a.env, meta.get("host_os") or doc["platform"]["system"], meta.get("guest_os"),
                            meta.get("macfuse") or None, meta.get("ntfs3g") or None, a.started or doc.get("started") or now,
                            now, cases)
    errs = report.validate(r)
    if errs:
        raise RuntimeError("result is invalid: " + "; ".join(errs))
    with open(os.path.join(a.out, "result.json"), "w", encoding="utf-8") as f:
        json.dump(r, f, indent=1)
        f.write("\n")
    lines = ["%s %s  %s" % (c["id"], c["status"].upper(), c["note"].split("; data=")[0]) for c in cases]
    with open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if all(c["status"] == "pass" for c in cases) else 1


def selftest():
    py = sys.executable
    small = ["--reps", "2", "--seq-mib", "4", "--rnd-ops", "40", "--small-files", "20", "--dir-entries", "30,60"]
    with tempfile.TemporaryDirectory() as t:
        os.makedirs(os.path.join(t, "w"))
        cnt = [0]

        def run(extra):
            cnt[0] += 1
            out = os.path.join(t, "b%d.json" % cnt[0])
            r = subprocess.run([py, "-I", os.path.join(HERE, "bench.py"), "--dir", os.path.join(t, "w"), "--out", out]
                               + small + extra, stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
            with open(out) as f:
                return r.returncode, json.load(f)

        rc, doc = run([])
        assert rc == 0, rc
        cs = {c["id"]: c for c in build_cases(doc)}
        assert all(c["status"] == "pass" for c in cs.values() if c["id"] != "P-RES-01"), cs
        # no worker pid in the plain-directory mode: P-RES-01 must say so, not pass
        assert cs["P-RES-01"]["status"] == "fail", cs["P-RES-01"]
        assert set(cs) >= {"P-SEQ-01", "P-RND-02", "P-SMALL-04", "P-DIR-01", "P-DIR-02", "P-DIR-03", "P-DIR-04", "P-RUN-01",
                           "P-RAW-01", "P-RAW-04"}, sorted(cs)
        assert "fsync_s" in cs["P-SEQ-01"]["note"] and doc["complete"] is True
        assert "data=" in cs["P-SEQ-01"]["note"]
        expect = {"corrupt-read": ["P-SEQ-02", "P-RND-02", "P-SMALL-03", "P-RAW-02", "P-RAW-04"], "short-read": ["P-SEQ-02"],
                  "drop-entry": ["P-DIR-01", "P-DIR-02", "P-DIR-03", "P-DIR-04"]}
        for fault, ids in expect.items():
            rc, doc = run(["--fault", fault])
            assert rc == 1, (fault, rc)
            cs = {c["id"]: c for c in build_cases(doc, rc)}
            for i in ids:
                assert cs[i]["status"] == "fail", (fault, i, cs[i])
            assert cs["P-RUN-01"]["status"] == "fail", fault
            assert cs["P-SEQ-01"]["status"] == "pass", fault  # the write phase is not what the fault touches
        # a partial result (killed run) must fail P-RUN-01
        doc["complete"] = False
        assert {c["id"]: c for c in build_cases(doc)}["P-RUN-01"]["status"] == "fail"
    print("result.py selftest ok")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        sys.exit(selftest())
    try:
        sys.exit(main(sys.argv[1:]))
    except (RuntimeError, ValueError, OSError) as e:
        print("result.py: %s" % e, file=sys.stderr)
        sys.exit(2)
