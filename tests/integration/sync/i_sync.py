# SPDX-License-Identifier: GPL-2.0-or-later
"""I-SYNC-01, I-SYNC-02 (T-M3-10, ADR-014, U10): the device sync path of the NTFS-3G worker on macOS.

I-SYNC-01  Trace upstream ntfs_fsync() (libntfs-3g/unix_io.c) at each sync point the worker gets:
           FUSE_FSYNC (from fsync, F_FULLFSYNC and sync(2) on open files) and FUSE_DESTROY at unmount.
           For each point: did the worker issue fcntl(F_FULLFSYNC) on the device, with what result, and
           did the plain fsync fallback run. The device calls are recorded by syncspy.c (a DYLD interpose
           library built and loaded here; no patch, no change to the worker). The FUSE requests come from
           the worker's own `-o debug` log. Done on an image file, on the buffered node (/dev/diskN) and
           on the raw node (/dev/rdiskN) of the same image attached with hdiutil, plus one run in which the
           interpose library makes F_FULLFSYNC fail, to show the fallback.
I-SYNC-02  Raw versus buffered node (U10): what the kernel accepts at the node (alignment), whether NTFS-3G
           can mount and write through each node without silent corruption, and a small benchmark of the
           variants default (image file), `sync` mount option, buffered node and raw node.

Everything runs in the Environment B guest on images this module creates in the case's work directory;
the only device nodes touched are the ones hdiutil returns for those images (checked by size and
"Virtual"). norecover always, backend=fskit, absolute mount points, a deadline and SIGKILL on every wait.
The nodes are owned by the guest user, so no sudo is needed; the worker gets the node as an inherited
descriptor named /dev/fd/N (patch 0003), as in the product.
"""

import fcntl
import hashlib
import json
import os
import plistlib
import re
import signal
import struct
import subprocess
import threading
import time

from harness import Fail, case
from harness.volume import detect_macfuse, guest_tool

HERE = os.path.dirname(os.path.abspath(__file__))
MIB = 1024 * 1024
IMG_SIZE = 256 * MIB
MOUNT_WAIT = 30
HDIUTIL = "/usr/bin/hdiutil"
DISKUTIL = "/usr/sbin/diskutil"
UMOUNT = "/sbin/umount"
MOUNT = "/sbin/mount"

FUSE_RE = re.compile(r"opcode: ([A-Z_]+) \((\d+)\), nodeid: (\d+)")
SPY_RE = re.compile(r"^(\d+\.\d+) (\S+) pid=(\d+) fd=(-?\d+) path=(.*?)(?: rc=(-?\d+) errno=(\d+))?"
                    r"(?: us=(\d+))?(?: writes=(\d+) bytes=(\d+))?$")


# ---------------------------------------------------------------------------------------------
# Environment
# ---------------------------------------------------------------------------------------------
def need_guest(ctx):
    """Skip unless this is Environment B with macFUSE and the guest-built NTFS-3G."""
    if ctx.env != "B":
        ctx.skip("the sync path is macOS only (FSKit backend, Darwin device nodes); Environment B")
    if detect_macfuse(ctx.transport) is None:
        ctx.skip("macFUSE is not installed in the guest")
    for tool in ("ntfs-3g", "mkntfs", "ntfsfix"):
        if guest_tool(tool) is None:
            ctx.skip("%s not found in the guest (build it with scripts/build-ntfs3g.sh --fuse external)" % tool)
    for p in (HDIUTIL, DISKUTIL, UMOUNT, MOUNT):
        if not os.access(p, os.X_OK):
            ctx.skip("%s not found" % p)


def build_spy(ctx):
    out = os.path.join(ctx.work, "syncspy.dylib")
    ctx.sh("cc -Wall -Wextra -Werror -dynamiclib -arch arm64 -arch arm64e -o %s %s" %
           (out, os.path.join(HERE, "syncspy.c")))
    return out


# ---------------------------------------------------------------------------------------------
# Devices: an NTFS image file, optionally attached as a disk (buffered and raw node)
# ---------------------------------------------------------------------------------------------
class Dev(object):
    """kind: file (the worker opens the image by path), buffered (/dev/diskN), raw (/dev/rdiskN)."""

    def __init__(self, ctx, kind, tag, size=IMG_SIZE, mkntfs=True):
        self.ctx = ctx
        self.kind = kind
        self.size = size
        self.img = os.path.join(ctx.work, "%s.img" % tag)
        self.node = None
        self.fd = None
        with open(self.img, "wb") as f:
            f.truncate(size)
        if mkntfs:
            ctx.sh("%s -F -Q -L sync %s" % (guest_tool("mkntfs"), self.img))
        if kind != "file":
            self.attach()

    def attach(self):
        r = self.ctx.sh("%s attach -nomount -imagekey diskimage-class=CRawDiskImage %s" % (HDIUTIL, self.img))
        first = r.out.decode("utf-8", "replace").split()
        base = first[0] if first else ""
        if not re.match(r"^/dev/disk[0-9]+$", base):
            raise RuntimeError("unexpected hdiutil attach output: %r" % r.out)
        self.base = base
        # The node must be the one for our image: whole disk, virtual, exactly the image size.
        info = subprocess.run([DISKUTIL, "info", "-plist", base], stdout=subprocess.PIPE, timeout=60)
        d = plistlib.loads(info.stdout)
        size = d.get("TotalSize") or d.get("Size")
        if size != self.size or not d.get("WholeDisk") or d.get("VirtualOrPhysical") != "Virtual":
            self.detach()
            raise RuntimeError("attached node %s does not match the image (size %r, whole %r, %r)" %
                               (base, size, d.get("WholeDisk"), d.get("VirtualOrPhysical")))
        self.node = base if self.kind == "buffered" else "/dev/r" + base[len("/dev/"):]
        self.ctx.log("attached %s as %s, using %s (%s)" % (os.path.basename(self.img), base, self.node, self.kind))
        self.ctx.add_cleanup(self.detach)

    def open_fd(self):
        if self.node is None:
            return None
        self.fd = os.open(self.node, os.O_RDWR)
        return self.fd

    def close_fd(self):
        if self.fd is not None:
            os.close(self.fd)
            self.fd = None

    def detach(self):
        self.close_fd()
        if getattr(self, "base", None):
            b = self.base
            self.base = None
            for _ in range(5):
                r = subprocess.run([HDIUTIL, "detach", b, "-force"], stdout=subprocess.DEVNULL,
                                   stderr=subprocess.DEVNULL, timeout=60)
                if r.returncode == 0:
                    break
                time.sleep(1)
            self.node = None


# ---------------------------------------------------------------------------------------------
# The worker: ntfs-3g in the foreground with a time-stamped copy of its output
# ---------------------------------------------------------------------------------------------
class Worker(object):
    def __init__(self, ctx, dev, tag, opts="", ro=False, debug=True, spy=None, fail_fullfsync=False):
        self.ctx = ctx
        self.dev = dev
        self.tag = tag
        self.mnt = os.path.realpath(os.path.join(ctx.work, "mnt-%s" % tag))
        self.lines = []  # (time, text)
        self.fuse = []  # (time, opcode, nodeid)
        self.spy_log = os.path.join(ctx.work, "spy-%s.log" % tag)
        self.proc = None
        self.mount_t0 = self.mount_t1 = None
        self.umount_t0 = self.umount_t1 = self.exit_t = None
        self.umount_rc = None
        self._reader = None
        self._watchdog = None
        o = "backend=fskit,local,norecover,uid=%d,gid=%d,no_detach" % (os.getuid(), os.getgid())
        if debug:
            o += ",debug"
        if ro:
            o += ",ro"
        if opts:
            o += "," + opts
        self.opts = o
        env = dict(os.environ)
        if spy:
            env["DYLD_INSERT_LIBRARIES"] = spy
            env["NTFSMAC_SYNCSPY_LOG"] = self.spy_log
            if fail_fullfsync:
                env["NTFSMAC_SYNCSPY_FAIL"] = "fullfsync"
        self.env = env

    def _read(self):
        for raw in self.proc.stdout:
            t = time.time()
            text = raw.decode("utf-8", "replace").rstrip("\n")
            self.lines.append((t, text))
            m = FUSE_RE.search(text)
            if m:
                self.fuse.append((t, m.group(1), int(m.group(3))))

    def _mounted(self):
        r = subprocess.run([MOUNT], stdout=subprocess.PIPE, timeout=30)
        return (" on %s (" % self.mnt) in r.stdout.decode("utf-8", "replace")

    def start(self, deadline=1200):
        os.makedirs(self.mnt, exist_ok=True)
        if os.path.exists(self.spy_log):
            os.unlink(self.spy_log)
        fd = self.dev.open_fd()
        target = "/dev/fd/%d" % fd if fd is not None else self.dev.img
        argv = [guest_tool("ntfs-3g"), "-o", self.opts, target, self.mnt]
        self.ctx.log("worker: %s" % " ".join(argv))
        self.mount_t0 = time.time()
        self.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, env=self.env,
                                     pass_fds=(fd,) if fd is not None else (), close_fds=True)
        self._reader = threading.Thread(target=self._read, daemon=True)
        self._reader.start()
        # Overall deadline for everything the worker does: SIGKILL, so no case can hang.
        self._watchdog = threading.Timer(deadline, self.kill)
        self._watchdog.daemon = True
        self._watchdog.start()
        self.ctx.add_cleanup(self.kill)
        end = time.time() + MOUNT_WAIT
        while time.time() < end:
            if self.proc.poll() is not None:
                break
            if self._mounted():
                self.mount_t1 = time.time()
                return
            time.sleep(0.2)
        self.kill()
        self._reader.join(5)
        tail = "\n".join(t for _t, t in self.lines[-8:])
        err = MountFailed("no mount on %s (%s); worker output tail:\n%s" %
                          (self.mnt, "worker exited rc=%s" % self.proc.returncode if self.proc.returncode is not None
                           else "no mount after %d s" % MOUNT_WAIT, tail))
        err.worker = self
        raise err

    def kill(self):
        if self.proc is not None and self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGKILL)
            except OSError:
                pass
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass

    def stop(self):
        """Unmount with /sbin/umount, wait for the worker to exit (deadline, then SIGKILL)."""
        self.umount_t0 = time.time()
        try:
            r = subprocess.run([UMOUNT, self.mnt], stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.umount_rc = r.returncode
            if r.returncode != 0:
                self.ctx.log("umount rc=%d: %s" % (r.returncode, r.stdout.decode("utf-8", "replace").strip()))
        except subprocess.TimeoutExpired:
            self.umount_rc = 124
            self.ctx.log("umount did not return in 60 s, killing the worker")
            self.kill()
        self.umount_t1 = time.time()
        try:
            self.proc.wait(MOUNT_WAIT)
        except subprocess.TimeoutExpired:
            self.ctx.log("worker still running %d s after umount, SIGKILL" % MOUNT_WAIT)
            self.kill()
        self.exit_t = time.time()
        if self._reader is not None:
            self._reader.join(5)
        if self._watchdog is not None:
            self._watchdog.cancel()
        self.dev.close_fd()
        if self.umount_rc != 0:
            raise RuntimeError("umount failed (rc=%s)" % self.umount_rc)

    # ---- logs ----
    def spy_records(self):
        """Records of the worker process from the interpose log: dicts with t, ev, fd, path, rc, errno, writes, bytes."""
        out = []
        try:
            with open(self.spy_log, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = SPY_RE.match(line.rstrip("\n"))
                    if not m or int(m.group(3)) != self.proc.pid:
                        continue
                    out.append({"t": float(m.group(1)), "ev": m.group(2), "fd": int(m.group(4)), "path": m.group(5),
                                "rc": int(m.group(6)) if m.group(6) is not None else None,
                                "errno": int(m.group(7)) if m.group(7) is not None else None,
                                "us": int(m.group(8)) if m.group(8) is not None else None,
                                "writes": int(m.group(9)) if m.group(9) is not None else None,
                                "bytes": int(m.group(10)) if m.group(10) is not None else None})
        except IOError:
            pass
        return out


class MountFailed(RuntimeError):
    pass


def point(w, name, t0, t1, ret, end=None):
    """Summarise one sync point: FUSE requests and device calls in [t0 - 20 ms, end or t1 + 100 ms]."""
    lo, hi = t0 - 0.02, (end if end is not None else t1 + 0.1)
    fsync = [f for f in w.fuse if lo <= f[0] <= hi and f[1] == "FSYNC"]
    destroy = [f for f in w.fuse if lo <= f[0] <= hi and f[1] == "DESTROY"]
    spy = [r for r in w.spy_records() if lo <= r["t"] <= hi and r["ev"] in ("fullfsync", "fsync")]
    full = [r for r in spy if r["ev"] == "fullfsync"]
    plain = [r for r in spy if r["ev"] == "fsync"]
    first = full[0] if full else None
    fallback = bool(first and first["rc"] != 0 and plain)
    return {
        "point": name,
        "app_ret": ret,
        "app_ms": round((t1 - t0) * 1000, 1),
        "fuse_fsync": len(fsync),
        "fuse_nodeids": sorted(set(f[2] for f in fsync)),
        "fuse_destroy": len(destroy),
        "device_fullfsync": len(full),
        "fullfsync_rc": first["rc"] if first else None,
        "fullfsync_errno": first["errno"] if first else None,
        "fullfsync_ms": round(first["us"] / 1000.0, 2) if first else None,
        "dirty_writes_before": first["writes"] if first else None,
        "dirty_bytes_before": first["bytes"] if first else None,
        "device_fsync": len(plain),
        "fallback_ran": fallback,
        "device_node": first["path"] if first else (plain[0]["path"] if plain else None),
    }


def pattern(n, salt):
    """Deterministic data, not compressible away: sha256 chain."""
    out = bytearray()
    h = hashlib.sha256(("sync-%d" % salt).encode()).digest()
    while len(out) < n:
        h = hashlib.sha256(h).digest()
        out += h
    return bytes(out[:n])


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(MIB), b""):
            h.update(chunk)
    return h.hexdigest()


def fmt_table(rows, cols):
    widths = [max(len(c), max([len(str(r.get(c))) for r in rows] or [0])) for c in cols]
    lines = ["  ".join(c.ljust(w) for c, w in zip(cols, widths))]
    for r in rows:
        lines.append("  ".join(str(r.get(c)).ljust(w) for c, w in zip(cols, widths)))
    return "\n".join(lines)


def verify_by_path(ctx, dev, expected, tag):
    """Detach the node, mount the image file read-only by path and compare the files with the expected hashes."""
    dev.detach()
    w = Worker(ctx, dev, "verify-" + tag, ro=True, debug=False)
    w.start()
    try:
        bad = []
        for name, (size, digest) in sorted(expected.items()):
            p = os.path.join(w.mnt, name)
            try:
                if os.path.getsize(p) != size:
                    bad.append("%s: size %d, expected %d" % (name, os.path.getsize(p), size))
                elif sha256_file(p) != digest:
                    bad.append("%s: sha256 differs" % name)
            except OSError as e:
                bad.append("%s: %s" % (name, e))
    finally:
        w.stop()
    fix = subprocess.run([guest_tool("ntfsfix"), "-n", dev.img], stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                         timeout=120)
    ctx.log("ntfsfix -n after %s: rc=%d" % (tag, fix.returncode))
    return bad, fix.returncode, fix.stdout.decode("utf-8", "replace")


# ---------------------------------------------------------------------------------------------
# I-SYNC-01
# ---------------------------------------------------------------------------------------------
def sync_points(ctx, dev, tag, spy, fail_fullfsync=False):
    """Mount, exercise every sync point the worker can get, unmount. Returns (points, expected hashes, worker)."""
    w = Worker(ctx, dev, tag, spy=spy, fail_fullfsync=fail_fullfsync)
    w.start()
    time.sleep(3.0)  # macOS fsyncs .fseventsd four times right after the mount; let that finish first
    pts = []
    expected = {}
    blk = pattern(4096, 1)
    name = "sp.dat"
    path = os.path.join(w.mnt, name)
    size = 0

    def step(label, fn):
        t0 = time.time()
        ret = fn()
        t1 = time.time()
        time.sleep(1.2)
        pts.append(point(w, label, t0, t1, ret))

    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o644)
    try:
        def wr():
            nonlocal size
            os.pwrite(fd, blk, size)
            size += 4096

        wr()
        step("fsync after write", lambda: os.fsync(fd))
        step("fsync, nothing new written", lambda: os.fsync(fd))
        wr()
        step("F_FULLFSYNC after write", lambda: fcntl.fcntl(fd, fcntl.F_FULLFSYNC))
        step("F_FULLFSYNC, nothing new written", lambda: fcntl.fcntl(fd, fcntl.F_FULLFSYNC))
        wr()
        step("sync(2), file open and dirty", lambda: os.sync())
        step("sync(2), file open, nothing new", lambda: os.sync())
        wr()
    finally:
        os.close(fd)
    # Written and closed, nothing open: sync(2) must not reach the worker (M2-sync); the device stays dirty.
    step("sync(2), written and closed, no file open", lambda: os.sync())
    expected[name] = (size, hashlib.sha256(blk * (size // 4096)).hexdigest())
    w.stop()
    end = (w.exit_t or time.time()) + 0.2
    pts.append(point(w, "unmount (FUSE_DESTROY)", w.umount_t0, w.umount_t1, w.umount_rc, end=end))
    return pts, expected, w


def trace_report(title, pts):
    cols = ["point", "fuse_fsync", "fuse_destroy", "device_fullfsync", "fullfsync_rc", "fullfsync_errno",
            "fullfsync_ms", "dirty_writes_before", "device_fsync", "fallback_ran", "app_ret", "app_ms"]
    return "== %s\n%s\n" % (title, fmt_table(pts, cols))


def run_trace(ctx, kind, tag, spy, fail=False):
    """One device kind through all sync points; returns (points, problems). A node that cannot be mounted
    is a result, not an error: the points are empty and the reason is returned."""
    dev = Dev(ctx, kind, tag)
    try:
        pts, expected, w = sync_points(ctx, dev, tag, spy, fail_fullfsync=fail)
    except MountFailed as e:
        ctx.log("%s: mount failed: %s" % (tag, e))
        w = e.worker
        with open(ctx.evidence_file("%s.mountfail.log" % tag), "w", encoding="utf-8") as f:
            f.write("worker output:\n")
            f.write("\n".join(t for _t, t in w.lines) + "\n\nfailed device calls (syncspy):\n")
            for r in w.spy_records():
                if r["ev"].endswith("-fail"):
                    f.write("%s %s %s errno=%s\n" % (r["t"], r["ev"], r["path"], r["errno"]))
        return None, str(e), None
    bad, fixrc, fixout = verify_by_path(ctx, dev, expected, tag)
    ctx.log("%s: data check by path: %s" % (tag, "identical" if not bad else bad))
    with open(ctx.evidence_file("%s.spy.log" % tag), "w", encoding="utf-8") as f:
        f.write(open(w.spy_log, encoding="utf-8", errors="replace").read() if os.path.exists(w.spy_log) else "")
    with open(ctx.evidence_file("%s.fuse.log" % tag), "w", encoding="utf-8") as f:
        t0 = w.fuse[0][0] if w.fuse else 0
        for t, op, node in w.fuse:
            if op in ("FSYNC", "FSYNCDIR", "DESTROY", "FLUSH", "RELEASE", "INIT"):
                f.write("%.6f %s nodeid=%d\n" % (t, op, node))
    return pts, bad, (fixrc, fixout)


@case("I-SYNC-01", envs=("B",), title="device sync trace: F_FULLFSYNC and fallback at each sync point (ADR-014)")
def i_sync_01(ctx):
    need_guest(ctx)
    spy = build_spy(ctx)
    result = {"kinds": {}}
    problems = []
    for kind, tag in (("file", "file"), ("buffered", "buf"), ("raw", "raw")):
        pts, bad, fix = run_trace(ctx, kind, tag, spy)
        if pts is None:
            result["kinds"][kind] = {"mounted": False, "reason": bad}
            continue
        result["kinds"][kind] = {"mounted": True, "points": pts, "data_mismatch": bad, "ntfsfix_rc": fix[0]}
        ctx.log(trace_report("device kind: %s" % kind, pts))
        with open(ctx.evidence_file("trace-%s.txt" % kind), "w", encoding="utf-8") as f:
            f.write(trace_report("device kind: %s" % kind, pts))
        if bad:
            problems.append("%s: data differs after remount by path: %s" % (kind, bad[0]))
        by = dict((p["point"], p) for p in pts)

        def need(label, **want):
            p = by[label]
            for k, v in want.items():
                got = p[k]
                ok = (got >= v[1]) if isinstance(v, tuple) else (got == v)
                if not ok:
                    problems.append("%s / %s: %s = %r, expected %r" % (kind, label, k, got, v))

        # The request side is the same on every device kind (M2-sync, re-checked here).
        for label in ("fsync after write", "fsync, nothing new written", "F_FULLFSYNC after write",
                      "F_FULLFSYNC, nothing new written", "sync(2), file open and dirty",
                      "sync(2), file open, nothing new"):
            need(label, fuse_fsync=("min", 1))
        need("sync(2), written and closed, no file open", fuse_fsync=0, device_fullfsync=0, device_fsync=0)
        need("unmount (FUSE_DESTROY)", fuse_destroy=1, fuse_fsync=0)
        # The device side: exactly one F_FULLFSYNC for a dirty device, none for a clean one (device.c
        # ntfs_device_sync() skips a clean device), and one unconditional sync at unmount (volume.c).
        for label in ("fsync after write", "F_FULLFSYNC after write", "sync(2), file open and dirty"):
            need(label, device_fullfsync=1, dirty_writes_before=("min", 1))
        for label in ("fsync, nothing new written", "F_FULLFSYNC, nothing new written",
                      "sync(2), file open, nothing new"):
            need(label, device_fullfsync=0, device_fsync=0)
        need("unmount (FUSE_DESTROY)", device_fullfsync=1)
        # The fallback flag must agree with the result of F_FULLFSYNC.
        for p in pts:
            if p["device_fullfsync"] and p["fallback_ran"] != (p["fullfsync_rc"] != 0):
                problems.append("%s / %s: fallback_ran=%r but F_FULLFSYNC rc=%r" %
                                (kind, p["point"], p["fallback_ran"], p["fullfsync_rc"]))
            if p["app_ret"] not in (None, 0) and p["point"] != "unmount (FUSE_DESTROY)":
                problems.append("%s / %s: the application call returned %r" % (kind, p["point"], p["app_ret"]))

    # Injected F_FULLFSYNC failure on the image file: the fallback must run, and the application must not see it.
    pts, bad, fix = run_trace(ctx, "file", "inject", spy, fail=True)
    if pts is None:
        problems.append("inject: mount failed: %s" % bad)
    else:
        result["kinds"]["file-fullfsync-fails"] = {"mounted": True, "points": pts, "data_mismatch": bad,
                                                   "ntfsfix_rc": fix[0]}
        with open(ctx.evidence_file("trace-file-fullfsync-fails.txt"), "w", encoding="utf-8") as f:
            f.write(trace_report("device kind: file, F_FULLFSYNC made to fail with ENOTTY", pts))
        ctx.log(trace_report("file, F_FULLFSYNC fails", pts))
        for p in pts:
            if p["device_fullfsync"]:
                if p["fullfsync_rc"] != -1 or p["device_fsync"] != 1 or not p["fallback_ran"]:
                    problems.append("inject / %s: expected F_FULLFSYNC failure then one fsync, got %r" % (p["point"], p))
            if p["point"] != "unmount (FUSE_DESTROY)" and p["app_ret"] not in (None, 0):
                problems.append("inject / %s: the application saw an error (%r); the fallback is meant to be silent"
                                % (p["point"], p["app_ret"]))
        if bad:
            problems.append("inject: data differs after remount by path: %s" % bad[0])
        by = dict((p["point"], p) for p in pts)
        if by["unmount (FUSE_DESTROY)"]["device_fullfsync"] != 1 or not by["unmount (FUSE_DESTROY)"]["fallback_ran"]:
            problems.append("inject: the unmount did not show the fallback")

    # Read-only mount: nothing is synced at unmount (unix_io.c ntfs_device_unix_io_sync() returns at NDevReadOnly).
    dev = Dev(ctx, "file", "ro")
    w = Worker(ctx, dev, "ro", ro=True, spy=spy)
    w.start()
    time.sleep(3.0)
    w.stop()
    ro_pts = point(w, "unmount, read-only mount", w.umount_t0, w.umount_t1, w.umount_rc, end=(w.exit_t or 0) + 0.2)
    result["kinds"]["file-read-only"] = {"mounted": True, "points": [ro_pts]}
    ctx.log("read-only unmount: %r" % ro_pts)
    if ro_pts["device_fullfsync"] or ro_pts["device_fsync"]:
        problems.append("read-only unmount issued a device sync: %r" % ro_pts)

    with open(ctx.evidence_file("trace.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    if problems:
        for p in problems:
            ctx.log("PROBLEM " + p)
        raise Fail("%d problem(s), first: %s" % (len(problems), problems[0]))


# ---------------------------------------------------------------------------------------------
# I-SYNC-02
# ---------------------------------------------------------------------------------------------
# DKIOCSYNCHRONIZE = _IOW('d', 22, dk_synchronize_t), dk_synchronize_t = {u64 offset, u64 length, u32 options} (24 bytes)
DKIOCSYNCHRONIZE = 0x80000000 | (24 << 16) | (ord("d") << 8) | 22
ALIGN_OFFSETS = (0, 1, 512, 513, 4096)
ALIGN_LENGTHS = (1, 100, 512, 513, 4096, 4140)


def probe_alignment(ctx):
    """pread and pwrite matrix on the bare nodes of a scratch image (no NTFS involved)."""
    res = {}
    for kind in ("buffered", "raw"):
        dev = Dev(ctx, kind, "align-" + kind, size=8 * MIB, mkntfs=False)
        fd = dev.open_fd()
        rows = {}
        try:
            for op in ("pread", "pwrite"):
                for off in ALIGN_OFFSETS:
                    for ln in ALIGN_LENGTHS:
                        try:
                            if op == "pread":
                                n = len(os.pread(fd, ln, off))
                            else:
                                n = os.pwrite(fd, b"\0" * ln, off)
                            rows["%s off=%d len=%d" % (op, off, ln)] = "ok %d" % n
                        except OSError as e:
                            rows["%s off=%d len=%d" % (op, off, ln)] = "errno %d" % e.errno
            try:
                r = fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
                rows["F_FULLFSYNC"] = "ok %d" % r
            except OSError as e:
                rows["F_FULLFSYNC"] = "errno %d" % e.errno
            try:
                os.fsync(fd)
                rows["fsync"] = "ok"
            except OSError as e:
                rows["fsync"] = "errno %d" % e.errno
            try:
                fcntl.ioctl(fd, DKIOCSYNCHRONIZE, struct.pack("QQI4x", 0, 0, 0))
                rows["DKIOCSYNCHRONIZE"] = "ok"
            except OSError as e:
                rows["DKIOCSYNCHRONIZE"] = "errno %d" % e.errno
        finally:
            dev.detach()
        res[kind] = rows
    return res


def node_io(ctx):
    """Aligned sequential I/O on the bare nodes of a scratch image: 32 x 1 MiB and 2048 x 4 KiB writes, a
    DKIOCSYNCHRONIZE after each, then 32 MiB read back. MB/s."""
    res = {}
    for kind in ("buffered", "raw"):
        dev = Dev(ctx, kind, "io-" + kind, size=64 * MIB, mkntfs=False)
        fd = dev.open_fd()
        chunk = pattern(MIB, 7)
        out = {}
        try:
            t0 = time.time()
            for i in range(32):
                os.pwrite(fd, chunk, i * MIB)
            t1 = time.time()
            fcntl.ioctl(fd, DKIOCSYNCHRONIZE, struct.pack("QQI4x", 0, 0, 0))
            t2 = time.time()
            out["write_1MiB_MBps"] = round(32 / (t1 - t0), 1)
            out["write_then_flush_MBps"] = round(32 / (t2 - t0), 1)
            out["flush_ms"] = round((t2 - t1) * 1000, 1)
            blk = chunk[:4096]
            t0 = time.time()
            for i in range(2048):
                os.pwrite(fd, blk, 32 * MIB + i * 4096)
            t1 = time.time()
            fcntl.ioctl(fd, DKIOCSYNCHRONIZE, struct.pack("QQI4x", 0, 0, 0))
            t2 = time.time()
            out["write_4KiB_x2048_MBps"] = round(8 / (t1 - t0), 1)
            out["write_4KiB_then_flush_MBps"] = round(8 / (t2 - t0), 1)
            t0 = time.time()
            for i in range(32):
                os.pread(fd, MIB, i * MIB)
            out["read_1MiB_MBps"] = round(32 / (time.time() - t0), 1)
        finally:
            dev.detach()
        res[kind] = out
    return res


FILE_SIZES = (1, 100, 511, 512, 513, 4095, 4096, 4097, 100000, 1 * MIB, 1 * MIB + 1, 5 * MIB + 777)


def correctness(ctx, kind, tag, spy):
    """Mount NTFS through the node, write files of aligned and unaligned sizes, an append and a truncate;
    record every errno; unmount; compare by path. Returns a dict."""
    out = {"kind": kind}
    dev = Dev(ctx, kind, tag)
    w = Worker(ctx, dev, tag, debug=False, spy=spy)
    try:
        w.start()
    except MountFailed as e:
        out["mounted"] = False
        out["reason"] = str(e)
        out["failed_device_calls"] = [r["ev"] + " " + r["path"] + " errno=" + str(r["errno"])
                                      for r in w.spy_records() if r["ev"].endswith("-fail")][:10]
        return out
    out["mounted"] = True
    errors = {}
    expected = {}
    try:
        for i, n in enumerate(FILE_SIZES):
            name = "f%02d_%d" % (i, n)
            data = pattern(n, i)
            try:
                fd = os.open(os.path.join(w.mnt, name), os.O_CREAT | os.O_WRONLY, 0o644)
                try:
                    os.write(fd, data)
                    os.fsync(fd)
                finally:
                    os.close(fd)
                expected[name] = (n, hashlib.sha256(data).hexdigest())
            except OSError as e:
                errors["write %s" % name] = e.errno
        # An append of 7 bytes to the 4096-byte file and a truncate that cuts a cluster in half.
        for name, op in (("f06_4096", "append7"), ("f08_100000", "trunc50000")):
            if name not in expected:
                continue
            p = os.path.join(w.mnt, name)
            try:
                data = pattern(expected[name][0], 6 if op == "append7" else 8)
                if op == "append7":
                    fd = os.open(p, os.O_WRONLY | os.O_APPEND)
                    try:
                        os.write(fd, b"APPEND7")
                        os.fsync(fd)
                    finally:
                        os.close(fd)
                    data = data + b"APPEND7"
                else:
                    os.truncate(p, 50000)
                    data = data[:50000]
                expected[name] = (len(data), hashlib.sha256(data).hexdigest())
            except OSError as e:
                errors["%s %s" % (op, name)] = e.errno
                expected.pop(name, None)  # state after a failed update is unknown: do not claim it
    finally:
        w.stop()
    out["op_errors"] = errors
    out["files_expected_ok"] = len(expected)
    bad, fixrc, fixout = verify_by_path(ctx, dev, expected, tag)
    out["mismatch_after_remount"] = bad
    out["ntfsfix_rc"] = fixrc
    out["ntfsfix_tail"] = fixout.strip().splitlines()[-3:]
    pcs = [t for _t, t in w.lines if "ailed" in t or "rror" in t]
    out["worker_errors"] = pcs[:12]
    out["failed_device_calls"] = [r["ev"] + " " + r["path"] + " errno=" + str(r["errno"])
                                  for r in w.spy_records() if r["ev"].endswith("-fail")][:10]
    return out


def median(xs):
    s = sorted(xs)
    return s[len(s) // 2] if s else None


def bench_one(ctx, label, kind, opts, tag):
    """The small fixed workloads on one variant. Sizes are multiples of 4096 (a raw node cannot take others)."""
    res = {"variant": label, "kind": kind, "opts": opts}
    dev = Dev(ctx, kind, tag)
    w = Worker(ctx, dev, tag, opts=opts, debug=False)
    try:
        w.start()
    except MountFailed as e:
        res["mounted"] = False
        res["reason"] = str(e).splitlines()[0]
        return res
    res["mounted"] = True
    chunk = pattern(MIB, 99)
    seq, small, fsyncs, errs = [], [], [], []
    try:
        for rep in range(3):
            try:
                t0 = time.time()
                p = os.path.join(w.mnt, "seq%d.bin" % rep)
                fd = os.open(p, os.O_CREAT | os.O_WRONLY, 0o644)
                for _ in range(32):
                    os.write(fd, chunk)
                os.fsync(fd)
                os.close(fd)
                seq.append(32.0 / (time.time() - t0))
            except OSError as e:
                errs.append("seq_write: errno %d" % e.errno)
                break
        for rep in range(3):
            try:
                d = os.path.join(w.mnt, "small%d" % rep)
                os.mkdir(d)
                blk = chunk[:4096]
                t0 = time.time()
                for i in range(100):
                    fd = os.open(os.path.join(d, "f%03d" % i), os.O_CREAT | os.O_WRONLY, 0o644)
                    os.write(fd, blk)
                    os.close(fd)
                small.append(100.0 / (time.time() - t0))
            except OSError as e:
                errs.append("small_files: errno %d" % e.errno)
                break
        try:
            fd = os.open(os.path.join(w.mnt, "fs.bin"), os.O_CREAT | os.O_WRONLY, 0o644)
            for i in range(30):
                os.pwrite(fd, chunk[:4096], i * 4096)
                t0 = time.time()
                os.fsync(fd)
                fsyncs.append((time.time() - t0) * 1000.0)
            os.close(fd)
        except OSError as e:
            errs.append("fsync_loop: errno %d" % e.errno)
    finally:
        w.stop()
    res["seq_write_MBps"] = [round(x, 1) for x in seq]
    res["small_files_per_s"] = [round(x) for x in small]
    res["fsync_ms"] = [round(x, 1) for x in fsyncs]
    res["unmount_ms"] = round((w.umount_t1 - w.umount_t0) * 1000, 0)
    res["errors"] = errs
    dev.detach()
    return res


def sync_count(ctx, spy, opts, tag):
    """Device syncs the worker issues for one 4 MiB write (1 MiB writes) and an fsync, with and without `sync`."""
    dev = Dev(ctx, "file", tag)
    w = Worker(ctx, dev, tag, opts=opts, debug=True, spy=spy)
    w.start()
    time.sleep(3.0)
    t0 = time.time()
    fd = os.open(os.path.join(w.mnt, "c.bin"), os.O_CREAT | os.O_WRONLY, 0o644)
    for _ in range(4):
        os.write(fd, pattern(MIB, 5))
    t1 = time.time()
    os.fsync(fd)
    os.close(fd)
    t2 = time.time()
    w.stop()
    rows = [r for r in w.spy_records() if r["ev"] in ("fullfsync", "fsync")]
    during = [r for r in rows if t0 <= r["t"] <= t1]
    at_fsync = [r for r in rows if t1 < r["t"] <= t2]
    return {"opts": opts or "(default)", "syncs_during_4_MiB_write": len(during),
            "syncs_at_fsync_and_close": len(at_fsync),
            "fuse_fsync_requests": len([f for f in w.fuse if t1 - 0.02 <= f[0] <= t2 and f[1] == "FSYNC"]),
            "mean_fullfsync_ms": round(sum(r["us"] for r in rows if r["ev"] == "fullfsync") /
                                       max(1, len([r for r in rows if r["ev"] == "fullfsync"])) / 1000.0, 2)}


def bench_report(rows):
    lines = ["%-28s %-9s %-22s %-22s %-24s %s" % ("variant", "mounted", "seq write 32 MiB MB/s", "100 x 4 KiB files/s",
                                                   "fsync 4 KiB ms (med/max)", "unmount ms")]
    for r in rows:
        if not r.get("mounted"):
            lines.append("%-28s %-9s %s" % (r["variant"], "no", r.get("reason")))
            continue
        fm = r["fsync_ms"]
        lines.append("%-28s %-9s %-22s %-22s %-24s %s%s" % (
            r["variant"], "yes",
            "%s (%s)" % (median(r["seq_write_MBps"]), "/".join(str(x) for x in r["seq_write_MBps"])),
            "%s (%s)" % (median(r["small_files_per_s"]), "/".join(str(x) for x in r["small_files_per_s"])),
            ("%s/%s" % (median(fm), max(fm))) if fm else "-", r["unmount_ms"],
            ("  ERRORS: " + "; ".join(r["errors"])) if r["errors"] else ""))
    return "\n".join(lines)


@case("I-SYNC-02", envs=("B",), title="raw versus buffered device node (U10): alignment, correctness, benchmark")
def i_sync_02(ctx):
    need_guest(ctx)
    spy = build_spy(ctx)
    result = {}
    problems = []

    # A. the kernel at the node
    align = probe_alignment(ctx)
    result["alignment"] = align
    with open(ctx.evidence_file("alignment.txt"), "w", encoding="utf-8") as f:
        keys = sorted(align["raw"].keys())
        f.write("%-28s %-12s %-12s\n" % ("call", "buffered", "raw"))
        for k in keys:
            f.write("%-28s %-12s %-12s\n" % (k, align["buffered"].get(k), align["raw"].get(k)))
    ctx.log(open(ctx.evidence_file("alignment.txt")).read())
    for k, v in align["buffered"].items():
        if k.startswith("p") and not v.startswith("ok"):
            problems.append("buffered node refused %s: %s" % (k, v))
    for kind in ("buffered", "raw"):
        if align[kind]["DKIOCSYNCHRONIZE"] != "ok":
            problems.append("%s node: DKIOCSYNCHRONIZE gave %s" % (kind, align[kind]["DKIOCSYNCHRONIZE"]))
    # Raw: unaligned offset or length must fail with EINVAL (22), aligned ones must work. If this changes the
    # premise of the findings has changed and the case says so.
    for k, v in align["raw"].items():
        m = re.match(r"p(?:read|write) off=(\d+) len=(\d+)", k)
        if not m:
            continue
        aligned = int(m.group(1)) % 512 == 0 and int(m.group(2)) % 512 == 0
        if aligned != v.startswith("ok"):
            problems.append("raw node: %s gave %s, expected %s" % (k, v, "ok" if aligned else "errno 22"))

    result["node_io"] = node_io(ctx)
    ctx.log("node I/O (MB/s): %s" % json.dumps(result["node_io"], sort_keys=True))
    with open(ctx.evidence_file("node-io.txt"), "w", encoding="utf-8") as f:
        for kind in ("buffered", "raw"):
            f.write("%-9s %s\n" % (kind, json.dumps(result["node_io"][kind], sort_keys=True)))

    # B. NTFS-3G through the nodes: no silent corruption, whatever else happens
    corr = {}
    for kind, tag in (("file", "cfile"), ("buffered", "cbuf"), ("raw", "craw")):
        corr[kind] = correctness(ctx, kind, tag, spy)
        c = corr[kind]
        ctx.log("correctness %s: %s" % (kind, json.dumps(c, sort_keys=True)))
        if c.get("mounted"):
            if c["mismatch_after_remount"]:
                problems.append("%s: a write reported success but the data is wrong: %s" %
                                (kind, c["mismatch_after_remount"][0]))
            if kind in ("file", "buffered") and c["op_errors"]:
                problems.append("%s: operations failed: %r" % (kind, c["op_errors"]))
            if kind in ("file", "buffered") and c["ntfsfix_rc"] != 0:
                problems.append("%s: ntfsfix -n found a problem after the run (rc=%d)" % (kind, c["ntfsfix_rc"]))
    result["correctness"] = corr

    # C. benchmark
    variants = (
        ("default (image file)", "file", "", "bfile"),
        ("sync option (image file)", "file", "sync", "bsync"),
        ("buffered node /dev/diskN", "buffered", "", "bbuf"),
        ("sync option, buffered node", "buffered", "sync", "bbufsync"),
        ("raw node /dev/rdiskN", "raw", "", "braw"),
    )
    bench = []
    for label, kind, opts, tag in variants:
        r = bench_one(ctx, label, kind, opts, tag)
        bench.append(r)
        ctx.log("bench %s: %s" % (label, json.dumps(r, sort_keys=True)))
    result["bench"] = bench
    counts = [sync_count(ctx, spy, "", "cdef"), sync_count(ctx, spy, "sync", "csync")]
    result["sync_counts"] = counts
    ctx.log("sync counts: %s" % json.dumps(counts))
    text = bench_report(bench) + "\n\ndevice syncs for 4 x 1 MiB write + fsync + close (image file):\n" + \
        "\n".join(json.dumps(c, sort_keys=True) for c in counts)
    ctx.log("\n" + text)
    with open(ctx.evidence_file("bench.txt"), "w", encoding="utf-8") as f:
        f.write(text + "\n")
    for r in bench:
        if r["variant"].startswith(("default", "buffered")) and (not r.get("mounted") or r.get("errors")):
            problems.append("benchmark variant %s did not run clean: %r" % (r["variant"], r.get("errors") or r.get("reason")))

    with open(ctx.evidence_file("u10.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1, sort_keys=True)
    if problems:
        for p in problems:
            ctx.log("PROBLEM " + p)
        raise Fail("%d problem(s), first: %s" % (len(problems), problems[0]))
