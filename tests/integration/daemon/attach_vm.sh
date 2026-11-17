#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-08: manual attach of the fixtures `clean-64m-4k` (must mount rw) and `dirty-flag` (must
# mount ro) with the wired daemon, in a disposable macOS guest (Environment B), and the checks of
# the H1 mount-table watcher. The daemon runs as the launchd job of integration/launchd/ (the
# MachServices entry included), installed at the manifest paths, with the real Disk Arbitration,
# probe, volmark, sandbox and worker. Nothing touches a host disk: the two fixtures are copies of
# image FILES that tart attaches to the guest as virtual disks, and every mount is inside the guest
# (NTFS-3G with `norecover`, never `recover`, `remove_hiberfile` or `fsname`).
#
# Like tests/integration/daemon/journal_vm.sh: clone a tart VM from the golden VM, boot it, build
# NTFS-3G (the worker), the probe and volmark in the guest, copy the host-built daemon and CLI in,
# run tests/integration/daemon/attach_vm_guest.sh as the guest user, copy the output back, always
# delete the VM.
#
# Usage: tests/integration/daemon/attach_vm.sh [--build-dir DIR] [--fixtures DIR] [--out FILE]
#   --build-dir  a CMake build tree with src/daemon/ntfsmacd and src/cli/ntfsmac (default build/release;
#                it must be a build with the test seams, which the release preset has)
#   --fixtures   a directory with clean-64m-4k.img and dirty-flag.img
#                (tests/fixtures/gen/clean.sh --only clean-64m-4k --export DIR and
#                 tests/fixtures/gen/unsafe.sh --only dirty-flag --export DIR)
#   --out        also write the output to FILE
#   --deps-tar   a tar of the guest's ~/ntfsmac/deps: used instead of building NTFS-3G in the guest
#                when FILE exists; when it does not, the build is made and its result is saved there
# Exit: 0 all checks passed, 1 a check failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-m408-$$
build=$root/build/release
fx=
out=
deps_tar=

while [ $# -gt 0 ]; do
	case "$1" in
	--build-dir) [ $# -ge 2 ] || { echo "attach_vm: --build-dir needs a directory" >&2; exit 2; }; build=$2; shift 2 ;;
	--fixtures) [ $# -ge 2 ] || { echo "attach_vm: --fixtures needs a directory" >&2; exit 2; }; fx=$2; shift 2 ;;
	--deps-tar) [ $# -ge 2 ] || { echo "attach_vm: --deps-tar needs a file" >&2; exit 2; }; deps_tar=$2; shift 2 ;;
	--out) [ $# -ge 2 ] || { echo "attach_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	*) echo "attach_vm: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$fx" ] || { echo "attach_vm: --fixtures is required" >&2; exit 2; }
for f in "$fx/clean-64m-4k.img" "$fx/dirty-flag.img" "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac"; do
	[ -f "$f" ] || { echo "attach_vm: $f is missing" >&2; exit 2; }
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/attach_vm.XXXXXX")
cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "attach_vm: WARNING: could not delete $name" >&2
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "attach_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { [ -n "$deps_tar" ] && [ -f "$deps_tar" ]; } || { echo "attach_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

# The guest gets its own copies of the images: the originals stay as they were generated.
cp "$fx/clean-64m-4k.img" "$tmp/clean-64m-4k.img"
cp "$fx/dirty-flag.img" "$tmp/dirty-flag.img"

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" --disk "$tmp/clean-64m-4k.img" --disk "$tmp/dirty-flag.img" >&2

if [ -n "$deps_tar" ] && [ -f "$deps_tar" ]; then
	echo "attach_vm: NTFS-3G from $deps_tar (no build)" >&2
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/deps && mkdir -p ~/ntfsmac && tar -xf - -C ~/ntfsmac' <"$deps_tar"
else
	echo "attach_vm: building NTFS-3G in the guest" >&2
	(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
		third_party/patches "third_party/cache/$tarball") |
		tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
	"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
		{ echo "attach_vm: guest build failed" >&2; exit 2; }
	if [ -n "$deps_tar" ]; then
		tart exec "$name" /bin/sh -c 'cd ~/ntfsmac && tar -cf - deps' >"$deps_tar" ||
			{ rm -f "$deps_tar"; echo "attach_vm: WARNING: could not save $deps_tar" >&2; }
	fi
fi

echo "attach_vm: copying the sources and the host-built daemon" >&2
mkdir -p "$tmp/bin"
cp "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac" "$tmp/bin/"
(cd "$root" && tar -cf - include integration/sandbox integration/launchd src/probe src/volmark src/common \
	tests/integration/daemon/attach_vm_guest.sh) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/avm && mkdir -p ~/ntfsmac/avm && tar -xf - -C ~/ntfsmac/avm'
(cd "$tmp" && tar -cf - bin) | tart exec -i "$name" /bin/sh -c 'tar -xf - -C ~/ntfsmac/avm'

rc=0
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- 'sh ~/ntfsmac/avm/tests/integration/daemon/attach_vm_guest.sh ~/ntfsmac/avm 2>&1' >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- 'sh ~/ntfsmac/avm/tests/integration/daemon/attach_vm_guest.sh ~/ntfsmac/avm 2>&1' || rc=$?
fi
exit "$rc"
