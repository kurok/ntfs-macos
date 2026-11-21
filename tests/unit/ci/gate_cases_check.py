#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every case id of tests/gate/<MILESTONE>.cases must be one that a real emitter can write as `pass`.

Static check over the result builders (tests/stress, tests/harness) and the harness @case declarations:
  - the id must appear in a builder or be declared by a harness @case("ID") (a -CONTROL id is also accepted
    when its base id appears in a file that builds ids with + "-CONTROL");
  - the id must not be one that a builder writes only as a literal `"status": "fail"` (an error-only case such
    as S-SOAK-RUN, which exists only when the run went wrong: a gate that requires it can never pass);
  - a builder of a stress suite or the fuzz campaign must also know the suffix "-SHORT": the plain id is written
    only on the criterion path (M5-F04), so a source that emits a plain id without it is rejected. This is a COARSE
    guard (it only looks for the text "-SHORT" in the builder's source); the real check is each builder's own
    selftest (mix_result.py, classify.py, kill/result.py, disc_result.py, campaign tests), which asserts the ids of
    short and criterion runs.
Usage: gate_cases_check.py REPO MILESTONE      (exit 0 = every listed id has a possible passing emitter)
"""
import os
import re
import sys

repo, milestone = sys.argv[1:3]
sources = []
suite_sources = []  # the builders under tests/stress and tests/fuzz
for sub in ("stress", "harness", "integration", "compatibility", "fuzz"):
    for dp, _, fns in os.walk(os.path.join(repo, "tests", sub)):
        for fn in fns:
            if fn.endswith(".py") and not fn.startswith("test_"):
                with open(os.path.join(dp, fn), encoding="utf-8", errors="replace") as f:
                    sources.append(f.read())
                    if "stress" in dp.split(os.sep) or "fuzz" in dp.split(os.sep):
                        suite_sources.append(sources[-1])

with open(os.path.join(repo, "tests", "gate", milestone + ".cases"), encoding="utf-8") as f:
    ids = [t for line in f for t in re.sub(r"#.*", "", line).split()]
if not ids:
    print("gate_cases_check: no id in the list")
    sys.exit(1)

PARAMETRISED = ("S-SOAK", "S-PAR", "S-LEAK", "S-KILL", "S-DISC", "F-CAMPAIGN")  # ids with criterion parameters
bad = 0
for cid in ids:
    declared = any(re.search(r'@case\(\s*"%s"' % re.escape(cid), s) for s in sources)
    base = re.sub(r"-(CONTROL|RUN)$", "", cid)
    suffix = cid[len(base):]
    toks = cid.split("-")
    prefix = "-".join(toks[:2])
    rest = ["-" + t for t in toks[2:]]  # an id built as PREFIX + "-01" + "-CONTROL" (soak, parallel)
    appears = any(('"%s"' % cid) in s or (suffix and ('"%s"' % base) in s and ('+ "%s"' % suffix) in s) or
                  (rest and ('"%s"' % prefix) in s and all(('+ "%s"' % r) in s for r in rest)) for s in sources)
    fail_only = False
    for s in sources:
        for line in s.splitlines():
            if '"status": "fail",' not in line or '"id":' not in line:
                continue
            if ('"id": "%s"' % cid) in line or (suffix and ('+ "%s"' % suffix) in line and ('"%s"' % base) in s):
                fail_only = True
    emitters = [x for x in suite_sources if ('"%s"' % cid) in x or ('"%s"' % prefix) in x or ('"%s"' % base) in x]
    no_short = prefix in PARAMETRISED and bool(emitters) and not any('-SHORT' in x for x in emitters)
    if no_short:
        print("gate_cases_check: %s: its builder has no -SHORT suffix, so a shortened run could write the plain id" % cid)
        bad += 1
    elif fail_only:
        print("gate_cases_check: %s is written only as fail by its builder (it can never pass)" % cid)
        bad += 1
    elif not (declared or appears):
        print("gate_cases_check: %s has no emitter in tests/stress or tests/harness" % cid)
        bad += 1
print("gate_cases_check: %d id(s), %d bad" % (len(ids), bad))
sys.exit(1 if bad else 0)
