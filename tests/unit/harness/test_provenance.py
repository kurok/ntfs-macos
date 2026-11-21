# SPDX-License-Identifier: GPL-2.0-or-later
"""The commit of a criterion-run result is the commit at the START of the run (M5-F04/F05 review): each stress run.sh
records DIR/provenance.txt before it builds or copies anything, and the result builders use `report.run_state`.
Throw-away git repositories; skipped without git. Nothing starts a VM.
Run: python3 -B -m unittest discover -s tests/unit/harness  (or this file)"""

import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, TESTS)
sys.path.insert(0, os.path.join(TESTS, "stress", "disconnect"))

from harness import report  # noqa: E402

HAVE_GIT = shutil.which("git") is not None


def git(repo, *a):
    return subprocess.run(("git", "-C", repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid") + a, check=True,
                          capture_output=True, text=True).stdout.strip()


@unittest.skipUnless(HAVE_GIT, "git is not installed")
class RunState(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.repo = os.path.join(self.tmp.name, "r")
        os.makedirs(self.repo)
        git(self.repo, "init", "-q")
        with open(os.path.join(self.repo, "a.c"), "w") as f:
            f.write("x\n")
        git(self.repo, "add", "-A")
        git(self.repo, "commit", "-q", "-m", "one")
        self.run_dir = os.path.join(self.tmp.name, "run")
        os.makedirs(self.run_dir)

    def start(self):
        return report.write_provenance(os.path.join(self.run_dir, "provenance.txt"), self.repo)

    def test_clean_run(self):
        sha, dirty = self.start()
        self.assertEqual((dirty, report.run_state(self.run_dir, self.repo)), (False, (sha, False, [])))

    def test_head_moved_during_the_run(self):
        sha, _ = self.start()
        with open(os.path.join(self.repo, "a.c"), "w") as f:
            f.write("y\n")
        git(self.repo, "commit", "-q", "-am", "two")
        got, dirty, notes = report.run_state(self.run_dir, self.repo)
        self.assertEqual(got, sha)  # the START commit, not the one at the end
        self.assertTrue(dirty)
        self.assertIn("HEAD moved", notes[0])

    def test_dirty_at_start(self):
        with open(os.path.join(self.repo, "a.c"), "w") as f:
            f.write("edit\n")
        sha, d = self.start()
        self.assertTrue(d)
        git(self.repo, "commit", "-q", "-am", "commit the edit")  # clean at the end, HEAD moved too
        self.assertTrue(report.run_state(self.run_dir, self.repo)[1])

    def test_dirty_at_end(self):
        self.start()
        with open(os.path.join(self.repo, "a.c"), "w") as f:
            f.write("edit\n")
        self.assertTrue(report.run_state(self.run_dir, self.repo)[1])

    def test_no_start_record(self):
        sha, dirty, notes = report.run_state(self.run_dir, self.repo)
        self.assertTrue(dirty)
        self.assertEqual(sha, git(self.repo, "rev-parse", "HEAD"))
        self.assertIn("no usable start record", notes[0])
        with open(os.path.join(self.run_dir, "provenance.txt"), "w") as f:
            f.write("git_sha=notasha\ndirty=false\n")
        self.assertTrue(report.run_state(self.run_dir, self.repo)[1])

    def test_provenance_command(self):
        out = os.path.join(self.run_dir, "p.txt")
        r = subprocess.run([sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "--repo", self.repo,
                            "provenance", "--out", out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertEqual(open(out).read(), "git_sha=%s\ndirty=false\n" % git(self.repo, "rev-parse", "HEAD"))

    def test_disconnect_builder_uses_the_start_commit(self):
        """end to end through a real builder: a commit made during the run does not become the tested commit"""
        import disc_result
        sha, _ = self.start()
        with open(os.path.join(self.run_dir, "plan.txt"), "w") as f:
            f.write("reps=1\nworkloads=seq\ncontrols=\nfault=\n")
        with open(os.path.join(self.run_dir, "reps.tsv"), "w") as f:
            f.write("S-DISC-01\t1\tseq\t1\tpass\t\n")
        with open(os.path.join(self.run_dir, "meta.txt"), "w") as f:
            f.write("guest_rc=0\nsetup_failed=0\n")
        with open(os.path.join(self.repo, "a.c"), "w") as f:
            f.write("y\n")
        git(self.repo, "commit", "-q", "-am", "moved")
        self.assertEqual(disc_result.finalize(self.run_dir, self.repo, "2026-11-21T10:00:00Z"), 0)
        doc = json.load(open(os.path.join(self.run_dir, "result.json")))
        self.assertEqual((doc["git_sha"], doc["dirty_tree"]), (sha, True))
        self.assertIn("HEAD moved", doc["cases"][0]["note"])
        # a result of a run that moved cannot be adopted
        with self.assertRaises(ValueError):
            report.adopt(self.run_dir, "B", os.path.join(self.tmp.name, "res"), self.repo)


class BuildCheck(unittest.TestCase):
    """the prebuilt host binaries must come from this checkout (re-review blocker)"""

    def cache(self, d, home):
        os.makedirs(d, exist_ok=True)
        with open(os.path.join(d, "CMakeCache.txt"), "w") as f:
            f.write("# cache\nCMAKE_HOME_DIRECTORY:INTERNAL=%s\n" % home)

    def test_match_mismatch_and_missing(self):
        with tempfile.TemporaryDirectory() as t:
            repo, other = os.path.join(t, "repo"), os.path.join(t, "other")
            os.makedirs(repo)
            os.makedirs(other)
            self.cache(os.path.join(t, "b1"), repo)
            self.assertIsNone(report.build_check(os.path.join(t, "b1"), repo))
            os.symlink(repo, os.path.join(t, "link"))
            self.assertIsNone(report.build_check(os.path.join(t, "b1"), os.path.join(t, "link")))
            self.cache(os.path.join(t, "b2"), other)
            self.assertIn("not for this checkout", report.build_check(os.path.join(t, "b2"), repo))
            self.assertIn("no CMakeCache.txt", report.build_check(os.path.join(t, "nobuild"), repo))
            os.makedirs(os.path.join(t, "b3"))
            open(os.path.join(t, "b3", "CMakeCache.txt"), "w").write("x=1\n")
            self.assertIn("no CMAKE_HOME_DIRECTORY", report.build_check(os.path.join(t, "b3"), repo))

    def test_command_refuses_a_foreign_tree(self):
        with tempfile.TemporaryDirectory() as t:
            self.cache(os.path.join(t, "b"), t)  # a tree configured for another directory than the repository
            r = subprocess.run([sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "build-check", "--build",
                                os.path.join(t, "b")], capture_output=True, text=True)
            self.assertEqual(r.returncode, 1)
            self.assertIn("not for this checkout", r.stderr)


class Wiring(unittest.TestCase):
    """the shell and Python parts that need a VM are checked by what they say"""

    def read(self, rel):
        with open(os.path.join(TESTS, rel)) as f:
            return f.read()

    def test_every_run_sh_records_the_start_state(self):
        for rel in ("stress/soak/mix_host.sh", "stress/leaks/run.sh", "stress/kill/run.sh", "stress/disconnect/run.sh"):
            s = self.read(rel)
            self.assertIn('report.py" provenance --out "$out/provenance.txt"', s, rel)
            # written right after the result directory is made, before any guest is cloned or built
            self.assertIn('mkdir -p "$out"\n# the commit and tree state at the START', s, rel)

    def test_host_binary_suites_rebuild_after_the_provenance(self):
        for rel in ("stress/soak/mix_host.sh", "stress/kill/run.sh", "stress/disconnect/run.sh"):
            s = self.read(rel)
            a, b, c = s.index("provenance --out"), s.index('build-check --build "$build"'), s.index('cmake --build "$build"')
            self.assertTrue(a < b < c, rel)
            self.assertIn("rebuilt with cmake --build at the recorded commit", s, rel)
            # the binaries are looked up only after the rebuild
            if 'for f in "$build/src/daemon/ntfsmacd"' in s:
                self.assertLess(c, s.index('for f in "$build/src/daemon/ntfsmacd"'), rel)

    def test_leaks_guest_output_cannot_overwrite_the_start_record(self):
        s = self.read("stress/leaks/run.sh")
        self.assertIn("--exclude ./provenance.txt --exclude provenance.txt", s)
        self.assertIn('cmp -s "$out/provenance.txt" "$tmp/provenance.keep"', s)

    def test_kill_and_leaks_list_provenance_as_evidence(self):
        self.assertIn('"provenance.txt") if os.path.isfile', self.read("stress/leaks/classify.py"))
        self.assertIn('c["evidence"].append("provenance.txt")', self.read("stress/kill/result.py"))

    def test_every_builder_takes_the_start_state(self):
        for rel in ("stress/soak/mix_result.py", "stress/leaks/classify.py", "stress/kill/result.py", "stress/disconnect/disc_result.py"):
            s = self.read(rel)
            self.assertIn("report.run_state(", s, rel)
            self.assertNotIn("report.git_info(", s, rel)


if __name__ == "__main__":
    unittest.main()
