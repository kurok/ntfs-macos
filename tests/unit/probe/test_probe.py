#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""U-PROBE-*, U-PROBE-MIRROR-01..03 and 05, U-PROBE-NAMES-01..03 (stdlib only).

Runs the built ntfsmac-probe over the fixture images (Environment A: tests/unit/probe/run.sh
makes them in the container with tests/fixtures/gen/*.sh and runs this test there).

Every probe run in this file goes through run_probe(), which
  - opens the image O_RDONLY and passes it as the inherited descriptor,
  - checks the image SHA-256 before and after (no write, ever),
  - requires exit status 0 and an empty stderr,
  - parses the output strictly (no duplicate key, at most 64 KiB), validates it against
    docs/schemas/probe.schema.json with the validator of tests/unit/contracts, and checks the
    cross-field rules of ARCHITECTURE.md section 3.1 that the schema cannot express.
"""
import argparse
import hashlib
import importlib.util
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import tempfile
import unicodedata
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ntfsrec import (damage_directory_record, damage_index_block, find_attr, geometry,  # noqa: E402
                    mutate_record, set_volume_flags, set_volume_version, synth_split)

ARGS = None
FIX = {}        # fixture name -> image path
LISTS = {}      # fixture name -> directory holding NAME.nonnfc and NAME.all
MANIFEST = {}
SCHEMA = None
CONTRACT = None
TMP = None


def load_contract():
    spec = importlib.util.spec_from_file_location("probe_schema_test", ARGS.contract)
    mod = importlib.util.module_from_spec(spec)
    sys.modules["probe_schema_test"] = mod
    spec.loader.exec_module(mod)
    return mod


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


class Verdict(dict):
    pass


def cross_field(v):
    """The rules of ARCHITECTURE.md section 3.1 beyond the schema."""
    errs = []
    if (v["state"] == "clean") != (len(v["reason_codes"]) == 0):
        errs.append("state clean iff no reason code")
    if (v["logfile_lsn"] is None) != (v["logfile"] == "unreadable"):
        errs.append("logfile_lsn null iff logfile unreadable")
    has = "NAMES_NOT_NFC" in v["reason_codes"]
    if (v["nonnfc_count"] > 0) != has:
        errs.append("nonnfc_count > 0 iff NAMES_NOT_NFC")
    if len(v["nonnfc_paths"]) != min(v["nonnfc_count"], 16):
        errs.append("nonnfc_paths has min(count, 16) entries")
    for p in v["nonnfc_paths"]:
        if len(p.encode("utf-8")) > 512 or "\x00" in p:
            errs.append("path longer than 512 bytes or with NUL")
    if len(v["label"].encode("utf-8")) > 512 or "\x00" in v["label"]:
        errs.append("label too long or with NUL")
    if len(v["reason_codes"]) != len(set(v["reason_codes"])):
        errs.append("duplicate reason code")
    return errs


def run_raw(img, tool=None, extra=(), check_hash=True, stderr_ok=False):
    tool = tool or ARGS.probe
    before = sha256(img) if check_hash else None
    fd = os.open(img, os.O_RDONLY)
    try:
        r = subprocess.run([tool, "--fd", str(fd)] + list(extra), pass_fds=(fd,), capture_output=True)
    finally:
        os.close(fd)
    if check_hash:
        assert sha256(img) == before, "the probe changed %s" % img
    if not stderr_ok:
        assert r.stderr == b"", "stderr: %r" % r.stderr[:200]
    return r


def run_probe(img, tool=None, extra=(), rc=0):
    r = run_raw(img, tool, extra)
    assert r.returncode == rc, "exit %d: %r" % (r.returncode, r.stderr[:200])
    if rc != 0:
        return None
    assert len(r.stdout) <= 64 * 1024
    text = r.stdout.decode("utf-8")  # UTF-8 only
    assert "\\u0000" not in text
    v = Verdict(CONTRACT.loads_strict(text))
    errs = CONTRACT.validate(v, SCHEMA)
    assert not errs, errs
    errs = cross_field(v)
    assert not errs, errs
    return v


def fx(name):
    if name not in FIX:  # a missing fixture is a failure: nothing is skipped silently
        raise AssertionError("fixture %s not present (tests/unit/probe/run.sh makes them)" % name)
    return FIX[name]


class Probe(unittest.TestCase):
    def verdict(self, name, **kw):
        return run_probe(fx(name), **kw)

    def assertVerdict(self, v, state, reasons, **fields):
        self.assertEqual(v["state"], state, v)
        self.assertEqual(sorted(v["reason_codes"]), sorted(reasons), v)
        for k, want in fields.items():
            self.assertEqual(v[k], want, "%s: %r" % (k, v))

    # ---- every fixture with an expected verdict in the manifest --------------------------
    def test_manifest_fixtures(self):
        n = 0
        for e in MANIFEST["fixtures"]:
            name = e["name"]
            exp = e.get("probe")
            if exp is None or name not in FIX:
                continue
            with self.subTest(fixture=name):
                v = run_probe(FIX[name])
                self.assertEqual(v["state"], exp["state"], v)
                self.assertEqual(sorted(v["reason_codes"]), sorted(exp["reason_codes"]), v)
                for k in ("logfile", "hiberfile", "logfile_lsn", "bitlocker"):
                    if k in exp:
                        self.assertEqual(v[k], exp[k], "%s of %s" % (k, name))
                if "volume_flags" in exp:
                    self.assertEqual(sorted(v["volume_flags"]), sorted(exp["volume_flags"]), name)
                if "nonnfc_count" in exp:
                    self.assertEqual(v["nonnfc_count"], exp["nonnfc_count"], name)
                if "nonnfc_path_count" in exp:
                    self.assertEqual(len(v["nonnfc_paths"]), exp["nonnfc_path_count"], name)
                n += 1
        self.assertGreater(n, 20)

    # ---- U-PROBE: the classes of TEST_STRATEGY 3.2 ----------------------------------------
    def test_clean_fixtures(self):
        names = [e["name"] for e in MANIFEST["fixtures"] if e["generator"] == "clean" and e["name"] in FIX]
        if not names:
            self.skipTest("no clean fixture")
        for name in names:
            with self.subTest(fixture=name):
                v = run_probe(FIX[name])
                self.assertVerdict(v, "clean", [], logfile="clean", hiberfile="absent", is_ntfs=True,
                                   nonnfc_count=0, nonnfc_paths=[], volume_flags=[], bitlocker=False)
                self.assertEqual(v["ntfs_version"], "3.1")
                e = next(x for x in MANIFEST["fixtures"] if x["name"] == name)
                self.assertEqual(v["cluster_size"], e["cluster_size"])
                self.assertEqual(v["sector_size"], e["sector_size"])
                self.assertEqual(v["total_sectors"] * v["sector_size"] // 1, e["size_bytes"] - v["sector_size"])

    def test_hibernation_variants(self):
        # U19: the root is listed and matched ignoring case; a subdirectory does not count.
        self.assertVerdict(self.verdict("hiber-case"), "hibernated", ["HIBER_SIGNATURE"], hiberfile="hibernated")
        self.assertVerdict(self.verdict("hiber-subdir"), "clean", [], hiberfile="absent")
        self.assertVerdict(self.verdict("hiber-wake"), "hibernated", ["HIBER_WAKE"], hiberfile="hibernated")
        self.assertVerdict(self.verdict("hiber-short"), "hibernated", ["HIBER_SHORT"], hiberfile="hibernated")
        self.assertVerdict(self.verdict("hiber-inactive"), "clean", [], hiberfile="inactive")
        self.assertVerdict(self.verdict("hiber-lower"), "hibernated", ["HIBER_SIGNATURE"])
        self.assertVerdict(self.verdict("hiber-upper"), "hibernated", ["HIBER_SIGNATURE"])

    def test_truncated_images(self):
        # truncated-60 mounts in the engine; the device-size part of check 2 makes it corrupt,
        # and checks 3 to 5 still run.
        v = self.verdict("truncated-60")
        self.assertVerdict(v, "corrupt", ["MOUNT_FAILED"], logfile="clean", hiberfile="absent",
                           logfile_lsn="0x0000000000000000")
        v = self.verdict("truncated")
        self.assertVerdict(v, "corrupt", ["MOUNT_FAILED"], logfile="unreadable", logfile_lsn=None,
                           hiberfile="unreadable")

    def test_size_check_edges(self):
        src = fx("log-init")
        total = None
        v = run_probe(src)
        total = v["total_sectors"] * v["sector_size"]
        for cut, state in ((total, "clean"), (total - 512, "corrupt"), (total + 4096, "clean")):
            with self.subTest(size=cut):
                img = os.path.join(TMP, "size.img")
                shutil.copy(src, img)
                os.truncate(img, cut)
                v = run_probe(img)
                self.assertEqual(v["state"], state, v)
                os.unlink(img)

    def test_informational_flags(self):
        self.assertVerdict(self.verdict("chkdsk-flag"), "clean", [], volume_flags=["MODIFIED_BY_CHKDSK"])
        self.assertVerdict(self.verdict("dirty-chkdsk-flag"), "dirty_flag", ["FLAG_DIRTY"],
                           volume_flags=["DIRTY", "MODIFIED_BY_CHKDSK"])

    def test_flag_bits(self):
        # Each bit of check 3, set in $Volume of a scratch copy of log-init through raw edit
        # is not possible without the tool: use setvolflags when it is given.
        if not ARGS.setvolflags:
            self.skipTest("no --setvolflags")
        cases = [
            ("MOUNTED_ON_NT4", "clean", [], ["MOUNTED_ON_NT4"]),
            ("RESIZE_LOG_FILE", "dirty_flag", ["FLAG_PENDING_WORK"], ["RESIZE_LOG_FILE"]),
            ("UPGRADE_ON_MOUNT", "dirty_flag", ["FLAG_PENDING_WORK"], ["UPGRADE_ON_MOUNT"]),
            ("DELETE_USN_UNDERWAY", "dirty_flag", ["FLAG_PENDING_WORK"], ["DELETE_USN_UNDERWAY"]),
            ("REPAIR_OBJECT_ID", "dirty_flag", ["FLAG_PENDING_WORK"], ["REPAIR_OBJECT_ID"]),
            ("CHKDSK_UNDERWAY", "dirty_flag", ["FLAG_CHKDSK_UNDERWAY"], ["CHKDSK_UNDERWAY"]),
            ("DIRTY,CHKDSK_UNDERWAY,RESIZE_LOG_FILE", "dirty_flag",
             ["FLAG_DIRTY", "FLAG_CHKDSK_UNDERWAY", "FLAG_PENDING_WORK"],
             ["DIRTY", "CHKDSK_UNDERWAY", "RESIZE_LOG_FILE"]),
        ]
        for flags, state, reasons, listed in cases:
            with self.subTest(flags=flags):
                img = os.path.join(TMP, "flags.img")
                shutil.copy(fx("clean-64m-4k" if "clean-64m-4k" in FIX else "log-init"), img)
                subprocess.run([ARGS.setvolflags, img, "--set", flags], check=True, capture_output=True)
                v = run_probe(img)
                self.assertVerdict(v, state, reasons)
                self.assertEqual(sorted(v["volume_flags"]), sorted(listed))
                os.unlink(img)

    def test_unknown_flag_bits(self):
        # Raw edit of the flags word in $Volume's $VOLUME_INFORMATION, in both copies:
        # a bit outside VOLUME_FLAGS_MASK (0xc03f) gives dirty_flag and FLAG_UNKNOWN_BITS.
        img = os.path.join(TMP, "unknownbits.img")
        shutil.copy(fx("log-init"), img)
        set_volume_flags(img, 0x0100)
        v = run_probe(img)
        self.assertVerdict(v, "dirty_flag", ["FLAG_UNKNOWN_BITS"], volume_flags=["UNKNOWN_BITS"])
        set_volume_flags(img, 0x0100 | 0x8000)
        v = run_probe(img)
        self.assertVerdict(v, "dirty_flag", ["FLAG_UNKNOWN_BITS"])
        self.assertEqual(sorted(v["volume_flags"]), ["MODIFIED_BY_CHKDSK", "UNKNOWN_BITS"])
        os.unlink(img)

    def test_ntfs_version(self):
        # 1.0 to 3.9 are known; anything else is check 6: unknown, with VERSION_UNKNOWN and no version.
        for major, minor, version in ((3, 1, "3.1"), (3, 0, "3.0"), (1, 2, "1.2"), (2, 0, "2.0"), (3, 9, "3.9"),
                                      (0, 0, None), (4, 0, None), (3, 10, None), (255, 255, None)):
            with self.subTest(version="%d.%d" % (major, minor)):
                img = os.path.join(TMP, "version.img")
                shutil.copy(fx("log-init"), img)
                set_volume_version(img, major, minor)
                v = run_probe(img)
                if version:
                    self.assertVerdict(v, "clean", [], ntfs_version=version)
                else:
                    self.assertVerdict(v, "unknown", ["VERSION_UNKNOWN"], ntfs_version=None, logfile="clean")
                os.unlink(img)

    def test_log_variants(self):
        v = self.verdict("log-v2")
        self.assertVerdict(v, "fast_startup", ["LOG_V2"], logfile="v2_fast_startup",
                           logfile_lsn="0x0000000000100400")
        v = self.verdict("unclean-log")
        self.assertVerdict(v, "unclean_log", ["LOG_UNCLEAN"], logfile="unclean")
        a = self.verdict("log-init")["logfile_lsn"]
        b = self.verdict("log-bumped")["logfile_lsn"]
        self.assertEqual((a, b), ("0x0000000000100400", "0x0000000000180400"))

    def test_not_ntfs_and_tiny_images(self):
        cases = {
            "exfat": b"\xeb\x76\x90EXFAT   " + b"\0" * (1 << 20),
            "zeros": b"\0" * (1 << 20),
            "short": b"NTFS    ",
            "empty": b"",
            "random": random.Random(3).randbytes(1 << 16),
        }
        for name, data in cases.items():
            with self.subTest(image=name):
                img = os.path.join(TMP, "not-ntfs.img")
                with open(img, "wb") as f:
                    f.write(data)
                v = run_probe(img)
                self.assertVerdict(v, "unsupported", ["NOT_NTFS"], is_ntfs=False, bitlocker=False,
                                   ntfs_version=None, logfile="unreadable", logfile_lsn=None,
                                   hiberfile="unreadable", nonnfc_count=0, nonnfc_paths=[])
                os.unlink(img)

    def test_ntfs_oem_with_garbage_geometry(self):
        img = os.path.join(TMP, "garbage.img")
        with open(img, "wb") as f:
            f.write(b"\xeb\x52\x90NTFS    " + b"\xff" * 500 + b"\0" * (1 << 20))
        v = run_probe(img)
        self.assertVerdict(v, "corrupt", ["MOUNT_FAILED"], is_ntfs=True)
        os.unlink(img)

    def test_bitlocker_flag(self):
        v = self.verdict("bitlocker-sig")
        self.assertVerdict(v, "unsupported", ["BITLOCKER"], bitlocker=True, is_ntfs=False)

    # ---- hostile label ----------------------------------------------------------------------
    def test_label_is_escaped_not_interpreted(self):
        if not shutil.which("ntfslabel"):
            self.skipTest("no ntfslabel")
        for label in ['X,remove_hiberfile', 'q"uote\\back', "tab\there", "nl\nx", "‮evil", "é" * 128, ""]:
            with self.subTest(label=label):
                img = os.path.join(TMP, "label.img")
                shutil.copy(fx("log-init"), img)
                r = subprocess.run(["ntfslabel", "-f", img, label], capture_output=True)
                if r.returncode != 0:
                    self.skipTest("ntfslabel refused %r: %s" % (label, r.stderr))
                v = run_probe(img)
                self.assertEqual(v["label"], label)
                os.unlink(img)

    # ---- U-PROBE-MIRROR-01..03, 05 ------------------------------------------------------------
    def test_mirror_01_split(self):
        v = self.verdict("mirror-split")
        self.assertVerdict(v, "mirror_split", ["MFTMIRR_STALE"], logfile="clean", hiberfile="absent",
                           volume_flags=[], is_ntfs=True)
        self.assertEqual(v["label"], "mirror-split")  # read through the overlay view

    def test_mirror_02_not_split(self):
        for name in ("mirror-newer", "mirror-torn", "mirror-extra"):
            with self.subTest(fixture=name):
                v = self.verdict(name)
                self.assertVerdict(v, "corrupt", ["MOUNT_FAILED"], logfile="unreadable", logfile_lsn=None)

    def test_mirror_01_vmk_split(self):
        # Record 3 ($Volume): volmark's two writes cut between W1 and W2 (rule M4, k = 3). `set` leaves
        # VOLUME_IS_DIRTY in $MFT only, an interrupted `clear` leaves it in $MFTMirr only; the overlay
        # view reads the $MFT copy, so the flag of the verdict is that of $MFT.
        v = self.verdict("vmk-split")
        self.assertVerdict(v, "mirror_split", ["MFTMIRR_STALE", "FLAG_DIRTY"], logfile="clean",
                           hiberfile="absent", volume_flags=["DIRTY"], is_ntfs=True)
        v = self.verdict("vmk-split-clear")
        self.assertVerdict(v, "mirror_split", ["MFTMIRR_STALE"], logfile="clean", hiberfile="absent",
                           volume_flags=[], is_ntfs=True)

    def test_mirror_02_vmk_split_not_split(self):
        # Anything beyond the USN and the VOLUME_IS_DIRTY bit in record 3 is corrupt (rule M4), a
        # wrong USN order is corrupt (rule M5), and so is a record without a usable
        # $VOLUME_INFORMATION. run_probe() checks that the image is unchanged.
        def vi(rec):
            return find_attr(rec, 0x70)

        def extra_flag_bit(rec, usn):
            o = vi(rec) + struct.unpack_from("<H", rec, vi(rec) + 0x14)[0] + 10
            rec[o] |= 0x02
            return usn

        def other_byte(rec, usn):
            o = find_attr(rec, 0x10)
            rec[o + struct.unpack_from("<H", rec, o + 0x14)[0] + 2] ^= 0x01
            return usn

        def other_byte_beside_flags(rec, usn):
            rec[vi(rec) + struct.unpack_from("<H", rec, vi(rec) + 0x14)[0] + 9] ^= 0x01  # minor version
            return usn

        def usn_plus_one(rec, usn):
            return usn + 1

        def retype(rec, usn):  # no $VOLUME_INFORMATION: 0x70 becomes 0x78
            struct.pack_into("<I", rec, vi(rec), 0x78)
            return usn

        def short_value(rec, usn):
            struct.pack_into("<I", rec, vi(rec) + 0x10, 8)
            return usn

        def cut_inside(rec, usn):  # bytes in use end inside the attribute: the record is invalid
            struct.pack_into("<I", rec, 0x18, vi(rec) + 0x20)
            return usn

        def usn_same(rec, usn):
            return usn - 1

        cases = (("extra flag bit", [("mft", extra_flag_bit)]),
                 ("another byte changed in $MFT", [("mft", other_byte)]),
                 ("another byte changed in the mirror", [("mir", other_byte)]),
                 ("a byte next to the flags", [("mft", other_byte_beside_flags)]),
                 ("USN two ahead", [("mft", usn_plus_one)]),
                 ("USN equal", [("mft", usn_same)]),
                 ("attribute missing in both", [("mft", retype), ("mir", retype)]),
                 ("attribute missing in the mirror", [("mir", retype)]),
                 ("value too short in both", [("mft", short_value), ("mir", short_value)]),
                 ("attribute truncated in both", [("mft", cut_inside), ("mir", cut_inside)]))
        for src in ("vmk-split", "vmk-split-clear"):
            for name, edits in cases:
                with self.subTest(fixture=src, case=name):
                    img = os.path.join(TMP, "vmk-neg.img")
                    shutil.copy(fx(src), img)
                    for which, fn in edits:
                        mutate_record(img, which, 3, fn)
                    v = run_probe(img)
                    self.assertVerdict(v, "corrupt", ["MOUNT_FAILED"], logfile="unreadable", logfile_lsn=None)
                    os.unlink(img)
        # the same copies without the dirty bit difference: only the USN differs, which is not enough
        img = os.path.join(TMP, "vmk-neg.img")
        shutil.copy(fx("vmk-split"), img)

        def clear_flag(rec, usn):
            rec[vi(rec) + struct.unpack_from("<H", rec, vi(rec) + 0x14)[0] + 10] &= 0xFE
            return usn

        mutate_record(img, "mft", 3, clear_flag)
        self.assertVerdict(run_probe(img), "corrupt", ["MOUNT_FAILED"], logfile="unreadable", logfile_lsn=None)
        os.unlink(img)

    def test_mirror_02_record4_area_is_not_compared(self):
        # An mkntfs mirror holds four records: bytes at record index 4 and above of that cluster
        # are not part of $MFTMirr, the engine does not compare them and neither does the rule
        # (the in-memory test_mftmirr covers a larger mirror). The verdict stays mirror_split.
        img = os.path.join(TMP, "m4.img")
        shutil.copy(fx("mirror-split"), img)
        g = geometry(img)
        with open(img, "r+b") as f:
            f.seek(g["mir_off"] + 4 * g["rs"] + 100)
            f.write(b"\x5a" * 16)
        self.assertVerdict(run_probe(img), "mirror_split", ["MFTMIRR_STALE"])
        os.unlink(img)

    def test_mirror_03_other_findings_are_listed(self):
        v = self.verdict("mirror-split-dirty")
        self.assertVerdict(v, "mirror_split", ["MFTMIRR_STALE", "FLAG_DIRTY"], volume_flags=["DIRTY"])
        v = self.verdict("mirror-split-hiber")
        self.assertVerdict(v, "mirror_split", ["MFTMIRR_STALE", "HIBER_SIGNATURE"], hiberfile="hibernated")

    def test_mirror_03_split_with_names(self):
        # mirror_split beside NAMES_NOT_NFC and beside a dirty flag (ADR-021, ADR-023): a split made
        # from names-nfd by the same one-record step the sweep images show (USN + 1, $MFT data and
        # initialized size one record larger), since a split volume cannot be mounted to add names.
        for src, extra in (("names-nfd", ["NAMES_NOT_NFC"]), ("names-dirty", ["FLAG_DIRTY", "NAMES_NOT_NFC"])):
            with self.subTest(source=src):
                img = os.path.join(TMP, "synth.img")
                shutil.copy(fx(src), img)
                synth_split(img)
                v = run_probe(img)
                self.assertVerdict(v, "mirror_split", ["MFTMIRR_STALE"] + extra)
                self.assertGreaterEqual(v["nonnfc_count"], 1)
                self.assertEqual(len(v["nonnfc_paths"]), min(v["nonnfc_count"], 16))
                os.unlink(img)

    def test_mirror_64k_cluster_and_record_index_4_or_above(self):
        # On a 64 KiB-cluster volume the mirror holds 64 records and the engine compares 16, so a
        # difference at index 4 or above can exist (U-PROBE-MIRROR-02): corrupt, never mirror_split.
        # Without it the same split is recognised (the rule works for n > 4 records).
        for name in ("clean-64m-64k", "clean-64m-64k-ss4096"):
            if name not in FIX:
                continue
            with self.subTest(fixture=name):
                img = os.path.join(TMP, "split64.img")
                shutil.copy(FIX[name], img)
                synth_split(img)
                self.assertVerdict(run_probe(img), "mirror_split", ["MFTMIRR_STALE"])
                for k in (4, 5, 7, 15):
                    shutil.copy(FIX[name], img)
                    synth_split(img)

                    def flip(rec, usn):
                        off = find_attr(rec, 0x10)  # $STANDARD_INFORMATION: a time stamp byte
                        rec[off + struct.unpack_from("<H", rec, off + 0x14)[0] + 2] ^= 0x01
                        return usn

                    mutate_record(img, "mir", k, flip)
                    v = run_probe(img)
                    self.assertVerdict(v, "corrupt", ["MOUNT_FAILED"], logfile="unreadable")
                os.unlink(img)
        if not any(n in FIX for n in ("clean-64m-64k", "clean-64m-64k-ss4096")):
            self.skipTest("no 64k fixture")

    def test_mirror_05_sweep_images(self):
        sweep = sorted(n for n in FIX if n.startswith("sweep-"))
        self.assertTrue(sweep, "no sweep image")
        cuts = MANIFEST_BY_NAME["mirror-sweep"]
        want = sum(len(v) for v in cuts["cuts_full"].values()) + sum(len(v) for v in cuts["cuts_short"].values())
        self.assertEqual(len(sweep), want, "sweep images present: %s" % sweep)
        for name in sweep:
            with self.subTest(image=name):
                v = run_probe(FIX[name])
                self.assertVerdict(v, "mirror_split", ["MFTMIRR_STALE"], logfile="clean", hiberfile="absent")

    # ---- U-PROBE-NAMES-01..03 -------------------------------------------------------------------
    def listed(self, name):
        path = os.path.join(LISTS[name], name + ".nonnfc")
        with open(path, encoding="utf-8") as f:
            return [ln.rstrip("\n") for ln in f if ln.rstrip("\n")]

    def test_names_01_nfd(self):
        v = self.verdict("names-nfd")
        self.assertVerdict(v, "names_not_nfc", ["NAMES_NOT_NFC"], nonnfc_count=22, logfile="clean",
                           hiberfile="absent")
        self.assertEqual(len(v["nonnfc_paths"]), 16)
        self.assertEqual(len(set(v["nonnfc_paths"])), 16)
        expected = set(self.listed("names-nfd"))
        self.assertEqual(len(expected), 22)
        for p in v["nonnfc_paths"]:
            self.assertIn(p, expected)
            self.assertTrue(p.startswith("/"))

    def test_names_01_paths_include_subdirectories_when_few(self):
        v = self.verdict("names-wide")
        self.assertEqual(v["nonnfc_count"], 22)
        for p in v["nonnfc_paths"]:
            self.assertIn(p, set(self.listed("names-wide")))
        v = self.verdict("names-longpath")
        self.assertEqual(v["nonnfc_count"], 1)
        p = v["nonnfc_paths"][0]
        raw = p.encode("utf-8")
        self.assertTrue(p.startswith("/" + "0" * 100 + "/" + "1" * 100 + "/"))
        self.assertLessEqual(len(raw), 512)
        self.assertGreater(len(raw), 500)  # cut, not dropped
        raw.decode("utf-8")  # a cut on a code point boundary: still valid UTF-8
        self.assertEqual(len(self.listed("names-longpath")), 1)

    def test_names_01_deep_tree_has_no_recursion_limit(self):
        v = self.verdict("names-deep")
        self.assertVerdict(v, "names_not_nfc", ["NAMES_NOT_NFC"], nonnfc_count=1)
        p = v["nonnfc_paths"][0]
        self.assertEqual(len(p.encode()), 512)
        self.assertTrue(p.startswith("/a/a/a/a/") and set(p.split("/")[1:]) == {"a"}, p[:20])

    def test_names_01_reparse_directory_is_walked(self):
        # A junction is a directory with an index of its own; the walk follows the on-disk index,
        # never the target, so the NFD name inside is found.
        v = self.verdict("names-reparse")
        self.assertVerdict(v, "names_not_nfc", ["NAMES_NOT_NFC"], nonnfc_count=1)
        self.assertEqual(v["nonnfc_paths"], self.listed("names-reparse"))
        self.assertTrue(v["nonnfc_paths"][0].startswith("/junc/"))

    def test_names_03_undecidable_entries_fail_the_scan(self):
        # A directory with the system attribute: its entry type comes from opening the record.
        self.assertVerdict(self.verdict("names-sysdir"), "clean", [], nonnfc_count=0)
        img = os.path.join(TMP, "sysdir-broken.img")
        shutil.copy(fx("names-sysdir"), img)
        self.assertTrue(damage_directory_record(img, "sysdir"), "record of the directory not found")
        # the entry sits in the root, whose listing is what failed: hiberfile cannot be answered either
        self.assertVerdict(run_probe(img), "unknown", ["CHECK_FAILED", "HIBER_UNREADABLE"], nonnfc_count=0)
        os.unlink(img)
        # the same for the reparse directory: its record cannot be opened
        shutil.copy(fx("names-reparse"), img)
        self.assertTrue(damage_directory_record(img, "junc"), "record of the directory not found")
        self.assertVerdict(run_probe(img), "unknown", ["CHECK_FAILED", "HIBER_UNREADABLE"], nonnfc_count=0)
        os.unlink(img)

    def test_names_02_clean_and_other_fixtures(self):
        v = self.verdict("names-nfc")
        self.assertVerdict(v, "clean", [], nonnfc_count=0, nonnfc_paths=[])
        for name in sorted(FIX):
            if name in ("names-nfd", "names-wide", "names-longpath", "names-deep", "names-dirty",
                        "names-reparse") or \
                    name.startswith("sweep-"):
                continue
            with self.subTest(fixture=name):
                v = run_probe(FIX[name])
                self.assertEqual(v["nonnfc_count"], 0, name)
                self.assertNotIn("NAMES_NOT_NFC", v["reason_codes"], name)
                self.assertEqual(v["nonnfc_paths"], [], name)

    def test_names_02_dirty_with_nfd_name(self):
        v = self.verdict("names-dirty")
        self.assertVerdict(v, "dirty_flag", ["FLAG_DIRTY", "NAMES_NOT_NFC"], nonnfc_count=1,
                           volume_flags=["DIRTY"])
        self.assertEqual(v["nonnfc_paths"], self.listed("names-dirty"))

    def test_names_03_scan_cannot_finish(self):
        # (a) one directory unreadable, found after the root has listed its names;
        # (b) a time budget of zero. Both give unknown, never clean and never a partial name list.
        img = os.path.join(TMP, "names-broken.img")
        shutil.copy(fx("names-nfd"), img)
        self.assertTrue(damage_directory_record(img, "sub"), "record of the directory not found")
        v = run_probe(img)
        self.assertVerdict(v, "unknown", ["CHECK_FAILED"], nonnfc_count=0, nonnfc_paths=[], hiberfile="absent")
        os.unlink(img)
        for name in ("names-nfd", "names-nfc", "log-init"):
            with self.subTest(budget0=name):
                v = run_probe(fx(name), tool=ARGS.probe_test, extra=["--scan-budget-ms", "0"])
                self.assertEqual(v["state"], "unknown", v)
                self.assertEqual(sorted(v["reason_codes"]), ["CHECK_FAILED", "HIBER_UNREADABLE"])
                self.assertEqual(v["nonnfc_count"], 0)
                self.assertEqual(v["hiberfile"], "unreadable")

    def test_names_03_index_block_of_a_directory_unreadable(self):
        img = os.path.join(TMP, "wide-broken.img")
        shutil.copy(fx("names-wide"), img)
        block = damage_index_block(img, "w-007-with-a-long-name-to-fill-the-index-block.txt")
        self.assertIsNotNone(block, "index block of the wide directory not found")
        v = run_probe(img)
        # the root listed 22 names before the wide directory failed: dropped, the scan did not finish
        self.assertVerdict(v, "unknown", ["CHECK_FAILED"], nonnfc_count=0, nonnfc_paths=[], hiberfile="absent")
        os.unlink(img)

    def test_names_03_root_index_unreadable_is_a_mount_failure(self):
        # ntfs_mount itself reads the root index (it looks up $Extend): corrupt, check 2.
        img = os.path.join(TMP, "root-broken.img")
        shutil.copy(fx("names-nfd"), img)
        block = damage_index_block(img, unicodedata.normalize("NFD", "nfd-00-\u00fc.txt"))
        self.assertIsNotNone(block, "index block holding a root entry not found")
        self.assertVerdict(run_probe(img), "corrupt", ["MOUNT_FAILED"], nonnfc_count=0)
        os.unlink(img)

    def test_budget_default_is_not_zero(self):
        v = run_probe(fx("names-nfc"), tool=ARGS.probe_test, extra=["--scan-budget-ms", "60000"])
        self.assertEqual(v["state"], "clean")

    # ---- command line, exit codes -------------------------------------------------------------------
    def test_usage_errors(self):
        for args in ([], ["--fd"], ["--fd", "x"], ["--fd", "0"], ["--fd", "1"], ["--fd", "2"], ["--fd", "007"],
                     ["--fd", "-3"], ["--fd", "3", "--bsd-name", "../x"], ["--fd", "3", "--bsd-name", "disk"],
                     ["--fd", "3", "--bsd-name", "disk1s"], ["--fd", "3", "--bsd-name", "diskxs1"],
                     ["--fd", "3", "--unknown"], ["--bsd-name", "disk2"], ["--fd", "3", "--fd", "4"],
                     ["--fd", "99999"], ["--fd", "3", "--scan-budget-ms", "0"], ["--fd", "3", "--no-alarm"]):
            with self.subTest(args=args):
                r = subprocess.run([ARGS.probe] + args, capture_output=True, stdin=subprocess.DEVNULL,
                                   pass_fds=())
                self.assertEqual(r.returncode, 2, r.stderr)
                self.assertEqual(r.stdout, b"")

    def test_version_output_names_the_unicode_tables(self):
        r = subprocess.run([ARGS.probe, "--version"], capture_output=True)
        self.assertEqual(r.returncode, 0)
        self.assertRegex(r.stdout.decode(), r"^ntfsmac-probe schema 1, NFC tables Unicode \d+\.\d+\.\d+\n$")
        r = run_raw(fx("log-init"), extra=["--debug"], stderr_ok=True)
        self.assertRegex(r.stderr.decode(), r"NFC tables: Unicode \d+\.\d+\.\d+")

    def test_accepts_bsd_name(self):
        for name in ("disk2", "disk12s3", "disk0s100"):
            with self.subTest(name=name):
                v = run_probe(fx("log-init"), extra=["--bsd-name", name])
                self.assertEqual(v["state"], "clean")

    def test_descriptor_problems_exit_3(self):
        # not open
        r = subprocess.run([ARGS.probe, "--fd", "77"], capture_output=True)
        self.assertEqual(r.returncode, 3)
        self.assertEqual(r.stdout, b"")
        # a directory
        fd = os.open(TMP, os.O_RDONLY)
        r = subprocess.run([ARGS.probe, "--fd", str(fd)], pass_fds=(fd,), capture_output=True)
        os.close(fd)
        self.assertEqual(r.returncode, 3)
        # write-only
        path = os.path.join(TMP, "wo.img")
        with open(path, "wb") as f:
            f.write(b"x")
        fd = os.open(path, os.O_WRONLY)
        r = subprocess.run([ARGS.probe, "--fd", str(fd)], pass_fds=(fd,), capture_output=True)
        os.close(fd)
        self.assertEqual(r.returncode, 3)
        self.assertEqual(os.path.getsize(path), 1)
        os.unlink(path)
        # a pipe
        rfd, wfd = os.pipe()
        r = subprocess.run([ARGS.probe, "--fd", str(rfd)], pass_fds=(rfd,), capture_output=True)
        os.close(rfd)
        os.close(wfd)
        self.assertEqual(r.returncode, 3)

    def test_read_error_on_boot_sector_exit_3(self):
        # a directory-less way to get EIO is not available without privileges; a descriptor on a
        # regular file that is shorter than a sector gives a verdict (NOT_NTFS), not an error.
        img = os.path.join(TMP, "tiny.img")
        with open(img, "wb") as f:
            f.write(b"abc")
        v = run_probe(img)
        self.assertEqual(v["state"], "unsupported")
        os.unlink(img)

    def test_read_write_descriptor_is_refused(self):
        # Only an O_RDONLY descriptor is used: O_RDWR gives exit 3, no output, image untouched.
        img = os.path.join(TMP, "rw.img")
        shutil.copy(fx("hiber-lower"), img)
        before = sha256(img)
        fd = os.open(img, os.O_RDWR)
        r = subprocess.run([ARGS.probe, "--fd", str(fd)], pass_fds=(fd,), capture_output=True)
        os.close(fd)
        self.assertEqual(r.returncode, 3)
        self.assertEqual(r.stdout, b"")
        self.assertEqual(sha256(img), before)
        os.unlink(img)

    def test_the_library_never_tries_to_write(self):
        # The device layer refuses every write with EROFS; the count of refusals stays 0 on every
        # fixture, so no code path of the checks wants to write in the first place.
        n = 0
        for name in sorted(FIX):
            if name.startswith("sweep-") and n > 40:
                continue
            r = run_raw(FIX[name], extra=["--debug"], stderr_ok=True)
            self.assertEqual(r.returncode, 0, name)
            lines = [ln for ln in r.stderr.decode().splitlines() if "device writes refused" in ln]
            self.assertEqual(lines, ["ntfsmac-probe: device writes refused: 0"], name)
            n += 1
        self.assertGreater(n, 20)

    def test_output_is_one_object(self):
        r = run_raw(fx("log-init"))
        text = r.stdout.decode()
        self.assertTrue(text.startswith("{\n"))
        self.assertTrue(text.endswith("}\n"))
        self.assertEqual(text.count("\n}\n"), 1)
        keys = list(json.loads(text))
        self.assertEqual(keys, ["schema", "is_ntfs", "ntfs_version", "label", "serial", "total_sectors",
                                "cluster_size", "sector_size", "volume_flags", "logfile", "logfile_lsn",
                                "hiberfile", "bitlocker", "state", "reason_codes", "nonnfc_count",
                                "nonnfc_paths"])

    # ---- robustness: no crash on damaged images ---------------------------------------------------------
    def test_mutated_images_never_crash(self):
        rng = random.Random(2026)
        sources = [n for n in ("log-init", "names-nfd", "names-wide", "mirror-split", "hiber-lower", "dirty-flag")
                   if n in FIX]
        if not sources:
            self.skipTest("no source image")
        count = int(os.environ.get("NTFSMAC_PROBE_MUTATIONS", "150"))
        img = os.path.join(TMP, "mut.img")
        stats = {}
        for i in range(count):
            src = FIX[rng.choice(sources)]
            shutil.copy(src, img)
            with open(img, "r+b") as f:
                for _ in range(rng.choice([1, 1, 2, 8, 64])):
                    zone = rng.choice(["boot", "mft", "mft", "log", "index", "any"])
                    if zone == "boot":
                        off = rng.randrange(0, 512)
                    elif zone == "mft":
                        off = 0x4000 + rng.randrange(0, 96 * 1024)
                    elif zone == "log":
                        off = rng.randrange(0, 1 << 22) + 0x300000
                    elif zone == "index":
                        off = rng.randrange(0x1fff000 - 0x400000, 0x1fff000)
                    else:
                        off = rng.randrange(0, os.path.getsize(img))
                    f.seek(off)
                    f.write(bytes([rng.randrange(256)]) if rng.random() < 0.7 else b"\x00" * rng.choice([4, 64, 1024]))
            before = sha256(img)
            r = run_raw(img, check_hash=False, stderr_ok=True)
            self.assertEqual(sha256(img), before)
            self.assertIn(r.returncode, (0, 3), "mutation %d of %s: exit %d" % (i, src, r.returncode))
            if r.returncode == 0:
                v = Verdict(CONTRACT.loads_strict(r.stdout.decode("utf-8")))
                self.assertEqual(CONTRACT.validate(v, SCHEMA), [])
                self.assertEqual(cross_field(v), [])
                stats[v["state"]] = stats.get(v["state"], 0) + 1
        print("mutations: %d runs, states %s" % (count, stats), file=sys.stderr)
        os.unlink(img)

    def test_truncated_at_every_stage_never_crashes(self):
        img = os.path.join(TMP, "cut.img")
        src = fx("names-wide")
        size = os.path.getsize(src)
        for frac in (0, 1, 3, 17, 33, 50, 51, 52, 64, 99):
            cut = size * frac // 100
            shutil.copy(src, img)
            os.truncate(img, cut)
            r = run_raw(img, stderr_ok=True)
            self.assertIn(r.returncode, (0, 3), (frac, r.returncode))
            if r.returncode == 0:
                v = Verdict(CONTRACT.loads_strict(r.stdout.decode("utf-8")))
                self.assertEqual(CONTRACT.validate(v, SCHEMA), [])
        os.unlink(img)

    # ---- no path is opened ---------------------------------------------------------------------------------
    def test_syscall_audit(self):
        if os.environ.get("NTFS_ASAN") == "1":
            self.skipTest("valgrind and AddressSanitizer do not run together; the release build is audited")
        # A missing valgrind is a failure, not a skip: Environment A has it (vm/linux/Dockerfile).
        self.assertTrue(shutil.which("valgrind"), "valgrind is missing: the syscall audit cannot run")
        img = fx("mirror-split")
        fd = os.open(img, os.O_RDONLY)
        try:
            r = subprocess.run(["valgrind", "--trace-syscalls=yes", "--quiet", "--error-exitcode=99",
                                ARGS.probe, "--fd", str(fd)], pass_fds=(fd,), capture_output=True)
        finally:
            os.close(fd)
        self.assertEqual(r.returncode, 0, r.stderr[-400:])
        log = r.stderr.decode(errors="replace")
        # valgrind prints: SYSCALL[pid,tid](nr) sys_openat ( -100, 0xADDR(/path), flags ) --> ...
        events = []  # (kind, argument text) in order
        for m in re.finditer(r"SYSCALL\[\d+,\d+\]\( *\d+\) sys_(\w+) \( ([^)]*(?:\([^)]*\))?[^)]*)\)", log):
            events.append((m.group(1), m.group(2)))
        reads = [i for i, (k, a) in enumerate(events) if k == "pread64" and a.startswith("%d," % fd)]
        self.assertGreater(len(reads), 10, "no pread64 of the device in the trace: format not recognised")
        paths = []
        for i, (k, a) in enumerate(events):
            if k in ("open", "openat", "openat2", "creat"):
                paths.append(a.split("(", 1)[1] if "(" in a else a)
        self.assertGreater(len(paths), 0, "no open() in the trace: format not recognised")
        # only the dynamic loader, libc and valgrind's own preload libraries: no data path, no /dev/fd/N
        loose = [p for p in paths if not re.match(r"^(/etc/ld\.so\.|/lib/|/usr/lib/|/usr/libexec/valgrind/)", p)]
        self.assertEqual(loose, [], "paths opened: %s" % paths)
        dev = [(k, a) for k, a in events
               if k in ("write", "pwrite64", "writev", "pwritev", "ftruncate", "fallocate", "fsync", "fdatasync",
                        "ioctl", "lseek", "dup", "dup3") and re.match(r"(%d|0x%x)[,)]? ?" % (fd, fd), a)]
        self.assertEqual(dev, [], "write-type or position call on the device descriptor: %s" % dev)
        line = "syscall audit: %s; %d open() calls %s (loader, libc, valgrind preloads only); %d pread64 of the device; no write-type call on fd %d" % (
            subprocess.run(["valgrind", "--version"], capture_output=True).stdout.decode().strip(),
            len(paths), paths, len(reads), fd)
        print(line, file=sys.stderr)
        ev = os.environ.get("NTFSMAC_EVIDENCE_DIR")
        if ev:
            with open(os.path.join(ev, "syscall-audit.log"), "w") as f:
                f.write(line + "\n")


MANIFEST_BY_NAME = {}


def collect(dirs):
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".img"):
                FIX.setdefault(fn[:-4], os.path.join(d, fn))
                LISTS.setdefault(fn[:-4], d)


def main():
    global ARGS, SCHEMA, CONTRACT, TMP, MANIFEST
    ap = argparse.ArgumentParser()
    ap.add_argument("--probe", required=True)
    ap.add_argument("--probe-test", required=True)
    ap.add_argument("--images", action="append", default=[], help="directory of fixture images (repeatable)")
    ap.add_argument("--manifest", required=True)
    ap.add_argument("--schema", required=True)
    ap.add_argument("--contract", required=True, help="tests/unit/contracts/test_probe_schema.py")
    ap.add_argument("--setvolflags")
    ARGS, rest = ap.parse_known_args()
    CONTRACT = load_contract()
    with open(ARGS.schema, encoding="utf-8") as f:
        SCHEMA = CONTRACT.loads_strict(f.read())
    with open(ARGS.manifest, encoding="utf-8") as f:
        MANIFEST = json.load(f)
    for e in MANIFEST["fixtures"]:
        MANIFEST_BY_NAME[e["name"]] = e
    collect(ARGS.images)
    TMP = tempfile.mkdtemp(prefix="probe-test-")
    print("fixtures: %d images from %s" % (len(FIX), ", ".join(ARGS.images)), file=sys.stderr)
    try:
        res = unittest.main(argv=[sys.argv[0]] + rest, verbosity=2, exit=False)
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    asan = "/tmp/asan"
    if os.environ.get("NTFS_ASAN") == "1" and os.path.isdir(asan) and os.listdir(asan):
        print("sanitizer reports in %s: %s" % (asan, os.listdir(asan)), file=sys.stderr)
        return 1
    return 0 if res.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
