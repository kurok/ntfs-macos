# SPDX-License-Identifier: GPL-2.0-or-later
"""Tests of the short fuzz stage's finding classifier (shortcheck.py) and of run.sh --known-ok. Python standard
library only.

Run: python3 -I -B tests/fuzz/campaign/test_shortcheck.py

Each test builds the extracted result of one fuzz target (summary.txt, leaks.log.sym, artifacts/, libFuzzer logs)
from fake logs and checks the verdict. Only known leaks of the corpus replay are forgiven (known.tsv row with a
non-regression status, as in triage.py); every artifact, bad run exit, count mismatch or unknown leak fails.
"""
import os
import re
import shutil
import subprocess
import sys
import tarfile
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
SHORTCHECK = os.path.join(HERE, "shortcheck.py")
KNOWN = os.path.join(HERE, "testdata", "known.tsv")  # frozen copy: the live known.tsv may grow
NTFS = "/tmp/b-eng-fuzz/src/libntfs-3g/"

# leaks.log.sym of the failing full CI run (symbolize.py output: "#N function at file:line")
F2 = """Direct leak of 4096 byte(s) in 1 object(s) allocated from:
    #0 ___interceptor_malloc at ??:?
    #1 ntfs_malloc at %smisc.c:57
    #2 ntfs_mapping_pairs_decompress at %srunlist.c:820
    #3 ntfs_mft_load at %svolume.c:365
    #4 ntfs_volume_startup at %svolume.c:652
    #5 ntfs_device_mount at %svolume.c:956
    #6 mount_view at /work/src/probe/probe.c:98
    #7 fuzzer::Fuzzer::ExecuteCallback(unsigned char const*, unsigned long) at :?
""" % ((NTFS,) * 5)
F5 = """Direct leak of 56 byte(s) in 1 object(s) allocated from:
    #0 ___interceptor_malloc at ??:?
    #1 ntfs_malloc at %smisc.c:57
    #2 ntfs_attr_readall at %sattrib.c:7025
    #3 ntfs_make_symlink at %sreparse.c:814
    #4 LLVMFuzzerTestOneInput at /work/tests/fuzz/image/fuzz_mountwalk.c:414
""" % ((NTFS,) * 3)
UNKNOWN_LEAK = """Direct leak of 32 byte(s) in 1 object(s) allocated from:
    #0 ___interceptor_malloc at ??:?
    #1 ntfs_malloc at %smisc.c:57
    #2 ntfs_attr_open at %sattrib.c:5000
    #3 LLVMFuzzerTestOneInput at /work/tests/fuzz/image/fuzz_mountwalk.c:414
""" % ((NTFS,) * 2)
PROJECT_LEAK = """Direct leak of 16 byte(s) in 1 object(s) allocated from:
    #0 ___interceptor_malloc at ??:?
    #1 build_name at /work/src/probe/names.c:120
    #2 LLVMFuzzerTestOneInput at /work/tests/fuzz/image/fuzz_probe.c:50
"""
CRASH_LOG = """==1==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x5020 at pc 0x1
READ of size 4 at 0x5020 thread T0
    #0 evil_read at %sdir.c:100
    #1 LLVMFuzzerTestOneInput at /work/tests/fuzz/image/fuzz_mountwalk.c:414

SUMMARY: AddressSanitizer: heap-buffer-overflow
""" % NTFS
F3_LOG = """==1==ERROR: AddressSanitizer: heap-buffer-overflow on address 0x5020 at pc 0x1
READ of size 4 at 0x5020 thread T0
    #0 ntfs_valid_descr at %sacls.c:682
    #1 getsecurityattr at %ssecurity.c:1894
""" % (NTFS, NTFS)


def write(path, text):
    with open(path, "w") as f:
        f.write(text)


UBSAN_ENGINE = ("%sbootsect.c:229:21: runtime error: left shift of 5 by 40 places cannot be represented in type 'int'\n" % NTFS)


def make_run(root, leaks=(), artifacts=None, summary_extra=(), result="FINDING (artifacts/, libfuzzer-*.log)", runs=(0,),
             counted=None, log_text=None, complete=True, failed=0):
    """The extracted result of one target in `root`. artifacts is {file name: log text of its run}; runs the exit
    codes of the libFuzzer runs; counted the leak count of the summary (default: the leak lines of the log)."""
    os.makedirs(os.path.join(root, "artifacts"), exist_ok=True)
    blocks = [x if x.startswith("===") else "=== replay input h%d (exit 1)\n%s" % (i, x) for i, x in enumerate(leaks)]
    text = ("==1==ERROR: LeakSanitizer: detected memory leaks\n\n" + "\n".join(blocks)) if leaks else ""
    n = len(re.findall(r"^(Direct|Indirect) leak of", text, re.M))
    with open(os.path.join(root, "summary.txt"), "w") as f:
        f.write("target: probe  seconds: 300\n")
        for i, rc in enumerate(runs, 1):
            f.write("run %d: exit %d after 301 s (17:25:42Z)\n" % (i, rc))
        for ln in summary_extra:
            f.write(ln + "\n")
        f.write("leak replay: inputs 495, leaking %d, failed without a leak report %d\n" %
                (len(re.findall(r"^=== replay input ", text, re.M)), failed))
        f.write("replay complete\n" if complete else "replay INCOMPLETE (time bound)\n")
        f.write("leaks in the corpus replay: %d (leaks.log.sym)\n" % (n if counted is None else counted))
        f.write("RESULT: %s\n" % result)
    if leaks or log_text is not None:
        write(os.path.join(root, "leaks.log.sym"), text if log_text is None else log_text)
    for name, log in (artifacts or {}).items():
        write(os.path.join(root, "artifacts", name), "x")
        if log is not None:
            m = name.split("-", 1)[0]
            target = "libfuzzer-%s.log.sym" % m[1:] if m.startswith("i") and m[1:].isdigit() else "libfuzzer.log"
            write(os.path.join(root, target), log)


def check(root, known=KNOWN):
    p = subprocess.run([sys.executable, "-I", "-B", SHORTCHECK, root, "--known", known], capture_output=True, text=True)
    return p.returncode, p.stdout


class Classify(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.mkdtemp()
        self.root = os.path.join(self.tmp, "t")
        self.addCleanup(shutil.rmtree, self.tmp)

    def fixed_known(self):
        """known.tsv with F2 changed to status fixed."""
        path = os.path.join(self.tmp, "known.tsv")
        with open(KNOWN) as f, open(path, "w") as g:
            for ln in f:
                cols = ln.split("\t")
                if cols[0] == "F2":
                    cols[4], cols[5] = "fixed", "0099"
                g.write("\t".join(cols))
        return path

    def test_known_leaks_pass_and_are_named(self):
        make_run(self.root, leaks=[F2, F5])
        rc, out = check(self.root)
        self.assertEqual(rc, 0, out)
        self.assertEqual(out.splitlines()[-1], "short-check: known: F2, F5")

    def test_known_leak_f2_alone(self):
        make_run(self.root, leaks=[F2])
        rc, out = check(self.root)
        self.assertEqual((rc, out.splitlines()[-1]), (0, "short-check: known: F2"))

    def test_unknown_leak_among_known_fails(self):
        make_run(self.root, leaks=[F2, UNKNOWN_LEAK, F5])
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertTrue(out.splitlines()[-1].startswith("short-check: FINDING"), out)
        self.assertIn("NEW  leak", out)

    def test_project_leak_fails(self):
        make_run(self.root, leaks=[PROJECT_LEAK])
        self.assertEqual(check(self.root)[0], 1)

    def test_regression_of_a_fixed_row_fails(self):
        """A known.tsv row with status fixed, seen on an engine that carries the patches: REGRESSION."""
        make_run(self.root, leaks=[F2, F5])
        rc, out = check(self.root, self.fixed_known())
        self.assertEqual(rc, 1, out)
        self.assertIn("REGRESSION F2", out)
        self.assertIn("known F5", out)

    def test_crash_artifact_fails_even_with_known_leaks(self):
        make_run(self.root, leaks=[F2, F5], artifacts={"i1-crash-abc": CRASH_LOG})
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("artifacts: i1-crash-abc", out)

    def test_artifact_whose_log_has_a_known_engine_ubsan_line_fails(self):
        """B1: a benign engine UBSan line (row U) in the log must not make the artifact known."""
        make_run(self.root, leaks=[F2], artifacts={"i1-crash-abc": UBSAN_ENGINE + "==1==ERROR: libFuzzer: fuzz target exited\n"})
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("artifacts: i1-crash-abc", out)

    def test_artifact_of_a_known_row_fails(self):
        make_run(self.root, leaks=[F2], artifacts={"i1-crash-abc": F3_LOG})
        self.assertEqual(check(self.root)[0], 1)

    def test_sanitize_target_artifact_without_prefix(self):
        make_run(self.root, artifacts={"crash-abc": CRASH_LOG})
        self.assertEqual(check(self.root)[0], 1)

    def test_artifact_without_log_fails(self):
        make_run(self.root, leaks=[F2], artifacts={"i2-timeout-abc": None})
        self.assertEqual(check(self.root)[0], 1)

    def test_slow_unit_artifact_fails(self):
        """B2: image slow units are named i<N>-slow-unit-*."""
        make_run(self.root, leaks=[F2], artifacts={"i1-slow-unit-abc": "Slowest unit\n"})
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("artifacts: i1-slow-unit-abc", out)

    def test_bad_run_exit_without_artifact_fails(self):
        """S1: an OOM-killed run (137) leaves no artifact; the known leak must not hide it."""
        make_run(self.root, leaks=[F2, F5], runs=(0, 137))
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("run 2 exited 137", out)

    def test_empty_leak_log_with_leaks_counted_fails(self):
        """S2."""
        make_run(self.root, counted=2, log_text="")
        self.assertEqual(check(self.root)[0], 1)

    def test_leak_count_mismatch_fails(self):
        make_run(self.root, leaks=[F2], counted=2)
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("summary counts 2", out)

    def test_unparsable_leak_log_fails(self):
        make_run(self.root, counted=1, log_text="Direct leak of 8 byte(s) in 1 object(s) allocated from:\n")
        self.assertEqual(check(self.root)[0], 1)

    def test_ids_sorted_numerically(self):
        known = os.path.join(self.tmp, "known.tsv")
        with open(KNOWN) as f, open(known, "w") as g:
            g.write(f.read().replace("F7\t", "F10\t"))
        leak10 = """Direct leak of 8 byte(s) in 1 object(s) allocated from:
    #0 ___interceptor_malloc at ??:?
    #1 ntfs_inode_open at %sinode.c:1
    #2 ntfs_device_mount at %svolume.c:1240
""" % (NTFS, NTFS)
        make_run(self.root, leaks=[leak10, F2])
        rc, out = check(self.root, known)
        self.assertEqual((rc, out.splitlines()[-1]), (0, "short-check: known: F2, F10"), out)

    def test_second_input_leak_unknown_fails(self):
        """The replay reports one input per block: a known leak first must not hide an unknown one behind it."""
        make_run(self.root, leaks=["=== replay input a (exit 1)\n" + F2, "=== replay input b (exit 1)\n" + UNKNOWN_LEAK])
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("NEW leak", out)

    def test_replay_without_complete_marker_fails(self):
        make_run(self.root, leaks=[F2, F5], complete=False)
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("replay incomplete", out)

    def test_old_summary_without_replay_lines_fails(self):
        make_run(self.root, leaks=[F2])
        with open(os.path.join(self.root, "summary.txt")) as f:
            kept = [ln for ln in f if not ln.startswith(("leak replay", "replay "))]
        write(os.path.join(self.root, "summary.txt"), "".join(kept))
        self.assertEqual(check(self.root)[0], 1)

    def test_replay_run_failed_without_leak_report_fails(self):
        make_run(self.root, leaks=["=== replay input a (exit 1)\n" + F2], failed=1)
        self.assertEqual(check(self.root)[0], 1)

    def test_leaking_count_differs_from_blocks_fails(self):
        """Summary says leaking 2, the log has one block."""
        make_run(self.root, leaks=["=== replay input a (exit 1)\n" + F2])
        with open(os.path.join(self.root, "summary.txt")) as f:
            t = f.read().replace("leaking 1,", "leaking 2,")
        write(os.path.join(self.root, "summary.txt"), t)
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("blocks", out)

    def test_asan_error_in_leak_log_fails(self):
        make_run(self.root, leaks=[F2, CRASH_LOG])
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("error in leaks.log.sym", out)

    def test_libfuzzer_error_in_leak_log_fails(self):
        make_run(self.root, leaks=[F2, "==1==ERROR: libFuzzer: deadly signal\n"])
        self.assertEqual(check(self.root)[0], 1)

    def test_engine_ubsan_line_in_leak_log_is_fine(self):
        make_run(self.root, leaks=[UBSAN_ENGINE + F2])
        self.assertEqual(check(self.root)[0], 0)

    def test_non_finding_result_line_fails(self):
        make_run(self.root, leaks=[F2], summary_extra=["RESULT: no finding in 300 s"])
        self.assertEqual(check(self.root)[0], 1)

    def test_failed_run_without_any_finding_fails(self):
        make_run(self.root)
        rc, out = check(self.root)
        self.assertEqual(rc, 1, out)
        self.assertIn("counts no leak", out)

    def test_run_without_finding_result_is_not_classified(self):
        make_run(self.root, leaks=[F2], result="engine build failed")
        self.assertEqual(check(self.root)[0], 1)

    def test_real_ci_run_if_present(self):
        """The result of the failing full CI run: F2 on probe, F5 on mountwalk, nothing unknown."""
        base = os.environ.get("NTFSMAC_SHORTCHECK_REAL")
        if not base or not os.path.isdir(base):
            self.skipTest("NTFSMAC_SHORTCHECK_REAL not set")
        for t, want in (("probe", "F2"), ("mountwalk", "F5")):
            rc, out = check(os.path.join(base, t))
            self.assertEqual((rc, out.splitlines()[-1]), (0, "short-check: known: " + want), out)


class RunScript(unittest.TestCase):
    """tests/fuzz/image/run.sh and tests/fuzz/sanitize/run.sh --known-ok, with a fake vm/linux/run.sh that returns a
    prepared result tar and exit status."""

    def tree(self, script):
        tmp = tempfile.mkdtemp()
        self.addCleanup(shutil.rmtree, tmp)
        for d in ("tests/fuzz/image", "tests/fuzz/sanitize", "vm/linux"):
            os.makedirs(os.path.join(tmp, d))
        shutil.copy(os.path.join(REPO, "tests/fuzz", script, "run.sh"), os.path.join(tmp, "tests/fuzz", script, "run.sh"))
        # a real directory of symlinks: the modules, and known.tsv pointing at the frozen copy (the scripts default to
        # known.tsv next to shortcheck.py, found by abspath, not realpath), so a row appended to the live file cannot
        # change these tests
        cam = os.path.join(tmp, "tests/fuzz/campaign")
        os.makedirs(cam)
        for name in sorted(os.listdir(HERE)):
            if name.endswith(".py") and not name.startswith("test_"):
                os.symlink(os.path.join(HERE, name), os.path.join(cam, name))
        os.symlink(KNOWN, os.path.join(cam, "known.tsv"))
        return tmp

    def run_script(self, script, tmp, tgz, rc, *opts):
        with open(os.path.join(tmp, "vm/linux/run.sh"), "w") as f:
            f.write('#!/bin/sh\ncat "%s"\nexit %d\n' % (tgz, rc))
        target = {"image": "probe", "sanitize": "label"}[script]
        out = os.path.join(tmp, "out")
        p = subprocess.run(["sh", os.path.join(tmp, "tests/fuzz", script, "run.sh"), "--target", target, "--out", out] + list(opts),
                           capture_output=True, text=True)
        return p.returncode, p.stdout + p.stderr, out, target

    def tgz(self, tmp, **kw):
        d = os.path.join(tmp, "res")
        make_run(d, **kw)
        path = os.path.join(tmp, "res.tgz")
        with tarfile.open(path, "w:gz") as t:
            t.add(d, arcname=".")
        return path

    def test_known_ok_passes_on_known_leaks_only(self):
        tmp = self.tree("image")
        rc, out, outdir, t = self.run_script("image", tmp, self.tgz(tmp, leaks=[F2, F5]), 1, "--known-ok")
        self.assertEqual(rc, 0, out)
        self.assertIn("known findings only: F2,F5", out)
        with open(os.path.join(outdir, t + ".known")) as f:
            self.assertEqual(f.read().strip(), "F2,F5")

    def test_known_ok_is_a_noop_for_sanitize(self):
        """Project code: no leak replay, a failed run is never forgiven."""
        tmp = self.tree("sanitize")
        rc, out, _, _ = self.run_script("sanitize", tmp, self.tgz(tmp, artifacts={"crash-abc": CRASH_LOG}), 1, "--known-ok")
        self.assertEqual(rc, 1, out)

    def test_without_known_ok_a_leak_still_fails(self):
        tmp = self.tree("image")
        rc, out, _, _ = self.run_script("image", tmp, self.tgz(tmp, leaks=[F2]), 1)
        self.assertEqual(rc, 1, out)

    def test_known_ok_fails_on_unknown_finding(self):
        tmp = self.tree("image")
        rc, out, outdir, t = self.run_script("image", tmp, self.tgz(tmp, leaks=[F2, UNKNOWN_LEAK]), 1, "--known-ok")
        self.assertEqual(rc, 1, out)
        self.assertFalse(os.path.exists(os.path.join(outdir, t + ".known")))

    def test_stale_known_file_without_known_ok_fails(self):
        """S3: leftovers of an earlier run in --out must not turn a failure into a pass."""
        tmp = self.tree("image")
        os.makedirs(os.path.join(tmp, "out"))
        write(os.path.join(tmp, "out", "probe.known"), "F2\n")
        rc, out, _, _ = self.run_script("image", tmp, self.tgz(tmp, leaks=[UNKNOWN_LEAK]), 1)
        self.assertEqual(rc, 1, out)

    def test_stale_known_file_with_known_ok_is_removed(self):
        tmp = self.tree("image")
        os.makedirs(os.path.join(tmp, "out"))
        write(os.path.join(tmp, "out", "probe.known"), "F2\n")
        rc, out, _, _ = self.run_script("image", tmp, self.tgz(tmp, leaks=[UNKNOWN_LEAK]), 1, "--known-ok")
        self.assertEqual(rc, 1, out)

    def test_known_ok_with_bad_run_exit_fails(self):
        tmp = self.tree("image")
        rc, out, _, _ = self.run_script("image", tmp, self.tgz(tmp, leaks=[F2], runs=(137,)), 1, "--known-ok")
        self.assertEqual(rc, 1, out)

    def test_known_ok_does_not_hide_a_failed_container_run(self):
        tmp = self.tree("image")
        rc, out, _, _ = self.run_script("image", tmp, self.tgz(tmp, leaks=[F2]), 3, "--known-ok")
        self.assertEqual(rc, 1, out)


if __name__ == "__main__":
    unittest.main()
