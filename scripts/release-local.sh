#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Local release pipeline (ADR-015: nothing is uploaded; the result stays in git-ignored dist/).
#
#   1. ci        scripts/ci-local.sh --quick on the host
#   2. build     packaging/build-pkg.sh in a disposable clone of the golden VM (macOS 27 +
#                macFUSE, which the engine links); the package is copied back to dist/<label>/
#      source   scripts/make-source-release.sh on the host, after the build: ntfs-macos-<label>-source.tar.gz,
#                the GPL source of exactly the tree that was built: one tree id, made before the build (the
#                commit's tree, or with --allow-dirty and on the host the working-tree file set of scripts/lib/tree.sh),
#                sent to the VM or extracted for the host build, and archived with --tree. With the pinned
#                NTFS-3G tarball and the patch series, next to the package in dist/<label>/ with its line
#                in SHA256SUMS (T-M6-05). It is made for the commit read at the start (--rev); it fails when
#                HEAD moved, and then the package is removed: no package is left without its source
#   3. verify    a second, fresh clone installs the package with installer(8) (preflight and
#                postinstall run), checks that the daemon serves its socket, runs every installed
#                program once, runs uninstall.sh and checks that nothing is left (the full checks
#                are I-PKG-03..05, tests/integration/packaging/pkg_release_vm.sh and
#                pkg_preflight_vm.sh)
#
# Both VMs are always deleted, also on failure or interrupt (unless --keep-vm).
# --host builds on this Mac instead of a VM; it needs macFUSE installed here and skips verify.
#
# The label is <version> when HEAD carries the tag v<version>, else
# <version>-dev.<UTC date>.g<12-hex commit>, plus ".dirty" for uncommitted changes
# (only with --allow-dirty; tracked files are packaged as they are in the working tree).
#
# Usage: scripts/release-local.sh [--allow-dirty] [--no-ci] [--no-verify] [--host] [--keep-vm]
# Exit:  0 package built (and verified), 1 a stage failed, 2 usage or setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
tart_sh=$root/vm/macos/tart.sh
# shellcheck source=lib/label.sh
. "$here/lib/label.sh"
# shellcheck source=lib/tree.sh
. "$here/lib/tree.sh"

die() { printf 'release-local: %s\n' "$1" >&2; exit "${2:-1}"; }
log() { printf '\n== release-local: %s ==\n' "$*"; }
usage() { printf 'usage: %s [--allow-dirty] [--no-ci] [--no-verify] [--host] [--keep-vm]\n' "$0" >&2; }

allow_dirty=0 run_ci=1 verify=1 on_host=0 keep_vm=0
while [ $# -gt 0 ]; do
	case "$1" in
	--allow-dirty) allow_dirty=1 ;;
	--no-ci) run_ci=0 ;;
	--no-verify) verify=0 ;;
	--host) on_host=1; verify=0 ;;
	--keep-vm) keep_vm=1 ;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" 2 ;;
	esac
	shift
done

srcrel() { # the GPL source archive of the tree that was built (TREE, made before the build), next to the package
	# fetch-deps.sh may download the pinned tarball (checked against deps.lock); make-source-release.sh never does
	[ -f "third_party/cache/$tarball" ] || sh scripts/fetch-deps.sh ntfs-3g >&2
	sh "$here/make-source-release.sh" --label "$label" --out "$out" --rev "$rev" --tree "$tree" || {
		# no package without its source
		rm -f "${out:?}/ntfs-macos-$label.pkg"
		printf 'source release archive failed; the package was removed from %s\n' "$out" >&2
		die "source release archive failed"
	}
}

# git_tree_tar: the tree and the verified tarball as one tar stream (what the build gets)
git_tree_tar() {
	git -c tar.umask=022 -c core.autocrlf=false -c core.eol=lf archive --format=tar \
		--prefix=third_party/cache/ --add-file="$root/third_party/cache/$tarball" --prefix= "$tree"
}

cd "$root"
git rev-parse --git-dir >/dev/null 2>&1 || die "not a git checkout" 2

# --- label (scripts/lib/label.sh, shared with make-source-release.sh) ---
version=$(ntfsmac_project_version)
[ -n "$version" ] || die "cannot read the project version from CMakeLists.txt" 2
rev=$(git rev-parse HEAD)
dirty=
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	[ "$allow_dirty" -eq 1 ] || die "uncommitted changes to tracked files; commit them or pass --allow-dirty" 2
	dirty=.dirty
fi

# --- the tree that is built and archived: made once, here, before anything is built ---
# Clean: the tree of the commit. With --allow-dirty and for the host build: the file set of
# `git ls-files --cached --others --exclude-standard` with the working-tree contents (scripts/lib/tree.sh).
# The build gets this tree, and make-source-release.sh archives this tree, so an edit made while the
# build runs changes neither.
rl_tmp=$(mktemp -d "${TMPDIR:-/tmp}/release-local.XXXXXX") || die "mktemp failed" 2
trap 'rm -rf "${rl_tmp:?}"' EXIT
trap 'exit 130' INT TERM HUP
if [ "$allow_dirty" -eq 1 ] || [ "$on_host" -eq 1 ]; then
	tree=$(ntfsmac_worktree_tree 1 "$rl_tmp") || die "cannot make the tree of the working-tree files (an untracked nested repository? remove or ignore it)" 2
else
	tree=$(git rev-parse "$rev^{tree}")
fi
[ "$tree" = "$(git rev-parse "$rev^{tree}")" ] || dirty=.dirty
label=$(ntfsmac_make_label "$version" "$dirty")
out=$root/dist/$label
printf 'release-local: version %s, label %s, commit %s, tree %s\n' "$version" "$label" "$rev" "$tree"

# --- 1. ci ---
if [ "$run_ci" -eq 1 ]; then
	log "ci (ci-local.sh --quick)"
	sh "$here/ci-local.sh" --quick || die "ci-local --quick failed; no package built"
fi

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' third_party/deps.lock)
[ -n "$tarball" ] || die "no ntfs-3g entry in third_party/deps.lock" 2

rm -rf "$out"
mkdir -p "$out"

# --- 2. build ---
if [ "$on_host" -eq 1 ]; then
	log "build on host, from a checkout of tree $tree"
	# Not from the live working tree: a fresh extraction of the tree (so no cached CMake or deps state,
	# and a later edit changes nothing), the same file set the archive gets.
	[ -f "third_party/cache/$tarball" ] || sh scripts/fetch-deps.sh ntfs-3g >&2
	mkdir "$rl_tmp/src"
	git_tree_tar >"$rl_tmp/tree.tar" || die "git archive of the tree failed"
	tar -xf "$rl_tmp/tree.tar" -C "$rl_tmp/src"
	[ -f "$rl_tmp/src/packaging/build-pkg.sh" ] || die "the extracted tree has no packaging/build-pkg.sh"
	NTFSMAC_SOURCE_REV=$rev$dirty sh "$rl_tmp/src/packaging/build-pkg.sh" --label "$label" --out "$out" --work "$rl_tmp/src/build/pkg"
	log "source release archive"
	srcrel
	log "done: $out"
	exit 0
fi

command -v tart >/dev/null 2>&1 || die "tart not found (vm/macos/README.md)" 2
[ -f "third_party/cache/$tarball" ] || sh scripts/fetch-deps.sh ntfs-3g >&2

build_vm=ntfsmac-release-build-$$
verify_vm=ntfsmac-release-verify-$$
cleanup() {
	rm -rf "${rl_tmp:?}"
	[ "$keep_vm" -eq 1 ] && { echo "release-local: kept $build_vm $verify_vm (delete with $tart_sh delete NAME)" >&2; return; }
	for vm in "$build_vm" "$verify_vm"; do
		tart list --quiet 2>/dev/null | grep -qx "$vm" || continue
		"$tart_sh" delete "$vm" >/dev/null 2>&1 || echo "release-local: WARNING: could not delete $vm" >&2
	done
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

gexec() { # vm command-string: run in the guest with Homebrew on PATH
	tart exec "$1" /bin/sh -c "export PATH=/opt/homebrew/bin:/usr/local/bin:\$PATH; $2"
}

log "build in VM $build_vm"
"$tart_sh" clone "$build_vm" --from golden >&2
"$tart_sh" run "$build_vm" >&2
# The tree made above plus the verified tarball (equal to HEAD unless --allow-dirty; with --allow-dirty
# also the files not yet tracked and not ignored, as tests/integration/packaging/pkg_vm.sh sends them).
git_tree_tar >"$rl_tmp/tree.tar" || die "git archive of the tree failed"
tart exec -i "$build_vm" /bin/sh -c 'rm -rf ~/ntfsmac && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree' <"$rl_tmp/tree.tar"
gexec "$build_vm" 'test -f ~/ntfsmac/tree/packaging/build-pkg.sh' || die "the tree in the VM has no packaging/build-pkg.sh"
gexec "$build_vm" "cd ~/ntfsmac/tree && NTFSMAC_SOURCE_REV='$rev$dirty' sh packaging/build-pkg.sh --label '$label' --out ~/ntfsmac/dist --work ~/ntfsmac/work 2>&1" ||
	die "package build in the VM failed"
gexec "$build_vm" 'cd ~/ntfsmac/dist && tar -cf - .' | tar -xf - -C "$out"
(cd "$out" && shasum -a 256 -c SHA256SUMS) || die "copied package does not match its checksums"
[ "$keep_vm" -eq 1 ] || "$tart_sh" delete "$build_vm" >/dev/null 2>&1 || true

log "source release archive"
srcrel

pkg=ntfs-macos-$label.pkg
if [ "$verify" -eq 0 ]; then
	log "done (not verified): $out/$pkg and the source archive"
	exit 0
fi

# --- 3. verify ---
log "verify install in fresh VM $verify_vm"
"$tart_sh" clone "$verify_vm" --from golden >&2
"$tart_sh" run "$verify_vm" >&2
tar -cf - -C "$out" "$pkg" uninstall.sh manifest.txt |
	tart exec -i "$verify_vm" /bin/sh -c 'rm -rf ~/ntfsmac-verify && mkdir -p ~/ntfsmac-verify && tar -xf - -C ~/ntfsmac-verify'
# shellcheck disable=SC2016 # expanded in the guest
gexec "$verify_vm" '
set -eu
cd ~/ntfsmac-verify
d=/usr/local/libexec/ntfs-macos
sudo -n /usr/sbin/installer -pkg '"$pkg"' -target /
pkgutil --pkg-info org.ntfs-macos.pkg
fail=0
check() { # name args...: the binary must load and run (exit < 126; dyld or signature failures are >= 126)
	_n=$1; shift
	set +e; "$d/$_n" "$@" >/dev/null 2>&1; _rc=$?; set -e
	if [ "$_rc" -ge 126 ]; then echo "FAIL $_n (exit $_rc)"; fail=1; else echo "ok   $_n (exit $_rc)"; fi
}
if [ -S /var/run/ntfsmacd.sock ] && sudo -n launchctl print system/org.ntfs-macos.ntfsmacd >/dev/null 2>&1; then echo "ok   the daemon runs"; else echo "FAIL the daemon does not run"; fail=1; fi
set +e; "$d/ntfsmac" doctor; _rc=$?; set -e
if [ "$(readlink /usr/local/bin/ntfsmac 2>/dev/null)" = "$d/ntfsmac" ]; then echo "ok   the link /usr/local/bin/ntfsmac"; else echo "NOTE no link /usr/local/bin/ntfsmac (/usr/local/bin is not root-only here?)"; fi
if [ "$_rc" -eq 0 ]; then echo "ok   ntfsmac doctor"; else echo "FAIL ntfsmac doctor (exit $_rc)"; fail=1; fi
check ntfsmac --version
check ntfsmac-worker --help
check ntfsmac-probe --version
check ntfsmac-volmark
check mkntfs --version
check ntfsfix --version
check ntfsinfo --version
check ntfslabel --version
for f in "$d"/*; do codesign --verify "$f" 2>/dev/null || case $f in *.sh | */manifest.txt) ;; *) echo "FAIL signature $f"; fail=1 ;; esac; done
sudo -n sh ./uninstall.sh
if pkgutil --pkg-info org.ntfs-macos.pkg >/dev/null 2>&1; then echo "FAIL receipt still present"; fail=1; fi
if [ -e "$d" ] || [ -e /usr/local/bin/ntfsmac ] || [ -L /usr/local/bin/ntfsmac ] || [ -e /Library/LaunchDaemons/org.ntfs-macos.ntfsmacd.plist ] || [ -e "/Library/Application Support/ntfs-macos" ]; then echo "FAIL files left after uninstall"; fail=1; fi
if sudo -n launchctl print system/org.ntfs-macos.ntfsmacd >/dev/null 2>&1; then echo "FAIL the daemon is still loaded"; fail=1; fi
[ "$fail" -eq 0 ] && echo "verify: PASSED"
exit "$fail"
' || die "install verification failed"

log "done: $out/$pkg"
ls -l "$out"
