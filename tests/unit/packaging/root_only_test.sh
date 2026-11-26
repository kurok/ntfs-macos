#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-08 (audit M6-F01): the root-only check of the root-run scripts. Reads the scripts and runs
# the shared functions on a throw-away tree in a temporary directory; nothing runs as root and
# nothing is installed.
#
#   a. packaging/scripts/preinstall, packaging/scripts/postinstall, packaging/uninstall.sh and
#      packaging/dev-install.sh each hold the block "# >>> root-only check" ... "# <<< root-only
#      check" exactly once, and the four copies are the same text (a package script cannot source
#      a file of the repository, so each carries a copy); no other definition of root_only_chain,
#      root_only_dir or ro_show, and no physical-path walk (cd -P, pwd -P) is left in them. A
#      copy changed by one character is found (mutant of this lint).
#   b. root_only_chain, root_only_dir and ro_show on a tree with real symbolic links: the owner
#      that /usr/bin/stat reports is replaced by a stand-in (the test cannot chown), the modes and
#      the links and access control lists are real. The case of the audit is there: a user-owned usr/local whose libexec is
#      a link to a root-only directory is refused at usr/local; a root-owned usr/local with the
#      same link is refused at the link.
#   c. mutants of the functions (no link check, no owner check, no mode check, a walk that stops
#      early, a missing path that ends the walk although a file is there) each fail a case of b.
#   d. the callers (issue #6): preinstall refuses through root_only_chain before it changes
#      anything; uninstall.sh checks every dir, file, statedir and statefile with root_only_chain
#      and stops before its first change (launchctl bootout, rm, rmdir, pkgutil --forget) unless
#      --allow-unsafe-ancestors is given, and then still removes each path through may_remove;
#      dev-install.sh refuses through root_only_chain while it reads the manifest, before it
#      builds; no refusal or message builds a `sudo chown` or `sudo chmod` command (audit M6-F06).
#
# Usage: root_only_test.sh ROOT
# Exit: 0 all checks passed, 1 a check failed, 2 usage error.
# The functions under test come from a generated file (ro_bad, ro_why), fake_stat is called by them,
# and the sed scripts of the mutants hold a literal $.
# shellcheck disable=SC2016,SC2154,SC2329
set -u

root=${1:?usage: root_only_test.sh ROOT}
scripts="packaging/scripts/preinstall packaging/scripts/postinstall packaging/uninstall.sh packaging/dev-install.sh"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/root_only_test.XXXXXX")
trap 'chmod -RN "$tmp" 2>/dev/null; rm -rf "$tmp"' EXIT
tmp=$(CDPATH='' cd -P -- "$tmp" && pwd -P)
fail=0
pass() { echo "PASS: $*"; }
bad() { echo "FAIL: $*"; fail=1; }
nl='
'

# block FILE: the shared block of FILE (both marker lines included)
block() { awk '/^# >>> root-only check/ { on = 1 } on { print } /^# <<< root-only check/ { on = 0 }' "$1"; }
# same_blocks FILE...: 0 when each FILE holds the block once and all blocks are the same text
same_blocks() {
	sb_first=
	for sb_f in "$@"; do
		if [ "$(grep -c '^# >>> root-only check' "$sb_f")" != 1 ] || [ "$(grep -c '^# <<< root-only check' "$sb_f")" != 1 ]; then
			echo "  $sb_f: the block markers are not there exactly once"
			return 1
		fi
		block "$sb_f" >"$tmp/blk.cur"
		if [ -z "$sb_first" ]; then
			sb_first=$sb_f
			cp "$tmp/blk.cur" "$tmp/blk.first"
		elif ! cmp -s "$tmp/blk.first" "$tmp/blk.cur"; then
			echo "  the block of $sb_f differs from the block of $sb_first:"
			diff "$tmp/blk.first" "$tmp/blk.cur" | sed 's/^/    /'
			return 1
		fi
	done
	return 0
}

# ---- a. one text in four scripts ----
set --
for s in $scripts; do set -- "$@" "$root/$s"; done
if same_blocks "$@"; then pass "the root-only block is the same text in the four root-run scripts"; else bad "the root-only blocks differ"; fi
block "$root/packaging/scripts/preinstall" >"$tmp/block.sh"
[ "$(wc -l <"$tmp/block.sh")" -gt 20 ] || bad "the root-only block of preinstall is too short to be the check"
for s in $scripts; do
	awk '/^# >>> root-only check/ { on = 1 } !on { print } /^# <<< root-only check/ { on = 0 }' "$root/$s" >"$tmp/rest"
	if grep -n -E '^[[:space:]]*(root_only_chain|root_only_dir|ro_show|check_ancestors)\(\)' "$tmp/rest"; then
		bad "$s defines a root-only function outside the shared block"
	fi
	if grep -n -E 'cd -P|pwd -P' "$tmp/rest"; then
		bad "$s resolves a physical path (cd -P, pwd -P); the root-only check must not follow a link"
	fi
done
pass "no other root-only function and no physical-path walk in the four scripts"
# mutant of the lint: one character of one copy changed
sed '/^# >>> root-only check/,/^# <<< root-only check/s/is a symbolic link/is a symbolic lint/' \
	"$root/packaging/uninstall.sh" >"$tmp/uninstall.mut"
set --
for s in $scripts; do
	if [ "$s" = packaging/uninstall.sh ]; then set -- "$@" "$tmp/uninstall.mut"; else set -- "$@" "$root/$s"; fi
done
if cmp -s "$root/packaging/uninstall.sh" "$tmp/uninstall.mut"; then
	bad "the lint mutant did not change the copy"
elif same_blocks "$@" >/dev/null; then
	bad "the lint accepted a copy changed by one character"
else
	pass "the lint refuses a copy changed by one character"
fi
sed '/^# <<< root-only check/d' "$root/packaging/scripts/postinstall" >"$tmp/postinstall.mut"
if same_blocks "$root/packaging/scripts/preinstall" "$tmp/postinstall.mut" >/dev/null; then
	bad "the lint accepted a copy without its end marker"
else
	pass "the lint refuses a copy without its end marker"
fi

# ---- b. the functions on a real tree ----
grep -q '/bin/ls -lde "$ro_p"' "$tmp/block.sh" || bad "the block no longer reads the access control list with /bin/ls -lde; adjust this test"
grep -q "/usr/bin/stat -f '%Su %Lp' \"\$ro_p\"" "$tmp/block.sh" || bad "the block no longer reads the owner with /usr/bin/stat -f '%Su %Lp'; adjust this test"
# The functions run on a real tree only on macOS: the block reads BSD stat -f and the access control lists of
# /bin/ls -lde, and the test sets them with chmod +a. Elsewhere (the Linux container of Environment A) a, the
# grep checks above and d. still run, and b and c are skipped with a message.
if [ "$(uname -s)" != Darwin ]; then
	echo "skipped: b and c (the functions on a real tree, the mutants) need macOS (BSD stat, ls -lde, chmod +a)"
else
# The owner is a stand-in: "someone" for the paths in user_owned, root for every other path. The
# mode is the real one inside the test tree and 755 above it (the temporary directory and its
# parents are not part of the case).
user_owned=
fake_stat() { # -f FORMAT PATH
	case "$nl$user_owned$nl" in *"$nl$3$nl"*) fs_u=someone ;; *) fs_u=root ;; esac
	case $3 in
	"$T" | "$T"/*) fs_m=$(/usr/bin/stat -f '%Lp' "$3") || return 1 ;;
	*) fs_m=755 ;;
	esac
	printf '%s %s\n' "$fs_u" "$fs_m"
}
# /bin/ls -lde, the real one inside the test tree (its access control lists are real), a plain line
# without a list above it
fake_ls() { # -lde PATH
	case $2 in
	"$T" | "$T"/*) /bin/ls -lde "$2" ;;
	*) printf 'drwxr-xr-x  2 root  wheel  64 Jan  1 00:00 %s\n' "$2" ;;
	esac
}
T=$tmp/t
mkd() { for m_d in "$@"; do mkdir -p "$T/$m_d" && chmod 755 "$T/$m_d"; done; }
mkd rootonly ok/usr/local/libexec a/usr/local b/usr/local c/usr/local/libexec d/usr/local e/usr/local f \
	g/usr/local/libexec h/usr/local/libexec
chmod 755 "$T"
ln -s "$T/rootonly" "$T/a/usr/local/libexec"
ln -s "$T/rootonly" "$T/b/usr/local/libexec"
chmod 775 "$T/c/usr/local/libexec"
: >"$T/d/usr/local/libexec"
ln -s "$T/nowhere" "$T/e/usr/local/libexec"
ln -s "$T/ok/usr" "$T/f/usr"
user_owned="$T/a/usr/local"
# access control lists (the owner of a directory may set them): an entry that allows writing in
# g/usr/local, a deny-only list in h/usr/local
acl_ok=1
chmod +a "everyone allow add_file,add_subdirectory,delete_child" "$T/g/usr/local" 2>/dev/null || acl_ok=0
chmod +a "everyone deny chown,writesecurity" "$T/h/usr/local" 2>/dev/null || acl_ok=0
[ "$acl_ok" -eq 1 ] || bad "could not set an access control list in the test tree (chmod +a)"

# expect NAME FUNCTION PATH WANT-RC [WANT-BAD [WANT-WHY-TEXT]]
expect() {
	e_n=$1 e_fn=$2 e_p=$3 e_rc=$4 e_bad=${5-} e_why=${6-}
	"$e_fn" "$e_p"
	e_got=$?
	if [ "$e_got" != "$e_rc" ]; then
		echo "  case '$e_n': $e_fn returned $e_got, expected $e_rc (ro_bad '$ro_bad', ro_why '$ro_why')"
		return 1
	fi
	if [ -n "$e_bad" ] && [ "$ro_bad" != "$e_bad" ]; then
		echo "  case '$e_n': ro_bad '$ro_bad', expected '$e_bad'"
		return 1
	fi
	case $ro_why in *"$e_why"*) ;; *)
		echo "  case '$e_n': ro_why '$ro_why' does not say '$e_why'"
		return 1
		;;
	esac
	return 0
}
# cases: every case of b; the number of failed cases is the exit status (0: all passed)
cases() {
	c_f=0
	expect "root-only chain, missing tail" root_only_chain "$T/ok/usr/local/libexec/ntfs-macos" 0 || c_f=$((c_f + 1))
	expect "root-only chain to an existing directory" root_only_chain "$T/ok/usr/local/libexec" 0 || c_f=$((c_f + 1))
	expect "the root directory" root_only_chain / 0 || c_f=$((c_f + 1))
	expect "audit M6-F01: user-owned usr/local, libexec a link to a root-only directory" root_only_chain \
		"$T/a/usr/local/libexec/ntfs-macos" 1 "$T/a/usr/local" "is owned by someone with mode 755" || c_f=$((c_f + 1))
	expect "root-owned usr/local, libexec a link to a root-only directory" root_only_chain \
		"$T/b/usr/local/libexec/ntfs-macos" 1 "$T/b/usr/local/libexec" "is a symbolic link" || c_f=$((c_f + 1))
	expect "the link itself as the last component" root_only_chain "$T/b/usr/local/libexec" 1 "$T/b/usr/local/libexec" "is a symbolic link" || c_f=$((c_f + 1))
	expect "a group-writable directory" root_only_chain "$T/c/usr/local/libexec/x" 1 "$T/c/usr/local/libexec" "mode 775" || c_f=$((c_f + 1))
	expect "a file where a directory must be" root_only_chain "$T/d/usr/local/libexec/x" 1 "$T/d/usr/local/libexec" "is not a directory" || c_f=$((c_f + 1))
	expect "a dangling link" root_only_chain "$T/e/usr/local/libexec/x" 1 "$T/e/usr/local/libexec" "is a symbolic link" || c_f=$((c_f + 1))
	expect "a link high up, to a root-only tree" root_only_chain "$T/f/usr/local/libexec" 1 "$T/f/usr" "is a symbolic link" || c_f=$((c_f + 1))
	expect "a relative path" root_only_chain "usr/local" 1 "usr/local" "not an absolute path" || c_f=$((c_f + 1))
	expect "a .. component" root_only_chain "$T/ok/usr/../usr/local" 1 "" "two dots" || c_f=$((c_f + 1))
	expect "an empty component" root_only_chain "$T/ok//usr" 1 "" "empty component" || c_f=$((c_f + 1))
	expect "a . component" root_only_chain "$T/ok/./usr" 1 "$T/ok/./usr" "a dot" || c_f=$((c_f + 1))
	expect "a trailing .. (refused before any directory is read)" root_only_chain "$T/b/usr/local/libexec/.." 1 "$T/b/usr/local/libexec/.." "two dots" || c_f=$((c_f + 1))
	expect "an access control entry that allows writing" root_only_chain "$T/g/usr/local/libexec" 1 "$T/g/usr/local" "access control entry" || c_f=$((c_f + 1))
	expect "a deny-only access control list" root_only_chain "$T/h/usr/local/libexec/x" 0 || c_f=$((c_f + 1))
	expect "root_only_dir, a root-only directory" root_only_dir "$T/ok/usr/local" 0 || c_f=$((c_f + 1))
	expect "root_only_dir, a missing directory" root_only_dir "$T/ok/usr/local/nothing" 1 "$T/ok/usr/local/nothing" "is missing" || c_f=$((c_f + 1))
	expect "root_only_dir, a link to a root-only directory" root_only_dir "$T/b/usr/local/libexec" 1 "$T/b/usr/local/libexec" "is a symbolic link" || c_f=$((c_f + 1))
	expect "root_only_dir below a user-owned directory" root_only_dir "$T/a/usr/local/libexec" 1 "$T/a/usr/local" "is owned by someone" || c_f=$((c_f + 1))
	c_s=$(ro_show "/usr/local/x'; rm -rf / #\$(id)\"
next")
	[ "$c_s" = '"/usr/local/x?? rm -rf / ???id???next"' ] || { echo "  ro_show of a hostile name: $c_s"; c_f=$((c_f + 1)); }
	c_s=$(ro_show "/Library/Application Support/ntfs-macos")
	[ "$c_s" = '"/Library/Application Support/ntfs-macos"' ] || { echo "  ro_show of a plain path: $c_s"; c_f=$((c_f + 1)); }
	return "$c_f"
}
# run_cases BLOCK-FILE: the cases with the functions of BLOCK-FILE (the owner from fake_stat)
run_cases() {
	sed -e 's|/usr/bin/stat |fake_stat |' -e 's|/bin/ls -lde |fake_ls -lde |' "$1" >"$tmp/run.sh"
	(
		# shellcheck disable=SC1091
		. "$tmp/run.sh"
		cases
	)
}
if out=$(run_cases "$tmp/block.sh"); then
	pass "root_only_chain, root_only_dir and ro_show on the test tree (21 paths, 2 names), the case of audit M6-F01 included"
else
	bad "the root-only functions fail on the test tree"
	echo "$out"
fi

# ---- c. mutants of the functions: each must fail a case ----
mutant() { # NAME SED-SCRIPT
	sed "$2" "$tmp/block.sh" >"$tmp/mut.sh"
	if cmp -s "$tmp/block.sh" "$tmp/mut.sh"; then bad "mutant '$1' did not change the block"; return; fi
	if out=$(run_cases "$tmp/mut.sh"); then bad "mutant '$1' passes every case"; else pass "mutant '$1' fails: $(printf '%s\n' "$out" | head -n 1 | sed 's/^ *//')"; fi
}
mutant "no symbolic link check" 's/if \[ -L "\$ro_p" \]; then/if false; then/'
mutant "no owner check" 's/\[ "\${ro_st% \*}" != root \] ||/false ||/'
mutant "no mode check" 's/& 022/\& 0/'
mutant "the walk stops after /" 's/\[ -n "\$ro_rest" \] || return 0/return 0/'
mutant "no access control check" 's/\*" allow "\*)/*" allow-nothing "*)/'
mutant "no up-front component check" 's/\*\/\.\.\/\* | \*\/\.\/\* | \*\/\/\*)/*\/no-such\/*)/'
mutant "a file ends the walk as if missing" 's/\[ -e "\$ro_p" \] || return 0/[ -d "$ro_p" ] || return 0/'
mutant "ro_show passes every character" "s/tr -c 'A-Za-z0-9 \/._,:@+=-' '?'/tr -c '\\\\000-\\\\377' '?'/"

fi

# ---- d. the callers ----
# first LINE-NUMBER of a fixed string in FILE (0 when absent)
at() { grep -n -F -- "$2" "$1" | head -n 1 | cut -d: -f1 | grep . || echo 0; }
pre=$root/packaging/scripts/preinstall
un=$root/packaging/uninstall.sh
dev=$root/packaging/dev-install.sh
c1=$(at "$pre" 'if ! root_only_chain "$d"; then')
c2=$(at "$pre" '/bin/launchctl bootout')
c3=$(at "$pre" '/bin/rm -f')
if [ "$c1" -gt 0 ] && [ "$c1" -lt "$c2" ] && [ "$c1" -lt "$c3" ]; then pass "preinstall refuses through root_only_chain before its first change"; else bad "preinstall: root_only_chain at $c1, bootout at $c2, rm at $c3"; fi
u1=$(at "$un" 'root_only_chain "$pf_d" && continue')
u2=$(at "$un" '$dirs$files$sdirs$sfiles')
u3=$(at "$un" 'if [ "$unsafe" -eq 0 ]; then')
# the first change outside a function body (the bodies of remove_link, may_remove and remove_receipt
# run only when called, after the check): the boot-out of the job
first_change=$(awk '/^[a-z_]+\(\) \{/ { infn = 1 } infn && /^\}/ { infn = 0; next } !infn && /^[^#]*(launchctl bootout|rm -f |rm -rf |rmdir |pkgutil --forget|security authorizationdb remove|remove_receipt )/ { print NR; exit }' "$un")
if [ "$u1" -gt 0 ] && [ "$u2" -gt "$u1" ] && [ "$u3" -gt "$u2" ] && [ -n "$first_change" ] && [ "$u3" -lt "$first_change" ]; then
	pass "uninstall.sh checks every dir, file and state path with root_only_chain and stops before its first change (line $u3 < $first_change)"
else
	bad "uninstall.sh: check at $u1, list at $u2, stop at $u3, first change at $first_change"
fi
grep -q -- '--allow-unsafe-ancestors) unsafe=1' "$un" || bad "uninstall.sh has no --allow-unsafe-ancestors"
n_rm=$(grep -c -E '^[^#]*(rm -f "|rm -rf ")' "$un")
n_mr=$(grep -c -E 'may_remove "[^"]*" \|\| continue' "$un")
if [ "$n_rm" -gt 0 ] && [ "$n_mr" -ge "$((n_rm - 1))" ]; then pass "uninstall.sh: every file removal but the link's goes through may_remove ($n_mr of $n_rm)"; else bad "uninstall.sh: $n_rm removals, $n_mr through may_remove"; fi
d1=$(at "$dev" 'if ! root_only_chain "$an"; then')
d2=$(at "$dev" '# ---- 1. NTFS-3G')
if [ "$d1" -gt 0 ] && [ "$d1" -lt "$d2" ]; then pass "dev-install.sh refuses through root_only_chain before it builds"; else bad "dev-install.sh: root_only_chain at $d1, build at $d2"; fi
sc_bad=0
for s in $scripts; do
	if grep -n -E 'sudo (chown|chmod)' "$root/$s"; then bad "$s suggests a sudo chown or chmod command"; sc_bad=1; fi
done
[ "$sc_bad" -eq 1 ] || pass "no root-run script suggests a sudo chown or chmod command"

[ "$fail" -eq 0 ] && echo "PASS: the root-only check is one text and refuses links and user-writable directories"
exit "$fail"
