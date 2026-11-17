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
# only if it is empty then (a foreign file keeps it), and never if it is a symbolic link.
#
# A package installed with `installer` (receipt org.ntfs-macos.pkg, ADR-011 developer package) is
# removed the same way: the files of its receipt are removed if each lies below a `dir` of the
# manifest, then the receipt is forgotten.
#
# The journal of unsafe volumes (/var/db/ntfs-macos) and the daemon log are state: they stay unless
# --purge-state is given.
#
# Usage: sudo uninstall.sh [--dry-run] [--purge-state] [--manifest FILE]
# Exit:  0 removed (or nothing installed), 1 error, 2 usage error.
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

die() { printf 'uninstall: %s\n' "$1" >&2; exit "${2:-1}"; }
usage() { printf 'usage: sudo %s [--dry-run] [--purge-state] [--manifest FILE]\n' "$0" >&2; }

while [ $# -gt 0 ]; do
	case "$1" in
	--dry-run) dry=1 ;;
	--purge-state) purge=1 ;;
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
		/usr/local/share/doc/ntfs-macos | /usr/local/share/doc/ntfs-macos/*) return 0 ;;
		"/Library/Application Support/ntfs-macos" | "/Library/Application Support/ntfs-macos/"*) return 0 ;;
		/usr/local/bin/ntfsmac | /Library/LaunchDaemons/$job.plist) [ "$1" = file ] && return 0 ;;
		esac
		;;
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
has_right=0
n=0
while read -r kind mode owner group path || [ -n "$kind" ]; do
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
	esac
done <"$manifest"
[ -n "$files" ] || die "$manifest lists no file"

say() { printf 'uninstall: %s\n' "$*"; }
problems=0

# Warn (the installer refuses) when a directory above a project path is not root-only: whoever
# can write there could have replaced what this script is about to remove.
warned=
while IFS= read -r wp; do
	[ -n "$wp" ] || continue
	_d=${wp%/*}
	[ -n "$_d" ] || _d=/
	until [ -d "$_d" ]; do _d=${_d%/*}; [ -n "$_d" ] || _d=/; done
	_p=$(CDPATH='' cd -P -- "$_d" 2>/dev/null && pwd -P) || continue
	while :; do
		case "$warned" in *"|$_p|"*) break ;; esac
		warned="$warned|$_p|"
		_st=$(stat -f '%Su %Lp' "$_p") || break
		if [ "${_st% *}" != root ] || [ $((0${_st#* } & 022)) -ne 0 ]; then
			say "WARNING: $_p is owned by ${_st% *} with mode ${_st#* }: it is not root-only, so what lies below it may not be what the installer put there"
		fi
		[ "$_p" != / ] || break
		_p=${_p%/*}
		[ -n "$_p" ] || _p=/
	done
done <<EOF2
$files$dirs
EOF2

# ---- a package receipt: its files must lie below a dir of the manifest (checked before any change) ----
have_receipt=0
rfiles=
if pkgutil --pkg-info "$pkg_id" >/dev/null 2>&1; then
	have_receipt=1
	vol=$(pkgutil --pkg-info "$pkg_id" | sed -n 's/^volume: //p')
	loc=$(pkgutil --pkg-info "$pkg_id" | sed -n 's/^location: //p')
	case "$vol|$loc" in
	'/|' | '/|/') ;;
	*) die "unexpected install location '$vol' '$loc' of $pkg_id" ;;
	esac
	rfiles=$(pkgutil --only-files --files "$pkg_id")
	while IFS= read -r f; do
		[ -n "$f" ] || continue
		case $f in *..*) die "refusing receipt path with '..': $f" ;; esac
		ok=0
		while IFS= read -r d; do
			[ -n "$d" ] || continue
			case /$f in "$d"/*) ok=1 ;; esac
		done <<EOF2
$dirs
EOF2
		[ "$ok" -eq 1 ] || die "refusing receipt path outside the directories of the manifest: /$f"
	done <<EOF2
$rfiles
EOF2
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
if [ "$have_receipt" -eq 1 ]; then
	printf '%s\n' "$rfiles" | while IFS= read -r f; do
		[ -n "$f" ] || continue
		if [ "$dry" -eq 1 ]; then
			say "would remove /$f (package receipt)"
		elif [ -e "/$f" ] || [ -L "/$f" ]; then
			rm -f "/$f"
		fi
	done
	# the package payload may hold sub-directories (LICENSES): remove the empty parents of the
	# receipt's files, up to but not including a dir of the manifest (those go in the step below);
	# an empty directory the package did not make is not touched
	if [ "$dry" -eq 0 ]; then
		nl='
'
		printf '%s\n' "$rfiles" | while IFS= read -r f; do
			[ -n "$f" ] || continue
			dd=/${f%/*}
			while [ "$dd" != / ]; do
				case "$nl$dirs" in *"$nl$dd$nl"*) break ;; esac
				rmdir "$dd" 2>/dev/null || break
				dd=${dd%/*}
			done
		done
		pkgutil --forget "$pkg_id" >/dev/null
	else
		say "would forget $pkg_id"
	fi
fi

# ---- files ----
printf '%s' "$files" | while IFS= read -r f; do
	[ -n "$f" ] || continue
	if [ -d "$f" ] && [ ! -L "$f" ]; then
		say "WARNING: $f is a directory, not removed"
	elif [ -e "$f" ] || [ -L "$f" ]; then
		if [ "$dry" -eq 1 ]; then say "would remove $f"; else rm -f "$f"; fi
	fi
done

# ---- directories, deepest first, only when empty ----
sorted=$(printf '%s' "$dirs" | awk 'NF { print length($0) "\t" $0 }' | sort -rn | cut -f2-)
printf '%s\n' "$sorted" | while IFS= read -r d; do
	[ -n "$d" ] && [ -d "$d" ] || continue
	if [ "$dry" -eq 1 ]; then
		say "would remove $d if empty"
	elif rmdir "$d" 2>/dev/null; then
		:
	else
		say "kept $d: it holds files that ntfs-macos did not install"
	fi
done

# ---- state ----
if [ "$purge" -eq 1 ]; then
	printf '%s' "$sfiles" | while IFS= read -r f; do
		[ -n "$f" ] || continue
		if [ -e "$f" ] || [ -L "$f" ]; then
			if [ "$dry" -eq 1 ]; then say "would remove $f"; else rm -f "$f"; fi
		fi
	done
	printf '%s' "$sdirs" | while IFS= read -r d; do
		[ -n "$d" ] || continue
		if [ -d "$d" ] && [ ! -L "$d" ]; then
			if [ "$dry" -eq 1 ]; then say "would remove $d and everything in it"; else rm -rf "$d"; fi
		fi
	done
else
	printf '%s' "$sdirs$sfiles" | while IFS= read -r p; do
		[ -n "$p" ] && { [ -e "$p" ] || [ -L "$p" ]; } && say "kept state $p (--purge-state removes it)" || true
	done
fi

if [ "$dry" -eq 1 ]; then
	say "dry run: nothing was removed"
	exit 0
fi
say "removed ntfs-macos"
[ "$problems" -eq 0 ] || exit 1
