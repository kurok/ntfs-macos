# SPDX-License-Identifier: GPL-2.0-or-later
"""Seed corpus for F-CTL-01 (T-M5-09). Python standard library only.

Usage: python3 -I -B tests/fuzz/ctl/gen_seeds.py [--out DIR]

Writes corpus/*.bin below DIR (default: the directory of this script): two knob bytes (see
fuzz_ctl.c) and one request line, the valid requests of U-CTL-FUZZ-01 under every knob kind, strategy,
peer uid and busy state.
The inputs are synthetic and the output is deterministic.
"""
import argparse
import os

AUTH = "00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff"
LINES = {
    "list": '{"cmd":"list"}',
    "doctor": '{"cmd":"doctor"}',
    "status": '{"cmd":"status","bsd":"disk4s1"}',
    "mount": '{"cmd":"mount","bsd":"disk4s1","mode":"ro"}',
    "unmount": '{"cmd":"unmount","bsd":"disk4s1","force":true}',
    "journal_clear": '{"cmd":"journal_clear","bsd":"disk3s2","confirm_label":"My Disk",'
                     '"confirm_serial":"0A1B2C3D","auth":"%s"}' % AUTH,
    "repair": '{"cmd":"repair","bsd":"disk3s2","action":"sync_mirror","confirm_label":'
              '"\\u00e9\\ud83d\\ude00","confirm_serial":"0A1B2C3D","auth":"%s"}' % AUTH,
    "empty": "",
    "trailing": '{"cmd":"list"}{"cmd":"list"}',
}


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("--out", default=os.path.dirname(os.path.abspath(__file__)))
    out = os.path.join(ap.parse_args().out, "corpus")
    os.makedirs(out, exist_ok=True)
    for name, line in LINES.items():
        for kind in range(10):
            # variants: root peer with uid 501 (no tag in the name), external form with uid 501, root peer
            # with uid 0, external form with a busy operation; kinds 3 and 4 also other answers of the system
            variants = [("", 0x10 | 0x20, 0), ("-ext", 0x10, 0), ("-root", 0x10 | 0x20 | 0x40, 0),
                        ("-busy", 0x10 | 0x80, 0)]
            if kind in (3, 4):
                variants += [("-ans%d" % a, 0x10, a) for a in (1, 2, 3, 5, 8)]
            for tag, bits, k1 in variants:
                with open(os.path.join(out, "%s-k%d%s.bin" % (name, kind, tag)), "wb") as f:
                    f.write(bytes([kind | bits, k1]) + line.encode())


if __name__ == "__main__":
    main()
