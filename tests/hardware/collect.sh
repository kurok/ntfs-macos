#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# collect.sh - Environment C (docs/HARDWARE_TEST_PROTOCOL.md): read-only snapshot of one physical NTFS volume as
# the Mac sees it, for the "before" and "after" records of H-USB-03, 08, 09, 10 and the other cases.
#
# Usage: tests/hardware/collect.sh --disk DISK --slice SLICE --tag TAG --out DIR [--serial TEXT] [--copy [--copy-dir DIR]]
#   --disk DISK    whole-disk BSD name (disk4)
#   --slice SLICE  the NTFS partition (disk4s1); must belong to DISK
#   --tag TAG      short name of this snapshot, letters, digits, dot, dash, for example 08-before
#   --out DIR      output directory for TAG.txt (the protocol passes "$EV")
#   --serial TEXT  passed to identify.sh; the snapshot is refused unless it matches
#   --copy         also copy the NTFS VOLUME (total_sectors + 1 sectors of the slice: the volume and its backup boot
#                  sector, not the rest of the partition) to COPYDIR/TAG.img, a READ of the device node into a file,
#                  and print its SHA-256, as evidence for later analysis with logfile_mark --dump and setvolflags
#                  --show in Environment A. The size comes from the probe; if the probe gives none, nothing is copied.
#   --copy-dir DIR where the copy goes (default $HOME/hwimages: outside the repository; `*.img` is also ignored by
#                  .gitignore, but disk images are never committed)
#
# What it runs (nothing writes to the device): identify.sh; `ntfsmac list` and `ntfsmac status SLICE`;
# `diskutil info SLICE`; `mount` for the slice; the journal directory listing and entries, read by a root shell
# (the directory is root-only, so the entry's `logfile_lsn_at_start` is recorded); `ntfsmac-probe --fd 3` on
# /dev/rSLICE opened read-only by a root shell (the probe prints the volume flags, the $LogFile state and
# `logfile_lsn`, which is `current_lsn`); with --copy, `dd if=/dev/rSLICE of=FILE`.
# It asks for sudo. It never mounts, unmounts, ejects or repairs anything, and never opens a device for writing.
# The host name is written as <host>. The probe is the product's own read-only reader (ARCHITECTURE.md section 3.1);
# it may be run while the volume is mounted (U18: /dev/rdiskN opens read-only).
# Exit: 0 snapshot written; 1 refused or a step failed (the file says which); 2 usage error.
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$here")")
PROBE=${NTFSMAC_PROBE:-/usr/local/libexec/ntfs-macos/ntfsmac-probe}
CLI=${NTFSMAC_CLI:-/usr/local/libexec/ntfs-macos/ntfsmac}
JOURNAL=/var/db/ntfs-macos/journal

usage() { printf 'usage: %s --disk DISK --slice SLICE --tag TAG --out DIR [--serial TEXT] [--copy [--copy-dir DIR]]\n' "$0" >&2; exit 2; }

disk=
slice=
tag=
serial=
out=
copy=0
copydir=$HOME/hwimages
while [ $# -gt 0 ]; do
	case $1 in
	--disk) [ $# -ge 2 ] || usage; disk=$2; shift ;;
	--slice) [ $# -ge 2 ] || usage; slice=$2; shift ;;
	--tag) [ $# -ge 2 ] || usage; tag=$2; shift ;;
	--serial) [ $# -ge 2 ] || usage; serial=$2; shift ;;
	--out) [ $# -ge 2 ] || usage; out=$2; shift ;;
	--copy) copy=1 ;;
	--copy-dir) [ $# -ge 2 ] || usage; copydir=$2; shift ;;
	*) usage ;;
	esac
	shift
done
[ -n "$disk" ] && [ -n "$slice" ] && [ -n "$tag" ] && [ -n "$out" ] || usage
case $disk in disk[0-9] | disk[0-9][0-9] | disk[0-9][0-9][0-9]) ;; *) usage ;; esac
case $slice in "${disk}"s[0-9] | "${disk}"s[0-9][0-9]) ;; *) printf 'collect: %s is not a slice of %s\n' "$slice" "$disk" >&2; exit 2 ;; esac
case $tag in *[!A-Za-z0-9._-]* | "") printf 'collect: bad tag\n' >&2; exit 2 ;; esac
case $out in "$repo"/* | /*) ;; *) out=$PWD/$out ;; esac
mkdir -p "$out" || exit 1
file=$out/$tag.txt
rc=0
sudo -v || { printf 'collect: sudo is needed\n' >&2; exit 1; }

section() { printf '\n== %s ==\n' "$1"; }
run() { printf '$ %s\n' "$*"; "$@" 2>&1 || { printf '[exit %s]\n' "$?"; rc=1; }; }

(
	printf 'tag: %s\nutc: %s\nhost: <host>\nmacOS: %s %s\ndisk: %s\nslice: %s\n' "$tag" "$(date -u +%Y-%m-%dT%H:%M:%SZ)" \
		"$(sw_vers -productVersion)" "$(sw_vers -buildVersion)" "$disk" "$slice"
	printf 'commit: %s\n' "$(git -C "$repo" rev-parse HEAD 2>/dev/null || echo unknown)"

	section identify
	if [ -n "$serial" ]; then
		sh "$here/identify.sh" "$disk" --serial "$serial" || { printf 'REFUSED by identify.sh\n'; exit 1; }
	else
		sh "$here/identify.sh" "$disk" || { printf 'REFUSED by identify.sh\n'; exit 1; }
	fi

	section "ntfsmac list"
	run "$CLI" list
	section "ntfsmac status $slice"
	run "$CLI" status "$slice"
	section "diskutil info $slice"
	run diskutil info "$slice"
	section "mount"
	mount | grep -F "/dev/$slice " || echo "(not mounted)"

	section "journal (read by a root shell; the directory is root-only)"
	sudo sh -c '
		d=$1
		ls -l "$d" 2>&1
		for f in "$d"/*; do
			[ -f "$f" ] || continue
			printf "%s\n" "$f"
			cat "$f"
			printf "\n"
		done' sh "$JOURNAL" 2>&1

	section "probe (read-only), raw"
	probe_out=$(sudo sh -c 'exec "$1" --fd 3 --bsd-name "$2" 3<"/dev/r$2"' sh "$PROBE" "$slice" 2>&1)
	prc=$?
	printf '%s\n[probe exit %s]\n' "$probe_out" "$prc"
	[ "$prc" -eq 0 ] || rc=1
	section "probe summary"
	printf '%s\n' "$probe_out" | python3 -I -c '
import json, sys
try:
    v = json.loads(sys.stdin.read())
except ValueError:
    print("probe output is not JSON"); sys.exit(0)
for k in ("state", "reason_codes", "volume_flags", "logfile", "logfile_lsn", "hiberfile", "bitlocker",
          "label", "serial", "total_sectors", "sector_size", "nonnfc_count"):
    print("%s: %s" % (k, v.get(k)))
'

	if [ "$copy" -eq 1 ]; then
		section "copy of the NTFS volume"
		vol=$(printf '%s\n' "$probe_out" | python3 -I -c '
import json, sys
try:
    v = json.loads(sys.stdin.read())
    print(int(v["total_sectors"]) * int(v["sector_size"]) + int(v["sector_size"]))
except (ValueError, KeyError, TypeError):
    pass
')
		if [ -z "$vol" ] || [ "$vol" -gt 42949672960 ]; then
			echo "refused: volume size '$vol' is unknown or above 40 GiB"
			rc=1
		else
			mkdir -p "$copydir" || exit 1
			sudo dd if="/dev/r$slice" of="$copydir/$tag.img" bs=512 count=$((vol / 512)) 2>&1
			shasum -a 256 "$copydir/$tag.img"
		fi
	fi
	exit $rc
) >"$file" 2>&1
rc=$?
cat "$file"
printf '\ncollect: wrote %s (exit %s)\n' "$file" "$rc"
exit "$rc"
