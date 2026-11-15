#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The stale-mirror rule of ARCHITECTURE.md section 3.7 (src/common/mftmirr.c), rules M1 to M5,
on in-memory copies (U-PROBE-MIRROR-04) through tests/unit/probe/mm_tool (stdlib only).

Source: the first records of `$MFT` and `$MFTMirr` of fixture images, loaded into buffers and
edited there; no image is changed. mirror-split gives a real split (USN 6 against 5, one record
ahead). clean-64m-64k, with the split of ntfsrec.synth_split put on its buffers, gives a volume
whose mirror holds 64 records, of which the engine compares 16 (rule M1: only records 0 to 3 may
differ).

Record 3 (`$Volume`, rule M4 k = 3) is tested on the same buffers: the USN and the VOLUME_IS_DIRTY bit of
$VOLUME_INFORMATION may differ, in either direction, and nothing else.

Each rule is negated alone and must give NOT_SPLIT with that rule; the unchanged input gives SPLIT.
"""
import argparse
import os
import struct
import subprocess
import sys
import tempfile
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from ntfsrec import attributes, find_attr, geometry, protect, unprotect  # noqa: E402

ARGS = None
TMP = None


class Vol:
    """In-memory $MFT and $MFTMirr of an image: lists of records with the original tails put back."""

    def __init__(self, img, split=False):
        g = self.g = geometry(img)
        rs = g["rs"]
        n_cap = 4 if g["cluster"] <= 4 * rs else min(16, g["cluster"] // rs)
        self.n_cap = n_cap
        with open(img, "rb") as f:
            f.seek(g["mft_off"])
            mft = [unprotect(f.read(rs)) for _ in range(n_cap)]
            f.seek(g["mir_off"])
            mir = [unprotect(f.read(rs)) for _ in range(n_cap)]
        self.mft = [[bytearray(r), u] for r, u in mft]
        self.mir = [[bytearray(r), u] for r, u in mir]
        if split:  # as ntfsrec.synth_split, on the buffers
            rec = self.mft[0][0]
            off = find_attr(rec, 0x80)
            alloc, size, init = struct.unpack_from("<QQQ", rec, off + 0x28)
            assert init + rs <= alloc
            struct.pack_into("<QQ", rec, off + 0x30, size + rs, init + rs)
            self.mft[0][1] += 1

    def raw(self, side):
        return b"".join(bytes(protect(r, u)) for r, u in getattr(self, side))

    def run(self, mir_override=None, mft_override=None, fail_read=0, mirr_lcn=None, mft_lcn=None):
        g = self.g
        mft = mft_override if mft_override is not None else self.raw("mft")
        mir = mir_override if mir_override is not None else self.raw("mir")
        pm, pi = os.path.join(TMP, "mft.bin"), os.path.join(TMP, "mir.bin")
        with open(pm, "wb") as f:
            f.write(mft)
        with open(pi, "wb") as f:
            f.write(mir)
        cmd = [ARGS.tool, "rule", "--sector-size", str(g["sector"]), "--cluster-size", str(g["cluster"]),
               "--record-size", str(g["rs"]), "--mft-lcn", str(mft_lcn if mft_lcn is not None else g["mft_lcn"]),
               "--mirr-lcn", str(mirr_lcn if mirr_lcn is not None else g["mir_lcn"]), "--mft", pm, "--mir", pi]
        if fail_read:
            cmd += ["--fail-read", str(fail_read)]
        out = subprocess.run(cmd, capture_output=True, check=True).stdout.decode().strip()
        parts = out.split()
        if parts[0] == "SPLIT":
            return ("SPLIT", int(parts[1].split("=")[1]), int(parts[2].split("=")[1]))
        if parts[0] == "NOT_SPLIT":
            return ("NOT_SPLIT", int(parts[1].split("=")[1]), out)
        return (parts[0], 0, out)


def data_off(rec):
    return find_attr(rec, 0x80)


def flags_off(rec):
    """Offset of the flags word of $VOLUME_INFORMATION in a record with its original tails."""
    off = find_attr(rec, 0x70)
    return off + struct.unpack_from("<H", rec, off + 0x14)[0] + 10


def set_flags(rec, flags):
    struct.pack_into("<H", rec, flags_off(rec), flags)


class Rule(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.have4k = os.path.exists(ARGS.split_image)
        cls.have64k = bool(ARGS.image64) and os.path.exists(ARGS.image64)

    def vol(self):
        if not self.have4k:
            self.skipTest("no mirror-split image")
        return Vol(ARGS.split_image)

    def vol64(self):
        if not self.have64k:
            self.skipTest("no 64k image")
        return Vol(ARGS.image64, split=True)

    def not_split(self, res, rule):
        self.assertEqual(res[0], "NOT_SPLIT", res)
        self.assertEqual(res[1], rule, res)

    # ---- the real split and its neighbours ----------------------------------------------------------
    def test_real_split_holds(self):
        v = self.vol()
        self.assertEqual(v.run(), ("SPLIT", 1, 4))
        # the evidence: USN one apart, $MFT one record ahead
        self.assertEqual(v.mft[0][1], v.mir[0][1] + 1)

    def test_split_with_more_than_four_records(self):
        v = self.vol64()
        r = v.run()
        self.assertEqual(r[:2], ("SPLIT", 1))
        self.assertEqual(r[2], 16)  # min(mirror size, 16)

    # ---- U-PROBE-MIRROR-04: each rule negated alone ----------------------------------------------------
    def test_m3_nothing_differs(self):
        v = self.vol()
        v.mir[0] = [bytearray(v.mft[0][0]), v.mft[0][1]]
        self.not_split(v.run(), 3)
        v = self.vol()
        for k in range(4):
            v.mir[k] = [bytearray(v.mft[k][0]), v.mft[k][1]]
        self.not_split(v.run(), 3)

    def test_m5_usn_equal(self):
        v = self.vol()
        v.mir[0][1] = v.mft[0][1]
        self.not_split(v.run(), 5)

    def test_m5_usn_reversed(self):
        v = self.vol()
        v.mir[0][1] = v.mft[0][1] + 1  # the mirror carries the higher USN, the $MFT data is the newer
        self.not_split(v.run(), 5)

    def test_m5_usn_two_apart(self):
        v = self.vol()
        v.mir[0][1] = v.mft[0][1] - 2
        self.not_split(v.run(), 5)

    def test_m5_usn_wraps_skipping_0_and_ffff(self):
        v = self.vol()
        v.mir[0][1], v.mft[0][1] = 0xFFFE, 1
        self.assertEqual(v.run()[0], "SPLIT")
        v.mft[0][1] = 0xFFFF
        self.not_split(v.run(), 5)
        v.mft[0][1] = 0
        self.not_split(v.run(), 5)

    def test_m4_size_two_records_ahead(self):
        v = self.vol()
        rs = v.g["rs"]
        rec = v.mft[0][0]
        off = data_off(rec)
        alloc, size, init = struct.unpack_from("<QQQ", rec, off + 0x28)
        struct.pack_into("<QQ", rec, off + 0x30, size + rs, init + rs)
        self.not_split(v.run(), 4)

    def test_m4_only_the_usn_differs_in_record_0(self):
        v = self.vol()
        v.mir[0] = [bytearray(v.mft[0][0]), v.mft[0][1] - 1]
        self.not_split(v.run(), 4)  # $MFT would not be one record ahead

    def test_m4_run_list_changed_before_the_mirror_end(self):
        v = self.vol()
        rec = v.mir[0][0]
        off = data_off(rec)
        mp = off + struct.unpack_from("<H", rec, off + 0x20)[0]
        hdr = rec[mp]
        lsz, osz = hdr & 15, hdr >> 4
        rec[mp + 1 + lsz] ^= 0x01  # the first LCN of the mirror's run list
        self.not_split(v.run(), 4)

    def test_m4_attribute_length_changed(self):
        # grow the mirror's $DATA by 8 bytes (zeros after its run list) and move what follows
        v = self.vol()
        rec = v.mir[0][0]
        off = data_off(rec)
        ln = struct.unpack_from("<I", rec, off + 4)[0]
        used = struct.unpack_from("<I", rec, 0x18)[0]
        rec[off + ln + 8:used + 8] = rec[off + ln:used]
        rec[off + ln:off + ln + 8] = b"\0" * 8
        struct.pack_into("<I", rec, off + 4, ln + 8)
        struct.pack_into("<I", rec, 0x18, used + 8)
        self.not_split(v.run(), 4)

    def test_m4_header_field_changed(self):
        for fieldoff, size in ((0x10, 2), (0x12, 2), (0x16, 2), (0x08, 8), (0x28, 2)):
            with self.subTest(offset=fieldoff):
                v = self.vol()
                rec = v.mir[0][0]
                rec[fieldoff] ^= 0x01 if fieldoff != 0x16 else 0x02  # flags: directory bit
                self.not_split(v.run(), 4)

    def test_m4_other_attribute_byte_changed(self):
        v = self.vol()
        rec = v.mir[0][0]
        off = find_attr(rec, 0x10)
        rec[off + struct.unpack_from("<H", rec, off + 0x14)[0] + 2] ^= 0x01
        self.not_split(v.run(), 4)

    def test_m4_sizes_not_equal_in_one_copy(self):
        v = self.vol()
        rec = v.mft[0][0]
        off = data_off(rec)
        struct.pack_into("<Q", rec, off + 0x30, struct.unpack_from("<Q", rec, off + 0x30)[0] + 1)
        self.not_split(v.run(), 4)

    def test_m4_allocation_larger_by_a_part_of_a_cluster(self):
        v = self.vol()
        rec = v.mft[0][0]
        off = data_off(rec)
        struct.pack_into("<Q", rec, off + 0x28, struct.unpack_from("<Q", rec, off + 0x28)[0] + 512)
        self.not_split(v.run(), 4)

    def test_m4_highest_vcn_wrong(self):
        v = self.vol()
        rec = v.mft[0][0]
        off = data_off(rec)
        struct.pack_into("<Q", rec, off + 0x18, struct.unpack_from("<Q", rec, off + 0x18)[0] + 1)
        self.not_split(v.run(), 4)

    # ---- rule M4, k = 3: the USN and VOLUME_IS_DIRTY of $VOLUME_INFORMATION (T-M3-12) --------------------
    def only3(self, v):
        """Make record 0 equal in both copies, so that record 3 is the only difference."""
        v.mir[0] = [bytearray(v.mft[0][0]), v.mft[0][1]]
        return v

    def test_m4_record_3_dirty_set_in_mft_is_accepted(self):
        # volmark set cut between W1 and W2: $MFT has VOLUME_IS_DIRTY and the next USN
        v = self.only3(self.vol())
        set_flags(v.mft[3][0], 0x0001)
        v.mft[3][1] += 1
        self.assertEqual(v.run(), ("SPLIT", 8, 4))
        v = self.vol()  # together with the record 0 split of the image
        set_flags(v.mft[3][0], 0x0001)
        v.mft[3][1] += 1
        self.assertEqual(v.run(), ("SPLIT", 9, 4))

    def test_m4_record_3_dirty_cleared_in_mft_is_accepted(self):
        # volmark clear cut between W1 and W2: the reverse direction
        v = self.only3(self.vol())
        set_flags(v.mir[3][0], 0x0001)
        v.mft[3][1] += 1
        self.assertEqual(v.run(), ("SPLIT", 8, 4))

    def test_m4_record_3_other_flags_may_be_set_in_both(self):
        for flags in (0x8000, 0x0080 | 0x4000):
            with self.subTest(flags=flags):
                v = self.only3(self.vol())
                set_flags(v.mir[3][0], flags)
                set_flags(v.mft[3][0], flags | 0x0001)
                v.mft[3][1] += 1
                self.assertEqual(v.run(), ("SPLIT", 8, 4))

    def test_m4_record_3_usn_wraps_skipping_0_and_ffff(self):
        v = self.only3(self.vol())
        set_flags(v.mft[3][0], 0x0001)
        v.mir[3][1], v.mft[3][1] = 0xFFFE, 1
        self.assertEqual(v.run()[:2], ("SPLIT", 8))

    def test_m4_record_3_usn_only_is_not_accepted(self):
        v = self.only3(self.vol())
        v.mft[3][1] += 1
        self.not_split(v.run(), 4)

    def test_m4_record_3_flag_bits_other_than_dirty(self):
        for name, mft_flags, mir_flags in (
                ("an extra bit with the dirty bit", 0x0003, 0x0000),
                ("an extra bit in the mirror", 0x0001, 0x0002),
                ("another bit only", 0x0002, 0x0000),
                ("a high bit with the dirty bit", 0x8001, 0x0000),
                ("a high bit only", 0x4000, 0x0000),
                ("dirty swapped for another bit", 0x0002, 0x0001)):
            with self.subTest(case=name):
                v = self.only3(self.vol())
                set_flags(v.mft[3][0], mft_flags)
                set_flags(v.mir[3][0], mir_flags)
                v.mft[3][1] += 1
                self.not_split(v.run(), 4)

    def test_m4_record_3_other_byte_changed(self):
        def si_byte(rec):  # $STANDARD_INFORMATION, a time stamp byte
            off = find_attr(rec, 0x10)
            rec[off + struct.unpack_from("<H", rec, off + 0x14)[0] + 2] ^= 0x01

        def data_byte(rec):  # the unnamed $DATA of $Volume (empty): the attribute header flags
            rec[find_attr(rec, 0x80) + 0x0C] ^= 0x01

        def name_byte(rec):  # $VOLUME_NAME value
            off = find_attr(rec, 0x60)
            rec[off + struct.unpack_from("<H", rec, off + 0x14)[0]] ^= 0x01

        def vi_version(rec):  # the minor version of $VOLUME_INFORMATION, next to the flags word
            rec[flags_off(rec) - 1] ^= 0x01

        def vi_reserved(rec):
            rec[flags_off(rec) - 10] ^= 0x01

        def vi_next_to_flags(rec):  # the high byte of the flags word
            rec[flags_off(rec) + 1] ^= 0x01

        def header(rec):  # sequence number
            rec[0x10] ^= 0x01

        for name, edit in (("standard information", si_byte), ("data header", data_byte), ("volume name", name_byte),
                           ("volume minor version", vi_version), ("volume information reserved", vi_reserved),
                           ("flags high byte", vi_next_to_flags), ("header", header)):
            for side in ("mft", "mir"):
                with self.subTest(case=name, changed_in=side):
                    v = self.only3(self.vol())
                    set_flags(v.mft[3][0], 0x0001)
                    v.mft[3][1] += 1
                    edit(v.mft[3][0] if side == "mft" else v.mir[3][0])
                    self.not_split(v.run(), 4)

    def test_m4_record_3_usn_order(self):
        for name, mft_usn, mir_usn in (("not the successor", 7, 5), ("equal", 5, 5), ("mirror ahead", 5, 6),
                                       ("one behind, wrapped", 0xFFFF, 1)):
            with self.subTest(case=name):
                v = self.only3(self.vol())
                set_flags(v.mft[3][0], 0x0001)
                v.mft[3][1], v.mir[3][1] = mft_usn, mir_usn
                self.not_split(v.run(), 5)
        # the dirty bit set in the mirror, the mirror carrying the successor: mirror ahead
        v = self.only3(self.vol())
        set_flags(v.mir[3][0], 0x0001)
        v.mir[3][1] = v.mft[3][1] + 1
        self.not_split(v.run(), 5)

    def test_m4_record_3_volume_information_missing_or_unusable(self):
        def retype(rec):  # 0x70 becomes 0x78: still in ascending order, but no $VOLUME_INFORMATION
            struct.pack_into("<I", rec, find_attr(rec, 0x70), 0x78)

        def short_value(rec):  # a value of 8 bytes: the flags word is not in it
            struct.pack_into("<I", rec, find_attr(rec, 0x70) + 0x10, 8)

        def named(rec):
            rec[find_attr(rec, 0x70) + 0x09] = 1

        def nonresident(rec):
            rec[find_attr(rec, 0x70) + 0x08] = 1

        def value_in_header(rec):  # value offset inside the attribute header
            struct.pack_into("<H", rec, find_attr(rec, 0x70) + 0x14, 0x10)

        for name, edit, copies in (("retyped in both", retype, "both"), ("retyped in $MFT", retype, "mft"),
                                   ("retyped in the mirror", retype, "mir"), ("short value in both", short_value, "both"),
                                   ("named in both", named, "both"), ("non-resident in both", nonresident, "both"),
                                   ("value offset in the header", value_in_header, "both")):
            with self.subTest(case=name):
                v = self.only3(self.vol())
                set_flags(v.mft[3][0], 0x0001)
                v.mft[3][1] += 1
                if copies in ("both", "mft"):
                    edit(v.mft[3][0])
                if copies in ("both", "mir"):
                    edit(v.mir[3][0])
                res = v.run()
                self.assertEqual(res[0], "NOT_SPLIT", res)
                # a non-resident $VOLUME_INFORMATION already fails the record check (M2)
                self.assertEqual(res[1], 2 if name.startswith("non-resident") else 4, (name, res))

    def test_m4_record_3_volume_information_truncated(self):
        # the attribute or its value reaches past the record's bytes in use: the record is invalid (M2)
        def long_value(rec):
            struct.pack_into("<I", rec, find_attr(rec, 0x70) + 0x10, 0x1000)

        def long_attr(rec):
            struct.pack_into("<I", rec, find_attr(rec, 0x70) + 0x04, 0x1000)

        def cut_record(rec):  # bytes in use end inside the attribute
            struct.pack_into("<I", rec, 0x18, find_attr(rec, 0x70) + 0x20)

        for name, edit in (("value too long", long_value), ("attribute too long", long_attr),
                           ("bytes in use end inside it", cut_record)):
            with self.subTest(case=name):
                v = self.only3(self.vol())
                set_flags(v.mft[3][0], 0x0001)
                v.mft[3][1] += 1
                edit(v.mft[3][0])
                edit(v.mir[3][0])
                self.not_split(v.run(), 2)

    def test_m4_record_3_attribute_not_at_the_fixed_offset(self):
        # the flags are found by walking: move the attribute 8 bytes by growing the attribute before it
        # ($VOLUME_NAME) in both copies, and the rule still reads the flags word where it now is
        v = self.only3(self.vol())
        for rec in (v.mft[3][0], v.mir[3][0]):
            off = find_attr(rec, 0x60)
            ln = struct.unpack_from("<I", rec, off + 4)[0]
            used = struct.unpack_from("<I", rec, 0x18)[0]
            rec[off + ln + 8:used + 8] = rec[off + ln:used]
            rec[off + ln:off + ln + 8] = b"\0" * 8
            struct.pack_into("<I", rec, off + 4, ln + 8)
            struct.pack_into("<I", rec, 0x18, used + 8)
        self.assertEqual(flags_off(v.mft[3][0]), 0x1ba + 8)
        set_flags(v.mft[3][0], 0x0001)
        v.mft[3][1] += 1
        self.assertEqual(v.run(), ("SPLIT", 8, 4))
        # a difference at the old fixed offset, with the flags word elsewhere, is not the dirty bit
        v.mft[3][0][0x1ba] ^= 0x01
        self.not_split(v.run(), 4)

    def test_m4_records_1_and_2_may_differ_in_the_usn_only(self):
        v = self.vol()
        v.mft[1][1] += 1
        self.assertEqual(v.run()[:2], ("SPLIT", 3))
        v.mft[2][1] += 1
        self.assertEqual(v.run()[:2], ("SPLIT", 7))
        v = self.vol()
        rec = v.mft[1][0]
        rec[find_attr(rec, 0x10) + 0x30] ^= 0x01
        v.mft[1][1] += 1
        self.not_split(v.run(), 4)

    def test_m4_record_1_usn_not_successor(self):
        v = self.vol()
        v.mft[1][1] += 2
        self.not_split(v.run(), 5)

    def test_m1_records_beyond_the_fourth_must_not_differ(self):
        v = self.vol64()
        rec = v.mir[9][0]
        off = find_attr(rec, 0x10)
        rec[off + struct.unpack_from("<H", rec, off + 0x14)[0] + 2] ^= 0x01
        self.not_split(v.run(), 1)

    def test_m1_mirror_smaller_than_four_records(self):
        v = self.vol()
        for side in (v.mft, v.mir):
            rec = side[1][0]
            off = data_off(rec)
            struct.pack_into("<Q", rec, off + 0x30, 2048)
        self.not_split(v.run(), 1)

    def test_m1_mirror_or_mft_not_where_the_boot_sector_says(self):
        v = self.vol()
        # the run lists say where $MFT and $MFTMirr start; a boot sector that says otherwise
        self.not_split(v.run(mirr_lcn=v.g["mir_lcn"] + 1), 1)
        self.not_split(v.run(mft_lcn=v.g["mft_lcn"] + 1), 1)
        # the run lists of record 0 and record 1 start elsewhere than the boot sector says
        v = self.vol()
        rec = v.mft[1][0]
        off = data_off(rec)
        mp = off + struct.unpack_from("<H", rec, off + 0x20)[0]
        lsz = rec[mp] & 15
        rec[mp + 1 + lsz] ^= 0x02
        self.not_split(v.run(), 1)

    def test_m2_invalid_records(self):
        cases = {
            "mirror magic": lambda v: v.mir[0][0].__setitem__(slice(0, 4), b"BAAD"),
            "mft magic": lambda v: v.mft[0][0].__setitem__(slice(0, 4), b"BAAD"),
            "mirror not in use": lambda v: v.mir[0][0].__setitem__(0x16, 0),
            "mft not in use": lambda v: v.mft[2][0].__setitem__(0x16, 0),
            "mirror record 2 not in use": lambda v: v.mir[2][0].__setitem__(0x16, 0),
            "bytes_allocated wrong": lambda v: struct.pack_into("<I", v.mir[1][0], 0x1C, 512),
            "bytes_in_use beyond record": lambda v: struct.pack_into("<I", v.mft[1][0], 0x18, 4096),
            "attrs offset unaligned": lambda v: struct.pack_into("<H", v.mir[0][0], 0x14, 0x39),
            "attribute length unaligned": lambda v: struct.pack_into("<I", v.mft[3][0], find_attr(v.mft[3][0], 0x10) + 4, 0x61),
            "no end marker": lambda v: v.mir[1][0].__setitem__(slice(attributes(v.mir[1][0])[-1][0], attributes(v.mir[1][0])[-1][0] + 4), b"\0\0\0\0"),
        }
        for name, edit in cases.items():
            with self.subTest(case=name):
                v = self.vol()
                edit(v)
                self.not_split(v.run(), 2)

    def test_m2_torn_records(self):
        v = self.vol()
        raw = bytearray(v.raw("mft"))
        raw[0x3FE] ^= 0x01  # last sector tail of $MFT record 0: torn
        self.not_split(v.run(mft_override=bytes(raw)), 2)
        raw = bytearray(v.raw("mir"))
        raw[0x1FE] ^= 0x01
        self.not_split(v.run(mir_override=bytes(raw)), 2)
        raw = bytearray(v.raw("mir"))
        raw[1024 + 0x1FE] ^= 0x01  # a record that does not differ (record 1), also torn
        self.not_split(v.run(mir_override=bytes(raw)), 2)

    def test_m2_usa_header_invalid(self):
        for name, edit in {
            "odd offset": lambda r: struct.pack_into("<H", r, 4, 0x31),
            "count too small": lambda r: struct.pack_into("<H", r, 6, 2),
            "count too large": lambda r: struct.pack_into("<H", r, 6, 5),
            "offset in the header": lambda r: struct.pack_into("<H", r, 4, 0x20),
        }.items():
            with self.subTest(case=name):
                v = self.vol()
                raw = bytearray(v.raw("mir"))
                edit(raw)
                self.not_split(v.run(mir_override=bytes(raw)), 2)

    # ---- I/O and geometry ---------------------------------------------------------------------------------------------
    def test_read_errors_are_not_a_verdict(self):
        v = self.vol()
        for k in (1, 2):
            self.assertEqual(v.run(fail_read=k)[0], "IO_ERROR", k)

    def test_parse_boot(self):
        if not self.have4k:
            self.skipTest("no image")
        with open(ARGS.split_image, "rb") as f:
            bs = f.read(512)
        p = os.path.join(TMP, "boot.bin")

        def boot(edit=None):
            b = bytearray(bs)
            if edit:
                edit(b)
            with open(p, "wb") as f:
                f.write(b)
            return subprocess.run([ARGS.tool, "boot", p], capture_output=True, check=True).stdout.decode().strip()

        self.assertTrue(boot().startswith("sector=512 cluster=4096 record=1024 sectors=131071 mft=4 mirr=8191"))
        bad = {
            "sector size 500": lambda b: struct.pack_into("<H", b, 11, 500),
            "sector size 8192": lambda b: struct.pack_into("<H", b, 11, 8192),
            "spc 3": lambda b: b.__setitem__(13, 3),
            "spc 0": lambda b: b.__setitem__(13, 0),
            "spc 0xFF with 4096 sectors": lambda b: (struct.pack_into("<H", b, 11, 4096), b.__setitem__(13, 0xF0)),
            "zero sectors": lambda b: struct.pack_into("<Q", b, 0x28, 0),
            "negative sectors": lambda b: struct.pack_into("<Q", b, 0x28, 1 << 63),
            "mft lcn beyond volume": lambda b: struct.pack_into("<Q", b, 0x30, 1 << 40),
            "mirror lcn negative": lambda b: struct.pack_into("<Q", b, 0x38, (1 << 64) - 1),
            "record size 3 clusters": lambda b: b.__setitem__(0x40, 3),
            "record size 2^20": lambda b: b.__setitem__(0x40, 0xEC),
            "record size zero": lambda b: b.__setitem__(0x40, 0),
        }
        for name, edit in bad.items():
            with self.subTest(case=name):
                self.assertEqual(boot(edit), "invalid", name)
        # c < 0: record size 2^-c; spc 0xF4 gives 4096 sectors
        self.assertIn("record=4096", boot(lambda b: b.__setitem__(0x40, 0xF4)))  # -12
        def big_cluster(b):
            b[13] = 0xF4  # 2^12 sectors of 512 bytes
            struct.pack_into("<Q", b, 0x28, 1 << 30)
            struct.pack_into("<QQ", b, 0x30, 4, 8)

        self.assertIn("cluster=2097152", boot(big_cluster))


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--split-image", required=True, help="the mirror-split fixture")
    ap.add_argument("--image64", help="a clean image with 64 KiB clusters (clean-64m-64k)")
    ARGS, rest = ap.parse_known_args()
    with tempfile.TemporaryDirectory(prefix="mm-test-") as TMP:
        unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
