#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# inner.sh - one unit (one target, one shard, one process) of the extended fuzz campaign (T-M5-09), run
# inside Environment A (the container). Called by run.sh.
#
# Usage (inside the container, repo at /work, read-only):
#   sh /work/tests/fuzz/campaign/inner.sh --target probe|mountwalk|verdict|label|display|ctl --seconds N
#      [--shard K] [--no-patches] [--tar]
#
# Stages:
#   engine    probe and mountwalk only: libntfs-3g from the pinned tarball with the release patch series
#             (scripts/build-ntfs3g.sh; --no-patches: pristine upstream), clang, static, built with
#             -fsanitize=fuzzer-no-link,address,undefined so the library is instrumented too (UBSan reports in
#             the library are printed, once per location, and do not stop the run)
#   build     cmake -DNTFSMAC_FUZZ=ON, the one fuzz target (ctl: the whole tree with the sanitizer flags)
#   fuzz      one libFuzzer process for N seconds in all, restarted after a crash (the artifact is saved, a
#             seed that crashes is dropped from the corpus), seeded with the repository corpora and the
#             reproducers in tests/fuzz/image/findings and tests/fuzz/findings
#   triage    every artifact replayed, signature taken (fuzzsig.py), duplicates dropped (smallest input kept),
#             the keeper minimised (libFuzzer -minimize_crash); probe and mountwalk then replay the corpus
#             with leak detection on and take one signature per distinct leak (libFuzzer stops at the first
#             leak, so those targets fuzz with leak detection off)
# Results land in /tmp/camp-out: summary.txt, findings.tsv (one row per distinct signature), inputs/ (the
# minimised inputs), logs/ (the replay of each), ubsan.tsv, libfuzzer.tail, corpus/. With --tar the directory
# is streamed as a gzip tar on the original stdout.
# Exit: 0 nothing found, 1 something found (see findings.tsv), 3 build or setup failure.
set -eu

work=/work
out=/tmp/camp-out
target=
seconds=600
rss=1800
shard=0
tar_out=0
nopatch=
while [ $# -gt 0 ]; do
	case $1 in
	--target) target=$2; shift ;;
	--seconds) seconds=$2; shift ;;
	--rss-mb) rss=$2; shift ;;
	--shard) shard=$2; shift ;;
	--no-patches) nopatch=--no-patches ;;
	--tar) tar_out=1 ;;
	*) echo "inner.sh: unknown option $1" >&2; exit 2 ;;
	esac
	shift
done
bld=/tmp/fuzz-build
dict=
leakmode=1
case $target in
probe) maxlen=4096; seeddir=$work/tests/fuzz/image/corpus/probe; bin=$bld/tests/fuzz/image/fuzz_probe; leakmode=0 ;;
mountwalk) maxlen=4096; seeddir=$work/tests/fuzz/image/corpus/mountwalk; bin=$bld/tests/fuzz/image/fuzz_mountwalk; leakmode=0 ;;
verdict) maxlen=70000; seeddir=$work/tests/fuzz/sanitize/corpus/verdict; bin=$bld/tests/fuzz/sanitize/fuzz_sanitize_verdict
	dict="-dict=$work/tests/fuzz/sanitize/verdict.dict" ;;
label) maxlen=1100; seeddir=$work/tests/fuzz/sanitize/corpus/label; bin=$bld/tests/fuzz/sanitize/fuzz_sanitize_label ;;
display) maxlen=1100; seeddir=$work/tests/fuzz/sanitize/corpus/display; bin=$bld/tests/fuzz/sanitize/fuzz_sanitize_display ;;
ctl) maxlen=8200; seeddir=$work/tests/fuzz/ctl/corpus; bin=$bld/tests/fuzz/ctl/fuzz_ctl ;;
*) echo "inner.sh: --target probe|mountwalk|verdict|label|display|ctl" >&2; exit 2 ;;
esac
sig="python3 -I -B $work/tests/fuzz/campaign/fuzzsig.py"
sym="python3 -I -B $work/tests/fuzz/image/symbolize.py"
known=$work/tests/fuzz/campaign/known.tsv
unit=$target-s$shard

mkdir -p "$out/inputs" "$out/logs" "$out/artifacts"
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi
tar_done=0
finish_tar() {
	if [ "$tar_out" -eq 1 ] && [ "$tar_done" -eq 0 ]; then
		tar_done=1
		tar -czf - -C "$out" . >&3
	fi
}
# whatever happens (a failed command under set -e, a signal), the results directory reaches the host
trap finish_tar EXIT

# The container image sets sanitizer log paths for the engine build; the fuzzers report on stderr.
unset ASAN_OPTIONS UBSAN_OPTIONS
asan="abort_on_error=0:allocator_may_return_null=0"
export UBSAN_OPTIONS=print_stacktrace=1
malloc=$rss
[ "$malloc" -le 1024 ] || malloc=1024
summary=$out/summary.txt
: >"$summary"
say() { printf '%s\n' "$*" | tee -a "$summary"; }
jobs=$(nproc 2>/dev/null || echo 2)

say "unit: $unit  target: $target  seconds: $seconds  host: $(uname -m)  clang: $(clang --version | head -1)"
say "start: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [ -n "$nopatch" ]; then say "engine-patches: none"; else say "engine-patches: series"; fi

# ---- engine and build ---------------------------------------------------------------------------
rm -rf "$bld"
case $target in
probe | mountwalk)
	eng=/tmp/deps-fuzz
	CC=clang CFLAGS="-g -O1 -fsanitize=fuzzer-no-link,address,undefined -fsanitize-recover=undefined -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address,undefined" \
		sh "$work/scripts/build-ntfs3g.sh" --prefix "$eng" --build-dir /tmp/b-eng-fuzz --jobs "$jobs" $nopatch \
		>"$out/engine.log" 2>&1 || { tail -30 "$out/engine.log"; say "RESULT: engine build failed"; finish_tar; exit 3; }
	{
		echo "libntfs-3g: $(PKG_CONFIG_PATH=$eng/lib/pkgconfig pkg-config --modversion libntfs-3g)"
		echo "patches applied: $(grep -o 'applying [0-9][0-9]*-[a-z0-9-]*\.patch' "$out/engine.log" | sed 's/^applying //' | tr '\n' ' ')"
	} | tee -a "$summary"
	PKG_CONFIG_PATH=$eng/lib/pkgconfig CC=clang cmake -S "$work" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Debug \
		-DNTFSMAC_FUZZ=ON -DCMAKE_C_COMPILER=clang >"$out/cmake.log" 2>&1 ||
		{ cat "$out/cmake.log"; say "RESULT: configure failed"; finish_tar; exit 3; }
	;;
ctl)
	# The daemon sources need the GNU feature macros on Linux (they are written for macOS); the
	# whole tree is built with the sanitizer flags so that the code under test is instrumented.
	CC=clang cmake -S "$work" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Debug -DNTFSMAC_FUZZ=ON -DCMAKE_C_COMPILER=clang \
		"-DCMAKE_C_FLAGS=-D_GNU_SOURCE -Wno-macro-redefined -g -O1 -fsanitize=fuzzer-no-link,address,undefined -fno-sanitize-recover=all -fno-omit-frame-pointer" \
		>"$out/cmake.log" 2>&1 || { cat "$out/cmake.log"; say "RESULT: configure failed"; finish_tar; exit 3; }
	;;
*)
	CC=clang cmake -S "$work" -B "$bld" -G Ninja -DCMAKE_BUILD_TYPE=Release -DNTFSMAC_FUZZ=ON -DCMAKE_C_COMPILER=clang \
		>"$out/cmake.log" 2>&1 || { cat "$out/cmake.log"; say "RESULT: configure failed"; finish_tar; exit 3; }
	;;
esac
case $target in
probe | mountwalk) tgt=fuzz_$target ;;
ctl) tgt=fuzz_ctl ;;
*) tgt=fuzz_sanitize_$target ;;
esac
cmake --build "$bld" --target "$tgt" >"$out/build.log" 2>&1 || { tail -30 "$out/build.log"; say "RESULT: build failed"; finish_tar; exit 3; }

# ---- corpus -------------------------------------------------------------------------------------
corpus=/tmp/fuzz-corpus
rm -rf "$corpus"
mkdir -p "$corpus"
cp "$seeddir"/* "$corpus/"
case $target in
probe | mountwalk)
	# reproducers of the known findings and the campaign's own regression inputs are seeds too
	for f in "$work"/tests/fuzz/image/findings/*.bin; do
		[ -f "$f" ] && cp "$f" "$corpus/seed-$(basename "$f")"
	done
	;;
esac
# the campaign's own regression inputs for this target (tests/fuzz/findings/M5-NN-<target>.bin)
for f in "$work"/tests/fuzz/findings/*-"$target".bin; do
	[ -f "$f" ] && cp "$f" "$corpus/seed-$(basename "$f")"
done
say "seeds: $(ls "$corpus" | wc -l | tr -d ' ')"

# ---- fuzz ---------------------------------------------------------------------------------------
now() { date +%s; }
# CPU milliseconds (user + system) of all children reaped so far, in $cpu. The shell's own `times` is read
# without a subshell (a command substitution would report only its own, empty, children).
cpu=0
cpu_ms() {
	times >"$out/times.tmp"
	cpu=$(sed -n 2p "$out/times.tmp" | awk '{ t = 0; for (i = 1; i <= 2; i++) { split($i, a, /[ms]/); t += a[1] * 60000 + a[2] * 1000 } printf "%d", t }')
}
used=0
wall=0
iter=0
execs=0
: >"$out/libfuzzer.log"
while [ "$used" -lt "$seconds" ]; do
	iter=$((iter + 1))
	left=$((seconds - used))
	log=$out/libfuzzer-$iter.log
	rc=0
	t0=$(now)
	cpu_ms
	c0=$cpu
	# shellcheck disable=SC2086
	ASAN_OPTIONS="$asan:detect_leaks=$leakmode" "$bin" "$corpus" $dict -max_len=$maxlen -max_total_time="$left" \
		-timeout=20 -rss_limit_mb=$rss -malloc_limit_mb=$malloc -use_value_profile=1 -print_final_stats=1 \
		-artifact_prefix="$out/artifacts/i$iter-" -seed=$((20261120 + shard * 1000 + iter)) >"$log" 2>&1 || rc=$?
	t1=$(now)
	cpu_ms
	c1=$cpu
	dcpu=$(((c1 - c0 + 500) / 1000))
	# a process that is mostly idle (or an accounting that reads 0) must not stretch the unit forever: charge wall time then
	[ $((dcpu * 10)) -ge $((t1 - t0)) ] || dcpu=$((t1 - t0))
	used=$((used + dcpu))
	wall=$((wall + t1 - t0))
	say "run $iter: exit $rc after $((t1 - t0)) s wall, $dcpu s charged ($(date -u +%H:%M:%SZ))"
	n=$(grep '^stat::number_of_executed_units' "$log" | tail -1 | tr -dc '0-9')
	execs=$((execs + ${n:-0}))
	$sym "$log" >"$log.sym" 2>/dev/null || cp "$log" "$log.sym"
	if [ "$rc" -ne 0 ]; then
		grep -E "ERROR: |SUMMARY: |oracle failure|Test unit written" "$log" | head -4 | sed 's/^/  /' | tee -a "$summary"
		# a seed that crashes would crash every restart: drop corpus files equal to a new artifact
		for a in "$out/artifacts/i$iter-"*; do
			[ -f "$a" ] || continue
			for c in "$corpus"/*; do cmp -s "$a" "$c" && rm -f "$c"; done
		done
		# an exit without an artifact (killed, out of memory in the container, 137) is a row to review
		if ! ls "$out/artifacts/i$iter-"* >/dev/null 2>&1; then
			: >"$out/artifacts/i$iter-exit-$rc-noartifact"
			say "  exit $rc without an artifact (recorded as a row to review)"
		fi
	fi
	[ "$iter" -lt 80 ] || { say "too many restarts, stopping"; break; }
done
say "executions in all runs: $execs"
for k in stat::average_exec_per_sec stat::new_units_added stat::slowest_unit_time_sec stat::peak_rss_mb; do
	grep "^$k" "$out/libfuzzer-$iter.log" | tail -1 | tee -a "$summary" || true
done
say "last progress line: $(grep -E '^#[0-9]+' "$out/libfuzzer-$iter.log" | tail -1 || true)"
tail -40 "$out/libfuzzer-$iter.log" >"$out/libfuzzer.tail"
say "fuzz CPU seconds: $used"
say "fuzz wall seconds: $wall"
say "corpus after the run: $(ls "$corpus" | wc -l | tr -d ' ') inputs"
mkdir -p "$out/corpus"
cp "$corpus"/* "$out/corpus/" 2>/dev/null || true
cat "$out"/libfuzzer-[0-9]*.log.sym 2>/dev/null >"$out/all.log.sym" || true
$sig ubsan "$out/all.log.sym" >"$out/ubsan.tsv" 2>/dev/null || true
say "UBSan locations in the engine (distinct, with counts): $(wc -l <"$out/ubsan.tsv" | tr -d ' ')"

# ---- triage -------------------------------------------------------------------------------------
# replay one input: log in $2 (symbolized), signature line on stdout (empty: no event)
replay() {
	ASAN_OPTIONS="$asan:detect_leaks=$3" "$bin" -runs=1 -timeout=20 -rss_limit_mb=$rss -malloc_limit_mb=$malloc "$1" \
		>"$2.raw" 2>&1 || true
	$sym "$2.raw" >"$2" 2>/dev/null || cp "$2.raw" "$2"
	rm -f "$2.raw"
	$sig sig "$2" || true
}
: >"$out/all-artifacts.tsv"
for a in "$out/artifacts"/*; do
	[ -f "$a" ] || continue
	b=$(basename "$a")
	s=$(replay "$a" "$out/logs/$b.log" "$leakmode")
	size=$(wc -c <"$a" | tr -d ' ')
	if [ -z "$s" ]; then
		pre=$(echo "$b" | sed 's/^i[0-9]*-//; s/-[0-9a-f]\{40\}$//')
		s="not-reproducible $pre	other	-	unknown	-	-"
	fi
	printf '%s\t%s\t%s\n' "$b" "$size" "$s" >>"$out/all-artifacts.tsv"
done
# distinct signatures, smallest input first
: >"$out/findings.tsv"
n=0
sort -t "$(printf '\t')" -k3,3 -k2,2n "$out/all-artifacts.tsv" | awk -F '\t' '{ c[$3]++; if (!($3 in first)) { first[$3] = $0; order[++k] = $3 } } END { for (i = 1; i <= k; i++) print c[order[i]] "\t" first[order[i]] }' |
	while IFS="$(printf '\t')" read -r cnt art size key kind access comp caller frames; do
		n=$((n + 1))
		id=$unit-$(printf '%02d' "$n")
		min=$out/inputs/$id.bin
		cp "$out/artifacts/$art" "$min"
		note=original
		case $kind in
		other) ;;
		*)
			ASAN_OPTIONS="$asan:detect_leaks=$leakmode" "$bin" -minimize_crash=1 -runs=20000 -max_total_time=120 -timeout=20 \
				-rss_limit_mb=$rss -malloc_limit_mb=$malloc -exact_artifact_path="$out/min.tmp" \
				"$out/artifacts/$art" >"$out/logs/$id.minimize.log" 2>&1 || true
			if [ -s "$out/min.tmp" ]; then
				s2=$(replay "$out/min.tmp" "$out/logs/$id.min.log" "$leakmode" | cut -f1)
				if [ "$s2" = "$key" ]; then cp "$out/min.tmp" "$min"; note=minimised; else note=minimise-drifted; fi
				rm -f "$out/min.tmp"
			fi
			;;
		esac
		cp "$out/logs/$art.log" "$out/logs/$id.log" 2>/dev/null || true
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$id" "$key" "$kind" "$access" "$comp" "$caller" "$frames" "$cnt" "$size" \
			"$(wc -c <"$min" | tr -d ' ')" "$note" >>"$out/findings.tsv"
	done
n=$(wc -l <"$out/findings.tsv" | tr -d ' ')

# leaks of the corpus (probe and mountwalk: libFuzzer ran with leak detection off)
if [ "$leakmode" -eq 0 ]; then
	leaklog=$out/logs/leaks.log
	ASAN_OPTIONS="$asan:detect_leaks=1" "$bin" "$corpus" -runs=0 -rss_limit_mb=$rss -malloc_limit_mb=$malloc >"$leaklog.raw" 2>&1 || true
	$sym "$leaklog.raw" >"$leaklog" 2>/dev/null || cp "$leaklog.raw" "$leaklog"
	rm -f "$leaklog.raw"
	$sig leaks "$leaklog" >"$out/leaks.tsv" 2>/dev/null || true
	say "distinct leaks in the corpus replay: $(wc -l <"$out/leaks.tsv" | tr -d ' ')"
	k=0
	while IFS="$(printf '\t')" read -r key kind access comp caller frames size; do
		k=$((k + 1))
		id=$unit-L$(printf '%02d' "$k")
		line=$(printf '%s\t%s\t%s\t%s\t%s\t%s' "$key" "$kind" "$access" "$comp" "$caller" "$frames")
		file=-
		msize=0
		if [ "$($sig match "$known" "$line")" = "-" ]; then
			# a leak that no known finding explains: find the corpus input that shows it (one run per file, capped)
			t0=$(now)
			for c in "$corpus"/*; do
				[ "$(($(now) - t0))" -lt 600 ] || break
				s=$(replay "$c" "$out/logs/probe.tmp" 1 | cut -f1)
				if [ "$s" = "$key" ]; then file=$id.bin; cp "$c" "$out/inputs/$file"; msize=$(wc -c <"$c" | tr -d ' '); break; fi
			done
			rm -f "$out/logs/probe.tmp"
		fi
		printf '%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\t%s\n' "$id" "$key" "$kind" "$access" "$comp" "$caller" "$frames" 1 "$msize" \
			"$msize" "corpus-replay:$file" >>"$out/findings.tsv"
	done <"$out/leaks.tsv"
	n=$(wc -l <"$out/findings.tsv" | tr -d ' ')
fi
say "artifacts: $(ls "$out/artifacts" | wc -l | tr -d ' ')  distinct signatures: $n"
cut -f1,2,3,5 "$out/findings.tsv" | sed 's/^/  /' | tee -a "$summary"
say "end: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
if [ "$n" -ne 0 ]; then say "RESULT: FINDING ($n distinct, see findings.tsv)"; res=1; else say "RESULT: no finding in $seconds s"; res=0; fi
rm -f "$out"/libfuzzer-[0-9]*.log "$out"/all.log.sym
finish_tar
exit "$res"
