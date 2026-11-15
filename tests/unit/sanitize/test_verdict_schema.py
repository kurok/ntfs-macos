# SPDX-License-Identifier: GPL-2.0-or-later
"""T-M3-19: the strict verdict parser accepts exactly what the schema accepts.

Differential test (Python standard library only) of src/sanitize/verdict.c, through
the verdict_tool filter, against docs/schemas/probe.schema.json:

  1. Corpus: every document that tests/unit/contracts/test_probe_schema.py checks with
     ok() or bad() is captured (the test class is re-run with those two hooks replaced)
     and sent to the parser, in compact, indented and non-ASCII forms. The parser must
     accept it if and only if the schema does, after the rules the schema cannot express
     (label and path length in BYTES, path count equal to min(nonnfc_count, 16), no
     lone surrogate, no NUL, integers without sign, fraction or exponent, 64 KiB).
  2. Mutations: seeded random mutations of the corpus at the byte level (flip, insert,
     delete, structural characters, invalid UTF-8, NUL) and at the document level
     (drop, duplicate, retype, hostile numbers and strings). The expectation is computed
     by an independent reader (json.loads with strict hooks and the schema validator).
  3. For every accepted input the parsed fields equal what Python reads, and the probe's
     JSON writer reproduces the verdict (rt=same): the oracle of F-SANITIZE-01.

Run with -I and -B (the standard library only, no bytecode).
"""
import argparse
import copy
import importlib.util
import json
import os
import random
import re
import subprocess
import sys
import unittest

ARGS = None
MAX_BYTES = 64 * 1024
SEED = 0x7E57


def load_contract_module():
    path = os.path.join(ARGS.contracts_dir, "test_probe_schema.py")
    spec = importlib.util.spec_from_file_location("contract_probe_schema", path)
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    mod.ARGS = argparse.Namespace(schema=ARGS.schema, header=ARGS.header,
                                  architecture=ARGS.architecture)
    return mod


def capture_corpus(mod):
    """(document, schema_says_valid) for every ok() and bad() call of the contract test."""
    docs = []

    class Capture(mod.ProbeSchema):
        def ok(self, doc):
            docs.append(copy.deepcopy(doc))

        def bad(self, doc, fragment):
            docs.append(copy.deepcopy(doc))

    suite = unittest.defaultTestLoader.loadTestsFromTestCase(Capture)
    with open(os.devnull, "w") as null:
        result = unittest.TextTestRunner(stream=null, verbosity=0).run(suite)
    if not result.wasSuccessful():
        raise RuntimeError("the contract test itself failed while capturing its corpus")
    return docs


class Reader:
    """The independent expectation: what a strict JSON reader plus the schema say."""

    def __init__(self, mod, schema, flags, reasons, states, logfiles, hiberfiles):
        self.mod = mod
        self.schema = schema
        self.flags, self.reasons = flags, reasons
        self.states, self.logfiles, self.hiberfiles = states, logfiles, hiberfiles

    @staticmethod
    def _no_dups(pairs):
        out = {}
        for k, v in pairs:
            if k in out:
                raise ValueError("duplicate key")
            out[k] = v
        return out

    @staticmethod
    def _bad_int(s):
        # Number form: no sign (the parser rejects "-0" although Python reads it as 0).
        if s.startswith("-"):
            raise ValueError("signed integer")
        return int(s)

    @staticmethod
    def _bad_float(s):
        raise ValueError("fraction or exponent")

    @staticmethod
    def _bad_const(s):
        raise ValueError("NaN or Infinity")

    def read(self, raw):
        """Return the document if the input is acceptable, else None."""
        if len(raw) > MAX_BYTES:
            return None
        try:
            text = raw.decode("utf-8")  # strict: no overlong form, no surrogate
        except UnicodeDecodeError:
            return None
        if "\x00" in text:
            return None
        try:
            doc = json.loads(text, object_pairs_hook=self._no_dups, parse_int=self._bad_int,
                             parse_float=self._bad_float, parse_constant=self._bad_const)
        except (ValueError, RecursionError):
            return None
        if not isinstance(doc, dict):
            return None
        if self.mod.validate(doc, self.schema):
            return None
        if not self.extra_ok(doc):
            return None
        return doc

    @staticmethod
    def _strings(value):
        if isinstance(value, str):
            yield value
        elif isinstance(value, list):
            for v in value:
                yield from Reader._strings(v)
        elif isinstance(value, dict):
            for k, v in value.items():
                yield k
                yield from Reader._strings(v)

    def extra_ok(self, doc):
        """Rules of section 3.1 that the schema cannot express."""
        for s in self._strings(doc):
            if any(0xD800 <= ord(c) <= 0xDFFF for c in s):
                return False  # a lone surrogate has no UTF-8 form
        if len(doc["label"].encode("utf-8")) > 512:
            return False
        for p in doc["nonnfc_paths"]:
            if len(p.encode("utf-8")) > 512:
                return False
        if len(doc["nonnfc_paths"]) != min(doc["nonnfc_count"], 16):
            return False
        return True

    def render(self, doc):
        """The verdict_tool line for an accepted document."""
        def bits(names, present):
            got = [n for n in names if n in present]
            return ",".join(got) if got else "-"

        def hexs(s):
            b = s.encode("utf-8")
            return b.hex() if b else "-"

        ver = doc["ntfs_version"] if doc["ntfs_version"] is not None else "-"
        lsn = "%016X" % int(doc["logfile_lsn"], 16) if doc["logfile_lsn"] is not None else "-"
        paths = ",".join(hexs(p) for p in doc["nonnfc_paths"]) if doc["nonnfc_paths"] else "-"
        return ("ok is_ntfs=%d bitlocker=%d version=%s label=%s serial=%016X total=%d cluster=%d "
                "sector=%d flags=%s logfile=%s lsn=%s hiberfile=%s state=%s reasons=%s count=%d "
                "paths=%s rt=same") % (
            doc["is_ntfs"], doc["bitlocker"], ver, hexs(doc["label"]), int(doc["serial"], 16),
            doc["total_sectors"], doc["cluster_size"], doc["sector_size"],
            bits(self.flags, doc["volume_flags"]), doc["logfile"], lsn, doc["hiberfile"],
            doc["state"], bits(self.reasons, doc["reason_codes"]), doc["nonnfc_count"], paths)


def run_tool(inputs):
    """Send raw inputs to verdict_tool, return one result line each."""
    out = []
    for i in range(0, len(inputs), 100):
        chunk = inputs[i:i + 100]
        data = "".join((r.hex() if r else "-") + "\n" for r in chunk)
        p = subprocess.run([ARGS.tool], input=data, capture_output=True, text=True, check=False)
        if p.returncode != 0:
            raise RuntimeError("verdict_tool exited with %d: %s" % (p.returncode, p.stderr))
        lines = p.stdout.split("\n")[:-1]
        if len(lines) != len(chunk):
            raise RuntimeError("verdict_tool answered %d lines for %d inputs" % (len(lines), len(chunk)))
        out.extend(lines)
    return out


def dumps_forms(doc):
    forms = []
    for kw in ({"ensure_ascii": True}, {"ensure_ascii": False, "indent": 2},
               {"ensure_ascii": True, "separators": (",", ":")}):
        try:
            forms.append(json.dumps(doc, **kw).encode("utf-8"))
        except (UnicodeEncodeError, ValueError):
            pass  # a lone surrogate cannot be written as raw UTF-8
    return forms


STRUCT = [b"{", b"}", b"[", b"]", b",", b":", b'"', b"\\", b"\\u0000", b"\\ud800", b"\\udc00", b"-",
          b".", b"e", b"E", b"0", b"00", b" ", b"\n", b"\t", b"\x00", b"\x1b", b"\xc0\xaf",
          b"\xed\xa0\x80", b"\xff", b"\xef\xbb\xbf", b"null", b"true", b"false", b"\\n", b"\\/"]

HOSTILE_VALUES = [
    "null", "true", "false", "0", "1", "-1", "01", "1.0", "1e2", "2", "4294967295", "4294967296",
    "9223372036854775807", "9223372036854775808", "18446744073709551616", "[]", "{}", "[[]]", '""',
    '"0x1A2B3C4D5E6F7788"', '"0x1a2b3c4d5e6f7788"', '"0x1A2B3C4D5E6F778"', '"3.1"', '"clean"',
    '"FLAG_DIRTY"', '["FLAG_DIRTY"]', '["NAMES_NOT_NFC"]', '["FLAG_DIRTY","FLAG_DIRTY"]',
    '["\\u001b[31m"]', '"\\u0000"', '"\\ud800"', '"\\ud83d\\ude00"', '"' + "x" * 513 + '"',
    '"' + "é" * 257 + '"', '["/a"]', '["/' + "p" * 512 + '"]',
]


def mutate_text(rng, raw):
    raw = bytearray(raw)
    for _ in range(rng.randint(1, 3)):
        op = rng.randrange(6)
        pos = rng.randrange(len(raw) + 1) if raw else 0
        if op == 0 and raw:
            raw[min(pos, len(raw) - 1)] ^= 1 << rng.randrange(8)
        elif op == 1:
            raw[pos:pos] = rng.choice(STRUCT)
        elif op == 2 and raw:
            del raw[min(pos, len(raw) - 1):min(pos, len(raw) - 1) + rng.randint(1, 4)]
        elif op == 3 and raw:
            raw[min(pos, len(raw) - 1)] = rng.randrange(256)
        elif op == 4 and raw:
            a = min(pos, len(raw) - 1)
            raw[a:a] = raw[a:a + rng.randint(1, 20)]  # duplicate a run
        else:
            raw[pos:pos] = rng.choice(STRUCT)
    return bytes(raw)


def mutate_doc(rng, doc, keys):
    """A document-level mutation, returned as JSON text (a duplicate key needs text)."""
    d = copy.deepcopy(doc)
    key = rng.choice(keys)
    op = rng.randrange(5)
    if op == 0:
        d.pop(key, None)
    elif op == 1:
        text = json.dumps(d)
        return (text[:-1] + ", %s: %s}" % (json.dumps(key), rng.choice(HOSTILE_VALUES))).encode()
    elif op == 2:
        d["extra_" + key] = 1
    else:
        parts = json.dumps(d)
        # replace the value of `key` by a hostile literal, as text
        d[key] = "@@HOSTILE@@"
        text = json.dumps(d).replace('"@@HOSTILE@@"', rng.choice(HOSTILE_VALUES))
        return text.encode()
    return json.dumps(d).encode()


POOL = ([chr(c) for c in range(1, 0x80)] * 2 + [chr(c) for c in (0x80, 0x85, 0x9F, 0xA0, 0xAD, 0xE9, 0x7FF,
        0x800, 0x202E, 0x2028, 0x4E2D, 0xE000, 0xFEFF, 0xFFFD, 0xFFFE, 0xFFFF, 0x10000, 0x1F600,
        0xF0000, 0x10FFFD, 0x10FFFF)])


def rand_text(rng, max_bytes):
    out, used = [], 0
    for _ in range(rng.choice([0, 1, 2, 5, 20, 100, 600])):
        c = rng.choice(POOL)
        n = len(c.encode("utf-8"))
        if used + n > max_bytes:
            break
        out.append(c)
        used += n
    return "".join(out)


def random_valid(rng, rd):
    reasons = rng.sample(rd.reasons, rng.choice([0, 0, 1, 2, 5, 18]))
    names = "NAMES_NOT_NFC" in reasons
    count = rng.choice([1, 2, 15, 16, 17, 100, 2 ** 32 - 1]) if names else 0
    state = "clean" if not reasons else rng.choice([x for x in rd.states if x != "clean"])
    logfile = rng.choice(rd.logfiles)
    return {
        "schema": 1,
        "is_ntfs": rng.choice([True, False]),
        "ntfs_version": rng.choice([None, "%d.%d" % (rng.randint(1, 3), rng.randint(0, 9))]),
        "label": rand_text(rng, 512),
        "serial": "0x%016X" % rng.choice([0, 1, 2 ** 64 - 1, rng.getrandbits(64)]),
        "total_sectors": rng.choice([1, 2 ** 63 - 1, rng.getrandbits(rng.randint(1, 63)) or 1]),
        "cluster_size": 512 << rng.randint(0, 12),
        "sector_size": 512 << rng.randint(0, 3),
        "volume_flags": rng.sample(rd.flags, rng.randint(0, 9)),
        "logfile": logfile,
        "logfile_lsn": None if logfile == "unreadable" else "0x%016X" % rng.getrandbits(64),
        "hiberfile": rng.choice(rd.hiberfiles),
        "bitlocker": rng.choice([True, False]),
        "state": state,
        "reason_codes": reasons,
        "nonnfc_count": count,
        "nonnfc_paths": [rand_text(rng, 512) for _ in range(min(count, 16))],
    }


def esc_string(rng, s):
    """A JSON string with a random subset of its characters written as \\u escapes."""
    out = ['"']
    for c in s:
        o = ord(c)
        if c in '"\\':
            out.append("\\" + c)
        elif o < 0x20 or 0xD800 <= o <= 0xDFFF or rng.random() < 0.3:
            if o >= 0x10000:
                o -= 0x10000
                out.append("\\u%04x\\u%04X" % (0xD800 + (o >> 10), 0xDC00 + (o & 0x3FF)))
            else:
                out.append(("\\u%04x" if rng.random() < 0.5 else "\\u%04X") % o)
        else:
            out.append(c)
    out.append('"')
    return "".join(out)


def esc_value(rng, v):
    if isinstance(v, str):
        return esc_string(rng, v)
    if isinstance(v, list):
        return "[" + rng.choice([",", " , ", ",\n"]).join(esc_value(rng, x) for x in v) + "]"
    return json.dumps(v)


def esc_dumps(rng, doc):
    items = list(doc.items())
    rng.shuffle(items)
    ws = rng.choice(["", " ", "\n  ", "\t"])
    body = ("," + ws).join(esc_string(rng, k) + rng.choice([":", " : ", ": "]) + esc_value(rng, v)
                           for k, v in items)
    return (ws + "{" + ws + body + ws + "}" + rng.choice(["", "\n", " \r\n"])).encode("utf-8")


class VerdictSchema(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        cls.mod = load_contract_module()
        with open(ARGS.schema, encoding="utf-8") as f:
            cls.schema = cls.mod.loads_strict(f.read())
        with open(ARGS.header, encoding="utf-8") as f:
            header = f.read()
        x = lambda name: cls.mod.xmacro_strings(header, name)
        cls.reader = Reader(cls.mod, cls.schema, x("NTFSMAC_PROBE_VOLFLAG_LIST"),
                            x("NTFSMAC_PROBE_REASON_LIST"), x("NTFSMAC_PROBE_STATE_LIST"),
                            x("NTFSMAC_PROBE_LOGFILE_LIST"), x("NTFSMAC_PROBE_HIBERFILE_LIST"))
        with open(ARGS.architecture, encoding="utf-8") as f:
            cls.example = cls.mod.doc_example(f.read())
        cls.corpus = capture_corpus(cls.mod)
        cls.keys = cls.schema["required"]

    def compare(self, inputs, label):
        results = run_tool(inputs)
        accepted = rejected = 0
        for raw, got in zip(inputs, results):
            doc = self.reader.read(raw)
            if doc is None:
                rejected += 1
                if got.startswith("ok"):
                    self.fail("%s: parser accepted what the schema or the grammar rejects:\n%s\n-> %s"
                              % (label, raw[:400], got))
            else:
                accepted += 1
                want = self.reader.render(doc)
                if got != want:
                    self.fail("%s: parser result differs:\n%s\n got %s\nwant %s" % (label, raw[:400], got, want))
        return accepted, rejected

    def test_corpus_size(self):
        self.assertGreater(len(self.corpus), 60)

    def test_corpus_forms(self):
        inputs = []
        for doc in [self.example] + self.corpus:
            inputs.extend(dumps_forms(doc))
        acc, rej = self.compare(inputs, "corpus")
        self.assertGreater(acc, 5)
        self.assertGreater(rej, 50)

    def test_example_of_section_3_1(self):
        acc, rej = self.compare(dumps_forms(self.example), "example")
        self.assertEqual((acc > 0, rej), (True, 0))

    def test_length_caps(self):
        # exactly 64 KiB (padded with whitespace) parses, one byte more does not
        base = json.dumps(self.example).encode()
        pad = base + b" " * (MAX_BYTES - len(base))
        self.assertEqual(self.compare([pad], "64 KiB"), (1, 0))
        self.assertEqual(self.compare([pad + b" "], "64 KiB + 1"), (0, 1))
        self.assertEqual(run_tool([pad + b" "]), ["too_large"])

    def test_mutations(self):
        rng = random.Random(SEED)
        seeds = []
        for doc in [self.example] + self.corpus:
            seeds.extend(dumps_forms(doc)[:1])
        inputs = []
        for _ in range(2500):
            inputs.append(mutate_text(rng, rng.choice(seeds)))
        for _ in range(1500):
            inputs.append(mutate_doc(rng, rng.choice([self.example] + self.corpus[:20]), self.keys))
        acc, rej = self.compare(inputs, "mutations")
        self.assertGreater(rej, 1000)

    def test_random_valid_documents(self):
        """Valid documents in random spellings: always accepted, fields equal."""
        rng = random.Random(SEED + 1)
        inputs = []
        for _ in range(1500):
            doc = random_valid(rng, self.reader)
            self.assertEqual(self.mod.validate(doc, self.schema), [], doc)
            self.assertTrue(self.reader.extra_ok(doc))
            inputs.append(esc_dumps(rng, doc))
            inputs.extend(dumps_forms(doc)[:1])
        acc, rej = self.compare(inputs, "random valid")
        self.assertEqual((acc, rej), (len(inputs), 0))

    def test_valid_documents_with_one_mutation(self):
        """One schema-level change to a valid document: accept iff the schema does."""
        rng = random.Random(SEED + 2)
        hostile = [json.loads(h) for h in HOSTILE_VALUES if h != "01"]
        inputs = []
        for _ in range(3000):
            doc = random_valid(rng, self.reader)
            key = rng.choice(self.keys)
            roll = rng.randrange(3)
            if roll == 0:
                doc.pop(key)
            elif roll == 1:
                other = random_valid(rng, self.reader)
                doc[key] = other[rng.choice(self.keys)]
            else:
                doc[key] = rng.choice(hostile)
            inputs.append(esc_dumps(rng, doc))
        acc, rej = self.compare(inputs, "one change")
        self.assertGreater(acc, 100)
        self.assertGreater(rej, 1000)

    def test_truncations(self):
        raw = json.dumps(self.example, indent=1).encode()
        inputs = [raw[:i] for i in range(len(raw))]
        acc, rej = self.compare(inputs, "truncations")
        self.assertEqual(acc, 0)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--tool", required=True)
    p.add_argument("--contracts-dir", required=True)
    p.add_argument("--schema", required=True)
    p.add_argument("--header", required=True)
    p.add_argument("--architecture", required=True)
    ARGS, rest = p.parse_known_args()
    sys.dont_write_bytecode = True
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
