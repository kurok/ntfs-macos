#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M5-05: repeated forced terminations (S-KILL-*, criterion 4.4) in ONE macOS guest, with the real
# daemon, probe, volmark, sandbox and worker (installed by tests/integration/daemon/dmn_lib.sh).
# Usage: kill_guest.sh DIR     (DIR has include/, integration/, src/, tests/, bin/ and fixtures/)
#
# Environment:
#   KILL_REPS     repetitions per target (default 2; the full run is 20)
#   KILL_TARGETS  space separated: worker daemon module detach (default: all four)
#   KILL_STEP     deadline in seconds of one waiting step, measured on the wall clock (default 90); every wait and
#                 every command that can block (CLI, hdiutil, lsof, a read on a dead mount) has a deadline, so a
#                 hang is a FAIL
#   KILL_CONTROL  1 (default): after the targets, controls with injected damage (each must fail on exactly the
#                 intended check) and never-ending steps (S-KILL-05); 0 turns them off
#
# Output (parsed by tests/stress/kill/result.py): PASS:/FAIL:/NOTE: lines of the checks and
#   REP TARGET N PASS|FAIL SECONDS     one line per repetition
#   TARGET TARGET reps=N failed=M      per target summary
#   MEASURE module#N mmap=.. plain=.. clean_report=..   the unsynced-data measurement (not a check)
#   RUN PASS|FAIL DETAIL               failures outside the repetitions (daemon start, setup)
#   CONTROL NAME PASS|FAIL DETAIL      the injected controls
#
# After each forced termination: the journal entry is in place, the volume's dirty flag is set, no worker and no
# macFUSE mount is left, the daemon (or a restarted one) answers, the next attach is read-only with the documented
# reason, the file that was synced before the kill is intact, and the read-only session leaves the image unchanged.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
# shellcheck source=../../integration/daemon/dmn_lib.sh
. "$d/tests/integration/daemon/dmn_lib.sh"

reps=${KILL_REPS:-2}
targets=${KILL_TARGETS:-"worker daemon module detach"}
step=${KILL_STEP:-90}
inject=
control=${KILL_CONTROL:-1}
case $reps in '' | *[!0-9]* | 0) echo "kill_guest: KILL_REPS must be a positive number" >&2; exit 2 ;; esac
case $step in '' | *[!0-9]* | 0) echo "kill_guest: KILL_STEP must be a positive number" >&2; exit 2 ;; esac

prepare
cc -std=c17 -O2 -o "$work/bin/mmapdirty" "$d/tests/stress/kill/mmapdirty.c" || { echo "FAIL: mmapdirty did not build"; exit 2; }
# a disposable guest, but a leftover attached image or a stale job would decide the first repetition
detach_leftovers() {
	hdiutil info 2>/dev/null | awk '/^\/dev\/disk[0-9]+[[:space:]]/ { print $1 }' | while read -r _dev; do
		tmo 60 hdiutil detach "$_dev" -force >/dev/null 2>&1 || true
	done
}
detach_leftovers

# ---- the lib's failure counting, with a mode for the controls (a failure there is the expected result);
# the descriptions of the failures of a control are kept, so that it can be required to fail on one check only
ctl=0
ctl_fail=0
ctl_msgs=
failf() {
	if [ "$ctl" = 1 ]; then
		echo "CONTROL-FAIL: $*"
		ctl_fail=$((ctl_fail + 1))
		ctl_msgs="$ctl_msgs
$*"
	else
		echo "FAIL: $*"
		fail=$((fail + 1))
	fi
}
# ---- a wait on the wall clock: the lib's wait_for counts iterations, so a slow check would stretch it
wait_for() {
	_wt=$1
	shift
	_wend=$(($(date +%s) + _wt))
	while :; do
		if "$@" >/dev/null 2>&1; then return 0; fi
		[ "$(date +%s)" -ge "$_wend" ] && return 1
		sleep 1
	done
}
# ---- every command that can block gets a deadline (the lib's versions have none)
sget() { tmo 20 "$CLI" status "$1" 2>&1 | sed -n "s/^  $2: //p" | head -1; }
list() { tmo 20 "$CLI" list 2>&1; }
answers() { tmo 20 "$CLI" list >/dev/null 2>&1; }
# the CLI must answer: an empty answer is not "gone" and not "left Mounted"
gone() { answers && [ -z "$(row "$1")" ]; }
left_mounted() { _s=$(state_of "$1"); [ -n "$_s" ] && [ "$_s" != Mounted ]; }
no_volmark() { ! ps -axo comm= | grep -q ntfsmac-volmark; }
# the session has ended for the daemon: the volume is gone or in a state that is not transient, no volmark runs
settled_after() {
	answers || return 1
	no_volmark || return 1
	[ -z "$(row "$1")" ] && return 0
	_s=$(state_of "$1")
	[ -n "$_s" ] && ! transient "$_s"
}
no_fuse_mounts() { [ "$(mount | grep -cF macfuse)" = 0 ]; }
no_workers() { [ "$(workers)" = 0 ]; }
same_daemon() { [ "$(daemon_pid)" = "$1" ] && answers; }
nothing_left() { no_workers && no_fuse_mounts; }
none_alive() { for _p in $1; do kill -0 "$_p" 2>/dev/null && return 1; done; return 0; }
path_gone() { tmo 10 test ! -e "$1"; }
mount_is_ro() { [ "$(mount_of "$1")" = ro ]; }
new_daemon() { _p=$(daemon_pid); [ -n "$_p" ] && [ "$_p" != "$1" ]; }
attach_b() { # attach_b FILE: attach without the system's mount; prints the BSD name
	_o=$(tmo 120 hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$1" 2>&1) || { echo "hdiutil: $_o" >&2; return 1; }
	printf '%s\n' "$_o" | awk '/^\/dev\/disk/ && !done { sub("^/dev/", "", $1); print $1; done = 1 }'
}
flags_of() { "$work/bin/setvolflags" "$1" --show 2>&1; }
# the volume's dirty flag is set on the (detached) image
image_dirty() { contains "$(flags_of "$1")" "flags_after=0x0001"; }

# ---- the macFUSE module process.
# FSKit runs a macFUSE volume in the FS module extension (io.macfuse.app.fsmodule.macfuse-local.appex), a
# process of the console user that the system starts. There is one long-lived instance without a volume
# (it holds no device) and, per mounted volume, an instance that holds the volume's raw device
# (/dev/rdiskNsM, a synthetic disk FSKit made) and a unix socket to the worker. The instance that serves the
# mount is the module process holding a raw disk device.
# worker_of BSD: the worker that holds the disk (not any worker: a stale one must not stand in for it)
worker_of() {
	for _p in $(worker_pids); do
		tmo 15 sudo -n lsof -p "$_p" 2>/dev/null | grep -q " /dev/$1\$" && echo "$_p"
	done
}
module_all() { ps -axo pid=,comm= | awk '/fsmodule\.macfuse/ { print $1 }' | tr '\n' ' '; }
# module_of MOUNTPOINT: the module processes holding the raw device of the kernel mount at MOUNTPOINT
module_of() {
	_dev=$(mount | awk -v m="$1" 'index($0, " on " m " (") { print $1; exit }' | sed 's|^/dev/|/dev/r|')
	[ -n "$_dev" ] || return 0
	for _p in $(ps -axo pid=,comm= | awk '/fsmodule\.macfuse/ { print $1 }'); do
		tmo 15 sudo -n lsof -p "$_p" 2>/dev/null | grep -q " $_dev\$" && echo "$_p"
	done
}
# no module instance serves a volume any more (the one without a volume may stay), and there is at most one other
modules_idle() {
	for _p in $(ps -axo pid=,comm= | awk '/fsmodule\.macfuse/ { print $1 }'); do
		tmo 15 sudo -n lsof -p "$_p" 2>/dev/null | grep -q ' /dev/rdisk' && return 1
	done
	[ "$(module_all | wc -w | tr -d ' ')" -le 1 ]
}
rw_up() { # rw_up IMG: attach, wait for the end state; prints the BSD name
	_bsd=$(attach_b "$1") || return 1
	wait_for "$step" settled_row "$_bsd" || true
	sleep 1
	mount_diag "$_bsd" >&2
	echo "$_bsd"
}
hide_journal() { mkdir -p "$work/jsave" && sudo -n sh -c "mv $JDIR/* $work/jsave/" 2>/dev/null; }
restore_journal() { sudo -n sh -c "mv $work/jsave/* $JDIR/" 2>/dev/null; }

# ---- the unsynced-data measurement of S-KILL-03 (a NOTE, not a check yet)
# before the kill: a page of mm.bin is dirtied through a shared mapping (no msync) and plain.txt is written with
# write(2) and no fsync. measure_read MP reads both back after the kill and says whether the session was reported clean.
measure_setup() {
	_mp=$1
	tmo 30 perl -MIO::Handle -e 'open(my $h, ">", $ARGV[0]) or die; print $h "A" x 4096; $h->flush; $h->sync or die; close $h' "$_mp/mm.bin" || return 1
	"$work/bin/mmapdirty" "$_mp/mm.bin" >"$work/mm.out" 2>&1 &
	mmpid=$!
	wait_for 15 grep -q dirty "$work/mm.out" || { echo "NOTE: the mapping helper did not report a dirty page"; return 1; }
	printf 'plainwrite\n' >"$_mp/plain.txt"
}
measure_read() {
	_mp=$1
	_m=$(tmo 20 head -c 1 "$_mp/mm.bin" 2>/dev/null)
	_p=$(tmo 20 cat "$_mp/plain.txt" 2>/dev/null)
	case $_m in B) _ms=survived ;; A) _ms=lost ;; *) _ms=unreadable ;; esac
	[ "$_p" = plainwrite ] && _ps=survived || _ps=lost
	echo "MEASURE $id mmap=$_ms plain=$_ps clean_report=$meas_clean"
	note "$id unsynced data after the module kill: dirty mmap page (no msync) $_ms, plain write() without fsync $_ps; session reported clean: $meas_clean"
}

# attach_ro ID IMG FILE WANT [HOOK]: the next attach is read-only with a documented reason, the file synced before
# the failure is intact, and the read-only session leaves the image as it was
attach_ro() {
	id=$1 img=$2 f=$3 want=$4 hook=${5:-}
	h0=$(sha "$img")
	bsd2=$(attach_b "$img") || { failf "$id next attach: hdiutil attach failed"; return 1; }
	wait_for "$step" settled_row "$bsd2" || true
	sleep 2
	echo "-- $id next attach ($bsd2): $(row "$bsd2")"
	eq "$id next attach: state" "$(state_of "$bsd2")" Mounted
	eq "$id next attach: mode" "$(mount_of "$bsd2")" ro
	rs=$(reason_of "$bsd2")
	note "$id next attach: reason '$rs'"
	check "$id next attach: the reason is unclean_session or dirty_flag_journal (the volume was left open by this Mac)" sh -c "case '$rs' in unclean_session|dirty_flag_journal) exit 0 ;; esac; exit 1"
	mp2=$(mpoint_of "$bsd2")
	got=$(tmo 20 cat "$mp2/$f" 2>/dev/null)
	eq "$id next attach: the file synced before the failure is intact" "$got" "$want"
	[ -z "$hook" ] || "$hook" "$mp2"
	check "$id next attach: the journal entry is still there" [ "$(journal_count)" = 1 ]
	tmo 60 "$CLI" unmount "$bsd2" >/dev/null 2>&1
	wait_for 60 has_state "$bsd2" Appeared || true
	detach_image "$bsd2"
	if [ "$inject" = image ] && [ "$ctl" = 1 ]; then printf x >>"$img"; fi
	eq "$id next attach: the read-only session left the image unchanged" "$(sha "$img")" "$h0"
}

# module_outcome ID IMG BSD FILE WANT: what the kill of the module process left. Required (the owner's decision:
# a module death is not a clean unmount): the journal entry is kept, the dirty marker is set, the next attach is
# read-only with the documented reason. The session being closed as clean (journal entry removed by the
# clean-session path) is a FAIL: "module death reported as a clean session". The next attach is still made, to
# read the unsynced-data measurement and to leave the guest in a known state.
module_outcome() {
	id=$1 img=$2 bsd=$3 f=$4 want=$5
	jc=$(journal_count)
	if [ "$jc" = 1 ]; then meas_clean=no; else meas_clean=yes; fi
	detach_image "$bsd"
	fl=$(flags_of "$img")
	if [ "$jc" = 1 ]; then
		note "$id outcome: the journal entry was kept (flags: $fl)"
		check "$id: the dirty flag is set on the image ($fl)" contains "$fl" "flags_after=0x0001"
		attach_ro "$id" "$img" "$f" "$want" measure_read
		return 0
	fi
	failf "$id: module death reported as a clean session: the journal entry is gone (removed by the clean-session path: $(clean_removals) removal(s) in the log, now $(journal_count) entries), the volume was closed as clean (flags: $fl)"
	bsd2=$(attach_b "$img") || { failf "$id next attach: hdiutil attach failed"; return 1; }
	wait_for "$step" settled_row "$bsd2" || true
	sleep 2
	echo "-- $id next attach ($bsd2): $(row "$bsd2")"
	have=$(tmo 20 cat "$(mpoint_of "$bsd2")/$f" 2>/dev/null)
	eq "$id next attach: the file synced before the kill is intact" "$have" "$want"
	measure_read "$(mpoint_of "$bsd2")"
	tmo 60 "$CLI" unmount "$bsd2" >/dev/null 2>&1
	wait_for 60 has_state "$bsd2" Appeared || true
	detach_image "$bsd2"
}
clean_removals() { logcat | grep -c 'removed by path 1 (clean-session)'; }

# rep TARGET N: one repetition. Prints the checks; the caller counts them.
rep() {
	tgt=$1 n=$2 id="$1#$2"
	wpid= mmpid= img=
	reset_journal
	dp0=$(daemon_pid)
	if [ -z "$dp0" ] || ! answers; then
		failf "$id: the daemon is not running or does not answer at the start of the repetition"
		start_daemon
		dp0=$(daemon_pid)
	fi
	img=$(fresh_image clean-64m-4k) || { failf "$id: no image"; return 1; }
	bsd=$(rw_up "$img") || { failf "$id: attach failed"; return 1; }
	eq "$id: the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
	mp=$(mpoint_of "$bsd")
	tok="synced-before-$tgt-$n-$$"
	tmo 60 perl -MIO::Handle -e 'my ($f,$t)=@ARGV; open(my $h,">",$f) or die; print $h "$t\n"; $h->flush; $h->sync or die; close $h;
		(my $dir=$f)=~s{/[^/]*$}{}; open(my $dh,"<",$dir) or die; $dh->sync or die' "$mp/keep.txt" "$tok" ||
		failf "$id: durable write failed"
	check "$id: the journal holds the entry of the rw session" [ "$(journal_count)" = 1 ]
	oldw=$(worker_pids | tr '\n' ' ')
	wp=$(worker_of "$bsd" | head -1)
	[ -n "$wp" ] || failf "$id: no worker holds /dev/$bsd"
	case $tgt in
	worker)
		note "$id: worker pid $wp, SIGKILL"
		sudo -n kill -9 "$wp"
		;;
	module)
		mods=$(module_of "$mp")
		eq "$id: exactly one macFUSE module process holds the volume's raw device" "$(echo "$mods" | wc -w | tr -d ' ')" 1
		mod=$(echo "$mods" | head -1)
		note "$id: module instances before: $(module_all); the one holding a raw disk: ${mod:-none}"
		measure_setup "$mp" || note "$id: the unsynced-data setup failed; the measurement is incomplete"
		[ -z "$mod" ] || sudo -n kill -9 "$mod"
		;;
	detach)
		( dd if=/dev/zero of="$mp/big.bin" bs=64k count=600 2>/dev/null ) &
		wpid=$!
		sleep 1
		check "$id: the writer is still writing at the moment of the yank" kill -0 "$wpid"
		note "$id: writer pid $wpid is running; the image is yanked (the process that serves it is killed)"
		yank_image "$img"
		check "$id: the yank worked" test $? -eq 0
		;;
	daemon)
		note "$id: daemon pid $dp0, SIGKILL; worker pids: $oldw"
		sudo -n kill -9 "$dp0"
		;;
	esac
	case $tgt in
	daemon)
		check "$id: launchd starts the daemon again (new pid) within ${step} s" wait_for "$step" new_daemon "$dp0"
		check "$id: the control socket answers again" wait_for "$step" answers
		sleep 5
		check "$id: no worker of the dead daemon is alive (pids: ${oldw:-none})" wait_for "$step" none_alive "$oldw"
		check "$id: the restarted daemon holds no read/write mount: its volume is ro" wait_for "$step" mount_is_ro "$bsd"
		eq "$id: no read/write macfuse mount of the image is left" "$(mount | grep -F macfuse | grep -vc read-only)" 0
		;;
	*)
		if [ "$tgt" = detach ]; then
			check "$id: the volume row goes away within ${step} s" wait_for "$step" gone "$bsd"
			kill -9 "$wpid" >/dev/null 2>&1
		else
			check "$id: the volume leaves Mounted within ${step} s" wait_for "$step" left_mounted "$bsd"
		fi
		check "$id: the session is settled for the daemon (volume gone or not transient, no volmark running) within ${step} s" wait_for "$step" settled_after "$bsd"
		echo "-- $id after the kill: row '$(row "$bsd")'; modules: $(module_all)"
		check "$id: no worker is left within ${step} s" wait_for "$step" no_workers
		check "$id: no macfuse mount is left within ${step} s" wait_for "$step" no_fuse_mounts
		check "$id: the mount point directory is gone" wait_for 30 path_gone "$mp"
		check "$id: the daemon is still the same process and answers" same_daemon "$dp0"
		;;
	esac
	if [ "$tgt" = module ]; then
		module_outcome "$id" "$img" "$bsd" keep.txt "$tok"
		return 0
	fi
	[ "$tgt" != worker ] || check "$id: the daemon told the user that the session did not end cleanly" wait_for 30 sh -c "sudo -n grep -q 'did not end its session cleanly' $LOG"
	if [ "$inject" = journal ] && [ "$ctl" = 1 ]; then hide_journal; fi
	check "$id: the journal entry stays (the session is not reported clean)" [ "$(journal_count)" = 1 ]
	if [ "$inject" = journal ] && [ "$ctl" = 1 ]; then restore_journal; fi
	# bring the system to a known state, then the next attach
	if [ "$tgt" = daemon ]; then
		stop_daemon
		check "$id: after the stop of the job no macfuse mount is left" wait_for "$step" no_fuse_mounts
		detach_image "$bsd"
		start_daemon
	else
		detach_image "$bsd"
	fi
	if [ "$inject" = flags ] && [ "$ctl" = 1 ]; then "$work/bin/setvolflags" "$img" --clear DIRTY >/dev/null 2>&1; fi
	check "$id: the dirty flag is still set on the image after the kill ($(flags_of "$img"))" image_dirty "$img"
	if [ "$inject" = flags ] && [ "$ctl" = 1 ]; then "$work/bin/setvolflags" "$img" --set DIRTY >/dev/null 2>&1; fi
	attach_ro "$id" "$img" keep.txt "$tok"
}

# rep_cleanup: whatever the repetition left, also after an early return
rep_cleanup() {
	[ -z "${wpid:-}" ] || kill -9 "$wpid" >/dev/null 2>&1
	[ -z "${mmpid:-}" ] || kill -9 "$mmpid" >/dev/null 2>&1
	wpid= mmpid=
	[ -z "${img:-}" ] || rm -f "$img"
	detach_all
	check "$id: nothing is left behind after the repetition (no worker, no macfuse mount)" wait_for 30 nothing_left
	check "$id: no module instance serves a volume any more, at most one instance remains (now: $(module_all))" wait_for "$step" modules_idle
	if ! answers; then
		failf "$id: the daemon does not answer after the repetition"
		start_daemon
	fi
}
do_rep() { rep "$1" "$2"; rep_cleanup; }

inrep_fail=0
# run_target TARGET N: N repetitions, a line per repetition and one for the target
run_target() {
	_t=$1 _n=$2 _bad=0 _i=1
	echo "== target $_t: $_n repetition(s)"
	while [ "$_i" -le "$_n" ]; do
		_f0=$fail
		_s0=$(date +%s)
		echo "== $_t repetition $_i/$_n"
		do_rep "$_t" "$_i"
		_secs=$(($(date +%s) - _s0))
		inrep_fail=$((inrep_fail + fail - _f0))
		if [ "$fail" -eq "$_f0" ]; then
			echo "REP $_t $_i PASS $_secs"
		else
			echo "REP $_t $_i FAIL $_secs"
			_bad=$((_bad + 1))
			echo "-- diagnostics of the failed repetition: daemon log"
			logcat | tail -30
			sudo -n tail -n 20 "$CLOG" 2>/dev/null || true
			mount | grep -v "apfs\|devfs\|^map "
			ps -axo pid,ppid,etime,stat,args | grep -E "ntfsmac-worker|fsmodule.macfuse|ntfsmacd" | grep -v grep | cut -c1-200
		fi
		_i=$((_i + 1))
	done
	echo "TARGET $_t reps=$_n failed=$_bad"
}

echo "META guest_os=macOS $(sw_vers -productVersion) $(uname -m)"
echo "META macfuse=$(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null || echo)"
echo "META ntfs3g=$("$INSTALL/ntfsmac-worker" --version 2>&1 | sed -n 's/^.*ntfs-3g \([0-9][0-9.]*\).*/\1/p;' | head -1)"
echo "== start the job"
start_daemon
echo "kill_guest: reps $reps, step deadline ${step} s (wall clock), targets: $targets, controls: $control"
echo "kill_guest: macFUSE module instances at the start: $(module_all)"

for t in $targets; do
	case $t in
	worker | daemon | module | detach) run_target "$t" "$reps" ;;
	*) echo "kill_guest: unknown target $t" >&2; exit 2 ;;
	esac
done
outside=$((fail - inrep_fail))
if [ "$outside" -eq 0 ]; then echo "RUN PASS no failure outside the repetitions"; else echo "RUN FAIL $outside failure(s) outside the repetitions (daemon start, setup)"; fi

# ---- S-KILL-05: the controls. A check that cannot fail proves nothing: damage the state on purpose and see
# that the check that is meant to catch it, and no other, reports it; and that a step that never ends, or a check
# that is slow, is a FAIL after its deadline in seconds.
# control KIND TARGET REQUIRED: one repetition with the damage KIND; every failure must name REQUIRED
control_rep() {
	kind=$1 tg=$2 req=$3
	echo "== control (inject $kind, target $tg): every failing check must be '$req'"
	inject=$kind ctl=1 ctl_fail=0 ctl_msgs=
	do_rep "$tg" 0
	ctl=0 inject=
	_other=$(printf '%s\n' "$ctl_msgs" | sed '/^$/d' | grep -vF "$req" | head -3 | tr '\n' ';')
	if [ "$ctl_fail" -ge 1 ] && [ -z "$_other" ]; then
		echo "CONTROL inject-$kind PASS $ctl_fail failing check(s), all '$req'"
	elif [ "$ctl_fail" -eq 0 ]; then
		echo "CONTROL inject-$kind FAIL the injected damage was not reported by any check"
		fail=$((fail + 1))
	else
		echo "CONTROL inject-$kind FAIL other checks failed too: $_other"
		fail=$((fail + 1))
	fi
}
if [ "$control" = 1 ]; then
	control_rep journal worker "the journal entry stays"
	control_rep image worker "the read-only session left the image unchanged"
	control_rep flags worker "the dirty flag is still set on the image"
	echo "== control: steps that do not end, and a slow check"
	slow_false() { sleep 4; return 1; }
	ctl=1 ctl_fail=0 ctl_msgs=
	h0=$(date +%s)
	check "control: a command that never returns" tmo 3 sleep 60
	check "control: a wait that never succeeds" wait_for 3 false
	w0=$(date +%s)
	check "control: a wait whose check takes 4 s and never succeeds, deadline 6 s" wait_for 6 slow_false
	w1=$(($(date +%s) - w0))
	ctl=0
	h1=$(($(date +%s) - h0))
	# 6 s deadline, the last check may overrun it by its own 4 s; the iteration-counting wait would need 30 s
	if [ "$ctl_fail" -eq 3 ] && [ "$h1" -le 25 ] && [ "$w1" -le 12 ]; then
		echo "CONTROL hang PASS three never-ending steps failed after their deadline (${h1} s in total, the slow wait ${w1} s for a 6 s deadline)"
	else
		echo "CONTROL hang FAIL never-ending steps: $ctl_fail failure(s) in ${h1} s, the slow wait took ${w1} s"
		fail=$((fail + 1))
	fi
fi

echo "-- daemon log"
logcat | sed -n '1,60p'
finish
