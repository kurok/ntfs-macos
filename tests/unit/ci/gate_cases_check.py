#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Every case id of tests/gate/<MILESTONE>.cases must be one that a real emitter can write as `pass`.

Static check over the result builders (tests/stress, tests/harness) and the harness @case declarations:
  - the id must appear in a builder or be declared by a harness @case("ID") (a -CONTROL id is also accepted
    when its base id appears in a file that builds ids with + "-CONTROL");
  - the id must not be one that a builder writes only as a literal `"status": "fail"` (an error-only case such
    as S-SOAK-RUN, which exists only when the run went wrong: a gate that requires it can never pass).
Usage: gate_cases_check.py REPO MILESTONE      (exit 0 = every listed id has a possible passing emitter)
"""
import os
import re
import sys

repo, milestone = sys.argv[1:3]
sources = []
for sub in ("stress", "harness", "integration", "compatibility"):
    for dp, _, fns in os.walk(os.path.join(repo, "tests", sub)):
        for fn in fns:
            if fn.endswith(".py") and not fn.startswith("test_"):
                with open(os.path.join(dp, fn), encoding="utf-8", errors="replace") as f:
                    sources.append(f.read())

with open(os.path.join(repo, "tests", "gate", milestone + ".cases"), encoding="utf-8") as f:
    ids = [t for line in f for t in re.sub(r"#.*", "", line).split()]
if not ids:
    print("gate_cases_check: no id in the list")
    sys.exit(1)

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
    if fail_only:
        print("gate_cases_check: %s is written only as fail by its builder (it can never pass)" % cid)
        bad += 1
    elif not (declared or appears):
        print("gate_cases_check: %s has no emitter in tests/stress or tests/harness" % cid)
        bad += 1
print("gate_cases_check: %d id(s), %d bad" % (len(ids), bad))
sys.exit(1 if bad else 0)
