#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# I-SMOKE-01 (T-M2-03): NTFS-3G on macFUSE's FSKit backend, read/write mount as a normal user.
#
# Runs IN THE macOS GUEST (Environment B), as an unprivileged user, on an NTFS image FILE that
# the caller made (never a host disk). Steps:
#   1. mount the image with -o backend=fskit,local,norecover,uid=<me>,gid=<my group>
#      and check that `mount` reports an fskit mount on the mount point,
#   2. create, write, read back, rename and delete files and a directory; check content,
#   3. sync, unmount with /sbin/umount, wait for the ntfs-3g process to exit,
#   4. remount read-only, check the data written in step 2 and that a write is refused, unmount,
#   5. ntfsfix -n on the image (check only, never repairs).
# Exit 0 only if every check passed.
#
# Usage: mount_smoke.sh [-m MOUNTPOINT] [-o EXTRA_OPTS] IMAGE
# Env:   NTFS3G (default: ntfs-3g from PATH), NTFSFIX (default: ntfsfix from PATH),
#        NTFSMAC_SMOKE_WAIT (seconds to wait for mount/exit, default 30)
# The mandatory option norecover is always passed; recover, remove_hiberfile and fsname are refused.
set -u

wait_s=${NTFSMAC_SMOKE_WAIT:-30}
ntfs3g=${NTFS3G:-ntfs-3g}
ntfsfix=${NTFSFIX:-ntfsfix}
mnt=
extra=
img=
fails=0

die() { echo "mount_smoke: error: $*" >&2; exit 2; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
check() { # check DESC CMD...
	d=$1
	shift
	if "$@" >/dev/null 2>&1; then pass "$d"; else fail "$d"; fi
}

while [ $# -gt 0 ]; do
	case $1 in
	-m) [ $# -ge 2 ] || die "-m needs a mount point"; mnt=$2; shift 2 ;;
	-o) [ $# -ge 2 ] || die "-o needs options"; extra=$2; shift 2 ;;
	-h | --help) sed -n '2,22p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option: $1" ;;
	*) [ -z "$img" ] || die "one IMAGE only"; img=$1; shift ;;
	esac
done
[ -n "$img" ] || die "usage: mount_smoke.sh [-m MOUNTPOINT] [-o EXTRA_OPTS] IMAGE"
[ -f "$img" ] || die "not a regular file: $img"
[ "$(id -u)" != 0 ] || die "must run as a normal user, not root"
case ",$extra," in
*,recover,* | *,remove_hiberfile,* | *,fsname=* | *,norecover,*) die "banned or duplicate option in -o: $extra" ;;
esac
case $img in /*) ;; *) img=$(pwd)/$img ;; esac

own_mnt=0
if [ -z "$mnt" ]; then
	mnt=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmac-smoke.XXXXXX") || die "mktemp failed"
	own_mnt=1
fi
mnt=$(cd "$mnt" && pwd -P) || die "bad mount point"

opts="backend=fskit,local,norecover,uid=$(id -u),gid=$(id -g)${extra:+,$extra}"

mounted() { /sbin/mount | grep -F " on $mnt (" >/dev/null 2>&1; }
# shellcheck disable=SC2329 # called from no_driver, which wait_for runs indirectly
driver_pids() { pgrep -f "ntfs-3g .*$img" 2>/dev/null; }

# shellcheck disable=SC2329 # runs from the EXIT trap
cleanup() {
	if mounted; then /sbin/umount "$mnt" >/dev/null 2>&1 || /sbin/umount -f "$mnt" >/dev/null 2>&1; fi
	[ "$own_mnt" = 0 ] || rmdir "$mnt" 2>/dev/null
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

wait_for() { # wait_for DESC CMD...: poll up to wait_s seconds
	d=$1
	shift
	i=0
	while [ "$i" -lt $((wait_s * 5)) ]; do
		if "$@"; then return 0; fi
		sleep 0.2
		i=$((i + 1))
	done
	echo "timeout waiting for: $d" >&2
	return 1
}
# shellcheck disable=SC2329 # runs through wait_for
no_driver() { [ -z "$(driver_pids)" ]; }

do_mount() { # do_mount EXTRA_OPT_FOR_THIS_MOUNT (e.g. ro or empty)
	o=$opts${1:+,$1}
	echo "+ $ntfs3g -o $o $img $mnt"
	"$ntfs3g" -o "$o" "$img" "$mnt"
	rc=$?
	echo "ntfs-3g exit status: $rc"
	[ "$rc" = 0 ] || return 1
	# ntfs-3g forks into the background; the FSKit mount shows up a moment later.
	wait_for "mount of $mnt" mounted
}

do_umount() {
	sync
	/sbin/umount "$mnt" || return 1
	wait_for "ntfs-3g to exit" no_driver
}

command -v "$ntfs3g" >/dev/null 2>&1 || [ -x "$ntfs3g" ] || die "ntfs-3g not found: $ntfs3g"
command -v "$ntfsfix" >/dev/null 2>&1 || [ -x "$ntfsfix" ] || die "ntfsfix not found: $ntfsfix"
echo "user: $(id -un) (uid $(id -u)); image: $img; mount point: $mnt"
"$ntfs3g" --version | head -1

# ---- 1. read/write mount ----
if do_mount ""; then pass "rw mount (-o $opts)"; else fail "rw mount (-o $opts)"; echo "smoke: FAIL (no mount)"; exit 1; fi
line=$(/sbin/mount | grep -F " on $mnt (")
echo "mount: $line"
case $line in *fskit*) pass "mount output shows fskit" ;; *) fail "mount output shows fskit" ;; esac
case $line in *read-only*) fail "mounted read-write" ;; *) pass "mounted read-write" ;; esac
check "volume root is listable" ls "$mnt"

# ---- 2. write / read / rename / delete ----
w=$mnt/smoke-$$
payload=$(mktemp "${TMPDIR:-/tmp}/ntfsmac-payload.XXXXXX") || die "mktemp failed"
dd if=/dev/urandom of="$payload" bs=1024 count=1024 2>/dev/null
want_sum=$(shasum -a 256 <"$payload" | cut -d' ' -f1)
check "mkdir" mkdir "$w"
check "write small file" sh -c "printf 'hello ntfs\n' >'$w/hello.txt'"
check "read small file back" sh -c "[ \"\$(cat '$w/hello.txt')\" = 'hello ntfs' ]"
check "write 1 MiB file" cp "$payload" "$w/blob.bin"
check "1 MiB file hash after write" sh -c "[ \"\$(shasum -a 256 <'$w/blob.bin' | cut -d' ' -f1)\" = '$want_sum' ]"
check "rename file" mv "$w/hello.txt" "$w/renamed.txt"
check "old name gone after rename" sh -c "[ ! -e '$w/hello.txt' ]"
check "renamed file content" sh -c "[ \"\$(cat '$w/renamed.txt')\" = 'hello ntfs' ]"
check "append to file" sh -c "printf 'more\n' >>'$w/renamed.txt'"
check "append content" sh -c "[ \"\$(wc -l <'$w/renamed.txt' | tr -d ' ')\" = 2 ]"
check "write Unicode name" sh -c "printf x >'$w/caf$(printf '\303\251')-$(printf '\346\227\245\346\234\254').txt'"
check "create and delete file" sh -c "printf tmp >'$w/gone.txt' && rm '$w/gone.txt' && [ ! -e '$w/gone.txt' ]"
check "stat reports a size" sh -c "[ \"\$(stat -f %z '$w/blob.bin')\" = 1048576 ]"
echo "root listing after writes:"
ls -la "$w"
rm -f "$payload"

# ---- 3. unmount ----
if do_umount; then pass "unmount and driver exit"; else fail "unmount and driver exit"; fi
if mounted; then fail "mount point is gone from mount output"; else pass "mount point is gone from mount output"; fi

# ---- 4. read-only remount, verify ----
if do_mount ro; then
	pass "ro mount"
	line=$(/sbin/mount | grep -F " on $mnt (")
	echo "mount: $line"
	case $line in *read-only*) pass "mount output shows read-only" ;; *) fail "mount output shows read-only" ;; esac
	check "ro: hash of 1 MiB file survives remount" sh -c "[ \"\$(shasum -a 256 <'$w/blob.bin' | cut -d' ' -f1)\" = '$want_sum' ]"
	check "ro: renamed file content survives" sh -c "[ \"\$(wc -l <'$w/renamed.txt' | tr -d ' ')\" = 2 ] && [ \"\$(head -1 '$w/renamed.txt')\" = 'hello ntfs' ]"
	check "ro: deleted file stays deleted" sh -c "[ ! -e '$w/gone.txt' ] && [ ! -e '$w/hello.txt' ]"
	check "ro: Unicode name survives" sh -c "ls '$w' | grep -q 'caf'"
	if touch "$w/should-fail" 2>/dev/null; then fail "ro: write is refused"; else pass "ro: write is refused"; fi
else
	fail "ro mount"
fi
if mounted; then
	if do_umount; then pass "ro unmount and driver exit"; else fail "ro unmount and driver exit"; fi
fi

# ---- 5. ntfsfix -n (check only) ----
echo "+ $ntfsfix -n $img"
"$ntfsfix" -n "$img"
rc=$?
echo "ntfsfix exit status: $rc"
if [ "$rc" = 0 ]; then pass "ntfsfix -n finds no problem"; else fail "ntfsfix -n finds no problem"; fi

if [ "$fails" = 0 ]; then
	echo "I-SMOKE-01: PASS"
	exit 0
fi
echo "I-SMOKE-01: FAIL ($fails check(s) failed)"
exit 1
