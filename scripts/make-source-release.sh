#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# GPL source release archive (T-M6-05, criterion 6.5; ADR-015 amendment, ADR-010, ADR-011).
#
# Makes ntfs-macos-<label>-source.tar.gz: the corresponding source of the package that was built.
# Under one top directory ntfs-macos-<label>-source/ it holds
#   - the file set that the build got (see "What is archived"): the sources, the build scripts, the
#     NTFS-3G patch series in third_party/patches/ntfs-3g/, the documents,
#   - the unmodified NTFS-3G tarball pinned in third_party/deps.lock, as third_party/cache/<file>
#     (its SHA-256 is verified against the lock; nothing is downloaded),
#   - SOURCES.txt: what the archive is, the licence and the corresponding-source statement, the base
#     commit, the tree id, the dirty flag and the tool versions.
#
# What is archived: a git tree, so every file and mode in it is exact. With --tree it is that tree: the
# one scripts/release-local.sh made before the build and gave to the build, so an edit made during
# the build cannot change the archive. Otherwise it is the tree of the commit (tracked files only), or,
# with --allow-dirty or --worktree, the tree that scripts/lib/tree.sh makes from `git ls-files
# --cached --others --exclude-standard` with the working-tree contents (modified files as they are, new
# files that are not ignored, force-added ignored files, deleted files absent, no line-ending
# conversion; third_party/cache is left out and added as the verified tarball). The commit, the tree
# id and the dirty flag (the tree differs from the commit's) are written to SOURCES.txt; the archive is
# the corresponding source of the binary either way.
#
# Reproducibility: the same tree and the same git version give the same bytes. git archive
# writes the commit time as the time of every entry, owner root:root (0/0) and the mode with
# umask 022 (0644 or 0755), entries in tree order; the format is tar.gz made by git's built-in gzip (`tar.tar.gz.command=git archive gzip`: no
# name, no time, not the system gzip). The git version is in SOURCES.txt.
#
# Usage: scripts/make-source-release.sh [--label LABEL] [--out DIR] [--rev COMMIT] [--tree TREE] [--allow-dirty] [--worktree]
#   --label        version label (default: scripts/lib/label.sh, shared with release-local.sh)
#   --out          output directory (default: dist/<label>, where packaging/build-pkg.sh writes)
#   --rev          the commit that was built; refused when HEAD is another commit (HEAD moved)
#   --tree         archive exactly this tree id (no dirty or untracked checks: the caller built it)
#   --allow-dirty  accept uncommitted changes to tracked files (archive the working-tree file set)
#   --worktree     archive the working-tree file set (also untracked files that are not ignored)
#                  although tracked files are clean; release-local.sh uses it for the host build
#   Without those two, an untracked *.patch file in the patch directory is refused.
# Env:   DEPS_LOCK, DEPS_CACHE_DIR (default third_party/deps.lock and third_party/cache)
# Output: <out>/ntfs-macos-<label>-source.tar.gz; its line is written to <out>/SHA256SUMS (an existing
#         line for the same file is replaced, other lines are kept), format of build-pkg.sh.
# Exit:  0 archive made, 1 refused or error, 2 usage error.
set -eu
umask 022

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
# shellcheck source=lib/label.sh
. "$here/lib/label.sh"
# shellcheck source=lib/tree.sh
. "$here/lib/tree.sh"

die() { printf 'make-source-release: error: %s\n' "$1" >&2; exit "${2:-1}"; }
log() { printf 'make-source-release: %s\n' "$*"; }
usage() { printf 'usage: %s [--label LABEL] [--out DIR] [--rev COMMIT] [--tree TREE] [--allow-dirty] [--worktree]\n' "$0" >&2; }

label=
out=
allow_dirty=0
worktree=0
rev_arg=
tree_arg=
while [ $# -gt 0 ]; do
	case "$1" in
	--label) [ $# -ge 2 ] || { usage; die "--label needs a value" 2; }; label=$2; shift ;;
	--out) [ $# -ge 2 ] || { usage; die "--out needs a value" 2; }; out=$2; shift ;;
	--rev) [ $# -ge 2 ] || { usage; die "--rev needs a value" 2; }; rev_arg=$2; shift ;;
	--tree) [ $# -ge 2 ] || { usage; die "--tree needs a value" 2; }; tree_arg=$2; shift ;;
	--allow-dirty) allow_dirty=1 ;;
	--worktree) worktree=1 ;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" 2 ;;
	esac
	shift
done

if command -v sha256sum >/dev/null 2>&1; then
	sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
	sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
	die "need sha256sum or shasum"
fi

cd "$root"
git rev-parse --git-dir >/dev/null 2>&1 || die "not a git checkout"
git archive --help 2>/dev/null | grep -q -e '--add-file' || die "this git has no 'git archive --add-file' (need git 2.32 or newer)"
rev=$(git rev-parse --verify "HEAD^{commit}") || die "no commit at HEAD"
if [ -n "$rev_arg" ]; then
	want=$(git rev-parse --verify "$rev_arg^{commit}") || die "--rev is not a commit: $rev_arg" 2
	[ "$want" = "$rev" ] || die "HEAD moved: the build was of $want, HEAD is $rev now"
fi
ctime=$(git log -1 --format=%ct "$rev")
cdate=$(TZ=UTC date -u -r "$ctime" '+%Y-%m-%dT%H:%M:%SZ' 2>/dev/null || TZ=UTC date -u -d "@$ctime" '+%Y-%m-%dT%H:%M:%SZ')

# --- the tree to archive ---
tmp=$(mktemp -d "${TMPDIR:-/tmp}/srcrel.XXXXXX") || die "mktemp failed"
trap 'rm -rf "${tmp:?}"' EXIT HUP INT TERM
base_tree=$(git rev-parse "$rev^{tree}")
pdir=third_party/patches/ntfs-3g
dirty=
if [ -n "$tree_arg" ]; then
	# The caller built exactly this tree (release-local.sh makes it before the build).
	tree=$(git rev-parse --verify "$tree_arg^{tree}") || die "--tree is not a tree: $tree_arg" 2
else
	if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
		[ "$allow_dirty" -eq 1 ] || die "uncommitted changes to tracked files; commit them or pass --allow-dirty (the archive then holds the working-tree file set)"
	fi
	if [ "$allow_dirty" -eq 1 ] || [ "$worktree" -eq 1 ]; then
		tree=$(ntfsmac_worktree_tree 1 "$tmp") || die "cannot build the tree of the working-tree file set (an untracked nested repository?)"
	else
		untracked_patch=$(git ls-files --others --exclude-standard -- "$pdir" | grep '\.patch$' | head -1 || true)
		[ -z "$untracked_patch" ] || die "untracked patch file $untracked_patch: a build from the working tree applies it, the archive of the commit would not hold it; commit it or pass --worktree"
		tree=$base_tree
	fi
fi
[ "$tree" = "$base_tree" ] || dirty=.dirty
if [ "$tree" = "$base_tree" ]; then tree_dirty=no; else tree_dirty="yes (the tree differs from the commit)"; fi
changes=$(git diff-tree -r --no-renames --name-status "$base_tree" "$tree" || true)

# --- label ---
version=$(ntfsmac_project_version)
[ -n "$version" ] || die "cannot read the project version from CMakeLists.txt"
[ -n "$label" ] || label=$(ntfsmac_make_label "$version" "$dirty")
case $label in '' | *[!A-Za-z0-9._+-]*) die "label has characters outside [A-Za-z0-9._+-]: $label" 2 ;; esac
[ -n "$out" ] || out=$root/dist/$label
case $out in *[[:space:]]*) die "paths with whitespace are not supported" ;; esac

# --- the pinned NTFS-3G tarball ---
lock=${DEPS_LOCK:-$root/third_party/deps.lock}
cache=${DEPS_CACHE_DIR:-$root/third_party/cache}
[ -r "$lock" ] || die "lock file not readable: $lock"
entry=$(awk '$1 == "ntfs-3g" { print $2, $3, $4; exit }' "$lock")
[ -n "$entry" ] || die "no ntfs-3g entry in $lock"
# shellcheck disable=SC2086 # three words
set -- $entry
n3g_version=$1 n3g_url=$2 n3g_sha=$3
n3g_file=${n3g_url##*/}
[ -n "$n3g_file" ] || die "cannot read the file name from the lock entry: $n3g_url"
if [ ! -f "$cache/$n3g_file" ]; then
	printf 'make-source-release: error: the NTFS-3G tarball is not cached: %s\n' "$cache/$n3g_file" >&2
	printf 'make-source-release: fetch it first (it is checked against %s):\n  scripts/fetch-deps.sh ntfs-3g\n' "$lock" >&2
	exit 1
fi
got=$(sha256_of "$cache/$n3g_file")
[ "$got" = "$n3g_sha" ] || die "SHA-256 of $cache/$n3g_file is $got, the lock wants $n3g_sha; delete the file and fetch it again with scripts/fetch-deps.sh ntfs-3g"

# --- the patch series (as in the tree): every patch the README lists as in the series must be there ---
git cat-file -e "$tree:$pdir/README.md" 2>/dev/null || die "$pdir/README.md is missing"
patches=$(git cat-file blob "$tree:$pdir/README.md" | awk -F'|' '
	/^\| 00[0-9][0-9] \|/ {
		num = $2; gsub(/[[:space:]]/, "", num)
		if ($4 ~ /\*\*(Applied|Proposed)/) print num
	}')
[ -n "$patches" ] || die "no applied patch found in the table of $pdir/README.md"
npatch=0
for num in $patches; do
	f=$(git ls-tree --name-only "$tree" "$pdir/" | grep "^$pdir/$num-.*\.patch$" || true)
	[ -n "$f" ] || die "patch $num is listed in $pdir/README.md but there is no $pdir/$num-*.patch in the archived tree"
	[ "$(printf '%s\n' "$f" | wc -l)" -eq 1 ] || die "more than one file for patch $num in $pdir"
	npatch=$((npatch + 1))
done
patch_list=$(git ls-tree --name-only "$tree" "$pdir/" | grep '\.patch$' | sed 's|.*/||')

# --- build the archive ---
name=ntfs-macos-$label-source
archive=$name.tar.gz
mkdir "$tmp/add"
cp "$cache/$n3g_file" "$tmp/add/$n3g_file"
chmod 0644 "$tmp/add/$n3g_file"
git_version=$(git --version)
{
	cat <<EOT
ntfs-macos source release $label
================================

This archive is the corresponding source of the ntfs-macos package ntfs-macos-$label.pkg and of
every binary in it (GPL-2.0-or-later, see COPYING; the licences of the parts are in LICENSES/).
ntfs-macos bundles NTFS-3G and ntfsprogs, which are licensed under the GPL (see the COPYING files
inside the NTFS-3G tarball).

Corresponding-source statement: this archive holds everything needed to build, install and run
the binaries of that package, whether the build tree was clean or not: the complete source of
ntfs-macos as it was built (the tree below), the build scripts and the CMake files that control the
build, the NTFS-3G release tarball exactly as released, and the patch series that the build applies
to it. You may copy, modify and redistribute it under the terms of the GNU General Public License
version 2 or, at your option, any later version.

Not part of this source: macFUSE (libfuse and its headers) is a system prerequisite that the engine
links; it is installed on the build Mac, not bundled and not in this archive. Get macFUSE 5.5.0 or
newer from https://macfuse.github.io/ (source: https://github.com/macfuse/macfuse). The build
finds its headers and library with pkg-config (fuse).

Contents
--------
  .                                   the file set that was built (below)
  third_party/cache/$n3g_file
                                      NTFS-3G $n3g_version, unmodified, SHA-256
                                      $n3g_sha
                                      (from $n3g_url;
                                      the pin is in third_party/deps.lock)
  third_party/patches/ntfs-3g/        the patch series ($npatch patches in the series), applied in
                                      lexical order with patch -p1 to the unpacked tarball by
                                      scripts/build-ntfs3g.sh:
EOT
	printf '%s\n' "$patch_list" | sed 's/^/                                        /'
	cat <<EOT
  SOURCES.txt                         this file

Source
------
  base commit     $rev
  commit date     $cdate
  tree            $tree
  label           $label
  dirty           $tree_dirty
  git             $git_version
  gzip            git's built-in gzip (git archive gzip)
EOT
	if [ -n "$changes" ]; then
		printf '  changes against the base commit (git diff-tree --name-status):\n'
		printf '%s\n' "$changes" | sed 's/^/    /'
	fi
	cat <<EOT

Build
-----
  scripts/bootstrap.sh                check the prerequisites (macOS 27, Apple silicon, macFUSE 5.5.0
                                      or newer, Xcode command line tools, CMake 3.28 or newer)
  scripts/fetch-deps.sh ntfs-3g       finds the tarball above in third_party/cache and checks it
                                      against third_party/deps.lock; no download is needed
  NTFSMAC_SOURCE_REV=$rev
  packaging/build-pkg.sh --label $label
                                      builds the package; the variable makes BUILD-INFO name the base
                                      commit (this archive is not a git clone); docs/INSTALL.md and
                                      packaging/README.md describe the build
EOT
} >"$tmp/SOURCES.txt"
chmod 0644 "$tmp/SOURCES.txt"

mkdir -p "$out"
out=$(CDPATH='' cd -- "$out" && pwd)
# The prefix before each --add-file is the directory of that file; the last --prefix is for the tree.
git -c tar.umask=022 -c core.autocrlf=false -c core.eol=lf -c tar.tar.gz.command="git archive gzip" archive --format=tar.gz -9 --mtime="$cdate" \
	--prefix="$name/third_party/cache/" --add-file="$tmp/add/$n3g_file" \
	--prefix="$name/" --add-file="$tmp/SOURCES.txt" \
	--prefix="$name/" "$tree" >"$tmp/$archive" || die "git archive failed"
mv -f "$tmp/$archive" "${out:?}/$archive"

# --- check what was written ---
list=$tmp/list
tar -tzf "$out/$archive" >"$list" || die "the archive does not read back"
for want in "$name/SOURCES.txt" "$name/third_party/cache/$n3g_file" "$name/third_party/deps.lock" "$name/COPYING"; do
	grep -qxF "$want" "$list" || die "the archive lacks $want"
done
for p in $patch_list; do
	grep -qxF "$name/$pdir/$p" "$list" || die "the archive lacks $pdir/$p"
done
inner=$(tar -xzOf "$out/$archive" "$name/third_party/cache/$n3g_file" | if command -v sha256sum >/dev/null 2>&1; then sha256sum; else shasum -a 256; fi | cut -d' ' -f1)
[ "$inner" = "$n3g_sha" ] || die "the NTFS-3G tarball inside the archive does not match its pin"

# --- SHA256SUMS: same format as build-pkg.sh (shasum -a 256 NAME); replace our line, keep the others ---
sum=$(sha256_of "$out/$archive")
{
	if [ -f "$out/SHA256SUMS" ]; then grep -v "  $archive\$" "$out/SHA256SUMS" || true; fi
	printf '%s  %s\n' "$sum" "$archive"
} >"$tmp/SHA256SUMS.new"
cp "$tmp/SHA256SUMS.new" "$out/SHA256SUMS"

log "commit $rev$dirty, tree $tree, label $label"
log "entries: $(wc -l <"$list" | tr -d ' '), patches: $npatch, size: $(wc -c <"$out/$archive" | tr -d ' ') bytes"
log "sha256: $sum"
log "done: $out/$archive"
