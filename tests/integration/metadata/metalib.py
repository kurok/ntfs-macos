# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers of the metadata suite (I-META-*, T-M3-06). Not a case module.

- on-disk reader: $STANDARD_INFORMATION and $FILE_NAME times and file attribute flags of an entry,
  read straight from the MFT of the unmounted image (100 ns FILETIME, no tool rounding)
- macOS views of the times: stat (ns), getattrlist ATTR_CMN_CRTIME/MODTIME/CHGTIME/ACCTIME
- setters: utimensat with UTIME_OMIT, setattrlist ATTR_CMN_CRTIME
Python 3.9, standard library only (ctypes for the libc calls).
"""

import ctypes
import ctypes.util
import os
import struct
import subprocess
import sys

from harness import Fail

EPOCH_DELTA = 11644473600          # seconds from 1601-01-01 to 1970-01-01
FT_PER_SEC = 10000000
FA_READONLY, FA_HIDDEN, FA_SYSTEM, FA_ARCHIVE = 0x1, 0x2, 0x4, 0x20
FA_DIRECTORY_FLAG = 0x10000000     # in $FILE_NAME flags, not in $STANDARD_INFORMATION
IS_MAC = sys.platform == "darwin"


# ---------------------------------------------------------------- FILETIME

def ft_to_ns(ft):
    """FILETIME -> ns since the Unix epoch (exact; ns is a multiple of 100)."""
    return (ft - EPOCH_DELTA * FT_PER_SEC) * 100


def ns_to_ft(ns):
    return ns // 100 + EPOCH_DELTA * FT_PER_SEC


def trunc100(ns):
    """What NTFS keeps of a ns timestamp (floor to 100 ns)."""
    return ns - (ns % 100)


def fmt_ns(ns):
    sec, frac = divmod(ns, 10 ** 9)
    import time
    try:
        t = time.strftime("%Y-%m-%d %H:%M:%S", time.gmtime(sec))
    except (ValueError, OverflowError, OSError):
        t = "sec=%d" % sec
    return "%s.%09d" % (t, frac)


# ---------------------------------------------------------------- raw MFT reader

class RawVolume(object):
    """Reads MFT records of an NTFS image file. Only the first extent of $MFT is addressed (records
    up to the point where the image has them contiguous), which is enough for a fresh test image."""

    def __init__(self, img):
        self.img = img
        with open(img, "rb") as f:
            boot = f.read(512)
            if boot[3:11] != b"NTFS    ":
                raise Fail("%s: no NTFS boot sector" % img)
            self.bps = struct.unpack_from("<H", boot, 0x0B)[0]
            self.spc = boot[0x0D]
            mft_lcn = struct.unpack_from("<q", boot, 0x30)[0]
            cpr = struct.unpack_from("<b", boot, 0x40)[0]
            self.cluster = self.bps * self.spc
            self.recsize = cpr * self.cluster if cpr > 0 else 1 << (-cpr)
            self.mft_off = mft_lcn * self.cluster
            f.seek(0, 2)
            self.size = f.tell()
        self._fh = open(img, "rb")

    def close(self):
        self._fh.close()

    def record(self, n):
        self._fh.seek(self.mft_off + n * self.recsize)
        rec = bytearray(self._fh.read(self.recsize))
        if len(rec) < self.recsize or rec[0:4] != b"FILE":
            return None
        usa_off, usa_n = struct.unpack_from("<HH", rec, 4)
        usa = rec[usa_off:usa_off + 2 * usa_n]
        for i in range(1, usa_n):
            end = i * 512 - 2
            rec[end:end + 2] = usa[2 * i:2 * i + 2]
        return rec

    def attrs(self, rec):
        """[(type, resident, value_bytes_or_None)] of the base record."""
        off = struct.unpack_from("<H", rec, 0x14)[0]
        out = []
        while off + 8 <= len(rec):
            typ, ln = struct.unpack_from("<II", rec, off)
            if typ == 0xFFFFFFFF or ln == 0:
                break
            nonres = rec[off + 8]
            val = None
            if not nonres:
                vlen, voff = struct.unpack_from("<IH", rec, off + 0x10)
                val = bytes(rec[off + voff:off + voff + vlen])
            out.append((typ, not nonres, val))
            off += ln
        return out

    def scan(self, limit=20000):
        """{name: (recno, si, fn)} for in-use records (last record wins on a duplicate name)."""
        out = {}
        n = 0
        empty = 0
        while n < limit and self.mft_off + (n + 1) * self.recsize <= self.size:
            rec = self.record(n)
            n += 1
            if rec is None:
                empty += 1
                if empty > 64:
                    break
                continue
            empty = 0
            if not struct.unpack_from("<H", rec, 0x16)[0] & 1:
                continue
            if struct.unpack_from("<Q", rec, 0x20)[0] != 0:    # extension record
                continue
            si = None
            fns = []
            for typ, res, val in self.attrs(rec):
                if typ == 0x10 and res and len(val) >= 36 and si is None:
                    c, m, ch, a = struct.unpack_from("<QQQQ", val, 0)
                    flags = struct.unpack_from("<I", val, 32)[0]
                    si = {"crtime": c, "mtime": m, "ctime": ch, "atime": a, "flags": flags,
                          "size": len(val)}
                if typ == 0x30 and res and len(val) >= 66:
                    nlen = val[64]
                    nspace = val[65]
                    if nspace == 2:    # DOS-only name, the Win32 one is the other attribute
                        continue
                    nm = val[66:66 + 2 * nlen].decode("utf-16-le", "replace")
                    c, m, ch, a = struct.unpack_from("<QQQQ", val, 8)
                    fns.append((nm, {"crtime": c, "mtime": m, "ctime": ch, "atime": a,
                                     "flags": struct.unpack_from("<I", val, 56)[0],
                                     "parent": struct.unpack_from("<Q", val, 0)[0] & 0xFFFFFFFFFFFF}))
            if si is not None:
                for nm, fn in fns:
                    out[nm] = (n - 1, si, fn)
        return out


def disk_entries(img):
    rv = RawVolume(img)
    try:
        return rv.scan()
    finally:
        rv.close()


def disk(ents, name):
    if name not in ents:
        raise Fail("%s not found in the MFT of the image (found %d names)" % (name, len(ents)))
    return ents[name]


def si_ns(si):
    return dict((k, ft_to_ns(si[k])) for k in ("crtime", "mtime", "ctime", "atime"))


def fmt_si(si):
    return "cr=%s mt=%s ct=%s at=%s flags=0x%x" % (
        fmt_ns(ft_to_ns(si["crtime"])), fmt_ns(ft_to_ns(si["mtime"])), fmt_ns(ft_to_ns(si["ctime"])),
        fmt_ns(ft_to_ns(si["atime"])), si["flags"])


# ---------------------------------------------------------------- macOS and Linux views

class _Attrlist(ctypes.Structure):
    _fields_ = [("bitmapcount", ctypes.c_ushort), ("reserved", ctypes.c_ushort),
                ("commonattr", ctypes.c_uint32), ("volattr", ctypes.c_uint32),
                ("dirattr", ctypes.c_uint32), ("fileattr", ctypes.c_uint32),
                ("forkattr", ctypes.c_uint32)]


ATTR_CMN_CRTIME, ATTR_CMN_MODTIME, ATTR_CMN_CHGTIME, ATTR_CMN_ACCTIME = 0x200, 0x400, 0x800, 0x1000
FSOPT_NOFOLLOW = 0x1
_libc = ctypes.CDLL(ctypes.util.find_library("c") or None, use_errno=True)


def getattrlist_times(path):
    """macOS only: {crtime, mtime, ctime, atime} in ns from getattrlist, or raises OSError."""
    al = _Attrlist(5, 0, ATTR_CMN_CRTIME | ATTR_CMN_MODTIME | ATTR_CMN_CHGTIME | ATTR_CMN_ACCTIME, 0, 0, 0, 0)
    buf = ctypes.create_string_buffer(4 + 4 * 16)
    r = _libc.getattrlist(os.fsencode(path), ctypes.byref(al), buf, ctypes.sizeof(buf), FSOPT_NOFOLLOW)
    if r != 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))
    vals = struct.unpack_from("<" + "qq" * 4, buf.raw, 4)
    names = ("crtime", "mtime", "ctime", "atime")
    return dict((n, vals[2 * i] * 10 ** 9 + vals[2 * i + 1]) for i, n in enumerate(names))


def setattrlist_crtime(path, ns):
    """macOS only: set the creation time with setattrlist(ATTR_CMN_CRTIME). Raises OSError."""
    al = _Attrlist(5, 0, ATTR_CMN_CRTIME, 0, 0, 0, 0)
    sec, frac = divmod(ns, 10 ** 9)
    buf = ctypes.create_string_buffer(struct.pack("<qq", sec, frac), 16)
    r = _libc.setattrlist(os.fsencode(path), ctypes.byref(al), buf, 16, FSOPT_NOFOLLOW)
    if r != 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))


class _Timespec(ctypes.Structure):
    _fields_ = [("tv_sec", ctypes.c_long), ("tv_nsec", ctypes.c_long)]


UTIME_NOW = -1 if IS_MAC else (1 << 30) - 1
UTIME_OMIT = -2 if IS_MAC else (1 << 30) - 2
AT_FDCWD = -2 if IS_MAC else -100
AT_SYMLINK_NOFOLLOW = 0x20 if IS_MAC else 0x100


def utimens(path, atime_ns=None, mtime_ns=None):
    """utimensat; None leaves that time alone (UTIME_OMIT). Raises OSError."""
    def ts(ns):
        if ns is None:
            return _Timespec(0, UTIME_OMIT)
        s, f = divmod(ns, 10 ** 9)
        return _Timespec(s, f)
    arr = (_Timespec * 2)(ts(atime_ns), ts(mtime_ns))
    r = _libc.utimensat(AT_FDCWD, os.fsencode(path), arr, 0)
    if r != 0:
        e = ctypes.get_errno()
        raise OSError(e, os.strerror(e))


def view(path):
    """What the OS shows: {mtime, ctime, atime} ns from stat, birth ns or None, flags (st_flags) or None."""
    st = os.lstat(path)
    d = {"mtime": st.st_mtime_ns, "ctime": st.st_ctime_ns, "atime": st.st_atime_ns,
         "birth": None, "flags": getattr(st, "st_flags", None)}
    if hasattr(st, "st_birthtime_ns"):
        d["birth"] = st.st_birthtime_ns
    elif hasattr(st, "st_birthtime"):
        d["birth"] = int(st.st_birthtime * 10 ** 9)
    return d


def fmt_view(v):
    return "mt=%s ct=%s at=%s birth=%s flags=%s" % (
        fmt_ns(v["mtime"]), fmt_ns(v["ctime"]), fmt_ns(v["atime"]),
        "-" if v["birth"] is None else fmt_ns(v["birth"]), v["flags"])


def run(argv, timeout=30):
    r = subprocess.run(argv, stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                       timeout=timeout)
    return r.returncode, (r.stdout + r.stderr).decode("utf-8", "replace").strip()


def xattr_get(path, name):
    """bytes or an errno name; macOS through /usr/bin/xattr, Linux through os.getxattr."""
    import errno
    if IS_MAC:
        rc, out = run(["/usr/bin/xattr", "-px", name, path])
        if rc != 0:
            return out.splitlines()[-1] if out else "rc%d" % rc
        return bytes.fromhex("".join(out.split()))
    try:
        return os.getxattr(path, name)
    except OSError as e:
        return errno.errorcode.get(e.errno, str(e.errno))


def xattr_set(path, name, data):
    import errno
    if IS_MAC:
        rc, out = run(["/usr/bin/xattr", "-wx", name, data.hex(), path])
        return "ok" if rc == 0 else (out.splitlines()[-1] if out else "rc%d" % rc)
    try:
        os.setxattr(path, name, data)
        return "ok"
    except OSError as e:
        return errno.errorcode.get(e.errno, str(e.errno))


# ---------------------------------------------------------------- mounts and snapshots

def mount_raw(ctx, vol, opts, rw=True):
    """Mount with exactly `opts` after the mandatory ones (no uid, gid, fmask, dmask are added), the
    deadline and SIGKILL of fsops. Environment B: backend=fskit,local,norecover,<opts>."""
    import shlex
    from harness import volume as hv
    import fsops
    fsops._check_extra(opts)
    if ctx.env == "B":
        o = "backend=fskit,local,norecover,%s%s" % (opts, "" if rw else ",ro")
        ctx.sh("%s -o %s %s %s" % (shlex.quote(hv.guest_tool("ntfs-3g")), o, shlex.quote(vol.img),
                                    shlex.quote(vol.mnt)), timeout=60)
        fsops._wait_b_mount(ctx, vol)
    else:
        ctx.sh("ntfs-3g %s %s -o %s,%s" % (shlex.quote(vol.img), shlex.quote(vol.mnt),
                                           "rw" if rw else "ro", opts), timeout=60)
        vol.mounted = True


def snap(mnt, names):
    """{name: view dict or None} for paths relative to mnt."""
    out = {}
    for n in names:
        try:
            out[n] = view(os.path.join(mnt, n))
        except OSError:
            out[n] = None
    return out


def cycle(ctx, vol, names, extra=""):
    """Unmount, read the MFT of the image, mount read-only, snapshot the views, unmount.
    Returns (disk entries, views after remount)."""
    import fsops
    if vol.mounted:
        fsops.unmount(ctx, vol)
    ents = disk_entries(vol.img)
    fsops.mount(ctx, vol, rw=False, extra=extra)
    try:
        after = snap(vol.mnt, names)
    finally:
        fsops.unmount(ctx, vol)
    return ents, after


def view_vs_disk(ents, after, kinds=("mtime", "ctime", "atime")):
    """Differences between the views after remount and the on-disk $STANDARD_INFORMATION (ns)."""
    bad = []
    for n, v in sorted(after.items()):
        base = os.path.basename(n)
        if v is None or base not in ents:
            continue
        d = si_ns(ents[base][1])
        for k in kinds:
            if v[k] != d[k]:
                bad.append("%s %s: view %s, disk %s" % (n, k, fmt_ns(v[k]), fmt_ns(d[k])))
    return bad


def ts(sec):
    """Unix seconds -> ns."""
    return int(sec) * 10 ** 9


def ymd(y, mo, d, h=0, mi=0, s=0):
    import calendar
    return calendar.timegm((y, mo, d, h, mi, s, 0, 0, 0))


def dump_json(ctx, name, obj):
    import json
    with open(ctx.evidence_file(name), "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, sort_keys=True, default=str)


def outcome(fn, *args):
    """'ok' or the errno name of the OSError fn(*args) raises."""
    import errno
    try:
        fn(*args)
        return "ok"
    except OSError as e:
        return errno.errorcode.get(e.errno, str(e.errno))


def ntfsinfo_times(ctx, vol, path):
    """The four $STANDARD_INFORMATION times of a root-level entry as ntfsinfo -F prints them, in
    seconds since the Unix epoch (ntfsinfo shows whole seconds), in the order creation, data change,
    MFT change, access. The volume must be unmounted. TZ=UTC makes the text unambiguous."""
    import calendar
    import re
    import shlex
    import time
    from harness import volume as hv
    tool = hv.guest_tool("ntfsinfo") if ctx.env == "B" else "ntfsinfo"
    r = ctx.transport.run("TZ=UTC %s -F %s -v %s" % (shlex.quote(tool), shlex.quote(path), shlex.quote(vol.img)))
    text = (r.out + r.err).decode("utf-8", "replace")
    if r.rc != 0:
        raise Fail("ntfsinfo -F %s failed (rc %d): %s" % (path, r.rc, text[-200:]))
    out = []
    for ln in text.splitlines():
        mm = re.match(r"^\s*([A-Za-z ]*Time):\s+(\w{3} \w{3}\s+\d+ \d\d:\d\d:\d\d \d{4})(?: [A-Z]+)?\s*$", ln)
        if mm:
            st = time.strptime(mm.group(2), "%a %b %d %H:%M:%S %Y")
            out.append((mm.group(1).strip(), calendar.timegm(st)))
        if len(out) == 4:
            break
    if len(out) != 4:
        raise Fail("ntfsinfo -F %s: found %d time lines in the output, expected 4: %s" % (path, len(out), text[:1500]))
    return out
