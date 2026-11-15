# SPDX-License-Identifier: GPL-2.0-or-later
"""Shared helpers of the read-path suite (I-READ-*, T-M3-07) and the large-file suite (I-BIG-*, T-M3-09).

Not a case module (it declares no cases, so the harness does not import it as one); the case modules put
their directory on sys.path and import it as `readlib`. Python 3.9, standard library only.

- mount options of the product (B) and the reference (A)
- raw $DATA attribute view of an unmounted image (resident or not, sparse, compressed, sizes), read
  straight from the MFT with metalib.RawVolume
- read primitives: open with or without the page cache (F_NOCACHE on macOS), pread matrix at and past EOF
"""

import errno
import fcntl
import os
import struct
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
for _p in (os.path.join(TESTS, "integration", "functional"), os.path.join(TESTS, "integration", "metadata")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import metalib as ml  # noqa: E402

IS_MAC = sys.platform == "darwin"
F_NOCACHE = 48            # <sys/fcntl.h>, Darwin only
KIB, MIB, GIB = 1024, 1 << 20, 1 << 30

EXTRA_B = "noatime,windows_names,streams_interface=openxattr"
EXTRA_A = "noatime"

ATTR_FLAG_COMPRESSED, ATTR_FLAG_SPARSE = 0x1, 0x8000
FILE_SPARSE, FILE_COMPRESSED = 0x200, 0x800


def extra(ctx):
    return EXTRA_B if ctx.env == "B" else EXTRA_A


# ---------------------------------------------------------------- raw $DATA view

def data_attr(rec):
    """Unnamed $DATA header of a base MFT record as a dict, or None."""
    off = struct.unpack_from("<H", rec, 0x14)[0]
    while off + 16 <= len(rec):
        typ, ln = struct.unpack_from("<II", rec, off)
        if typ == 0xFFFFFFFF or ln == 0:
            return None
        if typ == 0x80 and rec[off + 9] == 0:           # name length 0: the unnamed stream
            nonres = rec[off + 8]
            flags = struct.unpack_from("<H", rec, off + 0x0C)[0]
            d = {"resident": not nonres, "flags": flags,
                 "sparse": bool(flags & ATTR_FLAG_SPARSE), "compressed": bool(flags & ATTR_FLAG_COMPRESSED)}
            if not nonres:
                d["data_size"] = struct.unpack_from("<I", rec, off + 0x10)[0]
                d["alloc_size"] = d["init_size"] = d["data_size"]
                d["unit"] = 0
                d["compressed_size"] = None
            else:
                d["unit"] = struct.unpack_from("<H", rec, off + 0x22)[0]
                d["alloc_size"], d["data_size"], d["init_size"] = struct.unpack_from("<QQQ", rec, off + 0x28)
                d["compressed_size"] = struct.unpack_from("<Q", rec, off + 0x40)[0] if d["unit"] else None
            return d
        off += ln
    return None


def disk_view(img, names=None):
    """{file name: {"recno", "attrs": file attribute flags of $STANDARD_INFORMATION, "data": data_attr()}}
    for the in-use files of the unmounted image (names must be unique across directories)."""
    rv = ml.RawVolume(img)
    try:
        out = {}
        for nm, (recno, si, _fn) in rv.scan().items():
            if names is not None and nm not in names:
                continue
            rec = rv.record(recno)
            out[nm] = {"recno": recno, "attrs": si["flags"], "data": data_attr(rec) if rec is not None else None}
        return out, rv.cluster
    finally:
        rv.close()


def fmt_data(d):
    if d is None:
        return "no $DATA in the base record (attribute list?)"
    if d["resident"]:
        return "resident, %d bytes" % d["data_size"]
    s = "non-resident, size %d alloc %d init %d" % (d["data_size"], d["alloc_size"], d["init_size"])
    if d["sparse"]:
        s += ", sparse"
    if d["compressed"]:
        s += ", compressed"
    if d["unit"]:
        s += " (unit 2^%d clusters, compressed size %s)" % (d["unit"], d["compressed_size"])
    return s


def cluster_of(img):
    rv = ml.RawVolume(img)
    try:
        return rv.cluster
    finally:
        rv.close()


# ---------------------------------------------------------------- reading

def open_read(path, nocache=False, flags=os.O_RDONLY):
    fd = os.open(path, flags)
    if nocache and IS_MAC:
        fcntl.fcntl(fd, F_NOCACHE, 1)
    return fd


def ename(e):
    return errno.errorcode.get(e, str(e))


def offsets_for(n):
    s = {0, 1, n - 1, n, n + 1, n + 511, n + 4096, n + 65536, n + MIB, 1 << 32, 1 << 40}
    return sorted(o for o in s if o >= 0)


LENGTHS = (1, 511, 512, 4096, 4097, 65536, MIB)


def pread_matrix(fd, data, bad, tag, step=None):
    """pread at and past EOF for many offsets and lengths; every result must equal data[off:off+len]
    (b"" at and past EOF), no error. `bad` collects the failures."""
    n = len(data)
    count = 0
    for off in offsets_for(n):
        for ln in LENGTHS:
            count += 1
            try:
                got = os.pread(fd, ln, off)
            except OSError as e:
                bad.append("%s: pread(len=%d, off=%d) size %d -> %s" % (tag, ln, off, n, ename(e.errno)))
                continue
            want = data[off:off + ln]
            if got != want:
                bad.append("%s: pread(len=%d, off=%d) size %d -> %d bytes, expected %d%s" % (
                    tag, ln, off, n, len(got), len(want), "" if len(got) == len(want) else " (length differs)"))
    return count


def read_all(fd, bufsize, bad, tag, expect=None):
    """Read from the start with read(bufsize) until it returns b"". Returns (data, [chunk sizes]).
    Then read three more times: each must return b"" again."""
    os.lseek(fd, 0, os.SEEK_SET)
    chunks, parts = [], []
    while True:
        try:
            b = os.read(fd, bufsize)
        except OSError as e:
            bad.append("%s: read(%d) after %d bytes -> %s" % (tag, bufsize, sum(chunks), ename(e.errno)))
            return b"".join(parts), chunks
        if not b:
            break
        chunks.append(len(b))
        parts.append(b)
        if len(chunks) > 1 << 20:
            bad.append("%s: read loop does not end" % tag)
            break
    for i in range(3):
        try:
            b = os.read(fd, bufsize)
        except OSError as e:
            bad.append("%s: read at EOF (try %d) -> %s" % (tag, i + 1, ename(e.errno)))
            continue
        if b:
            bad.append("%s: read at EOF (try %d) returned %d bytes" % (tag, i + 1, len(b)))
    return b"".join(parts), chunks


def read_whole(path, bufsize=MIB):
    parts = []
    with open(path, "rb") as f:
        while True:
            b = f.read(bufsize)
            if not b:
                break
            parts.append(b)
    return b"".join(parts)


def short_form(chunks):
    """'3 reads: 1048576 1048576 1' with long runs folded."""
    if not chunks:
        return "0 reads: (none)"
    runs, prev, n = [], None, 0
    for c in chunks:
        if c == prev:
            n += 1
        else:
            if prev is not None:
                runs.append("%dx%d" % (n, prev) if n > 1 else str(prev))
            prev, n = c, 1
    runs.append("%dx%d" % (n, prev) if n > 1 else str(prev))
    return "%d reads: %s" % (len(chunks), " ".join(runs))


class Problems(object):
    """Collects the failures of a case so one run shows all of them."""

    def __init__(self, ctx):
        self.ctx = ctx
        self.bad = []

    def add(self, msg):
        self.bad.append(msg)
        self.ctx.log("PROBLEM " + msg)

    def extend(self, items):
        for i in items:
            self.add(i)

    def done(self, what):
        from harness import Fail
        if self.bad:
            for b in self.bad[:30]:
                self.ctx.log("MISMATCH " + b)
            raise Fail("%s: %d problems, first: %s" % (what, len(self.bad), self.bad[0]))
