#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - repeated forced terminations (T-M5-05, criterion 4.4, S-KILL-*). Environment B.
#
# Usage: tests/stress/kill/run.sh --out DIR [--reps N] [--targets "worker daemon module detach"] [--step SECONDS]
#                                 [--fixtures DIR] [--deps-tar FILE] [--build-dir DIR] [--no-control]
#                                 [--deadline SECONDS]
#   --out DIR       result directory: result.json (ntfsmac-test-result/1: S-KILL-01..05 and S-KILL-RUN), summary.txt,
#                   kill.log, kill-TARGET.log, kill-module-measure.json, driver.log
#   --reps N        repetitions per target (default 2, a smoke run; the full run is 20; also read from KILL_REPS)
#   --targets LIST  worker (S-KILL-01), daemon (02), module = the macFUSE module process (03), detach = force-detach
#                   of the image (04); default all four
#   --step SECONDS  deadline of one waiting step in the guest, wall clock (default 90, KILL_STEP); expiry is a FAIL
#   --fixtures DIR  directory with clean-64m-4k.img (default: generated in Environment A with
#                   tests/fixtures/gen/clean.sh --only clean-64m-4k --export, needs Docker)
#   --deps-tar FILE tar of the guest's ~/ntfsmac/deps, used instead of building NTFS-3G when the file exists
#   --build-dir DIR CMake build tree with the daemon and the CLI (default build/release)
#   --no-control    skip S-KILL-05 (the controls)
#   --deadline SECONDS  the host deadline of the whole guest run (default: derived, see below); a test knob
# One guest, named ntfsmac-kill-PID, is cloned from ntfsmac-golden, runs every repetition of every target
# (tests/stress/kill/kill_guest.sh, started through tests/integration/daemon/dmn_vm.sh --name NAME stress-kill)
# and is deleted: by the driver on every exit path, and by this script as a last resort by its known name.
# This script takes no VM slot: the caller holds it (scripts/test-all.sh and vm/macos/tart.sh callers do).
# Nothing is mounted on the host. There is no --keep: a kept guest would outlive the lock of its slot.
# Host deadline (backstop for a step that ignores its own deadline): 600 s for the clone and the copy, 900 s more
# when NTFS-3G has to be built (no existing --deps-tar), 120 s + 2 x step per repetition (measured: about 20 s),
# 600 s for the controls. At the deadline the driver gets SIGTERM (it deletes its guest), then SIGKILL after
# 180 s; the cases that did not finish are FAIL and S-KILL-RUN says why.
# Time: 4 targets x 2 repetitions plus the controls about 6 minutes of guest time, the full 4 x 20 about 30 minutes.
# Exit: 0 every case passed, 1 a case failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
py=${PYTHON:-python3}
out=
reps=${KILL_REPS:-2}
targets=${KILL_TARGETS:-"worker daemon module detach"}
step=${KILL_STEP:-90}
fx=
deps_tar=
build=$root/build/release
control=1
deadline=
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "kill: --out needs a directory" >&2; exit 2; }; out=$2; shift 2 ;;
	--reps) [ $# -ge 2 ] || { echo "kill: --reps needs a number" >&2; exit 2; }; reps=$2; shift 2 ;;
	--targets) [ $# -ge 2 ] || { echo "kill: --targets needs a list" >&2; exit 2; }; targets=$2; shift 2 ;;
	--step) [ $# -ge 2 ] || { echo "kill: --step needs a number" >&2; exit 2; }; step=$2; shift 2 ;;
	--fixtures) [ $# -ge 2 ] || { echo "kill: --fixtures needs a directory" >&2; exit 2; }; fx=$2; shift 2 ;;
	--deps-tar) [ $# -ge 2 ] || { echo "kill: --deps-tar needs a file" >&2; exit 2; }; deps_tar=$2; shift 2 ;;
	--build-dir) [ $# -ge 2 ] || { echo "kill: --build-dir needs a directory" >&2; exit 2; }; build=$2; shift 2 ;;
	--deadline) [ $# -ge 2 ] || { echo "kill: --deadline needs a number" >&2; exit 2; }; deadline=$2; shift 2 ;;
	--no-control) control=0; shift ;;
	--keep) echo "kill: --keep is not supported (a kept guest would outlive the lock of its slot)" >&2; exit 2 ;;
	-h | --help) sed -n '4,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "kill: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$out" ] || { echo "kill: --out is required" >&2; exit 2; }
case $reps in '' | *[!0-9]* | 0) echo "kill: --reps must be a positive number" >&2; exit 2 ;; esac
case $step in '' | *[!0-9]* | 0) echo "kill: --step must be a positive number" >&2; exit 2 ;; esac
case $deadline in *[!0-9]*) echo "kill: --deadline must be a positive number" >&2; exit 2 ;; esac
for t in $targets; do
	case $t in worker | daemon | module | detach) ;; *) echo "kill: unknown target $t (worker daemon module detach)" >&2; exit 2 ;; esac
done
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
case $fx in '' | /*) ;; *) fx=$(pwd)/$fx ;; esac
case $deps_tar in '' | /*) ;; *) deps_tar=$(pwd)/$deps_tar ;; esac
case $build in /*) ;; *) build=$(pwd)/$build ;; esac
mkdir -p "$out"

"$py" -B "$here/result.py" selftest >&2 || { echo "kill: result.py selftest failed" >&2; exit 2; }
for f in "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac"; do
	[ -f "$f" ] || { echo "kill: $f is missing; build first (cmake --build --preset release)" >&2; exit 2; }
done

name=ntfsmac-kill-$$
tmp=$(mktemp -d "${TMPDIR:-/tmp}/kill.XXXXXX")
cleanup() {
	# last resort: the driver deletes its guest itself; a guest that is still there (an interrupt during the
	# clone, a driver that was killed) is deleted here by the name this script chose
	"$tart_sh" delete "$name" >/dev/null 2>&1 || true
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
if [ -z "$fx" ]; then
	fx=$tmp/fixtures
	echo "kill: generating clean-64m-4k in Environment A" >&2
	sh "$root/tests/fixtures/gen/clean.sh" --only clean-64m-4k --export "$fx" >&2 || { echo "kill: fixture generation failed" >&2; exit 2; }
fi
[ -f "$fx/clean-64m-4k.img" ] || { echo "kill: $fx/clean-64m-4k.img is missing" >&2; exit 2; }

started=$("$py" -B -c 'import time; print(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))')
ntargets=$(echo $targets | wc -w | tr -d ' ')
if [ -z "$deadline" ]; then
	build_s=900
	[ -n "$deps_tar" ] && [ -f "$deps_tar" ] && build_s=0
	deadline=$((600 + build_s + reps * ntargets * (120 + 2 * step) + control * 600))
fi
echo "kill: $reps repetition(s) of: $targets; guest $name; step deadline ${step} s; host deadline ${deadline} s" >&2
rm -f "$out/kill.log"
rc=0
# the host deadline: perl is a supervisor. The driver runs in a process group of its own; on the deadline or on
# INT/TERM/HUP the group gets SIGTERM (the driver's trap deletes the guest), SIGKILL after 180 s more.
# Exit status: the driver's, 124 on the deadline, 130 on a signal.
KILL_REPS=$reps KILL_TARGETS=$targets KILL_STEP=$step KILL_CONTROL=$control \
	perl -e '
my ($t, @cmd) = @ARGV;
my $pid = fork();
defined $pid or exit 127;
if (!$pid) { setpgrp(0, 0); exec @cmd; exit 127 }
my $why = 0;
$SIG{ALRM} = sub { if ($why) { kill "KILL", -$pid } else { $why = 124; kill "TERM", -$pid; alarm 180 } };
for my $s (qw(INT TERM HUP)) { $SIG{$s} = sub { $why ||= 130; kill "TERM", -$pid; alarm 180 } }
alarm $t;
my $r;
do { $r = waitpid($pid, 0) } while ($r == -1 && $!{EINTR});
exit($why) if $why;
exit($? >> 8) if ($? & 127) == 0;
exit(128 + ($? & 127));
' "$deadline" \
	sh "$root/tests/integration/daemon/dmn_vm.sh" --name "$name" --build-dir "$build" --fixtures "$fx" ${deps_tar:+--deps-tar "$deps_tar"} \
	--out "$out/kill.log" stress-kill >"$out/driver.log" 2>&1 &
sup=$!
# a signal to this script goes on to the supervisor (which stops the driver and so deletes the guest); this
# script waits for it to end, then the EXIT trap deletes the guest by name if it is still there
interrupted=0
trap 'interrupted=1; kill -TERM "$sup" 2>/dev/null' INT TERM HUP
wait "$sup" || rc=$?
while kill -0 "$sup" 2>/dev/null; do wait "$sup" || rc=$?; done
trap 'exit 130' INT TERM HUP
[ "$interrupted" -eq 0 ] || exit 130
tail -n 3 "$out/driver.log" >&2 || true
if [ "$rc" -eq 124 ]; then
	echo "kill: the host deadline of ${deadline} s expired: a step hung; the unfinished cases are FAIL" >&2
fi
[ -f "$out/kill.log" ] || : >"$out/kill.log"
noctl=
[ "$control" -eq 1 ] || noctl=--no-control
# shellcheck disable=SC2086
"$py" -B "$here/result.py" --log "$out/kill.log" --out "$out" --repo "$root" --reps "$reps" --targets "$targets" \
	--started "$started" --driver-rc "$rc" $noctl
