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

[ "$fail" -eq 0 ] && echo "PASS: payload and package scripts agree with the manifest"
exit "$fail"
