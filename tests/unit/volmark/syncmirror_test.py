#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""U-VMK-11..14 (docs/TEST_STRATEGY.md): `ntfsmac-volmark sync-mirror`, the stale-$MFTMirr repair of
docs/ARCHITECTURE.md section 3.7 (ADR-021, T-M4-16). Stdlib only.

Runs the built ntfsmac-volmark over copies of the fixture images (Environment A, through
tests/unit/volmark/run.sh; fixtures from tests/fixtures/gen/mirror.sh, unsafe.sh and clean.sh). The
device is always a regular file in a temporary directory, opened O_RDWR and passed as descriptor 3, as
the daemon does. Every run is checked against a copy of the image taken before it:

  U-VMK-11  the repair of a split of record 0, of record 3 (set cut, clear cut) and of both: exit 0,
            `w` and `d`; the only bytes that change are inside the differing $MFTMirr records, which
            end up byte-equal to the $MFT records (sector diff); the next probe says what the table
            of section 3.7 says; the engine mounts the image read-only (ntfsinfo -m) and ntfsfix -n
            exits 0 on a scratch copy.
  U-VMK-12  refusals leave the image hash unchanged and print no progress mark: the mirror rule
            negated (mirror-newer, mirror-torn, mirror-extra), a volume that is not split, a hibernation
            file, a volume flag besides the informational ones, a wrong serial or sector count.
  U-VMK-13  the fault layer (patch 0004, --volmark-fault): the writes of the repair are counted
            (exactly one per differing record, at the $MFTMirr offset, one record long), then cut:N,
            short:N:K, skip:N, eio:N and dead:N over every write, with and without the kill: each outcome is
            classified from the differing records and checked against its row of the crash table of
            section 3.7 and a fresh probe; a rerun after a cut that was not inside a record write exits 0;
            without the kill, a lost write is never reported as success.
  U-VMK-14  a second run finds nothing to do: exit 10, image unchanged.
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
from ntfsrec import find_attr, geometry, mutate_record, set_volume_flags, synth_split  # noqa: E402
from vmk_make import VOLUME_IS_DIRTY, boot_ids, flags_offset, make, next_usn  # noqa: E402

ARGS = None
FIX = {}
TMP = None
EVIDENCE = []

RESIZE_LOG_FILE = 0x0002
MODIFIED_BY_CHKDSK = 0x8000

_n = [0]


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def copy(src, tag="img"):
    _n[0] += 1
    dst = os.path.join(TMP, "%s-%d.img" % (tag, _n[0]))
    shutil.copyfile(src, dst)
    return dst


def fx(name):
    if name not in FIX:  # a missing fixture is a failure: nothing is skipped silently
        raise AssertionError("fixture %s not present (tests/unit/volmark/run.sh makes them)" % name)
    return FIX[name]


def run_sync(img, tool=None, serial=None, sectors=None, env=None, extra=(), redirect="<>"):
    """sync-mirror with `img` as descriptor 3."""
    tool = tool or ARGS.volmark
    s, n = boot_ids(img)
    serial = s if serial is None else serial
    sectors = n if sectors is None else sectors
    argv = [tool, "sync-mirror", "--fd", "3", "--expect-serial", serial, "--expect-sectors", str(sectors)] + list(extra)
    full_env = {k: v for k, v in os.environ.items() if not k.startswith("NTFSMAC_FAULT_")}
    full_env.update(env or {})
    return subprocess.run(["sh", "-c", 'img=$1; shift; exec "$@" 3%s"$img"' % redirect, "sh", img] + argv,
                          env=full_env, capture_output=True)


def probe(img):
    before = sha256(img)
    r = subprocess.run(["sh", "-c", 'img=$1; shift; exec "$@" 3<"$img"', "sh", img, ARGS.probe, "--fd", "3"],
                       capture_output=True)
    assert sha256(img) == before, "the probe changed the image"
    assert r.returncode == 0, "probe exit %d: %r" % (r.returncode, r.stderr[-200:])
    return json.loads(r.stdout)


def record(img, which, k):
    g = geometry(img)
    base = g["mft_off"] if which == "mft" else g["mir_off"]
    with open(img, "rb") as f:
        f.seek(base + k * g["rs"])
        return f.read(g["rs"])


def differing(img):
    """Records 0 to 3 whose raw bytes differ between $MFT and $MFTMirr (what the writer copies)."""
    return [k for k in range(4) if record(img, "mft", k) != record(img, "mir", k)]


def mirror_ranges(img, ks):
    g = geometry(img)
    return [(g["mir_off"] + k * g["rs"], g["mir_off"] + (k + 1) * g["rs"]) for k in ks]


def changed_offsets(a_img, b_img):
    out = []
    with open(a_img, "rb") as fa, open(b_img, "rb") as fb:
        off = 0
        while True:
            a = fa.read(1 << 20)
            b = fb.read(1 << 20)
            if a != b:
                if len(a) != len(b):
                    raise AssertionError("images differ in length")
                out.extend(off + i for i in range(len(a)) if a[i] != b[i])
            if not a:
                return out
            off += len(a)


def engine_accepts(img):
    """The pinned engine mounts a scratch copy read-only (ntfsinfo -m) and `ntfsfix -n` exits 0."""
    c = copy(img, "engine")
    r = subprocess.run(["ntfsinfo", "-f", "-m", c], capture_output=True, text=True)
    ok = r.returncode == 0 and "does not match" not in r.stdout + r.stderr
    f = subprocess.run(["ntfsfix", "-n", c], capture_output=True, text=True)
    return ok, f.returncode


def make_split0_and_3(src, dst):
    """mirror-split plus a split of record 3: $MFT record 3 one USN ahead with VOLUME_IS_DIRTY set (as
    an interrupted `set` leaves it). Differing records: 0 and 3."""
    shutil.copyfile(src, dst)

    def edit(rec, usn):
        fo = flags_offset(rec)
        rec[fo] |= VOLUME_IS_DIRTY
        return next_usn(usn)

    mutate_record(dst, "mft", 3, edit)


class Base(unittest.TestCase):
    def assertRefused(self, img, rc, reason, **kw):
        """Exit `rc`, no progress mark, image unchanged, and the stderr line that names the check that
        refused (so that a hibernation refusal is not a rule refusal in disguise)."""
        before = sha256(img)
        r = run_sync(img, **kw)
        self.assertEqual(r.returncode, rc, "exit %d, stderr %r" % (r.returncode, r.stderr[-300:]))
        self.assertIn(reason.encode(), r.stderr, "wrong refusal: %r" % r.stderr[-300:])
        self.assertEqual(r.stdout, b"", "a refusal printed a progress mark")
        self.assertEqual(sha256(img), before, "a refusal changed the image")
        return r

    def assertRepaired(self, src_name_or_path, want_probe, want_diff, tag="rep"):
        """Run sync-mirror on a copy; return the image. Checks the whole of U-VMK-11 for one image."""
        orig = src_name_or_path if os.path.isabs(src_name_or_path) else fx(src_name_or_path)
        img = copy(orig, tag)
        before = copy(orig, tag + "-before")
        self.assertEqual(differing(img), want_diff)
        r = run_sync(img)
        self.assertEqual(r.returncode, 0, "exit %d, stderr %r" % (r.returncode, r.stderr[-300:]))
        self.assertEqual(r.stdout, b"w\nd\n")
        # no byte outside the differing $MFTMirr records changed (this covers $MFT, $LogFile, the flags)
        ranges = mirror_ranges(img, want_diff)
        changed = changed_offsets(before, img)
        self.assertTrue(changed, "nothing was written")
        stray = [o for o in changed if not any(s <= o < e for s, e in ranges)]
        self.assertEqual(stray, [], "bytes outside the differing $MFTMirr records changed: %s" % stray[:8])
        for k in want_diff:
            self.assertEqual(record(img, "mir", k), record(before, "mft", k), "mirror record %d is not $MFT's" % k)
        self.assertEqual(differing(img), [], "records still differ")
        for k in range(4):  # $MFT itself, record by record, and the records that did not differ
            self.assertEqual(record(img, "mft", k), record(before, "mft", k))
            if k not in want_diff:
                self.assertEqual(record(img, "mir", k), record(before, "mir", k))
        self.assertEqual(probe(img)["state"], want_probe)
        ok, fix_rc = engine_accepts(img)
        self.assertTrue(ok, "the engine does not mount the repaired image")
        self.assertEqual(fix_rc, 0, "ntfsfix -n exit %d" % fix_rc)
        EVIDENCE.append("U-VMK-11 %s: exit 0, w d; %d byte(s) changed, all inside the mirror record(s) %s; "
                        "probe %s; ntfsinfo -m ok, ntfsfix -n exit 0" % (os.path.basename(orig), len(changed),
                                                                         want_diff, want_probe))
        return img


class SyncMirror(Base):
    # ---- U-VMK-11 -----------------------------------------------------------------------------------
    def test_vmk11_record0(self):
        self.assertRepaired("mirror-split", "clean", [0])

    def test_vmk11_record0_dirty(self):
        self.assertRepaired("mirror-split-dirty", "dirty_flag", [0])

    def test_vmk11_record3_set_cut(self):
        for reverse, want in ((False, "dirty_flag"), (True, "clean")):
            if not ARGS.volmark_fault:
                self.fail("--volmark-fault not given: vmk-split is made with the fault layer")
            name = "vmk-split-clear" if reverse else "vmk-split"
            path = os.path.join(TMP, name + ".img")
            make(ARGS.volmark_fault, fx("clean-64m-4k"), path, reverse=reverse)
            self.assertRepaired(path, want, [3], tag=name)

    def test_vmk11_records_0_and_3(self):
        both = os.path.join(TMP, "split0and3.img")
        make_split0_and_3(fx("mirror-split"), both)
        self.assertEqual(probe(both)["state"], "mirror_split")
        self.assertRepaired(both, "dirty_flag", [0, 3])

    # ---- U-VMK-12 -----------------------------------------------------------------------------------
    def test_vmk12_refusals(self):
        rule = "the mirror rule does not hold"
        want = {"mirror-newer": rule, "mirror-torn": rule, "mirror-extra": rule, "clean-64m-4k": rule,
                "hiber-lower": rule, "mirror-split-hiber": "hibernation file"}
        for name, reason in want.items():
            with self.subTest(image=name):
                img = copy(fx(name), "ref")
                self.assertRefused(img, 10, reason)
                EVIDENCE.append("U-VMK-12 %s: exit 10 (%s), no mark, image unchanged" % (name, reason))
        # a split mirror whose $LogFile is unclean, or is version 2.0: volmark's own log refusal on the view
        for name in ("unclean-log", "log-v2"):
            with self.subTest(image=name + "+split"):
                img = copy(fx(name), "logsplit")
                synth_split(img)
                self.assertRefused(img, 10, "$LogFile")
                EVIDENCE.append("U-VMK-12 %s with a split mirror: exit 10 ($LogFile refusal on the view), "
                                "no mark, image unchanged" % name)

    def test_vmk12_binding(self):
        img = copy(fx("mirror-split"), "bind")
        serial, sectors = boot_ids(img)
        self.assertRefused(img, 15, "binding", serial="%016x" % (int(serial, 16) ^ 1))
        self.assertRefused(img, 15, "binding", sectors=sectors + 1)
        self.assertRefused(img, 15, "binding", sectors=sectors - 1)
        EVIDENCE.append("U-VMK-12 mirror-split with a wrong serial or sector count: exit 15, image unchanged")

    def test_vmk12_flags(self):
        # a split with another volume flag set in both copies of record 3: exit 13 (step 2)
        for flags in (RESIZE_LOG_FILE, VOLUME_IS_DIRTY | RESIZE_LOG_FILE):
            img = copy(fx("mirror-split"), "flags")
            set_volume_flags(img, flags)
            self.assertRefused(img, 13, "volume flag")
        # the informational bits are allowed
        img = copy(fx("mirror-split"), "info")
        set_volume_flags(img, MODIFIED_BY_CHKDSK)
        r = run_sync(img)
        self.assertEqual(r.returncode, 0, r.stderr[-300:])
        EVIDENCE.append("U-VMK-12 split with a flag besides DIRTY and the informational two: exit 13, unchanged; "
                        "with MODIFIED_BY_CHKDSK: exit 0")

    def test_vmk12_usage_and_descriptor(self):
        img = copy(fx("mirror-split"), "usage")
        before = sha256(img)
        for argv in (["sync-mirror"], ["sync-mirror", "--fd", "3"],
                     ["sync-mirror", "--fd", "3", "--expect-serial", "0", "--expect-sectors", "1"],
                     ["sync-mirror", "--fd", "3", "--expect-serial", "%016x" % 1, "--expect-sectors", "1",
                      "--stop-before-write"]):
            r = subprocess.run(["sh", "-c", 'img=$1; shift; exec "$@" 3<>"$img"', "sh", img, ARGS.volmark] + argv,
                               capture_output=True)
            self.assertEqual(r.returncode, 2, argv)
            self.assertEqual(r.stdout, b"")
        # a read-only descriptor is refused before anything is read
        self.assertRefused(img, 3, "descriptor", redirect="<")
        self.assertEqual(sha256(img), before)

    # ---- U-VMK-14 -----------------------------------------------------------------------------------
    def test_vmk14_twice(self):
        img = self.assertRepaired("mirror-split", "clean", [0], tag="twice")
        self.assertRefused(img, 10, "the mirror rule does not hold")
        EVIDENCE.append("U-VMK-14 second run: exit 10, image unchanged")

    # ---- nothing is written before `w` --------------------------------------------------------------
    def test_sigkill_before_write(self):
        img = copy(fx("mirror-split"), "stop")
        before = sha256(img)
        serial, sectors = boot_ids(img)
        p = subprocess.Popen(["sh", "-c", 'img=$1; shift; exec "$@" 3<>"$img"', "sh", img, ARGS.volmark_test,
                              "sync-mirror", "--fd", "3", "--expect-serial", serial, "--expect-sectors",
                              str(sectors), "--stop-before-write"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
        _, status = os.waitpid(p.pid, os.WUNTRACED)
        self.assertTrue(os.WIFSTOPPED(status), "volmark_test did not stop before the write")
        os.kill(p.pid, signal.SIGKILL)
        out, _ = p.communicate()
        self.assertEqual(p.returncode, -signal.SIGKILL)
        self.assertEqual(out, b"", "a progress mark before the write")
        self.assertEqual(sha256(img), before, "image changed before the write")
        self.assertEqual(probe(img)["state"], "mirror_split")
        EVIDENCE.append("U-VMK-13 SIGKILL right before `w`: no output, image unchanged, probe mirror_split")


class SyncMirrorFault(Base):
    """U-VMK-13: the fault layer (patch 0004)."""

    def setUp(self):
        if not ARGS.volmark_fault:
            self.fail("--volmark-fault not given: the cut tests need volmark_test built with --enable-faultio")

    def run_fault(self, img, plan, kill=False, state=None, trace=None):
        env = {"NTFSMAC_FAULT_PLAN": plan}
        if kill:
            env["NTFSMAC_FAULT_KILL"] = "1"
        if state:
            env["NTFSMAC_FAULT_STATE"] = state
        if trace:
            env["NTFSMAC_FAULT_TRACE"] = trace
        return run_sync(img, tool=ARGS.volmark_fault, env=env)

    @staticmethod
    def writes(r):
        for line in r.stderr.decode("utf-8", "replace").splitlines():
            if line.startswith("ntfsmac-faultio:") and "writes=" in line:
                return int(line.rsplit("writes=", 1)[1])
        raise AssertionError("no fault-layer line on stderr: %r" % r.stderr[-300:])

    def test_count_and_offsets(self):
        """One device write per differing record, at the $MFTMirr record, one record long; none before."""
        both = os.path.join(TMP, "split0and3.img")
        make_split0_and_3(fx("mirror-split"), both)
        for path, ks in ((fx("mirror-split"), [0]), (both, [0, 3])):
            img = copy(path, "count")
            g = geometry(img)
            trace = os.path.join(TMP, "trace-%d" % _n[0])
            r = self.run_fault(img, "count", trace=trace)
            self.assertEqual(r.returncode, 0, r.stderr[-300:])
            self.assertEqual(self.writes(r), len(ks))
            with open(trace) as f:
                rows = [line.split() for line in f if line.strip()]
            self.assertEqual([int(x[1]) for x in rows], [g["mir_off"] + k * g["rs"] for k in ks])
            self.assertEqual([int(x[2]) for x in rows], [g["rs"]] * len(ks))
            self.assertEqual([x[3] for x in rows], ["46494c45"] * len(ks))  # "FILE"
            EVIDENCE.append("U-VMK-13 count %s: %d write(s), at the $MFTMirr offset(s) of record(s) %s, %d bytes "
                            "each, none before `w`" % (os.path.basename(path), len(ks), ks, g["rs"]))

    def state_of(self, img, before, ks):
        """Per differing record: old (as before), new (== $MFT) or torn."""
        out = []
        for k in ks:
            m = record(img, "mir", k)
            if m == record(before, "mir", k):
                out.append("old")
            elif m == record(before, "mft", k):
                out.append("new")
            else:
                out.append("torn")
        return out

    def check_outcome(self, tag, src, ks, plan, kill):
        img = copy(src, "cut")
        before = copy(src, "cutb")
        r = self.run_fault(img, plan, kill)
        # whatever happened: only bytes inside the differing mirror records changed
        ranges = mirror_ranges(img, ks)
        stray = [o for o in changed_offsets(before, img) if not any(s <= o < e for s, e in ranges)]
        self.assertEqual(stray, [], "%s %s: bytes outside the differing mirror records changed" % (tag, plan))
        states = self.state_of(img, before, ks)
        pr = probe(img)["state"]
        if kill and states != ["new"] * len(ks):
            self.assertEqual(r.returncode, -signal.SIGKILL, "%s %s: not killed at the cut" % (tag, plan))
        # a lost or torn write is never reported as success
        if states != ["new"] * len(ks):
            self.assertNotEqual(r.returncode, 0, "%s %s: success reported with states %s" % (tag, plan, states))
        # the crash table of section 3.7
        if "torn" in states:
            row = "inside a record write"
            self.assertEqual(pr, "corrupt")
            rr = run_sync(img)
            self.assertEqual(rr.returncode, 10, "a torn mirror must be refused")
        elif all(s == "new" for s in states):
            row = "after the last write"
            self.assertIn(pr, ("clean", "dirty_flag"))
        else:
            row = "before step 3" if all(s == "old" for s in states) else "between two record writes"
            self.assertEqual(pr, "mirror_split")
            rr = run_sync(img)
            self.assertEqual(rr.returncode, 0, "a rerun must finish: %r" % rr.stderr[-300:])
            self.assertEqual(differing(img), [])
            self.assertIn(probe(img)["state"], ("clean", "dirty_flag"))
        return "%s %s%s: exit %d, mirror records %s, probe %s -> %s" % (tag, plan, " +kill" if kill else "",
                                                                         r.returncode, states, pr, row)

    def test_vmk13_cut_every_write(self):
        both = os.path.join(TMP, "split0and3.img")
        make_split0_and_3(fx("mirror-split"), both)
        for tag, src, ks in (("mirror-split", fx("mirror-split"), [0]), ("split0and3", both, [0, 3])):
            n = len(ks)
            plans = ["cut:%d" % i for i in range(0, n + 1)]
            plans += ["short:%d:%d" % (i, k) for i in range(1, n + 1) for k in (0, 1, 511, 512, 513, 1023)]
            plans += ["skip:%d" % i for i in range(1, n + 1)]
            plans += ["eio:%d" % i for i in range(1, n + 1)] + ["dead:%d" % i for i in range(1, n + 1)]
            for plan in plans:
                for kill in (True, False):
                    if kill and not plan.startswith(("cut", "short")):
                        continue
                    with self.subTest(image=tag, plan=plan, kill=kill):
                        EVIDENCE.append("U-VMK-13 " + self.check_outcome(tag, src, ks, plan, kill))


def collect(dirs):
    for d in dirs:
        if not os.path.isdir(d):
            continue
        for fn in sorted(os.listdir(d)):
            if fn.endswith(".img"):
                FIX.setdefault(fn[:-4], os.path.join(d, fn))


def main():
    global ARGS, TMP
    ap = argparse.ArgumentParser()
    ap.add_argument("--volmark", required=True)
    ap.add_argument("--volmark-test", required=True)
    ap.add_argument("--volmark-fault")
    ap.add_argument("--probe", required=True)
    ap.add_argument("--images", action="append", default=[])
    ap.add_argument("--out", help="directory for syncmirror-evidence.txt")
    ARGS, rest = ap.parse_known_args()
    collect(ARGS.images)
    TMP = tempfile.mkdtemp(prefix="syncmirror-test-")
    print("fixtures: %d images from %s" % (len(FIX), ", ".join(ARGS.images)), file=sys.stderr)
    try:
        res = unittest.main(argv=[sys.argv[0]] + rest, verbosity=2, exit=False)
    finally:
        shutil.rmtree(TMP, ignore_errors=True)
    if ARGS.out:
        os.makedirs(ARGS.out, exist_ok=True)
        with open(os.path.join(ARGS.out, "syncmirror-evidence.txt"), "w", encoding="utf-8") as f:
            f.write("\n".join(EVIDENCE) + "\n")
    asan = "/tmp/asan"
    if os.environ.get("NTFS_ASAN") == "1" and os.path.isdir(asan) and os.listdir(asan):
        print("sanitizer reports in %s: %s" % (asan, os.listdir(asan)), file=sys.stderr)
        return 1
    return 0 if res.result.wasSuccessful() else 1


if __name__ == "__main__":
    sys.exit(main())
