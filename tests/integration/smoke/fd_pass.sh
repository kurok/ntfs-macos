#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# I-SMOKE-02 (T-M2-05): mount an NTFS device through an inherited descriptor (/dev/fd/N).
# A root parent (fd_pass, started with sudo -n) opens the device; an unprivileged child that
# has no permission on the device node runs ntfs-3g /dev/fd/N and mounts it.
#
# Runs IN THE macOS GUEST (Environment B) as a normal user with passwordless sudo, on a disk
# image the caller made and attached, or on an image file. Never on a host disk: TARGET must
# have exactly EXPECT_BYTES bytes and an NTFS boot sector, or nothing is done.
# Steps:
#   1. build fd_pass.c (cc -Wall -Wextra -Werror) next to a temp dir,
#   2. for a block device unmount the macOS automount first (diskutil unmount; while it is
#      mounted even root gets EBUSY), then check TARGET (size, NTFS boot sector); for an
#      image file chmod 000 so the child cannot open it by path,
#   3. record /dev/fd/N semantics (fd_pass semantics),
#   4. rw case: O_RDWR descriptor, mount, check rw in `mount`, write and read back a file,
#      unmount, wait for ntfs-3g to exit,
#   5. ro case: O_RDONLY descriptor, mount with ro, check read-only in `mount`, the file
#      from step 4 is intact, a write fails, unmount, wait,
#   6. ntfsfix -n on the device, also through an inherited O_RDONLY descriptor.
# On a mount timeout every ntfs-3g of this user on /dev/fd/ gets SIGKILL (SIGTERM is ignored
# when no mount appears). Prints PASS/FAIL lines; exit 0 only if every check passed.
#
# Usage: fd_pass.sh [-u USER] [-c rw,ro] TARGET EXPECT_BYTES
#        fd_pass.sh --find-disk EXPECT_BYTES   (print the one whole disk of that size)
# Env:   NTFS3G, NTFSFIX (default: from PATH), CC (default cc), FD_PASS_WAIT (seconds, default 30)
# The mandatory option norecover is always passed; recover, remove_hiberfile and fsname never are.
set -u

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
wait_s=${FD_PASS_WAIT:-30}
ntfs3g=${NTFS3G:-ntfs-3g}
ntfsfix=${NTFSFIX:-ntfsfix}
cc=${CC:-cc}
user=$(id -un)
cases=rw,ro
fails=0

die() { echo "fd_pass: error: $*" >&2; exit 2; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
note() { echo "NOTE: $*"; }

disk_size() { # /dev/diskN -> bytes (diskutil, no device open)
	diskutil info -plist "$1" 2>/dev/null | plutil -extract Size raw -o - - 2>/dev/null
}

find_disk() { # BYTES -> the single whole disk with that size
	found=
	n=0
	for d in /dev/disk[0-9]*; do
		case $d in *s[0-9]*) continue ;; esac
		[ "$(disk_size "$d")" = "$1" ] || continue
		found=$d
		n=$((n + 1))
	done
	[ "$n" = 1 ] || { echo "fd_pass: $n whole disks of $1 bytes, need exactly 1" >&2; return 1; }
	printf '%s\n' "$found"
}

if [ "${1:-}" = --find-disk ]; then
	[ $# = 2 ] || die "usage: fd_pass.sh --find-disk EXPECT_BYTES"
	find_disk "$2"
	exit $?
fi

while [ $# -gt 0 ]; do
	case $1 in
	-u) [ $# -ge 2 ] || die "-u needs a user"; user=$2; shift 2 ;;
	-c) [ $# -ge 2 ] || die "-c needs cases"; cases=$2; shift 2 ;;
	-h | --help) sed -n '2,29p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option: $1" ;;
	*) break ;;
	esac
done
[ $# = 2 ] || die "usage: fd_pass.sh [-u USER] [-c rw,ro] TARGET EXPECT_BYTES"
target=$1
expect=$2
[ "$(id -u)" != 0 ] || die "run as a normal user; only fd_pass itself uses sudo -n"
[ "$user" = "$(id -un)" ] || die "USER must be the calling user (it owns the mount point and unmounts)"
case $expect in '' | *[!0-9]*) die "EXPECT_BYTES must be a number" ;; esac
command -v "$ntfs3g" >/dev/null 2>&1 || die "ntfs-3g not found: $ntfs3g"
command -v "$ntfsfix" >/dev/null 2>&1 || die "ntfsfix not found: $ntfsfix"
ntfs3g=$(command -v "$ntfs3g")
ntfsfix=$(command -v "$ntfsfix")

is_blk=0
if [ -b "$target" ]; then
	is_blk=1
	case $target in /dev/disk[0-9]*) ;; *) die "block device must be /dev/diskN: $target" ;; esac
	case $target in *s[0-9]*) die "use the whole disk, not a slice: $target" ;; esac
	[ "$(disk_size "$target")" = "$expect" ] || die "$target is not $expect bytes (diskutil)"
elif [ -f "$target" ]; then
	case $target in /*) ;; *) target=$(pwd)/$target ;; esac
else
	die "not a block device or regular file: $target"
fi

work=$(mktemp -d "${TMPDIR:-/tmp}/fd_pass.XXXXXX") || die "mktemp failed"
bin=$work/fd_pass
mnt=$work/mnt
mkdir "$mnt" || die "mkdir $mnt"
mnt=$(cd "$mnt" && pwd -P)

mounted() { /sbin/mount | grep -F " on $mnt (" >/dev/null 2>&1; }
# shellcheck disable=SC2329 # used through wait_for and cleanup
driver_pids() { pgrep -U "$(id -u)" -f "ntfs-3g /dev/fd/" 2>/dev/null; }
# shellcheck disable=SC2329 # used through wait_for
no_driver() { [ -z "$(driver_pids)" ]; }
wait_for() { # wait_for CMD...: poll up to wait_s seconds
	i=0
	while [ "$i" -lt $((wait_s * 5)) ]; do
		"$@" && return 0
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}
kill_drivers() {
	for p in $(driver_pids); do
		note "SIGKILL to stray ntfs-3g pid $p"
		kill -9 "$p" 2>/dev/null
	done
}

unmount_automount() { # a block device: unmount whatever macOS mounted from it
	if /sbin/mount | grep -E "^$target(s[0-9]+)? on " >/dev/null; then
		note "automount present: $(/sbin/mount | grep -E "^$target(s[0-9]+)? on ")"
		diskutil unmount "$target" || diskutil unmountDisk "$target" || return 1
	fi
	! /sbin/mount | grep -E "^$target(s[0-9]+)? on " >/dev/null
}

# shellcheck disable=SC2329 # runs from the EXIT trap
cleanup() {
	if mounted; then /sbin/umount "$mnt" >/dev/null 2>&1 || /sbin/umount -f "$mnt" >/dev/null 2>&1; fi
	kill_drivers
	[ "$is_blk" = 1 ] || chmod 644 "$target" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

echo "== build fd_pass ($cc -Wall -Wextra -Werror)"
if "$cc" -std=c11 -Wall -Wextra -Werror -O2 -o "$bin" "$here/fd_pass.c"; then
	pass "fd_pass.c builds with -Wall -Wextra -Werror"
else
	fail "fd_pass.c build"
	exit 1
fi
fdp() { sudo -n "$bin" "$@"; }

echo "== target $target (expect $expect bytes)"
if [ "$is_blk" = 1 ]; then
	ls -l "$target" "$(echo "$target" | sed 's|/dev/disk|/dev/rdisk|')"
	# The macOS automount holds the device: even a root O_RDONLY open gets EBUSY (T-M2-05).
	# Size was already checked through diskutil, which does not open the device.
	if unmount_automount; then pass "no other mount of $target"; else fail "could not unmount automount"; exit 1; fi
fi
fdp check "$target" "$expect" || die "target check failed; refusing to touch $target"
if [ "$is_blk" = 0 ]; then
	chmod 000 "$target" || die "chmod 000 $target"
	ls -l "$target"
fi

echo "== /dev/fd/N semantics"
fdp semantics "$target" "$user" || fail "semantics probe"

opts="backend=fskit,local,norecover,uid=@UID@,gid=@GID@"
payload="fd_pass $(date -u +%Y%m%dT%H%M%SZ) $$"

mount_case() { # mount_case rw|ro: mount through an inherited descriptor; 0 if mounted
	m=$1
	o=$opts
	[ "$m" = ro ] && o=$o,ro
	[ "$is_blk" = 0 ] || unmount_automount >/dev/null || note "automount came back and could not be unmounted"
	echo "-- $m: sudo -n fd_pass run $m $target $expect $user $ntfs3g @FD@ $mnt -o $o"
	fdp run "$m" "$target" "$expect" "$user" "$ntfs3g" @FD@ "$mnt" -o "$o"
	rc=$?
	echo "-- $m: ntfs-3g exit $rc"
	if [ "$rc" != 0 ]; then
		fail "$m: ntfs-3g exit $rc"
		kill_drivers
		return 1
	fi
	if wait_for mounted; then
		pass "$m: mount appeared: $(/sbin/mount | grep -F " on $mnt (")"
	else
		fail "$m: no mount after ${wait_s}s"
		kill_drivers
		return 1
	fi
	echo "-- $m: driver: $(ps -o pid=,user=,command= -p "$(driver_pids | head -1)" 2>/dev/null)"
	return 0
}

unmount_case() { # unmount_case rw|ro
	sync
	if /sbin/umount "$mnt"; then pass "$1: umount"; else fail "$1: umount"; fi
	if wait_for no_driver; then pass "$1: ntfs-3g exited"; else fail "$1: ntfs-3g still running"; kill_drivers; fi
}

case ,$cases, in *,rw,*)
	echo "== case rw (O_RDWR descriptor)"
	if mount_case rw; then
		if /sbin/mount | grep -F " on $mnt (" | grep -F read-only >/dev/null; then
			fail "rw: mount is read-only"
		else
			pass "rw: mount is read/write"
		fi
		if mkdir -p "$mnt/fd_pass" && printf '%s\n' "$payload" >"$mnt/fd_pass/hello.txt"; then
			pass "rw: write file"
		else
			fail "rw: write file"
		fi
		dd if=/dev/urandom of="$mnt/fd_pass/rand.bin" bs=65536 count=16 2>/dev/null
		sum_w=$(shasum -a 256 "$mnt/fd_pass/rand.bin" | cut -d' ' -f1)
		if [ "$(cat "$mnt/fd_pass/hello.txt" 2>/dev/null)" = "$payload" ]; then pass "rw: read back"; else fail "rw: read back"; fi
		unmount_case rw
	fi
	;;
esac

case ,$cases, in *,ro,*)
	echo "== case ro (O_RDONLY descriptor)"
	if mount_case ro; then
		if /sbin/mount | grep -F " on $mnt (" | grep -F read-only >/dev/null; then
			pass "ro: mount is read-only"
		else
			fail "ro: mount is not read-only"
		fi
		find "$mnt" -maxdepth 1 | head -5
		case ,$cases, in *,rw,*)
			if [ "$(cat "$mnt/fd_pass/hello.txt" 2>/dev/null)" = "$payload" ]; then pass "ro: file from rw case intact"; else fail "ro: file from rw case"; fi
			if [ "$(shasum -a 256 "$mnt/fd_pass/rand.bin" 2>/dev/null | cut -d' ' -f1)" = "${sum_w:-x}" ]; then pass "ro: 1 MiB file sha256 intact"; else fail "ro: 1 MiB file sha256"; fi
			;;
		esac
		if (printf x >"$mnt/ro-write-test") 2>/dev/null; then
			fail "ro: write succeeded"
		else
			pass "ro: write refused"
		fi
		unmount_case ro
	fi
	;;
esac

echo "== ntfsfix -n through an inherited O_RDONLY descriptor"
[ "$is_blk" = 0 ] || unmount_automount >/dev/null || true
if fdp run ro "$target" "$expect" "$user" "$ntfsfix" -n @FD@; then
	pass "ntfsfix -n exit 0"
else
	fail "ntfsfix -n"
fi

echo "== result: $fails failure(s)"
[ "$fails" = 0 ]
