#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# env_a.sh - the Environment A side of tests/perf/run.sh, run INSIDE the Linux container
# (`vm/linux/run.sh run -- sh /work/tests/perf/env_a.sh`). Not for direct use.
#
# stdin:  the gzip-compressed fixture image (the same file the macOS guest gets)
# stdout: `PERF-JSON {...}` lines of bench.py (one after every repetition, the last is the result); progress and the
#         hook's messages go to stderr
# env:    PERF_BENCH_ARGS  the bench.py parameters (--reps, --seq-mib, ...), PERF_LABEL
# exit:   the benchmark's status (0 all phases finished, 1 not)
# NTFS-3G is mounted with `norecover` and the option list of the product (src/options/options.c) without the macFUSE
# only ones (backend=fskit, local, no_detach) and with uid 0 (the user of the benchmark in the container).
set -eu
img=/tmp/perf.img
hook=/tmp/perf-hook.sh
out=/tmp/perf-bench.json

gunzip >"$img" || { echo "env_a: the image did not unpack" >&2; exit 2; }
sha=$(sha256sum "$img" | awk '{ print $1 }')
cat >"$hook" <<'HOOK'
#!/bin/sh
img=/tmp/perf.img
mnt=/tmp/perf.mnt
OPTS=rw,norecover,noatime,windows_names,nocompression,streams_interface=openxattr,uid=0,gid=0,fmask=0133,dmask=0022
pids() {
	for p in /proc/[0-9]*; do
		# a daemon that has exited is a zombie until something reaps it (the container has no init): not a worker
		[ "$(cat "$p/comm" 2>/dev/null)" = ntfs-3g ] && ! grep -q '^State:[[:space:]]*Z' "$p/status" 2>/dev/null && basename "$p"
	done
	return 0
}
do_mount() {
	mkdir -p "$mnt"
	if ! mountpoint -q "$mnt"; then
		ntfs-3g "$img" "$mnt" -o "$OPTS" >&2 || { echo "hook: ntfs-3g failed" >&2; exit 1; }
	fi
	mountpoint -q "$mnt" || exit 1
	echo "$mnt"
}
# the baseline: the same block sizes on what the file system sits on. Scratch zero-filled files, never the image of the
# file system. `file`: a regular file on the container's file system (page cached). `loop`: a loop device on another such
# file with direct I/O, when the container may make one (it has /dev/fuse and CAP_SYS_ADMIN only, so usually it may not).
do_raw() {
	dd if=/dev/zero of=/tmp/perf-raw-file.img bs=1048576 count="$1" 2>/dev/null || exit 1
	echo "TARGET file /tmp/perf-raw-file.img"
	dd if=/dev/zero of=/tmp/perf-raw-loop.img bs=1048576 count="$1" 2>/dev/null || exit 1
	if dev=$(losetup --direct-io=on -f --show /tmp/perf-raw-loop.img 2>/tmp/perf-losetup.err); then
		echo "$dev" >/tmp/perf-raw-loop.dev
		echo "TARGET loop $dev"
	else
		echo "SKIP loop losetup: $(tr '\n' ' ' </tmp/perf-losetup.err)"
	fi
}
do_rawdone() {
	[ ! -f /tmp/perf-raw-loop.dev ] || losetup -d "$(cat /tmp/perf-raw-loop.dev)" >&2 || true
	rm -f /tmp/perf-raw-file.img /tmp/perf-raw-loop.img /tmp/perf-raw-loop.dev
}
do_unmount() {
	if mountpoint -q "$mnt"; then
		umount "$mnt" >&2 || { echo "hook: umount failed" >&2; exit 1; }
	fi
	i=0
	while [ -n "$(pids)" ] && [ "$i" -lt 300 ]; do sleep 0.1; i=$((i + 1)); done
	[ -z "$(pids)" ] || { echo "hook: ntfs-3g did not exit" >&2; exit 1; }
}
case $1 in
mount) do_mount ;;
unmount) do_unmount ;;
remount) do_unmount && do_mount ;;
pids) pids ;;
raw) do_raw "$2" ;;
rawdone) do_rawdone ;;
*) exit 2 ;;
esac
HOOK
ver=$(ntfs-3g --version 2>&1 | head -1 | sed -n 's/^.*ntfs-3g \([0-9][0-9.]*\).*/\1/p')
rc=0
# shellcheck disable=SC2086
python3 -I /work/tests/perf/bench.py --hook "sh $hook" --out "$out" --label "A: NTFS-3G ${ver:-?} on Linux (container)" \
	--meta "ntfs3g=$ver" --meta "image_sha256=$sha" --meta "kernel=$(uname -sr)" --meta "cpus=$(nproc)" \
	--meta "mem_kib=$(awk '/^MemTotal:/ { print $2 }' /proc/meminfo)" --emit-json $PERF_BENCH_ARGS || rc=$?
sh "$hook" unmount || true
sh "$hook" rawdone || true
exit "$rc"
