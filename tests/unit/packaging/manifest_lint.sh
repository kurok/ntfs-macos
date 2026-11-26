#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-01 (T-M4-10): packaging/manifest.txt is well formed and agrees with the rest of the tree.
# Reads files only; it never runs the installer or the uninstaller.
#
# Usage: manifest_lint.sh ROOT [MANIFEST]
#   ROOT      the source tree
#   MANIFEST  the manifest to check (default ROOT/packaging/manifest.txt; the mutant test passes
#             altered copies)
# Exit: 0 all checks passed, 1 a check failed, 2 usage error.
set -u

root=${1:?usage: manifest_lint.sh ROOT [MANIFEST]}
manifest=${2:-$root/packaging/manifest.txt}
fail=0
bad() { printf 'FAIL: %s\n' "$*"; fail=1; }

[ -f "$manifest" ] || { echo "no manifest: $manifest"; exit 2; }

# ---- format, paths, ownership, modes ----
awk -v m="$manifest" '
function bad(msg) { printf "FAIL: %s:%d: %s\n", m, NR, msg; rc = 1 }
/^[ \t]*(#|$)/ { next }
{
	kind = $1; mode = $2; owner = $3; group = $4
	path = $0
	for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", path)
	sub(/^[ \t]+/, "", path); sub(/[ \t]+$/, "", path)
	if (path == "") { bad("malformed entry"); next }
	if (kind !~ /^(dir|file|statedir|statefile|right|link)$/) { bad("unknown type " kind); next }
	if (path in seen) bad("duplicate " path)
	seen[path] = 1
	if (kind == "right") {
		if (mode != "-" || owner != "-" || group != "-") bad("a right has - for mode, owner and group")
		if (path != "org.ntfs-macos.journal-modify") bad("unexpected right " path)
		nright++
		next
	}
	if (kind == "link") {
		if (mode != "-" || owner != "-" || group != "-") bad("a link has - for mode, owner and group")
		if (path != "/usr/local/bin/ntfsmac") bad("unexpected link " path)
		nlink++
		next
	}
	if (path !~ /^\//) bad("path is not absolute: " path)
	if (path ~ /\.\.|\/\/|\/$/) bad("bad path " path)
	if (path ~ /[[:cntrl:]]/) bad("control character in path")
	if (mode !~ /^0[0-7][0-7][0-7]$/) bad("bad mode " mode)
	else if (mode ~ /[2367]$/ || mode ~ /^0.[2367]/) bad("group or others can write: " mode " " path)
	if (owner != "root" || group != "wheel") bad("owner and group must be root wheel: " path)
	ok = 0
	if (kind == "dir" || kind == "file") {
		if (path ~ /^\/usr\/local\/libexec\/ntfs-macos(\/|$)/) ok = 1
		if (path ~ /^\/Library\/Application Support\/ntfs-macos(\/|$)/) ok = 1
		if (kind == "file" && path == "/Library/LaunchDaemons/org.ntfs-macos.ntfsmacd.plist") ok = 1
	}
	if (kind == "statedir" && path == "/var/db/ntfs-macos") ok = 1
	if (kind == "statefile" && path == "/var/log/ntfsmacd.log") ok = 1
	if (!ok) bad(kind " outside the paths ntfs-macos may own: " path)
	if (kind == "file" && path ~ /^\/usr\/local\/libexec\/ntfs-macos\// && path !~ /manifest\.txt$/ && mode != "0755") bad("a program must be 0755: " path)
	if (kind == "file" && path ~ /\.(sb|plist)$|cli-requirement$/ && mode != "0644") bad("a profile, plist or requirement file must be 0644: " path)
	if (kind == "file") {
		d = path
		sub(/\/[^\/]*$/, "", d)
		needdir[d] = path
	}
	if (kind == "dir") isdir[path] = 1
}
END {
	if (nright != 1) { printf "FAIL: %s: exactly one right expected, found %d\n", m, nright; rc = 1 }
	if (nlink != 1) { printf "FAIL: %s: exactly one link expected, found %d\n", m, nlink; rc = 1 }
	for (d in needdir) {
		if (d == "/usr/local/bin" || d == "/Library/LaunchDaemons") continue
		if (!(d in isdir)) { printf "FAIL: %s: the directory of %s is not listed as a dir\n", m, needdir[d]; rc = 1 }
	}
	exit rc
}' "$manifest" || fail=1

# listed entries, one "kind<TAB>path" per line, for the checks below
tab=$(printf '\t')
paths=$(awk '/^[ \t]*(#|$)/ { next } { p = $0; for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p); sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p); print $1 "\t" p }' "$manifest")
listed() { printf '%s\n' "$paths" | grep -qxF "$1$tab$2"; }

# ---- the files the tree ships must all be listed ----
for f in "$root"/integration/sandbox/*.sb; do
	listed file "/Library/Application Support/ntfs-macos/sandbox/${f##*/}" || bad "sandbox profile ${f##*/} is not in the manifest"
done
for f in "$root"/integration/launchd/*.plist; do
	listed file "/Library/LaunchDaemons/${f##*/}" || bad "launchd plist ${f##*/} is not in the manifest"
done
# and the manifest lists no profile the tree does not have
for s in $(printf '%s\n' "$paths" | awk -F '\t' '$2 ~ /\/sandbox\/[^\/]*\.sb$/ { n = split($2, a, "/"); print a[n] }'); do
	[ -f "$root/integration/sandbox/$s" ] || bad "manifest lists sandbox/$s, which the tree does not have"
done

# ---- agreement with the constants of the code ----
def() { # header macro: the string value of a #define
	sed -n "s/^#define $2[[:space:]]*\"\\(.*\\)\"\$/\\1/p" "$root/$1" | head -n 1
}
want_file() { # label value
	[ -n "$2" ] || { bad "cannot read $1 from the sources"; return; }
	listed file "$2" || bad "$1 ($2) is not a file of the manifest"
}
want_file NTFSMAC_PATH_PROBE "$(def src/daemon/spawn_config.h NTFSMAC_PATH_PROBE)"
want_file NTFSMAC_PATH_VOLMARK "$(def src/daemon/spawn_config.h NTFSMAC_PATH_VOLMARK)"
want_file NTFSMAC_PATH_WORKER "$(def src/daemon/spawn_config.h NTFSMAC_PATH_WORKER)"
want_file NTFSMAC_CTL_REQUIREMENT_FILE "$(sed -n '/^#define NTFSMAC_CTL_REQUIREMENT_FILE/{n;s/^[[:space:]]*"\(.*\)"$/\1/p;}' "$root/src/daemon/ctl_priv.h")"
sbx=$(def src/daemon/spawn_config.h NTFSMAC_PATH_SANDBOX_DIR)
if [ -z "$sbx" ] || ! listed dir "$sbx"; then bad "the sandbox directory of spawn_config.h is not a dir of the manifest"; fi
right=$(def include/ntfsmac/ctl_proto.h NTFSMAC_CTL_RIGHT_NAME)
if [ -z "$right" ] || ! listed right "$right"; then bad "the authorization right of ctl_proto.h is not in the manifest"; fi
jd=$(def include/ntfsmac/journal.h NTFSMAC_JOURNAL_DEFAULT_DIR)
case $jd in
/var/db/ntfs-macos/*) listed statedir /var/db/ntfs-macos || bad "the journal directory is not under a statedir of the manifest" ;;
*) bad "unexpected journal directory: $jd" ;;
esac
prog=$(sed -n 's|.*<string>\(/usr/local/libexec/ntfs-macos/ntfsmacd\)</string>.*|\1|p' "$root/integration/launchd/org.ntfs-macos.ntfsmacd.plist" | head -n 1)
if [ -z "$prog" ] || ! listed file "$prog"; then bad "the program of the launchd plist is not a file of the manifest"; fi
listed file /usr/local/libexec/ntfs-macos/ntfsmac || bad "the CLI is not in the manifest (/usr/local/libexec/ntfs-macos/ntfsmac)"
listed link /usr/local/bin/ntfsmac || bad "the link /usr/local/bin/ntfsmac is not in the manifest"

# ---- the two scripts keep one allow-list, and the installer has a source for every file ----
a1=$(sed -n '/^allowed()/,/^}/p' "$root/packaging/dev-install.sh")
a2=$(sed -n '/^allowed()/,/^}/p' "$root/packaging/uninstall.sh")
if [ -z "$a1" ] || [ "$a1" != "$a2" ]; then bad "the allowed() lists of dev-install.sh and uninstall.sh differ"; fi
printf '%s\n' "$paths" | awk -F '\t' '$1 == "file" { print $2 }' >"${TMPDIR:-/tmp}/manifest_lint.$$"
while IFS= read -r p; do
	case $p in
	*/ntfsfix | */ntfsinfo | */ntfslabel | */mkntfs) ;; # the ntfsprogs are mapped by one rule
	"/Library/Application Support/ntfs-macos/sandbox/"*.sb) ;; # the profiles are mapped by one pattern
	/Library/LaunchDaemons/*) ;; # mapped through the job label
	*) grep -qF -- "$p" "$root/packaging/dev-install.sh" || bad "dev-install.sh has no source for $p" ;;
	esac
done <"${TMPDIR:-/tmp}/manifest_lint.$$"
rm -f "${TMPDIR:-/tmp}/manifest_lint.$$"

[ "$fail" -eq 0 ] && echo "PASS: manifest and tree agree"
exit "$fail"
