#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11, U15: does Apple's ntfs.fs mount the images of a split $MFT and $MFTMirr? There is no
# daemon in this suite (nothing is installed, so nothing dissents): each image is attached as a
# virtual disk WITH the system's automount, as a USB disk would be, and the system's answer is
# recorded. `clean-64m-4k` is the control (Apple's driver mounts it read-only, M4-02).
#
# The images: `mirror-split` (record 0, the classified crash cut of the sweep), `vmk-split` and
# `vmk-split-clear` (record 3, the cut of the volmark write sequence). All three are refused by
# NTFS-3G ("$MFTMirr does not match $MFT"). Nothing is written to an image by this suite; the
# hashes before and after are compared.
# Usage: dmn_u15_guest.sh DIR.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
. "$d/tests/integration/daemon/dmn_lib.sh"
work=$(cd -P "$(mktemp -d /tmp/dmnu15.XXXXXX)" && pwd)
echo "guest: $(sw_vers -productVersion) $(uname -m)"
check "the daemon is not loaded (nothing dissents)" sh -c "! sudo -n launchctl print system/$LABEL >/dev/null 2>&1"

for n in clean-64m-4k vmk-split vmk-split-clear mirror-split; do
	img=$(fresh_image "$n")
	h0=$(sha "$img")
	echo "== $n"
	out=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$img" 2>&1)
	rc=$?
	echo "hdiutil attach rc $rc: $out"
	bsd=$(printf '%s\n' "$out" | awk '/^\/dev\/disk/ && !done { sub("^/dev/", "", $1); print $1; done = 1 }')
	sleep 8
	if [ -z "$bsd" ]; then
		bsd=$(hdiutil info 2>/dev/null | awk -v p="$img" '$1 == "image-path" && $3 == p { f = 1 } f && /^\/dev\/disk/ { sub("^/dev/", "", $1); print $1; exit }')
	fi
	if [ -n "$bsd" ]; then
		echo "-- diskutil info $bsd"
		diskutil info "$bsd" 2>&1 | grep -E "Volume Name|File System Personality|Type \(Bundle\)|Mounted|Mount Point|Read-Only Volume|Device Node" || true
		echo "-- mount table"
		mount | grep -F "/dev/$bsd" || echo "(not in the mount table)"
		mounted=$(mount | grep -cF "/dev/$bsd ")
		if [ "$mounted" -gt 0 ]; then
			note "U15 $n: Apple's driver MOUNTED it: $(mount | grep -F "/dev/$bsd ")"
			mp=$(mount | grep -F "/dev/$bsd " | sed 's/^.* on \(.*\) (.*$/\1/')
			note "U15 $n: listing of the root: $(tmo 20 ls "$mp" 2>&1 | tr '\n' ' ')"
		else
			note "U15 $n: Apple's driver did NOT mount it (attach rc $rc)"
		fi
		echo "RESULT U15 $n mounted=$mounted"
		tmo 60 hdiutil detach "/dev/$bsd" >/dev/null 2>&1 || tmo 60 hdiutil detach "/dev/$bsd" -force >/dev/null 2>&1
	else
		echo "RESULT U15 $n mounted=0 (no disk: the image went away or was never attached)"
		note "U15 $n: the image is not attached after 8 s"
	fi
	eq "$n: the image is unchanged" "$(sha "$img")" "$h0"
	rm -f "$img"
done
detach_all
sudo -n rm -rf "$work"
echo "result: $fail check(s) failed"
[ "$fail" -eq 0 ]
