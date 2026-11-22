# SPDX-License-Identifier: GPL-2.0-or-later
"""Merge the units of a fuzz campaign into one triage table (T-M5-09). Python standard library only.

Usage: python3 -I -B triage.py --results DIR --known known.tsv [--findings-dir DIR] [--adopt]
                               [--plan "t:s ..."] [--budget S] [--result-out FILE [--allow-dirty]]

Reads DIR/units/*/{summary.txt,findings.tsv,ubsan.tsv,inputs/} (written by inner.sh), merges the findings of
all units by signature, matches each against known.tsv and classifies it. Writes DIR/triage.tsv and
DIR/triage.md. With --adopt, the smallest input of each NEW finding is copied to --findings-dir as
N-<hash>-<target>.bin (a regression input; never overwrites). Exit 0: nothing new and no regression; 1: a new finding or UBSan location, a
regression or a crash that does not replay; 2: usage error.

Statuses
  known      matches a row of known.tsv (status open or benign, or fixed on a pristine engine: expected)
  REGRESSION matches a "fixed" row but the unit's engine carries the patch
  NEW        no row matches: a finding to classify by hand (the automatic class is a first guess)
  REVIEW     an artifact that did not reproduce on replay (crash, timeout, oom): needs a human
  info       a slow unit that does not fail on replay

Classes (docs/SECURITY_FINDINGS.md section 2)
  W1 memory corruption: out-of-bounds write, use after free, double or invalid free   exploitable: possible
  R1 out-of-bounds read of heap, stack or global memory                               exploitable: information leak or crash
  R2 uncontrolled resource use: allocation size, memory limit, time                   exploitable: denial of service
  C  crash without memory corruption (null dereference, abort)                        exploitable: denial of service
  O  oracle failure in project code (logic error found by a harness oracle)           exploitable: decided by hand
  L  memory leak                                                                      not exploitable (bounded per process)
  U  undefined behaviour without a sanitizer-detected memory error                    not exploitable as built
  S  slow unit that does not fail on replay                                           not exploitable
  X  anything else                                                                    decided by hand
"""
import argparse
import glob
import hashlib
import os
import re
import shutil
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzsig  # noqa: E402

CLASS_TEXT = {
    "W1": "possible (memory corruption)",
    "R1": "information leak or crash",
    "R2": "denial of service",
    "C": "denial of service",
    "O": "decided by hand",
    "L": "no (bounded per process)",
    "U": "no (as built)",
    "S": "no",
    "X": "decided by hand",
}


def auto_class(kind, access):
    if kind.startswith("asan:"):
        t = kind[5:]
        if t in ("use-after-free", "double-free", "attempting-double-free", "bad-free", "attempting-free-on-address-which-was-not-malloced",
                 "stack-use-after-return", "stack-use-after-scope", "heap-use-after-free", "container-overflow"):
            return "W1"
        if t.endswith("buffer-overflow") or t.endswith("buffer-underflow"):
            return "W1" if access == "WRITE" else "R1"
        if t in ("allocation-size-too-big", "requested-allocation-size-exceeds-maximum-supported-size", "stack-overflow"):
            return "R2"
        if t in ("SEGV", "FPE", "ABRT"):
            return "C"
        return "X"
    return {"leak": "L", "ubsan": "U", "oracle": "O", "timeout": "R2", "oom": "R2", "crash": "C", "slow": "S"}.get(kind, "X")


def classify(known, key, kind, frames, patched):
    """(status, known row or None) of one signature: known, REGRESSION or NEW. Shared with shortcheck.py (the short
    fuzz stage of ci-local.sh), so that the campaign and the short stage never disagree."""
    r = fuzzsig.match_known(known, key, kind, frames)
    if not r:
        return "NEW", None
    if r["status"] == "fixed":
        # a fix that does not hold: project fixes always, engine patches on a patched engine;
        # on a pristine engine the finding is the expected result
        return ("REGRESSION" if (r["component"] == "project" or patched) else "known"), r
    return "known", r


def read_kv(path):
    d = {}
    if os.path.exists(path):
        for ln in open(path, errors="replace"):
            m = re.match(r"^([a-zA-Z -]+): (.*)$", ln)
            if m:
                d[m.group(1)] = m.group(2)
    return d


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--results", required=True)
    ap.add_argument("--known", required=True)
    ap.add_argument("--findings-dir")
    ap.add_argument("--adopt", action="store_true")
    ap.add_argument("--plan", default="", help="planned units, 'target:shard' separated by spaces; each needs a summary")
    ap.add_argument("--budget", type=int, default=0, help="CPU seconds planned; the fuzz CPU time must reach 95 percent")
    ap.add_argument("--problem", action="append", default=[], help="a problem found by the caller (repeatable): fails the run and the case")
    ap.add_argument("--result-out", help="write an ntfsmac-test-result/1 file (case F-CAMPAIGN-01, campaign_result.py); needs "
                    "DIR/provenance.txt (written by run.sh); refuses a dirty tree unless --allow-dirty")
    ap.add_argument("--repo", default=os.path.dirname(os.path.dirname(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))),
                    help="repository whose state is checked for --result-out (default: this one)")
    ap.add_argument("--allow-dirty", action="store_true", help="with --result-out: accept a campaign started on a dirty tree (the result is dirty)")
    a = ap.parse_args()
    known = fuzzsig.load_known(a.known)
    units = sorted(glob.glob(os.path.join(a.results, "units", "*", "summary.txt")))
    if not units:
        print("triage.py: no units under %s" % a.results, file=sys.stderr)
        return 2

    problems = list(a.problem)
    for u in a.plan.split():
        t, _, sh = u.partition(":")
        sp = os.path.join(a.results, "units", "%s-s%s" % (t, sh), "summary.txt")
        if not os.path.exists(sp):
            problems.append("planned unit %s has no summary (it did not run to the end)" % u)
    stats = []
    merged = {}  # key -> dict
    ubsan = {}
    for sp in units:
        d = os.path.dirname(sp)
        unit = os.path.basename(d)
        kv = read_kv(sp)
        target = unit.rsplit("-s", 1)[0]
        patched = kv.get("engine-patches", "series") == "series"
        last = kv.get("last progress line", "")
        cov = re.search(r"cov: (\d+)", last)
        stats.append({
            "unit": unit, "target": target, "cpu": kv.get("fuzz CPU seconds", "?"),
            "execs": kv.get("executions in all runs", "?"), "corpus": kv.get("corpus after the run", "?").split(" ")[0],
            "edges": cov.group(1) if cov else "?", "patched": "yes" if patched else "no",
            "result": kv.get("RESULT", "?"), "sec": kv.get("seconds", ""),
        })
        if "RESULT" not in kv:
            problems.append("unit %s has no RESULT line (it did not finish)" % unit)
        fp = os.path.join(d, "findings.tsv")
        if os.path.exists(fp):
            for ln in open(fp, errors="replace"):
                f = ln.rstrip("\n").split("\t")
                if len(f) < 11:
                    continue
                uid, key, kind, access, comp, caller, frames, cnt, osize, msize, note = f[:11]
                e = merged.setdefault(key, {"key": key, "kind": kind, "access": access, "comp": comp, "caller": caller,
                                            "frames": frames, "units": [], "count": 0, "best": None, "targets": set(),
                                            "patched": False, "unpatched": False})
                e["units"].append(unit)
                e["targets"].add(target)
                e["count"] += int(cnt) if cnt.isdigit() else 1
                if patched:
                    e["patched"] = True
                else:
                    e["unpatched"] = True
                inp = os.path.join(d, "inputs", uid + ".bin")
                if os.path.exists(inp):
                    size = os.path.getsize(inp)
                    if e["best"] is None or size < e["best"][1]:
                        e["best"] = (inp, size, target, note)
        up = os.path.join(d, "ubsan.tsv")
        if os.path.exists(up):
            for ln in open(up, errors="replace"):
                n, _, text = ln.rstrip("\n").partition("\t")
                u = ubsan.setdefault(text, {"count": 0, "units": set()})
                u["count"] += int(n) if n.isdigit() else 1
                u["units"].add(unit)

    rows = []
    new_n = 0
    for key in sorted(merged):
        e = merged[key]
        kind = e["kind"]
        cls = auto_class(kind, e["access"])
        status = None
        kid = "-"
        fixed_by = "-"
        ref = ""
        if key.startswith("not-reproducible"):
            if "slow-unit" in key:
                status, cls = "info", "S"
            else:
                status, cls = "REVIEW", "X"
        else:
            status, r = classify(known, key, kind, e["frames"], e["patched"])
            if r:
                kid = r["id"]
                fixed_by = r["fixed_by"]
                ref = r["ref"]
                cls = r["klass"]
        rows.append({"e": e, "status": status, "id": kid, "cls": cls, "fixed_by": fixed_by, "ref": ref})
    # ids for new findings: stable, from the signature (rename to M5-NN when the row goes into known.tsv)
    for r in rows:
        if r["status"] in ("NEW", "REVIEW"):
            r["id"] = "N-" + hashlib.sha1(r["e"]["key"].encode()).hexdigest()[:6]
            new_n += 1

    adopted = []
    if a.adopt and a.findings_dir:
        os.makedirs(a.findings_dir, exist_ok=True)
        for r in rows:
            e = r["e"]
            if r["status"] == "NEW" and e["best"]:
                dst = os.path.join(a.findings_dir, "%s-%s.bin" % (r["id"], e["best"][2]))
                if os.path.exists(dst):
                    print("not adopted (exists, never overwritten): %s" % dst)
                    continue
                shutil.copyfile(e["best"][0], dst)
                adopted.append(dst)

    new_ub = []
    ub_rows = []
    for text in sorted(ubsan):
        u = ubsan[text]
        loc = text.split(" ", 1)[0]
        r = fuzzsig.match_known(known, "ubsan " + text, "ubsan", loc)
        st = "known" if r else "NEW"
        if not r:
            new_ub.append(text)
        ub_rows.append((text, u["count"], len(u["units"]), r["id"] if r else "-", st))

    # outputs
    tsv = os.path.join(a.results, "triage.tsv")
    with open(tsv, "w") as f:
        f.write("id\tstatus\tkind\tcomponent\tclass\texploitable\tsignature\tcaller\tcount\tunits\tfixed_by\tinput\n")
        for r in rows:
            e = r["e"]
            inp = os.path.relpath(e["best"][0], a.results) if e["best"] else "-"
            f.write("\t".join([r["id"], r["status"], e["kind"], e["comp"], r["cls"], CLASS_TEXT[r["cls"]], e["key"], e["caller"],
                               str(e["count"]), ",".join(e["units"]), r["fixed_by"], inp]) + "\n")
    md = os.path.join(a.results, "triage.md")
    with open(md, "w") as f:
        f.write("# Fuzz campaign triage\n\n## Units\n\n| Unit | Target | CPU s | Executions | Corpus | Edges (cov) | Engine patched | Result |\n|---|---|---|---|---|---|---|---|\n")
        tot = 0
        for s in stats:
            f.write("| %s | %s | %s | %s | %s | %s | %s | %s |\n" % (s["unit"], s["target"], s["cpu"], s["execs"], s["corpus"], s["edges"],
                                                                    s["patched"], s["result"]))
            tot += int(s["cpu"]) if str(s["cpu"]).isdigit() else 0
        f.write("\nTotal fuzz CPU time: %d s (%.2f h).\n\n## Signatures\n\n" % (tot, tot / 3600.0))
        if rows:
            f.write("| Id | Status | Component | Class | Exploitable | Signature | Count | Units | Input |\n|---|---|---|---|---|---|---|---|---|\n")
            for r in rows:
                e = r["e"]
                inp = os.path.relpath(e["best"][0], a.results) if e["best"] else "-"
                f.write("| %s | %s | %s | %s | %s | `%s` | %d | %s | %s |\n" % (
                    r["id"], r["status"], e["comp"], r["cls"], CLASS_TEXT[r["cls"]], e["key"].replace("|", "/"), e["count"],
                    ",".join(e["units"]), inp))
        else:
            f.write("No crash, oracle failure, timeout, out-of-memory or leak in any unit.\n")
        f.write("\n## UBSan locations in the engine (recoverable, once per location and process)\n\n")
        if ub_rows:
            f.write("| Location and message | Reports | Units | Known | Status |\n|---|---|---|---|---|\n")
            for t, c, nu, kid, st in ub_rows:
                f.write("| `%s` | %d | %d | %s | %s |\n" % (t, c, nu, kid, st))
        else:
            f.write("None.\n")

    with open(md) as f:
        sys.stdout.write(f.read())
    for p in adopted:
        print("adopted: %s" % p)
    tot = sum(int(x["cpu"]) for x in stats if str(x["cpu"]).isdigit())
    if a.budget and tot < 0.95 * a.budget:
        problems.append("fuzz CPU time %d s is below 95 percent of the budget %d s (units stopped early?)" % (tot, a.budget))
    for pr in problems:
        print("PROBLEM: " + pr)
    bad = [r for r in rows if r["status"] in ("NEW", "REGRESSION", "REVIEW")]
    print("triage: %d signature(s), %d new or to review, %d regression(s), %d new UBSan location(s)" % (
        len(rows), sum(1 for r in rows if r["status"] in ("NEW", "REVIEW")), sum(1 for r in rows if r["status"] == "REGRESSION"),
        len(new_ub)))
    if a.result_out:
        import campaign_result
        planned = [u.partition(":")[0] for u in a.plan.split()]
        summary = {
            "budget": a.budget, "cpu": tot, "units": len(stats), "targets": sorted(set(planned) | {x["target"] for x in stats}),
            "patched": all(x["patched"] == "yes" for x in stats),
            "new": sum(1 for r in rows if r["status"] == "NEW"), "review": sum(1 for r in rows if r["status"] == "REVIEW"),
            "regression": sum(1 for r in rows if r["status"] == "REGRESSION"), "new_ubsan": len(new_ub),
            "known": sorted({r["id"] for r in rows if r["status"] == "known" and r["id"] != "-"}), "problems": problems,
        }
        try:
            case = campaign_result.write_result(a.results, summary, a.result_out, a.allow_dirty, a.repo)
        except ValueError as e:
            print("triage.py: no result file: %s" % e, file=sys.stderr)
            return 2
        print("result: %s %s -> %s" % (case["id"], case["status"].upper(), a.result_out))
    return 1 if (bad or new_ub or problems) else 0


if __name__ == "__main__":
    sys.exit(main())
