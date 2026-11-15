#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - the volmark tests in Environment A (T-M3-12: U-VMK-01..10, the crash table of
# ARCHITECTURE.md section 3.6 with the fault layer of patch 0004, the vmk-split image for U15,
# ASan + UBSan).
#
# Usage: tests/unit/volmark/run.sh [--out DIR] [--no-asan] [--with-image]
#   --out DIR     result directory on the host (default tests/results/evidence/m312/<UTC stamp>)
#   --with-image  also copy vmk-split.img (64 MiB sparse content, git-ignored) to DIR/vmk/
#
# Runs through vm/linux/run.sh (container with the pinned NTFS-3G, repo mounted read-only,
# nothing on the host): builds libntfs-3g statically (release, --enable-faultio, and both with
# AddressSanitizer), makes the fixture images in the container, builds volmark and runs ctest
# -R '^volmark\.', then the same under ASan + UBSan. Images live in the container's /tmp.
# Logs, JUnit files, summary.txt, volmark-evidence.txt and vmk-split-record3.txt are copied to
# DIR. Exit: 0 all stages passed, 1 a stage failed, 2 setup failure.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
stamp=$(date -u +%Y%m%dT%H%M%SZ)
out=
args="--tar"
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "run.sh: --out needs a value" >&2; exit 2; }; out=$2; shift 2 ;;
	--no-asan | --with-image) args="$args $1"; shift ;;
	-h | --help) sed -n '4,17p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "run.sh: unknown option: $1" >&2; exit 2 ;;
	esac
done
[ -n "$out" ] || out=$repo/tests/results/evidence/m312/$stamp
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/volmark-tests.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

rc=0
# shellcheck disable=SC2086
sh "$repo/vm/linux/run.sh" run -- sh /work/tests/unit/volmark/inner.sh $args >"$tmp/out.tgz" 2>"$out/run.log" || rc=$?
if [ -s "$tmp/out.tgz" ]; then tar -xzf "$tmp/out.tgz" -C "$out"; fi
echo "run.sh: results in $out (exit $rc)" >&2
exit "$rc"
