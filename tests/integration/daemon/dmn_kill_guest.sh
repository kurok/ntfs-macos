#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: I-DMN-14 (SIGKILL of the worker and of the daemon), I-DMN-15 (restart reconciliation,
# the daemon-level run) and I-DMN-16 (force-detach of the image during a write) with the real
# daemon, probe, volmark, sandbox and worker in a macOS guest. Usage: dmn_kill_guest.sh DIR.
#
# After each failure: no worker, no mount left, the journal entry and the on-disk marker stay
# (silence is never success), the next attach is read-only with a reason, the data that was
# synced before the failure is intact, and the image is not changed by the read-only session.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
# shellcheck source=dmn_lib.sh
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare

gone() { [ -z "$(row "$1")" ]; }
no_fuse_mounts() { [ "$(mount | grep -cF macfuse)" = 0 ]; }
# the dirty flag as the probe sees it: `ntfsmac status` of the volume shows the probe state
probe_state() { "$CLI" status "$1" 2>&1 | sed -n 's/^  state: //p' | tail -1; }
# mount_ro_check BSD IMG FILE EXPECTED: the next attach is read-only, with a reason, the synced
# file is intact, and a read-only session leaves the image as it was
next_attach_ro() {
	id=$1 img=$2 f=$3 want=$4
	h0=$(sha "$img")
	bsd=$(attach_image "$img")
	wait_for 120 settled_row "$bsd" || true
	sleep 2
	echo "-- $id next attach ($bsd): $(row "$bsd")"
	eq "$id next attach: state" "$(state_of "$bsd")" Mounted
	eq "$id next attach: mode" "$(mount_of "$bsd")" ro
	note "$id next attach: reason '$(reason_of "$bsd")'"
	check "$id next attach: the reason is unclean_session or dirty_flag_journal (the volume was left open by this Mac)" sh -c "case '$(reason_of "$bsd")' in unclean_session|dirty_flag_journal) exit 0 ;; esac; exit 1"
	mp=$(mpoint_of "$bsd")
	check "$id next attach: the file synced before the failure is intact" sh -c "[ \"\$(cat '$mp/$f' 2>/dev/null)\" = '$want' ]"
	check "$id next attach: the journal entry is still there" [ "$(journal_count)" = 1 ]
	"$CLI" unmount "$bsd" >/dev/null 2>&1
	wait_for 60 has_state "$bsd" Appeared || true
	detach_image "$bsd"
	eq "$id next attach: the read-only session left the image unchanged" "$(sha "$img")" "$h0"
}
rw_mount() { # rw_mount IMG: attach, wait, print the BSD name; the volume must be Mounted rw
	_bsd=$(attach_image "$1")
	wait_for 120 settled_row "$_bsd" || true
	sleep 1
	mount_diag "$_bsd" >&2
	echo "$_bsd"
}

echo "== start the job"
start_daemon

# ---------------------------------------------------------------- I-DMN-14 a: SIGKILL of the worker
echo "== I-DMN-14 a: SIGKILL of the worker during a rw session"
img=$(fresh_image clean-64m-4k)
bsd=$(rw_mount "$img")
eq "I-DMN-14 a: the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
mp=$(mpoint_of "$bsd")
durable_write "$mp/keep.txt" synced-before-the-kill
check "the journal holds the entry of the rw session" [ "$(journal_count)" = 1 ]
wp=$(worker_pids | head -1)
note "worker pid $wp, killed with SIGKILL"
sudo -n kill -9 "$wp"
check "the volume leaves Mounted within 60 s" wait_for 60 sh -c ". '$d/tests/integration/daemon/dmn_lib.sh' >/dev/null 2>&1; [ \"\$(state_of $bsd)\" != Mounted ]"
sleep 3
echo "-- after the kill: $(row "$bsd")"
check "no worker is left" [ "$(workers)" = 0 ]
check "the mount of the dead worker is gone from the mount table" wait_for 60 no_fuse_mounts
check "the mount point directory is gone" sh -c "[ ! -e '$mp' ]"
check_no_device_holder "I-DMN-14 a" "$bsd" 60
check "the journal entry stays (the session is not reported clean)" [ "$(journal_count)" = 1 ]
check "the daemon told the user that the session did not end cleanly" sh -c "sudo -n grep -q 'notification: ' $LOG"
logcat | grep -i "notification" | tail -3
detach_image "$bsd"
next_attach_ro "I-DMN-14 a" "$img" keep.txt synced-before-the-kill
rm -f "$img"
sudo -n sh -c "rm -f $JDIR/*" 2>/dev/null

# ---------------------------------------------------------------- I-DMN-14 b / I-DMN-15: SIGKILL of the daemon
echo "== I-DMN-14 b and I-DMN-15: SIGKILL of the daemon with a rw volume mounted, then the launchd restart"
img=$(fresh_image clean-64m-4k)
bsd=$(rw_mount "$img")
eq "I-DMN-14 b: the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
mp=$(mpoint_of "$bsd")
durable_write "$mp/keep.txt" synced-before-the-daemon-kill
# #9 (T-M7-12): under real macFUSE the entry records the mount: owner (f_owner) is the console
# user the worker runs as, then f_fsid and the source, both hex
cu=$(stat -f %u /dev/console)
[ "$cu" = 0 ] && cu=$(id -u)
mline=$(sudo -n sh -c "cat $JDIR/*" 2>/dev/null | grep '^mount ' | head -1)
note "journal mount line: '$mline' (console uid $cu)"
check "#9: the entry records the mount with owner = console uid $cu" sh -c "printf '%s\n' '$mline' | grep -Eq '^mount $cu [0-9A-F]{16} [0-9a-f]+\$'"
loglines=$(logcat | wc -l | tr -d ' ')
dp=$(daemon_pid)
oldw=$(worker_pids | tr '\n' ' ')
note "daemon pid $dp, killed with SIGKILL; worker pids: $oldw"
sudo -n kill -9 "$dp"
# launchd (KeepAlive, ThrottleInterval 10) starts it again
check "launchd starts the daemon again (new pid)" wait_for 90 sh -c ". '$d/tests/integration/daemon/dmn_lib.sh' >/dev/null 2>&1; p=\$(daemon_pid); [ -n \"\$p\" ] && [ \"\$p\" != '$dp' ]"
check "the control socket answers again" wait_for 60 sh -c "'$CLI' list >/dev/null 2>&1"
sleep 5
echo "-- after the restart: list"
list
echo "-- mount table"
mount | grep -F macfuse || echo "(no macfuse mount)"
echo "-- workers: $(worker_pids | tr '\n' ' ')"
# the restarted daemon mounts the disk again (read-only), with a worker of its own, so the
# assertion is about the workers of the dead daemon: none of them may be alive
none_alive() { for _p in $1; do kill -0 "$_p" 2>/dev/null && return 1; done; return 0; }
check "after the restart no worker of the dead daemon is alive (pids: ${oldw:-none})" wait_for 60 none_alive "$oldw"
mount_is_ro() { [ "$(mount_of "$1")" = ro ]; }
check "the restarted daemon holds no read/write mount: its volume is ro" wait_for 90 mount_is_ro "$bsd"
check "the journal entry is still there" [ "$(journal_count)" = 1 ]
rwleft=$(mount | grep -F macfuse | grep -vc read-only)
eq "no read/write macfuse mount of the image is left after the restart" "$rwleft" 0
# #9: the recorded mount was the earlier session's, so the restart force-unmounted it (and left
# nothing mounted with a warning)
newlog=$(logcat | tail -n +"$((loglines + 1))")
check "#9: reconcile force-unmounted the recorded mount of the earlier daemon" contains "$newlog" "restart: forced unmount of a mount of the earlier daemon: ok"
check "#9: reconcile left no writable mount mounted" sh -c "! printf '%s' \"\$1\" | grep -q 'left mounted; the entry stays'" _ "$newlog"
check "#9: doctor reports no mount left mounted at the restart" sh -c "'$CLI' doctor 2>&1 | grep -q 'No read/write volume was left mounted'"
echo "-- daemon log after the restart"
logcat | tail -25
# bring the system to a known state: stop the job, detach, start again
stop_daemon
check "after the stop no macfuse mount is left" wait_for 60 no_fuse_mounts
detach_image "$bsd"
start_daemon
next_attach_ro "I-DMN-14 b" "$img" keep.txt synced-before-the-daemon-kill
rm -f "$img"
sudo -n sh -c "rm -f $JDIR/*" 2>/dev/null

# ---------------------------------------------------------------- I-DMN-16: force-detach during a write
echo "== I-DMN-16: force-detach of the image during a write"
img=$(fresh_image clean-64m-4k)
bsd=$(rw_mount "$img")
eq "I-DMN-16: the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
mp=$(mpoint_of "$bsd")
durable_write "$mp/keep.txt" synced-before-the-detach
# One process rewrites a 10 MiB file from offset 0 until a write fails (the yank), so it is still
# writing however long the refused detach below takes (up to 60 s; a writer of fixed size, one dd of
# 37.5 MiB, ended first). As in tests/stress/kill/kill_guest.sh: the file is made first and opened once
# without O_CREAT, so nothing is created on the system volume under /Volumes after the mount is gone.
: >"$mp/big.bin" || failf "I-DMN-16: cannot create the writer's file"
perl -MFcntl -e 'sysopen(my $h, $ARGV[0], O_WRONLY) or exit 200; my $b = "\0" x 65536;
	for (;;) { defined sysseek($h, 0, 0) or exit(($! + 0) || 201);
		for (1 .. 160) { my $n = syswrite($h, $b); defined $n or exit(($! + 0) || 202);
			$n == 65536 or exit 203; } }' "$mp/big.bin" &
wpid=$!
sleep 1
note "writer pid $wpid is running; force-detach of /dev/$bsd now"
# `hdiutil detach -force` and `diskutil eject force` both answer "Resource busy" while the worker
# holds the device open, so the image is yanked the way an unplug would take it: the process that
# serves the virtual disk (diskimages-helper) is killed and the disk vanishes under the open files
tmo 60 hdiutil detach "/dev/$bsd" -force >/dev/null 2>&1
hrc=$?
eq "hdiutil detach -force is refused while the worker holds the device (rc 16, EBUSY)" "$hrc" 16
check "the writer is still running at the moment of the yank" kill -0 "$wpid"
yank_image "$img"
check "the yank (kill of the process that serves the image) worked" test $? -eq 0
check "the volume row goes away within 120 s" wait_for 120 gone "$bsd"
echo "-- after the force-detach: row '$(row "$bsd")'"
sleep 2
kill -9 "$wpid" >/dev/null 2>&1
check "no worker is left" wait_for 60 sh -c ". '$d/tests/integration/daemon/dmn_lib.sh' >/dev/null 2>&1; [ \"\$(workers)\" = 0 ]"
check "no macfuse mount is left" wait_for 60 no_fuse_mounts
check "the mount point directory is gone" wait_for 30 sh -c "[ ! -e '$mp' ]"
check "the journal entry stays" [ "$(journal_count)" = 1 ]
logcat | grep -i "notification" | tail -3
echo "-- ntfsfix -n of the image after the force-detach (read-only check)"
tmo 120 "$HOME/ntfsmac/deps/bin/ntfsfix" -n "$img" >"$work/nf.txt" 2>&1
nfrc=$?
sed -n '1,12p' "$work/nf.txt"
eq "ntfsfix -n exit status on the yanked image" "$nfrc" 0
fl=$("$work/bin/setvolflags" "$img" --show 2>&1)
check "the dirty flag is still set on the yanked image ($fl)" contains "$fl" "flags_after=0x0001"
next_attach_ro "I-DMN-16" "$img" keep.txt synced-before-the-detach
rm -f "$img"

echo "-- daemon log"
logcat | sed -n '1,100p'
echo "-- children log"
sudo -n tail -n 20 "$CLOG" 2>/dev/null || true
finish
