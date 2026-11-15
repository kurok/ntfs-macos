# SPDX-License-Identifier: GPL-2.0-or-later
"""Sparse base image format of the F-PROBE and F-MOUNT-WALK fuzz harnesses (T-M3-16).

Usage: python3 -I -B tests/fuzz/image/nsb.py pack RAW.img OUT.nsb
       python3 -I -B tests/fuzz/image/nsb.py unpack IN.nsb OUT.img

A tiny NTFS image is mostly zeros and 0xFF fill ($LogFile), so it is stored as a list of
extents. Format (all integers little endian):
  "NSB1"                  magic
  u64 size                size of the image in bytes
  records, until the end of the file:
    u64 offset            multiple of 512
    u32 length            multiple of 512, at least 512
    u8  kind              0: the bytes follow; 1: every byte is `fill`
    u8  fill              the fill byte (kind 1), else 0
    bytes[length]         kind 0 only
Bytes not covered by a record are zero. Records are sorted and do not overlap. The loader
in image.c checks all of this: a base is as untrusted as any other input of the harness.
"""
import struct
import sys

SECTOR = 512


def pack(raw):
    """Return the NSB1 form of the bytes `raw` (length a multiple of 512)."""
    if len(raw) % SECTOR:
        raise ValueError("image size is not a multiple of 512")
    out = bytearray(b"NSB1" + struct.pack("<Q", len(raw)))
    recs = []  # [kind, fill, first sector, count]
    for i in range(len(raw) // SECTOR):
        sec = raw[i * SECTOR:(i + 1) * SECTOR]
        if sec.count(sec[0]) == SECTOR:
            kind, fill = (None, 0) if sec[0] == 0 else (1, sec[0])
        else:
            kind, fill = 0, 0
        if kind is None:
            continue
        if recs and recs[-1][0] == kind and recs[-1][1] == fill and recs[-1][2] + recs[-1][3] == i:
            recs[-1][3] += 1
        else:
            recs.append([kind, fill, i, 1])
    for kind, fill, first, count in recs:
        out += struct.pack("<QIBB", first * SECTOR, count * SECTOR, kind, fill)
        if kind == 0:
            out += raw[first * SECTOR:(first + count) * SECTOR]
    return bytes(out)


def unpack(blob):
    """Return the image bytes of an NSB1 blob."""
    if blob[:4] != b"NSB1":
        raise ValueError("bad magic")
    size = struct.unpack_from("<Q", blob, 4)[0]
    img = bytearray(size)
    pos = 12
    while pos < len(blob):
        off, length, kind, fill = struct.unpack_from("<QIBB", blob, pos)
        pos += 14
        if kind == 0:
            img[off:off + length] = blob[pos:pos + length]
            pos += length
        else:
            img[off:off + length] = bytes([fill]) * length
    return bytes(img)


def main(argv):
    if len(argv) != 4 or argv[1] not in ("pack", "unpack"):
        sys.stderr.write(__doc__)
        return 2
    with open(argv[2], "rb") as f:
        data = f.read()
    with open(argv[3], "wb") as f:
        f.write(pack(data) if argv[1] == "pack" else unpack(data))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
