#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# inner.sh - one F-SANITIZE-* fuzz run inside Environment A (the container). Called by run.sh.
#
# Usage (inside the container, repo at /work, read-only):
#   sh /work/tests/fuzz/sanitize/inner.sh --target verdict|label|display --seconds N [--tar]
#
# Builds the libFuzzer target with clang (-fsanitize=fuzzer,address,undefined, cmake option
# NTFSMAC_FUZZ), seeds it from tests/fuzz/sanitize/corpus/<target> and the dictionary, runs it for N
# seconds of one process (N = 3600 is the 1 CPU-hour of the acceptance), then replays the final corpus
# through a gcc --coverage build of the code under test and runs gcov. Everything lands in
# /tmp/fuzz-out (summary.txt, libFuzzer log, stats, any crash artifact, gcov reports, the grown
# corpus); with --tar it is streamed as a gzip tar on the original stdout.
# Exit: 0 no finding, 1 a finding (crash, oracle abort, timeout, out of memory) or a build failure.
set -eu

work=/work
out=/tmp/fuzz-out
target=
seconds=3600
tar_out=0
while [ $# -gt 0 ]; do
	case $1 in
	--target) target=$2; shift ;;
	--seconds) seconds=$2; shift ;;
	--tar) tar_out=1 ;;
	*) echo "inner.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
case $target in
verdict) maxlen=70000 ;;
label) maxlen=1100 ;;
display) maxlen=1100 ;;
*) echo "inner.sh: --target verdict|label|display" >&2; exit 2 ;;
esac
mkdir -p "$out"
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

# The container image sets sanitizer log paths for the engine build; the fuzzers report on stderr.
unset ASAN_OPTIONS UBSAN_OPTIONS
export ASAN_OPTIONS=abort_on_error=0:detect_leaks=1:allocator_may_return_null=0
export UBSAN_OPTIONS=print_stacktrace=1

summary=$out/summary.txt
: >"$summary"
say() { printf '%s\n' "$*" | tee -a "$summary"; }

say "target: $target  seconds: $seconds  host: $(uname -m)  clang: $(clang --version | head -1)"
say "start: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

build=/tmp/fuzz-build
rm -rf "$build"
CC=clang cmake -S "$work" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNTFSMAC_FUZZ=ON \
	-DCMAKE_C_COMPILER=clang >"$out/cmake.log" 2>&1 || { cat "$out/cmake.log"; say "RESULT: build failed"; exit 1; }
cmake --build "$build" --target "fuzz_sanitize_$target" >"$out/build.log" 2>&1 ||
	{ cat "$out/build.log"; say "RESULT: build failed"; exit 1; }
bin=$build/tests/fuzz/sanitize/fuzz_sanitize_$target

corpus=/tmp/fuzz-corpus
rm -rf "$corpus"
mkdir -p "$corpus" "$out/artifacts"
cp "$work/tests/fuzz/sanitize/corpus/$target"/* "$corpus/"
seeds=$(ls "$corpus" | wc -l | tr -d ' ')
dict=
[ "$target" != verdict ] || dict="-dict=$work/tests/fuzz/sanitize/verdict.dict"
say "seeds: $seeds"

rc=0
# shellcheck disable=SC2086
"$bin" "$corpus" $dict -max_len=$maxlen -max_total_time="$seconds" -timeout=10 -rss_limit_mb=2048 \
	-use_value_profile=1 -print_final_stats=1 -artifact_prefix="$out/artifacts/" \
	-seed=20261115 >"$out/libfuzzer.log" 2>&1 || rc=$?
tail -40 "$out/libfuzzer.log" >"$out/libfuzzer.tail"
say "libFuzzer exit: $rc"
for k in stat::number_of_executed_units stat::average_exec_per_sec stat::new_units_added \
	stat::slowest_unit_time_sec stat::peak_rss_mb; do
	grep "^$k" "$out/libfuzzer.log" | tail -1 | tee -a "$summary" || true
done
last=$(grep -E "^#[0-9]+" "$out/libfuzzer.log" | tail -1 || true)
say "last progress line: $last"
artifacts=$(ls "$out/artifacts" | wc -l | tr -d ' ')
say "artifacts (crash, oom, timeout, slow unit): $artifacts"
if [ "$rc" -ne 0 ] || [ "$artifacts" -ne 0 ]; then
	say "RESULT: FINDING (see libfuzzer.tail and artifacts/)"
	tail -60 "$out/libfuzzer.log"
fi
units=$(ls "$corpus" | wc -l | tr -d ' ')
say "corpus after the run: $units inputs"

# Line coverage of src/sanitize/ from the grown corpus (gcc --coverage, replay driver).
cov=/tmp/fuzz-cov
rm -rf "$cov"
mkdir -p "$cov"
(
	cd "$cov"
	srcs="$work/src/common/label_cp.c $work/src/sanitize/label.c $work/src/sanitize/display.c $work/src/sanitize/verdict.c
	      $work/src/options/options.c $work/src/options/argv_check.c $work/src/probe/json.c"
	# shellcheck disable=SC2086
	gcc -std=c17 -O0 -g --coverage -I"$work/include" -I"$work/src/sanitize" -I"$work/src/probe" \
		-o replay "$work/tests/fuzz/sanitize/fuzz_main.c" "$work/tests/fuzz/sanitize/fuzz_$target.c" $srcs
	./replay "$corpus" >/dev/null
	gcov -b ./*.gcda >"$cov/gcov.txt" 2>&1 || true
	cp "$cov/gcov.txt" "$out/gcov.txt"
	cp ./*.gcov "$out/" 2>/dev/null || true
)
say "coverage of the code under test (gcov, replay of the corpus above):"
grep -A1 -E "^File '.*/(label|display|verdict|label_cp)\.c'" "$out/gcov.txt" | grep -v '^--' | tee -a "$summary" || true
say "end: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [ "$rc" -ne 0 ] || [ "$artifacts" -ne 0 ]; then
	say "RESULT: FINDING"
	res=1
else
	say "RESULT: no finding in $seconds s"
	res=0
fi
mkdir -p "$out/corpus"
cp "$corpus"/* "$out/corpus/" 2>/dev/null || true
if [ "$tar_out" -eq 1 ]; then
	tar -czf - -C "$out" . >&3
fi
exit "$res"
