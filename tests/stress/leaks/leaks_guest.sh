#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# S-LEAK-01, guest part (T-M5-03): mount a fixture the way the daemon starts the worker, run the
# functional workload, and scan the running worker with Apple's `leaks`.
# Usage: leaks_guest.sh DIR OUT        DIR: the copied tree (tests/stress/leaks, fixtures/NAME.img),
#                                      OUT: where the evidence goes.  Env: LEAK_ITER (default 3).
# Guest only: it mounts and sends a signal to its own worker.
#
# How the worker is started (and why it is not started by the daemon): the daemon gives the worker a
# fixed environment (src/daemon/spawn.c: PATH only), so the daemon cannot pass MallocStackLogging,
# and `leaks` without it prints no allocation stacks, which the project/engine classification needs.
# The worker is therefore started here with the SAME program (the guest-built ntfs-3g that the daemon
# installs as ntfsmac-worker), the SAME argument vector as src/options builds it (device as the
# inherited descriptor /dev/fd/N, patch 0003; mount point under /Volumes; the fixed option string;
# NTFSMAC_ERRFD set, patch 0006), as the console user, plus MallocStackLogging=1. Only the sandbox
# profile and the launchd job are missing; neither changes how the worker allocates.
#
# Scans of the worker: 1 right after the mount, 2 after the first workload iteration, 3 after the
# last one; classify.py reports a problem when the leak count of scan 3 is higher than that of scan 2.
# Control: a program with 8 leaks from a known function (leakctl.c) is scanned the same way.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
PATH=/sbin:/usr/sbin:/usr/bin:/bin:/usr/local/bin
export PATH
d=${1:?directory}
out=${2:?output directory}
iter=${LEAK_ITER:-3}
img_name=${LEAK_FIXTURE:-clean-64m-4k}
mp=/Volumes/ntfsmacleak
worker=$HOME/ntfsmac/deps/bin/ntfs-3g
t0=$(date +%s)
mkdir -p "$out"
work=$(mktemp -d /tmp/leakrun.XXXXXX) || exit 2
wpid=
cpid=

note() { echo "NOTE: $*"; }
cleanup() {
	[ -z "$cpid" ] || kill -9 "$cpid" 2>/dev/null
	if [ -n "$wpid" ] && kill -0 "$wpid" 2>/dev/null; then
		/sbin/umount "$mp" >/dev/null 2>&1 || /sbin/umount -f "$mp" >/dev/null 2>&1
		kill -9 "$wpid" 2>/dev/null
	fi
	sudo -n rmdir "$mp" 2>/dev/null
	rm -rf "$work"
}
trap cleanup EXIT

[ -x "$worker" ] || { echo "FAIL: $worker is missing (the guest build of NTFS-3G)" >&2; exit 2; }
[ -f "$d/fixtures/$img_name.img" ] || { echo "FAIL: fixture $img_name.img is missing" >&2; exit 2; }
cp "$d/fixtures/$img_name.img" "$work/vol.img" || exit 2
: >"$work/errfd"

macfuse=$(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null || echo)
{
	echo "guest_os=macOS $(sw_vers -productVersion) $(uname -m)"
	echo "macfuse=$macfuse"
	echo "ntfs3g=$("$worker" --version 2>&1 | sed -n 's/^.*ntfs-3g \([0-9][0-9.]*\).*/\1/p;' | head -1)"
	echo "iterations=$iter"
	echo "fixture=$img_name"
} >"$out/meta.txt"
# `leaks` gets source lines of the worker from the debug map of the binary, which points at the object
# files of the guest build (~/ntfsmac/build); without them only function names are known, and the
# classification by patched line (R1) is blind, so classify.py fails the run
oso=$(nm -a "$worker" 2>/dev/null | awk '$NF ~ /ntfs-3g\.o$/ { print $NF; exit }')
if [ -n "$oso" ] && [ -f "$oso" ]; then echo "lineinfo=1" >>"$out/meta.txt"; else echo "lineinfo=0" >>"$out/meta.txt"; note "no object file for the worker's debug map ($oso): no source lines"; fi

# ---- control: the pipeline sees a leak, with a stack and a source line -----------------------
echo "== control: leakctl leaks 8 blocks from ctl_leak_site"
mkdir -p "$work/ctl"
cp "$d/tests/stress/leaks/leakctl.c" "$work/ctl/leakctl.c"
if (cd "$work/ctl" && cc -g -O0 -c leakctl.c -o leakctl.o && cc -o leakctl leakctl.o); then
	MallocStackLogging=1 "$work/ctl/leakctl" "$work/ctl/ready" >/dev/null 2>&1 &
	cpid=$!
	i=0
	while [ ! -f "$work/ctl/ready" ] && [ "$i" -lt 30 ]; do sleep 1; i=$((i + 1)); done
	sudo -n /usr/bin/leaks "$cpid" >"$out/leaks-control.txt" 2>&1
	echo "leaks control rc=$?"
	kill -9 "$cpid" 2>/dev/null
	cpid=
else
	echo "FAIL: the control program did not build" | tee "$out/leaks-control.txt"
fi

# ---- the worker, started as the daemon starts it -------------------------------------------
echo "== mount $img_name with the worker (MallocStackLogging=1)"
sudo -n mkdir -p "$mp" && sudo -n chown "$(id -u)" "$mp" || { echo "FAIL: cannot create $mp" >&2; exit 2; }
opts="backend=fskit,local,norecover,noatime,windows_names,nocompression,no_detach,streams_interface=openxattr,uid=$(id -u),gid=$(id -g),fmask=0133,dmask=0022"
(
	exec 9<>"$work/vol.img"
	exec 8>"$work/errfd"
	MallocStackLogging=1 NTFSMAC_ERRFD=8 PATH=/usr/bin:/bin exec "$worker" /dev/fd/9 "$mp" -o "$opts"
) >"$out/worker.log" 2>&1 &
wpid=$!
i=0
while [ "$i" -lt 60 ]; do
	mount | grep -F " on $mp (" | grep -q fskit && break
	kill -0 "$wpid" 2>/dev/null || break
	sleep 1
	i=$((i + 1))
done
if ! mount | grep -F " on $mp (" | grep -q fskit; then
	echo "FAIL: the volume did not mount"
	cat "$out/worker.log"
	exit 2
fi
mount | grep -F " on $mp ("
# the pid of the worker is the pid of the subshell that exec'ed it; check the program
wpath=$(ps -p "$wpid" -o comm= 2>/dev/null)
note "worker pid $wpid ($wpath)"
case $wpath in *ntfs-3g) ;; *) echo "FAIL: pid $wpid is not the worker ($wpath)"; exit 2 ;; esac

scan() { # scan N LABEL
	sudo -n /usr/bin/leaks "$wpid" >"$out/leaks-worker-scan$1.txt" 2>&1
	echo "scan $1 ($2): leaks rc=$? $(sed -n 's/^Process [0-9]*: \([0-9]* leaks\{0,1\} for [0-9]* total leaked bytes\).*/\1/p' "$out/leaks-worker-scan$1.txt")"
}
scan 1 "after the mount, before the workload"

echo "== workload: $iter iteration(s)"
: >"$out/workload.log"
rc=0
python3 -I "$d/tests/stress/leaks/workload.py" "$mp" --iterations 1 --start 1 >>"$out/workload.log" 2>&1 || rc=$?
scan 2 "after iteration 1"
if [ "$rc" -eq 0 ] && [ "$iter" -gt 1 ]; then
	python3 -I "$d/tests/stress/leaks/workload.py" "$mp" --iterations $((iter - 1)) --start 2 >>"$out/workload.log" 2>&1 || rc=$?
fi
sync
scan 3 "after iteration $iter"
tail -n 3 "$out/workload.log"
echo "workload_rc=$rc" >>"$out/meta.txt"
kill -0 "$wpid" 2>/dev/null || { echo "FAIL: the worker died during the run"; echo "worker_alive=0" >>"$out/meta.txt"; }

echo "== unmount"
/sbin/umount "$mp" 2>&1
i=0
while kill -0 "$wpid" 2>/dev/null && [ "$i" -lt 30 ]; do sleep 1; i=$((i + 1)); done
if kill -0 "$wpid" 2>/dev/null; then
	echo "FAIL: the worker did not exit within 30 s of the unmount"
	echo "worker_exit=hung" >>"$out/meta.txt"
else
	wait "$wpid" 2>/dev/null
	echo "worker_exit=$?" >>"$out/meta.txt"
fi
wpid=
echo "seconds=$(($(date +%s) - t0))" >>"$out/meta.txt"
exit 0
