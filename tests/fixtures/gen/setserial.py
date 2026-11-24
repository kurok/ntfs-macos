# SPDX-License-Identifier: GPL-2.0-or-later
#
# Gives a fixture image a volume serial number of its own.
#
# Why: the daemon's session journal is keyed by the NTFS volume serial number (the 8 bytes at offset
# 0x48 of the boot sector). The generators run `mkntfs -T` (fake time 0, so that the images do not
# depend on the time of the run), and mkntfs seeds its serial from that time: without this step
# every fixture has the serial 0x34F5EE1202469FF7. Two fixtures attached together (clean-64m-4k rw
# and dirty-flag ro, I-DMN attach driver) would then share one journal entry, and the rw session of
# the first one made the second one look like a volume this Mac left open (reason
# dirty_flag_journal instead of dirty_flag), depending on the order of the probes.
#
# The serial is the first 8 bytes of SHA-256("ntfs-macos fixture serial: " + NAME), little endian,
# the same bytes at offset 0x48 of the primary boot sector (sector 0) and of the backup boot sector
# (the last sector of the volume, at number_of_sectors * bytes_per_sector). Nothing else is changed;
# both sectors are checked before anything is written.
# Same name, same serial: two generations give the same bytes. Use before the image is mounted.
#
# Library: set_serial(img, name) -> serial (int). Command line: setserial.py IMAGE NAME
import hashlib
import struct
import sys

SERIAL_OFFSET = 0x48


def serial_for(name):
    return struct.unpack("<Q", hashlib.sha256(b"ntfs-macos fixture serial: " + name.encode("utf-8")).digest()[:8])[0]


def set_serial(img, name):
    new = serial_for(name)
    with open(img, "r+b") as f:
        boot = f.read(512)
        if len(boot) < 512 or boot[3:11] != b"NTFS    ":
            raise ValueError("%s: no NTFS boot sector" % img)
        bps = struct.unpack_from("<H", boot, 11)[0]
        nsect = struct.unpack_from("<Q", boot, 0x28)[0]
        backup = nsect * bps
        f.seek(backup)
        b2 = f.read(512)
        # check the backup first, so that an error leaves the image unchanged
        if len(b2) != 512 or b2[3:11] != b"NTFS    " or b2[:SERIAL_OFFSET] != boot[:SERIAL_OFFSET]:
            raise ValueError("%s: no backup boot sector at %d" % (img, backup))
        f.seek(SERIAL_OFFSET)
        f.write(struct.pack("<Q", new))
        f.seek(backup + SERIAL_OFFSET)
        f.write(struct.pack("<Q", new))
    return new


if __name__ == "__main__":
    if len(sys.argv) != 3:
        sys.exit("usage: setserial.py IMAGE NAME")
    print("%s serial %016X" % (sys.argv[2], set_serial(sys.argv[1], sys.argv[2])))
