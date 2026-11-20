#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""result.py - turn the output of kill_guest.sh into a result document (ntfsmac-test-result/1), S-KILL-*.

Usage: result.py --log FILE --out DIR --reps N [--driver-rc N] [--repo DIR] [--targets "worker daemon module detach"]
                 [--started TS] [--no-control]
       result.py selftest

Cases: S-KILL-01 worker, S-KILL-02 daemon, S-KILL-03 macFUSE module process, S-KILL-04 force-detach of the image
(each: N repetitions, every one must pass, none may hang), S-KILL-05 the controls (injected damage must be
reported by the checks, a never-ending step must fail after its deadline). A target that did not finish (the guest
died, a host deadline) is a fail, never a missing case.
Exit: 0 every case passed, 1 a case failed, 2 usage or setup error.
"""
import argparse
import json
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "tests", "harness"))

ORDER = [("worker", "S-KILL-01", "SIGKILL of the worker"),
         ("daemon", "S-KILL-02", "SIGKILL of the daemon (launchd restart)"),
         ("module", "S-KILL-03", "SIGKILL of the macFUSE module process that serves the mount"),
         ("detach", "S-KILL-04", "force-detach (yank) of the image during a write")]
CONTROLS = ("inject-journal", "inject-image", "inject-flags", "hang")


def parse(text):
    """Split the guest output into per-target sections and facts."""
    d = {"meta": {}, "reps": {}, "targets": {}, "controls": {}, "sections": {}, "finished": False, "fails": {}}
    cur = None
    for line in text.splitlines():
        m = re.match(r"^META (\w+)=(.*)$", line)
        if m:
            d["meta"][m.group(1)] = m.group(2).strip()
        m = re.match(r"^== target (\w+): (\d+) repetition", line)
        if m:
            cur = m.group(1)
            d["sections"][cur] = []
        if cur is not None:
            d["sections"][cur].append(line)
        m = re.match(r"^REP (\w+) (\d+) (PASS|FAIL) (\d+)$", line)
        if m:
            d["reps"].setdefault(m.group(1), []).append((int(m.group(2)), m.group(3), int(m.group(4))))
        m = re.match(r"^TARGET (\w+) reps=(\d+) failed=(\d+)$", line)
        if m:
            d["targets"][m.group(1)] = (int(m.group(2)), int(m.group(3)))
            cur = None
        m = re.match(r"^CONTROL (\S+) (PASS|FAIL) ?(.*)$", line)
        if m:
            d["controls"][m.group(1)] = (m.group(2), m.group(3))
        m = re.match(r"^MEASURE (\S+) mmap=(\S+) plain=(\S+) clean_report=(\S+)$", line)
        if m:
            d.setdefault("measure", []).append({"rep": m.group(1), "mmap": m.group(2), "plain": m.group(3),
                                                "clean_report": m.group(4)})
        m = re.match(r"^RUN (PASS|FAIL) ?(.*)$", line)
        if m:
            d["run"] = (m.group(1), m.group(2))
        if line.startswith("FAIL: ") and cur:
            d["fails"].setdefault(cur, []).append(line[6:])
        if line.startswith("result: "):
            d["finished"] = True
    return d


def build_cases(d, reps, targets, control, log_name, section_files, driver_rc=0):
    cases = []
    for tgt, cid, what in ORDER:
        if tgt not in targets:
            continue
        done = d["reps"].get(tgt, [])
        secs = sum(r[2] for r in done)
        ev = [log_name] + ([section_files[tgt]] if tgt in section_files else [])
        summ = d["targets"].get(tgt)
        bad = [r for r in done if r[1] != "PASS"]
        if summ is None or len(done) != reps:
            status = "fail"
            note = ("%s: only %d of %d repetitions finished (the guest ended, or a step hung past the host deadline)"
                    % (what, len(done), reps))
        elif bad or summ[1] != 0:
            status = "fail"
            note = "%s: %d of %d repetitions failed (%s)" % (what, len(bad), reps,
                                                            "; ".join(d["fails"].get(tgt, [])[:4]) or "see the log")
        else:
            status = "pass"
            note = ("%s: %d of %d repetitions passed (journal entry kept, next attach read-only with a documented "
                    "reason, no stray worker or mount, data synced before the kill intact, image unchanged by the ro "
                    "session); %d s per repetition at most" % (what, reps, reps, max(r[2] for r in done)))
        if tgt == "module":
            ms = d.get("measure", [])
            if ms:
                agg = {}
                for m in ms:
                    k = "dirty mmap page %s, plain write %s, reported clean: %s" % (m["mmap"], m["plain"], m["clean_report"])
                    agg[k] = agg.get(k, 0) + 1
                note += ("; MEASUREMENT (not a check): " + "; ".join("%s x%d" % kv for kv in sorted(agg.items())))
            else:
                note += "; MEASUREMENT: no result"
        cases.append({"id": cid, "status": status, "duration_s": float(secs), "evidence": ev, "note": note})
    run = d.get("run")
    problems = []
    if not d["finished"]:
        problems.append("the guest output has no final 'result:' line (the guest run did not finish)")
    if driver_rc in (124, 130, 2, 127):
        problems.append("the driver ended with status %d (%s)" % (driver_rc, {124: "host deadline", 130: "interrupted",
                                                                          2: "setup error", 127: "not started"}[driver_rc]))
    if run is None:
        problems.append("the guest printed no RUN line")
    elif run[0] != "PASS":
        problems.append(run[1])
    cases.append({"id": "S-KILL-RUN", "status": "fail" if problems else "pass", "duration_s": 0.0, "evidence": [log_name],
                  "note": ("; ".join(problems)) if problems else "no failure outside the repetitions; the guest run finished"})
    if control:
        got = d["controls"]
        parts, ok = [], True
        for c in CONTROLS:
            st = got.get(c)
            if st is None:
                ok = False
                parts.append("%s: missing" % c)
            else:
                ok = ok and st[0] == "PASS"
                parts.append("%s: %s %s" % (c, st[0], st[1]))
        cases.append({"id": "S-KILL-05", "status": "pass" if ok else "fail", "duration_s": 0.0, "evidence": [log_name],
                      "note": "injected damage is reported by the checks; a never-ending step fails after its deadline. "
                              + "; ".join(parts)})
    return cases


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--log")
    ap.add_argument("--out")
    ap.add_argument("--repo", default=REPO)
    ap.add_argument("--reps", type=int, default=2)
    ap.add_argument("--targets", default="worker daemon module detach")
    ap.add_argument("--started")
    ap.add_argument("--no-control", action="store_true")
    ap.add_argument("--driver-rc", type=int, default=0)
    a = ap.parse_args(argv)
    if not a.log or not a.out:
        ap.error("--log and --out are required")
    import report
    with open(a.log, "r", encoding="utf-8", errors="replace") as f:
        text = f.read()
    d = parse(text)
    os.makedirs(a.out, exist_ok=True)
    log_name = "kill.log"
    if os.path.abspath(a.log) != os.path.abspath(os.path.join(a.out, log_name)):
        with open(os.path.join(a.out, log_name), "w", encoding="utf-8") as f:
            f.write(text)
    section_files = {}
    for t, lines in d["sections"].items():
        fn = "kill-%s.log" % t
        with open(os.path.join(a.out, fn), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        section_files[t] = fn
    targets = a.targets.split()
    cases = build_cases(d, a.reps, targets, not a.no_control, log_name, section_files, a.driver_rc)
    ms = d.get("measure", [])
    with open(os.path.join(a.out, "kill-module-measure.json"), "w", encoding="utf-8") as f:
        json.dump({"unsynced_data_after_module_kill": ms}, f, indent=1)
        f.write("\n")
    for c in cases:
        if c["id"] == "S-KILL-03":
            c["evidence"].append("kill-module-measure.json")
    sha, dirty = report.git_info(a.repo)
    now = report.utc_now()
    doc = report.build_result(sha, dirty, "B", d["meta"].get("host_os") or "macOS (host)", d["meta"].get("guest_os"),
                              d["meta"].get("macfuse") or None, d["meta"].get("ntfs3g") or None, a.started or now, now, cases)
    errs = report.validate(doc)
    if errs:
        raise RuntimeError("result is invalid: " + "; ".join(errs))
    with open(os.path.join(a.out, "result.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    lines = ["%s %s  %s" % (c["id"], c["status"].upper(), c["note"]) for c in cases]
    with open(os.path.join(a.out, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0 if all(c["status"] == "pass" for c in cases) else 1


def selftest():
    good = ("META guest_os=macOS 27.2 arm64\nMETA macfuse=5.5.0\n"
            "== target worker: 2 repetition(s)\nPASS: x\nREP worker 1 PASS 40\nREP worker 2 PASS 41\nTARGET worker reps=2 failed=0\n"
            "== target daemon: 2 repetition(s)\nFAIL: daemon#2: no\nREP daemon 1 PASS 50\nREP daemon 2 FAIL 52\nTARGET daemon reps=2 failed=1\n"
            "== target module: 2 repetition(s)\nMEASURE module#1 mmap=lost plain=survived clean_report=no\nREP module 1 PASS 9\n"
            "RUN PASS none\n"
            "CONTROL inject-journal PASS 1 check(s)\nCONTROL inject-image PASS 1\nCONTROL inject-flags PASS 1\n"
            "CONTROL hang PASS two\nresult: 1 check(s) failed\n")
    d = parse(good)
    cs = {c["id"]: c for c in build_cases(d, 2, ["worker", "daemon", "module"], True, "kill.log", {})}
    assert cs["S-KILL-01"]["status"] == "pass", cs["S-KILL-01"]
    assert cs["S-KILL-02"]["status"] == "fail" and "daemon#2" in cs["S-KILL-02"]["note"], cs["S-KILL-02"]
    assert cs["S-KILL-03"]["status"] == "fail" and "only 1 of 2" in cs["S-KILL-03"]["note"], cs["S-KILL-03"]
    assert "mmap page lost" in cs["S-KILL-03"]["note"], cs["S-KILL-03"]
    assert cs["S-KILL-05"]["status"] == "pass", cs["S-KILL-05"]
    assert cs["S-KILL-RUN"]["status"] == "pass", cs["S-KILL-RUN"]
    d2 = parse(good.replace("CONTROL inject-image PASS 1\n", "CONTROL inject-image FAIL none\n"))
    assert {c["id"]: c for c in build_cases(d2, 2, ["worker"], True, "kill.log", {})}["S-KILL-05"]["status"] == "fail"
    d3 = parse(good.replace("CONTROL hang PASS two\n", ""))
    assert {c["id"]: c for c in build_cases(d3, 2, ["worker"], True, "kill.log", {})}["S-KILL-05"]["status"] == "fail"
    # a repetition count different from the requested one is a fail, not a pass of fewer repetitions
    assert {c["id"]: c for c in build_cases(d, 3, ["worker"], False, "kill.log", {})}["S-KILL-01"]["status"] == "fail"
    # failures outside the repetitions, an unfinished guest and a host deadline fail the run case
    d4 = parse(good.replace("RUN PASS none", "RUN FAIL 2 failure(s) outside the repetitions"))
    assert {c["id"]: c for c in build_cases(d4, 2, ["worker"], False, "kill.log", {})}["S-KILL-RUN"]["status"] == "fail"
    d5 = parse(good.replace("result: 1 check(s) failed\n", ""))
    assert {c["id"]: c for c in build_cases(d5, 2, ["worker"], False, "kill.log", {})}["S-KILL-RUN"]["status"] == "fail"
    assert {c["id"]: c for c in build_cases(d, 2, ["worker"], False, "kill.log", {}, 124)}["S-KILL-RUN"]["status"] == "fail"
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
