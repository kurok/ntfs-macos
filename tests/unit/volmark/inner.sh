#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# inner.sh - the volmark tests inside Environment A (the container). Called by run.sh.
#
# Usage (inside the container, repo at /work, read-only):
#   sh /work/tests/unit/volmark/inner.sh [--tar] [--no-asan] [--with-image]
#
# Stages (a stage that fails ends the run with exit 1 after the summary):
#   deps-release        static libntfs-3g from the pinned tarball with the patch series
#   deps-faultio        the same plus --enable-faultio (patch 0004), own prefix and build dir
#   deps-asan           both again with -fsanitize=address (unless --no-asan)
#   fixtures            tests/fixtures/gen/unsafe.sh and clean.sh --only clean-64m-4k
#   build-faultio       volmark_test against the fault-layer library
#   release             cmake build, ctest -R '^volmark\.' (U-VMK-01..10, crash table, vmk-split)
#   asan                the same with AddressSanitizer + UBSan (project code; libntfs-3g ASan)
# Everything lands in /tmp/volmark-out (logs, ctest JUnit, summary.txt, volmark-evidence.txt,
# vmk-split-record3.txt; vmk-split.img only with --with-image); with --tar it is streamed as a
# gzip tar on the original stdout.
# shellcheck disable=SC2329  # the stage functions are invoked through stage()
set -eu

work=/work
out=/tmp/volmark-out
tar_out=0
do_asan=1
with_image=0
for a in "$@"; do
	case $a in
	--tar) tar_out=1 ;;
	--no-asan) do_asan=0 ;;
	--with-image) with_image=1 ;;
	*) echo "inner.sh: unknown option $a" >&2; exit 2 ;;
	esac
done
mkdir -p "$out"
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

jobs=$(nproc 2>/dev/null || echo 2)
summary=
failed=0
fix_dirs="/tmp/ntfsmac-unsafe;/tmp/ntfsmac-fixtures"

stage() { # NAME CMD...: run, log to $out/NAME.log, record the seconds
	_n=$1; shift
	printf '\n== stage: %s ==\n' "$_n"
	_t0=$(date +%s)
	_rc=0
	"$@" >"$out/$_n.log" 2>&1 || _rc=$?
	_t1=$(date +%s)
	if [ "$_rc" -eq 0 ]; then _r=ok; else _r=FAIL; failed=1; tail -40 "$out/$_n.log"; fi
	summary="${summary}$(printf '%-16s %-5s %5ss' "$_n" "$_r" "$((_t1 - _t0))")
"
	printf '%s: %s (%ss)\n' "$_n" "$_r" "$((_t1 - _t0))"
	return 0
}

finish() {
	if [ "$with_image" -eq 0 ]; then rm -f "$out"/vmk/*.img "$out"/vmk-asan/*.img; fi
	printf '%s' "$summary" >"$out/summary.txt"
	printf '\n== summary ==\n%s' "$summary"
	{
		echo "ntfs-3g: $(ntfs-3g --version 2>&1 | head -1)"
		echo "kernel: $(uname -sr) $(uname -m)"
		echo "cc: $(cc --version | head -1)"
		echo "python: $(python3 --version)"
	} >"$out/environment.txt"
	if [ "$tar_out" -eq 1 ]; then tar -czf - -C "$out" . >&3; fi
	if [ "$failed" -ne 0 ]; then echo "volmark tests: FAILED"; exit 1; fi
	echo "volmark tests: PASSED"
	exit 0
}

# ---- dependencies ---------------------------------------------------------------------------
deps() { # PREFIX BUILD-DIR [faultio]
	_p=$1 _b=$2
	if [ "${3:-}" = faultio ]; then
		sh "$work/scripts/build-ntfs3g.sh" --fuse internal --prefix "$_p" --build-dir "$_b" --jobs "$jobs" \
			--configure-arg --enable-faultio
	else
		sh "$work/scripts/build-ntfs3g.sh" --prefix "$_p" --build-dir "$_b" --jobs "$jobs"
	fi
}
stage deps-release deps /tmp/deps-rel /tmp/b-ntfs-rel
stage deps-faultio deps /tmp/deps-fio /tmp/b-ntfs-fio faultio
if [ "$do_asan" -eq 1 ]; then
	# libntfs-3g under AddressSanitizer only (its own undefined behaviour is upstream's, as in the
	# probe tests); UBSan covers the project's code.
	stage deps-asan env CFLAGS="-g -O1 -fsanitize=address -fno-omit-frame-pointer" LDFLAGS="-fsanitize=address" \
		sh "$work/scripts/build-ntfs3g.sh" --prefix /tmp/deps-asan --build-dir /tmp/b-ntfs-asan --jobs "$jobs"
	stage deps-faultio-asan env CFLAGS="-g -O1 -fsanitize=address -fno-omit-frame-pointer" \
		LDFLAGS="-fsanitize=address" sh "$work/scripts/build-ntfs3g.sh" --fuse internal \
		--prefix /tmp/deps-fio-asan --build-dir /tmp/b-ntfs-fio-asan --jobs "$jobs" --configure-arg --enable-faultio
fi
[ "$failed" -eq 0 ] || finish

# ---- fixtures -------------------------------------------------------------------------------
stage fixtures-unsafe sh "$work/tests/fixtures/gen/unsafe.sh" --inner
stage fixtures-clean sh "$work/tests/fixtures/gen/clean.sh" --inner --only clean-64m-4k
[ "$failed" -eq 0 ] || finish

# ---- volmark_test against the fault layer ---------------------------------------------------------
fault_build() { # PKGDIR BUILDDIR [cmake args]
	_pk=$1 _bd=$2; shift 2
	PKG_CONFIG_PATH=$_pk cmake -S "$work" -B "$_bd" -G Ninja "$@" && cmake --build "$_bd" --target volmark_test
}
stage build-faultio fault_build /tmp/deps-fio/lib/pkgconfig /tmp/bld-fio -DCMAKE_BUILD_TYPE=Release
[ "$failed" -eq 0 ] || finish

# ---- release build and tests ----------------------------------------------------------------------
release() {
	PKG_CONFIG_PATH=/tmp/deps-rel/lib/pkgconfig cmake -S "$work" -B /tmp/bld-rel -G Ninja \
		-DCMAKE_BUILD_TYPE=Release "-DNTFSMAC_VOLMARK_FIXTURES=$fix_dirs" \
		-DNTFSMAC_VOLMARK_FAULT_BIN=/tmp/bld-fio/src/volmark/volmark_test "-DNTFSMAC_VOLMARK_OUT=$out/vmk" &&
		cmake --build /tmp/bld-rel &&
		ctest --test-dir /tmp/bld-rel -R '^volmark\.' --output-on-failure --output-junit "$out/ctest-release.xml" &&
		cp "$out/vmk/volmark-evidence.txt" "$out/volmark-evidence.txt" &&
		cp "$out/vmk/vmk-split-record3.txt" "$out/vmk-split-record3.txt"
}
stage release release
if [ "$failed" -ne 0 ]; then finish; fi

# ---- AddressSanitizer + UBSan ----------------------------------------------------------------------
if [ "$do_asan" -eq 1 ]; then
	asan() {
		rm -rf /tmp/asan && mkdir -p /tmp/asan && chmod 1777 /tmp/asan
		export ASAN_OPTIONS=detect_leaks=1:abort_on_error=0:log_path=/tmp/asan/asan
		export UBSAN_OPTIONS=print_stacktrace=1:log_path=/tmp/asan/ubsan
		export NTFS_ASAN=1
		fault_build /tmp/deps-fio-asan/lib/pkgconfig /tmp/bld-asan-fio -DCMAKE_BUILD_TYPE=Debug \
			-DNTFSMAC_SANITIZE=address,undefined &&
			PKG_CONFIG_PATH=/tmp/deps-asan/lib/pkgconfig cmake -S "$work" -B /tmp/bld-asan -G Ninja \
				-DCMAKE_BUILD_TYPE=Debug -DNTFSMAC_SANITIZE=address,undefined "-DNTFSMAC_VOLMARK_FIXTURES=$fix_dirs" \
				-DNTFSMAC_VOLMARK_FAULT_BIN=/tmp/bld-asan-fio/src/volmark/volmark_test \
				"-DNTFSMAC_VOLMARK_OUT=$out/vmk-asan" &&
			cmake --build /tmp/bld-asan &&
			ctest --test-dir /tmp/bld-asan -R '^volmark\.' --output-on-failure --output-junit "$out/ctest-asan.xml" ||
			return 1
		if ls /tmp/asan/* >/dev/null 2>&1; then
			echo "sanitizer reports:"
			cat /tmp/asan/*
			return 1
		fi
		echo "ASan/UBSan: no report"
	}
	stage asan asan
fi

finish
