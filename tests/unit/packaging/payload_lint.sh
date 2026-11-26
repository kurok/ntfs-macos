#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-03 (T-M6-01): the release package and the manifest agree, and the root-run package scripts
# keep their rules. Reads files and runs `build-pkg.sh --plan` (which builds nothing); it never
# runs the installer, the package scripts or the uninstaller.
#
#   a. build-pkg.sh --plan names a source for every file and stages exactly the manifest's dirs and
#      files, with the manifest's modes
#   b. packaging/payload-check.sh accepts the manifest's own list (pkgutil --payload-files and
#      pkgutil --files spelling) and refuses a missing file, an extra file in an owned directory,
#      an extra file in a shared directory and a licence directory the manifest does not list
#   c. the path list of preinstall is the manifest's, in its order
#   d. the minimum macFUSE version of preinstall and distribution.xml is the daemon's
#   e. preinstall classifies /Library/Filesystems names like src/daemon/conflict.c
#   f. postinstall installs the right with the definition of dev-install.sh
#   g. preinstall and postinstall: `set -eu`, a fixed PATH, no environment variable, no argument
#      but $3, no eval, and every program by its absolute path
#   h. the package identifier is org.ntfs-macos.pkg.v2 in build-pkg.sh, preinstall, uninstall.sh,
#      release-local.sh and the VM tests, and the earlier layout's org.ntfs-macos.pkg is named as
#      old_pkg_id in preinstall and uninstall.sh (owner decision 2026-11-26, audit M6-F02)
#   i. preinstall refuses a downgrade (owner decision 2026-11-26, issue #10): build-pkg.sh writes the
#      package version into its placeholder, ver_older orders versions numerically and rejects what
#      it cannot read, and the check comes before the running daemon is stopped
#
# Usage: payload_lint.sh ROOT
# Exit: 0 all checks passed, 1 a check failed, 2 usage error.
set -u

root=${1:?usage: payload_lint.sh ROOT}
manifest=$root/packaging/manifest.txt
pre=$root/packaging/scripts/preinstall
post=$root/packaging/scripts/postinstall
tmp=$(mktemp -d "${TMPDIR:-/tmp}/payload_lint.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
fail=0
pass() { echo "PASS: $*"; }
bad() { echo "FAIL: $*"; fail=1; }

# the manifest's dir and file entries, "kind mode path"
awk '/^[ \t]*(#|$)/ { next } $1 == "dir" || $1 == "file" {
	p = $0; for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p); sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p)
	print $1 " " $2 " " p }' "$manifest" >"$tmp/want"
[ -s "$tmp/want" ] || { echo "no dir or file in $manifest"; exit 2; }

# ---- a. the plan of build-pkg.sh ----
if sh "$root/packaging/build-pkg.sh" --plan >"$tmp/plan" 2>"$tmp/plan.err"; then
	pass "build-pkg.sh --plan has a source for every file of the manifest"
else
	bad "build-pkg.sh --plan: $(cat "$tmp/plan.err")"
fi
sed 's/ <- .*$//' "$tmp/plan" >"$tmp/plan.paths"
if cmp -s "$tmp/want" "$tmp/plan.paths"; then
	pass "the payload plan is the manifest's dirs and files with their modes ($(wc -l <"$tmp/want" | tr -d ' ') entries)"
else
	bad "the payload plan differs from the manifest"
	diff "$tmp/want" "$tmp/plan.paths" | sed 's/^/  /'
fi
if grep -q -e 'LICENSES' -e '<- *$' "$tmp/plan"; then bad "the plan has an entry without a source or outside the manifest"; fi

# ---- b. payload-check.sh ----
cut -d' ' -f3- "$tmp/want" >"$tmp/paths"
{
	echo .
	awk '{ p = $0; while (sub(/\/[^\/]*$/, "", p) && p != "") print "." p }' "$tmp/paths"
	sed 's|^|.|' "$tmp/paths"
} | sort -u >"$tmp/payload.list"
pc() { sh "$root/packaging/payload-check.sh" "$manifest" "$1" >"$tmp/pc.out" 2>&1; }
if pc "$tmp/payload.list"; then pass "payload-check accepts the manifest's paths with their parents (pkgutil --payload-files form)"; else bad "payload-check refuses the manifest's own list: $(cat "$tmp/pc.out")"; fi
sed -e 's|^\./||' -e '/^\.$/d' "$tmp/payload.list" >"$tmp/files.list"
if pc "$tmp/files.list"; then pass "payload-check accepts the same list in pkgutil --files form"; else bad "payload-check refuses the pkgutil --files form: $(cat "$tmp/pc.out")"; fi
mutant() { # NAME SED-SCRIPT | NAME "" EXTRA-LINE
	if [ -n "$2" ]; then sed "$2" "$tmp/payload.list" >"$tmp/m.list"; else { cat "$tmp/payload.list"; echo "$3"; } >"$tmp/m.list"; fi
	if pc "$tmp/m.list"; then bad "payload-check accepted: $1"; else pass "payload-check refuses $1 ($(head -n 1 "$tmp/pc.out"))"; fi
}
mutant "a missing worker" '/ntfsmac-worker$/d'
mutant "a missing launchd plist" '/LaunchDaemons\/org.ntfs-macos.ntfsmacd.plist$/d'
mutant "an extra file in an owned directory" "" "./usr/local/libexec/ntfs-macos/extra"
mutant "an extra file in a shared directory" "" "./usr/local/bin/ntfs-3g"
mutant "a licence directory the manifest does not list" "" "./Library/Application Support/ntfs-macos/doc/LICENSES"
mutant "the CLI as a file in the shared bin directory" "" "./usr/local/bin/ntfsmac"
mutant "documents in the user-writable /usr/local/share" "" "./usr/local/share/doc/ntfs-macos/COPYING"
mutant "a file outside the project" "" "./etc/hosts"
: >"$tmp/empty.manifest"
printf '# comments only\nright - - - org.ntfs-macos.journal-modify\n' >"$tmp/nodir.manifest"
for m in empty nodir; do
	if sh "$root/packaging/payload-check.sh" "$tmp/$m.manifest" "$tmp/payload.list" >"$tmp/pc.out" 2>&1 || ! grep -q -e 'no dir or file entry' -e 'the manifest is empty' "$tmp/pc.out"; then bad "payload-check accepted a manifest with no dir or file entry ($m)"; else pass "payload-check refuses a manifest with no dir or file entry ($m: $(head -n 1 "$tmp/pc.out"))"; fi
done
: >"$tmp/empty.list"
if sh "$root/packaging/payload-check.sh" "$tmp/nodir.manifest" "$tmp/empty.list" >"$tmp/pc.out" 2>&1; then bad "payload-check accepted an empty list against a manifest with no dir or file entry"; else pass "payload-check refuses an empty list against a manifest with no dir or file entry ($(head -n 1 "$tmp/pc.out"))"; fi

# ---- c. the path list of preinstall ----
sed -n "/^paths='/,/'\$/p" "$pre" | sed -e "s/^paths='//" -e "s/'\$//" >"$tmp/pre.paths"
awk '{ k = $1; $2 = ""; sub(/^[^ ]+  /, ""); print k " " $0 }' "$tmp/want" >"$tmp/want.kp"
if [ -s "$tmp/pre.paths" ] && cmp -s "$tmp/want.kp" "$tmp/pre.paths"; then
	pass "the path list of preinstall is the manifest's"
else
	bad "the path list of preinstall differs from the manifest"
	diff "$tmp/want.kp" "$tmp/pre.paths" | sed 's/^/  /'
fi

# ---- d. minimum macFUSE version ----
dmin=$(awk '/^#define NTFSMAC_MACFUSE_MIN_(MAJOR|MINOR|PATCH)/ { printf "%s%s", sep, $3; sep = "." }' "$root/src/daemon/internal.h")
pmin=$(awk -F= '/^min_major=/ { a = $2 } /^min_minor=/ { b = $2 } /^min_patch=/ { c = $2 } END { print a "." b "." c }' "$pre")
xmin=$(sed -n "s/.*var MACFUSE_MIN = '\\([0-9.]*\\)'.*/\\1/p" "$root/packaging/distribution.xml")
if [ -n "$dmin" ] && [ "$pmin" = "$dmin" ] && [ "$xmin" = "$dmin" ]; then
	pass "minimum macFUSE $dmin in the daemon, preinstall and distribution.xml"
else
	bad "minimum macFUSE differs: daemon '$dmin', preinstall '$pmin', distribution.xml '$xmin'"
fi

# ---- e. conflict classification ----
sed -n '/^conflict() {$/,/^}$/p' "$pre" >"$tmp/conflict.sh"
# shellcheck disable=SC1091
. "$tmp/conflict.sh"
for t in "ufsd_NTFS.fs=Paragon NTFS" "UFSD_ntfs.FS=Paragon NTFS" "tuxera_ntfs.fs=Tuxera NTFS" "ntfs.fs=" "NTFS.fs=" \
	"ntfsmac-x.fs=" "ntfsmac.fs=" "macfuse.fs=" "fuse-t.fs=" "exfat.fs=" "ntfs-3g.fs=another NTFS driver" \
	"Acme-NTFS.fs=another NTFS driver" "myntfs.fs=another NTFS driver" "ntfs-tool=" "=" ; do
	n=${t%%=*}
	w=${t#*=}
	g=$(conflict "$n")
	[ "$g" = "$w" ] || bad "preinstall conflict '$n': '$g', expected '$w'"
done
for n in ufsd_NTFS.fs tuxera_ntfs.fs ntfs.fs ntfsmac; do
	grep -q "\"$n\"" "$root/src/daemon/conflict.c" || bad "conflict.c no longer names $n; check preinstall against it"
done
pass "preinstall classifies bundle names as conflict.c does (15 names)"

# ---- f. the right ----
sed -n "/^cat >\"\$stage\/right.plist\" <<'EOF'\$/,/^EOF\$/p" "$root/packaging/dev-install.sh" | sed '1d;$d' >"$tmp/right.dev"
sed -n "/^right_def='/,/'\$/p" "$post" | sed -e "s/^right_def='//" -e "s/'\$//" >"$tmp/right.post"
if [ -s "$tmp/right.dev" ] && cmp -s "$tmp/right.dev" "$tmp/right.post"; then
	pass "postinstall installs the right with the definition of dev-install.sh"
else
	bad "the right of postinstall differs from dev-install.sh"
	diff "$tmp/right.dev" "$tmp/right.post" | sed 's/^/  /'
fi

# ---- g. rules of the root-run scripts ----
for s in "$pre" "$post"; do
	n=${s##*/}
	# code lines only: comments and the quoted path and right blocks are dropped
	sed -e '/^[[:space:]]*#/d' -e "/^paths='/,/'\$/d" -e "/^right_def='/,/'\$/d" "$s" >"$tmp/code"
	[ "$(sed -n '2,$p' "$s" | grep -m1 -v '^#')" = "set -eu" ] || bad "$n: the first command is not set -eu"
	grep -qx 'PATH=/usr/bin:/bin:/usr/sbin:/sbin' "$s" || bad "$n: PATH is not set to the system directories"
	grep -qx 'umask 022' "$s" || bad "$n: no umask 022"
	if grep -n -E '\$\{?(HOME|TMPDIR|USER|LOGNAME|SUDO_[A-Z]+|INSTALLER_[A-Z_]+|PACKAGE_PATH|DSTROOT|DSTVOLUME|SCRIPT_NAME|PWD|IFS|SHELL|ENV)\b' "$tmp/code"; then
		bad "$n reads an environment variable"
	fi
	# outside the functions (whose $1 and $* are their own arguments) and the quoted awk programs
	sed -e '/^[a-z_]*() {.*}$/d' -e '/^[a-z_]*() {/,/^}$/d' -e "s/'[^']*'//g" "$tmp/code" >"$tmp/top"
	if grep -n -E '\$\{?[0-9@*#]' "$tmp/top" | grep -v -E '\$\{3:-\}|\$\{#hash\}'; then
		bad "$n uses an argument other than \$3"
	fi
	if grep -n -E '(^|[;&|[:space:]])(eval|source|exec)[[:space:]]|(^|[;&|[:space:]])\.[[:space:]]' "$tmp/code"; then
		bad "$n has eval, source, exec or ."
	fi
	tools='launchctl|security|plutil|codesign|pkgutil|stat|sw_vers|uname|mkdir|rm|rmdir|mv|cp|chown|chmod|sed|awk|tr|mktemp|sleep|tail|cat|grep|printf|pwd|install|ditto|kill|killall|logger|id|find|xargs|ln|touch|head|dirname|basename|env|sudo|sh|bash|zsh'
	if sed -e 's/"[^"]*"//g' -e "s/'[^']*'//g" "$tmp/code" | grep -n -E "(^|[;&|(\`[:space:]]|\\\$\\()($tools)([[:space:]]|\$)"; then
		bad "$n runs a program without its absolute path (lines above)"
	fi
	[ "$fail" -ne 0 ] || pass "$n: set -eu, fixed PATH, umask, no environment, only \$3, absolute programs"
done

# ---- h. package identifiers ----
want_id=org.ntfs-macos.pkg.v2
old_id=org.ntfs-macos.pkg
id_bad=0
for f in packaging/build-pkg.sh packaging/scripts/preinstall packaging/uninstall.sh; do
	[ "$(sed -n 's/^pkg_id=//p' "$root/$f")" = "$want_id" ] || { bad "$f: pkg_id is not $want_id"; id_bad=1; }
done
for f in packaging/scripts/preinstall packaging/uninstall.sh; do
	[ "$(sed -n 's/^old_pkg_id=//p' "$root/$f")" = "$old_id" ] || { bad "$f: old_pkg_id is not $old_id"; id_bad=1; }
done
for f in tests/integration/packaging/pkg_release_vm_guest.sh tests/integration/packaging/pkg_preflight_vm_guest.sh; do
	[ "$(sed -n 's/^PKGID=//p' "$root/$f")" = "$want_id" ] || { bad "$f: PKGID is not $want_id"; id_bad=1; }
done
if grep -n 'org\.ntfs-macos\.pkg\([^.]\|$\)' "$root/scripts/release-local.sh"; then bad "release-local.sh names the receipt of the earlier layout"; id_bad=1; fi
grep -q "pkgutil --pkg-info $want_id" "$root/scripts/release-local.sh" || { bad "release-local.sh does not check the receipt $want_id"; id_bad=1; }
[ "$id_bad" -eq 1 ] || pass "the package identifier is $want_id everywhere, and preinstall and uninstall.sh know the earlier $old_id"

# ---- i. no downgrade ----
dg=0
[ "$(grep -c "^new_version='@VERSION@'\$" "$pre")" = 1 ] || { bad "preinstall has no single new_version='@VERSION@' line"; dg=1; }
grep -q "^sed \"s/^new_version='@VERSION@'" "$root/packaging/build-pkg.sh" || { bad "build-pkg.sh does not write the version into preinstall"; dg=1; }
# the substitution of build-pkg.sh, as it runs there
pv=$(sed -n 's/^project(ntfs-macos VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
sed "s/^new_version='@VERSION@'\$/new_version='$pv'/" "$pre" >"$tmp/pre.sub"
if grep -qx "new_version='$pv'" "$tmp/pre.sub" && ! grep -q '@VERSION@' "$tmp/pre.sub" && sh -n "$tmp/pre.sub"; then :; else
	bad "the version $pv is not written into preinstall as build-pkg.sh does it"; dg=1
fi
sed -n '/^ver_older() {$/,/^}$/p' "$pre" >"$tmp/ver.sh"
# shellcheck disable=SC1091
. "$tmp/ver.sh"
for t in "0.1.0 0.2.0 0" "0.2.0 0.1.0 1" "0.1.0 0.1.0 1" "0.1 0.1.0 1" "0.1.0 0.1 1" "0.9 0.10 0" "0.10 0.9 1" \
	"1.0.0 0.99.99.99 1" "0.99.99.99 1.0.0 0" "1.2.3.4 1.2.3.5 0" "x 0.1 2" "0.1 x 2" "0.1.0- 0.1.0 2" "0.1.0-dev 0.1.0 2" \
	"1.2.3.4.5 1 2" "1234567890 1 2" ".1 1 2" "1. 1 2" "1..2 1 2"; do
	# shellcheck disable=SC2086 # the three words of the case
	set -- $t
	ver_older "$1" "$2"
	g=$?
	[ "$g" = "$3" ] || { bad "ver_older $1 $2: $g, expected $3"; dg=1; }
done
ver_older "" 0.1
[ $? = 2 ] || { bad "ver_older of an empty version is not 2"; dg=1; }
ln_dg=$(grep -n '^# ---- 6. no downgrade' "$pre" | cut -d: -f1)
ln_stop=$(grep -n 'launchctl bootout' "$pre" | head -n 1 | cut -d: -f1)
ln_pass=$(grep -n '^say "preflight passed' "$pre" | cut -d: -f1)
if [ -n "$ln_dg" ] && [ -n "$ln_stop" ] && [ -n "$ln_pass" ] && [ "$ln_dg" -lt "$ln_pass" ] && [ "$ln_pass" -lt "$ln_stop" ]; then :; else
	bad "the downgrade check is not before 'preflight passed' and the stop of the daemon"; dg=1
fi
# shellcheck disable=SC2016 # the text of the script, not an expansion
grep -q 'refuse "this package is ntfs-macos $new_version, older than the installed $inst_ver' "$pre" || { bad "preinstall has no downgrade refusal text"; dg=1; }
[ "$dg" -eq 1 ] || pass "preinstall refuses a downgrade: version $pv written by build-pkg.sh, ver_older (20 cases), before the daemon is stopped"

[ "$fail" -eq 0 ] && echo "PASS: payload and package scripts agree with the manifest"
exit "$fail"
