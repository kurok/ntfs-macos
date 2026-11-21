#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - leak detection run on the NTFS-3G worker (T-M5-03, criterion 5.4, S-LEAK-01). Environment B.
#
# Usage: tests/stress/leaks/run.sh --out DIR [--fixtures DIR] [--iterations N] [--deps-tar FILE]
#                                  [--project-image NAME]... [--keep]
#   --out DIR         result directory: result.json (ntfsmac-test-result/1: S-LEAK-01, S-LEAK-01-CONTROL),
#                     summary.txt, classification.json, the raw `leaks` output of every scan, the
#                     control scan, the workload log, the patched-line map
#   --fixtures DIR    directory with clean-64m-4k.img (default: generated in Environment A with
#                     tests/fixtures/gen/clean.sh --only clean-64m-4k --export, needs Docker)
#   --iterations N    workload iterations (default 3, about 20 s of guest time; a long run is 50)
#   --deps-tar FILE   tar of the guest's ~/ntfsmac/deps AND ~/ntfsmac/build (the object files: `leaks` finds
#                     source lines through them), used instead of building NTFS-3G when the file exists;
#                     when it does not, the build is made and saved there. Not the tar of dmn_vm.sh.
#   --project-image NAME  test of the FAIL path: every frame of the image NAME counts as project code
#   --keep            keep the guest (delete it yourself with vm/macos/tart.sh delete NAME)
# One guest is cloned from ntfsmac-golden and deleted at the end. This script takes no VM slot: the
# caller holds it (scripts/test-all.sh and vm/macos/run-suite.sh do; a standalone run takes none: with two
# other guests running, wait or take a slot-N directory yourself). Nothing is mounted on the host.
#
# What it checks (docs/TEST_STRATEGY.md S-LEAK-01): the worker, the guest-built NTFS-3G with the
# patch series, serves a fixture mounted with the daemon's argument vector; after the functional
# workload `leaks` scans it; every leaked root is classified by its allocation stack (classify.py):
# project (a patched line or function of third_party/patches, or src/) fails the run, engine
# (upstream NTFS-3G, libfuse) is listed in the result. A control program with known leaks must
# be found by the same scan, and must turn into a FAIL under a project rule.
# Exit: 0 pass, 1 a project leak or a failed control or workload, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
py=${PYTHON:-python3}
out=
fx=
iter=3
deps_tar=
keep=0
proj=
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "leaks: --out needs a directory" >&2; exit 2; }; out=$2; shift 2 ;;
	--fixtures) [ $# -ge 2 ] || { echo "leaks: --fixtures needs a directory" >&2; exit 2; }; fx=$2; shift 2 ;;
	--iterations) [ $# -ge 2 ] || { echo "leaks: --iterations needs a number" >&2; exit 2; }; iter=$2; shift 2 ;;
	--deps-tar) [ $# -ge 2 ] || { echo "leaks: --deps-tar needs a file" >&2; exit 2; }; deps_tar=$2; shift 2 ;;
	--project-image) [ $# -ge 2 ] || { echo "leaks: --project-image needs a name" >&2; exit 2; }; proj="$proj --project-image $2"; shift 2 ;;
	--keep) keep=1; shift ;;
	-h | --help) sed -n '4,30p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "leaks: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$out" ] || { echo "leaks: --out is required" >&2; exit 2; }
case $iter in '' | *[!0-9]* | 0) echo "leaks: --iterations must be a positive number" >&2; exit 2 ;; esac
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
case $deps_tar in '' | /*) ;; *) deps_tar=$(pwd)/$deps_tar ;; esac
mkdir -p "$out"
# the commit and tree state at the START of the run (a result is tied to it, not to the state at the end)
"$py" -B "$root/tests/harness/report.py" provenance --out "$out/provenance.txt" >/dev/null || { echo "leaks: cannot record the start state (is this a repository checkout?)" >&2; exit 2; }
# the keys of the inputs (deps, fixtures) are read once, now, and appended to the start record; the record at the end uses these
sh "$root/scripts/run-inputs.sh" snapshot "$out/provenance.txt" --fixtures >&2 ||
	{ echo "leaks: the inputs keys are unavailable" >&2; exit 2; }

# This suite rebuilds after seeding (see below), so a stale tar is harmless; the tar is only recorded. The fixtures
# are checked against the generators of this checkout (scripts/run-inputs.sh, audit M5-F07).
tmp=$(mktemp -d "${TMPDIR:-/tmp}/leaks.XXXXXX")
name=ntfsmac-leaks-$$
started=$($py -B -c 'import time; print(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))')
vm_up=0
cleanup() {
	if [ "$vm_up" -eq 1 ] && [ "$keep" -eq 0 ]; then
		"$tart_sh" delete "$name" >&2 || echo "leaks: WARNING: could not delete $name" >&2
	elif [ "$vm_up" -eq 1 ]; then
		echo "leaks: guest $name kept (--keep): delete it with vm/macos/tart.sh delete $name" >&2
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# the patched-line map needs only the pinned tarball and the patches (host side, nothing is built)
tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "leaks: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] ||
	{ echo "leaks: $root/third_party/cache/$tarball is missing; copy it from the main checkout or run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }
# the option string the guest part copies must still be the one the daemon builds
opt='backend=fskit,local,norecover,noatime,windows_names,'
grep -q "$opt\"" "$root/src/options/options.c" && grep -q 'nocompression,no_detach,streams_interface=openxattr' "$root/src/options/options.c" &&
	grep -q "backend=fskit,local,norecover,noatime,windows_names,nocompression,no_detach,streams_interface=openxattr" "$here/leaks_guest.sh" ||
	{ echo "leaks: the option string of leaks_guest.sh no longer matches src/options/options.c" >&2; exit 2; }
"$py" -B "$here/classify.py" selftest >&2 || { echo "leaks: classifier selftest failed" >&2; exit 2; }
"$py" -B "$here/classify.py" patched-map --tarball "$root/third_party/cache/$tarball" \
	--patches "$root/third_party/patches/ntfs-3g" --out "$out/patched-map.json" >"$out/patched-map.txt" ||
	{ echo "leaks: could not build the patched-line map" >&2; exit 2; }

if [ -z "$fx" ]; then
	fx=$tmp/fixtures
	echo "leaks: generating clean-64m-4k in Environment A" >&2
	sh "$root/tests/fixtures/gen/clean.sh" --only clean-64m-4k --export "$fx" >&2 || { echo "leaks: fixture generation failed" >&2; exit 2; }
fi
[ -f "$fx/clean-64m-4k.img" ] || { echo "leaks: $fx/clean-64m-4k.img is missing" >&2; exit 2; }

sh "$root/scripts/run-inputs.sh" record "$out/inputs.txt" --snapshot "$out/provenance.txt" --deps-tar-seed "$deps_tar" --fixtures "$fx" --names clean-64m-4k >&2 ||
	{ echo "leaks: the inputs check failed" >&2; exit 2; }
vm_up=1
"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

# A saved tar only seeds the build; the build always runs with the current patch series and tarball
# (build-ntfs3g.sh keeps a stamp of both, so it does nothing when the tar is current and rebuilds when
# it is stale: a stale build would misalign the source lines of the patched-line map).
if [ -n "$deps_tar" ] && [ -f "$deps_tar" ]; then
	echo "leaks: seeding the guest build from $deps_tar" >&2
	tart exec -i "$name" /bin/sh -c 'mkdir -p ~/ntfsmac && tar -xf - -C ~/ntfsmac' <"$deps_tar"
	saved=1
else
	saved=0
fi
echo "leaks: build-ntfs3g.sh in the guest (a no-op when the seed is current)" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball") |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
	{ echo "leaks: guest build failed" >&2; exit 2; }
if [ -n "$deps_tar" ]; then
	tart exec "$name" /bin/sh -c 'cd ~/ntfsmac && tar -cf - deps build' >"$tmp/deps.tar" &&
		cp "$tmp/deps.tar" "$deps_tar" || echo "leaks: WARNING: could not save $deps_tar" >&2
fi

echo "leaks: copying the workload and the fixture" >&2
mkdir -p "$tmp/pkg/fixtures"
cp "$fx/clean-64m-4k.img" "$tmp/pkg/fixtures/"
(cd "$root" && tar -cf - tests/stress/leaks) | (cd "$tmp/pkg" && tar -xf -)
(cd "$tmp/pkg" && tar -cf - .) | tart exec -i "$name" /bin/sh -c 'mkdir -p ~/leakd && tar -xf - -C ~/leakd'

echo "leaks: running in the guest" >&2
grc=0
"$tart_sh" exec "$name" -- "LEAK_ITER=$iter sh ~/leakd/tests/stress/leaks/leaks_guest.sh ~/leakd ~/leakout 2>&1" >"$out/guest.log" || grc=$?
cat "$out/guest.log" >&2
# the start records (provenance.txt with the input keys, inputs.txt) are the host's: the guest output must not replace them
cp "$out/provenance.txt" "$tmp/provenance.keep"
cp "$out/inputs.txt" "$tmp/inputs.keep"
tart exec "$name" /bin/sh -c 'cd ~/leakout 2>/dev/null && tar -cf - .' | tar -xf - --exclude ./provenance.txt --exclude provenance.txt --exclude ./inputs.txt --exclude inputs.txt -C "$out" ||
	{ echo "leaks: could not fetch the evidence from the guest" >&2; exit 2; }
cmp -s "$out/provenance.txt" "$tmp/provenance.keep" || { echo "leaks: the start record provenance.txt was changed by the guest output" >&2; exit 2; }
cmp -s "$out/inputs.txt" "$tmp/inputs.keep" || { echo "leaks: the record inputs.txt was changed by the guest output" >&2; exit 2; }
[ "$grc" -eq 0 ] || { echo "leaks: the guest script failed (rc $grc), see $out/guest.log" >&2; exit 2; }
echo "host_os=macOS $(sw_vers -productVersion 2>/dev/null || echo host)" >>"$out/meta.txt"
grep -E '^(deps_tar|fixtures?[_.])' "$out/inputs.txt" >>"$out/meta.txt" || true

# shellcheck disable=SC2086
"$py" -B "$here/classify.py" finalize --dir "$out" --repo "$root" --started "$started" $proj
