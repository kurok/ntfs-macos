#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compare the path list of a package with the install manifest (T-M6-01, criterion 6.3). Reads
# files only. The list is the output of `pkgutil --payload-files PKG` (paths such as ./usr/local)
# or of `pkgutil --files org.ntfs-macos.pkg` (paths such as usr/local), one per line.
#
# The list agrees with the manifest when
#   - every `dir` and `file` of the manifest is in the list, and
#   - every path of the list is a `dir` or `file` of the manifest, or a directory above one of
#     them (the shared parents /usr/local/bin, /Library/LaunchDaemons and so on, which the package
#     names but never owns).
# `statedir`, `statefile` and `right` entries are not package content and are ignored.
#
# Usage: packaging/payload-check.sh MANIFEST LIST
# Exit:  0 the list and the manifest agree, 1 they differ (each difference is printed), 2 usage.
set -eu

manifest=${1:-}
list=${2:-}
[ -n "$manifest" ] && [ -n "$list" ] || { echo "usage: payload-check.sh MANIFEST LIST" >&2; exit 2; }
[ -f "$manifest" ] || { echo "payload-check: no manifest: $manifest" >&2; exit 2; }
[ -f "$list" ] || { echo "payload-check: no list: $list" >&2; exit 2; }

awk -v m="$manifest" '
FNR == NR {
	if ($0 ~ /^[ \t]*(#|$)/) next
	if ($1 != "dir" && $1 != "file") next
	p = $0
	for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p)
	sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p)
	want[p] = $1
	# every directory above a manifest path may appear in the list as a shared parent
	a = p
	while (sub(/\/[^\/]*$/, "", a) && a != "") parent[a] = 1
	next
}
{
	p = $0
	sub(/^\.\//, "", p); sub(/^\.$/, "", p); sub(/\/$/, "", p)
	if (p == "") next
	if (p !~ /^\//) p = "/" p
	if (p in seen) next
	seen[p] = 1
	if (p in want) { have[p] = 1; next }
	if (p in parent) next
	printf "extra in the package (not in the manifest): %s\n", p
	rc = 1
}
END {
	for (p in want) if (!(p in have)) { printf "missing from the package (%s of the manifest): %s\n", want[p], p; rc = 1 }
	exit rc
}' "$manifest" "$list"
