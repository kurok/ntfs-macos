# SPDX-License-Identifier: GPL-2.0-or-later
"""Functional workload for the leak run (S-LEAK-01, T-M5-03).

  python3 workload.py MOUNTPOINT [--iterations N] [--start K] [--seed S]

Runs inside the macOS guest against a mounted NTFS volume and exercises the paths of the worker
that allocate: directories, small and medium files, overwrite, truncate and extend, rename (also
over an existing name), hard links, symbolic links, chmod and utimes, named streams (xattr),
Unicode and 255-byte names, mmap, readdir, statfs, copy trees, and the error paths (EEXIST,
ENOTEMPTY, ENOENT, a name that windows_names forbids). Every write is read back and compared with
an in-memory model; a mismatch is a failure (exit 1), so this is a functional suite and not only
a load generator. Standard library only (guest /usr/bin/python3, 3.9).
"""

import argparse
import ctypes
import ctypes.util
import errno
import hashlib
import mmap
import os
import random
import shutil
import stat
import sys
import unicodedata

_libc = ctypes.CDLL(ctypes.util.find_library("c"), use_errno=True)
_libc.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                           ctypes.c_uint32, ctypes.c_int]
_libc.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                           ctypes.c_uint32, ctypes.c_int]
_libc.getxattr.restype = ctypes.c_ssize_t
_libc.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
_libc.listxattr.restype = ctypes.c_ssize_t
_libc.removexattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]


class Mismatch(Exception):
    pass


def check(cond, what):
    if not cond:
        raise Mismatch(what)


def expect_error(fn, what, allowed=None):
    """The call must fail (with one of the errnos in allowed, if given)."""
    try:
        fn()
    except OSError as e:
        if allowed is not None and e.errno not in allowed:
            raise Mismatch("%s: errno %s (%s), expected one of %s" % (what, e.errno, e, sorted(allowed)))
        return e.errno
    raise Mismatch("%s: succeeded, expected an error" % what)


def setx(path, name, value):
    if _libc.setxattr(path.encode(), name.encode(), value, len(value), 0, 0) != 0:
        e = ctypes.get_errno()
        raise OSError(e, "setxattr %s %s: %s" % (path, name, os.strerror(e)))


def getx(path, name):
    n = _libc.getxattr(path.encode(), name.encode(), None, 0, 0, 0)
    if n < 0:
        e = ctypes.get_errno()
        raise OSError(e, "getxattr %s %s: %s" % (path, name, os.strerror(e)))
    buf = ctypes.create_string_buffer(max(n, 1))
    n2 = _libc.getxattr(path.encode(), name.encode(), buf, n, 0, 0)
    if n2 < 0:
        e = ctypes.get_errno()
        raise OSError(e, "getxattr %s %s: %s" % (path, name, os.strerror(e)))
    return buf.raw[:n2]


def listx(path):
    n = _libc.listxattr(path.encode(), None, 0, 0)
    if n < 0:
        e = ctypes.get_errno()
        raise OSError(e, "listxattr %s: %s" % (path, os.strerror(e)))
    buf = ctypes.create_string_buffer(max(n, 1))
    n2 = _libc.listxattr(path.encode(), buf, n, 0)
    return [x.decode("utf-8", "replace") for x in buf.raw[:max(n2, 0)].split(b"\0") if x]


def delx(path, name):
    if _libc.removexattr(path.encode(), name.encode(), 0) != 0:
        e = ctypes.get_errno()
        raise OSError(e, "removexattr %s %s: %s" % (path, name, os.strerror(e)))


def blob(rnd, n):
    if n == 0:
        return b""
    unit = bytes(rnd.getrandbits(8) for _ in range(64))
    return (unit * (n // 64 + 1))[:n]


def write_file(path, data, sync=False):
    with open(path, "wb") as f:
        f.write(data)
        if sync:
            f.flush()
            os.fsync(f.fileno())


def read_file(path):
    with open(path, "rb") as f:
        return f.read()


def small_files(d, rnd):
    model = {}
    for k in range(60):
        name = "s%03d.dat" % k
        data = blob(rnd, rnd.choice([0, 1, 17, 511, 512, 513, 4095, 4096, 4097, 9000]))
        write_file(os.path.join(d, name), data, sync=(k % 15 == 0))
        model[name] = data
    names = sorted(os.listdir(d))
    check(names == sorted(model), "readdir lists the files that were created")
    for name, data in model.items():
        check(read_file(os.path.join(d, name)) == data, "small file %s reads back" % name)
        check(os.stat(os.path.join(d, name)).st_size == len(data), "size of %s" % name)
    return model


def medium_file(d, rnd):
    path = os.path.join(d, "medium.bin")
    model = bytearray()
    with open(path, "wb") as f:
        for _ in range(32):
            chunk = blob(rnd, 65536)
            f.write(chunk)
            model += chunk
    # overwrite in the middle, extend past the end (a hole), shrink, grow again
    fd = os.open(path, os.O_RDWR)
    try:
        for off, n in ((1000, 5000), (65536 * 3 - 10, 100), (len(model) - 50, 200)):
            data = blob(rnd, n)
            os.pwrite(fd, data, off)
            if off + n > len(model):
                model += b"\0" * (off + n - len(model))
            model[off:off + n] = data
        os.pwrite(fd, b"tail", len(model) + 70000)
        model += b"\0" * 70000 + b"tail"
        os.fsync(fd)
        os.ftruncate(fd, 300000)
        del model[300000:]
        os.ftruncate(fd, 500000)
        model += b"\0" * (500000 - len(model))
        check(os.pread(fd, 500000, 0) == bytes(model), "medium file reads back after pwrite/truncate")
    finally:
        os.close(fd)
    check(hashlib.sha256(read_file(path)).digest() == hashlib.sha256(bytes(model)).digest(),
          "medium file hash after close")
    # mmap read and write
    with open(path, "r+b") as f:
        mm = mmap.mmap(f.fileno(), 200000)
        check(mm[0:4096] == bytes(model[0:4096]), "mmap read")
        mm[10:20] = b"0123456789"
        model[10:20] = b"0123456789"
        mm.flush()
        mm.close()
    check(read_file(path)[:64] == bytes(model[:64]), "mmap write is visible")
    return path


def names_and_links(d, rnd):
    # Unicode (NFC), astral, 255 bytes of name, spaces and dots inside
    for name in ("café.txt", "日本語.txt", "emoji-\U0001f600.txt", "a" * 250 + ".txt",
                 "with space.txt", "x.y.z"):
        write_file(os.path.join(d, name), name.encode("utf-8"))
        check(read_file(os.path.join(d, name)) == name.encode("utf-8"), "name %r" % name[:20])
        listed = {unicodedata.normalize("NFC", x) for x in os.listdir(d)}
        check(unicodedata.normalize("NFC", name) in listed, "name %r is listed" % name[:20])
    # rename, over an existing file, across directories
    a, b = os.path.join(d, "ren-a"), os.path.join(d, "ren-b")
    write_file(a, b"A")
    write_file(b, b"B")
    os.rename(a, b)
    check(read_file(b) == b"A" and not os.path.exists(a), "rename over an existing file")
    sub = os.path.join(d, "sub1", "sub2", "sub3")
    os.makedirs(sub)
    os.rename(b, os.path.join(sub, "moved"))
    check(read_file(os.path.join(sub, "moved")) == b"A", "rename across directories")
    os.rename(os.path.join(d, "sub1"), os.path.join(d, "sub1-renamed"))
    check(os.path.isdir(os.path.join(d, "sub1-renamed", "sub2", "sub3")), "rename of a directory tree")
    # hard link: link() answers EIO in Environment B but creates the name (I-META-05, I-XATTR-07), so
    # the call is made for the path it exercises and either outcome is accepted
    src = os.path.join(d, "link-src")
    write_file(src, b"linked")
    hl = os.path.join(d, "link-hard")
    try:
        os.link(src, hl)
    except OSError as e:
        check(e.errno == errno.EIO, "link() fails only with the known EIO, got %s" % e)
    if os.path.exists(hl):
        os.unlink(hl)
    check(read_file(src) == b"linked", "the first name survives link()")
    hl = src
    sl = os.path.join(d, "link-sym")
    os.symlink("link-src", sl)
    check(os.readlink(sl) == "link-src" and read_file(sl) == b"linked", "symbolic link")
    check(stat.S_ISLNK(os.lstat(sl).st_mode), "lstat of a symbolic link")
    os.unlink(sl)
    # permissions and times
    os.chmod(hl, 0o600)
    os.utime(hl, (1000000000, 1100000000))
    st = os.stat(hl)
    check(int(st.st_mtime) == 1100000000, "utime sets the modification time")


def streams(d, rnd):
    path = os.path.join(d, "streams.txt")
    write_file(path, b"main data")
    vals = {"user.alpha": b"one", "user.beta": blob(rnd, 700), "user.empty": b"x",
            "com.apple.metadata:kMDItemFinderComment": b"comment"}
    for k, v in vals.items():
        setx(path, k, v)
    for k, v in vals.items():
        check(getx(path, k) == v, "stream %s reads back" % k)
    names = listx(path)
    for k in vals:
        check(k in names, "stream %s is listed (%s)" % (k, names))
    setx(path, "user.alpha", b"replaced")
    check(getx(path, "user.alpha") == b"replaced", "stream replace")
    delx(path, "user.beta")
    expect_error(lambda: getx(path, "user.beta"), "getxattr of a removed stream")
    expect_error(lambda: getx(path, "user.never"), "getxattr of a missing stream")
    expect_error(lambda: delx(path, "user.never"), "removexattr of a missing stream")
    check(read_file(path) == b"main data", "main stream is unchanged by the named streams")


def error_paths(d, rnd):
    p = os.path.join(d, "err-dir")
    os.mkdir(p)
    write_file(os.path.join(p, "inside"), b"x")
    expect_error(lambda: os.mkdir(p), "mkdir of an existing directory", {errno.EEXIST})
    expect_error(lambda: os.rmdir(p), "rmdir of a non-empty directory", {errno.ENOTEMPTY, errno.EEXIST})
    expect_error(lambda: os.stat(os.path.join(d, "nope")), "stat of a missing name", {errno.ENOENT})
    expect_error(lambda: os.unlink(p), "unlink of a directory", {errno.EPERM, errno.EISDIR, errno.EACCES})
    expect_error(lambda: open(os.path.join(d, "bad?name"), "wb"), "create of a name windows_names forbids")
    expect_error(lambda: open(os.path.join(d, "bad:name"), "wb"), "create of a name with a colon")
    expect_error(lambda: os.rename(p, os.path.join(p, "inner")), "rename of a directory into itself")
    expect_error(lambda: os.link(p, os.path.join(d, "dirlink")), "hard link of a directory")
    expect_error(lambda: os.open(os.path.join(d, "x" * 300), os.O_CREAT | os.O_WRONLY, 0o644),
                 "create of a name longer than 255", {errno.ENAMETOOLONG, errno.EINVAL})
    shutil.rmtree(p)
    check(not os.path.exists(p), "rmtree of a tree")


def tree_copy(d, rnd):
    src = os.path.join(d, "tree-src")
    for i in range(6):
        sub = os.path.join(src, "d%d" % i, "e")
        os.makedirs(sub)
        for j in range(8):
            write_file(os.path.join(sub, "f%d" % j), blob(rnd, rnd.randrange(0, 20000)))
    dst = os.path.join(d, "tree-dst")
    shutil.copytree(src, dst)
    for dp, dn, fns in os.walk(src):
        for fn in fns:
            a = os.path.join(dp, fn)
            b = os.path.join(dst, os.path.relpath(a, src))
            check(read_file(a) == read_file(b), "copy of %s" % os.path.relpath(a, src))
    n = sum(len(fns) for _, _, fns in os.walk(dst))
    check(n == 48, "copy has 48 files, has %d" % n)
    with os.scandir(dst) as it:
        check(len(list(it)) == 6, "scandir of the copy")
    shutil.rmtree(src)
    shutil.rmtree(dst)


def iteration(root, i, rnd):
    d = os.path.join(root, "it%03d" % i)
    os.mkdir(d)
    small = os.path.join(d, "small")
    os.mkdir(small)
    small_files(small, rnd)
    medium_file(d, rnd)
    names_and_links(d, rnd)
    streams(d, rnd)
    error_paths(d, rnd)
    tree_copy(d, rnd)
    sv = os.statvfs(root)
    check(sv.f_blocks > 0 and sv.f_bfree <= sv.f_blocks, "statvfs")
    # delete about half of the small files, then the whole iteration except for every third one
    for name in sorted(os.listdir(small))[::2]:
        os.unlink(os.path.join(small, name))
    os.sync()
    if i % 3 != 0:
        shutil.rmtree(d)
        check(not os.path.exists(d), "rmtree of iteration %d" % i)


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("mount")
    ap.add_argument("--iterations", type=int, default=3)
    ap.add_argument("--seed", type=int, default=20261120)
    ap.add_argument("--start", type=int, default=1, help="number of the first iteration (a later call continues)")
    a = ap.parse_args()
    root = os.path.join(a.mount, "leakwork")
    os.makedirs(root, exist_ok=True)
    rnd = random.Random(a.seed + a.start)
    for i in range(a.start, a.start + a.iterations):
        try:
            iteration(root, i, rnd)
        except (Mismatch, OSError) as e:
            print("FAIL: iteration %d: %s" % (i, e))
            return 1
        print("workload: iteration %d done" % i, flush=True)
    print("workload: ok")
    return 0


if __name__ == "__main__":
    sys.exit(main())
