#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - crash-consistency sweep on Linux (T-M3-14, criterion 3.9, S-CRASH-*).
#
# Usage: tests/stress/crashsweep/run.sh [--out DIR] [crashsweep.py options]
#   --out DIR     result directory on the host (default: tests/results/evidence/m314/<UTC stamp>)
#   --mode short  50 cut points per workload, 5 workloads (the CI stage linux-crash-sweep-short)
#   --mode full   110 cut points per workload, 550 in all (default: short)
#   --plan short  torn write short:N:512 instead of cut:N; --cuts N, --workloads A,B, --jobs N
#
# Runs in Environment A through vm/linux/run.sh (container, internal FUSE, /dev/fuse and
# CAP_SYS_ADMIN, nothing on the host): builds the release NTFS-3G and the --enable-faultio NTFS-3G
# (patch 0004), proves the release build has no trace of the layer (nm, object comparison), then
# sweeps. Images live in the container /tmp; the image of a failing point is gzipped and copied
# to build/crashsweep-failing/<stamp>/ (git-ignored), not into the evidence directory.
# Result directory: results.tsv, results.jsonl, summary.json, build-proof.txt, sweep.log.
# Exit: 0 every cut point passed, 1 a cut point or control failed, 2 setup failure.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
stamp=$(date -u +%Y%m%dT%H%M%SZ)
out=
args=
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "run.sh: --out needs a value" >&2; exit 2; }; out=$2; shift 2 ;;
	-h | --help) sed -n '4,18p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) args="$args $1"; shift ;;
	esac
done
[ -n "$out" ] || out=$repo/tests/results/evidence/m314/$stamp
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"
tmp=$(mktemp -d "${TMPDIR:-/tmp}/crashsweep.XXXXXX")
trap 'rm -rf "$tmp"' EXIT

rc=0
# shellcheck disable=SC2086
sh "$repo/vm/linux/run.sh" run -- sh /work/tests/stress/crashsweep/inner.sh $args \
	>"$tmp/out.tgz" 2>"$out/sweep.log" || rc=$?
if [ -s "$tmp/out.tgz" ]; then
	mkdir "$tmp/x"
	tar -xzf "$tmp/out.tgz" -C "$tmp/x"
	if [ -d "$tmp/x/failing-images" ]; then
		fail=$repo/build/crashsweep-failing/$stamp
		mkdir -p "$fail"
		mv "$tmp/x/failing-images"/* "$fail"/
		rmdir "$tmp/x/failing-images"
		echo "run.sh: failing images and event logs: $fail" >&2
	fi
	cp -R "$tmp/x"/. "$out"/
fi
echo "run.sh: results in $out (exit $rc)" >&2
exit "$rc"
