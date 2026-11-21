#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# inner.sh - the probe tests inside Environment A (the container). Called by run.sh.
#
# Usage (inside the container, repo at /work, read-only):
#   sh /work/tests/unit/probe/inner.sh [--tar] [--no-asan] [--big] [--quick-fixtures]
#
# Stages (a stage that fails ends the run with exit 1 after the summary):
#   deps-release  static libntfs-3g from the pinned tarball (scripts/build-ntfs3g.sh)
#   deps-asan     the same with -fsanitize=address,undefined
#   fixtures      tests/fixtures/gen/{unsafe,clean,names,mirror,vmk}.sh --inner
#   release       cmake build, ctest (the whole suite: policy, contracts, probe)
#   asan          the probe and its tests built with AddressSanitizer and UBSan (libntfs-3g: ASan);
#                 the journal tests are left out: journal.c:1150 passes NULL to qsort for an empty
#                 list, which UBSan reports on glibc (not part of T-M3-02)
#   big           1,000,000 entries, layouts dirs and flat (--big): tests/unit/probe/bigvol.py
# Everything lands in /tmp/probe-out (logs, ctest JUnit, summary.txt); with --tar it is
# streamed as a gzip tar on the original stdout.
# --quick-fixtures leaves out the 256 MiB clean fixtures (development only).
set -eu

work=/work
out=/tmp/probe-out
tar_out=0
do_asan=1
do_big=0
quick=0
for a in "$@"; do
	case $a in
	--tar) tar_out=1 ;;
	--no-asan) do_asan=0 ;;
	--big) do_big=1 ;;
	--quick-fixtures) quick=1 ;;
	*) echo "inner.sh: unknown option $a" >&2; exit 2 ;;
	esac
done
mkdir -p "$out"
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

jobs=$(nproc 2>/dev/null || echo 2)
summary=
failed=0
fix_dirs="/tmp/ntfsmac-unsafe;/tmp/ntfsmac-fixtures;/tmp/ntfsmac-names;/tmp/ntfsmac-mirror;/tmp/ntfsmac-vmk"

stage() { # NAME CMD...: run, log to $out/NAME.log, record the seconds
	_n=$1; shift
	printf '\n== stage: %s ==\n' "$_n"
	_t0=$(date +%s)
	_rc=0
	"$@" >"$out/$_n.log" 2>&1 || _rc=$?
	_t1=$(date +%s)
	if [ "$_rc" -eq 0 ]; then _r=ok; else _r=FAIL; failed=1; tail -40 "$out/$_n.log"; fi
	summary="${summary}$(printf '%-14s %-5s %5ss' "$_n" "$_r" "$((_t1 - _t0))")
"
	printf '%s: %s (%ss)\n' "$_n" "$_r" "$((_t1 - _t0))"
	return 0
}

finish() {
	printf '%s' "$summary" >"$out/summary.txt"
	printf '\n== summary ==\n%s' "$summary"
	{
		echo "ntfs-3g: $(ntfs-3g --version 2>&1 | head -1)"
		echo "kernel: $(uname -sr) $(uname -m)"
		echo "cc: $(cc --version | head -1)"
		echo "python: $(python3 --version)"
	} >"$out/environment.txt"
	if [ "$tar_out" -eq 1 ]; then tar -czf - -C "$out" . >&3; fi
	if [ "$failed" -ne 0 ]; then echo "probe tests: FAILED"; exit 1; fi
	_left=
	if [ "$do_asan" -eq 1 ]; then _left="ASan stage runs without the journal.* tests"; else _left="ASan stage not run (--no-asan)"; fi
	[ "$do_big" -eq 1 ] || _left="$_left; no 1,000,000-entry stage"
	[ "$quick" -eq 0 ] || _left="$_left; clean fixtures other than clean-64m-4k (--quick-fixtures)"
	echo "probe tests: PASSED (left out: $_left)"
	exit 0
}

# ---- dependencies ----------------------------------------------------------------------
stage deps-release sh "$work/scripts/build-ntfs3g.sh" --prefix /tmp/deps-rel --build-dir /tmp/b-ntfs-rel --jobs "$jobs"
[ "$failed" -eq 0 ] || finish
if [ "$do_asan" -eq 1 ]; then
	# libntfs-3g under AddressSanitizer only: its own undefined behaviour (for example the left
	# shift of a negative value in runlist.c:897, hit by every run list with a backward offset) is
	# upstream's and not what is tested; UBSan covers the project's code (-fno-sanitize-recover).
	stage deps-asan env CFLAGS="-g -O1 -fsanitize=address -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address" \
		sh "$work/scripts/build-ntfs3g.sh" --prefix /tmp/deps-asan --build-dir /tmp/b-ntfs-asan --jobs "$jobs"
	[ "$failed" -eq 0 ] || finish
fi

# ---- fixtures --------------------------------------------------------------------------------
gen() { # SCRIPT
	sh "$work/tests/fixtures/gen/$1.sh" --inner
}
stage fixtures-unsafe gen unsafe
stage fixtures-names gen names
stage fixtures-mirror gen mirror
if [ "$quick" -eq 1 ]; then
	export NTFSMAC_PROBE_ALLOW_MISSING_FIXTURES=1 # the development mode: missing clean fixtures skip (test_probe.py)
	stage fixtures-clean sh "$work/tests/fixtures/gen/clean.sh" --inner --only clean-64m-4k
else
	stage fixtures-clean gen clean
fi
[ "$failed" -eq 0 ] || finish
# vmk-split and vmk-split-clear (U-PROBE-MIRROR-01): volmark_test against the fault-layer
# libntfs-3g, from clean-64m-4k made just above
stage fixtures-vmk gen vmk
[ "$failed" -eq 0 ] || finish
# keep the generators' own logs next to ours
for d in ntfsmac-unsafe ntfsmac-names ntfsmac-mirror ntfsmac-vmk; do
	[ -d "/tmp/$d" ] && find "/tmp/$d" -maxdepth 1 \( -name '*results*.txt' -o -name '*record3.txt' \) -exec cp {} "$out/" \; 2>/dev/null
done
true

# ---- release build and tests --------------------------------------------------------------------
release() {
	export NTFSMAC_EVIDENCE_DIR="$out"
	PKG_CONFIG_PATH=/tmp/deps-rel/lib/pkgconfig cmake -S "$work" -B /tmp/bld-rel -G Ninja \
		-DCMAKE_BUILD_TYPE=Release "-DNTFSMAC_PROBE_FIXTURES=$fix_dirs" &&
		cmake --build /tmp/bld-rel &&
		ctest --test-dir /tmp/bld-rel --output-on-failure --output-junit "$out/ctest-release.xml"
}
stage release release
if [ "$failed" -ne 0 ]; then finish; fi

# ---- AddressSanitizer + UBSan ------------------------------------------------------------------------
if [ "$do_asan" -eq 1 ]; then
	asan() {
		rm -rf /tmp/asan && mkdir -p /tmp/asan && chmod 1777 /tmp/asan
		export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:log_path=/tmp/asan/asan
		export UBSAN_OPTIONS=print_stacktrace=1:log_path=/tmp/asan/ubsan
		export NTFS_ASAN=1
		export NTFSMAC_PROBE_MUTATIONS=500
		PKG_CONFIG_PATH=/tmp/deps-asan/lib/pkgconfig cmake -S "$work" -B /tmp/bld-asan -G Ninja \
			-DCMAKE_BUILD_TYPE=Debug -DNTFSMAC_SANITIZE=address,undefined "-DNTFSMAC_PROBE_FIXTURES=$fix_dirs" &&
			cmake --build /tmp/bld-asan &&
			ctest --test-dir /tmp/bld-asan -E '^journal\.' --output-on-failure --output-junit "$out/ctest-asan.xml" || return 1
		if ls /tmp/asan/* >/dev/null 2>&1; then
			echo "sanitizer reports:"
			cat /tmp/asan/*
			return 1
		fi
		echo "ASan/UBSan: no report"
	}
	stage asan asan
fi

# ---- 1,000,000 entries ----------------------------------------------------------------------------------
if [ "$do_big" -eq 1 ]; then
	for layout in dirs flat; do
		stage "big-$layout" python3 -I "$work/tests/unit/probe/bigvol.py" --probe /tmp/bld-rel/src/probe/ntfsmac-probe \
			--image "/tmp/big-$layout.img" --entries 1000000 --layout "$layout" --runs 3 --plant-nfd \
			--out "$out/big-$layout.json"
	done
	if [ "$do_asan" -eq 1 ]; then
		stage big-asan sh -c "ASAN_OPTIONS=detect_leaks=1:log_path=/tmp/asan/asan python3 -I '$work/tests/unit/probe/bigvol.py' \
			--probe /tmp/bld-asan/src/probe/ntfsmac-probe --image /tmp/big-asan.img --entries 1000000 --layout dirs \
			--runs 1 --limit-s 120 --plant-nfd --out '$out/big-asan.json'"
	fi
fi

finish
