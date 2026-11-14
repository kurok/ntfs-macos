# SPDX-License-Identifier: GPL-2.0-or-later
"""I-FUNC-DIR-01..03 and I-INT-CYCLE-01 (T-M3-05, IMPLEMENTATION_PLAN 3.3).

I-FUNC-DIR-01   nesting depth 32
I-FUNC-DIR-02   10,000 entries in one directory
I-FUNC-DIR-03   rename a directory with its contents (same parent, other parent)
I-INT-CYCLE-01  200 overwrite cycles on 50 files, hash checks after remounts in between
"""

import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fsops  # noqa: E402
from fsops import blob, expect_errno, model_files, put, remount_check, sha, slurp  # noqa: E402
from harness import Fail, case  # noqa: E402
from harness.seeded import compare, hash_tree  # noqa: E402

KIB = 1024
MIB = KIB * KIB


@case("I-FUNC-DIR-01", envs=("A", "B"), title="nested directories, depth 32, files on every level, bottom-up removal")
def i_func_dir_01(ctx):
    vol = fsops.open_volume(ctx, 128)
    fsops.mount(ctx, vol)
    m = vol.mnt
    depth = 32
    model, dirs = {}, []
    rel = ""
    for level in range(1, depth + 1):
        rel = (rel + "/" if rel else "") + "lvl%02d" % level
        os.mkdir(os.path.join(m, rel))
        dirs.append(rel)
        f = rel + "/file%02d.dat" % level
        model[f] = blob("dir01:%d" % level, 1000 + level * 129)
        put(os.path.join(m, f), model[f])
    leaf = rel
    ctx.log("deepest directory: %s (%d characters relative)" % (leaf, len(leaf)))
    if os.path.relpath(leaf, ".").count("/") != depth - 1:
        raise Fail("depth is not %d" % depth)
    # Reach the leaf by the full path and by descending with chdir.
    if slurp(os.path.join(m, leaf, "file32.dat")) != model[leaf + "/file32.dat"]:
        raise Fail("leaf file differs")
    cwd = os.getcwd()
    try:
        os.chdir(m)
        for level in range(1, depth + 1):
            os.chdir("lvl%02d" % level)
        if slurp("file32.dat") != model[leaf + "/file32.dat"]:
            raise Fail("leaf file via chdir differs")
        os.chdir("..")
        expect_errno(ctx, ("ENOTEMPTY", "EEXIST"), os.rmdir, "lvl32", desc="rmdir of the non-empty leaf")
    finally:
        os.chdir(cwd)
    remount_check(ctx, vol, model_files(model), dirs, "I-FUNC-DIR-01 (before removal)")
    # Remove bottom-up on a read/write mount, then the volume must be empty.
    fsops.mount(ctx, vol)
    parts = leaf.split("/")
    for level in range(depth, 0, -1):
        d = "/".join(parts[:level])
        os.unlink(os.path.join(m, d, "file%02d.dat" % level))
        os.rmdir(os.path.join(m, d))
    left = [n for n in os.listdir(m) if n != ".fseventsd"]
    if left:
        raise Fail("volume not empty after bottom-up removal: %s" % left[:5])
    remount_check(ctx, vol, {}, [], "I-FUNC-DIR-01 (after removal)")


@case("I-FUNC-DIR-02", envs=("A", "B"), title="10,000 entries in one directory: list, stat, hash after remount, delete half")
def i_func_dir_02(ctx):
    vol = fsops.open_volume(ctx, 192)
    fsops.mount(ctx, vol)
    m = vol.mnt
    big = os.path.join(m, "big")
    os.mkdir(big)
    n = 10000

    def content(i):
        return ("entry %05d " % i).encode() * (1 + i % 7)

    t0 = time.monotonic()
    model = {}
    for i in range(n):
        name = "e%05d.txt" % i
        with open(os.path.join(big, name), "wb") as f:
            f.write(content(i))
        model["big/" + name] = content(i)
    ctx.log("created %d files in %.1f s" % (n, time.monotonic() - t0))
    names = sorted(os.listdir(big))
    if len(names) != n or names != sorted(k[4:] for k in model):
        raise Fail("listing has %d names, expected %d (or the names differ)" % (len(names), n))
    seen = 0
    for e in os.scandir(big):
        if not e.is_file() or e.stat().st_size != len(model["big/" + e.name]):
            raise Fail("scandir entry %s has the wrong type or size" % e.name)
        seen += 1
    if seen != n:
        raise Fail("scandir yielded %d entries" % seen)
    for i in (0, 1, 4999, 5000, 9999):
        if slurp(os.path.join(big, "e%05d.txt" % i)) != content(i):
            raise Fail("e%05d.txt differs on the live mount" % i)
    remount_check(ctx, vol, model_files(model), ["big"], "I-FUNC-DIR-02 (10,000 entries)")

    # Delete every other entry and rename a hundred; check again.
    fsops.mount(ctx, vol)
    for i in range(0, n, 2):
        os.unlink(os.path.join(big, "e%05d.txt" % i))
        del model["big/e%05d.txt" % i]
    for i in range(1, 200, 2):
        old = "e%05d.txt" % i
        new = "r%05d.txt" % i
        os.rename(os.path.join(big, old), os.path.join(big, new))
        model["big/" + new] = model.pop("big/" + old)
    if len(os.listdir(big)) != n // 2:
        raise Fail("listing has %d names after the deletes, expected %d" % (len(os.listdir(big)), n // 2))
    remount_check(ctx, vol, model_files(model), ["big"], "I-FUNC-DIR-02 (5,000 entries)")


@case("I-FUNC-DIR-03", envs=("A", "B"), title="rename a directory with contents: same parent, other parent, tree unchanged")
def i_func_dir_03(ctx):
    vol = fsops.open_volume(ctx, 128)
    fsops.mount(ctx, vol)
    m = vol.mnt
    j = lambda *a: os.path.join(m, *a)  # noqa: E731
    model, dirs = {}, set()
    for a in range(4):
        for b in range(3):
            d = "tree/a%d/b%d" % (a, b)
            os.makedirs(j(d))
            dirs.update(("tree", "tree/a%d" % a, d))
            for k in range(8):
                f = "%s/f%d.bin" % (d, k)
                model[f] = blob("dir03:" + f, 100 + a * 1000 + b * 100 + k * 7)
                put(j(f), model[f])
    os.mkdir(j("empty-in-tree"))
    os.rename(j("empty-in-tree"), j("tree/empty-in-tree"))
    dirs.add("tree/empty-in-tree")
    before = hash_tree(m)
    if compare(model_files(model), before):
        raise Fail("tree differs from the model before the rename")

    def renamed(prefix_from, prefix_to):
        nonlocal model, dirs
        model = dict(((prefix_to + k[len(prefix_from):]) if k.startswith(prefix_from + "/") else k, v)
                     for k, v in model.items())
        dirs = set((prefix_to + d[len(prefix_from):]) if d == prefix_from or d.startswith(prefix_from + "/") else d
                   for d in dirs)

    os.rename(j("tree"), j("tree2"))
    renamed("tree", "tree2")
    expect_errno(ctx, ("ENOENT",), os.stat, j("tree"), desc="old directory name")
    after = hash_tree(m)
    if compare(dict((k.replace("tree/", "tree2/", 1), v) for k, v in before.items()), after):
        raise Fail("tree content changed by renaming its top directory")
    os.mkdir(j("other"))
    dirs.add("other")
    os.rename(j("tree2/a1"), j("other/moved-a1"))
    renamed("tree2/a1", "other/moved-a1")
    os.rename(j("other/moved-a1/b2"), j("tree2/a0/b-from-a1"))
    renamed("other/moved-a1/b2", "tree2/a0/b-from-a1")
    actual = hash_tree(m)
    bad = compare(model_files(model), actual)
    if bad:
        raise Fail("live tree differs from the model after the moves: %s" % bad[0])
    remount_check(ctx, vol, model_files(model), dirs, "I-FUNC-DIR-03")


@case("I-INT-CYCLE-01", envs=("A", "B"), title="200 overwrite cycles on 50 files with remounts, hash checks, ntfsfix")
def i_int_cycle_01(ctx):
    nfiles, cycles, remount_every = 50, 200, 50
    vol = fsops.open_volume(ctx, 192)
    fsops.mount(ctx, vol)
    m = vol.mnt
    model = {}
    total = 0
    t0 = time.monotonic()
    for c in range(cycles):
        for i in range(nfiles):
            rel = "cyc%02d/f%02d.bin" % (i % 5, i)
            # Sizes move up and down between cycles: 1 KiB .. about 190 KiB, some below a cluster.
            size = 1 + ((c * 7919 + i * 104729) % 190) * KIB + (c * 31 + i) % 1024
            data = blob("cycle:%d:%d" % (c, i), size)
            path = os.path.join(m, rel)
            if c == 0 and i < 5:
                os.mkdir(os.path.join(m, "cyc%02d" % i))
            if c % 3 == 2 and rel in model and model[rel][0] == size:
                with open(path, "r+b") as f:  # same size, in place
                    f.write(data)
            else:
                with open(path, "wb") as f:  # O_TRUNC then write
                    fsops.write_chunks(f, data, 65521)
            model[rel] = (size, sha(data))
            total += size
        # Read back a rotating subset on the live mount.
        for i in range(c % 10, nfiles, 10):
            rel = "cyc%02d/f%02d.bin" % (i % 5, i)
            got = slurp(os.path.join(m, rel))
            if (len(got), sha(got)) != model[rel]:
                raise Fail("cycle %d: %s differs on the live mount" % (c, rel))
        if (c + 1) % remount_every == 0:
            ctx.log("cycle %d done, %.1f MiB written so far, %.0f s" % (c + 1, total / MIB, time.monotonic() - t0))
            remount_check(ctx, vol, model, ["cyc%02d" % i for i in range(5)], "after cycle %d" % (c + 1))
            if c + 1 < cycles:
                fsops.mount(ctx, vol)
    ctx.log("%d overwrites, %.1f MiB written in %.0f s" % (cycles * nfiles, total / MIB, time.monotonic() - t0))
