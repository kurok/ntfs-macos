#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-09 (audit M6-F07): packaging/bom-check.sh, the host check of the owners and modes in the
# bill of materials of a built component package. Builds small component packages here with
# pkgbuild from a tree staged the way build-pkg.sh stages the payload (the manifest's dirs and
# files with its modes, placeholder contents, no extended attributes) and reads their Bom with
# `lsbom -p MUGf`; nothing is installed or signed.
#
#   a. the staged tree, built with --ownership recommended as build-pkg.sh does: accepted
#   b. the same tree with --ownership preserve (the files keep the builder's owner): refused
#   c. a tree with a program at 0775, a directory at 0700 and an extra file: refused, and each of
#      the three differences is named
#   d. mutants of the list of a: a missing file, a link in place of a file, a shared parent with
#      group admin, a setuid program, a group-writable file, a line listed twice, an AppleDouble
#      ._ file: each refused
#
# Usage: bom_check_test.sh ROOT
# Exit: 0 all checks passed, 1 a check failed, 2 usage error (or no pkgbuild).
set -u

root=${1:?usage: bom_check_test.sh ROOT}
manifest=$root/packaging/manifest.txt
check=$root/packaging/bom-check.sh
if ! command -v pkgbuild >/dev/null 2>&1 || ! command -v lsbom >/dev/null 2>&1; then
	echo "bom_check_test: needs pkgbuild and lsbom (macOS)"
	exit 2
fi
tmp=$(mktemp -d "${TMPDIR:-/tmp}/bom_check_test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
fail=0
pass() { echo "PASS: $*"; }
bad() { echo "FAIL: $*"; fail=1; }

# stage DIR: the manifest's dirs and files below DIR, with the manifest's modes
stage() {
	awk '/^[ \t]*(#|$)/ { next } $1 == "dir" || $1 == "file" {
		p = $0; for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p); sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p)
		print $1 "\t" $2 "\t" p }' "$manifest" | while IFS='	' read -r k m p; do
		if [ "$k" = dir ]; then
			install -d -m "$m" "$1$p"
		else
			install -d -m 0755 "$1${p%/*}"
			printf 'placeholder %s\n' "$p" >"$1$p"
			chmod "$m" "$1$p"
		fi
	done
	xattr -cr "$1" 2>/dev/null || true
}
# bom NAME OWNERSHIP: build $tmp/NAME (staged) as a component package, its Bom list in $tmp/NAME.list
bom() {
	pkgbuild --root "$tmp/$1" --identifier org.ntfs-macos.bomtest --version 1 --install-location / \
		--ownership "$2" --filter '\.DS_Store$' "$tmp/$1.pkg" >/dev/null 2>&1 || { bad "pkgbuild of $1"; return 1; }
	pkgutil --expand "$tmp/$1.pkg" "$tmp/$1.exp" || { bad "pkgutil --expand of $1"; return 1; }
	# On a host where the test process cannot clear com.apple.provenance, pkgbuild adds an AppleDouble
	# ._ entry for every path. build-pkg.sh refuses a staged tree with any extended attribute before
	# pkgbuild, so those entries cannot reach a release package; here they are dropped (case d checks
	# that bom-check refuses one).
	lsbom -p MUGf "$tmp/$1.exp/Bom" | grep -v '/\._[^/]*$' >"$tmp/$1.list"
}
run() { sh "$check" "$manifest" "$1" >"$tmp/out" 2>&1; }

# ---- a. the staged tree, recommended ownership ----
stage "$tmp/good"
if bom good recommended && run "$tmp/good.list"; then
	pass "bom-check accepts the staged payload built with --ownership recommended ($(wc -l <"$tmp/good.list" | tr -d ' ') Bom entries)"
else
	bad "bom-check refuses the staged payload: $(cat "$tmp/out")"
fi
n_want=$(awk '/^[ \t]*(#|$)/ { next } $1 == "dir" || $1 == "file" { n++ } END { print n }' "$manifest")
[ "$(grep -c -E '	root	wheel	\./(usr|Library)/' "$tmp/good.list")" -ge "$n_want" ] ||
	bad "the Bom of the staged payload has fewer root:wheel entries than the manifest has dirs and files"

# ---- b. the builder's own owner ----
if [ "$(id -u)" -eq 0 ]; then
	echo "NOTE: running as root: --ownership preserve would keep root, case b skipped"
else
	cp -R "$tmp/good" "$tmp/preserve"
	if bom preserve preserve; then
		if run "$tmp/preserve.list"; then bad "bom-check accepted a package whose files keep the builder's owner"; else pass "bom-check refuses --ownership preserve ($(head -n 1 "$tmp/out"))"; fi
	fi
fi

# ---- c. wrong modes and an extra file in the staged tree ----
cp -R "$tmp/good" "$tmp/modes"
chmod 0775 "$tmp/modes/usr/local/libexec/ntfs-macos/ntfsmacd"
chmod 0700 "$tmp/modes/Library/Application Support/ntfs-macos/sandbox"
printf 'extra\n' >"$tmp/modes/usr/local/libexec/ntfs-macos/extra"
if bom modes recommended; then
	if run "$tmp/modes.list"; then
		bad "bom-check accepted a package with wrong modes and an extra file"
	else
		grep -q 'ntfsmacd: the package has -rwxrwxr-x root wheel' "$tmp/out" || bad "the 0775 program is not named: $(cat "$tmp/out")"
		grep -q 'sandbox: the package has drwx------ root wheel' "$tmp/out" || bad "the 0700 directory is not named: $(cat "$tmp/out")"
		grep -q 'extra in the package.*ntfs-macos/extra' "$tmp/out" || bad "the extra file is not named: $(cat "$tmp/out")"
		pass "bom-check refuses a 0775 program, a 0700 directory and an extra file, and names each"
	fi
fi

# ---- d. mutants of the list ----
mutant() { # NAME SED-SCRIPT
	sed "$2" "$tmp/good.list" >"$tmp/m.list"
	if cmp -s "$tmp/good.list" "$tmp/m.list"; then bad "mutant '$1' did not change the list"; return; fi
	if run "$tmp/m.list"; then bad "bom-check accepted: $1"; else pass "bom-check refuses $1 ($(head -n 1 "$tmp/out"))"; fi
}
mutant "a missing file" '/\/ntfsmac-worker$/d'
mutant "a link in place of a file" 's|^-rwxr-xr-x\([[:space:]]*	root	wheel	\./usr/local/libexec/ntfs-macos/ntfsmac\)$|lrwxr-xr-x\1|'
mutant "a shared parent with group admin" 's|^\(drwxr-xr-x[[:space:]]*	root	\)wheel\(	\./Library/LaunchDaemons\)$|\1admin\2|'
mutant "a setuid program" 's|^-rwxr-xr-x\([[:space:]]*	root	wheel	\./usr/local/libexec/ntfs-macos/ntfsmacd\)$|-rwsr-xr-x\1|'
mutant "a group-writable data file" 's|^-rw-r--r--\([[:space:]]*	root	wheel	\./usr/local/libexec/ntfs-macos/manifest.txt\)$|-rw-rw-r--\1|'
mutant "an entry listed twice" '/\/ntfsmac-probe$/p'
mutant "an AppleDouble file of extended attributes" 's|^\(-rwxr-xr-x[[:space:]]*	root	wheel	\./usr/local/libexec/ntfs-macos/\)ntfsmacd$|&\
\1._ntfsmacd|'
if sh "$check" "$manifest" /dev/null >/dev/null 2>&1; then bad "bom-check accepted an empty list"; else pass "bom-check refuses an empty list"; fi
# a manifest that names nothing must not let any list pass (an empty file would also make awk read
# the list as the manifest)
: >"$tmp/empty.manifest"
printf '# comments only\n\nright - - - org.ntfs-macos.journal-modify\n' >"$tmp/nodir.manifest"
for m in empty nodir; do
	if sh "$check" "$tmp/$m.manifest" "$tmp/good.list" >"$tmp/out" 2>&1 || ! grep -q -e 'no dir or file entry' -e 'the manifest is empty' "$tmp/out"; then bad "bom-check accepted a manifest with no dir or file entry ($m)"; else pass "bom-check refuses a manifest with no dir or file entry ($m: $(head -n 1 "$tmp/out"))"; fi
done

[ "$fail" -eq 0 ] && echo "PASS: bom-check compares the owners and modes of a built package with the manifest"
exit "$fail"
