# SPDX-License-Identifier: GPL-2.0-or-later
"""U-ARGV-06: the builder's option set and the argv_check() allow-list agree
with integration/mount-options.json (T-M2-04). Python standard library only.

Usage: test_mount_options_json.py --tool <argv_tool> --json <mount-options.json>
"""
import argparse
import json
import subprocess
import sys
import unittest

ARGS = None


def run_tool(lines):
    out = subprocess.run(
        [ARGS.tool], input="".join(l + "\n" for l in lines), capture_output=True,
        text=True, check=True).stdout.splitlines()
    assert len(out) == len(lines), (lines, out)
    return out


def build(dec="rw", perm=0, um=0, hide=0):
    return run_tool(["build\t%s\t%d\t%d\t%d" % (dec, perm, um, hide)])[0]


def check(dec, opt):
    return run_tool(["check\t%s\t%s" % (dec, opt)])[0]


def key_of(option):
    """Key of a JSON 'option' entry such as 'uid=<console uid>' or 'allow_root'."""
    return option.split("=")[0].split(" ")[0]


def keys(optstr):
    return {t.split("=")[0] for t in optstr.split(",")}


class MountOptions(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        with open(ARGS.json, encoding="utf-8") as f:
            cls.doc = json.load(f)
        assert cls.doc["schema"] == "ntfsmac-mount-options"

    def test_always_set_equals_builder(self):
        always = {key_of(o["option"]) for o in self.doc["always"]}
        self.assertEqual(keys(build("rw", 0, 0, 0)), always)

    def test_always_values(self):
        by_key = {key_of(o["option"]): o["option"] for o in self.doc["always"]}
        toks = build("rw").split(",")
        for key in ("backend", "local", "norecover", "noatime", "windows_names",
                    "streams_interface", "fmask", "dmask"):
            self.assertIn(by_key[key], toks)
        self.assertIn("uid=501", toks)
        self.assertIn("gid=20", toks)

    def test_opt_in_set_equals_builder(self):
        opt_in = {key_of(o["option"]) for o in self.doc["opt_in"]}
        always = {key_of(o["option"]) for o in self.doc["always"]}
        everything = keys(build("ro", 1, 1, 1))
        # permissions replaces uid, gid, fmask, dmask (mount-options.json 'requires')
        ownership = {"uid", "gid", "fmask", "dmask"}
        self.assertEqual(everything, (always - ownership) | opt_in)
        perm = [o for o in self.doc["opt_in"] if key_of(o["option"]) == "permissions"][0]
        self.assertIn("uid", perm["requires"])
        # each opt-in alone
        for k, args in (("ro", ("ro", 0, 0, 0)), ("hide_hid_files", ("rw", 0, 0, 1)),
                        ("usermapping", ("rw", 0, 1, 0)), ("permissions", ("rw", 1, 0, 0))):
            self.assertIn(k, keys(build(*args)))
        self.assertIn("usermapping=/usr/local/etc/ntfs-macos/UserMapping",
                      build("rw", 0, 1, 0).split(","))
        self.assertNotIn("ro", keys(build("rw", 1, 1, 1)))

    def test_builder_output_accepted(self):
        for dec in ("rw", "ro"):
            for perm in (0, 1):
                for um in (0, 1):
                    for hide in (0, 1):
                        self.assertEqual(check(dec, build(dec, perm, um, hide)), "ok")

    def test_banned_never_built(self):
        banned = {key_of(o["option"]) for o in self.doc["banned"]
                  if not o["option"].startswith("any key")}
        for dec in ("rw", "ro"):
            for perm in (0, 1):
                for um in (0, 1):
                    for hide in (0, 1):
                        self.assertFalse(banned & keys(build(dec, perm, um, hide)))
        self.assertTrue({"recover", "remove_hiberfile", "fsname"} <= banned)

    def test_every_listed_key_single_token_addition(self):
        base = build("rw", 0, 0, 0)
        allowed_keys = ({key_of(o["option"]) for o in self.doc["always"]} |
                        {key_of(o["option"]) for o in self.doc["opt_in"]})
        lines, expect = [], []

        def add(opt, why):
            lines.append("check\trw\t%s,%s" % (base, opt))
            expect.append((opt, why))

        for section in ("ignored", "rejected", "banned"):
            for o in self.doc[section]:
                name = o["option"]
                k = key_of(name)
                if name.startswith("any key"):
                    continue
                if k in allowed_keys:
                    continue  # streams_interface, handled below
                add(k, section)
                add(k + "=1", section)
        for k in self.doc["not_rejected_by_backend"]:
            if k.startswith("an empty token"):
                lines.append("check\trw\t%s,," % base)
                expect.append((k, "empty"))
                continue
            k = k.split("=")[0].split(" ")[0]
            if k in allowed_keys:
                continue
            add(k, "not_rejected")
            add(k + "=1", "not_rejected")
        for line, (opt, why) in zip(lines, expect):
            pass
        for out, (opt, why) in zip(run_tool(lines), expect):
            self.assertNotEqual(out, "ok", "%s (%s) was accepted" % (opt, why))

    def test_value_grammars_from_json(self):
        # keys of always/opt_in appear only with their grammar
        base = build("rw", 0, 0, 0)
        sv = self.doc["streams_interface_values"]
        self.assertEqual(set(sv["allowed"]), {"openxattr", "none"})
        stripped = base.replace("streams_interface=openxattr,", "")
        for v in sv["allowed"]:
            self.assertEqual(check("rw", "streams_interface=%s,%s" % (v, stripped)), "ok")
        for v in ("xattr", "user_xattr", "windows", "", "OPENXATTR", "bogus"):
            self.assertNotEqual(check("rw", "streams_interface=%s,%s" % (v, stripped)), "ok")
        self.assertNotEqual(check("rw", "streams_interface,%s" % stripped), "ok")
        # rejected entries of the JSON
        self.assertNotEqual(check("rw", base.replace("uid=501", "uid=abc")), "ok")
        self.assertNotEqual(check("rw", base.replace("backend=fskit", "backend=other")), "ok")
        self.assertNotEqual(check("rw", base.replace("backend=fskit,", "")), "ok")
        # usermapping: only the fixed absolute path, and only without a space
        um = build("rw", 0, 1, 0)
        self.assertEqual(check("rw", um), "ok")
        for bad in ("usermapping=/missing", "usermapping=/usr/local/etc/ntfs-macos/User Mapping",
                    "usermapping=relative", "usermapping="):
            self.assertNotEqual(check("rw", base + "," + bad), "ok")
        # mandatory flags of the JSON: dropping each is rejected
        for o in self.doc["always"]:
            tok = o["option"]
            tok = tok.replace("<console uid>", "501").replace("<console gid>", "20")
            toks = base.split(",")
            self.assertIn(tok, toks)
            toks.remove(tok)
            self.assertNotEqual(check("rw", ",".join(toks)), "ok", tok)


if __name__ == "__main__":
    p = argparse.ArgumentParser()
    p.add_argument("--tool", required=True)
    p.add_argument("--json", required=True)
    ARGS, rest = p.parse_known_args()
    unittest.main(argv=[sys.argv[0]] + rest)
