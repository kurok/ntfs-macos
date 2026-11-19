#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11, Finder tests (I-FND-*, docs/TEST_STRATEGY.md section 4): what Finder shows for a volume
# that the real daemon mounted, and Finder's own copy, rename, delete and eject on it. Run by
# tests/integration/daemon/dmn_vm.sh (suite `finder`) in a disposable macOS guest, as the guest
# user, with the real daemon, probe, volmark and worker; or by hand in such a guest with
# `--guided` for the steps that need a person. Usage: finder_guest.sh DIR [--guided].
#
#   I-FND-01  the volume is visible to Finder (`tell Finder to get name of every disk`) under the
#             leaf the daemon created; the name Finder shows is recorded (U14: no `volname` is
#             passed, and the system names every worker mount "macFUSE Volume")
#   I-FND-02  copy in and out with Finder (`duplicate`): a file and a folder tree; the bytes read
#             back from the volume equal the source
#   I-FND-03  rename and delete with Finder on the volume
#   I-FND-04  Finder eject (`eject disk`): the daemon ends the session cleanly ("ejected, all data
#             written"), the journal entry is gone, the mount point is removed
#
# Scripted means AppleScript through `osascript`, which drives the Finder of the console user. That
# needs a logged-in session and the Automation permission of the calling process (macOS asks the
# person once). If the guest refuses (error -1743, a timeout, or no Finder), the Finder cases are
# reported as NOT RUNNABLE UNATTENDED with the reason and are NOT counted as passed: the checks that
# do not need Finder (the volume's name from `diskutil` and `mdls`, a copy with `ditto`, the unmount
# through the system) still run, and `--guided` prints the steps for a person at the guest's screen
# (the same steps as H-FND-01 of TEST_STRATEGY.md) and asks for each result.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
guided=0
[ "${2:-}" = --guided ] && guided=1
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare

ejected_count() { _n=$(sudo -n grep -c 'ejected, all data written\.' "$LOG" 2>/dev/null); echo "${_n:-0}"; }
asc() { tmo 45 osascript -e "$1" 2>&1; }

echo "== start the job and attach a clean volume"
start_daemon
img=$(fresh_image clean-64m-4k)
bsd=$(attach_image "$img")
wait_for 120 settled_row "$bsd" || true
sleep 1
mp=$(mpoint_of "$bsd")
leaf=${mp#/Volumes/}
eq "the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
echo "-- mount point '$mp'"

echo "== what does not need Finder"
check "I-FND-01 the mount point is /Volumes/<leaf> and is a directory" test -d "$mp"
check "I-FND-01 the volume is listed by diskutil under its disk" sh -c "diskutil list | grep -q '$bsd' || diskutil info '$mp' >/dev/null 2>&1"
# U14: the name that the system (and so Finder) shows for the volume. No volname is passed, so the
# leaf of the mount point is not necessarily the name: this records what the system says.
sysname=$(tmo 20 osascript -l JavaScript -e "ObjC.import('Foundation'); \$.NSFileManager.defaultManager.displayNameAtPath('$mp').js" 2>/dev/null)
echo "RESULT U14: display name of $mp is '$sysname' (the leaf is '$leaf')"
note "U14: the system's display name for the worker's mount is '$sysname', the leaf is '$leaf'"
note "mdls kMDItemDisplayName: $(mdls -name kMDItemDisplayName "$mp" 2>&1 | head -1)"
note "Finder running: $(pgrep -x Finder | head -1 || true) (empty: no Finder in this session)"

echo "== can this session drive Finder?"
out=$(asc 'tell application "Finder" to get name of every disk')
rc=$?
echo "osascript rc $rc: $out"
runnable=0
if [ "$rc" -eq 0 ]; then
	runnable=1
else
	case $out in
	*-1743* | *"not allowed"* | *"not authorized"*) why="the Automation permission for Finder is not granted to the calling process (error -1743)" ;;
	*) why="osascript failed or timed out (rc $rc)" ;;
	esac
	note "NOT RUNNABLE UNATTENDED: $why. The Finder cases below were not run; use --guided in the guest's GUI session."
fi

if [ "$runnable" -eq 1 ]; then
	echo "== I-FND-01: visibility"
	fname=$leaf
	contains "$out" "$leaf" || fname="macFUSE Volume"
	check "I-FND-01 Finder lists the volume (as '$fname'; the leaf is '$leaf')" contains "$out" "$fname"
	fn=$(asc "tell application \"Finder\" to get name of disk \"$fname\"")
	note "I-FND-01 the name Finder shows for the disk: '$fn' (U14: the leaf is '$leaf')"

	echo "== I-FND-02: copy in and out with Finder"
	src=$work/fsrc
	mkdir -p "$src/tree/sub"
	dd if=/dev/urandom of="$src/one.bin" bs=1024 count=1500 2>/dev/null
	echo "hello" >"$src/tree/a.txt"
	dd if=/dev/urandom of="$src/tree/sub/b.bin" bs=1024 count=300 2>/dev/null
	o=$(asc "tell application \"Finder\" to duplicate (POSIX file \"$src/one.bin\" as alias) to (POSIX file \"$mp\" as alias)")
	echo "$o"
	o=$(asc "tell application \"Finder\" to duplicate (POSIX file \"$src/tree\" as alias) to (POSIX file \"$mp\" as alias)")
	echo "$o"
	eq "I-FND-02 the file copied in by Finder has the same bytes" "$(shasum -a 256 "$mp/one.bin" 2>/dev/null | awk '{ print $1 }')" "$(shasum -a 256 "$src/one.bin" | awk '{ print $1 }')"
	check "I-FND-02 the folder tree copied in by Finder is identical" sh -c "diff -r '$src/tree' '$mp/tree'"
	mkdir -p "$work/fout"
	o=$(asc "tell application \"Finder\" to duplicate (POSIX file \"$mp/tree\" as alias) to (POSIX file \"$work/fout\" as alias)")
	echo "$o"
	check "I-FND-02 the folder copied out by Finder is identical" sh -c "diff -r '$src/tree' '$work/fout/tree'"

	echo "== I-FND-03: rename and delete with Finder"
	o=$(asc "tell application \"Finder\" to set name of (POSIX file \"$mp/one.bin\" as alias) to \"renamed.bin\"")
	echo "$o"
	check "I-FND-03 Finder renamed the file" test -f "$mp/renamed.bin"
	o=$(asc "tell application \"Finder\" to delete (POSIX file \"$mp/renamed.bin\" as alias)")
	echo "$o"
	sleep 2
	check "I-FND-03 the file is no longer in the root of the volume" sh -c "[ ! -e '$mp/renamed.bin' ]"
	note "I-FND-03 where the Trash put it: $(ls -A "$mp" 2>/dev/null | tr '\n' '|')"

	echo "== I-FND-04: Finder eject"
	sync
	n0=$(ejected_count)
	o=$(asc "tell application \"Finder\" to eject disk \"$fname\"")
	echo "osascript eject: $o"
	check "I-FND-04 the volume returns to Appeared" wait_for 90 has_state "$bsd" Appeared
	check "I-FND-04 the mount point is gone" sh -c "[ ! -e '$mp' ]"
	check "I-FND-04 the journal entry is gone" [ "$(journal_count)" = 0 ]
	check "I-FND-04 the daemon said 'ejected, all data written'" sh -c "[ \"$(ejected_count)\" -gt \"$n0\" ]"
	detach_image "$bsd"
else
	echo "== steps without Finder automation"
	mkdir -p "$work/fsrc/tree/sub"
	dd if=/dev/urandom of="$work/fsrc/one.bin" bs=1024 count=1500 2>/dev/null
	ditto "$work/fsrc" "$mp/fsrc" 2>&1
	check "a ditto copy in (the call Finder's copy ends in) has the same bytes" sh -c "diff -r '$work/fsrc' '$mp/fsrc'"
	"$CLI" unmount "$bsd" >/dev/null 2>&1
	wait_for 90 has_state "$bsd" Appeared || true
	detach_image "$bsd"
fi

if [ "$guided" -eq 1 ]; then
	echo "== guided: steps for a person at the guest's screen"
	img=$(fresh_image clean-64m-4k)
	bsd=$(attach_image "$img")
	wait_for 120 settled_row "$bsd" || true
	mp=$(mpoint_of "$bsd")
	ask() { printf '%s [y/n] ' "$1"; read -r a </dev/tty; case $a in y | Y) pass "guided: $1" ;; *) failf "guided: $1" ;; esac; }
	echo "The volume is mounted at $mp. In Finder:"
	ask "The volume appears in the sidebar and on the desktop (write down the name it shows; the mount point leaf is '${mp#/Volumes/}', the system's display name is expected to be 'macFUSE Volume', U14)?"
	ask "Dragging a folder from the desktop onto the volume copies it without an error?"
	ask "The copy can be opened, renamed, and moved to the Trash, and the Trash empties without an error?"
	ask "The eject button next to the volume ejects it and the volume disappears?"
	n=$("$CLI" list | grep -c "$bsd")
	note "volume rows left for $bsd: $n"
fi
echo "-- daemon log (tail)"
logcat | tail -15
finish
