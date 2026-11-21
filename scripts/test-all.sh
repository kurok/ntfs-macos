#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# One command per environment (T-M5-01, criterion 5.1; TEST_STRATEGY.md sections 9 and 10).
# Runs every stage of the environment in order, keeps going after a failing stage, and writes ONE
# result file (schema ntfsmac-test-result/1, tests/harness/report.py aggregate). Nothing is
# uploaded or downloaded; nothing is mounted on the host; NTFS volumes are mounted only in the Linux
# container (Environment A) and in disposable clones of the golden VM (Environment B).
#
# The whole suite is THREE commands: --env host (macOS build and unit tests), --env A (Linux
# container) and --env B (macOS VM). Each writes its own result file.
#
# Usage: scripts/test-all.sh --env A|B|host [--quick] [--out FILE] [--cases GLOB] [--dry-run]
#                            [--fixtures DIR] [--slot-dir DIR] [--keep-work] [--update-readme]
#   --env host   build and unit tests on this Mac (release, product, ASan): never a mount
#   --env A      Linux container: selftest, probe and volmark suites, harness cases of
#                Environment A, stress entries registered for A
#   --env B      macOS VM (through vm/macos/run-suite.sh): harness cases of B, the host-side
#                reference comparison (C-REF-*), the daemon suites (dmn_all.sh), stress entries for B
#   --quick      a smoke run: skips the long stages (ASan, probe/volmark, compat, daemon suites,
#                stress) and runs only the I-INT-* harness cases
#   --out FILE   result file (default tests/results/<UTC date>-<short sha>-<env>[-runN].json);
#                the evidence goes to evidence/<name of FILE> next to it
#   --cases GLOB comma-separated case ids or fnmatch patterns for the harness stages; every other
#                stage is then reported as skipped ("not selected by --cases")
#   --dry-run    print the stage plan and exit; nothing runs, nothing is written
#   --fixtures DIR  env B: reuse the image files in DIR instead of generating them (in Docker)
#   --slot-dir DIR  env B: directory of the VM slot locks, taken with mkdir around the VMs
#                (default: ${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots})
#   --keep-work  keep the temporary work directory (stage logs, partial results)
#   --update-readme  after the run, regenerate the README status table (report.py readme)
#   --list-stress ENV  print the stress entry points registered for ENV and exit
#
# A stage that cannot start for a missing prerequisite (docker, tart, the golden VM, the image
# ntfs-macos-linux:release, the pinned NTFS-3G tarball) FAILS the stage "preflight", and every later
# stage is recorded as skipped. Nothing is built or downloaded to make up for it: run
# `vm/linux/run.sh build` or `sh scripts/fetch-deps.sh ntfs-3g` yourself.
#
# Stress entry points (T-M5-02..06): a directory tests/stress/NAME with a run.sh is registered for
# the environments named in tests/stress/NAME/ENVS (space separated); crashsweep (T-M3-14) is built
# in as A. It is run as `sh tests/stress/NAME/run.sh --out DIR`, DIR a scratch evidence directory.
# Cases declared with @case under tests/stress are run by the harness stage, not here.
#
# Exit: 0 every stage passed and something was verified (skipped stages do not fail, but a run in
#       which nothing passed is not a success), 1 a stage or a case failed or errored or nothing
#       was verified, 2 usage error or the result could not be written (the work directory is kept),
#       3 every stage passed but the result is marked dirty (the tree was dirty at the start or
#       changed during the run, or a sub-result came from another commit): it prints
#       "PASSED (dirty, not usable for a gate)" and no gate accepts the file (M5-F12). A caller that
#       runs a dirty tree on purpose (ci-local.sh --allow-dirty) must accept 3 itself.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
py=${PYTHON:-python3}

report() { "$py" -B "$root/tests/harness/report.py" "$@"; }

die() {
	printf 'test-all: %s\n' "$1" >&2
	exit "${2:-2}"
}

usage() {
	sed -n '4,50p' "$0" | sed 's/^# \{0,1\}//' >&2
}

# stress_entries ENV: names of the registered tests/stress/* entry points for ENV
stress_entries() {
	for _d in "$root"/tests/stress/*/; do
		[ -f "$_d/run.sh" ] || continue
		_n=$(basename "$_d")
		if [ -f "$_d/ENVS" ]; then
			_e=$(tr -s ' \t\n' '   ' <"$_d/ENVS")
		else
			case $_n in crashsweep) _e=A ;; *) _e= ;; esac
		fi
		case " $_e " in *" $1 "*) printf '%s\n' "$_n" ;; esac
	done
}

env_name=
quick=0
out=
cases=
dry=0
fixtures=
slot_dir=
keep=0
update_readme=0
while [ $# -gt 0 ]; do
	case $1 in
	--env) [ $# -ge 2 ] || die "--env needs A, B or host"; env_name=$2; shift ;;
	--quick) quick=1 ;;
	--out) [ $# -ge 2 ] || die "--out needs a file"; out=$2; shift ;;
	--cases) [ $# -ge 2 ] || die "--cases needs a pattern"; cases=$2; shift ;;
	--dry-run) dry=1 ;;
	--fixtures) [ $# -ge 2 ] || die "--fixtures needs a directory"; fixtures=$2; shift ;;
	--slot-dir) [ $# -ge 2 ] || die "--slot-dir needs a directory"; slot_dir=$2; shift ;;
	--keep-work) keep=1 ;;
	--update-readme) update_readme=1 ;;
	--list-stress) [ $# -ge 2 ] || die "--list-stress needs an environment"; stress_entries "$2"; exit 0 ;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" ;;
	esac
	shift
done
case $env_name in
A | B | host) ;;
*) usage; die "--env A, B or host is required" ;;
esac
case $out in '' | /*) ;; *) out=$(pwd)/$out ;; esac
case $fixtures in '' | /*) ;; *) fixtures=$(pwd)/$fixtures ;; esac
case $slot_dir in '' | /*) ;; *) slot_dir=$(pwd)/$slot_dir ;; esac
if [ "$env_name" = B ] && [ -z "$slot_dir" ]; then
	slot_dir=${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}
fi

n_stage=0
work=
failed=0
blocked=
started=$(date -u +%Y-%m-%dT%H:%M:%SZ)

# --quick without --cases runs only the I-INT-* cases of the harness
hcases=$cases
[ "$quick" -eq 0 ] || [ -n "$cases" ] || hcases='I-INT-*'

# ---- stage machinery -------------------------------------------------------------------------

# record NAME STATUS SECONDS NOTE [LOG]  (tab-separated; tabs and newlines in the note become spaces)
record() {
	_note=$(printf '%s' "$4" | tr '\t\n' '  ')
	printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$_note" "${5:-}" >>"$work/stages.tsv"
}

# skip NAME DESCRIPTION NOTE
note_re=
skip() {
	note_re=
	n_stage=$((n_stage + 1))
	if [ "$dry" -eq 1 ]; then
		printf '%2d  %-22s %s  [%s]\n' "$n_stage" "$1" "$2" "$3"
		return 0
	fi
	printf '\n== stage %s: skipped (%s) ==\n' "$1" "$3" >&2
	record "$1" skip 0 "$3" ""
}

# stage NAME DESCRIPTION CMD...: run CMD (stdout and stderr to the terminal's stderr and to
# logs/NAME.log) and record pass/fail. A set note_re makes the stage's note the last log line it
# matches. Returns the command's status; a failure never stops the run. After a failed preflight
# (blocked is set) every stage is recorded as skipped instead.
stage() {
	_name=$1
	_desc=$2
	shift 2
	if [ -n "$blocked" ]; then
		skip "$_name" "$_desc" "$blocked"
		return 0
	fi
	n_stage=$((n_stage + 1))
	if [ "$dry" -eq 1 ]; then
		printf '%2d  %-22s %s\n' "$n_stage" "$_name" "$_desc"
		return 0
	fi
	printf '\n== stage %s: %s ==\n' "$_name" "$_desc" >&2
	_log=$work/logs/$_name.log
	rm -f "$work/rc"
	_t0=$(date +%s)
	{
		_rc=0
		"$@" 2>&1 </dev/null || _rc=$?
		echo "$_rc" >"$work/rc"
	} | tee "$_log" >&2
	_rc=$(cat "$work/rc" 2>/dev/null) || _rc=
	case $_rc in '' | *[!0-9]*) _rc=255 ;; esac
	_t1=$(date +%s)
	_note=
	if [ -n "$note_re" ]; then _note=$(grep -E "$note_re" "$_log" | tail -n 1 || true); fi
	note_re=
	if [ "$_rc" -eq 0 ]; then
		record "$_name" pass "$((_t1 - _t0))" "$_note" "logs/$_name.log"
	else
		failed=1
		[ -n "$_note" ] || _note=$(grep . "$_log" | tail -n 1 | cut -c1-300 || true)
		record "$_name" fail "$((_t1 - _t0))" "exit $_rc${_note:+; $_note}" "logs/$_name.log"
	fi
	return "$_rc"
}

# only_harness: true when --cases limits the run to the harness stages
only_harness() { [ -n "$cases" ]; }
# heavy NAME DESCRIPTION CMD...: a stage that --quick and --cases leave out
heavy() {
	_n=$1
	_d=$2
	if [ -n "$blocked" ]; then
		skip "$_n" "$_d" "$blocked"
	elif only_harness; then
		skip "$_n" "$_d" "not selected by --cases"
	elif [ "$quick" -eq 1 ]; then
		skip "$_n" "$_d" "skipped by --quick"
	else
		shift 2
		stage "$_n" "$_d" "$@" || true
	fi
}
# light NAME DESCRIPTION CMD...: a stage that only --cases leaves out
light() {
	_n=$1
	_d=$2
	if [ -n "$blocked" ]; then
		skip "$_n" "$_d" "$blocked"
	elif only_harness; then
		skip "$_n" "$_d" "not selected by --cases"
	else
		shift 2
		stage "$_n" "$_d" "$@" || true
	fi
}

run_harness() { # ENV
	if [ -n "$hcases" ]; then
		"$py" -B "$root/tests/harness/run.py" --env "$1" --out "$work/results" --cases "$hcases"
	else
		"$py" -B "$root/tests/harness/run.py" --env "$1" --out "$work/results"
	fi
}

# stress stage(s) for ENV: one stage per registered entry point, or one skipped "none registered"
stress_stages() {
	_entries=$(stress_entries "$1")
	if [ -z "$_entries" ]; then
		skip stress "tests/stress/*/run.sh entry points for $1" "${blocked:-none registered}"
		return 0
	fi
	for _e in $_entries; do
		heavy "stress-$_e" "tests/stress/$_e/run.sh" \
			sh "$root/tests/stress/$_e/run.sh" --out "$work/artifacts/stress-$_e"
	done
}

cleanup_work() {
	if [ -n "$work" ] && [ -d "$work" ]; then
		if [ "$keep" -eq 1 ]; then
			printf 'test-all: work directory kept: %s\n' "$work" >&2
		else
			rm -rf "$work"
		fi
	fi
}

finish() {
	trap '' INT TERM HUP
	_rc=0
	if [ "$dry" -eq 1 ]; then
		printf 'dry run: nothing was run, no result was written\n'
		return 0
	fi
	# HEAD or the tree must still be what the run started with, else the result is marked dirty
	_dirty=$git_dirty
	_gi=$(report git-info 2>/dev/null) || _gi=
	set -- $_gi
	_nsha=${1:-}
	_ndirty=${2:-}
	if [ "$_nsha" != "$git_sha" ]; then
		_dirty=true
		record tree-state pass 0 "HEAD moved or could not be read during the run: result marked dirty" ""
	elif [ "$_ndirty" = true ] && [ "$git_dirty" != true ]; then
		_dirty=true
		record tree-state pass 0 "the working tree became dirty during the run: result marked dirty" ""
	fi
	# the run mode, for the gate (ci-local.sh --verify-result accepts only `full`): case STAGE-<ENV>-MODE
	_mode=full
	[ "$quick" -eq 0 ] || _mode=quick
	if [ -n "$cases" ]; then
		if [ "$quick" -eq 1 ]; then _mode="quick cases=$cases"; else _mode="cases=$cases"; fi
	fi
	record mode pass 0 "$_mode" ""
	set -- aggregate --env "$env_name" --work "$work" --started "$started" --git-sha "$git_sha" --dirty "$_dirty"
	[ ! -d "$work/b" ] || set -- "$@" --work "$work/b"
	[ -z "$out" ] || set -- "$@" --out "$out"
	printf '\n== result ==\n' >&2
	_path=$(report "$@") || _rc=$?
	case $_rc in
	0 | 1 | 3) ;;
	*)
		keep=1
		printf 'test-all: could not write the result file\n' >&2
		cleanup_work
		exit 2
		;;
	esac
	if [ "$update_readme" -eq 1 ]; then report readme >&2 || true; fi
	printf '%s\n' "$_path"
	case $_rc in
	0) printf 'test-all --env %s: PASSED\n' "$env_name" >&2 ;;
	3) printf 'test-all --env %s: PASSED (dirty, not usable for a gate)\n' "$env_name" >&2 ;;
	*) printf 'test-all --env %s: FAILED\n' "$env_name" >&2 ;;
	esac
	cleanup_work
	exit "$_rc"
}

interrupted() {
	trap '' INT TERM HUP
	record interrupted fail 0 "interrupted by a signal" ""
	finish
}

# ---- preflight checks (read-only; nothing is built or downloaded) -----------------------------
preflight_a() {
	docker info >/dev/null 2>&1 || { echo "docker is not reachable"; return 1; }
	docker image inspect ntfs-macos-linux:release >/dev/null 2>&1 ||
		{ echo "image ntfs-macos-linux:release is missing: run 'vm/linux/run.sh build' yourself (test-all never builds or downloads it)"; return 1; }
}

preflight_b() { # QUICK
	command -v tart >/dev/null 2>&1 || { echo "tart is not installed"; return 1; }
	tart list --quiet 2>/dev/null | grep -qx ntfsmac-golden || { echo "the VM ntfsmac-golden does not exist"; return 1; }
	if [ "$1" != 1 ]; then
		docker info >/dev/null 2>&1 || { echo "docker is not reachable (the compat stage and the fixtures need it)"; return 1; }
	fi
	_tb=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
	[ -n "$_tb" ] && [ -f "$root/third_party/cache/$_tb" ] ||
		{ echo "the pinned NTFS-3G tarball is missing: run 'sh scripts/fetch-deps.sh ntfs-3g' yourself (test-all never downloads it)"; return 1; }
}

# ---- the plans -------------------------------------------------------------------------------
cd "$root"
git_sha=
git_dirty=
if [ "$dry" -eq 0 ]; then
	if [ -n "$out" ]; then
		[ ! -e "$out" ] || die "$out exists already; give a new --out name"
		mkdir -p "$(dirname "$out")" || die "cannot create the directory of $out"
	fi
	gi=$(report git-info) || die "cannot read the git state"
	set -- $gi
	git_sha=$1
	git_dirty=$2
	work=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmac-test-all.XXXXXX") || die "cannot create a work directory"
	mkdir -p "$work/logs" "$work/results" "$work/artifacts"
	: >"$work/stages.tsv"
	trap interrupted INT TERM HUP
else
	printf 'test-all plan: env=%s quick=%s cases=%s\n' "$env_name" "$quick" "${cases:--}"
fi

plan_host() {
	light build-release "cmake --preset release; cmake --build --preset release" \
		sh -c 'cmake --preset release && cmake --build --preset release'
	note_re='tests passed|tests failed'
	light unit-release "ctest --preset release (every unit test, harness.unit included)" \
		ctest --preset release
	light product-build "preset product (no test seams): the daemon must build" \
		sh -c 'cmake --preset product && cmake --build --preset product --target ntfsmac_daemon'
	heavy build-asan "cmake --preset asan; cmake --build --preset asan (ASan + UBSan)" \
		sh -c 'cmake --preset asan && cmake --build --preset asan'
	note_re='tests passed|tests failed'
	heavy unit-asan "ctest --preset asan" ctest --preset asan
	skip harness "harness cases of env host" "they drive the VM and the container: they run in --env B"
	stress_stages host
}

plan_a() {
	stage preflight "docker reachable and the image ntfs-macos-linux:release present (read-only)" preflight_a ||
		blocked="preflight failed"
	light selftest "vm/linux/run.sh selftest: mkntfs, mount, write, unmount, remount" \
		sh "$root/vm/linux/run.sh" selftest
	note_re='probe tests: (PASSED|FAILED)'
	heavy unit-probe "tests/unit/probe/run.sh (probe tests, ASan)" \
		sh "$root/tests/unit/probe/run.sh" --out "$work/artifacts/unit-probe"
	heavy unit-volmark "tests/unit/volmark/run.sh (volmark tests, ASan)" \
		sh "$root/tests/unit/volmark/run.sh" --out "$work/artifacts/unit-volmark"
	stage harness "tests/harness/run.py --env A${hcases:+ --cases $hcases}" run_harness A || true
	stress_stages A
}

plan_b() {
	stage preflight "tart, the golden VM, docker and the NTFS-3G tarball are there (read-only checks)" \
		preflight_b "$quick" || blocked="preflight failed"
	fx_ok=0
	if [ -z "$cases" ]; then
		light build-release "cmake --preset release (the daemon and ntfsmac the VM suites install)" \
			sh -c 'cmake --preset release && cmake --build --preset release'
		if [ -n "$blocked" ]; then
			skip fixtures "image files for the daemon suites" "$blocked"
		elif [ -n "$fixtures" ] && [ -d "$fixtures" ]; then
			skip fixtures "image files for the daemon suites" "reusing $fixtures"
			fx_ok=1
		elif [ "$quick" -eq 1 ]; then
			skip fixtures "image files for the daemon suites (tests/fixtures/gen/*.sh --export, in Docker)" "skipped by --quick"
		else
			[ -n "$fixtures" ] || fixtures=${work:-<work>}/fixtures
			if stage fixtures "tests/fixtures/gen/{clean,unsafe,labels,mirror,vmk}.sh --export (in Docker)" \
				sh -c 'for g in clean unsafe labels mirror vmk; do sh "$1/tests/fixtures/gen/$g.sh" --export "$2" || exit 1; done' sh "$root" "$fixtures"; then
				fx_ok=1
			fi
		fi
	fi
	if [ -n "$blocked" ]; then
		for _n in integration compat daemon stress; do skip "$_n" "vm/macos/run-suite.sh stage" "$blocked"; done
		return 0
	fi
	set --
	[ "$dry" -eq 1 ] || set -- --work "$work/b"
	[ "$quick" -eq 0 ] || set -- "$@" --quick
	[ -z "$cases" ] || set -- "$@" --cases "$cases"
	[ "$fx_ok" -eq 0 ] || set -- "$@" --fixtures "$fixtures"
	set -- "$@" --slot-dir "$slot_dir"
	if [ "$dry" -eq 1 ]; then
		printf '    -- vm/macos/run-suite.sh (clones the golden VM, runs, deletes the clone):\n'
		sh "$root/vm/macos/run-suite.sh" --dry-run "$@" | sed 's/^/    /'
		return 0
	fi
	mkdir -p "$work/b"
	rm -f "$work/b/suite.rc"
	sh "$root/vm/macos/run-suite.sh" "$@" 2>&1 </dev/null | tee "$work/logs/vm-suite.log" >&2 || true
	# the exit status of the pipeline's first command is not available in sh: run-suite.sh leaves it in a file
	_rc=$(cat "$work/b/suite.rc" 2>/dev/null) || _rc=
	case $_rc in '' | *[!0-9]*) _rc=2 ;; esac
	if [ "$_rc" -ne 0 ]; then
		failed=1
		# run-suite.sh records its own stages; add one only if it died before it could
		if ! grep -q '	fail	' "$work/b/stages.tsv" 2>/dev/null; then
			record vm-suite fail 0 "run-suite.sh exit $_rc before any stage failed" "logs/vm-suite.log"
		fi
	fi
}

case $env_name in
host) plan_host ;;
A) plan_a ;;
B) plan_b ;;
esac
finish
