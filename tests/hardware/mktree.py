#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""mktree.py - the deterministic test tree and the hash check of the hardware protocol (docs/HARDWARE_TEST_PROTOCOL.md).

Python 3.6+, standard library only. It reads and writes only below the directory it is given; it never opens a
device node.

  mktree.py gen DIR [--mib N] [--seed N]   write a tree of about N MiB (default 1024) below DIR and DIR/MANIFEST.sha256
                                           (`<sha256 hex>  <relative path>`, forward slashes, sorted)
  mktree.py verify DIR MANIFEST            hash every file the manifest lists below DIR; print BAD/MISSING lines and
                                           `OK n` or `FAILED bad=b missing=m of n`; exit 0 only if all match. Files
                                           that are not listed are ignored. The manifest may come from Windows
                                           (UTF-8, with or without a byte order mark, CRLF or LF)
  mktree.py selftest                       make a small tree in a temporary directory and check gen and verify

`gen` is for a staging directory on the Mac's own disk, never on the volume under test: copy the tree to the volume
with `cp -R` (or Finder), so the copy is what is tested. Names are ASCII, and the tree holds many small files, some
medium files and a few large ones, because each of them stresses a different path (resident data, runs, MFT growth).
"""
import argparse
import hashlib
import os
import random
import sys

MIB = 1 << 20


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(4 * MIB)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def write_file(path, size, rng):
    with open(path, "wb") as f:
        left = size
        while left > 0:
            n = min(left, MIB)
            f.write(rng.getrandbits(8 * n).to_bytes(n, "little"))
            left -= n


def cmd_gen(a):
    rng = random.Random(a.seed)
    root = a.dir
    if os.path.exists(root) and os.listdir(root):
        sys.exit("mktree: %s exists and is not empty" % root)
    os.makedirs(root, exist_ok=True)
    total = a.mib * MIB
    plan = []  # (relative path, size)
    # about 5 percent in small files (0 to 64 KiB, one in ten empty), 25 in medium (1 to 16 MiB), the rest large
    small_budget = total // 20
    n = 0
    used = 0
    while used < small_budget:
        sz = 0 if n % 10 == 0 else rng.randint(1, 64 * 1024)
        plan.append(("small/d%02d/s%05d.bin" % (n % 16, n), sz))
        used += sz + 1024
        n += 1
    med_budget = total // 4
    used = 0
    n = 0
    while used < med_budget:
        sz = rng.randint(1, 16) * MIB + rng.randint(0, 4095)
        plan.append(("medium/m%04d.bin" % n, sz))
        used += sz
        n += 1
    used = sum(s for _, s in plan)
    n = 0
    while used < total:
        sz = min(total - used, rng.randint(64, 256) * MIB)
        plan.append(("large/L%02d.bin" % n, sz))
        used += sz
        n += 1
    lines = []
    for rel, sz in plan:
        p = os.path.join(root, *rel.split("/"))
        os.makedirs(os.path.dirname(p), exist_ok=True)
        write_file(p, sz, rng)
        lines.append((rel, sha256_file(p)))
    lines.sort()
    with open(os.path.join(root, "MANIFEST.sha256"), "w", encoding="utf-8", newline="\n") as f:
        for rel, h in lines:
            f.write("%s  %s\n" % (h, rel))
    print("wrote %d files, %d bytes, below %s" % (len(lines), sum(s for _, s in plan), root))
    return 0


def cmd_verify(a):
    bad = missing = n = 0
    with open(a.manifest, "r", encoding="utf-8-sig") as f:
        for line in f:
            line = line.rstrip("\r\n")
            if not line:
                continue
            h, sep, rel = line.partition("  ")
            if not sep:
                sys.exit("mktree: bad manifest line: %r" % line)
            n += 1
            p = os.path.join(a.dir, *rel.split("/"))
            if not os.path.isfile(p):
                print("MISSING %s" % rel)
                missing += 1
            elif sha256_file(p) != h.lower():
                print("BAD %s" % rel)
                bad += 1
    if n == 0:
        print("FAILED: empty manifest")
        return 1
    if bad or missing:
        print("FAILED bad=%d missing=%d of %d" % (bad, missing, n))
        return 1
    print("OK %d" % n)
    return 0


def cmd_selftest(_a):
    """gen a small tree in a temporary directory, verify it, then show that a changed file, a missing file and a
    manifest with a byte order mark and CRLF line ends are all handled."""
    import tempfile
    with tempfile.TemporaryDirectory() as t:
        root = os.path.join(t, "tree")
        ns = argparse.Namespace(dir=root, mib=3, seed=1)
        cmd_gen(ns)
        man = os.path.join(root, "MANIFEST.sha256")
        v = argparse.Namespace(dir=root, manifest=man)
        assert cmd_verify(v) == 0, "clean tree must verify"
        with open(man, "rb") as f:
            data = f.read()
        win = os.path.join(t, "win.sha256")
        with open(win, "wb") as f:
            f.write(b"\xef\xbb\xbf" + data.replace(b"\n", b"\r\n"))
        assert cmd_verify(argparse.Namespace(dir=root, manifest=win)) == 0, "BOM and CRLF manifest must verify"
        first = [l for l in data.decode().splitlines() if l][0].split("  ", 1)[1]
        p = os.path.join(root, *first.split("/"))
        with open(p, "ab") as f:
            f.write(b"x")
        assert cmd_verify(v) == 1, "a changed file must fail"
        os.unlink(p)
        assert cmd_verify(v) == 1, "a missing file must fail"
    print("selftest OK")
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd")
    g = sub.add_parser("gen")
    g.add_argument("dir")
    g.add_argument("--mib", type=int, default=1024)
    g.add_argument("--seed", type=int, default=20261120)
    v = sub.add_parser("verify")
    v.add_argument("dir")
    v.add_argument("manifest")
    sub.add_parser("selftest")
    a = ap.parse_args(argv)
    if a.cmd == "selftest":
        return cmd_selftest(a)
    if a.cmd == "gen":
        if a.mib < 1:
            ap.error("--mib must be positive")
        return cmd_gen(a)
    if a.cmd == "verify":
        return cmd_verify(a)
    ap.error("a command is needed: gen or verify")


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
