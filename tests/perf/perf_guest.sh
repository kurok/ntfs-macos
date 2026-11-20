#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# perf_guest.sh - the Environment B side of tests/perf/run.sh (T-M5-07), run INSIDE the macOS guest by
# tests/integration/daemon/dmn_vm.sh (suite `perf`). Not for direct use.
# Usage: perf_guest.sh DIR    (DIR has include/, integration/, src/, tests/, bin/ and fixtures/perf-ntfs.img)
#
# The image is attached as a virtual disk and mounted by the real daemon (the product path: sandboxed worker,
# macFUSE FSKit). tests/perf/bench.py runs on the mount; its cold-cache step is `ntfsmac unmount` followed by
# `ntfsmac mount` (the image stays attached). Environment: PERF_ARGS (the bench.py parameters), PERF_LABEL.
# Output: PASS:/FAIL:/NOTE: lines, META lines and one `PERF-JSON {...}` line with the whole bench.py result.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
# shellcheck source=../integration/daemon/dmn_lib.sh
. "$d/tests/integration/daemon/dmn_lib.sh"

prepare
py=/usr/bin/python3
[ -x "$py" ] || py=$(command -v python3 || true)
[ -n "$py" ] || { echo "FAIL: no python3 in the guest"; exit 2; }
start_daemon
img=$(fresh_image perf-ntfs) || { echo "FAIL: no image"; exit 2; }
sha=$(sha "$img")
bsd=$(attach_image "$img") || { failf "hdiutil attach failed"; finish; exit $?; }
wait_for 90 settled_s "$bsd" || true
sleep 1
mount_diag "$bsd"
eq "the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
echo "$bsd" >"$work/bsd"

cat >"$work/hook.sh" <<HOOK
#!/bin/sh
PATH=/sbin:/usr/sbin:/usr/bin:/bin:\$PATH
CLI=$CLI
BSD=\$(cat $work/bsd)
get() { "\$CLI" status "\$BSD" 2>&1 | sed -n "s/^  \$1: //p" | head -1; }
wait_state() { i=0; while [ "\$i" -lt \$2 ]; do [ "\$(get state)" = "\$1" ] && return 0; sleep 1; i=\$((i + 1)); done; return 1; }
do_mount() {
	if [ "\$(get state)" != Mounted ]; then
		"\$CLI" mount "\$BSD" >&2 || { echo "hook: mount failed" >&2; exit 1; }
		wait_state Mounted 90 || { echo "hook: not Mounted after 90 s (state \$(get state))" >&2; exit 1; }
	fi
	[ "\$(get mount)" = rw ] || { echo "hook: the volume is not read-write" >&2; exit 1; }
	get mountpoint
}
do_unmount() {
	if [ "\$(get state)" = Mounted ]; then
		"\$CLI" unmount "\$BSD" >&2 || { echo "hook: unmount failed" >&2; exit 1; }
	fi
	wait_state Appeared 90 || { echo "hook: not Appeared after 90 s (state \$(get state))" >&2; exit 1; }
}
# the baseline: scratch zero-filled image files (never the image of the file system): one attached as a virtual disk
# (the buffered node /dev/diskN and the raw node /dev/rdiskN the file system sits on) and one plain file
do_raw() {
	dd if=/dev/zero of=$work/raw-file.img bs=1048576 count="\$1" 2>/dev/null || exit 1
	dd if=/dev/zero of=$work/raw-disk.img bs=1048576 count="\$1" 2>/dev/null || exit 1
	o=\$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage $work/raw-disk.img 2>&1) || { echo "SKIP disk hdiutil: \$o" | tr '\\n' ' '; echo; exit 0; }
	n=\$(printf '%s\\n' "\$o" | awk '/^\\/dev\\/disk/ && !done { print \$1; done = 1 }')
	echo "\$n" >$work/rawdisk
	sudo -n chown \$(id -u) "\$n" "\${n%disk*}r\${n##*/}" >&2 2>/dev/null || true
	echo "TARGET disk \$n"
	echo "TARGET rdisk \${n%disk*}r\${n##*/}"
	echo "TARGET file $work/raw-file.img"
}
do_rawdone() {
	[ ! -f $work/rawdisk ] || { hdiutil detach "\$(cat $work/rawdisk)" >&2 || hdiutil detach "\$(cat $work/rawdisk)" -force >&2; } || true
	rm -f $work/raw-file.img $work/raw-disk.img $work/rawdisk
}
case \$1 in
mount) do_mount ;;
unmount) do_unmount ;;
remount) do_unmount && do_mount ;;
raw) do_raw "\$2" ;;
rawdone) do_rawdone ;;
pids) ps -axo pid=,comm= | awk -v c="$INSTALL/ntfsmac-worker" '\$2 == c { print \$1 }' ;;
*) exit 2 ;;
esac
HOOK

macfuse=$(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null || echo)
ntfs3g=$("$INSTALL/ntfsmac-worker" --version 2>&1 | sed -n 's/^.*ntfs-3g \([0-9][0-9.]*\).*/\1/p;' | head -1)
guest_os="macOS $(sw_vers -productVersion) $(uname -m)"
echo "META guest_os=$guest_os"
echo "META macfuse=$macfuse"
echo "META ntfs3g=$ntfs3g"
rc=0
# shellcheck disable=SC2086
"$py" -I "$d/tests/perf/bench.py" --hook "sh $work/hook.sh" --out "$work/bench.json" --label "${PERF_LABEL:-B: this product in a macOS guest}" \
	--meta "ntfs3g=$ntfs3g" --meta "macfuse=$macfuse" --meta "guest_os=$guest_os" --meta "image_sha256=$sha" \
	--meta "kernel=$(uname -sr)" --meta "cpus=$(sysctl -n hw.ncpu)" --meta "mem_kib=$(($(sysctl -n hw.memsize) / 1024))" \
	--emit-json ${PERF_ARGS:-} || rc=$?
if [ "$rc" -eq 0 ]; then pass "bench.py finished every phase of every repetition"; else failf "bench.py status $rc"; fi
sh "$work/hook.sh" unmount >/dev/null 2>&1 || true
sh "$work/hook.sh" rawdone >/dev/null 2>&1 || true
detach_image "$bsd"
finish
