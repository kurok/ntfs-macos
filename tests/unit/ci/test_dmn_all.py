#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""tests/integration/daemon/dmn_all.sh with stub drivers and a stub vm/macos/tart.sh (CI speedup step 5): no VM.

A "guest" is a directory of the stub tart.sh (clone makes it, delete removes it), so the number of directories is the
number of guests. The stub drivers clone a guest of their own unless they get --guest, record when they ran and in
which guest, and write PASS lines. Checked: the grouping (the module-level drivers share one guest, the others keep
their own), the reset check between the drivers of the shared guest, the two guests only with a free second slot,
the old order with --share-guests 0 / NTFSMAC_DMN_SHARE=0, the summary format, the --deps-tar writer, and that every
guest and slot is gone at the end. The --guest option of the real drivers is checked with a stub `tart` binary.
Usage: tests/unit/ci/test_dmn_all.py   (exit 0 = all pass)
"""
import os
import platform
import re
import shutil
import signal
import stat
import subprocess
import sys
import tempfile
import time
import unittest

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
DAEMON = os.path.join(REPO, "tests", "integration", "daemon")

ALL = ["spawn", "spawn-sbx", "sandbox", "errwatch", "journal", "xpc", "mirror", "attach", "dmn"]
SHARED = ["spawn", "spawn-sbx", "errwatch", "journal", "mirror"]
OWN = ["sandbox", "xpc", "attach", "dmn"]
# the summary line of the old dmn_all.sh: printf '%-10s exit %s  PASS %s  FAIL %s'
LINE_RE = re.compile(r"^(\S+) +exit (\d+)  PASS (\d+)  FAIL (\d+)$")

TART_STUB = r'''#!/bin/sh
# stub of vm/macos/tart.sh: a guest is the directory $STUB_DIR/guests/NAME
g=$STUB_DIR/guests
mkdir -p "$g"
sub=$1; shift
case $sub in
clone)
	[ -z "${STUB_CLONE_FAIL:-}" ] || case $1 in *share*) exit 1 ;; esac
	mkdir "$g/$1" || exit 1
	ls "$g" | wc -l | tr -d ' ' >>"$STUB_DIR/counts"
	echo "clone $1" >>"$STUB_DIR/tart.log" ;;
run) [ -d "$g/$1" ] || exit 1; echo "run $1" >>"$STUB_DIR/tart.log" ;;
delete) rm -rf "$g/$1"; echo "delete $1" >>"$STUB_DIR/tart.log" ;;
exec)
	n=$1; shift; [ "$1" != -- ] || shift
	[ -d "$g/$n" ] || exit 1
	case $* in
	*"-- volumes"*) echo "-- volumes"; echo "Macintosh HD"; cat "$g/$n/dirt" 2>/dev/null; echo "-- macfuse"; cat "$g/$n/mf" 2>/dev/null; true ;;
	*) echo "exec $n" >>"$STUB_DIR/tart.log" ;;
	esac ;;
*) exit 2 ;;
esac
'''

DRIVER_STUB = r'''#!/bin/sh
# stub driver: NAME from the script name (and --sandbox); a guest of its own unless --guest
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
tart_sh=$here/../../../vm/macos/tart.sh
base=$(basename "$0" .sh)
out= guest= deps= sbx=0
while [ $# -gt 0 ]; do
	case $1 in
	--out) out=$2; shift 2 ;;
	--guest) guest=$2; shift 2 ;;
	--deps-tar) deps=$2; shift 2 ;;
	--fixtures) shift 2 ;;
	--sandbox) sbx=1; shift ;;
	*) shift ;;
	esac
done
case $base in
spawn_vm) n=spawn; [ "$sbx" -eq 0 ] || n=spawn-sbx ;;
dmn_mirror_vm) n=mirror ;;
*) n=${base%_vm} ;;
esac
val() { printf '%s\n' "$1" | tr ',' '\n' | sed -n "s/^$2=//p" | tail -n 1; }
now() { perl -MTime::HiRes=time -e 'printf "%.3f\n", time'; }
own=
if [ -z "$guest" ]; then
	own=ntfsmac-$n-$$
	"$tart_sh" clone "$own" || exit 2
	trap '"$tart_sh" delete "$own"' EXIT
fi
trap 'exit 143' TERM
echo "$n start $(now) ${guest:-own} ${deps:-nodeps}" >>"$STUB_DIR/events"
if [ -n "$deps" ] && [ ! -f "$deps" ] && [ -z "$(val "${STUB_NOSAVE:-}" "$n")" ]; then echo tar >"$deps"; fi
s=$(val "${STUB_SLEEP:-}" "$n")
[ -z "$s" ] || { sleep "$s" & wait $!; }
p=$(val "${STUB_PASS:-}" "$n"); p=${p:-3}
i=0; : >"$out"
while [ "$i" -lt "$p" ]; do echo "PASS: $n $i" >>"$out"; i=$((i + 1)); done
[ -z "$(val "${STUB_FAILS:-}" "$n")" ] || echo "FAIL: $n broke" >>"$out"
[ -z "$(val "${STUB_DIRTY:-}" "$n")" ] || [ -z "$guest" ] || echo "/Volumes/left-by-$n" >"$STUB_DIR/guests/$guest/dirt"
# macFUSE's two services as the guest's ps would list them (pid path state): started, restarted, doubled, wedged
fsm=/Library/Filesystems/macfuse.fs/Contents/Resources/macfuse.app/Contents/Extensions/io.macfuse.app.fsmodule.macfuse-local.appex/Contents/MacOS/io.macfuse.app.fsmodule.macfuse-local
lsd=/Library/PrivilegedHelperTools/io.macfuse.app.launchservice.daemon
if [ -n "$guest" ]; then
	case $(val "${STUB_MF:-}" "$n") in
	start) printf '%s\n' "101 $lsd ok" "100 $fsm ok" >"$STUB_DIR/guests/$guest/mf" ;;
	restart) printf '%s\n' "101 $lsd ok" "200 $fsm ok" >"$STUB_DIR/guests/$guest/mf" ;;
	double) printf '%s\n' "101 $lsd ok" "100 $fsm ok" "300 $fsm ok" >"$STUB_DIR/guests/$guest/mf" ;;
	wedged) printf '%s\n' "101 $lsd ok" "100 $fsm wedged:T" >"$STUB_DIR/guests/$guest/mf" ;;
	esac
fi
echo "$n end $(now)" >>"$STUB_DIR/events"
rc=$(val "${STUB_RC:-}" "$n")
exit "${rc:-0}"
'''


def put(path, data, mode=0o755):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w") as f:
        f.write(data)
    os.chmod(path, mode)


class DmnAllTest(unittest.TestCase):
    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        d = self.tmp.name
        self.fake = os.path.join(d, "repo")
        dd = os.path.join(self.fake, "tests", "integration", "daemon")
        os.makedirs(dd)
        shutil.copy(os.path.join(DAEMON, "dmn_all.sh"), dd)
        for s in ("spawn_vm", "sandbox_vm", "errwatch_vm", "journal_vm", "xpc_vm", "dmn_mirror_vm", "attach_vm", "dmn_vm"):
            put(os.path.join(dd, s + ".sh"), DRIVER_STUB)
        put(os.path.join(self.fake, "vm", "macos", "tart.sh"), TART_STUB)
        self.sd = os.path.join(d, "stub")
        os.makedirs(self.sd)
        self.out = os.path.join(d, "out")
        self.slots = os.path.join(d, "slots")
        self.fx = os.path.join(d, "fx")
        os.makedirs(self.fx)
        self.tmpdir = os.path.join(d, "t")
        os.makedirs(self.tmpdir)

    def run_all(self, *args, slots=True, **env):
        argv = ["sh", os.path.join(self.fake, "tests", "integration", "daemon", "dmn_all.sh"), "--fixtures", self.fx,
                "--out-dir", self.out]
        if slots:
            argv += ["--slot-dir", self.slots]
        e = dict(os.environ, STUB_DIR=self.sd, TMPDIR=self.tmpdir)
        for k in ("NTFSMAC_DMN_SHARE", "NTFSMAC_DMN_LANES"):
            e.pop(k, None)
        e.update(env)
        return subprocess.run(argv + list(args), env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)

    def events(self):
        """{driver: (start, end, guest, deps)}"""
        out = {}
        with open(os.path.join(self.sd, "events")) as f:
            for line in f:
                w = line.split()
                if w[1] == "start":
                    out[w[0]] = [float(w[2]), None, w[3], w[4]]
                else:
                    out[w[0]][1] = float(w[2])
        return out

    def overlap(self, a, b):
        t = self.events()
        return min(t[a][1], t[b][1]) - max(t[a][0], t[b][0]) > 0

    def max_guests(self):
        with open(os.path.join(self.sd, "counts")) as f:
            return max(int(x) for x in f.read().split())

    def summary(self):
        with open(os.path.join(self.out, "summary.txt")) as f:
            return f.read().splitlines()

    def assert_clean_end(self):
        self.assertEqual(os.listdir(os.path.join(self.sd, "guests")), [], "a guest was left")
        if os.path.isdir(self.slots):
            self.assertEqual(os.listdir(self.slots), [], "a slot was left")
        self.assertEqual(os.listdir(self.tmpdir), [], "the run state was left")

    def assert_summary(self, order, passes=None):
        lines = self.summary()
        self.assertEqual([LINE_RE.match(x).group(1) for x in lines], order)
        for x in lines:
            m = LINE_RE.match(x)
            self.assertIsNotNone(m, x)
            # the exact old format, padding included
            self.assertEqual(x, "%-10s exit %s  PASS %s  FAIL %s" % m.groups())
            if passes is not None:
                self.assertEqual(int(m.group(3)), passes.get(m.group(1), 3))

    SLEEP = "dmn=1.5,spawn=0.2,spawn-sbx=0.2,errwatch=0.2,journal=0.2,mirror=0.2,sandbox=0.3,attach=0.3,xpc=0.1"

    def test_grouping_shared_guest_and_two_guests(self):
        r = self.run_all(STUB_SLEEP=self.SLEEP, STUB_PASS="dmn=549,journal=121")
        self.assertEqual(r.returncode, 0, r.stdout.decode() + r.stderr.decode())
        ev = self.events()
        self.assertEqual(sorted(ev), sorted(ALL))
        shared = {ev[n][2] for n in SHARED}
        self.assertEqual(len(shared), 1, ev)  # one guest for the five module-level drivers
        self.assertTrue(shared.pop().startswith("ntfsmac-share1-"))
        for n in OWN:
            self.assertEqual(ev[n][2], "own", n)  # a fresh guest each
        self.assertEqual(self.max_guests(), 2)  # two at once, never three
        self.assertTrue(self.overlap("dmn", "spawn"))  # dmn and the shared group side by side
        for a in SHARED:  # inside the shared guest one after the other
            for b in SHARED:
                if a != b:
                    self.assertFalse(self.overlap(a, b), (a, b))
        self.assert_summary(ALL, {"dmn": 549, "journal": 121})
        out = r.stdout.decode()
        for line in self.summary():
            self.assertIn(line + "\n", out)
        self.assertIn("dmn_all: wall time", out)
        self.assertTrue(out.rstrip("\n").endswith("\n".join(self.summary())))  # the ordered block ends the output
        self.assert_clean_end()

    def test_share_guests_0_is_the_old_order(self):
        for args, env in ((["--share-guests", "0"], {}), ([], {"NTFSMAC_DMN_SHARE": "0"})):
            with self.subTest(args=args, env=env):
                for p in ("events", "counts"):
                    if os.path.exists(os.path.join(self.sd, p)):
                        os.remove(os.path.join(self.sd, p))
                r = self.run_all(*args, STUB_SLEEP="dmn=0.2,spawn=0.1", **env)
                self.assertEqual(r.returncode, 0, r.stderr.decode())
                ev = self.events()
                self.assertEqual(sorted(ev, key=lambda n: ev[n][0]), ALL)  # in DRIVER order
                for a, b in zip(ALL, ALL[1:]):
                    self.assertLessEqual(ev[a][1], ev[b][0])
                for n in ALL:
                    self.assertEqual(ev[n][2], "own", n)
                self.assertEqual(self.max_guests(), 1)
                self.assert_summary(ALL)
                self.assert_clean_end()

    def test_without_a_free_second_slot_one_guest_at_a_time(self):
        os.makedirs(os.path.join(self.slots, "slot-2"))
        holder = subprocess.Popen(["sleep", "60"])
        self.addCleanup(holder.wait)
        self.addCleanup(holder.kill)
        with open(os.path.join(self.slots, "slot-2", "pid"), "w") as f:
            f.write("%d\n" % holder.pid)
        r = self.run_all(STUB_SLEEP="dmn=0.3,spawn=0.1")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 1)
        ev = self.events()
        self.assertEqual(len({ev[n][2] for n in SHARED}), 1)  # still shared
        self.assertEqual(os.listdir(self.slots), ["slot-2"])  # the other holder's lock is untouched
        with open(os.path.join(self.slots, "slot-2", "pid")) as f:
            self.assertEqual(int(f.read()), holder.pid)
        self.assert_summary(ALL)

    def test_lanes_1_never_takes_a_second_slot(self):
        r = self.run_all("--lanes", "1", STUB_SLEEP="dmn=0.3")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 1)
        self.assert_clean_end()

    def test_caller_holds_a_slot_lane_slot_dir(self):
        # run-suite holds slot-1 (as its own pid): the second guest may only use slot-2, and only when it is free
        os.makedirs(os.path.join(self.slots, "slot-1"))
        holder = subprocess.Popen(["sleep", "60"])
        self.addCleanup(holder.wait)
        self.addCleanup(holder.kill)
        with open(os.path.join(self.slots, "slot-1", "pid"), "w") as f:
            f.write("%d\n" % holder.pid)
        r = self.run_all("--lane-slot-dir", self.slots, slots=False, STUB_SLEEP=self.SLEEP)
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 2)
        self.assertIn("holding slot-2 as well", r.stdout.decode())
        self.assertEqual(os.listdir(self.slots), ["slot-1"])  # slot-2 given back, slot-1 untouched
        # both held by the caller: one guest
        os.makedirs(os.path.join(self.slots, "slot-2"))
        with open(os.path.join(self.slots, "slot-2", "pid"), "w") as f:
            f.write("%d\n" % holder.pid)
        os.remove(os.path.join(self.sd, "counts"))
        r = self.run_all("--lane-slot-dir", self.slots, slots=False, STUB_SLEEP="dmn=0.3")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 1)
        self.assertEqual(sorted(os.listdir(self.slots)), ["slot-1", "slot-2"])
        # no slot directory at all: never a second guest
        os.remove(os.path.join(self.sd, "counts"))
        r = self.run_all(slots=False, STUB_SLEEP="dmn=0.3")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.max_guests(), 1)

    def test_unclean_state_retires_the_shared_guest(self):
        # errwatch leaves something behind: journal must not run in that guest
        r = self.run_all(STUB_DIRTY="errwatch=1", STUB_SLEEP="dmn=0.5")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        ev = self.events()
        self.assertEqual(ev["spawn"][2], ev["errwatch"][2])
        self.assertNotEqual(ev["errwatch"][2], ev["journal"][2])
        self.assertEqual(ev["journal"][2], ev["mirror"][2])
        self.assertIn("left-by-errwatch", r.stdout.decode())  # the difference is shown
        self.assert_clean_end()

    def test_macfuse_services_get_a_second_baseline_after_the_first_driver(self):
        # started by spawn and kept: the guest stays shared; restarted by journal: mirror gets a fresh guest
        r = self.run_all(*SHARED, STUB_MF="spawn=start,journal=restart")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        ev = self.events()
        self.assertEqual(len({ev[n][2] for n in ("spawn", "spawn-sbx", "errwatch", "journal")}), 1, ev)
        self.assertNotEqual(ev["journal"][2], ev["mirror"][2])
        out = r.stdout.decode()
        self.assertIn("second baseline", out)
        self.assertIn("macfuse < 100 ", out)  # the old pid and the new one are shown
        self.assertIn("macfuse > 200 ", out)
        self.assert_clean_end()

    def test_macfuse_services_twice_or_wedged_retire_the_guest(self):
        for kind in ("double", "wedged"):
            with self.subTest(kind=kind):
                if os.path.exists(os.path.join(self.sd, "events")):
                    os.remove(os.path.join(self.sd, "events"))
                # wrong already after the first driver (no baseline taken), and wrong after a later one
                r = self.run_all("spawn", "spawn-sbx", "errwatch", "journal", STUB_MF="spawn=%s,errwatch=start" % kind)
                self.assertEqual(r.returncode, 0, r.stderr.decode())
                ev = self.events()
                self.assertNotEqual(ev["spawn"][2], ev["spawn-sbx"][2])
                os.remove(os.path.join(self.sd, "events"))
                r = self.run_all("spawn", "spawn-sbx", "errwatch", STUB_MF="spawn=start,spawn-sbx=%s" % kind)
                self.assertEqual(r.returncode, 0, r.stderr.decode())
                ev = self.events()
                self.assertEqual(ev["spawn"][2], ev["spawn-sbx"][2])
                self.assertNotEqual(ev["spawn-sbx"][2], ev["errwatch"][2])
                self.assert_clean_end()

    def test_a_failing_driver_retires_the_shared_guest_and_fails_the_run(self):
        r = self.run_all("spawn", "spawn-sbx", "errwatch", STUB_RC="spawn-sbx=1", STUB_FAILS="spawn-sbx=1")
        self.assertEqual(r.returncode, 1)
        ev = self.events()
        self.assertEqual(ev["spawn"][2], ev["spawn-sbx"][2])
        self.assertNotEqual(ev["spawn-sbx"][2], ev["errwatch"][2])
        self.assertEqual(self.summary()[1], "spawn-sbx  exit 1  PASS 3  FAIL 1")
        self.assert_clean_end()

    def test_shared_guest_that_cannot_be_made_falls_back_to_a_guest_each(self):
        r = self.run_all("spawn", "journal", STUB_CLONE_FAIL="1")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        ev = self.events()
        self.assertEqual((ev["spawn"][2], ev["journal"][2]), ("own", "own"))
        self.assert_clean_end()

    def test_a_single_module_driver_gets_its_own_guest(self):
        r = self.run_all("journal", "xpc")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertEqual(self.events()["journal"][2], "own")
        self.assert_summary(["journal", "xpc"])

    def test_deps_tar_has_one_writer(self):
        tar = os.path.join(self.tmp.name, "deps.tar")
        # two guests at once: the first of the two makes the tar, the other one builds for itself and saves nothing
        r = self.run_all("attach", "dmn", "--deps-tar", tar, STUB_SLEEP="dmn=1,attach=1")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        self.assertTrue(self.overlap("attach", "dmn"))
        ev = self.events()
        self.assertEqual(sorted((ev["dmn"][3], ev["attach"][3])), sorted((tar, "nodeps")))
        # one guest at a time: the second one uses the saved tar
        os.remove(tar)
        r = self.run_all("attach", "dmn", "--deps-tar", tar, "--lanes", "1")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        ev = self.events()
        self.assertEqual((ev["dmn"][3], ev["attach"][3]), (tar, tar))
        # the old order, attach does not save it (a failed build): dmn builds and saves it, as before
        os.remove(tar)
        r = self.run_all("attach", "dmn", "--deps-tar", tar, "--share-guests", "0", STUB_NOSAVE="attach=1")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        ev = self.events()
        self.assertEqual((ev["attach"][3], ev["dmn"][3]), (tar, tar))
        self.assertTrue(os.path.exists(tar))
        self.assertNotIn("other guest", r.stdout.decode())
        # the old order: attach saves, dmn uses (as before)
        os.remove(tar)
        r = self.run_all("attach", "dmn", "--deps-tar", tar, "--share-guests", "0")
        self.assertEqual(r.returncode, 0, r.stderr.decode())
        ev = self.events()
        self.assertEqual((ev["dmn"][3], ev["attach"][3]), (tar, tar))

    def test_options_are_validated(self):
        for args in (["--share-guests", "2"], ["--lanes", "3"], ["nosuch"]):
            self.assertEqual(self.run_all(*args).returncode, 2, args)
        self.assertEqual(self.run_all(NTFSMAC_DMN_SHARE="x").returncode, 2)
        self.assertFalse(os.path.exists(os.path.join(self.sd, "events")))  # nothing ran

    def interrupt(self, extra, sleep, finished, main_only=False):
        argv = ["sh", os.path.join(self.fake, "tests", "integration", "daemon", "dmn_all.sh"), "--fixtures", self.fx,
                "--out-dir", self.out, "--slot-dir", self.slots] + extra
        e = dict(os.environ, STUB_DIR=self.sd, TMPDIR=self.tmpdir, STUB_SLEEP=sleep)
        p = subprocess.Popen(argv, env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        self.addCleanup(p.kill)
        deadline = time.time() + 30
        res = os.path.join(self.sd, "events")
        while time.time() < deadline:
            if os.path.exists(res):
                with open(res) as f:
                    ended = {ln.split()[0] for ln in f if ln.split()[1] == "end"}
                if set(finished) <= ended:
                    break
            time.sleep(0.1)
        time.sleep(0.5)
        if main_only:
            os.kill(p.pid, signal.SIGTERM)  # the main shell only: it must pass the signal on
        else:
            os.killpg(p.pid, signal.SIGTERM)
        t0 = time.time()
        p.communicate(timeout=60)
        self.assertLess(time.time() - t0, 20)  # not held until the 60 s drivers end
        self.assertNotEqual(p.returncode, 0)
        self.assert_clean_end()
        return self.summary()

    def test_an_interrupt_keeps_the_lines_of_the_finished_drivers(self):
        # old order: spawn and spawn-sbx ended, sandbox runs
        lines = self.interrupt(["--share-guests", "0"], "sandbox=60", ["spawn", "spawn-sbx"])
        self.assertEqual([LINE_RE.match(x).group(1) for x in lines], ["spawn", "spawn-sbx"])
        # shared and two guests: the shared group ended, dmn runs
        os.remove(os.path.join(self.sd, "events"))
        lines = self.interrupt([], "dmn=60,sandbox=60,attach=60,xpc=60", SHARED)
        self.assertEqual([LINE_RE.match(x).group(1) for x in lines], [d for d in ALL if d in SHARED])

    def test_term_to_the_main_shell_only_stops_the_drivers_at_once(self):
        lines = self.interrupt([], "dmn=60,spawn-sbx=60", ["spawn"], main_only=True)
        self.assertEqual([LINE_RE.match(x).group(1) for x in lines], ["spawn"])

    def test_a_signal_deletes_every_guest_and_releases_the_slots(self):
        argv = ["sh", os.path.join(self.fake, "tests", "integration", "daemon", "dmn_all.sh"), "--fixtures", self.fx,
                "--out-dir", self.out, "--slot-dir", self.slots]
        e = dict(os.environ, STUB_DIR=self.sd, TMPDIR=self.tmpdir, STUB_SLEEP="dmn=60,spawn=60")
        p = subprocess.Popen(argv, env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, start_new_session=True)
        self.addCleanup(p.kill)
        guests = os.path.join(self.sd, "guests")
        deadline = time.time() + 30
        while time.time() < deadline:
            if os.path.isdir(guests) and len(os.listdir(guests)) == 2:
                break
            time.sleep(0.1)
        self.assertEqual(len(os.listdir(guests)), 2, "both guests should be up")
        os.killpg(p.pid, signal.SIGTERM)  # as run-suite's stop_stages: the whole tree
        p.communicate(timeout=60)
        self.assertNotEqual(p.returncode, 0)
        self.assert_clean_end()


TART_BIN_STUB = r'''#!/bin/sh
# stub of the tart binary for vm/macos/tart.sh: records every call; `exec NAME /bin/sh -c CMD` answers the stamp test
echo "tart $*" >>"$STUB_DIR/tart.calls"
[ "$1" = exec ] || { [ "$1" = list ] && echo ntfsmac-golden; exit 0; }
shift; [ "$1" != -i ] || shift
cmd=$4 # NAME /bin/sh -c CMD
case $cmd in
*"cat ~/ntfsmac/deps-built"*) [ -f "$STUB_DIR/built" ]; exit $? ;;
*deps-fio*) echo build-fio >>"$STUB_DIR/tart.calls" ;;
*"~/ntfsmac/deps-built"*) echo stamp >>"$STUB_DIR/tart.calls"; : >"$STUB_DIR/built" ;;
*build-ntfs3g.sh*) echo build >>"$STUB_DIR/tart.calls" ;;
*) cat >/dev/null 2>&1 || true ;;
esac
exit 0
'''


@unittest.skipUnless(platform.system() == "Darwin" and platform.machine() == "arm64", "vm/macos/tart.sh needs macOS arm64")
class DriverGuestOptionTest(unittest.TestCase):
    """the --guest option of the four module-level drivers, run for real against a stub `tart` binary"""

    def setUp(self):
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.sd = os.path.join(self.tmp.name, "stub")
        os.makedirs(self.sd)
        self.bindir = os.path.join(self.tmp.name, "bin")
        put(os.path.join(self.bindir, "tart"), TART_BIN_STUB)
        lock = os.path.join(REPO, "third_party", "deps.lock")
        with open(lock) as f:
            tb = [ln.split()[2].rsplit("/", 1)[1] for ln in f if ln.split()[:1] == ["ntfs-3g"]][0]
        if not os.path.exists(os.path.join(REPO, "third_party", "cache", tb)):
            self.skipTest("third_party/cache/%s is missing (the drivers refuse to start without it)" % tb)

    def run_driver(self, script, *args):
        e = dict(os.environ, STUB_DIR=self.sd, PATH=self.bindir + os.pathsep + os.environ["PATH"],
                 NTFSMAC_STATE=os.path.join(self.tmp.name, "state"))
        out = os.path.join(self.tmp.name, "out.txt")
        r = subprocess.run(["sh", os.path.join(DAEMON, script), "--out", out, "--guest", "ntfsmac-share1-1"] + list(args),
                           env=e, stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=120)
        with open(os.path.join(self.sd, "tart.calls")) as f:
            calls = f.read()
        os.remove(os.path.join(self.sd, "tart.calls"))
        return r, calls

    def test_guest_is_neither_cloned_nor_deleted_and_builds_once(self):
        extra = {"dmn_mirror_vm.sh": ["--images", self.images()]}
        for script in ("spawn_vm.sh", "errwatch_vm.sh", "journal_vm.sh", "dmn_mirror_vm.sh"):
            with self.subTest(script=script):
                if os.path.exists(os.path.join(self.sd, "built")):
                    os.remove(os.path.join(self.sd, "built"))
                r, calls = self.run_driver(script, *extra.get(script, []))
                self.assertEqual(r.returncode, 0, r.stderr.decode())
                self.assertNotIn("tart clone", calls)
                self.assertNotIn("tart delete", calls)
                self.assertNotIn("tart run", calls)
                self.assertIn("build\n", calls)  # the first driver in the guest builds NTFS-3G
                self.assertIn("stamp\n", calls)  # and says so
                r, calls = self.run_driver(script, *extra.get(script, []))
                self.assertEqual(r.returncode, 0, r.stderr.decode())
                self.assertNotIn("build\n", calls)  # the next one uses it
                self.assertIn("already built in the shared guest ntfsmac-share1-1", r.stderr.decode())

    def images(self):
        d = os.path.join(self.tmp.name, "img")
        os.makedirs(d, exist_ok=True)
        for f in ("split", "splitdirty", "newer", "splitlog"):
            open(os.path.join(d, f + ".img"), "w").close()
        return d


if __name__ == "__main__":
    unittest.main(verbosity=2)
