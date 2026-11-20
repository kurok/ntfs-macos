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
