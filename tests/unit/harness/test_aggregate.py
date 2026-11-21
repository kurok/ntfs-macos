# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for result aggregation, the README status table, scripts/test-all.sh and
vm/macos/run-suite.sh (T-M5-01). Nothing here starts a VM, a container or a build: the external
commands are stubs on PATH. Run: python3 -m unittest discover -s tests/unit/harness"""

import glob
import gzip
import json
import os
import re
import shutil
import stat
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
REPO = os.path.dirname(TESTS)
sys.path.insert(0, TESTS)

from harness import report  # noqa: E402

# A read-only checkout in a container has no usable git state. The stubbed runs put a `git` stub on
# PATH (GIT_STUB); nothing here sets variables in os.environ.
GIT_STUB = 'case "$*" in *rev-parse*) echo 1111111111111111111111111111111111111111 ;; esac; exit 0'

SHA = "0123456789abcdef0123456789abcdef01234567"
SHA2 = "fedcba9876543210fedcba9876543210fedcba98"
STARTED = "2026-11-20T10:00:00Z"


def mkcase(cid, status="pass", note=None, evidence=None):
    return {"id": cid, "status": status, "duration_s": 1.0, "evidence": evidence or [], "note": note}


def write_doc(d, name, env, cases, started="2026-11-14T10:00:00Z", dirty=False, sha=SHA):
    os.makedirs(d, exist_ok=True)
    doc = report.build_result(sha, dirty, env, "macOS 27", None, None, None, started, started, cases)
    with open(os.path.join(d, name), "w", encoding="utf-8") as f:
        json.dump(doc, f)
    return doc


def put(path, data):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb" if isinstance(data, bytes) else "w") as f:
        f.write(data)


class AggregateBase(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.work = os.path.join(self.tmp.name, "work")
        self.out = os.path.join(self.tmp.name, "res", "2026-11-20-0123456-B.json")
        put(os.path.join(self.work, "stages.tsv"), "build\tpass\t3\tdone\tlogs/build.log\n")
        put(os.path.join(self.work, "logs", "build.log"), "compiling in %s/x and /Users/someone/y\n" % REPO)

    def agg(self, **kw):
        args = dict(works=[self.work], env="B", repo=REPO, out_path=self.out, sha=SHA, dirty=False, started=STARTED)
        args.update(kw)
        return report.aggregate(**args)

    def evidence_file(self, rel):
        return os.path.join(os.path.dirname(self.out), rel)


class AggregateTest(AggregateBase):
    def setUp(self):
        super().setUp()
        put(os.path.join(self.work, "stages.tsv"),
            "build\tpass\t3\tdone\tlogs/build.log\nstress\tskip\t0\tnone registered\t\ndaemon\tfail\t7\texit 1\t\n")
        put(os.path.join(self.work, "results", "evidence", "sub-B", "I-X-01.log"), "ok /private/tmp/abc\n")
        write_doc(os.path.join(self.work, "results"), "sub-B.json", "B",
                  [mkcase("I-X-01", evidence=["evidence/sub-B/I-X-01.log"]), mkcase("I-X-02", "skip", "no")])

    def test_aggregate_merges_stages_and_cases(self):
        doc = self.agg()
        self.assertEqual(report.validate(doc), [])
        ids = [c["id"] for c in doc["cases"]]
        self.assertEqual(ids, ["STAGE-B-BUILD", "STAGE-B-STRESS", "STAGE-B-DAEMON", "I-X-01", "I-X-02"])
        by = {c["id"]: c for c in doc["cases"]}
        self.assertEqual(by["STAGE-B-DAEMON"]["status"], "fail")
        self.assertEqual(by["STAGE-B-STRESS"]["note"], "none registered")
        text, ok = report.summarize(doc)
        self.assertFalse(ok)
        self.assertIn("STAGE-B-DAEMON", text)

    def test_evidence_copied_and_scrubbed(self):
        doc = self.agg()
        by = {c["id"]: c for c in doc["cases"]}
        for rel in by["STAGE-B-BUILD"]["evidence"] + by["I-X-01"]["evidence"]:
            self.assertTrue(rel.startswith("evidence/2026-11-20-0123456-B/"), rel)
            self.assertTrue(os.path.isfile(self.evidence_file(rel)), rel)
        with open(self.evidence_file(by["STAGE-B-BUILD"]["evidence"][0])) as f:
            log = f.read()
        self.assertNotIn("/Users/", log)
        self.assertIn("<repo>/x", log)
        with open(self.evidence_file(by["I-X-01"]["evidence"][0])) as f:
            self.assertIn("<tmp>/abc", f.read())

    def test_duplicate_case_ids_refused(self):
        write_doc(os.path.join(self.work, "results"), "other.json", "B", [mkcase("I-X-01")])
        with self.assertRaises(ValueError):
            self.agg()

    def test_bad_stage_line_refused(self):
        with open(os.path.join(self.work, "stages.tsv"), "a") as f:
            f.write("oops\tmaybe\t1\t\t\n")
        with self.assertRaises(ValueError):
            self.agg()

    def test_cli_exit_codes(self):
        cli = [sys.executable, "-B", os.path.join(TESTS, "harness", "report.py")]
        r = subprocess.run(cli + ["aggregate", "--env", "B", "--work", self.work, "--out", self.out,
                                  "--git-sha", SHA, "--dirty", "true"], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        self.assertEqual(r.returncode, 1, r.stderr)  # the daemon stage failed
        with open(self.out) as f:
            self.assertTrue(json.load(f)["dirty_tree"])
        self.assertEqual(subprocess.run(cli + ["validate", self.out], stdout=subprocess.PIPE).returncode, 0)
        self.assertEqual(subprocess.run(cli + [self.out], stdout=subprocess.PIPE).returncode, 0)  # old interface


class ProvenanceTest(AggregateBase):
    """Review finding 1: a merged result that is dirty or from another commit taints the aggregate."""

    def test_dirty_sub_result_marks_aggregate_dirty(self):
        write_doc(os.path.join(self.work, "results"), "sub.json", "B", [mkcase("I-X-01")], dirty=True)
        doc = self.agg(dirty=False)
        self.assertTrue(doc["dirty_tree"])
        prov = [c for c in doc["cases"] if c["id"] == "STAGE-B-PROVENANCE"]
        self.assertEqual(len(prov), 1)
        self.assertIn("dirty", prov[0]["note"])

    def test_other_commit_marks_aggregate_dirty(self):
        write_doc(os.path.join(self.work, "results"), "sub.json", "B", [mkcase("I-X-01")], sha=SHA2)
        doc = self.agg(dirty=False)
        self.assertTrue(doc["dirty_tree"])
        note = [c for c in doc["cases"] if c["id"] == "STAGE-B-PROVENANCE"][0]["note"]
        self.assertIn(SHA2[:7], note)

    def cli_aggregate(self, dirty, tag):
        out = os.path.join(os.path.dirname(self.out), "2026-11-20-0123456-B-%s.json" % tag)
        cli = [sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "aggregate", "--env", "B", "--work",
               self.work, "--out", out, "--git-sha", SHA, "--dirty", dirty]
        return subprocess.run(cli, stdout=subprocess.PIPE, stderr=subprocess.PIPE).returncode

    def test_aggregate_exit_status_for_a_dirty_or_foreign_result(self):
        """M5-F12: all stages passed, but the result is dirty or holds a sub-result of another commit: exit 3, not 0."""
        self.assertEqual(self.cli_aggregate("false", "clean"), 0)  # clean, no sub-result
        self.assertEqual(self.cli_aggregate("true", "dirty"), report.EXIT_DIRTY)
        write_doc(os.path.join(self.work, "results"), "sub.json", "B", [mkcase("I-X-01")], sha=SHA2)
        self.assertEqual(self.cli_aggregate("false", "foreign"), report.EXIT_DIRTY)  # sub-result from another commit
        self.assertEqual(report.EXIT_DIRTY, 3)

    def test_clean_same_commit_stays_clean(self):
        write_doc(os.path.join(self.work, "results"), "sub.json", "B", [mkcase("I-X-01")])
        doc = self.agg(dirty=False)
        self.assertFalse(doc["dirty_tree"])
        self.assertFalse([c for c in doc["cases"] if c["id"] == "STAGE-B-PROVENANCE"])


class ScrubTest(AggregateBase):
    """Review finding 4 and the LOW scrub items."""

    def test_non_utf8_log_is_scrubbed_on_bytes(self):
        put(os.path.join(self.work, "logs", "build.log"), b"bad \xff byte /Users/someone/secret\n")
        doc = self.agg()
        with open(self.evidence_file(doc["cases"][0]["evidence"][0]), "rb") as f:
            data = f.read()
        self.assertNotIn(b"/Users/", data)
        self.assertIn(b"\xff", data)
        self.assertIn(b"<home>/secret", data)

    def test_archives_are_not_stored_and_listed(self):
        put(os.path.join(self.work, "artifacts", "build", "keep.txt"), "see /Users/z/q\n")
        put(os.path.join(self.work, "artifacts", "build", "dump.bin"), gzip.compress(b"/Users/z/q"))
        put(os.path.join(self.work, "artifacts", "build", "disk.img"), b"\0" * 16)
        doc = self.agg()
        case = doc["cases"][0]
        self.assertFalse([e for e in case["evidence"] if e.endswith("dump.bin") or e.endswith("disk.img")])
        self.assertIn("dump.bin", case["note"])
        self.assertIn("disk.img", case["note"])
        self.assertTrue([e for e in case["evidence"] if e.endswith("keep.txt")])
        for dp, _dn, fns in os.walk(os.path.dirname(self.out)):
            for fn in fns:
                self.assertNotIn(fn, ("dump.bin", "disk.img"))

    def test_artifact_cap_is_noted(self):
        for i in range(report.MAX_ARTIFACTS_PER_STAGE + 3):
            put(os.path.join(self.work, "artifacts", "build", "f%03d.txt" % i), "x")
        case = self.agg()["cases"][0]
        self.assertIn("capped", case["note"])

    def test_stress_entry_result_cases_are_merged(self):
        """artifacts/<stage>/result.json of a stress entry: its cases appear next to the stage case, with evidence."""
        art = os.path.join(self.work, "artifacts", "stress-leaks")
        put(os.path.join(art, "summary.txt"), "at /Users/z/q\n")
        write_doc(art, "result.json", "B", [mkcase("S-LEAK-01", evidence=["summary.txt", "gone.txt"]),
                                           mkcase("S-LEAK-01-CONTROL")])
        doc = self.agg()
        self.assertEqual(report.validate(doc), [])
        cases = {c["id"]: c for c in doc["cases"]}
        self.assertIn("S-LEAK-01", cases)
        self.assertIn("S-LEAK-01-CONTROL", cases)
        self.assertIn("evidence not stored: gone.txt", cases["S-LEAK-01"]["note"])
        ev = cases["S-LEAK-01"]["evidence"]
        self.assertEqual(len(ev), 1)
        with open(self.evidence_file(ev[0])) as f:
            self.assertNotIn("/Users/", f.read())

    def test_stress_entry_failing_case_is_kept(self):
        art = os.path.join(self.work, "artifacts", "stress-leaks")
        write_doc(art, "result.json", "B", [mkcase("S-LEAK-01", "fail", "project leak")])
        c = [c for c in self.agg()["cases"] if c["id"] == "S-LEAK-01"][0]
        self.assertEqual(c["status"], "fail")

    def test_missing_sub_evidence_is_noted(self):
        write_doc(os.path.join(self.work, "results"), "sub.json", "B",
                  [mkcase("I-X-01", evidence=["evidence/sub/gone.log"])])
        case = [c for c in self.agg()["cases"] if c["id"] == "I-X-01"][0]
        self.assertIn("evidence not stored", case["note"])
        self.assertIn("gone.log", case["note"])

    def test_tmp_and_hostname_scrubbed(self):
        self.assertEqual(report.scrub_text("x /tmp/ntfsmac-out.1/a y"), "x <tmp>/ntfsmac-out.1/a y")
        self.assertEqual(report.scrub_text("at /private/tmp/q"), "at <tmp>/q")
        orig = report._hostnames
        report._hostnames = lambda: ["Testhost-7"]
        try:
            self.assertEqual(report.scrub_text("run on testhost-7 now"), "run on <host> now")
        finally:
            report._hostnames = orig


class AtomicEvidenceTest(AggregateBase):
    """Review finding 5: no evidence directory without a result file."""

    def test_failed_write_leaves_nothing(self):
        with self.assertRaises(ValueError):
            self.agg(started="not-a-timestamp")
        base = os.path.dirname(self.out)
        self.assertEqual([n for n in os.listdir(base)] if os.path.isdir(base) else [], [])

    def test_success_has_json_and_evidence_and_no_temp(self):
        self.agg()
        base = os.path.dirname(self.out)
        self.assertTrue(os.path.isfile(self.out))
        self.assertTrue(os.path.isdir(os.path.join(base, "evidence", "2026-11-20-0123456-B")))
        self.assertEqual(sorted(os.listdir(base)), ["2026-11-20-0123456-B.json", "evidence"])

    def test_existing_result_not_overwritten(self):
        self.agg()
        with self.assertRaises(ValueError):
            self.agg()


class NothingVerifiedTest(AggregateBase):
    """Review finding 2: a run in which nothing passed is not a success."""

    def test_all_skipped_is_not_ok(self):
        put(os.path.join(self.work, "stages.tsv"), "a\tskip\t0\tn\t\nb\tskip\t0\tn\t\n")
        text, ok = report.summarize(self.agg())
        self.assertFalse(ok)
        self.assertIn("nothing verified", text)

    def test_cli_exit_one_when_nothing_passed(self):
        put(os.path.join(self.work, "stages.tsv"), "a\tskip\t0\tn\t\n")
        cli = [sys.executable, "-B", os.path.join(TESTS, "harness", "report.py")]
        r = subprocess.run(cli + ["aggregate", "--env", "B", "--work", self.work, "--out", self.out,
                                  "--git-sha", SHA, "--dirty", "false"], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        self.assertEqual(r.returncode, 1)
        self.assertIn(b"nothing verified", r.stderr)


FEATURES = [
    {"id": "f1", "name": "Feature one", "cases": ["I-A-*"]},
    {"id": "f2", "name": "Feature two", "cases": ["I-B-01"]},
    {"id": "f3", "name": "Feature three", "cases": ["I-C-01"]},
    {"id": "f4", "name": "Feature four", "cases": ["I-D-01"]},
    {"id": "f5", "name": "Feature five", "cases": ["I-E-01"]},
]


class FeatureStateTest(unittest.TestCase):
    """Review finding 7."""

    F = {"id": "f", "name": "F", "cases": ["I-*"]}

    def st(self, vm, linux=None, hw=()):
        return report.feature_state(self.F, vm, linux or {}, set(hw))[0]

    def test_verified_needs_every_case(self):
        self.assertEqual(self.st({"I-1": "pass", "I-2": "pass"}), "Verified (VM)")
        self.assertEqual(self.st({"I-1": "pass", "I-2": "skip"}), "Partial (VM)")

    def test_failure_beats_pass_and_hardware(self):
        self.assertEqual(self.st({"I-1": "pass", "I-2": "fail"}), "Failing (VM)")
        self.assertEqual(self.st({"I-1": "error"}, hw=["f"]), "Failing (VM)")
        self.assertEqual(self.st({}, {"I-1": "fail"}, hw=["f"]), "Failing (Linux)")

    def test_linux_states(self):
        self.assertEqual(self.st({}, {"I-1": "pass"}), "Linux only")
        self.assertEqual(self.st({}, {"I-1": "pass", "I-2": "fail"}), "Failing (Linux)")
        self.assertEqual(self.st({"I-1": "skip"}, {"I-1": "pass"}), "Linux only")

    def test_hardware_and_untested(self):
        self.assertEqual(self.st({}, hw=["f"]), "Verified (hardware)")
        self.assertEqual(self.st({}), "Untested")


class ReadmeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        self.res = os.path.join(d, "results")
        os.makedirs(os.path.join(self.res, "hardware"))
        self.feat = os.path.join(d, "features.json")
        with open(self.feat, "w") as f:
            json.dump(FEATURES, f)
        write_doc(self.res, "a.json", "B", [mkcase("I-A-01"), mkcase("I-A-02"), mkcase("I-B-01", "fail")],
                  "2026-11-14T10:00:00Z")
        # a newer B run re-runs I-B-01 (now passes) and only skips I-A-01: a skip never hides a pass
        write_doc(self.res, "b.json", "B", [mkcase("I-B-01"), mkcase("I-A-01", "skip", "n")], "2026-11-15T10:00:00Z")
        write_doc(self.res, "lin.json", "A", [mkcase("I-C-01")], "2026-11-15T10:00:00Z")
        write_doc(self.res, "dirty.json", "B", [mkcase("I-D-01")], "2026-11-16T10:00:00Z", dirty=True)
        with open(os.path.join(self.res, "hardware", "2026-11-17.md"), "w") as f:
            f.write("Hardware-verified: f5\n")
        self.readme = os.path.join(d, "README.md")
        with open(self.readme, "w") as f:
            f.write("intro\n\n<!-- status-table:begin -->\nold\n<!-- status-table:end -->\n\ntail\n")

    def states(self):
        out = {}
        for line in report.render_table(self.res, self.feat).splitlines():
            parts = [p.strip() for p in line.strip("|").split("|")]
            if parts[0].startswith("Feature "):
                out[parts[0]] = parts[1]
        return out

    def test_states(self):
        s = self.states()
        self.assertEqual(s["Feature one"], "Verified (VM)")  # the later skip of I-A-01 does not hide its pass
        self.assertEqual(s["Feature two"], "Verified (VM)")
        self.assertEqual(s["Feature three"], "Linux only")
        self.assertEqual(s["Feature four"], "Untested")  # its only pass is from a dirty tree
        self.assertEqual(s["Feature five"], "Verified (hardware)")

    def test_table_names_no_result_files(self):
        self.assertNotIn(".json", report.render_table(self.res, self.feat))

    def run_cli(self, *extra):
        cli = [sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "readme", "--readme", self.readme,
               "--results-dir", self.res, "--features", self.feat]
        return subprocess.run(cli + list(extra), stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def test_idempotent_and_check(self):
        self.assertEqual(self.run_cli("--check").returncode, 1)  # stale
        self.assertEqual(self.run_cli().returncode, 0)
        with open(self.readme) as f:
            first = f.read()
        self.assertTrue(first.startswith("intro\n"))
        self.assertTrue(first.endswith("\ntail\n"))
        self.assertEqual(self.run_cli().returncode, 0)
        with open(self.readme) as f:
            self.assertEqual(f.read(), first)
        self.assertEqual(self.run_cli("--check").returncode, 0)
        # a new result that changes no status leaves the table current (it names no files)
        write_doc(self.res, "same.json", "B", [mkcase("I-B-01")], "2026-11-18T10:00:00Z")
        self.assertEqual(self.run_cli("--check").returncode, 0)
        write_doc(self.res, "d.json", "B", [mkcase("I-B-01", "fail")], "2026-11-18T11:00:00Z")
        self.assertEqual(self.run_cli("--check").returncode, 1)

    def test_missing_markers(self):
        with open(self.readme, "w") as f:
            f.write("no markers\n")
        self.assertEqual(self.run_cli().returncode, 2)


# ---------------------------------------------------------------------------------------------
# scripts/test-all.sh and vm/macos/run-suite.sh
# ---------------------------------------------------------------------------------------------

def plan_lines(text):
    """[(name, rest)] of the numbered plan lines (the nested run-suite plan included), in order."""
    out = []
    for line in text.splitlines():
        m = re.match(r"^\s*\d+\s+(\S+)\s+(.*)$", line)
        if m:
            out.append((m.group(1), m.group(2)))
    return out


class DryRunTest(unittest.TestCase):
    def run_all(self, *args):
        return subprocess.run(["sh", os.path.join(REPO, "scripts", "test-all.sh")] + list(args),
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE, cwd=REPO)

    def names(self, *args):
        r = self.run_all(*args, "--dry-run")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertIn("nothing was run", r.stdout.decode())
        return plan_lines(r.stdout.decode())

    def test_plans_name_every_stage_in_order(self):
        self.assertEqual([n for n, _ in self.names("--env", "host")],
                         ["build-release", "unit-release", "product-build", "build-asan", "unit-asan", "harness",
                          "stress"])
        self.assertEqual([n for n, _ in self.names("--env", "A")],
                         ["preflight", "selftest", "unit-probe", "unit-volmark", "harness", "stress-crashsweep"])
        self.assertEqual([n for n, _ in self.names("--env", "B")],
                         ["preflight", "build-release", "fixtures", "integration", "compat", "daemon", "stress-disconnect",
                          "stress-kill", "stress-leaks", "stress-parallel", "stress-soak"])

    def test_host_never_runs_the_vm_cases(self):
        d = dict(self.names("--env", "host"))
        self.assertTrue(d["harness"].endswith("]"))

    def test_quick_states(self):
        d = dict(self.names("--env", "A", "--quick"))
        self.assertTrue(d["unit-probe"].endswith("[skipped by --quick]"))
        self.assertTrue(d["unit-volmark"].endswith("[skipped by --quick]"))
        self.assertTrue(d["stress-crashsweep"].endswith("[skipped by --quick]"))
        self.assertNotIn("[", d["selftest"])
        self.assertIn("--cases I-INT-*", d["harness"])
        db = dict(self.names("--env", "B", "--quick"))
        self.assertTrue(db["compat"].endswith("[skipped by --quick]"))
        self.assertTrue(db["daemon"].endswith("[skipped by --quick]"))

    def test_cases_runs_only_harness(self):
        d = dict(self.names("--env", "A", "--cases", "I-INT-*"))
        for n in ("selftest", "unit-probe", "unit-volmark", "stress-crashsweep"):
            self.assertTrue(d[n].endswith("[not selected by --cases]"), n)
        self.assertIn("--env A --cases I-INT-*", d["harness"])
        self.assertNotIn("[", d["harness"])

    def test_dry_run_writes_nothing(self):
        with tempfile.TemporaryDirectory() as d:
            out = os.path.join(d, "r.json")
            self.assertEqual(self.run_all("--env", "host", "--dry-run", "--out", out).returncode, 0)
            self.assertFalse(os.path.exists(out))

    def test_usage_errors(self):
        self.assertEqual(self.run_all().returncode, 2)
        self.assertEqual(self.run_all("--env", "C").returncode, 2)
        self.assertEqual(self.run_all("--env", "A", "--bogus").returncode, 2)

    def test_stress_registry(self):
        r = self.run_all("--list-stress", "A")
        self.assertEqual(r.returncode, 0)
        self.assertIn("crashsweep", r.stdout.decode())
        self.assertEqual(self.run_all("--list-stress", "B").stdout.decode().split(), ["disconnect", "kill", "leaks", "parallel", "soak"])

    def test_usage_text_states_three_commands(self):
        r = subprocess.run(["sh", os.path.join(REPO, "scripts", "test-all.sh"), "--help"], stdout=subprocess.PIPE,
                           stderr=subprocess.PIPE)
        self.assertIn("THREE commands", r.stderr.decode())


def make_stubs(d, scripts):
    bindir = os.path.join(d, "bin")
    os.makedirs(bindir, exist_ok=True)
    scripts = dict(scripts)
    scripts.setdefault("git", GIT_STUB)
    for name, body in scripts.items():
        p = os.path.join(bindir, name)
        with open(p, "w") as f:
            f.write("#!/bin/sh\n" + body + "\n")
        os.chmod(p, os.stat(p).st_mode | stat.S_IXUSR)
    return bindir


HOST_STUBS = {
    "cmake": "exit 0",
    "ctest": 'echo "100% tests passed, 0 tests failed out of 3"; exit ${STUB_CTEST_RC:-0}',
}


class TestAllStubbedTest(unittest.TestCase):
    """scripts/test-all.sh really running, with stub cmake/ctest/docker on PATH."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.d = self.tmp.name
        os.makedirs(os.path.join(self.d, "tmp"))

    def run_all(self, stubs, *args, **env):
        bindir = make_stubs(self.d, stubs)
        e = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"], TMPDIR=os.path.join(self.d, "tmp"))
        e.update(env)
        return subprocess.run(["sh", os.path.join(REPO, "scripts", "test-all.sh")] + list(args), cwd=REPO, env=e,
                              stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def result(self, name="r.json"):
        with open(os.path.join(self.d, name)) as f:
            doc = json.load(f)
        self.assertEqual(report.validate(doc), [])
        return doc, {c["id"]: c for c in doc["cases"]}

    def leftovers(self):
        return glob.glob(os.path.join(self.d, "tmp", "ntfsmac-test-all.*"))

    def test_host_quick_passes(self):
        out = os.path.join(self.d, "r.json")
        r = self.run_all(HOST_STUBS, "--env", "host", "--quick", "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        doc, by = self.result()
        self.assertEqual(by["STAGE-HOST-UNIT-RELEASE"]["status"], "pass")
        self.assertIn("100% tests passed", by["STAGE-HOST-UNIT-RELEASE"]["note"])
        self.assertEqual(by["STAGE-HOST-BUILD-ASAN"]["status"], "skip")
        self.assertEqual(self.leftovers(), [])  # the work directory is removed after a good run

    def test_mode_record_full_quick_cases(self):
        """STAGE-HOST-MODE says which kind of run wrote the file (ci-local --verify-result accepts only full)."""
        for name, args, want in (("full.json", (), "full"), ("quick.json", ("--quick",), "quick"),
                                 ("cases.json", ("--cases", "X-*"), "cases=X-*")):
            out = os.path.join(self.d, name)
            self.run_all(HOST_STUBS, "--env", "host", "--out", out, *args)
            _doc, by = self.result(name)
            self.assertEqual(by["STAGE-HOST-MODE"]["status"], "pass", name)
            self.assertEqual(by["STAGE-HOST-MODE"]["note"], want, name)

    @unittest.skipUnless(subprocess.run(["git", "-C", REPO, "rev-parse", "--verify", "-q", "HEAD"],
                                        stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL).returncode == 0,
                         "no git checkout (the Linux container): the gate needs a commit; runs in the host ctest")
    def test_real_full_host_result_passes_the_gate_stage_and_mode_rules(self):
        """A real (stubbed-command) full host result, through ci-local --verify-result with the REAL plan: the
        by-design skips of the plan (harness, stress) and the mode record are accepted. It would have failed
        while the gate and test-all.sh disagreed about the note of the host harness skip."""
        out = os.path.join(self.d, "r.json")
        r = self.run_all(HOST_STUBS, "--env", "host", "--out", out)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        doc, by = self.result()
        self.assertEqual(by["STAGE-HOST-HARNESS"]["status"], "skip")
        self.assertEqual(by["STAGE-HOST-STRESS"]["status"], "skip")
        git_sha = subprocess.run(["git", "-C", REPO, "rev-parse", "HEAD"], stdout=subprocess.PIPE).stdout.decode().strip()
        doc["git_sha"], doc["dirty_tree"] = git_sha, False  # the tree under test is this checkout, maybe dirty
        res = os.path.join(self.d, "2026-11-20-%s-host.json" % git_sha[:7])
        with open(res, "w") as f:
            json.dump(doc, f)
        ci_doc = dict(doc, cases=[{"id": "STAGE-HOST-BOOTSTRAP", "status": "pass", "duration_s": 1, "evidence": [],
                                   "note": None},
                                  {"id": "STAGE-HOST-TSAN", "status": "pass", "duration_s": 1, "evidence": [], "note": None},
                                  {"id": "STAGE-HOST-STATIC-ANALYSIS", "status": "pass", "duration_s": 1, "evidence": [],
                                   "note": None}])
        ci_res = os.path.join(self.d, "2026-11-20-%s-host-ci.json" % git_sha[:7])
        with open(ci_res, "w") as f:
            json.dump(ci_doc, f)
        e = dict(os.environ, TMPDIR=os.path.join(self.d, "tmp"))
        e.pop("CI_LOCAL_TEST_ALL", None)
        cmd = ["sh", os.path.join(REPO, "scripts", "ci-local.sh"), "--verify-result", res, "--pre-commit",
               "--require-env", "host"]
        g = subprocess.run(cmd + ["--verify-result", ci_res], cwd=REPO, env=e, stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT)
        self.assertEqual(g.returncode, 0, g.stdout.decode())
        self.assertIn("verify-result: PASSED", g.stdout.decode())
        # the same result without the -ci file only lacks the own stages: no stage or mode complaint
        g = subprocess.run(cmd, cwd=REPO, env=e, stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        txt = g.stdout.decode()
        self.assertEqual(g.returncode, 1, txt)
        self.assertIn("lacks the stage case(s): STAGE-HOST-BOOTSTRAP, STAGE-HOST-TSAN, STAGE-HOST-STATIC-ANALYSIS", txt)
        for bad in ("not passed", "not a full run", "run-mode"):
            self.assertNotIn(bad, txt)

    def test_dirty_result_exits_3_with_a_distinct_message(self):
        """M5-F12: every stage passed on a dirty tree: the result is marked dirty and the exit status is 3 (not 0)."""
        dirty_git = 'case "$*" in *rev-parse*) echo 1111111111111111111111111111111111111111 ;; *status*) echo " M src/x.c" ;; esac; exit 0'
        out = os.path.join(self.d, "r.json")
        r = self.run_all(dict(HOST_STUBS, git=dirty_git), "--env", "host", "--quick", "--out", out)
        self.assertEqual(r.returncode, 3, r.stderr.decode())
        self.assertIn("PASSED (dirty, not usable for a gate)", r.stderr.decode())
        doc, _by = self.result()
        self.assertTrue(doc["dirty_tree"])
        # a failure on a dirty tree stays a failure (1), and a clean pass stays 0
        r = self.run_all(dict(HOST_STUBS, git=dirty_git), "--env", "host", "--quick", "--out", out + "2",
                         STUB_CTEST_RC="1")
        self.assertEqual(r.returncode, 1, r.stderr.decode())
        r = self.run_all(HOST_STUBS, "--env", "host", "--quick", "--out", out + "3")
        self.assertEqual(r.returncode, 0, r.stderr.decode())

    def test_failure_does_not_stop_the_run(self):
        out = os.path.join(self.d, "r.json")
        r = self.run_all(HOST_STUBS, "--env", "host", "--quick", "--out", out, STUB_CTEST_RC="1")
        self.assertEqual(r.returncode, 1)
        _doc, by = self.result()
        self.assertEqual(by["STAGE-HOST-UNIT-RELEASE"]["status"], "fail")
        self.assertEqual(by["STAGE-HOST-PRODUCT-BUILD"]["status"], "pass")  # a later stage still ran

    def test_nothing_verified_is_not_success(self):
        out = os.path.join(self.d, "r.json")
        r = self.run_all(HOST_STUBS, "--env", "host", "--cases", "X", "--out", out)
        self.assertEqual(r.returncode, 1, r.stderr.decode())
        self.assertIn("nothing verified", r.stderr.decode())

    def test_missing_stage_command_is_a_failure(self):
        out = os.path.join(self.d, "r.json")
        r = self.run_all({"cmake": "exit 3", "ctest": "exit 0"}, "--env", "host", "--quick", "--out", out)
        self.assertEqual(r.returncode, 1)
        _doc, by = self.result()
        self.assertEqual(by["STAGE-HOST-BUILD-RELEASE"]["status"], "fail")
        self.assertIn("exit 3", by["STAGE-HOST-BUILD-RELEASE"]["note"])

    def test_preflight_failure_skips_every_later_stage(self):
        out = os.path.join(self.d, "r.json")
        r = self.run_all({"docker": "exit 1"}, "--env", "A", "--out", out)
        self.assertEqual(r.returncode, 1)
        _doc, by = self.result()
        self.assertEqual(by["STAGE-A-PREFLIGHT"]["status"], "fail")
        for n in ("SELFTEST", "UNIT-PROBE", "UNIT-VOLMARK", "HARNESS", "STRESS-CRASHSWEEP"):
            c = by["STAGE-A-" + n]
            self.assertEqual(c["status"], "skip", n)
            self.assertEqual(c["note"], "preflight failed", n)

    def test_missing_image_is_reported_not_built(self):
        log = os.path.join(self.d, "docker.log")
        docker = 'echo "$@" >>"%s"; case "$1 $2" in "image inspect") exit 1;; esac; exit 0' % log
        out = os.path.join(self.d, "r.json")
        r = self.run_all({"docker": docker}, "--env", "A", "--out", out)
        self.assertEqual(r.returncode, 1)
        _doc, by = self.result()
        self.assertEqual(by["STAGE-A-PREFLIGHT"]["status"], "fail")
        with open(os.path.join(self.d, "r.json")) as f:
            self.assertIn("vm/linux/run.sh build", f.read())
        with open(log) as f:
            calls = f.read()
        self.assertNotIn("build", calls)
        self.assertNotIn("pull", calls)

    def test_existing_out_refused(self):
        out = os.path.join(self.d, "r.json")
        put(out, "{}")
        self.assertEqual(self.run_all(HOST_STUBS, "--env", "host", "--quick", "--out", out).returncode, 2)

    @unittest.skipIf(hasattr(os, "geteuid") and os.geteuid() == 0, "root ignores directory permissions")
    def test_write_failure_keeps_the_work_directory(self):
        ro = os.path.join(self.d, "ro")
        os.makedirs(ro)
        os.chmod(ro, 0o500)
        self.addCleanup(os.chmod, ro, 0o700)
        r = self.run_all(HOST_STUBS, "--env", "host", "--quick", "--out", os.path.join(ro, "r.json"))
        self.assertEqual(r.returncode, 2, r.stderr.decode())
        self.assertIn("work directory kept", r.stderr.decode())
        self.assertEqual(len(self.leftovers()), 1)


class RunSuiteTest(unittest.TestCase):
    """vm/macos/run-suite.sh in a fake tree with stub children: the stages.tsv and suite.rc hand-off."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        self.fake = os.path.join(d, "repo")
        for rel in ("vm/macos/run-suite.sh", "scripts/test-all.sh"):
            os.makedirs(os.path.dirname(os.path.join(self.fake, rel)), exist_ok=True)
            shutil.copy(os.path.join(REPO, rel), os.path.join(self.fake, rel))
        put(os.path.join(self.fake, "tests", "harness", "run-env-b.sh"),
            'echo "env-b $*"; exit ${STUB_ENVB_RC:-0}\n')
        put(os.path.join(self.fake, "tests", "harness", "run.py"),
            "import os, sys\n"
            "if '--list' in sys.argv:\n"
            "    m = os.environ.get('STUB_LIST', 'ok')\n"
            "    if m == 'fail':\n"
            "        sys.stderr.write('run.py: no case matches boom\\n'); sys.exit(2)\n"
            "    if m == 'ok':\n"
            "        print('I-INT-01\\tB\\tt')\n"
            "sys.exit(0)\n")
        self.work = os.path.join(d, "work")
        self.slots = os.path.join(d, "slots")
        self.stubs = {"tart": 'echo ntfsmac-golden', "docker": "exit 0"}

    def run_suite(self, *args, **env):
        bindir = make_stubs(self.tmp.name, self.stubs)
        e = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"])
        e.update(env)
        return subprocess.run(["sh", os.path.join(self.fake, "vm", "macos", "run-suite.sh"), "--work", self.work,
                               "--slot-dir", self.slots] + list(args), env=e, stdout=subprocess.PIPE,
                              stderr=subprocess.PIPE)

    def stages(self):
        out = {}
        with open(os.path.join(self.work, "stages.tsv")) as f:
            for line in f:
                p = line.rstrip("\n").split("\t")
                out[p[0]] = (p[1], p[3])
        return out

    def suite_rc(self):
        with open(os.path.join(self.work, "suite.rc")) as f:
            return f.read().strip()

    def test_quick_pass_hand_off(self):
        r = self.run_suite("--quick")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.suite_rc(), "0")
        st = self.stages()
        self.assertEqual(st["integration"][0], "pass")
        self.assertEqual(st["compat"][0], "skip")
        self.assertEqual(st["daemon"][0], "skip")
        self.assertEqual(st["stress"], ("skip", "none registered"))
        self.assertEqual(os.listdir(self.slots), [])  # the slot lock is released

    def test_discovery_failure_is_a_failed_stage_not_a_skip(self):
        r = self.run_suite("--quick", STUB_LIST="fail")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.suite_rc(), "1")
        st = self.stages()
        self.assertEqual(st["integration"][0], "fail")
        self.assertIn("case discovery failed", st["integration"][1])
        self.assertIn("boom", st["integration"][1])

    def test_no_matching_case_is_a_skip_with_reason(self):
        r = self.run_suite("--quick", STUB_LIST="none")
        self.assertEqual(r.returncode, 0)
        self.assertEqual(self.stages()["integration"][0], "skip")
        self.assertIn("matches", self.stages()["integration"][1])

    def test_child_failure_fails_the_stage_and_suite(self):
        r = self.run_suite("--quick", STUB_ENVB_RC="1")
        self.assertEqual(r.returncode, 1)
        self.assertEqual(self.suite_rc(), "1")
        self.assertEqual(self.stages()["integration"][0], "fail")

    def test_preflight_failure_skips_the_rest_and_takes_no_slot(self):
        self.stubs["tart"] = "exit 0"  # no golden VM listed
        r = self.run_suite("--quick")
        self.assertEqual(r.returncode, 1)
        st = self.stages()
        self.assertEqual(st["vm-preflight"][0], "fail")
        self.assertNotIn("preflight", st)  # test-all.sh owns the name "preflight"
        for n in ("integration", "compat", "daemon", "stress"):
            self.assertEqual(st[n], ("skip", "vm-preflight failed"), n)
        self.assertFalse(os.path.exists(self.slots))

    def test_slot_default_directory_is_used(self):
        with open(os.path.join(REPO, "vm", "macos", "run-suite.sh")) as f:
            self.assertIn("ntfsmac-vm-slots", f.read())


if __name__ == "__main__":
    unittest.main()


class GitInfoEnvTest(unittest.TestCase):
    """git-info reads the tree, never the environment."""

    def test_exported_sha_and_dirty_have_no_effect(self):
        with tempfile.TemporaryDirectory() as d:
            bindir = make_stubs(d, {})
            e = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"],
                     NTFSMAC_GIT_SHA="2" * 40, NTFSMAC_GIT_DIRTY="false")
            r = subprocess.run([sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "--repo", REPO,
                                "git-info"], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(r.stdout.decode().split(), ["1" * 40, "false"], r.stderr.decode())
            e["PATH"] = bindir + os.pathsep + "/usr/bin:/bin"
            with open(os.path.join(bindir, "git"), "w") as f:
                f.write('#!/bin/sh\ncase "$*" in *rev-parse*) echo %s ;; *status*) echo " M x" ;; esac\n' % ("3" * 40))
            r = subprocess.run([sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "--repo", REPO,
                                "git-info"], env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(r.stdout.decode().split(), ["3" * 40, "true"], r.stderr.decode())
