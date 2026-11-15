# SPDX-License-Identifier: GPL-2.0-or-later
"""Generates tests/compatibility/ref/ops-01.json .. ops-12.json, the twelve operation scripts of the
reference comparison (T-M3-15, IMPLEMENTATION_PLAN criterion 3.10, TEST_STRATEGY section 5).

  python3 make_ops.py           write the files
  python3 make_ops.py --check   exit 1 if a file on disk differs from what this script produces

A script is a JSON document: {"id", "name", "title", "image_mib", "ops": [...], "ed_ops", "ed_paths"}.
Every op is deterministic (content comes from a text seed) and never depends on wall-clock time.
Op kinds are listed in reflib.py (replay). `ed_ops` maps an op index to the id of an expected
difference in expected-differences.md that explains a different result code in B; `ed_paths` maps a
path to the list of expected-difference ids that may explain a difference in the dump of that path.
"""

import json
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
MIB = 1 << 20
NS = 1000000000


def mk(p):
    return {"op": "mkdir", "path": p}


def create(p, size, seed=None, chunk=7919):
    return {"op": "create", "path": p, "size": size, "seed": seed or p, "chunk": chunk}


def write(p, off, size, seed, chunk=7919):
    return {"op": "write", "path": p, "off": off, "size": size, "seed": seed, "chunk": chunk}


def append(p, size, seed, chunk=7919):
    return {"op": "append", "path": p, "size": size, "seed": seed, "chunk": chunk}


def trunc(p, size):
    return {"op": "truncate", "path": p, "size": size}


def ren(a, b):
    return {"op": "rename", "src": a, "dst": b}


def unl(p):
    return {"op": "unlink", "path": p}


def rmd(p):
    return {"op": "rmdir", "path": p}


def xset(p, name, size, seed=None):
    return {"op": "setxattr", "path": p, "name": name, "size": size, "seed": seed or (p + name)}


def utime(p, atime_ns, mtime_ns):
    return {"op": "utime", "path": p, "atime_ns": atime_ns, "mtime_ns": mtime_ns}


def ops_01():
    o = [
        mk("w"),
        create("w/small", 100),
        create("w/medium", 70000, chunk=4097),
        {"op": "create", "path": "w/zero", "size": 0, "seed": "z"},
        {"op": "create", "path": "w/exact-cluster", "size": 4096, "seed": "ec"},
        {"op": "create", "path": "w/cluster-plus-1", "size": 4097, "seed": "ec1"},
        write("w/small", 4900, 100, "grow"),  # resident file becomes non-resident
        append("w/small", 300, "a1"),
        append("w/small", 300, "a2"),
        append("w/small", 4096, "a3"),
        trunc("w/medium", 5000),
        trunc("w/medium", 100000),
        write("w/medium", 99990, 4000, "tail"),
        create("w/medium", 12345, seed="rewrite"),  # O_TRUNC rewrite
        write("w/medium", 4000, 300, "middle"),
        append("w/medium", MIB + 3, "big-append", chunk=65536),
        trunc("w/medium", 0),
        create("w/medium", 30, seed="after-zero"),
        write("w/zero", 0, 1, "one"),
        trunc("w/zero", 0),
        write("w/exact-cluster", 4096, 4096, "second-cluster"),
        trunc("w/cluster-plus-1", 4096),
        {"op": "fsync", "path": "w/small"},
        mk("w/sub"),
        create("w/sub/nested", 33333, chunk=1000),
        write("w/sub/nested", 10, 20000, "overlap", chunk=333),
        trunc("w/sub/nested", 1),
        create("w/odd-sizes", 511),
        append("w/odd-sizes", 513, "o1"),
        append("w/odd-sizes", 1, "o2"),
    ]
    return o


def ops_02():
    return [
        mk("a"), mk("b"), mk("a/sub"), mk("b/empty-target"),
        create("a/x", 5000), create("a/sub/y", 70000), create("b/z", 123), create("b/over", 4444),
        ren("a/x", "b/x"),                      # move into another directory
        ren("a/sub/y", "b/z"),                  # over an existing file
        ren("a/sub", "b/sub2"),                 # directory move
        ren("b/sub2", "b/empty-target"),        # directory over an empty directory
        create("b/keep", 10),
        ren("b/keep", "b/keep with space"),
        ren("b/keep with space", "b/Keep"),     # case only
        ren("b/Keep", "b/keep"),
        mk("c"), create("c/f1", 200), create("c/f2", 70000),
        ren("c", "d"),                          # directory with content
        ren("d/f1", "d/f1.renamed"),
        ren("b/over", "b/over"),                # same name
        ren("nonexistent", "b/nope"),           # error: ENOENT
        ren("a", "b"),                          # error: ENOTEMPTY (b is not empty)
        ren("b/x", "a/sub"),                    # file over directory: EISDIR
        mk("e"), mk("e/e1"), mk("e/e1/e2"), create("e/e1/e2/leaf", 6000),
        ren("e/e1/e2/leaf", "leaf-up"),
        ren("e/e1", "e/e1-renamed"),
    ]


def ops_03():
    o = [mk("del"), mk("del/many")]
    o.append({"op": "many", "dir": "del/many", "fmt": "f%04d", "count": 200, "min": 0, "mod": 3000, "seed": "m"})
    o += [
        {"op": "unlink_many", "dir": "del/many", "fmt": "f%04d", "start": 0, "stop": 200, "step": 2},
        create("del/gone", 9000), unl("del/gone"),
        create("del/recreated", 100, seed="first"), unl("del/recreated"), create("del/recreated", 7000, seed="second"),
        create("del/file-then-dir", 10), unl("del/file-then-dir"), mk("del/file-then-dir"),
        mk("del/empty"), rmd("del/empty"),
        mk("del/tree"), mk("del/tree/a"), mk("del/tree/a/b"), create("del/tree/a/b/f", 2000), create("del/tree/g", 50),
        rmd("del/tree"),                        # error: ENOTEMPTY
        unl("del/tree/a"),                      # error: EPERM or EISDIR
        unl("del/tree/a/b/f"), rmd("del/tree/a/b"), rmd("del/tree/a"),
        unl("del/nonexistent"),                 # error: ENOENT
        rmd("del/nonexistent"),                 # error: ENOENT
        create("del/big", 2 * MIB, chunk=65536), unl("del/big"), create("del/after-big", 3 * MIB, chunk=65536),
        create("del/zero", 0), unl("del/zero"),
    ]
    return o


def ops_04():
    o = [mk("deep")]
    o.append({"op": "deep", "base": "deep", "depth": 40, "name": "level%02d", "file": "leaf.txt", "fsize": 1500,
              "seed": "deep", "every": 10})
    o.append(mk("wide"))
    o.append({"op": "many", "dir": "wide", "fmt": "file-%04d.dat", "count": 300, "min": 0, "mod": 2500, "seed": "w"})
    for i in range(30):
        o.append(mk("wide/sub%02d" % i))
        o.append({"op": "many", "dir": "wide/sub%02d" % i, "fmt": "n%02d", "count": 5, "min": 1, "mod": 900,
                  "seed": "sub%d" % i})
    o.append(mk("wide/" + "n" * 200))                    # a long directory name
    o.append(create("wide/" + "n" * 200 + "/" + "m" * 255, 77))   # a 255-character file name
    return o


def ops_05():
    names = [
        "caf\u00e9.txt",                                  # NFC
        "\u65e5\u672c\u8a9e/\u0444\u0430\u0439\u043b.bin",   # CJK directory, Cyrillic file
        "emoji \U0001F642.bin",
        "family \U0001F468\u200d\U0001F469\u200d\U0001F467.txt",   # ZWJ sequence
        "flag \U0001F1FA\U0001F1E6.txt",
        "\ud55c\uad6d\uc5b4.txt",                            # Hangul
        "\u05e9\u05dc\u05d5\u05dd.txt",                      # Hebrew
        "\u0645\u0631\u062d\u0628\u0627.txt",                # Arabic
        "\u03b5\u03bb\u03bb\u03b7\u03bd\u03b9\u03ba\u03ac.txt",   # Greek
        "stra\u00dfe.txt",
        "with space inside.txt",
        " leading space",
        "dots.in.name...txt",
        "UPPER", "upper", "Upper",                        # case-sensitive volume
        "\u00c5.txt",       # U+00C5 in NFC
        "\u212b-angstrom.txt",                            # ANGSTROM SIGN: NFC maps it to U+00C5
        "x" * 255,                                        # 255 UTF-16 units
        "\u65e5" * 85,                                    # 85 CJK characters, 255 bytes
        "\U0001F642" * 126,                               # 126 emoji, 252 UTF-16 units
    ]
    o = [mk("uni"), mk("uni/\u65e5\u672c\u8a9e")]
    for i, n in enumerate(names):
        o.append(create("uni/" + n, 100 + 1000 * i, seed="u%d" % i))
    o.append(mk("uni/\u00fcber dir"))
    o.append(create("uni/\u00fcber dir/in\u00fcber.txt", 4321, seed="ud"))
    o.append(ren("uni/caf\u00e9.txt", "uni/caf\u00e9-renamed.txt"))
    # Names typed in NFD: B stores NFC (ED-02).
    nfd = "re\u0301sume\u0301.txt"
    o.append(create("uni/" + nfd, 555, seed="nfd1"))
    o.append(mk("uni/nfd-dir-u\u0308"))
    o.append(create("uni/nfd-dir-u\u0308/file", 66, seed="nfd2"))
    # Names the product's windows_names option refuses: both sides must refuse them the same way.
    for bad in ("a:b", "q?", "trail.", "trail ", "CON", "nul.txt", "a<b", "back\\slash", "tab\tname", "pipe|"):
        o.append({"op": "create", "path": "uni/" + bad, "size": 1, "seed": "bad", "chunk": 1})
    return o


def ops_06():
    o = [mk("x"), create("x/file", 5000), create("x/second", 100), mk("x/dir"), create("x/dir/in", 10)]
    o += [
        xset("x/file", "user.k", 2),
        xset("x/file", "user.empty", 0),
        xset("x/file", "user.big", 64 * 1024),
        xset("x/file", "user.mid", 5000),
        xset("x/file", "com.apple.quarantine", 57),
        xset("x/file", "com.apple.FinderInfo", 32),
        xset("x/file", "com.apple.ResourceFork", 3000),
        xset("x/file", "com.apple.lastuseddate#PS", 16),
        xset("x/file", "com.apple.metadata:kMDItemFinderComment", 7),     # ':' in the name (patch 0009)
        xset("x/file", "user.na\u00efve", 5),                              # non-ASCII name, typed in NFC
        xset("x/second", "user.k", 2, seed="second"),
        xset("x/dir", "user.dk", 10),
        xset("x/dir", "com.apple.FinderInfo", 32, seed="dirfi"),
        {"op": "removexattr", "path": "x/file", "name": "user.mid"},
        {"op": "removexattr", "path": "x/file", "name": "user.absent"},   # error: no such attribute
        xset("x/file", "user.k", 1, seed="shorter"),                      # replace by a shorter value
        xset("x/file", "user.huge", 100000),                              # above the Linux xattr limit (ED-06)
        append("x/file", 100, "data-after-xattr"),
        ren("x/second", "x/dir/second-moved"),
        mk("x/y"), create("x/y/f", 4), xset("x/y/f", "user.t", 3),
        unl("x/y/f"), create("x/y/f", 4, seed="again"),                   # a recreated file has no streams
    ]
    return o


T2000 = 946684800 * NS               # 2000-01-01
T_FRAC = T2000 + 123456789           # fraction below 100 ns, stored floored
T1950 = -631152000 * NS + 250000000  # 1950-01-01 00:00:00.25: negative fraction case
T2106 = 4294967296 * NS + 5 * NS     # past 32 bits
T2200 = 7258118400 * NS              # 2200-01-01


def ops_07():
    return [
        mk("t"), create("t/f1", 1000), create("t/f2", 2000), create("t/f3", 3000), create("t/f4", 4000),
        create("t/f5", 5000), mk("t/d1"), mk("t/d2"), create("t/d1/inner", 10),
        utime("t/f1", T2000, T2000),
        utime("t/f2", T_FRAC, T_FRAC + 1000000000),
        utime("t/f3", T1950, T1950),
        utime("t/f4", T2106, T2106 + 7 * NS),
        utime("t/f5", T2200, T2200),
        write("t/f1", 10, 10, "touch-after"),      # a write moves mtime (not compared: wall clock)
        utime("t/f1", T2000 + 5 * NS, T2000 + 9 * NS),
        # rename keeps the times of the file
        create("t/moved", 100), utime("t/moved", T2000 + 3 * NS, T2000 + 4 * NS), ren("t/moved", "t/d2/moved"),
        # set only both times: the same value for both
        create("t/same", 50), utime("t/same", T_FRAC, T_FRAC),
        # directories last: any entry change moves the directory mtime
        utime("t/d1", T2000, T_FRAC),
        utime("t/d2", T2106, T1950),
        utime("t", T2000 + 77 * NS, T2000 + 99 * NS),
    ]


def ops_08():
    h = 1 * MIB + 4099
    return [
        mk("s"),
        # a sparse file is made with ftruncate first
        {"op": "create", "path": "s/hole-end", "size": 0, "seed": "e"},
        trunc("s/hole-end", 64 * MIB),
        write("s/hole-end", 0, h, "he", chunk=65536),
        {"op": "create", "path": "s/hole-middle", "size": 0, "seed": "m"},
        trunc("s/hole-middle", 64 * MIB),
        write("s/hole-middle", 32 * MIB + 17, h, "hm", chunk=65536),
        {"op": "create", "path": "s/hole-start", "size": 0, "seed": "s"},
        trunc("s/hole-start", 64 * MIB),
        write("s/hole-start", 64 * MIB - h, h, "hs", chunk=65536),
        {"op": "create", "path": "s/all-hole", "size": 0, "seed": "a"},
        trunc("s/all-hole", 64 * MIB),
        # a small file grown by truncate, then data written into the new tail
        create("s/grown", 30, seed="g"),
        trunc("s/grown", 10 * MIB),
        write("s/grown", 5 * MIB, 5000, "gt"),
        trunc("s/grown", 2 * MIB),
        # write into a hole, overwrite data
        write("s/hole-end", 20 * MIB, 100 * 1024 + 3, "into-hole", chunk=65536),
        write("s/hole-end", 1000, 5000, "overwrite"),
        # zeros written into data stay data
        {"op": "writezero", "path": "s/hole-start", "off": 64 * MIB - 65536, "size": 65536},
        # a write past EOF without a preceding truncate is dense in B (ED-05)
        {"op": "create", "path": "s/dense", "size": 0, "seed": "d"},
        write("s/dense", 24 * MIB, 1, "pastEOF"),
        # truncate down into the data
        trunc("s/hole-middle", 40 * MIB),
    ]


def ops_09():
    return [
        mk("big"),
        create("big/file", 160 * MIB, seed="big", chunk=MIB),
        write("big/file", 100 * MIB + 7, 3 * MIB, "ow", chunk=MIB + 13),
        append("big/file", 5 * MIB + 13, "ap", chunk=MIB),
        trunc("big/file", 120 * MIB),
        create("big/second", 50 * MIB, seed="s2", chunk=MIB),
        create("big/third", 10 * MIB + 1, seed="s3", chunk=999983),
        unl("big/second"),
        create("big/after", 20 * MIB, seed="aft", chunk=MIB),
        {"op": "fsync", "path": "big/file"},
    ]


def ops_10():
    return [
        mk("h"), mk("h/dir"),
        create("h/f", 5000),
        {"op": "link", "src": "h/f", "dst": "h/l1"},
        {"op": "link", "src": "h/f", "dst": "h/dir/l2"},
        append("h/l1", 3000, "via-l1"),
        write("h/dir/l2", 100, 200, "via-l2"),
        unl("h/f"),
        {"op": "link", "src": "h/l1", "dst": "h/l3"},
        create("h/g", 70000, seed="g"),
        {"op": "link", "src": "h/g", "dst": "h/g-link"},
        unl("h/g-link"),
        {"op": "link", "src": "h/dir", "dst": "h/dirlink"},        # error: EPERM (directory)
        {"op": "link", "src": "h/nonexistent", "dst": "h/zzz"},    # error: ENOENT
        {"op": "link", "src": "h/g", "dst": "h/l1"},               # error: EEXIST
        xset("h/l3", "user.shared", 9),
        trunc("h/dir/l2", 100),
    ]


def ops_11():
    return [
        mk("sl"), mk("sl/dir"), create("sl/target", 4000), create("sl/dir/inner", 300),
        {"op": "symlink", "target": "target", "path": "sl/to-file"},
        {"op": "symlink", "target": "dir", "path": "sl/to-dir"},
        {"op": "symlink", "target": "missing", "path": "sl/dangling"},
        {"op": "symlink", "target": "../sl/target", "path": "sl/dir/up"},
        {"op": "symlink", "target": "/absolute/elsewhere", "path": "sl/abs"},
        {"op": "symlink", "target": "t" * 200, "path": "sl/long"},
        {"op": "symlink", "target": "caf\u00e9-target", "path": "sl/uni-target"},
        {"op": "symlink", "target": "target", "path": "sl/\u65e5\u672c-link"},
        {"op": "symlink", "target": "target", "path": "sl/to-file"},     # error: EEXIST
        append("sl/to-file", 100, "through-link"),                       # write through the link
        {"op": "unlink", "path": "sl/dangling"},
        {"op": "symlink", "target": "dir/inner", "path": "sl/dangling"},
        {"op": "rename", "src": "sl/to-dir", "dst": "sl/to-dir-renamed"},
        mk("sl/real"), create("sl/real/f", 77), {"op": "symlink", "target": "real", "path": "sl/real-link"},
        {"op": "unlink", "path": "sl/real-link"},
    ]


def ops_12():
    return [
        mk("cdir"), mk("plain"),
        {"op": "chattr", "path": "cdir", "or": 0x800},             # FILE_ATTRIBUTE_COMPRESSED
        {"op": "text", "path": "cdir/text", "size": 2 * MIB, "label": "zt"},
        create("cdir/random", MIB, seed="rand", chunk=65536),
        create("cdir/small", 100),
        create("cdir/one", 1, seed="one"),
        create("cdir/empty", 0),
        {"op": "text", "path": "cdir/big-text", "size": 8 * MIB, "label": "zb"},
        mk("cdir/sub"),
        {"op": "text", "path": "cdir/sub/nested", "size": 65536, "label": "zn"},
        {"op": "text", "path": "plain/text", "size": MIB, "label": "pt"},
        write("cdir/text", MIB, 70000, "upd"),
        append("cdir/text", 5000, "app"),
        trunc("cdir/text", MIB + 17),
        trunc("cdir/text", 3 * MIB),
        write("cdir/small", 5000, 100, "sm"),
        ren("cdir/one", "cdir/sub/one-moved"),
        unl("cdir/random"),
        {"op": "text", "path": "cdir/after-delete", "size": 300000, "label": "ad"},
        {"op": "chattr", "path": "plain/text", "or": 0x800},       # mark an existing plain file
        append("plain/text", 100, "after-mark"),
    ]


SPECS = [
    (1, "write-append-truncate", "create, write at offset, append, truncate, rewrite, resident to non-resident", 64, ops_01, {}, {}),
    (2, "rename-move", "rename and move of files and directories, over existing names, errors", 64, ops_02, {}, {}),
    (3, "delete", "unlink, rmdir, recreate, delete half of a wide directory, errors", 64, ops_03,
     {"20": "ED-09"}, {}),
    (4, "deep-wide-dirs", "40 levels deep, 300 files wide, 30 subdirectories, 200 and 255 character names", 96, ops_04, {}, {}),
    (5, "unicode-names", "names in many scripts, case pairs, NFD-typed names, names refused by windows_names", 64, ops_05, {}, {}),
    (6, "xattr-streams", "xattrs as NTFS named streams: user.*, com.apple.*, ':' names, large values, replace, remove", 64, ops_06,
     {"13": "ED-08", "21": "ED-06"}, {"x/file": ["ED-06", "ED-08"]}),
    (7, "timestamps", "explicit mtime and atime: 1950, 2000, sub-100 ns fraction, past 2106, 2200, on files and directories", 64, ops_07, {}, {}),
    (8, "sparse", "sparse files made by ftruncate, holes at start, middle, end, whole; dense write past EOF", 96, ops_08,
     {}, {"s/dense": ["ED-05"], "s/hole-start": ["ED-07"], "s/hole-middle": ["ED-07"], "s/hole-end": ["ED-07"]}),
    (9, "large-file", "160 MiB file written, overwritten, appended, truncated; 50 MiB file made and deleted", 320, ops_09, {}, {}),
    (10, "hard-link", "hard links in one directory and across directories, write through a link, unlink the first name", 64, ops_10,
     {"3": "ED-04", "4": "ED-04", "8": "ED-04", "10": "ED-04"}, {}),
    (11, "symlink", "symbolic links to a file, a directory, dangling, absolute, long, Unicode; write through a link", 64, ops_11, {}, {}),
    (12, "compressed-dir", "compressed directory: files made, updated, truncated, moved inside it; a plain file marked afterwards", 96, ops_12, {}, {}),
]


def build():
    out = {}
    for n, name, title, mib, fn, ed_ops, ed_paths in SPECS:
        out["ops-%02d.json" % n] = {
            "id": "C-REF-OP-%02d" % n, "name": name, "title": title, "image_mib": mib,
            "ops": fn(), "ed_ops": ed_ops, "ed_paths": ed_paths,
        }
    return out


def render(doc):
    return json.dumps(doc, indent=1, ensure_ascii=True, sort_keys=True) + "\n"


def main(argv):
    docs = build()
    if "--check" in argv:
        bad = 0
        for fn, doc in sorted(docs.items()):
            p = os.path.join(HERE, fn)
            try:
                with open(p, "r", encoding="utf-8") as f:
                    cur = f.read()
            except OSError:
                cur = None
            if cur != render(doc):
                sys.stderr.write("%s is out of date\n" % fn)
                bad += 1
        return 1 if bad else 0
    for fn, doc in sorted(docs.items()):
        with open(os.path.join(HERE, fn), "w", encoding="utf-8") as f:
            f.write(render(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
