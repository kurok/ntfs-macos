#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-10: I-PKG-01 (developer install) and I-PKG-02 (uninstall) in a disposable macOS guest
# (Environment B). In the guest, packaging/dev-install.sh builds NTFS-3G and the product (preset
# `product`, no test seams), installs at the manifest paths, loads the launchd job and installs the
# authorization right; packaging/uninstall.sh then removes it again. The checks are in
# tests/integration/packaging/pkg_vm_guest.sh. Nothing touches the host: no mount, no disk, no
# install; the installer and the uninstaller run only in the clone, which is always deleted.
#
# Like tests/integration/daemon/attach_vm.sh: clone a tart VM from the golden VM, boot it, copy the
# source tree in, run the guest script as the guest user, copy the output back, delete the VM.
#
# Usage: tests/integration/packaging/pkg_vm.sh [--out FILE] [--deps-tar FILE]
#   --out        also write the output to FILE
#   --deps-tar   a tar of the guest's build/deps (the NTFS-3G prefix): used instead of building
#                NTFS-3G in the guest when FILE exists; when it does not, the guest builds it and
#                the result is saved there
# Exit: 0 all checks passed, 1 a check failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-pkg-$$
out=
deps_tar=

while [ $# -gt 0 ]; do
	case "$1" in
	--out) [ $# -ge 2 ] || { echo "pkg_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	--deps-tar) [ $# -ge 2 ] || { echo "pkg_vm: --deps-tar needs a file" >&2; exit 2; }; deps_tar=$2; shift 2 ;;
	*) echo "pkg_vm: unknown option $1" >&2; exit 2 ;;
	esac
done

cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "pkg_vm: WARNING: could not delete $name" >&2
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "pkg_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { [ -n "$deps_tar" ] && [ -f "$deps_tar" ]; } ||
	{ echo "pkg_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

# Pre-built inputs must come from this checkout (scripts/run-inputs.sh, audit M5-F07): refused here, before a guest is cloned.
# A missing, unreadable or different key refuses; there is no fallback to building and no override.
# The deps key is read once, now, before the tree is copied to the guest; a tar saved by this run is sealed with it.
# The layout is pkg (~/ntfsmac/pkg/build/deps): a tar made by dmn_vm.sh (layout dmn) is refused here and the other way round.
dkey=
if [ -n "$deps_tar" ]; then
	dkey=$(sh "$root/scripts/run-inputs.sh" deps-key) || { echo "pkg_vm: the deps key could not be computed" >&2; exit 2; }
	[ ! -f "$deps_tar" ] || sh "$root/scripts/run-inputs.sh" deps-check "$deps_tar" --key "$dkey" --layout pkg ||
		{ echo "pkg_vm: --deps-tar refused" >&2; exit 2; }
fi

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

echo "pkg_vm: copying the source tree" >&2
# The working tree as it is (tracked and not yet tracked files, never the ignored ones) plus the tarball.
{
	(cd "$root" && git ls-files -z --cached --others --exclude-standard -- . ':!third_party/cache')
	printf '%s\0' "third_party/cache/$tarball"
} | (cd "$root" && tar --null -T - -cf -) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/pkg && mkdir -p ~/ntfsmac/pkg && tar -xf - -C ~/ntfsmac/pkg'

if [ -n "$deps_tar" ] && [ -f "$deps_tar" ]; then
	echo "pkg_vm: NTFS-3G from $deps_tar (no build)" >&2
	tart exec -i "$name" /bin/sh -c 'mkdir -p ~/ntfsmac/pkg/build && tar -xf - -C ~/ntfsmac/pkg/build' <"$deps_tar"
fi

rc=0
run='env -u CFLAGS -u LDFLAGS sh ~/ntfsmac/pkg/tests/integration/packaging/pkg_vm_guest.sh ~/ntfsmac/pkg 2>&1'
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "$run" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "$run" || rc=$?
fi

if [ -n "$deps_tar" ] && [ ! -f "$deps_tar" ]; then
	# the key of this checkout goes into the tar, so that a later run can tell whether it is current (M5-F07)
	{ tart exec "$name" /bin/sh -c 'cd ~/ntfsmac/pkg/build && tar -cf - deps' >"$deps_tar" &&
		sh "$root/scripts/run-inputs.sh" deps-seal "$deps_tar" "$dkey" pkg; } ||
		{ rm -f "$deps_tar"; echo "pkg_vm: WARNING: could not save $deps_tar" >&2; }
fi
exit "$rc"
