# SPDX-License-Identifier: GPL-2.0-or-later
"""Seed corpus of the F-PROBE and F-MOUNT-WALK fuzz targets (T-M3-16). Standard library only.

Usage: python3 -I -B tests/fuzz/image/gen_seeds.py [--out DIR]

Writes corpus/probe/*.bin and corpus/mountwalk/*.bin below DIR (default: the directory of this
script), the same inputs in both. An input is a recipe for an image (image.h): a base (tree,
geom, dirty), a truncation, and edits. The seeds cover the base unchanged, and for each base
one edit at a time of the places the probe and the library parse: boot sector fields, the
signature of BitLocker, MFT records (magic, fixup array, attribute offsets), the mirror,
$Volume flags, $LogFile restart pages, the hibernation signature, index blocks, and names that
are not NFC. The output is deterministic, so the committed corpus can be regenerated.
"""
import argparse
import os
import struct
import sys

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import nsb  # noqa: E402

BASES = ["tree", "geom", "dirty"]


def edit(off, data, op=0):
    """One edit at an absolute image offset (the 'uniform' form of image.h)."""
    assert 1 <= len(data) <= 64 and off < (1 << 31)
    a = 0x8000 | (off >> 16)
    b = off & 0xFFFF
    return struct.pack("<HHB", a, b, (op << 6) | (len(data) - 1)) + data


def copy_edit(off, length, src):
    """Copy `length` bytes from the start of hot sector number `src` (modulo their count)."""
    return struct.pack("<HHB", 0x8000 | (off >> 16), off & 0xFFFF, (3 << 6) | (length - 1)) + bytes([src])


def recipe(sel, edits=(), trunc=0):
    return bytes([sel]) + struct.pack("<H", trunc) + b"".join(edits)


def find_all(img, pat, limit=4):
    out, pos = [], 0
    while len(out) < limit:
        pos = img.find(pat, pos)
        if pos < 0:
            break
        out.append(pos)
        pos += 1
    return out


def seeds_for(bi, img):
    """List of (name, input bytes) for base number bi."""
    bps = struct.unpack_from("<H", img, 0x0B)[0]
    spc = img[0x0D]
    cluster = bps * spc
    mft = struct.unpack_from("<Q", img, 0x30)[0] * cluster
    mirr = struct.unpack_from("<Q", img, 0x38)[0] * cluster
    rec = 1024
    out = []
    sel = bi

    def add(name, edits=(), trunc=0, flags=0):
        out.append(("%s-%s" % (BASES[bi], name), recipe(sel + 3 * flags, edits, trunc)))

    add("plain")
    add("plain-sysfiles", flags=1)
    add("plain-case", flags=2)
    add("plain-dot", flags=4)
    add("plain-all", flags=7)
    sectors = len(img) // 512
    add("trunc-half", trunc=0xF000 | (sectors // 2 & 0xFFF))
    add("trunc-mft", trunc=0xF000 | ((sectors - mft // 512 - 8) & 0xFFF))
    add("trunc-boot", trunc=0xF000 | ((sectors - 1) & 0xFFF))

    # boot sector
    add("bps-0", [edit(0x0B, b"\x00\x00")])
    add("bps-3", [edit(0x0B, b"\x03\x00")])
    add("bps-4096-on-512", [edit(0x0B, b"\x00\x10")])
    add("spc-0", [edit(0x0D, b"\x00")])
    add("spc-128", [edit(0x0D, b"\x80")])
    add("spc-3", [edit(0x0D, b"\x03")])
    add("total-huge", [edit(0x28, b"\xff\xff\xff\xff\xff\xff\xff\x0f")])
    add("total-zero", [edit(0x28, b"\x00" * 8)])
    add("mft-lcn-far", [edit(0x30, b"\xff\xff\xff\x7f\x00\x00\x00\x00")])
    add("mft-lcn-zero", [edit(0x30, b"\x00" * 8)])
    add("mftmirr-lcn-zero", [edit(0x38, b"\x00" * 8)])
    add("rec-size-0", [edit(0x40, b"\x00")])
    add("rec-size-neg-big", [edit(0x40, b"\xe0")])
    add("rec-size-clusters-big", [edit(0x40, b"\x7f")])
    add("index-size-0", [edit(0x44, b"\x00")])
    add("oem-not-ntfs", [edit(0x03, b"NTFT")])
    add("bitlocker", [edit(0x03, b"-FVE-FS-")])
    add("boot-signature", [edit(0x1FE, b"\x00\x00")])
    add("serial-ones", [edit(0x48, b"\xff" * 8)])

    # MFT records: $MFT 0, $MFTMirr 1, $LogFile 2, $Volume 3, $AttrDef 4, root 5, $Bitmap 6,
    # $Boot 7, $Secure 9, $UpCase 10, first user files 24 and on
    for n in (0, 1, 2, 3, 4, 5, 6, 9, 10, 24, 25, 26, 30):
        base = mft + n * rec
        add("rec%d-magic" % n, [edit(base, b"BAAD")])
        add("rec%d-fixup-count" % n, [edit(base + 6, b"\xff\xff")])
        add("rec%d-first-attr" % n, [edit(base + 0x14, b"\x01\x00")])
        add("rec%d-used-size" % n, [edit(base + 0x18, b"\xff\xff\xff\x7f")])
        add("rec%d-alloc-size" % n, [edit(base + 0x1C, b"\x00\x00\x00\x00")])
        add("rec%d-flags" % n, [edit(base + 0x16, b"\xff\xff")])
        add("rec%d-attr-len" % n, [edit(base + 0x34, b"\xff\xff\xff\x7f")])
        add("rec%d-attr-ff" % n, [edit(base + 0x38, b"\xff" * 16)])
    add("mft-record-copy", [copy_edit(mft + 5 * rec, 64, 1), copy_edit(mft + 24 * rec, 64, 7)])
    add("mirror-zero", [edit(mirr, b"\x00" * 16)])
    add("mirror-stale", [edit(mirr + 0x38, b"\xff" * 8)])

    # $Volume flags: find the $VOLUME_INFORMATION attribute (0x70) of record 3
    r3 = img[mft + 3 * rec:mft + 4 * rec]
    pos = struct.unpack_from("<H", r3, 0x14)[0]
    while pos + 8 <= len(r3):
        atype, alen = struct.unpack_from("<II", r3, pos)
        if atype == 0xFFFFFFFF or alen == 0 or alen > len(r3) - pos:
            break
        if atype == 0x70:
            voff = pos + struct.unpack_from("<H", r3, pos + 0x14)[0]
            for name, val in (("dirty", 1), ("resize", 2), ("chkdsk", 0x4000), ("unknown", 0x0080),
                              ("pending", 0x0010)):
                add("volflags-" + name, [edit(mft + 3 * rec + voff + 10, struct.pack("<H", val))])
            add("version-1-0", [edit(mft + 3 * rec + voff + 8, b"\x01\x00")])
            add("version-4-9", [edit(mft + 3 * rec + voff + 8, b"\x04\x09")])
            add("version-0-0", [edit(mft + 3 * rec + voff + 8, b"\x00\x00")])
            break
        pos += alen

    # $LogFile restart pages
    for i, p in enumerate(find_all(img, b"RSTR", 2)):
        add("log%d-chkd" % i, [edit(p, b"CHKD")])
        add("log%d-version" % i, [edit(p + 0x1A, b"\x02\x00\x02\x00")])
        add("log%d-sizes" % i, [edit(p + 0x1C, b"\xff" * 12)])
        add("log%d-lsn" % i, [edit(p + 8, b"\x01\x00\x00\x00\x00\x00\x00\x00")])
        add("log%d-fixup" % i, [edit(p + 4, b"\xff\xff\xff\xff")])
    for i, p in enumerate(find_all(img, b"RCRD", 1)):
        add("rcrd%d-bad" % i, [edit(p, b"\x00\x00\x00\x00")])

    # hibernation file
    for i, p in enumerate(find_all(img, b"hibr", 1)):
        add("hiber-wake", [edit(p, b"wake")])
        add("hiber-HIBR", [edit(p, b"HIBR")])
        add("hiber-zero", [edit(p, b"\x00\x00\x00\x00")])

    # directory indexes: INDX blocks and index roots
    for i, p in enumerate(find_all(img, b"INDX", 3)):
        add("indx%d-magic" % i, [edit(p, b"BAAD")])
        add("indx%d-entries-offset" % i, [edit(p + 0x18, b"\xff\xff\xff\xff")])
        add("indx%d-entries-size" % i, [edit(p + 0x1C, b"\xff\xff\xff\x7f")])
        add("indx%d-vcn" % i, [edit(p + 0x10, b"\xff" * 8)])
        add("indx%d-first-entry" % i, [edit(p + 0x18 + 0x18, b"\xff" * 16)])

    # names: a combining mark after "e" makes a name that is not NFC; a lone surrogate; a slash
    hello = "hello.txt".encode("utf-16-le")
    for i, p in enumerate(find_all(img, hello, 3)):
        add("name%d-nfd" % i, [edit(p + 4, b"e\x00\x01\x03")])
        add("name%d-surrogate" % i, [edit(p + 4, b"\x00\xd8")])
        add("name%d-slash" % i, [edit(p + 4, b"/\x00")])
        add("name%d-nul" % i, [edit(p + 4, b"\x00\x00")])
    nfd = "café".encode("utf-16-le")
    for i, p in enumerate(find_all(img, nfd, 2)):
        add("nfd%d-composed" % i, [edit(p + 6, b"\xe9\x00\x00\x00")])
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=HERE)
    args = ap.parse_args()
    seeds = []
    for bi, name in enumerate(BASES):
        with open(os.path.join(HERE, "bases", name + ".nsb"), "rb") as f:
            img = nsb.unpack(f.read())
        seeds += seeds_for(bi, img)
    for target in ("probe", "mountwalk"):
        d = os.path.join(args.out, "corpus", target)
        os.makedirs(d, exist_ok=True)
        for old in os.listdir(d):
            if old.endswith(".bin"):
                os.remove(os.path.join(d, old))
        for name, data in seeds:
            with open(os.path.join(d, name + ".bin"), "wb") as f:
                f.write(data)
    print("%d seeds per target, %d bytes in all" % (len(seeds), sum(len(s[1]) for s in seeds)))
    return 0


if __name__ == "__main__":
    sys.exit(main())
