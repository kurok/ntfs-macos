#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# hiber_make.sh - put a Windows-style hibernation file on an NTFS IMAGE FILE.
#
# Usage: tests/tools/hiber_make.sh IMAGE VARIANT [MOUNTPOINT]
#
# Runs inside Environment A only (vm/linux/run.sh): it mounts the image with the
# container's ntfs-3g (internal FUSE, always "norecover") and writes the file
# through the mount. Never run it on the host, never on a device.
#
# VARIANT       file                    size   first bytes   engine / probe view
#   lower       /hiberfil.sys           8 KiB  "hibr"        hibernated (both)
#   upper       /hiberfil.sys           8 KiB  "HIBR"        hibernated (both)
#   short       /hiberfil.sys           100 B  zeros         hibernated: size < 4096
#   wake        /hiberfil.sys           8 KiB  "wake"        probe only (engine misses it)
#   case        /HiberFil.sys           8 KiB  "hibr"        probe only (engine lookup is
#                                                            case sensitive)
#   subdir      /sub/hiberfil.sys       8 KiB  "hibr"        neither (not in the root)
#   inactive    /hiberfil.sys           8 KiB  zeros         clean (Windows wrote zeros)
# The file body after the first four bytes is zeros.
#
# Exit status: 0 success, 1 failure, 2 usage error.
set -eu

die() { printf 'hiber_make: error: %s\n' "$*" >&2; exit 1; }

[ $# -ge 2 ] && [ $# -le 3 ] || {
	echo "usage: hiber_make.sh IMAGE lower|upper|short|wake|case|subdir|inactive [MOUNTPOINT]" >&2
	exit 2
}
img=$1
variant=$2
mnt=${3:-/tmp/ntfsmac-hiber.mnt}

[ -f "$img" ] || die "$img: not a regular file (image files only)"
command -v ntfs-3g >/dev/null 2>&1 || die "ntfs-3g not found: run through Environment A"

dir=
name=hiberfil.sys
sig=hibr
size=8192
case $variant in
lower) ;;
upper) sig=HIBR ;;
short) sig=; size=100 ;;
wake) sig=wake ;;
case) name=HiberFil.sys ;;
subdir) dir=sub ;;
inactive) sig= ;;
*) echo "hiber_make: unknown variant: $variant" >&2; exit 2 ;;
esac

mkdir -p "$mnt"
mounted=0
cleanup() {
	if [ "$mounted" -eq 1 ]; then umount "$mnt" 2>/dev/null || true; fi
}
trap cleanup EXIT HUP INT TERM

ntfs-3g "$img" "$mnt" -o rw,norecover || die "ntfs-3g mount failed"
mounted=1
target=$mnt
if [ -n "$dir" ]; then
	mkdir "$mnt/$dir" || die "mkdir $dir failed"
	target=$mnt/$dir
fi
{
	if [ -n "$sig" ]; then printf '%s' "$sig"; fi
	head -c "$((size - ${#sig}))" /dev/zero
} >"$target/$name" || die "writing $name failed"
sync
umount "$mnt" || die "umount failed"
mounted=0
printf 'hiber_make: %s: %s/%s, %s bytes, header "%s"\n' "$variant" "${dir:-}" "$name" "$size" "$sig"
