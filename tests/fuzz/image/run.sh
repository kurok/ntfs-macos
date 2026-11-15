#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - F-PROBE and F-MOUNT-WALK (T-M3-16) in Environment A (libFuzzer needs clang with its runtime, which the
# host's Apple clang lacks).
#
# Usage: tests/fuzz/image/run.sh [--target probe|mountwalk|all] [--seconds N] [--skip-security] [--out DIR]
#   --target   one target (default all: the two run in parallel, one container each)
#   --seconds  CPU time of each target's single process (default 3600 = 1 CPU-hour)
#   --skip-security  mountwalk: leave out the security descriptor reads (known faults F1, F3)
#   --out DIR  result directory on the host (default build/fuzz/image/<UTC stamp>)
#
# Each target runs through vm/linux/run.sh (container with the pinned toolchain, repo mounted
# read-only, nothing on the host). Results (summary.txt, libFuzzer log, gcov reports, grown
# corpus, any crash artifact) are copied to DIR/<target>/. Exit: 0 no finding in any target, 1 a
# finding or a failed run, 2 usage or setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
stamp=$(date -u +%Y%m%dT%H%M%SZ)
targets="probe mountwalk"
seconds=3600
out=
extra=
while [ $# -gt 0 ]; do
	case $1 in
	--target)
		[ $# -ge 2 ] || { echo "run.sh: --target needs a value" >&2; exit 2; }
		case $2 in
		all) targets="probe mountwalk" ;;
		probe | mountwalk) targets=$2 ;;
		*) echo "run.sh: unknown target $2" >&2; exit 2 ;;
		esac
		shift
		;;
	--skip-security) extra="--skip-security" ;;
	--seconds) [ $# -ge 2 ] || { echo "run.sh: --seconds needs a value" >&2; exit 2; }; seconds=$2; shift ;;
	--out) [ $# -ge 2 ] || { echo "run.sh: --out needs a value" >&2; exit 2; }; out=$2; shift ;;
	-h | --help) sed -n '4,16p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "run.sh: unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done
[ -n "$out" ] || out=$repo/build/fuzz/image/$stamp
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"

pids=
for t in $targets; do
	mkdir -p "$out/$t"
	(
		rc=0
		sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fuzz/image/inner.sh --target "$t" \
			--seconds "$seconds" --tar $extra >"$out/$t.tgz" 2>"$out/$t.log" || rc=$?
		if [ -s "$out/$t.tgz" ]; then tar -xzf "$out/$t.tgz" -C "$out/$t" || true; fi
		echo "$rc" >"$out/$t.rc"
	) &
	pids="$pids $!"
done
# shellcheck disable=SC2086
wait $pids || true

res=0
for t in $targets; do
	rc=$(cat "$out/$t.rc" 2>/dev/null || echo 2)
	if [ "$rc" -ne 0 ]; then res=1; fi
	printf '%s: exit %s\n' "$t" "$rc"
	if [ -f "$out/$t/summary.txt" ]; then sed 's/^/  /' "$out/$t/summary.txt"; fi
done
echo "run.sh: results in $out (exit $res)" >&2
exit "$res"
