#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# perf_hook.sh - the mount hook that lets tests/perf/bench.py measure the product mount of ONE physical volume
# (H-PERF-01 in docs/HARDWARE_TEST_PROTOCOL.md). bench.py calls it as `sh perf_hook.sh VERB`:
#   mount | remount | unmount | pids | raw N | rawdone
# It only drives `ntfsmac mount` and `ntfsmac unmount` for the volume HW_SLICE (for example disk4s1) and prints the
# mount point. It never opens a device node. The bench writes below <mount point>/perfbench on the volume, so use
# it only on the disposable disk; the volume must be mounted read/write by the product (the hook refuses otherwise).
# `raw` reports that no raw-device baseline is made (bench.py then measures only the file system); the read-only
# device baselines are separate commands in the protocol.
#
# Environment: HW_SLICE (required, for example disk4s1), HW_SERIAL (required: the USB serial of the disposable disk;
# tests/hardware/identify.sh is run with it at every mount), NTFSMAC_CLI (default /usr/local/bin/ntfsmac).
CLI=${NTFSMAC_CLI:-/usr/local/bin/ntfsmac}
PATH=/sbin:/usr/sbin:/usr/bin:/bin:$PATH
export PATH
case ${HW_SLICE:-} in
disk[0-9]s[0-9] | disk[0-9]s[0-9][0-9] | disk[0-9][0-9]s[0-9] | disk[0-9][0-9]s[0-9][0-9]) ;;
*) echo "perf_hook: set HW_SLICE to the slice of the disposable disk, for example disk4s1" >&2; exit 2 ;;
esac
BSD=$HW_SLICE
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# The benchmark writes to the volume: the disk must be the disposable one, checked at every mount.
check_identity() {
	[ -n "${HW_SERIAL:-}" ] || { echo "perf_hook: set HW_SERIAL to the USB serial of the disposable disk" >&2; exit 2; }
	sh "$here/identify.sh" "${HW_SLICE%s[0-9]*}" --serial "$HW_SERIAL" >&2 || { echo "perf_hook: identity check failed" >&2; exit 1; }
}

get() { "$CLI" status "$BSD" 2>&1 | sed -n "s/^  $1: //p" | head -1; }
wait_state() {
	i=0
	while [ "$i" -lt "$2" ]; do
		[ "$(get state)" = "$1" ] && return 0
		sleep 1
		i=$((i + 1))
	done
	return 1
}
do_mount() {
	check_identity
	if [ "$(get state)" != Mounted ]; then
		"$CLI" mount "$BSD" >&2 || { echo "hook: mount failed" >&2; exit 1; }
		wait_state Mounted 120 || { echo "hook: not Mounted after 120 s (state $(get state))" >&2; exit 1; }
	fi
	[ "$(get mount)" = rw ] || { echo "hook: the volume is not read-write" >&2; exit 1; }
	get mountpoint
}
do_unmount() {
	if [ "$(get state)" = Mounted ]; then
		"$CLI" unmount "$BSD" >&2 || { echo "hook: unmount failed" >&2; exit 1; }
	fi
	wait_state Appeared 120 || { echo "hook: not Appeared after 120 s (state $(get state))" >&2; exit 1; }
}
case ${1:-} in
mount) do_mount ;;
unmount) do_unmount ;;
remount) do_unmount && do_mount ;;
raw) echo "SKIP device no raw baseline on physical media from this hook" ;;
rawdone) : ;;
pids) ps -axo pid=,comm= | awk '$2 ~ /ntfsmac-worker$/ { print $1 }' ;;
*) exit 2 ;;
esac
