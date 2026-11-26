#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Remove an installed ntfs-macos (T-M4-10). Removes exactly the paths that manifest.txt lists, boots
# out the launchd job org.ntfs-macos.ntfsmacd and removes the authorization right
# org.ntfs-macos.journal-modify (ADR-019). Nothing else is touched: macFUSE, NTFS volumes and every
# path the manifest does not list stay as they are.
#
# The manifest is read from the file next to this script (the installed copy is
# /usr/local/libexec/ntfs-macos/manifest.txt) or from --manifest. It is checked completely before
# anything is removed: an entry that is not absolute, holds `..`, a control character or a double
# slash, or lies outside the paths below stops the script with nothing removed. A `dir` is removed
# only if it is empty then (a foreign file keeps it), and never if it is a symbolic link. The
# `link` /usr/local/bin/ntfsmac is removed only when it is a symbolic link to exactly
# /usr/local/libexec/ntfs-macos/ntfsmac. A shared directory is never removed.
#
# A package installed with `installer` is removed the same way: the files of its receipt are
# removed if each lies below a `dir` of the manifest or is exactly a `file` or `link` of the
# manifest (anything else in the receipt refuses the whole run), then the receipt is forgotten.
# The receipt is org.ntfs-macos.pkg (the release package of T-M6-01 or the earlier developer
# package).
#
# The journal of unsafe volumes (/var/db/ntfs-macos) and the daemon log are state: they stay unless
# --purge-state is given.
#
# Root removes nothing below a directory that a user could change (audit M6-F01, M6-F05, issue
# #6). Before any change, every directory from / down to each path of the manifest is checked
# without following a symbolic link: when one is a symbolic link, or is not a real directory owned
# by root and not writable by group or others, the run stops and nothing is removed. Whoever could
# write there could also have replaced this script, so run the uninstall.sh of the release that
# you downloaded and checked against its SHA256SUMS; the installed copy is a convenience. With
# --allow-unsafe-ancestors the run goes on, but still removes a path only when every directory
# from / down to it passes the check; every other path is kept and named, the receipt is then
# kept, and the exit status is 1.
#
# Usage: sudo uninstall.sh [--dry-run] [--purge-state] [--manifest FILE] [--allow-unsafe-ancestors]
# Exit:  0 removed (or nothing installed), 1 error or a path kept (named), 2 usage error.
set -eu

# system tools only: a root script must not run a program that a user put early in PATH
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

pkg_id=org.ntfs-macos.pkg
job=org.ntfs-macos.ntfsmacd
right=org.ntfs-macos.journal-modify

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
manifest=$here/manifest.txt
dry=0
purge=0
unsafe=0

die() { printf 'uninstall: %s\n' "$1" >&2; exit "${2:-1}"; }
usage() { printf 'usage: sudo %s [--dry-run] [--purge-state] [--manifest FILE] [--allow-unsafe-ancestors]\n' "$0" >&2; }

while [ $# -gt 0 ]; do
	case "$1" in
	--dry-run) dry=1 ;;
	--purge-state) purge=1 ;;
	--allow-unsafe-ancestors) unsafe=1 ;;
	--manifest) [ $# -ge 2 ] || { usage; exit 2; }; manifest=$2; shift ;;
	-h | --help) usage; exit 0 ;;
	*) usage; exit 2 ;;
	esac
	shift
done

[ -f "$manifest" ] || die "manifest not found: $manifest"
[ "$dry" -eq 1 ] || [ "$(id -u)" -eq 0 ] || die "run with sudo"

# ---- the paths a manifest may name (nothing else is ever removed) ----
allowed() { # kind path
	case $1 in
	dir | file)
		case $2 in
		/usr/local/libexec/ntfs-macos | /usr/local/libexec/ntfs-macos/*) return 0 ;;
		"/Library/Application Support/ntfs-macos" | "/Library/Application Support/ntfs-macos/"*) return 0 ;;
		"/Library/LaunchDaemons/$job.plist") [ "$1" = file ] && return 0 ;;
		esac
		;;
	link) [ "$2" = /usr/local/bin/ntfsmac ] && return 0 ;;
	statedir) [ "$2" = /var/db/ntfs-macos ] && return 0 ;;
	statefile) [ "$2" = /var/log/ntfsmacd.log ] && return 0 ;;
	right) [ "$2" = "$right" ] && return 0 ;;
	esac
	return 1
}

# ---- read and check the whole manifest first ----
dirs=
files=
sdirs=
sfiles=
links=
nl='
'
link_target=/usr/local/libexec/ntfs-macos/ntfsmac
has_right=0
n=0
while read -r kind _ _ _ path || [ -n "$kind" ]; do
	n=$((n + 1))
	case $kind in '' | '#'*) continue ;; esac
	[ -n "$path" ] || die "$manifest:$n: malformed entry"
	if [ "$kind" != right ]; then
		case $path in
		/*) ;;
		*) die "$manifest:$n: refusing a path that is not absolute: $path" ;;
		esac
		case $path in
		*..* | *//* | */) die "$manifest:$n: refusing path: $path" ;;
		esac
	fi
	case $path in *[[:cntrl:]]*) die "$manifest:$n: refusing path with a control character" ;; esac
	allowed "$kind" "$path" || die "$manifest:$n: refusing $kind outside the paths of ntfs-macos: $path"
	case $kind in
	dir)
		[ ! -L "$path" ] || die "$manifest:$n: refusing a symbolic link as a directory: $path"
		dirs="$dirs$path
"
		;;
	file) files="$files$path
" ;;
	statedir)
		[ ! -L "$path" ] || die "$manifest:$n: refusing a symbolic link: $path"
		sdirs="$sdirs$path
"
		;;
	statefile) sfiles="$sfiles$path
" ;;
	right) has_right=1 ;;
	link) links="$links$path
" ;;
	esac
done <"$manifest"
[ -n "$files" ] || die "$manifest lists no file"

say() { printf 'uninstall: %s\n' "$*"; }

# >>> root-only check (M6-F01). The same text in packaging/scripts/preinstall,
# packaging/scripts/postinstall, packaging/uninstall.sh and packaging/dev-install.sh: a package
# script cannot source a file of the repository, so each carries a copy, and the unit test
# packaging.root_only (tests/unit/packaging/root_only_test.sh) keeps the copies equal.
#
# root_only_chain PATH: 0 when every component of the absolute PATH that exists, from / down to
# PATH itself, is a real directory owned by root, not writable by group or others and without an
# access control entry that allows anything (`ls -lde` shows "allow"; a deny-only list is fine,
# audit M6 review). PATH must not hold an empty, "." or ".." component. A symbolic link is never
# followed: a link at any component fails, so the directory that holds a link is
# always checked as well (a user-owned /usr/local with /usr/local/libexec a link to a root-only
# directory fails at /usr/local). The components below the first missing one do not exist yet,
# which is allowed. On failure ro_bad is the component that failed and ro_why the reason.
root_only_chain() {
	ro_bad=
	ro_why=
	case $1 in
	/*) ;;
	*) ro_bad=$1; ro_why='is not an absolute path'; return 1 ;;
	esac
	if [ "$1" != / ]; then
		case "$1/" in
		*/../* | */./* | *//*) ro_bad=$1; ro_why='has an empty component or a component that is a dot or two dots'; return 1 ;;
		esac
	fi
	ro_rest=${1#/}
	ro_p=/
	while :; do
		if [ -L "$ro_p" ]; then ro_bad=$ro_p; ro_why='is a symbolic link'; return 1; fi
		[ -e "$ro_p" ] || return 0
		if [ ! -d "$ro_p" ]; then ro_bad=$ro_p; ro_why='is not a directory'; return 1; fi
		ro_st=$(/usr/bin/stat -f '%Su %Lp' "$ro_p") || { ro_bad=$ro_p; ro_why='cannot be read'; return 1; }
		if [ "${ro_st% *}" != root ] || [ $((0${ro_st#* } & 022)) -ne 0 ]; then
			ro_bad=$ro_p
			ro_why="is owned by ${ro_st% *} with mode ${ro_st#* }"
			return 1
		fi
		ro_acl=$(/bin/ls -lde "$ro_p") || { ro_bad=$ro_p; ro_why='cannot be read'; return 1; }
		case $ro_acl in
		*"
"*" allow "*)
			ro_bad=$ro_p
			ro_why='has an access control entry that allows changes to it (ls -le shows it)'
			return 1
			;;
		esac
		[ -n "$ro_rest" ] || return 0
		ro_c=${ro_rest%%/*}
		case $ro_rest in */*) ro_rest=${ro_rest#*/} ;; *) ro_rest= ;; esac
		case $ro_c in '' | '.' | '..') ro_bad=$1; ro_why='has an empty component or a component that is a dot or two dots'; return 1 ;; esac
		if [ "$ro_p" = / ]; then ro_p=/$ro_c; else ro_p=$ro_p/$ro_c; fi
	done
}
# root_only_dir DIR: 0 when DIR exists and root_only_chain accepts it (DIR included)
root_only_dir() {
	ro_bad=
	ro_why=
	[ -d "$1" ] || { ro_bad=$1; ro_why='is missing'; return 1; }
	root_only_chain "$1"
}
# ro_show PATH: PATH for a message, in double quotes, for reading only. Each character other than
# a letter, a digit, a space or / . _ , : @ + = - is shown as ?, so a name that a user chose
# cannot end the quotes or add a command or a line to the log.
ro_show() {
	/usr/bin/printf '"%s"' "$(/usr/bin/printf '%s' "$1" | LC_ALL=C /usr/bin/tr -c 'A-Za-z0-9 /._,:@+=-' '?')"
}
# <<< root-only check
# remove_link PATH: the manifest's link, also when a receipt names it. Removed only when it is a
# symbolic link to exactly the CLI of the manifest and its directory is root-only; anything else
# at that path (a file, a link elsewhere, a link in a user-owned directory) is left and named.
remove_link() {
	if [ -L "$1" ]; then
		if [ "$(readlink "$1")" != "$link_target" ]; then
			say "kept $1: it points to $(ro_show "$(readlink "$1")"), not to $link_target"
		elif ! root_only_dir "${1%/*}"; then
			say "kept $1: ${1%/*} is not root-only or is reached through a symbolic link; remove it yourself"
		elif [ "$dry" -eq 1 ]; then
			say "would remove the link $1"
		else
			rm -f "$1"
		fi
	elif [ -e "$1" ]; then
		say "kept $1: it is not a symbolic link to $link_target (an earlier CLI or another program); remove it yourself if it is the CLI of an earlier ntfs-macos"
	fi
}
# ---- what may be removed (M6-F05) ----
# Root removes a path only when every directory from / down to the directory that holds it is a
# real directory (no symbolic link) owned by root and not writable by group or others: whoever can
# write in a directory on the way could have replaced what lies below it, or could point the
# removal at another directory. Any other path is kept and named, the rest is removed, and the run
# exits 1. /var is the system's own link to private/var: the state below it is checked at its real
# path.
kept=0
kept_list=
may_remove() { # PATH: 0 when PATH may be removed; else names it (once), counts it, returns 1
	mr_d=${1%/*}
	[ -n "$mr_d" ] || mr_d=/
	case $mr_d in
	/var | /var/*)
		if [ "$(readlink /var)" = private/var ]; then mr_d=/private$mr_d; fi
		;;
	esac
	if root_only_dir "$mr_d"; then return 0; fi
	case "$nl$kept_list" in *"$nl$1$nl"*) return 1 ;; esac
	kept_list="$kept_list$1$nl"
	kept=$((kept + 1))
	mr_w=kept
	[ "$dry" -eq 0 ] || mr_w='would keep'
	say "$mr_w $(ro_show "$1"): the directory $(ro_show "$ro_bad") $ro_why, so a user may have replaced what is there; it is not removed. Check it, make every directory on its way a real directory owned by root and not writable by group or others, then run this again"
	return 1
}

# ---- the package receipt: every file must be a path of ours (checked before any change) ----
# receipt_files ID: 1 when there is no receipt ID; else 0 with its files (as pkgutil prints them,
# without the leading /) in rc_files. A file must lie below a dir of the manifest or be exactly a
# `file` or `link` of it (the plist in /Library/LaunchDaemons; the CLI file /usr/local/bin/ntfsmac
# of an earlier layout). Anything else stops the run with nothing removed.
receipt_files() {
	rc_files=
	pkgutil --pkg-info "$1" >/dev/null 2>&1 || return 1
	rc_vol=$(pkgutil --pkg-info "$1" | sed -n 's/^volume: //p')
	rc_loc=$(pkgutil --pkg-info "$1" | sed -n 's/^location: //p')
	case "$rc_vol|$rc_loc" in
	'/|' | '/|/') ;;
	*) die "unexpected install location '$rc_vol' '$rc_loc' of $1" ;;
	esac
	rc_files=$(pkgutil --only-files --files "$1")
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		case $f in *..* | *[[:cntrl:]]*) die "refusing receipt path of $1 with '..' or a control character" ;; esac
		ok=0
		while IFS= read -r d; do
			[ -n "$d" ] || continue
			case /$f in "$d"/*) ok=1 ;; esac
		done <<EOF2
$dirs
EOF2
		case "$nl$files$links" in *"$nl/$f$nl"*) ok=1 ;; esac
		[ "$ok" -eq 1 ] || die "refusing receipt path of $1 outside the paths of ntfs-macos: /$f"
	done <<EOF2
$rc_files
EOF2
	return 0
}
have_receipt=0
rfiles=
if receipt_files "$pkg_id"; then
	have_receipt=1
	rfiles=$rc_files
fi

# ---- every directory above a path of the manifest is root-only (M6-F01, issue #6) ----
# Checked before any change, without following a symbolic link. /var is the system's own link to
# private/var: the state below it is checked at its real path.
bad_chain=0
pf_seen=
while IFS= read -r p; do
	[ -n "$p" ] || continue
	pf_d=${p%/*}
	case $pf_d in
	/var | /var/*)
		if [ "$(readlink /var)" = private/var ]; then pf_d=/private$pf_d; fi
		;;
	esac
	case "$nl$pf_seen" in *"$nl$pf_d$nl"*) continue ;; esac
	pf_seen="$pf_seen$pf_d$nl"
	root_only_chain "$pf_d" && continue
	bad_chain=1
	say "the directory $(ro_show "$ro_bad") above $(ro_show "$p") $ro_why, so a user could have replaced what lies below it"
done <<EOF2
$dirs$files$sdirs$sfiles
EOF2
if [ "$bad_chain" -eq 1 ]; then
	if [ "$unsafe" -eq 0 ]; then
		die "nothing was removed. Every directory above the install must be a real directory (not a symbolic link), owned by root and not writable by group or others. Replace a symbolic link by a real directory, or give each directory named above to root and remove the write permission of group and others, then run this again. This script may itself have been replaced: run the uninstall.sh of the release you downloaded and checked against its SHA256SUMS. With --allow-unsafe-ancestors the rest is removed and every path below those directories is kept and named."
	fi
	say "--allow-unsafe-ancestors: the rest is removed; every path below those directories is kept and named"
fi

# ---- the launchd job ----
if launchctl print "system/$job" >/dev/null 2>&1; then
	if [ "$dry" -eq 1 ]; then
		say "would boot out system/$job"
	else
		launchctl bootout "system/$job" >/dev/null 2>&1 || launchctl bootout system "/Library/LaunchDaemons/$job.plist" >/dev/null 2>&1 || true
		i=0
		while launchctl print "system/$job" >/dev/null 2>&1 && [ "$i" -lt 70 ]; do sleep 1; i=$((i + 1)); done
		if launchctl print "system/$job" >/dev/null 2>&1; then
			die "could not boot out system/$job; nothing was removed (a running daemon would keep serving files that are about to go)"
		fi
	fi
fi

# ---- the authorization right ----
if [ "$has_right" -eq 1 ]; then
	if [ "$dry" -eq 1 ]; then
		say "would remove the authorization right $right"
	else
		security authorizationdb remove "$right" >/dev/null 2>&1 || true
	fi
fi

# ---- the package receipt: remove its files ----
# remove_receipt ID FILES: removes the files of a checked receipt (each through may_remove; the
# manifest's link by its own rule), then the empty directories that held them: only directories
# strictly below a dir of the manifest (the dirs of the manifest go in the step below), and only in
# a root-only directory. A shared directory (/usr/local/bin, /Library/LaunchDaemons) is never
# removed. The receipt is forgotten when nothing of it was kept.
remove_receipt() {
	rr_k=$kept
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		# a receipt path that is the manifest's link follows the rule of the link
		case "$nl$links" in *"$nl/$f$nl"*) remove_link "/$f"; continue ;; esac
		[ -e "/$f" ] || [ -L "/$f" ] || continue
		may_remove "/$f" || continue
		if [ "$dry" -eq 1 ]; then
			say "would remove /$f (receipt $1)"
		else
			rm -f "/$f"
		fi
	done <<EOF2
$2
EOF2
	if [ "$dry" -eq 1 ]; then
		say "would forget $1"
		return 0
	fi
	printf '%s\n' "$2" | while IFS= read -r f; do
		[ -n "$f" ] || continue
		dd=/${f%/*}
		while [ "$dd" != / ]; do
			case "$nl$dirs" in *"$nl$dd$nl"*) break ;; esac
			below=0
			while IFS= read -r od; do
				[ -n "$od" ] || continue
				case $dd in "$od"/*) below=1 ;; esac
			done <<EOF2
$dirs
EOF2
			[ "$below" -eq 1 ] || break
			root_only_dir "${dd%/*}" || break
			rmdir "$dd" 2>/dev/null || break
			dd=${dd%/*}
		done
	done
	if [ "$kept" -eq "$rr_k" ]; then
		pkgutil --forget "$1" >/dev/null
		say "forgot the receipt $1"
	else
		say "kept the receipt $1: paths of it are still there (named above); run this again after the fix"
	fi
}
[ "$have_receipt" -eq 0 ] || remove_receipt "$pkg_id" "$rfiles"

# ---- files ----
while IFS= read -r f; do
	[ -n "$f" ] || continue
	if [ -d "$f" ] && [ ! -L "$f" ]; then
		say "WARNING: $f is a directory, not removed"
	elif [ -e "$f" ] || [ -L "$f" ]; then
		may_remove "$f" || continue
		if [ "$dry" -eq 1 ]; then say "would remove $f"; else rm -f "$f"; fi
	fi
done <<EOF2
$files
EOF2

# ---- the convenience link: only a symbolic link to exactly the CLI of the manifest ----
printf '%s' "$links" | while IFS= read -r l; do
	[ -n "$l" ] || continue
	remove_link "$l"
done

# ---- directories, deepest first, only when empty ----
sorted=$(printf '%s' "$dirs" | awk 'NF { print length($0) "\t" $0 }' | sort -rn | cut -f2-)
while IFS= read -r d; do
	[ -n "$d" ] && [ -d "$d" ] || continue
	may_remove "$d" || continue
	if [ "$dry" -eq 1 ]; then
		say "would remove $d if empty"
	elif rmdir "$d" 2>/dev/null; then
		:
	else
		say "kept $d: it holds files that ntfs-macos did not install"
	fi
done <<EOF2
$sorted
EOF2

# ---- state ----
if [ "$purge" -eq 1 ]; then
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		if [ -e "$f" ] || [ -L "$f" ]; then
			may_remove "$f" || continue
			if [ "$dry" -eq 1 ]; then say "would remove $f"; else rm -f "$f"; fi
		fi
	done <<EOF2
$sfiles
EOF2
	while IFS= read -r d; do
		[ -n "$d" ] || continue
		if [ -d "$d" ] && [ ! -L "$d" ]; then
			may_remove "$d" || continue
			# the directory itself too, before its tree is removed: a link or another owner in it is kept
			sd=$d
			case $sd in
			/var/*)
				if [ "$(readlink /var)" = private/var ]; then sd=/private$sd; fi
				;;
			esac
			if ! root_only_dir "$sd"; then
				kept=$((kept + 1))
				say "kept $(ro_show "$d"): the directory $(ro_show "$ro_bad") $ro_why; it is not removed"
				continue
			fi
			if [ "$dry" -eq 1 ]; then say "would remove $d and everything in it"; else rm -rf "$d"; fi
		fi
	done <<EOF2
$sdirs
EOF2
else
	printf '%s' "$sdirs$sfiles" | while IFS= read -r p; do
		[ -n "$p" ] && { [ -e "$p" ] || [ -L "$p" ]; } && say "kept state $p (--purge-state removes it)" || true
	done
fi

if [ "$dry" -eq 1 ]; then
	[ "$kept" -eq 0 ] || say "dry run: $kept path(s) would be kept (named above)"
	say "dry run: nothing was removed"
	exit 0
fi
if [ "$kept" -ne 0 ]; then
	say "removed ntfs-macos except $kept path(s) named above: each lies below a directory that is a symbolic link or is not owned by root and protected from other users"
	exit 1
fi
say "removed ntfs-macos"
