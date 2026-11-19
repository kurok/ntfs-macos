#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: a run of the whole I-DMN-* range that is runnable now (docs/TEST_STRATEGY.md section 4):
# every guest driver of the range, one disposable macOS guest each, in order, with a summary of
# exit codes and PASS/FAIL counts and the I-DMN ids each driver covers. Nothing here touches a host
# disk; each driver clones its own guest from the golden VM and deletes it.
#
#   spawn_vm.sh                 I-DMN-09, I-DMN-26 b and c (module level, U6, U20)
#   spawn_vm.sh --sandbox       I-SBX-06 (the sandboxed worker mounts)
#   sandbox_vm.sh               I-SBX-* (I-DMN-09 adjacent)
#   errwatch_vm.sh              I-DMN-24 a and b (module level, U16)
#   journal_vm.sh               I-DMN-19..22, I-DMN-25 a and c (module level)
#   xpc_vm.sh                   the XPC door of I-DMN-19 with fake operations
#   dmn_mirror_vm.sh            I-DMN-27 (module level)
#   attach_vm.sh                the T-M4-08 manual attach (needs --fixtures)
#   dmn_vm.sh classes kill unmount labels priv slow u15 finder
#                               I-DMN-01..08, 10..13, 14..16, 17, 18, 19..22, 23, 25 b, 27 at daemon
#                               level, U15, Finder
#
# Usage: tests/integration/daemon/dmn_all.sh --fixtures DIR [--deps-tar FILE] [--out-dir DIR]
#                                           [--slot-dir DIR] [DRIVER...]
#   --fixtures   the image files (tests/fixtures/gen/*.sh --export DIR), for attach_vm.sh and dmn_vm.sh
#   --out-dir    where each driver's output goes (default: a temporary directory, printed)
#   --slot-dir   a directory for the slot locks slot-1 and slot-2, taken with mkdir around each guest
#                (a `pid` file in the lock names the holder; a lock whose holder is gone is removed);
#                default: none, and then the caller holds whatever lock it needs
#   DRIVER       a subset of: spawn spawn-sbx sandbox errwatch journal xpc mirror attach dmn
# Exit: 0 every driver passed, 1 a driver failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fx=
deps_tar=
outdir=
slotdir=
drivers=
while [ $# -gt 0 ]; do
	case "$1" in
	--fixtures) fx=$2; shift 2 ;;
	--deps-tar) deps_tar=$2; shift 2 ;;
	--out-dir) outdir=$2; shift 2 ;;
	--slot-dir) slotdir=$2; shift 2 ;;
	-*) echo "dmn_all: unknown option $1" >&2; exit 2 ;;
	*) drivers="$drivers $1"; shift ;;
	esac
done
[ -n "$drivers" ] || drivers="spawn spawn-sbx sandbox errwatch journal xpc mirror attach dmn"
[ -n "$fx" ] || { echo "dmn_all: --fixtures is required" >&2; exit 2; }
[ -n "$outdir" ] || outdir=$(mktemp -d "${TMPDIR:-/tmp}/dmn_all.XXXXXX")
mkdir -p "$outdir"
echo "dmn_all: output in $outdir"

# The slot lock is a directory with a file `pid` (the process that holds it). A lock whose pid file
# names a process that is gone is stale and is removed; a lock without a pid file belongs to
# someone else and is never touched.
slot=
take_slot() {
	[ -n "$slotdir" ] || return 0
	while [ -z "$slot" ]; do
		for s in 1 2; do
			if mkdir "$slotdir/slot-$s" 2>/dev/null; then
				echo $$ >"$slotdir/slot-$s/pid"
				slot=$s
				break
			fi
			p=$(cat "$slotdir/slot-$s/pid" 2>/dev/null || true)
			if [ -n "$p" ] && ! kill -0 "$p" 2>/dev/null; then
				echo "dmn_all: removing the stale slot-$s (holder $p is gone)" >&2
				rm -f "$slotdir/slot-$s/pid"
				rmdir "$slotdir/slot-$s" 2>/dev/null || true
			fi
		done
		[ -n "$slot" ] || sleep 30
	done
}
release_slot() {
	if [ -n "$slot" ]; then
		rm -f "$slotdir/slot-$slot/pid"
		rmdir "$slotdir/slot-$slot"
	fi
	slot=
}
trap 'release_slot' EXIT
trap 'exit 130' INT TERM HUP

depsargs=
[ -z "$deps_tar" ] || depsargs="--deps-tar $deps_tar"
summary=$outdir/summary.txt
: >"$summary"
bad=0
for dr in $drivers; do
	out=$outdir/$dr.txt
	case $dr in
	spawn) cmd="sh $here/spawn_vm.sh --out $out" ;;
	spawn-sbx) cmd="sh $here/spawn_vm.sh --sandbox --out $out" ;;
	sandbox) cmd="sh $here/sandbox_vm.sh --out $out" ;;
	errwatch) cmd="sh $here/errwatch_vm.sh --out $out" ;;
	journal) cmd="sh $here/journal_vm.sh --out $out" ;;
	xpc) cmd="sh $here/xpc_vm.sh --out $out" ;;
	mirror) cmd="sh $here/dmn_mirror_vm.sh --out $out" ;;
	attach) cmd="sh $here/attach_vm.sh --fixtures $fx $depsargs --out $out" ;;
	dmn) cmd="sh $here/dmn_vm.sh --fixtures $fx $depsargs --out $out classes kill unmount labels priv slow u15 finder" ;;
	*) echo "dmn_all: unknown driver $dr" >&2; exit 2 ;;
	esac
	take_slot
	echo "dmn_all: === $dr: $cmd"
	rc=0
	# shellcheck disable=SC2086
	$cmd >"$outdir/$dr.log" 2>&1 || rc=$?
	release_slot
	p=$(grep -c '^PASS' "$out" 2>/dev/null || true)
	f=$(grep -c '^FAIL' "$out" 2>/dev/null || true)
	line=$(printf '%-10s exit %s  PASS %s  FAIL %s' "$dr" "$rc" "${p:-0}" "${f:-0}")
	echo "$line" | tee -a "$summary"
	[ "$rc" -eq 0 ] || bad=1
done
echo "dmn_all: summary in $summary"
exit "$bad"
