# SPDX-License-Identifier: GPL-2.0-or-later
"""I-ERR-01..07 (T-M3-18, ADR-018, ARCHITECTURE.md section 7): patch 0006, the session channel on NTFSMAC_ERRFD.

The worker is a build of the patch series made here in the case's own directory (scripts/build-ntfs3g.sh):
the fault-layer build (--enable-faultio, patch 0004) for the cases that inject a failure, and the plain
series for the clean runs (in Environment B the guest build in ~/ntfsmac/deps). The channel is a pipe made by
this module; the read end is read by a thread, so the records are exactly what a daemon would see.

I-ERR-01  A     eio:N on a device write: S rw first, exactly one "E pwrite 5", last record U 1 (or E umount).
I-ERR-02  A     the device sync and the close-path sync fail (fsync() made to fail by an LD_PRELOAD library):
                one "E sync", one "E close", no U.
I-ERR-03  A, B  no fault, workload, clean unmount: exactly "S rw", "U 0". NTFSMAC_ERRFD unset: the patched worker
                behaves as the unpatched one (A: same output, same tree, same exit status); a variable that names
                a regular file or garbage is ignored.
I-ERR-04  A     dead:N, the device dies at the last write of the workload so the unmount cannot sync: E records,
                then "E umount", no U, worker exit status 0.
I-ERR-05  A, B  "-o ro" and a read/write request on the unclean-log fixture: S ro first, image unchanged; in B the
                mount is read-only.
I-ERR-06  B     the drive flush forced to fail (syncspy) on a buffered device node (DKIOCSYNCHRONIZE) and on an
                image file (F_FULLFSYNC): one "W flush <errno>" per flush that failed, sync returns 0, session
                still clean; not forced: no W.
I-ERR-07  A, B  the channel has FD_CLOEXEC and is non-blocking, a child started by the worker has no copy of it
                (faultio hook, patch 0006); a worker whose reader went away is not blocked and not killed
                (S, U and an E record are written with the reader gone; an E record before mount_fuse() is not
                reachable: a norecover mount makes no device write before it).

Everything is mounted only on images made in the case's work directory (inside the container or the guest).
norecover always, backend=fskit in B, absolute mount points, a deadline and SIGKILL on every wait.
"""

import atexit
import fcntl
import hashlib
import os
import re
import shutil
import signal
import subprocess
import tempfile
import threading
import time

from harness import Fail, case

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
MIB = 1024 * 1024
MOUNT_WAIT = 30
RECORD_RE = re.compile(r"^(S (rw|ro)|E (pwrite|write|sync|close|umount) [0-9]+|W flush [0-9]+|U [0-9]+)$")
HOOK_RE = re.compile(r"ntfsmac-errfd-hook: fd=(\d+) fdflags=(-?\d+) flflags=(-?\d+)")
SPY_RE = re.compile(r"^(\d+\.\d+) (\S+) pid=(\d+) fd=(-?\d+) path=(.*?)(?: rc=(-?\d+) errno=(\d+))?"
                    r"(?: us=(\d+))?(?: writes=(\d+) bytes=(\d+))?$")
EIO, ENOTTY = 5, 25


# ---------------------------------------------------------------------------------------------
# Environment and builds
# ---------------------------------------------------------------------------------------------
def deps_prefix():
    return os.environ.get("NTFSMAC_DEPS_PREFIX") or os.path.expanduser("~/ntfsmac/deps")


def tool(name, prefix=None):
    """Absolute path of an NTFS-3G tool: PREFIX/bin or sbin, then (B) the guest build, then PATH."""
    dirs = []
    if prefix:
        dirs += [os.path.join(prefix, "bin"), os.path.join(prefix, "sbin")]
    dirs += [os.path.join(deps_prefix(), "bin"), os.path.join(deps_prefix(), "sbin")] + os.environ.get("PATH", "").split(os.pathsep)
    for d in dirs:
        p = os.path.join(d, name)
        if d and os.access(p, os.X_OK):
            return p
    return None


def need(ctx, envs):
    if ctx.env not in envs:
        ctx.skip("runs in Environment %s" % "/".join(envs))
    if ctx.env == "B":
        from harness.volume import detect_macfuse

        if detect_macfuse(ctx.transport) is None:
            ctx.skip("macFUSE is not installed in the guest")
        for p in ("/usr/bin/hdiutil", "/sbin/umount", "/sbin/mount"):
            if not os.access(p, os.X_OK):
                ctx.skip("%s not found" % p)
    for t in ("mkntfs", "ntfsfix"):
        if tool(t) is None:
            ctx.skip("%s not found" % t)


def build_script():
    for p in (os.path.join(TESTS, "..", "scripts", "build-ntfs3g.sh"),
              os.path.expanduser("~/ntfsmac/tree/scripts/build-ntfs3g.sh")):
        if os.path.isfile(p):
            return os.path.abspath(p)
    return None


_builds = {}


def build(ctx, faultio):
    """Prefix of an NTFS-3G build of the whole patch series (with --enable-faultio when asked)."""
    if ctx.env == "B" and not faultio:
        p = deps_prefix()
        if not os.access(os.path.join(p, "bin", "ntfs-3g"), os.X_OK):
            ctx.skip("guest build of NTFS-3G not found in %s" % p)
        return p
    key = (ctx.env, faultio)
    if key in _builds:
        return _builds[key]
    script = build_script()
    if script is None:
        ctx.skip("scripts/build-ntfs3g.sh not found")
    base = tempfile.mkdtemp(prefix="errfd-build-")
    atexit.register(shutil.rmtree, base, True)
    cmd = ["sh", script, "--fuse", "external" if ctx.env == "B" else "internal", "--jobs", str(os.cpu_count() or 2),
           "--prefix", base + "/prefix", "--build-dir", base + "/build"]
    if faultio:
        cmd += ["--configure-arg", "--enable-faultio"]
    ctx.log("build: %s" % " ".join(cmd))
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
    out = r.stdout.decode("utf-8", "replace")
    ctx.log("build rc=%d, tail:\n%s" % (r.returncode, "\n".join(out.splitlines()[-6:])))
    if r.returncode != 0:
        raise RuntimeError("build of the NTFS-3G series failed (faultio=%s)" % faultio)
    _builds[key] = base + "/prefix"
    return _builds[key]


def build_reference(ctx):
    """Prefix of NTFS-3G built from the same tarball with no patch at all (build-ntfs3g.sh --no-patches).
    Patches 0007 to 0009 are re-based on 0006, so "the series without 0006" is the pristine source."""
    key = (ctx.env, "reference")
    if key in _builds:
        return _builds[key]
    script = build_script()
    if script is None:
        raise Fail("scripts/build-ntfs3g.sh not found: no reference binary")
    base = tempfile.mkdtemp(prefix="errfd-ref-")
    atexit.register(shutil.rmtree, base, True)
    cmd = ["sh", script, "--fuse", "external" if ctx.env == "B" else "internal", "--jobs", str(os.cpu_count() or 2),
           "--no-patches", "--prefix", base + "/prefix", "--build-dir", base + "/build"]
    r = subprocess.run(cmd, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=1800)
    ctx.log("reference build rc=%d" % r.returncode)
    exe = base + "/prefix/bin/ntfs-3g"
    if r.returncode != 0 or not os.access(exe, os.X_OK):
        raise Fail("reference build failed: %s" % r.stdout.decode("utf-8", "replace")[-800:])
    _builds[key] = base + "/prefix"
    return _builds[key]


def sh(ctx, argv, timeout=300, check=True):
    r = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
    out = r.stdout.decode("utf-8", "replace")
    ctx.log("$ %s -> rc=%d%s" % (" ".join(argv), r.returncode, ("\n" + out.rstrip()[-1500:]) if out.strip() else ""))
    if check and r.returncode != 0:
        raise RuntimeError("command failed (rc=%d): %s" % (r.returncode, " ".join(argv)))
    return r.returncode, out


def fresh_image(ctx, tag, size=64 * MIB, ntfs_tool_prefix=None):
    img = os.path.join(ctx.work, "%s.img" % tag)
    with open(img, "wb") as f:
        f.truncate(size)
    sh(ctx, [tool("mkntfs", ntfs_tool_prefix), "-F", "-Q", "-L", "errfd", img])
    return img


def unclean_log_image(ctx, tag):
    """The unclean-log fixture (tests/fixtures/manifest.json): mkntfs, then logfile_mark --init --unclean."""
    img = fresh_image(ctx, tag)
    prefix = "/opt/ntfs-3g" if ctx.env == "A" else deps_prefix()
    exe = os.path.join(ctx.work, "logfile_mark")
    src = os.path.join(HERE, "logfile_mark.c")
    orig = os.path.join(TESTS, "tools", "logfile_mark.c")
    if os.path.exists(orig):
        with open(src) as f:
            mine = f.read().split("\n", 3)[3]
        with open(orig) as f:
            theirs = f.read().split("\n", 1)[1]
        expect(mine == theirs, "tests/integration/errfd/logfile_mark.c differs from tests/tools/logfile_mark.c")
    if not os.path.exists(exe):
        cc = ["cc", "-std=c17", "-O2", "-Wall", "-Wextra"] + (["-D_DARWIN_C_SOURCE"] if ctx.env == "B" else []) + [
            "-isystem", prefix + "/include", "-o", exe, src, "-L" + prefix + "/lib", "-lntfs-3g"]
        if ctx.env == "B":
            cc += ["-framework", "CoreFoundation"]  # unistr.o of the static library uses CFString
        if sh(ctx, cc, check=False)[0] != 0:
            sh(ctx, cc + ["-liconv"])
    sh(ctx, [exe, img, "--init", "--unclean"])
    return img


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        for chunk in iter(lambda: f.read(MIB), b""):
            h.update(chunk)
    return h.hexdigest()


def pattern(n, salt):
    out = bytearray()
    h = hashlib.sha256(("errfd-%d" % salt).encode()).digest()
    while len(out) < n:
        h = hashlib.sha256(h).digest()
        out += h
    return bytes(out[:n])


# ---------------------------------------------------------------------------------------------
# The worker with its session channel
# ---------------------------------------------------------------------------------------------
class MountFailed(RuntimeError):
    pass


class Session(object):
    """ntfs-3g in the foreground on an image file (or a device node given as an inherited descriptor), with a
    pipe as NTFSMAC_ERRFD whose read end this object reads."""

    def __init__(self, ctx, tag, prefix, img=None, dev=None, ro=False, env=None, errfd=True, reader_gone=False,
                 opts="", extra_fds=()):
        self.ctx, self.tag, self.prefix, self.img, self.dev = ctx, tag, prefix, img, dev
        self.mnt = os.path.realpath(os.path.join(ctx.work, "mnt-%s" % tag))
        self.errfd, self.reader_gone = errfd, reader_gone
        self.extra_fds = tuple(extra_fds)
        self.exe = tool("ntfs-3g", prefix)
        o = "norecover,no_detach"
        if ctx.env == "B":
            o = "backend=fskit,local,norecover,uid=%d,gid=%d,no_detach" % (os.getuid(), os.getgid())
        if ro:
            o += ",ro"
        if opts:
            o += "," + opts
        self.opts = o
        self.env = dict(os.environ)
        self.env.update(env or {})
        self.raw = b""
        self.lines = []
        self.proc = None
        self.rc = None
        self.umount_rc = None
        self.chan_eof = False
        self._r = None
        self._threads = []
        self._watchdog = None

    # ---- reading ----
    def _read_out(self):
        for raw in self.proc.stdout:
            self.lines.append(raw.decode("utf-8", "replace").rstrip("\n"))

    def _read_chan(self):
        while True:
            try:
                b = os.read(self._r, 4096)
            except OSError:
                break
            if not b:
                self.chan_eof = True
                break
            self.raw += b

    def _mounted(self):
        if self.ctx.env == "B":
            r = subprocess.run(["/sbin/mount"], stdout=subprocess.PIPE, timeout=30)
            for line in r.stdout.decode("utf-8", "replace").splitlines():
                if (" on %s (" % self.mnt) in line:
                    self.mount_line = line
                    return True
            return False
        with open("/proc/mounts", "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                p = line.split()
                if len(p) > 2 and p[1] == self.mnt:
                    self.mount_line = line
                    return True
        return False

    def start(self, deadline=900):
        os.makedirs(self.mnt, exist_ok=True)
        fd = None
        target = self.img
        pass_fds = list(self.extra_fds)
        if self.dev is not None:
            fd = self.dev.open_fd()
            target = "/dev/fd/%d" % fd
            pass_fds.append(fd)
        n = None
        if self.errfd:
            r, w = os.pipe()
            n = fcntl.fcntl(w, fcntl.F_DUPFD, 20)  # a high number: no clash with the lowest free descriptor
            os.close(w)
            self.env["NTFSMAC_ERRFD"] = str(n)
            pass_fds.append(n)
            if self.reader_gone:
                os.close(r)
                self._r = None
            else:
                self._r = r
        argv = [self.exe, "-o", self.opts, target, self.mnt]
        self.ctx.log("worker: %s  NTFSMAC_ERRFD=%s" % (" ".join(argv), self.env.get("NTFSMAC_ERRFD")))
        self.proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                     env=self.env, pass_fds=tuple(pass_fds), close_fds=True)
        if n is not None:
            os.close(n)  # the write end now lives only in the worker
        t = threading.Thread(target=self._read_out, daemon=True)
        t.start()
        self._threads.append(t)
        if self._r is not None:
            t = threading.Thread(target=self._read_chan, daemon=True)
            t.start()
            self._threads.append(t)
        self._watchdog = threading.Timer(deadline, self.kill)
        self._watchdog.daemon = True
        self._watchdog.start()
        self.ctx.add_cleanup(self.kill)
        end = time.time() + MOUNT_WAIT
        while time.time() < end:
            if self.proc.poll() is not None:
                break
            if self._mounted():
                return
            time.sleep(0.2)
        self.kill()
        tail = "\n".join(self.lines[-8:])
        raise MountFailed("no mount on %s (%s); worker output tail:\n%s" % (
            self.mnt, "worker exited rc=%s" % self.proc.returncode if self.proc.returncode is not None
            else "no mount after %d s" % MOUNT_WAIT, tail))

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

    def stop(self, check=True):
        """umount, wait for the worker to exit (deadline, then SIGKILL), wait for end-of-file on the channel."""
        um = ["/sbin/umount", self.mnt] if self.ctx.env == "B" else ["umount", self.mnt]
        try:
            r = subprocess.run(um, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=60)
            self.umount_rc = r.returncode
            if r.returncode:
                self.ctx.log("umount rc=%d: %s" % (r.returncode, r.stdout.decode("utf-8", "replace").strip()))
        except subprocess.TimeoutExpired:
            self.umount_rc = 124
            self.kill()
        try:
            self.proc.wait(MOUNT_WAIT)
        except subprocess.TimeoutExpired:
            self.ctx.log("worker still running %d s after umount, SIGKILL" % MOUNT_WAIT)
            self.kill()
        self.rc = self.proc.returncode
        for t in self._threads:
            t.join(5)
        if self._watchdog is not None:
            self._watchdog.cancel()
        if self._r is not None:
            try:
                os.close(self._r)
            except OSError:
                pass
            self._r = None
        if self.dev is not None:
            self.dev.close_fd()
        self.ctx.log("worker exit rc=%s, umount rc=%s, channel at EOF: %s" % (self.rc, self.umount_rc, self.chan_eof))
        self.ctx.log("records: %s" % self.records())
        if check and self.umount_rc != 0:
            raise RuntimeError("umount failed (rc=%s)" % self.umount_rc)

    def records(self):
        txt = self.raw.decode("ascii", "replace")
        if not txt:
            return []
        return txt.split("\n")[:-1] if txt.endswith("\n") else txt.split("\n")


def check_records(recs, raw=None):
    """Every record in the grammar, at most 32 bytes with the newline, at most 64 records."""
    if len(recs) > 64:
        raise Fail("%d records, more than 64" % len(recs))
    for r in recs:
        if len(r) + 1 > 32 or not RECORD_RE.match(r):
            raise Fail("record outside the grammar: %r" % r)


def expect(cond, msg):
    if not cond:
        raise Fail(msg)


# ---------------------------------------------------------------------------------------------
# Workload and tree hash
# ---------------------------------------------------------------------------------------------
def workload(mnt, errors):
    """Directories, small and large files, rename, truncate, unlink, fsync. Failures are collected, not raised
    (a fault case expects some)."""
    root = os.path.join(mnt, "w")

    def run(label, fn):
        try:
            fn()
        except OSError as e:
            errors.append("%s: %s" % (label, e))

    run("mkdir", lambda: os.makedirs(os.path.join(root, "a", "b", "c")))
    for i in range(30):
        def mk(i=i):
            with open(os.path.join(root, "a", "f%02d" % i), "wb") as f:
                f.write(pattern(100 + i * 997, i))
        run("small%d" % i, mk)

    def big():
        with open(os.path.join(root, "a", "b", "big.bin"), "wb") as f:
            f.write(pattern(3 * MIB + 17, 100))
            f.flush()
            os.fsync(f.fileno())
    run("big", big)
    run("rename", lambda: os.rename(os.path.join(root, "a", "f01"), os.path.join(root, "a", "b", "c", "moved")))
    run("truncate", lambda: os.truncate(os.path.join(root, "a", "f02"), 10))
    run("unlink", lambda: os.unlink(os.path.join(root, "a", "f03")))

    def grow():
        with open(os.path.join(root, "a", "f04"), "ab") as f:
            f.write(pattern(50000, 200))
    run("append", grow)


def tree_hash(mnt):
    out = []
    for dp, dns, fns in os.walk(mnt):
        dns.sort()
        for fn in sorted(fns):
            p = os.path.join(dp, fn)
            try:
                out.append((os.path.relpath(p, mnt), os.path.getsize(p), sha256_file(p)))
            except OSError as e:
                out.append((os.path.relpath(p, mnt), -1, str(e)))
        out.append((os.path.relpath(dp, mnt) + "/", 0, ""))
    return out


def faultio_env(ctx, plan, tag):
    state = os.path.join(ctx.work, "state-%s" % tag)
    return {"NTFSMAC_FAULT_PLAN": plan, "NTFSMAC_FAULT_STATE": state}, state


def writes_now(state):
    try:
        with open(state, "r") as f:
            return int(f.read().strip() or 0)
    except (IOError, ValueError):
        return 0


def count_run(ctx, prefix, tag):
    """Plan count: the number of device writes at the end of the workload and after the unmount."""
    img = fresh_image(ctx, tag)
    env, state = faultio_env(ctx, "count", tag)
    s = Session(ctx, tag, prefix, img=img, env=env)
    s.start()
    workload(s.mnt, [])
    w_work = writes_now(state)
    s.stop()
    w_all = writes_now(state)
    recs = s.records()
    check_records(recs)
    expect(recs == ["S rw", "U 0"], "count run: records %r, expected S rw, U 0" % (recs,))
    ctx.log("count run: %d device writes during the workload, %d after the unmount" % (w_work, w_all))
    return w_work, w_all


# ---------------------------------------------------------------------------------------------
# I-ERR-01 .. I-ERR-04 (Environment A: Linux reference, fault-layer build)
# ---------------------------------------------------------------------------------------------
@case("I-ERR-01", envs=("A",), title="session channel: eio on a device write gives one E pwrite 5 (ADR-018)")
def i_err_01(ctx):
    need(ctx, ("A",))
    prefix = build(ctx, True)
    w_work, _w_all = count_run(ctx, prefix, "c1")
    n = max(1, w_work // 2)
    img = fresh_image(ctx, "e1")
    env, _state = faultio_env(ctx, "eio:%d" % n, "e1")
    s = Session(ctx, "e1", prefix, img=img, env=env)
    s.start()
    errors = []
    workload(s.mnt, errors)
    ctx.log("workload errors: %s" % errors)
    s.stop()
    recs = s.records()
    check_records(recs)
    expect(recs and recs[0] == "S rw", "first record %r, expected S rw" % (recs[:1],))
    e = [r for r in recs if r.startswith("E ")]
    pw = [r for r in e if r.startswith("E pwrite") or r.startswith("E write")]
    expect(pw == ["E pwrite 5"], "write error records %r, expected exactly one E pwrite 5 (all E: %r)" % (pw, e))
    last = recs[-1]
    expect(last == "U 1" or last.startswith("E umount "), "last record %r, expected U 1 or E umount" % last)
    expect(s.chan_eof, "no end-of-file on the channel after the worker exit")


@case("I-ERR-02", envs=("A",), title="session channel: failed device sync and close-path sync give E sync and E close")
def i_err_02(ctx):
    need(ctx, ("A",))
    prefix = build(ctx, True)
    shim = os.path.join(ctx.work, "fsyncfail.so")
    sh(ctx, ["cc", "-Wall", "-Wextra", "-Werror", "-shared", "-fPIC", "-o", shim,
             os.path.join(HERE, "fsyncfail.c"), "-ldl"])
    flag = os.path.join(ctx.work, "fsync-fail.flag")
    img = fresh_image(ctx, "e2")
    s = Session(ctx, "e2", prefix, img=img, env={"LD_PRELOAD": shim, "NTFSMAC_FSYNCFAIL_FLAG": flag})
    s.start()
    workload(s.mnt, [])
    # The workload's fsync worked (no flag). Make some data dirty, then fail every fsync() of the worker.
    with open(os.path.join(s.mnt, "w", "dirty.bin"), "wb") as f:
        f.write(pattern(20000, 7))
    open(flag, "w").close()
    s.stop(check=False)
    recs = s.records()
    check_records(recs)
    expect(recs and recs[0] == "S rw", "first record %r, expected S rw" % (recs[:1],))
    expect(recs.count("E sync 5") == 1, "E sync 5 x%d, expected one: %r" % (recs.count("E sync 5"), recs))
    expect(recs.count("E close 5") == 1, "E close 5 x%d, expected one: %r" % (recs.count("E close 5"), recs))
    expect(not [r for r in recs if r.startswith("U ")], "a U record after failed syncs: %r" % (recs,))
    expect(not [r for r in recs if r.startswith("E pwrite") or r.startswith("E write")], "write errors: %r" % (recs,))


@case("I-ERR-03", envs=("A", "B"), title="session channel: clean session is exactly S rw, U 0; unset is like unpatched")
def i_err_03(ctx):
    need(ctx, ("A", "B"))
    prefix = build(ctx, False)
    img = fresh_image(ctx, "c3")
    s = Session(ctx, "c3", prefix, img=img)
    s.start()
    errors = []
    workload(s.mnt, errors)
    expect(not errors, "workload errors: %s" % errors)
    tree_on = tree_hash(os.path.join(s.mnt, "w"))
    s.stop()
    recs = s.records()
    check_records(recs)
    expect(recs == ["S rw", "U 0"], "records %r, expected exactly S rw, U 0" % (recs,))
    expect(s.chan_eof and s.rc == 0, "channel EOF %s, worker rc %s" % (s.chan_eof, s.rc))
    rc, out = sh(ctx, [tool("ntfsfix"), "-n", img], check=False)
    expect(rc == 0, "ntfsfix -n rc=%d after the clean session" % rc)

    # The variable unset: no channel, same behaviour.
    img2 = fresh_image(ctx, "c3u")
    s2 = Session(ctx, "c3u", prefix, img=img2, errfd=False)
    s2.env.pop("NTFSMAC_ERRFD", None)
    s2.start()
    workload(s2.mnt, [])
    tree_off = tree_hash(os.path.join(s2.mnt, "w"))
    s2.stop()
    expect(s2.raw == b"" and s2.rc == 0, "unset: channel bytes %r, worker rc %s" % (s2.raw, s2.rc))
    expect(tree_on == tree_off, "tree differs with and without the channel")
    expect(sh(ctx, [tool("ntfsfix"), "-n", img2], check=False)[0] == 0, "ntfsfix -n after the unset session")

    if ctx.env == "A":
        # Unset: the patched worker equals the unpatched one (same output, tree and exit status).
        stock = tool("ntfs-3g", build_reference(ctx))
        expect(stock and os.path.realpath(stock) != os.path.realpath(s.exe), "no reference ntfs-3g to compare with")
        img3 = fresh_image(ctx, "c3s")
        s3 = Session(ctx, "c3s", None, img=img3, errfd=False)
        s3.exe = stock
        s3.env.pop("NTFSMAC_ERRFD", None)
        s3.start()
        workload(s3.mnt, [])
        tree_stock = tree_hash(os.path.join(s3.mnt, "w"))
        s3.stop()

        def norm(sess, lines):
            return [ln.replace(sess.mnt, "MNT").replace(sess.img, "IMG") for ln in lines]

        expect(tree_stock == tree_off, "tree differs between the unpatched and the patched worker")
        expect(norm(s3, s3.lines) == norm(s2, s2.lines) and s3.rc == s2.rc,
               "output or exit status differs from the unpatched worker:\n%s\n--\n%s" % (s3.lines, s2.lines))
        # A variable that names a regular file, or is not a number, is ignored: nothing is written.
        junk = os.path.join(ctx.work, "junk.out")
        with open(junk, "wb") as f:
            jfd = fcntl.fcntl(f.fileno(), fcntl.F_DUPFD, 30)
        try:
            for tag, val in (("file", str(jfd)), ("text", "abc"), ("low", "1"), ("neg", "-5"), ("big", "99999999999")):
                imgj = fresh_image(ctx, "c3j" + tag)
                sj = Session(ctx, "c3j" + tag, prefix, img=imgj, errfd=False, env={"NTFSMAC_ERRFD": val},
                             extra_fds=(jfd,))
                sj.start()
                workload(sj.mnt, [])
                sj.stop()
                expect(sj.rc == 0, "variable %r: worker rc %s" % (val, sj.rc))
                ctx.log("variable %r ignored, worker rc 0" % val)
        finally:
            os.close(jfd)
        expect(os.path.getsize(junk) == 0, "a record was written to a regular file named by NTFSMAC_ERRFD")


@case("I-ERR-04", envs=("A",), title="session channel: failed ntfs_umount gives E umount, no U, exit status 0")
def i_err_04(ctx):
    need(ctx, ("A",))
    prefix = build(ctx, True)
    w_work, w_all = count_run(ctx, prefix, "c4")
    # The workload leaves nothing for the unmount to write (every inode is flushed by then), so
    # the device dies at the last write of the workload: that write, every later one and every later sync
    # fail, the unmount's sync included.
    ctx.log("device writes: %d at the end of the workload, %d after the unmount" % (w_work, w_all))
    img = fresh_image(ctx, "e4")
    env, _state = faultio_env(ctx, "dead:%d" % w_work, "e4")
    s = Session(ctx, "e4", prefix, img=img, env=env)
    s.start()
    workload(s.mnt, [])
    s.stop(check=False)
    recs = s.records()
    check_records(recs)
    expect(recs and recs[0] == "S rw", "first record %r, expected S rw" % (recs[:1],))
    expect(recs[-1].startswith("E umount "), "last record %r, expected E umount <errno>" % recs[-1])
    expect(not [r for r in recs if r.startswith("U ")], "a U record after a failed unmount: %r" % (recs,))
    expect("E pwrite 5" in recs, "no E pwrite 5 from the dead device: %r" % (recs,))
    expect(s.rc == 0, "worker exit status %s, expected 0 (exit status is not evidence)" % s.rc)
    expect(s.chan_eof, "no end-of-file on the channel after the worker exit")


# ---------------------------------------------------------------------------------------------
# I-ERR-05 (A and B)
# ---------------------------------------------------------------------------------------------
@case("I-ERR-05", envs=("A", "B"), title="session channel: S ro for -o ro and for the unclean-log fallback")
def i_err_05(ctx):
    need(ctx, ("A", "B"))
    prefix = build(ctx, False)
    # (a) -o ro on a clean image.
    img = fresh_image(ctx, "r5")
    before = sha256_file(img)
    s = Session(ctx, "r5", prefix, img=img, ro=True)
    s.start()
    os.listdir(s.mnt)
    if ctx.env == "B":
        expect("read-only" in s.mount_line, "mount line has no read-only flag: %s" % s.mount_line)
    s.stop()
    recs = s.records()
    check_records(recs)
    expect(recs == ["S ro", "U 0"], "-o ro: records %r, expected S ro, U 0" % (recs,))
    expect(sha256_file(img) == before, "-o ro changed the image")
    # (b) a read/write request on unclean-log: the engine mounts read-only by itself.
    img = unclean_log_image(ctx, "u5")
    before = sha256_file(img)
    s = Session(ctx, "u5", prefix, img=img)
    s.start()
    os.listdir(s.mnt)
    if ctx.env == "B":
        expect("read-only" in s.mount_line, "unclean-log: mount line has no read-only flag: %s" % s.mount_line)
    s.stop()
    recs = s.records()
    check_records(recs)
    expect("Falling back to read-only mount" in "\n".join(s.lines),
           "no read-only fallback message in the worker output: %s" % s.lines)
    expect(recs == ["S ro", "U 0"], "unclean-log: records %r, expected S ro, U 0" % (recs,))
    expect(sha256_file(img) == before, "the unclean-log session changed the image")


# ---------------------------------------------------------------------------------------------
# I-ERR-06 (B): the drive flush fails, W flush
# ---------------------------------------------------------------------------------------------
def load_isync():
    import importlib.util

    spec = importlib.util.spec_from_file_location("errfd_isync", os.path.join(HERE, "..", "sync", "i_sync.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod


def spy_records(path, pid):
    out = []
    try:
        with open(path, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                m = SPY_RE.match(line.rstrip("\n"))
                if m and int(m.group(3)) == pid:
                    out.append({"ev": m.group(2), "rc": int(m.group(6)) if m.group(6) is not None else None,
                                "errno": int(m.group(7)) if m.group(7) is not None else None})
    except IOError:
        pass
    return out


def flush_session(ctx, isync, spy, kind, tag, fail):
    """One session with every sync point: write, fsync, F_FULLFSYNC, sync(2), unmount."""
    prefix = build(ctx, False)
    dev = isync.Dev(ctx, kind, tag, size=64 * MIB)
    log = os.path.join(ctx.work, "spy-%s.log" % tag)
    env = {"DYLD_INSERT_LIBRARIES": spy, "NTFSMAC_SYNCSPY_LOG": log}
    if fail:
        env["NTFSMAC_SYNCSPY_FAIL"] = fail
    if kind == "file":
        s = Session(ctx, tag, prefix, img=dev.img, env=env)
    else:
        s = Session(ctx, tag, prefix, dev=dev, env=env)
    s.start()
    time.sleep(3.0)  # macOS fsyncs .fseventsd right after the mount
    path = os.path.join(s.mnt, "sp.dat")
    fd = os.open(path, os.O_CREAT | os.O_RDWR, 0o644)
    rets = []
    try:
        for i in range(3):
            os.pwrite(fd, pattern(4096, i), i * 4096)
            if i == 0:
                rets.append(os.fsync(fd))
            elif i == 1:
                rets.append(fcntl.fcntl(fd, fcntl.F_FULLFSYNC))
            else:
                rets.append(os.sync())
            time.sleep(1.0)
    finally:
        os.close(fd)
    s.stop()
    dev.detach()
    recs = s.records()
    check_records(recs)
    spy_recs = spy_records(log, s.proc.pid)
    return s, recs, spy_recs, rets


@case("I-ERR-06", envs=("B",), title="session channel: W flush when the drive flush fails, none otherwise (ADR-022)")
def i_err_06(ctx):
    need(ctx, ("B",))
    isync = load_isync()
    isync.need_guest(ctx)
    spy = isync.build_spy(ctx)
    for kind, fail, flush_ev, errno_want, label in (
            ("buffered", "dksync", "dksync", EIO, "device node, DKIOCSYNCHRONIZE forced to fail"),
            ("file", "fullfsync", "fullfsync", ENOTTY, "image file, F_FULLFSYNC forced to fail"),
            ("buffered", None, "dksync", None, "device node, no forced failure"),
            ("file", None, "fullfsync", None, "image file, no forced failure")):
        tag = "%s-%s" % (kind, fail or "ok")
        s, recs, spy_recs, rets = flush_session(ctx, isync, spy, kind, tag, fail)
        w = [r for r in recs if r.startswith("W ")]
        flushes = [r for r in spy_recs if r["ev"] == flush_ev]
        ctx.log("%s: %d flush calls seen by syncspy, W records %s, other records %s" % (
            label, len(flushes), w, [r for r in recs if not r.startswith("W ")]))
        expect(recs and recs[0] == "S rw" and recs[-1] == "U 0" and not [r for r in recs if r.startswith("E ")],
               "%s: session not clean: %r" % (label, recs))
        expect(all(r == 0 for r in rets if r is not None), "%s: a sync call returned %r" % (label, rets))
        expect(len(flushes) >= 2, "%s: only %d flush calls reached the device" % (label, len(flushes)))
        if fail:
            expect(all(r["rc"] != 0 for r in flushes), "%s: syncspy did not fail every flush" % label)
            expect(w and set(w) == {"W flush %d" % errno_want},
                   "%s: W records %r, expected only W flush %d" % (label, w, errno_want))
            expect(len(w) == len(flushes), "%s: %d W records for %d failed flushes" % (label, len(w), len(flushes)))
        else:
            expect(not w, "%s: W records without a forced failure: %r" % (label, w))
            expect(all(r["rc"] == 0 for r in flushes), "%s: a flush failed without injection" % label)


# ---------------------------------------------------------------------------------------------
# I-ERR-07 (A and B): descriptor hygiene
# ---------------------------------------------------------------------------------------------
@case("I-ERR-07", envs=("A", "B"), title="session channel: FD_CLOEXEC, non-blocking, no copy in a child, reader gone")
def i_err_07(ctx):
    need(ctx, ("A", "B"))
    prefix = build(ctx, True)
    # Control: the listing shows an inherited descriptor.
    r, w = os.pipe()
    n = fcntl.fcntl(w, fcntl.F_DUPFD, 20)
    ctl = subprocess.run(["ls", "/dev/fd"], stdout=subprocess.PIPE, pass_fds=(n,), timeout=30)
    os.close(n)
    os.close(w)
    os.close(r)
    expect(str(n) in ctl.stdout.decode().split(), "control: ls /dev/fd does not list the inherited descriptor %d" % n)

    out = os.path.join(ctx.work, "child-fds.txt")
    img = fresh_image(ctx, "h7")
    envout = os.path.join(ctx.work, "child-env.txt")
    s = Session(ctx, "h7", prefix, img=img,
                env={"NTFSMAC_FAULT_ERRFD_CMD": "ls /dev/fd > %s; env > %s" % (out, envout)})
    s.start()
    workload(s.mnt, [])
    s.stop()
    hook = [HOOK_RE.search(ln) for ln in s.lines]
    hook = [m for m in hook if m]
    expect(hook, "no hook line in the worker output (faultio build with patch 0006?): %s" % s.lines[:6])
    fdn, fdflags, flflags = (int(x) for x in hook[0].groups())
    expect(fdn == int(s.env["NTFSMAC_ERRFD"]), "hook names descriptor %d, expected %s" % (fdn, s.env["NTFSMAC_ERRFD"]))
    expect(fdflags & fcntl.FD_CLOEXEC, "the channel has no FD_CLOEXEC (F_GETFD=%d)" % fdflags)
    expect(flflags & os.O_NONBLOCK, "the channel is not non-blocking (F_GETFL=%d)" % flflags)
    with open(out) as f:
        child = f.read().split()
    ctx.log("descriptors of the child: %s" % child)
    expect(child and str(fdn) not in child, "the child has a copy of the channel (descriptor %d in %s)" % (fdn, child))
    with open(envout) as f:
        expect("NTFSMAC_ERRFD=" not in f.read(), "the child inherited the NTFSMAC_ERRFD variable")
    recs = s.records()
    check_records(recs)
    expect(recs == ["S rw", "U 0"], "records %r, expected S rw, U 0" % (recs,))

    # The reader went away before the worker started: records are dropped, the worker is neither blocked nor killed.
    img = fresh_image(ctx, "g7")
    s = Session(ctx, "g7", prefix, img=img, reader_gone=True)
    s.start()
    errors = []
    workload(s.mnt, errors)
    expect(not errors, "workload errors with the reader gone: %s" % errors)
    ok = sh(ctx, ["ls", s.mnt + "/w/a"], check=False)[0] == 0
    s.stop()
    expect(ok and s.rc == 0, "reader gone: worker rc %s, listing ok %s" % (s.rc, ok))
    expect(sh(ctx, [tool("ntfsfix"), "-n", img], check=False)[0] == 0, "ntfsfix -n after the reader-gone session")

    # Same, with an E record: eio:1 fails the first device write, the reader is already gone, and the worker must
    # not die of SIGPIPE. Not shown: an E record before mount_fuse(). A read/write mount with norecover makes no
    # device write before the mount appears (the case logs the write count at that point: 0), so no fault plan can
    # fail one there. The S and U records of the run above and the E record here are written with the reader gone;
    # S and the E record are written while libfuse's signal handling may already be installed, which is why the
    # write itself blocks SIGPIPE (or sets F_SETNOSIGPIPE) instead of relying on it.
    img = fresh_image(ctx, "p7")
    env, state = faultio_env(ctx, "eio:1", "p7")
    s = Session(ctx, "p7", prefix, img=img, env=env, reader_gone=True)
    try:
        s.start()
        early = writes_now(state)
        ctx.log("device writes seen by the time the mount appeared: %d" % early)
        workload(s.mnt, [])
        s.stop(check=False)
    except MountFailed:
        early = -1
    ctx.log("worker rc=%s (negative: killed by that signal), output tail: %s" % (s.rc, s.lines[-3:]))
    expect(s.proc.returncode is None or s.proc.returncode != -signal.SIGPIPE,
           "the worker died of SIGPIPE (rc %s)" % s.proc.returncode)
    expect(s.proc.returncode is not None and s.proc.returncode >= 0, "the worker was killed (rc %s)" % s.proc.returncode)
