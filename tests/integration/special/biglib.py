# SPDX-License-Identifier: GPL-2.0-or-later
"""Helpers of the large, sparse and compressed file cases (I-BIG-*, T-M3-09). Not a case module.

Model: the expected content of a file of many GiB without holding it in memory. A block generator makes
position-dependent, incompressible 1 MiB blocks cheaply (one random block, 8 bytes replaced by the block
number); patches (writes made later), truncation and growth are tracked on top, and `read(off, len)`
returns the expected bytes of any range, so the whole-file hash and spot reads are checked against it.
Python 3.9, standard library only.
"""

import hashlib
import os
import struct
import sys
import time

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
for _p in (os.path.join(TESTS, "integration", "readpath"), os.path.join(TESTS, "integration", "functional")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import readlib as rl  # noqa: E402,F401
from fsops import blob  # noqa: E402

KIB, MIB, GIB = 1 << 10, 1 << 20, 1 << 30
BLOCK = MIB


class Model(object):
    """Expected content: generated blocks below `valid`, zeros from `valid` to `size`, patches on top."""

    def __init__(self, label, size):
        self.base = blob("big:" + label, BLOCK)
        self.size = size
        self.valid = size
        self.patches = []

    def block(self, i):
        return struct.pack("<Q", i) + self.base[8:]

    def read(self, off, ln):
        """Expected bytes of [off, off+ln) clipped to the file size."""
        end = min(off + ln, self.size)
        if off >= end:
            return b""
        out = bytearray()
        pos = off
        while pos < end:
            i, o = divmod(pos, BLOCK)
            take = min(end - pos, BLOCK - o)
            lim = min(take, max(0, self.valid - pos))
            if lim > 0:
                out += self.block(i)[o:o + lim]
            if take > lim:
                out += b"\0" * (take - lim)
            pos += take
        for poff, pdata in self.patches:
            lo, hi = max(off, poff), min(end, poff + len(pdata))
            if lo < hi:
                out[lo - off:hi - off] = pdata[lo - poff:hi - poff]
        return bytes(out)

    def patch(self, off, data):
        self.patches.append((off, data))
        self.size = max(self.size, off + len(data))

    def truncate(self, n):
        if n < self.size:
            self.patches = [(o, d[:n - o]) for o, d in self.patches if o < n]
            self.valid = min(self.valid, n)
        self.size = n

    def hash(self):
        h = hashlib.sha256()
        for off in range(0, self.size, BLOCK):
            h.update(self.read(off, BLOCK))
        return h.hexdigest()


def write_model(path, model, ctx=None):
    """Write the model sequentially in 1 MiB writes; returns sha256 and seconds."""
    h = hashlib.sha256()
    t0 = time.time()
    with open(path, "wb") as f:
        for off in range(0, model.size, BLOCK):
            b = model.read(off, BLOCK)
            h.update(b)
            f.write(b)
        f.flush()
        os.fsync(f.fileno())
    return h.hexdigest(), time.time() - t0


def hash_file(path, nocache=False):
    """sha256 and seconds of a sequential read of the whole file in 1 MiB reads."""
    h = hashlib.sha256()
    t0 = time.time()
    fd = rl.open_read(path, nocache)
    try:
        while True:
            b = os.read(fd, BLOCK)
            if not b:
                break
            h.update(b)
    finally:
        os.close(fd)
    return h.hexdigest(), time.time() - t0


def rate(nbytes, secs):
    return "%.0f MiB in %.1f s (%.0f MiB/s)" % (nbytes / MIB, secs, nbytes / MIB / secs if secs > 0 else 0)


def spot_check(fd, model, offsets, lengths=(1, 4096, MIB + 3)):
    """pread at the offsets with the lengths, compare with the model. Returns a list of problems."""
    bad = []
    for off in offsets:
        if off < 0:
            continue
        for ln in lengths:
            try:
                got = os.pread(fd, ln, off)
            except OSError as e:
                bad.append("pread(len=%d, off=%d) -> %s" % (ln, off, rl.ename(e.errno)))
                continue
            want = model.read(off, ln)
            if got != want:
                bad.append("pread(len=%d, off=%d): %d bytes, expected %d%s" % (
                    ln, off, len(got), len(want), "" if len(got) == len(want) else " (length differs)"))
    return bad
