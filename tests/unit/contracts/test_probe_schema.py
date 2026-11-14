# SPDX-License-Identifier: GPL-2.0-or-later
"""T-M3-00: check docs/schemas/probe.schema.json (stdlib only).

The jsonschema package is not available, so this holds a minimal validator for
the subset of JSON Schema 2020-12 that the schema uses: type, const, enum,
pattern, minimum, maximum, maxLength, minItems, maxItems, uniqueItems, items,
properties, required, additionalProperties, oneOf, allOf, if/then/else.
An unknown keyword in the schema fails the test, so the validator cannot
silently skip a constraint.

Checks:
  1. the example of ARCHITECTURE.md section 3.1 validates;
  2. a few more valid documents validate;
  3. negative documents (bad serial, unknown reason code, missing key, ...) fail;
  4. the enum lists of the schema equal the X-macro lists of probe.h;
  5. duplicate keys are rejected by the loader used here (the strict parser's
     rule; JSON Schema itself cannot express it).
"""
import argparse
import copy
import json
import re
import sys
import unittest

ANNOTATIONS = {"$schema", "$id", "title", "description"}
KEYWORDS = ANNOTATIONS | {
    "type", "const", "enum", "pattern", "minimum", "maximum", "maxLength",
    "minItems", "maxItems", "uniqueItems", "items", "properties", "required",
    "additionalProperties", "oneOf", "allOf", "if", "then", "else",
}


def jtype(value):
    if value is None:
        return "null"
    if isinstance(value, bool):
        return "boolean"
    if isinstance(value, int):
        return "integer"
    if isinstance(value, float):
        return "number"
    if isinstance(value, str):
        return "string"
    if isinstance(value, list):
        return "array"
    return "object"


def same(a, b):
    """JSON equality that does not equate True with 1."""
    return jtype(a) == jtype(b) and a == b


def validate(value, schema, path="$"):
    """Return a list of error strings; empty means valid."""
    unknown = set(schema) - KEYWORDS
    if unknown:
        raise ValueError("unsupported schema keyword(s) at %s: %s" % (path, sorted(unknown)))
    errs = []
    t = jtype(value)
    if "type" in schema and not (t == schema["type"] or (t == "integer" and schema["type"] == "number")):
        return ["%s: type %s, expected %s" % (path, t, schema["type"])]
    if "const" in schema and not same(value, schema["const"]):
        errs.append("%s: not const %r" % (path, schema["const"]))
    if "enum" in schema and not any(same(value, e) for e in schema["enum"]):
        errs.append("%s: %r not in enum" % (path, value))
    if t == "string":
        if "maxLength" in schema and len(value) > schema["maxLength"]:
            errs.append("%s: too long" % path)
        # ECMA-262 `$` matches only at the very end; Python's also matches
        # before a trailing newline, so translate it to \Z.
        if "pattern" in schema and re.search(re.sub(r"\$$", r"\\Z", schema["pattern"]), value) is None:
            errs.append("%s: %r does not match %s" % (path, value, schema["pattern"]))
    if t == "integer":
        if "minimum" in schema and value < schema["minimum"]:
            errs.append("%s: below minimum" % path)
        if "maximum" in schema and value > schema["maximum"]:
            errs.append("%s: above maximum" % path)
    if t == "array":
        if "minItems" in schema and len(value) < schema["minItems"]:
            errs.append("%s: too few items" % path)
        if "maxItems" in schema and len(value) > schema["maxItems"]:
            errs.append("%s: too many items" % path)
        if schema.get("uniqueItems"):
            seen = []
            for item in value:
                if any(same(item, s) for s in seen):
                    errs.append("%s: duplicate item %r" % (path, item))
                seen.append(item)
        if "items" in schema:
            for i, item in enumerate(value):
                errs += validate(item, schema["items"], "%s[%d]" % (path, i))
    if t == "object":
        props = schema.get("properties", {})
        for key in schema.get("required", []):
            if key not in value:
                errs.append("%s: missing key %s" % (path, key))
        if schema.get("additionalProperties") is False:
            for key in value:
                if key not in props:
                    errs.append("%s: unknown key %s" % (path, key))
        for key, sub in props.items():
            if key in value:
                errs += validate(value[key], sub, "%s.%s" % (path, key))
    if "oneOf" in schema:
        n = sum(1 for sub in schema["oneOf"] if not validate(value, sub, path))
        if n != 1:
            errs.append("%s: matches %d of oneOf, expected 1" % (path, n))
    for sub in schema.get("allOf", []):
        errs += validate(value, sub, path)
    if "if" in schema:
        if not validate(value, schema["if"], path):
            if "then" in schema:
                errs += validate(value, schema["then"], path)
        elif "else" in schema:
            errs += validate(value, schema["else"], path)
    return errs


def no_duplicates(pairs):
    out = {}
    for k, v in pairs:
        if k in out:
            raise ValueError("duplicate key %s" % k)
        out[k] = v
    return out


def loads_strict(text):
    return json.loads(text, object_pairs_hook=no_duplicates)


def doc_example(architecture_md):
    """The JSON block that follows 'schema version 1' in section 3.1. The
    block is concrete JSON; an older `"a | b | c"` alternative, if one is ever
    reintroduced, is replaced by one concrete value."""
    sec = architecture_md.split("### 3.1 Safety probe interface", 1)[1]
    block = re.search(r"```json\n(.*?)\n```", sec, re.S).group(1)
    # The example lists DIRTY and FLAG_DIRTY, so the consistent choice is
    # state dirty_flag with a clean log and no hibernation file.
    choose = {"state": "dirty_flag"}

    def pick(m):
        key, alts = m.group(1), [a.strip() for a in m.group(2).split("|")]
        return '"%s": "%s"' % (key, choose.get(key, alts[0]))

    block = re.sub(r'"(\w+)":\s*"([^"]*\|[^"]*)"', pick, block)
    return loads_strict(block)


def xmacro_strings(header, name):
    m = re.search(r"#define\s+%s\(X\)((?:[^\n]*\\\n)*[^\n]*)" % name, header)
    return re.findall(r'X\(\s*\w+\s*,\s*"([^"]*)"\s*\)', m.group(1))


ARGS = None


class ProbeSchema(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(ARGS.schema, encoding="utf-8") as f:
            cls.schema = loads_strict(f.read())
        with open(ARGS.architecture, encoding="utf-8") as f:
            cls.example = doc_example(f.read())
        with open(ARGS.header, encoding="utf-8") as f:
            cls.header = f.read()

    def ok(self, doc):
        self.assertEqual(validate(doc, self.schema), [])

    def bad(self, doc, fragment):
        errs = validate(doc, self.schema)
        self.assertTrue(errs, "document unexpectedly valid")
        self.assertTrue(any(fragment in e for e in errs), "%r not in %r" % (fragment, errs))

    def mutated(self, **kw):
        doc = copy.deepcopy(self.example)
        for k, v in kw.items():
            if v is KeyError:
                del doc[k]
            else:
                doc[k] = v
        return doc

    # --- positive ---------------------------------------------------------
    def test_schema_uses_supported_keywords_only(self):
        validate(self.example, self.schema)  # raises on an unknown keyword

    def test_doc_example_validates(self):
        self.assertEqual(self.example["serial"], "0x1A2B3C4D5E6F7788")
        self.ok(self.example)

    def test_clean_volume_validates(self):
        self.ok(self.mutated(state="clean", volume_flags=[], reason_codes=[]))

    def test_unreadable_log_has_null_lsn(self):
        self.ok(self.mutated(logfile="unreadable", logfile_lsn=None, state="unknown",
                             reason_codes=["LOG_UNREADABLE"]))

    def test_null_version_and_extreme_integers(self):
        self.ok(self.mutated(ntfs_version=None, total_sectors=2**63 - 1,
                             cluster_size=2097152, sector_size=4096,
                             label="", volume_flags=[]))

    def test_all_flags_and_codes_at_the_caps(self):
        flags = xmacro_strings(self.header, "NTFSMAC_PROBE_VOLFLAG_LIST")
        codes = xmacro_strings(self.header, "NTFSMAC_PROBE_REASON_LIST")
        self.assertEqual((len(flags), len(codes)), (9, 17))
        self.ok(self.mutated(volume_flags=flags, reason_codes=codes))

    # --- negative ---------------------------------------------------------
    def test_bad_serial(self):
        for s in ["0x1a2b3c4d5e6f7788", "0x1A2B3C4D5E6F778", "0x1A2B3C4D5E6F77889",
                  "1A2B3C4D5E6F7788", "0X1A2B3C4D5E6F7788", "0x1A2B3C4D5E6F778G",
                  "0x1A2B3C4D5E6F7788\n", "../../x", 123]:
            with self.subTest(serial=s):
                self.bad(self.mutated(serial=s), "serial")

    def test_bad_logfile_lsn(self):
        for s in ["0x0000000001a2b3c4", "0x1A2B", "0", 5]:
            with self.subTest(lsn=s):
                self.bad(self.mutated(logfile_lsn=s), "logfile_lsn")

    def test_unknown_reason_code(self):
        self.bad(self.mutated(reason_codes=["FLAG_DIRTY", "TOTALLY_NEW_CODE"]), "reason_codes[1]")
        self.bad(self.mutated(reason_codes=["flag_dirty"]), "reason_codes[0]")

    def test_missing_key(self):
        for key in self.schema["required"]:
            with self.subTest(key=key):
                self.bad(self.mutated(**{key: KeyError}), "missing key %s" % key)

    def test_unknown_key(self):
        self.bad(self.mutated(reasons=["text from the disk"]), "unknown key reasons")

    def test_enum_violations(self):
        self.bad(self.mutated(state="clean | dirty_flag"), "state")
        self.bad(self.mutated(logfile="Clean"), "logfile")
        self.bad(self.mutated(hiberfile="hibernating"), "hiberfile")
        self.bad(self.mutated(volume_flags=["DIRTY", "SOMETHING_ELSE"]), "volume_flags[1]")

    def test_integer_ranges(self):
        self.bad(self.mutated(total_sectors=0), "total_sectors")
        self.bad(self.mutated(total_sectors=2**63), "total_sectors")
        self.bad(self.mutated(total_sectors="131071"), "total_sectors")
        self.bad(self.mutated(total_sectors=1.5), "total_sectors")
        self.bad(self.mutated(cluster_size=3000), "cluster_size")
        self.bad(self.mutated(cluster_size=256), "cluster_size")
        self.bad(self.mutated(cluster_size=4194304), "cluster_size")
        self.bad(self.mutated(sector_size=256), "sector_size")
        self.bad(self.mutated(schema=2), "schema")
        self.bad(self.mutated(schema=True), "schema")

    def test_version_label_types(self):
        self.bad(self.mutated(ntfs_version="4.0"), "ntfs_version")
        self.bad(self.mutated(ntfs_version="3.10"), "ntfs_version")
        self.bad(self.mutated(label="a\u0000b"), "label")
        self.bad(self.mutated(label="x" * 513), "label")
        self.bad(self.mutated(is_ntfs="true"), "is_ntfs")
        self.bad(self.mutated(bitlocker=0), "bitlocker")

    def test_array_caps_and_duplicates(self):
        self.bad(self.mutated(volume_flags=["DIRTY", "DIRTY"]), "duplicate")
        self.bad(self.mutated(reason_codes=["FLAG_DIRTY", "FLAG_DIRTY"]), "duplicate")

    def test_cross_field_rules(self):
        # null lsn with a readable log, and an lsn with an unreadable log
        self.bad(self.mutated(logfile_lsn=None), "logfile_lsn")
        self.bad(self.mutated(logfile="unreadable"), "logfile_lsn")
        # clean with reasons, and non-clean without
        self.bad(self.mutated(state="clean"), "reason_codes")
        self.bad(self.mutated(reason_codes=[]), "reason_codes")

    def test_duplicate_keys_rejected_by_loader(self):
        with self.assertRaises(ValueError):
            loads_strict('{"schema": 1, "schema": 1}')

    # --- header and schema agree -------------------------------------------
    def test_enums_match_probe_h(self):
        props = self.schema["properties"]
        pairs = [
            ("NTFSMAC_PROBE_STATE_LIST", props["state"]["enum"]),
            ("NTFSMAC_PROBE_LOGFILE_LIST", props["logfile"]["enum"]),
            ("NTFSMAC_PROBE_HIBERFILE_LIST", props["hiberfile"]["enum"]),
            ("NTFSMAC_PROBE_VOLFLAG_LIST", props["volume_flags"]["items"]["enum"]),
            ("NTFSMAC_PROBE_REASON_LIST", props["reason_codes"]["items"]["enum"]),
        ]
        for macro, enum in pairs:
            with self.subTest(macro=macro):
                self.assertEqual(xmacro_strings(self.header, macro), enum)
        self.assertEqual(props["volume_flags"]["maxItems"], 9)
        self.assertEqual(props["reason_codes"]["maxItems"], 17)

    def test_mirror_split_validates(self):
        # ADR-021: the probe could not read the log through the overlay view
        # in this example, so logfile is unreadable and the LSN null.
        self.ok(self.mutated(state="mirror_split", volume_flags=["DIRTY"],
                             reason_codes=["MFTMIRR_STALE", "FLAG_DIRTY"]))
        self.ok(self.mutated(state="mirror_split", volume_flags=[], logfile="unreadable",
                             logfile_lsn=None, reason_codes=["MFTMIRR_STALE", "LOG_UNREADABLE"]))
        self.bad(self.mutated(state="mirror_split", reason_codes=[]), "reason_codes")


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--schema", required=True)
    p.add_argument("--header", required=True)
    p.add_argument("--architecture", required=True)
    ARGS, rest = p.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + rest, verbosity=2)
