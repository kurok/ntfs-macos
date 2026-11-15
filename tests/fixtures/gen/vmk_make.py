#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The `vmk-split` image (T-M3-12, docs/TEST_STRATEGY.md section 3.2, U15) and the record-3
helpers of the volmark tests (stdlib only).

vmk-split: `clean-64m-4k` after `ntfsmac-volmark set` cut between the `$MFT` and `$MFTMirr` writes
of record 3. Made with volmark_test built against the fault-layer libntfs-3g (patch 0004,
--enable-faultio), plan cut:1 with NTFSMAC_FAULT_KILL=1: write 1 (`$MFT` record 3) reaches the
image, the process is killed at write 2 (`$MFTMirr` record 3), as a power loss between the two.

Usage (inside Environment A; tests/fixtures/gen/vmk.sh runs it):
  vmk_make.py --volmark-fault BIN --clean IMG --out DIR
writes DIR/vmk-split.img and DIR/vmk-split-record3.txt (the exact difference of the two copies of
record 3) and checks that the image is what the fixture table says: only the two record-3 copies
differ from the clean image, `$MFT` is the one step ahead (USN + 1, VOLUME_IS_DIRTY set), both
copies are valid multi-sector records, and the engine refuses it ("$MFTMirr does not match $MFT
(record 3)", ntfsinfo -m). Exit 0 made and checked, 1 a check failed, 2 usage or setup error.
"""
import argparse
import os
import shutil
import struct
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(HERE)), "unit", "probe"))
from ntfsrec import SECTOR, find_attr, geometry  # noqa: E402

VOLUME_IS_DIRTY = 0x0001
AT_VOLUME_INFORMATION = 0x70


def boot_ids(img):
    """(serial as 16 hex digits, number_of_sectors) of the boot sector."""
    with open(img, "rb") as f:
        b = f.read(512)
    return "%016x" % struct.unpack_from("<Q", b, 0x48)[0], struct.unpack_from("<Q", b, 0x28)[0]


def read_record(img, which, k=3):
    g = geometry(img)
    base = g["mft_off"] if which == "mft" else g["mir_off"]
    with open(img, "rb") as f:
        f.seek(base + k * g["rs"])
        return f.read(g["rs"])


def record_ranges(img, k=3):
    """[(start, end)] byte ranges of record k in $MFT and in $MFTMirr."""
    g = geometry(img)
    return [(g["mft_off"] + k * g["rs"], g["mft_off"] + (k + 1) * g["rs"]),
            (g["mir_off"] + k * g["rs"], g["mir_off"] + (k + 1) * g["rs"])]


def unprotect_checked(raw):
    """(record with the original sector tails, usn), or None if the record is torn or not FILE."""
    if raw[:4] != b"FILE":
        return None
    usa_ofs, usa_count = struct.unpack_from("<HH", raw, 4)
    if usa_count < 2 or usa_ofs + 2 * usa_count > len(raw) or (usa_count - 1) * SECTOR > len(raw):
        return None
    usn = struct.unpack_from("<H", raw, usa_ofs)[0]
    rec = bytearray(raw)
    for i in range(1, usa_count):
        if struct.unpack_from("<H", raw, i * SECTOR - 2)[0] != usn:
            return None
        rec[i * SECTOR - 2:i * SECTOR] = raw[usa_ofs + 2 * i:usa_ofs + 2 * i + 2]
    return rec, usn


def flags_offset(rec):
    """Offset of the flags word of $VOLUME_INFORMATION in an unprotected record 3."""
    off = find_attr(rec, AT_VOLUME_INFORMATION)
    if off is None:
        raise AssertionError("no $VOLUME_INFORMATION in record 3")
    return off + struct.unpack_from("<H", rec, off + 0x14)[0] + 10  # reserved 8, major, minor, flags


def volume_flags(rec):
    return struct.unpack_from("<H", rec, flags_offset(rec))[0]


def next_usn(u):
    """The USN ntfs_mst_pre_write_fixup() writes after u (0 and 0xffff are skipped)."""
    u = (u + 1) & 0xffff
    return 1 if u in (0, 0xffff) else u


def changed_offsets(a_img, b_img):
    """Byte offsets at which two equally long image files differ."""
    out = []
    with open(a_img, "rb") as fa, open(b_img, "rb") as fb:
        off = 0
        while True:
            a = fa.read(1 << 20)
            b = fb.read(1 << 20)
            if a != b:
                if len(a) != len(b):
                    raise AssertionError("images differ in length")
                out.extend(off + i for i in range(len(a)) if a[i] != b[i])
            if not a:
                return out
            off += len(a)


def outside_record3(before, after):
    """Changed offsets that are not inside one of the two copies of record 3 (must be empty)."""
    ranges = record_ranges(before)
    return [o for o in changed_offsets(before, after) if not any(s <= o < e for s, e in ranges)]


def record_change(rec_a, usn_a, rec_b, usn_b):
    """What differs between two unprotected copies of one record: the list of offsets and the USNs."""
    return {"offsets": [i for i in range(len(rec_a)) if rec_a[i] != rec_b[i]], "usn": (usn_a, usn_b)}


def classify(img):
    """State of the two copies of record 3:
         mft-torn / mirr-torn   a copy fails its fix-up check (or is not FILE)
         equal                  both valid and byte-identical
         mft-ahead / mirr-ahead both valid, differ; that copy has the successor USN
         split-other            both valid, differ, no USN order
    Returns (class, info) with info holding the flags of the valid copies."""
    raw_m, raw_r = read_record(img, "mft"), read_record(img, "mir")
    um, ur = unprotect_checked(raw_m), unprotect_checked(raw_r)
    info = {}
    if um:
        info["mft_flags"] = volume_flags(um[0])
        info["mft_usn"] = um[1]
    if ur:
        info["mirr_flags"] = volume_flags(ur[0])
        info["mirr_usn"] = ur[1]
    if not um:
        return "mft-torn", info
    if not ur:
        return "mirr-torn", info
    if raw_m == raw_r:
        return "equal", info
    if um[1] == next_usn(ur[1]):
        return "mft-ahead", info
    if ur[1] == next_usn(um[1]):
        return "mirr-ahead", info
    return "split-other", info


def record3_report(img):
    """The exact difference of the two copies of record 3 of a split image, as text lines."""
    g = geometry(img)
    raw_m, raw_r = read_record(img, "mft"), read_record(img, "mir")
    um, ur = unprotect_checked(raw_m), unprotect_checked(raw_r)
    if not um or not ur:
        raise AssertionError("a copy of record 3 is torn")
    usa_ofs, usa_count = struct.unpack_from("<HH", raw_m, 4)
    fo = flags_offset(um[0])
    tails = [i * SECTOR - 2 for i in range(1, usa_count)]
    raw_diff = [i for i in range(len(raw_m)) if raw_m[i] != raw_r[i]]
    rec_diff = [i for i in range(len(um[0])) if um[0][i] != ur[0][i]]
    lines = [
        "vmk-split: record 3 ($Volume), record size %d, $MFT copy at byte %d, $MFTMirr copy at byte %d"
        % (g["rs"], g["mft_off"] + 3 * g["rs"], g["mir_off"] + 3 * g["rs"]),
        "update sequence array at record offset 0x%x, %d entries; sector tails at %s"
        % (usa_ofs, usa_count, ", ".join("0x%x" % t for t in tails)),
        "$VOLUME_INFORMATION flags word at record offset 0x%x" % fo,
        "USN: $MFT 0x%04x, $MFTMirr 0x%04x (the $MFT copy is the successor: %s)"
        % (um[1], ur[1], um[1] == next_usn(ur[1])),
        "flags: $MFT 0x%04x, $MFTMirr 0x%04x (differ only in VOLUME_IS_DIRTY: %s)"
        % (volume_flags(um[0]), volume_flags(ur[0]),
           (volume_flags(um[0]) ^ volume_flags(ur[0])) == VOLUME_IS_DIRTY),
        "raw (on-disk) bytes that differ, record offsets: %s"
        % ", ".join("0x%x: %02x/%02x" % (i, raw_m[i], raw_r[i]) for i in raw_diff),
        "after the fix-up (sector tails restored), record offsets that differ: %s"
        % ", ".join("0x%x: %02x/%02x" % (i, um[0][i], ur[0][i]) for i in rec_diff),
    ]
    allowed_raw = set()
    for t in tails + [usa_ofs]:
        allowed_raw.update((t, t + 1))
    allowed_raw.update((fo, fo + 1))
    ok = (um[1] == next_usn(ur[1]) and (volume_flags(um[0]) ^ volume_flags(ur[0])) == VOLUME_IS_DIRTY
          and set(raw_diff) <= allowed_raw
          and set(rec_diff) <= {usa_ofs, usa_ofs + 1, fo})
    lines.append("difference confined to the USN (update sequence number and sector tails) and the "
                 "VOLUME_IS_DIRTY bit: %s" % ok)
    return lines, ok


def run_fault_set(volmark_fault, img, plan, kill=True):
    serial, sectors = boot_ids(img)
    env = dict(os.environ, NTFSMAC_FAULT_PLAN=plan)
    if kill:
        env["NTFSMAC_FAULT_KILL"] = "1"
    return subprocess.run(["sh", "-c", 'img=$1; shift; exec "$@" 3<>"$img"', "sh", img, volmark_fault, "set",
                           "--fd", "3", "--expect-serial", serial, "--expect-sectors", str(sectors)],
                          env=env, capture_output=True)


def make(volmark_fault, clean, out_img):
    """Make vmk-split from a clean image. Returns the report lines; raises on any failed check."""
    shutil.copyfile(clean, out_img)
    r = run_fault_set(volmark_fault, out_img, "cut:1", kill=True)
    if r.returncode != -9 or r.stdout != b"w\n":
        raise AssertionError("volmark set under cut:1 with the kill: exit %r, stdout %r, stderr %r"
                             % (r.returncode, r.stdout, r.stderr[-300:]))
    bad = outside_record3(clean, out_img)
    if bad:
        raise AssertionError("bytes outside the two copies of record 3 changed: %r" % bad[:8])
    cls, info = classify(out_img)
    if cls != "mft-ahead":
        raise AssertionError("record 3 state %s %r, want mft-ahead" % (cls, info))
    lines, ok = record3_report(out_img)
    if not ok:
        raise AssertionError("record 3 difference not confined:\n" + "\n".join(lines))
    e = subprocess.run(["ntfsinfo", "-m", out_img], capture_output=True, text=True)
    msg = "$MFTMirr does not match $MFT (record 3)"
    if e.returncode == 0 or msg not in e.stdout + e.stderr:
        raise AssertionError("the engine did not refuse vmk-split with %r: %r" % (msg, (e.stdout + e.stderr)[-300:]))
    lines.append("engine (ntfsinfo -m, NTFS-3G 2026.9.28): refused with \"%s.\"" % msg)
    return lines


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--volmark-fault", required=True, help="volmark_test built against the fault-layer libntfs-3g")
    ap.add_argument("--clean", required=True, help="clean-64m-4k.img")
    ap.add_argument("--out", required=True)
    a = ap.parse_args()
    if not os.access(a.volmark_fault, os.X_OK) or not os.path.isfile(a.clean):
        print("vmk_make: missing --volmark-fault or --clean", file=sys.stderr)
        return 2
    os.makedirs(a.out, exist_ok=True)
    img = os.path.join(a.out, "vmk-split.img")
    try:
        lines = make(a.volmark_fault, a.clean, img)
    except AssertionError as e:
        print("vmk_make: FAIL: %s" % e, file=sys.stderr)
        return 1
    with open(os.path.join(a.out, "vmk-split-record3.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    print("\n".join(lines))
    return 0


if __name__ == "__main__":
    sys.exit(main())
