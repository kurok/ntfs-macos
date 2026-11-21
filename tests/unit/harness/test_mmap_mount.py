# SPDX-License-Identifier: GPL-2.0-or-later
"""Host-side tests of the mount wait and the single FSKit-refusal retry of Worker.start() (tests/integration/mmap/mmaplib.py).
No VM: the mount table and the worker process are fakes. Run: python3 -m unittest discover -s tests/unit/harness"""

import io
import os
import sys
import tempfile
import unittest

sys.dont_write_bytecode = True
HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
sys.path.insert(0, TESTS)
sys.path.insert(0, os.path.join(TESTS, "integration", "mmap"))

import mmaplib  # noqa: E402
from harness import core, runner  # noqa: E402

REFUSAL = "MFMount: MFMount(_:_:_:_:): Failed to initialize volume"


class FakeCtx(object):
    def __init__(self, work):
        self.work = work
        self.logs = []
        self.result_notes = []
        self.cleanups = []

    def log(self, msg):
        self.logs.append(msg)

    def evidence_file(self, suffix):
        return os.path.join(self.work, "I-X." + suffix)

    def add_cleanup(self, fn):
        self.cleanups.append(fn)

    def add_result_note(self, text):
        self.result_notes.append(text)


class FakeVol(object):
    def __init__(self, work):
        self.img = os.path.join(work, "vol.img")
        self.mnt = os.path.join(work, "mnt")


class FakeProc(object):
    """A worker: prints `output`, then exits with `rc` (None: keeps running until killed)."""

    def __init__(self, output, rc):
        self.stdout = io.BytesIO(output.encode())
        self.returncode = rc
        self.killed = False
        self.pid = 4242

    def poll(self):
        return self.returncode

    def send_signal(self, sig):
        self.killed = True
        self.returncode = -9

    def wait(self, timeout=None):
        return self.returncode


class MountTests(unittest.TestCase):
    def setUp(self):
        self._tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self._tmp.cleanup)
        self.work = os.path.realpath(self._tmp.name)
        self.ctx = FakeCtx(self.work)
        self.vol = FakeVol(self.work)
        self.table = []  # lines of the fake /sbin/mount
        self.procs = []  # worker scripts, one per launch: (output, rc, mounts)
        self.launches = []
        saved = (mmaplib.mount_table, mmaplib.subprocess.Popen, mmaplib.guest_tool, mmaplib.time.sleep,
                 dict(mmaplib._SEEN_DEVICES), mmaplib.MOUNT_WAIT)
        self.addCleanup(self._restore, saved)
        mmaplib.mount_table = lambda: list(self.table)
        mmaplib.subprocess.Popen = self._popen
        mmaplib.guest_tool = lambda name: "/fake/" + name
        real_sleep = mmaplib.time.sleep
        mmaplib.time.sleep = lambda s: real_sleep(min(s, 0.01))
        mmaplib._SEEN_DEVICES.clear()
        mmaplib.MOUNT_WAIT = 1

    def _restore(self, saved):
        (mmaplib.mount_table, mmaplib.subprocess.Popen, mmaplib.guest_tool, mmaplib.time.sleep, seen,
         mmaplib.MOUNT_WAIT) = saved
        mmaplib._SEEN_DEVICES.clear()
        mmaplib._SEEN_DEVICES.update(seen)

    def _popen(self, argv, **kw):
        output, rc, mounts = self.procs[len(self.launches)]
        p = FakeProc(output, rc)
        self.launches.append(p)
        if mounts:
            self.table.append("/dev/disk9s1 on %s (macfuse, local, fskit)" % os.path.realpath(self.vol.mnt))
        return p

    def worker(self):
        w = mmaplib.Worker(self.ctx, self.vol, "t")
        self.addCleanup(lambda: w._watchdog and w._watchdog.cancel())
        return w

    def fast_wait(self, w):
        orig = w.wait_clear
        w.wait_clear = lambda deadline=0.3: orig(deadline=deadline, poll=0.01, settle=0.0)

    # ---- the wait ----
    def test_wait_succeeds_when_the_entry_goes_away(self):
        w = self.worker()
        stale = "/dev/disk6s1 on %s (macfuse, local, fskit)" % w.mnt
        mmaplib._SEEN_DEVICES[w.mnt] = {"/dev/disk6s1"}
        seq = [[stale], [stale], []]
        mmaplib.mount_table = lambda: list(seq.pop(0)) if len(seq) > 1 else []
        w.wait_clear(deadline=2, poll=0.01, settle=0.0)
        self.assertIsNotNone(w.wait_s)
        self.assertEqual(seq, [[]])  # the table was read three times: the wait polled
        self.assertTrue(any("mount table clear" in m and "after" in m for m in self.ctx.logs))

    def test_wait_also_covers_the_previous_device_on_another_path(self):
        w = self.worker()
        mmaplib._SEEN_DEVICES[w.mnt] = {"/dev/disk6s1"}
        self.table = ["/dev/disk6s1 on /private/var/elsewhere (macfuse, local)"]
        with self.assertRaisesRegex(mmaplib.WaitTimeout, "still lists"):
            w.wait_clear(deadline=0.05, poll=0.01, settle=0.0)

    def test_wait_times_out_with_a_clear_error(self):
        w = self.worker()
        self.table = ["/dev/disk6s1 on %s (macfuse, local, fskit)" % w.mnt]
        with self.assertRaises(mmaplib.WaitTimeout) as cm:
            w.wait_clear(deadline=0.05, poll=0.01, settle=0.0)
        self.assertIn("mount table still lists %s" % w.mnt, str(cm.exception))
        self.assertIn("after 0 s", str(cm.exception))

    def test_start_waits_before_spawning(self):
        w = self.worker()
        self.fast_wait(w)
        self.table = ["/dev/disk6s1 on %s (macfuse, local, fskit)" % w.mnt]
        self.procs = [("", 0, False)]
        with self.assertRaises(RuntimeError) as cm:
            w.start()
        self.assertIn("still lists", str(cm.exception))
        self.assertEqual(self.launches, [])  # nothing was launched on a busy mount point

    # ---- the retry ----
    def test_refusal_then_success_is_recorded_in_the_result(self):
        w = self.worker()
        self.fast_wait(w)
        self.procs = [("noise\n" + REFUSAL + "\n", None, False), ("ok\n", None, True)]
        w.start()
        self.assertEqual(len(self.launches), 2)
        self.assertTrue(self.launches[0].killed)
        self.assertEqual(len(w.refusals), 1)
        self.assertTrue(w.refusals[0]["worker_alive_when_killed"])
        with open(self.ctx.evidence_file("worker-refused-t.log")) as f:
            self.assertIn(REFUSAL, f.read())
        self.assertEqual(len(self.ctx.result_notes), 1)
        note = self.ctx.result_notes[0]
        self.assertIn(REFUSAL, note)
        self.assertIn("still running when killed", note)

    def test_successful_mount_records_its_device(self):
        w = self.worker()
        self.fast_wait(w)
        self.procs = [("ok\n", None, True)]
        w.start()
        self.assertEqual(mmaplib._SEEN_DEVICES[w.mnt], {"/dev/disk9s1"})

    def test_the_wait_runs_again_before_the_retry(self):
        w = self.worker()
        calls = []
        w.wait_clear = lambda: (calls.append(len(self.launches)), setattr(w, "wait_s", 0.0))
        self.procs = [(REFUSAL + "\n", None, False), ("ok\n", None, True)]
        w.start()
        self.assertEqual(calls, [0, 1])  # once before each launch

    def test_the_note_is_joined_for_fail_and_error(self):
        results = tempfile.mkdtemp(dir=self.work)

        def failing(ctx):
            ctx.add_result_note("retried")
            raise core.Fail("data differs")

        def erroring(ctx):
            ctx.add_result_note("retried")
            raise RuntimeError("boom")

        cases = [core.Case("I-X-02", ("B",), failing, "t", "x"), core.Case("I-X-03", ("B",), erroring, "t", "x")]
        recs = runner.run_cases(cases, "B", "run", results, None, tmp_root=self.work, quiet=True)
        self.assertEqual([(r["status"], r["note"]) for r in recs],
                         [("fail", "data differs; retried"), ("error", "RuntimeError: boom; retried")])

    # ---- verify_image ----
    def verify(self, start_error):
        class Sh(object):
            rc, out, err = 0, b"", b""

        self.ctx.sh = lambda cmd, check=True, timeout=None: Sh()

        def start(self_w, deadline=1500):
            raise start_error

        orig = mmaplib.Worker.start
        mmaplib.Worker.start = start
        self.addCleanup(setattr, mmaplib.Worker, "start", orig)
        return mmaplib.verify_image(self.ctx, self.vol, {}, "x")

    def test_verify_image_keeps_ordinary_failure_as_observation(self):
        out = self.verify(mmaplib.MountFailed("no mount on /x (worker rc=1); tail:\ndirty"))
        self.assertTrue(out["mount"].startswith("failed: no mount on"))

    def test_verify_image_raises_a_refusal(self):
        with self.assertRaises(mmaplib.MountFailed):
            self.verify(mmaplib.MountFailed("second failure", refused=True, first_line=REFUSAL))

    def test_verify_image_raises_a_wait_timeout(self):
        with self.assertRaises(mmaplib.WaitTimeout):
            self.verify(mmaplib.WaitTimeout("mount table still lists /x after 30 s"))

    def test_the_note_reaches_the_case_record(self):
        results = tempfile.mkdtemp(dir=self.work)

        def func(ctx):
            ctx.add_result_note("mount retried once after FSKit refusal")

        c = core.Case("I-X-01", ("B",), func, "t", "x")
        recs = runner.run_cases([c], "B", "run", results, None, tmp_root=self.work, quiet=True)
        self.assertEqual((recs[0]["status"], recs[0]["note"]), ("pass", "mount retried once after FSKit refusal"))

    def test_worker_that_exited_is_recorded_as_exited(self):
        w = self.worker()
        self.fast_wait(w)
        self.procs = [(REFUSAL + "\n", 1, False), ("ok\n", None, True)]
        w.start()
        self.assertFalse(w.refusals[0]["worker_alive_when_killed"])
        self.assertIn("had already exited", self.ctx.result_notes[0])

    def test_second_refusal_fails(self):
        w = self.worker()
        self.fast_wait(w)
        self.procs = [(REFUSAL + "\n", None, False), (REFUSAL + "\n", None, False)]
        with self.assertRaises(mmaplib.MountFailed) as cm:
            w.start()
        self.assertEqual(len(self.launches), 2)
        self.assertIn("second failure", str(cm.exception))
        self.assertEqual(self.ctx.result_notes, [])

    def test_other_failure_is_not_retried(self):
        w = self.worker()
        self.fast_wait(w)
        self.procs = [("ntfs-3g: volume is dirty\n", 1, False), ("ok\n", None, True)]
        with self.assertRaises(mmaplib.MountFailed) as cm:
            w.start()
        self.assertEqual(len(self.launches), 1)
        self.assertFalse(cm.exception.refused)
        self.assertIn("no mount on", str(cm.exception))
        self.assertEqual(w.refusals, [])

    def test_worker_surviving_sigkill_stops_the_retry(self):
        w = self.worker()
        self.fast_wait(w)
        self.procs = [(REFUSAL + "\n", None, False), ("ok\n", None, True)]
        orig = FakeProc.send_signal
        FakeProc.send_signal = lambda self, sig: setattr(self, "killed", True)  # SIGKILL has no effect
        self.addCleanup(setattr, FakeProc, "send_signal", orig)
        with self.assertRaisesRegex(RuntimeError, "still running after SIGKILL"):
            w.start()
        self.assertEqual(len(self.launches), 1)


if __name__ == "__main__":
    unittest.main()
