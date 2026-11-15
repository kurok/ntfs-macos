# SPDX-License-Identifier: GPL-2.0-or-later
"""Fill a mounted tiny NTFS volume with the feature mix of the fuzz bases (T-M3-16).

Usage (inside Environment A, on a volume mounted with
       -o rw,streams_interface=xattr,permissions,compression):
       python3 -I -B populate.py MOUNTPOINT full|light [--hiber]

full:  nested directories, resident and non-resident files, a sparse file, a compressed file,
       named streams (xattr interface), hard link, symlink, extended attributes, a reparse
       point, a 255 unit name, NFD and NFC names, emoji and CJK names
light: a few files and one stream (used for the larger-cluster base)
--hiber also writes /hiberfil.sys with the "hibr" signature (check 5 of the probe)
"""
import os
import struct
import sys
import unicodedata


def ea_blob(items):
    """Packed FILE_FULL_EA_INFORMATION list: [(name, value)]."""
    out = b""
    for name, value in items:
        n = name.encode()
        ent = struct.pack("<IBBH", 0, 0, len(n), len(value)) + n + b"\0" + value
        ent += b"\0" * (-len(ent) % 4)
        # ntfs-3g wants a non-zero offset to next in the last entry too
        out += struct.pack("<I", len(ent)) + ent[4:]
    return out


def reparse_symlink(target):
    """Windows symbolic link reparse data (tag 0xA000000C), relative target."""
    t = target.encode("utf-16-le")
    body = struct.pack("<HHHHI", 0, len(t), 0, len(t), 1) + t + t
    return struct.pack("<IHH", 0xA000000C, len(body), 0) + body


def put(path, data):
    d = os.path.dirname(path)
    if d:
        os.makedirs(d, exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def main(argv):
    if len(argv) < 3:
        sys.stderr.write(__doc__)
        return 2
    os.chdir(argv[1])
    full = argv[2] == "full"
    pat = bytes(range(251)) * 4

    put("hello.txt", b"hello ntfs\n")
    put("dir/data.bin", pat * 12)
    put("dir/sub/leaf.txt", b"leaf\n")
    os.setxattr("hello.txt", "user.note", b"stream payload")
    if not full:
        put("dir/more.bin", pat * 3)
        if "--hiber" in argv:
            put("hiberfil.sys", b"hibr" + b"\0" * 8188)
        return 0

    put("empty.txt", b"")
    put("long/" + "n" * 255, b"long name\n")
    put("deep/a/b/c/d/e/f/g/h/leaf", b"deep\n")
    put("unicode/" + unicodedata.normalize("NFD", "café") + "-nfd.txt", b"nfd\n")
    put("unicode/" + unicodedata.normalize("NFC", "café") + "-nfc.txt", b"nfc\n")
    put("unicode/\U0001F600-emoji.txt", b"emoji\n")
    put("unicode/漢字.txt", b"cjk\n")
    put("unicode/Case.TXT", b"upper\n")
    put("unicode/case.txt", b"lower\n")
    put("big.bin", pat * 30)
    put("many/" + "x", b"x")
    for i in range(24):
        put("many/f%02d.dat" % i, bytes([i]) * (i * 97))

    # sparse file
    with open("sparse.bin", "wb") as f:
        f.truncate(300000)
        f.seek(100000)
        f.write(b"sparse tail" * 20)
    # compressed file: the directory flag is inherited by new files
    os.makedirs("comp", exist_ok=True)
    os.setxattr("comp", "system.ntfs_attrib_be", struct.pack(">I", 0x10 | 0x800))
    put("comp/text.txt", (b"compress me " * 400))
    # named streams
    os.setxattr("hello.txt", "user.second", b"second stream")
    os.setxattr("dir/data.bin", "user.big", b"s" * 3000)
    os.setxattr("dir", "user.dirstream", b"on a directory")
    # links
    os.link("hello.txt", "dir/hard-link")
    os.symlink("hello.txt", "sym")
    os.symlink("../dir/sub", "dir/dsym")
    # extended attributes and reparse data
    os.setxattr("big.bin", "system.ntfs_ea", ea_blob([("EAONE", b"value one"), ("EATWO", b"2")]))
    put("rp.txt", b"")
    try:
        os.setxattr("rp.txt", "system.ntfs_reparse_data", reparse_symlink("hello.txt"))
    except OSError as e:
        sys.stderr.write("populate: reparse data refused: %s\n" % e)
    # attributes: hidden, system, readonly
    os.setxattr("empty.txt", "system.ntfs_attrib_be", struct.pack(">I", 0x27))
    if "--hiber" in argv:
        put("hiberfil.sys", b"hibr" + b"\0" * 8188)
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv))
