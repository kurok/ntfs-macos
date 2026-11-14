# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers of the functional suite (I-FUNC-*, I-NAME-*, I-INT-CYCLE-*, T-M3-05).

Not a case module (no case declarations, so the harness does not import it as one); the case
modules put their own directory on sys.path and import it as `fsops`. Python 3.9, standard library only.

What it adds to harness.volume:
  mount(ctx, vol, rw, extra)   mount with extra NTFS-3G options (windows_names, nonfconv, ...), a
                               deadline, and SIGKILL of the driver when the mount never appears
  mount_debug(...)             Environment B only: foreground driver with `debug`, its stderr in a file
  unmount(ctx, vol)            unmount; on failure forced unmount and SIGKILL of the driver
  remount_check(...)           unmount, remount read-only, hash the whole tree, compare with the model
  fsck(ctx, vol)               `ntfsfix -n` on the unmounted image, exit 0 required
The mandatory option norecover is always passed in Environment B; recover, remove_hiberfile and
fsname are refused here as well as by the adapter's own policy (ARCHITECTURE.md 3.3).
"""

import errno
import hashlib
import os
import random
import shlex
import subprocess
import time
import unicodedata

from harness import Fail, volume as hv
from harness.seeded import MACOS_ROOT_METADATA, MIB, compare, hash_tree

BANNED_OPTIONS = ("recover", "remove_hiberfile", "fsname")
MOUNT_DEADLINE = 40


def blob(label, size):
    """Deterministic bytes from a text label."""
    return random.Random(label).randbytes(size)


def sha(data):
    return hashlib.sha256(data).hexdigest()


def write_chunks(f, data, step=7919):
    """Write in odd-sized pieces (7919 is prime) so that no write lines up with a cluster."""
    for i in range(0, len(data), step):
        f.write(data[i:i + step])


def put(path, data, mode="wb", step=7919):
    with open(path, mode) as f:
        write_chunks(f, data, step)


def slurp(path):
    with open(path, "rb") as f:
        return f.read()


def expect_errno(ctx, accepted, fn, *args, desc=""):
    """Call fn(*args); it must raise OSError with an errno in `accepted` (names or numbers)."""
    nums = set(getattr(errno, a) if isinstance(a, str) else a for a in accepted)
    try:
        fn(*args)
    except OSError as e:
        ctx.log("%s -> errno %d (%s)" % (desc or fn.__name__, e.errno, errno.errorcode.get(e.errno, "?")))
        if e.errno not in nums:
            raise Fail("%s: errno %s (%s), expected one of %s" % (
                desc, e.errno, errno.errorcode.get(e.errno, "?"), sorted(errno.errorcode[n] for n in nums)))
        return e.errno
    raise Fail("%s: succeeded, expected an error from %s" % (desc, sorted(errno.errorcode[n] for n in nums)))


def list_dirs(root):
    out = []
    for dp, dn, _fns in os.walk(root):
        if dp == root:
            dn[:] = [d for d in dn if d not in MACOS_ROOT_METADATA]
        for d in dn:
            out.append(os.path.relpath(os.path.join(dp, d), root).replace(os.sep, "/"))
    return sorted(out)


def nfc_keys(files):
    return dict((unicodedata.normalize("NFC", k), v) for k, v in files.items())


# ---- driver processes ----

def driver_pids(img):
    r = subprocess.run(["/bin/ps", "-axo", "pid=,args="], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL)
    out = []
    for line in r.stdout.decode("utf-8", "replace").splitlines():
        parts = line.split(None, 1)
        if len(parts) != 2:
            continue
        args = parts[1].split()
        if args and os.path.basename(args[0]) == "ntfs-3g" and img in parts[1]:
            out.append(int(parts[0]))
    return out


def kill_driver(ctx, img):
    for pid in driver_pids(img):
        ctx.log("SIGKILL ntfs-3g pid %d" % pid)
        try:
            os.kill(pid, 9)
        except OSError:
            pass


def _check_extra(extra):
    for tok in [t for t in extra.split(",") if t]:
        if tok in BANNED_OPTIONS or tok.startswith("fsname="):
            raise ValueError("banned mount option: %s" % tok)
        if tok == "norecover":
            raise ValueError("norecover is always passed; do not repeat it")


def _b_options(rw, extra):
    o = "backend=fskit,local,norecover,uid=%d,gid=%d" % (os.getuid(), os.getgid())
    if not rw:
        o += ",ro"
    return o + ("," + extra if extra else "")


def _wait_b_mount(ctx, vol, proc=None):
    deadline = time.time() + MOUNT_DEADLINE
    line = None
    while time.time() < deadline:
        line = vol._mount_line()
        if line:
            break
        # With `debug` the driver still daemonises (daemon(0, debug) keeps stderr), so a zero exit of
        # the process we started is normal; only a failure exit ends the wait early.
        if proc is not None and proc.poll() not in (None, 0):
            break
        time.sleep(0.2)
    if not line:
        kill_driver(ctx, vol.img)
        raise hv.VolumeError("no mount on %s within %d s (driver killed)" % (vol.mnt, MOUNT_DEADLINE))
    ctx.log("mount: %s" % line)
    if "fskit" not in line:
        kill_driver(ctx, vol.img)
        raise hv.VolumeError("not an fskit mount: %s" % line)
    vol.mounted = True


def mount(ctx, vol, rw=True, extra=""):
    """Mount vol with extra options. Environment B: backend=fskit,local,norecover,uid,gid,[ro,]extra."""
    _check_extra(extra)
    if ctx.env == "B":
        ctx.sh("%s -o %s %s %s" % (shlex.quote(hv.guest_tool("ntfs-3g")), _b_options(rw, extra),
                                    shlex.quote(vol.img), shlex.quote(vol.mnt)))
        _wait_b_mount(ctx, vol)
    elif ctx.env == "A":
        mode = "ro" if not rw else "rw,big_writes"
        ctx.sh("ntfs-3g %s %s -o %s%s" % (shlex.quote(vol.img), shlex.quote(vol.mnt), mode,
                                           "," + extra if extra else ""))
        vol.mounted = True
    else:
        raise RuntimeError("no mount for env %s" % ctx.env)


def mount_debug(ctx, vol, log_path, rw=True, extra=""):
    """Environment B: foreground driver with `debug` (libfuse and NTFS-3G trace on stderr), killed on
    timeout. The log is raw bytes (names are not escaped). unmount() reaps the process. The driver
    forks after start-up (stderr stays open), so the process we started exits 0 early."""
    if ctx.env != "B":
        raise RuntimeError("mount_debug is for Environment B")
    _check_extra(extra)
    opts = _b_options(rw, extra) + ",debug"
    logf = open(log_path, "wb")
    argv = [hv.guest_tool("ntfs-3g"), "-o", opts, vol.img, vol.mnt]
    ctx.log("$ %s" % " ".join(shlex.quote(a) for a in argv))
    proc = subprocess.Popen(argv, stdin=subprocess.DEVNULL, stdout=logf, stderr=subprocess.STDOUT,
                            start_new_session=True)
    vol._dbg = (proc, logf)
    try:
        _wait_b_mount(ctx, vol, proc)
    except Exception:
        proc.kill()
        logf.close()
        vol._dbg = None
        raise


def _reap_debug(ctx, vol):
    d = getattr(vol, "_dbg", None)
    if not d:
        return
    proc, logf = d
    try:
        proc.wait(timeout=20)
    except subprocess.TimeoutExpired:
        ctx.log("debug driver still running 20 s after umount, SIGKILL")
        proc.kill()
        proc.wait()
    kill_driver(ctx, vol.img)  # the forked driver, if umount did not end it
    logf.close()
    vol._dbg = None


def unmount(ctx, vol):
    try:
        vol.unmount()
    except Exception as e:
        ctx.log("unmount failed (%s: %s); forcing" % (type(e).__name__, e))
        if ctx.env == "B":
            ctx.transport.run("/sbin/umount -f %s" % shlex.quote(vol.mnt))
        else:
            ctx.transport.run("umount -l %s" % shlex.quote(vol.mnt))
        vol.mounted = False
        time.sleep(1)
        kill_driver(ctx, vol.img)
        raise
    finally:
        _reap_debug(ctx, vol)


def fsck(ctx, vol):
    """ntfsfix -n (check only, never repairs) on the unmounted image; exit 0 required."""
    tool = hv.guest_tool("ntfsfix") if ctx.env == "B" else "ntfsfix"
    if not tool:
        ctx.log("ntfsfix not found, consistency check skipped")
        return
    r = ctx.sh("%s -n %s" % (shlex.quote(tool), shlex.quote(vol.img)), check=False)
    if r.rc != 0:
        raise Fail("ntfsfix -n exited %d: %s" % (r.rc, (r.out + r.err).decode("utf-8", "replace")[-300:]))


def remount_check(ctx, vol, files, dirs, label="", nfc=False, extra=""):
    """Unmount, check the image with ntfsfix -n, remount read-only, hash every file, compare with
    `files` ({rel: (size, sha256)}) and `dirs` (iterable of rel paths). Leaves the volume unmounted.
    nfc=True compares names in NFC (Environment B lists non-ASCII names in NFD, U22)."""
    if vol.mounted:
        unmount(ctx, vol)
    fsck(ctx, vol)
    mount(ctx, vol, rw=False, extra=extra)
    try:
        actual = hash_tree(vol.mnt)
        adirs = list_dirs(vol.mnt)
    finally:
        unmount(ctx, vol)
    if nfc:
        actual = nfc_keys(actual)
        adirs = sorted(unicodedata.normalize("NFC", d) for d in adirs)
        files = nfc_keys(files)
        dirs = [unicodedata.normalize("NFC", d) for d in dirs]
    bad = compare(files, actual)
    want = sorted(set(dirs))
    bad += ["%s: directory missing" % d for d in sorted(set(want) - set(adirs))]
    bad += ["%s: unexpected extra directory" % d for d in sorted(set(adirs) - set(want))]
    ctx.log("remount check%s: %d files, %d directories compared, %d problems" % (
        " (%s)" % label if label else "", len(files), len(want), len(bad)))
    if bad:
        for b in bad[:20]:
            ctx.log("MISMATCH " + b)
        raise Fail("%s: %d problems after remount, first: %s" % (label or "remount check", len(bad), bad[0]))
    return actual


def model_files(model):
    """{rel: bytes} -> {rel: (size, sha256)}."""
    return dict((k, (len(v), sha(v))) for k, v in model.items())


def open_volume(ctx, size_mib):
    return ctx.volume(size_mib * MIB)


def free_bytes(path):
    st = os.statvfs(path)
    return st.f_bavail * st.f_frsize
