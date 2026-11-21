#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - disconnect simulation S-DISC-* (T-M5-04, docs/TEST_STRATEGY.md sections 4 and 6). Environment B.
#
# Usage: tests/stress/disconnect/run.sh --out DIR [--reps N | --full] [--fixtures DIR] [--build-dir DIR]
#                                       [--deps-tar FILE] [--workloads LIST] [--controls LIST] [--keep]
#   --out DIR         result directory: result.json (ntfsmac-test-result/1: S-DISC-01..05), reps.tsv (one line
#                     per yank), rep-CASE-N.log, guest.log, meta.txt
#   --reps N          yanks per workload (default 3: 12 yanks and 2 controls; a yank cycle took 24 s in the guest, so
#                     about 6 min of guest time plus about 2 min setup with --deps-tar, 25-35 min in all without it
#                     because NTFS-3G is then built in the guest)
#   --full            --reps 20 (80 yanks and 2 controls, about 35 min of guest time plus the setup)
#   --fixtures DIR    directory with clean-64m-4k.img (default: generated in Environment A with
#                     tests/fixtures/gen/clean.sh --only clean-64m-4k --export, needs Docker)
#   --build-dir DIR   CMake tree with src/daemon/ntfsmacd and src/cli/ntfsmac (default build/release)
#   --deps-tar FILE   tar of the guest's ~/ntfsmac/deps (as dmn_vm.sh), used instead of building NTFS-3G when it
#                     exists; when it does not, the build is made and saved there
#   --workloads LIST  subset of "seq small churn fsync" (default all four)
#   --controls LIST   negative controls: "hash dirty" (default both), or "" for none
#   --fault KIND      "hash" or "dirty": inject a fault into every real yank (the suite must then FAIL); a test seam
#   --keep            keep the guest (delete it yourself with vm/macos/tart.sh delete NAME)
#
# Workloads (S-DISC-01..04): a large sequential write, many small file creates, rename and metadata churn,
# and a loop of write + fsync(file) + fsync(directory); the image is yanked at varied points of each. The
# yank is the one of I-DMN-16 (tests/integration/daemon/dmn_kill_guest.sh): the process that serves the virtual
# disk is killed, because `hdiutil detach -force` answers EBUSY while the worker holds the device. After each
# yank (disc_guest.sh): the volume leaves Mounted with no worker and no macfuse mount left, the journal entry
# stays, `ntfsfix -n` exits 0 and the image carries VOLUME_IS_DIRTY, the next attach is read-only with the
# documented reason, every file fsynced (file and directory) before the yank has its hash, and the read-only
# session leaves the image hash unchanged. S-DISC-05: the same checks must FAIL on an injected fault (a flipped
# byte in a synced file; the dirty flag cleared).
#
# The guest comes from tests/integration/daemon/dmn_vm.sh (--keep: clone, NTFS-3G, daemon, probe, volmark, the
# fixture); this script adds disc_guest.sh and runs it. This script takes no VM slot: the caller holds it
# (scripts/test-all.sh and vm/macos/run-suite.sh do; a standalone run takes none: with two other guests
# running, wait or take a slot-N directory yourself). Nothing is mounted on the host.
# A partial run (fewer yanks than planned, no S-DISC-05 when controls were requested, a guest exit other than
# 0 or 1, a failed setup step) FAILs. An ntfsfix failure may be a mirror_split point: classify it by hand.
# Exit: 0 every case passed, 1 a case failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
py=${PYTHON:-python3}
out=
fx=
reps=3
build=$root/build/release
deps_tar=
workloads="seq small churn fsync"
controls="hash dirty"
keep=0
fault=
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "disconnect: --out needs a directory" >&2; exit 2; }; out=$2; shift 2 ;;
	--reps) [ $# -ge 2 ] || { echo "disconnect: --reps needs a number" >&2; exit 2; }; reps=$2; shift 2 ;;
	--full) reps=20; shift ;;
	--fixtures) [ $# -ge 2 ] || { echo "disconnect: --fixtures needs a directory" >&2; exit 2; }; fx=$2; shift 2 ;;
	--build-dir) [ $# -ge 2 ] || { echo "disconnect: --build-dir needs a directory" >&2; exit 2; }; build=$2; shift 2 ;;
	--deps-tar) [ $# -ge 2 ] || { echo "disconnect: --deps-tar needs a file" >&2; exit 2; }; deps_tar=$2; shift 2 ;;
	--workloads) [ $# -ge 2 ] || { echo "disconnect: --workloads needs a list" >&2; exit 2; }; workloads=$2; shift 2 ;;
	--controls) [ $# -ge 2 ] || { echo "disconnect: --controls needs a list" >&2; exit 2; }; controls=$2; shift 2 ;;
	--fault) [ $# -ge 2 ] || { echo "disconnect: --fault needs hash or dirty" >&2; exit 2; }; fault=$2; shift 2 ;;
	--keep) keep=1; shift ;;
	-h | --help) sed -n '4,48p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "disconnect: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$out" ] || { echo "disconnect: --out is required" >&2; exit 2; }
case $reps in '' | *[!0-9]* | 0) echo "disconnect: --reps must be a positive number" >&2; exit 2 ;; esac
for w in $workloads; do
	case $w in seq | small | churn | fsync) ;; *) echo "disconnect: unknown workload $w" >&2; exit 2 ;; esac
done
for c in $controls; do
	case $c in hash | dirty) ;; *) echo "disconnect: unknown control $c" >&2; exit 2 ;; esac
done
case $fault in '' | hash | dirty) ;; *) echo "disconnect: --fault must be hash or dirty" >&2; exit 2 ;; esac
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
case $fx in '' | /*) ;; *) fx=$(pwd)/$fx ;; esac
case $build in /*) ;; *) build=$(pwd)/$build ;; esac
case $deps_tar in '' | /*) ;; *) deps_tar=$(pwd)/$deps_tar ;; esac
mkdir -p "$out"
# the commit and tree state at the START of the run (a result is tied to it, not to the state at the end)
"$py" -B "$root/tests/harness/report.py" provenance --out "$out/provenance.txt" >/dev/null || { echo "disconnect: cannot record the start state (is this a repository checkout?)" >&2; exit 2; }
# the keys of the inputs (deps, fixtures) are read once, now, and appended to the start record; the record at the end uses these
sh "$root/scripts/run-inputs.sh" snapshot "$out/provenance.txt" --deps --fixtures >&2 ||
	{ echo "disconnect: the inputs keys are unavailable" >&2; exit 2; }
# the host binaries under test must come from the recorded commit: the build tree must belong to this checkout,
# and an incremental build (a no-op when current) brings it up to date before anything is copied
"$py" -B "$root/tests/harness/report.py" build-check --build "$build" >&2 || exit 2
cmake --build "$build" >&2 || { echo "disconnect: cmake --build $build failed" >&2; exit 2; }
echo "binaries=ntfsmacd and ntfsmac rebuilt with cmake --build at the recorded commit before the run" >>"$out/provenance.txt"

"$py" -B "$here/disc_result.py" selftest >&2 || { echo "disconnect: disc_result.py selftest failed" >&2; exit 2; }
for f in "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac"; do
	[ -f "$f" ] || { echo "disconnect: $f is missing (cmake --build --preset release, or --build-dir)" >&2; exit 2; }
done

# a prebuilt NTFS-3G must be the one this checkout builds (scripts/run-inputs.sh, audit M5-F07): refused before anything starts
[ -z "$deps_tar" ] || [ ! -f "$deps_tar" ] || sh "$root/scripts/run-inputs.sh" deps-check "$deps_tar" --key "$(sed -n 's/^deps_key=//p' "$out/provenance.txt" | tail -n 1)" ||
	{ echo "disconnect: --deps-tar refused" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/disconnect.XXXXXX")
# the guest's name is chosen here, before the clone, so that the trap deletes it on every exit path
name=ntfsmac-disc-$$
dpid=
started=$($py -B -c 'import time; print(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))')
cleanup() {
	trap '' INT TERM HUP
	if [ -n "$dpid" ]; then
		kill "$dpid" 2>/dev/null || true
		wait "$dpid" 2>/dev/null || true
	fi
	if [ "$keep" -eq 0 ]; then
		# tart.sh delete of a name that was never cloned (an early exit) is a no-op
		"$tart_sh" delete "$name" >&2 || echo "disconnect: WARNING: could not delete $name" >&2
	else
		echo "disconnect: guest $name kept (--keep): delete it with vm/macos/tart.sh delete $name" >&2
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

if [ -z "$fx" ]; then
	fx=$tmp/fixtures
	echo "disconnect: generating clean-64m-4k in Environment A" >&2
	sh "$root/tests/fixtures/gen/clean.sh" --only clean-64m-4k --export "$fx" >&2 || { echo "disconnect: fixture generation failed" >&2; exit 2; }
fi
[ -f "$fx/clean-64m-4k.img" ] || { echo "disconnect: $fx/clean-64m-4k.img is missing" >&2; exit 2; }

# the guest: dmn_vm.sh with --keep clones the golden VM, gets NTFS-3G, and copies the daemon, the sources
# and the fixture in, running no suite; its name is in its output
dargs=
[ -z "$deps_tar" ] || dargs="--deps-tar $deps_tar"
# shellcheck disable=SC2086
# in the background, so that a TERM reaches it through the trap (a foreground child would only get it after it ends)
sh "$root/tests/integration/daemon/dmn_vm.sh" --keep --name "$name" --build-dir "$build" --fixtures "$fx" $dargs kill >"$tmp/dmn_vm.log" 2>&1 &
dpid=$!
wait "$dpid" || true
dpid=
if ! grep -q 'is up with everything copied in' "$tmp/dmn_vm.log"; then
	tail -n 20 "$tmp/dmn_vm.log" >&2
	echo "disconnect: the guest $name is not ready" >&2
	exit 2
fi

# what the run used (deps tar hash and key, fixtures key and manifest): inputs.txt, meta.txt, and every case note
sh "$root/scripts/run-inputs.sh" record "$out/inputs.txt" --snapshot "$out/provenance.txt" --deps-tar "$deps_tar" --fixtures "$fx" --names clean-64m-4k >&2 ||
	{ echo "disconnect: the inputs check failed" >&2; exit 2; }
# the plan: what disc_result.py holds the run to (a partial run must not pass)
{ echo "reps=$reps"; echo "workloads=$workloads"; echo "controls=$controls"; echo "fault=$fault"; } >"$out/plan.txt"
nrep=0
for w in $workloads; do nrep=$((nrep + reps)); done
for c in $controls; do nrep=$((nrep + 1)); done
budget=$((nrep * 240 + 300)) # seconds: a yank cycle is about 25 s; 240 s is its hang limit

echo "disconnect: copying the suite into $name" >&2
(cd "$root" && tar -cf - tests/stress/disconnect) |
	tart exec -i "$name" /bin/sh -c 'mkdir -p ~/ntfsmac/dmn && tar -xf - -C ~/ntfsmac/dmn'

echo "disconnect: running in the guest ($reps yank(s) per workload: $workloads; controls: ${controls:-none})" >&2
grc=0
perl -e 'alarm shift; exec @ARGV or exit 127' "$budget" sh "$tart_sh" exec "$name" -- "rm -rf ~/discout; DISC_FAULT='$fault' DISC_REPS=$reps DISC_WORKLOADS='$workloads' DISC_CONTROLS='$controls' sh ~/ntfsmac/dmn/tests/stress/disconnect/disc_guest.sh ~/ntfsmac/dmn ~/discout 2>&1" >"$out/guest.log" || grc=$?
tail -n 40 "$out/guest.log" >&2
# a failed fetch is not fatal: the result then says so (no reps, or an abnormal guest exit) and FAILs
perl -e 'alarm shift; exec @ARGV or exit 127' 180 tart exec "$name" /bin/sh -c 'cd ~/discout 2>/dev/null && tar -cf - .' | tar -xf - -C "$out" ||
	echo "disconnect: WARNING: could not fetch the evidence from the guest" >&2
echo "disconnect: guest script exit $grc" >&2
echo "guest_rc=$grc" >>"$out/meta.txt"
echo "host_os=macOS $(sw_vers -productVersion 2>/dev/null || echo host)" >>"$out/meta.txt"
grep -E '^(deps_tar|fixtures?[_.])' "$out/inputs.txt" >>"$out/meta.txt" || true

"$py" -B "$here/disc_result.py" finalize --dir "$out" --repo "$root" --started "$started"
