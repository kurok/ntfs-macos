# SPDX-License-Identifier: GPL-2.0-or-later
"""Raw NTFS record helpers for the probe tests (stdlib only): multi sector protection of MFT
records, a few edits on image files, and the geometry of an image. Test code, no I/O beyond
the image files it is given."""
import struct

SECTOR = 512


def geometry(img):
    with open(img, "rb") as f:
        bs = f.read(512)
    bps, spc = struct.unpack_from("<HB", bs, 11)
    mft, mir = struct.unpack_from("<QQ", bs, 0x30)
    cpr = struct.unpack_from("<b", bs, 0x40)[0]
    cs = bps * (spc if spc <= 128 else 1 << (256 - spc))
    rs = (1 << -cpr) if cpr < 0 else cpr * cs
    return {"mft_off": mft * cs, "mir_off": mir * cs, "rs": rs, "cluster": cs, "mft_lcn": mft, "mir_lcn": mir,
            "sector": bps, "sectors": struct.unpack_from("<Q", bs, 0x28)[0]}


def unprotect(raw):
    """Raw record -> (record with the original sector tails put back, usn)."""
    usa_ofs, usa_count = struct.unpack_from("<HH", raw, 4)
    usn = struct.unpack_from("<H", raw, usa_ofs)[0]
    rec = bytearray(raw)
    for i in range(1, usa_count):
        assert struct.unpack_from("<H", raw, i * SECTOR - 2)[0] == usn, "torn record"
        rec[i * SECTOR - 2:i * SECTOR] = raw[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]
    return rec, usn


def protect(rec, usn):
    """Record with original tails -> raw record that carries `usn`."""
    usa_ofs, usa_count = struct.unpack_from("<HH", rec, 4)
    out = bytearray(rec)
    struct.pack_into("<H", out, usa_ofs, usn)
    for i in range(1, usa_count):
        out[usa_ofs + 2 * i:usa_ofs + 2 * i + 2] = rec[i * SECTOR - 2:i * SECTOR]
        struct.pack_into("<H", out, i * SECTOR - 2, usn)
    return out


def attributes(rec):
    """[(offset, type, length, name_length)] of a record that has its original tails."""
    off = struct.unpack_from("<H", rec, 0x14)[0]
    out = []
    while struct.unpack_from("<I", rec, off)[0] != 0xFFFFFFFF:
        t, ln = struct.unpack_from("<II", rec, off)
        out.append((off, t, ln, rec[off + 9]))
        off += ln
    return out


def find_attr(rec, type_, unnamed=True):
    for off, t, ln, nl in attributes(rec):
        if t == type_ and (not unnamed or nl == 0):
            return off
    return None


def mutate_record(img, which, k, fn):
    """Apply fn(rec, usn) -> usn' to record k of the $MFT ("mft") or $MFTMirr ("mir") of an image
    file. fn works on the record with original tails and returns the USN to protect it with."""
    g = geometry(img)
    base = g["mft_off"] if which == "mft" else g["mir_off"]
    with open(img, "r+b") as f:
        f.seek(base + k * g["rs"])
        rec, usn = unprotect(f.read(g["rs"]))
        usn = fn(rec, usn)
        f.seek(base + k * g["rs"])
        f.write(protect(rec, usn))


def set_volume_flags(img, flags):
    """Set the flags word of $VOLUME_INFORMATION in record 3 of $MFT and $MFTMirr (both copies)."""
    def edit(rec, usn):
        off = find_attr(rec, 0x70)
        voff = off + struct.unpack_from("<H", rec, off + 0x14)[0]
        struct.pack_into("<H", rec, voff + 10, flags)  # reserved 8, major, minor, then the flags
        return usn

    for which in ("mft", "mir"):
        mutate_record(img, which, 3, edit)


def set_volume_version(img, major, minor):
    """Set the NTFS version in $VOLUME_INFORMATION of record 3, in both copies."""
    def edit(rec, usn):
        off = find_attr(rec, 0x70)
        voff = off + struct.unpack_from("<H", rec, off + 0x14)[0]
        rec[voff + 8] = major
        rec[voff + 9] = minor
        return usn

    for which in ("mft", "mir"):
        mutate_record(img, which, 3, edit)


def synth_split(img):
    """Put the image in the state of a cut between the two record-0 writes of one MFT growth step,
    as the sweep images show it: the $MFT copy of record 0 has USN + 1 and a $DATA data size and
    initialized size one record larger; the mirror copy is left as it is."""
    g = geometry(img)

    def edit(rec, usn):
        off = find_attr(rec, 0x80)
        alloc, size, init = struct.unpack_from("<QQQ", rec, off + 0x28)
        assert init + g["rs"] <= alloc, "no room for one more record"
        struct.pack_into("<QQ", rec, off + 0x30, size + g["rs"], init + g["rs"])
        return usn + 1

    mutate_record(img, "mft", 0, edit)


def damage_index_block(img, name):
    """Zero the 4096-byte index block (magic INDX) that holds `name` as a $FILE_NAME key; returns its
    offset or None."""
    needle = name.encode("utf-16-le")
    with open(img, "r+b") as f:
        data = f.read()
        pos = 0
        while True:
            i = data.find(needle, pos)
            if i < 0:
                return None
            start = i & ~0xFFF
            if data[start:start + 4] == b"INDX":
                f.seek(start)
                f.write(b"\x00" * 4096)
                return start
            pos = i + 1


def damage_directory_record(img, name):
    """Tear the MFT record of the directory called `name` (ASCII): the last two bytes of its second
    sector no longer carry the update sequence number, so the record cannot be read. True if found."""
    g = geometry(img)
    needle = name.encode("utf-16-le")
    with open(img, "r+b") as f:
        data = f.read()
        pos = 0
        while True:
            i = data.find(needle, pos)
            if i < 0:
                return False
            start = i - (i % g["rs"])
            if data[start:start + 4] == b"FILE" and struct.unpack_from("<H", data, start + 0x16)[0] & 2:
                # a $FILE_NAME of this very directory: its name is the last field of the attribute
                f.seek(start + 1022)
                f.write(b"\xde\xad")
                return True
            pos = i + 1
