# SPDX-License-Identifier: GPL-2.0-or-later
"""Synthetic-log tests of the campaign tooling (T-M5-09 review). Python standard library only.

Run: python3 -I -B tests/fuzz/campaign/test_campaign.py

Each test builds a small log or result tree and checks fuzzsig.py or triage.py; each one fails on the tooling
as it was before the review fix it names (R1 to R10 are the numbered review findings).
"""
import os
import re
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import fuzzsig  # noqa: E402

KNOWN = os.path.join(HERE, "known.tsv")
UB229 = "/tmp/b-eng-fuzz/src/libntfs-3g/bootsect.c:229:21: runtime error: left shift of 5 by 40 places cannot be represented in type 'int'"


def asan(kind, access, frames):
    lines = ["==1==ERROR: AddressSanitizer: %s on address 0x5020 at pc 0x1" % kind]
    if access:
        lines.append("%s of size 4 at 0x5020 thread T0" % access)
    for i, (fn, loc) in enumerate(frames):
        lines.append("    #%d %s at %s" % (i, fn, loc))
    lines.append("")
    lines.append("SUMMARY: AddressSanitizer: %s" % kind)
    return lines


def sig(lines):
    with tempfile.NamedTemporaryFile("w", suffix=".log", delete=False) as f:
        f.write("\n".join(lines) + "\n")
    try:
        out = subprocess.run([sys.executable, "-I", "-B", os.path.join(HERE, "fuzzsig.py"), "sig", f.name],
                             capture_output=True, text=True, check=True).stdout
    finally:
        os.unlink(f.name)
    return out.rstrip("\n").split("\t") if out.strip() else None


def known_id(s):
    r = fuzzsig.match_known(fuzzsig.load_known(KNOWN), s[0], s[1], s[-1])
    return r["id"] if r else None


NTFS = "/tmp/b-eng-fuzz/src/libntfs-3g/"
F3_FRAMES = [("___interceptor_memcpy", "x"), ("ntfs_valid_descr", NTFS + "acls.c:682"),
             ("getsecurityattr", NTFS + "security.c:1894"), ("ntfs_get_ntfs_acl", NTFS + "security.c:2000")]


class Sig(unittest.TestCase):
    def test_r1_ubsan_before_fatal_event(self):
        """R1: a recoverable UBSan line printed first must not hide the crash that follows."""
        s = sig([UB229] + asan("heap-buffer-overflow", "WRITE", [("evil_write", NTFS + "dir.c:100")]))
        self.assertEqual(s[1], "asan:heap-buffer-overflow")
        self.assertEqual(s[2], "WRITE")
        self.assertIsNone(known_id(s))

    def test_r1_ubsan_alone(self):
        s = sig([UB229])
        self.assertEqual(s[1], "ubsan")
        self.assertEqual(known_id(s), "U")

    def test_r1_ubsan_before_oracle(self):
        s = sig([UB229, "F-CTL-01 oracle failure: x (line 5)", "ERROR: libFuzzer: deadly signal"])
        self.assertEqual(s[1], "oracle")

    def test_r6_direct_leak(self):
        """R6: a leak in a replay log is a leak signature with its frames."""
        s = sig(["ERROR: LeakSanitizer: detected memory leaks", "Direct leak of 136 byte(s) in 1 object(s) allocated from:",
                 "    #0 ___interceptor_calloc at ??:?", "    #1 ntfs_calloc at " + NTFS + "misc.c:60",
                 "    #2 ntfs_inode_open at " + NTFS + "inode.c:95", "    #3 ntfs_device_mount at " + NTFS + "volume.c:1240"])
        self.assertEqual(s[1], "leak")
        self.assertEqual(s[3], "engine")
        self.assertEqual(known_id(s), "F7")

    def test_r8_key_has_access_and_location(self):
        r = sig(asan("heap-buffer-overflow", "READ", F3_FRAMES))
        w = sig(asan("heap-buffer-overflow", "WRITE", F3_FRAMES))
        self.assertNotEqual(r[0], w[0])
        self.assertIn("acls.c:682", r[0])
        self.assertEqual(known_id(r), "F3")

    def test_r8_known_anchored_to_top_frame(self):
        """R8: ntfs_valid_descr three frames down is not F3; the crash is somewhere else."""
        s = sig(asan("heap-buffer-overflow", "WRITE", [("other_fn", NTFS + "dir.c:9"), ("x", NTFS + "dir.c:8"),
                                                       ("ntfs_valid_descr", NTFS + "acls.c:682")]))
        self.assertIsNone(known_id(s))

    def test_r3_ubsan_line_anchors(self):
        rows = fuzzsig.load_known(KNOWN)
        m = lambda t: fuzzsig.match_known(rows, "", "ubsan", t)  # noqa: E731
        self.assertIsNotNone(m("volume.c:273"))
        self.assertIsNone(m("volume.c:2731"))
        self.assertIsNone(m("reparse.c:999"))
        self.assertIsNotNone(m("reparse.c:848"))

    def test_r2_m501_is_fixed(self):
        rows = fuzzsig.load_known(KNOWN)
        r = [x for x in rows if x["id"] == "M5-01"][0]
        self.assertEqual((r["status"], r["component"]), ("fixed", "project"))


def write_unit(root, name, summary, findings=(), ubsan=(), inputs=None):
    d = os.path.join(root, "units", name)
    os.makedirs(os.path.join(d, "inputs"))
    with open(os.path.join(d, "summary.txt"), "w") as f:
        f.write(summary)
    with open(os.path.join(d, "findings.tsv"), "w") as f:
        for row in findings:
            f.write("\t".join(row) + "\n")
    with open(os.path.join(d, "ubsan.tsv"), "w") as f:
        for u in ubsan:
            f.write(u + "\n")
    for k, v in (inputs or {}).items():
        with open(os.path.join(d, "inputs", k), "wb") as f:
            f.write(v)


def summary(patches="series", cpu=100, result="no finding in 100 s"):
    return ("unit: u\nengine-patches: %s\nfuzz CPU seconds: %d\nexecutions in all runs: 10\ncorpus after the run: 3 inputs\n"
            "last progress line: #1 DONE cov: 5\nRESULT: %s\n" % (patches, cpu, result))


def triage(root, *extra):
    p = subprocess.run([sys.executable, "-I", "-B", os.path.join(HERE, "triage.py"), "--results", root, "--known", KNOWN] + list(extra),
                       capture_output=True, text=True)
    return p.returncode, p.stdout


def frow(uid, key, kind, access, comp, frames, cnt="1"):
    return (uid, key, kind, access, comp, "-", frames, cnt, "4", "4", "minimised")


class Triage(unittest.TestCase):
    def test_r3_new_ubsan_location_fails(self):
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "mountwalk-s0", summary(), ubsan=["3\tdir.c:123 left shift of N by N places"])
            rc, out = triage(t)
            self.assertEqual(rc, 1)
            self.assertIn("1 new UBSan", out)

    def test_r3_known_ubsan_passes(self):
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "mountwalk-s0", summary(), ubsan=["3\tvolume.c:273 left shift of N by N places"])
            self.assertEqual(triage(t)[0], 0)

    def test_r4_missing_planned_unit(self):
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "mountwalk-s0", summary())
            rc, out = triage(t, "--plan", "mountwalk:0 probe:0")
            self.assertEqual(rc, 1)
            self.assertIn("probe:0", out)

    def test_r4_unit_without_result(self):
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "label-s0", "unit: label-s0\nfuzz CPU seconds: 5\n")
            self.assertEqual(triage(t)[0], 1)

    def test_r5_exit_without_artifact_is_review(self):
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "mountwalk-s0", summary(), [frow("a", "not-reproducible exit-137-noartifact", "other", "-", "unknown",
                                                        "-")])
            rc, out = triage(t)
            self.assertEqual(rc, 1)
            self.assertIn("REVIEW", out)

    def test_r7_budget(self):
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "label-s0", summary(cpu=40))
            self.assertEqual(triage(t, "--budget", "100")[0], 1)
            self.assertEqual(triage(t, "--budget", "40")[0], 0)

    def test_r2_fixed_project_finding_is_regression_everywhere(self):
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "label-s0", summary(patches="none"),
                       [frow("a", "oracle: argv_check accepted a leaf that ADR-016 refuses", "oracle", "-", "project",
                             "argv_check accepted a leaf that ADR-016 refuses")])
            rc, out = triage(t)
            self.assertEqual(rc, 1)
            self.assertIn("REGRESSION", out)

    def test_pristine_engine_known_fixed_is_expected(self):
        f3 = frow("a", "asan:heap-buffer-overflow READ ntfs_valid_descr < getsecurityattr @acls.c:682", "asan:heap-buffer-overflow",
                  "READ", "engine", "ntfs_valid_descr@acls.c:682 < getsecurityattr@security.c:1894")
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "mountwalk-s0", summary(patches="none"), [f3])
            self.assertEqual(triage(t)[0], 0)
        with tempfile.TemporaryDirectory() as t:
            write_unit(t, "mountwalk-s0", summary(patches="series"), [f3])
            self.assertEqual(triage(t)[0], 1)

    def test_r10_adopt_stable_and_no_overwrite(self):
        row = frow("mountwalk-s0-01", "asan:heap-buffer-overflow WRITE zzz @dir.c:1", "asan:heap-buffer-overflow", "WRITE", "engine",
                   "zzz@dir.c:1")
        ids = []
        for payload in (b"one", b"two"):
            with tempfile.TemporaryDirectory() as t, tempfile.TemporaryDirectory() as fd:
                write_unit(t, "mountwalk-s0", summary(), [row], inputs={"mountwalk-s0-01.bin": payload})
                rc, out = triage(t, "--findings-dir", fd, "--adopt")
                names = os.listdir(fd)
                self.assertEqual(len(names), 1)
                ids.append(re.match(r"N-[0-9a-f]{6}", names[0]).group(0))
        self.assertEqual(ids[0], ids[1])
        with tempfile.TemporaryDirectory() as t, tempfile.TemporaryDirectory() as fd:
            write_unit(t, "mountwalk-s0", summary(), [row], inputs={"mountwalk-s0-01.bin": b"new"})
            triage(t, "--findings-dir", fd, "--adopt")
            name = os.path.join(fd, os.listdir(fd)[0])
            with open(name, "wb") as f:
                f.write(b"edited")
            triage(t, "--findings-dir", fd, "--adopt")
            self.assertEqual(open(name, "rb").read(), b"edited")


class Result(unittest.TestCase):
    """M5-F05: the campaign writes an ntfsmac-test-result/1 file with F-CAMPAIGN-01 (fake triage inputs, no run)."""

    TARGETS = ["probe", "mountwalk", "verdict", "label", "display", "ctl"]

    def setUp(self):
        import shutil
        if shutil.which("git") is None:
            self.skipTest("git is not installed")
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.repo = os.path.join(self._tmp.name, "repo")
        os.makedirs(os.path.join(self.repo, "tests", "fuzz", "campaign"))
        for rel, text in (("src.c", "x\n"), ("tests/fuzz/campaign/known.tsv", "a\n")):
            with open(os.path.join(self.repo, rel), "w") as f:
                f.write(text)
        self.git("init", "-q")
        self.git("add", "-A")
        self.git("commit", "-q", "-m", "fuzzed")
        self.SHA = self.git("rev-parse", "HEAD")

    def git(self, *a):
        return subprocess.run(("git", "-C", self.repo, "-c", "user.name=t", "-c", "user.email=t@example.invalid") + a, check=True,
                              capture_output=True, text=True).stdout.strip()

    def edit(self, rel, text, commit=None):
        with open(os.path.join(self.repo, rel), "w") as f:
            f.write(text)
        if commit:
            self.git("commit", "-q", "-am", commit)

    def tree(self, t, dirty="false", end_sha=None, end_dirty="false", budget=28800, patches="series", targets=None):
        for tg in targets or self.TARGETS:
            write_unit(t, "%s-s0" % tg, summary(patches=patches, cpu=budget // len(targets or self.TARGETS)))
        with open(os.path.join(t, "provenance.txt"), "w") as f:
            f.write("git_sha=%s\ndirty=%s\nstarted=2026-11-21T10:00:00Z\nbudget=%d\n" % (self.SHA, dirty, budget))
        if end_sha != "none":
            with open(os.path.join(t, "provenance-end.txt"), "w") as f:
                f.write("git_sha=%s\ndirty=%s\n" % (end_sha or self.SHA, end_dirty))

    def run_triage(self, t, budget=28800, targets=None, extra=(), findings=()):
        plan = " ".join("%s:0" % x for x in (targets or self.TARGETS))
        rc, out = triage(t, "--plan", plan, "--budget", str(budget), "--result-out", os.path.join(t, "result.json"),
                         "--repo", self.repo, *extra)
        doc = None
        if os.path.exists(os.path.join(t, "result.json")):
            import json
            doc = json.load(open(os.path.join(t, "result.json")))
        return rc, out, doc

    def test_all_classified_criterion_passes(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            rc, out, doc = self.run_triage(t)
            self.assertEqual(rc, 0, out)
            self.assertEqual([c["id"] for c in doc["cases"]], ["F-CAMPAIGN-01"])
            self.assertEqual(doc["cases"][0]["status"], "pass")
            self.assertEqual((doc["git_sha"], doc["dirty_tree"], doc["env"]), (self.SHA, False, "A"))
            sys.path.insert(0, os.path.join(HERE, "..", "..", "harness"))
            import report
            self.assertEqual(report.validate(doc), [])

    def test_unclassified_crash_fails(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            with open(os.path.join(t, "units", "ctl-s0", "findings.tsv"), "w") as f:
                f.write("\t".join(frow("a", "asan:heap-buffer-overflow WRITE zzz @dir.c:1", "asan:heap-buffer-overflow", "WRITE",
                                       "engine", "zzz@dir.c:1")) + "\n")
            rc, out, doc = self.run_triage(t)
            self.assertEqual(rc, 1)
            c = doc["cases"][0]
            self.assertEqual((c["id"], c["status"]), ("F-CAMPAIGN-01", "fail"))
            self.assertIn("not every crash is classified", c["note"])

    def test_short_budget_is_not_the_criterion_id(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t, budget=600)
            rc, out, doc = self.run_triage(t, budget=600)
            self.assertEqual(rc, 0, out)
            self.assertEqual([c["id"] for c in doc["cases"]], ["F-CAMPAIGN-01-SHORT"])
            self.assertIn("shortened or weaker run", doc["cases"][0]["note"])

    def test_missing_target_or_pristine_engine_is_not_the_criterion_id(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t, targets=self.TARGETS[:5])
            self.assertEqual(self.run_triage(t, targets=self.TARGETS[:5])[2]["cases"][0]["id"], "F-CAMPAIGN-01-SHORT")
        with tempfile.TemporaryDirectory() as t:
            self.tree(t, patches="none")
            self.assertEqual(self.run_triage(t)[2]["cases"][0]["id"], "F-CAMPAIGN-01-SHORT")

    def test_unit_missing_or_failed_unit_fails_the_case(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t, targets=self.TARGETS[:5])
            c = self.run_triage(t, targets=self.TARGETS, budget=28800)[2]["cases"][0]
            self.assertEqual(c["status"], "fail")
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            c = self.run_triage(t, extra=("--problem", "a unit failed"))[2]["cases"][0]
            self.assertEqual((c["id"], c["status"]), ("F-CAMPAIGN-01", "fail"))

    def test_dirty_tree_refused_unless_allowed(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t, dirty="true")
            rc, out, doc = self.run_triage(t)
            self.assertEqual(rc, 2)
            self.assertIsNone(doc)
        with tempfile.TemporaryDirectory() as t:
            self.tree(t, dirty="true")
            rc, out, doc = self.run_triage(t, extra=("--allow-dirty",))
            self.assertTrue(doc["dirty_tree"])

    def test_head_moved_or_end_state_missing_marks_dirty(self):
        for kw in ({"end_sha": "f" * 40}, {"end_dirty": "true"}, {"end_sha": "none"}):
            with tempfile.TemporaryDirectory() as t:
                self.tree(t, **kw)
                doc = self.run_triage(t)[2]
                self.assertTrue(doc["dirty_tree"], kw)
                self.assertEqual(doc["cases"][0]["status"], "fail", kw)

    def test_no_provenance_no_result(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            os.unlink(os.path.join(t, "provenance.txt"))
            rc, out, doc = self.run_triage(t)
            self.assertEqual(rc, 2)
            self.assertIsNone(doc)

    def test_retriage_after_classification_commit_passes_at_head(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            self.edit("tests/fuzz/campaign/known.tsv", "a\nb\n", "classify")
            head = self.git("rev-parse", "HEAD")
            rc, out, doc = self.run_triage(t)
            self.assertEqual(rc, 0, out)
            self.assertEqual((doc["git_sha"], doc["dirty_tree"]), (head, False))
            c = doc["cases"][0]
            self.assertEqual((c["id"], c["status"]), ("F-CAMPAIGN-01", "pass"))
            self.assertIn("fuzzed at " + self.SHA, c["note"])

    def test_retriage_after_a_code_change_is_dirty(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            self.edit("src.c", "changed\n", "code")
            doc = self.run_triage(t)[2]
            self.assertTrue(doc["dirty_tree"])
            self.assertEqual(doc["git_sha"], self.SHA)
            self.assertEqual(doc["cases"][0]["status"], "fail")
            self.assertIn("differs from the fuzzed commit", doc["cases"][0]["note"])
            self.assertIn("src.c", doc["cases"][0]["note"])

    def test_retriage_with_uncommitted_known_tsv_is_dirty(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            self.edit("tests/fuzz/campaign/known.tsv", "a\nb\n")  # not committed
            doc = self.run_triage(t)[2]
            self.assertTrue(doc["dirty_tree"])
            self.assertEqual(doc["cases"][0]["status"], "fail")
            self.assertIn("dirty now", doc["cases"][0]["note"])

    def retriage_with(self, files):
        """commit the given {path: text} after the fuzzed commit, then re-triage"""
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            for rel, text in files.items():
                os.makedirs(os.path.dirname(os.path.join(self.repo, rel)), exist_ok=True)
                with open(os.path.join(self.repo, rel), "w") as f:
                    f.write(text)
            self.git("add", "-A")
            self.git("commit", "-q", "-m", "classification")
            return self.run_triage(t)[2]

    def test_retriage_allows_findings_with_known_tsv(self):
        doc = self.retriage_with({"tests/fuzz/campaign/known.tsv": "a\nb\n", "tests/fuzz/findings/N-abc123-ctl.bin": "x",
                                  "docs/SECURITY_FINDINGS.md": "t\n", "docs/audits/M5.md": "t\n"})
        self.assertFalse(doc["dirty_tree"])
        self.assertEqual(doc["cases"][0]["status"], "pass")

    def test_retriage_rejects_lookalike_and_other_paths(self):
        for rel in ("tests/fuzz/campaign/known.tsv.bak", "docs/TEST_STRATEGY.md", "CHECKPOINT.md.old", "tests/fuzz/campaign/triage.py"):
            doc = self.retriage_with({rel: "x\n"})
            self.assertTrue(doc["dirty_tree"], rel)
            self.assertEqual(doc["cases"][0]["status"], "fail", rel)
            self.assertIn(os.path.basename(rel), doc["cases"][0]["note"], rel)

    def test_retriage_result_can_be_adopted_and_a_forged_record_cannot(self):
        sys.path.insert(0, os.path.join(HERE, "..", "..", "harness"))
        import report
        with tempfile.TemporaryDirectory() as t, tempfile.TemporaryDirectory() as res:
            self.tree(t)
            self.edit("tests/fuzz/campaign/known.tsv", "a\nb\n", "classify")
            head = self.git("rev-parse", "HEAD")
            rc, out, doc = self.run_triage(t)
            self.assertEqual((rc, doc["git_sha"]), (0, head), out)
            rt = open(os.path.join(t, "retriage.txt")).read()
            self.assertEqual(rt, "git_sha=%s\nfuzzed=%s\n" % (head, self.SHA))
            # a forged record: the right commit, a wrong fuzzed commit
            with open(os.path.join(t, "retriage.txt"), "w") as f:
                f.write("git_sha=%s\nfuzzed=%s\n" % (head, "e" * 40))
            with self.assertRaises(ValueError) as cm:
                report.adopt(t, "A", res, self.repo)
            self.assertIn("differs from the start record", str(cm.exception))
            os.unlink(os.path.join(t, "retriage.txt"))  # no record at all: refused as well
            with self.assertRaises(ValueError):
                report.adopt(t, "A", res, self.repo)
            with open(os.path.join(t, "retriage.txt"), "w") as f:
                f.write(rt)
            os.utime(os.path.join(t, "retriage.txt"))
            path = report.adopt(t, "A", res, self.repo)
            adopted = __import__("json").load(open(path))
            self.assertEqual(adopted["git_sha"], head)
            self.assertEqual([c["id"] for c in adopted["cases"]], ["F-CAMPAIGN-01", "STAGE-A-MODE"])

    def test_plain_run_has_no_retriage_record_and_drops_a_stale_one(self):
        with tempfile.TemporaryDirectory() as t:
            self.tree(t)
            with open(os.path.join(t, "retriage.txt"), "w") as f:
                f.write("stale\n")
            self.run_triage(t)
            self.assertFalse(os.path.exists(os.path.join(t, "retriage.txt")))

    def test_run_sh_wires_it(self):
        run = open(os.path.join(HERE, "run.sh")).read()
        for word in ("provenance.txt", "provenance-end.txt", "--result-out", "--allow-dirty", "report.py"):
            self.assertIn(word, run)


class Scripts(unittest.TestCase):
    """The shell parts need Docker; their fixes are checked by what the scripts say."""

    def read(self, name):
        return open(os.path.join(HERE, name)).read()

    def test_r4_trap_and_rc_handling(self):
        self.assertIn("trap finish_tar EXIT", self.read("inner.sh"))
        run = self.read("run.sh")
        self.assertIn("RESULT: FINDING", run)
        self.assertIn('--plan "', run)

    def test_r5_r7_inner(self):
        inner = self.read("inner.sh")
        self.assertIn("noartifact", inner)
        self.assertIn("cpu_ms", inner)
        self.assertNotIn("SKIP_KNOWN", inner)
        run = self.read("run.sh")
        self.assertIn("docker info --format '{{.NCPU}}'", run)
        self.assertIn("MemTotal", run)

    def test_r10_header(self):
        self.assertNotIn("no network", self.read("run.sh"))


if __name__ == "__main__":
    unittest.main()
