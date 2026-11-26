#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-06 (T-M6-05): scripts/make-source-release.sh, run in a throw-away git repository that holds
# a copy of the script, a CMakeLists.txt with a project version, the real patch series, a fake
# deps.lock entry and a fake small NTFS-3G tarball. Nothing is downloaded; the real repository and
# the real tarball are not touched.
#
# Usage: source_release_test.sh PROJECT_ROOT
# Exit: 0 all passed, 1 a check failed, 2 usage error.
# shellcheck disable=SC2015 # ok and bad never fail, so A && ok || bad is an if-then-else here
set -u

proj=${1:?usage: source_release_test.sh PROJECT_ROOT}
script=$proj/scripts/make-source-release.sh
[ -f "$script" ] || { echo "no script: $script"; exit 2; }
[ -d "$proj/third_party/patches/ntfs-3g" ] || { echo "no patch series in $proj"; exit 2; }
fail=0
pass=0

tmp=$(mktemp -d "${TMPDIR:-/tmp}/srcrel-test.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

if command -v sha256sum >/dev/null 2>&1; then
	sha_of() { sha256sum "$1" | cut -d' ' -f1; }
else
	sha_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
fi

ok() { pass=$((pass + 1)); }
bad() { echo "FAIL: $*"; fail=$((fail + 1)); }
# check NAME WANT_EXIT PATTERN COMMAND...: exit status and a grep pattern on the combined output.
check() {
	name=$1 want=$2 pat=$3
	shift 3
	"$@" >"$tmp/out" 2>&1
	rc=$?
	if [ "$rc" != "$want" ]; then
		bad "$name: exit $rc, want $want"
		sed 's/^/    /' "$tmp/out"
	elif ! grep -q -- "$pat" "$tmp/out"; then
		bad "$name: output lacks '$pat'"
		sed 's/^/    /' "$tmp/out"
	else
		ok
	fi
}

# --- the throw-away repository ---
repo=$tmp/repo
mkdir -p "$repo/scripts" "$repo/third_party/cache" "$repo/third_party/patches"
cp "$script" "$repo/scripts/make-source-release.sh"
mkdir "$repo/scripts/lib"
cp "$proj/scripts/lib/label.sh" "$proj/scripts/lib/tree.sh" "$repo/scripts/lib/"
cp -R "$proj/third_party/patches/ntfs-3g" "$repo/third_party/patches/ntfs-3g"
printf 'project(ntfs-macos VERSION 9.8.7 LANGUAGES C)\n' >"$repo/CMakeLists.txt"
printf 'GPL text\n' >"$repo/COPYING"
printf 'to be deleted\n' >"$repo/gone.txt"
printf 'untracked in git\n' >"$repo/ignored.txt"
printf 'ignored.txt\nthird_party/cache/\n' >"$repo/.gitignore"
tarball=ntfs-3g_ntfsprogs-0.0.1.tgz
printf 'fake ntfs-3g tarball\n' >"$tmp/$tarball"
sha=$(sha_of "$tmp/$tarball")
printf 'ntfs-3g 0.0.1 https://example.invalid/%s %s\n' "$tarball" "$sha" >"$repo/third_party/deps.lock"
git -C "$repo" init -q . || exit 2
git -C "$repo" add -A
GIT_COMMITTER_DATE='2026-01-02T03:04:05Z' git -C "$repo" -c user.name=t -c user.email=t@example.invalid \
	-c commit.gpgsign=false commit -q -m test --date '2026-01-02T03:04:05Z' || exit 2
cp "$tmp/$tarball" "$repo/third_party/cache/$tarball"
base=$(git -C "$repo" rev-parse HEAD)
base_tree=$(git -C "$repo" rev-parse 'HEAD^{tree}')

run() { (cd "$repo" && "$@"); }
mk() { run sh scripts/make-source-release.sh "$@"; }

# 1. clean tree: layout, tarball, SOURCES.txt, SHA256SUMS
out1=$tmp/out1
check clean-run 0 'done:' mk --label t1 --out "$out1"
arc=$out1/ntfs-macos-t1-source.tar.gz
top=ntfs-macos-t1-source
if [ -f "$arc" ]; then
	tar -tzf "$arc" >"$tmp/list"
	for want in "$top/" "$top/CMakeLists.txt" "$top/COPYING" "$top/scripts/make-source-release.sh" \
		"$top/third_party/deps.lock" "$top/third_party/cache/$tarball" "$top/SOURCES.txt" \
		"$top/third_party/patches/ntfs-3g/README.md" "$top/third_party/patches/ntfs-3g/0016-index-root-block-size-limit.patch"; do
		grep -qxF "$want" "$tmp/list" && ok || bad "archive lacks $want"
	done
	grep -qv "^$top/" "$tmp/list" && bad "entry outside the top directory: $(grep -v "^$top/" "$tmp/list" | head -1)" || ok
	grep -q 'ignored.txt' "$tmp/list" && bad "an untracked ignored file is in the archive" || ok
	# the tarball inside is the cached one; SOURCES.txt carries the licence and the statement
	mkdir "$tmp/x" && tar -xzf "$arc" -C "$tmp/x"
	[ "$(sha_of "$tmp/x/$top/third_party/cache/$tarball")" = "$sha" ] && ok || bad "tarball in the archive differs"
	grep -q 'GPL-2.0-or-later' "$tmp/x/$top/SOURCES.txt" && grep -q 'Corresponding-source statement' "$tmp/x/$top/SOURCES.txt" &&
		grep -q "$sha" "$tmp/x/$top/SOURCES.txt" && grep -q '^  dirty  *no' "$tmp/x/$top/SOURCES.txt" && ok || bad "SOURCES.txt lacks the licence, the statement, the digest or the dirty line"
	# owner 0/0 and the commit time on every entry
	tar -tzvf "$arc" | awk '{ if ($3 != "root" || $4 != "root") n++ } END { exit n > 0 }' && ok || bad "an entry is not owned by root:root"
	tar -tzvf "$arc" | grep -v 'Jan  2  *\(03:04\|2026\)' | grep -q . && bad "an entry has another time than the commit time" || ok
	# gzip -n: no name, no time in the header (bytes 4..8 = mtime = 0, flags 0)
	[ "$(od -An -tx1 -j3 -N5 "$arc" | tr -d ' \n')" = 0000000000 ] && ok || bad "gzip header carries a name or time"
	# modes: umask 022 (no group write), as the header promises
	tar -tzvf "$arc" | grep -q '^[-d]rw[x-]rw' && bad "an entry is group-writable" || ok
	# the tree of a clean run is the tree of HEAD; the base commit and the git version are recorded
	grep -q "^  tree  *$base_tree\$" "$tmp/x/$top/SOURCES.txt" && grep -q "^  base commit  *$base\$" "$tmp/x/$top/SOURCES.txt" && ok || bad "SOURCES.txt lacks the tree or the base commit"
	# the build section names the commit and its time, as build-pkg.sh reads them (T-M6-04)
	grep -q "^  NTFSMAC_SOURCE_REV=$base\$" "$tmp/x/$top/SOURCES.txt" &&
		grep -q "^  SOURCE_DATE_EPOCH=$(git -C "$repo" log -1 --format=%ct "$base")\$" "$tmp/x/$top/SOURCES.txt" && ok || bad "SOURCES.txt lacks NTFSMAC_SOURCE_REV or SOURCE_DATE_EPOCH (the commit time)"
	grep -q '^  git  *git version' "$tmp/x/$top/SOURCES.txt" && grep -q "^  gzip  *git's built-in" "$tmp/x/$top/SOURCES.txt" && ok || bad "SOURCES.txt lacks the git or gzip version"
	grep -q 'https://macfuse.github.io/' "$tmp/x/$top/SOURCES.txt" && grep -q 'system prerequisite' "$tmp/x/$top/SOURCES.txt" && ok || bad "SOURCES.txt lacks the macFUSE note"
	# a clean archive holds exactly the files of the commit's tree, plus the tarball and SOURCES.txt
	git -C "$repo" ls-tree -r --name-only HEAD | sed "s|^|$top/|" >"$tmp/want"
	{ echo "$top/SOURCES.txt"; echo "$top/third_party/cache/$tarball"; } >>"$tmp/want"
	sort "$tmp/want" >"$tmp/want.s"
	grep -v '/$' "$tmp/list" | sort >"$tmp/have.s"
	cmp -s "$tmp/want.s" "$tmp/have.s" && ok || bad "a clean archive does not hold exactly the tree of HEAD"
	# SHA256SUMS in the format of build-pkg.sh
	[ "$(cat "$out1/SHA256SUMS")" = "$(sha_of "$arc")  ntfs-macos-t1-source.tar.gz" ] && ok || bad "SHA256SUMS line is wrong"
fi

# 2. SHA256SUMS: append, and replace our own line
printf 'abc123  other.pkg\n' >"$out1/SHA256SUMS"
check sums-append 0 'done:' mk --label t1 --out "$out1"
[ "$(wc -l <"$out1/SHA256SUMS" | tr -d ' ')" = 2 ] && grep -q '  other.pkg$' "$out1/SHA256SUMS" && ok || bad "SHA256SUMS was not appended"

# 3. determinism: two runs, two output directories, same bytes
out2=$tmp/out2
check second-run 0 'done:' mk --label t1 --out "$out2"
if [ -f "$arc" ] && [ -f "$out2/ntfs-macos-t1-source.tar.gz" ]; then
	cmp -s "$arc" "$out2/ntfs-macos-t1-source.tar.gz" && ok || bad "two runs differ"
else
	bad "no archive to compare"
fi

# 4. the default label: <version>-dev.<date>.g<commit>
check default-label 0 'ntfs-macos-9\.8\.7-dev\.[0-9]*\.g[0-9a-f]\{12\}-source\.tar\.gz' mk --out "$tmp/out3"

# 5. dirty tree: refused, then accepted; the archive holds what was built (modified, new, deleted), recorded
echo changed >>"$repo/COPYING"
printf 'new file\n' >"$repo/newfile.txt"
rm "$repo/gone.txt"
check dirty-refused 1 'uncommitted changes' mk --label t4 --out "$tmp/out4"
[ -e "$tmp/out4" ] && bad "a dirty refusal wrote output" || ok
check dirty-allowed 0 'done:' mk --label t4 --out "$tmp/out4" --allow-dirty
d=ntfs-macos-t4-source
if [ -f "$tmp/out4/$d.tar.gz" ]; then
	tar -xzOf "$tmp/out4/$d.tar.gz" $d/SOURCES.txt >"$tmp/dsrc"
	grep -q '^  dirty  *yes' "$tmp/dsrc" && ok || bad "--allow-dirty is not recorded"
	grep -q "^  base commit  *$base\$" "$tmp/dsrc" && ok || bad "dirty SOURCES.txt lacks the base commit"
	grep -q "^  tree  *$base_tree\$" "$tmp/dsrc" && bad "dirty tree id equals the commit's" || ok
	grep -q '^    M	COPYING' "$tmp/dsrc" && grep -q '^    A	newfile.txt' "$tmp/dsrc" && grep -q '^    D	gone.txt' "$tmp/dsrc" && ok || bad "dirty SOURCES.txt lacks the change list"
	tar -xzOf "$tmp/out4/$d.tar.gz" $d/COPYING | grep -qx changed && ok || bad "the dirty archive lacks the modification"
	tar -xzOf "$tmp/out4/$d.tar.gz" $d/newfile.txt 2>/dev/null | grep -qx 'new file' && ok || bad "the dirty archive lacks the new file"
	tar -tzf "$tmp/out4/$d.tar.gz" | grep -qx "$d/gone.txt" && bad "the dirty archive holds a deleted file" || ok
	tar -tzf "$tmp/out4/$d.tar.gz" | grep -q 'third_party/cache/.*tgz' && ok || bad "the dirty archive lacks the tarball"
	tar -tzf "$tmp/out4/$d.tar.gz" | grep -q 'ignored.txt' && bad "the dirty archive holds an ignored file" || ok
else
	bad "no dirty archive"
fi
# --worktree takes the working-tree file set although tracked files are clean (the new file is untracked)
git -C "$repo" checkout -q -- COPYING gone.txt
check worktree-clean-tracked 0 'done:' mk --label t4w --out "$tmp/out4w" --worktree
tar -tzf "$tmp/out4w/ntfs-macos-t4w-source.tar.gz" | grep -qx "ntfs-macos-t4w-source/newfile.txt" && ok || bad "--worktree lacks the untracked file"
rm -f "$repo/newfile.txt"
check worktree-same-as-clean 0 'done:' mk --label t1 --out "$tmp/out4c" --worktree
cmp -s "$arc" "$tmp/out4c/ntfs-macos-t1-source.tar.gz" && ok || bad "a clean --worktree archive differs from the clean one"

# 5b. an untracked patch: refused without --worktree / --allow-dirty (a build of the working tree would apply it)
printf 'x\n' >"$repo/third_party/patches/ntfs-3g/0099-untracked.patch"
check untracked-patch 1 'untracked patch file' mk --label t4p --out "$tmp/out4p"
check untracked-patch-worktree 0 'done:' mk --label t4p --out "$tmp/out4p" --worktree
tar -tzf "$tmp/out4p/ntfs-macos-t4p-source.tar.gz" | grep -q '0099-untracked.patch' && ok || bad "--worktree lacks the untracked patch"
rm -f "$repo/third_party/patches/ntfs-3g/0099-untracked.patch"

# 5c. --rev: the commit that was built; HEAD moved is refused
check rev-ok 0 'done:' mk --label t4r --out "$tmp/out4r" --rev "$base"
git -C "$repo" -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q --allow-empty -m moved
check rev-moved-commit 1 'HEAD moved' mk --label t4s --out "$tmp/out4s" --rev "$base"
[ -e "$tmp/out4s" ] && bad "a moved HEAD wrote output" || ok
git -C "$repo" reset -q --hard "$base"

# 6. sha mismatch: refused
printf 'tampered\n' >"$repo/third_party/cache/$tarball"
check sha-mismatch 1 'SHA-256' mk --label t5 --out "$tmp/out5"
[ -e "$tmp/out5/ntfs-macos-t5-source.tar.gz" ] && bad "a mismatch wrote an archive" || ok

# 7. missing tarball: exit 1 with the hint, nothing downloaded
rm -f "$repo/third_party/cache/$tarball"
check missing-tarball 1 'scripts/fetch-deps.sh ntfs-3g' mk --label t6 --out "$tmp/out6"
[ -e "$tmp/out6/ntfs-macos-t6-source.tar.gz" ] && bad "a missing tarball wrote an archive" || ok
cp "$tmp/$tarball" "$repo/third_party/cache/$tarball"

# 7b. what is archived is exact: CRLF kept under core.autocrlf=input, a force-added ignored file in
git -C "$repo" config core.autocrlf input
printf 'a\r\nb\r\n' >"$repo/crlf.txt"
git -C "$repo" add -f ignored.txt
check dirty-crlf 0 'done:' mk --label t8 --out "$tmp/out8" --allow-dirty
x=ntfs-macos-t8-source
if [ -f "$tmp/out8/$x.tar.gz" ]; then
	[ "$(tar -xzOf "$tmp/out8/$x.tar.gz" $x/crlf.txt | grep -c "$(printf '\r')")" = 2 ] && ok || bad "CRLF was changed in the archive (autocrlf=input)"
	tar -tzf "$tmp/out8/$x.tar.gz" | grep -qx "$x/ignored.txt" && ok || bad "a force-added ignored file is not in the archive"
else
	bad "no crlf archive"
fi
# an untracked nested repository cannot be archived exactly: refused
git -C "$repo" init -q nested && printf 'x\n' >"$repo/nested/f"
check nested-repo 1 'nested' mk --label t8n --out "$tmp/out8n" --allow-dirty
rm -rf "$repo/nested"

# 7c. --tree: exactly that tree, whatever is edited afterwards (the build/archive race)
mkdir "$tmp/tt"
tree=$(cd "$repo" && . ./scripts/lib/tree.sh && ntfsmac_worktree_tree 1 "$tmp/tt")
echo late >>"$repo/COPYING"
echo late2 >"$repo/crlf.txt"
check tree-arg 0 'done:' mk --label t9 --out "$tmp/out9" --rev "$base" --tree "$tree"
x=ntfs-macos-t9-source
if [ -f "$tmp/out9/$x.tar.gz" ]; then
	tar -xzOf "$tmp/out9/$x.tar.gz" $x/COPYING | grep -qx late && bad "an edit after the tree was made is in the archive" || ok
	[ "$(tar -xzOf "$tmp/out9/$x.tar.gz" $x/crlf.txt | grep -c "$(printf '\r')")" = 2 ] && ok || bad "crlf.txt differs from the tree"
	tar -xzOf "$tmp/out9/$x.tar.gz" $x/SOURCES.txt | grep -q "^  tree  *$tree\$" && ok || bad "SOURCES.txt does not record the --tree id"
else
	bad "no --tree archive"
fi
check tree-bad 2 'not a tree' mk --label t9b --out "$tmp/out9b" --tree 1234567890123456789012345678901234567890
git -C "$repo" reset -q --hard "$base"
git -C "$repo" config --unset core.autocrlf
rm -f "$repo/crlf.txt"

# 7d. release-local.sh with a stub build-pkg.sh (the host path; no VM, no CI): the build gets the tree,
# an edit while it builds does not reach the archive, an untracked file makes the label dirty, and a
# moved HEAD removes the package.
mkdir -p "$repo/packaging"
cp "$proj/scripts/release-local.sh" "$repo/scripts/release-local.sh"
cat >"$repo/packaging/build-pkg.sh" <<'STUB'
#!/bin/sh
root=$(cd "$(dirname "$0")/.." && pwd)
while [ $# -gt 0 ]; do
	case $1 in --label) label=$2; shift ;; --out) out=$2; shift ;; esac
	shift
done
mkdir -p "$out"
cp "$root/COPYING" "$out/seen-COPYING"
ls "$root" >"$out/seen-ls"
printf '%s\n' "$NTFSMAC_SOURCE_REV" >"$out/seen-rev"
echo late >>"$LIVE/COPYING"
[ -z "${STUB_MOVE:-}" ] || git -C "$LIVE" -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q --allow-empty -m moved
echo pkg >"$out/ntfs-macos-$label.pkg"
(cd "$out" && shasum -a 256 "ntfs-macos-$label.pkg" >SHA256SUMS)
STUB
printf 'dist/\nbuild/\n' >>"$repo/.gitignore"
git -C "$repo" add -A
git -C "$repo" -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q -m stub
rl_tree=$(git -C "$repo" rev-parse 'HEAD^{tree}')
env LIVE="$repo" sh -c "cd '$repo' && sh scripts/release-local.sh --no-ci --host" >"$tmp/rl.out" 2>&1 && ok || { bad "release-local host run failed"; sed 's/^/    /' "$tmp/rl.out"; }
d=$(find "$repo/dist" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1)/
if [ "$d" != / ] && ls "$d"ntfs-macos-*-source.tar.gz >/dev/null 2>&1; then
	sa=$(ls "$d"ntfs-macos-*-source.tar.gz)
	sx=$(basename "$sa" .tar.gz)
	case $d in *.dirty/) bad "a clean tree got a dirty label" ;; *) ok ;; esac
	tar -xzOf "$sa" "$sx/COPYING" | grep -qx late && bad "an edit made during the build is in the archive" || ok
	grep -qx late "$d/seen-COPYING" && bad "the build saw an edit made during the build" || ok
	tar -xzOf "$sa" "$sx/SOURCES.txt" | grep -q "^  tree  *$rl_tree\$" && ok || bad "SOURCES.txt tree is not the commit's tree"
	grep -q "$(basename "$sa")" "$d/SHA256SUMS" && grep -q '\.pkg$' "$d/SHA256SUMS" && ok || bad "SHA256SUMS lacks the package or the source archive"
else
	bad "no source archive from release-local"
fi
git -C "$repo" checkout -q -- COPYING
rm -rf "$repo/dist"
printf 'untracked\n' >"$repo/extra.txt"
env LIVE="$repo" sh -c "cd '$repo' && sh scripts/release-local.sh --no-ci --host" >"$tmp/rl.out" 2>&1 && ok || { bad "release-local host run (untracked file) failed"; sed 's/^/    /' "$tmp/rl.out"; }
d=$(find "$repo/dist" -mindepth 1 -maxdepth 1 -type d 2>/dev/null | head -1)/
case $d in *.dirty/) ok ;; *) bad "an untracked file did not make the label dirty: $d" ;; esac
grep -qx extra.txt "$d/seen-ls" && ok || bad "the host build did not get the untracked file"
grep -q '\.dirty$' "$d/seen-rev" && ok || bad "BUILD-INFO source rev lacks .dirty"
sa=$(ls "$d"ntfs-macos-*-source.tar.gz 2>/dev/null)
[ -n "$sa" ] && tar -tzf "$sa" | grep -q '/extra.txt$' && ok || bad "the source archive lacks the untracked file"
[ -n "$sa" ] && tar -xzOf "$sa" "$(basename "$sa" .tar.gz)/SOURCES.txt" | grep -q '^  dirty  *yes' && ok || bad "SOURCES.txt does not say dirty"
git -C "$repo" checkout -q -- COPYING
rm -rf "$repo/dist" "$repo/extra.txt"
env LIVE="$repo" STUB_MOVE=1 sh -c "cd '$repo' && sh scripts/release-local.sh --no-ci --host" >"$tmp/rl.out" 2>&1 && bad "release-local accepted a moved HEAD" || ok
grep -q 'HEAD moved' "$tmp/rl.out" && ok || bad "no HEAD moved message"
ls "$repo"/dist/*/ntfs-macos-*.pkg >/dev/null 2>&1 && bad "a package was left without its source" || ok
git -C "$repo" reset -q --hard "$rl_tree" 2>/dev/null || true
git -C "$repo" checkout -q -- COPYING
rm -rf "$repo/dist"

# 8. a patch listed in the README but not in the repository: refused
git -C "$repo" rm -q third_party/patches/ntfs-3g/0010-valid-descr-header-length.patch
git -C "$repo" -c user.name=t -c user.email=t@example.invalid -c commit.gpgsign=false commit -q -m rmpatch
check missing-patch 1 'patch 0010' mk --label t7 --out "$tmp/out7"

# 9. argument errors
check bad-option 2 'unknown option' mk --bogus
check label-chars 2 'label has characters' mk --label 'a b'
check label-value 2 'needs a value' mk --label

echo "source_release_test: $pass passed, $fail failed"
[ "$fail" -eq 0 ]
