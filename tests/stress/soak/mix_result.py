#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""mix_result.py - result of the soak and parallel suites (T-M5-02, S-SOAK-01, S-PAR-01), host side.

  mix_result.py finalize --dir OUT --repo REPO --started ISO     OUT/{plan.txt,meta.txt,runs.tsv,NAME/*} -> OUT/result.json
  mix_result.py selftest

OUT/plan.txt (written by the host script before the guest starts): kind=soak|par, duration, baseline, verify,
workers, hang, controls (list of "corrupt stall"). OUT/runs.tsv has one line per mount cycle (guest script).

The real case (S-SOAK-01 / S-PAR-01) passes only when ALL of this holds, judged here and not in the guest:
the driver exited 0 with verdict pass; it ran the planned duration and the planned number of workers; zero
mismatches and errors; no hang (longest single operation and every progress gap below the limit); at least the
planned number of full verifications per worker; the volume unmounted cleanly, `ntfsfix -n` exit 0, the
remount came up `Mounted rw` and the walk of the whole tree equals the manifests (exit 0, no mismatch); for the
soak, the RSS rule is applied AGAIN here to OUT/NAME/rss.tsv with mixlib.rss_growth (baseline = first sample at
or after the baseline point, growth = peak after it, below 10 percent, enough samples).
The control case (S-SOAK-01-CONTROL / S-PAR-01-CONTROL) passes only when the same checks FAIL on the injected
fault and on nothing else: an injected mismatch must give a driver exit 1 whose only failure kind is mismatch
and a failing remount verification; the simulated stall (par) a driver exit 1 whose only failure kind is hang,
naming the stalled worker; for the soak, the RSS rule on synthetic samples must give pass / fail / error where
it should (boundaries included). A partial run (fewer cycles than planned) fails.
"""
import argparse
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "harness"))
sys.path.insert(0, HERE)
import mixlib  # noqa: E402
import report  # noqa: E402

IDS = {"soak": "S-SOAK", "par": "S-PAR"}
TITLES = {"soak": "soak: mixed workload with a model, full verification, worker RSS",
          "par": "parallel: concurrent writers in private and shared directories, latency and hang watch"}


def read_kv(path):
    kv = {}
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
    return kv


def read_json(path):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def read_runs(d):
    runs = {}
    p = os.path.join(d, "runs.tsv")
    if os.path.isfile(p):
        with open(p, "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                c = line.split("\t")
                if len(c) == 8:
                    runs[c[0]] = {"name": c[0], "role": c[1], "drc": c[2], "vrc": c[3], "nf1": c[4], "nf2": c[5], "mode1": c[6], "mode2": c[7]}
    return runs


def read_rss(path):
    s = []
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                a = line.split("\t")
                if len(a) == 2:
                    try:
                        s.append((float(a[0]), int(a[1])))
                    except ValueError:
                        pass
    return s


def kinds(summary):
    return sorted(set(f["kind"] for f in (summary or {}).get("failures", [])))


CRITERION = {"soak": ("5.2", 21600, 1, 3600), "par": ("5.3", 1800, 16, None)}
CONTROLS = {"soak": {"corrupt"}, "par": {"corrupt", "stall"}}


def is_criterion(kind, plan):
    """does the planned run have the parameters of the criterion (5.2: 6 h, baseline 1 h; 5.3: 30 min, 16 writers; hang 60 s),
    run ALL of its negative controls and no injected fault (M5-F04, M5-F10). Only such a run writes the plain case ids;
    every other run writes the ids with the suffix -SHORT"""
    _, dur, workers, base = CRITERION[kind]
    try:
        return (float(plan["duration"]) >= dur and int(plan["workers"]) == workers and float(plan["hang"]) <= 60
                and (base is None or float(plan["baseline"]) == base)
                and CONTROLS[kind] <= set(plan.get("controls", "").split()) and not plan.get("fault"))
    except (KeyError, ValueError):
        return False


def real_problems(kind, plan, run, d):
    """list of strings: everything that disqualifies the real run"""
    pr = []
    if run is None:
        return ["the real run did not happen (no entry in runs.tsv)"]
    sd = os.path.join(d, run["name"])
    s = read_json(os.path.join(sd, "summary.json"))
    if s is None:
        return ["no summary.json: the driver did not finish"]
    dur = float(plan["duration"])
    if run["drc"] != "0" or s.get("verdict") != "pass":
        pr.append("driver exit %s, verdict %s, failures %s: %s" % (run["drc"], s.get("verdict"), kinds(s), "; ".join(f["detail"][:150] for f in s.get("failures", [])[:3])))
    if s.get("duration_actual_s", 0) < dur:
        pr.append("ran %.1f s of the planned %.0f s" % (s.get("duration_actual_s", 0), dur))
    if s.get("workers") != int(plan["workers"]) or len(s.get("worker_ok", [])) != int(plan["workers"]):
        pr.append("%s worker(s) reported, %s planned" % (len(s.get("worker_ok", [])), plan["workers"]))
    if s.get("mismatches") != 0 or s.get("errors") != 0:
        pr.append("%s mismatch(es), %s error(s)" % (s.get("mismatches"), s.get("errors")))
    lim = float(plan["hang"])
    if s.get("hang") or s.get("lat_max_s", 0) >= lim:
        pr.append("hang: longest operation %.2f s (limit %.0f s), watchdog %s" % (s.get("lat_max_s", 0), lim, s.get("hang")))
    need = int(dur // float(plan["verify"]))
    if s.get("verifies_min_per_worker", 0) < max(need, 1):
        pr.append("only %s full verification(s) per worker, %d planned" % (s.get("verifies_min_per_worker"), max(need, 1)))
    if run["vrc"] != "0":
        pr.append("remount verification exit %s" % run["vrc"])
    rm = read_json(os.path.join(sd, "remount.json"))
    if rm is None or rm.get("mismatches") != 0 or not rm.get("files"):
        pr.append("remount.json missing, empty or with mismatches: %s" % (rm and rm.get("detail", [])[:3]))
    if run["nf1"] != "0" or run["nf2"] != "0":
        pr.append("ntfsfix -n exit %s / %s" % (run["nf1"], run["nf2"]))
    if run["mode1"] != "Mounted rw" or run["mode2"] != "Mounted rw":
        pr.append("mount state '%s' then '%s' (Mounted rw expected twice)" % (run["mode1"], run["mode2"]))
    if kind == "soak":
        base = float(plan["baseline"])
        if not (0 < base < dur):
            pr.append("baseline point %.0f s is not inside the run of %.0f s" % (base, dur))
        r = mixlib.rss_growth(read_rss(os.path.join(sd, "rss.tsv")), base)
        if r["status"] != "pass":
            pr.append("RSS rule: %s" % json.dumps(r))
    return pr


def rss_synthetic():
    """the RSS rule on synthetic samples: (ok, text)"""
    def ser(base, later):
        return [(float(i * 10), 1000) for i in range(6)] + [(60.0, base)] + [(60.0 + (i + 1) * 10, later) for i in range(8)]
    want = [(ser(1000, 1000), "pass"), (ser(1000, 1099), "pass"), (ser(1000, 1100), "fail"), (ser(1000, 1300), "fail"),
            ([(0.0, 1000), (10.0, 1000)], "error")]
    got = [mixlib.rss_growth(s, 60.0)["status"] for s, _ in want]
    ok = got == [w for _, w in want]
    return ok, "synthetic samples 0%%/9.9%%/10.0%%/30%%/no baseline gave %s (expected %s)" % ("/".join(got), "/".join(w for _, w in want))


def control_checks(kind, plan, runs, d):
    """list of (name, ok, text)"""
    out = []
    want = plan.get("controls", "").split()
    if kind == "soak":
        ok, txt = rss_synthetic()
        out.append(("rss-synthetic", ok, txt))
    for c in want:
        nm = "%s-ctl-%s" % (kind, c)
        run = runs.get(nm)
        if run is None:
            out.append((c, False, "control %s did not run" % c))
            continue
        s = read_json(os.path.join(d, nm, "summary.json"))
        if s is None:
            out.append((c, False, "control %s: no summary.json" % c))
            continue
        if c == "corrupt":
            rm = read_json(os.path.join(d, nm, "remount.json")) or {}
            ok = (run["drc"] == "1" and kinds(s) == ["mismatch"] and s.get("mismatches", 0) >= 1 and run["vrc"] == "1" and rm.get("mismatches", 0) >= 1)
            out.append((c, ok, "injected byte flip: driver exit %s, failure kinds %s, %s mismatch(es), remount verification exit %s with %s mismatch(es)" % (
                run["drc"], kinds(s), s.get("mismatches"), run["vrc"], rm.get("mismatches"))))
        elif c == "stall":
            h = s.get("hang") or {}
            ok = run["drc"] == "1" and kinds(s) == ["hang"] and h.get("worker") == 1
            out.append((c, ok, "simulated stall of worker 1: driver exit %s, failure kinds %s, watchdog %s" % (run["drc"], kinds(s), h)))
    return out


def pack_state(d):
    """merge the per-worker files of every run directory into one workers.json and an archive (the harness keeps at
    most 40 evidence files per stage)"""
    import tarfile
    for n in sorted(os.listdir(d)):
        sd = os.path.join(d, n)
        if not os.path.isdir(sd):
            continue
        ws, bulk = [], []
        for f in sorted(os.listdir(sd)):
            if f.startswith("worker-") and f.endswith(".json"):
                w = read_json(os.path.join(sd, f))
                if w is not None:
                    w.pop("hist", None)
                    ws.append(w)
                bulk.append(f)
            elif f.startswith("man-") and f.endswith(".tsv"):
                bulk.append(f)
        if ws:
            with open(os.path.join(sd, "workers.json"), "w", encoding="utf-8") as fh:
                json.dump(ws, fh, indent=1)
        if bulk:
            with tarfile.open(os.path.join(sd, "state.tar.gz"), "w:gz") as tf:
                for f in bulk:
                    tf.add(os.path.join(sd, f), arcname=f)
            for f in bulk:
                os.unlink(os.path.join(sd, f))


def build_cases(kind, plan, runs, meta, d, have):
    crit = is_criterion(kind, plan)
    sfx = "" if crit else "-SHORT"
    cid = IDS[kind] + "-01"
    cases = []
    real = runs.get("%s-real" % kind)
    pr = real_problems(kind, plan, real, d)
    s = read_json(os.path.join(d, "%s-real" % kind, "summary.json")) or {}
    ev = [e for e in ("plan.txt", "meta.txt", "runs.tsv", "guest.log", "provenance.txt") if e in have]
    for n in ("summary.json", "run.log", "rss.tsv", "workers.json", "remount.json", "verify.log", "state.tar.gz"):
        if os.path.isfile(os.path.join(d, "%s-real" % kind, n)):
            ev.append("%s-real/%s" % (kind, n))
    if s:
        note = "%s: %.0f s planned, %.0f s run, %s worker(s), %s ops, %s full verification(s), %.1f MiB written, %.1f MiB verified, %s mismatch(es), %s error(s); op latency p50<=%.0f ms p99<=%.0f ms max %.2f s (hang limit %s s)" % (
            TITLES[kind], float(plan["duration"]), s.get("duration_actual_s", 0), s.get("workers"), s.get("ops_total"), s.get("verifies"),
            s.get("bytes_written", 0) / 1048576.0, s.get("bytes_verified", 0) / 1048576.0, s.get("mismatches"), s.get("errors"),
            s.get("lat_p50_s", 0) * 1000, s.get("lat_p99_s", 0) * 1000, s.get("lat_max_s", 0), plan["hang"])
        if kind == "soak":
            r = mixlib.rss_growth(read_rss(os.path.join(d, "soak-real", "rss.tsv")), float(plan["baseline"]))
            if r["status"] == "pass" or r["status"] == "fail":
                note += "; worker RSS baseline %d kB at %.0f s, peak %d kB, final %d kB, growth %.2f%% (rule: below %.0f%%), %d samples after the baseline" % (
                    r["baseline_kb"], r["baseline_t"], r["peak_kb"], r["final_kb"], r["growth_pct"], r["limit_pct"], r["samples_after"])
    else:
        note = TITLES[kind]
    if not crit:
        note = "shortened run, not criterion %s (the case id has the suffix -SHORT): %s" % (CRITERION[kind][0], note)
    note += "; parameters: duration %s s, baseline %s s, workers %s, hang limit %s s, controls '%s', fault '%s'" % (
        plan["duration"], plan["baseline"], plan["workers"], plan["hang"], plan.get("controls", ""), plan.get("fault", ""))
    if plan.get("fault"):
        note += "; a fault was injected into this run on purpose (%s), so it cannot pass" % plan["fault"]
        pr.append("injected fault")
    if meta.get("checks_failed") not in (None, "", "0"):
        pr.append("%s guest check(s) failed (mount mode, one worker holds the device, clean unmount, ntfsfix; see guest.log)" % meta["checks_failed"])
    for p in pr:
        note += "; " + p
    cases.append({"id": cid + sfx, "status": "fail" if pr else "pass", "duration_s": round(float(s.get("duration_actual_s", 0)), 3), "evidence": ev, "note": note})
    if plan.get("controls", "").split():
        cks = control_checks(kind, plan, runs, d)
        bad = [c for c in cks if not c[1]]
        cev = ["runs.tsv"] if "runs.tsv" in have else []
        for r in runs.values():
            if r["role"].startswith("control"):
                for n in ("summary.json", "run.log", "remount.json", "verify.log"):
                    if os.path.isfile(os.path.join(d, r["name"], n)):
                        cev.append("%s/%s" % (r["name"], n))
        note = "negative controls (%s): %s" % (" ".join(plan["controls"].split()), "; ".join("%s %s: %s" % (c[0], "caught" if c[1] else "NOT CAUGHT", c[2]) for c in cks))
        cases.append({"id": cid + "-CONTROL" + sfx, "status": "fail" if bad or not cks else "pass", "duration_s": 0.0, "evidence": cev, "note": note})
    run_problems = []
    if meta.get("guest_rc") not in ("0", "1"):
        run_problems.append("the guest script ended abnormally (exit %s; 0 or 1 expected)" % meta.get("guest_rc", "unknown"))
    if meta.get("checks_failed") not in (None, "", "0"):
        run_problems.append("%s guest check(s) failed" % meta["checks_failed"])
    if meta.get("setup_failed") not in (None, "", "0"):
        run_problems.append("%s setup step(s) failed before the first run" % meta["setup_failed"])
    if run_problems:
        cases.append({"id": IDS[kind] + "-RUN" + sfx, "status": "fail", "duration_s": 0.0,
                      "evidence": [e for e in ("guest.log", "meta.txt") if e in have], "note": "; ".join(run_problems)})
    return cases


def finalize(d, repo, started):
    plan = read_kv(os.path.join(d, "plan.txt"))
    if plan.get("kind") not in IDS or not all(k in plan for k in ("duration", "baseline", "verify", "workers", "hang")):
        print("mix_result: plan.txt is missing or unusable in %s" % d, file=sys.stderr)
        return 2
    meta = read_kv(os.path.join(d, "meta.txt"))
    pack_state(d)
    runs = read_runs(d)
    cases = build_cases(plan["kind"], plan, runs, meta, d, set(os.listdir(d)))
    sha, dirty, pnotes = report.run_state(d, repo)
    if pnotes and cases:
        cases[0]["note"] = (cases[0]["note"] or "") + "; provenance: " + "; ".join(pnotes)
    now = report.utc_now()
    doc = report.build_result(sha, dirty, "B", meta.get("host_os") or "macOS (host)", meta.get("guest_os"),
                              meta.get("macfuse") or None, meta.get("ntfs3g") or None, started or now, now, cases)
    errs = report.validate(doc)
    if errs:
        raise RuntimeError("result is invalid: " + "; ".join(errs))
    with open(os.path.join(d, "result.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    for c in cases:
        print("%s %s: %s" % (c["id"], c["status"].upper(), c["note"][:400]))
    return 0 if all(c["status"] == "pass" for c in cases) else 1


def selftest():
    import shutil
    import tempfile
    ok, txt = rss_synthetic()
    assert ok, txt
    tmp = tempfile.mkdtemp(prefix="mixres.")
    try:
        def mk(kind, summary, run, rss=None, remount=None, ctl=None):
            d = os.path.join(tmp, "x%d" % mk.n)
            mk.n += 1
            nm = kind + "-real"
            os.makedirs(os.path.join(d, nm))
            with open(os.path.join(d, nm, "summary.json"), "w") as f:
                json.dump(summary, f)
            if rss is not None:
                with open(os.path.join(d, nm, "rss.tsv"), "w") as f:
                    f.write("".join("%s\t%s\n" % x for x in rss))
            with open(os.path.join(d, nm, "remount.json"), "w") as f:
                json.dump(remount or {"files": 10, "mismatches": 0, "detail": []}, f)
            return d, {nm: dict(run, name=nm, role="real")}
        mk.n = 0
        plan = {"kind": "soak", "duration": "100", "baseline": "20", "verify": "30", "workers": "1", "hang": "60"}
        good_run = {"drc": "0", "vrc": "0", "nf1": "0", "nf2": "0", "mode1": "Mounted rw", "mode2": "Mounted rw"}
        good = {"verdict": "pass", "failures": [], "duration_actual_s": 101.0, "workers": 1, "worker_ok": [True], "mismatches": 0, "errors": 0,
                "lat_max_s": 0.2, "hang": None, "verifies_min_per_worker": 4}
        rss = [(float(t), 1000) for t in range(0, 101, 5)]
        d, runs = mk("soak", good, good_run, rss)
        assert real_problems("soak", plan, runs["soak-real"], d) == [], real_problems("soak", plan, runs["soak-real"], d)
        # each failure condition is seen
        def bad(summary=None, run=None, rss_=None, remount=None, plan_=None, expect=""):
            d, runs = mk("soak", dict(good, **(summary or {})), dict(good_run, **(run or {})), rss_ if rss_ is not None else rss, remount)
            p = real_problems("soak", plan_ or plan, runs["soak-real"], d)
            assert p and any(expect in x for x in p), (expect, p)
        bad({"mismatches": 1}, expect="mismatch")
        bad({"errors": 1}, expect="error")
        bad({"duration_actual_s": 50.0}, expect="planned")
        bad({"lat_max_s": 61.0}, expect="hang")
        bad({"hang": {"worker": 0}}, expect="hang")
        bad({"verifies_min_per_worker": 1}, expect="verification")
        bad({"verdict": "fail"}, expect="verdict")
        bad({"workers": 2, "worker_ok": [True, True]}, expect="worker")
        bad(run={"vrc": "1"}, expect="remount verification")
        bad(remount={"files": 10, "mismatches": 2, "detail": ["x"]}, expect="remount.json")
        bad(run={"nf1": "1"}, expect="ntfsfix")
        bad(run={"mode2": "Mounted ro"}, expect="Mounted rw")
        bad(rss_=[(float(t), 1000 if t < 40 else 1200) for t in range(0, 101, 5)], expect="RSS rule")
        bad(rss_=[(float(t), 1000) for t in range(0, 31, 5)], expect="RSS rule")  # too short to apply the rule
        bad(plan_=dict(plan, baseline="100"), expect="baseline")
        assert real_problems("soak", plan, None, tmp)
        # the case as a whole: guest check failures, an injected fault and a shortened run
        d, runs = mk("soak", good, good_run, rss)
        meta = {"guest_rc": "0", "setup_failed": "0", "checks_failed": "0"}
        c = build_cases("soak", plan, runs, meta, d, set())
        assert c[0]["status"] == "pass" and c[0]["note"].startswith("shortened run, not criterion 5.2"), c
        assert c[0]["id"] == "S-SOAK-01-SHORT" and "parameters: duration 100 s" in c[0]["note"], c
        c = build_cases("soak", plan, runs, dict(meta, checks_failed="2"), d, set())
        assert c[0]["status"] == "fail" and "guest check" in c[0]["note"] and c[-1]["id"] == "S-SOAK-RUN-SHORT", c
        c = build_cases("soak", dict(plan, fault="corrupt"), runs, meta, d, set())
        assert c[0]["status"] == "fail" and "injected fault" in c[0]["note"], c
        assert not is_criterion("soak", plan)
        crit = dict(plan, duration="21600", baseline="3600", controls="corrupt", fault="")
        assert is_criterion("soak", crit)
        # a criterion run writes the plain ids; the same run without its control or with a fault never does (M5-F04, M5-F10)
        cd0, cruns = mk("soak", dict(good, duration_actual_s=21601.0, verifies_min_per_worker=36), good_run, [(float(t), 1000) for t in range(0, 21601, 30)])
        cids = [x["id"] for x in build_cases("soak", crit, cruns, meta, cd0, set())]
        assert cids == ["S-SOAK-01", "S-SOAK-01-CONTROL"], cids
        for bad_plan in (dict(crit, controls=""), dict(crit, fault="corrupt"), dict(crit, duration="21599")):
            ids = [x["id"] for x in build_cases("soak", bad_plan, cruns, meta, cd0, set())]
            assert ids[0] == "S-SOAK-01-SHORT" and all(i.endswith("-SHORT") for i in ids), (bad_plan, ids)
            assert not is_criterion("soak", bad_plan)
        assert not is_criterion("soak", dict(crit, controls=" "))
        assert not is_criterion("par", {"duration": "1800", "workers": "16", "hang": "60", "baseline": "0", "controls": "corrupt"})
        assert is_criterion("par", {"duration": "1800", "workers": "16", "hang": "60", "baseline": "0", "controls": "stall corrupt"})
        assert not is_criterion("soak", dict(plan, duration="21600", baseline="3000"))
        assert not is_criterion("par", {"duration": "1800", "workers": "8", "hang": "60"})
        assert not is_criterion("par", {"duration": "1800", "workers": "16", "hang": "120"})
        assert not is_criterion("par", {"duration": "1800", "workers": "16", "hang": "60", "baseline": "0"})
        # the controls: caught only when the right and only the right failure shows
        cd = os.path.join(tmp, "c")
        os.makedirs(os.path.join(cd, "par-ctl-corrupt"))
        os.makedirs(os.path.join(cd, "par-ctl-stall"))
        for nm, sm in (("par-ctl-corrupt", {"failures": [{"kind": "mismatch", "detail": "x"}], "mismatches": 1}),
                       ("par-ctl-stall", {"failures": [{"kind": "hang", "detail": "x"}], "hang": {"worker": 1}})):
            with open(os.path.join(cd, nm, "summary.json"), "w") as f:
                json.dump(sm, f)
        with open(os.path.join(cd, "par-ctl-corrupt", "remount.json"), "w") as f:
            json.dump({"mismatches": 1}, f)
        runs = {"par-ctl-corrupt": {"drc": "1", "vrc": "1"}, "par-ctl-stall": {"drc": "1", "vrc": "NA"}}
        pl = {"controls": "corrupt stall"}
        assert all(c[1] for c in control_checks("par", pl, runs, cd)), control_checks("par", pl, runs, cd)
        runs["par-ctl-stall"]["drc"] = "0"
        assert not control_checks("par", pl, runs, cd)[1][1]
        runs["par-ctl-stall"]["drc"] = "1"
        runs["par-ctl-corrupt"]["vrc"] = "0"  # the remount verification did not see the corruption
        assert not control_checks("par", pl, runs, cd)[0][1]
        runs["par-ctl-corrupt"]["vrc"] = "1"
        with open(os.path.join(cd, "par-ctl-corrupt", "summary.json"), "w") as f:
            json.dump({"failures": [{"kind": "mismatch", "detail": "x"}, {"kind": "error", "detail": "y"}], "mismatches": 1}, f)  # also an unrelated failure
        assert not control_checks("par", pl, runs, cd)[0][1]
        del runs["par-ctl-stall"]
        assert not control_checks("par", pl, runs, cd)[1][1]
    finally:
        shutil.rmtree(tmp, ignore_errors=True)
    print("mix_result selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    f = sp.add_parser("finalize")
    f.add_argument("--dir", required=True)
    f.add_argument("--repo", required=True)
    f.add_argument("--started")
    sp.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "selftest":
        return selftest()
    return finalize(a.dir, a.repo, a.started)


if __name__ == "__main__":
    sys.exit(main())
