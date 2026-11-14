#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fixture generator v1 (T-M3-04): unsafe-state NTFS images, docs/TEST_STRATEGY.md 3.2.
#
# Usage: tests/fixtures/gen/unsafe.sh [--only NAME] [--export DIR] [--print]
#
# Run on the host: the script re-runs itself inside Environment A
# (vm/linux/run.sh, debian:13-slim with the pinned NTFS-3G and ntfsprogs).
# Images are made and mounted ONLY inside the container; nothing is attached or
# mounted on the host. Images live in the container's /tmp and vanish with it.
# Use --export DIR to copy images and logs to the host (gzip stream, extracted
# under DIR, which should be below the git-ignored build/). Images are never
# committed.
#
# For every entry of tests/fixtures/manifest.json whose generator is "unsafe":
#   1. mkntfs an empty image of the given size, cluster and sector size (-T fixes
#      the time mkntfs seeds its serial and GUIDs from; this is the "empty"
#      clean-64m-4k row of TEST_STRATEGY 3.1, not the seeded manifest entry of
#      that name),
#   2. apply the entry's "recipe" with the tools of tests/tools (setvolflags,
#      logfile_mark, txf_make, hiber_make.sh) or plain dd/truncate,
#   3. validate against the pinned NTFS-3G, always with "norecover":
#        - "-o norecover,ro" on the image itself; the image SHA-256 must not
#          change,
#        - "-o norecover" (read/write request) on a COPY, which may be changed by
#          the session; the result is classified as mounts-rw, mounts-ro
#          (engine fell back to read-only) or fails, and must equal the
#          manifest's linux.ro / linux.rw "result" and contain its "msg".
#      Also checked on the copy: $Volume flags and the $LogFile restart pages are
#      the same after the session (NTFS-3G neither sets the dirty flag nor writes
#      the log with norecover), and for after_rw "txf-resident" the root's
#      $TXF_DATA is resident again (fix_txf_data(), volume.c).
# --print prints the observed result line of every fixture and does not compare
# (use it to fill in the manifest after changing a fixture).
#
# The expected probe verdicts in the manifest are for T-M3-02; nothing here checks
# them. Logs (exact engine messages) go to NTFSMAC_FIXTURE_OUT/NAME.{make,ro,rw}.log
# and a summary to NTFSMAC_FIXTURE_OUT/unsafe-results.txt.
# Env: NTFSMAC_FIXTURE_OUT (container output dir, default /tmp/ntfsmac-unsafe).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
manifest=tests/fixtures/manifest.json

die() { printf 'unsafe.sh: error: %s\n' "$*" >&2; exit 1; }

only=
export_dir=
print_only=0
inner=0
tar_out=0
while [ $# -gt 0 ]; do
	case $1 in
	--only) [ $# -ge 2 ] || die "--only needs a name"; only=$2; shift ;;
	--export) [ $# -ge 2 ] || die "--export needs a dir"; export_dir=$2; shift ;;
	--print) print_only=1 ;;
	--inner) inner=1 ;;
	--tar) tar_out=1 ;;
	-h | --help) sed -n '4,40p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

if [ "$inner" -eq 0 ]; then
	# Host side: hand over to Environment A.
	args="--inner"
	[ -z "$only" ] || args="$args --only $only"
	[ "$print_only" -eq 0 ] || args="$args --print"
	if [ -n "$export_dir" ]; then
		mkdir -p "$export_dir"
		# shellcheck disable=SC2086
		sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/unsafe.sh $args --tar |
			tar -xzf - -C "$export_dir"
		exit 0
	fi
	# shellcheck disable=SC2086
	exec sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/unsafe.sh $args
fi

# ---- inside the container from here on ----
work=/work
out=${NTFSMAC_FIXTURE_OUT:-/tmp/ntfsmac-unsafe}
mnt=/tmp/ntfsmac-unsafe.mnt
bin=/tmp/ntfsmac-unsafe-bin
prefix=${NTFS3G_PREFIX:-/opt/ntfs-3g}
for t in mkntfs ntfs-3g ntfsinfo python3 cc sha256sum; do
	command -v "$t" >/dev/null 2>&1 || die "$t not found: run through Environment A"
done
mkdir -p "$out" "$mnt" "$bin"

# stdout carries the tar stream with --tar, so all messages go to stderr then.
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

log() { printf '%s\n' "$*"; }

# The ASan container (run.sh --asan) carries an ASan build of libntfs-3g: the
# tools need the runtime too.
san=
if [ "${NTFS_ASAN:-0}" = 1 ]; then san="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
for t in setvolflags logfile_mark txf_make; do
	# shellcheck disable=SC2086  # $san is a word list
	cc -std=c17 -O2 -Wall -Wextra -Werror $san -isystem "$prefix/include" \
		-o "$bin/$t" "$work/tests/tools/$t.c" -L"$prefix/lib" -lntfs-3g ||
		die "building $t failed"
done
"$bin/logfile_mark" --selftest >/dev/null || die "logfile_mark --selftest failed"

python3 -I -c '
import json
import sys

m = json.load(open(sys.argv[1]))
for e in m["fixtures"]:
    if e.get("generator") != "unsafe":
        continue
    lx = e["linux"]
    print("\t".join(str(x) for x in (
        e["name"], e["size_bytes"], e["cluster_size"], e["sector_size"],
        e["label"], e["recipe"],
        lx["ro"]["result"], lx["ro"].get("msg", "") or "-",
        lx["rw"]["result"], lx["rw"].get("msg", "") or "-",
        lx.get("after_rw", "") or "-",
        e["probe"].get("logfile_lsn", "") or "-",
        lx["rw"].get("msg2", "") or "-")))
' "$work/$manifest" >"$bin/fixtures.tsv"

mounted=0
# shellcheck disable=SC2329  # invoked through trap
cleanup() {
	if [ "$mounted" -eq 1 ]; then umount "$mnt" 2>/dev/null || true; fi
}
trap cleanup EXIT HUP INT TERM

# make_image NAME SIZE CLUSTER SECTOR LABEL RECIPE: build $out/NAME.img
make_image() {
	_n=$1 _size=$2 _cl=$3 _ss=$4 _label=$5 _recipe=$6
	_img=$out/$_n.img
	_log=$out/$_n.make.log
	rm -f "$_img"
	truncate -s "$_size" "$_img"
	# -T: fake time 0 so mkntfs' time-seeded serial and GUIDs are fixed.
	mkntfs -F -Q -T -L "$_label" -c "$_cl" -s "$_ss" "$_img" >"$_log" 2>&1 ||
		{ cat "$_log"; die "mkntfs failed for $_n"; }
	# Every multi-command branch below is an && chain: inside this brace group
	# (the left side of ||) set -e is off, so only the last command would count.
	{
		printf 'recipe: %s\n' "$_recipe"
		case $_recipe in
		flags:*) "$bin/setvolflags" "$_img" --set "${_recipe#flags:}" ;;
		log:init) "$bin/logfile_mark" "$_img" --init ;;
		log:unclean) "$bin/logfile_mark" "$_img" --init --unclean ;;
		log:v2) "$bin/logfile_mark" "$_img" --init --v2 ;;
		log:bumped)
			# a clean log first, then the bump as a second, separate edit
			"$bin/logfile_mark" "$_img" --init &&
				"$bin/logfile_mark" "$_img" --bump-lsn
			;;
		hiber:*) sh "$work/tests/tools/hiber_make.sh" "$_img" "${_recipe#hiber:}" "$mnt" ;;
		txf) "$bin/txf_make" "$_img" ;;
		bitlocker)
			# 64 MiB of zeros with the BitLocker OEM ID at offset 3 of sector 0.
			truncate -s 0 "$_img" && truncate -s "$_size" "$_img" &&
				printf -- '-FVE-FS-' | dd of="$_img" bs=1 seek=3 conv=notrunc 2>/dev/null
			;;
		truncated:*)
			# PCT percent of the image, cut at a sector boundary. $MFTMirr is at
			# 50 percent (LCN 8191 of 16383): a cut below it makes ntfs_mount()
			# fail, a cut above it still mounts.
			_cut=$((_size * ${_recipe#truncated:} / 100 / _ss * _ss))
			truncate -s "$_cut" "$_img" && printf 'cut to %s bytes\n' "$_cut"
			;;
		empty) ;;
		*) die "unknown recipe: $_recipe" ;;
		esac
	} >>"$_log" 2>&1 || { cat "$_log"; die "recipe $_recipe failed for $_n"; }
}

# try_mount IMG OPTS LOGFILE: sets m_rc, m_state (rw|ro|none), m_msg.
try_mount() {
	m_rc=0
	m_state=none
	ntfs-3g "$1" "$mnt" -o "$2" >"$3" 2>&1 || m_rc=$?
	if grep -q " $mnt " /proc/mounts; then
		mounted=1
		_opt=$(awk -v m="$mnt" '$2 == m { split($4, a, ","); print a[1] }' /proc/mounts)
		_w=no
		if touch "$mnt/.write-test" 2>/dev/null; then _w=yes; rm -f "$mnt/.write-test"; fi
		if [ "$_opt" = rw ] && [ "$_w" = yes ]; then m_state=rw; else m_state=ro; fi
		umount "$mnt" || die "umount failed"
		mounted=0
	fi
	m_msg=$(tr '\n' ' ' <"$3" | sed 's/  */ /g; s/ $//')
}

# check_result WHAT GOT WANT_RESULT WANT_MSG: record a mismatch in $bad.
check_result() {
	_what=$1 _got=$2 _wr=$3 _wm=$4
	if [ "$_got" != "$_wr" ]; then
		log "MISMATCH $_what result: got $_got want $_wr"
		bad=1
	fi
	if [ "$_wm" != "-" ] && ! printf '%s' "$m_msg" | grep -qF -- "$_wm"; then
		log "MISMATCH $_what message: want to contain: $_wm"
		bad=1
	fi
}

# classify: mounts-rw | mounts-ro | fails
classify() { if [ "$m_state" = none ]; then echo fails; else echo "mounts-$m_state"; fi; }

# log_state IMG: restart pages and engine verdict, or "n/a"
log_state() { "$bin/logfile_mark" "$1" --dump 2>/dev/null || echo "log=n/a"; }
flags_state() {
	_f=$("$bin/setvolflags" "$1" --show 2>/dev/null | sed 's/ mft=.*//; s/^flags_before=0x[0-9a-f]* //')
	echo "${_f:-n/a}"
}

: >"$out/unsafe-results.txt"
export LC_ALL=C.UTF-8
tab=$(printf '\t')
rc=0
count=0
while IFS=$tab read -r name size cluster sector label recipe ro_res ro_msg rw_res rw_msg after_rw want_lsn rw_msg2; do
	[ -n "$name" ] || continue
	if [ -n "$only" ] && [ "$only" != "$name" ]; then continue; fi
	count=$((count + 1))
	bad=0
	make_image "$name" "$size" "$cluster" "$sector" "$label" "$recipe"
	img=$out/$name.img

	# 1. read-only request on the image itself: nothing may change
	h0=$(sha256sum "$img" | cut -d' ' -f1)
	try_mount "$img" norecover,ro "$out/$name.ro.log"
	ro_got=$(classify)
	ro_rc=$m_rc ro_text=$m_msg
	h1=$(sha256sum "$img" | cut -d' ' -f1)
	[ "$h0" = "$h1" ] || { log "MISMATCH $name: image changed by a read-only mount"; bad=1; }
	[ "$print_only" -eq 1 ] || check_result "$name ro" "$ro_got" "$ro_res" "$ro_msg"

	# 2. read/write request on a copy
	cp "$img" "$img.rw"
	log0=$(log_state "$img.rw")
	flags0=$(flags_state "$img.rw")
	try_mount "$img.rw" norecover "$out/$name.rw.log"
	rw_got=$(classify)
	rw_rc=$m_rc rw_text=$m_msg
	if [ "$print_only" -eq 0 ]; then
		check_result "$name rw" "$rw_got" "$rw_res" "$rw_msg"
		check_result "$name rw" "$rw_got" "$rw_res" "$rw_msg2"
		if [ "$want_lsn" != "-" ]; then
			dump=$("$bin/logfile_mark" "$img" --dump 2>/dev/null)
			got_lsn=$(printf '%s\n' "$dump" | sed -n 's/^current_lsn=//p')
			# an empty log has no restart page: the probe reports LSN 0
			if [ -z "$got_lsn" ] && [ "$(printf '%s\n' "$dump" | sed -n 's/^log=//p')" = empty ]; then
				got_lsn=0x0000000000000000
			fi
			[ "$got_lsn" = "$want_lsn" ] || { log "MISMATCH $name: current_lsn $got_lsn, want $want_lsn"; bad=1; }
		fi
	fi
	extra=
	if [ "$rw_got" != fails ]; then
		# n/a means the dump failed: the before/after comparison would prove nothing
		[ "$log0" != "log=n/a" ] && [ "$flags0" != "n/a" ] ||
			{ log "MISMATCH $name: \$LogFile or \$Volume state could not be read"; bad=1; }
		[ "$(log_state "$img.rw")" = "$log0" ] || { log "MISMATCH $name: \$LogFile restart pages changed by the session"; bad=1; }
		[ "$(flags_state "$img.rw")" = "$flags0" ] || { log "MISMATCH $name: \$Volume flags changed by the session"; bad=1; }
		if [ "$after_rw" = txf-resident ]; then
			res=$(ntfsinfo -F / -f "$img.rw" 2>/dev/null |
				awk '/LOGGED_UTILITY_STREAM/ { f = 1 } f && /Resident:/ { print $2; exit }')
			extra=" txf_resident_after_rw=${res:-?}"
			[ "$res" = Yes ] || { log "MISMATCH $name: \$TXF_DATA not resident after the read/write session"; bad=1; }
			res=$(ntfsinfo -F / -f "$img" 2>/dev/null |
				awk '/LOGGED_UTILITY_STREAM/ { f = 1 } f && /Resident:/ { print $2; exit }')
			extra="$extra txf_resident_after_ro=${res:-?}"
			[ "$res" = No ] || { log "MISMATCH $name: \$TXF_DATA changed by the read-only mount"; bad=1; }
		fi
	fi
	rm -f "$img.rw"

	line="$name ro=$ro_got(rc=$ro_rc) rw=$rw_got(rc=$rw_rc)$extra"
	{
		printf '%s\n' "$line"
		printf '  ro msg: %s\n' "${ro_text:--}"
		printf '  rw msg: %s\n' "${rw_text:--}"
		printf '  flags: %s\n  log: %s\n' "$flags0" "$(printf '%s' "$log0" | tr '\n' ' ')"
	} >>"$out/unsafe-results.txt"
	if [ "$print_only" -eq 1 ]; then
		log "$line"
	elif [ "$bad" -eq 0 ]; then
		log "ok       $line"
	else
		log "MISMATCH $line"
		rc=1
	fi
done <"$bin/fixtures.tsv"
[ "$count" -gt 0 ] || die "no fixture generated (unknown --only name?)"
if [ "$tar_out" -eq 1 ]; then
	tar -czf - -C "$out" . >&3
fi
exit "$rc"
