#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: I-DMN-10..13 (the four unmount paths) and I-DMN-17 (passive mode) with the real daemon.
#   I-DMN-10  `ntfsmac unmount`
#   I-DMN-11  `diskutil unmount`
#   I-DMN-12  `umount` (plain)
#   I-DMN-13  Finder eject (AppleScript `eject disk`) needs a Finder session and the Automation
#             permission of the calling process, which a guest without a person does not give
#             (osascript waits for the permission dialog), so that part is recorded as not runnable
#             unattended and `tests/integration/finder/finder_guest.sh --guided` covers it. The call
#             Finder's eject button ends in, NSWorkspace unmountAndEjectDeviceAtPath, needs no
#             permission and is run here as the second part of I-DMN-13
#   I-DMN-17  a dummy third-party NTFS bundle (/Library/Filesystems/ufsd_NTFS.fs, an empty
#             directory in the disposable guest) is present: no dissent, no automount, and
#             `ntfsmac doctor` names the bundle
# Each unmount path ends with: the positive clean-session proof (the daemon says "ejected, all data
# written"), the journal entry gone, the mount point removed, the dirty flag clear on the image,
# and the data intact when the volume is mounted again. Usage: dmn_unmount_guest.sh DIR.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare

ejected_count() { _n=$(sudo -n grep -c 'ejected, all data written\.' "$LOG" 2>/dev/null); echo "${_n:-0}"; }
flags_of() { "$work/bin/setvolflags" "$1" --show 2>&1 | sed -n 's/.*flags_after=\(0x[0-9a-fA-F]*\).*/\1/p'; }

# unmount_case ID NAME TRIGGER [KNOWN]: TRIGGER is a shell fragment with $bsd and $leaf. KNOWN is a
# text: if the trigger fails, that is a known failure of the environment (reported as KNOWN-FAIL
# with that text, not counted), and the volume is then unmounted with `ntfsmac unmount`.
unmount_case() {
	id=$1 trig=$3 known=${4:-}
	img=$(fresh_image clean-64m-4k)
	bsd=$(attach_image "$img")
	wait_for 120 settled_row "$bsd" || true
	sleep 1
	mp=$(mpoint_of "$bsd")
	leaf=${mp#/Volumes/}
	mount_diag "$bsd"
	eq "$id: the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
	echo "unmount-path-$id" >"$mp/data.txt" && sync
	sum=$(shasum -a 256 "$mp/data.txt" | awk '{ print $1 }')
	check "$id: the journal holds the entry while the volume is rw" [ "$(journal_count)" = 1 ]
	n0=$(ejected_count)
	echo "-- $id: trigger: $trig"
	tout=$(eval "$trig" 2>&1)
	trc=$?
	echo "$tout"
	note "$id: trigger returned $trc"
	if [ "$trc" -ne 0 ] && [ -n "$known" ] && { [ "$trc" -ne 1 ] || ! contains "$tout" "failed to unmount"; }; then
		failf "$id: the trigger failed in a way that is not the known one (rc $trc, expected rc 1 with 'failed to unmount')"
	elif [ "$trc" -ne 0 ] && [ -n "$known" ]; then
		echo "KNOWN-FAIL: $id: $known"
		"$CLI" unmount "$bsd" >/dev/null 2>&1
		wait_for 90 has_state "$bsd" Appeared || true
		detach_image "$bsd"
		rm -f "$img"
		return
	fi
	check "$id: the volume returns to Appeared" wait_for 90 has_state "$bsd" Appeared
	check "$id: the mount point directory is gone" sh -c "[ ! -e '$mp' ]"
	check "$id: no macfuse mount is left" sh -c "[ \"\$(mount | grep -cF macfuse)\" = 0 ]"
	check "$id: the journal entry is gone (positive clean-session proof)" [ "$(journal_count)" = 0 ]
	check "$id: the daemon said 'ejected, all data written'" sh -c "[ \"$(ejected_count)\" -gt \"$n0\" ]"
	check "$id: no worker is left" [ "$(workers)" = 0 ]
	detach_image "$bsd"
	eq "$id: the dirty flag on the image is clear" "$(flags_of "$img")" 0x0000
	# the data is intact when the volume is mounted again
	bsd=$(attach_image "$img")
	wait_for 120 settled_row "$bsd" || true
	mp=$(mpoint_of "$bsd")
	eq "$id: the next attach is rw (the session ended clean)" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
	eq "$id: the data is intact after the next mount" "$(shasum -a 256 "$mp/data.txt" 2>/dev/null | awk '{ print $1 }')" "$sum"
	"$CLI" unmount "$bsd" >/dev/null 2>&1
	wait_for 60 has_state "$bsd" Appeared || true
	detach_image "$bsd"
	rm -f "$img"
}

echo "== start the job"
start_daemon
unmount_case I-DMN-10 x '"$CLI" unmount "$bsd" 2>&1'
unmount_case I-DMN-11 x 'diskutil unmount "/Volumes/$leaf" 2>&1' "diskutil unmount of the worker's mount answers 'Volume macFUSE Volume on diskNs1 failed to unmount' (the unified log shows macFUSE's FSKit module answering access checks with error 45 at that moment); the same without the daemon loaded, on a plain ntfs-3g mount (evidence diskutil_unmount-without-daemon.txt), so it is the FSKit mount on this macOS and not the daemon; umount and ntfsmac unmount work"
unmount_case I-DMN-12 x 'umount "/Volumes/$leaf" 2>&1'

echo "== I-DMN-13: Finder eject"
img=$(fresh_image clean-64m-4k)
bsd=$(attach_image "$img")
wait_for 120 settled_row "$bsd" || true
mp=$(mpoint_of "$bsd")
leaf=${mp#/Volumes/}
out=$(tmo 40 osascript -e "tell application \"Finder\" to eject disk \"$leaf\"" 2>&1)
rc=$?
echo "osascript rc $rc: $out"
if [ "$rc" -eq 0 ]; then
	check "I-DMN-13: the volume returns to Appeared after the Finder eject" wait_for 90 has_state "$bsd" Appeared
	check "I-DMN-13: the mount point is gone and the journal entry removed" sh -c "[ ! -e '$mp' ] && [ \"$(journal_count)\" = 0 ]"
else
	note "I-DMN-13: NOT RUNNABLE unattended here: Finder did not accept the AppleScript (rc $rc). See tests/integration/finder/finder_guest.sh"
	"$CLI" unmount "$bsd" >/dev/null 2>&1
	wait_for 60 has_state "$bsd" Appeared || true
fi
detach_image "$bsd"
rm -f "$img"
# The call that Finder's eject button ends in, without Finder: NSWorkspace unmountAndEjectDeviceAtPath
# (Disk Arbitration unmount, then eject). It needs no Automation permission, so it runs unattended.
unmount_case I-DMN-13 x 'o=$(tmo 60 osascript -l JavaScript -e "ObjC.import(\"AppKit\"); \$.NSWorkspace.sharedWorkspace.unmountAndEjectDeviceAtPath(\"/Volumes/$leaf\")" 2>&1); echo "NSWorkspace: $o"; [ "$o" = true ]'
echo "-- daemon log"
logcat | tail -30
stop_daemon

echo "== I-DMN-17: passive mode with a dummy third-party NTFS bundle"
sudo -n mkdir -p /Library/Filesystems/ufsd_NTFS.fs
start_daemon
note "doctor output:"
"$CLI" doctor 2>&1 | sed -n '1,30p'
check "doctor names the bundle" sh -c "'$CLI' doctor 2>&1 | grep -qi 'ufsd_NTFS\|Paragon'"
check "the daemon says passive mode in its log" sh -c "sudo -n grep -q 'passive mode' $LOG"
img=$(fresh_image clean-64m-4k)
h0=$(sha "$img")
bsd=$(attach_image_auto "$img")
sleep 15
echo "-- list in passive mode"
list
check "the daemon did nothing with the disk: its state stays Appeared (no state machine was started)" has_state "$bsd" Appeared
check "the system's own driver mounted the disk: the daemon issued no dissent" [ -n "$(apple_mounted "$bsd")" ]
check "the daemon did not mount it: no macfuse mount" sh -c "[ \"\$(mount | grep -cF macfuse)\" = 0 ]"
check "no worker was started" [ "$(workers)" = 0 ]
detach_image "$bsd"
eq "I-DMN-17: image hash unchanged" "$(sha "$img")" "$h0"
stop_daemon
sudo -n rmdir /Library/Filesystems/ufsd_NTFS.fs
rm -f "$img"
finish
