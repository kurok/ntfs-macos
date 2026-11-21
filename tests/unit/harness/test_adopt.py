# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests of `report.py adopt` (M5-F05): the standalone result.json of a criterion run becomes a named result
file under tests/results/ with its evidence copied and its paths rewritten. No VM, no git needed.
Run: python3 -m unittest discover -s tests/unit/harness  (or this file)"""

import json
import os
import re
import subprocess
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
REPO = os.path.dirname(TESTS)
sys.path.insert(0, TESTS)

from harness import report  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"
NAME_RE = re.compile(r"^\d{4}-\d{2}-\d{2}-([0-9a-f]{7})-(host|A|B)(-[A-Za-z0-9._-]+)?\.json$")  # as in ci-local.sh


def case(cid, status="pass", ev=()):
    return {"id": cid, "status": status, "duration_s": 1.0, "evidence": list(ev), "note": "n in " + REPO}


class Adopt(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.src = os.path.join(self.tmp.name, "run")
        self.out = os.path.join(self.tmp.name, "results")
        os.makedirs(os.path.join(self.src, "soak-real"))
        with open(os.path.join(self.src, "plan.txt"), "w") as f:
            f.write("kind=soak at %s\n" % REPO)
        with open(os.path.join(self.src, "soak-real", "summary.json"), "w") as f:
            f.write("{}")
        with open(os.path.join(self.src, "state.tar.gz"), "wb") as f:
            f.write(b"\x1f\x8b archive")

    def write(self, cases=None, sha=SHA, dirty=False, env="B"):
        cases = cases if cases is not None else [case("S-SOAK-01", ev=["plan.txt", "soak-real/summary.json", "state.tar.gz"]),
                                                 case("S-SOAK-01-CONTROL", ev=["plan.txt"])]
        doc = report.build_result(sha, dirty, env, "macOS 27", None, None, None, "2026-11-21T10:00:00Z", "2026-11-21T16:30:00Z", cases)
        with open(os.path.join(self.src, "result.json"), "w") as f:
            json.dump(doc, f)
        return doc

    def adopt(self, env="B", suite=None):
        return report.adopt(self.src, env, self.out, REPO, suite)

    def test_accepts_a_clean_criterion_result(self):
        self.write()
        path = self.adopt()
        name = os.path.basename(path)
        self.assertEqual(name, "2026-11-21-0123456-B-soak.json")
        self.assertTrue(NAME_RE.match(name))
        doc = json.load(open(path))
        self.assertEqual(report.validate(doc), [])
        ids = [c["id"] for c in doc["cases"]]
        self.assertEqual(ids, ["S-SOAK-01", "S-SOAK-01-CONTROL", "STAGE-B-MODE"])
        mode = doc["cases"][-1]
        self.assertEqual((mode["status"], mode["note"]), ("pass", "criterion:soak"))
        self.assertEqual((doc["git_sha"], doc["dirty_tree"], doc["env"]), (SHA, False, "B"))

    def test_evidence_copied_and_paths_rewritten(self):
        self.write()
        doc = json.load(open(self.adopt()))
        ev = doc["cases"][0]["evidence"]
        self.assertEqual(ev, ["evidence/2026-11-21-0123456-B-soak/plan.txt", "evidence/2026-11-21-0123456-B-soak/soak-real/summary.json"])
        for p in ev:
            self.assertTrue(os.path.isfile(os.path.join(self.out, p)), p)
        # an archive is not stored and says so; host paths are scrubbed
        self.assertIn("evidence not stored: state.tar.gz", doc["cases"][0]["note"])
        self.assertNotIn(REPO, open(os.path.join(self.out, ev[0])).read())
        self.assertNotIn(REPO, doc["cases"][0]["note"])

    def refused(self, text, **kw):
        with self.assertRaises(ValueError) as cm:
            self.adopt(**kw)
        self.assertIn(text, str(cm.exception))
        self.assertFalse(os.path.exists(self.out) and [n for n in os.listdir(self.out) if not n.startswith(".")])

    def test_refuses_dirty(self):
        self.write(dirty=True)
        self.refused("dirty tree")

    def test_refuses_sha_less_and_invalid(self):
        doc = self.write()
        del doc["git_sha"]
        json.dump(doc, open(os.path.join(self.src, "result.json"), "w"))
        self.refused("invalid")
        doc["git_sha"] = ""
        json.dump(doc, open(os.path.join(self.src, "result.json"), "w"))
        self.refused("git_sha")
        open(os.path.join(self.src, "result.json"), "w").write("not json")
        self.refused("cannot read")
        os.unlink(os.path.join(self.src, "result.json"))
        self.refused("cannot read")

    def test_refuses_short_failed_foreign_or_missing_control(self):
        self.write([case("S-SOAK-01-SHORT"), case("S-SOAK-01-CONTROL-SHORT")])
        self.refused("cannot tell the criterion suite")
        self.write([case("S-SOAK-01"), case("S-SOAK-01-CONTROL-SHORT")])
        self.refused("not plain criterion cases")
        self.write([case("S-SOAK-01")])
        self.refused("criterion case(s) missing: S-SOAK-01-CONTROL")
        self.write([case("S-SOAK-01"), case("S-SOAK-01-CONTROL", "fail")])
        self.refused("not passed")
        self.write([case("S-SOAK-01"), case("S-SOAK-01-CONTROL"), case("S-KILL-01")])
        self.refused("cannot tell the criterion suite")
        self.write([case("S-SOAK-01"), case("S-SOAK-01-CONTROL"), case("STAGE-B-DAEMON")])
        self.refused("STAGE-")

    def test_refuses_wrong_env_and_existing_target(self):
        self.write()
        self.refused("environment B, not A", env="A")
        self.adopt()
        with self.assertRaises(ValueError) as cm:
            self.adopt()
        self.assertIn("exists already", str(cm.exception))

    def test_every_suite_has_a_valid_name_and_env(self):
        for suite, spec in report.CRITERION_SUITES.items():
            self.assertTrue(NAME_RE.match("2026-11-21-0123456-%s-%s.json" % (spec["env"], suite)), suite)

    def test_refuses_symlinked_or_escaping_evidence(self):
        outside = os.path.join(self.tmp.name, "secret.txt")
        with open(outside, "w") as f:
            f.write("not part of the run")
        os.symlink(outside, os.path.join(self.src, "link.txt"))
        self.write([case("S-SOAK-01", ev=["link.txt"]), case("S-SOAK-01-CONTROL")])
        self.refused("symlink or lies outside")
        # a symlinked directory inside the run leads out of it too
        os.symlink(self.tmp.name, os.path.join(self.src, "dirlink"))
        self.write([case("S-SOAK-01", ev=["dirlink/secret.txt"]), case("S-SOAK-01-CONTROL")])
        self.refused("symlink or lies outside")

    def test_refuses_a_reused_or_mixed_directory(self):
        self.write()
        prov = os.path.join(self.src, "provenance.txt")
        res = os.path.join(self.src, "result.json")
        t = os.path.getmtime(res)

        def put(sha, mtime):
            with open(prov, "w") as f:
                f.write("git_sha=%s\ndirty=false\n" % sha)
            os.utime(prov, (mtime, mtime))
        put("f" * 40, t - 10)
        self.refused("differs from the start record")
        put(SHA, t + 100)  # provenance written after result.json: an interrupted, reused directory
        self.refused("older than provenance.txt")
        put(SHA, t - 10)
        self.assertTrue(os.path.isfile(self.adopt()))

    def test_command_line(self):
        self.write()
        r = subprocess.run([sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "adopt", "--env", "B", "--dir", self.src,
                            "--out", self.out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 0, r.stderr)
        self.assertTrue(os.path.isfile(r.stdout.strip()))
        r = subprocess.run([sys.executable, "-B", os.path.join(TESTS, "harness", "report.py"), "adopt", "--env", "B", "--dir", self.src,
                            "--out", self.out], capture_output=True, text=True)
        self.assertEqual(r.returncode, 1)
        self.assertIn("refused", r.stderr)


if __name__ == "__main__":
    unittest.main()
