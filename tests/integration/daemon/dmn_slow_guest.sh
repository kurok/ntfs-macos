#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11, the mount_timeout regression: with a probe that answers 12 s late (a loaded host), the
# daemon must still mount a clean volume. Before the fix the daemon took macFUSE's own 8 MiB
# backing disk (it appears while the volume is Mounting, with no content yet) for a raw NTFS disk
# and gave a mount-approval dissent for it, so macFUSE's mount failed ("Failed to activate
# virtual device"). Usage: dmn_slow_guest.sh DIR.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare
use_probe slow
start_daemon
for n in 1 2 3; do
	img=$(fresh_image clean-64m-4k)
	t0=$(date +%s)
	bsd=$(attach_image "$img")
	wait_for 150 settled_row "$bsd" || true
	dt=$(( $(date +%s) - t0 ))
	mount_diag "$bsd"
	eq "slow probe, run $n: the volume mounts rw ($dt s)" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
	"$CLI" unmount "$bsd" >/dev/null 2>&1
	wait_for 60 has_state "$bsd" Appeared || true
	detach_image "$bsd"
	rm -f "$img"
	reset_journal
done
check "no 'did not mount' in the daemon log" sh -c "! sudo -n grep -q 'did not mount' $LOG"
check "macFUSE's own 8 MiB disks were never taken for NTFS disks (no 'appeared (8388608' line)" sh -c "! sudo -n grep -q 'appeared (8388608' $LOG"
logcat | tail -15
finish
