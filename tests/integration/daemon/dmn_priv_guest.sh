#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: the privileged operations and their surroundings at daemon level, with the real daemon
# over its real XPC service, real peers (the CLI run with sudo, re-signed copies of it, a process
# spawned by the daemon) and the real Disk Arbitration. Usage: dmn_priv_guest.sh DIR.
#
#   I-DMN-19  journal-clear: refused for a wrong label and serial, a re-signed CLI, a CLI without
#             the hardened runtime, a caller that is not root, a request from a process the daemon
#             spawned, a volume that is not clean, no entry, a volume mounted rw; accepted on a
#             clean volume with an entry and on one with only the informational flag 0x8000
#   I-DMN-20  evidence rule at daemon level: the rule is configured off in the daemon (section
#             3.5), so flag cleared with and without an LSN change both give ro and keep the entry
#   I-DMN-21  repair --clear-dirty: refused unless the probe says exactly dirty_flag with a clean
#             log; refused while mounted; refused for the peers of I-DMN-19; accepted on
#             dirty-flag and dirty-chkdsk-flag (0x8000 kept)
#   I-DMN-22  marker in the daemon flow: flag set during rw, clear after clean eject, set after a
#             kill; volmark replaced by a program that exits 1 gives a ro mount
#   I-DMN-25  b (the disk already mounted by Apple's driver when the daemon starts) and c (a
#             volmark that changes sector 0) with the real Disk Arbitration
#   I-DMN-27  repair --sync-mirror over XPC: the notification text of the refused attach, the
#             refusals and the repair on mirror-split, then a read/write mount
#
# The CLI is the real one, signed ad hoc with the hardened runtime, its cdhash recorded as the
# requirement (as the installer does). Check 4 is the ADR-019 root fallback (the product default
# since U17): the typed label and serial are given with --label and --serial.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare

CLI_C=$work/ntfsmacC # re-signed: another identifier
CLI_B=$work/ntfsmacB # no hardened runtime
cp "$work/ntfsmac" "$CLI_C" && codesign --force -s - -o runtime -i org.example.other "$CLI_C" >/dev/null 2>&1
cp "$work/ntfsmac" "$CLI_B" && codesign --force -s - "$CLI_B" >/dev/null 2>&1
cc -std=c17 -O2 -o "$work/bin/jstub" tests/integration/daemon/journal_stub.c || { echo "FAIL: journal_stub did not build"; exit 2; }
ln -sf "$work/bin/jstub" "$work/bin/jstub-exit1"
ln -sf "$work/bin/jstub" "$work/bin/jstub-sector0"

serial8() { "$CLI" status "$1" 2>&1 | sed -n 's/^  serial: 0x//p' | head -1 | cut -c9-16; }
flags_of() { "$work/bin/setvolflags" "$1" --show 2>&1 | sed -n 's/.*flags_after=\(0x[0-9a-fA-F]*\).*/\1/p'; }
settle() { wait_for 120 settled_row "$1" || true; sleep 1; }
attach_settle() { _b=$(attach_image "$1"); settle "$_b"; mount_diag "$_b" >&2; echo "$_b"; }
tombstones() { sudo -n sh -c "ls $JDIR/cleared 2>/dev/null | wc -l" | tr -d ' '; }
# priv CLI SUBCOMMAND... : run a privileged CLI as root (sudo), print the output; rc in $prc
priv() { _c=$1; shift; prc=0; pout=$(sudo -n "$_c" "$@" 2>&1) || prc=$?; }
# leave_entry IMG: a rw session that is killed: the entry and the dirty flag stay on the image
leave_entry() {
	_b=$(attach_settle "$1")
	[ "$(state_of "$_b") $(mount_of "$_b")" = "Mounted rw" ] || failf "leave_entry: not mounted rw: $(row "$_b")"
	echo x >"$(mpoint_of "$_b")/x.txt" && sync
	sudo -n kill -9 "$(worker_pids | head -1)"
	# the daemon closes the device before it applies Appeared: wait for the state, then for the last holder
	wait_for 90 has_state "$_b" Appeared || true
	wait_for 60 no_device_holder "$_b" || true
	detach_image "$_b"
}
unmount_wait() { "$CLI" unmount "$1" >/dev/null 2>&1; wait_for 90 has_state "$1" Appeared || true; }

echo "== start the job"
start_daemon

# =========================================================== I-DMN-19: journal-clear
echo "== I-DMN-19: journal-clear over the real XPC service"
reset_journal
img=$(fresh_image clean-64m-4k)
leave_entry "$img"
note "journal_count $(journal_count), flags_of $(flags_of "$img")"
check "the killed session left the entry and the flag on the image" sh -c "[ '$(journal_count)' = 1 ] && [ '$(flags_of "$img")' = 0x0001 ]"
# 19.a the volume is not clean (the marker flag is still set): refused by the probe gate
bsd=$(attach_settle "$img")
echo "-- ($bsd) $(row "$bsd")"
S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
note "label '$LB', serial (last 8) '$S8'"
priv "$CLI" journal-clear --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-19 probe not clean: refused (exit $prc)" test "$prc" -ne 0
check "I-DMN-19 probe not clean: the reason is given" contains "$pout" "not clean"
check "I-DMN-19 probe not clean: the entry is still there" [ "$(journal_count)" = 1 ]
unmount_wait "$bsd"; detach_image "$bsd"
# the Windows-style repair: the flag is cleared elsewhere, the log is untouched
"$work/bin/setvolflags" "$img" --clear DIRTY >/dev/null
bsd=$(attach_settle "$img")
echo "-- ($bsd) $(row "$bsd")"
eq "I-DMN-20 flag cleared, LSN unchanged: ro, entry kept (state mode)" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted ro"
eq "I-DMN-20 flag cleared, LSN unchanged: reason" "$(reason_of "$bsd")" unclean_session
check "I-DMN-20 the entry is kept" [ "$(journal_count)" = 1 ]
# 19.b wrong label, wrong serial
priv "$CLI" journal-clear --label "wrong" --serial "$S8" "$bsd"
check "I-DMN-19 wrong label: refused (exit $prc): $(echo "$pout" | tail -1)" test "$prc" -ne 0
check "I-DMN-19 wrong label: the reason is the confirmation" contains "$pout" "does not match"
priv "$CLI" journal-clear --label "$LB" --serial "00000000" "$bsd"
check "I-DMN-19 wrong serial: refused (exit $prc)" test "$prc" -ne 0
# 19.c the peers
priv "$CLI_C" journal-clear --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-19 a re-signed copy of the CLI: refused by check 3, the connection is dropped (exit $prc)" has_any "$pout" "closed the connection"
priv "$CLI_B" journal-clear --label "$LB" --serial "$S8" "$bsd"
check "I-DMN-19 a CLI without the hardened runtime: refused by check 3 (exit $prc)" has_any "$pout" "closed the connection"
nr=$("$CLI" journal-clear --label "$LB" --serial "$S8" "$bsd" 2>&1); nrc=$?
echo "$nr" | sed 's/^/   /'
check "I-DMN-19 not root: the CLI says to use sudo before anything is typed (exit $nrc)" contains "$nr" "sudo"
check "I-DMN-19 none of the refusals touched the entry" [ "$(journal_count)" = 1 ]
# 19.d a request from inside the worker's process group
echo "-- spawned peer: a stand-in worker runs a client of the XPC service, under a worker profile that allows everything"
# The real worker runs under the real profile, which denies the XPC lookup (I-SBX-08, I-SBX-11):
# a request from it never reaches the daemon (the second line). A setuid `sudo` does not run under
# a sandbox profile either, and the CLI refuses a caller that is not root before it sends anything.
# And the connection of any program but the installed CLI is dropped by the code requirement of
# check 3 before the request is read. So this case (1) records the cdhash of dmn-peer-client (the
# CLI's request code with its root test answered yes) as the requirement, as the installer does for
# the CLI, (2) restarts the daemon, and (3) lets the daemon start a stand-in worker under a profile
# that allows everything; the stand-in runs the client, so a request goes out as the console user,
# an administrator, from a descendant of a process the daemon spawned: check 3 passes by
# construction, the gate and check 1 pass, and the daemon's own check 2 must refuse it.
cp "$work/bin/dmn-peer-client" "$work/dmn-peer-client.signed"
codesign --force -s - -o runtime "$work/dmn-peer-client.signed" >/dev/null 2>&1
phash=$(codesign -d -vvv "$work/dmn-peer-client.signed" 2>&1 | sed -n "s/^CDHash=//p")
printf 'cdhash H"%s"\n' "$phash" >"$work/req.peer"
sudo -n install -m 644 -o root -g wheel "$work/req.peer" "/Library/Application Support/ntfs-macos/cli-requirement"
sudo -n cp "$SBX/worker.sb" "$work/worker.sb.real"
printf '(version 1)\n(allow default)\n' >"$work/worker.sb.open"
sudo -n cp "$INSTALL/ntfsmac-worker" "$work/worker.real"
printf '%s journal-clear --label %s --serial %s %s\n' "$work/dmn-peer-client.signed" "$LB" "$S8" "$bsd" >/var/tmp/dmn-peer.cmd
: >/var/tmp/dmn-peer.out; chmod 666 /var/tmp/dmn-peer.cmd /var/tmp/dmn-peer.out
unmount_wait "$bsd"
start_daemon # the XPC listener reads the requirement at start
# the same client run from this shell: check 3 passes, check 2 does not apply, the root fallback (check 4) refuses
ctl=$("$work/dmn-peer-client.signed" journal-clear --label "$LB" --serial "$S8" "$bsd" 2>&1)
echo "$ctl" | tail -2 | sed 's/^/   /'
check "I-DMN-19 spawned peer: the control run from this shell is not refused by check 2 (it is stopped later by check 4: no root)" lacks "$ctl" "process that the NTFS daemon started"
check "I-DMN-19 spawned peer: the control run reaches check 4 (the root fallback refuses the form)" has_any "$ctl" "authorization"
sudo -n install -m 644 -o root -g wheel "$work/worker.sb.open" "$SBX/worker.sb"
sudo -n install -m 755 -o root -g wheel tests/integration/daemon/dmn_peer_worker.sh "$INSTALL/ntfsmac-worker"
settle "$bsd"
"$CLI" unmount "$bsd" >/dev/null 2>&1; wait_for 60 has_state "$bsd" Appeared || true
# `mount` makes the daemon start a worker, which is the stand-in
"$CLI" mount "$bsd" >/dev/null 2>&1
check "I-DMN-19 spawned peer: the request was made from the stand-in worker" wait_for 120 grep -q "^exit " /var/tmp/dmn-peer.out
echo "-- the stand-in worker's client run:"; sed 's/^/   /' /var/tmp/dmn-peer.out
check "I-DMN-19 spawned peer: refused by check 2, the daemon's own text" grep -q "process that the NTFS daemon started" /var/tmp/dmn-peer.out
check "I-DMN-19 spawned peer: the entry is still there" [ "$(journal_count)" = 1 ]
sudo -n pkill -9 -f dmn_peer_worker >/dev/null 2>&1
sudo -n install -m 755 -o root -g wheel "$work/worker.real" "$INSTALL/ntfsmac-worker"
sudo -n install -m 644 -o root -g wheel "$work/worker.sb.real" "$SBX/worker.sb"
sudo -n install -m 644 -o root -g wheel "$work/req" "/Library/Application Support/ntfs-macos/cli-requirement"
wait_for 120 has_state "$bsd" Failed || true
wait_for 90 sh -c "[ \"\$(workers)\" = 0 ]" || true
start_daemon # back to the requirement of the installed CLI
# 19.e accepted: clean volume with an entry (the restart attached the disk again)
settle "$bsd"
eq "I-DMN-19 before the accept: ro, unclean_session" "$(state_of "$bsd") $(mount_of "$bsd") $(reason_of "$bsd")" "Mounted ro unclean_session"
t0=$(tombstones)
priv "$CLI" journal-clear --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-19 accepted on a clean volume with an entry (exit $prc)" test "$prc" -eq 0
check "I-DMN-19 accepted: the entry moved to the tombstones" sh -c "[ '$(journal_count)' = 0 ] && [ '$(tombstones)' -gt '$t0' ]"
eq "I-DMN-19 accepted: the current mount mode is unchanged" "$(mount_of "$bsd")" ro
"$CLI" unmount "$bsd" >/dev/null 2>&1; wait_for 90 has_state "$bsd" Appeared || true
mo=$("$CLI" mount "$bsd" 2>&1)
wait_for 90 settled_row "$bsd" || true
note "ntfsmac mount: $(echo "$mo" | tr '\n' ' ')"
eq "I-DMN-19 accepted: the next mount is rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
unmount_wait "$bsd"; detach_image "$bsd"; rm -f "$img"
# 19.f accepted with only the informational flag 0x8000 (N-04)
reset_journal
img=$(fresh_image clean-64m-4k)
leave_entry "$img"
"$work/bin/setvolflags" "$img" --clear DIRTY --set MODIFIED_BY_CHKDSK >/dev/null
note "flags on the image: $(flags_of "$img")"
bsd=$(attach_settle "$img")
echo "-- ($bsd) $(row "$bsd")"
S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
priv "$CLI" journal-clear --label "$LB" --serial "$S8" "$bsd"
check "I-DMN-19 accepted with only 0x8000 set (chkdsk-flag, N-04) (exit $prc): $(echo "$pout" | tail -1)" test "$prc" -eq 0
unmount_wait "$bsd"; detach_image "$bsd"; rm -f "$img"
# 19.g no entry; and the volume mounted rw
reset_journal
img=$(fresh_image clean-64m-4k)
bsd=$(attach_settle "$img")
eq "the clean volume is rw (live session, entry present)" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
priv "$CLI" journal-clear --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-19 volume mounted rw with a live session: refused (exit $prc)" test "$prc" -ne 0
check "I-DMN-19 mounted rw: the text names the gate that failed (the marker makes the probe see a dirty volume first)" has_any "$pout" "not clean" "read/write" "live session"
check "I-DMN-19 mounted rw: the entry stays" [ "$(journal_count)" = 1 ]
unmount_wait "$bsd"; detach_image "$bsd"; rm -f "$img"
reset_journal
img=$(fresh_image dirty-flag)
bsd=$(attach_settle "$img")
S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
priv "$CLI" journal-clear --label "$LB" --serial "$S8" "$bsd"
check "I-DMN-19 no entry: refused (exit $prc)" test "$prc" -ne 0
check "I-DMN-19 no entry: the text says so" contains "$pout" "no session journal entry"
unmount_wait "$bsd"; detach_image "$bsd"; rm -f "$img"

# =========================================================== I-DMN-20: LSN changed (rule off)
echo "== I-DMN-20: flag cleared and the LSN advanced; the evidence rule is off in the daemon"
reset_journal
img=$(fresh_image log-init)
leave_entry "$img"
"$work/bin/setvolflags" "$img" --clear DIRTY >/dev/null
"$work/bin/logfile_mark" "$img" --bump-lsn >/dev/null 2>&1
note "logfile_mark --bump-lsn exit $?"
bsd=$(attach_settle "$img")
echo "-- ($bsd) $(row "$bsd")"
eq "I-DMN-20 LSN changed, rule disabled: attach is ro" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted ro"
check "I-DMN-20 LSN changed, rule disabled: the entry is kept" [ "$(journal_count)" = 1 ]
note "I-DMN-20 rule ENABLED is not runnable at daemon level: evidence_rule_enabled is false in main.c (ARCHITECTURE.md section 3.5 rule 2, U12); the module level is journal_vm.sh case evidence"
unmount_wait "$bsd"; detach_image "$bsd"; rm -f "$img"

# =========================================================== I-DMN-21: repair --clear-dirty
echo "== I-DMN-21: repair --clear-dirty"
reset_journal
img=$(fresh_image dirty-flag)
cp "$img" "$work/dirty-before.img"
bsd=$(attach_settle "$img")
eq "dirty-flag attaches ro" "$(state_of "$bsd") $(mount_of "$bsd") $(reason_of "$bsd")" "Mounted ro dirty_flag"
S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
priv "$CLI" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-21 refused while mounted (exit $prc)" test "$prc" -ne 0
check "I-DMN-21 mounted: the text says to unmount first" contains "$pout" "mounted"
unmount_wait "$bsd"
priv "$CLI_C" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd"
check "I-DMN-21 a re-signed CLI: refused by check 3 (exit $prc)" has_any "$pout" "closed the connection"
nr=$("$CLI" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd" 2>&1); nrc=$?
check "I-DMN-21 not root: told to use sudo (exit $nrc)" contains "$nr" "sudo"
priv "$CLI" repair --clear-dirty --label "wrong" --serial "$S8" "$bsd"
check "I-DMN-21 wrong label: refused (exit $prc)" test "$prc" -ne 0
check "I-DMN-21 the refusals left the flag set" sh -c "cmp -s '$img' '$work/dirty-before.img'"
priv "$CLI" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-21 accepted on dirty-flag (exit $prc)" test "$prc" -eq 0
detach_image "$bsd"
eq "I-DMN-21 the flag is clear on the image" "$(flags_of "$img")" 0x0000
nd=$(cmp -l "$work/dirty-before.img" "$img" | wc -l | tr -d ' ')
check "I-DMN-21 only a few bytes of the image changed ($nd; record 3 of \$MFT and \$MFTMirr)" test "$nd" -gt 0 -a "$nd" -le 64
bsd=$(attach_settle "$img")
eq "I-DMN-21 after the repair the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
unmount_wait "$bsd"; detach_image "$bsd"; rm -f "$img" "$work/dirty-before.img"
reset_journal
img=$(fresh_image dirty-chkdsk-flag)
bsd=$(attach_settle "$img"); S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
unmount_wait "$bsd"
priv "$CLI" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd"
check "I-DMN-21 accepted on dirty-chkdsk-flag (exit $prc)" test "$prc" -eq 0
detach_image "$bsd"
eq "I-DMN-21 dirty-chkdsk-flag: the flag 0x8000 is kept, DIRTY cleared" "$(flags_of "$img")" 0x8000
rm -f "$img"
# refusals by the probe: unclean-log, hiber-lower, an extra non-informational flag
for n in unclean-log hiber-lower; do
	img=$(fresh_image "$n")
	bsd=$(attach_settle "$img"); S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
	unmount_wait "$bsd"
	priv "$CLI" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd"
	check "I-DMN-21 $n: refused (exit $prc): $(echo "$pout" | tail -1)" test "$prc" -ne 0
	detach_image "$bsd"; rm -f "$img"
done
img=$(fresh_image clean-64m-4k)
"$work/bin/setvolflags" "$img" --set DIRTY,CHKDSK_UNDERWAY >/dev/null
bsd=$(attach_settle "$img"); S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
unmount_wait "$bsd"
priv "$CLI" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd"
check "I-DMN-21 an extra non-informational flag (CHKDSK_UNDERWAY): refused (exit $prc): $(echo "$pout" | tail -1)" test "$prc" -ne 0
detach_image "$bsd"; rm -f "$img"
# the extra warning when a journal entry exists
reset_journal
img=$(fresh_image clean-64m-4k)
leave_entry "$img"
bsd=$(attach_settle "$img"); S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
unmount_wait "$bsd"
priv "$CLI" repair --clear-dirty --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-21 with a journal entry: the extra warning text is present" contains "$pout" "left the volume open earlier"
detach_image "$bsd"; rm -f "$img"
reset_journal

# =========================================================== I-DMN-22: the marker in the daemon flow
echo "== I-DMN-22: the dirty marker in the daemon flow"
img=$(fresh_image clean-64m-4k)
bsd=$(attach_settle "$img")
eq "I-DMN-22 rw mount" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
# the image is locked while it is mounted, so the flag is read from a copy of the image file
cp "$img" "$work/snap.img"
eq "I-DMN-22 during the rw session the image has VOLUME_IS_DIRTY" "$(flags_of "$work/snap.img")" 0x0001
rm -f "$work/snap.img"
unmount_wait "$bsd"; detach_image "$bsd"
eq "I-DMN-22 after the clean eject the flag is clear" "$(flags_of "$img")" 0x0000
rm -f "$img"
reset_journal
img=$(fresh_image clean-64m-4k)
leave_entry "$img"
eq "I-DMN-22 after the kill (force-detach class) the flag stays set" "$(flags_of "$img")" 0x0001
rm -f "$img"
reset_journal
echo "-- volmark replaced by a program that exits 1"
use_volmark "$work/bin/jstub-exit1"
img=$(fresh_image clean-64m-4k)
h0=$(sha "$img")
bsd=$(attach_settle "$img")
echo "-- ($bsd) $(row "$bsd")"
eq "I-DMN-22 volmark exits 1: ro mount" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted ro"
eq "I-DMN-22 volmark exits 1: reason" "$(reason_of "$bsd")" marker_failed
unmount_wait "$bsd"; detach_image "$bsd"
eq "I-DMN-22 volmark exits 1: the image is unchanged" "$(sha "$img")" "$h0"
rm -f "$img"
reset_journal
echo "-- I-DMN-25 c: a volmark that changes sector 0"
use_volmark "$work/bin/jstub-sector0"
img=$(fresh_image clean-64m-4k)
h0=$(sha "$img")
bsd=$(attach_settle "$img")
check "I-DMN-25 c: the attach happened (a BSD name and a row)" [ -n "$(row "$bsd")" ]
echo "-- ($bsd) $(row "$bsd")"
check "I-DMN-25 c: not mounted rw (state '$(state_of "$bsd")', mode '$(mount_of "$bsd")')" [ "$(mount_of "$bsd")" != rw ]
note "I-DMN-25 c: reason '$(reason_of "$bsd")'"
detach_image "$bsd"
check "I-DMN-25 c: the byte the stub changed is the only change" sh -c "[ \"\$(cmp -l '$img' '$d/fixtures/clean-64m-4k.img' | wc -l | tr -d ' ')\" -le 1 ]"
note "I-DMN-25 c: image hash changed: $([ "$(sha "$img")" != "$h0" ] && echo 'yes (by the stub, as designed)' || echo no)"
rm -f "$img"
use_volmark real
reset_journal

# =========================================================== I-DMN-25 b: mounted by Apple's driver
echo "== I-DMN-25 b: the disk is already mounted by Apple's driver when the daemon starts"
# The daemon never opens the device for writing while another driver has the disk (ARCHITECTURE.md
# section 4.1 steps 3 and 6): Disk Arbitration shows the mount (DAVolumePath), the daemon unmounts
# it first, and only then probes and opens. The race itself (another driver mounting between the
# probe and the O_RDWR open) cannot be made to happen on demand without a test seam; it stays open.
stop_daemon
reset_journal
img=$(fresh_image clean-64m-4k)
h0=$(sha "$img")
bsd=$(attach_image_auto "$img")
sleep 8
note "Apple's mount before the daemon: $(apple_mounted "$bsd" || true)"
check "Apple's driver has the disk mounted (the premise)" [ -n "$(apple_mounted "$bsd")" ]
start_daemon
wait_for 90 settled_row "$bsd" || true
sleep 3
echo "-- ($bsd) $(row "$bsd")"
echo "-- mount table for the disk:"; mount | grep -F "$bsd" || true
logcat | grep -F "$bsd" | tail -12
eq "I-DMN-25 b: the daemon found the disk mounted by another driver and still mounts it (state mode)" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
check "I-DMN-25 b: Apple's mount is gone (the daemon unmounted it before opening the device)" [ -z "$(apple_mounted "$bsd")" ]
check "I-DMN-25 b: the log says the disk was mounted by another driver at the start" sh -c "sudo -n grep -q '$bsd: appeared.*mounted by another driver' $LOG"
unmount_wait "$bsd"
detach_image "$bsd"
note "I-DMN-25 b: image hash changed by the rw session: $([ "$(sha "$img")" != "$h0" ] && echo yes || echo no)"
rm -f "$img"
reset_journal

# =========================================================== I-DMN-27: repair --sync-mirror over XPC
echo "== I-DMN-27: repair --sync-mirror, the notification text and the XPC door"
img=$(fresh_image mirror-split)
h0=$(sha "$img")
bsd=$(attach_settle "$img")
sleep 5
echo "-- ($bsd) $(row "$bsd")"
check "I-DMN-27 the attach of mirror-split is refused (state Failed)" has_state "$bsd" Failed
check "I-DMN-27 the reason is mirror_split" [ "$(reason_of "$bsd")" = mirror_split ]
echo "-- notifications in the daemon log:"
logcat | grep "notification" | tail -3
check "I-DMN-27 the notification names repair --sync-mirror" sh -c "sudo -n grep 'notification' $LOG | grep -qi 'sync-mirror'"
check "I-DMN-27 the notification names Windows chkdsk" sh -c "sudo -n grep 'notification' $LOG | grep -qi 'chkdsk'"
eq "I-DMN-27 the refused attach did not write the image" "$(sha "$img")" "$h0"
S8=$(serial8 "$bsd"); LB=$(sget "$bsd" label)
am=$(apple_mounted "$bsd")
note "U15 in the daemon flow: Apple's ntfs.fs mounted the refused mirror-split image: ${am:-no}"
if [ -n "$am" ]; then
	# ARCHITECTURE.md section 3.2: after the refusal the system may mount it; the repair needs it unmounted
	priv "$CLI" repair --sync-mirror --label "$LB" --serial "$S8" "$bsd"
	echo "$pout" | sed 's/^/   /'
	check "I-DMN-27 while Apple's driver has the disk mounted the repair is refused with the hint (exit $prc)" has_any "$pout" "another driver"
	eq "I-DMN-27 the refusal did not write the image" "$(sha "$img")" "$h0"
	diskutil unmount "/dev/$bsd" >/dev/null 2>&1
	check "I-DMN-27 diskutil unmount takes Apple's mount away" wait_for 30 sh -c "[ -z \"\$(mount | grep -F '/dev/$bsd ' | grep -F '(ntfs,')\" ]"
fi
priv "$CLI" repair --sync-mirror --label "wrong" --serial "$S8" "$bsd"
check "I-DMN-27 wrong label: refused by the confirmation (exit $prc)" has_any "$pout" "does not match"
priv "$CLI" repair --sync-mirror --label "$LB" --serial "00000000" "$bsd"
check "I-DMN-27 wrong serial: refused by the confirmation (exit $prc)" has_any "$pout" "does not match"
priv "$CLI_C" repair --sync-mirror --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-27 a re-signed CLI: refused over XPC (exit $prc)" test "$prc" -ne 0
check "I-DMN-27 a re-signed CLI: the refusal is check 3, not the volume" has_any "$pout" "closed the connection"
priv "$CLI_B" repair --sync-mirror --label "$LB" --serial "$S8" "$bsd"
check "I-DMN-27 a CLI without the hardened runtime: refused by check 3 (exit $prc)" has_any "$pout" "closed the connection"
nr=$("$CLI" repair --sync-mirror --label "$LB" --serial "$S8" "$bsd" 2>&1); nrc=$?
check "I-DMN-27 not root: told to use sudo (exit $nrc)" contains "$nr" "sudo"
eq "I-DMN-27 none of the refusals wrote the image" "$(sha "$img")" "$h0"
priv "$CLI" repair --sync-mirror --label "$LB" --serial "$S8" "$bsd"
echo "$pout" | sed 's/^/   /'
check "I-DMN-27 accepted on mirror-split (exit $prc)" test "$prc" -eq 0
check "I-DMN-27 the reply says repaired and shows a fresh state" has_any "$pout" "repaired"
check "I-DMN-27 the image changed" [ "$(sha "$img")" != "$h0" ]
cp "$img" "$work/after.img"
nd=$(cmp -l "$d/fixtures/mirror-split.img" "$work/after.img" | wc -l | tr -d ' ')
note "I-DMN-27 bytes changed on the image: $nd"
check "I-DMN-27 only a few bytes changed ($nd, at most 4096: the differing \$MFTMirr records)" test "$nd" -gt 0 -a "$nd" -le 4096
echo "-- ntfsfix -n on a copy after the repair"
tmo 120 "$HOME/ntfsmac/deps/bin/ntfsfix" -n "$work/after.img" >"$work/nf.txt" 2>&1
nfrc=$?
sed -n '1,8p' "$work/nf.txt"
eq "I-DMN-27 ntfsfix -n exit status on the repaired image" "$nfrc" 0
rm -f "$work/after.img"
mo=$("$CLI" mount "$bsd" 2>&1)
note "ntfsmac mount: $(echo "$mo" | tr '\n' ' ')"
wait_for 90 settled_row "$bsd" || true
echo "-- ($bsd) $(row "$bsd")"
eq "I-DMN-27 after the repair the volume mounts rw" "$(state_of "$bsd") $(mount_of "$bsd")" "Mounted rw"
unmount_wait "$bsd"; detach_image "$bsd"; rm -f "$img"

echo "-- daemon log (tail)"
logcat | tail -40
echo "-- children log (tail)"
sudo -n tail -n 12 "$CLOG" 2>/dev/null || true
finish
