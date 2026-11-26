#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Local release pipeline (ADR-015: nothing is uploaded; the result stays in git-ignored dist/).
#
#   1. ci        scripts/ci-local.sh --quick on the host
#   2. build     packaging/build-pkg.sh in a disposable clone of the golden VM (macOS 27 +
#                macFUSE, which the engine links); the package is copied back to dist/<label>/
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

cd "$root"
git rev-parse --git-dir >/dev/null 2>&1 || die "not a git checkout" 2

# --- label ---
version=$(sed -n 's/^project(ntfs-macos VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || die "cannot read the project version from CMakeLists.txt" 2
rev=$(git rev-parse HEAD)
dirty=
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	[ "$allow_dirty" -eq 1 ] || die "uncommitted changes to tracked files; commit them or pass --allow-dirty" 2
	dirty=.dirty
fi
if [ "$(git tag --points-at HEAD | grep -cx "v$version")" -eq 1 ] && [ -z "$dirty" ]; then
	label=$version
else
	label=$version-dev.$(date -u +%Y%m%d).g$(git rev-parse --short=12 HEAD)$dirty
fi
out=$root/dist/$label
printf 'release-local: version %s, label %s, commit %s\n' "$version" "$label" "$rev"

# --- 1. ci ---
if [ "$run_ci" -eq 1 ]; then
	log "ci (ci-local.sh --quick)"
	sh "$here/ci-local.sh" --quick || die "ci-local --quick failed; no package built"
fi

rm -rf "$out"
mkdir -p "$out"

# --- 2. build ---
if [ "$on_host" -eq 1 ]; then
	log "build on host"
	NTFSMAC_SOURCE_REV=$rev$dirty sh "$root/packaging/build-pkg.sh" --label "$label" --out "$out"
	log "done: $out"
	exit 0
fi

command -v tart >/dev/null 2>&1 || die "tart not found (vm/macos/README.md)" 2
tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' third_party/deps.lock)
[ -n "$tarball" ] || die "no ntfs-3g entry in third_party/deps.lock" 2
[ -f "third_party/cache/$tarball" ] || sh scripts/fetch-deps.sh ntfs-3g >&2

build_vm=ntfsmac-release-build-$$
verify_vm=ntfsmac-release-verify-$$
cleanup() {
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
# Tracked files from the working tree (equal to HEAD unless --allow-dirty; with --allow-dirty also the
# files not yet tracked and not ignored, as tests/integration/packaging/pkg_vm.sh sends them) plus the
# verified tarball.
if [ "$allow_dirty" -eq 1 ]; then ls_opts='--cached --others --exclude-standard'; else ls_opts=--cached; fi
# shellcheck disable=SC2086 # the options
{ git ls-files -z $ls_opts -- . ':!third_party/cache'; printf '%s\0' "third_party/cache/$tarball"; } |
	tar --null -T - -cf - |
	tart exec -i "$build_vm" /bin/sh -c 'rm -rf ~/ntfsmac && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
gexec "$build_vm" "cd ~/ntfsmac/tree && NTFSMAC_SOURCE_REV='$rev$dirty' sh packaging/build-pkg.sh --label '$label' --out ~/ntfsmac/dist --work ~/ntfsmac/work 2>&1" ||
	die "package build in the VM failed"
gexec "$build_vm" 'cd ~/ntfsmac/dist && tar -cf - .' | tar -xf - -C "$out"
(cd "$out" && shasum -a 256 -c SHA256SUMS) || die "copied package does not match its checksums"
[ "$keep_vm" -eq 1 ] || "$tart_sh" delete "$build_vm" >/dev/null 2>&1 || true

pkg=ntfs-macos-$label.pkg
if [ "$verify" -eq 0 ]; then
	log "done (not verified): $out/$pkg"
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
