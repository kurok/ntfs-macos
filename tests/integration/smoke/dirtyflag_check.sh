#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M3-01 (U9, U13): confirm by test that NTFS-3G
#   U9   never sets VOLUME_IS_DIRTY in $Volume during a read/write session, and
#   U13  leaves the $LogFile restart area (both copies, incl. current_lsn) unchanged in a
#        read/write session mounted with norecover.
#
# Runs IN Environment A (Linux container, run as root, NTFS-3G with internal FUSE) or IN
# Environment B (macOS guest, normal user, macFUSE FSKit backend), on NTFS image FILES that this
# script makes in WORKDIR (never a host disk). Two log fixtures are run:
#   empty  fresh mkntfs image: $LogFile is all 0xff (no restart pages, "empty")
#   synth  the same image with two hand-built version 1.1 restart pages in $LogFile: valid,
#          RESTART_VOLUME_IS_CLEAN set, current_lsn non-zero and different in the two copies,
#          so that a change of the restart area would be visible. Synthetic (not written by
#          Windows); it passes ntfs_check_logfile() and ntfs_is_logfile_clean(), so the driver
#          takes the normal "log is clean" path.
# Per fixture: snapshot before; mount rw norecover, write, sync, snapshot DURING; clean unmount,
# snapshot; mount rw norecover, write, sync, kill -9 the driver, force the stale mount away,
# snapshot AFTER-KILL; remount rw norecover and write (must work), unmount, ntfsfix -n (on a copy),
# snapshot. A snapshot is a copy of the image file (the driver writes the file through the page
# cache, so a copy made while mounted sees what the driver has written so far; data still held in
# the driver's own caches is not in it, hence `sync` first and an ntfsls check that the files are
# visible in the DURING copy). It records: $Volume flags (ntfsinfo -F '$Volume' and ntfsinfo -m),
# sha-256 of MFT record 3 ($Volume), sha-256 of the whole $LogFile and of its first 8192 bytes,
# the two restart pages parsed from the raw image bytes (magic, current_lsn, chkdsk_lsn, client
# lists, flags, open count) and a hex dump of the first 0x120 bytes of each.
# Positive control (dirty flag): on a copy of the final image `ntfsfix` (without -n; never mounted; never the
# real image) must make the detector report the dirty flag set. Unclean-log case (synth): see unclean_case.
#
# Mandatory option norecover is in every mount. recover, remove_hiberfile and fsname are never
# passed. The script never touches a mount it did not make.
#
# Usage: dirtyflag_check.sh -e linux|macos [-w WORKDIR]
#   linux: in the container, e.g.
#     vm/linux/run.sh run -- sh /work/tests/integration/smoke/dirtyflag_check.sh -e linux -w /tmp/dfc
#   macos: in the guest as a normal user (tools from PATH or NTFS3G, NTFSINFO, ... below)
# Env:   NTFS3G, NTFSINFO, NTFSCAT, NTFSCLUSTER, NTFSLS, NTFSFIX, MKNTFS (default: names from PATH)
# Exit:  0 all checks passed, 1 a check failed, 2 setup error.
# shellcheck disable=SC2329,SC2016,SC2086 # traps/wait_for; $Volume is a literal NTFS file name; pid lists split on purpose

set -u

PATH=/sbin:/usr/sbin:$PATH
export PATH

envname=
work=
ntfs3g=${NTFS3G:-ntfs-3g}
ntfsinfo=${NTFSINFO:-ntfsinfo}
ntfsls=${NTFSLS:-ntfsls}
ntfsfix=${NTFSFIX:-ntfsfix}
ntfscluster=${NTFSCLUSTER:-ntfscluster}
mkntfs=${MKNTFS:-mkntfs}
wait_s=30
fails=0

die() { echo "dirtyflag_check: error: $*" >&2; exit 2; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

while [ $# -gt 0 ]; do
	case $1 in
	-e) [ $# -ge 2 ] || die "-e needs linux or macos"; envname=$2; shift 2 ;;
	-w) [ $# -ge 2 ] || die "-w needs a directory"; work=$2; shift 2 ;;
	-h | --help) sed -n '2,45p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) die "unknown argument: $1" ;;
	esac
done
case $envname in linux | macos) ;; *) die "usage: dirtyflag_check.sh -e linux|macos [-w WORKDIR]" ;; esac
if [ "$envname" = linux ]; then
	[ "$(id -u)" = 0 ] || die "Environment A runs as root inside the container"
else
	[ "$(id -u)" != 0 ] || die "Environment B runs as a normal user"
fi
for t in "$ntfs3g" "$ntfsinfo" "$ntfsls" "$ntfsfix" "$ntfscluster" "$mkntfs" xxd od awk dd cp; do
	command -v "$t" >/dev/null 2>&1 || [ -x "$t" ] || die "tool not found: $t"
done
ntfscat=${NTFSCAT:-ntfscat}
command -v "$ntfscat" >/dev/null 2>&1 || [ -x "$ntfscat" ] || die "tool not found: $ntfscat"

if [ -z "$work" ]; then
	work=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmac-dirtyflag.XXXXXX") || die "mktemp failed"
else
	mkdir -p "$work" || die "cannot create $work"
fi
work=$(cd "$work" && pwd -P) || die "bad workdir"
mnt=$work/mnt
mkdir -p "$mnt"

sha() {
	if command -v sha256sum >/dev/null 2>&1; then sha256sum | cut -d' ' -f1; else shasum -a 256 | cut -d' ' -f1; fi
}

run() { # run DESC CMD... : log the command line and its exit status, return the status
	d=$1
	shift
	echo "+ $*"
	"$@"
	rc=$?
	echo "  exit status: $rc ($d)"
	return $rc
}

mounted() { mount | grep -F " on $mnt " >/dev/null 2>&1 || mount | grep -F " on $mnt (" >/dev/null 2>&1; }
driver_pids() { pgrep -f "ntfs-3g .*$cur_img" 2>/dev/null; }
no_driver() { [ -z "$(driver_pids)" ]; }

wait_for() { # wait_for DESC CMD...
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

force_umount() {
	if [ "$envname" = linux ]; then
		umount "$mnt" >/dev/null 2>&1 || umount -l "$mnt" >/dev/null 2>&1 || fusermount -uz "$mnt" >/dev/null 2>&1
	else
		/sbin/umount "$mnt" >/dev/null 2>&1 || /sbin/umount -f "$mnt" >/dev/null 2>&1
	fi
}

cleanup() {
	pids=$(driver_pids 2>/dev/null) || pids=
	if mounted; then force_umount; fi
	[ -z "$pids" ] || kill -9 $pids 2>/dev/null
	rmdir "$mnt" 2>/dev/null
	if [ -n "${KEEP_WORK:-}" ]; then echo "work dir kept: $work"; else rm -rf "$work"/*.img "$work"/page*.bin "$work"/logfile*.bin 2>/dev/null; fi
}
cur_img=$work/none.img
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# Mount options. norecover is always present.
if [ "$envname" = linux ]; then
	mopts="rw,norecover"
else
	mopts="backend=fskit,local,norecover,uid=$(id -u),gid=$(id -g)"
fi

do_mount() { # do_mount IMAGE
	cur_img=$1
	echo "+ $ntfs3g -o $mopts $cur_img $mnt"
	"$ntfs3g" -o "$mopts" "$cur_img" "$mnt"
	rc=$?
	echo "  exit status: $rc (ntfs-3g mount)"
	[ "$rc" = 0 ] || return 1
	wait_for "mount of $mnt" mounted || return 1
	mount | grep -F " on $mnt" | sed 's/^/  mount: /'
}

do_umount() {
	sync
	force_umount || return 1
	wait_for "ntfs-3g to exit" no_driver
}

write_files() { # write_files TAG
	t=$1
	mkdir "$mnt/dir-$t" || return 1
	dd if=/dev/urandom of="$mnt/dir-$t/blob.bin" bs=65536 count=16 2>/dev/null || return 1
	printf 'hello %s\n' "$t" >"$mnt/dir-$t/hello.txt" || return 1
	n=0
	while [ "$n" -lt 40 ]; do
		printf 'f%s\n' "$n" >"$mnt/dir-$t/small-$n.txt" || return 1
		n=$((n + 1))
	done
	mv "$mnt/dir-$t/hello.txt" "$mnt/dir-$t/renamed.txt" || return 1
	rm "$mnt/dir-$t/small-0.txt" || return 1
	sync
}

# ---- byte readers (little-endian fields, printed as big-endian hex) ----
rdle() { # rdle FILE OFFSET LEN
	od -An -v -tx1 -j "$2" -N "$3" "$1" | tr -d ' \n' |
		awk '{ s = ""; for (i = length($0) - 1; i >= 1; i -= 2) s = s substr($0, i, 2); print s }'
}
hexle() { # hexle VALUE NBYTES : value as little-endian hex string
	hl_fmt="%0$(($2 * 2))x"
	# shellcheck disable=SC2059 # the width is computed
	printf "$hl_fmt" "$1" |
		awk '{ s = ""; for (i = length($0) - 1; i >= 1; i -= 2) s = s substr($0, i, 2); print s }'
}

# ---- geometry from an image (cluster size, MFT LCN, $LogFile LCN) ----
geometry() { # geometry IMAGE -> sets g_cs g_mftlcn g_loglcn
	g_cs=$("$ntfsinfo" -f -m "$1" 2>/dev/null | awk -F': *' '/^[ \t]*Cluster Size:/ { print $2; exit }' | tr -d ' \t')
	g_mftlcn=$("$ntfsinfo" -f -m "$1" 2>/dev/null | awk -F': *' '/LCN of Data Attribute for FILE_MFT:/ { print $2; exit }' | tr -d ' \t')
	g_loglcn=$("$ntfscluster" -f -F '$LogFile' "$1" 2>/dev/null | awk '/^ *[0-9]+ +[0-9]+ +[0-9]+ *$/ { print $2; exit }')
	[ -n "$g_cs" ] && [ -n "$g_mftlcn" ] && [ -n "$g_loglcn" ] || return 1
}

# ---- synthetic restart pages ----
build_page() { # build_page OUTFILE CURRENT_LSN
	out=$2
	_o=$1
	h=$(printf '%s' 52535452) # RSTR
	h=$h$(hexle 30 2)$(hexle 9 2)$(hexle 0 8)  # usa_ofs, usa_count, chkdsk_lsn
	h=$h$(hexle 4096 4)$(hexle 4096 4)         # system_page_size, log_page_size
	h=$h$(hexle 48 2)$(hexle 1 2)$(hexle 1 2)  # restart_area_offset, minor 1, major 1
	h=$h$(hexle 1 2)                           # usn (usa[0]) at offset 30
	h=$h$(printf '%032d' 0)                    # usa[1..8], saved sector tails (all zero)
	# RESTART_AREA at 48
	h=$h$(hexle "$out" 8)                      # current_lsn
	h=$h$(hexle 1 2)$(hexle 65535 2)$(hexle 0 2) # log_clients, client_free_list, client_in_use_list
	h=$h$(hexle 2 2)                           # flags: RESTART_VOLUME_IS_CLEAN
	h=$h$(hexle 45 4)                          # seq_number_bits = 67 - 22 (file_size 2 MiB)
	h=$h$(hexle 224 2)$(hexle 64 2)            # restart_area_length, client_array_offset
	h=$h$(hexle 2097152 8)                     # file_size
	h=$h$(hexle 0 4)$(hexle 48 2)$(hexle 64 2) # last_lsn_data_length, log_record_header_length, log_page_data_offset
	h=$h$(hexle 1 4)$(hexle 0 4)               # restart_log_open_count, reserved
	h=$h$(printf '%032d' 0)                    # pad to client array (offset 112)
	# LOG_CLIENT_RECORD at 112
	h=$h$(hexle "$out" 8)$(hexle "$out" 8)     # oldest_lsn, client_restart_lsn
	h=$h$(hexle 65535 2)$(hexle 65535 2)$(hexle 0 2)$(printf '%012d' 0) # prev, next, seq_number, reserved
	h=$h$(hexle 8 4)4e00540046005300           # client_name_length, "NTFS" UTF-16LE
	dd if=/dev/zero of="$_o" bs=4096 count=1 2>/dev/null
	printf '%s' "$h" | xxd -r -p | dd of="$_o" bs=1 conv=notrunc 2>/dev/null
	i=0
	while [ "$i" -lt 8 ]; do # multi sector fixups: last two bytes of every 512-byte sector = usn
		printf '\001\000' | dd of="$_o" bs=1 seek=$((510 + 512 * i)) conv=notrunc 2>/dev/null
		i=$((i + 1))
	done
}

inject_logfile() { # inject_logfile IMAGE : write two restart pages into $LogFile
	geometry "$1" || return 1
	lsize=$("$ntfscat" -f "$1" '$LogFile' 2>/dev/null | wc -c | tr -d ' ')
	[ "${lsize:-0}" = 2097152 ] || { echo "unexpected \$LogFile size: ${lsize:-?}" >&2; return 1; }
	build_page "$work/page0.bin" 0x1a2b3c4c5 # current_lsn of copy 0
	build_page "$work/page1.bin" 0x1a2b3c4d5 # current_lsn of copy 1, newer
	boff=$((g_loglcn * g_cs / 512))
	dd if="$work/page0.bin" of="$1" bs=512 seek="$boff" conv=notrunc 2>/dev/null || return 1
	dd if="$work/page1.bin" of="$1" bs=512 seek=$((boff + 8)) conv=notrunc 2>/dev/null || return 1
}

# ---- snapshots ----
# snap LABEL IMAGE : copy the image, then record the state. Sets s_* variables and a SNAP line.
snap() {
	label=$1
	cp "$2" "$work/snap-$label.img" || return 1
	s=$work/snap-$label.img
	geometry "$s" || { echo "snap $label: geometry failed"; return 1; }
	s_flags_F=$("$ntfsinfo" -f -F '$Volume' -v "$s" 2>/dev/null | sed -n 's/^[[:space:]]*Volume Flags:.*(\(0x[0-9a-fA-F]*\)).*/\1/p' | head -1)
	s_flags_m=$("$ntfsinfo" -f -m "$s" 2>/dev/null | sed -n 's/^[[:space:]]*Volume Flags:[[:space:]]*\(0x[0-9a-fA-F]*\).*/\1/p' | head -1)
	s_mft3=$(dd if="$s" bs=1024 skip=$((g_mftlcn * g_cs / 1024 + 3)) count=1 2>/dev/null | sha)
	"$ntfscat" -f "$s" '$LogFile' >"$work/logfile-$label.bin" 2>/dev/null
	s_logsize=$(wc -c <"$work/logfile-$label.bin" | tr -d ' ')
	s_logsha=$(sha <"$work/logfile-$label.bin")
	dd if="$s" of="$work/page-$label.bin" bs=512 skip=$((g_loglcn * g_cs / 512)) count=16 2>/dev/null
	s_pagesha=$(sha <"$work/page-$label.bin")
	s_rawsha=$(sha <"$work/page-$label.bin")
	s_catsha=$(head -c 8192 "$work/logfile-$label.bin" | sha)
	echo "---- snapshot $label ----"
	echo "  \$Volume flags: ntfsinfo -F '\$Volume' = $s_flags_F ; ntfsinfo -m = $s_flags_m"
	echo "  MFT record 3 sha256: $s_mft3"
	echo "  \$LogFile: size $s_logsize, sha256 (whole) $s_logsha"
	echo "  \$LogFile first 8192 bytes sha256: raw image $s_rawsha ; via ntfscat $s_catsha"
	s_summary="flags=$s_flags_F/$s_flags_m"
	for p in 0 1; do
		base=$((p * 4096))
		magic=$(od -An -v -tx1 -j "$base" -N 4 "$work/page-$label.bin" | tr -d ' \n')
		if [ "$magic" = 52535452 ]; then
			rao=$((0x$(rdle "$work/page-$label.bin" $((base + 24)) 2)))
			ra=$((base + rao))
			lsn=$(rdle "$work/page-$label.bin" "$ra" 8)
			cl=$(rdle "$work/page-$label.bin" $((ra + 8)) 2)
			fr=$(rdle "$work/page-$label.bin" $((ra + 10)) 2)
			iu=$(rdle "$work/page-$label.bin" $((ra + 12)) 2)
			fl=$(rdle "$work/page-$label.bin" $((ra + 14)) 2)
			oc=$(rdle "$work/page-$label.bin" $((ra + 40)) 4)
			ck=$(rdle "$work/page-$label.bin" $((base + 8)) 8)
			echo "  restart page $p (\$LogFile offset $base): magic RSTR, chkdsk_lsn 0x$ck, current_lsn 0x$lsn, log_clients 0x$cl, client_free_list 0x$fr, client_in_use_list 0x$iu, flags 0x$fl (RESTART_VOLUME_IS_CLEAN=0x0002), open_count 0x$oc"
			s_summary="$s_summary p$p=0x$lsn/0x$fl"
		else
			echo "  restart page $p (\$LogFile offset $base): no RSTR magic (first bytes $magic): empty log, no restart area, no current_lsn"
			s_summary="$s_summary p$p=none"
		fi
		dd if="$work/page-$label.bin" bs=1 skip="$base" count=$((0x120)) 2>/dev/null | xxd -o "$base" | sed 's/^/    /'
	done
	echo "  $("$ntfsinfo" -f -m "$s" 2>/dev/null | awk '/^File_Logfile Information/ { f = 1; next } f && /^[A-Za-z_]+.* Information/ { f = 0 } f && NF { sub(/^[ \t]+/, ""); printf "ntfsinfo -m File_Logfile: %s; ", $0 }')"
	eval "v_${label}_flags=\$s_flags_F"
	eval "v_${label}_mft3=\$s_mft3"
	eval "v_${label}_log=\$s_logsha"
	eval "v_${label}_page=\$s_pagesha"
	eval "v_${label}_summary=\$s_summary"
	echo "SNAP $label $s_summary mft3=$s_mft3 logfile=$s_logsha pages=$s_pagesha"
}

val() { eval "printf '%s' \"\${v_$1_$2:-}\""; }

same() { # same DESC FIELD LABEL... : FIELD identical in all labels
	d=$1
	f=$2
	shift 2
	first=$(val "$1" "$f")
	ok=1
	for l in "$@"; do [ "$(val "$l" "$f")" = "$first" ] || ok=0; done
	if [ "$ok" = 1 ] && [ -n "$first" ]; then pass "$d ($f identical in: $*)"; else fail "$d ($f differs in: $*)"; fi
}

# Unclean $LogFile (restart area patched: client in use, RESTART_VOLUME_IS_CLEAN cleared) on a copy:
# a norecover rw mount must not write the log (the driver falls back to read-only), and the
# positive control ntfsfix, which does reset the journal, must be visible to the detector.
unclean_case() { # unclean_case IMAGE
	ui=$work/unclean.img
	cp "$1" "$ui" || return 1
	geometry "$ui" || return 1
	lo=$((g_loglcn * g_cs))
	printf '\000\000' | dd of="$ui" bs=1 seek=$((lo + 62)) conv=notrunc 2>/dev/null
	printf '\000\000' | dd of="$ui" bs=1 seek=$((lo + 4096 + 62)) conv=notrunc 2>/dev/null
	echo
	echo "======== unclean-log case (copy of the synth image, flags cleared in both restart pages) ========"
	snap unclean_before "$ui"
	cur_img=$ui
	echo "+ $ntfs3g -o $mopts $ui $mnt   (messages below are the driver's own)"
	"$ntfs3g" -o "$mopts" "$ui" "$mnt"
	echo "  exit status: $? (ntfs-3g mount, norecover, unclean log)"
	wait_for "mount of $mnt" mounted
	mount | grep -F " on $mnt" | sed 's/^/  mount: /'
	if mounted && touch "$mnt/should-fail" 2>/dev/null; then
		fail "unclean: norecover mount accepts writes"
	else
		pass "unclean: norecover mount refuses writes or does not mount (log not repaired by the driver)"
	fi
	snap unclean_during "$ui"
	if mounted; then do_umount; fi
	snap unclean_after "$ui"
	cp "$ui" "$work/uc-control.img"
	run "ntfsfix (control on the unclean copy)" "$ntfsfix" "$work/uc-control.img"
	snap unclean_control "$work/uc-control.img"
	rm -f "$work/uc-control.img"
	same "unclean U13: restart area byte-identical across a norecover rw mount attempt" page unclean_before unclean_during unclean_after
	same "unclean U13: whole \$LogFile byte-identical" log unclean_before unclean_during unclean_after
	same "unclean U9: \$Volume flags unchanged" flags unclean_before unclean_during unclean_after
	if [ "$(val unclean_control log)" != "$(val unclean_before log)" ]; then
		pass "unclean control: ntfsfix resets \$LogFile and the detector sees it"
	else
		fail "unclean control: detector did not see ntfsfix \$LogFile reset"
	fi
}

run_fixture() { # run_fixture NAME (empty|synth)
	fx=$1
	img=$work/$fx.img
	echo
	echo "======== fixture $fx ========"
	dd if=/dev/null of="$img" bs=1048576 seek=32 2>/dev/null
	run "mkntfs" "$mkntfs" -F -Q -L "dfc-$fx" "$img" >"$work/mkntfs-$fx.log" 2>&1 || { cat "$work/mkntfs-$fx.log"; die "mkntfs failed"; }
	if [ "$fx" = synth ]; then
		inject_logfile "$img" || die "cannot inject synthetic restart pages"
		echo "synthetic restart pages written into \$LogFile (current_lsn 0x1a2b3c4c5 and 0x1a2b3c4d5, flags 0x0002 clean)"
	fi
	snap "${fx}_before" "$img" || die "snapshot failed"

	# Session 1: clean unmount.
	if do_mount "$img"; then pass "$fx: rw mount (-o $mopts)"; else fail "$fx: rw mount"; return 1; fi
	if write_files s1; then pass "$fx: session 1 writes"; else fail "$fx: session 1 writes"; fi
	snap "${fx}_during" "$img"
	n=$("$ntfsls" "$work/snap-${fx}_during.img" -p /dir-s1 2>/dev/null | wc -l | tr -d ' ')
	echo "  files visible in the DURING copy: $n (expected 42: the dot entry, blob.bin, renamed.txt, small-1..39)"
	if [ "$n" = 42 ]; then pass "$fx: DURING copy contains the session's writes"; else fail "$fx: DURING copy contains the session's writes ($n)"; fi
	if do_umount; then pass "$fx: clean unmount and driver exit"; else fail "$fx: clean unmount"; fi
	snap "${fx}_afterclean" "$img"

	# Session 2: kill -9.
	if do_mount "$img"; then pass "$fx: rw mount, session 2"; else fail "$fx: rw mount, session 2"; return 1; fi
	if write_files s2; then pass "$fx: session 2 writes"; else fail "$fx: session 2 writes"; fi
	snap "${fx}_during2" "$img"
	pids=$(driver_pids)
	echo "driver pid(s): $pids"
	# shellcheck disable=SC2086
	kill -9 $pids
	echo "  exit status: $? (kill -9 $pids)"
	if wait_for "driver to be gone" no_driver; then pass "$fx: driver killed"; else fail "$fx: driver killed"; fi
	force_umount
	if wait_for "mount to be gone" sh -c '! mount | grep -F " on '"$mnt"'" >/dev/null'; then pass "$fx: stale mount removed"; else fail "$fx: stale mount removed"; fi
	snap "${fx}_afterkill" "$img"

	# Remount after kill -9: must mount rw with norecover and accept writes.
	if do_mount "$img"; then pass "$fx: remount rw norecover after kill -9"; else fail "$fx: remount rw norecover after kill -9"; fi
	if mounted; then
		if write_files s3; then pass "$fx: write after remount"; else fail "$fx: write after remount"; fi
		if [ -f "$mnt/dir-s2/renamed.txt" ]; then pass "$fx: session 2 data readable after kill -9"; else fail "$fx: session 2 data readable after kill -9"; fi
		do_umount || fail "$fx: unmount after remount"
	fi
	cp "$img" "$work/check.img"
	if run "ntfsfix -n after kill -9 (on a copy)" "$ntfsfix" -n "$work/check.img"; then pass "$fx: ntfsfix -n passes after kill -9"; else fail "$fx: ntfsfix -n passes after kill -9"; fi
	rm -f "$work/check.img"
	snap "${fx}_final" "$img"

	# Positive control: ntfsfix without -n on a copy must be visible to the detector.
	cp "$img" "$work/control.img"
	run "ntfsfix (control, copy of the final image)" "$ntfsfix" "$work/control.img"
	snap "${fx}_control" "$work/control.img"
	rm -f "$work/control.img"

	if [ "$fx" = synth ]; then
		unclean_case "$img" || fail "synth: unclean-log case could not run"
	fi

	echo
	echo "---- $fx: assertions ----"
	all="${fx}_before ${fx}_during ${fx}_afterclean ${fx}_during2 ${fx}_afterkill ${fx}_final"
	# shellcheck disable=SC2086
	same "$fx U9: \$Volume flags never change" flags $all
	for l in $all; do
		if [ "$(val "$l" flags)" = 0x0000 ]; then pass "$fx U9: $l has VOLUME_IS_DIRTY clear (flags $(val "$l" flags))"; else fail "$fx U9: $l flags $(val "$l" flags)"; fi
	done
	# shellcheck disable=SC2086
	same "$fx U9: \$Volume MFT record unchanged by sessions and kill -9" mft3 $all
	# shellcheck disable=SC2086
	same "$fx U13: restart area (first 8192 bytes of \$LogFile) byte-identical" page $all
	# shellcheck disable=SC2086
	same "$fx U13: whole \$LogFile byte-identical" log $all
	c=$(val "${fx}_control" flags)
	if [ "$c" = 0x0001 ]; then pass "$fx control: ntfsfix sets VOLUME_IS_DIRTY, detector sees it (flags $c)"; else fail "$fx control: detector did not see ntfsfix dirty flag ($c)"; fi
	if [ "$fx" = synth ]; then
		case "$(val "${fx}_before" summary)" in *p0=0x00000001a2b3c4c5/0x0002*p1=0x00000001a2b3c4d5/0x0002*) pass "$fx: before-state has the intended current_lsn and clean flag" ;; *) fail "$fx: before-state restart area not as intended: $(val "${fx}_before" summary)" ;; esac
	fi
}

echo "dirtyflag_check: env=$envname user=$(id -un) work=$work"
"$ntfs3g" --version | head -1
run_fixture empty
run_fixture synth

echo
if [ "$fails" = 0 ]; then
	echo "T-M3-01 dirtyflag_check ($envname): PASS"
	exit 0
fi
echo "T-M3-01 dirtyflag_check ($envname): FAIL ($fails check(s) failed)"
exit 1
