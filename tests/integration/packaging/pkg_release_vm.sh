#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M6-01: I-PKG-03 (install of the release package) and I-PKG-04 (its uninstall) in a FRESH clone
# of the golden VM (Environment B). The package is built beforehand by scripts/release-local.sh
# (or packaging/build-pkg.sh in a guest); this script never builds. The fixture clean-64m-4k is
# attached to the guest as a virtual disk (a copy of the image FILE; the original is not changed),
# and the daemon that the package installs mounts it inside the guest with norecover. Nothing
# touches the host: no install, no mount, no disk. The guest checks are in
# tests/integration/packaging/pkg_release_vm_guest.sh. The VM is always deleted.
#
# This script starts one VM. Run it through the VM slot protocol (vm/macos/run-suite.sh) when other
# runs may use the VMs.
#
# Usage: tests/integration/packaging/pkg_release_vm.sh --pkg-dir DIR --fixtures DIR [--out FILE]
#   --pkg-dir   the output of the package build: ntfs-macos-<label>.pkg, uninstall.sh,
#               manifest.txt and SHA256SUMS (dist/<label>)
#   --fixtures  a directory with clean-64m-4k.img (tests/fixtures/gen/clean.sh --only clean-64m-4k
#               --export DIR), stamped by the generators of this checkout
#   --out       also write the guest output to FILE
# Exit: 0 all checks passed, 1 a check failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-pkgrel-$$
pkgdir=
fx=
out=

while [ $# -gt 0 ]; do
	case "$1" in
	--pkg-dir) [ $# -ge 2 ] || { echo "pkg_release_vm: --pkg-dir needs a directory" >&2; exit 2; }; pkgdir=$2; shift 2 ;;
	--fixtures) [ $# -ge 2 ] || { echo "pkg_release_vm: --fixtures needs a directory" >&2; exit 2; }; fx=$2; shift 2 ;;
	--out) [ $# -ge 2 ] || { echo "pkg_release_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	*) echo "pkg_release_vm: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$pkgdir" ] && [ -n "$fx" ] || { echo "pkg_release_vm: --pkg-dir and --fixtures are required" >&2; exit 2; }
set -- "$pkgdir"/ntfs-macos-*.pkg
[ $# -eq 1 ] && [ -f "$1" ] || { echo "pkg_release_vm: $pkgdir must hold exactly one ntfs-macos-*.pkg" >&2; exit 2; }
pkg=$1
(cd "$pkgdir" && shasum -a 256 -c SHA256SUMS >/dev/null) || { echo "pkg_release_vm: $pkgdir does not match its SHA256SUMS" >&2; exit 2; }
sh "$root/scripts/run-inputs.sh" fixtures-check "$fx" clean-64m-4k || { echo "pkg_release_vm: --fixtures refused" >&2; exit 2; }

tmp=$(mktemp -d "${TMPDIR:-/tmp}/pkg_release_vm.XXXXXX")
# shellcheck disable=SC2329 # called by the EXIT trap
cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "pkg_release_vm: WARNING: could not delete $name" >&2
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

cp "$fx/clean-64m-4k.img" "$tmp/clean-64m-4k.img"
mkdir -p "$tmp/pkg"
cp "$pkg" "$tmp/pkg/"

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" --disk "$tmp/clean-64m-4k.img" >&2

echo "pkg_release_vm: copying ${pkg##*/} and the reference files" >&2
(cd "$tmp" && tar -cf - pkg) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/rel && mkdir -p ~/ntfsmac/rel && tar -xf - -C ~/ntfsmac/rel'
(cd "$root" && tar -cf - packaging/manifest.txt packaging/uninstall.sh packaging/payload-check.sh integration/launchd \
	tests/integration/packaging/pkg_release_vm_guest.sh) |
	tart exec -i "$name" /bin/sh -c 'mkdir -p ~/ntfsmac/rel/tree && tar -xf - -C ~/ntfsmac/rel/tree'

rc=0
run='sh ~/ntfsmac/rel/tree/tests/integration/packaging/pkg_release_vm_guest.sh ~/ntfsmac/rel 2>&1'
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "$run" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "$run" || rc=$?
fi
exit "$rc"
