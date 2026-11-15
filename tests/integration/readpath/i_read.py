# SPDX-License-Identifier: GPL-2.0-or-later
"""I-READ-01..07 (T-M3-07, IMPLEMENTATION_PLAN 3.8, R-02): reads at and across EOF.

macFUSE 5.5.0 rejects an empty FUSE_READ reply with EIO (ARCHITECTURE.md, risk R-02). NTFS-3G answers a
read at or past EOF with an empty reply, so every read that reaches the end of a file is a place where an
application could see EIO. These cases read at, across and past EOF and require that no read ever fails
and that every read returns exactly the bytes the file holds.

  01  sizes 0, 1, 511, 512, 4095, 4096, 4097, cluster-1/cluster/cluster+1, 1 MiB+1: pread at and past EOF
      (offsets up to 2^40, lengths 1 B to 1 MiB), read(2) loops with 4 KiB to 8 MiB buffers, with and
      without the page cache (F_NOCACHE), right after writing and after a remount
  02  file truncated down through the open descriptor, then read; truncated and grown again
  03  file truncated by another descriptor (truncate(2) by path, O_TRUNC, ftruncate) while a reader is open
  04  resident (in the MFT record) against non-resident data: the size where a file leaves the record,
      transitions by write, truncate and grow, on-disk residency read from the unmounted image
  05  (B) the same reads against `-o debug`: which FUSE_READ requests reach the driver, which replies are
      empty
  06  read length crossing EOF with large buffers, readv, mmap of the last page, cat, dd, cp, tail
  07  cluster sizes 512 and 64 KiB (cluster +/- 1 means something other than 4095 and 4097)

Environment B is the product; Environment A (Linux, kernel FUSE) is the reference for 01-04, 06, 07.
What the backend returns for a read that crosses EOF (the short read) is logged for each size.
Python 3.9, standard library only.
"""

import mmap
import os
import re
import shlex
import subprocess
import sys
import time

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)

import readlib as rl  # noqa: E402
import fsops  # noqa: E402
from fsops import blob, put, sha  # noqa: E402
from harness import Fail, case, volume as hv  # noqa: E402
from harness.seeded import MIB  # noqa: E402

FIXED = (0, 1, 511, 512, 4095, 4096, 4097)


def sizes_for(cluster):
    return sorted(set(FIXED) | set([cluster - 1, cluster, cluster + 1, MIB + 1]))


def new_volume(ctx, mib, cluster=None):
    vol = fsops.open_volume(ctx, mib)
    if cluster:
        tool = hv.guest_tool("mkntfs") if ctx.env == "B" else "mkntfs"
        ctx.sh("%s -F -Q -c %d -L ntfsmac %s" % (shlex.quote(tool), cluster, shlex.quote(vol.img)))
    return vol


def modes():
    return (False, True) if rl.IS_MAC else (False,)


def check_file(ctx, path, data, P, tag):
    """The whole read matrix on one file, cached and (macOS) not cached. Logs the read sizes."""
    n = len(data)
    for nocache in modes():
        t = "%s %s" % (tag, "nocache" if nocache else "cached")
        fd = rl.open_read(path, nocache)
        try:
            st = os.fstat(fd)
            if st.st_size != n:
                P.add("%s: st_size %d, expected %d" % (t, st.st_size, n))
            rl.pread_matrix(fd, data, P.bad, t)
            for bs in (n + 100, 4096, MIB, 8 * MIB):
                got, chunks = rl.read_all(fd, bs, P.bad, "%s read(%d)" % (t, bs))
                if got != data:
                    P.add("%s read(%d): %d bytes read, expected %d%s" % (
                        t, bs, len(got), n, "" if len(got) == n else " (length differs)"))
                if bs in (n + 100, 8 * MIB):
                    ctx.log("%s read(%d): %s" % (t, bs, rl.short_form(chunks)))
            if n and os.read(fd, 0) != b"":
                P.add("%s: read(0) returned data" % t)
        finally:
            os.close(fd)


def write_model(root, model):
    for rel, data in model.items():
        put(os.path.join(root, rel), data)


def verify_model(ctx, vol, model, P, tag):
    for rel, data in sorted(model.items()):
        check_file(ctx, os.path.join(vol.mnt, rel), data, P, "%s %s" % (tag, rel))


def residency(ctx, vol, names):
    """Residency of each name on the unmounted image: {name: data_attr dict}."""
    view, cluster = rl.disk_view(vol.img, set(names))
    for nm in sorted(names, key=lambda x: (len(x), x)):
        v = view.get(nm)
        ctx.log("on disk %s: %s" % (nm, rl.fmt_data(v["data"]) if v else "not found"))
    return view, cluster


# ---------------------------------------------------------------- 01

@case("I-READ-01", envs=("A", "B"), title="reads at, across and past EOF for sizes around the sector, page and cluster size")
def i_read_01(ctx):
    vol = new_volume(ctx, 64)
    cluster = rl.cluster_of(vol.img)
    sizes = sizes_for(cluster)
    ctx.log("cluster %d bytes, sizes %s" % (cluster, sizes))
    model = dict(("s%d" % n, blob("read01:%d" % n, n)) for n in sizes)
    P = rl.Problems(ctx)
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        write_model(vol.mnt, model)
        verify_model(ctx, vol, model, P, "warm")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    view, _ = residency(ctx, vol, model)
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        verify_model(ctx, vol, model, P, "cold")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    P.done("reads at and across EOF")
    ctx.log("PASS: %d sizes, no read failed, every read returned the file's bytes" % len(sizes))


# ---------------------------------------------------------------- 02

TRUNC_SIZES = (200, 1000, 4097, 65536 + 3, MIB + 1)


def trunc_targets(size, cluster):
    return sorted(t for t in set([0, 1, 100, cluster - 1, cluster, cluster + 1, size // 2]) if 0 <= t < size)


@case("I-READ-02", envs=("A", "B"), title="truncate down through the open descriptor, then read; grow again")
def i_read_02(ctx):
    vol = new_volume(ctx, 128)
    cluster = rl.cluster_of(vol.img)
    P = rl.Problems(ctx)
    final = {}
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        for size in TRUNC_SIZES:
            for t in trunc_targets(size, cluster):
                for nocache in modes():
                    name = "f%d_%d_%s" % (size, t, "n" if nocache else "c")
                    tag = "trunc %d->%d %s" % (size, t, "nocache" if nocache else "cached")
                    path = os.path.join(vol.mnt, name)
                    data = blob("read02:" + name, size)
                    put(path, data)
                    fd = rl.open_read(path, nocache, os.O_RDWR)
                    try:
                        if os.pread(fd, size + 10, 0) != data:       # prime the cache with the whole file
                            P.add("%s: first read differs" % tag)
                        os.ftruncate(fd, t)
                        cur = data[:t]
                        if os.fstat(fd).st_size != t:
                            P.add("%s: st_size %d after ftruncate, expected %d" % (tag, os.fstat(fd).st_size, t))
                        rl.pread_matrix(fd, cur, P.bad, tag + " open fd")
                        got, ch = rl.read_all(fd, 4096, P.bad, tag + " read(4096)")
                        if got != cur:
                            P.add("%s: read after truncate returned %d bytes, expected %d" % (tag, len(got), t))
                        fd2 = rl.open_read(path, nocache)           # a second descriptor sees the same
                        try:
                            rl.pread_matrix(fd2, cur, P.bad, tag + " second fd")
                        finally:
                            os.close(fd2)
                        # grow: the new bytes are zeros
                        os.ftruncate(fd, t + 5000)
                        cur = cur + b"\0" * 5000
                        rl.pread_matrix(fd, cur, P.bad, tag + " grown")
                        os.pwrite(fd, b"tail", t + 5000)
                        cur = cur + b"tail"
                        rl.pread_matrix(fd, cur, P.bad, tag + " grown and written")
                        ctx.log("%s: ok (%d bytes)" % (tag, len(cur)))
                    finally:
                        os.close(fd)
                    final[name] = cur
        for nocache in modes():                                       # truncate to zero, read, write again
            name = "zero_%s" % ("n" if nocache else "c")
            path = os.path.join(vol.mnt, name)
            data = blob("read02:" + name, MIB + 1)
            put(path, data)
            fd = rl.open_read(path, nocache, os.O_RDWR)
            try:
                os.pread(fd, MIB + 10, 0)
                os.ftruncate(fd, 0)
                for off in (0, 1, 4096, MIB):
                    if os.pread(fd, 100, off) != b"":
                        P.add("%s: pread at %d after truncate to 0 returned data" % (name, off))
                new = blob("read02:new:" + name, 9000)
                os.pwrite(fd, new, 0)
                rl.pread_matrix(fd, new, P.bad, name + " rewritten")
            finally:
                os.close(fd)
            final[name] = new
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        verify_model_sample(ctx, vol, final, P)
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    P.done("truncate while open")
    ctx.log("PASS: %d truncate cases" % len(final))


def verify_model_sample(ctx, vol, model, P):
    """After the remount: whole-file read of every file must equal the model; the full matrix on a few."""
    names = sorted(model)
    for nm in names:
        got = rl.read_whole(os.path.join(vol.mnt, nm))
        if got != model[nm]:
            P.add("after remount %s: %d bytes, sha %s..., expected %d bytes, sha %s..." % (
                nm, len(got), sha(got)[:12], len(model[nm]), sha(model[nm])[:12]))
    for nm in names[::7]:
        check_file(ctx, os.path.join(vol.mnt, nm), model[nm], P, "after remount " + nm)


# ---------------------------------------------------------------- 03

@case("I-READ-03", envs=("A", "B"), title="file truncated by another descriptor while a reader has it open")
def i_read_03(ctx):
    vol = new_volume(ctx, 64)
    P = rl.Problems(ctx)
    final = {}
    ways = ("truncate(path)", "open(O_TRUNC)", "ftruncate(fd2)")
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        for size in (200, 5000, MIB + 1):
            for way in ways:
                for newlen in (0, 100, size // 4):
                    for nocache in modes():
                        name = "g%d_%s_%d_%s" % (size, re.sub(r"\W", "", way), newlen, "n" if nocache else "c")
                        tag = "%s by %s to %d %s" % (size, way, newlen, "nocache" if nocache else "cached")
                        path = os.path.join(vol.mnt, name)
                        data = blob("read03:" + name, size)
                        put(path, data)
                        rd = rl.open_read(path, nocache)
                        try:
                            pos = size // 2 if newlen else size
                            got = os.read(rd, pos) if pos else b""
                            if got != data[:pos]:
                                P.add("%s: first read differs" % tag)
                            if way == "truncate(path)":
                                os.truncate(path, newlen)
                            elif way == "open(O_TRUNC)":
                                if newlen:
                                    continue                       # O_TRUNC only gives length 0
                                os.close(os.open(path, os.O_WRONLY | os.O_TRUNC))
                            else:
                                w = os.open(path, os.O_WRONLY)
                                os.ftruncate(w, newlen)
                                os.close(w)
                            cur = data[:newlen]
                            if os.fstat(rd).st_size != newlen:
                                P.add("%s: reader sees st_size %d, expected %d" % (tag, os.fstat(rd).st_size, newlen))
                            # the reader's offset is past the new EOF (or at it): the next read is empty
                            try:
                                tail = os.read(rd, 4096)
                            except OSError as e:
                                P.add("%s: read after truncate -> %s" % (tag, rl.ename(e.errno)))
                                tail = b""
                            want = cur[pos:pos + 4096]
                            if tail != want:
                                P.add("%s: read at offset %d after truncate returned %d bytes, expected %d" % (
                                    tag, pos, len(tail), len(want)))
                            rl.pread_matrix(rd, cur, P.bad, tag + " reader")
                            # the writer refills the file; the reader's offset is inside the new data now
                            new = blob("read03:new:" + name, newlen + 3000)
                            w = os.open(path, os.O_WRONLY)
                            os.pwrite(w, new, 0)
                            os.close(w)
                            os.lseek(rd, newlen, os.SEEK_SET)
                            tail = os.read(rd, 8192)
                            if tail != new[newlen:newlen + 8192]:
                                P.add("%s: read at %d after the writer extended the file: %d bytes, expected %d" % (
                                    tag, newlen, len(tail), len(new[newlen:newlen + 8192])))
                            rl.pread_matrix(rd, new, P.bad, tag + " reader after rewrite")
                            final[name] = new
                            ctx.log("%s: ok" % tag)
                        finally:
                            os.close(rd)
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        verify_model_sample(ctx, vol, final, P)
    finally:
        fsops.unmount(ctx, vol)
    P.done("truncated by another descriptor")
    ctx.log("PASS: %d cases" % len(final))


# ---------------------------------------------------------------- 04

LIMIT_SIZES = (0, 1, 64, 128, 256, 384, 448, 512, 576, 640, 704, 768, 832, 896, 960)


@case("I-READ-04", envs=("A", "B"), title="resident and non-resident $DATA: the size limit, growth and shrink transitions")
def i_read_04(ctx):
    vol = new_volume(ctx, 64)
    P = rl.Problems(ctx)
    ex = rl.extra(ctx)

    def session(fn):
        fsops.mount(ctx, vol, extra=ex)
        try:
            fn()
        finally:
            fsops.unmount(ctx, vol)

    # part 1: where does a new file leave the MFT record?
    model = dict(("r%d" % n, blob("read04:%d" % n, n)) for n in LIMIT_SIZES)
    session(lambda: write_model(vol.mnt, model))
    view, cluster = residency(ctx, vol, model)
    limit = None
    for n in LIMIT_SIZES:
        d = view["r%d" % n]["data"]
        if d is None:
            continue
        if d["resident"]:
            limit = n
        elif limit is not None and n < limit:
            P.add("size %d is non-resident but %d was resident" % (n, limit))
    if limit is None:
        P.add("no size up to %d is resident" % LIMIT_SIZES[-1])
    ctx.log("largest resident size among %s: %s" % (list(LIMIT_SIZES), limit))
    for n in (0, 1, 64):
        d = view["r%d" % n]["data"]
        if d is not None and not d["resident"]:
            P.add("size %d is not resident (expected a resident small file)" % n)
    session(lambda: verify_model(ctx, vol, model, P, "cold"))

    # part 2: transitions. One file, each step in its own mount session, residency read after each.
    path_of = lambda: os.path.join(vol.mnt, "tr")      # noqa: E731
    state = {"data": b""}

    def create():
        state["data"] = blob("read04:tr0", 100)
        put(path_of(), state["data"])

    def grow_by_write():
        fd = os.open(path_of(), os.O_RDWR)
        try:
            extra_ = blob("read04:tr1", 100)
            os.pwrite(fd, extra_, 4900)
            state["data"] = state["data"] + b"\0" * 4800 + extra_
        finally:
            os.close(fd)

    def shrink_small():
        os.truncate(path_of(), 50)
        state["data"] = state["data"][:50]

    def shrink_zero():
        os.truncate(path_of(), 0)
        state["data"] = b""

    def rewrite_small():
        state["data"] = blob("read04:tr4", 30)
        put(path_of(), state["data"])

    def grow_by_truncate():
        os.truncate(path_of(), 10 * MIB)
        state["data"] = state["data"] + b"\0" * (10 * MIB - 30)

    def append_in_session():
        # in-session transition: the reads straddle the point where the data leaves the record
        fd = os.open(path_of(), os.O_RDWR)
        try:
            os.ftruncate(fd, 0)
            cur = b""
            for i, n in enumerate((300, 300, 300, 300, 4096)):
                piece = blob("read04:app%d" % i, n)
                os.pwrite(fd, piece, len(cur))
                cur += piece
                rl.pread_matrix(fd, cur, P.bad, "append step %d (%d bytes)" % (i, len(cur)))
            state["data"] = cur
        finally:
            os.close(fd)

    steps = (("create 100 B", create), ("write at 4900 (grow to 5000)", grow_by_write),
             ("truncate to 50", shrink_small), ("truncate to 0", shrink_zero), ("write 30 B", rewrite_small),
             ("truncate up to 10 MiB", grow_by_truncate), ("append 300 B x4 then 4096", append_in_session))
    for label, fn in steps:
        session(fn)
        v, _ = residency(ctx, vol, ["tr"])
        d = v["tr"]["data"]
        ctx.log("after '%s': %d bytes; %s" % (label, len(state["data"]), rl.fmt_data(d)))
        if d is not None and d["data_size"] != len(state["data"]):
            P.add("after '%s': data size on disk %d, expected %d" % (label, d["data_size"], len(state["data"])))
        session(lambda: check_file(ctx, path_of(), state["data"], P, "after '%s'" % label))
    fsops.fsck(ctx, vol)
    P.done("resident and non-resident data")
    ctx.log("PASS: records leave the MFT record above %s bytes; every transition read back correctly" % limit)


# ---------------------------------------------------------------- 05

UNIQUE_RE = re.compile(r"^unique: (\d+), opcode: READ \(15\)")
OUT_RE = re.compile(r"^\s+unique: (\d+), (success|error: (-?\d+)[^,]*), outsize: (\d+)")
RD_REQ_RE = re.compile(r"^read\[\d+\] (\d+) bytes from (\d+) flags")
RD_RET_RE = re.compile(r"^\s+read\[\d+\] (\d+) bytes from (\d+)\s*$")


def parse_reads(text):
    """[(unique, requested size, offset, returned bytes, outsize, status)] of the FUSE_READ requests in a
    libfuse2 debug log: the `read[fh] N bytes from OFF flags` line is the request, the indented
    `read[fh] M bytes from OFF` line what ntfs_fuse_read returned, `unique: U, success, outsize: S` the reply
    (S includes the 16-byte reply header, so S == 16 is an empty reply)."""
    reqs, order, cur = {}, [], None
    for line in text.splitlines():
        m = UNIQUE_RE.match(line)
        if m:
            cur = m.group(1)
            reqs[cur] = {"size": None, "off": None, "ret": None, "out": None, "status": None}
            order.append(cur)
            continue
        if cur is None:
            continue
        m = RD_REQ_RE.match(line)
        if m and reqs[cur]["size"] is None:
            reqs[cur]["size"], reqs[cur]["off"] = int(m.group(1)), int(m.group(2))
            continue
        m = RD_RET_RE.match(line)
        if m and reqs[cur]["ret"] is None:
            reqs[cur]["ret"] = int(m.group(1))
            continue
        m = OUT_RE.match(line)
        if m and m.group(1) in reqs and reqs[m.group(1)]["out"] is None:
            reqs[m.group(1)]["out"] = int(m.group(4))
            reqs[m.group(1)]["status"] = m.group(2)
    return [(u, reqs[u]["size"], reqs[u]["off"], reqs[u]["ret"], reqs[u]["out"], reqs[u]["status"]) for u in order]


@case("I-READ-05", envs=("B",), title="FUSE_READ requests and replies at and past EOF in the debug log")
def i_read_05(ctx):
    vol = new_volume(ctx, 64)
    P = rl.Problems(ctx)
    cluster = rl.cluster_of(vol.img)
    sizes = [0, 1, 511, 4095, 4097, cluster + 1, MIB + 1]
    model = dict(("d%d" % n, blob("read05:%d" % n, n)) for n in sizes)
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        write_model(vol.mnt, model)
    finally:
        fsops.unmount(ctx, vol)
    log_path = ctx.evidence_file("driver-debug.log")
    fsops.mount_debug(ctx, vol, log_path, extra=rl.extra(ctx))
    empty_below, empty_at, reached_eof, total = [], [], [], 0
    try:
        for n, name in sorted((len(d), nm) for nm, d in model.items()):
            data = model[name]
            path = os.path.join(vol.mnt, name)
            time.sleep(0.3)
            start = os.path.getsize(log_path)
            fd = rl.open_read(path, nocache=True)
            try:
                rl.pread_matrix(fd, data, P.bad, "debug " + name)
                got, chunks = rl.read_all(fd, 8 * MIB, P.bad, "debug %s read" % name)
                if got != data:
                    P.add("%s: read returned %d bytes, expected %d" % (name, len(got), n))
            finally:
                os.close(fd)
            time.sleep(0.5)
            with open(log_path, "rb") as f:
                f.seek(start)
                chunk = f.read().decode("utf-8", "replace")
            reads = parse_reads(chunk)
            ctx.log("%s (%d bytes): %d FUSE_READ requests reached the driver" % (name, n, len(reads)))
            seen = {}
            for u, size, off, ret, out, status in reads:
                total += 1
                empty = out is not None and out <= 16
                key = (size, off, ret, status, out)
                seen[key] = seen.get(key, 0) + 1
                if off is None or out is None:
                    continue
                if empty and off < n:
                    empty_below.append("%s: empty reply to READ at %d < size %d" % (name, off, n))
                if empty and off >= n:
                    empty_at.append("%s: empty reply to READ at %d >= size %d" % (name, off, n))
                if status and status.startswith("error"):
                    P.add("%s: READ at %d answered %s" % (name, off, status))
                if off >= n:
                    reached_eof.append("%s: READ of %s bytes at %s reached the driver (size %d)" % (name, size, off, n))
            for (size, off, ret, status, out), cnt in sorted(seen.items(), key=lambda kv: (kv[0][1] or 0, kv[0][0] or 0)):
                ctx.log("  %dx READ %s bytes from %s: driver returned %s, %s, outsize %s%s" % (
                    cnt, size, off, ret, status, out, " (EMPTY)" if out is not None and out <= 16 else ""))
        # a reader whose cached size is stale: another descriptor truncates the file under it
        name, n = "d1048577", 100
        path = os.path.join(vol.mnt, name)
        time.sleep(0.3)
        start = os.path.getsize(log_path)
        rd = rl.open_read(path, nocache=True)
        try:
            os.pread(rd, 4096, 500000)
            os.truncate(path, n)
            for off in (0, 50, 99, 100, 101, 500000, MIB):
                got = os.pread(rd, 4096, off)
                if got != model[name][:n][off:off + 4096]:
                    P.add("stale reader: pread at %d returned %d bytes" % (off, len(got)))
            os.truncate(path, 2 * MIB)                                   # grow again: zeros
            got = os.pread(rd, 4096, MIB)
            if got != b"\0" * 4096:
                P.add("stale reader: bytes of the regrown part are not zeros")
        finally:
            os.close(rd)
        time.sleep(0.5)
        with open(log_path, "rb") as f:
            f.seek(start)
            chunk = f.read().decode("utf-8", "replace")
        ctx.log("truncate under a reader: %d FUSE_READ requests" % len(parse_reads(chunk)))
        for u, size, off, ret, out, status in parse_reads(chunk):
            ctx.log("  READ %s bytes from %s: driver returned %s, %s, outsize %s" % (size, off, ret, status, out))
            total += 1
            if out is not None and out <= 16:
                empty_at.append("truncate under a reader: empty reply to READ at %s (final size 2 MiB, shrunk to %d meanwhile)" % (off, n))
    finally:
        fsops.unmount(ctx, vol)
    for e in empty_below:
        P.add(e)
    ctx.log("%d FUSE_READ requests in all; empty replies below EOF: %d; empty replies at or past EOF: %d" % (
        total, len(empty_below), len(empty_at)))
    ctx.log("FUSE_READ requests at or past EOF that reached the driver: %d" % len(reached_eof))
    for e in reached_eof[:10]:
        ctx.log("note: " + e)
    if total == 0:
        P.add("no FUSE_READ request found in the debug log (parser or the log is wrong)")
    P.done("debug log of reads")
    ctx.log("PASS: no application read failed; no READ below EOF was answered empty")


# ---------------------------------------------------------------- 06

CROSS_SIZES = (1, 511, 4095, 4097, 16383, 16384, 16385, 65537, 5 * MIB + 7)


@case("I-READ-06", envs=("A", "B"), title="read length crossing EOF: large buffers, readv, mmap of the last page, cat, dd, cp, tail")
def i_read_06(ctx):
    vol = new_volume(ctx, 96)
    P = rl.Problems(ctx)
    model = dict(("c%d" % n, blob("read06:%d" % n, n)) for n in CROSS_SIZES)
    ex = rl.extra(ctx)

    def work(tag):
        for name, data in sorted(model.items(), key=lambda kv: len(kv[1])):
            n = len(data)
            path = os.path.join(vol.mnt, name)
            for nocache in modes():
                t = "%s %s %s" % (tag, name, "nocache" if nocache else "cached")
                fd = rl.open_read(path, nocache)
                try:
                    for ln in (MIB, 4 * MIB, 8 * MIB, 16 * MIB, 64 * MIB):
                        for off in sorted(set([0, max(0, n - 1), max(0, n - 4097), max(0, n - MIB // 2), n // 2])):
                            got = os.pread(fd, ln, off)
                            want = data[off:off + ln]
                            if got != want and data[off:off + len(got)] != got:
                                P.add("%s: pread(len=%d, off=%d) returned wrong bytes (%d)" % (t, ln, off, len(got)))
                            elif len(got) != len(want):
                                # a short read below EOF: legal for POSIX, record it, then ask for the rest
                                ctx.log("%s: pread(len=%d, off=%d) returned %d of %d bytes" % (t, ln, off, len(got), len(want)))
                                rest = os.pread(fd, ln, off + len(got))
                                if not rest and off + len(got) < n:
                                    P.add("%s: empty read at %d below EOF %d" % (t, off + len(got), n))
                    # readv with two buffers that cross EOF
                    for off in sorted(set([max(0, n - 100), max(0, n - 5000), 0])):
                        b1, b2 = bytearray(4096), bytearray(70000)
                        os.lseek(fd, off, os.SEEK_SET)
                        try:
                            got = os.readv(fd, [b1, b2])
                        except OSError as e:
                            P.add("%s: readv(off=%d) -> %s" % (t, off, rl.ename(e.errno)))
                            continue
                        joined = bytes(b1) + bytes(b2)
                        want = data[off:off + 4096 + 70000]
                        if joined[:got] != want[:got]:
                            P.add("%s: readv(off=%d) returned wrong bytes" % (t, off))
                        elif got != len(want):
                            ctx.log("%s: readv(off=%d) returned %d of %d bytes" % (t, off, got, len(want)))
                            if off + got < n:
                                P.add("%s: readv(off=%d) stopped at %d below EOF %d" % (t, off, off + got, n))
                finally:
                    os.close(fd)
            # mmap: the last page is read in full, the file ends inside it
            with open(path, "rb") as f:
                mm = mmap.mmap(f.fileno(), 0, access=mmap.ACCESS_READ)
                try:
                    if mm[:] != data:
                        P.add("%s %s: mmap content differs" % (tag, name))
                    if mm[n - 1:n] != data[n - 1:]:
                        P.add("%s %s: mmap last byte differs" % (tag, name))
                finally:
                    mm.close()
            # tools
            q = shlex.quote(path)
            cp = os.path.join(vol.mnt, "copy_" + name)
            for label, cmd in (("cat", "cat %s" % q), ("dd bs=4194304", "dd if=%s bs=4194304 2>/dev/null" % q),
                               ("dd bs=1", "dd if=%s bs=1 count=%d 2>/dev/null" % (q, min(n, 5000))),
                               ("tail -c 100", "tail -c 100 %s" % q), ("head -c 100000", "head -c 100000 %s" % q)):
                r = subprocess.run(cmd, shell=True, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
                want = {"cat": data, "dd bs=4194304": data, "dd bs=1": data[:5000], "tail -c 100": data[-100:],
                        "head -c 100000": data[:100000]}[label]
                if r.returncode != 0 or r.stdout != want:
                    P.add("%s %s: %s rc=%d, %d bytes, expected %d (%s)" % (
                        tag, name, label, r.returncode, len(r.stdout), len(want), r.stderr.decode("utf-8", "replace")[:80]))
            r = subprocess.run(["cp", path, cp], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            if r.returncode != 0 or rl.read_whole(cp) != data:
                P.add("%s %s: cp rc=%d or copy differs" % (tag, name, r.returncode))
            os.unlink(cp)
        ctx.log("%s: all files done" % tag)

    fsops.mount(ctx, vol, extra=ex)
    try:
        write_model(vol.mnt, model)
        work("warm")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    fsops.mount(ctx, vol, extra=ex)
    try:
        work("cold")
    finally:
        fsops.unmount(ctx, vol)
    P.done("reads crossing EOF")
    ctx.log("PASS")


# ---------------------------------------------------------------- 07

@case("I-READ-07", envs=("A", "B"), title="EOF reads on volumes with 512 B and 64 KiB clusters")
def i_read_07(ctx):
    P = rl.Problems(ctx)
    for cluster in (512, 65536):
        vol = new_volume(ctx, 128, cluster=cluster)
        got = rl.cluster_of(vol.img)
        ctx.log("volume formatted with -c %d, boot sector says %d" % (cluster, got))
        if got != cluster:
            P.add("mkntfs -c %d made a volume with %d byte clusters" % (cluster, got))
        sizes = sorted(set(FIXED) | set([cluster - 1, cluster, cluster + 1, 2 * cluster - 1, 2 * cluster, 2 * cluster + 1,
                                         MIB + 1]))
        model = dict(("k%d" % n, blob("read07:%d:%d" % (cluster, n), n)) for n in sizes)
        fsops.mount(ctx, vol, extra=rl.extra(ctx))
        try:
            write_model(vol.mnt, model)
            verify_model(ctx, vol, model, P, "c%d warm" % cluster)
        finally:
            fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        residency(ctx, vol, model)
        fsops.mount(ctx, vol, extra=rl.extra(ctx))
        try:
            verify_model(ctx, vol, model, P, "c%d cold" % cluster)
        finally:
            fsops.unmount(ctx, vol)
    P.done("EOF reads on other cluster sizes")
    ctx.log("PASS")
