# SPDX-License-Identifier: GPL-2.0-or-later
"""Unit tests for the harness itself. Run: python3 -m unittest discover -s tests/unit/harness"""

import copy
import importlib.util
import json
import os
import subprocess
import sys
import tempfile
import textwrap
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, TESTS)

from harness import core, report, runner, transport  # noqa: E402

SHA = "0123456789abcdef0123456789abcdef01234567"


def good_doc():
    return report.build_result(
        SHA, True, "A", "macOS 27.2 (26B123) arm64", "Debian 13", None, "2026.9.28",
        "2026-11-14T10:00:00Z", "2026-11-14T10:00:05Z",
        [
            {"id": "I-INT-01", "status": "pass", "duration_s": 1.5, "evidence": ["evidence/x/I-INT-01.log"], "note": None},
            {"id": "I-INT-02", "status": "skip", "duration_s": 0.0, "evidence": [], "note": "not installed"},
        ],
    )


def write_case_module(root, name, body):
    path = os.path.join(root, name)
    with open(path, "w", encoding="utf-8") as f:
        f.write("from harness import case, Skip, Fail\n" + textwrap.dedent(body))
    return path


class DiscoveryTests(unittest.TestCase):
    def test_discover_select_and_errors(self):
        with tempfile.TemporaryDirectory() as d:
            write_case_module(d, "a.py", """
                @case("X-ONE-01", envs=("A",))
                def one(ctx): pass
                @case("X-ONE-02", envs=("A", "B"), title="two")
                def two(ctx): pass
            """)
            with open(os.path.join(d, "plain.py"), "w") as f:
                f.write("raise SystemExit('must not be imported: no case marker')\n")
            cases = core.discover([d])
            self.assertEqual([c.id for c in cases], ["X-ONE-01", "X-ONE-02"])
            self.assertEqual(cases[1].title, "two")
            self.assertEqual([c.id for c in core.select(cases, "", "B")], ["X-ONE-02"])
            self.assertEqual([c.id for c in core.select(cases, "X-ONE-*", "A")], ["X-ONE-01", "X-ONE-02"])
            with self.assertRaises(core.DiscoveryError):
                core.select(cases, "NOPE-01", "A")

    def test_duplicate_id_rejected(self):
        with tempfile.TemporaryDirectory() as d:
            write_case_module(d, "a.py", '@case("X-DUP-01")\ndef f(ctx): pass\n')
            write_case_module(d, "b.py", '@case("X-DUP-01")\ndef g(ctx): pass\n')
            with self.assertRaises(core.DiscoveryError):
                core.discover([d])

    def test_bad_id_rejected(self):
        with self.assertRaises(core.DiscoveryError):
            core.case("lower-case")

    def test_repo_cases_discoverable(self):
        ids = [c.id for c in core.discover([os.path.join(TESTS, "integration")])]
        self.assertIn("I-INT-01", ids)
        self.assertIn("I-INT-03", ids)


class RunnerTests(unittest.TestCase):
    def test_statuses(self):
        with tempfile.TemporaryDirectory() as d:
            write_case_module(d, "m.py", """
                @case("X-RUN-01")
                def ok(ctx): ctx.log("hello")
                @case("X-RUN-02")
                def bad(ctx): raise Fail("nope")
                @case("X-RUN-03")
                def skipped(ctx): ctx.skip("because")
                @case("X-RUN-04")
                def skip_no_note(ctx): raise Skip("")
                @case("X-RUN-05")
                def boom(ctx): raise KeyError("k")
                @case("X-RUN-06")
                def assertion(ctx): assert 1 == 2, "math"
            """)
            cases = core.discover([d])
            res = os.path.join(d, "res")
            recs = runner.run_cases(cases, "host", "rid", res, transport.LocalTransport(), quiet=True)
            got = dict((r["id"], r) for r in recs)
            self.assertEqual([got["X-RUN-0%d" % i]["status"] for i in range(1, 7)],
                             ["pass", "fail", "skip", "error", "error", "fail"])
            self.assertEqual(got["X-RUN-03"]["note"], "because")
            self.assertIn("without a note", got["X-RUN-04"]["note"])
            self.assertEqual(got["X-RUN-01"]["evidence"], ["evidence/rid/X-RUN-01.log"])
            self.assertTrue(os.path.isfile(os.path.join(res, got["X-RUN-01"]["evidence"][0])))
            doc = report.build_result(SHA, False, "host", "h", None, None, None,
                                      "2026-11-14T10:00:00Z", "2026-11-14T10:00:01Z", recs)
            self.assertEqual(report.validate(doc), [])

    def test_cleanup_runs_after_failure(self):
        with tempfile.TemporaryDirectory() as d:
            marker = os.path.join(d, "cleaned")
            write_case_module(d, "m.py", """
                @case("X-CLN-01")
                def f(ctx):
                    ctx.add_cleanup(lambda: open(%r, "w").close())
                    raise Fail("x")
            """ % marker)
            recs = runner.run_cases(core.discover([d]), "host", "rid", os.path.join(d, "r"),
                                    transport.LocalTransport(), quiet=True)
            self.assertEqual(recs[0]["status"], "fail")
            self.assertTrue(os.path.exists(marker))


class SchemaTests(unittest.TestCase):
    def test_valid(self):
        self.assertEqual(report.validate(good_doc()), [])

    def _bad(self, mutate, needle):
        doc = good_doc()
        mutate(doc)
        errs = report.validate(doc)
        self.assertTrue(any(needle in e for e in errs), "%r not in %r" % (needle, errs))

    def test_rejections(self):
        self._bad(lambda d: d.pop("git_sha"), "missing key: git_sha")
        self._bad(lambda d: d.update(extra=1), "unexpected key: extra")
        self._bad(lambda d: d.update(git_sha="abc"), "git_sha")
        self._bad(lambda d: d.update(dirty_tree="yes"), "dirty_tree")
        self._bad(lambda d: d.update(env="C"), "env")
        self._bad(lambda d: d.update(host_os=""), "host_os")
        self._bad(lambda d: d.update(started="yesterday"), "started")
        self._bad(lambda d: d.update(finished="2026-11-14T09:00:00Z"), "before started")
        self._bad(lambda d: d["cases"][0].update(status="passed"), "status")
        self._bad(lambda d: d["cases"][0].update(duration_s=-1), "duration_s")
        self._bad(lambda d: d["cases"][0].update(duration_s=True), "duration_s")
        self._bad(lambda d: d["cases"][0].update(evidence=["/abs/x"]), "evidence path")
        self._bad(lambda d: d["cases"][0].update(evidence=["a/../b"]), "evidence path")
        self._bad(lambda d: d["cases"][1].update(id="I-INT-01"), "duplicate id")
        self._bad(lambda d: d["cases"][0].pop("note"), "missing key note")

    def test_skip_without_note_is_error(self):
        for note in (None, "", "   "):
            doc = good_doc()
            doc["cases"][1]["note"] = note
            errs = report.validate(doc)
            self.assertTrue(any("skip needs a non-empty note" in e for e in errs), (note, errs))
        doc = good_doc()
        doc["cases"][0]["note"] = None  # a pass may have no note
        self.assertEqual(report.validate(doc), [])

    def test_write_result_name_and_refusal(self):
        with tempfile.TemporaryDirectory() as d:
            path = report.write_result(good_doc(), d)
            self.assertEqual(os.path.basename(path), "2026-11-14-0123456-A.json")
            with open(path) as f:
                self.assertEqual(report.validate(json.load(f)), [])
            bad = good_doc()
            bad["cases"][1]["note"] = ""
            with self.assertRaises(ValueError):
                report.write_result(bad, d)
            self.assertEqual(os.listdir(d), ["2026-11-14-0123456-A.json"])


class TransportTests(unittest.TestCase):
    def test_local_run_and_tree_roundtrip(self):
        t = transport.LocalTransport()
        r = t.run("printf abc; printf err >&2; exit 3")
        self.assertEqual((r.rc, r.out, r.err), (3, b"abc", b"err"))
        self.assertEqual(t.run("cat", input=b"\x00\xffbin").out, b"\x00\xffbin")
        with tempfile.TemporaryDirectory() as d:
            src = os.path.join(d, "src")
            os.makedirs(os.path.join(src, "pkg", "__pycache__"))
            for rel, data in (("pkg/a.txt", b"A"), ("pkg/__pycache__/x.pyc", b"junk"), ("top.bin", b"\x00\x01")):
                with open(os.path.join(src, rel), "wb") as f:
                    f.write(data)
            t.put_tree(src, ["pkg", "top.bin"], os.path.join(d, "remote"))
            self.assertTrue(os.path.isfile(os.path.join(d, "remote", "pkg", "a.txt")))
            self.assertFalse(os.path.exists(os.path.join(d, "remote", "pkg", "__pycache__")))
            t.get_tree(os.path.join(d, "remote"), os.path.join(d, "back"))
            with open(os.path.join(d, "back", "top.bin"), "rb") as f:
                self.assertEqual(f.read(), b"\x00\x01")

    def test_extract_refuses_escape(self):
        import io
        import tarfile

        buf = io.BytesIO()
        with tarfile.open(fileobj=buf, mode="w") as tf:
            ti = tarfile.TarInfo("../evil")
            ti.size = 1
            tf.addfile(ti, io.BytesIO(b"x"))
        with tempfile.TemporaryDirectory() as d:
            with self.assertRaises(RuntimeError):
                transport.extract_tar(buf.getvalue(), d)


class RemountHashLogicTests(unittest.TestCase):
    @classmethod
    def setUpClass(cls):
        path = os.path.join(TESTS, "integration", "integrity", "remount_hash.py")
        spec = importlib.util.spec_from_file_location("remount_hash_under_test", path)
        cls.m = importlib.util.module_from_spec(spec)
        spec.loader.exec_module(cls.m)

    def test_sizes_cover_range_and_are_deterministic(self):
        s = self.m.plan_sizes(1, 100)
        self.assertEqual(len(s), 100)
        self.assertEqual((s[0], s[1]), (1024, 64 * 1024 * 1024))
        self.assertTrue(all(1024 <= x <= 64 * 1024 * 1024 for x in s))
        self.assertEqual(s, self.m.plan_sizes(1, 100))
        self.assertNotEqual(s, self.m.plan_sizes(2, 100))

    def test_compare_catches_each_kind_of_difference(self):
        with tempfile.TemporaryDirectory() as d:
            exp = self.m.write_files(d, 7, [1024, 5000, 3 * 1024 * 1024 + 17])
            self.assertEqual(self.m.compare(exp, self.m.hash_tree(d)), [])
            victim = os.path.join(d, self.m.rel_name(1))
            with open(victim, "r+b") as f:
                f.seek(100)
                b = f.read(1)
                f.seek(100)
                f.write(bytes([b[0] ^ 1]))
            bad = self.m.compare(exp, self.m.hash_tree(d))
            self.assertEqual(len(bad), 1)
            self.assertIn("sha256", bad[0])
            os.remove(os.path.join(d, self.m.rel_name(0)))
            with open(os.path.join(d, "extra.bin"), "wb") as f:
                f.write(b"x")
            bad = self.m.compare(exp, self.m.hash_tree(d))
            self.assertTrue(any("missing" in b for b in bad))
            self.assertTrue(any("extra" in b for b in bad))


class CliTests(unittest.TestCase):
    def test_host_run_writes_valid_result_and_exit_codes(self):
        with tempfile.TemporaryDirectory() as d:
            roots = os.path.join(d, "cases")
            os.makedirs(roots)
            write_case_module(roots, "m.py", """
                @case("X-CLI-01", envs=("host",))
                def ok(ctx): pass
                @case("X-CLI-02", envs=("host",))
                def sk(ctx): ctx.skip("no hardware")
            """)
            out = os.path.join(d, "out")
            base = [sys.executable, "-B", os.path.join(TESTS, "harness", "run.py"), "--env", "host", "--roots", roots,
                    "--out", out, "--git-sha", SHA, "--dirty", "true", "--quiet"]
            r = subprocess.run(base, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(r.returncode, 0, r.stderr.decode())
            path = r.stdout.decode().strip()
            with open(path) as f:
                doc = json.load(f)
            self.assertEqual(report.validate(doc), [])
            self.assertTrue(doc["dirty_tree"])
            self.assertEqual([c["status"] for c in doc["cases"]], ["pass", "skip"])
            self.assertEqual(doc["guest_os"], None)
            r = subprocess.run(base + ["--cases", "NOPE-*"], stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(r.returncode, 2)
            write_case_module(roots, "n.py", '@case("X-CLI-03", envs=("host",))\ndef f(ctx): raise Fail("x")\n')
            r = subprocess.run(base, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            self.assertEqual(r.returncode, 1)


if __name__ == "__main__":
    unittest.main()
