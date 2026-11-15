# SPDX-License-Identifier: GPL-2.0-or-later
"""I-BIG-01..03, I-BIG-SPARSE-01..02 (T-M3-09, IMPLEMENTATION_PLAN 3.5): small, large and sparse files.

I-BIG-01         0 B, 1 B, cluster +/- 1, 1 MiB +/- 1, 64 MiB + 1, 128 MiB: written, hashed, remounted, hashed
I-BIG-02  (B)    4 GiB + 1 byte: write, hash, boundary reads at 2^31 and 2^32, update across both, truncate
I-BIG-03  (B)    5 GiB: the same, then truncate down below and above 4 GiB and grow again
I-BIG-SPARSE-01  hole at start, middle and end, all hole: hash and allocated size (st_blocks, on-disk $DATA
                 attribute, ntfsinfo), updates into holes and into data, grow and shrink
I-BIG-SPARSE-02  a 6 GiB sparse file with data below, across and above 4 GiB: hash, allocated size

The 4 GiB and 5 GiB cases need free space on the guest disk (image about size + 300 MiB, created in the case's
work directory and deleted afterwards); without it they skip with a note. A file is made sparse by ftruncate to
its final size and writes after that; a write past the end of a file without the ftruncate fills the gap with
zeros on disk (observed, I-BIG-SPARSE-01). Content comes from biglib.Model, so no file is held in memory.
Environment B is the product, A (Linux, kernel FUSE) the reference where the case runs there. Python 3.9.
"""

import fcntl
import os
import re
import shlex
import struct
import sys

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)

import biglib as bl  # noqa: E402
import readlib as rl  # noqa: E402
import fsops  # noqa: E402
from biglib import GIB, KIB, MIB, Model  # noqa: E402
from harness import Fail, case, volume as hv  # noqa: E402

F_PUNCHHOLE = 99


def mib_up(n):
    return (n + MIB - 1) // MIB * MIB


def image_mib(size):
    return mib_up(int(size * 1.02) + 320 * MIB) // MIB


def ntfsinfo(ctx, vol, name, tag):
    """ntfsinfo -v of one file of the unmounted image, saved to evidence; returns the $DATA lines."""
    tool = hv.guest_tool("ntfsinfo") if ctx.env == "B" else "ntfsinfo"
    if not tool:
        return []
    r = ctx.transport.run("%s -v -F %s %s" % (shlex.quote(tool), shlex.quote("/" + name), shlex.quote(vol.img)))
    text = r.out.decode("utf-8", "replace")
    with open(ctx.evidence_file("%s.ntfsinfo.txt" % re.sub(r"\W", "_", tag)), "w") as f:
        f.write(text)
    keep = []
    for line in text.splitlines():
        if re.search(r"File attributes|Allocated [Ss]ize|Data [Ss]ize|Initialized size|Compressed size|Compression unit|"
                     r"Attribute flags", line) and "Resident" not in line:
            keep.append(" ".join(line.split()))
    return keep


def on_disk(ctx, vol, names, tag=""):
    view, cluster = rl.disk_view(vol.img, set(names))
    for nm in sorted(names):
        v = view.get(nm)
        ctx.log("on disk %s%s: %s; file attributes 0x%x" % (tag, nm, rl.fmt_data(v["data"]) if v else "not found",
                                                           v["attrs"] if v else 0))
    return view


def matrix(path, m, bad, tag):
    fd = os.open(path, os.O_RDONLY)
    try:
        rl.pread_matrix(fd, m.read(0, m.size), bad, tag)
    finally:
        os.close(fd)


def need_space(ctx, size):
    img = image_mib(size) * MIB
    free = fsops.free_bytes(ctx.work)
    ctx.log("image %d MiB, free on the work directory's disk %d MiB" % (img // MIB, free // MIB))
    if free < img + 3 * GIB:
        ctx.skip("not enough free disk for a %d MiB image plus 3 GiB headroom: %d MiB free in %s "
                 "(the golden VM has about 7 GiB; clone with a larger disk, tart set --disk-size)" % (
                     img // MIB, free // MIB, ctx.work))


# ---------------------------------------------------------------- I-BIG-01

@case("I-BIG-01", envs=("A", "B"), title="0 B, 1 B, cluster +/- 1, 1 MiB +/- 1, 64 MiB + 1, 128 MiB: write, hash, remount, hash")
def i_big_01(ctx):
    vol = fsops.open_volume(ctx, 512)
    cluster = rl.cluster_of(vol.img)
    sizes = sorted(set([0, 1, cluster - 1, cluster, cluster + 1, MIB - 1, MIB, MIB + 1, 64 * MIB + 1, 128 * MIB]))
    models = dict(("b%d" % n, Model("b01:%d" % n, n)) for n in sizes)
    want = {}
    bad = []
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        for nm, m in models.items():
            path = os.path.join(vol.mnt, nm)
            h, secs = bl.write_model(path, m)
            want[nm] = h
            if os.stat(path).st_size != m.size:
                bad.append("%s: st_size %d, expected %d" % (nm, os.stat(path).st_size, m.size))
            h2, secs2 = bl.hash_file(path)
            if h2 != h:
                bad.append("%s: hash after write differs" % nm)
            if m.size <= MIB + 1:
                matrix(path, m, bad, nm + " warm")
            ctx.log("%s: %d bytes, write %s" % (nm, m.size, bl.rate(m.size, secs)))
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    view = on_disk(ctx, vol, models)
    fsops.mount(ctx, vol, extra=rl.extra(ctx))
    try:
        for nm, m in sorted(models.items(), key=lambda kv: kv[1].size):
            path = os.path.join(vol.mnt, nm)
            h, secs = bl.hash_file(path)
            st = os.stat(path)
            ctx.log("cold %s: size %d, st_blocks %d (%d bytes), hash %s" % (
                nm, st.st_size, st.st_blocks, st.st_blocks * 512, "equal" if h == want[nm] else "DIFFERS"))
            if h != want[nm]:
                bad.append("%s: hash after remount differs" % nm)
            if st.st_size != m.size:
                bad.append("%s: st_size %d after remount, expected %d" % (nm, st.st_size, m.size))
            if m.size <= MIB + 1:
                matrix(path, m, bad, nm + " cold")
            d = view[nm]["data"]
            if d is not None and d["data_size"] != m.size:
                bad.append("%s: $DATA size on disk %d, expected %d" % (nm, d["data_size"], m.size))
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    if bad:
        for b in bad[:20]:
            ctx.log("MISMATCH " + b)
        raise Fail("%d problems, first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: %d sizes, hashes equal after write and after remount" % len(sizes))


# ---------------------------------------------------------------- I-BIG-02, 03

def big_case(ctx, size, tag):
    need_space(ctx, size)
    vol = fsops.open_volume(ctx, image_mib(size))
    m = Model(tag, size)
    bad = []
    ex = rl.extra(ctx)
    BND = [1 << 31, 1 << 32]

    def bounds(sz):
        o = [0, 4095, 4096, MIB - 1, MIB, sz - 1, sz, sz + 1, sz + MIB]
        for b in BND:
            o += [b - MIB - 1, b - 4097, b - 1, b, b + 1, b + 4095, b + MIB]
        return sorted(set(x for x in o if x >= 0))

    def spot(path, label):
        for nocache in ((False, True) if rl.IS_MAC else (False,)):
            fd = rl.open_read(path, nocache)
            try:
                bad.extend("%s %s: %s" % (label, "nocache" if nocache else "cached", b)
                           for b in bl.spot_check(fd, m, bounds(m.size)))
            finally:
                os.close(fd)

    def full_hash(path, label):
        h, secs = bl.hash_file(path)
        eh = m.hash()
        ctx.log("%s: hash of %d bytes %s in %s" % (label, m.size, "equal" if h == eh else "DIFFERS",
                                                    bl.rate(m.size, secs)))
        if h != eh:
            bad.append("%s: hash differs from the model" % label)

    def session(fn):
        fsops.mount(ctx, vol, extra=ex)
        try:
            fn(os.path.join(vol.mnt, "big.bin"))
        finally:
            fsops.unmount(ctx, vol)

    def s_write(path):
        h, secs = bl.write_model(path, m)
        ctx.log("written %d bytes in %s" % (size, bl.rate(size, secs)))
        st = os.stat(path)
        ctx.log("st_size %d, st_blocks %d (%d MiB)" % (st.st_size, st.st_blocks, st.st_blocks * 512 // MIB))
        if st.st_size != size:
            bad.append("st_size %d after write, expected %d" % (st.st_size, size))
        spot(path, "warm")
        full_hash(path, "warm")
        if h != m.hash():
            bad.append("hash of the written stream differs from the model")

    def s_cold(path):
        st = os.stat(path)
        if st.st_size != m.size:
            bad.append("st_size %d after remount, expected %d" % (st.st_size, m.size))
        spot(path, "cold")
        full_hash(path, "cold")

    def s_update(path):
        fd = os.open(path, os.O_RDWR)
        try:
            for off, ln, lab in ((BND[0] + 3 - MIB // 2, MIB + 7, "across 2 GiB"), (BND[1] - 30000, 65541, "across 4 GiB"),
                                 (m.size - 1, 1, "last byte"), (0, 5, "first bytes"), (m.size // 2 + 11, 100003, "middle")):
                data = bl.blob("upd:%s:%d" % (tag, off), ln)
                os.pwrite(fd, data, off)
                m.patch(off, data)
                ctx.log("updated %d bytes at %d (%s)" % (ln, off, lab))
            os.fsync(fd)
        finally:
            os.close(fd)
        spot(path, "after update")
        full_hash(path, "after update")

    def s_trunc(path):
        low = (1 << 32) - 1 if size <= (1 << 32) + 1 else (1 << 32) + 12345
        os.truncate(path, low)
        m.truncate(low)
        ctx.log("truncated to %d" % low)
        if os.stat(path).st_size != low:
            bad.append("st_size after truncate is %d, expected %d" % (os.stat(path).st_size, low))
        spot(path, "after truncate")
        grown = low + MIB + 9
        os.truncate(path, grown)
        m.truncate(grown)
        ctx.log("grown to %d" % grown)
        spot(path, "after grow")
        full_hash(path, "after truncate and grow")

    session(s_write)
    fsops.fsck(ctx, vol)
    view = on_disk(ctx, vol, ["big.bin"])
    d = view["big.bin"]["data"]
    if d is not None and d["data_size"] != size:
        bad.append("$DATA size on disk %d, expected %d" % (d["data_size"], size))
    session(s_cold)
    session(s_update)
    session(s_cold)
    fsops.fsck(ctx, vol)
    session(s_trunc)
    session(s_cold)
    fsops.fsck(ctx, vol)
    on_disk(ctx, vol, ["big.bin"], "final ")
    if bad:
        for b in bad[:20]:
            ctx.log("MISMATCH " + b)
        raise Fail("%d problems, first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: %d byte file written, read after remount, updated across 2 GiB and 4 GiB, truncated and grown; "
            "every hash equals the model" % size)


@case("I-BIG-02", envs=("B",), title="4 GiB + 1 byte file: write, hash, boundary reads, update across 2 GiB and 4 GiB, truncate")
def i_big_02(ctx):
    big_case(ctx, 4 * GIB + 1, "4g1")


@case("I-BIG-03", envs=("B",), title="5 GiB file: write, hash, boundary reads, update across 2 GiB and 4 GiB, truncate below and above 4 GiB")
def i_big_03(ctx):
    big_case(ctx, 5 * GIB, "5g")


# ---------------------------------------------------------------- sparse

def make_sparse(path, size, pieces):
    """ftruncate to size, then write the pieces [(off, bytes)]; the file stays sparse."""
    fd = os.open(path, os.O_RDWR | os.O_CREAT | os.O_TRUNC, 0o644)
    try:
        os.ftruncate(fd, size)
        for off, data in pieces:
            os.pwrite(fd, data, off)
        os.fsync(fd)
    finally:
        os.close(fd)


def sparse_model(label, size, pieces):
    m = Model(label, size)
    m.valid = 0
    for off, data in pieces:
        m.patch(off, data)
    return m


def seek_probe(ctx, path, tag):
    fd = os.open(path, os.O_RDONLY)
    try:
        out = []
        for name, whence in (("SEEK_DATA", getattr(os, "SEEK_DATA", 3)), ("SEEK_HOLE", getattr(os, "SEEK_HOLE", 4))):
            try:
                out.append("%s(0)=%d" % (name, os.lseek(fd, 0, whence)))
            except OSError as e:
                out.append("%s(0)=%s" % (name, rl.ename(e.errno)))
        ctx.log("%s: %s" % (tag, ", ".join(out)))
    finally:
        os.close(fd)


def punch_probe(ctx, path, off, ln):
    """macOS F_PUNCHHOLE: recorded, not asserted."""
    if not rl.IS_MAC:
        return None
    fd = os.open(path, os.O_RDWR)
    try:
        try:
            fcntl.fcntl(fd, F_PUNCHHOLE, struct.pack("<IIqq", 0, 0, off, ln))
            r = "ok"
        except OSError as e:
            r = rl.ename(e.errno)
    finally:
        os.close(fd)
    ctx.log("F_PUNCHHOLE(%d, %d) on %s: %s" % (off, ln, os.path.basename(path), r))
    return r


def blocks(path):
    return os.stat(path).st_blocks * 512


@case("I-BIG-SPARSE-01", envs=("A", "B"), title="sparse files with a hole at start, middle, end: hash, allocated size, updates into holes")
def i_big_sparse_01(ctx):
    size = 256 * MIB
    piece = MIB + 4099
    vol = fsops.open_volume(ctx, 640)
    layouts = {
        "hole_start": [(size - piece - 13, bl.blob("sp:start", piece))],
        "hole_middle": [(7, bl.blob("sp:mid-a", piece)), (size - piece - 13, bl.blob("sp:mid-b", piece))],
        "hole_end": [(7, bl.blob("sp:end", piece))],
        "all_hole": [],
    }
    models = dict((nm, sparse_model("sp01:" + nm, size, pcs)) for nm, pcs in layouts.items())
    bad = []
    ex = rl.extra(ctx)
    allocated = {}
    fsops.mount(ctx, vol, extra=ex)
    try:
        for nm, pcs in layouts.items():
            path = os.path.join(vol.mnt, nm)
            make_sparse(path, size, pcs)
            m = models[nm]
            data_bytes = sum(len(d) for _o, d in pcs)
            h, secs = bl.hash_file(path)
            allocated[nm] = blocks(path)
            ctx.log("%s: size %d, data %d bytes, st_blocks %d bytes allocated, hash %s (%s)" % (
                nm, os.stat(path).st_size, data_bytes, allocated[nm], "equal" if h == m.hash() else "DIFFERS",
                bl.rate(size, secs)))
            if h != m.hash():
                bad.append("%s: hash differs from the model" % nm)
            if allocated[nm] > data_bytes + 4 * MIB:
                bad.append("%s: %d bytes allocated for %d bytes of data: the file is not sparse" % (nm, allocated[nm], data_bytes))
            fd = os.open(path, os.O_RDONLY)
            try:
                offs = [0, 1, 6, 7, 8, size // 2, size - piece - 14, size - piece - 13, size - 1, size, size + 1]
                bad.extend("%s: %s" % (nm, b) for b in bl.spot_check(fd, m, offs))
            finally:
                os.close(fd)
            seek_probe(ctx, path, nm)
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    view = on_disk(ctx, vol, layouts)
    for nm in layouts:
        v = view[nm]
        d = v["data"]
        for line in ntfsinfo(ctx, vol, nm, "sparse01_" + nm):
            ctx.log("ntfsinfo %s: %s" % (nm, line))
        if not v["attrs"] & rl.FILE_SPARSE:
            bad.append("%s: FILE_ATTRIBUTE_SPARSE_FILE (0x200) is not set on disk (attributes 0x%x)" % (nm, v["attrs"]))
        if d is None or d["resident"] or not d["sparse"]:
            bad.append("%s: $DATA is not a non-resident sparse attribute on disk: %s" % (nm, rl.fmt_data(d)))
        elif d["data_size"] != size:
            bad.append("%s: $DATA size on disk %d, expected %d" % (nm, d["data_size"], size))
        elif d["compressed_size"] is not None:
            ctx.log("%s: on-disk allocated (compressed size) %d bytes of a %d byte file" % (nm, d["compressed_size"], size))
            dbytes = sum(len(x[1]) for x in layouts[nm])
            if d["compressed_size"] > dbytes + 4 * MIB:
                bad.append("%s: %d bytes allocated on disk for %d bytes of data" % (nm, d["compressed_size"], dbytes))
    fsops.mount(ctx, vol, extra=ex)
    try:
        for nm in layouts:
            path = os.path.join(vol.mnt, nm)
            h, secs = bl.hash_file(path)
            ctx.log("cold %s: st_blocks %d bytes, hash %s" % (nm, blocks(path), "equal" if h == models[nm].hash() else "DIFFERS"))
            if h != models[nm].hash():
                bad.append("%s: hash after remount differs" % nm)
            if blocks(path) > sum(len(x[1]) for x in layouts[nm]) + 4 * MIB:
                bad.append("%s: st_blocks %d after remount" % (nm, blocks(path)))
        # updates: into the hole, into data, zeros into data, grow, shrink
        path = os.path.join(vol.mnt, "hole_middle")
        m = models["hole_middle"]
        before = blocks(path)
        fd = os.open(path, os.O_RDWR)
        try:
            steps = [("write 100 KiB+3 into the hole at 128 MiB+17", size // 2 + 17, bl.blob("sp:upd1", 100 * KIB + 3)),
                     ("overwrite 5000 bytes inside data at 1000", 1000, bl.blob("sp:upd2", 5000)),
                     ("write 1 byte into the hole at 64 MiB", 64 * MIB, b"\x07"),
                     ("write zeros (64 KiB) into data at 70000", 70000, b"\0" * 65536)]
            for lab, off, data in steps:
                os.pwrite(fd, data, off)
                m.patch(off, data)
                os.fsync(fd)
                ctx.log("%s: st_blocks %d -> %d bytes" % (lab, before, blocks(path)))
                before = blocks(path)
            h, _s = bl.hash_file(path)
            if h != m.hash():
                bad.append("hole_middle: hash after updates differs from the model")
            os.ftruncate(fd, size + 10 * MIB)
            m.truncate(size + 10 * MIB)
            os.ftruncate(fd, size // 2 + 5)
            m.truncate(size // 2 + 5)
            ctx.log("grown by 10 MiB, then shrunk into the middle of the former hole: st_blocks %d bytes" % blocks(path))
            h, _s = bl.hash_file(path)
            if h != m.hash():
                bad.append("hole_middle: hash after grow and shrink differs from the model")
        finally:
            os.close(fd)
        punch_probe(ctx, path, 7, 4 * MIB)
        # all_hole: one byte at the very end, one at the start
        path = os.path.join(vol.mnt, "all_hole")
        m = models["all_hole"]
        fd = os.open(path, os.O_RDWR)
        try:
            for off in (size - 1, 0):
                os.pwrite(fd, b"\x5a", off)
                m.patch(off, b"\x5a")
            os.fsync(fd)
        finally:
            os.close(fd)
        h, _s = bl.hash_file(path)
        ctx.log("all_hole after two single-byte writes: st_blocks %d bytes, hash %s" % (blocks(path), "equal" if h == m.hash() else "DIFFERS"))
        if h != m.hash():
            bad.append("all_hole: hash after single-byte writes differs")
        # a write past EOF without ftruncate: dense or sparse?
        path = os.path.join(vol.mnt, "seek_write")
        fd = os.open(path, os.O_RDWR | os.O_CREAT, 0o644)
        try:
            os.pwrite(fd, b"y", 64 * MIB)
            os.fsync(fd)
        finally:
            os.close(fd)
        ctx.log("pwrite of 1 byte at 64 MiB into an empty file (no ftruncate): st_size %d, st_blocks %d bytes" % (
            os.stat(path).st_size, blocks(path)))
        sw = Model("sp01:seek", 64 * MIB + 1)
        sw.valid = 0
        sw.patch(64 * MIB, b"y")
        h, _s = bl.hash_file(path)
        if h != sw.hash():
            bad.append("seek_write: hash differs from the model")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    models_final = {"hole_middle": models["hole_middle"], "all_hole": models["all_hole"]}
    fsops.mount(ctx, vol, rw=False, extra=ex)
    try:
        for nm, m in models_final.items():
            h, _s = bl.hash_file(os.path.join(vol.mnt, nm))
            if h != m.hash():
                bad.append("%s: hash after the update session and remount differs" % nm)
    finally:
        fsops.unmount(ctx, vol)
    on_disk(ctx, vol, ["hole_middle", "all_hole", "seek_write"], "after updates ")
    if bad:
        for b in bad[:20]:
            ctx.log("MISMATCH " + b)
        raise Fail("%d problems, first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: sparse files keep their holes (allocated size small, SPARSE flag on disk), hashes equal")


@case("I-BIG-SPARSE-02", envs=("A", "B"), title="6 GiB sparse file with data below, across and above 4 GiB: hash and allocated size")
def i_big_sparse_02(ctx):
    size = 6 * GIB
    vol = fsops.open_volume(ctx, 512)
    pieces = [(5, bl.blob("sp2:a", 100003)), ((1 << 32) - 70001, bl.blob("sp2:b", 140009)),
              (size - 65537, bl.blob("sp2:c", 65537))]
    m = sparse_model("sp02", size, pieces)
    bad = []
    ex = rl.extra(ctx)
    expected = m.hash()
    data_bytes = sum(len(d) for _o, d in pieces)

    def check(path, tag):
        h, secs = bl.hash_file(path)
        ctx.log("%s: size %d, st_blocks %d bytes, hash %s in %s" % (tag, os.stat(path).st_size, blocks(path),
                                                                    "equal" if h == expected else "DIFFERS", bl.rate(size, secs)))
        if h != expected:
            bad.append("%s: hash differs from the model" % tag)
        if os.stat(path).st_size != size:
            bad.append("%s: st_size %d" % (tag, os.stat(path).st_size))
        if blocks(path) > data_bytes + 4 * MIB:
            bad.append("%s: %d bytes allocated for %d bytes of data" % (tag, blocks(path), data_bytes))
        fd = os.open(path, os.O_RDONLY)
        try:
            offs = [4, 5, 5 + 100003, (1 << 32) - 70002, (1 << 32) - 1, 1 << 32, (1 << 32) + 70008, (1 << 32) + 70009,
                    size - 65538, size - 1, size, size + 1]
            bad.extend("%s: %s" % (tag, b) for b in bl.spot_check(fd, m, offs))
        finally:
            os.close(fd)

    fsops.mount(ctx, vol, extra=ex)
    try:
        path = os.path.join(vol.mnt, "sparse6g")
        make_sparse(path, size, pieces)
        check(path, "warm")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    view = on_disk(ctx, vol, ["sparse6g"])
    for line in ntfsinfo(ctx, vol, "sparse6g", "sparse02"):
        ctx.log("ntfsinfo: " + line)
    d = view["sparse6g"]["data"]
    if d is None or not d["sparse"] or d["data_size"] != size:
        bad.append("on-disk $DATA is not a sparse attribute of %d bytes: %s" % (size, rl.fmt_data(d)))
    fsops.mount(ctx, vol, extra=ex)
    try:
        check(os.path.join(vol.mnt, "sparse6g"), "cold")
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    if bad:
        for b in bad[:20]:
            ctx.log("MISMATCH " + b)
        raise Fail("%d problems, first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: 6 GiB sparse file, hash equal after write and remount, allocated size small")
