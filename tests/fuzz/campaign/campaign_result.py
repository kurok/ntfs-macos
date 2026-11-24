# SPDX-License-Identifier: GPL-2.0-or-later
"""The result file of a fuzz campaign (T-M5-09, M5-F05): an ntfsmac-test-result/1 document with the case F-CAMPAIGN-01.

Used by triage.py (`--result-out FILE`); run.sh passes it. Python standard library plus tests/harness/report.py.

F-CAMPAIGN-01 (acceptance of T-M5-09: 8 CPU-hours of fuzzing, every crash classified) is `pass` only when ALL hold:
  - the planned budget is at least CRITERION_BUDGET_S (8 CPU-hours) and the fuzz CPU time reached 95 percent of it,
  - all six targets ran (probe, mountwalk, verdict, label, display, ctl), every planned unit has its summary, and
    the engine of every unit carried the patch series (a --no-patches run is a self-test of the triage, not a campaign),
  - every signature is classified: none is NEW, none needs REVIEW, none is a REGRESSION, no new UBSan location,
    and triage found no other problem,
  - the tree was clean at the start and at the end of the run, and HEAD did not move (provenance.txt and
    provenance-end.txt, written by run.sh): the commit in the result is the commit that was fuzzed. A re-triage
    after the classification was committed (new known.tsv, docs) gets git_sha = HEAD and names the fuzzed commit in
    the note, when the tree is clean now and only classification paths changed since; a code change or an
    uncommitted change makes the result dirty (see provenance()).
Only such a run writes the plain id F-CAMPAIGN-01. A shorter, partial or --no-patches run writes F-CAMPAIGN-01-SHORT
(it records what was run, and can never satisfy the criterion case). A dirty tree is refused unless allow_dirty is
given; the result is then marked dirty_tree true and cannot satisfy a gate.
The result goes into tests/results/ with `python3 tests/harness/report.py adopt --env A --dir DIR` (suite `fuzz`).
"""
import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(HERE, "..", "..", "harness"))
import report  # noqa: E402  (tests/harness/report.py)

CRITERION_BUDGET_S = 28800
CRITERION_TARGETS = ("probe", "mountwalk", "verdict", "label", "display", "ctl")
CASE_ID = "F-CAMPAIGN-01"


def read_provenance(path):
    kv = {}
    if os.path.isfile(path):
        with open(path, "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
    return kv


# Paths that may change between the fuzzed commit and the commit of a re-triage without touching the fuzzed code
# (the classification: known.tsv appended to, new minimised findings, documents, results). The same rule the gate
# applies: defined once in tests/harness/report.py (change_allowed).
def retriage_bad(repo, fuzzed, cur, paths):
    """The paths changed between the fuzzed commit and CUR that the re-triage does not allow."""
    return report.disallowed_changes(
        paths, lambda which, p: report.git_blob(repo, fuzzed if which == "old" else cur, p))


def provenance(results_dir, allow_dirty=False, repo=None):
    """(sha, dirty, started, notes, info) from DIR/provenance.txt (at the start), DIR/provenance-end.txt (after the units) and,
    with repo, the state NOW. Raises ValueError when the commit is unknown, or the tree was dirty at the start without
    allow_dirty. The result is dirty when the tree was dirty at either point, HEAD differs between the two records, or
    the end record is missing. With repo it is also dirty when the tree is dirty now (an uncommitted known.tsv).
    A re-triage after the classification was committed (HEAD now differs from the fuzzed commit) gets git_sha = HEAD
    and a note naming the fuzzed commit, but only when every path changed since the fuzzed commit is allowed by retriage_bad(),
    so the fuzzed code is unchanged; any other change makes the result dirty, with the reason."""
    start = read_provenance(os.path.join(results_dir, "provenance.txt"))
    end = read_provenance(os.path.join(results_dir, "provenance-end.txt"))
    fuzzed = start.get("git_sha", "")
    if not report.SHA_RE.match(fuzzed):
        raise ValueError("provenance.txt in %s has no git_sha: the campaign cannot be tied to a commit "
                         "(run it with tests/fuzz/campaign/run.sh)" % results_dir)
    sha = fuzzed
    notes = []
    info = []
    dirty = start.get("dirty") != "false"
    if dirty and not allow_dirty:
        raise ValueError("the tree was dirty (or its state unknown) when the campaign started: commit first, or pass "
                         "--allow-dirty (the result is then marked dirty and cannot satisfy a gate)")
    if end.get("git_sha") != fuzzed:
        dirty = True
        notes.append("HEAD at the end of the run (%s) is not the commit at the start" % (end.get("git_sha", "")[:12] or "unknown"))
    if end.get("dirty") != "false":
        dirty = True
        notes.append("the tree was dirty or its state unknown at the end of the run")
    if repo is not None:
        try:
            cur, cur_dirty = report.git_info(repo)
        except RuntimeError as e:
            cur, cur_dirty = None, True
            notes.append("cannot read the git state now: %s" % e)
        if cur_dirty:
            dirty = True
            notes.append("the tree is dirty now (uncommitted changes, e.g. known.tsv): commit the classification first")
        if cur is not None and cur != fuzzed:
            out = report._git(repo, "diff", "--name-only", "--no-renames", "-z", fuzzed, cur)
            paths = [p for p in (out or "").split("\0") if p]
            other = retriage_bad(repo, fuzzed, cur, paths)
            if out is None:
                dirty = True
                notes.append("cannot compare the fuzzed commit %s with HEAD" % fuzzed[:12])
            elif other:
                dirty = True
                notes.append("HEAD (%s) differs from the fuzzed commit %s in code, not only in the classification: %s"
                             % (cur[:12], fuzzed[:12], ", ".join(other[:6])))
            else:
                sha = cur
                info.append("re-triage at %s of the campaign fuzzed at %s (only the classification changed)" % (cur[:12], fuzzed))
    started = start.get("started", "")
    if not report.TS_RE.match(started):
        started = report.utc_now()
        notes.append("no start time recorded")
    return sha, dirty, started, notes, info


def is_criterion(s):
    """s: the summary dict of triage (see build_case). True for a run with the criterion budget, all six targets, the
    patched engine and 95 percent of the budget as fuzz CPU time (problems are judged separately)."""
    return (s["budget"] >= CRITERION_BUDGET_S and set(CRITERION_TARGETS) <= set(s["targets"]) and s["patched"]
            and s["cpu"] >= 0.95 * CRITERION_BUDGET_S)


def build_case(s, evidence):
    """The one case. s keys: budget (planned CPU seconds), cpu (fuzz CPU seconds of all units), targets (list),
    units (int), patched (every unit's engine carried the patch series), new, review, regression, new_ubsan (counts),
    known (list of known ids seen), problems (list of strings from triage), extra (list of strings, e.g. provenance)."""
    crit = is_criterion(s)
    why = list(s["problems"]) + list(s.get("extra", []))
    if s["new"] + s["review"] + s["regression"] + s["new_ubsan"]:
        why.append("%d new or to review, %d regression(s), %d new UBSan location(s): not every crash is classified"
                   % (s["new"] + s["review"], s["regression"], s["new_ubsan"]))
    status = "fail" if why else "pass"
    note = ("%d unit(s), targets %s, planned budget %d s (criterion %d s, 8 CPU-hours), fuzz CPU time %d s, "
            "engine patch series %s; known findings seen: %s"
            % (s["units"], " ".join(sorted(s["targets"])) or "none", s["budget"], CRITERION_BUDGET_S, s["cpu"],
               "in every unit" if s["patched"] else "NOT in every unit (--no-patches)", ", ".join(s["known"]) or "none"))
    if not crit:
        note = ("shortened or weaker run, not the criterion campaign (the case id has the suffix -SHORT; needs a budget of "
                "%d s, all six targets, the patched engine): " % CRITERION_BUDGET_S) + note
    for w in why:
        note += "; " + w
    for w in s.get("info", []):
        note += "; " + w
    return {"id": CASE_ID if crit else CASE_ID + "-SHORT", "status": status, "duration_s": float(s["cpu"]),
            "evidence": evidence, "note": note}


def write_result(results_dir, summary, out_path, allow_dirty=False, repo=None):
    """Write the result file for the campaign in results_dir; returns the case. Raises ValueError (nothing written)
    when the commit is unknown or the tree was dirty and allow_dirty is not set."""
    sha, dirty, started, notes, info = provenance(results_dir, allow_dirty, repo)
    s = dict(summary, extra=list(summary.get("extra", [])) + notes, info=info)
    ev = [n for n in ("triage.md", "triage.tsv", "provenance.txt", "provenance-end.txt")
          if os.path.isfile(os.path.join(results_dir, n))]
    udir = os.path.join(results_dir, "units")
    if os.path.isdir(udir):
        for u in sorted(os.listdir(udir)):
            if os.path.isfile(os.path.join(udir, u, "summary.txt")):
                ev.append("units/%s/summary.txt" % u)
    # a re-triage at another commit than the fuzzed one leaves a record that `report.py adopt` checks against provenance.txt
    fuzzed = read_provenance(os.path.join(results_dir, "provenance.txt")).get("git_sha", "")
    rt = os.path.join(results_dir, "retriage.txt")
    if sha != fuzzed:
        with open(rt, "w", encoding="utf-8") as f:
            f.write("git_sha=%s\nfuzzed=%s\n" % (sha, fuzzed))
        ev.append("retriage.txt")
    elif os.path.isfile(rt):
        os.unlink(rt)  # a stale record of an earlier re-triage
    case = build_case(s, ev)
    now = report.utc_now()
    doc = report.build_result(sha, dirty, "A", report._detect_host_os(), None, None, None,
                              started, now if now >= started else started, [case])
    errs = report.validate(doc)
    if errs:
        raise ValueError("result is invalid: " + "; ".join(errs))
    with open(out_path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    return case
