#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests the VM slot protocol (slot_try of scripts/ci-local.sh, grab_slot of vm/macos/run-suite.sh) and the start
# retry of vm/macos/tart.sh run, with stubs only: no VM is started, nothing is built.
#   - a stale slot taken over by two contenders at once: exactly one wins, the live slot is never removed;
#   - a holder that kill -0 cannot signal (EPERM, a live process of another user) counts as alive;
#   - tart run fails once with VZErrorDomain Code=1, then starts: retried once; Code=6 and others: not retried.
# Registered with CTest as ci.vm_slots (tests/unit/ci/CMakeLists.txt); exit 77 = skip (tart.sh needs macOS arm64).
# Usage: tests/unit/ci/test_vm_slots.sh   (exit 0 = all pass)
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH='' cd -- "$here/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/vm-slots-test.XXXXXX")
cleanup() {
	for _f in "$work"/state/*.pid; do
		[ -f "$_f" ] || continue
		kill "$(cat "$_f")" 2>/dev/null || true
	done
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP
fails=0
check() { if [ "$2" -eq 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }

# the functions, cut out of the scripts. libs: NAME:GRAB:RELEASE:DIRVAR
sed -n '/^pid_gone() {$/,/^# take_slot: wait/p' "$repo/vm/macos/run-suite.sh" | sed '$d' >"$work/rs.sh"
sed -n '/^\tpid_gone() {$/,/^\ttake_slot() {$/p' "$repo/scripts/ci-local.sh" | sed '$d' >"$work/cl.sh"
sed -n '/^pid_gone() {$/,/^take_slot() {$/p' "$repo/tests/integration/daemon/dmn_all.sh" | sed '$d' >"$work/dm.sh"
libs='rs:grab_slot:release_one:slot_dir cl:slot_try:slot_rel:slot_dir dm:grab_slot:release_one:slotdir'
for _l in $libs; do
	_n=${_l%%:*}; _g=${_l#*:}; _g=${_g%%:*}
	[ -s "$work/$_n.sh" ] && grep -q "^[[:space:]]*$_g() {" "$work/$_n.sh" || { echo "FAIL  $_n: slot functions not found"; exit 1; }
done

# a pid that is certainly dead
sh -c 'exit 0' & dead=$!
wait "$dead" || true

# ---- contenders: one script per role, started together after a barrier -----------------------------------------
# usage: contender.sh LIB FUNC DIRVAR DIR ID RESDIR; wins -> res/won.ID; the winner stays alive until all are done
cat >"$work/contender.sh" <<'EOS'
lib=$1 fn=$2 dv=$3 dir=$4 id=$5 res=$6
. "$lib"
eval "$dv=\$dir"
while [ ! -e "$res/go" ]; do :; done
if $fn 1; then : >"$res/won.$id"; else : >"$res/lost.$id"; fi
: >"$res/done.$id"
n=0
while [ ! -e "$res/done.a" ] || [ ! -e "$res/done.b" ]; do
	n=$((n + 1)); [ "$n" -lt 20000 ] || break
	sleep 0.01
done
EOS

race() { # race LIB FUNC DIRVAR NAME
	_ok=0
	_round=0
	while [ "$_round" -lt 40 ]; do
		_round=$((_round + 1))
		_sd=$work/$4.$_round/slots; _res=$work/$4.$_round/res
		mkdir -p "$_sd/slot-1" "$_res"
		echo "$dead" >"$_sd/slot-1/pid"
		sh "$work/contender.sh" "$1" "$2" "$3" "$_sd" a "$_res" 2>/dev/null & _pa=$!
		sh "$work/contender.sh" "$1" "$2" "$3" "$_sd" b "$_res" 2>/dev/null & _pb=$!
		: >"$_res/go"
		wait "$_pa" "$_pb" 2>/dev/null || true
		_w=0; for _f in "$_res"/won.*; do [ ! -e "$_f" ] || _w=$((_w + 1)); done
		_h=$(cat "$_sd/slot-1/pid" 2>/dev/null || true)
		# exactly one winner, the slot exists and holds the pid of no other process than a contender
		if [ "$_w" -ne 1 ] || [ -z "$_h" ] || [ "$_h" = "$dead" ]; then _ok=1; echo "round $_round: winners=$_w pid='$_h'" >&2; break; fi
		_left=0; for _f in "$_sd"/* "$_sd"/slot-1/pid.*; do [ "$_f" = "$_sd/slot-1" ] || [ ! -e "$_f" ] || _left=$((_left + 1)); done
		[ "$_left" -eq 0 ] || { _ok=1; echo "round $_round: leftovers in $_sd" >&2; break; }
	done
	return "$_ok"
}
# dir_var for the sourced functions is set per lib: slot_dir or slotdir
for lib in $libs; do
	IFS=: read -r l fn rel dv <<EOT
$lib
EOT
	eval "$dv=\$work/unused"
	if race "$work/$l.sh" "$fn" "$dv" "$l"; then r=0; else r=1; fi
	check "$l: a stale slot taken by two contenders at once: exactly one wins (40 rounds)" $r
done

for lib in $libs; do
	IFS=: read -r l fn rel dv <<EOT
$lib
EOT
	# shellcheck disable=SC1090
	. "$work/$l.sh"
	d=$work/live.$l; mkdir -p "$d/slot-1"; eval "$dv=\$d"
	sleep 30 & live=$!
	echo "$dead" >"$d/slot-1/pid"
	ino=$(ls -di "$d/slot-1")
	# the third-contender window: between the read of the stale pid and the claim a live run takes the slot over
	# (simulated by a cat that, after reading the slot's pid file, swaps the pid file for the pid of a live process).
	# The slot must stay where it is, keep the live pid, and no second holder may appear.
	cat() {
		command cat "$@"
		if [ "$#" -eq 1 ] && [ "$1" = "$d/slot-1/pid" ]; then echo "$live" >"$d/slot-1/pid"; fi
	}
	rc=0; $fn 1 2>/dev/null || rc=$?
	unset -f cat
	left=0; for f in "$d"/slot-1/pid.*; do [ ! -e "$f" ] || left=$((left + 1)); done
	if [ "$rc" -eq 1 ] && [ "$(command cat "$d/slot-1/pid" 2>/dev/null)" = "$live" ] && [ "$(ls -di "$d/slot-1")" = "$ino" ] && [ "$left" -eq 0 ]; then r=0; else r=1; fi
	check "$l: a live run that took the slot during the claim keeps it (same dir, same pid, no claim file left)" $r
	# release removes only a slot of this shell
	$rel 1
	if [ -f "$d/slot-1/pid" ] && [ "$(command cat "$d/slot-1/pid")" = "$live" ]; then r=0; else r=1; fi
	check "$l: release does not remove a slot held by another pid" $r
	echo $$ >"$d/slot-1/pid"; $rel 1
	if [ ! -e "$d/slot-1" ]; then r=0; else r=1; fi
	check "$l: release removes the slot whose pid file holds its own pid" $r
	kill "$live" 2>/dev/null || true; wait "$live" 2>/dev/null || true

	# a crash after the claim rename: no pid file, pid.N of a dead N holding a dead pid -> taken over
	mkdir -p "$d/slot-1"; echo "$dead" >"$d/slot-1/pid.$dead"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 0 ] && [ "$(command cat "$d/slot-1/pid")" = "$$" ] && [ ! -e "$d/slot-1/pid.$dead" ]; then r=0; else r=1; fi
	check "$l: a claim file of a dead claimer (crash after the rename) is stale" $r
	rm -rf "$d/slot-1"
	# a claim in progress (claimer alive) is respected
	mkdir -p "$d/slot-1"; echo "$dead" >"$d/slot-1/pid.1"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 1 ] && [ ! -e "$d/slot-1/pid" ]; then r=0; else r=1; fi
	check "$l: a claim file of a live claimer is respected" $r
	rm -rf "$d/slot-1"
	# an empty pid file: left alone while fresh (a taker between its open and its write), stale when a minute old
	mkdir -p "$d/slot-1"; : >"$d/slot-1/pid"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 1 ] && [ ! -s "$d/slot-1/pid" ]; then r=0; else r=1; fi
	check "$l: a fresh empty pid file is not taken" $r
	touch -t 200001010000 "$d/slot-1/pid"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 0 ] && [ "$(command cat "$d/slot-1/pid")" = "$$" ]; then r=0; else r=1; fi
	check "$l: an empty pid file older than a minute is stale" $r
	rm -rf "$d/slot-1"
	# a claim file older than a minute is stale although its claimer pid is alive (pid reuse; pid 1 here)
	mkdir -p "$d/slot-1"; echo "$dead" >"$d/slot-1/pid.1"; touch -t 200001010000 "$d/slot-1/pid.1"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 0 ] && [ "$(command cat "$d/slot-1/pid")" = "$$" ] && [ ! -e "$d/slot-1/pid.1" ]; then r=0; else r=1; fi
	check "$l: a claim file older than a minute is stale even when its claimer pid is alive" $r
	rm -rf "$d/slot-1"
	# every claim file is checked before one is acted on: a dead claimer's file next to a live claimer's recent one
	mkdir -p "$d/slot-1"; echo "$dead" >"$d/slot-1/pid.$dead"; echo "$dead" >"$d/slot-1/pid.1"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 1 ] && [ -e "$d/slot-1/pid.$dead" ] && [ -e "$d/slot-1/pid.1" ] && [ ! -e "$d/slot-1/pid" ]; then r=0; else r=1; fi
	check "$l: a live claimer anywhere among the claim files stops the takeover" $r
	rm -rf "$d/slot-1"
	# a claim file taken over while its content changed: the live pid it now holds is put back, never deleted
	mkdir -p "$d/slot-1"; echo "$dead" >"$d/slot-1/pid.$dead"
	sleep 30 & live=$!
	cat() {
		command cat "$@"
		if [ "$#" -eq 1 ] && [ "$1" = "$d/slot-1/pid.$dead" ]; then echo "$live" >"$d/slot-1/pid.$dead"; fi
	}
	rc=0; $fn 1 2>/dev/null || rc=$?
	unset -f cat
	# it goes back under its own name (the claimer's); the next try, seeing a dead claimer and a live pid, gives it back as the pid file
	if [ "$rc" -eq 1 ] && [ "$(command cat "$d/slot-1/pid.$dead" 2>/dev/null)" = "$live" ] && [ ! -e "$d/slot-1/pid" ]; then r=0; else r=1; fi
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$r" -eq 0 ] && [ "$rc" -eq 1 ] && [ "$(command cat "$d/slot-1/pid" 2>/dev/null)" = "$live" ] && [ ! -e "$d/slot-1/pid.$dead" ]; then r=0; else r=1; fi
	check "$l: a claim file that holds a live pid at the takeover is put back, then given back as the pid file" $r
	rm -rf "$d/slot-1"
	# a live run L takes the slot while M claims the crashed claimer's file: L's pid survives, M does not hold it
	mkdir -p "$d/slot-1"; echo "$dead" >"$d/slot-1/pid.$dead"
	cat() {
		command cat "$@"
		if [ "$#" -eq 1 ] && [ "$1" = "$d/slot-1/pid.$dead" ]; then echo "$live" >"$d/slot-1/pid"; fi
	}
	rc=0; $fn 1 2>/dev/null || rc=$?
	unset -f cat
	left=0; for f in "$d"/slot-1/pid.*; do [ ! -e "$f" ] || left=$((left + 1)); done
	if [ "$rc" -eq 1 ] && [ "$(command cat "$d/slot-1/pid" 2>/dev/null)" = "$live" ] && [ "$left" -eq 0 ]; then r=0; else r=1; fi
	check "$l: a live run's pid file survives a claim of the crashed claimer's file (one holder only)" $r
	kill "$live" 2>/dev/null || true; wait "$live" 2>/dev/null || true
	rm -rf "$d/slot-1"
	# an empty slot: left alone while fresh (a taker between mkdir and echo), taken when a minute old
	mkdir -p "$d/slot-1"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 1 ] && [ ! -e "$d/slot-1/pid" ]; then r=0; else r=1; fi
	check "$l: a fresh empty slot is not taken" $r
	touch -t 200001010000 "$d/slot-1"
	rc=0; $fn 1 2>/dev/null || rc=$?
	if [ "$rc" -eq 0 ] && [ "$(command cat "$d/slot-1/pid")" = "$$" ]; then r=0; else r=1; fi
	check "$l: an empty slot older than a minute (crash between mkdir and the pid file) is taken" $r
	rm -rf "$d/slot-1"
done

# ---- EPERM: kill -0 on a live process of another user fails with "Operation not permitted" -------------------
# pid 1 is such a process for a non-root user; ps -p still finds it. As root kill -0 succeeds, so the case is not tested
if [ "$(id -u)" -eq 0 ]; then
	echo "SKIP: EPERM check (running as root: kill -0 1 succeeds)"
else
	for lib in $libs; do
		IFS=: read -r l fn rel dv <<EOT
$lib
EOT
		# shellcheck disable=SC1090
		. "$work/$l.sh"
		d=$work/eperm.$l; mkdir -p "$d/slot-1"; eval "$dv=\$d"
		echo 1 >"$d/slot-1/pid"
		rc=0; $fn 1 2>/dev/null || rc=$?
		if [ "$rc" -eq 1 ] && [ "$(command cat "$d/slot-1/pid" 2>/dev/null)" = 1 ]; then r=0; else r=1; fi
		check "$l: a holder that kill -0 reports as EPERM (pid 1) counts as alive" $r
	done
fi
if pid_gone 1; then r=1; else r=0; fi; check "pid_gone 1 is false" $r
if pid_gone "$dead"; then r=0; else r=1; fi; check "pid_gone of an exited pid is true" $r

# ---- tart.sh run: the VZErrorDomain Code=1 retry -------------------------------------------------------------------
if [ "$(uname -s)" != Darwin ] || [ "$(uname -m)" != arm64 ]; then
	echo "SKIP: tart.sh needs an Apple silicon Mac"
	if [ "$fails" -ne 0 ]; then echo "$fails FAILED"; exit 1; fi
	echo "PASSED (tart part skipped)"; exit 77
fi
mkdir -p "$work/bin" "$work/state" "$work/vm"
cp "$repo/vm/macos/tart.sh" "$repo/vm/macos/versions.env" "$work/vm/"
cat >"$work/bin/tart" <<'EOS'
#!/bin/sh
# stub (a started VM is 'ready' for tart exec): STUB_ERR = the text the first STUB_FAILS runs print before they exit 1
case $1 in
list) echo ntfsmac-t1 ;;
ip) echo 192.0.2.7 ;;
exec) [ -e "$STUB_DIR/ready" ] ;;
run)
	n=$(cat "$STUB_DIR/runs" 2>/dev/null || echo 0); n=$((n + 1)); echo "$n" >"$STUB_DIR/runs"
	if [ "$n" -le "${STUB_FAILS:-0}" ]; then echo "$STUB_ERR" >&2; exit 1; fi
	: >"$STUB_DIR/ready"; exec sleep 30 ;;
*) exit 0 ;;
esac
EOS
chmod +x "$work/bin/tart"
tart_run() { # tart_run FAILS ERRTEXT
	rm -f "$work/runs" "$work/ready" "$work/state/"*
	rc=0
	PATH=$work/bin:$PATH STUB_DIR=$work STUB_FAILS=$1 STUB_ERR=$2 NTFSMAC_STATE=$work/state NTFSMAC_START_RETRY_WAIT=1 \
		NTFSMAC_SSH_WAIT=20 NTFSMAC_TRANSPORT=exec bash "$work/vm/tart.sh" run t1 >"$work/tout" 2>"$work/terr" || rc=$?
	if [ -f "$work/state/ntfsmac-t1.pid" ]; then kill "$(cat "$work/state/ntfsmac-t1.pid")" 2>/dev/null || true; fi
	runs=$(cat "$work/runs" 2>/dev/null || echo 0)
}
# the text tart writes to $STATE/$vm.log (seen in a real failure), which tart.sh prints with tail -5
tart_run 1 'Error Domain=VZErrorDomain Code=1 "The virtual machine failed to start." UserInfo={NSLocalizedFailure=Internal Virtualization error., NSLocalizedFailureReason=The virtual machine failed to start.}'
if [ "$rc" -eq 0 ] && [ "$runs" -eq 2 ] && grep -q 'retrying once' "$work/terr"; then r=0; else r=1; fi; check "tart run: Code=1 once, then success: started twice, the retry is logged" $r
tart_run 5 'Error Domain=VZErrorDomain Code=1 "The virtual machine failed to start."'
if [ "$rc" -ne 0 ] && [ "$runs" -eq 2 ] && grep -q 'tart run exited early' "$work/terr"; then r=0; else r=1; fi; check "tart run: Code=1 twice: retried once only, then fails" $r
tart_run 5 'Error Domain=VZErrorDomain Code=6 "The virtual machine limit has been reached."'
if [ "$rc" -ne 0 ] && [ "$runs" -eq 1 ] && ! grep -q 'retrying' "$work/terr" && grep -q 'tart run exited early' "$work/terr"; then r=0; else r=1; fi; check "tart run: Code=6 is not retried" $r
tart_run 5 'Error Domain=VZErrorDomain Code=12 "other"'
if [ "$rc" -ne 0 ] && [ "$runs" -eq 1 ]; then r=0; else r=1; fi; check "tart run: Code=12 (Code=1 prefix only) and other errors are not retried" $r
tart_run 5 'something else failed'
if [ "$rc" -ne 0 ] && [ "$runs" -eq 1 ]; then r=0; else r=1; fi; check "tart run: an unrelated error is not retried" $r

if [ "$fails" -ne 0 ]; then echo "$fails FAILED"; exit 1; fi
echo "ALL PASSED"
