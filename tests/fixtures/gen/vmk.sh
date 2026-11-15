#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fixture generator (T-M3-12): the `vmk-split` image of docs/TEST_STRATEGY.md section 3.2, for U15
# and H-USB-10: `clean-64m-4k` after `ntfsmac-volmark set` cut between the `$MFT` and `$MFTMirr`
# writes of record 3; and `vmk-split-clear` (U-PROBE-MIRROR-01), the reverse direction: after a
# complete `set`, `ntfsmac-volmark clear` cut between the same two writes.
#
# Usage: tests/fixtures/gen/vmk.sh [--export DIR]
#
# Run on the host: the script re-runs itself inside Environment A (vm/linux/run.sh). Images are
# made ONLY inside the container, in its /tmp; nothing is attached or mounted on the host. Use
# --export DIR to copy both images and their record-3 reports to the host (gzip stream, extracted
# under DIR, which should be below the git-ignored build/). Images are never committed.
#
# How: scripts/build-ntfs3g.sh builds the --enable-faultio libntfs-3g (patch 0004, test only) into
# its own prefix, cmake builds volmark_test against it, tests/fixtures/gen/clean.sh makes
# clean-64m-4k, and tests/fixtures/gen/vmk_make.py runs `set` with plan cut:1 and
# NTFSMAC_FAULT_KILL=1, checks the result (only the two copies of record 3 changed, `$MFT` one step
# ahead, both valid, the engine refuses it) and writes the exact record-3 difference to
# vmk-split-record3.txt (and the same for vmk-split-clear, with `clear` after a full `set`).
# tests/unit/volmark (volmark.run) makes and checks the same vmk-split image.
# Env: NTFSMAC_FIXTURE_OUT (container output dir, default /tmp/ntfsmac-vmk).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")

die() { printf 'vmk.sh: error: %s\n' "$*" >&2; exit 1; }

export_dir=
inner=0
tar_out=0
while [ $# -gt 0 ]; do
	case $1 in
	--export) [ $# -ge 2 ] || die "--export needs a dir"; export_dir=$2; shift ;;
	--inner) inner=1 ;;
	--tar) tar_out=1 ;;
	-h | --help) sed -n '4,24p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

if [ "$inner" -eq 0 ]; then
	if [ -n "$export_dir" ]; then
		mkdir -p "$export_dir"
		sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/vmk.sh --inner --tar |
			tar -xzf - -C "$export_dir"
		exit 0
	fi
	exec sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/vmk.sh --inner
fi

# ---- inside the container from here on ----
work=/work
out=${NTFSMAC_FIXTURE_OUT:-/tmp/ntfsmac-vmk}
for t in cmake ninja python3 ntfsinfo mkntfs; do
	command -v "$t" >/dev/null 2>&1 || die "$t not found: run through Environment A"
done
mkdir -p "$out"
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi
jobs=$(nproc 2>/dev/null || echo 2)

sh "$work/scripts/build-ntfs3g.sh" --fuse internal --prefix /tmp/deps-fio --build-dir /tmp/b-ntfs-fio \
	--jobs "$jobs" --configure-arg --enable-faultio >"$out/build-faultio.log" 2>&1 ||
	{ tail -20 "$out/build-faultio.log"; die "fault-layer libntfs-3g build failed"; }
{ PKG_CONFIG_PATH=/tmp/deps-fio/lib/pkgconfig cmake -S "$work" -B /tmp/bld-fio -G Ninja -DCMAKE_BUILD_TYPE=Release &&
	cmake --build /tmp/bld-fio --target volmark_test; } >"$out/build-volmark.log" 2>&1 ||
	{ tail -20 "$out/build-volmark.log"; die "volmark_test build failed"; }
clean=/tmp/ntfsmac-fixtures/clean-64m-4k.img
if [ ! -f "$clean" ]; then
	sh "$work/tests/fixtures/gen/clean.sh" --inner --only clean-64m-4k >"$out/clean.log" 2>&1 ||
		{ tail -20 "$out/clean.log"; die "clean-64m-4k failed"; }
fi
rc=0
python3 -I "$work/tests/fixtures/gen/vmk_make.py" --volmark-fault /tmp/bld-fio/src/volmark/volmark_test \
	--clean "$clean" --out "$out" || rc=$?
if [ "$tar_out" -eq 1 ]; then
	tar -czf - -C "$out" vmk-split.img vmk-split-record3.txt vmk-split-clear.img vmk-split-clear-record3.txt >&3
fi
exit "$rc"
