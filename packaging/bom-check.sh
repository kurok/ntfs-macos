#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Compare the owners and modes in the bill of materials of a built component package with the
# install manifest (audit M6-F07; T-M6-01, criterion 6.3). Reads files only. The list is the output
# of `lsbom -p MUGf Bom` of the component package (expanded with `pkgutil --expand`): one line per
# entry, the symbolic mode, the user, the group and the path (./usr/local/...), separated by tabs.
#
# The list agrees with the manifest when
#   - every `dir` and `file` of the manifest is in the list, as a directory or a regular file, with
#     the manifest's mode and with owner root and group wheel (what the Installer will give it), and
#   - every other entry of the list is a directory above one of them (".", /usr, /usr/local,
#     /Library, /Library/LaunchDaemons and so on), with mode 0755, owner root and group wheel.
# Anything else (an extra entry, a link, a missing path, another owner, group or mode) is a
# difference. `statedir`, `statefile`, `link` and `right` entries are not package content.
#
# Usage: packaging/bom-check.sh MANIFEST LIST
# Exit:  0 the list and the manifest agree, 1 they differ (each difference is printed), 2 usage.
set -eu

manifest=${1:-}
list=${2:-}
[ -n "$manifest" ] && [ -n "$list" ] || { echo "usage: bom-check.sh MANIFEST LIST" >&2; exit 2; }
[ -f "$manifest" ] || { echo "bom-check: no manifest: $manifest" >&2; exit 2; }
# an empty manifest would make awk read the list as the manifest
[ -s "$manifest" ] || { echo "bom-check: the manifest is empty: $manifest" >&2; exit 1; }
[ -f "$list" ] || { echo "bom-check: no list: $list" >&2; exit 2; }
[ -s "$list" ] || { echo "bom-check: the list is empty: $list" >&2; exit 1; }

awk '
# sym TYPE MODE: the mode as lsbom -p M prints it ("drwxr-xr-x"); MODE is octal, 0755 or 755;
# anything but plain permission bits gives a text no entry can have
function sym(t, m,    s, i, d) {
	if (m !~ /^0?[0-7][0-7][0-7]$/) return "invalid mode " m
	m = substr(m, length(m) - 2)
	s = t
	for (i = 1; i <= 3; i++) {
		d = substr(m, i, 1) + 0
		s = s ((d >= 4) ? "r" : "-") ((d % 4 >= 2) ? "w" : "-") ((d % 2) ? "x" : "-")
	}
	return s
}
FNR == NR {
	if ($0 ~ /^[ \t]*(#|$)/) next
	if ($1 != "dir" && $1 != "file") next
	entries++
	p = $0
	for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p)
	sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p)
	want[p] = sym(($1 == "dir") ? "d" : "-", $2) " " $3 " " $4
	kind[p] = $1
	# every directory above a manifest path is a shared parent: 0755 root wheel
	a = p
	while (sub(/\/[^\/]*$/, "", a) && a != "") parent[a] = 1
	parent["/"] = 1
	next
}
{
	n = split($0, f, "\t")
	if (n < 4) { printf "malformed line of the list: %s\n", $0; rc = 1; next }
	m = f[1]; sub(/[ \t]+$/, "", m)
	p = f[4]
	for (i = 5; i <= n; i++) p = p "\t" f[i]
	sub(/^\.\//, "", p); sub(/^\.$/, "", p); sub(/\/$/, "", p)
	p = "/" p
	if (p == "//") p = "/"
	got = m " " f[2] " " f[3]
	if (p in seen) { printf "listed twice: %s\n", p; rc = 1; next }
	seen[p] = 1
	if (p in want) {
		have[p] = 1
		if (got != want[p]) { printf "%s %s: the package has %s, the manifest says %s\n", kind[p], p, got, want[p]; rc = 1 }
		next
	}
	if (p in parent) {
		if (got != "drwxr-xr-x root wheel") { printf "shared parent %s: the package has %s, expected drwxr-xr-x root wheel\n", p, got; rc = 1 }
		next
	}
	printf "extra in the package (not in the manifest): %s (%s)\n", p, got
	rc = 1
}
END {
	if (entries == 0) { print "the manifest has no dir or file entry"; exit 1 }
	for (p in want) if (!(p in have)) { printf "missing from the package (%s of the manifest): %s\n", kind[p], p; rc = 1 }
	exit rc
}' "$manifest" "$list"
