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

# The slot lock is a directory with a file `pid` (the process that holds it); the protocol is the one of
# vm/macos/run-suite.sh. A lock whose holder is gone is taken over by claiming its pid file (the directory never
# moves); a lock is released only when its pid file holds this shell's pid.
slot=
# pid_gone PID: 0 when no process has that pid. kill -0 alone would call a live process of another user dead
# (EPERM), so ps -p is asked too: a process is gone only when both fail.
pid_gone() {
	case $1 in '' | *[!0-9]*) return 0 ;; esac
	kill -0 "$1" 2>/dev/null && return 1
	ps -p "$1" >/dev/null 2>&1 && return 1
	return 0
}
# grab_slot_claim SRC PID: take over a stale slot of dir gs_d by claiming its pid file SRC (the slot dir itself never
# moves, so the number of slots can never be exceeded): rename SRC to pid.$$ (atomic, one contender wins), check it
# still holds the stale PID, then create the pid file (noclobber: it must not exist). A file whose content is not the
# stale PID (a live run's pid) is never deleted: it is put back, or left where it is.
grab_slot_claim() {
	mv "$1" "$gs_d/pid.$$" 2>/dev/null || return 1
	gs_x=$(cat "$gs_d/pid.$$" 2>/dev/null || true)
	if [ "$gs_x" = "$2" ]; then
		if (set -C; echo $$ >"$gs_d/pid") 2>/dev/null; then
			rm -f "$gs_d/pid.$$"
			return 0
		fi
		rm -f "$gs_d/pid.$$" # the stale content only; somebody else has created the pid file
		return 1
	fi
	[ -e "$1" ] || mv "$gs_d/pid.$$" "$1" 2>/dev/null || true
	return 1
}
# grab_slot N: take slot-N when it is free or its holder is gone; 0 on success, 1 when someone else holds it.
# Stale: the pid file names a dead process, or is empty and more than a minute old (a crash between its creation
# and its write); or there is no pid file and a pid.N claim file of a dead claimer N, or more than a minute old (a
# crash after the rename; N may be a reused pid) holding a dead pid; or an empty slot more than a minute old (a
# crash between mkdir and the pid file).
grab_slot() {
	gs_d=$slotdir/slot-$1
	if mkdir "$gs_d" 2>/dev/null; then
		echo $$ >"$gs_d/pid"
		return 0
	fi
	if [ -e "$gs_d/pid" ]; then
		gs_p=$(cat "$gs_d/pid" 2>/dev/null || true)
		if [ -n "$gs_p" ]; then
			pid_gone "$gs_p" || return 1
		else
			[ -n "$(find "$gs_d/pid" -mmin +1 2>/dev/null)" ] || return 1
		fi
		grab_slot_claim "$gs_d/pid" "$gs_p" || return 1
		echo "dmn_all: removed the stale slot-$1 (holder '$gs_p' is gone)" >&2
		return 0
	fi
	gs_k=
	for gs_c in "$gs_d"/pid.*; do
		[ -f "$gs_c" ] || continue
		gs_k=1
		# a claim in progress (its claimer is alive and the file is recent): leave the slot alone
		if ! pid_gone "${gs_c##*/pid.}" && [ -z "$(find "$gs_c" -mmin +1 2>/dev/null)" ]; then
			return 1
		fi
	done
	for gs_c in "$gs_d"/pid.*; do
		[ -f "$gs_c" ] || continue
		gs_p=$(cat "$gs_c" 2>/dev/null || true)
		if [ -n "$gs_p" ] && pid_gone "$gs_p"; then
			grab_slot_claim "$gs_c" "$gs_p" || return 1
			echo "dmn_all: removed the stale slot-$1 (claim of a gone run, holder $gs_p is gone)" >&2
			return 0
		fi
		# the claimer died holding a live pid: give the pid file back
		[ ! -e "$gs_d/pid" ] && mv "$gs_c" "$gs_d/pid" 2>/dev/null || true
		return 1
	done
	[ -z "$gs_k" ] || return 1
	# an empty slot that is more than a minute old
	[ -n "$(find "$gs_d" -maxdepth 0 -mmin +1 2>/dev/null)" ] || return 1
	(set -C; echo $$ >"$gs_d/pid") 2>/dev/null || return 1
	echo "dmn_all: took the abandoned slot-$1 (no pid file)" >&2
	return 0
}
# release_one N: give slot-N back, but only when its pid file holds this shell's pid
release_one() {
	[ "$(cat "$slotdir/slot-$1/pid" 2>/dev/null || true)" = "$$" ] || return 0
	rm -f "$slotdir/slot-$1/pid"
	rmdir "$slotdir/slot-$1" 2>/dev/null || true
}
take_slot() {
	[ -n "$slotdir" ] || return 0
	while [ -z "$slot" ]; do
		for s in 1 2; do
			if grab_slot "$s"; then
				slot=$s
				break
			fi
		done
		[ -n "$slot" ] || sleep 30
	done
}
release_slot() {
	[ -z "$slot" ] || release_one "$slot"
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
