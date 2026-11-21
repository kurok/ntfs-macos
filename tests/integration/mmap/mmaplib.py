# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers of the mmap suite (I-MMAP-*, T-M3-11).

Not a case module (no case declarations, so the harness does not import it as one); i_mmap.py puts
this directory on sys.path and imports it as `mmaplib`. Python 3.9, standard library only.

  Worker   ntfs-3g in the foreground (backend=fskit, norecover, debug, no_detach) on an image file, with a
           time-stamped copy of its output (FUSE requests with size and offset), an optional interpose
           library (tests/integration/sync/syncspy.c, built the same way as I-SYNC-01) that logs the
           worker's device writes and syncs, umount, forced unmount, SIGKILL of the worker.
  Tool     the C helper mmaptool.c: one descriptor, one MAP_SHARED mapping, commands on stdin.
  verify   unmount-independent check of an image: ntfsfix -n, read-only mount, hash of named files.

Every mount wait, command wait and umount has a deadline; the worker has an overall watchdog that
sends SIGKILL. Mount only on images the case made in the guest (ctx.volume).
"""

import hashlib
import os
import queue
import re
import shlex
import signal
import subprocess
import threading
import time

from harness.volume import guest_tool

HERE = os.path.dirname(os.path.abspath(__file__))
SYNC_DIR = os.path.join(os.path.dirname(HERE), "sync")
UMOUNT = "/sbin/umount"
MOUNT = "/sbin/mount"
DISKUTIL = "/usr/sbin/diskutil"
MOUNT_WAIT = 30
UNMOUNT_WAIT = 30  # deadline of the wait for the mount table to be clear before a mount
SETTLE = 1.0  # fixed pause after the table is clear; FSKit needs a moment after a clean unmount
REFUSED = "Failed to initialize volume"  # macFUSE FSKit refusing the volume (MFMount)
PAGE = os.sysconf("SC_PAGESIZE")

FUSE_RE = re.compile(r"opcode: ([A-Z_]+) \((\d+)\), nodeid: (\d+)")
WRITE_RE = re.compile(r"^write\[\d+\] (\d+) bytes to (\d+)")
READ_RE = re.compile(r"^read\[\d+\] (\d+) bytes from (\d+)")
SPY_RE = re.compile(r"^(\d+\.\d+) (\S+) pid=(\d+) fd=(-?\d+) path=(.*?)(?: rc=(-?\d+) errno=(\d+))?"
                    r"(?: us=(\d+))?(?: writes=(\d+) bytes=(\d+))?$")
# Requests macOS adds around most calls; they are not part of the answer.
NOISE = ("STATFS", "GETATTR", "GETXATTR", "LISTXATTR", "LOOKUP", "FORGET", "ACCESS", "SETXATTR", "REMOVEXATTR")


# Devices (first field of the mount table line) seen mounted on a mount point; a new mount waits for them to go too.
_SEEN_DEVICES = {}


def mount_table():
    """Lines of /sbin/mount."""
    r = subprocess.run([MOUNT], stdout=subprocess.PIPE, timeout=30)
    return r.stdout.decode("utf-8", "replace").splitlines()


class WaitTimeout(RuntimeError):
    """The mount table still listed the mount point (or a device of an earlier worker) at the deadline."""


class MountFailed(RuntimeError):
    """The worker did not mount. refused: its output has REFUSED; first_line: the first such line; was_alive: the
    worker was still running when it was killed (it hung instead of exiting); rc: its exit status."""

    def __init__(self, msg, refused=False, first_line="", was_alive=False, rc=None):
        RuntimeError.__init__(self, msg)
        self.refused, self.first_line, self.was_alive, self.rc = refused, first_line, was_alive, rc


def sha(data):
    return hashlib.sha256(data).hexdigest()


def need_guest(ctx):
    from harness.volume import detect_macfuse

    if ctx.env != "B":
        ctx.skip("mapped I/O on the FSKit backend is macOS only; Environment B")
    if detect_macfuse(ctx.transport) is None:
        ctx.skip("macFUSE is not installed in the guest")
    for tool in ("ntfs-3g", "mkntfs", "ntfsfix"):
        if guest_tool(tool) is None:
            ctx.skip("%s not found in the guest (build it with scripts/build-ntfs3g.sh --fuse external)" % tool)
    for p in (UMOUNT, MOUNT, DISKUTIL):
        if not os.access(p, os.X_OK):
            ctx.skip("%s not found" % p)


def build(ctx):
    """Build the helper and the interpose library in the case's work directory; return (tool, spy)."""
    tool = os.path.join(ctx.work, "mmaptool")
    ctx.sh("cc -Wall -Wextra -Werror -o %s %s" % (shlex.quote(tool), shlex.quote(os.path.join(HERE, "mmaptool.c"))))
    spy = os.path.join(ctx.work, "syncspy.dylib")
    ctx.sh("cc -Wall -Wextra -Werror -dynamiclib -arch arm64 -arch arm64e -o %s %s" %
           (shlex.quote(spy), shlex.quote(os.path.join(SYNC_DIR, "syncspy.c"))))
    return tool, spy


# ---------------------------------------------------------------------------------------------
class Worker(object):
    def __init__(self, ctx, vol, tag, spy=None, ro=False, opts=""):
        self.ctx = ctx
        self.vol = vol
        self.tag = tag
        self.mnt = os.path.realpath(vol.mnt)
        self.lines = []
        self.fuse = []  # dicts: t, op, node, size, off
        self.refusals = []  # one dict per FSKit refusal that was retried
        self.wait_s = None  # seconds the last wait before a mount took
        self.spy_log = os.path.join(ctx.work, "spy-%s.log" % tag)
        self.proc = None
        self.umount_rc = None
        self.umount_out = ""
        self._reader = None
        self._watchdog = None
        self._cleanup_added = False
        o = "backend=fskit,local,norecover,uid=%d,gid=%d,no_detach,debug" % (os.getuid(), os.getgid())
        if ro:
            o += ",ro"
        if opts:
            o += "," + opts
        self.opts = o
        env = dict(os.environ)
        if spy:
            env["DYLD_INSERT_LIBRARIES"] = spy
            env["NTFSMAC_SYNCSPY_LOG"] = self.spy_log
            env["NTFSMAC_SYNCSPY_WRITES"] = "1"
        self.env = env

    def _read(self, proc, lines, fuse):
        last = None
        for raw in proc.stdout:
            t = time.time()
            text = raw.decode("utf-8", "replace").rstrip("\n")
            lines.append((t, text))
            m = FUSE_RE.search(text)
            if m:
                last = {"t": t, "op": m.group(1), "node": int(m.group(3)), "size": None, "off": None}
                fuse.append(last)
                continue
            if last is not None and last["size"] is None:
                m = WRITE_RE.match(text) if last["op"] == "WRITE" else READ_RE.match(text) if last["op"] == "READ" else None
                if m:
                    last["size"], last["off"] = int(m.group(1)), int(m.group(2))

    def mounted(self):
        for line in mount_table():
            if (" on %s (" % self.mnt) in line:
                return line
        return None

    def _listed(self):
        """Mount table lines for this mount point or for a device that was mounted on it before."""
        devs = _SEEN_DEVICES.get(self.mnt, set())
        return [line for line in mount_table()
                if (" on %s (" % self.mnt) in line or line.split(" on ", 1)[0] in devs]

    def wait_clear(self, deadline=UNMOUNT_WAIT, poll=0.2, settle=SETTLE):
        """Wait until the mount table has no line for this mount point and none for the devices earlier workers
        had on it, then settle. The wait catches leftover or forced mounts; the settle and the single retry in
        start() handle the FSKit timing race after a clean unmount. Raises WaitTimeout at the deadline."""
        t0 = time.time()
        while True:
            left = self._listed()
            if not left:
                break
            if time.time() - t0 >= deadline:
                raise WaitTimeout("mount table still lists %s after %d s: %s" % (self.mnt, deadline, "; ".join(left)))
            time.sleep(poll)
        waited = time.time() - t0
        time.sleep(settle)
        self.wait_s = waited
        self.ctx.log("mount table clear for %s after %.2f s, settled %.1f s" % (self.mnt, waited, settle))

    def start(self, deadline=1500):
        """Wait for a clear mount table, mount. When macFUSE FSKit refuses the volume, make sure the worker is
        dead, keep its output as evidence and mount once more after the same wait; a second failure, and any
        other failure, raises."""
        os.makedirs(self.mnt, exist_ok=True)
        self.wait_clear()
        try:
            self._launch(deadline)
            return
        except MountFailed as e:
            if not e.refused:
                raise
            first = e
        self._ensure_dead()
        name = "worker-refused-%s.log" % self.tag
        with open(self.ctx.evidence_file(name), "a", encoding="utf-8", errors="replace") as f:
            for t, text in self.lines[-2000:]:
                f.write("%.3f %s\n" % (t, text))
        self.ctx.log("FSKit refused the volume (%s); worker %s when killed; retrying once" % (
            first.first_line, "still running" if first.was_alive else "had exited"))
        self.wait_clear()
        try:
            self._launch(deadline)
        except MountFailed as e:
            raise MountFailed("%s (second failure, after one retry; first: %s)" % (e, first.first_line),
                              e.refused, e.first_line, e.was_alive, e.rc)
        rec = {"error": first.first_line, "worker_alive_when_killed": first.was_alive, "rc": first.rc,
               "retry_wait_s": round(self.wait_s, 2), "evidence": name}
        self.refusals.append(rec)
        self.ctx.add_result_note("mount retried once after FSKit refusal (%s); worker %s when killed (rc %s); "
                                 "retry mounted" % (first.first_line, "still running" if first.was_alive
                                                    else "had already exited", first.rc))

    def _ensure_dead(self):
        self.kill()
        if self._reader is not None:
            self._reader.join(5)
        if self.proc is not None and self.proc.poll() is None:
            raise RuntimeError("worker pid %d is still running after SIGKILL" % self.proc.pid)

    def _launch(self, deadline):
        if os.path.exists(self.spy_log):
            os.unlink(self.spy_log)
        argv = [guest_tool("ntfs-3g"), "-o", self.opts, self.vol.img, self.mnt]
        self.ctx.log("worker: %s" % " ".join(shlex.quote(a) for a in argv))
        if self._watchdog is not None:
            self._watchdog.cancel()
        self.lines, self.fuse = [], []
        self.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                                     stderr=subprocess.STDOUT, env=self.env, close_fds=True)
        self._reader = threading.Thread(target=self._read, args=(self.proc, self.lines, self.fuse), daemon=True)
        self._reader.start()
        self._watchdog = threading.Timer(deadline, self.kill)
        self._watchdog.daemon = True
        self._watchdog.start()
        if not self._cleanup_added:
            self._cleanup_added = True
            self.ctx.add_cleanup(self.cleanup)
        end = time.time() + MOUNT_WAIT
        while time.time() < end:
            if self.proc.poll() is not None:
                break
            line = self.mounted()
            if line:
                self.ctx.log("mount: %s" % line)
                _SEEN_DEVICES.setdefault(self.mnt, set()).add(line.split(" on ", 1)[0])
                time.sleep(3.0)  # macOS opens and fsyncs .fseventsd right after the mount
                return
            time.sleep(0.2)
        was_alive = self.proc.poll() is None
        self.kill()
        self._reader.join(5)
        tail = "\n".join(t for _t, t in self.lines[-8:])
        first = next((t for _t, t in self.lines if REFUSED in t), "")
        raise MountFailed("no mount on %s (worker rc=%s); tail:\n%s" % (self.mnt, self.proc.returncode, tail),
                          bool(first), first, was_alive, self.proc.returncode)

    def alive(self):
        return self.proc is not None and self.proc.poll() is None

    def pid(self):
        return self.proc.pid

    def kill(self):
        """SIGKILL the worker, deadline on the wait."""
        if self.proc is not None and self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGKILL)
            except OSError:
                pass
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass

    def run(self, argv, timeout=60):
        """Run a command with a deadline; returns (rc, output). rc 124 on timeout."""
        try:
            r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
            return r.returncode, r.stdout.decode("utf-8", "replace").strip()
        except subprocess.TimeoutExpired:
            return 124, "timeout after %d s" % timeout

    def umount(self, force=False):
        """/sbin/umount (-f when force); returns (rc, output). Does not wait for the worker."""
        argv = [UMOUNT] + (["-f"] if force else []) + [self.mnt]
        rc, out = self.run(argv)
        self.ctx.log("%s -> rc=%d %s" % (" ".join(argv), rc, out))
        self.umount_rc, self.umount_out = rc, out
        return rc, out

    def diskutil_force(self):
        rc, out = self.run([DISKUTIL, "unmount", "force", self.mnt])
        self.ctx.log("diskutil unmount force -> rc=%d %s" % (rc, out))
        return rc, out

    def wait_exit(self, timeout=MOUNT_WAIT):
        try:
            self.proc.wait(timeout)
        except subprocess.TimeoutExpired:
            self.ctx.log("worker still running %d s after unmount, SIGKILL" % timeout)
            self.kill()
            return False
        finally:
            if self._reader is not None:
                self._reader.join(5)
        return True

    def stop(self):
        """Clean unmount with /sbin/umount and wait for the worker to exit (deadline, then SIGKILL)."""
        rc, out = self.umount()
        if rc != 0:
            raise RuntimeError("umount failed rc=%d: %s" % (rc, out))
        if not self.wait_exit():
            raise RuntimeError("worker did not exit after umount")
        if self._watchdog is not None:
            self._watchdog.cancel()

    def cleanup(self):
        """Always leave nothing mounted and no worker running (cleanup of the case)."""
        if self._watchdog is not None:
            self._watchdog.cancel()
        if self.mounted():
            rc, _o = self.run([UMOUNT, self.mnt], timeout=30)
            if rc != 0 and self.mounted():
                self.run([DISKUTIL, "unmount", "force", self.mnt], timeout=30)
            if self.mounted():
                self.run([UMOUNT, "-f", self.mnt], timeout=30)
        self.kill()

    # ---- logs ----
    def events(self, t0, t1, noise=False):
        return [e for e in self.fuse if t0 <= e["t"] <= t1 and (noise or e["op"] not in NOISE)]

    def spy_records(self, t0, t1):
        out = []
        try:
            with open(self.spy_log, "r", encoding="utf-8", errors="replace") as f:
                for line in f:
                    m = SPY_RE.match(line.rstrip("\n"))
                    if not m or int(m.group(3)) != self.proc.pid:
                        continue
                    t = float(m.group(1))
                    if t0 <= t <= t1:
                        out.append({"t": t, "ev": m.group(2), "path": m.group(5),
                                    "rc": int(m.group(6)) if m.group(6) is not None else None})
        except IOError:
            pass
        return out


def summarize(w, t0, t1, tmark=None):
    """Summary of the events of the worker from 20 ms before t0 (the start of the call) to t1. tmark: time of the call's return, to say
    whether a request arrived during the call or after it."""
    ev = w.events(t0 - 0.02, t1)
    parts = []
    for e in ev:
        rel = (e["t"] - t0) * 1000.0
        when = "" if tmark is None else ("<" if e["t"] <= tmark else ">")
        extra = "" if e["size"] is None else " %d@%d" % (e["size"], e["off"])
        parts.append("%s%s n%d%s +%.0fms" % (when, e["op"], e["node"], extra, rel))
    spy = [r for r in w.spy_records(t0 - 0.02, t1) if r["ev"] in ("fullfsync", "fsync", "pwrite", "write")]
    nw = len([r for r in spy if r["ev"] in ("pwrite", "write")])
    ns = len([r for r in spy if r["ev"] in ("fullfsync", "fsync")])
    return {"fuse": parts, "device_writes": nw, "device_syncs": ns,
            "write_requests": len([e for e in ev if e["op"] == "WRITE"]),
            "write_bytes": sum(e["size"] or 0 for e in ev if e["op"] == "WRITE"),
            "write_bytes_during": sum(e["size"] or 0 for e in ev if e["op"] == "WRITE" and tmark is not None
                                      and e["t"] <= tmark),
            "opcodes": sorted(set(e["op"] for e in ev))}


# ---------------------------------------------------------------------------------------------
class Tool(object):
    """The C helper. cmd() returns the reply line, or "TIMEOUT" after `timeout` seconds."""

    def __init__(self, ctx, exe):
        self.ctx = ctx
        self.q = queue.Queue()
        self.proc = subprocess.Popen([exe], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     close_fds=True)
        self._t = threading.Thread(target=self._read, daemon=True)
        self._t.start()
        ctx.add_cleanup(self.kill)
        first = self.q.get(timeout=20)
        self.pid = int(first.split("pid=")[1])
        self.dead = False

    def _read(self):
        for raw in self.proc.stdout:
            self.q.put(raw.decode("utf-8", "replace").rstrip("\n"))
        self.q.put("EOF")

    def cmd(self, line, timeout=40):
        if self.proc.poll() is not None and self.q.empty():
            return "EOF"
        try:
            self.proc.stdin.write((line + "\n").encode())
            self.proc.stdin.flush()
        except (BrokenPipeError, OSError):
            return "EOF"
        try:
            r = self.q.get(timeout=timeout)
        except queue.Empty:
            r = "TIMEOUT"
        self.ctx.log("tool> %s => %s" % (line, r))
        return r

    def kill(self):
        if self.proc.poll() is None:
            try:
                self.proc.send_signal(signal.SIGKILL)
            except OSError:
                pass
            try:
                self.proc.wait(10)
            except subprocess.TimeoutExpired:
                pass

    def quit(self):
        self.cmd("quit", timeout=10)
        try:
            self.proc.wait(10)
        except subprocess.TimeoutExpired:
            self.kill()

    # data helpers; files live in the case's work directory, outside the NTFS volume
    def mput(self, off, data, tag="in"):
        p = os.path.join(self.ctx.work, "tool-%s.bin" % tag)
        with open(p, "wb") as f:
            f.write(data)
        return self.cmd("mput %d %s" % (off, p))

    def mget(self, off, n, tag="out"):
        """Returns (reply, bytes or None)."""
        p = os.path.join(self.ctx.work, "tool-%s.bin" % tag)
        if os.path.exists(p):
            os.unlink(p)
        r = self.cmd("mget %d %d %s" % (off, n, p))
        if r.startswith("ok"):
            with open(p, "rb") as f:
                return r, f.read()
        return r, None

    def pwrite(self, off, data, tag="pw"):
        p = os.path.join(self.ctx.work, "tool-%s.bin" % tag)
        with open(p, "wb") as f:
            f.write(data)
        return self.cmd("pwrite %d %s" % (off, p))

    def pread(self, off, n, tag="pr"):
        p = os.path.join(self.ctx.work, "tool-%s.bin" % tag)
        if os.path.exists(p):
            os.unlink(p)
        r = self.cmd("pread %d %d %s" % (off, n, p))
        if r.startswith("ok"):
            with open(p, "rb") as f:
                return r, f.read()
        return r, None


# ---------------------------------------------------------------------------------------------
def slurp(path):
    with open(path, "rb") as f:
        return f.read()


def put(path, data, step=7919):
    """write(2) in odd-sized pieces, close."""
    with open(path, "wb") as f:
        for i in range(0, len(data), step):
            f.write(data[i:i + step])


def verify_image(ctx, vol, expected, tag, alts=None, pages=None):
    """Check an unmounted image: ntfsfix -n (exit code and last lines), read-only mount, size and
    SHA-256 of each file in expected {name: bytes}; pages {name: {label: bytes}} also names, for each page-size piece
    of that file, which of the labelled contents it equals (for example "old", "p1"); alts {name: {label: bytes}} names other known contents. Returns {"fsck_rc", "fsck_tail", "mount", "files": {name: state}}
    where state is "ok", "size N", "sha differs" or an errno text. An ordinary mount failure is recorded, not raised; a second FSKit refusal (MountFailed.refused) and a
    WaitTimeout are raised: the caller judges the rest."""
    out = {"tag": tag}
    r = ctx.sh("%s -n %s" % (shlex.quote(guest_tool("ntfsfix")), shlex.quote(vol.img)), check=False, timeout=180)
    out["fsck_rc"] = r.rc
    out["fsck_tail"] = (r.out + r.err).decode("utf-8", "replace").strip().splitlines()[-4:]
    w = Worker(ctx, vol, "verify-" + tag, ro=True)
    try:
        w.start()
    except Exception as e:  # a dirty volume may refuse; that is an observation
        if isinstance(e, WaitTimeout) or getattr(e, "refused", False):
            w.cleanup()
            raise  # FSKit refused twice, or the mount table never cleared: not an observation about the image
        out["mount"] = "failed: %s" % str(e).splitlines()[0]
        out["mount_tail"] = [t for _t, t in w.lines[-6:]]
        out["files"] = {}
        w.cleanup()
        return out
    out["mount"] = "ok"
    files = {}
    try:
        for name, data in sorted(expected.items()):
            p = os.path.join(w.mnt, name)
            try:
                got = slurp(p)
                if got == data:
                    files[name] = "ok"
                else:
                    files[name] = ("size %d (want %d)" % (len(got), len(data))) if len(got) != len(data) else "sha differs"
                    for label, alt in (alts or {}).get(name, {}).items():
                        if got == alt:
                            files[name] = "equals " + label
                if name in (pages or {}):
                    pm = []
                    for i in range(0, len(got), PAGE):
                        hit = [lb for lb, ref in pages[name].items() if ref[i:i + PAGE] == got[i:i + PAGE]]
                        pm.append("/".join(hit) if hit else "other")
                    out.setdefault("page_map", {})[name] = pm
            except OSError as e:
                files[name] = "error %s" % e
    finally:
        try:
            w.stop()
        except Exception as e:
            out["verify_unmount"] = str(e)
            w.cleanup()
    out["files"] = files
    return out
