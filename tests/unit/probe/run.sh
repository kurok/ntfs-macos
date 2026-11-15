#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - the probe tests in Environment A (T-M3-02: U-PROBE-*, U-PROBE-MIRROR-*,
# U-PROBE-NAMES-*, the probe's run time on 1,000,000 entries, ASan).
#
# Usage: tests/unit/probe/run.sh [--out DIR] [--big] [--no-asan] [--quick-fixtures]
#   --out DIR  result directory on the host (default tests/results/evidence/m302/<UTC stamp>)
#   --big      also make the two 1,000,000-entry volumes and record the probe's run time
#
# Runs through vm/linux/run.sh (container with the pinned NTFS-3G, repo mounted read-only,
# /dev/fuse and CAP_SYS_ADMIN, nothing on the host): builds libntfs-3g statically (plain and
# with AddressSanitizer), makes every fixture image in the container, builds the probe and
# runs ctest, then the same under ASan + UBSan. Images live in the container's /tmp.
# Logs, JUnit files and summary.txt are copied to DIR. Exit: 0 all stages passed, 1 a stage
# failed, 2 setup failure.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
stamp=$(date -u +%Y%m%dT%H%M%SZ)
out=
args="--tar"
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "run.sh: --out needs a value" >&2; exit 2; }; out=$2; shift 2 ;;
	--big | --no-asan | --quick-fixtures) args="$args $1"; shift ;;
	-h | --help) sed -n '4,20p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "run.sh: unknown option: $1" >&2; exit 2 ;;
	esac
done
[ -n "$out" ] || out=$repo/tests/results/evidence/m302/$stamp
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/probe-tests.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

rc=0
# shellcheck disable=SC2086
sh "$repo/vm/linux/run.sh" run -- sh /work/tests/unit/probe/inner.sh $args >"$tmp/out.tgz" 2>"$out/run.log" || rc=$?
if [ -s "$tmp/out.tgz" ]; then tar -xzf "$tmp/out.tgz" -C "$out"; fi
echo "run.sh: results in $out (exit $rc)" >&2
exit "$rc"
