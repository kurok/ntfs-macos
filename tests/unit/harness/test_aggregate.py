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
import signal
import stat
import subprocess
import sys
import tempfile
import time
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


def make_git_repo(d):
    """A throwaway repository: main has commits C1 <- C2 (HEAD); side has C3 off C1 (not an ancestor of
    HEAD). Returns (repo, C1, C2, C3), or None when the git binary is absent (any other git error is a failure)."""
    if shutil.which("git") is None:
        return None
    repo = os.path.join(d, "repo")
    os.makedirs(repo)

    def g(*a):
        r = subprocess.run(("git", "-c", "user.name=t", "-c", "user.email=t@example.invalid", "-c", "commit.gpgsign=false",
                            "-C", repo) + a, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        if r.returncode != 0:
            raise AssertionError("git %s failed: %s" % (" ".join(a), r.stderr.decode()[:200]))
        return r.stdout.decode().strip()
    g("init", "-q", "-b", "main")
    out = []
    for name in ("one", "two"):
        with open(os.path.join(repo, name), "w") as f:
            f.write(name)
        g("add", name)
        g("commit", "-q", "-m", name)
        out.append(g("rev-parse", "HEAD"))
    g("checkout", "-q", "-b", "side", out[0])
    with open(os.path.join(repo, "three"), "w") as f:
        f.write("three")
    g("add", "three")
    g("commit", "-q", "-m", "three")
    c3 = g("rev-parse", "HEAD")
    g("checkout", "-q", "main")
    return repo, out[0], out[1], c3


class ReadmeTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        self.git = make_git_repo(d)  # None: no git binary; the tests that need it skip with that reason
        self.sha = self.git[2] if self.git else SHA
        self.res = os.path.join(d, "results")
        os.makedirs(os.path.join(self.res, "hardware"))
        self.feat = os.path.join(d, "features.json")
        with open(self.feat, "w") as f:
            json.dump(FEATURES, f)
        write_doc(self.res, "a.json", "B", [mkcase("I-A-01"), mkcase("I-A-02"), mkcase("I-B-01", "fail")],
                  "2026-11-14T10:00:00Z", sha=self.sha)
        # a newer B run re-runs I-B-01 (now passes) and only skips I-A-01: a skip never hides a pass
        write_doc(self.res, "b.json", "B", [mkcase("I-B-01"), mkcase("I-A-01", "skip", "n")], "2026-11-15T10:00:00Z",
                  sha=self.sha)
        write_doc(self.res, "lin.json", "A", [mkcase("I-C-01")], "2026-11-15T10:00:00Z", sha=self.sha)
        write_doc(self.res, "dirty.json", "B", [mkcase("I-D-01")], "2026-11-16T10:00:00Z", dirty=True, sha=self.sha)
        with open(os.path.join(self.res, "hardware", "2026-11-17-%s.md" % self.sha[:7]), "w") as f:
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

    def need_git(self):
        if self.git is None:
            self.skipTest("the git binary is not installed")

    def run_cli(self, *extra):
        self.need_git()
        cli = [sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "--repo", self.git[0], "readme",
               "--readme", self.readme, "--results-dir", self.res, "--features", self.feat]
        return subprocess.run(cli + list(extra), stdout=subprocess.PIPE, stderr=subprocess.PIPE)

    def rows(self, **kw):
        out = {}
        for line in report.render_table(self.res, self.feat, **kw).splitlines():
            parts = [p.strip() for p in line.strip("|").split("|")]
            if len(parts) > 1 and parts[0] != "Feature" and not parts[0].startswith("---"):
                out[parts[0]] = parts[1:]
        return out

    def test_row_shows_the_commit_of_the_newest_result(self):
        self.need_git()
        _repo, c1, c2, _c3 = self.git
        # f1: I-A-01, I-A-02 come from a.json (c2); a newer run on c2 re-runs I-B-01 only
        r = self.rows(repo=self.git[0])
        self.assertEqual(r["Feature one"][-1], c2[:7])
        self.assertEqual(r["Feature four"][-1], "-")  # no clean result
        self.assertEqual(r["Feature five"][-1], c2[:7])  # the commit in the hardware file name
        # a newer result on an older commit still counts as the newest result of the row
        write_doc(self.res, "c.json", "B", [mkcase("I-A-01")], "2026-11-18T10:00:00Z", sha=c1)
        self.assertEqual(self.rows(repo=self.git[0])["Feature one"][-1], c1[:7])

    def test_commit_is_that_of_the_side_that_decided_the_state(self):
        self.need_git()
        _repo, c1, c2, _c3 = self.git
        # (a) a hardware sign-off (c2) and a newer VM failure (c1): Failing (VM), the failure's commit
        write_doc(self.res, "f.json", "B", [mkcase("I-E-01", "fail")], "2026-11-19T10:00:00Z", sha=c1)
        r = self.rows(repo=self.git[0])["Feature five"]
        self.assertEqual((r[0], r[-1]), ("Failing (VM)", c1[:7]))
        # (b) the Linux case fails (c1, newest) while a VM pass exists (c2): Failing (Linux), the Linux commit
        write_doc(self.res, "vm.json", "B", [mkcase("I-C-01")], "2026-11-19T11:00:00Z", sha=c2)
        write_doc(self.res, "lf.json", "A", [mkcase("I-C-01", "fail")], "2026-11-19T12:00:00Z", sha=c1)
        r = self.rows(repo=self.git[0])["Feature three"]
        self.assertEqual((r[0], r[-1]), ("Failing (Linux)", c1[:7]))

    def test_result_of_a_commit_that_is_not_an_ancestor_is_ignored(self):
        self.need_git()
        _repo, _c1, _c2, c3 = self.git
        write_doc(self.res, "side.json", "B", [mkcase("I-D-01")], "2026-11-19T10:00:00Z", sha=c3)
        write_doc(self.res, "gone.json", "B", [mkcase("I-A-01", "fail")], "2026-11-19T11:00:00Z", sha="0" * 40)
        self.assertEqual(self.rows()["Feature four"][0], "Verified (VM)")  # unfiltered: counted
        self.assertEqual(self.rows()["Feature one"][0], "Failing (VM)")
        r = self.rows(repo=self.git[0])
        self.assertEqual(r["Feature four"][0], "Untested")  # filtered: the side branch and the unknown commit do not count
        self.assertEqual(r["Feature one"][0], "Verified (VM)")

    def test_hardware_line_is_bound_to_the_commit_in_the_file_name(self):
        self.need_git()
        _repo, _c1, _c2, c3 = self.git
        hw = os.path.join(self.res, "hardware")
        os.unlink(os.path.join(hw, "2026-11-17-%s.md" % self.sha[:7]))
        for name in ("2026-11-17.md", "notes.md", "2026-11-17-%s.md" % c3[:7]):  # no commit, no date, not an ancestor
            with open(os.path.join(hw, name), "w") as f:
                f.write("Hardware-verified: f5\n")
        self.assertEqual(self.rows(repo=self.git[0])["Feature five"][0], "Untested")
        with open(os.path.join(hw, "2026-11-18-%s.md" % self.git[1][:7]), "w") as f:
            f.write("Hardware-verified: f5\n")
        self.assertEqual(self.rows(repo=self.git[0])["Feature five"], ["Verified (hardware)", "hardware run", self.git[1][:7]])

    def test_short_results_never_decide_a_row(self):
        write_doc(self.res, "s.json", "B", [mkcase("I-D-01-SHORT")], "2026-11-19T10:00:00Z", sha=self.sha)
        f = [{"id": "x", "name": "X", "cases": ["I-D-*", "I-D-01"]}]
        with open(self.feat, "w") as fh:
            json.dump(f, fh)
        self.assertEqual(self.rows()["X"][0], "Untested")  # a -SHORT pass is not a pass of the row
        write_doc(self.res, "t.json", "B", [mkcase("I-D-01"), mkcase("I-D-01-SHORT", "fail")], "2026-11-20T10:00:00Z",
                  sha=self.sha)
        self.assertEqual(self.rows()["X"][0], "Verified (VM)")  # and a -SHORT failure is not a failure of it

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
        write_doc(self.res, "same.json", "B", [mkcase("I-B-01")], "2026-11-18T10:00:00Z", sha=self.sha)
        self.assertEqual(self.run_cli("--check").returncode, 0)
        write_doc(self.res, "d.json", "B", [mkcase("I-B-01", "fail")], "2026-11-18T11:00:00Z", sha=self.sha)
        self.assertEqual(self.run_cli("--check").returncode, 1)

    def test_readme_without_resolvable_head_exits_2(self):
        self.need_git()
        empty = os.path.join(self.tmp.name, "empty")
        os.makedirs(empty)
        env = dict(os.environ, GIT_CEILING_DIRECTORIES=self.tmp.name)  # git must not find a repository above
        r = subprocess.run([sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "--repo", empty,
                            "readme", "--print", "--results-dir", self.res, "--features", self.feat],
                           stdout=subprocess.PIPE, stderr=subprocess.PIPE, env=env)
        self.assertEqual(r.returncode, 2)
        self.assertIn(b"cannot resolve", r.stderr)

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
                         ["preflight", "build-release", "fixtures", "guest-build", "integration", "compat", "daemon",
                          "stress-disconnect", "stress-kill", "stress-leaks", "stress-parallel", "stress-soak"])

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
        put(os.path.join(self.fake, "vm", "macos", "guest-build.sh"), 'echo "guest-build $*"; exit ${STUB_GBUILD_RC:-0}\n')
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

    def test_guest_build_runs_first_in_a_full_run_and_a_failure_skips_the_rest(self):
        r = self.run_suite()
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.stages()["guest-build"][0], "pass")
        r = self.run_suite(STUB_GBUILD_RC="1")
        self.assertEqual(r.returncode, 1)
        st = self.stages()
        self.assertEqual(st["guest-build"][0], "fail")
        for n in ("integration", "compat", "daemon"):
            self.assertEqual(st[n], ("skip", "guest-build failed"), n)

    def test_guest_build_is_skipped_by_quick_and_by_its_option(self):
        self.run_suite("--quick")
        self.assertEqual(self.stages()["guest-build"], ("skip", "not run with --quick or --cases"))
        shutil.rmtree(self.work)
        self.run_suite("--no-guest-build")
        self.assertEqual(self.stages()["guest-build"], ("skip", "skipped by --no-guest-build"))

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


STAGE_STUB = r'''# a fake stage: one "guest" (a directory) for its whole run, an event log with times, and the exit status and the
# duration the test asks for (STUB_SLEEP="name=seconds,..." and STUB_RC="name=status,...")
name=$1
val() { printf '%s\n' "$1" | tr ',' '\n' | sed -n "s/^$2=//p" | tail -n 1; }
g=$STUB_DIR/guests
mkdir -p "$g"
mkdir "$g/$name-$$"
trap 'rmdir "$g/$name-$$"' EXIT
trap 'exit 143' TERM
ls "$g" | wc -l | tr -d ' ' >>"$STUB_DIR/counts"
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
echo "$name start $(now)" >>"$STUB_DIR/events"
s=$(val "${STUB_SLEEP:-}" "$name")
if [ -n "$s" ]; then sleep "$s" & wait $!; fi
echo "$name end $(now)" >>"$STUB_DIR/events"
rc=$(val "${STUB_RC:-}" "$name")
exit "${rc:-0}"
'''

PLAN = ["guest-build", "integration", "compat", "daemon", "stress-disconnect", "stress-kill", "stress-leaks",
        "stress-parallel", "stress-soak"]


class RunSuiteLanesTest(unittest.TestCase):
    """vm/macos/run-suite.sh, a full run, with stub stages that sleep: the two lanes (CI speedup, phase 2).
    A "guest" is a directory a stub holds for its whole run, so the number of directories is the number of guests."""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        self.fake = os.path.join(d, "repo")
        self.sd = os.path.join(d, "stub")  # events, counts and the fake guests
        os.makedirs(self.sd)
        for rel in ("vm/macos/run-suite.sh", "scripts/test-all.sh"):
            os.makedirs(os.path.dirname(os.path.join(self.fake, rel)), exist_ok=True)
            shutil.copy(os.path.join(REPO, rel), os.path.join(self.fake, rel))
        put(os.path.join(self.fake, "vm", "macos", "stage.sh"), STAGE_STUB)
        for rel, name in (("vm/macos/guest-build.sh", "guest-build"), ("tests/integration/daemon/dmn_all.sh", "daemon")):
            put(os.path.join(self.fake, rel), 'exec sh "%s/vm/macos/stage.sh" %s "$@"\n' % (self.fake, name))
        put(os.path.join(self.fake, "tests", "harness", "run-env-b.sh"),
            'case " $* " in *" --orchestrate "*) n=compat ;; *) n=integration ;; esac\n'
            'exec sh "%s/vm/macos/stage.sh" $n "$@"\n' % self.fake)
        for e in ("disconnect", "kill", "leaks", "parallel", "soak"):
            put(os.path.join(self.fake, "tests", "stress", e, "ENVS"), "B\n")
            put(os.path.join(self.fake, "tests", "stress", e, "run.sh"),
                'exec sh "%s/vm/macos/stage.sh" stress-%s "$@"\n' % (self.fake, e))
        put(os.path.join(self.fake, "build", "release", "src", "daemon", "ntfsmacd"), "")
        put(os.path.join(self.fake, "tests", "harness", "run.py"),
            "import sys\nif '--list' in sys.argv:\n    print('I-INT-01\\tB\\tt')\nsys.exit(0)\n")
        self.fx = os.path.join(d, "fixtures")
        os.makedirs(self.fx)
        self.work = os.path.join(d, "work")
        self.slots = os.path.join(d, "slots")
        self.stubs = {"tart": "echo ntfsmac-golden", "docker": "exit 0"}

    def env(self, **env):
        bindir = make_stubs(self.tmp.name, self.stubs)
        e = dict(os.environ, PATH=bindir + os.pathsep + os.environ["PATH"], STUB_DIR=self.sd)
        e.update(env)
        return e

    def argv(self, *args):
        return ["sh", os.path.join(self.fake, "vm", "macos", "run-suite.sh"), "--work", self.work, "--slot-dir",
                self.slots, "--fixtures", self.fx] + list(args)

    def run_suite(self, *args, **env):
        return subprocess.run(self.argv(*args), env=self.env(**env), stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                              timeout=120)

    def stages(self):
        with open(os.path.join(self.work, "stages.tsv")) as f:
            return [line.rstrip("\n").split("\t") for line in f]

    def times(self):
        """{stage: (start, end)} from the events of the stubs"""
        out = {}
        with open(os.path.join(self.sd, "events")) as f:
            for line in f:
                n, what, t = line.split()
                s, e = out.get(n, (None, None))
                out[n] = (float(t), e) if what == "start" else (s, float(t))
        return out

    def overlap(self, a, b):
        t = self.times()
        return min(t[a][1], t[b][1]) - max(t[a][0], t[b][0]) > 0

    def max_guests(self):
        with open(os.path.join(self.sd, "counts")) as f:
            return max(int(x) for x in f.read().split())

    SLEEP = ("guest-build=0.2,integration=1.5,compat=1,daemon=2,stress-soak=2.5,stress-kill=1,stress-leaks=0.5,"
             "stress-parallel=1.5,stress-disconnect=1")

    def test_two_lanes_run_at_the_same_time_and_never_more_than_two_guests(self):
        r = self.run_suite(STUB_SLEEP=self.SLEEP)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 2)  # two at once, never three
        # phase 2: integration then compat in lane 1, daemon in lane 2
        self.assertTrue(self.overlap("integration", "daemon"))
        self.assertTrue(self.overlap("compat", "daemon"))
        self.assertFalse(self.overlap("integration", "compat"))
        # the stress suites in pairs: the heavy ones are never together, nor are kill and disconnect
        self.assertTrue(self.overlap("stress-soak", "stress-kill"))
        self.assertTrue(self.overlap("stress-parallel", "stress-disconnect"))
        self.assertFalse(self.overlap("stress-soak", "stress-parallel"))
        self.assertFalse(self.overlap("stress-kill", "stress-disconnect"))
        self.assertFalse(self.overlap("guest-build", "integration"))  # guest-build runs alone
        self.assertEqual(os.listdir(self.slots), [])  # both slots released
        self.assertEqual(os.listdir(os.path.join(self.sd, "guests")), [])
        # wall time is below the sum of the stages (the whole point)
        t = self.times()
        total = sum(e - s for s, e in t.values())
        self.assertLess(max(e for _, e in t.values()) - min(s for s, _ in t.values()), total - 3)

    def test_records_are_in_plan_order_not_completion_order(self):
        r = self.run_suite(STUB_SLEEP=self.SLEEP)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual([p[0] for p in self.stages()], PLAN)
        for p in self.stages():
            self.assertEqual(p[1], "pass")
            self.assertEqual(p[4], "logs/%s.log" % p[0])
        t = self.times()
        done = sorted(t, key=lambda n: t[n][1])
        self.assertNotEqual(done, PLAN)  # the stubs really finish in another order
        for n in PLAN:  # every stage its own log
            self.assertTrue(os.path.exists(os.path.join(self.work, "logs", n + ".log")))

    def test_lanes_one_is_the_old_sequential_order(self):
        r = self.run_suite("--lanes", "1", STUB_SLEEP=self.SLEEP)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 1)
        self.assertEqual([p[0] for p in self.stages()], PLAN)
        t = self.times()
        self.assertEqual(sorted(t, key=lambda n: t[n][0]), PLAN)  # started one after the other in plan order
        for a, b in zip(PLAN, PLAN[1:]):
            self.assertLessEqual(t[a][1], t[b][0])
        self.assertNotIn("(lane", r.stderr.decode())
        self.assertEqual(os.listdir(self.slots), [])

    def test_a_failing_stage_does_not_stop_the_other_lane(self):
        # daemon (lane 2) fails early; integration and compat (lane 1) go on and pass; a failing soak leaves its partner
        r = self.run_suite(STUB_SLEEP=self.SLEEP + ",daemon=0.3", STUB_RC="daemon=1,stress-soak=3")
        self.assertEqual(r.returncode, 1)
        st = {p[0]: p for p in self.stages()}
        self.assertEqual([p[0] for p in self.stages()], PLAN)
        self.assertEqual(st["daemon"][1], "fail")
        self.assertIn("exit 1", st["daemon"][3])
        self.assertEqual(st["integration"][1], "pass")
        self.assertEqual(st["compat"][1], "pass")
        self.assertEqual(st["stress-soak"][1], "fail")
        self.assertEqual(st["stress-kill"][1], "pass")
        self.assertEqual(st["stress-leaks"][1], "pass")
        self.assertEqual(st["stress-parallel"][1], "pass")  # and the next phase still runs
        with open(os.path.join(self.work, "suite.rc")) as f:
            self.assertEqual(f.read().strip(), "1")
        self.assertGreater(self.times()["compat"][1], self.times()["daemon"][1])  # compat ended after the failure

    def test_a_guest_build_failure_blocks_both_lanes(self):
        r = self.run_suite(STUB_RC="guest-build=1")
        self.assertEqual(r.returncode, 1)
        st = {p[0]: p for p in self.stages()}
        self.assertEqual([p[0] for p in self.stages()], PLAN)
        self.assertEqual(st["guest-build"][1], "fail")
        for n in PLAN[1:]:
            self.assertEqual((st[n][1], st[n][3]), ("skip", "guest-build failed"), n)
        self.assertNotIn("integration", self.times())  # nothing of the lanes started
        self.assertNotIn("daemon", self.times())

    def test_without_a_second_free_slot_the_lanes_run_one_after_the_other(self):
        os.makedirs(os.path.join(self.slots, "slot-2"))
        holder = subprocess.Popen(["sleep", "60"])
        self.addCleanup(holder.wait)
        self.addCleanup(holder.kill)
        with open(os.path.join(self.slots, "slot-2", "pid"), "w") as f:
            f.write("%d\n" % holder.pid)
        r = self.run_suite(STUB_SLEEP="integration=0.3,daemon=0.3,compat=0.3")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertIn("no second VM slot is free", r.stderr.decode())
        self.assertEqual(self.max_guests(), 1)
        self.assertEqual([p[0] for p in self.stages()], PLAN)
        self.assertEqual(os.listdir(self.slots), ["slot-2"])  # the other holder's lock is untouched

    def test_quick_uses_one_lane(self):
        r = self.run_suite("--quick", STUB_SLEEP="integration=0.3")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 1)
        self.assertNotIn("== lanes:", r.stderr.decode())

    def test_lanes_option_is_validated(self):
        self.assertEqual(self.run_suite("--lanes", "3").returncode, 2)
        self.assertEqual(self.run_suite("--lanes", "x").returncode, 2)

    def test_dry_run_plan_is_the_same_for_one_and_two_lanes(self):
        a = subprocess.run(self.argv("--dry-run"), env=self.env(), stdout=subprocess.PIPE).stdout
        b = subprocess.run(self.argv("--dry-run", "--lanes", "1"), env=self.env(), stdout=subprocess.PIPE).stdout
        self.assertEqual(a, b)
        self.assertEqual([n for n, _ in plan_lines(a.decode())], PLAN)

    def test_a_signal_stops_both_lanes_and_deletes_every_guest(self):
        p = subprocess.Popen(self.argv(), env=self.env(STUB_SLEEP="guest-build=0.1,integration=60,daemon=60"),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(p.kill)
        guests = os.path.join(self.sd, "guests")
        deadline = time.time() + 30
        while time.time() < deadline:
            if os.path.isdir(guests) and len(os.listdir(guests)) == 2:
                break
            time.sleep(0.1)
        self.assertEqual(len(os.listdir(guests)), 2, "both lanes should hold a guest")
        p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=60)
        self.assertEqual(p.returncode, 130, err.decode())
        self.assertEqual(os.listdir(guests), [])  # the cleanup of both stages ran
        self.assertEqual(os.listdir(self.slots), [])
        st = {x[0]: x for x in self.stages()}
        self.assertEqual(st["integration"][1], "fail")
        self.assertEqual(st["daemon"][1], "fail")
        self.assertNotIn("compat", self.times())  # nothing started after the signal
        with open(os.path.join(self.work, "suite.rc")) as f:
            self.assertEqual(f.read().strip(), "130")
        self.assertEqual(os.listdir(os.path.join(self.work, "pid")), [])

    DATE_STUB = (
        'if [ "$1" = +%s ]; then\n'
        '  [ ! -e "$STUB_DIR/crash-date" ] || exit 1\n'
        '  if [ -e "$STUB_DIR/slow-date" ]; then\n'
        '    n=$(cat "$STUB_DIR/datecount" 2>/dev/null || echo 0); n=$((n + 1)); echo $n >"$STUB_DIR/datecount"\n'
        '    [ "$n" != "$(cat "$STUB_DIR/slow-date")" ] || sleep 3\n'
        '  fi\n'
        'fi\n'
        'exec /bin/date "$@"')

    def wait_guests(self, n):
        guests = os.path.join(self.sd, "guests")
        deadline = time.time() + 30
        while time.time() < deadline:
            if os.path.isdir(guests) and len(os.listdir(guests)) == n:
                return
            time.sleep(0.02)
        self.fail("%d guests did not appear" % n)

    def test_a_stage_that_is_being_prepared_when_the_signal_comes_is_failed_not_run(self):
        self.stubs["date"] = self.DATE_STUB
        p = subprocess.Popen(self.argv(), env=self.env(STUB_SLEEP="integration=1,daemon=60"),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(p.wait)
        self.addCleanup(p.kill)
        self.wait_guests(2)
        put(os.path.join(self.sd, "slow-date"), "2")  # the 2nd `date +%s` from now (compat's start) takes 3 s
        time.sleep(2.5)  # integration ended after 1 s, compat is in its slow date
        p.send_signal(signal.SIGTERM)
        out, err = p.communicate(timeout=60)
        self.assertEqual(p.returncode, 130, err.decode())
        st = {x[0]: x for x in self.stages()}
        self.assertEqual(st["integration"][1], "pass")
        self.assertEqual(st["compat"][1], "fail")
        self.assertIn("before it started", st["compat"][3])
        self.assertNotIn("compat", self.times())  # it never ran
        self.assertEqual(os.listdir(os.path.join(self.sd, "guests")), [])

    def test_a_crashing_lane_is_a_failure_and_its_missing_stages_are_recorded(self):
        self.stubs["date"] = self.DATE_STUB
        p = subprocess.Popen(self.argv(), env=self.env(STUB_SLEEP="guest-build=0.1,integration=4,compat=0.3,daemon=1"),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(p.wait)
        self.addCleanup(p.kill)
        self.wait_guests(2)
        put(os.path.join(self.sd, "crash-date"), "")  # daemon's `date +%s` after its stage ends fails: lane 2 dies under set -e
        time.sleep(2.5)
        os.unlink(os.path.join(self.sd, "crash-date"))
        out, err = p.communicate(timeout=120)
        self.assertEqual(p.returncode, 1, err.decode())
        st = {x[0]: x for x in self.stages()}
        self.assertEqual(st["daemon"][1], "fail")
        self.assertIn("not run", st["daemon"][3])
        self.assertEqual(st["lane-2"][1], "fail")
        self.assertEqual(st["integration"][1], "pass")
        self.assertEqual(st["compat"][1], "pass")
        for n in PLAN[4:]:  # the later phases still ran
            self.assertEqual(st[n][1], "pass", n)
        with open(os.path.join(self.work, "suite.rc")) as f:
            self.assertEqual(f.read().strip(), "1")

    def test_a_signal_to_test_all_reaches_run_suite_and_the_guests_are_gone(self):
        """scripts/test-all.sh --env B (quick, in a fake tree): TERM stops the stage, deletes its guest, writes a result."""
        shutil.copytree(os.path.join(REPO, "tests", "harness"), os.path.join(self.fake, "tests", "harness"),
                        dirs_exist_ok=True, ignore=shutil.ignore_patterns("__pycache__"))
        put(os.path.join(self.fake, "tests", "harness", "run-env-b.sh"),
            'exec sh "%s/vm/macos/stage.sh" integration "$@"\n' % self.fake)
        put(os.path.join(self.fake, "tests", "harness", "run.py"),
            "import sys\nif '--list' in sys.argv:\n    print('I-INT-01\\tB\\tt')\nsys.exit(0)\n")
        put(os.path.join(self.fake, "third_party", "deps.lock"), "ntfs-3g 1 http://x/ntfs-3g.tar sum\n")
        put(os.path.join(self.fake, "third_party", "cache", "ntfs-3g.tar"), "")
        self.stubs.update({"cmake": "exit 0", "git": GIT_STUB})
        out = os.path.join(self.tmp.name, "res", "r.json")
        tmpdir = os.path.join(self.tmp.name, "tmp")
        os.makedirs(tmpdir)
        p = subprocess.Popen(["sh", os.path.join(self.fake, "scripts", "test-all.sh"), "--env", "B", "--quick", "--out",
                              out, "--slot-dir", self.slots], cwd=self.fake,
                             env=self.env(STUB_SLEEP="integration=60", TMPDIR=tmpdir),
                             stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        self.addCleanup(p.wait)
        self.addCleanup(p.kill)
        guests = os.path.join(self.sd, "guests")
        deadline = time.time() + 30
        while time.time() < deadline and not (os.path.isdir(guests) and os.listdir(guests)):
            time.sleep(0.1)
        self.assertEqual(len(os.listdir(guests)), 1, "the stage should hold a guest")
        p.send_signal(signal.SIGTERM)
        so, err = p.communicate(timeout=90)
        self.assertEqual(p.returncode, 1, err.decode())
        self.assertEqual(os.listdir(guests), [])
        self.assertEqual(os.listdir(self.slots), [])
        with open(out) as f:
            by = {c["id"]: c for c in json.load(f)["cases"]}
        self.assertEqual(by["STAGE-B-INTERRUPTED"]["status"], "fail")
        self.assertEqual(by.get("STAGE-B-INTEGRATION", {}).get("status"), "fail", sorted(by))


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
