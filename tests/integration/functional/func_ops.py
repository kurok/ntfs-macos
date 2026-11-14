# SPDX-License-Identifier: GPL-2.0-or-later
"""I-FUNC-01..08: functional file operations on an NTFS volume (T-M3-05, IMPLEMENTATION_PLAN 3.3).

create and read, append, overwrite, truncate (shrink and extend), rename (same directory, across
directories, over an existing target), delete (including a file that is still open), rmdir.
Every case ends the same way: unmount, `ntfsfix -n`, remount read-only, SHA-256 of every file against
the model kept in memory, and the directory list. Hash checks before the unmount use the live mount.
Names are ASCII here; Unicode names are in names.py. Environments: B (the product), A (reference).
"""

import errno
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fsops  # noqa: E402
from fsops import blob, expect_errno, model_files, put, remount_check, sha, slurp  # noqa: E402
from harness import Fail, case  # noqa: E402

KIB = 1024
MIB = KIB * KIB


def _start(ctx, mib=256):
    vol = fsops.open_volume(ctx, mib)
    fsops.mount(ctx, vol)
    return vol, vol.mnt


def _same(ctx, path, data, what):
    got = slurp(path)
    if got != data:
        raise Fail("%s: content differs (size %d, expected %d, sha %s != %s)" % (
            what, len(got), len(data), sha(got)[:12], sha(data)[:12]))


@case("I-FUNC-01", envs=("A", "B"), title="create and read: sizes around cluster edges, O_EXCL, missing parent")
def i_func_01(ctx):
    vol, m = _start(ctx)
    sizes = (0, 1, 511, 512, 4095, 4096, 4097, 65536, MIB + 13, 24 * MIB + 1)
    model, dirs = {}, ["sub"]
    os.mkdir(os.path.join(m, "sub"))
    for i, size in enumerate(sizes):
        rel = ("sub/" if i % 2 else "") + "file%02d.bin" % i
        data = blob("func01:%d" % i, size)
        put(os.path.join(m, rel), data, mode="xb")
        st = os.stat(os.path.join(m, rel))
        if st.st_size != size:
            raise Fail("%s: stat size %d, expected %d" % (rel, st.st_size, size))
        _same(ctx, os.path.join(m, rel), data, rel)
        model[rel] = data
    # Positioned reads across cluster and page edges against the model.
    big = model["sub/file09.bin"]
    path = os.path.join(m, "sub/file09.bin")
    with open(path, "rb") as f:
        for off, n in ((0, 1), (4095, 2), (4096, 4096), (MIB - 1, 5), (len(big) - 3, 100), (len(big), 10), (len(big) + 5, 10)):
            f.seek(off)
            got = f.read(n)
            if got != big[off:off + n]:
                raise Fail("read at %d+%d differs" % (off, n))
    # Creating over an existing name with O_EXCL and below a missing parent must fail.
    expect_errno(ctx, ("EEXIST",), open, os.path.join(m, "file00.bin"), "xb", desc="O_EXCL over existing")
    expect_errno(ctx, ("ENOENT",), open, os.path.join(m, "nodir/x"), "wb", desc="create below missing dir")
    expect_errno(ctx, ("ENOENT",), open, os.path.join(m, "missing"), "rb", desc="open missing")
    remount_check(ctx, vol, model_files(model), dirs, "I-FUNC-01")


@case("I-FUNC-02", envs=("A", "B"), title="append: many small appends, O_APPEND, append after a remount")
def i_func_02(ctx):
    vol, m = _start(ctx)
    p = os.path.join(m, "log.bin")
    data = bytearray(blob("func02:base", 10000))
    put(p, bytes(data))
    for i in range(60):
        chunk = blob("func02:%d" % i, 1 + (i * 977) % 9000)
        with open(p, "ab") as f:
            f.write(chunk)
        data += chunk
        if i % 10 == 0 and os.stat(p).st_size != len(data):
            raise Fail("size after append %d: %d, expected %d" % (i, os.stat(p).st_size, len(data)))
    # One descriptor with O_APPEND and an explicit seek to 0 before each write: still appends.
    fd = os.open(p, os.O_WRONLY | os.O_APPEND)
    try:
        for i in range(40):
            chunk = blob("func02:fd:%d" % i, 3 + i * 211)
            os.lseek(fd, 0, os.SEEK_SET)
            os.write(fd, chunk)
            data += chunk
            if i % 8 == 0:
                os.fsync(fd)
    finally:
        os.close(fd)
    _same(ctx, p, bytes(data), "log.bin after appends")
    # Append to an empty file, and to a file that grows from zero across cluster edges.
    e = os.path.join(m, "grow.bin")
    open(e, "wb").close()
    grow = bytearray()
    for i in range(30):
        chunk = blob("func02:grow:%d" % i, 1000 + i * 333)
        with open(e, "ab") as f:
            f.write(chunk)
        grow += chunk
    _same(ctx, e, bytes(grow), "grow.bin")
    model = {"log.bin": bytes(data), "grow.bin": bytes(grow)}
    # Remount read/write and append once more to an existing file.
    fsops.unmount(ctx, vol)
    fsops.mount(ctx, vol)
    tail = blob("func02:after-remount", 65537)
    with open(p, "ab") as f:
        f.write(tail)
    model["log.bin"] = bytes(data) + tail
    _same(ctx, p, model["log.bin"], "log.bin after remount and append")
    remount_check(ctx, vol, model_files(model), [], "I-FUNC-02")


@case("I-FUNC-03", envs=("A", "B"), title="overwrite in place, across cluster edges, past EOF, O_TRUNC shorter and longer")
def i_func_03(ctx):
    vol, m = _start(ctx)
    p = os.path.join(m, "over.bin")
    data = bytearray(blob("func03:base", 3 * MIB))
    put(p, bytes(data))
    with open(p, "r+b") as f:
        for off, label, n in ((1500000, "mid", 1000), (4094, "edge1", 8), (65535, "edge2", 3), (0, "head", 100)):
            patch = blob("func03:" + label, n)
            f.seek(off)
            f.write(patch)
            data[off:off + n] = patch
        # Overwrite the last 10 bytes and extend by 100.
        tail = blob("func03:tail", 110)
        f.seek(len(data) - 10)
        f.write(tail)
        data[len(data) - 10:] = tail
        # pwrite far inside, with the seek position elsewhere.
        os.pwrite(f.fileno(), b"PWRITE", 2 * MIB)
        data[2 * MIB:2 * MIB + 6] = b"PWRITE"
    _same(ctx, p, bytes(data), "over.bin after patches")
    model = {"over.bin": bytes(data)}
    # Whole-file rewrites: shorter (O_TRUNC), longer, same size via r+b.
    for name, size in (("short.bin", 500), ("long.bin", 200000), ("same.bin", 100000)):
        put(os.path.join(m, name), blob("func03:%s:v1" % name, 150000))
    short = blob("func03:short:v2", 500)
    put(os.path.join(m, "short.bin"), short)
    long_ = blob("func03:long:v2", 800000)
    put(os.path.join(m, "long.bin"), long_)
    same = blob("func03:same:v2", 150000)
    with open(os.path.join(m, "same.bin"), "r+b") as f:
        f.write(same)
    model.update({"short.bin": short, "long.bin": long_, "same.bin": same})
    for k in ("short.bin", "long.bin", "same.bin"):
        _same(ctx, os.path.join(m, k), model[k], k)
    remount_check(ctx, vol, model_files(model), [], "I-FUNC-03")


@case("I-FUNC-04", envs=("A", "B"), title="truncate: shrink, extend with zeros, no stale data after shrink then extend")
def i_func_04(ctx):
    vol, m = _start(ctx)
    model = {}

    def mk(name, size):
        d = blob("func04:" + name, size)
        put(os.path.join(m, name), d)
        return bytearray(d)

    a = mk("shrink-path.bin", MIB)
    os.truncate(os.path.join(m, "shrink-path.bin"), 300000)
    del a[300000:]
    model["shrink-path.bin"] = bytes(a)

    b = mk("shrink-fd.bin", MIB)
    with open(os.path.join(m, "shrink-fd.bin"), "r+b") as f:
        for n in (524289, 4097, 4096, 1):
            f.truncate(n)
            del b[n:]
            if os.fstat(f.fileno()).st_size != n:
                raise Fail("ftruncate(%d): size %d" % (n, os.fstat(f.fileno()).st_size))
    model["shrink-fd.bin"] = bytes(b)

    c = mk("extend.bin", 100000)
    os.truncate(os.path.join(m, "extend.bin"), 100000 + 8 * MIB)
    c += bytes(8 * MIB)
    model["extend.bin"] = bytes(c)

    # Shrink then extend again: bytes past the shrink point must read as zeros, never the old data.
    d = mk("stale.bin", 64 * KIB)
    with open(os.path.join(m, "stale.bin"), "r+b") as f:
        f.truncate(1000)
        f.truncate(64 * KIB)
    d[1000:] = bytes(64 * KIB - 1000)
    model["stale.bin"] = bytes(d)

    # Truncate to zero, extend, then write inside the extension.
    e = mk("zero.bin", 50000)
    p = os.path.join(m, "zero.bin")
    os.truncate(p, 0)
    os.truncate(p, 100000)
    e = bytearray(100000)
    with open(p, "r+b") as f:
        f.seek(77777)
        f.write(b"tail-data")
    e[77777:77786] = b"tail-data"
    model["zero.bin"] = bytes(e)

    # Open with O_TRUNC while another descriptor still has the file open.
    g = mk("trunc-open.bin", 20000)
    p = os.path.join(m, "trunc-open.bin")
    other = open(p, "rb")
    try:
        put(p, b"new")
        if other.read() != b"new":
            raise Fail("second descriptor does not see the O_TRUNC rewrite")
    finally:
        other.close()
    model["trunc-open.bin"] = b"new"

    for k, v in model.items():
        _same(ctx, os.path.join(m, k), v, k)
    remount_check(ctx, vol, model_files(model), [], "I-FUNC-04")


@case("I-FUNC-05", envs=("A", "B"), title="rename in one directory: over existing, onto itself, directories, errors")
def i_func_05(ctx):
    vol, m = _start(ctx)
    j = lambda *a: os.path.join(m, *a)  # noqa: E731
    model, dirs = {}, set()

    put(j("a.bin"), blob("func05:a", 70000))
    os.rename(j("a.bin"), j("b.bin"))
    model["b.bin"] = blob("func05:a", 70000)
    expect_errno(ctx, ("ENOENT",), os.stat, j("a.bin"), desc="old name after rename")

    # Over an existing file: the target's data is gone, the source's data stays.
    put(j("src.bin"), blob("func05:src", 123456))
    put(j("dst.bin"), blob("func05:dst", 5000))
    os.rename(j("src.bin"), j("dst.bin"))
    model["dst.bin"] = blob("func05:src", 123456)
    _same(ctx, j("dst.bin"), model["dst.bin"], "dst.bin after rename over")

    # Onto itself: success and no change.
    os.rename(j("b.bin"), j("b.bin"))
    _same(ctx, j("b.bin"), model["b.bin"], "b.bin after self rename")

    expect_errno(ctx, ("ENOENT",), os.rename, j("nothing"), j("else"), desc="rename missing source")

    # Rename through an open descriptor: the descriptor follows the file.
    put(j("open.bin"), blob("func05:open", 9000))
    fd = os.open(j("open.bin"), os.O_RDWR)
    try:
        os.rename(j("open.bin"), j("open-renamed.bin"))
        os.lseek(fd, 0, os.SEEK_END)
        os.write(fd, b"+tail")
    finally:
        os.close(fd)
    model["open-renamed.bin"] = blob("func05:open", 9000) + b"+tail"
    _same(ctx, j("open-renamed.bin"), model["open-renamed.bin"], "open-renamed.bin")

    # Directories.
    os.mkdir(j("d1"))
    put(j("d1/in.bin"), blob("func05:d1in", 3000))
    os.rename(j("d1"), j("d2"))
    model["d2/in.bin"] = blob("func05:d1in", 3000)
    dirs.add("d2")
    expect_errno(ctx, ("ENOENT",), os.stat, j("d1"), desc="old directory name")
    os.mkdir(j("empty1"))
    os.mkdir(j("empty2"))
    os.rename(j("empty1"), j("empty2"))  # directory over an empty directory
    dirs.add("empty2")
    os.mkdir(j("full"))
    put(j("full/x"), b"x")
    model["full/x"] = b"x"
    dirs.add("full")
    os.mkdir(j("mover"))
    dirs.add("mover")
    expect_errno(ctx, ("ENOTEMPTY", "EEXIST"), os.rename, j("mover"), j("full"), desc="directory over non-empty directory")
    expect_errno(ctx, ("EISDIR",), os.rename, j("b.bin"), j("mover"), desc="file over directory")
    expect_errno(ctx, ("ENOTDIR",), os.rename, j("mover"), j("b.bin"), desc="directory over file")
    _same(ctx, j("b.bin"), model["b.bin"], "b.bin after refused renames")

    remount_check(ctx, vol, model_files(model), dirs, "I-FUNC-05")


@case("I-FUNC-06", envs=("A", "B"), title="rename across directories: file, over existing, directory, into itself, missing parent")
def i_func_06(ctx):
    vol, m = _start(ctx)
    j = lambda *a: os.path.join(m, *a)  # noqa: E731
    model, dirs = {}, set()
    for d in ("d1", "d2", "d1/deep", "d2/deep"):
        os.mkdir(j(d))
        dirs.add(d)

    put(j("d1/x.bin"), blob("func06:x", 250000))
    os.rename(j("d1/x.bin"), j("d2/x.bin"))
    model["d2/x.bin"] = blob("func06:x", 250000)
    expect_errno(ctx, ("ENOENT",), os.stat, j("d1/x.bin"), desc="source after move")

    put(j("d1/y.bin"), blob("func06:y-new", 4097))
    put(j("d2/y.bin"), blob("func06:y-old", 900000))
    os.rename(j("d1/y.bin"), j("d2/y.bin"))
    model["d2/y.bin"] = blob("func06:y-new", 4097)

    # Down and up the tree.
    put(j("d1/up.bin"), blob("func06:up", 10))
    os.rename(j("d1/up.bin"), j("up.bin"))
    _same(ctx, j("up.bin"), blob("func06:up", 10), "up.bin")
    os.rename(j("up.bin"), j("d1/deep/down.bin"))
    model["d1/deep/down.bin"] = blob("func06:up", 10)

    # A directory with content to another parent, and over an empty directory there.
    os.mkdir(j("d1/tree"))
    put(j("d1/tree/f1"), blob("func06:f1", 1111))
    put(j("d1/tree/f2"), blob("func06:f2", 2222))
    os.mkdir(j("d2/tree"))  # empty target, replaced
    os.rename(j("d1/tree"), j("d2/tree"))
    dirs.discard("d1/tree")
    dirs.add("d2/tree")
    model["d2/tree/f1"] = blob("func06:f1", 1111)
    model["d2/tree/f2"] = blob("func06:f2", 2222)

    expect_errno(ctx, ("EINVAL",), os.rename, j("d1"), j("d1/deep/inside"), desc="directory into its own subtree")
    expect_errno(ctx, ("ENOENT",), os.rename, j("d2/x.bin"), j("nodir/x.bin"), desc="target parent missing")
    _same(ctx, j("d2/x.bin"), model["d2/x.bin"], "d2/x.bin after refused moves")
    remount_check(ctx, vol, model_files(model), dirs, "I-FUNC-06")


@case("I-FUNC-07", envs=("A", "B"), title="delete: unlink, unlink while open, same name re-created, space returned")
def i_func_07(ctx):
    vol, m = _start(ctx)
    j = lambda *a: os.path.join(m, *a)  # noqa: E731
    model, dirs = {}, set()

    put(j("gone.bin"), blob("func07:gone", 10000))
    os.unlink(j("gone.bin"))
    expect_errno(ctx, ("ENOENT",), os.stat, j("gone.bin"), desc="stat after unlink")
    expect_errno(ctx, ("ENOENT",), os.unlink, j("gone.bin"), desc="unlink twice")

    # Delete an open file: the descriptor keeps working, the name is gone and can be re-used.
    orig = blob("func07:open", 300000)
    put(j("open.bin"), orig)
    fd = os.open(j("open.bin"), os.O_RDWR)
    try:
        os.unlink(j("open.bin"))
        expect_errno(ctx, ("ENOENT",), os.stat, j("open.bin"), desc="stat of the unlinked name")
        if os.pread(fd, 1000, 100000) != orig[100000:101000]:
            raise Fail("read through the descriptor of an unlinked file returned other data")
        os.pwrite(fd, b"still writable", 0)
        if os.pread(fd, 14, 0) != b"still writable":
            raise Fail("write through the descriptor of an unlinked file is not read back")
        put(j("open.bin"), blob("func07:reborn", 777))  # same name, new file, while the old one is open
        model["open.bin"] = blob("func07:reborn", 777)
        if os.pread(fd, 14, 0) != b"still writable":
            raise Fail("the new file with the old name changed the unlinked file")
        os.ftruncate(fd, 0)
    finally:
        os.close(fd)
    _same(ctx, j("open.bin"), model["open.bin"], "open.bin (new file)")

    os.makedirs(j("dir/inner"))
    dirs.update(("dir", "dir/inner"))
    for i in range(20):
        put(j("dir/inner/f%02d" % i), blob("func07:f%d" % i, 100 + i))
        if i % 2:
            os.unlink(j("dir/inner/f%02d" % i))
        else:
            model["dir/inner/f%02d" % i] = blob("func07:f%d" % i, 100 + i)

    # Space comes back after delete (statvfs of the mount; a loose bound, FSKit may lag).
    before = fsops.free_bytes(m)
    put(j("space.bin"), blob("func07:space", 20 * MIB))
    os.sync()
    during = fsops.free_bytes(m)
    os.unlink(j("space.bin"))
    os.sync()
    after = fsops.free_bytes(m)
    ctx.log("free bytes: before %d, with 20 MiB file %d, after unlink %d" % (before, during, after))
    if before - during < 15 * MIB:
        ctx.log("note: free space dropped by only %d bytes for a 20 MiB file" % (before - during))
    if after < before - 2 * MIB:
        raise Fail("free space not returned after unlink: before %d, after %d" % (before, after))

    remount_check(ctx, vol, model_files(model), dirs, "I-FUNC-07")


@case("I-FUNC-08", envs=("A", "B"), title="rmdir: non-empty refused, empty works, wrong type refused, unlink of a directory")
def i_func_08(ctx):
    vol, m = _start(ctx)
    j = lambda *a: os.path.join(m, *a)  # noqa: E731
    model, dirs = {}, set()

    os.mkdir(j("full"))
    put(j("full/file"), b"data")
    expect_errno(ctx, ("ENOTEMPTY", "EEXIST"), os.rmdir, j("full"), desc="rmdir with a file inside")
    os.mkdir(j("sub"))
    os.mkdir(j("sub/inner"))
    expect_errno(ctx, ("ENOTEMPTY", "EEXIST"), os.rmdir, j("sub"), desc="rmdir with a subdirectory inside")
    os.mkdir(j("hid"))
    put(j("hid/.hidden"), b"h")
    expect_errno(ctx, ("ENOTEMPTY", "EEXIST"), os.rmdir, j("hid"), desc="rmdir with only a dot file inside")
    expect_errno(ctx, ("ENOTDIR",), os.rmdir, j("full/file"), desc="rmdir on a file")
    expect_errno(ctx, ("ENOENT",), os.rmdir, j("nothing"), desc="rmdir on a missing name")
    expect_errno(ctx, ("EPERM", "EISDIR"), os.unlink, j("sub"), desc="unlink on a directory")
    # Nothing was lost by the refused calls.
    if slurp(j("full/file")) != b"data" or not os.path.isdir(j("sub/inner")):
        raise Fail("a refused rmdir or unlink removed something")
    # Emptied, then removed.
    os.unlink(j("full/file"))
    os.rmdir(j("full"))
    os.rmdir(j("sub/inner"))
    os.rmdir(j("sub"))
    os.unlink(j("hid/.hidden"))
    os.rmdir(j("hid"))
    for d in ("full", "sub", "hid"):
        expect_errno(ctx, ("ENOENT",), os.stat, j(d), desc="stat of removed %s" % d)
    os.mkdir(j("keep"))
    put(j("keep/k"), blob("func08:k", 4097))
    model["keep/k"] = blob("func08:k", 4097)
    dirs.add("keep")
    remount_check(ctx, vol, model_files(model), dirs, "I-FUNC-08")
