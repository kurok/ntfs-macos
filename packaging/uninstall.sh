#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Remove an installed ntfs-macos package. Deletes only files the package receipt
# (org.ntfs-macos.pkg) lists, and only below the two directories the package owns;
# any other path in the receipt stops the script before anything is removed.
# Does not touch macFUSE or any NTFS volume.
#
# Usage: sudo uninstall.sh [--dry-run]
# Exit:  0 removed (or nothing installed), 1 error, 2 usage error.
set -eu

pkg_id=org.ntfs-macos.pkg
owned1=usr/local/libexec/ntfs-macos
owned2=usr/local/share/doc/ntfs-macos

dry=0
case "${1:-}" in
'') ;;
--dry-run) dry=1 ;;
-h | --help) printf 'usage: sudo %s [--dry-run]\n' "$0"; exit 0 ;;
*) printf 'usage: sudo %s [--dry-run]\n' "$0" >&2; exit 2 ;;
esac

if ! pkgutil --pkg-info "$pkg_id" >/dev/null 2>&1; then
	echo "uninstall: $pkg_id is not installed"
	exit 0
fi
[ "$dry" -eq 1 ] || [ "$(id -u)" -eq 0 ] || { echo "uninstall: run with sudo" >&2; exit 1; }

vol=$(pkgutil --pkg-info "$pkg_id" | sed -n 's/^volume: //p')
loc=$(pkgutil --pkg-info "$pkg_id" | sed -n 's/^location: //p')
case "$vol|$loc" in
'/|' | '/|/') ;;
*) echo "uninstall: unexpected install location '$vol' '$loc'" >&2; exit 1 ;;
esac

files=$(pkgutil --only-files --files "$pkg_id")
# Validate every path first: owned prefix, no '..', no newline tricks.
printf '%s\n' "$files" | while IFS= read -r f; do
	[ -n "$f" ] || continue
	case $f in
	*..*) echo "uninstall: refusing path with '..': $f" >&2; exit 1 ;;
	"$owned1"/* | "$owned2"/*) ;;
	*) echo "uninstall: refusing path outside the package directories: $f" >&2; exit 1 ;;
	esac
done

printf '%s\n' "$files" | while IFS= read -r f; do
	[ -n "$f" ] || continue
	if [ "$dry" -eq 1 ]; then
		echo "would remove /$f"
	elif [ -e "/$f" ] || [ -L "/$f" ]; then
		rm -f "/$f"
	fi
done

if [ "$dry" -eq 1 ]; then
	echo "would remove /$owned1 and /$owned2 if empty, and forget $pkg_id"
	exit 0
fi
# Remove the owned directories bottom-up, only if empty.
for d in "/$owned1" "/$owned2"; do
	[ -d "$d" ] && find "$d" -depth -type d -exec rmdir {} \; 2>/dev/null || true
done
pkgutil --forget "$pkg_id" >/dev/null
echo "uninstall: removed $pkg_id"
