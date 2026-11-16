#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""U-VMK-01..10 (docs/TEST_STRATEGY.md), the crash table of docs/ARCHITECTURE.md section 3.6 and
the vmk-split image for U15 (T-M3-12). Stdlib only.

Runs the built ntfsmac-volmark over copies of the fixture images (Environment A:
tests/unit/volmark/run.sh makes them in the container and runs this test there). The device is
always a copy, opened O_RDWR and passed as descriptor 3, as the daemon does. Every run checks
what changed against the copy taken before: nothing at all for a refusal, nothing outside the two
copies of MFT record 3 ($Volume) for a write.

The cut tests use volmark_test built against the fault-layer libntfs-3g (patch 0004,
--enable-faultio, --volmark-fault): plan `count` first, then `cut:N`, `short:N:K`, `skip:N`, `eio:N`
and `dead:N` over every write, with NTFSMAC_FAULT_KILL=1 (the process dies at the cut, as at a
power loss) and without (the process goes on and must not report success). Each outcome is
classified from the two copies of record 3 and checked against its row of the crash table and
against a fresh probe verdict.
"""
import argparse
import hashlib
import json
import os
import shutil
import signal
import subprocess
import sys
import tempfile
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "tests", "unit", "probe"))
sys.path.insert(0, os.path.join(REPO, "tests", "fixtures", "gen"))
from ntfsrec import geometry, set_volume_flags  # noqa: E402
from vmk_make import (VOLUME_IS_DIRTY, boot_ids, classify, flags_offset, make, next_usn,  # noqa: E402
                      outside_record3, read_record, record3_report, unprotect_checked)

ARGS = None
FIX = {}
MANIFEST = {}
TMP = None
EVIDENCE = []   # lines for the evidence file (crash table, counts, vmk-split)

RESIZE_LOG_FILE = 0x0002
MOUNTED_ON_NT4 = 0x0008
MODIFIED_BY_CHKDSK = 0x8000

# Expected exit of `set` on a fixture whose probe state is not clean (section 3.6 steps 1 and 3,
# U-VMK-02, U-VMK-03). hiber-wake and hiber-case: exit 0 is expected (U-VMK-03: volmark applies the
# engine's rules; the probe is what refuses them).
SET_EXIT_BY_STATE = {"clean": 0, "dirty_flag": 11, "unclean_log": 10, "fast_startup": 10, "hibernated": 10,
                     "corrupt": 10, "unsupported": 10, "mirror_split": 10}
SET_EXIT_OVERRIDE = {"hiber-wake": 0, "hiber-unknown": 0, "hiber-case": 0}


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


_n = [0]


def copy(src, tag="img"):
    _n[0] += 1
    dst = os.path.join(TMP, "%s-%d.img" % (tag, _n[0]))
    shutil.copyfile(src, dst)
    return dst


def fx(name):
    if name not in FIX:  # a missing fixture is a failure: nothing is skipped silently
        raise AssertionError("fixture %s not present (tests/unit/volmark/run.sh makes them)" % name)
    return FIX[name]


def run_volmark(img, mode, tool=None, serial=None, sectors=None, env=None, extra=(), redirect="<>"):
    """Run volmark with `img` as descriptor 3 (O_RDWR unless redirect says otherwise)."""
    tool = tool or ARGS.volmark
    if serial is None or sectors is None:
        s, n = boot_ids(img)
        serial = s if serial is None else serial
        sectors = n if sectors is None else sectors
    argv = [tool, mode, "--fd", "3", "--expect-serial", serial, "--expect-sectors", str(sectors)] + list(extra)
    full_env = dict(os.environ)
    for k in list(full_env):
        if k.startswith("NTFSMAC_FAULT_"):
            del full_env[k]
    full_env.update(env or {})
    return subprocess.run(["sh", "-c", 'img=$1; shift; exec "$@" 3%s"$img"' % redirect, "sh", img] + argv,
                          env=full_env, capture_output=True)


def probe(img):
    """A probe verdict of the image (read-only descriptor; the image must not change)."""
    before = sha256(img)
    r = subprocess.run(["sh", "-c", 'img=$1; shift; exec "$@" 3<"$img"', "sh", img, ARGS.probe, "--fd", "3"],
                       capture_output=True)
    assert sha256(img) == before, "the probe changed the image"
    assert r.returncode == 0, "probe exit %d: %r" % (r.returncode, r.stderr[-200:])
    return json.loads(r.stdout)


def disk_flags(img):
    """(flags of the $MFT copy, flags of the $MFTMirr copy) of record 3; both must be valid."""
    um = unprotect_checked(read_record(img, "mft"))
    ur = unprotect_checked(read_record(img, "mir"))
    assert um and ur, "a copy of record 3 is torn"
    return (int.from_bytes(um[0][flags_offset(um[0]):flags_offset(um[0]) + 2], "little"),
            int.from_bytes(ur[0][flags_offset(ur[0]):flags_offset(ur[0]) + 2], "little"))


def ntfsinfo_flags(img):
    """The `Volume Flags:` line of ntfsinfo -m (with -f: the volume may be dirty); on a scratch copy."""
    c = copy(img, "ntfsinfo")
    r = subprocess.run(["ntfsinfo", "-f", "-m", c], capture_output=True, text=True)
    assert r.returncode == 0, "ntfsinfo -m failed: %r" % r.stderr[-200:]
    assert sha256(c) == sha256(img), "ntfsinfo changed the image"
    for line in r.stdout.splitlines():
        if line.strip().startswith("Volume Flags:"):
            return line.split(":", 1)[1].strip()
    raise AssertionError("no Volume Flags line")


def txf_resident(img):
    r = subprocess.run(["ntfsinfo", "-F", "/", "-f", copy(img, "txf")], capture_output=True, text=True)
    seen = False
    for line in r.stdout.splitlines():
        if "LOGGED_UTILITY_STREAM" in line:
            seen = True
        if seen and "Resident:" in line:
            return line.split()[1]
    return None


def fault_env(plan, kill=False, state=None, trace=None):
    e = {"NTFSMAC_FAULT_PLAN": plan}
    if kill:
        e["NTFSMAC_FAULT_KILL"] = "1"
    if state:
        e["NTFSMAC_FAULT_STATE"] = state
    if trace:
        e["NTFSMAC_FAULT_TRACE"] = trace
    return e


def fault_writes(r):
    """The write count of the fault layer's exit line on stderr ("ntfsmac-faultio: plan=... writes=N")."""
    for line in r.stderr.decode("utf-8", "replace").splitlines():
        if line.startswith("ntfsmac-faultio:") and "writes=" in line:
            return int(line.rsplit("writes=", 1)[1])
    raise AssertionError("no fault-layer line on stderr: %r" % r.stderr[-300:])


class Base(unittest.TestCase):
    def assertRefused(self, img, mode, rc, **kw):
        before = sha256(img)
        r = run_volmark(img, mode, **kw)
        self.assertEqual(r.returncode, rc, "%s: exit %d, stderr %r" % (mode, r.returncode, r.stderr[-300:]))
        self.assertEqual(r.stdout, b"", "a refusal printed a progress mark")
        self.assertEqual(sha256(img), before, "a refusal changed the image")
        return r

    def assertMarked(self, before, img, mode, want_flags, **kw):
        """`mode` exits 0 with `w` and `d`; only the two copies of record 3 change; both carry
        want_flags; the record differs from `before` only in the flags word and the USN."""
        r = run_volmark(img, mode, **kw)
        self.assertEqual(r.returncode, 0, "%s: exit %d, stderr %r" % (mode, r.returncode, r.stderr[-300:]))
        self.assertEqual(r.stdout, b"w\nd\n")
        self.assertEqual(outside_record3(before, img), [], "bytes outside the two copies of record 3 changed")
        self.assertEqual(disk_flags(img), (want_flags, want_flags))
        self.assertEqual(read_record(img, "mft"), read_record(img, "mir"), "the two copies differ")
        self.assertRecordOnlyFlagsAndUsn(before, img)
        return r

    def assertRecordOnlyFlagsAndUsn(self, before, after):
        """Record 3 of `after` equals that of `before` except the flags word and the USN (and the sector
        tails that carry it), and the USN moved on."""
        b = unprotect_checked(read_record(before, "mft"))
        a = unprotect_checked(read_record(after, "mft"))
        self.assertTrue(a and b)
        usa_ofs = int.from_bytes(a[0][4:6], "little")
        fo = flags_offset(a[0])
        diff = [i for i in range(len(a[0])) if a[0][i] != b[0][i]]
        self.assertTrue(set(diff) <= {usa_ofs, usa_ofs + 1, fo}, "record 3 bytes changed: %s" % diff)
        bits = int.from_bytes(a[0][fo:fo + 2], "little") ^ int.from_bytes(b[0][fo:fo + 2], "little")
        self.assertIn(bits, (0, VOLUME_IS_DIRTY), "a flag bit other than VOLUME_IS_DIRTY changed")


class VolmarkSteps(Base):
    # ---- U-VMK-01, U-VMK-04, ntfsinfo -m ----------------------------------------------------------
    def test_vmk01_set_and_vmk04_clear(self):
        orig = fx("clean-64m-4k")
        img = copy(orig)
        self.assertMarked(orig, img, "set", VOLUME_IS_DIRTY)
        self.assertEqual(ntfsinfo_flags(img), "0x0001 DIRTY")
        v = probe(img)
        self.assertEqual((v["state"], v["reason_codes"]), ("dirty_flag", ["FLAG_DIRTY"]))
        after_set = copy(img)
        self.assertMarked(after_set, img, "clear", 0)
        self.assertEqual(outside_record3(orig, img), [])
        self.assertRecordOnlyFlagsAndUsn(orig, img)
        # after set and clear: the record equals the one before set except the USN (two writes later)
        b = unprotect_checked(read_record(orig, "mft"))
        a = unprotect_checked(read_record(img, "mft"))
        self.assertEqual(a[1], next_usn(next_usn(b[1])))
        a0 = bytearray(a[0])
        usa_ofs = int.from_bytes(a0[4:6], "little")
        a0[usa_ofs:usa_ofs + 2] = b[0][usa_ofs:usa_ofs + 2]
        self.assertEqual(bytes(a0), bytes(b[0]), "record 3 after set and clear differs beyond the USN")
        self.assertEqual(ntfsinfo_flags(img), "0x0000")
        self.assertEqual(probe(img)["state"], "clean")
        EVIDENCE.append("U-VMK-01/04: set then clear on clean-64m-4k: exit 0, 0; ntfsinfo -m 'Volume Flags: "
                        "0x0001 DIRTY' after set, '0x0000' after clear; only record 3 (both copies) changed, "
                        "USN %d -> %d" % (b[1], a[1]))

    # ---- U-VMK-02, U-VMK-03 and every other fixture of the manifest ------------------------------
    def test_vmk02_vmk03_every_fixture(self):
        seen = []
        for e in MANIFEST["fixtures"]:
            name = e["name"]
            if e.get("generator") not in ("unsafe", "clean") or name not in FIX:
                if e.get("generator") == "unsafe":
                    raise AssertionError("unsafe fixture %s not present" % name)
                continue
            want = SET_EXIT_OVERRIDE.get(name, SET_EXIT_BY_STATE[e.get("probe", {}).get("state", "clean")])
            orig = fx(name)
            img = copy(orig)
            try:
                serial, sectors = boot_ids(orig)
            except Exception:
                serial, sectors = "0000000000000000", 1
            if sectors == 0:
                serial, sectors = "0000000000000000", 1  # not NTFS: any binding values
            with self.subTest(fixture=name):
                if want != 0:
                    self.assertRefused(img, "set", want, serial=serial, sectors=sectors)
                else:
                    f0 = disk_flags(orig)[0]
                    self.assertMarked(orig, img, "set", f0 | VOLUME_IS_DIRTY)
                    self.assertMarked(copy(img), img, "clear", f0)
                seen.append("%s=%d" % (name, want))
        EVIDENCE.append("U-VMK-02/03 and the manifest: set exit per fixture: " + ", ".join(seen))

    def test_vmk02_dirty_flag(self):
        self.assertRefused(copy(fx("dirty-flag")), "set", 11)
        self.assertRefused(copy(fx("dirty-chkdsk-flag")), "set", 11)

    def test_vmk03_unsafe_states(self):
        for name in ("unclean-log", "log-v2", "hiber-lower", "hiber-upper", "hiber-short", "truncated",
                     "truncated-60"):
            with self.subTest(fixture=name):
                img = copy(fx(name))
                r = self.assertRefused(img, "set", 10)
                self.assertRefused(img, "clear", 10)
                self.assertRefused(img, "clear-repair", 10)
                self.assertIn(b"refused", r.stderr)
        for name in ("hiber-wake", "hiber-unknown", "hiber-case"):
            with self.subTest(fixture=name):
                orig = fx(name)
                self.assertMarked(orig, copy(orig), "set", VOLUME_IS_DIRTY)

    # ---- U-VMK-05 ---------------------------------------------------------------------------------
    def test_vmk05_clear_without_flag(self):
        self.assertRefused(copy(fx("clean-64m-4k")), "clear", 13)
        self.assertRefused(copy(fx("chkdsk-flag")), "clear", 13)

    # ---- U-VMK-06 ---------------------------------------------------------------------------------
    def test_vmk06_clear_repair(self):
        orig = fx("dirty-flag")
        self.assertMarked(orig, copy(orig), "clear-repair", 0)
        orig = fx("dirty-chkdsk-flag")
        img = copy(orig)
        self.assertMarked(orig, img, "clear-repair", MODIFIED_BY_CHKDSK)
        self.assertEqual(ntfsinfo_flags(img).split()[0], "0x8000")
        # MOUNTED_ON_NT4 is informational too
        nt4 = copy(fx("clean-64m-4k"))
        set_volume_flags(nt4, VOLUME_IS_DIRTY | MOUNTED_ON_NT4)
        self.assertMarked(copy(nt4), nt4, "clear-repair", MOUNTED_ON_NT4)
        # a non-informational flag besides VOLUME_IS_DIRTY: refused, nothing written
        for extra in (RESIZE_LOG_FILE, 0x0004, 0x0010, 0x0020, 0x4000, 0x0100):
            with self.subTest(flags=hex(VOLUME_IS_DIRTY | extra)):
                img = copy(fx("clean-64m-4k"))
                set_volume_flags(img, VOLUME_IS_DIRTY | extra)
                self.assertRefused(img, "clear-repair", 13)
        # without VOLUME_IS_DIRTY: refused
        self.assertRefused(copy(fx("chkdsk-flag")), "clear-repair", 13)
        self.assertRefused(copy(fx("clean-64m-4k")), "clear-repair", 13)
        # plain `clear` keeps the chkdsk bit as well
        orig = fx("dirty-chkdsk-flag")
        self.assertMarked(orig, copy(orig), "clear", MODIFIED_BY_CHKDSK)

    # ---- U-VMK-09 ---------------------------------------------------------------------------------
    def test_vmk09_binding(self):
        img = copy(fx("clean-64m-4k"))
        serial, sectors = boot_ids(img)
        wrong = "%016x" % (int(serial, 16) ^ 1)
        for mode in ("set", "clear", "clear-repair"):
            with self.subTest(mode=mode):
                r = self.assertRefused(img, mode, 15, serial=wrong)
                self.assertIn(b"serial", r.stderr)
                r = self.assertRefused(img, mode, 15, sectors=sectors + 1)
                self.assertIn(b"sectors", r.stderr)
                self.assertRefused(img, mode, 15, sectors=sectors - 1)
                self.assertRefused(img, mode, 15, serial=serial.upper()[:-1] + ("0" if serial[-1] != "0" else "1"))
        # upper-case hex is the same number
        self.assertMarked(fx("clean-64m-4k"), img, "set", VOLUME_IS_DIRTY, serial=serial.upper())

    # ---- U-VMK-10 ---------------------------------------------------------------------------------
    def test_vmk10_flags_outside_the_mask(self):
        for bits in (0x0040, 0x0100, 0x2000, 0x0041):
            with self.subTest(flags=hex(bits)):
                img = copy(fx("clean-64m-4k"))
                set_volume_flags(img, bits)
                self.assertRefused(img, "set", 11)
                self.assertRefused(img, "clear", 13)
                self.assertRefused(img, "clear-repair", 13)

    def test_vmk10_pending_work_flags(self):
        # F-06: set refuses every flag but the informational MODIFIED_BY_CHKDSK and MOUNTED_ON_NT4.
        for bits in (0x0002, 0x0004, 0x0010, 0x0020, 0x4000, 0x4002):
            with self.subTest(flags=hex(bits)):
                img = copy(fx("clean-64m-4k"))
                set_volume_flags(img, bits)
                self.assertRefused(img, "set", 11)
        for bits in (MODIFIED_BY_CHKDSK, MOUNTED_ON_NT4, MODIFIED_BY_CHKDSK | MOUNTED_ON_NT4):
            with self.subTest(flags=hex(bits)):
                orig = copy(fx("clean-64m-4k"))
                set_volume_flags(orig, bits)
                self.assertMarked(orig, copy(orig), "set", bits | VOLUME_IS_DIRTY)

    def test_vmk10_txf_nonresident_release(self):
        orig = fx("txf-nonresident")
        self.assertEqual(txf_resident(orig), "No")
        img = copy(orig)
        self.assertMarked(orig, img, "set", VOLUME_IS_DIRTY)
        self.assertEqual(txf_resident(img), "No", "$TXF_DATA changed by set")
        self.assertMarked(copy(img), img, "clear", 0)
        self.assertEqual(txf_resident(img), "No", "$TXF_DATA changed by clear")

    # ---- command line and descriptor --------------------------------------------------------------
    def test_usage_and_descriptor(self):
        img = copy(fx("clean-64m-4k"))
        before = sha256(img)
        for argv in ([], ["set"], ["sync-mirror", "--fd", "3"], ["set", "--fd", "3"],
                     ["set", "--fd", "3", "--expect-serial", "0", "--expect-sectors", "1"],
                     ["set", "--fd", "3", "--expect-serial", "%016x" % 1, "--expect-sectors", "1",
                      "--stop-before-write"]):
            r = subprocess.run(["sh", "-c", 'img=$1; shift; exec "$@" 3<>"$img"', "sh", img, ARGS.volmark] + argv,
                               capture_output=True)
            self.assertEqual(r.returncode, 2, argv)
            self.assertEqual(r.stdout, b"")
        # read-only descriptor: refused before any mount
        r = self.assertRefused(img, "set", 3, redirect="<")
        self.assertIn(b"descriptor", r.stderr)
        # descriptor 3 not open
        r = subprocess.run([ARGS.volmark, "set", "--fd", "3", "--expect-serial", "%016x" % 1,
                            "--expect-sectors", "1"], capture_output=True, close_fds=True)
        self.assertEqual(r.returncode, 3)
        # a character device that is not NTFS: does not mount
        r = subprocess.run(["sh", "-c", 'exec "$@" 3<>/dev/null', "sh", ARGS.volmark, "set", "--fd", "3",
                            "--expect-serial", "%016x" % 1, "--expect-sectors", "1"], capture_output=True)
        self.assertEqual(r.returncode, 10)
        self.assertEqual(sha256(img), before)

    # ---- U-VMK-07 (first part): SIGKILL before step 4 ---------------------------------------------
    def test_vmk07_sigkill_before_step4(self):
        orig = fx("clean-64m-4k")
        dirty = copy(orig)
        self.assertMarked(orig, dirty, "set", VOLUME_IS_DIRTY)
        for mode, src in (("set", orig), ("clear", dirty), ("clear-repair", dirty)):
            with self.subTest(mode=mode):
                img = copy(src)
                before = sha256(img)
                serial, sectors = boot_ids(img)
                p = subprocess.Popen(["sh", "-c", 'img=$1; shift; exec "$@" 3<>"$img"', "sh", img, ARGS.volmark_test,
                                      mode, "--fd", "3", "--expect-serial", serial, "--expect-sectors", str(sectors),
                                      "--stop-before-write"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                _, status = os.waitpid(p.pid, os.WUNTRACED)
                self.assertTrue(os.WIFSTOPPED(status), "volmark_test did not stop before the write")
                os.kill(p.pid, signal.SIGKILL)
                out, _ = p.communicate()
                self.assertEqual(p.returncode, -signal.SIGKILL)
                self.assertEqual(out, b"", "a progress mark before step 4")
                self.assertEqual(sha256(img), before, "image changed before step 4")
                self.assertEqual(probe(img)["state"], "clean" if mode == "set" else "dirty_flag")
        EVIDENCE.append("U-VMK-07 SIGKILL before step 4 (set, clear, clear-repair): no output, image unchanged")


class VolmarkFault(Base):
    """Fault layer (patch 0004): writes counted, cut, torn, reordered, failed."""

    def setUp(self):
        if not ARGS.volmark_fault:
            self.fail("--volmark-fault not given: the cut tests need volmark_test built with --enable-faultio")

    def run_fault(self, img, mode, plan, kill=False, state=None, trace=None, extra=()):
        return run_volmark(img, mode, tool=ARGS.volmark_fault, env=fault_env(plan, kill, state, trace), extra=extra)

    # ---- the FORENSIC mount writes nothing before step 4; exactly two record-3 writes -------------
    def test_count_zero_before_step4_and_two_writes(self):
        orig = fx("clean-64m-4k")
        g = geometry(orig)
        want_offsets = [g["mft_off"] + 3 * g["rs"], g["mir_off"] + 3 * g["rs"]]
        dirty = copy(orig)
        self.assertMarked(orig, dirty, "set", VOLUME_IS_DIRTY)
        for name, mode, src in (("clean-64m-4k", "set", orig), ("clean-64m-4k", "clear", dirty),
                                ("txf-nonresident", "set", fx("txf-nonresident"))):
            with self.subTest(fixture=name, mode=mode):
                img = copy(src)
                state = os.path.join(TMP, "state-%d" % _n[0])
                trace = os.path.join(TMP, "trace-%d" % _n[0])
                open(state, "w").close()
                serial, sectors = boot_ids(img)
                p = subprocess.Popen(["sh", "-c", 'img=$1; shift; exec "$@" 3<>"$img"', "sh", img,
                                      ARGS.volmark_fault, mode, "--fd", "3", "--expect-serial", serial,
                                      "--expect-sectors", str(sectors), "--stop-before-write"],
                                     stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                                     env=dict(os.environ, **fault_env("count", state=state, trace=trace)))
                _, status = os.waitpid(p.pid, os.WUNTRACED)
                self.assertTrue(os.WIFSTOPPED(status))
                with open(state) as f:
                    at_stop = f.read().strip()
                self.assertIn(at_stop, ("", "0" * 20), "device writes before step 4: %r" % at_stop)
                self.assertFalse(os.path.exists(trace) and os.path.getsize(trace), "a write traced before step 4")
                os.kill(p.pid, signal.SIGCONT)
                out, err = p.communicate()
                self.assertEqual(p.returncode, 0, err[-300:])
                self.assertEqual(out, b"w\nd\n")
                with open(trace) as f:
                    rows = [line.split() for line in f if line.strip()]
                self.assertEqual(len(rows), 2, rows)
                self.assertEqual([int(r[1]) for r in rows], want_offsets, "writes not at record 3 of $MFT, $MFTMirr")
                self.assertEqual([int(r[2]) for r in rows], [g["rs"], g["rs"]])
                self.assertEqual([r[3] for r in rows], ["46494c45", "46494c45"])  # "FILE"
                self.assertEqual(outside_record3(src, img), [])
                if name == "txf-nonresident":
                    self.assertEqual(txf_resident(img), "No", "$TXF_DATA changed by set")
                EVIDENCE.append("count %s %s: 0 writes when stopped right before step 4; then %d writes: %s"
                                % (mode, name, len(rows), "; ".join(" ".join(r) for r in rows)))
        # refusals write nothing
        for name, mode in (("dirty-flag", "set"), ("hiber-lower", "set"), ("unclean-log", "set"),
                           ("clean-64m-4k", "clear")):
            with self.subTest(refusal=name):
                img = copy(fx(name))
                before = sha256(img)
                r = self.run_fault(img, mode, "count")
                self.assertNotEqual(r.returncode, 0)
                self.assertEqual(fault_writes(r), 0)
                self.assertEqual(sha256(img), before)
        img = copy(fx("clean-64m-4k"))
        r = self.run_fault(img, "set", "count", extra=())
        self.assertEqual(fault_writes(r), 2)

    # ---- U-VMK-08 ---------------------------------------------------------------------------------
    def test_vmk08_readback_mismatch(self):
        for plan in ("short:1:512", "short:2:512", "short:1:0", "cut:1", "cut:0", "skip:1", "skip:2"):
            with self.subTest(plan=plan):
                img = copy(fx("clean-64m-4k"))
                r = self.run_fault(img, "set", plan)
                self.assertEqual(r.returncode, 14, "%s: exit %d %r" % (plan, r.returncode, r.stderr[-300:]))
                self.assertEqual(r.stdout, b"w\nd\n")
                self.assertIn(b"read-back", r.stderr)

    def test_write_errors(self):
        """A write that fails (eio:N, dead:N): exit 12, no `d`, no write is repeated, and the image is
        in a row of the crash table (nothing written, or $MFT one step ahead)."""
        for plan, writes, want in (("eio:1", 1, "unchanged"), ("eio:2", 2, "mft-ahead"),
                                   ("dead:1", 1, "unchanged"), ("dead:2", 2, "mft-ahead")):
            with self.subTest(plan=plan):
                orig = fx("clean-64m-4k")
                img = copy(orig)
                r = self.run_fault(img, "set", plan)
                self.assertEqual(r.returncode, 12, "%s: exit %d %r" % (plan, r.returncode, r.stderr[-300:]))
                self.assertEqual(r.stdout, b"w\n")
                self.assertEqual(fault_writes(r), writes, "a failed write was repeated")
                self.assertEqual(outside_record3(orig, img), [])
                got = "unchanged" if sha256(img) == sha256(orig) else classify(img)[0]
                self.assertEqual(got, want)
                self.assertEqual(probe(img)["state"], "clean" if got == "unchanged" else "mirror_split")  # rule M4 k = 3
                EVIDENCE.append("write error %s: exit 12, stdout %r, device writes %d, record 3 %s"
                                % (plan, r.stdout, writes, got))

    # ---- U-VMK-07: a cut at every write against the crash table ------------------------------------
    def crash_case(self, mode, src, plan, kill, old, new):
        img = copy(src, "cut")
        r = self.run_fault(img, mode, plan, kill=kill)
        bad = outside_record3(src, img)
        self.assertEqual(bad, [], "%s %s: bytes outside record 3 changed" % (mode, plan))
        unchanged = sha256(img) == sha256(src)
        cls, info = classify(img)
        v = probe(img)
        old_state = "dirty_flag" if old & VOLUME_IS_DIRTY else "clean"
        new_state = "dirty_flag" if new & VOLUME_IS_DIRTY else "clean"
        if unchanged:
            row = "before step 4 / nothing persisted: unchanged"
            self.assertEqual(v["state"], old_state)
        elif cls == "equal":
            self.assertEqual(info["mft_flags"], new, "both copies equal but not the new value")
            row = "after step 5: new value on both copies"
            self.assertEqual(v["state"], new_state)
        elif cls == "mft-torn":
            row = "inside W1: $MFT record 3 torn"
            self.assertEqual(v["state"], "corrupt")
        elif cls == "mirr-torn":
            row = "inside W2: $MFTMirr record 3 torn"
            self.assertEqual(v["state"], "corrupt")
        elif cls == "mft-ahead":
            self.assertEqual((info["mft_flags"], info["mirr_flags"]), (new, old))
            lines, ok = record3_report(img)
            self.assertTrue(ok, lines)
            row = "between W1 and W2: copies differ, $MFT one step ahead"
            # rule M4, k = 3 (ARCHITECTURE.md section 3.7): the USN and VOLUME_IS_DIRTY only
            self.assertEqual(v["state"], "mirror_split")
            self.assertIn("MFTMIRR_STALE", v["reason_codes"])
        elif cls == "mirr-ahead":
            self.assertEqual((info["mft_flags"], info["mirr_flags"]), (old, new))
            row = "after W2, before the flush (drive kept W2, lost W1): mirror ahead"
            self.assertEqual(v["state"], "corrupt")  # rule M5
        else:
            self.fail("%s %s: unexpected record 3 state %s %r" % (mode, plan, cls, info))
        # never reported success unless the new value is on both copies
        if r.returncode == 0:
            self.assertEqual(cls, "equal", "%s %s: exit 0 without the new value on both copies" % (mode, plan))
            self.assertEqual(info["mft_flags"], new)
        if kill and r.returncode != -signal.SIGKILL:
            # the plan did not reach a dropped write: everything landed
            self.assertEqual(r.returncode, 0, "%s %s: %r" % (mode, plan, r.stderr[-300:]))
        # after `set` reported exit 0, never a probe `clean` with the flag lost
        if mode == "set" and r.returncode == 0:
            self.assertEqual(v["state"], "dirty_flag")
        return "%-12s %-6s %-14s kill=%d exit=%-4s %-11s probe=%-12s %s" % (
            mode, "", plan, kill, r.returncode, cls if not unchanged else "unchanged", v["state"], row)

    def test_vmk07_cut_at_every_write(self):
        orig = fx("clean-64m-4k")
        dirty = copy(orig)
        self.assertMarked(orig, dirty, "set", VOLUME_IS_DIRTY)
        chk = copy(fx("dirty-chkdsk-flag"))
        table = []
        for mode, src, old, new in (("set", orig, 0, VOLUME_IS_DIRTY), ("clear", dirty, VOLUME_IS_DIRTY, 0),
                                    ("clear-repair", chk, VOLUME_IS_DIRTY | MODIFIED_BY_CHKDSK, MODIFIED_BY_CHKDSK)):
            r = self.run_fault(copy(src), mode, "count")
            self.assertEqual(r.returncode, 0)
            n = fault_writes(r)
            self.assertEqual(n, 2, "%s: %d device writes, want 2 (record 3 of $MFT and $MFTMirr)" % (mode, n))
            plans = ["cut:%d" % i for i in range(0, n + 1)]
            plans += ["short:%d:%d" % (i, k) for i in range(1, n + 1) for k in (0, 1, 2, 256, 510, 511, 512, 513,
                                                                               767, 1022, 1023)]
            plans += ["skip:%d" % i for i in range(1, n + 1)]
            for plan in plans:
                for kill in (True, False):
                    if plan.startswith("skip") and kill:
                        continue  # skip never kills
                    with self.subTest(mode=mode, plan=plan, kill=kill):
                        line = self.crash_case(mode, src, plan, kill, old, new)
                        table.append(line)
        EVIDENCE.append("U-VMK-07 crash table (mode, plan, NTFSMAC_FAULT_KILL, exit, record 3, probe, row):")
        EVIDENCE.extend("  " + t for t in table)

    # ---- vmk-split (U15) --------------------------------------------------------------------------
    def test_vmk_split_image(self):
        out = ARGS.out or TMP
        os.makedirs(out, exist_ok=True)
        img = os.path.join(out, "vmk-split.img")
        lines = make(ARGS.volmark_fault, fx("clean-64m-4k"), img)
        v = probe(img)
        self.assertEqual(v["state"], "mirror_split")
        lines.append("probe (rule M4, k = 3): state %s, reason codes %s"
                     % (v["state"], v["reason_codes"]))
        with open(os.path.join(out, "vmk-split-record3.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(lines) + "\n")
        # volmark itself refuses it (step 1: the engine does not mount it)
        self.assertRefused(copy(img), "set", 10)
        self.assertRefused(copy(img), "clear", 10)
        EVIDENCE.append("vmk-split (U15):")
        EVIDENCE.extend("  " + t for t in lines)


def collect(dirs):
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".img"):
                FIX.setdefault(fn[:-4], os.path.join(d, fn))


def main():
    global ARGS, TMP, MANIFEST
    ap = argparse.ArgumentParser()
    ap.add_argument("--volmark", required=True)
    ap.add_argument("--volmark-test", required=True)
    ap.add_argument("--volmark-fault")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--images", action="append", default=[])
    ap.add_argument("--out", help="directory for vmk-split.img, its record-3 report and volmark-evidence.txt")
    ARGS, rest = ap.parse_known_args()
    with open(ARGS.manifest, encoding="utf-8") as f:
        MANIFEST = json.load(f)
    collect(ARGS.images)
    TMP = tempfile.mkdtemp(prefix="volmark-test-")
    print("fixtures: %d images from %s" % (len(FIX), ", ".join(ARGS.images)), file=sys.stderr)
    try:
        res = unittest.main(argv=[sys.argv[0]] + rest, verbosity=2, exit=False)
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    if ARGS.out:
        os.makedirs(ARGS.out, exist_ok=True)
        with open(os.path.join(ARGS.out, "volmark-evidence.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(EVIDENCE) + "\n")
    asan = "/tmp/asan"
    if os.environ.get("NTFS_ASAN") == "1" and os.path.isdir(asan) and os.listdir(asan):
        print("sanitizer reports in %s: %s" % (asan, os.listdir(asan)), file=sys.stderr)
        return 1
    return 0 if res.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
