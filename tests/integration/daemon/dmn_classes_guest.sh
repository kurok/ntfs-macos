#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: I-DMN-01..08 and I-DMN-23 in a macOS guest, as the guest user (passwordless sudo).
# Usage: dmn_classes_guest.sh DIR (see dmn_lib.sh). The real daemon (launchd job), probe, volmark,
# sandbox and worker; every fixture image is a file attached as a virtual disk with hdiutil
# -nomount, so the daemon sees a Disk Arbitration event and does the rest.
#
# Per fixture: attach, wait until the volume settles, read `ntfsmac list`, look at the kernel
# mount table, try a read and a write, unmount through the CLI, detach, and compare the image
# hash: it must be unchanged for every outcome that is not read/write.
#
#   I-DMN-01  clean classes            rw
#   I-DMN-02  dirty flag               ro, dirty_flag
#   I-DMN-03  unclean log              ro, unclean_log
#   I-DMN-04  log version 2 (fast start) ro, hibernated
#   I-DMN-05  hiberfil.sys hibr/HIBR, name in another case   ro, hibernated
#   I-DMN-06  hiberfil.sys short, wake, unknown signature    ro, hibernated
#   I-DMN-07  corrupt and unsupported (truncated, BitLocker) refused, no mount
#   I-DMN-08  stale mirror (mirror-split, vmk-split, vmk-split-clear) refused, no mount
#   I-DMN-23  the same unsafe images with a probe that reports `clean`
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
# shellcheck source=dmn_lib.sh
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare

# one_case ID NAME KIND [REASON]: KIND is rw, ro or refuse
one_case() {
	id=$1 n=$2 kind=$3 want_reason=${4:-}
	[ -f "$d/fixtures/$n.img" ] || { note "$id $n: not in this run (--only)"; return; }
	img=$(fresh_image "$n") || { failf "$id $n: no image"; return; }
	h0=$(sha "$img")
	t0=$(date +%s)
	bsd=$(attach_image "$img") || { failf "$id $n: hdiutil attach failed"; return; }
	[ -n "$bsd" ] || { failf "$id $n: no BSD name"; return; }
	# the volume settles: its state is Mounted or Failed; the time is criterion 4.1 (within 10 s)
	wait_for 120 settled_row "$bsd" || true
	dt=$(( $(date +%s) - t0 ))
	sleep 2
	st=$(state_of "$bsd")
	mo=$(mount_of "$bsd")
	re=$(reason_of "$bsd")
	mp=$(mpoint_of "$bsd")
	echo "-- $id $n ($bsd): $(row "$bsd")"
	km=$(mount | grep -F " on $mp (" || true)
	case $kind in
	rw)
		check "$id $n: mounted within 10 s of the attach (criterion 4.1; $dt s)" [ "$dt" -le 10 ]
		eq "$id $n state" "$st" Mounted
		eq "$id $n mode" "$mo" rw
		check "$id $n: the kernel mount is a macfuse mount that is not read-only" sh -c "echo '$km' | grep -F macfuse | grep -qv read-only"
		check "$id $n: a file can be written and read back" sh -c "echo dmn > '$mp/dmn.txt' && sync && grep -q dmn '$mp/dmn.txt'"
		check "$id $n: the session holds a journal entry" [ "$(journal_count)" = 1 ]
		;;
	ro)
		check "$id $n: mounted within 10 s of the attach (criterion 4.1; $dt s)" [ "$dt" -le 10 ]
		eq "$id $n state" "$st" Mounted
		eq "$id $n mode" "$mo" ro
		eq "$id $n reason" "$re" "$want_reason"
		check "$id $n: the kernel mount is read-only" sh -c "echo '$km' | grep -F macfuse | grep -q read-only"
		check "$id $n: the root can be listed" tmo 20 ls "$mp"
		check "$id $n: a write is refused" sh -c "! (echo x > '$mp/dmn.txt') 2>/dev/null"
		check "$id $n: no journal entry (no write session)" [ "$(journal_count)" = 0 ]
		;;
	refuse)
		note "$id $n: the refusal was reached $dt s after the attach"
		check "$id $n: no mount (state '$st', reason '$re')" sh -c "[ '$mo' != rw ] && [ '$mo' != ro ] && [ \"\$(mount | grep -cF macfuse)\" = 0 ]"
		eq "$id $n: the refusal ends in state Failed" "$st" Failed
		sleep 3
		am=$(apple_mounted "$bsd")
		note "$id $n: Apple's own ntfs.fs mount of the refused volume (U15): ${am:-none}"
		eq "$id $n reason" "$re" "$want_reason"
		;;
	esac
	if [ "$st" = Mounted ]; then
		"$CLI" unmount "$bsd" >/dev/null 2>&1
		wait_for 90 has_state "$bsd" Appeared ||
			note "$id $n: the volume did not return to Appeared (state $(state_of "$bsd"))"
	fi
	detach_image "$bsd"
	h1=$(sha "$img")
	if [ "$kind" = rw ]; then
		note "$id $n: image hash changed by the rw session: $([ "$h0" != "$h1" ] && echo yes || echo no)"
		check "$id $n: the journal entry is gone after the clean unmount" [ "$(journal_count)" = 0 ]
	else
		eq "$id $n: image hash unchanged after the session" "$h1" "$h0"
	fi
	rm -f "$img"
}

echo "== start the job (real probe, volmark and worker)"
start_daemon
check "the privileged XPC service is registered with launchd" sh -c "sudo -n launchctl print system/$LABEL | grep -q org.ntfs-macos.ctl.privileged"

echo "== I-DMN-01: clean classes mount read/write"
for n in clean-64m-4k log-init log-bumped chkdsk-flag hiber-inactive hiber-subdir txf-nonresident; do
	one_case I-DMN-01 "$n" rw
done
echo "== I-DMN-02: dirty flag"
one_case I-DMN-02 dirty-flag ro dirty_flag
one_case I-DMN-02 dirty-chkdsk-flag ro dirty_flag
echo "== I-DMN-03: unclean log"
one_case I-DMN-03 unclean-log ro unclean_log
echo "== I-DMN-04: log version 2 (fast startup)"
one_case I-DMN-04 log-v2 ro hibernated
echo "== I-DMN-05: hibernation file, signature hibr, HIBR, other case"
for n in hiber-lower hiber-upper hiber-case; do one_case I-DMN-05 "$n" ro hibernated; done
echo "== I-DMN-06: hibernation file, short, wake, unknown signature"
for n in hiber-short hiber-wake hiber-unknown; do one_case I-DMN-06 "$n" ro hibernated; done
echo "== I-DMN-07: corrupt and unsupported"
for n in truncated truncated-60 bitlocker-sig; do one_case I-DMN-07 "$n" refuse refused; done
echo "== I-DMN-08: stale mirror"
for n in mirror-split vmk-split vmk-split-clear; do one_case I-DMN-08 "$n" refuse mirror_split; done
echo "-- daemon log (first 120 lines)"
logcat | sed -n '1,120p'
check "no worker is left" [ "$(workers)" = 0 ]
stop_daemon

echo "== I-DMN-23: a probe that reports clean for the unsafe images; the marker step is on"
use_probe stub
start_daemon
for n in dirty-flag unclean-log log-v2 hiber-lower; do
	[ -f "$d/fixtures/$n.img" ] || continue
	reset_journal
	img=$(fresh_image "$n")
	h0=$(sha "$img")
	bsd=$(attach_image "$img") || { failf "I-DMN-23 $n: hdiutil attach failed"; continue; }
	[ -n "$bsd" ] || { failf "I-DMN-23 $n: no BSD name"; continue; }
	wait_for 120 settled_row "$bsd" || true
	sleep 2
	echo "-- I-DMN-23 $n ($bsd): $(row "$bsd")"
	st=$(state_of "$bsd"); mo=$(mount_of "$bsd"); re=$(reason_of "$bsd"); mp=$(mpoint_of "$bsd")
	km=$(mount | grep -F " on $mp (" || true)
	check "I-DMN-23 $n: the probe stand-in said clean (status shows probe state clean)" sh -c "'$CLI' status $bsd 2>&1 | grep -q 'state: clean'"
	eq "I-DMN-23 $n: state and mode (marker on: volmark refuses, so ro)" "$st $mo" "Mounted ro"
	check "I-DMN-23 $n: the reason is given ('$re')" [ -n "$re" ]
	if [ "$st" = Mounted ]; then
		check "I-DMN-23 $n: the kernel mount is read-only" sh -c "echo '$km' | grep -F macfuse | grep -q read-only"
		note "I-DMN-23 $n: reported ro with reason '$re'"
		"$CLI" unmount "$bsd" >/dev/null 2>&1
		wait_for 90 has_state "$bsd" Appeared || true
	fi
	detach_image "$bsd"
	check "I-DMN-23 $n: the image is no longer attached before it is hashed" not_attached "$img"
	eq "I-DMN-23 $n: image hash unchanged (no write reached the image)" "$(sha "$img")" "$h0"
	rm -f "$img"
done
echo "-- daemon log"
logcat | sed -n '1,80p'
stop_daemon

echo "== I-DMN-23 with the marker step disabled (--no-marker): the outcomes are recorded"
sudo -n rm -rf "$JDIR"/* 2>/dev/null
start_daemon --no-marker
for n in dirty-flag unclean-log log-v2 hiber-lower; do
	[ -f "$d/fixtures/$n.img" ] || continue
	reset_journal
	img=$(fresh_image "$n")
	h0=$(sha "$img")
	bsd=$(attach_image "$img") || { failf "I-DMN-23 (no marker) $n: hdiutil attach failed"; continue; }
	[ -n "$bsd" ] || { failf "I-DMN-23 (no marker) $n: no BSD name"; continue; }
	wait_for 120 settled_row "$bsd" || true
	sleep 2
	st=$(state_of "$bsd"); mo=$(mount_of "$bsd"); re=$(reason_of "$bsd")
	echo "-- I-DMN-23 (no marker) $n ($bsd): $(row "$bsd")"
	case $n in
	dirty-flag)
		note "I-DMN-23 (no marker) dirty-flag: state $st mode $mo reason '$re' (expected: written, rw: the engine does not check the flag)"
		;;
	*)
		eq "I-DMN-23 (no marker) $n: the engine's read-only fallback is reported as ro with a reason" "$st $mo" "Mounted ro"
		note "I-DMN-23 (no marker) $n: reason '$re'"
		;;
	esac
	if [ "$st" = Mounted ]; then
		"$CLI" unmount "$bsd" >/dev/null 2>&1
		wait_for 90 has_state "$bsd" Appeared || true
	fi
	detach_image "$bsd"
	check "I-DMN-23 (no marker) $n: the image is no longer attached before it is hashed" not_attached "$img"
	h1=$(sha "$img")
	if [ "$n" = dirty-flag ]; then
		note "I-DMN-23 (no marker) dirty-flag: image hash changed: $([ "$h0" != "$h1" ] && echo yes || echo no)"
	else
		eq "I-DMN-23 (no marker) $n: image hash unchanged" "$h1" "$h0"
	fi
	rm -f "$img"
done
echo "-- daemon log"
logcat | sed -n '1,80p'
use_probe real
finish
