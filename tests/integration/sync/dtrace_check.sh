#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-10: corroborate the syncspy trace (i_sync.py, I-SYNC-01) with dtrace, which sees the system
# calls of the worker from outside, with no library loaded into it. Not part of the harness run:
# it needs sudo and a guest in which dtrace may trace (SIP off, as in the golden VM). Runs IN THE
# Environment B GUEST ONLY, on images it creates itself under WORKDIR; never a host disk.
#
# For an image file and for the buffered node (/dev/diskN of the same image, attached with hdiutil and
# handed to the worker as an inherited descriptor, as in the product) it mounts the volume, traces
# fcntl() and fsync() of the worker process, does: write + fsync, write + F_FULLFSYNC, write + sync(2),
# unmount, and prints one line per traced call. F_FULLFSYNC is fcntl command 51.
#
# Usage (in the guest): sh dtrace_check.sh WORKDIR OUTDIR
# Needs: ~/ntfsmac/deps (scripts/build-ntfs3g.sh --fuse external), sudo -n, python3, hdiutil.
# Mount: backend=fskit, norecover, absolute mount point; every wait has a deadline and a SIGKILL.
# Exit: 0 traced both kinds, 1 a step failed, 2 usage or setup error.
set -u

[ $# -eq 2 ] || { echo "usage: dtrace_check.sh WORKDIR OUTDIR" >&2; exit 2; }
work=$1
out=$2
PATH=$HOME/ntfsmac/deps/bin:$HOME/ntfsmac/deps/sbin:/sbin:/usr/sbin:$PATH
export PATH
case $work in /*) ;; *) echo "dtrace_check: WORKDIR must be absolute" >&2; exit 2 ;; esac
sudo -n true 2>/dev/null || { echo "dtrace_check: needs passwordless sudo in the guest" >&2; exit 2; }
command -v ntfs-3g >/dev/null 2>&1 || { echo "dtrace_check: ntfs-3g not found under ~/ntfsmac/deps" >&2; exit 2; }
mkdir -p "$work" "$out" || exit 2

fail=0
base=
wpid=
dtpat=

# shellcheck disable=SC2329 # runs from the EXIT trap
cleanup() {
	[ -z "$wpid" ] || kill -9 "$wpid" 2>/dev/null
	[ -z "$dtpat" ] || sudo -n pkill -f "$dtpat" 2>/dev/null
	[ -z "$base" ] || hdiutil detach "$base" -force >/dev/null 2>&1
}
trap cleanup EXIT

run_kind() { # run_kind file|buffered
	kind=$1
	img=$work/$kind.img
	mnt=$work/mnt-$kind
	rm -f "$img"
	rm -rf "$mnt"
	mkdir -p "$mnt"
	dd if=/dev/zero of="$img" bs=1m count=0 seek=128 >/dev/null 2>&1
	mkntfs -F -Q "$img" >"$out/mkntfs-$kind.log" 2>&1 || { echo "dtrace_check: mkntfs failed" >&2; return 1; }
	real=$(cd "$mnt" && pwd -P)
	if [ "$kind" = buffered ]; then
		base=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$img" 2>/dev/null | awk 'NR == 1 { print $1 }')
		[ -n "$base" ] || { echo "dtrace_check: hdiutil attach failed" >&2; return 1; }
		diskutil info "$base" | grep -F "134217728 Bytes" >/dev/null 2>&1 || { echo "dtrace_check: $base is not the 128 MiB image" >&2; return 1; }
		ntfs-3g -o backend=fskit,local,norecover,uid="$(id -u)",gid="$(id -g)",no_detach /dev/fd/9 "$real" 9<>"$base" >"$out/worker-$kind.log" 2>&1 &
	else
		ntfs-3g -o backend=fskit,local,norecover,uid="$(id -u)",gid="$(id -g)",no_detach "$img" "$real" >"$out/worker-$kind.log" 2>&1 &
	fi
	wpid=$!
	i=0
	while [ "$i" -lt 60 ] && ! /sbin/mount | grep -F " on $real (" >/dev/null 2>&1; do
		sleep 0.5
		i=$((i + 1))
	done
	/sbin/mount | grep -F " on $real (" >/dev/null 2>&1 || { echo "dtrace_check: no mount for $kind" >&2; return 1; }
	sleep 3
	# fcntl: command (arg1) 51 is F_FULLFSYNC; print the descriptor and the result. fsync: print the result.
	dtpat="pid == $wpid"
	{ sudo -n timeout 90 dtrace -q -n "
	    syscall::*fcntl*:entry /pid == $wpid/ { self->c = arg1; self->f = arg0 }
	    syscall::*fcntl*:return /pid == $wpid && self->c == 51/ { printf(\"%d fcntl(fd %d, F_FULLFSYNC) = %d errno %d\\n\", walltimestamp / 1000000, self->f, (int)arg1, errno); self->c = 0 }
	    syscall::*fsync*:entry /pid == $wpid/ { self->s = arg0 }
	    syscall::*fsync*:return /pid == $wpid/ { printf(\"%d fsync(fd %d) = %d errno %d\\n\", walltimestamp / 1000000, self->s, (int)arg1, errno) }" 2>&1; } \
		>"$out/dtrace-$kind.log" &
	sleep 5
	python3 - "$real" >"$out/steps-$kind.log" <<'PY'
import fcntl, os, sys, time

def mark(name):
    print("%d STEP %s" % (int(time.time() * 1000), name), flush=True)

fd = os.open(os.path.join(sys.argv[1], "d.dat"), os.O_CREAT | os.O_RDWR, 0o644)
os.pwrite(fd, b"a" * 4096, 0)
mark("fsync after write")
os.fsync(fd)
time.sleep(2)
os.pwrite(fd, b"b" * 4096, 4096)
mark("F_FULLFSYNC after write")
fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
time.sleep(2)
os.pwrite(fd, b"c" * 4096, 8192)
mark("sync(2), file open and dirty")
os.sync()
time.sleep(2)
mark("fsync, nothing new written")
os.fsync(fd)
time.sleep(2)
os.close(fd)
mark("closed, unmount follows")
PY
	/sbin/umount "$real" || { echo "dtrace_check: umount failed" >&2; fail=1; }
	echo "$(python3 -c 'import time; print(int(time.time() * 1000))') STEP umount returned" >>"$out/steps-$kind.log"
	i=0
	while [ "$i" -lt 60 ] && kill -0 "$wpid" 2>/dev/null; do
		sleep 0.5
		i=$((i + 1))
	done
	kill -0 "$wpid" 2>/dev/null && { kill -9 "$wpid"; echo "dtrace_check: worker did not exit, SIGKILL" >&2; fail=1; }
	wpid=
	sleep 1
	sudo -n pkill -f "$dtpat" 2>/dev/null
	dtpat=
	sort -n "$out/steps-$kind.log" "$out/dtrace-$kind.log" >"$out/timeline-$kind.txt"
	[ -z "$base" ] || { hdiutil detach "$base" -force >/dev/null 2>&1; base=; }
	return 0
}

run_kind file || fail=1
run_kind buffered || fail=1
{
	echo "== file"
	cat "$out/timeline-file.txt" 2>/dev/null
	echo "== buffered node"
	cat "$out/timeline-buffered.txt" 2>/dev/null
} | tee "$out/timeline.txt"
exit "$fail"
