#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""disc_result.py - result of the disconnect suite (T-M5-04, S-DISC-*), host side.

  disc_result.py finalize --dir OUT --repo REPO --started ISO     OUT/reps.tsv + meta.txt + plan.txt -> OUT/result.json
  disc_result.py selftest

OUT/plan.txt (written by run.sh before the guest starts): reps=N, workloads=LIST, controls=LIST, fault=KIND (empty: none).
OUT/reps.tsv has one line per yank: case, rep, workload, delay, pass|fail, detail (tab separated).
S-DISC-01..04 are the workloads (sequential write, small creates, rename/metadata churn, right after fsync):
a case passes when it has exactly the planned number of reps and every rep passed; fewer reps (a partial
run) FAIL. S-DISC-05 holds the negative controls: real yanks with a fault injected into the image; when
controls were requested it must exist, have one rep per control, and every control must have caught its fault.
S-DISC-RUN fails when the guest script ended abnormally (exit other than 0 or 1) or a setup step failed.
A fault injected into the real yanks (--fault) fails the workload cases.
Only a run at the criterion parameters writes the plain ids: at least 20 yanks per workload (--full), all four
workloads, both controls, no fault. Every other run writes every id with the suffix -SHORT (S-DISC-01-SHORT, ...,
S-DISC-05-SHORT, S-DISC-RUN-SHORT), so it cannot satisfy a criterion case (M5-F04, M5-F10).
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "harness"))
import report  # noqa: E402  (tests/harness/report.py)
import inputs  # noqa: E402  (tests/harness/inputs.py)

TITLES = {
    "S-DISC-01": "force-detach during a large sequential write",
    "S-DISC-02": "force-detach during many small file creates",
    "S-DISC-03": "force-detach during rename and metadata churn",
    "S-DISC-04": "force-detach right after an fsync of file and directory",
    "S-DISC-05": "negative controls: an injected fault must make the checks fail",
}
WORKLOAD_CASE = {"seq": "S-DISC-01", "small": "S-DISC-02", "churn": "S-DISC-03", "fsync": "S-DISC-04"}
CHECKS = ("volume leaves Mounted, no worker, no macfuse mount, mount point gone, journal entry stays; "
          "ntfsfix -n exit 0 and VOLUME_IS_DIRTY set; next attach ro with the documented reason, "
          "synced files intact by hash, image hash unchanged by the ro session")
UNKNOWN_FAULT = "unknown (no fault= in plan.txt)"  # an absent record: no criterion run, but not an injected fault
CRITERION_REPS = 20  # T-M5-04, --full
CRITERION_CONTROLS = {"hash", "dirty"}
MIRROR_HINT = ("an ntfsfix failure may be a mirror_split point (yank between the two halves of a mirrored "
               "metadata write, TEST_STRATEGY section 6): it needs manual classification and is never accepted silently")


def read_kv(path):
    kv = {}
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
    return kv


def read_reps(d):
    reps = []
    p = os.path.join(d, "reps.tsv")
    if os.path.isfile(p):
        with open(p, "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                c = line.split("\t", 5)
                if len(c) == 6:
                    reps.append({"case": c[0], "rep": c[1], "mode": c[2], "delay": c[3], "status": c[4], "detail": c[5]})
    return reps


def parse_plan(kv):
    """(reps per workload, [workload case ids], number of controls, {"controls": [...], "fault": str}) from plan.txt;
    None when it is unusable."""
    try:
        n = int(kv["reps"])
        wl = [WORKLOAD_CASE[w] for w in kv["workloads"].split()]
        ctl = len(kv.get("controls", "").split())
    except (KeyError, ValueError):
        return None
    return n, wl, ctl, {"controls": kv.get("controls", "").split(), "fault": kv.get("fault", UNKNOWN_FAULT)}


def is_criterion(plan):
    """20 or more yanks per workload (--full), all four workloads, both controls, no injected fault. Only such a
    run writes the plain case ids (M5-F04, M5-F10)"""
    n, wl, _, extra = plan
    return (n >= CRITERION_REPS and set(wl) >= set(WORKLOAD_CASE.values()) and CRITERION_CONTROLS <= set(extra["controls"])
            and extra["fault"] == "")  # a plan.txt without fault= is not a criterion run (fail closed)


def build_cases(reps, seconds, have, plan, meta=None):
    """have: set of evidence file names that exist; plan: (reps, [case ids], controls) from parse_plan."""
    meta = meta or {}
    want_n, want_cases, want_ctl, extra = plan
    crit = is_criterion(plan)
    sfx = "" if crit else "-SHORT"
    params = "parameters: %d yank(s) per workload (criterion: %d), workloads %s, controls '%s', fault '%s'" % (
        want_n, CRITERION_REPS, " ".join(w for w, c in WORKLOAD_CASE.items() if c in want_cases), " ".join(extra["controls"]),
        extra["fault"])
    if sfx:
        params = "shortened or weaker run, not the criterion run (the case id has the suffix -SHORT); " + params
    cases = []
    by = {}
    for r in reps:
        by.setdefault(r["case"], []).append(r)
    for cid in sorted(TITLES):
        is_ctl = cid == "S-DISC-05"
        if is_ctl:
            expected = want_ctl
        else:
            expected = want_n if cid in want_cases else 0
        rs = by.get(cid, [])
        if expected == 0 and not rs:
            continue
        bad = [r for r in rs if r["status"] != "pass"]
        problems = []
        if len(rs) < expected:
            problems.append("only %d of %d planned yank(s) were made (a partial run)" % (len(rs), expected))
        elif len(rs) > expected:
            problems.append("%d yank(s) made, %d planned" % (len(rs), expected))
        if extra["fault"] not in ("", UNKNOWN_FAULT) and not is_ctl:
            problems.append("a fault (%s) was injected into the real yanks on purpose, so it cannot pass" % extra["fault"])
        status = "fail" if (bad or problems) else "pass"
        delays = ",".join(r["delay"] for r in rs)
        if is_ctl:
            note = "%s: %d control(s), %d caught their fault" % (TITLES[cid], len(rs), len(rs) - len(bad))
        else:
            note = "%s: %d of %d planned yank(s) at %s (seconds into the workload; the fsync workload: right after file number), %d passed. Checks per yank: %s" % (
                TITLES[cid], len(rs), expected, delays or "none", len(rs) - len(bad), CHECKS)
        for p in problems:
            note += "; " + p
        for r in bad[:5]:
            note += "; rep %s FAILED: %s" % (r["rep"], r["detail"])
        if any("ntfsfix -n exit status" in r["detail"] for r in bad):
            note += "; " + MIRROR_HINT
        ev = ["reps.tsv", "plan.txt"] + [n for n in ("meta.txt", "guest.log", "provenance.txt") if n in have]
        for r in ([x for x in rs if x["status"] != "pass"][:20] or rs[:1]):
            n = "rep-%s-%s.log" % (r["case"], r["rep"])
            if n in have and n not in ev:
                ev.append(n)
        cases.append({"id": cid + sfx, "status": status, "duration_s": round(seconds * len(rs) / max(len(reps), 1), 3),
                      "evidence": [e for e in ev if e in have], "note": note + "; " + params})
    run_problems = []
    if meta.get("guest_rc") not in ("0", "1"):
        run_problems.append("the guest script ended abnormally (exit %s; 0 or 1 expected)" % meta.get("guest_rc", "unknown"))
    if meta.get("setup_failed") not in (None, "", "0"):
        run_problems.append("%s setup step(s) failed before the first yank" % meta["setup_failed"])
    if run_problems:
        cases.append({"id": "S-DISC-RUN" + sfx, "status": "fail", "duration_s": 0.0,
                      "evidence": [e for e in ("guest.log", "meta.txt") if e in have], "note": "; ".join(run_problems)})
    return cases


def finalize(d, repo, started):
    meta = read_kv(os.path.join(d, "meta.txt"))
    plan = parse_plan(read_kv(os.path.join(d, "plan.txt")))
    if plan is None:
        print("disc_result: plan.txt is missing or unusable in %s" % d, file=sys.stderr)
        return 2
    reps = read_reps(d)
    have = set(os.listdir(d))
    cases = build_cases(reps, float(meta.get("seconds", 0) or 0), have, plan, meta)
    inputs.annotate(cases, d)
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
        print("%s %s: %s" % (c["id"], c["status"].upper(), c["note"][:300]))
    return 0 if cases and all(c["status"] == "pass" for c in cases) else 1


def selftest():
    row = lambda c, n, st, det="": {"case": c, "rep": str(n), "mode": "seq", "delay": "1", "status": st, "detail": det}
    ok_meta = {"guest_rc": "0", "setup_failed": "0"}
    plan = (2, ["S-DISC-01", "S-DISC-02"], 2, {"controls": ["hash", "dirty"], "fault": ""})
    full = [row("S-DISC-01", 1, "pass"), row("S-DISC-01", 2, "pass"), row("S-DISC-02", 1, "pass"),
            row("S-DISC-02", 2, "pass"), row("S-DISC-05", 1, "pass"), row("S-DISC-05", 2, "pass")]
    c = build_cases(full, 10, {"reps.tsv", "plan.txt"}, plan, ok_meta)
    assert [x["id"] for x in c] == ["S-DISC-01-SHORT", "S-DISC-02-SHORT", "S-DISC-05-SHORT"] and all(x["status"] == "pass" for x in c), c
    assert "parameters: 2 yank(s) per workload (criterion: 20), workloads seq small" in c[0]["note"], c
    # a partial run fails, also when every rep that ran passed
    c = build_cases(full[:3] + full[4:], 10, {"reps.tsv"}, plan, ok_meta)
    assert c[1]["status"] == "fail" and "1 of 2 planned" in c[1]["note"], c
    # a missing workload fails; a missing control case fails when controls were requested
    c = build_cases(full[:2] + full[4:], 10, {"reps.tsv"}, plan, ok_meta)
    assert [x["status"] for x in c] == ["pass", "fail", "pass"], c
    c = build_cases(full[:4], 10, {"reps.tsv"}, plan, ok_meta)
    assert c[-1]["id"] == "S-DISC-05-SHORT" and c[-1]["status"] == "fail", c
    # no controls requested: S-DISC-05 is not required
    c = build_cases(full[:4], 10, {"reps.tsv"}, (2, ["S-DISC-01", "S-DISC-02"], 0, {"controls": [], "fault": ""}), ok_meta)
    assert all(x["status"] == "pass" for x in c) and len(c) == 2, c
    # a failed rep, with the mirror_split hint for ntfsfix
    bad = full[:1] + [row("S-DISC-01", 2, "fail", "FAIL: x ntfsfix -n exit status on the yanked image")] + full[2:]
    c = build_cases(bad, 10, {"reps.tsv"}, plan, ok_meta)
    assert c[0]["status"] == "fail" and "rep 2 FAILED" in c[0]["note"] and "mirror_split" in c[0]["note"], c
    # an abnormal guest exit or a setup failure adds a failing run case
    c = build_cases(full, 10, {"reps.tsv"}, plan, {"guest_rc": "142", "setup_failed": "0"})
    assert c[-1]["id"] == "S-DISC-RUN-SHORT" and c[-1]["status"] == "fail", c
    c = build_cases(full, 10, {"reps.tsv"}, plan, {"guest_rc": "0", "setup_failed": "1"})
    assert c[-1]["id"] == "S-DISC-RUN-SHORT", c
    assert parse_plan({"reps": "x"}) is None
    assert parse_plan({"reps": "3", "workloads": "seq fsync", "controls": ""}) == (3, ["S-DISC-01", "S-DISC-04"], 0, {"controls": [], "fault": UNKNOWN_FAULT})
    # M5-F04/F10: the plain ids only at the criterion parameters, with both controls and no fault
    allw = "seq small churn fsync"
    crit_kv = {"reps": "20", "workloads": allw, "controls": "hash dirty", "fault": ""}
    assert is_criterion(parse_plan(crit_kv))
    for ch in ({"reps": "19"}, {"reps": "3"}, {"workloads": "seq small churn"}, {"controls": "hash"}, {"controls": ""},
               {"fault": "hash"}, {"fault": "dirty"}):
        assert not is_criterion(parse_plan(dict(crit_kv, **ch))), ch
    assert is_criterion(parse_plan(dict(crit_kv, reps="25", fault="")))
    nofault = dict(crit_kv)
    del nofault["fault"]  # a plan.txt without the fault= record (an old run, a hand-made plan) is no criterion run
    assert not is_criterion(parse_plan(nofault))
    c = build_cases([row("S-DISC-01", 1, "pass")], 1, {"reps.tsv"}, parse_plan(nofault), ok_meta)
    assert c[0]["id"].endswith("-SHORT") and "unknown (no fault= in plan.txt)" in c[0]["note"] and "None" not in c[0]["note"], c
    cplan = parse_plan(crit_kv)
    allrows = [row(w, n, "pass") for w in ("S-DISC-01", "S-DISC-02", "S-DISC-03", "S-DISC-04") for n in range(1, 21)]
    allrows += [row("S-DISC-05", 1, "pass"), row("S-DISC-05", 2, "pass")]
    c = build_cases(allrows, 10, {"reps.tsv"}, cplan, ok_meta)
    assert [x["id"] for x in c] == ["S-DISC-01", "S-DISC-02", "S-DISC-03", "S-DISC-04", "S-DISC-05"] and all(x["status"] == "pass" for x in c), c
    for ch in ({"controls": ""}, {"fault": "hash"}, {"reps": "19"}):
        c = build_cases(allrows, 10, {"reps.tsv"}, parse_plan(dict(crit_kv, **ch)), ok_meta)
        assert all(x["id"].endswith("-SHORT") for x in c), (ch, [x["id"] for x in c])
    # a fault injected into the real yanks fails the workload cases even when every yank passed
    c = build_cases(allrows, 10, {"reps.tsv"}, parse_plan(dict(crit_kv, fault="hash")), ok_meta)
    assert all(x["status"] == "fail" for x in c if x["id"].startswith("S-DISC-0") and not x["id"].startswith("S-DISC-05")), c
    print("disc_result selftest: ok")
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
