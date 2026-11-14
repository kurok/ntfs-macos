#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Local CI runner (ADR-015, TEST_STRATEGY.md section 9). Nothing is uploaded.
# Only --quick is implemented (T-M1-07): bootstrap check, configure, build,
# ctest. Full mode and --verify-result belong to T-M5-08.
#
# Usage: scripts/ci-local.sh --quick [--preset NAME] [--build-dir DIR]
# Exit:  0 all stages passed, 1 a stage failed, 2 usage error or not implemented.
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH= cd -- "$here/.." && pwd)

die() { # message [exit-code]
	printf 'ci-local: %s\n' "$1" >&2
	exit "${2:-2}"
}

usage() {
	printf 'usage: %s --quick [--preset NAME] [--build-dir DIR]\n' "$0" >&2
}

quick=0
preset=release
build_dir=
full_wanted=1

while [ $# -gt 0 ]; do
	case "$1" in
	--quick) quick=1; full_wanted=0 ;;
	--verify-result)
		die "--verify-result is not implemented yet (T-M5-08)"
		;;
	--preset)
		[ $# -ge 2 ] || { usage; die "--preset needs a value"; }
		preset=$2; shift
		;;
	--build-dir)
		[ $# -ge 2 ] || { usage; die "--build-dir needs a value"; }
		build_dir=$2; shift
		;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" ;;
	esac
	shift
done

if [ "$quick" -eq 0 ] && [ "$full_wanted" -eq 1 ]; then
	die "full mode is not implemented yet (T-M5-08); use --quick"
fi

[ -n "$build_dir" ] || build_dir=$root/build/$preset

summary=
failed=0

# stage NAME CMD...: run CMD, record result and seconds, stop at first failure.
stage() {
	_name=$1; shift
	printf '\n== stage: %s ==\n' "$_name"
	_t0=$(date +%s)
	_rc=0
	"$@" || _rc=$?
	_t1=$(date +%s)
	if [ "$_rc" -eq 0 ]; then _res=ok; else _res=FAIL; failed=1; fi
	summary="${summary}$(printf '%-12s %-5s %4ss' "$_name" "$_res" "$((_t1 - _t0))")
"
	[ "$_rc" -eq 0 ] || finish
}

finish() {
	printf '\n== summary ==\n%s' "$summary"
	if [ "$failed" -ne 0 ]; then
		printf 'ci-local --quick: FAILED\n'
		exit 1
	fi
	printf 'ci-local --quick: PASSED\n'
	exit 0
}

cd "$root"
stage bootstrap "$here/bootstrap.sh"
stage configure cmake --preset "$preset" -B "$build_dir"
stage build cmake --build "$build_dir"
stage ctest ctest --test-dir "$build_dir" --output-on-failure
finish
