# SPDX-License-Identifier: GPL-2.0-or-later
"""I-XATTR-01..08 (T-M3-08, IMPLEMENTATION_PLAN 3.7): extended attributes and named streams on an NTFS
volume mounted by NTFS-3G on the macFUSE FSKit backend, with streams_interface=openxattr.

  01  set/get/list/remove on a file and a directory, flags, errnos, name length, xattr(1)
  02  value sizes 1 B to 64 KiB, the largest value that works, many xattrs, persistence across remount
  03  com.apple.FinderInfo: the 32-byte rule, colour label, the all-zero value (T-M2-04 "write ok, read
      back fails") with the worker's debug trace as evidence
  04  com.apple.quarantine, com.apple.ResourceFork (xattr and the ..namedfork/rsrc path), other Apple names
  05  windows_names and xattr names with ':' (Finder tags live in com.apple.metadata:_kMDItemUserTags)
  06  ditto, cp, cp -p, cp -c, copyfile(3) COPYFILE_ALL: APFS to NTFS and back, whether ._* files appear
  07  xattrs across data writes, truncate, rename, hard link, unlink, read-only mount
  08  the same on streams_interface=xattr, for comparison (the value the design had before T-M2-04)
  A1  Environment A reference: set/get/list/remove and sizes through Linux FUSE (user.* names)

Mount options in B are the product's (backend=fskit,local,norecover,uid,gid from fsops) plus
xattr_ops.PRODUCT = windows_names,noatime,streams_interface=openxattr unless a case says otherwise.
The stream is visible as an NTFS named stream in a Linux tree: I-XATTR-09 (a_after_b.py).
Cases run in Environment B (the guest), images are created by the harness in the case's work directory.
"""

import errno
import os
import plistlib
import re
import sys
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(HERE), "functional"))
sys.path.insert(0, HERE)

import fsops  # noqa: E402
import xattr_ops as xo  # noqa: E402
from harness import Fail, case  # noqa: E402

KIB = 1024
QUAR, FI_RED, FI_ZERO = xo.QUAR, xo.FI_RED, xo.FI_ZERO
TAGS = plistlib.dumps(["Red\n6", "Work"], fmt=plistlib.FMT_BINARY)
COMMENT = plistlib.dumps("a Finder comment", fmt=plistlib.FMT_BINARY)
XATTR_BIN = "/usr/bin/xattr"


class Checks(object):
    """Collects every failed check of a case, so one run shows all differences."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.bad = []

    def ok(self, cond, msg):
        self.ctx.log("%s %s" % ("ok  " if cond else "FAIL", msg))
        if not cond:
            self.bad.append(msg)
        return bool(cond)

    def eq(self, got, want, msg):
        good = got == want
        self.ctx.log("%s %s%s" % ("ok  " if good else "FAIL", msg, "" if good else ": got %s, want %s" % (
            _show(got), _show(want))))
        if not good:
            self.bad.append("%s: got %s, want %s" % (msg, _show(got), _show(want)))
        return good

    def err(self, got, want, msg):
        """got is an errno from xattr_ops (int); want an errno number or a collection of them."""
        wants = set(want) if isinstance(want, (set, frozenset, list, tuple)) else set([want])
        good = isinstance(got, int) and got in wants
        self.ctx.log("%s %s -> %s%s" % ("ok  " if good else "FAIL", msg, xo.ename(got),
                                         "" if good else ", want %s" % sorted(xo.ename(w) for w in wants)))
        if not good:
            self.bad.append("%s: %s, want %s" % (msg, xo.ename(got) if isinstance(got, int) else "a value",
                                                 sorted(xo.ename(w) for w in wants)))
        return good

    def done(self):
        if self.bad:
            raise Fail("%d checks failed, first: %s" % (len(self.bad), self.bad[0]))


def _show(v):
    if isinstance(v, bytes):
        return "bytes(len %d, sha %s)" % (len(v), xo.sha(v)[:8])
    if isinstance(v, dict):
        return str(xo.short(v))
    return repr(v)


# ---- volume helpers ----

def _open(ctx, mib=256):
    if ctx.env != "B":
        ctx.skip("this case runs in Environment B")
    return fsops.open_volume(ctx, mib)


def _remount(ctx, vol, extra, rw=True, fsck=True):
    if vol.mounted:
        fsops.unmount(ctx, vol)
    if fsck:
        fsops.fsck(ctx, vol)
    fsops.mount(ctx, vol, rw=rw, extra=extra)


def _write(path, data=b"data"):
    with open(path, "wb") as f:
        f.write(data)


def _set_all(chk, path, attrs, what=""):
    for n, v in attrs.items():
        chk.err(xo.setx(path, n, v), 0, "set %s on %s%s" % (n, os.path.basename(path), what))


def _map_check(chk, path, want, label):
    got = xo.xmap(path)
    chk.eq(got, want, "%s: xattr map" % label)


def _nfc(s):
    return unicodedata.normalize("NFC", s)


def _bad_names(root):
    return xo.appledouble_files(root)


# ---- I-XATTR-01 ----

@case("I-XATTR-01", envs=("B",), title="xattr set/get/list/remove, flags, errnos, name length, xattr(1); file and directory")
def i_xattr_01(ctx):
    vol = _open(ctx)
    fsops.mount(ctx, vol, extra=xo.PRODUCT)
    m = vol.mnt
    chk = Checks(ctx)
    f, d = os.path.join(m, "f.txt"), os.path.join(m, "dir")
    _write(f)
    os.mkdir(d)
    attrs = {
        "user.a": b"1",
        "com.test.x": b"hello world",
        "com.apple.quarantine": QUAR,
        "naïve.é": "café".encode("utf-8"),
        "user.bin": bytes(range(256)),
        "user.empty": b"",
    }
    for target in (f, d):
        t = os.path.basename(target)
        chk.eq(xo.listx(target), [], "%s: new object has no xattrs" % t)
        _set_all(chk, target, attrs, " (%s)" % t)
        listed = xo.listx(target)
        chk.eq(sorted(_nfc(n) for n in listed), sorted(_nfc(n) for n in attrs), "%s: list equals the names set (NFC compared)" % t)
        ctx.log("%s: non-ASCII name as set %s, as listed %s" % (t, [n.encode().hex() for n in attrs if ord(n[-1]) > 127],
                                                                 [n.encode().hex() for n in listed if ord(n[-1]) > 127]))
        for n, v in attrs.items():
            chk.eq(xo.getx(target, n), v, "%s: get %s" % (t, n))
        # flags
        chk.err(xo.setx(target, "user.a", b"2", xo.XATTR_CREATE), errno.EEXIST, "%s: XATTR_CREATE over existing" % t)
        chk.err(xo.setx(target, "user.never", b"2", xo.XATTR_REPLACE), xo.ENOATTR, "%s: XATTR_REPLACE of missing" % t)
        chk.err(xo.setx(target, "user.fresh", b"f", xo.XATTR_CREATE), 0, "%s: XATTR_CREATE of new" % t)
        chk.err(xo.setx(target, "user.fresh", b"gg", xo.XATTR_REPLACE), 0, "%s: XATTR_REPLACE of existing" % t)
        chk.eq(xo.getx(target, "user.fresh"), b"gg", "%s: replaced value" % t)
        chk.err(xo.removex(target, "user.fresh"), 0, "%s: remove user.fresh" % t)
        # replace longer, then shorter
        chk.err(xo.setx(target, "user.a", b"x" * 5000), 0, "%s: replace 1 B by 5000 B" % t)
        chk.eq(xo.getx(target, "user.a"), b"x" * 5000, "%s: value after growing" % t)
        chk.err(xo.setx(target, "user.a", b"1"), 0, "%s: replace 5000 B by 1 B" % t)
        chk.eq(xo.getx(target, "user.a"), b"1", "%s: value after shrinking (no stale tail)" % t)
        # errnos
        chk.err(xo.getx(target, "user.missing"), xo.ENOATTR, "%s: get missing" % t)
        chk.err(xo.removex(target, "user.missing"), xo.ENOATTR, "%s: remove missing" % t)
        # name length: 127 bytes works, 128 is ENAMETOOLONG (macOS XATTR_MAXNAMELEN)
        chk.err(xo.setx(target, "x" * 127, b"v"), 0, "%s: 127-byte name" % t)
        chk.err(xo.setx(target, "x" * 128, b"v"), errno.ENAMETOOLONG, "%s: 128-byte name" % t)
        chk.err(xo.removex(target, "x" * 127), 0, "%s: remove 127-byte name" % t)
        # remove one by one
        for n in ("user.bin", "user.empty", "com.test.x"):
            chk.err(xo.removex(target, n), 0, "%s: remove %s" % (t, n))
            chk.err(xo.getx(target, n), xo.ENOATTR, "%s: get %s after remove" % (t, n))
        left = dict((k, v) for k, v in attrs.items() if k not in ("user.bin", "user.empty", "com.test.x"))
        left["user.a"] = b"1"
        _map_check(chk, target, left, "%s after removes" % t)
    # xattr(1)
    rc, out = xo.run([XATTR_BIN, "-w", "user.cli", "cli-value", f])
    chk.eq(rc, 0, "xattr -w rc (%s)" % out)
    rc, out = xo.run([XATTR_BIN, "-p", "user.cli", f])
    chk.eq((rc, out), (0, "cli-value"), "xattr -p")
    rc, out = xo.run([XATTR_BIN, f])
    chk.ok(rc == 0 and "user.cli" in out.split("\n"), "xattr (list) shows user.cli: rc %d %r" % (rc, out))
    rc, out = xo.run([XATTR_BIN, "-d", "user.cli", f])
    chk.eq(rc, 0, "xattr -d rc (%s)" % out)
    rc, out = xo.run([XATTR_BIN, "-p", "user.cli", f])
    chk.ok(rc != 0 and "No such xattr" in out, "xattr -p after -d: rc %d %r" % (rc, out))
    rc, out = xo.run([XATTR_BIN, "-c", d])
    chk.eq(rc, 0, "xattr -c rc (%s)" % out)
    chk.eq(xo.listx(d), [], "directory has no xattrs after xattr -c")
    # persistence
    want_f = dict(left)
    _remount(ctx, vol, xo.PRODUCT, rw=False)
    try:
        _map_check(chk, f, want_f, "file after remount")
        chk.eq(xo.listx(d), [], "directory after remount")
        chk.eq(_bad_names(m), [], "no ._* files")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    chk.done()


# ---- I-XATTR-02 ----

SIZES = [1, 2, 3, 16, 255, 256, 1000, 1024, 4095, 4096, 4097, 8191, 8192, 16 * KIB, 32767, 32768, 65535, 65536]


def _find_max(chk, ctx, path, name):
    """Largest value size that sets and reads back. Doubles from 64 KiB, then bisects. Returns (max, errno_above)."""
    lo, hi, e_hi = 64 * KIB, None, 0
    n = 128 * KIB
    while n <= 8 * 1024 * KIB:
        v = xo.blob("max:%d" % n, n)
        e = xo.setx(path, name, v)
        if e == 0 and xo.getx(path, name) == v:
            lo = n
            n *= 2
        else:
            hi, e_hi = n, e
            break
    if hi is None:
        return lo, 0
    while hi - lo > 1:
        mid = (lo + hi) // 2
        v = xo.blob("max:%d" % mid, mid)
        e = xo.setx(path, name, v)
        if e == 0 and xo.getx(path, name) == v:
            lo = mid
        else:
            hi, e_hi = mid, e
    return lo, e_hi


@case("I-XATTR-02", envs=("B",), title="xattr values 1 B to 64 KiB, largest value that works, many xattrs, persistence")
def i_xattr_02(ctx):
    vol = _open(ctx, 512)
    fsops.mount(ctx, vol, extra=xo.PRODUCT)
    m = vol.mnt
    chk = Checks(ctx)
    f = os.path.join(m, "sizes.bin")
    body = xo.blob("xattr02:body", 100000)
    _write(f, body)
    want = {}
    for n in SIZES:
        v = xo.blob("xattr02:%d" % n, n)
        name = "user.s%05d" % n
        e = xo.setx(f, name, v)
        chk.err(e, 0, "set %d B" % n)
        got = xo.getx(f, name)
        chk.eq(got, v, "get %d B" % n)
        want[name] = v
    # one 64 KiB value of every kind of byte, replaced by a short one (no stale tail)
    chk.err(xo.setx(f, "user.s65536", b"short"), 0, "replace 64 KiB by 5 B")
    chk.eq(xo.getx(f, "user.s65536"), b"short", "value after shrinking from 64 KiB")
    want["user.s65536"] = b"short"
    v = xo.blob("xattr02:again", 65536)
    chk.err(xo.setx(f, "user.s65536", v), 0, "replace 5 B by 64 KiB")
    want["user.s65536"] = v
    # the maximum
    mx, e_above = _find_max(chk, ctx, f, "user.probe")
    ctx.log("MAX xattr value size that works on this mount: %d bytes (%.1f KiB); next size fails with %s" % (
        mx, mx / 1024.0, xo.ename(e_above)))
    with open(ctx.evidence_file("max-size.txt"), "w") as out:
        out.write("max_value_bytes=%d\nerrno_above=%s\n" % (mx, xo.ename(e_above)))
    chk.ok(mx >= 64 * KIB, "the maximum (%d) is at least 64 KiB" % mx)
    mv = xo.blob("max:%d" % mx, mx)
    chk.err(xo.setx(f, "user.probe", mv), 0, "set the maximum value again")
    big = xo.blob("xattr02:over", mx + 1)
    chk.err(xo.setx(f, "user.probe", big), e_above or errno.EFBIG, "set max+1 fails the same way")
    chk.eq(xo.getx(f, "user.probe"), mv, "a failed replace leaves the old value intact")
    want["user.probe"] = mv
    # file data is untouched by all of it
    chk.eq(open(f, "rb").read(), body, "file data after the xattr writes")
    # many xattrs on one file, and several large ones
    g = os.path.join(m, "many.bin")
    _write(g, b"many")
    many = {}
    for i in range(200):
        many["user.n%03d" % i] = xo.blob("xattr02:many:%d" % i, 100 + i)
    for n, v in many.items():
        e = xo.setx(g, n, v)
        if e != 0:
            chk.err(e, 0, "set %s (many)" % n)
            break
    chk.eq(xo.xmap(g), many, "200 xattrs: map")
    h = os.path.join(m, "large4.bin")
    _write(h, b"large")
    large = dict(("user.L%d" % i, xo.blob("xattr02:L%d" % i, 64 * KIB + i)) for i in range(4))
    _set_all(chk, h, large)
    chk.eq(xo.xmap(h), large, "4 xattrs of 64 KiB: map")
    # directory with a large value
    d = os.path.join(m, "dir")
    os.mkdir(d)
    dv = xo.blob("xattr02:dir", 64 * KIB)
    chk.err(xo.setx(d, "user.d64", dv), 0, "64 KiB on a directory")
    # remount read-only: everything persists, image is consistent
    _remount(ctx, vol, xo.PRODUCT, rw=False)
    try:
        chk.eq(xo.xmap(f), want, "sizes.bin after remount")
        chk.eq(open(f, "rb").read(), body, "sizes.bin data after remount")
        chk.eq(xo.xmap(g), many, "many.bin after remount")
        chk.eq(xo.xmap(h), large, "large4.bin after remount")
        chk.eq(xo.getx(d, "user.d64"), dv, "dir 64 KiB after remount")
        chk.eq(_bad_names(m), [], "no ._* files")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    chk.done()


# ---- I-XATTR-03 ----

def _debug_getxattr_replies(raw, name_suffix, xname):
    """From the worker's -o debug log: [(requested size, reply text)] for getxattr of xname on a path
    ending in name_suffix. A request line is 'getxattr /path name size position'; the reply is the next
    line that starts with 'unique: N, success' or 'unique: N, error'."""
    text = raw.decode("utf-8", "replace").split("\n")
    out = []
    for i, line in enumerate(text):
        mt = re.match(r"^getxattr (\S*%s) %s (\d+) (\d+)" % (re.escape(name_suffix), re.escape(xname)), line)
        if not mt:
            continue
        reply = ""
        for nxt in text[i + 1:i + 6]:
            if re.match(r"^\s+unique: \d+, (success|error)", nxt):
                reply = nxt.strip()
                break
        out.append((int(mt.group(2)), reply))
    return out


@case("I-XATTR-03", envs=("B",), title="com.apple.FinderInfo: 32-byte rule, colour label, all-zero value, worker trace")
def i_xattr_03(ctx):
    vol = _open(ctx)
    fsops.mount(ctx, vol, extra=xo.PRODUCT)
    m = vol.mnt
    chk = Checks(ctx)
    FI = "com.apple.FinderInfo"
    f, d = os.path.join(m, "colour.txt"), os.path.join(m, "colour.dir")
    _write(f)
    os.mkdir(d)
    for target in (f, d):
        t = os.path.basename(target)
        chk.err(xo.setx(target, FI, FI_RED), 0, "%s: set 32-byte FinderInfo (colour label 6)" % t)
        chk.eq(xo.getx(target, FI), FI_RED, "%s: get FinderInfo reads the 32 bytes back" % t)
        chk.ok(FI in (xo.listx(target) or []), "%s: FinderInfo is listed" % t)
        # the label is bits 1-3 of byte 9
        got = xo.getx(target, FI)
        chk.eq((got[9] >> 1) & 7 if isinstance(got, bytes) else None, 6, "%s: colour label bits" % t)
        # the 32-byte rule: any other size is ERANGE, the old value stays
        for n in (1, 16, 31, 33, 40, 64, 4096):
            chk.err(xo.setx(target, FI, b"\x01" * n), errno.ERANGE, "%s: set %d-byte FinderInfo" % (t, n))
        e0 = xo.setx(target, FI, b"")
        ctx.log("%s: set empty FinderInfo -> %s (recorded, not asserted)" % (t, xo.ename(e0)))
        if e0 != 0:
            chk.eq(xo.getx(target, FI), FI_RED, "%s: value after the refused sizes" % t)
        else:
            chk.err(xo.setx(target, FI, FI_RED), 0, "%s: set the colour again" % t)
        # CLI round trip, nonzero
        rc, out = xo.run([XATTR_BIN, "-wx", FI, FI_RED.hex(), target])
        chk.eq(rc, 0, "%s: xattr -wx FinderInfo rc (%s)" % (t, out))
        rc, out = xo.run([XATTR_BIN, "-px", FI, target])
        chk.eq(bytes.fromhex(out.replace("\n", "").replace(" ", "")) if rc == 0 else None, FI_RED,
               "%s: xattr -px FinderInfo" % t)
    # The all-zero value (what T-M2-04 wrote): the set succeeds, the read back is ENOATTR, the name is listed.
    z = os.path.join(m, "zero.txt")
    _write(z)
    chk.err(xo.setx(z, FI, FI_ZERO), 0, "set all-zero FinderInfo")
    chk.err(xo.getx(z, FI), xo.ENOATTR, "get all-zero FinderInfo")
    chk.ok(FI in (xo.listx(z) or []), "all-zero FinderInfo is still listed")
    rc, out = xo.run([XATTR_BIN, "-wx", FI, FI_ZERO.hex(), z])
    rc2, out2 = xo.run([XATTR_BIN, "-l", z])
    chk.ok(rc == 0 and rc2 != 0 and "No such xattr: com.apple.FinderInfo" in out2,
           "T-M2-04 probe reproduced: xattr -wx zeros rc %d, xattr -l rc %d %r" % (rc, rc2, out2))
    # zero over a colour clears it (the way HFS+ and APFS treat an all-zero FinderInfo)
    chk.err(xo.setx(f, FI, FI_ZERO), 0, "overwrite a colour label by all-zero")
    chk.err(xo.getx(f, FI), xo.ENOATTR, "get after overwriting the colour by zeros")
    chk.err(xo.setx(f, FI, FI_RED), 0, "set the colour again")
    chk.eq(xo.getx(f, FI), FI_RED, "colour label back")
    chk.err(xo.removex(z, FI), 0, "remove an all-zero FinderInfo")
    chk.ok(FI not in (xo.listx(z) or []), "FinderInfo gone from the list after remove")
    # a nonzero value that differs from zero in one bit is returned
    one = os.path.join(m, "onebit.txt")
    _write(one)
    v1 = bytes(31) + b"\x01"
    chk.err(xo.setx(one, FI, v1), 0, "set FinderInfo with one bit")
    chk.eq(xo.getx(one, FI), v1, "one-bit FinderInfo is read back")
    # persistence
    _remount(ctx, vol, xo.PRODUCT, rw=False)
    try:
        chk.eq(xo.getx(f, FI), FI_RED, "file colour after remount")
        chk.eq(xo.getx(d, FI), FI_RED, "directory colour after remount")
        chk.eq(xo.getx(one, FI), v1, "one-bit value after remount")
        chk.err(xo.getx(z, FI), xo.ENOATTR, "removed FinderInfo after remount")
    finally:
        fsops.unmount(ctx, vol)
    # Where does the ENOATTR come from? Mount with the worker's debug trace and read an all-zero FinderInfo.
    raw = ctx.evidence_file("worker-debug.log")
    fsops.mount_debug(ctx, vol, raw, rw=True, extra=xo.PRODUCT)
    try:
        zz = os.path.join(vol.mnt, "trace-zero.txt")
        nz = os.path.join(vol.mnt, "trace-nonzero.txt")
        _write(zz)
        _write(nz)
        chk.err(xo.setx(zz, FI, FI_ZERO), 0, "trace: set all-zero FinderInfo")
        chk.err(xo.setx(nz, FI, FI_RED), 0, "trace: set nonzero FinderInfo")
        chk.err(xo.getx(zz, FI), xo.ENOATTR, "trace: get all-zero FinderInfo (the application sees ENOATTR)")
        chk.eq(xo.getx(nz, FI), FI_RED, "trace: get nonzero FinderInfo")
    finally:
        fsops.unmount(ctx, vol)
    with open(raw, "rb") as fh:
        rawdata = fh.read()
    rz = _debug_getxattr_replies(rawdata, "/trace-zero.txt", FI)
    rn = _debug_getxattr_replies(rawdata, "/trace-nonzero.txt", FI)
    ctx.log("worker replies, all-zero: %s" % rz)
    ctx.log("worker replies, nonzero:  %s" % rn)
    chk.ok(any(sz == 32 and "success" in rep and "outsize: 48" in rep for sz, rep in rz),
           "the worker answered the all-zero read with success and 32 data bytes (outsize 48)")
    chk.ok(any(sz == 32 and "success" in rep and "outsize: 48" in rep for sz, rep in rn),
           "the worker answered the nonzero read the same way")
    chk.done()


# ---- I-XATTR-04 ----

@case("I-XATTR-04", envs=("B",), title="com.apple.quarantine, com.apple.ResourceFork (xattr and ..namedfork/rsrc), other Apple names")
def i_xattr_04(ctx):
    vol = _open(ctx)
    fsops.mount(ctx, vol, extra=xo.PRODUCT)
    m = vol.mnt
    chk = Checks(ctx)
    f = os.path.join(m, "dl.bin")
    _write(f, xo.blob("xattr04:data", 3000))
    # quarantine
    chk.err(xo.setx(f, "com.apple.quarantine", QUAR), 0, "set com.apple.quarantine")
    chk.eq(xo.getx(f, "com.apple.quarantine"), QUAR, "get com.apple.quarantine")
    rc, out = xo.run([XATTR_BIN, "-p", "com.apple.quarantine", f])
    chk.eq((rc, out), (0, QUAR.decode()), "xattr -p quarantine")
    # other Apple names without ':'
    apple = {"com.apple.lastuseddate#PS": b"\x00" * 16, "com.apple.TextEncoding": b"utf-8;134217984"}
    _set_all(chk, f, apple)
    # resource fork through the xattr API
    rf = xo.blob("xattr04:rsrc", 70 * KIB)
    chk.err(xo.setx(f, "com.apple.ResourceFork", rf), 0, "set com.apple.ResourceFork (70 KiB)")
    chk.eq(xo.getx(f, "com.apple.ResourceFork"), rf, "get com.apple.ResourceFork")
    fork = f + "/..namedfork/rsrc"
    try:
        data = open(fork, "rb").read()
        chk.eq(data, rf, "..namedfork/rsrc reads what the xattr API stored")
        chk.eq(os.stat(fork).st_size, len(rf), "stat of ..namedfork/rsrc is the fork size")
    except OSError as e:
        chk.ok(False, "read ..namedfork/rsrc: %s" % e)
    # resource fork through the path
    g = os.path.join(m, "fork.bin")
    _write(g, b"data")
    rg = xo.blob("xattr04:rsrc2", 5000)
    try:
        with open(g + "/..namedfork/rsrc", "wb") as fh:
            fh.write(rg)
        chk.eq(xo.getx(g, "com.apple.ResourceFork"), rg, "xattr API reads what ..namedfork/rsrc wrote")
        chk.ok("com.apple.ResourceFork" in xo.listx(g), "ResourceFork is listed after a write through the path")
    except OSError as e:
        chk.ok(False, "write ..namedfork/rsrc: %s" % e)
    chk.eq(open(g, "rb").read(), b"data", "data fork untouched by the resource fork")
    # remove
    chk.err(xo.removex(f, "com.apple.quarantine"), 0, "remove quarantine")
    chk.err(xo.getx(f, "com.apple.quarantine"), xo.ENOATTR, "get quarantine after remove")
    chk.err(xo.removex(g, "com.apple.ResourceFork"), 0, "remove ResourceFork")
    try:
        left = open(g + "/..namedfork/rsrc", "rb").read()
        ctx.log("..namedfork/rsrc after removing the xattr: %d bytes (recorded)" % len(left))
    except OSError as e:
        ctx.log("..namedfork/rsrc after removing the xattr: errno %s (recorded)" % xo.ename(e.errno))
    chk.err(xo.setx(f, "com.apple.quarantine", QUAR), 0, "set quarantine again")
    want_f = dict(apple)
    want_f["com.apple.quarantine"] = QUAR
    want_f["com.apple.ResourceFork"] = rf
    chk.eq(xo.xmap(f), want_f, "dl.bin: all names before remount")
    _remount(ctx, vol, xo.PRODUCT, rw=False)
    try:
        chk.eq(xo.xmap(f), want_f, "dl.bin after remount")
        chk.eq(open(f + "/..namedfork/rsrc", "rb").read(), rf, "resource fork after remount, through the path")
        chk.eq(_bad_names(m), [], "no ._* files")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    # copyfile(3) reads a resource fork in pieces at a non-zero position (cp -p, Finder). Does the worker see it?
    raw = ctx.evidence_file("worker-debug.log")
    fsops.mount_debug(ctx, vol, raw, rw=True, extra=xo.PRODUCT)
    try:
        p = os.path.join(vol.mnt, "pos.bin")
        _write(p, b"d")
        chk.err(xo.setx(p, "com.apple.ResourceFork", b"r" * 100), 0, "trace: set a 100-byte resource fork")
        chk.eq(xo.getx_pos(p, "com.apple.ResourceFork", 100, 0), b"r" * 100, "trace: read at position 0")
        chk.err(xo.getx_pos(p, "com.apple.ResourceFork", 50, 50), errno.EINVAL, "trace: read at position 50")
        chk.err(xo.getx_pos(p, "com.apple.ResourceFork", 100, 100), errno.EINVAL, "trace: read at position 100 (end-of-fork probe)")
    finally:
        fsops.unmount(ctx, vol)
    with open(raw, "rb") as fh:
        text = fh.read().decode("utf-8", "replace")
    reqs = re.findall(r"^getxattr \S*pos\.bin com\.apple\.ResourceFork (\d+) (\d+)", text, re.M)
    ctx.log("worker getxattr requests for the resource fork (size, position): %s" % reqs)
    chk.ok(reqs and all(pos == "0" for _sz, pos in reqs),
           "the worker only ever sees position 0: the non-zero reads are refused before the request reaches it")
    chk.done()


# ---- I-XATTR-05 ----

REJECTED = ["com.apple.metadata:_kMDItemUserTags", "com.apple.metadata:kMDItemFinderComment", "a:b", "a/b", "a*b",
            'a"b', "a<b", "a|b", "a?b", "a\\b", "trail.", "sp ", "\x01c"]
ACCEPTED = ["con", "com.apple.lastuseddate#PS", "naïve", "a b", "x.y.z"]


@case("I-XATTR-05", envs=("B",), title="windows_names vs xattr names with ':' (Finder tags, comments); with and without the option")
def i_xattr_05(ctx):
    vol = _open(ctx)
    chk = Checks(ctx)
    tag_name = "com.apple.metadata:_kMDItemUserTags"
    # Phase 1: product options, windows_names on.
    fsops.mount(ctx, vol, extra=xo.PRODUCT)
    m = vol.mnt
    f = os.path.join(m, "tagged.txt")
    _write(f)
    for n in REJECTED:
        chk.err(xo.setx(f, n, TAGS), errno.EINVAL, "windows_names: set %r" % n)
    for n in ACCEPTED:
        chk.err(xo.setx(f, n, b"v"), 0, "windows_names: set %r" % n)
    chk.err(xo.setx(f, "com.apple.FinderInfo", FI_RED), 0, "windows_names: the colour label (FinderInfo) works")
    rc, out = xo.run([XATTR_BIN, "-wx", tag_name, TAGS.hex(), f])
    chk.ok(rc != 0, "xattr -wx of the tags name fails under windows_names: rc %d %r" % (rc, out))
    accepted_map = xo.xmap(f)
    chk.eq(sorted(accepted_map), sorted(ACCEPTED + ["com.apple.FinderInfo"]), "only the accepted names are stored")
    # Phase 2: without windows_names.
    _remount(ctx, vol, xo.OPEN)
    g = os.path.join(m, "tagged2.txt")
    _write(g)
    d = os.path.join(m, "tagdir")
    os.mkdir(d)
    for target in (g, d):
        t = os.path.basename(target)
        chk.err(xo.setx(target, tag_name, TAGS), 0, "no windows_names: set tags on %s" % t)
        chk.eq(xo.getx(target, tag_name), TAGS, "no windows_names: tags plist read back on %s" % t)
    for n in REJECTED:
        if n != tag_name:
            chk.err(xo.setx(g, n, b"v-" + n.encode()[:8]), 0, "no windows_names: set %r" % n)
    want_g = xo.xmap(g)
    # Phase 3: windows_names on again over a volume that already holds ':' streams.
    _remount(ctx, vol, xo.PRODUCT)
    after = xo.xmap(g)
    chk.eq(after, want_g, "windows_names remount: existing ':' streams still listed and readable")
    e_rep = xo.setx(g, tag_name, TAGS + b"x")
    ctx.log("windows_names over an existing ':' stream: replace -> %s (recorded)" % xo.ename(e_rep))
    e_rm = xo.removex(g, tag_name)
    ctx.log("windows_names over an existing ':' stream: remove -> %s (recorded)" % xo.ename(e_rm))
    chk.eq(xo.getx(d, tag_name), TAGS, "tags on the directory after the windows_names remount")
    chk.eq(_bad_names(m), [], "no ._* files")
    fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    chk.done()


# ---- I-XATTR-06 ----

S1 = {
    "user.k": b"v1",
    "com.apple.quarantine": QUAR,
    "com.apple.FinderInfo": FI_RED,
    "com.apple.ResourceFork": None,  # filled below (70 KiB)
    "com.apple.lastuseddate#PS": b"\x01" * 16,
    "com.apple.TextEncoding": b"utf-8;134217984",
}
S2_EXTRA = {"com.apple.metadata:_kMDItemUserTags": TAGS, "com.apple.metadata:kMDItemFinderComment": COMMENT}
DIR_ATTRS = {"com.apple.FinderInfo": bytes([0] * 9 + [0x0A] + [0] * 21 + [0xFF]), "user.dk": b"dir-value"}


def _s1():
    s = dict(S1)
    s["com.apple.ResourceFork"] = xo.blob("xattr06:rsrc", 70 * KIB)
    return s


TOOLS = [
    ("ditto", ["/usr/bin/ditto"]),
    ("cp-p", ["/bin/cp", "-p"]),
    ("cp-c", ["/bin/cp", "-c"]),
    ("cp-pc", ["/bin/cp", "-pc"]),
    ("copyfile-ALL", None),
]


def _copy(tool, src, dst):
    argv = tool[1]
    if argv is None:
        e = xo.copyfile_all(src, dst)
        return (0 if e == 0 else 1), ("copyfile(COPYFILE_ALL) errno %s" % xo.ename(e)) if e else ""
    return xo.run(argv + [src, dst])


def _map_diff(got, want):
    """Names whose value differs, names missing, names extra."""
    miss = sorted(n for n in want if n not in got)
    extra = sorted(n for n in got if n not in want)
    diff = sorted(n for n in want if n in got and got[n] != want[n])
    return miss, extra, diff


@case("I-XATTR-06", envs=("B",), title="ditto, cp -p, cp -c, copyfile ALL: APFS to NTFS and back, ._ files, windows_names and ':' names")
def i_xattr_06(ctx):
    vol = _open(ctx)
    chk = Checks(ctx)
    src = os.path.join(ctx.work, "src")
    back = os.path.join(ctx.work, "back")
    os.makedirs(src)
    os.makedirs(back)
    body = xo.blob("xattr06:body", 5000)
    s1 = _s1()
    s2 = dict(s1)
    s2.update(S2_EXTRA)
    s3 = dict(s1)
    del s3["com.apple.ResourceFork"]
    f1, f2, f3 = os.path.join(src, "f1"), os.path.join(src, "f2"), os.path.join(src, "f3")
    for p in (f1, f2, f3):
        _write(p, body)
    for p, s in ((f1, s1), (f2, s2), (f3, s3)):
        for n, v in s.items():
            e = xo.setx(p, n, v)
            if e != 0:
                raise RuntimeError("cannot set %s on the APFS source %s: %s" % (n, p, xo.ename(e)))
    d1 = os.path.join(src, "d1")
    os.mkdir(d1)
    for n, v in DIR_ATTRS.items():
        xo.setx(d1, n, v)
    fin = os.path.join(d1, "inner")
    _write(fin, b"inner data")
    for n, v in s1.items():
        xo.setx(fin, n, v)
    src_f1, src_f2, src_f3 = xo.xmap(f1), xo.xmap(f2), xo.xmap(f3)
    ctx.log("source f1 xattrs: %s" % xo.short(src_f1))
    ctx.log("source f3 xattrs (f1 without the resource fork): %s" % xo.short(src_f3))
    ctx.log("source f2 xattrs: %s" % xo.short(src_f2))
    src_d1, src_in = xo.xmap(d1), xo.xmap(fin)
    chk.ok(set(s1).issubset(src_f1) and set(s2).issubset(src_f2), "the APFS source holds every xattr")

    # ---- product options: windows_names on
    fsops.mount(ctx, vol, extra=xo.PRODUCT)
    m = vol.mnt
    t = os.path.join(m, "t")
    os.mkdir(t)
    for tool in TOOLS:
        dst = os.path.join(t, "%s.f1" % tool[0])
        rc, out = _copy(tool, f1, dst)
        chk.eq(rc, 0, "windows_names: %s f1 rc (%s)" % (tool[0], out))
        miss, extra, diff = _map_diff(xo.xmap(dst), src_f1)
        chk.ok(not (miss or extra or diff), "windows_names: %s f1 xattrs equal the source (missing %s, extra %s, differ %s)" % (
            tool[0], miss, extra, diff))
        chk.eq(open(dst, "rb").read() if os.path.exists(dst) else None, body, "windows_names: %s f1 data" % tool[0])
    for tool in TOOLS:
        dst = os.path.join(t, "%s.f3" % tool[0])
        rc, out = _copy(tool, f3, dst)
        chk.eq(rc, 0, "windows_names: %s f3 rc (%s)" % (tool[0], out))
        chk.eq(xo.xmap(dst), src_f3, "windows_names: %s f3 xattrs" % tool[0])
    # plain cp: record
    rc, out = xo.run(["/bin/cp", f1, os.path.join(t, "cp-plain.f1")])
    ctx.log("plain cp f1: rc %d %r, xattrs %s (recorded)" % (rc, out, xo.short(xo.xmap(os.path.join(t, "cp-plain.f1")))))
    rc, out = xo.run(["/bin/cp", "-X", f1, os.path.join(t, "cp-X.f1")])
    ctx.log("cp -X f1: rc %d %r, xattrs %s (recorded)" % (rc, out, xo.short(xo.xmap(os.path.join(t, "cp-X.f1")))))
    # directories
    rc, out = xo.run(["/usr/bin/ditto", d1, os.path.join(t, "d.ditto")])
    chk.eq(rc, 0, "windows_names: ditto directory rc (%s)" % out)
    rc2, out2 = xo.run(["/bin/cp", "-pR", d1, os.path.join(t, "d.cp")])
    chk.eq(rc2, 0, "windows_names: cp -pR directory rc (%s)" % out2)
    for dn in ("d.ditto", "d.cp"):
        chk.eq(xo.xmap(os.path.join(t, dn)), src_d1, "windows_names: %s directory xattrs" % dn)
        chk.eq(xo.xmap(os.path.join(t, dn, "inner")), src_in, "windows_names: %s inner file xattrs" % dn)
    # f2 carries ':' names: windows_names refuses them (EINVAL), so every copy tool that copies xattrs fails
    for tool in TOOLS:
        dst = os.path.join(t, "%s.f2" % tool[0])
        rc, out = _copy(tool, f2, dst)
        exists = os.path.exists(dst)
        got = xo.xmap(dst) if exists else {}
        miss, extra, diff = _map_diff(got, src_f2)
        ctx.log("windows_names: %s f2 -> rc %d %r; target exists %s, size %s; missing %s, extra %s, differ %s" % (
            tool[0], rc, out[:160], exists, os.path.getsize(dst) if exists else None, miss, extra, diff))
        chk.ok(rc != 0, "windows_names: %s of a file with a ':' xattr fails (rc %d)" % (tool[0], rc))
        if exists:
            chk.ok(set(miss) <= set(S2_EXTRA), "windows_names: %s f2 loses at most the ':' names (missing %s)" % (tool[0], miss))
    chk.eq(_bad_names(m), [], "windows_names: no ._* files after the copies")

    # ---- openxattr without windows_names
    _remount(ctx, vol, xo.OPEN)
    t2 = os.path.join(m, "t2")
    os.mkdir(t2)
    for tool in TOOLS:
        dst = os.path.join(t2, "%s.f2" % tool[0])
        rc, out = _copy(tool, f2, dst)
        chk.eq(rc, 0, "no windows_names: %s f2 rc (%s)" % (tool[0], out))
        miss, extra, diff = _map_diff(xo.xmap(dst), src_f2)
        chk.ok(not (miss or extra or diff), "no windows_names: %s f2 xattrs equal the source (missing %s, extra %s, differ %s)" % (
            tool[0], miss, extra, diff))
        chk.eq(open(dst, "rb").read() if os.path.exists(dst) else None, body, "no windows_names: %s f2 data" % tool[0])
    # NTFS back to APFS: every exact copy written above. A file with a resource fork: ditto works, cp -p and
    # copyfile(3) fail with EINVAL (copyfile reads the fork at a non-zero position, which macOS refuses
    # for this volume before the request reaches the worker). Without a resource fork all tools work.
    exact = ([("t/%s.f1" % tl[0], src_f1, True) for tl in TOOLS] + [("t2/%s.f2" % tl[0], src_f2, True) for tl in TOOLS] +
             [("t/%s.f3" % tl[0], src_f3, False) for tl in TOOLS])
    for rel, want, has_rf in exact:
        p = os.path.join(m, rel)
        for tool in (TOOLS[0], TOOLS[1], TOOLS[4]):
            dst = os.path.join(back, "%s.%s" % (tool[0], rel.replace("/", "_")))
            rc, out = _copy(tool, p, dst)
            if has_rf and tool[0] != "ditto":
                ctx.log("back to APFS: %s %s -> rc %d %r (a resource fork is on the source)" % (
                    tool[0], rel, rc, out.replace(ctx.work, "")[:140]))
                chk.ok(rc != 0, "back to APFS: %s of a file with a resource fork fails (rc %d)" % (tool[0], rc))
                continue
            chk.eq(rc, 0, "back to APFS: %s %s rc (%s)" % (tool[0], rel, out))
            miss, extra, diff = _map_diff(xo.xmap(dst), want)
            chk.ok(not (miss or extra or diff), "back to APFS: %s %s xattrs equal the source (missing %s, extra %s, differ %s)" % (
                tool[0], rel, miss, extra, diff))
    chk.eq(_bad_names(m), [], "no ._* files on the volume at the end")
    chk.eq(_bad_names(back), [], "no ._* files in the APFS copies")
    # persistence of one copy of each kind
    keep = {"t2/ditto.f2": src_f2, "t2/cp-p.f2": src_f2, "t/ditto.f1": src_f1, "t/copyfile-ALL.f1": src_f1}
    _remount(ctx, vol, xo.OPEN, rw=False)
    try:
        for rel, want in keep.items():
            chk.eq(xo.xmap(os.path.join(m, rel)), want, "after remount: %s" % rel)
        chk.eq(_bad_names(m), [], "no ._* files after remount")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    chk.done()


# ---- I-XATTR-07 ----

@case("I-XATTR-07", envs=("B",), title="xattrs across data writes, truncate, rename, hard link, unlink and recreate, read-only mount")
def i_xattr_07(ctx):
    vol = _open(ctx)
    fsops.mount(ctx, vol, extra=xo.PRODUCT)
    m = vol.mnt
    chk = Checks(ctx)
    attrs = {"user.a": b"alpha", "com.apple.quarantine": QUAR, "com.apple.FinderInfo": FI_RED,
             "com.apple.ResourceFork": xo.blob("xattr07:rf", 9000)}
    f = os.path.join(m, "f")
    _write(f, b"0123456789")
    _set_all(chk, f, attrs)
    # data operations
    with open(f, "ab") as fh:
        fh.write(b"appended" * 1000)
    chk.eq(xo.xmap(f), attrs, "after append")
    with open(f, "r+b") as fh:
        fh.seek(3)
        fh.write(b"XYZ")
    chk.eq(xo.xmap(f), attrs, "after overwrite in place")
    os.truncate(f, 0)
    chk.eq(xo.xmap(f), attrs, "after truncate to 0")
    os.truncate(f, 70000)
    chk.eq(xo.xmap(f), attrs, "after truncate to 70000")
    with open(f, "wb") as fh:  # O_TRUNC
        fh.write(b"new content")
    chk.eq(xo.xmap(f), attrs, "after rewrite with O_TRUNC")
    os.chmod(f, 0o600)
    os.utime(f, (1000000000, 1000000000))
    chk.eq(xo.xmap(f), attrs, "after chmod and utime")
    # rename in place and across directories
    os.mkdir(os.path.join(m, "sub"))
    g = os.path.join(m, "g")
    os.rename(f, g)
    chk.eq(xo.xmap(g), attrs, "after rename in the same directory")
    chk.err(xo.getx(f, "user.a"), errno.ENOENT, "old name is gone")
    h = os.path.join(m, "sub", "h")
    os.rename(g, h)
    chk.eq(xo.xmap(h), attrs, "after rename into a subdirectory")
    # rename over an existing file with its own xattrs: the target's stream set is replaced
    other = os.path.join(m, "other")
    _write(other)
    xo.setx(other, "user.other", b"o")
    os.rename(h, other)
    chk.eq(xo.xmap(other), attrs, "rename over an existing file: the source's xattrs, not a mix")
    # hard link: one MFT record, one set of streams (the backend may refuse link(): recorded, not asserted)
    l1 = os.path.join(m, "link2")
    try:
        os.link(other, l1)
    except OSError as e:
        ctx.log("hard link on this backend: errno %s (recorded; unrelated to xattrs)" % xo.ename(e.errno))
        l1 = None
    if l1:
        chk.eq(xo.xmap(l1), attrs, "hard link shows the same xattrs")
        chk.err(xo.setx(l1, "user.viahl", b"1"), 0, "set through the hard link")
        chk.eq(xo.getx(other, "user.viahl"), b"1", "visible through the first name")
        os.unlink(l1)
        chk.eq(xo.getx(other, "user.viahl"), b"1", "still there after removing the other name")
        chk.err(xo.removex(other, "user.viahl"), 0, "remove it again")
    # unlink and recreate under the same name
    os.unlink(other)
    _write(other)
    chk.eq(xo.listx(other), [], "a recreated file does not inherit the old xattrs")
    # a tree of files, then a read-only mount
    tree = {}
    for i in range(40):
        p = os.path.join(m, "sub", "t%02d" % i)
        _write(p, xo.blob("xattr07:%d" % i, i * 37))
        a = {"user.i": ("%d" % i).encode(), "com.apple.FinderInfo": FI_RED, "user.blob": xo.blob("xattr07:x%d" % i, 300 + i)}
        _set_all(chk, p, a)
        tree[p] = a
    _remount(ctx, vol, xo.PRODUCT, rw=False)
    try:
        for p, a in tree.items():
            if xo.xmap(p) != a:
                chk.ok(False, "tree file %s after remount" % os.path.basename(p))
        chk.ok(True, "40 files with 3 xattrs each checked after remount")
        victim = sorted(tree)[0]
        chk.err(xo.setx(victim, "user.new", b"1"), errno.EROFS, "read-only mount: set")
        chk.err(xo.removex(victim, "user.i"), errno.EROFS, "read-only mount: remove")
        chk.eq(xo.getx(victim, "user.i"), tree[victim]["user.i"], "read-only mount: get works")
        chk.eq(xo.xmap(victim), tree[victim], "read-only mount: nothing changed")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    chk.done()


# ---- I-XATTR-08 ----

@case("I-XATTR-08", envs=("B",), title="streams_interface=xattr for comparison: only user.* works, copies of macOS files fail")
def i_xattr_08(ctx):
    vol = _open(ctx)
    chk = Checks(ctx)
    src = os.path.join(ctx.work, "src")
    os.makedirs(src)
    s1 = _s1()
    f1 = os.path.join(src, "f1")
    body = xo.blob("xattr08:body", 5000)
    _write(f1, body)
    for n, v in s1.items():
        xo.setx(f1, n, v)
    src_f1 = xo.xmap(f1)
    fsops.mount(ctx, vol, extra=xo.XATTR_IF)
    m = vol.mnt
    f = os.path.join(m, "f")
    _write(f)
    chk.err(xo.setx(f, "user.k", b"v"), 0, "xattr interface: user.k set")
    chk.eq(xo.getx(f, "user.k"), b"v", "xattr interface: user.k get")
    for n in ("com.apple.quarantine", "com.apple.ResourceFork", "com.test.x", "com.apple.lastuseddate#PS"):
        chk.err(xo.setx(f, n, b"v"), xo.ENOTSUP_SET, "xattr interface: set %s" % n)
    chk.err(xo.setx(f, "com.apple.FinderInfo", FI_RED), xo.ENOTSUP_SET, "xattr interface: set FinderInfo")
    chk.eq(xo.listx(f), ["user.k"], "xattr interface: only user.k is listed")
    rows = []
    for tool in TOOLS:
        dst = os.path.join(m, "%s.f1" % tool[0])
        rc, out = _copy(tool, f1, dst)
        exists = os.path.exists(dst)
        size = os.path.getsize(dst) if exists else None
        got = xo.xmap(dst) if exists else {}
        rows.append((tool[0], rc, exists, size, sorted(got)))
        ctx.log("xattr interface: %s f1 -> rc %d %r; target exists %s, size %s, xattrs %s" % (
            tool[0], rc, out[:140], exists, size, sorted(got)))
        chk.ok(rc != 0, "xattr interface: %s of a file with Apple xattrs fails (rc %d)" % (tool[0], rc))
    chk.eq(_bad_names(m), [], "xattr interface: no ._* companion files either")
    _remount(ctx, vol, xo.XATTR_IF, rw=False)
    try:
        chk.eq(xo.getx(f, "user.k"), b"v", "xattr interface: user.k persists")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    chk.done()


# ---- I-XATTR-A1 (Environment A) ----

@case("I-XATTR-A1", envs=("A",), title="Environment A reference: user.* set/get/list/remove and sizes through Linux FUSE")
def i_xattr_a1(ctx):
    vol = fsops.open_volume(ctx, 256)
    fsops.mount(ctx, vol, extra="streams_interface=xattr")
    m = vol.mnt
    chk = Checks(ctx)
    f = os.path.join(m, "f")
    _write(f, b"data")
    chk.err(xo.setx(f, "user.a", b"1"), 0, "set user.a")
    chk.eq(xo.getx(f, "user.a"), b"1", "get user.a")
    chk.ok("user.a" in xo.listx(f), "user.a listed")
    chk.err(xo.setx(f, "user.a", b"2", xo.XATTR_CREATE), errno.EEXIST, "XATTR_CREATE over existing")
    chk.err(xo.setx(f, "user.nope", b"2", xo.XATTR_REPLACE), xo.ENOATTR, "XATTR_REPLACE of missing")
    chk.err(xo.getx(f, "user.missing"), xo.ENOATTR, "get missing")
    chk.err(xo.setx(f, "com.apple.quarantine", b"v"), xo.ENOTSUP_SET, "non-user namespace is refused by the kernel")
    want = {}
    for n in (1, 255, 4096, 32768, 65536):
        v = xo.blob("a1:%d" % n, n)
        chk.err(xo.setx(f, "user.s%d" % n, v), 0, "set %d B" % n)
        chk.eq(xo.getx(f, "user.s%d" % n), v, "get %d B" % n)
        want["user.s%d" % n] = v
    big = xo.blob("a1:over", 65537)
    e = xo.setx(f, "user.over", big)
    ctx.log("Linux: set 65537 B -> %s (the kernel limit XATTR_SIZE_MAX is 65536)" % xo.ename(e))
    chk.ok(e != 0, "Linux refuses a value above 64 KiB")
    chk.err(xo.removex(f, "user.s1"), 0, "remove")
    want.pop("user.s1")
    want["user.a"] = b"1"
    got = dict((k, v) for k, v in xo.xmap(f).items() if k.startswith("user."))
    chk.eq(got, want, "map before remount")
    fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    fsops.mount(ctx, vol, rw=False, extra="streams_interface=xattr")
    try:
        got = dict((k, v) for k, v in xo.xmap(f).items() if k.startswith("user."))
        chk.eq(got, want, "map after remount")
    finally:
        fsops.unmount(ctx, vol)
    chk.done()
