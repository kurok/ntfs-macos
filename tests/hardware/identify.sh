#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# identify.sh - Environment C (docs/HARDWARE_TEST_PROTOCOL.md): show which physical disk a BSD name is and
# refuse it unless it looks like the disposable external disk. READ-ONLY: it runs diskutil, ioreg, system_profiler
# and tmutil queries and nothing else; it never opens a device node and never writes anything.
#
# Usage: tests/hardware/identify.sh DISK [--serial TEXT]
#        tests/hardware/identify.sh --parse-serial DISK < ioreg-output     (parser self-test, used by the protocol)
#   DISK           the whole-disk BSD name, for example disk4 (not a slice, not a path)
#   --serial TEXT  the USB serial number the operator wrote down for the disposable disk; the script then exits 1
#                  unless the serial found for DISK is exactly TEXT (an empty or "Not Provided" serial never matches)
# Exit: 0 DISK is external, physical, not the boot disk (nor the physical store of it), not a Time Machine
#       destination, at most 512 GB (and the serial matches, if given); 1 refused (the reason is printed); 2 usage.
# The USB serial is found per USB device: `ioreg -l -w0 -r -c IOUSBHostDevice` prints each device as one tree; the
# serial ("USB Serial Number") and the "BSD Name" of its media are taken from the SAME tree, and nothing is carried
# from one device to the next. SPUSBDataType no longer exists on macOS 27; `system_profiler -json SPUSBHostDataType`
# is the fallback [U: the layout of both outputs for a storage device was not seen on a Mac with a stick attached;
# the parser is tested on a hand-made sample only. If the serial is NOT FOUND, read it from the label or from
# System Information > USB and do not continue with --serial until the script finds it].
set -u
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

die() { printf 'identify: REFUSED: %s\n' "$*" >&2; exit 1; }
usage() { printf 'usage: %s DISK [--serial TEXT]\n' "$0" >&2; exit 2; }

# serial_from_ioreg DISK < text: the serial of the USB device whose tree holds BSD Name = DISK.
serial_from_ioreg() {
	awk -v d="$1" '
	function flush() { if (found && sn != "") { print sn; done = 1; exit } }
	/^\+-o / { flush(); sn = ""; found = 0; next }
	/"USB Serial Number" = / && sn == "" {
		s = $0; sub(/^.*"USB Serial Number" = "/, "", s); sub(/".*$/, "", s); sn = s
	}
	/"BSD Name" = / {
		n = $0; sub(/^.*"BSD Name" = "/, "", n); sub(/".*$/, "", n); if (n == d) found = 1
	}
	END { if (!done) flush() }'
}

if [ "${1:-}" = "--parse-serial" ]; then
	[ $# -eq 2 ] || usage
	serial_from_ioreg "$2"
	exit 0
fi

[ $# -ge 1 ] || usage
disk=$1
shift
want=
while [ $# -gt 0 ]; do
	case $1 in
	--serial) [ $# -ge 2 ] || usage; want=$2; shift ;;
	*) usage ;;
	esac
	shift
done
case $disk in
disk[0-9] | disk[0-9][0-9] | disk[0-9][0-9][0-9]) ;;
*) printf 'identify: %s is not a whole-disk BSD name like disk4\n' "$disk" >&2; exit 2 ;;
esac

info=$(diskutil info "$disk" 2>&1) || die "diskutil cannot describe $disk: $info"
field() { printf '%s\n' "$info" | awk -v k="$1" 'match($0, "^ *" k ": *") { print substr($0, RLENGTH + 1); exit }'; }

loc=$(field "Device Location")
rem=$(field "Removable Media")
proto=$(field "Protocol")
media=$(field "Device / Media Name")
virt=$(field "Virtual")
whole=$(field "Whole")
bytes=$(printf '%s\n' "$info" | sed -n 's/.*Disk Size:.*(\([0-9][0-9]*\) Bytes).*/\1/p' | head -1)
[ -n "$bytes" ] || bytes=$(printf '%s\n' "$info" | sed -n 's/.*Total Size:.*(\([0-9][0-9]*\) Bytes).*/\1/p' | head -1)

# The disk(s) under the running system: the APFS physical store of / (disk0s2 -> disk0), else its whole disk.
rootinfo=$(diskutil info / 2>/dev/null)
boot_whole=$(printf '%s\n' "$rootinfo" | awk -F': *' '/APFS Physical Store:/ { print $2 }' | sed 's/s[0-9][0-9]*$//')
[ -n "$boot_whole" ] || boot_whole=$(printf '%s\n' "$rootinfo" | awk -F': *' '/Part of Whole:/ { print $2; exit }')
[ -n "$boot_whole" ] || die "cannot tell which disk holds the running system; will not continue"

printf 'disk:        %s\n' "$disk"
printf 'media name:  %s\n' "${media:-?}"
printf 'location:    %s\n' "${loc:-?}"
printf 'protocol:    %s\n' "${proto:-?}"
printf 'removable:   %s\n' "${rem:-?}"
printf 'size bytes:  %s\n' "${bytes:-?}"
printf 'boot disk:   %s\n' "$boot_whole"

[ "$whole" = Yes ] || die "$disk is not a whole disk"
for b in $boot_whole; do [ "$disk" != "$b" ] || die "$disk holds the running system"; done
[ "$loc" = External ] || die "$disk is not an external disk (Device Location: ${loc:-unknown})"
case $proto in
USB | Thunderbolt | "SATA" | "Secure Digital" | "Fibre Channel") ;; # [U] which bridges report what
"") die "no protocol reported for $disk" ;;
*) die "protocol '$proto' is not an expected external bus" ;;
esac
case $virt in Yes) die "$disk is a virtual disk" ;; esac
[ -n "$bytes" ] || die "cannot read the size of $disk"
[ "$bytes" -le 549755813888 ] || die "$disk is larger than 512 GB; the disposable media are smaller"

# A Time Machine or other backup destination on this disk: refuse.
tm=$(tmutil destinationinfo 2>/dev/null | awk -F': *' '/Mount Point/ { print $2 }')
if [ -n "$tm" ]; then
	for s in $(diskutil list "$disk" 2>/dev/null | awk '/disk[0-9]+s[0-9]+$/ { print $NF }'); do
		mp=$(diskutil info "$s" 2>/dev/null | awk -F': *' '/Mount Point/ { print $2; exit }')
		[ -n "$mp" ] || continue
		printf '%s\n' "$tm" | grep -qxF "$mp" && die "$s ($mp) is a Time Machine destination"
	done
fi

serial=$(ioreg -l -w0 -r -c IOUSBHostDevice 2>/dev/null | serial_from_ioreg "$disk")
if [ -z "$serial" ]; then
	serial=$(system_profiler -json SPUSBHostDataType 2>/dev/null | python3 -I -c '
import json, sys
want = sys.argv[1]
def has_bsd(o):
    if isinstance(o, dict):
        return o.get("bsd_name") == want or any(has_bsd(v) for v in o.values())
    if isinstance(o, list):
        return any(has_bsd(v) for v in o)
    return False
def walk(o):
    if isinstance(o, dict):
        if o.get("serial_num") and has_bsd(o):
            return o["serial_num"]
        for v in o.values():
            r = walk(v)
            if r: return r
    elif isinstance(o, list):
        for v in o:
            r = walk(v)
            if r: return r
try:
    print(walk(json.load(sys.stdin)) or "")
except ValueError:
    pass
' "$disk")
fi
case $serial in "Not Provided" | "not provided" | "None" | "0" | "") serial= ;; esac
printf 'serial:      %s\n' "${serial:-NOT FOUND}"
if [ -n "$want" ]; then
	[ -n "$want" ] && [ "${#want}" -ge 4 ] || die "the expected serial is empty or too short"
	[ -n "$serial" ] || die "no USB serial found for $disk; compare by hand and do not continue until it is certain"
	[ "$serial" = "$want" ] || die "serial of $disk is '$serial', expected '$want'"
	printf 'serial matches the one given.\n'
fi
printf 'OK: %s looks like an external disk. The operator must still confirm that it is THE disposable one.\n' "$disk"
exit 0
