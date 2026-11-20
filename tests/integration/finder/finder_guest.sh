#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11, Finder tests (I-FND-*, docs/TEST_STRATEGY.md section 4): what Finder shows for a volume
# that the real daemon mounted, and Finder's own copy, rename, delete and eject on it. Run by
# tests/integration/daemon/dmn_vm.sh (suite `finder`) in a disposable macOS guest, as the guest
# user, with the real daemon, probe, volmark and worker; or by hand in such a guest with
# `--guided` for the steps that need a person. Usage: finder_guest.sh DIR [--guided].
#
#   I-FND-01  the volume is visible to Finder (`tell Finder to get name of every disk`) and is an
#             item of the desktop; the name Finder shows is recorded (U14: no `volname` is passed,
#             and the system names every worker mount "macFUSE Volume"). Two screenshots
#             (`screencapture -x`) are kept as evidence: the desktop with the volume icon, and a
#             Finder window of the volume with the sidebar
#   I-FND-02  copy in and out with Finder (`duplicate`): a generated tree of exactly 1,000 files
#             (nested folders, 0-byte to 1 MiB, names with spaces and non-ASCII letters) in to the
#             volume and back out to a fresh local folder; the file count and the SHA-256 of every
#             file of the source, of the volume and of the copy-out are equal (M4 criterion 4.7)
#   I-FND-03  rename and delete with Finder on the volume
#   I-FND-04  Finder eject (`eject disk`): the daemon ends the session cleanly ("ejected, all data
#             written"), the journal entry is gone, the mount point is removed
#
# Evidence (PNG screenshots, the three manifests) goes to $FINDER_EVIDENCE (default ~/finder-evidence);
# the driver copies it to the host. FINDER_REQUIRED=1 (the attended run) turns a refused Finder
# permission into a FAIL; without it the Finder cases are only reported NOT RUNNABLE.
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
# asl SECONDS SCRIPT: a long Finder action (a 1,000-file copy); the AppleScript timeout is raised too,
# the default of 120 s would end the Apple event while Finder is still copying
asl() { tmo "$1" osascript -e "with timeout of $1 seconds" -e "$2" -e "end timeout" 2>&1; }
FINDER_REQUIRED=${FINDER_REQUIRED:-0}
ev=${FINDER_EVIDENCE:-$HOME/finder-evidence}
mkdir -p "$ev"
notrun=0
# notrun ID WHY: a Finder case that did not run. It is never a PASS; with FINDER_REQUIRED it is a FAIL.
notrun() {
	notrun=$((notrun + 1))
	echo "NOT RUNNABLE: $1: $2"
	[ "$FINDER_REQUIRED" -eq 0 ] || failf "$1 did not run ($2) and the run requires the Finder cases"
}
# shot NAME: a screenshot of the guest's screen into the evidence directory (PNG, not empty)
shot() {
	sleep 2
	screencapture -x -t png "$ev/$1.png" 2>&1
	[ -s "$ev/$1.png" ] && [ "$(stat -f %z "$ev/$1.png")" -gt 5000 ]
}
# manifest DIR OUT: OUT.raw is "sha256  relative path" for every file, sorted; OUT.lines is the same with
# the names in Unicode normalization form C (the volume, ditto and Finder may return a name in form D,
# which is the same name: the comparison is by name in form C and hash); OUT.count is the file count
manifest() {
	(cd "$1" && find . -type f -print0 | xargs -0 -n 40 shasum -a 256 2>/dev/null) >"$2.unsorted"
	LC_ALL=C sort -k2 "$2.unsorted" >"$2.raw"
	perl -MUnicode::Normalize -e 'binmode STDIN, ":utf8"; binmode STDOUT, ":utf8"; while (<STDIN>) { print NFC($_) }' <"$2.unsorted" | LC_ALL=C sort -k2 >"$2.lines"
	rm -f "$2.unsorted"
	wc -l <"$2.lines" | tr -d ' ' >"$2.count"
}
# same_names A B (manifest bases): record whether the names are byte-identical or differ only in form
same_names() {
	if cmp -s "$1.raw" "$2.raw"; then note "names are byte-identical between $(basename "$1") and $(basename "$2")"
	else note "names differ in bytes between $(basename "$1") and $(basename "$2") but not after normalization to form C (the volume or the copy returns another Unicode form)"; fi
}
# same_lines A B: the two manifests (form C) are identical; if not, show the first differences
same_lines() {
	cmp -s "$1" "$2" && return 0
	echo "-- manifests differ (first lines of diff, '<' source, '>' other):"
	{ diff "$1" "$2" | grep '^<' | head -3; diff "$1" "$2" | grep '^>' | head -4; } | cut -c1-200
	return 1
}
# gen_tree DIR: exactly 1,000 files, deterministic: 8 top folders x 5 mid folders, every third file
# one level deeper; names with spaces and non-ASCII letters; sizes 0, 512..8 KB, 100 KB, 1 MiB
gen_tree() {
	perl -e '
	use strict; use utf8; binmode(STDOUT, ":utf8");
	my $root = shift; my $n = 0;
	for my $i (0 .. 999) {
		my $dir = "$root/top " . ($i % 8) . "/mid-\x{e9}" . (int($i / 8) % 5);
		$dir .= "/deep er/\x{43d}\x{43e}\x{432}\x{430}\x{44f}" if $i % 3 == 0;
		my $name = ($i % 7 == 0) ? "na\x{ef}ve \x{444}\x{430}\x{439}\x{43b} $i.dat" : "file $i.bin";
		my $size = ($i % 20 == 0) ? 0 : ($i % 250 == 1) ? 1048576 : ($i % 50 == 3) ? 100000 : 512 + ($i * 37) % 7500;
		my $p = "$dir/$name"; utf8::encode($p);
		my $dd = "$dir"; utf8::encode($dd);
		system("mkdir", "-p", $dd) == 0 or die;
		open(my $fh, ">", $p) or die "$p: $!"; binmode $fh;
		srand($i + 1);
		my $left = $size;
		while ($left > 0) { my $c = $left < 4096 ? $left : 4096; print $fh pack("C*", map { int(rand(256)) } 1 .. $c); $left -= $c; }
		close $fh; $n++;
	}
	print "$n\n";
	' "$1"
}

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
	for _id in I-FND-01 I-FND-02 I-FND-03 I-FND-04; do notrun "$_id" "$why"; done
fi

if [ "$runnable" -eq 1 ]; then
	echo "== I-FND-01: visibility"
	fname=$leaf
	contains "$out" "$leaf" || fname="macFUSE Volume"
	check "I-FND-01 Finder lists the volume (as '$fname'; the leaf is '$leaf')" contains "$out" "$fname"
	fn=$(asc "tell application \"Finder\" to get name of disk \"$fname\"")
	note "I-FND-01 the name Finder shows for the disk: '$fn' (U14: the leaf is '$leaf')"
	note "Finder desktop preferences: ShowExternalHardDrivesOnDesktop=$(defaults read com.apple.finder ShowExternalHardDrivesOnDesktop 2>&1 | head -1) ShowMountedServersOnDesktop=$(defaults read com.apple.finder ShowMountedServersOnDesktop 2>&1 | head -1) (unset: the system default)"
	dsk=$(asc 'tell application "Finder" to get name of every item of desktop')
	note "I-FND-01 items on the desktop: $dsk"
	check "I-FND-01 the volume is an item of the desktop" contains "$dsk" "$fname"
	asc 'tell application "Finder" to close every window' >/dev/null
	check "I-FND-01 screenshot of the desktop with the volume icon (fnd01-desktop.png)" shot fnd01-desktop
	o=$(asc "tell application \"Finder\" to activate" ; asc "tell application \"Finder\" to open disk \"$fname\"")
	echo "$o"
	asc 'tell application "Finder" to set bounds of front window to {80, 80, 1000, 640}' >/dev/null
	# A window opened by a script may come without its toolbar and sidebar; show both for the evidence.
	asc 'tell application "Finder" to set toolbar visible of front window to true' >/dev/null
	asc 'tell application "Finder" to set sidebar width of front window to 200' >/dev/null
	sw=$(asc 'tell application "Finder" to get sidebar width of front window')
	check "I-FND-01 the Finder window shows its sidebar (width '$sw')" test "${sw:-0}" -gt 0 2>/dev/null
	check "I-FND-01 screenshot of a Finder window of the volume with the sidebar (fnd01-sidebar.png)" shot fnd01-sidebar
	asc 'tell application "Finder" to close every window' >/dev/null

	echo "== I-FND-02: copy in and out with Finder (1,000 files)"
	src=$work/fsrc
	mkdir -p "$src"
	dd if=/dev/urandom of="$src/one.bin" bs=1024 count=1500 2>/dev/null
	n=$(gen_tree "$src/tree")
	eq "I-FND-02 the generated source tree has exactly 1000 files" "$n" 1000
	manifest "$src/tree" "$ev/fnd02-source"
	eq "I-FND-02 the source manifest counts 1000 files" "$(cat "$ev/fnd02-source.count")" 1000
	o=$(asc "tell application \"Finder\" to duplicate (POSIX file \"$src/one.bin\" as alias) to (POSIX file \"$mp\" as alias)")
	echo "$o"
	eq "I-FND-02 the file copied in by Finder has the same bytes" "$(shasum -a 256 "$mp/one.bin" 2>/dev/null | awk '{ print $1 }')" "$(shasum -a 256 "$src/one.bin" | awk '{ print $1 }')"
	t0=$(date +%s)
	o=$(asl 1800 "tell application \"Finder\" to duplicate (POSIX file \"$src/tree\" as alias) to (POSIX file \"$mp\" as alias)")
	echo "copy in (Finder duplicate, $(($(date +%s) - t0)) s): $o"
	manifest "$mp/tree" "$ev/fnd02-volume"
	note "I-FND-02 on the volume: $(cat "$ev/fnd02-volume.count") files"
	eq "I-FND-02 the volume holds 1000 files after the copy in" "$(cat "$ev/fnd02-volume.count")" 1000
	check "I-FND-02 SHA-256 of every file on the volume equals the source" same_lines "$ev/fnd02-source.lines" "$ev/fnd02-volume.lines"
	same_names "$ev/fnd02-source" "$ev/fnd02-volume"
	mkdir -p "$work/fout"
	t0=$(date +%s)
	o=$(asl 1800 "tell application \"Finder\" to duplicate (POSIX file \"$mp/tree\" as alias) to (POSIX file \"$work/fout\" as alias)")
	echo "copy out (Finder duplicate, $(($(date +%s) - t0)) s): $o"
	manifest "$work/fout/tree" "$ev/fnd02-out"
	eq "I-FND-02 the copy out holds 1000 files" "$(cat "$ev/fnd02-out.count")" 1000
	check "I-FND-02 SHA-256 of every file copied out equals the source" same_lines "$ev/fnd02-source.lines" "$ev/fnd02-out.lines"
	same_names "$ev/fnd02-source" "$ev/fnd02-out"

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
	# the 1,000-file tree of I-FND-02 with ditto: not a Finder case and not counted as one
	eq "ditto: the generated tree has 1000 files" "$(gen_tree "$work/dtree")" 1000
	ditto "$work/dtree" "$mp/dtree" 2>&1
	manifest "$work/dtree" "$work/m-src"
	manifest "$mp/dtree" "$work/m-vol"
	eq "ditto: the volume holds 1000 files" "$(cat "$work/m-vol.count")" 1000
	check "ditto: SHA-256 of every file on the volume equals the source" same_lines "$work/m-src.lines" "$work/m-vol.lines"
	same_names "$work/m-src" "$work/m-vol"
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
echo "-- Finder cases not run: $notrun (a case that did not run is never counted as passed)"
[ -z "$(ls -A "$ev" 2>/dev/null)" ] || { echo "-- evidence in $ev:"; ls -l "$ev"; }
echo "-- daemon log (tail)"
logcat | tail -15
finish
