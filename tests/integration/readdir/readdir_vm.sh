#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# readdir_vm.sh - host side of I-RDIR-01..05 (tests/integration/readdir/readdir_cases.sh), Environment B.
# Clones ntfsmac-golden (vm/macos/tart.sh), builds NTFS-3G with the patch series in the guest (or
# unpacks --deps-tar, a tar of the guest's ~/ntfsmac/deps checked as dmn_vm.sh checks it), copies
# this directory in, builds dirtests, makes a fresh NTFS image FILE in the guest with mkntfs and runs
# readdir_cases.sh on it as the guest user. Every mount is inside the guest, with norecover. The clone
# is deleted on every exit path (unless --keep). Nothing touches a host disk; no sudo.
#
# Usage: tests/integration/readdir/readdir_vm.sh [--deps-tar FILE] [--sizes "1000 10000"] [--repeats N]
#                                                [--strict] [--timeout S] [--out FILE] [--keep]
#   --timeout S  deadline for the cases in the guest (default 5400 s); past it the run is a setup error
#   --strict  also fail on the concurrent-lister runs of I-RDIR-01 (a known macFUSE defect, ADR-027)
# Exit: 0 every check passed, 1 a check failed, 2 setup error.
set -u

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-rdir-$$
deps_tar= sizes="1000 10000" reps=3 out= keep=0 strict= tmo=5400
die2() { echo "readdir_vm: $*" >&2; exit 2; }
while [ $# -gt 0 ]; do
	case $1 in
	--deps-tar) [ $# -ge 2 ] || { echo "readdir_vm: --deps-tar needs a file" >&2; exit 2; }; deps_tar=$2; shift 2 ;;
	--sizes) [ $# -ge 2 ] || { echo "readdir_vm: --sizes needs a list" >&2; exit 2; }; sizes=$2; shift 2 ;;
	--repeats) [ $# -ge 2 ] || { echo "readdir_vm: --repeats needs a number" >&2; exit 2; }; reps=$2; shift 2 ;;
	--out) [ $# -ge 2 ] || { echo "readdir_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	--keep) keep=1; shift ;;
	--strict) strict=-s; shift ;;
	--timeout) [ $# -ge 2 ] || die2 "--timeout needs seconds"; tmo=$2; shift 2 ;;
	-h | --help) sed -n '2,15p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "readdir_vm: unknown option $1" >&2; exit 2 ;;
	esac
done
case $sizes in '' | *[!0-9\ ]*) echo "readdir_vm: --sizes takes numbers" >&2; exit 2 ;; esac
case $reps in '' | *[!0-9]* | 0) echo "readdir_vm: --repeats must be a positive number" >&2; exit 2 ;; esac
case $tmo in '' | *[!0-9]* | 0) die2 "--timeout must be a positive number of seconds" ;; esac
if [ -n "$deps_tar" ]; then
	[ -f "$deps_tar" ] || { echo "readdir_vm: $deps_tar is missing" >&2; exit 2; }
	sh "$root/scripts/run-inputs.sh" deps-check "$deps_tar" --key "$(sh "$root/scripts/run-inputs.sh" deps-key)" --layout dmn ||
		{ echo "readdir_vm: --deps-tar refused" >&2; exit 2; }
fi
tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$deps_tar" ] || [ -f "$root/third_party/cache/$tarball" ] ||
	{ echo "readdir_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

cleanup() {
	if [ "$keep" -eq 0 ]; then
		"$tart_sh" delete "$name" >&2 || echo "readdir_vm: WARNING: could not delete $name" >&2
	else
		echo "readdir_vm: guest $name kept (--keep): delete it with vm/macos/tart.sh delete $name" >&2
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# every setup step below exits 2 when it fails (a failed check is 1)
"$tart_sh" clone "$name" --from golden >&2 || die2 "clone failed"
"$tart_sh" run "$name" >&2 || die2 "the guest did not start"
if [ -n "$deps_tar" ]; then
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/deps && mkdir -p ~/ntfsmac && tar -xf - -C ~/ntfsmac' <"$deps_tar" ||
		die2 "could not unpack $deps_tar in the guest"
else
	echo "readdir_vm: building NTFS-3G in the guest" >&2
	(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock third_party/patches \
		"third_party/cache/$tarball") |
		tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree' ||
		die2 "could not copy the NTFS-3G sources into the guest"
	"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && env -u CFLAGS -u LDFLAGS sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build >~/ntfsmac/build.log 2>&1 || { tail -20 ~/ntfsmac/build.log; exit 1; }' >&2 ||
		{ echo "readdir_vm: guest build failed" >&2; exit 2; }
fi
(cd "$root" && tar -cf - tests/integration/readdir) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/rdir && mkdir -p ~/ntfsmac/rdir && tar -xf - -C ~/ntfsmac/rdir' ||
	die2 "could not copy the test into the guest"
"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/rdir/tests/integration/readdir && cc -O2 -Wall -Wno-deprecated-declarations -o dirtests dirtests.c' >&2 ||
	{ echo "readdir_vm: dirtests build failed" >&2; exit 2; }
"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/rdir && mkfile -n 2g rdir.img && ~/ntfsmac/deps/sbin/mkntfs -F -f -q -L rdir rdir.img >/dev/null 2>&1' >&2 ||
	die2 "could not make the NTFS image in the guest"
rc=0
# the cases under a host-side deadline (perl alarm, as tests/stress/soak/mix_host.sh does)
perl -e 'alarm shift; exec @ARGV or exit 127' "$tmo" sh "$tart_sh" exec "$name" -- "cd ~/ntfsmac/rdir && NTFS3G=\$HOME/ntfsmac/deps/bin/ntfs-3g sh tests/integration/readdir/readdir_cases.sh -m \$HOME/ntfsmac/rdir/mnt -n '$sizes' -r $reps $strict rdir.img 2>&1" >"${out:-/dev/stdout}" || rc=$?
[ -z "$out" ] || cat "$out"
case $rc in
0 | 1) exit $rc ;;
142) die2 "the cases did not finish within $tmo s" ;;
*) die2 "the guest command ended with status $rc" ;;
esac
