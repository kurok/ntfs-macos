#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""inputs.py - put the pre-built inputs of a run (deps tar, fixtures) into its result cases (audit M5-F07).

scripts/run-inputs.sh record writes OUT/inputs.txt (key=value lines: deps_tar, deps_tar_sha256, deps_tar_key,
fixtures_key, fixtures_manifest_sha256, fixture.NAME=...). A result builder calls annotate(cases, OUT) before it
builds the result file: every case then carries a short "inputs: ..." sentence in its note (before a trailing
"; data=" block, which the performance report parses) and inputs.txt in its evidence.

Usage: inputs.py selftest
"""
import os
import sys

NAME = "inputs.txt"


def read(out_dir):
    d = {}
    try:
        with open(os.path.join(out_dir, NAME), "r", encoding="utf-8") as f:
            for line in f:
                k, sep, v = line.rstrip("\n").partition("=")
                if sep and k.strip():
                    d[k.strip()] = v.strip()
    except OSError:
        pass
    return d


def note(d):
    """one sentence, "" when nothing was recorded"""
    if not d:
        return ""
    parts = []
    if str(d.get("deps_tar", "")).startswith("n/a"):
        pass  # no guest (perf in Environment A): nothing to say about a deps tar
    elif d.get("deps_tar_sha256"):
        seed = "seed only, the build re-runs; " if "seed only" in d.get("deps_tar", "") else ""
        parts.append("deps tar %ssha256 %s key %s" % (seed, d["deps_tar_sha256"][:12], (d.get("deps_tar_key") or "?")[:12]))
    elif d.get("deps_tar"):
        parts.append("deps tar %s (NTFS-3G built in the guest from this checkout)" % d["deps_tar"])
    if d.get("fixtures_manifest_sha256"):
        dirty = " (dirty tree: keyed on the file contents)" if d.get("fixtures_dirty") == "1" else ""
        parts.append("fixtures key %s%s manifest sha256 %s" % ((d.get("fixtures_key") or "?")[:12], dirty, d["fixtures_manifest_sha256"][:12]))
    return ("inputs: " + "; ".join(parts)) if parts else ""


def annotate(cases, out_dir):
    d = read(out_dir)
    txt = note(d)
    if not txt:
        return cases
    for c in cases:
        n = c.get("note") or ""
        head, sep, tail = n.partition("; data=")
        c["note"] = (head + "; " if head else "") + txt + (sep + tail if sep else "")
        ev = c.setdefault("evidence", [])
        if NAME not in ev:
            ev.append(NAME)
    return cases


def selftest():
    import tempfile
    with tempfile.TemporaryDirectory() as t:
        cases = [{"id": "X-1", "status": "pass", "note": "ok", "evidence": ["a"]},
                 {"id": "X-2", "status": "pass", "note": "run; data={\"a\":1}", "evidence": []}]
        assert annotate([dict(c) for c in cases], t) == cases, "no inputs.txt: cases unchanged"
        with open(os.path.join(t, NAME), "w") as f:
            f.write("deps_tar=d.tar\ndeps_tar_sha256=%s\ndeps_tar_key=%s\nfixtures_key=%s\nfixtures_manifest_sha256=%s\n"
                    % ("a" * 64, "b" * 64, "c" * 64, "d" * 64))
        out = annotate([dict(c, evidence=list(c["evidence"])) for c in cases], t)
        assert out[0]["note"] == "ok; inputs: deps tar sha256 aaaaaaaaaaaa key bbbbbbbbbbbb; fixtures key cccccccccccc " \
                                 "manifest sha256 dddddddddddd", out[0]["note"]
        assert out[0]["evidence"] == ["a", NAME] and out[1]["evidence"] == [NAME]
        assert out[1]["note"].startswith("run; inputs: ") and out[1]["note"].endswith("; data={\"a\":1}"), out[1]["note"]
        with open(os.path.join(t, NAME), "w") as f:
            f.write("fixtures_key=%s\nfixtures_dirty=1\nfixtures_manifest_sha256=%s\n" % ("c" * 64, "d" * 64))
        assert "(dirty tree" in annotate([{"id": "X", "note": "n"}], t)[0]["note"]
        with open(os.path.join(t, NAME), "w") as f:
            f.write("deps_tar=none\n")
        assert "built in the guest" in annotate([{"id": "X", "note": "n"}], t)[0]["note"]
        with open(os.path.join(t, NAME), "w") as f:
            f.write("deps_tar=n/a (no guest in this environment)\n")
        assert annotate([{"id": "X", "note": "n"}], t)[0]["note"] == "n", "env A of perf: no deps sentence"
        with open(os.path.join(t, NAME), "w") as f:
            f.write("deps_tar=s.tar (seed only: the build re-runs)\ndeps_tar_sha256=%s\ndeps_tar_key=none\n" % ("a" * 64))
        assert "seed only, the build re-runs; sha256 aaaaaaaaaaaa" in annotate([{"id": "X", "note": "n"}], t)[0]["note"]
    print("inputs.py selftest ok")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        sys.exit(selftest())
    sys.stderr.write(__doc__)
    sys.exit(2)
