#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""NFC module of the probe (src/probe/nfc.c) against Python's unicodedata (stdlib only).

The tables of src/probe/nfc_data.h come from unicodedata (gen_nfc.py). This test runs
the C code over many inputs and compares the NFC form and the "is normalized" answer:
  - every code point alone (lone surrogates included),
  - every canonical composition pair, in both orders, and with a second combining mark,
  - random sequences of starters and combining marks of different classes (ordering),
  - Hangul syllables and jamo sequences,
  - the names of the fixtures, when --names-dir is given (names-nfd.all, names-nfc.all).
If the Unicode version of Python differs from the tables, code points that Python does not
know (category Cn) are left out and the test says so.
"""
import argparse
import os
import random
import subprocess
import sys
import unicodedata
import unittest

ARGS = None


def to_units(cps):
    out = []
    for c in cps:
        if c >= 0x10000:
            c -= 0x10000
            out += [0xD800 + (c >> 10), 0xDC00 + (c & 0x3FF)]
        else:
            out.append(c)
    return out


def from_cps(cps):
    return "".join(chr(c) for c in cps)


def run_tool(cases):
    """cases: lists of code points -> list of (normalized, nfc code point list)."""
    data = "".join(" ".join("%X" % u for u in to_units(c)) + "\n" for c in cases)
    r = subprocess.run([ARGS.tool], input=data.encode(), capture_output=True, check=True)
    out = []
    for line in r.stdout.decode().splitlines():
        parts = line.split()
        out.append((int(parts[0]), [int(x, 16) for x in parts[1:]]))
    assert len(out) == len(cases), (len(out), len(cases))
    return out


def expected(cps):
    s = from_cps(cps)
    n = unicodedata.normalize("NFC", s)
    ok = n == s
    # a code point the tables' Unicode version does not assign is not trusted (fail closed);
    # lone surrogates and private use code points (category Co) are not "unassigned"
    if any(unicodedata.category(chr(c)) == "Cn" for c in cps):
        ok = False
    return (1 if ok else 0), [ord(c) for c in n]


class Nfc(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.tool_version = subprocess.run([ARGS.tool, "--version"], capture_output=True,
                                          check=True).stdout.decode().strip()
        cls.same_version = cls.tool_version == unicodedata.unidata_version
        print("tables: Unicode %s; python: Unicode %s%s" % (
            cls.tool_version, unicodedata.unidata_version,
            "" if cls.same_version else " (differs: unassigned code points left out)"), file=sys.stderr)

    # Blocks that have been complete for many Unicode versions: with another version of Python only
    # these are compared, since a newer Python assigns characters the tables do not know (which the
    # probe, fail closed, calls not NFC). The full range is compared in Environment A, where
    # Python's Unicode version equals the tables'.
    STABLE = ((0, 0x530), (0x590, 0x700), (0x900, 0x950), (0xE00, 0xE80), (0x1100, 0x1200), (0x1B00, 0x1B4E),
              (0x1DC0, 0x2000), (0x20D0, 0x2100), (0x3000, 0x3100), (0xAC00, 0xD7A4))

    def known(self, cp):
        if self.same_version:
            return True
        return any(a <= cp < b for a, b in self.STABLE)

    def compare(self, cases):
        cases = [c for c in cases if all(self.known(x) for x in c)]
        got = run_tool(cases)
        bad = [(c, g, expected(c)) for c, g in zip(cases, got) if g != expected(c)]
        self.assertEqual(bad[:5], [], "%d of %d cases differ" % (len(bad), len(cases)))
        return len(cases)

    def test_every_code_point(self):
        cases = [[cp] for cp in range(0x110000) if not (0xD800 <= cp <= 0xDFFF)]
        cases += [[cp] for cp in range(0xD800, 0xE000)]  # lone surrogates as units
        self.assertGreater(self.compare(cases), 1000000 if self.same_version else 10000)

    def test_composition_pairs(self):
        cases = []
        for cp in range(0x110000):
            if 0xD800 <= cp <= 0xDFFF:
                continue
            d = unicodedata.decomposition(chr(cp))
            if d and not d.startswith("<"):
                parts = [int(x, 16) for x in d.split()]
                cases.append(parts)
                cases.append(parts[::-1])
                cases.append(parts + [0x0323])  # a second mark of class 220
                cases.append(parts + [0x0301])
        self.assertGreater(self.compare(cases), 2000 if self.same_version else 300)

    def test_random_mark_sequences(self):
        rng = random.Random(15)
        marks = [cp for cp in range(0x300, 0x370)] + [0x0483, 0x05B0, 0x064B, 0x0E48, 0x1DC0, 0x3099, 0x302A, 0x20D0]
        starters = [0x41, 0x61, 0x6F, 0x75, 0xC5, 0x3B1, 0x438, 0x1100, 0x1161, 0xAC00, 0xAC01, 0x1E0C, 0x1EA0,
                    0x3046, 0x304B, 0x30AB, 0x0627, 0x0928, 0x1025, 0x1B05, 0x1B3A]
        cases = []
        for _ in range(60000):
            seq = []
            for _ in range(rng.randint(1, 8)):
                seq.append(rng.choice(marks) if rng.random() < 0.55 else rng.choice(starters))
            cases.append(seq)
        self.assertGreater(self.compare(cases), 50000 if self.same_version else 3000)

    def test_hangul(self):
        rng = random.Random(7)
        cases = []
        for _ in range(20000):
            seq = []
            for _ in range(rng.randint(1, 6)):
                kind = rng.randrange(4)
                if kind == 0:
                    seq.append(0x1100 + rng.randrange(19))
                elif kind == 1:
                    seq.append(0x1161 + rng.randrange(21))
                elif kind == 2:
                    seq.append(0x11A8 + rng.randrange(27))
                else:
                    seq.append(0xAC00 + rng.randrange(11172))
            cases.append(seq)
        self.assertGreaterEqual(self.compare(cases), 20000 if self.same_version else 1000)

    def test_unassigned_code_points_are_not_nfc(self):
        if not self.same_version:
            self.skipTest("needs the same Unicode version as the tables")
        for cp in (0x0378, 0x0380, 0x05FF, 0x2FE0, 0xFFFE, 0xFFFF, 0x1FFFF, 0xE0000, 0x10FFFE, 0x10FFFF, 0x50000):
            self.assertEqual(unicodedata.category(chr(cp)), "Cn", hex(cp))
            for seq in ([cp], [0x61, cp], [cp, 0x301], [0x65, 0x301, cp]):
                self.assertEqual(run_tool([seq])[0][0], 0, [hex(c) for c in seq])
        for cp in (0xE000, 0xF8FF, 0xF0000, 0x10FFFD, 0xD800, 0xDFFF):
            self.assertEqual(run_tool([[cp]])[0][0], 1, hex(cp))  # private use, lone surrogate: neutral

    def test_longest_decomposition_and_length_limits(self):
        # U+FDFA decomposes to 18 code points in NFKD only; the canonical maximum is 4 here,
        # but a name of 255 units of the worst case must fit the buffers.
        worst = [0x1D160] * 63 + [0x0344] * 127  # 126 + 127 UTF-16 units
        self.compare([worst, [0x41] * 255, [0xAC00] * 255, [0x0344] * 255])
        data = " ".join(["41"] * 256) + "\n"
        r = subprocess.run([ARGS.tool], input=data.encode(), capture_output=True)
        self.assertEqual(r.returncode, 2)  # more than 255 units: refused, not truncated

    def test_fixture_names(self):
        if not ARGS.names_dir:
            self.skipTest("no --names-dir")
        total = 0
        for name, nonnfc in (("names-nfd", 22), ("names-nfc", 0)):
            path = os.path.join(ARGS.names_dir, name + ".all")
            if not os.path.exists(path):
                self.skipTest(path + " missing")
            comps = []
            for line in open(path, encoding="utf-8"):
                leaf = line.rstrip("\n").split("/")[-1]  # the stored name is the last component
                if leaf:
                    comps.append([ord(c) for c in leaf])
            self.compare(comps)
            got = [g[0] for g in run_tool(comps)]
            self.assertEqual(sum(1 for g in got if g == 0), nonnfc, name)
            total += len(comps)
        self.assertGreater(total, 50)


if __name__ == "__main__":
    ap = argparse.ArgumentParser()
    ap.add_argument("--tool", required=True)
    ap.add_argument("--names-dir")
    ARGS, rest = ap.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
