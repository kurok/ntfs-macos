#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""disc_result.py - result of the disconnect suite (T-M5-04, S-DISC-*), host side.

  disc_result.py finalize --dir OUT --repo REPO --started ISO     OUT/reps.tsv + meta.txt + plan.txt -> OUT/result.json
  disc_result.py selftest

OUT/plan.txt (written by run.sh before the guest starts): reps=N, workloads=LIST, controls=LIST.
OUT/reps.tsv has one line per yank: case, rep, workload, delay, pass|fail, detail (tab separated).
S-DISC-01..04 are the workloads (sequential write, small creates, rename/metadata churn, right after fsync):
a case passes when it has exactly the planned number of reps and every rep passed; fewer reps (a partial
run) FAIL. S-DISC-05 holds the negative controls: real yanks with a fault injected into the image; when
controls were requested it must exist, have one rep per control, and every control must have caught its fault.
S-DISC-RUN fails when the guest script ended abnormally (exit other than 0 or 1) or a setup step failed.
"""
import argparse
import json
import os
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "..", "harness"))
import report  # noqa: E402  (tests/harness/report.py)

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
    """(reps per workload, [workload case ids], number of controls) from plan.txt; None when it is unusable."""
    try:
        n = int(kv["reps"])
        wl = [WORKLOAD_CASE[w] for w in kv["workloads"].split()]
        ctl = len(kv.get("controls", "").split())
    except (KeyError, ValueError):
        return None
    return n, wl, ctl


def build_cases(reps, seconds, have, plan, meta=None):
    """have: set of evidence file names that exist; plan: (reps, [case ids], controls) from parse_plan."""
    meta = meta or {}
    want_n, want_cases, want_ctl = plan
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
        ev = ["reps.tsv", "plan.txt"] + [n for n in ("meta.txt", "guest.log") if n in have]
        for r in ([x for x in rs if x["status"] != "pass"][:20] or rs[:1]):
            n = "rep-%s-%s.log" % (r["case"], r["rep"])
            if n in have and n not in ev:
                ev.append(n)
        cases.append({"id": cid, "status": status, "duration_s": round(seconds * len(rs) / max(len(reps), 1), 3),
                      "evidence": [e for e in ev if e in have], "note": note})
    run_problems = []
    if meta.get("guest_rc") not in ("0", "1"):
        run_problems.append("the guest script ended abnormally (exit %s; 0 or 1 expected)" % meta.get("guest_rc", "unknown"))
    if meta.get("setup_failed") not in (None, "", "0"):
        run_problems.append("%s setup step(s) failed before the first yank" % meta["setup_failed"])
    if run_problems:
        cases.append({"id": "S-DISC-RUN", "status": "fail", "duration_s": 0.0,
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
    sha, dirty = report.git_info(repo)
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
    plan = (2, ["S-DISC-01", "S-DISC-02"], 2)
    full = [row("S-DISC-01", 1, "pass"), row("S-DISC-01", 2, "pass"), row("S-DISC-02", 1, "pass"),
            row("S-DISC-02", 2, "pass"), row("S-DISC-05", 1, "pass"), row("S-DISC-05", 2, "pass")]
    c = build_cases(full, 10, {"reps.tsv", "plan.txt"}, plan, ok_meta)
    assert [x["id"] for x in c] == ["S-DISC-01", "S-DISC-02", "S-DISC-05"] and all(x["status"] == "pass" for x in c), c
    # a partial run fails, also when every rep that ran passed
    c = build_cases(full[:3] + full[4:], 10, {"reps.tsv"}, plan, ok_meta)
    assert c[1]["status"] == "fail" and "1 of 2 planned" in c[1]["note"], c
    # a missing workload fails; a missing control case fails when controls were requested
    c = build_cases(full[:2] + full[4:], 10, {"reps.tsv"}, plan, ok_meta)
    assert [x["status"] for x in c] == ["pass", "fail", "pass"], c
    c = build_cases(full[:4], 10, {"reps.tsv"}, plan, ok_meta)
    assert c[-1]["id"] == "S-DISC-05" and c[-1]["status"] == "fail", c
    # no controls requested: S-DISC-05 is not required
    c = build_cases(full[:4], 10, {"reps.tsv"}, (2, ["S-DISC-01", "S-DISC-02"], 0), ok_meta)
    assert all(x["status"] == "pass" for x in c) and len(c) == 2, c
    # a failed rep, with the mirror_split hint for ntfsfix
    bad = full[:1] + [row("S-DISC-01", 2, "fail", "FAIL: x ntfsfix -n exit status on the yanked image")] + full[2:]
    c = build_cases(bad, 10, {"reps.tsv"}, plan, ok_meta)
    assert c[0]["status"] == "fail" and "rep 2 FAILED" in c[0]["note"] and "mirror_split" in c[0]["note"], c
    # an abnormal guest exit or a setup failure adds a failing run case
    c = build_cases(full, 10, {"reps.tsv"}, plan, {"guest_rc": "142", "setup_failed": "0"})
    assert c[-1]["id"] == "S-DISC-RUN" and c[-1]["status"] == "fail", c
    c = build_cases(full, 10, {"reps.tsv"}, plan, {"guest_rc": "0", "setup_failed": "1"})
    assert c[-1]["id"] == "S-DISC-RUN", c
    assert parse_plan({"reps": "x"}) is None
    assert parse_plan({"reps": "3", "workloads": "seq fsync", "controls": ""}) == (3, ["S-DISC-01", "S-DISC-04"], 0)
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
