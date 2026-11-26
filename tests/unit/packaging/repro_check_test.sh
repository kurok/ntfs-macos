#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-07 (T-M6-04): the comparison and the allow-list of scripts/repro-check.sh, on the host, with
# small packages made here by pkgbuild and productbuild (nothing is installed or signed with an
# identity). Two packages of the same tree made a second apart must pass (only the xar table of
# contents times differ); a changed document, a changed BUILD-INFO date, a Mach-O that differs only
# in its ad-hoc signature, a Mach-O with other code, a non-text file that differs, a Bom that differs
# only in the modification times, and a SHA256SUMS line of another file than the .pkg must each fail
# with that cause named.
# Then the source and date that packaging/build-pkg.sh --source-info reports for BUILD-INFO (it builds
# nothing): from its own git repository the commit and its time; a tree inside another repository
# never borrows that repository's commit; with SOURCES.txt of a source archive both come from there;
# with neither, "unknown" and the current time, worded as the build time.
#
# Usage: repro_check_test.sh REPRO_CHECK_SH BUILD_PKG_SH
set -eu

check=${1:?usage: repro_check_test.sh REPRO_CHECK_SH BUILD_PKG_SH}
build_pkg=${2:?usage: repro_check_test.sh REPRO_CHECK_SH BUILD_PKG_SH}
for t in pkgbuild productbuild codesign cc git; do
	command -v "$t" >/dev/null 2>&1 || { echo "SKIP: $t not found"; exit 0; }
done
tmp=$(mktemp -d "${TMPDIR:-/tmp}/repro-check-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
fail=0
pass() { echo "PASS: $1"; }
bad() { echo "FAIL: $1"; fail=1; }

# mk NAME STAMP: a package directory like build-pkg.sh's output, from the tree $tmp/root
mk() {
	_d=$tmp/$1
	rm -rf "$_d" "$tmp/scripts.$1" "$tmp/root.$1"
	mkdir -p "$_d"
	cp -R "$tmp/root" "$tmp/root.$1"
	cp -R "$tmp/scripts" "$tmp/scripts.$1"
	(TZ=UTC0 && export TZ && find "$tmp/root.$1" "$tmp/scripts.$1" -exec touch -h -t "$2" {} +)
	pkgbuild --quiet --root "$tmp/root.$1" --scripts "$tmp/scripts.$1" --identifier org.ntfs-macos.pkg \
		--version 0.0.1 --install-location / --ownership recommended "$tmp/component.pkg" >/dev/null
	productbuild --quiet --package "$tmp/component.pkg" "$_d/ntfs-macos-t.pkg" >/dev/null
	cp "$tmp/BUILD-INFO" "$_d/BUILD-INFO"
	printf 'uninstall\n' >"$_d/uninstall.sh"
	printf 'manifest\n' >"$_d/manifest.txt"
	(cd "$_d" && shasum -a 256 ntfs-macos-t.pkg uninstall.sh manifest.txt >SHA256SUMS)
}
# run NAME A B WANT_RC [CAUSE]
run() {
	_rc=0
	sh "$check" --compare "$tmp/$2" "$tmp/$3" --work "$tmp/work" >"$tmp/out.$1" 2>&1 || _rc=$?
	if [ "$_rc" -ne "$4" ]; then
		bad "$1: exit $_rc, expected $4"; sed 's/^/    /' "$tmp/out.$1"; return
	fi
	if [ -n "${5:-}" ] && ! grep -q "differs   $5 " "$tmp/out.$1"; then
		bad "$1: cause $5 not named"; sed 's/^/    /' "$tmp/out.$1"; return
	fi
	pass "$1"
}

mkdir -p "$tmp/root/usr/local/libexec/ntfs-macos" "$tmp/root/Library/Application Support/ntfs-macos/doc" "$tmp/scripts"
# a program linked here (ad-hoc signed by the linker, like the product build)
printf 'int main(void) { return 0; }\n' >"$tmp/prog.c"
cc -o "$tmp/root/usr/local/libexec/ntfs-macos/prog" "$tmp/prog.c"
printf 'readme\n' >"$tmp/root/Library/Application Support/ntfs-macos/doc/README.md"
printf '#!/bin/sh\nexit 0\n' >"$tmp/scripts/postinstall"
chmod 0755 "$tmp/scripts/postinstall"
printf 'package      t\nsource-date  2026-11-01T00:00:00Z (SOURCE_DATE_EPOCH)\n' >"$tmp/BUILD-INFO"
xattr -cr "$tmp" 2>/dev/null || true

mk a 202611010000.00
sleep 1
mk b 202611010000.00
run same-tree a b 0
run self a a 0

printf 'readme changed\n' >"$tmp/root/Library/Application Support/ntfs-macos/doc/README.md"
mk c 202611010000.00
run changed-document a c 1 content
printf 'readme\n' >"$tmp/root/Library/Application Support/ntfs-macos/doc/README.md"

mk d 202611010000.00
sed 's/2026-11-01/2026-11-02/' "$tmp/a/BUILD-INFO" >"$tmp/d/BUILD-INFO"
run buildinfo-date a d 1 buildinfo-date

mk e 202611020000.00
run payload-mtimes a e 1 bom-mtime

cp "$tmp/root/usr/local/libexec/ntfs-macos/prog" "$tmp/prog.orig"
codesign --force --sign - --identifier org.ntfs-macos.other "$tmp/root/usr/local/libexec/ntfs-macos/prog" 2>/dev/null
mk f 202611010000.00
run macho-signature a f 1 macho-signature
cp "$tmp/prog.orig" "$tmp/root/usr/local/libexec/ntfs-macos/prog"

printf 'int main(void) { return 1; }\n' >"$tmp/prog2.c"
cc -o "$tmp/root/usr/local/libexec/ntfs-macos/prog" "$tmp/prog2.c"
mk g 202611010000.00
run macho-code a g 1 macho-code
cp "$tmp/prog.orig" "$tmp/root/usr/local/libexec/ntfs-macos/prog"

printf '\000\001\002' >"$tmp/root/Library/Application Support/ntfs-macos/doc/blob"
mk h 202611010000.00
printf '\000\001\003' >"$tmp/root/Library/Application Support/ntfs-macos/doc/blob"
mk i 202611010000.00
run binary-content h i 1 binary-content
rm -f "$tmp/root/Library/Application Support/ntfs-macos/doc/blob"

mk j 202611010000.00
awk '$2 == "manifest.txt" { $1 = "0000000000000000000000000000000000000000000000000000000000000000" } { print $1 "  " $2 }' \
	"$tmp/j/SHA256SUMS" >"$tmp/j/SHA256SUMS.new" && mv "$tmp/j/SHA256SUMS.new" "$tmp/j/SHA256SUMS"
run sums-other-line a j 1 content

# --- build-pkg.sh --source-info ---
# info DIR [VAR=value...]: run the copy of build-pkg.sh in DIR with a clean environment for the two variables
info() {
	_d=$1; shift
	env -u NTFSMAC_SOURCE_REV -u SOURCE_DATE_EPOCH "$@" sh "$_d/packaging/build-pkg.sh" --source-info 2>/dev/null
}
gitc() { GIT_COMMITTER_DATE='2026-01-02T03:04:05Z' GIT_AUTHOR_DATE='2026-01-02T03:04:05Z' git -C "$1" \
	-c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q --allow-empty -m t; }
srcdir() { # DIR: a tree with build-pkg.sh and its manifest
	mkdir -p "$1/packaging"
	cp "$build_pkg" "$1/packaging/build-pkg.sh"
	cp "$(dirname "$build_pkg")/manifest.txt" "$1/packaging/manifest.txt"
}
# want NAME FILE PATTERN...: every pattern (grep -E) is in FILE; NOT:PATTERN must be absent
want() {
	_n=$1 _f=$2; shift 2
	for _p in "$@"; do
		case $_p in
		NOT:*) if grep -Eq "${_p#NOT:}" "$_f"; then bad "$_n: has ${_p#NOT:}"; sed 's/^/    /' "$_f"; return; fi ;;
		*) if ! grep -Eq "$_p" "$_f"; then bad "$_n: lacks $_p"; sed 's/^/    /' "$_f"; return; fi ;;
		esac
	done
	pass "$_n"
}
# own repository: its commit and commit time
srcdir "$tmp/own"
git -C "$tmp/own" init -q && git -C "$tmp/own" add -A && gitc "$tmp/own"
own_rev=$(git -C "$tmp/own" rev-parse HEAD)
info "$tmp/own" >"$tmp/i.own" || true
want "source-info: own repository" "$tmp/i.own" "^source +$own_rev\$" '^SOURCE_DATE_EPOCH=1767323045$' \
	'^source-date +2026-01-02T03:04:05Z \(SOURCE_DATE_EPOCH, the commit time'
# an extracted archive inside another repository, without SOURCES.txt: nothing borrowed
git -C "$tmp" init -q "$tmp/outer" && gitc "$tmp/outer"
srcdir "$tmp/outer/src"
info "$tmp/outer/src" >"$tmp/i.outer" || true
want "source-info: enclosing repository not used, fallback worded as build time" "$tmp/i.outer" \
	'^source +unknown$' 'NOT:^SOURCE_DATE_EPOCH=1767323045$' '\(build time \(no commit time available\)\)$'
# the same with SOURCES.txt of a source archive: both values from there
printf 'Build\n-----\n  NTFSMAC_SOURCE_REV=0123456789abcdef0123456789abcdef01234567\n  SOURCE_DATE_EPOCH=1700000000\n  packaging/build-pkg.sh --label x\n' \
	>"$tmp/outer/src/SOURCES.txt"
info "$tmp/outer/src" >"$tmp/i.src" || true
want "source-info: SOURCES.txt" "$tmp/i.src" '^source +0123456789abcdef0123456789abcdef01234567$' \
	'^SOURCE_DATE_EPOCH=1700000000$' '^source-date +2023-11-14T22:13:20Z \(SOURCE_DATE_EPOCH, the commit time'
# the variables win over SOURCES.txt
info "$tmp/outer/src" NTFSMAC_SOURCE_REV=abc SOURCE_DATE_EPOCH=1 >"$tmp/i.env" || true
want "source-info: variables first" "$tmp/i.env" '^source +abc$' '^SOURCE_DATE_EPOCH=1$'
# no git at all, no SOURCES.txt: unknown and the build time
srcdir "$tmp/bare"
info "$tmp/bare" GIT_CEILING_DIRECTORIES="$tmp" >"$tmp/i.bare" || true
want "source-info: no git, no SOURCES.txt" "$tmp/i.bare" '^source +unknown$' '\(build time \(no commit time available\)\)$'

[ "$fail" -eq 0 ] && echo "repro_check_test: all passed"
exit "$fail"
