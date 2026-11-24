#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Stale-$MFTMirr fixture images (T-M3-02), made with the crash-sweep tools (T-M3-14).

Runs INSIDE Environment A, called by tests/fixtures/gen/mirror.sh (which builds the
fault-layer NTFS-3G first). It imports tests/stress/crashsweep/crashsweep.py for the
workloads, the mount and unmount helpers and the paths of the builds.

Images (docs/TEST_STRATEGY.md 3.2):
  mirror-split         workload smallfiles on a fresh mkntfs image, fault plan cut:N, N = 68:
                       write N is $MFT record 0, write N+1 would be $MFTMirr record 0
  mirror-newer         mirror-split with the two record-0 copies exchanged: the state after
                       write N+1 reached the device and write N did not (what plan skip:N
                       followed by power loss leaves; skip:N alone is healed by the next
                       record-0 pair of the workload)
  mirror-torn          plan short:68:512: only the first 512 bytes of write N reached the device
  mirror-extra         mirror-split with one byte of the mirror's record 0 changed in
                       $STANDARD_INFORMATION (outside what rule M4 allows), fix-up recomputed
Extras for U-PROBE-MIRROR-03:
  mirror-split-dirty   VOLUME_IS_DIRTY set before the workload (a session with the marker)
  mirror-split-hiber   /hiberfil.sys with a `hibr` header created before the cut
And for U-PROBE-MIRROR-05, the failing cut points of the full and the short sweep
(docs/spikes/M3-crashsweep-triage.md), regenerated: sweep-WORKLOAD-cN.img.

Every image is checked: the engine refuses it with the expected message; for the images
the rule must accept, copying $MFT record 0 over the mirror record on a scratch copy gives a
read-only mount and `ntfsfix -n` exit 0 (the sweeper's mirror_split classification).
Nothing is mounted on the host; images stay in the container.
Exit: 0 all images made and checked, 1 a check failed, 2 setup failure.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

ROOT = os.environ.get("CRASHSWEEP_ROOT", "/tmp/crashsweep")
os.environ["CRASHSWEEP_ROOT"] = ROOT
sys.path.insert(0, os.environ.get("CRASHSWEEP_DIR", "/work/tests/stress/crashsweep"))
import crashsweep as cs  # noqa: E402

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from setserial import set_serial  # noqa: E402

N_SPLIT = 68
# Failing cut points of the full (110 cuts per workload) and the short (50) sweep, from
# docs/spikes/M3-crashsweep-triage.md. Each must still be a $MFT/$MFTMirr record-0 pair
# in the write trace, or the image is refused.
SWEEP_FULL = {
    "smallfiles": [68, 558, 693, 760, 1283, 1385],
    "renamechurn": [70, 263, 305, 415, 471, 1162],
    "dirtree": [276, 459, 2292, 2338, 3255, 3805],
}
SWEEP_SHORT = {
    "smallfiles": [76, 226, 864, 1089],
    "renamechurn": [1015, 1138, 1384],
    "dirtree": [510, 1020, 1326, 4079],
}
RECORD0_MSG = "$MFTMirr does not match $MFT (record 0)."
TORN_MSG = "Incomplete multi-sector transfer"

failures = []
results = []


def log(msg):
    print(msg, flush=True)


def check(ok, what):
    results.append(("ok      " if ok else "MISMATCH ") + what)
    log(results[-1])
    if not ok:
        failures.append(what)


class Geometry:
    def __init__(self, base):
        with open(base, "rb") as f:
            bs = f.read(512)
        bps, spc = struct.unpack_from("<HB", bs, 11)
        self.mft_lcn, self.mir_lcn = struct.unpack_from("<QQ", bs, 0x30)
        cpr = struct.unpack_from("<b", bs, 0x40)[0]
        self.cluster = bps * spc
        self.rs = (1 << -cpr) if cpr < 0 else cpr * self.cluster
        self.mft_off = self.mft_lcn * self.cluster
        self.mir_off = self.mir_lcn * self.cluster


def make_base(path, label, dirty_tool=None, dirty=False):
    with open(path, "wb") as f:
        f.truncate(64 << 20)
    rc, msg = cs.run([cs.MKNTFS_REL, "-F", "-Q", "-T", "-L", label, "-s", "512", "-c", "4096", path])
    if rc != 0:
        sys.exit("mkntfs failed: " + msg)
    if dirty:
        rc, msg = cs.run([dirty_tool, path, "--set", "DIRTY"])
        if rc != 0:
            sys.exit("setvolflags failed: " + msg)


def pre_hiber(mnt):
    """/hiberfil.sys with a `hibr` header, synced, before the workload starts."""
    p = os.path.join(mnt, "hiberfil.sys")
    with open(p, "wb") as f:
        f.write(b"hibr" + b"\0" * (8192 - 4))
        f.flush()
        os.fsync(f.fileno())
    fd = os.open(mnt, os.O_RDONLY)
    try:
        os.fsync(fd)
    finally:
        os.close(fd)


def run_workload(work, base, wl, plan, name, trace=False, hiber=False):
    """Fresh copy of base, fault-layer mount, workload, plan applied. Returns (image, trace dict,
    counter after the pre-step)."""
    d = os.path.join(work, name)
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(os.path.join(d, "mnt"))
    img = os.path.join(d, "disk.img")
    state = os.path.join(d, "state")
    events = os.path.join(d, "events")
    tr = os.path.join(d, "trace")
    cs.run(["cp", "--sparse=always", base, img])
    env = dict(os.environ, NTFSMAC_FAULT_PLAN=plan, NTFSMAC_FAULT_STATE=state)
    if trace:
        env["NTFSMAC_FAULT_TRACE"] = tr
    if plan != "count":
        env["NTFSMAC_FAULT_KILL"] = "1"
    rc, msg = cs.run([cs.NTFS3G_FAULT, img, os.path.join(d, "mnt"), "-o", cs.MOUNT_OPTS], env=env)
    if rc != 0:
        sys.exit("fault-layer mount failed: " + msg)
    c0 = 0
    if hiber:
        pre_hiber(os.path.join(d, "mnt"))
        c0 = cs.Wl(d, state, os.devnull).cnt()
    cs.run([sys.executable, "-I", cs.HERE, "_workload", wl, os.path.join(d, "mnt"), state, events],
           timeout=cs.WORKLOAD_TIMEOUT)
    cs.unmount(os.path.join(d, "mnt"), img)
    return img, cs.Sweep.read_trace(tr) if trace else {}, c0


def is_pair(trace, n, g, k=0):
    a, b = trace.get(n), trace.get(n + 1)
    return bool(a and b and (a[0], a[1]) == (g.mft_off + k * g.rs, g.rs) and
                (b[0], b[1]) == (g.mir_off + k * g.rs, g.rs))


CHECK_MNT = "/tmp/ntfsmac-mirror-check.mnt"


def mount_msg(img, opts="ro,norecover"):
    mnt = CHECK_MNT
    os.makedirs(mnt, exist_ok=True)
    rc, msg = cs.run([cs.NTFS3G_REL, img, mnt, "-o", opts])
    if rc == 0:
        cs.unmount(mnt, img)
    return rc, msg


def repaired_ok(img, g):
    """Sweeper classification (c): $MFT record 0 over the mirror record on a scratch copy; the
    result mounts read-only and ntfsfix -n exits 0."""
    scratch = img + ".scratch"
    cs.run(["cp", "--sparse=always", img, scratch])
    with open(scratch, "r+b") as f:
        f.seek(g.mft_off)
        rec = f.read(g.rs)
        f.seek(g.mir_off)
        f.write(rec)
    rc1, m1 = mount_msg(scratch)
    rc2, m2 = cs.run([cs.NTFSFIX_REL, "-n", scratch])
    os.unlink(scratch)
    return rc1 == 0 and rc2 == 0


def usa_unprotect(rec):
    """Raw record -> (record with the original sector tails put back, usn)."""
    usa_ofs, usa_count = struct.unpack_from("<HH", rec, 4)
    usn = struct.unpack_from("<H", rec, usa_ofs)[0]
    out = bytearray(rec)
    for i in range(1, usa_count):
        assert struct.unpack_from("<H", rec, i * 512 - 2)[0] == usn
        out[i * 512 - 2:i * 512] = rec[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]
    return out, usn


def usa_protect(rec, usn):
    """Record with original tails -> raw record carrying the given USN."""
    usa_ofs, usa_count = struct.unpack_from("<HH", rec, 4)
    out = bytearray(rec)
    struct.pack_into("<H", out, usa_ofs, usn)
    for i in range(1, usa_count):
        out[usa_ofs + 2 * i:usa_ofs + 2 * i + 2] = rec[i * 512 - 2:i * 512]
        struct.pack_into("<H", out, i * 512 - 2, usn)
    return out


def derive_newer(src, dst, g):
    shutil.copy(src, dst)
    with open(dst, "r+b") as f:
        f.seek(g.mft_off)
        a = f.read(g.rs)
        f.seek(g.mir_off)
        b = f.read(g.rs)
        f.seek(g.mft_off)
        f.write(b)
        f.seek(g.mir_off)
        f.write(a)


def derive_extra(src, dst, g):
    """One byte of the mirror's record 0 changed in the creation time of $STANDARD_INFORMATION."""
    shutil.copy(src, dst)
    with open(dst, "r+b") as f:
        f.seek(g.mir_off)
        raw = f.read(g.rs)
        rec, usn = usa_unprotect(raw)
        off = struct.unpack_from("<H", rec, 0x14)[0]
        assert struct.unpack_from("<I", rec, off)[0] == 0x10, "record 0 starts with $STANDARD_INFORMATION"
        value = off + struct.unpack_from("<H", rec, off + 0x14)[0]
        rec[value + 2] ^= 0x01  # inside creation_time
        f.seek(g.mir_off)
        f.write(usa_protect(rec, usn))


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--work", default="/tmp/ntfsmac-mirror-work")
    ap.add_argument("--only", help="image name (sweep images: use --only sweep)")
    ap.add_argument("--setvolflags", default="/tmp/ntfsmac-names-bin/setvolflags")
    args = ap.parse_args()
    for b in (cs.NTFS3G_FAULT, cs.NTFS3G_REL, cs.NTFSFIX_REL, cs.MKNTFS_REL):
        if not os.path.exists(b):
            sys.exit("missing %s: run build.sh first" % b)
    out, work = args.out, args.work
    os.makedirs(out, exist_ok=True)
    shutil.rmtree(work, ignore_errors=True)
    os.makedirs(work)

    def want(name):
        return args.only is None or args.only == name or (args.only == "sweep" and name.startswith("sweep-"))

    base = os.path.join(work, "base.img")
    make_base(base, "mirror-split")
    g = Geometry(base)
    log("geometry: cluster %d record %d $MFT at %#x $MFTMirr at %#x" % (g.cluster, g.rs, g.mft_off, g.mir_off))

    # Trace of the whole smallfiles workload on the plain base: is N a record-0 pair?
    _, trace, _ = run_workload(work, base, "smallfiles", "count", "trace-smallfiles", trace=True)
    check(is_pair(trace, N_SPLIT, g), "smallfiles: write %d is $MFT record 0 and write %d $MFTMirr record 0" %
          (N_SPLIT, N_SPLIT + 1))

    def finish(name, img, expect_msg, rule_accepts):
        dst = os.path.join(out, name + ".img")
        shutil.move(img, dst)
        set_serial(dst, name)  # a serial of its own: the daemon's journal key (setserial.py)
        rc, msg = mount_msg(dst)
        first = next((ln.strip() for ln in msg.splitlines() if ln.strip()), "")
        check(rc != 0 and expect_msg in msg, "%s: ro,norecover mount refused with %r (got rc %d: %s)" %
              (name, expect_msg, rc, first))
        if rule_accepts is not None:
            check(repaired_ok(dst, g) == rule_accepts,
                  "%s: copying $MFT record 0 over the mirror %s a clean read-only mount and ntfsfix -n" %
                  (name, "gives" if rule_accepts else "does not give"))
        return dst

    split = None
    if want("mirror-split") or want("mirror-newer") or want("mirror-extra"):
        img, _, _ = run_workload(work, base, "smallfiles", "cut:%d" % N_SPLIT, "mirror-split")
        split = finish("mirror-split", img, RECORD0_MSG, True)
    if want("mirror-newer"):
        dst = os.path.join(out, "mirror-newer.img")
        derive_newer(split, dst, g)
        set_serial(dst, "mirror-newer")
        rc, msg = mount_msg(dst)
        check(rc != 0 and RECORD0_MSG in msg, "mirror-newer: ro,norecover mount refused with the record 0 message")
    if want("mirror-extra"):
        dst = os.path.join(out, "mirror-extra.img")
        derive_extra(split, dst, g)
        set_serial(dst, "mirror-extra")
        rc, msg = mount_msg(dst)
        check(rc != 0 and RECORD0_MSG in msg, "mirror-extra: ro,norecover mount refused with the record 0 message")
    if want("mirror-torn"):
        img, _, _ = run_workload(work, base, "smallfiles", "short:%d:512" % N_SPLIT, "mirror-torn")
        finish("mirror-torn", img, TORN_MSG, None)
    if want("mirror-split-dirty"):
        dbase = os.path.join(work, "base-dirty.img")
        make_base(dbase, "mirror-dirty", args.setvolflags, dirty=True)
        img, _, _ = run_workload(work, dbase, "smallfiles", "cut:%d" % N_SPLIT, "mirror-split-dirty")
        finish("mirror-split-dirty", img, RECORD0_MSG, True)
    if want("mirror-split-hiber"):
        _, htrace, c0 = run_workload(work, base, "smallfiles", "count", "trace-hiber", trace=True, hiber=True)
        n = next((k for k in sorted(htrace) if k > c0 + 8 and is_pair(htrace, k, g)), None)
        check(n is not None, "hiberfil variant: a record-0 pair after write %d exists" % (c0 + 8))
        if n is not None:
            img, _, _ = run_workload(work, base, "smallfiles", "cut:%d" % n, "mirror-split-hiber", hiber=True)
            finish("mirror-split-hiber", img, RECORD0_MSG, True)

    if args.only in (None, "sweep"):
        traces = {}
        for wl in ("smallfiles", "renamechurn", "dirtree"):
            if wl == "smallfiles":
                traces[wl] = trace
            else:
                _, traces[wl], _ = run_workload(work, base, wl, "count", "trace-" + wl, trace=True)
        for wl, ns in ((w, sorted(set(SWEEP_FULL[w]) | set(SWEEP_SHORT[w]))) for w in SWEEP_FULL):
            for n in ns:
                name = "sweep-%s-c%d" % (wl, n)
                ok = is_pair(traces[wl], n, g)
                check(ok, "%s: write %d is $MFT record 0 and write %d $MFTMirr record 0" % (name, n, n + 1))
                if not ok:
                    continue
                img, _, _ = run_workload(work, base, wl, "cut:%d" % n, name)
                finish(name, img, RECORD0_MSG, True)

    with open(os.path.join(out, "mirror-results.txt"), "w") as f:
        f.write("\n".join(results) + "\n")
    shutil.rmtree(work, ignore_errors=True)
    return 1 if failures else 0


if __name__ == "__main__":
    sys.exit(main())
