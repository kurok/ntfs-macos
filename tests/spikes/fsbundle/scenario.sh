#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-09 spike, GUEST only: attach an NTFS image file, run `diskutil mount` (or let DA
# automount with the second argument "auto"), report what the helper logged and what is
# mounted, try one write, then unmount and eject. Usage: scenario.sh IMAGE [auto]
set -u
# Refuse to run anywhere but a virtual machine guest.
[ "$(sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only (kern.hv_vmm_present is not 1)" >&2; exit 2; }
img=$1; mode=${2:-diskutil}
log=/var/tmp/ntfs3g-fs.log
cd ~/fsb || exit 2
: > $log 2>/dev/null
if [ "$mode" = auto ]; then
	out=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$img" 2>/dev/null)
else
	out=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$img" 2>/dev/null)
fi
disk=$(echo "$out" | awk 'NR==1{print $1}')
echo "attached $img as $disk"
if [ "$mode" != auto ]; then
	sleep 2
	diskutil mount "$disk" 2>&1 | head -2
fi
sleep 6
echo "--- helper log"
cut -c1-170 $log
echo "--- mount table"
/sbin/mount | egrep "Volumes/" | grep -v Macintosh
echo "--- diskutil info"
diskutil info "$disk" | egrep "Type \(Bundle\)|Mounted|Mount Point|Read-Only Volume"
echo "--- write test"
if [ -d /Volumes/NTFSCLEAN ]; then
	touch /Volumes/NTFSCLEAN/w.txt 2>&1 | head -1
	ls /Volumes/NTFSCLEAN
fi
echo "--- unmount/eject"
diskutil unmount "$disk" 2>&1 | head -2
diskutil eject "$disk" 2>&1 | head -2
pkill -9 -f "ntfs-3g -o" 2>/dev/null
exit 0
