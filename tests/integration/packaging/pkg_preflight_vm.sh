#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M6-01: I-PKG-05, the preflight of the release package, in a FRESH clone of the golden VM
# (Environment B): the install is refused without macFUSE, with a macFUSE that is too old, with
# another NTFS driver, with files in the way and with a user-owned directory above an installed
# path; then it works as an upgrade over an older package of ours. The package is built beforehand
# (scripts/release-local.sh); this script never builds. Nothing touches the host. The guest checks
# are in tests/integration/packaging/pkg_preflight_vm_guest.sh. The VM is always deleted.
#
# This script starts one VM. Run it through the VM slot protocol (vm/macos/run-suite.sh) when other
# runs may use the VMs.
#
# With --case userlocal it runs I-PKG-06 instead, in its own fresh clone: the install with a
# user-owned /usr/local/bin and /usr/local/share (no link, the CLI by its full path).
# With --case oldrel it runs the earlier-layout case of I-PKG-06 (audit M6-F02, M6-F04) in its own
# fresh clone: the published pre-release package (--old-pkg, a local copy; never downloaded here,
# and refused unless its sha256 is the published one), then this package, and the uninstaller.
#
# Usage: tests/integration/packaging/pkg_preflight_vm.sh --pkg-dir DIR [--case preflight|userlocal|oldrel]
#                                                        [--old-pkg FILE] [--out FILE]
#   --pkg-dir   the output of the package build: ntfs-macos-<label>.pkg and SHA256SUMS (dist/<label>)
#   --case      preflight (default, I-PKG-05), userlocal (I-PKG-06) or oldrel (I-PKG-06, earlier layout)
#   --old-pkg   oldrel only: ntfs-macos-0.1.0-dev.20261116.g91358db1a2ed.pkg, the published pre-release
#   --out       also write the guest output to FILE
# Exit: 0 all checks passed, 1 a check failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-pkgpre-$$
pkgdir=
out=
case_=preflight
old_pkg=
# sha256 of the published pre-release asset (docs/releases/0.1.0-dev.20261116.md)
old_sha=f6e8bcda6858498efd8642f4bd9f3b65bb23b972aa580841d97618aa1ce47e68

while [ $# -gt 0 ]; do
	case "$1" in
	--pkg-dir) [ $# -ge 2 ] || { echo "pkg_preflight_vm: --pkg-dir needs a directory" >&2; exit 2; }; pkgdir=$2; shift 2 ;;
	--case) [ $# -ge 2 ] || { echo "pkg_preflight_vm: --case needs a value" >&2; exit 2; }; case_=$2; shift 2 ;;
	--old-pkg) [ $# -ge 2 ] || { echo "pkg_preflight_vm: --old-pkg needs a file" >&2; exit 2; }; old_pkg=$2; shift 2 ;;
	--out) [ $# -ge 2 ] || { echo "pkg_preflight_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	*) echo "pkg_preflight_vm: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$pkgdir" ] || { echo "pkg_preflight_vm: --pkg-dir is required" >&2; exit 2; }
case $case_ in preflight | userlocal | oldrel) ;; *) echo "pkg_preflight_vm: --case is preflight, userlocal or oldrel" >&2; exit 2 ;; esac
if [ "$case_" = oldrel ]; then
	[ -n "$old_pkg" ] && [ -f "$old_pkg" ] || { echo "pkg_preflight_vm: --case oldrel needs --old-pkg FILE" >&2; exit 2; }
	[ "$(shasum -a 256 "$old_pkg" | awk '{print $1}')" = "$old_sha" ] ||
		{ echo "pkg_preflight_vm: $old_pkg is not the published pre-release (sha256 differs from $old_sha)" >&2; exit 2; }
fi
set -- "$pkgdir"/ntfs-macos-*.pkg
[ $# -eq 1 ] && [ -f "$1" ] || { echo "pkg_preflight_vm: $pkgdir must hold exactly one ntfs-macos-*.pkg" >&2; exit 2; }
pkg=$1
(cd "$pkgdir" && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "pkg_preflight_vm: $pkgdir does not match its SHA256SUMS" >&2; exit 2; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/pkg_preflight_vm.XXXXXX")
# shellcheck disable=SC2329 # called by the EXIT trap
cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "pkg_preflight_vm: WARNING: could not delete $name" >&2
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

mkdir -p "$tmp/pkg" "$tmp/oldpkg"
cp "$pkg" "$tmp/pkg/"
[ -z "$old_pkg" ] || cp "$old_pkg" "$tmp/oldpkg/"

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

echo "pkg_preflight_vm: copying ${pkg##*/}" >&2
(cd "$tmp" && tar -cf - pkg oldpkg) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/rel && mkdir -p ~/ntfsmac/rel && tar -xf - -C ~/ntfsmac/rel'
(cd "$root" && tar -cf - packaging/manifest.txt packaging/payload-check.sh packaging/uninstall.sh tests/integration/packaging/pkg_preflight_vm_guest.sh) |
	tart exec -i "$name" /bin/sh -c 'mkdir -p ~/ntfsmac/rel/tree && tar -xf - -C ~/ntfsmac/rel/tree'

rc=0
run="sh ~/ntfsmac/rel/tree/tests/integration/packaging/pkg_preflight_vm_guest.sh ~/ntfsmac/rel $case_ 2>&1"
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "$run" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "$run" || rc=$?
fi
exit "$rc"
