#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# S-SOAK-01 and S-PAR-01, guest part (T-M5-02): mixed workloads on a volume that the real daemon mounted
# read-write (dmn_lib.sh: the daemon, probe, volmark, sandbox and worker as installed in the guest).
# Usage: mix_guest.sh KIND DIR OUT     KIND soak|par; DIR: the tree dmn_vm.sh copied (tests/integration/daemon,
#                                      fixtures/, bin/) plus tests/stress/soak; OUT: where the evidence goes.
# Env: MIX_DURATION (seconds of the real run), MIX_BASELINE (soak: the baseline point of the RSS rule),
#      MIX_VERIFY (seconds between full verifications), MIX_RSS_EVERY (soak: RSS sample interval),
#      MIX_WORKERS (par: writer processes, default 16), MIX_HANG (hang limit in seconds, default 60),
#      MIX_CONTROLS (list of "corrupt stall": the negative controls to run, short runs on their own mount;
#      empty: none), MIX_FAULT (corrupt|stall: a fault injected into the REAL run, to show that the suite FAILs).
#
# Per run (one image file copy of clean-64m-4k): attach, the daemon mounts it rw, mixlib.py runs the workload
# (every byte compared with its model, periodic and final full verification, worker RSS from `ps`, hang
# watchdog), then the volume is unmounted through the daemon, `ntfsfix -n` must exit 0, the image is attached
# again (the daemon mounts it rw: the session was clean), and `mixlib.py verify` compares the whole tree with the
# manifests the workers wrote (every file's size and SHA-256, no extra and no missing entry).
# Controls: "corrupt" flips a byte on the volume behind the model's back, "stall" sleeps inside one operation
# longer than the hang limit; the same checks must FAIL, and only on that reason (judged on the host).
# Output: OUT/runs.tsv (name, role, driver rc, verify rc, ntfsfix rc before and after the remount, mount mode
# before and after), OUT/NAME/{summary.json,run.log,rss.tsv,worker-NN.json,man-NN.tsv,remount.json,verify.log}, OUT/meta.txt.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
kind=${1:?kind soak|par}
d=${2:?directory}
out=${3:?output directory}
case $kind in soak | par) ;; *) echo "$0: bad kind $kind" >&2; exit 2 ;; esac
dur=${MIX_DURATION:-180}
base=${MIX_BASELINE:-60}
verify=${MIX_VERIFY:-30}
rss=${MIX_RSS_EVERY:-5}
nw=${MIX_WORKERS:-16}
hang=${MIX_HANG:-60}
controls=${MIX_CONTROLS-}
fault=${MIX_FAULT:-}
[ "$kind" = soak ] && nw=1
here=$d/tests/stress/soak
mkdir -p "$out"
# shellcheck source=../../integration/daemon/dmn_lib.sh
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare
t0=$(date +%s)
: >"$out/runs.tsv"

attach_image() {
	_o=$(tmo 120 hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$1" 2>&1) || { echo "hdiutil: $_o" >&2; return 1; }
	printf '%s\n' "$_o" | awk '/^\/dev\/disk/ && !done { sub("^/dev/", "", $1); print $1; done = 1 }'
}
rw_mount() { # rw_mount IMG: attach, wait for the daemon, print the BSD name
	_bsd=$(attach_image "$1")
	wait_for 120 settled_row "$_bsd" || true
	sleep 1
	mount_diag "$_bsd" >&2
	echo "$_bsd"
}
# unmount_diag BSD MP: what the evidence needs when a volume stays in Unmounting (called once per volume): samples
# of the daemon and of the worker, the mount table, the volume list, the daemon's children, the holders of the
# device and the daemon log. The raw output goes to $out/stuck-BSD/, each file is summarised as NOTE lines.
# POSIX sh has no local variables: every variable here has the prefix _ud_.
unmount_diag() {
	_ud_dir=$out/stuck-$1
	mkdir -p "$_ud_dir"
	note "$1 stays Unmounting for over 20 s: capturing the daemon's and the worker's state into $_ud_dir"
	_ud_dp=$(pgrep -x ntfsmacd 2>/dev/null | head -1)
	[ -n "$_ud_dp" ] && tmo 30 sudo -n sample "$_ud_dp" 3 >"$_ud_dir/sample-daemon.txt" 2>&1
	for _ud_wp in $(worker_pids); do
		tmo 30 sudo -n sample "$_ud_wp" 3 >"$_ud_dir/sample-worker-$_ud_wp.txt" 2>&1
	done
	mount >"$_ud_dir/mount.txt" 2>&1
	tmo 20 "$CLI" list >"$_ud_dir/list.txt" 2>&1
	ps -axo pid,ppid,stat,etime,wchan,args 2>/dev/null | awk 'NR == 1 || /ntfsmac/' >"$_ud_dir/ps.txt"
	tmo 20 sudo -n lsof -nP "/dev/$1" "/dev/r$1" >"$_ud_dir/lsof.txt" 2>&1
	logcat | tail -n 80 >"$_ud_dir/daemon-log-tail.txt"
	for _ud_f in "$_ud_dir"/*; do
		note "$1 $(basename "$_ud_f"): $(wc -l <"$_ud_f" | tr -d ' ') lines"
		case $_ud_f in
		*/sample-*) sed -n '/Call graph:/,/Total number in stack/p' "$_ud_f" | head -25 | sed 's/^/NOTE:   /' ;;
		*/mount.txt) grep -i -e macfuse -e ntfs ${2:+-e "$2"} "$_ud_f" | head -5 | sed 's/^/NOTE:   /' ;;
		*/list.txt | */ps.txt | */lsof.txt) head -10 "$_ud_f" | sed 's/^/NOTE:   /' ;;
		*/daemon-log-tail.txt) tail -n 12 "$_ud_f" | sed 's/^/NOTE:   /' ;;
		esac
	done
}
# clean_unmount BSD: through the daemon, then detach; true when the volume left Mounted.
# The daemon asks the kernel again after EBUSY 4 times, 0.3 s apart (about 1.2 s, bound 6 s). If the holders are
# all Spotlight or Quick Look, the volume stays Unmounting and the daemon asks again every 2 s for up to 60 s
# (ADR-028, proposed); with any other holder, or after that time, it logs UNMOUNT_BUSY and the volume returns to
# Mounted, as for a user's eject. Right after a heavy workload something can hold the volume for longer (a 30 min
# run with 16 writers saw it once), so a refused unmount is asked again every 5 s, within a 150 s bound; each
# refusal is noted with what holds files on the volume.
# A volume in Unmounting for over 20 s is captured once by unmount_diag. Variables: prefix _cu_ (no locals in sh).
clean_unmount() {
	_cu_mp=$(mpoint_of "$1")
	_cu_end=$(($(date +%s) + 150))
	_cu_n=0
	_cu_ask=$(date +%s)
	_cu_um=0
	_cu_diag=0
	tmo 180 "$CLI" unmount "$1" >/dev/null 2>&1
	while ! has_state "$1" Appeared && ! has_state "$1" Failed && ! has_state "$1" Gone && [ "$(date +%s)" -lt "$_cu_end" ]; do
		if has_state "$1" Unmounting; then
			[ "$_cu_um" -eq 0 ] && _cu_um=$(date +%s)
			if [ "$_cu_diag" -eq 0 ] && [ $(($(date +%s) - _cu_um)) -ge 20 ]; then
				_cu_diag=1
				unmount_diag "$1" "$_cu_mp"
			fi
		else
			_cu_um=0
		fi
		if has_state "$1" Mounted && [ $(($(date +%s) - _cu_ask)) -ge 5 ]; then
			_cu_n=$((_cu_n + 1))
			note "unmount of $1 refused (still Mounted, busy), asking again ($_cu_n); files open on ${_cu_mp:-the volume}:"
			[ -n "$_cu_mp" ] && tmo 20 sudo -n lsof -n +f -- "$_cu_mp" 2>/dev/null | head -20 | sed 's/^/NOTE:   /'
			# holders without a file descriptor (Spotlight indexing, Quick Look, fseventsd) do not show in lsof
			pgrep -lf 'mds|mdworker|fseventsd|QuickLook|quicklook' 2>/dev/null | head -10 | sed 's/^/NOTE:   pgrep: /'
			_cu_ask=$(date +%s)
			tmo 180 "$CLI" unmount "$1" >/dev/null 2>&1
		fi
		sleep 1
	done
	has_state "$1" Appeared
	_cu_r=$?
	# also on success, so the evidence shows when unmounts start needing re-asks routinely
	[ "$_cu_n" -gt 0 ] && note "unmount of $1 needed $_cu_n re-ask(s)"
	_detach_raw "$1"
	return $_cu_r
}
json_get() { # json_get FILE KEY: top-level scalar of summary.json, empty when absent or null
	python3 -B -c 'import json,sys; v=json.load(open(sys.argv[1])).get(sys.argv[2]); print("" if v is None else v)' "$1" "$2" 2>/dev/null
}

# cycle NAME ROLE DURATION WORKERS HANG BASELINE INJECT(corrupt|stall|)
cycle() {
	nm=$1 role=$2 cdur=$3 cnw=$4 chang=$5 cbase=$6 inj=$7
	echo "== $nm ($role): $kind, $cdur s, $cnw worker(s), hang limit $chang s${inj:+, injected: $inj}"
	img=$work/$nm.img
	cp "$d/fixtures/clean-64m-4k.img" "$img" || { failf "$nm the image copy"; return 0; }
	st=$out/$nm
	mkdir -p "$st"
	bsd=$(rw_mount "$img")
	mode1="$(state_of "$bsd") $(mount_of "$bsd")"
	eq "$nm the volume mounts rw" "$mode1" "Mounted rw"
	mp=$(mpoint_of "$bsd")
	# separate words: a mount point can contain a space ("/Volumes/clean64m4k 2" after a stale mount)
	set -- --kind "$kind" --mp "$mp" --state "$st" --duration "$cdur" --workers "$cnw" --verify-every "$verify" --hang-limit "$chang"
	# the working set of the models stays far below the 64 MiB volume: 16 MiB (soak) or 16 workers of 1.5 MiB (par)
	if [ "$kind" = soak ]; then
		set -- "$@" --cap 16777216 --filecap 1048576
	else
		set -- "$@" --cap 1572864 --filecap 524288
	fi
	if [ "$kind" = soak ]; then
		check "$nm exactly one worker holds the device" [ "$(workers)" = 1 ]
		wp=$(worker_pids | head -1)
		set -- "$@" --baseline "$cbase" --rss-pid "${wp:-0}" --rss-every "$rss"
		note "$nm worker pid ${wp:-none}, RSS $(ps -o rss= -p "${wp:-0}" 2>/dev/null | tr -d ' ') kB at the start"
	fi
	case $inj in
	corrupt) set -- "$@" --inject-corrupt ;;
	stall) set -- "$@" --inject-stall "1:3:$((chang + 4))" ;;
	esac
	tmo $((cdur + 900)) python3 -B "$here/mixlib.py" run "$@" >"$st/run.log" 2>&1
	drc=$?
	# a driver killed by the deadline must not leave workers writing to the volume (they also leave on their own when
	# the parent is gone, and the driver's process group is killed on TERM and ALRM; this is the last resort)
	pkill -9 -f "$here/mixlib.py run" >/dev/null 2>&1
	cat "$st/run.log"
	vrc=NA nf1=NA nf2=NA mode2=NA
	if [ -f "$st/summary.json" ] && [ -z "$(json_get "$st/summary.json" hang)" ]; then
		sync
		if clean_unmount "$bsd"; then
			pass "$nm the volume unmounts cleanly through the daemon"
		else
			# the disk is still attached and mounted: a "remount" would get the same session back and verify nothing
			# new, so ntfsfix, the remount and the verification stay NA (runs.tsv), which fails the run on the host
			failf "$nm the volume unmounts cleanly through the daemon"
			note "$nm no ntfsfix and no remount verification: the volume did not leave Mounted"
			# take it down now (the FAIL is recorded, the holders are noted above), so the next cycle does not meet
			# a second mount of the same serial and label, and the image is detached before it is removed
			tmo 60 "$CLI" unmount --force "$bsd" >/dev/null 2>&1
			wait_for 60 has_state "$bsd" Appeared || true
			_detach_raw "$bsd" || true
			note "$nm forced unmount after the failed check: state '$(state_of "$bsd")'"
			printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$nm" "$role" "$drc" "$vrc" "$nf1" "$nf2" "$mode1" "$mode2" >>"$out/runs.tsv"
			rm -f "$img"
			reset_journal
			return 0
		fi
		tmo 120 "$HOME/ntfsmac/deps/bin/ntfsfix" -n "$img" >"$st/ntfsfix1.txt" 2>&1
		nf1=$?
		eq "$nm ntfsfix -n after the unmount" "$nf1" 0
		bsd2=$(rw_mount "$img")
		mode2="$(state_of "$bsd2") $(mount_of "$bsd2")"
		eq "$nm after the remount: state and mode" "$mode2" "Mounted rw"
		mp2=$(mpoint_of "$bsd2")
		tmo 900 python3 -B "$here/mixlib.py" verify --kind "$kind" --mp "$mp2" --state "$st" --out "$st/remount.json" >"$st/verify.log" 2>&1
		vrc=$?
		cat "$st/verify.log"
		clean_unmount "$bsd2" || note "$nm the volume did not leave Mounted after the verification"
		tmo 120 "$HOME/ntfsmac/deps/bin/ntfsfix" -n "$img" >"$st/ntfsfix2.txt" 2>&1
		nf2=$?
		eq "$nm ntfsfix -n after the verification session" "$nf2" 0
	else
		# a hang or a missing summary: the volume is taken down without further checks
		tmo 180 "$CLI" unmount "$bsd" >/dev/null 2>&1
		wait_for 60 has_state "$bsd" Appeared || true
		_detach_raw "$bsd"
		note "$nm no remount verification (hang or no summary)"
	fi
	printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$nm" "$role" "$drc" "$vrc" "$nf1" "$nf2" "$mode1" "$mode2" >>"$out/runs.tsv"
	rm -f "$img"
	reset_journal
}

echo "== start the job"
start_daemon
setup_fail=$fail
cycle "$kind-real" real "$dur" "$nw" "$hang" "$base" "$fault"
for c in $controls; do
	case $kind:$c in
	soak:corrupt) cycle "$kind-ctl-corrupt" control-corrupt 15 1 60 0 corrupt ;;
	par:corrupt) cycle "$kind-ctl-corrupt" control-corrupt 10 4 60 0 corrupt ;;
	par:stall) cycle "$kind-ctl-stall" control-stall 12 4 4 0 stall ;;
	*) note "unknown control $kind:$c" ;;
	esac
done
echo "== runs"
cat "$out/runs.tsv"
{
	echo "seconds=$(($(date +%s) - t0))"
	echo "setup_failed=$setup_fail"
	echo "checks_failed=$fail"
	echo "guest_os=macOS $(sw_vers -productVersion)"
	echo "ntfs3g=$("$HOME/ntfsmac/deps/bin/ntfs-3g" --version 2>&1 | sed -n 's/^.*ntfs-3g[^0-9]*\([0-9][0-9.]*\).*$/\1/p' | head -1)"
	echo "macfuse=$(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null)"
} >"$out/meta.txt"
pkill -9 -f "$here/mixlib.py" >/dev/null 2>&1
echo "-- daemon log (tail)"
logcat | tail -n 60 >"$out/daemon-log-tail.txt"
tail -n 15 "$out/daemon-log-tail.txt"
finish
