# SPDX-License-Identifier: GPL-2.0-or-later
"""Forgive known leaks of one short fuzz run (ci-local.sh stage fuzz-short). Python standard library only.

Usage: python3 -I -B shortcheck.py DIR [--known known.tsv]

DIR is the extracted result of one target of tests/fuzz/image/inner.sh or tests/fuzz/sanitize/inner.sh
(summary.txt, artifacts/, leaks.log.sym). The only thing forgiven is the leak report of the corpus replay: each
distinct leak is turned into a signature with fuzzsig.py (the frame text is the one of the campaign: the log went
through symbolize.py in the container) and classified with triage.classify against known.tsv, the code of the
8 CPU-hour campaign:

  known       a row of known.tsv with status open or benign, or fixed on a pristine engine (the short stage always
              builds the patched engine, so there a fixed row is a regression)
  REGRESSION  a row with status fixed: a project fix, or an engine patch on a patched engine
  NEW         no row matches

Everything else fails, whatever its signature: any file under artifacts/ (crash, timeout, out of memory, slow
unit), any libFuzzer run of the summary ("run N: exit RC") with RC not 0, a summary with a RESULT line that is not "RESULT: FINDING" (or none), a
summary without the line "replay complete" (every corpus input leak-checked, see inner.sh: one libFuzzer process over
the corpus stops at the first leak, which would let a known leak hide a new one), a replay that counted no input or no
leaking input or a failed run without a leak report, an AddressSanitizer or libFuzzer error or a deadly signal in
leaks.log.sym (a leak report has none of these), a leak count that differs from the leaks parsed (summary line, "Direct leak" and "Indirect leak" lines of the log,
signatures without frames), an empty or missing leaks.log.sym with leaks counted, and a failed run without leaks.
Exit 0: the run failed only on known leaks (the last output line is "short-check: known: F2, F5"); 1: anything
else (the last line starts with "short-check: FINDING"); 2: usage error. The sanitize targets have no leak replay
(project code, every finding is NEW), so for them this is always exit 1.
"""
import argparse
import os
import re
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import fuzzsig  # noqa: E402
import triage  # noqa: E402


def natural(i):
    return [int(x) if x.isdigit() else x for x in re.split(r"(\d+)", i)]


def problems(d, kv):
    """Reasons the run cannot be a leak-only finding."""
    out = []
    adir = os.path.join(d, "artifacts")
    if os.path.isdir(adir) and os.listdir(adir):
        out.append("artifacts: %s" % " ".join(sorted(os.listdir(adir))))
    for ln in open(os.path.join(d, "summary.txt"), errors="replace"):
        m = re.match(r"^\s*run (\d+): exit (\d+)", ln)
        if m and m.group(2) != "0":
            out.append("libFuzzer run %s exited %s" % (m.group(1), m.group(2)))
        m = re.match(r"^libFuzzer exit: (\d+)", ln)
        if m and m.group(1) != "0":
            out.append("libFuzzer exited %s" % m.group(1))
    summary = open(os.path.join(d, "summary.txt"), errors="replace").read()
    if not re.search(r"^replay complete$", summary, re.M):
        out.append("replay incomplete: no 'replay complete' line in summary.txt")
    m = re.search(r"^leak replay: inputs (\d+), leaking (\d+), failed without a leak report (\d+)$", summary, re.M)
    if not m or int(m.group(1)) == 0 or int(m.group(2)) == 0:
        out.append("no leak replay line, or no input or no leaking input in it")
    elif len(re.findall(r"^=== replay input ", open(os.path.join(d, "leaks.log.sym"), errors="replace").read()
                        if os.path.isfile(os.path.join(d, "leaks.log.sym")) else "", re.M)) != int(m.group(2)):
        out.append("summary says %s leaking input(s), leaks.log.sym has a different number of '=== replay input' blocks" % m.group(2))
    elif int(m.group(3)) != 0:
        out.append("%s replay run(s) failed without a leak report" % m.group(3))
    results = re.findall(r"^RESULT: (.*)$", summary, re.M)
    if not results or any(not r.startswith("FINDING") for r in results):
        out.append("RESULT lines of summary.txt: %s" % (results or "none"))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("dir")
    ap.add_argument("--known", default=os.path.join(os.path.dirname(os.path.abspath(__file__)), "known.tsv"))
    a = ap.parse_args()
    d = a.dir
    if not os.path.isdir(d):
        print("shortcheck.py: no directory %s" % d, file=sys.stderr)
        return 2
    if not os.path.isfile(os.path.join(d, "summary.txt")):
        print("short-check: FINDING (no summary.txt)")
        return 1
    kv = triage.read_kv(os.path.join(d, "summary.txt"))
    bad = problems(d, kv)
    counted = re.match(r"^(\d+)", kv.get("leaks in the corpus replay", ""))
    n = int(counted.group(1)) if counted else 0
    log = os.path.join(d, "leaks.log.sym")
    text = open(log, errors="replace").read() if os.path.isfile(log) else ""
    direct = len(re.findall(r"^Direct leak of", text, re.M))
    lines = len(re.findall(r"^(Direct|Indirect) leak of", text, re.M))
    for ln in text.splitlines():
        if re.search(r"ERROR: AddressSanitizer|ERROR: libFuzzer|deadly signal", ln):
            bad.append("error in leaks.log.sym: %s" % ln.strip()[:80])
            break
    sigs = [s for s, _ in fuzzsig.leak_signatures(log)] if text else []
    if n == 0:
        bad.append("the run failed and the summary counts no leak")
    elif lines != n or direct == 0 or not sigs:
        bad.append("summary counts %d leak(s), leaks.log.sym has %d leak line(s), %d direct, %d signature(s)" %
                   (n, lines, direct, len(sigs)))
    known = fuzzsig.load_known(a.known)
    patched = kv.get("engine-patches", "series") == "series"
    ids = []
    for s in sigs:
        f = s.split("\t")
        if f[5] == "-":
            print("  NEW  leak without frames")
            bad.append("leak without frames")
            continue
        status, r = triage.classify(known, f[0], f[1], f[5], patched)
        print("  %s%s  leak: %s" % (status, " " + r["id"] if r else "", f[0]))
        if status == "known":
            if r["id"] not in ids:
                ids.append(r["id"])
        else:
            bad.append("%s leak %s" % (status, f[0]))
    if bad:
        print("short-check: FINDING (%s)" % "; ".join(bad))
        return 1
    print("short-check: known: %s" % ", ".join(sorted(ids, key=natural)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
