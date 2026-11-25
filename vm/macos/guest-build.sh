#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The `guest-build` stage of Environment B (vm/macos/run-suite.sh), the first one: ONE disposable clone of the golden VM
# that builds NTFS-3G (for the cc lines that use libntfs-3g) and compiles, in that guest, every cc line of the VM
# scripts (tests/integration/daemon/{spawn,sandbox,errwatch,journal,dmn_mirror,xpc}_vm.sh), the lines that use
# libntfs-3g, macFUSE and the FUSE stand-in included. A missing header, a .c missing from a line or a link error fails here in
# the first minutes of a B run, not after the 30 minutes of the integration suite. The lists are those of
# tests/unit/ci/check_guest_sources.py (what ci.guest_sources checks against the includes, ci.guest_link builds on the
# host without the libraries); tests/unit/ci/guest_build_plan.py turns them into the guest script.
#
# Nothing is saved or shared: the NTFS-3G build lives and dies with this clone. The golden VMs are only cloned, never
# started or changed; nothing is mounted on the host, and nothing is mounted in the guest.
#
# Usage: vm/macos/guest-build.sh [--out-dir DIR]
#   --out-dir DIR    the guest script (build.sh) and the logs (guest-build.log, ntfs3g-build.log) are kept there
# This script takes no VM slot: the caller holds it (run-suite.sh does).
# Exit: 0 everything built, 1 a cc line failed, 2 setup error (the NTFS-3G build failed, no guest).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
tart_sh=$here/tart.sh
py=${PYTHON:-python3}
name=ntfsmac-gbuild-$$

die() {
	printf 'guest-build: %s\n' "$1" >&2
	exit 2
}

out_dir=
while [ $# -gt 0 ]; do
	case $1 in
	--out-dir) [ $# -ge 2 ] || die "--out-dir needs a directory"; out_dir=$2; shift ;;
	-h | --help) sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done
case $out_dir in '' | /*) ;; *) out_dir=$(pwd)/$out_dir ;; esac

tmp=$(mktemp -d "${TMPDIR:-/tmp}/guest-build.XXXXXX")
[ -n "$out_dir" ] || out_dir=$tmp/out
mkdir -p "$out_dir"
cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "guest-build: WARNING: could not delete $name" >&2
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || die "no ntfs-3g entry in third_party/deps.lock"
[ -f "$root/third_party/cache/$tarball" ] || die "$root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g"

# the plan is made first: a script the parser does not understand, or a copy list that names a missing file, fails before a guest exists
"$py" -I -B "$root/tests/unit/ci/guest_build_plan.py" "$root" "$tmp/plan" || die "the guest build plan could not be made"
cp "$tmp/plan/pack/build.sh" "$out_dir/build.sh"

# rt SECS WHAT CMD...: CMD under a deadline (the pattern of tart.sh and dmn_lib.sh: perl alarm); a timeout is a setup
# error (exit 2); any other failure is returned to the caller. Variables: rt_ prefix.
rt() {
	rt_s=$1
	rt_w=$2
	shift 2
	rt_rc=0
	perl -e 'alarm shift; exec @ARGV or exit 127' "$rt_s" "$@" || rt_rc=$?
	if [ "$rt_rc" -eq 142 ]; then die "$rt_w timed out after $rt_s s"; fi
	return "$rt_rc"
}

rt 300 "cloning the guest" "$tart_sh" clone "$name" --from golden >&2 || die "could not clone the golden VM"
# the guest's own boot wait is tart.sh's (SSH_WAIT); the bound here is above it
rt 400 "starting the guest" "$tart_sh" run "$name" >&2 || die "could not start the guest"

echo "guest-build: building NTFS-3G in the guest" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball") >"$tmp/ntfs3g-in.tar" || die "could not pack the NTFS-3G build inputs"
rt 120 "copying the NTFS-3G build inputs" tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree' <"$tmp/ntfs3g-in.tar" ||
	die "could not copy the NTFS-3G build inputs to the guest"
rt 600 "the NTFS-3G build in the guest" "$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && env -u CFLAGS -u LDFLAGS sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >"$out_dir/ntfs3g-build.log" ||
	{ tail -20 "$out_dir/ntfs3g-build.log" >&2; die "the NTFS-3G build in the guest failed"; }

echo "guest-build: copying the sources of the VM scripts" >&2
(cd "$tmp/plan/pack" && tar -cf - .) >"$tmp/pack.tar" || die "could not pack the sources of the VM scripts"
rt 120 "copying the sources" tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/gbuild && mkdir -p ~/ntfsmac/gbuild && tar -xf - -C ~/ntfsmac/gbuild' <"$tmp/pack.tar" ||
	die "could not copy the sources to the guest"
rc=0
rt 600 "the cc lines" "$tart_sh" exec "$name" -- 'sh ~/ntfsmac/gbuild/build.sh 2>&1' >"$out_dir/guest-build.log" || rc=$?
cat "$out_dir/guest-build.log" >&2
case $rc in
0) ;;
2) die "the guest build script could not start (see above)" ;;
*) exit 1 ;;
esac
