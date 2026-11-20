#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: daemon integration tests (the I-DMN-* range of docs/TEST_STRATEGY.md section 4) against
# the real installed daemon, in a disposable macOS guest (Environment B). One guest is cloned from
# the golden VM, NTFS-3G is unpacked (or built) in it, the probe, volmark, the stand-ins, the
# daemon and the CLI are installed as attach_vm.sh installs them, and the suites run in order.
# Nothing touches a host disk: the fixtures are image FILES that are attached as virtual disks
# with hdiutil inside the guest, and every mount is inside the guest (NTFS-3G with `norecover`,
# never `recover`, `remove_hiberfile` or `fsname`).
#
# Suites (each a guest script tests/integration/daemon/dmn_SUITE_guest.sh):
#   classes   I-DMN-01..08 (automount per fixture class: mode and reason, image hash unchanged
#             after a read-only session) and I-DMN-23 (a probe stand-in that reports `clean`)
#   kill      I-DMN-14 (SIGKILL of the worker and of the daemon), I-DMN-16 (force-detach during a
#             write), I-DMN-15 (restart reconciliation)
#   unmount   I-DMN-10..13 (the four unmount paths), I-DMN-17 (passive mode)
#   labels    I-DMN-18 (the hostile-label images of TEST_STRATEGY.md section 3.3)
#   priv      I-DMN-19..22, I-DMN-25 b, I-DMN-27 at daemon level: peers over XPC, the evidence
#             rule, `repair`, the marker, the disk mounted by another driver, notification text
#   slow      the mount_timeout regression: a probe that answers 12 s late
#   u15       U15: does Apple's ntfs.fs mount the vmk-split and mirror-split images (no daemon)
#   stress-kill   tests/stress/kill/kill_guest.sh (T-M5-05, S-KILL-*): repeated SIGKILL of the worker, the daemon
#             and the macFUSE module process and the yank of the image, KILL_REPS (default 2) times each, with
#             deadlines; started by tests/stress/kill/run.sh, which takes the KILL_* parameters from its options
#   perf      tests/perf/perf_guest.sh (T-M5-07, P-*): the benchmark on a daemon mount, PERF_ARGS are the bench.py
#             parameters; started by tests/perf/run.sh, which makes the fixture perf-ntfs.img
#   finder    tests/integration/finder: what Finder (and the system) show for a mounted volume
#
# Usage: tests/integration/daemon/dmn_vm.sh [--build-dir DIR] [--fixtures DIR] [--deps-tar FILE]
#                                          [--out FILE] [SUITE...]      (default: every suite)
#   --build-dir  a CMake build tree with src/daemon/ntfsmacd and src/cli/ntfsmac (default
#                build/release; it needs the test seams, which the release preset has)
#   --fixtures   a directory with the image files (tests/fixtures/gen/*.sh --export DIR)
#   --deps-tar   a tar of the guest's ~/ntfsmac/deps, used instead of building NTFS-3G when the file
#                exists; when it does not, the build is made and its result saved there
#   --name NAME  the guest's name (default ntfsmac-dmn-PID): the clone is named NAME exactly, so a caller knows it
#                before the clone and can delete it on every exit path
#   --only       a space-separated list of fixture names; the classes suite runs only those
#   --keep       development: copy everything in, run no suite and keep the guest (delete it yourself)
#   --out        also write the output to FILE
#   --graphics   open the guest's window (`vm/macos/tart.sh run NAME --graphics`), for a person to look at
#   --attended   M4 criterion 4.7 with a person at the host (docs/runbooks/finder-4.7.md): implies --graphics;
#                before the `finder` suite it triggers the Automation prompt once ('"tart-guest-agent" wants
#                access to control "Finder"'; guest commands run through tart exec are attributed to the
#                guest agent) and waits up to 5 minutes for the person to click Allow; the Finder cases
#                are then required (a refusal is a FAIL, not a NOT RUNNABLE)
#   --finder-base    clone from ntfsmac-golden-finder (vm/macos/provision-finder-base.sh: Finder automation
#                already allowed there) instead of ntfsmac-golden; the Finder cases are required and no
#                person is needed. Use it for M4 criterion 4.7 when the click in --attended is lost.
#   --evidence DIR   where the finder suite's screenshots and manifests are copied to before the guest is
#                deleted (default tests/results/evidence/m4-gate/finder/DATE-HEAD); done whenever `finder` runs
# This script does not take a guest slot: the caller holds the lock it needs (dmn_all.sh --slot-dir does).
# Exit: 0 all checks passed, 1 a check failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-dmn-$$
build=$root/build/release
fx=
out=
deps_tar=
suites=
only=
keep=0
graphics=0
attended=0
base=golden
require_finder=0
evidence=

while [ $# -gt 0 ]; do
	case "$1" in
	--build-dir) [ $# -ge 2 ] || { echo "dmn_vm: --build-dir needs a directory" >&2; exit 2; }; build=$2; shift 2 ;;
	--fixtures) [ $# -ge 2 ] || { echo "dmn_vm: --fixtures needs a directory" >&2; exit 2; }; fx=$2; shift 2 ;;
	--deps-tar) [ $# -ge 2 ] || { echo "dmn_vm: --deps-tar needs a file" >&2; exit 2; }; deps_tar=$2; shift 2 ;;
	--keep) keep=1; shift ;;
	--graphics) graphics=1; shift ;;
	--attended) attended=1; require_finder=1; graphics=1; shift ;;
	--finder-base) base=golden-finder; require_finder=1; shift ;;
	--evidence) [ $# -ge 2 ] || { echo "dmn_vm: --evidence needs a directory" >&2; exit 2; }; evidence=$2; shift 2 ;;
	--name) [ $# -ge 2 ] || { echo "dmn_vm: --name needs a guest name" >&2; exit 2; }; name=$2; shift 2 ;;
	--only) [ $# -ge 2 ] || { echo "dmn_vm: --only needs a list of fixture names" >&2; exit 2; }; only=$2; shift 2 ;;
	--out) [ $# -ge 2 ] || { echo "dmn_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	-*) echo "dmn_vm: unknown option $1" >&2; exit 2 ;;
	*) suites="$suites $1"; shift ;;
	esac
done
[ -n "$suites" ] || suites="classes kill unmount labels priv slow u15 finder"
[ -n "$fx" ] || { echo "dmn_vm: --fixtures is required" >&2; exit 2; }
for f in "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac"; do
	[ -f "$f" ] || { echo "dmn_vm: $f is missing" >&2; exit 2; }
done

# The fixtures a suite needs (names without .img)
fixtures_for() {
	case $1 in
	classes)
		[ -z "$only" ] || { echo "$only"; return; }
		echo "clean-64m-4k log-init log-bumped chkdsk-flag hiber-inactive hiber-subdir txf-nonresident dirty-flag dirty-chkdsk-flag unclean-log log-v2 hiber-lower hiber-upper hiber-case hiber-short hiber-wake hiber-unknown truncated truncated-60 bitlocker-sig vmk-split vmk-split-clear mirror-split" ;;
	kill | stress-kill) echo "clean-64m-4k" ;;
	perf) echo "perf-ntfs" ;;
	priv) echo "clean-64m-4k dirty-flag dirty-chkdsk-flag unclean-log hiber-lower log-init mirror-split" ;;
	unmount | finder) echo "clean-64m-4k" ;;
	slow) echo "clean-64m-4k" ;;
	labels) ls "$fx" | sed -n 's/^\(label-.*\)\.img$/\1/p' ;;
	u15) echo "clean-64m-4k vmk-split vmk-split-clear mirror-split" ;;
	*) echo "" ;;
	esac
}
need=
for s in $suites; do
	[ -f "$here/dmn_${s}_guest.sh" ] || [ "$s" = finder ] || [ "$s" = stress-kill ] || [ "$s" = perf ] || { echo "dmn_vm: no suite $s" >&2; exit 2; }
	for f in $(fixtures_for "$s"); do
		case " $need " in *" $f "*) ;; *) need="$need $f" ;; esac
		[ -f "$fx/$f.img" ] || { echo "dmn_vm: $fx/$f.img is missing" >&2; exit 2; }
	done
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/dmn_vm.XXXXXX")
cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "dmn_vm: WARNING: could not delete $name" >&2
	rm -rf "$tmp"
}
if [ "$keep" -eq 1 ]; then
	cleanup() { rm -rf "$tmp"; echo "dmn_vm: guest $name kept (--keep): delete it with vm/macos/tart.sh delete $name" >&2; }
fi
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "dmn_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { [ -n "$deps_tar" ] && [ -f "$deps_tar" ]; } ||
	{ echo "dmn_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

"$tart_sh" clone "$name" --from "$base" >&2
if [ "$graphics" -eq 1 ]; then "$tart_sh" run "$name" --graphics >&2; else "$tart_sh" run "$name" >&2; fi

if [ -n "$deps_tar" ] && [ -f "$deps_tar" ]; then
	echo "dmn_vm: NTFS-3G from $deps_tar (no build)" >&2
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/deps && mkdir -p ~/ntfsmac && tar -xf - -C ~/ntfsmac' <"$deps_tar"
else
	echo "dmn_vm: building NTFS-3G in the guest" >&2
	(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
		third_party/patches "third_party/cache/$tarball") |
		tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
	"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
		{ echo "dmn_vm: guest build failed" >&2; exit 2; }
	if [ -n "$deps_tar" ]; then
		tart exec "$name" /bin/sh -c 'cd ~/ntfsmac && tar -cf - deps' >"$deps_tar" ||
			{ rm -f "$deps_tar"; echo "dmn_vm: WARNING: could not save $deps_tar" >&2; }
	fi
fi

echo "dmn_vm: copying the sources, the host-built daemon and the fixtures" >&2
mkdir -p "$tmp/bin" "$tmp/fixtures"
cp "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac" "$tmp/bin/"
(cd "$root" && tar -cf - include integration src/probe src/volmark src/common src/cli src/daemon src/journal \
	src/policy src/options src/sanitize tests/integration/daemon tests/integration/finder tests/stress/kill tests/perf tests/tools) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/dmn && mkdir -p ~/ntfsmac/dmn && tar -xf - -C ~/ntfsmac/dmn'
(cd "$tmp" && tar -cf - bin) | tart exec -i "$name" /bin/sh -c 'tar -xf - -C ~/ntfsmac/dmn'
if [ -n "$need" ]; then
	for f in $need; do
		cp "$fx/$f.img" "$tmp/fixtures/$f.img"
		[ ! -f "$fx/$f.leaf" ] || cp "$fx/$f.leaf" "$tmp/fixtures/$f.leaf"
	done
	(cd "$tmp" && tar -czf - fixtures) | tart exec -i "$name" /bin/sh -c 'tar -xzf - -C ~/ntfsmac/dmn'
fi

# finder_allow: trigger the Automation prompt for Finder once and wait for a person to click Allow in
# the guest window. The prompt names the process that asks: for commands run with tart exec that is
# the guest agent (checked in the guest: '"tart-guest-agent" wants access to control "Finder"').
finder_allow() {
	echo "dmn_vm: >>> ACTION NEEDED: in the VM window, click Allow on the '\"tart-guest-agent\" wants access to control \"Finder\"' prompt (waiting up to 5 minutes)" >&2
	arc=0
	# AppleScript gives up on a reply after 2 minutes unless told otherwise (-1712), and the prompt
	# blocks the reply: wait the full 5 minutes. The error text is shown, so a refusal (-1743) and a
	# timeout can be told apart.
	aout=$("$tart_sh" exec "$name" -- "perl -e 'alarm 310; exec @ARGV or exit 127' osascript -e 'with timeout of 300 seconds' -e 'tell application \"Finder\" to get name of startup disk' -e 'end timeout' 2>&1 >/dev/null") || arc=$?
	if [ "$arc" -eq 0 ]; then
		echo "dmn_vm: Finder automation allowed" >&2
	else
		echo "dmn_vm: Finder automation was NOT allowed (osascript rc $arc: ${aout:-no message}); the Finder cases will FAIL" >&2
	fi
}
# finder_check: with --finder-base the permission is already in the guest; one quick event shows it.
finder_check() {
	arc=0
	aout=$("$tart_sh" exec "$name" -- "perl -e 'alarm 40; exec @ARGV or exit 127' osascript -e 'with timeout of 30 seconds' -e 'tell application \"Finder\" to get name of startup disk' -e 'end timeout' 2>&1 >/dev/null") || arc=$?
	if [ "$arc" -eq 0 ]; then
		echo "dmn_vm: Finder automation allowed (from ntfsmac-golden-finder)" >&2
	else
		echo "dmn_vm: Finder automation was NOT allowed in the clone of ntfsmac-golden-finder (osascript rc $arc: ${aout:-no message}); rebuild it with vm/macos/provision-finder-base.sh --rebuild" >&2
	fi
}
# copy_evidence: the finder suite's screenshots and manifests to the host, before the guest is deleted
copy_evidence() {
	if [ -z "$evidence" ]; then
		rev=$(git -C "$root" rev-parse --short HEAD 2>/dev/null || echo nogit)
		evidence=$root/tests/results/evidence/m4-gate/finder/$(date +%Y-%m-%d)-$rev
	fi
	mkdir -p "$evidence"
	if tart exec "$name" /bin/sh -c 'cd ~/finder-evidence 2>/dev/null && tar -cf - .' | tar -xf - -C "$evidence" &&
		[ -n "$(ls -A "$evidence")" ]; then
		echo "dmn_vm: finder evidence copied to ${evidence#"$root"/}: $(ls "$evidence" | tr '\n' ' ')" >&2
	elif rmdir "$evidence" 2>/dev/null; then
		echo "dmn_vm: no finder evidence (the Finder cases did not run)" >&2
	else
		echo "dmn_vm: WARNING: no finder evidence could be copied" >&2
	fi
}

# Each suite's exit status goes to a file, so that the output can stream through tee.
run_all() {
	[ "$keep" -eq 0 ] || { echo "dmn_vm: guest $name is up with everything copied in (--keep)"; return 0; }
	for s in $suites; do
		echo "######## suite $s"
		g=tests/integration/daemon/dmn_${s}_guest.sh
		[ "$s" = finder ] && g=tests/integration/finder/finder_guest.sh
		[ "$s" = stress-kill ] && g=tests/stress/kill/kill_guest.sh
		[ "$s" = perf ] && g=tests/perf/perf_guest.sh
		rc=0
		[ "$s" != finder ] || [ "$attended" -eq 0 ] || finder_allow
		[ "$s" != finder ] || [ "$base" != golden-finder ] || finder_check
		# the output is also kept in the guest (~/dmn-SUITE.log), so a long suite can be watched
		"$tart_sh" exec "$name" -- "{ DMN_ONLY='$only' KILL_REPS='${KILL_REPS:-}' KILL_TARGETS='${KILL_TARGETS:-}' KILL_STEP='${KILL_STEP:-}' KILL_CONTROL='${KILL_CONTROL:-}' PERF_ARGS='${PERF_ARGS:-}' PERF_LABEL='${PERF_LABEL:-}' FINDER_REQUIRED=$require_finder sh ~/ntfsmac/dmn/$g ~/ntfsmac/dmn; echo \$? >~/dmn-$s.rc; } 2>&1 | tee ~/dmn-$s.log; exit \$(cat ~/dmn-$s.rc)" || rc=$?
		[ "$s" != finder ] || copy_evidence
		echo "######## suite $s: exit $rc"
		[ "$rc" -eq 0 ] || echo 1 >"$tmp/failed"
	done
}
if [ -n "$out" ]; then
	run_all 2>&1 | tee "$out"
else
	run_all 2>&1
fi
[ ! -f "$tmp/failed" ]
