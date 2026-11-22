#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# inner.sh - one F-PROBE or F-MOUNT-WALK fuzz run inside Environment A (the container).
# Called by run.sh.
#
# Usage (inside the container, repo at /work, read-only):
#   sh /work/tests/fuzz/image/inner.sh --target probe|mountwalk --seconds N [--tar] [--skip-security]
#
# Stages:
#   engine     libntfs-3g from the pinned tarball with the release patch series
#              (scripts/build-ntfs3g.sh), static, built by clang with
#              -fsanitize=fuzzer-no-link,address,undefined so that the library is instrumented too.
#              UBSan reports in the library are printed and do not stop the run (recoverable,
#              each location once); in project code any report aborts
#   build      cmake -DNTFSMAC_FUZZ=ON, target fuzz_<target>
#   fuzz       (leak detection off, see leaks) one libFuzzer process at a time for N seconds of CPU in all (N = 3600 is the
#              1 CPU-hour of the acceptance), seeded with tests/fuzz/image/corpus/<target>. A
#              crash, timeout or out-of-memory input is saved under artifacts/ and the run
#              continues from the grown corpus with a new seed until the time is used up, so one
#              known finding does not end the hour
#   leaks      every input of the grown corpus replayed with LeakSanitizer on, so that every distinct leak of
#              the corpus is listed. One libFuzzer process over the whole corpus stops at the first leak
#              ("a leak has been found in the initial corpus"), and a known leak on the mount path would hide
#              every later one. So: the inputs go through in chunks of 64 files per process; a chunk that
#              fails is replayed one input per process. All reports go to leaks.log (leaks.log.sym), the
#              summary gets "leak replay: inputs N, leaking M" and, only if every input was replayed within
#              the time bound, "replay complete"
#   coverage   the grown corpus replayed through a gcc --coverage build of the harness, the
#              project code and libntfs-3g, then gcov
# Everything lands in /tmp/fuzz-out (summary.txt, libFuzzer logs, artifacts, gcov reports, the
# grown corpus); with --tar it is streamed as a gzip tar on the original stdout.
# Exit: 0 no finding, 1 a finding (crash, oracle abort, timeout, out of memory) or a failed build.
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
	--skip-security) export NTFSMAC_FUZZ_SKIP_SECURITY=1 ;;
	*) echo "inner.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
case $target in
probe | mountwalk) ;;
*) echo "inner.sh: --target probe|mountwalk" >&2; exit 2 ;;
esac
mkdir -p "$out"
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

# The container image sets sanitizer log paths for the engine build; the fuzzers report on stderr.
unset ASAN_OPTIONS UBSAN_OPTIONS
export ASAN_OPTIONS=abort_on_error=0:detect_leaks=0:allocator_may_return_null=0
export UBSAN_OPTIONS=print_stacktrace=1

summary=$out/summary.txt
: >"$summary"
say() { printf '%s\n' "$*" | tee -a "$summary"; }
jobs=$(nproc 2>/dev/null || echo 2)

say "target: $target  seconds: $seconds  host: $(uname -m)  clang: $(clang --version | head -1)"
say "start: $(date -u +%Y-%m-%dT%H:%M:%SZ)"

finish_tar() {
	if [ "$tar_out" -eq 1 ]; then tar -czf - -C "$out" . >&3; fi
}

# ---- engine -------------------------------------------------------------------------------------
eng=/tmp/deps-fuzz
CC=clang CFLAGS="-g -O1 -fsanitize=fuzzer-no-link,address,undefined -fsanitize-recover=undefined -fno-omit-frame-pointer" \
	LDFLAGS="-fsanitize=address,undefined" \
	sh "$work/scripts/build-ntfs3g.sh" --prefix "$eng" --build-dir /tmp/b-eng-fuzz --jobs "$jobs" \
	>"$out/engine.log" 2>&1 || { tail -30 "$out/engine.log"; say "RESULT: engine build failed"; finish_tar; exit 1; }
{
	echo "libntfs-3g: $(PKG_CONFIG_PATH=$eng/lib/pkgconfig pkg-config --modversion libntfs-3g)"
	echo "patches applied:"
	grep -o 'applying [0-9][0-9]*-[a-z0-9-]*\.patch' "$out/engine.log" | sed 's/^applying /  /'
} | tee -a "$summary"

# ---- build ----------------------------------------------------------------------------------------
build=/tmp/fuzz-build
rm -rf "$build"
PKG_CONFIG_PATH=$eng/lib/pkgconfig CC=clang cmake -S "$work" -B "$build" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
	-DNTFSMAC_FUZZ=ON -DCMAKE_C_COMPILER=clang >"$out/cmake.log" 2>&1 ||
	{ cat "$out/cmake.log"; say "RESULT: build failed"; finish_tar; exit 1; }
cmake --build "$build" --target "fuzz_$target" >"$out/build.log" 2>&1 ||
	{ cat "$out/build.log"; say "RESULT: build failed"; finish_tar; exit 1; }
bin=$build/tests/fuzz/image/fuzz_$target

corpus=/tmp/fuzz-corpus
rm -rf "$corpus"
mkdir -p "$corpus" "$out/artifacts"
cp "$work/tests/fuzz/image/corpus/$target"/* "$corpus/"
seeds=$(ls "$corpus" | wc -l | tr -d ' ')
say "seeds: $seeds"

# ---- fuzz -----------------------------------------------------------------------------------------
now() { date +%s; }
t_start=$(now)
used=0
iter=0
rc_all=0
execs=0
: >"$out/libfuzzer.log"
while [ "$used" -lt "$seconds" ]; do
	iter=$((iter + 1))
	left=$((seconds - used))
	log=$out/libfuzzer-$iter.log
	rc=0
	t0=$(now)
	"$bin" "$corpus" -max_len=4096 -max_total_time="$left" -timeout=20 -rss_limit_mb=4400 -malloc_limit_mb=4200 \
		-use_value_profile=1 -print_final_stats=1 -artifact_prefix="$out/artifacts/i$iter-" \
		-seed=$((20261115 + iter)) >"$log" 2>&1 || rc=$?
	t1=$(now)
	used=$((used + t1 - t0))
	say "run $iter: exit $rc after $((t1 - t0)) s ($(date -u +%H:%M:%SZ))"
	n=$(grep '^stat::number_of_executed_units' "$log" | tail -1 | tr -dc '0-9')
	execs=$((execs + ${n:-0}))
	python3 -I -B "$work/tests/fuzz/image/symbolize.py" "$log" >"$log.sym" 2>/dev/null || true
	cat "$log" >>"$out/libfuzzer.log"
	if [ "$rc" -ne 0 ]; then
		rc_all=1
		grep -E "ERROR: |SUMMARY: |runtime error|oracle failure|Test unit written" "$log" | head -6 | sed 's/^/  /' | tee -a "$summary"
	fi
	if [ "$iter" -ge 60 ]; then say "too many restarts, stopping"; break; fi
done
for k in stat::average_exec_per_sec stat::new_units_added stat::slowest_unit_time_sec stat::peak_rss_mb; do
	grep "^$k" "$out/libfuzzer-$iter.log" | tail -1 | tee -a "$summary" || true
done
say "executions in all runs: $execs"
say "last progress line: $(grep -E '^#[0-9]+' "$out/libfuzzer-$iter.log" | tail -1 || true)"
artifacts=$(ls "$out/artifacts" | wc -l | tr -d ' ')
say "artifacts (crash, oom, timeout, slow unit): $artifacts"
ls "$out/artifacts" | sed 's/^/  /' | tee -a "$summary"
# UBSan reports of the library (recoverable): the distinct locations
grep -h "runtime error" "$out"/libfuzzer-[0-9]*.log | sed 's/^[^ ]*src\/\|^[^ ]*libntfs-3g\///' | sort | uniq -c | sort -rn >"$out/ubsan-engine.txt" || true
say "UBSan reports (distinct lines, with counts): $(wc -l <"$out/ubsan-engine.txt" | tr -d ' ')"
head -20 "$out/ubsan-engine.txt" | sed 's/^/  /' | tee -a "$summary"
units=$(ls "$corpus" | wc -l | tr -d ' ')
say "corpus after the run: $units inputs"
say "fuzz CPU seconds: $used"
mkdir -p "$out/corpus"
cp "$corpus"/* "$out/corpus/" 2>/dev/null || true

# ---- leaks ------------------------------------------------------------------------------------------
leaklog=$out/leaks.log
: >"$leaklog"
lopts="-timeout=20 -rss_limit_mb=4400 -malloc_limit_mb=4200"
lasan=abort_on_error=0:detect_leaks=1:allocator_may_return_null=0
ls "$corpus" >"$out/replay-all.txt"
nin=$(wc -l <"$out/replay-all.txt" | tr -d ' ')
rm -f "$out"/replay-chunk.*
split -l 64 "$out/replay-all.txt" "$out/replay-chunk."
nleaking=0
nbad=0
complete=1
lt0=$(now)
for c in "$out"/replay-chunk.*; do
	if [ "$(($(now) - lt0))" -ge 1500 ]; then complete=0; break; fi
	# the file names of a chunk, one per line (hash names): split on newlines only, no globbing
	crc=0
	( cd "$corpus" && IFS='
' && set -f && set -- $(cat "$c") && IFS=' ' && ASAN_OPTIONS=$lasan "$bin" $lopts "$@" ) </dev/null >"$out/replay.tmp" 2>&1 || crc=$?
	[ "$crc" -ne 0 ] || continue
	nsingle=$nleaking
	while read -r f; do
		if [ "$(($(now) - lt0))" -ge 1500 ]; then complete=0; break; fi
		rc=0
		# shellcheck disable=SC2086
		ASAN_OPTIONS=$lasan "$bin" $lopts "$corpus/$f" </dev/null >"$out/replay.tmp" 2>&1 || rc=$?
		[ "$rc" -ne 0 ] || continue
		nleaking=$((nleaking + 1))
		{ echo "=== replay input $f (exit $rc)"; cat "$out/replay.tmp"; } >>"$leaklog"
		grep -q '^Direct leak of' "$out/replay.tmp" || nbad=$((nbad + 1))
	done <"$c"
	# the chunk failed but none of its inputs fails alone: not explained by a leak report, counts as failed
	if [ "$complete" -eq 1 ] && [ "$nleaking" -eq "$nsingle" ]; then nbad=$((nbad + 1)); fi
	[ "$complete" -eq 1 ] || break
done
rm -f "$out"/replay-chunk.* "$out/replay.tmp"
python3 -I -B "$work/tests/fuzz/image/symbolize.py" "$leaklog" >"$leaklog.sym" 2>/dev/null || true
nleaks=$(grep -c '^Direct leak\|^Indirect leak' "$leaklog" || true)
nleaks_sym=$(grep -c '^Direct leak\|^Indirect leak' "$leaklog.sym" || true)
say "leak replay: inputs $nin, leaking $nleaking, failed without a leak report $nbad"
if [ "$complete" -eq 1 ]; then say "replay complete"; else say "replay INCOMPLETE (time bound)"; fi
say "leaks in the corpus replay: $nleaks (leaks.log.sym)"
grep -A4 '^Direct leak' "$leaklog.sym" | grep -v '^--' | sed 's/^/  /' | head -60 | tee -a "$summary"
if [ "${nleaks_sym:-0}" -ne "${nleaks:-0}" ]; then say "symbolized leak report differs from the raw one: $nleaks_sym of $nleaks leak lines"; fi
if [ "${nleaks:-0}" -ne 0 ] || [ "$nleaking" -ne 0 ] || [ "${nleaks_sym:-0}" -ne "${nleaks:-0}" ] || [ "$complete" -ne 1 ] || [ "$nbad" -ne 0 ]; then leak_found=1; else leak_found=0; fi

# ---- coverage ---------------------------------------------------------------------------------------
cov=/tmp/fuzz-cov
rm -rf "$cov" /tmp/deps-cov /tmp/b-eng-cov
mkdir -p "$cov"
CC=gcc CFLAGS="-g -O0 --coverage" LDFLAGS="--coverage" \
	sh "$work/scripts/build-ntfs3g.sh" --prefix /tmp/deps-cov --build-dir /tmp/b-eng-cov --jobs "$jobs" \
	>"$out/engine-cov.log" 2>&1 || { tail -20 "$out/engine-cov.log"; say "coverage: engine build failed"; }
if [ -f /tmp/deps-cov/lib/libntfs-3g.a ]; then
	(
		cd "$cov"
		pc=/tmp/deps-cov/lib/pkgconfig
		cflags=$(PKG_CONFIG_PATH=$pc pkg-config --cflags libntfs-3g)
		libs=$(PKG_CONFIG_PATH=$pc pkg-config --libs --static libntfs-3g)
		common="$work/tests/fuzz/image/image.c $work/tests/fuzz/sanitize/fuzz_main.c"
		inc="-I$work/include -I$work/src/probe -I$work/src/sanitize -I$work/src/common -I$work/tests/fuzz/image"
		if [ "$target" = probe ]; then
			srcs="$work/tests/fuzz/image/fuzz_probe.c $work/src/probe/probe.c $work/src/probe/devio.c $work/src/probe/names.c
			      $work/src/probe/nfc.c $work/src/probe/state.c $work/src/probe/text.c $work/src/probe/json.c
			      $work/src/common/mftmirr.c $work/src/common/state_rank.c $work/src/common/label_cp.c
			      $work/src/sanitize/verdict.c $work/src/sanitize/label.c $work/src/sanitize/display.c"
		else
			srcs="$work/tests/fuzz/image/fuzz_mountwalk.c"
		fi
		# shellcheck disable=SC2086
		gcc -std=gnu17 -O0 -g --coverage -DFZ_BASES_DIR="\"$work/tests/fuzz/image/bases\"" $inc $cflags \
			-o replay $common $srcs $libs >"$out/cov-build.log" 2>&1 || { tail -20 "$out/cov-build.log"; exit 1; }
		unset ASAN_OPTIONS
		./replay "$corpus" >/dev/null 2>"$out/cov-replay.log" || echo "coverage replay exit $?" >&2
		# project code gcov from here; library gcov from the library's build directory
		gcov -b ./*.gcda >"$out/gcov-project.txt" 2>&1 || true
		cp ./*.gcov "$out/" 2>/dev/null || true
	) || say "coverage: harness build failed"
	# the replay wrote the .gcda files next to the library objects
	mkdir -p "$out/gcov-engine"
	for d in $(find /tmp/b-eng-cov -name '*.gcda' -exec dirname {} \; | sort -u); do
		(cd "$d" && gcov -b ./*.gcda >>"$out/gcov-engine.txt" 2>&1 || true; cp ./*.c.gcov "$out/gcov-engine/" 2>/dev/null || true)
	done
	say "coverage of the code under test (gcov, replay of the corpus above; lines executed of lines):"
	for f in "$out"/gcov-project.txt; do
		grep -A1 -E "^File '.*/(src/probe|src/common|tests/fuzz/image)/[a-z_]+\.c'" "$f" | grep -v '^--' |
			paste - - | sed "s|File '$work/||; s|'||" | awk '{print "  " $0}' | tee -a "$summary"
	done
	grep -h -A1 -E "^File '[a-z_0-9]+\.c'" "$out/gcov-engine.txt" 2>/dev/null | grep -v '^--' | paste - - |
		sed "s|File '||; s|'||" | sort -u | awk '{print "  " $0}' | tee -a "$summary"
fi
say "end: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [ "$rc_all" -ne 0 ] || [ "$artifacts" -ne 0 ] || [ "$leak_found" -ne 0 ]; then
	say "RESULT: FINDING (artifacts/, libfuzzer-*.log)"
	res=1
else
	say "RESULT: no finding in $seconds s"
	res=0
fi
finish_tar
exit "$res"
