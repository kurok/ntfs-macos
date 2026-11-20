#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# S-DISC-*, guest part (T-M5-04): force-detach of the image during a write, repeated at varied
# points of four write workloads, with the real daemon, probe, volmark, sandbox and worker (the
# guest code of I-DMN-16, tests/integration/daemon/dmn_kill_guest.sh, through dmn_lib.sh).
# Usage: disc_guest.sh DIR OUT     DIR: the tree dmn_vm.sh copied (tests/integration/daemon, fixtures/,
#                                  bin/) plus tests/stress/disconnect; OUT: where the evidence goes.
# Env: DISC_FAULT (hash|dirty: a fault injected into every REAL repetition, to show that the suite FAILs),
#      DISC_REPS (yanks per workload, default 3), DISC_WORKLOADS (default "seq small churn fsync"),
#      DISC_CONTROLS (default "hash dirty": the negative controls; empty: none).
#
# The yank: `hdiutil detach -force` answers EBUSY while the worker holds the device (I-DMN-16 shows it),
# so the image is taken away the way an unplug takes it: the process that serves the virtual disk
# (diskimages-helper) is killed (yank_image of dmn_lib.sh) while the workload is writing.
#
# After every yank (the checks of docs/TEST_STRATEGY.md section 6): the volume leaves Mounted, no worker
# and no macfuse mount is left, the mount point is gone, the journal entry stays; `ntfsfix -n` exits 0
# and the dirty flag is set in the image; the next attach is read-only with the documented reason, every
# file recorded as synced (file and directory fsync) before the yank has its hash, the read-only session
# leaves the image hash unchanged.
#
# Controls (S-DISC-05): a real yank, then a fault is injected into the image (a byte of a synced file
# flipped / the dirty flag cleared); the same checks must FAIL on the matching line, otherwise the
# suite could not tell. Control failures do not count as failures of the run.
#
# Every wait and every command that can hang has a deadline (wait_for, tmo). A control passes only when the
# target check is the ONLY FAIL of its repetition and its line shows the fault (the BAD line of the hash verifier;
# flags_after=0x0000 for the dirty flag).
# Output: OUT/reps.tsv (case, rep, mode, delay, result, detail), OUT/rep-CASE-N.log, OUT/meta.txt.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
out=${2:?output directory}
reps=${DISC_REPS:-3}
workloads=${DISC_WORKLOADS:-seq small churn fsync}
controls=${DISC_CONTROLS-hash dirty}
here=$d/tests/stress/disconnect
mkdir -p "$out"
# shellcheck source=../../integration/daemon/dmn_lib.sh
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare
t0=$(date +%s)
: >"$out/reps.tsv"

gone() { [ -z "$(row "$1")" ]; }
no_fuse_mounts() { [ "$(mount | grep -cF macfuse)" = 0 ]; }
# attach_image FILE with a deadline (the lib's version has none): prints the BSD name
attach_image() {
	_o=$(tmo 120 hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$1" 2>&1) || { echo "hdiutil: $_o" >&2; return 1; }
	printf '%s\n' "$_o" | awk '/^\/dev\/disk/ && !done { sub("^/dev/", "", $1); print $1; done = 1 }'
}
# proc_gone PID: the process does not exist or is a zombie waiting to be reaped
proc_gone() { _s=$(ps -o stat= -p "$1" 2>/dev/null); case $_s in '' | Z*) return 0 ;; esac; return 1; }
rw_mount() { # rw_mount IMG: attach, wait, print the BSD name
	_bsd=$(attach_image "$1")
	wait_for 120 settled_row "$_bsd" || true
	sleep 1
	mount_diag "$_bsd" >&2
	echo "$_bsd"
}
case_of() { # workload -> case id
	case $1 in seq) echo S-DISC-01 ;; small) echo S-DISC-02 ;; churn) echo S-DISC-03 ;; fsync) echo S-DISC-04 ;; esac
}
# the point of the yank: seconds after the start of the workload, cycling through a set (the number
# of fsynced files already done for the fsync workload)
delay_of() { # workload rep
	_k=$2
	case $1 in
	fsync) set -- 1 3 6 2 9 4 ;;
	*) set -- 0.3 1 2.2 0.6 3.5 1.6 ;;
	esac
	shift $((_k % $#))
	echo "$1"
}
# flip one byte of the sentinel file's data in the image (a synced file: its place is found by its magic)
corrupt_sentinel() {
	perl -e '
		my $f = shift; open(my $h, "+<", $f) or die "open: $!"; binmode($h);
		my $pos = 0; my $buf = ""; my $found;
		while (read($h, $buf, 4194304 + 64)) {
			my $i = index($buf, "DISCSENTINEL01");
			if ($i >= 0) { $found = $pos + $i + 100; last }
			$pos += 4194304; seek($h, $pos, 0);
		}
		die "sentinel not found\n" unless defined $found;
		seek($h, $found, 0); read($h, my $b, 1); seek($h, $found, 0); print $h chr(ord($b) ^ 0x55); close($h);
	' "$1"
}

# one_rep CASE REP MODE INJECT: a full yank cycle; the checks print PASS: / FAIL: lines
one_rep() {
	id=$1 n=$2 mode=$3 inject=${4:-${DISC_FAULT:-}}
	delay=$(delay_of "$mode" "$n")
	echo "== $id rep $n: workload $mode, yank at $delay${inject:+, injected fault: $inject}"
	img=$work/$id.$n.img # one image file per repetition
	cp "$d/fixtures/clean-64m-4k.img" "$img" || { failf "$id.$n the image copy"; return 0; }
	h_fix=$(sha "$img")
	md=$work/marks.$id.$n
	rm -rf "$md"
	mkdir -p "$md"
	bsd=$(rw_mount "$img")
	eq "$id.$n the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
	mp=$(mpoint_of "$bsd")
	perl "$here/wl.pl" base "$mp" "$md" "$n" || failf "$id.$n baseline write"
	check "$id.$n the journal holds the entry of the rw session" [ "$(journal_count)" = 1 ]
	if [ "$mode" = fsync ]; then
		perl "$here/wl.pl" fsync "$mp" "$md" "$n" "$delay" >"$md/wl.out" 2>&1 &
	else
		perl "$here/wl.pl" "$mode" "$mp" "$md" "$n" >"$md/wl.out" 2>&1 &
	fi
	wpid=$!
	if [ "$mode" = fsync ]; then
		# the writer sits idle once the fsyncs of file K (= delay) have returned: the yank is right after them
		wait_for 60 test -e "$md/fs.$delay" || failf "$id.$n the fsync workload did not reach file $delay"
	else
		sleep "$delay"
	fi
	check "$id.$n the writer is running at the moment of the yank" kill -0 "$wpid"
	yank_image "$img"
	check "$id.$n the yank (kill of the process that serves the image) worked" test $? -eq 0
	: >"$md/go"
	prog=$(cat "$md/progress" 2>/dev/null || echo 0)
	nrec=$(ls "$md" | grep -c '^fs\.[0-9]*$')
	if [ "$mode" = fsync ]; then
		note "$id.$n point of the yank: right after the fsyncs of file $delay returned (writer idle), $nrec fsync records"
	else
		note "$id.$n point of the yank: $delay s into the workload, $prog operations done"
		check "$id.$n the workload had written something at the yank ($prog operations)" test "$prog" -gt 0
	fi
	check "$id.$n the volume row goes away within 120 s" wait_for 120 gone "$bsd"
	sleep 2
	kill -9 "$wpid" >/dev/null 2>&1
	check "$id.$n the writer is gone after the kill" wait_for 10 proc_gone "$wpid"
	wait "$wpid" 2>/dev/null
	check "$id.$n no worker is left" wait_for 60 sh -c ". '$d/tests/integration/daemon/dmn_lib.sh' >/dev/null 2>&1; [ \"\$(workers)\" = 0 ]"
	check "$id.$n no macfuse mount is left" wait_for 60 no_fuse_mounts
	check "$id.$n the mount point directory is gone" wait_for 30 sh -c "[ ! -e '$mp' ]"
	check "$id.$n the journal entry stays" [ "$(journal_count)" = 1 ]
	if [ "$inject" = hash ]; then corrupt_sentinel "$img" && note "injected: one byte of base/sentinel.bin flipped in the image"; fi
	if [ "$inject" = dirty ]; then "$work/bin/setvolflags" "$img" --clear DIRTY >/dev/null && note "injected: the dirty flag cleared in the image"; fi
	tmo 120 "$HOME/ntfsmac/deps/bin/ntfsfix" -n "$img" >"$work/nf.txt" 2>&1
	nfrc=$?
	[ "$nfrc" -eq 0 ] || note "$id.$n ntfsfix -n exit $nfrc: if the yank fell between the two halves of a mirrored metadata write this is a possible mirror_split point (TEST_STRATEGY section 6); classify it by hand, do not accept it silently"
	eq "$id.$n ntfsfix -n exit status on the yanked image" "$nfrc" 0
	fl=$("$work/bin/setvolflags" "$img" --show 2>&1)
	check "$id.$n the dirty flag is set on the yanked image ($fl)" contains "$fl" "flags_after=0x0001"
	# the next attach: read-only, with the documented reason; synced files intact; image unchanged
	h0=$(sha "$img")
	bsd2=$(attach_image "$img")
	wait_for 120 settled_row "$bsd2" || true
	sleep 2
	echo "-- $id.$n next attach ($bsd2): $(row "$bsd2")"
	eq "$id.$n next attach: state" "$(state_of "$bsd2")" Mounted
	eq "$id.$n next attach: mode" "$(mount_of "$bsd2")" ro
	r=$(reason_of "$bsd2")
	note "$id.$n next attach: reason '$r'"
	check "$id.$n next attach: the reason is unclean_session or dirty_flag_journal (the volume was left open by this Mac)" has_any "$r" unclean_session dirty_flag_journal
	mp2=$(mpoint_of "$bsd2")
	vout=$(tmo 120 perl "$here/wl.pl" verify "$mp2" "$md" 2>&1)
	vrc=$?
	echo "-- $id.$n synced files: $vout"
	check "$id.$n next attach: every file synced before the yank is intact by hash ($(echo "$vout" | head -1))" test "$vrc" -eq 0
	check "$id.$n next attach: the journal entry is still there" [ "$(journal_count)" = 1 ]
	tmo 60 "$CLI" unmount "$bsd2" >/dev/null 2>&1
	wait_for 60 has_state "$bsd2" Appeared || true
	_detach_raw "$bsd2"
	eq "$id.$n next attach: the read-only session left the image unchanged" "$(sha "$img")" "$h0"
	note "$id.$n image hash fixture $h_fix, after yank $h0"
	rm -f "$img"
	reset_journal
	rm -rf "$md"
}

real_fail=0
run_rep() {
	rid=$1 rn=$2 rmode=$3 kind=$4
	log=$out/rep-$rid-$rn.log
	f0=$fail
	one_rep "$rid" "$rn" "$rmode" "$kind" >"$log" 2>&1
	cat "$log"
	nf=$((fail - f0))
	nfl=$(grep -c '^FAIL:' "$log")
	dl=$(delay_of "$rmode" "$rn")
	det=$(grep '^FAIL:' "$log" | head -3 | tr '\t\n' '  ' | cut -c1-400)
	if [ -z "$kind" ]; then
		real_fail=$((real_fail + nf))
		st=pass
		[ "$nf" -eq 0 ] || st=fail
		[ "$st" = fail ] || det=$(grep -m1 'point of the yank' "$log" | cut -c1-200)
	else
		# a control passes only when the target check is the ONLY failure of the repetition and it shows the fault
		case $kind in
		hash) pat='every file synced before the yank is intact'; extra='BAD base/sentinel.bin: sha256' ;;
		dirty) pat='the dirty flag is set on the yanked image'; extra='flags_after=0x0000' ;;
		esac
		st=fail
		if [ "$nfl" -eq 1 ] && grep '^FAIL:' "$log" | grep -F "$pat" | grep -qF "$extra"; then
			st=pass
			det="the check caught the injected fault ($kind) and nothing else failed: $(grep '^FAIL:' "$log" | cut -c1-200)"
		else
			det="the check did NOT cleanly catch the injected fault ($kind): $nfl FAIL line(s), expected exactly one with '$extra': $det"
		fi
		fail=$f0 # a control's FAIL lines are expected
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\n' "$rid" "$rn" "$rmode" "$dl" "$st" "$det" >>"$out/reps.tsv"
	echo "REP $rid $rn $rmode delay=$dl $st"
}

echo "== start the job"
start_daemon
setup_fail=$fail # a failure of prepare or of the job start stays in the final status
for w in $workloads; do
	id=$(case_of "$w")
	n=1
	while [ "$n" -le "$reps" ]; do
		run_rep "$id" "$n" "$w" ""
		n=$((n + 1))
	done
done
cn=1
for c in $controls; do
	run_rep S-DISC-05 "$cn" fsync "$c"
	cn=$((cn + 1))
done
echo "== reps"
cat "$out/reps.tsv"
{
	echo "seconds=$(($(date +%s) - t0))"
	echo "reps=$reps"
	echo "setup_failed=$setup_fail"
	echo "guest_os=macOS $(sw_vers -productVersion)"
	echo "ntfs3g=$("$HOME/ntfsmac/deps/bin/ntfs-3g" --version 2>&1 | sed -n 's/^.*ntfs-3g[^0-9]*\([0-9][0-9.]*\).*$/\1/p' | head -1)"
	echo "macfuse=$(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null)"
} >"$out/meta.txt"
echo "-- daemon log (tail)"
logcat | tail -n 60 >"$out/daemon-log-tail.txt"
tail -n 15 "$out/daemon-log-tail.txt"
fail=$((real_fail + setup_fail))
finish
