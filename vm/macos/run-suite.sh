#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The Environment B side of scripts/test-all.sh (T-M5-01): every suite that needs the macOS VM,
# in order. Each guest is a disposable clone of the golden VM, made, used and deleted by the
# script that owns it (tests/harness/run-env-b.sh, tests/integration/daemon/dmn_all.sh, a stress
# entry point), so this script reuses them and duplicates none of their VM handling. The golden
# VMs (ntfsmac-golden, ntfsmac-golden-finder) are only cloned, never started or changed. Nothing
# is mounted on the host; every NTFS mount is inside a guest or the Linux container.
#
# Stages (all recorded in WORK/stages.tsv, the format tests/harness/report.py aggregate reads):
#   integration  tests/harness/run-env-b.sh: the harness cases of env B, run in the guest
#   compat       tests/harness/run-env-b.sh --orchestrate: the cases of env host that drive the guest
#                and the Linux container (C-REF-*, I-BIG-COMP-01, I-NAME-U22, I-XATTR-09)
#   daemon       tests/integration/daemon/dmn_all.sh: the I-DMN-* drivers, attach and Finder suites
#   stress-NAME  tests/stress/NAME/run.sh for every entry point registered for B
#                (scripts/test-all.sh --list-stress B)
#
# Usage: vm/macos/run-suite.sh --work DIR [--quick] [--cases GLOB] [--fixtures DIR]
#                              [--deps-tar FILE] [--slot-dir DIR] [--dry-run]
#   --work DIR      work directory: stages.tsv, logs/, results/ (harness result files and evidence),
#                   artifacts/<stage>/, and suite.rc (this script's exit status)
#   --quick         only the harness cases of env B (default I-INT-*); no compat, daemon or stress
#   --cases GLOB    comma-separated case ids or fnmatch patterns for the two harness stages; daemon
#                   and stress stages are then skipped
#   --fixtures DIR  the image files for the daemon suites (tests/fixtures/gen/*.sh --export DIR);
#                   without it the daemon stage is skipped
#   --deps-tar FILE a tar of the guest's NTFS-3G build, passed to the daemon suites (they save it
#                   there when it does not exist)
#   --slot-dir DIR  directory of the VM slot locks: slot-N is taken with mkdir for the whole run
#                   (a `pid` file names the holder; a lock whose holder is gone is removed). The
#                   slots are those of NTFSMAC_VM_SLOTS (default "1 2"). Default directory:
#                   ${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}, so a run always takes a slot.
#   --dry-run       print the stage plan and exit
# A failed read-only check (tart, ntfsmac-golden, docker) records the stage vm-preflight as failed and
# every other stage as skipped. A harness case selection that cannot be resolved (run.py --list fails)
# fails its stage; it is never turned into a skip.
# Exit: 0 every stage passed or was skipped, 1 a stage failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
py=${PYTHON:-python3}

die() {
	printf 'run-suite: %s\n' "$1" >&2
	exit 2
}

work=
quick=0
cases=
fixtures=
deps_tar=
slot_dir=${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}
dry=0
while [ $# -gt 0 ]; do
	case $1 in
	--work) [ $# -ge 2 ] || die "--work needs a directory"; work=$2; shift ;;
	--quick) quick=1 ;;
	--cases) [ $# -ge 2 ] || die "--cases needs a pattern"; cases=$2; shift ;;
	--fixtures) [ $# -ge 2 ] || die "--fixtures needs a directory"; fixtures=$2; shift ;;
	--deps-tar) [ $# -ge 2 ] || die "--deps-tar needs a file"; deps_tar=$2; shift ;;
	--slot-dir) [ $# -ge 2 ] || die "--slot-dir needs a directory"; slot_dir=$2; shift ;;
	--dry-run) dry=1 ;;
	-h | --help) sed -n '4,40p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done
[ "$dry" -eq 1 ] || [ -n "$work" ] || die "--work is required"
case $work in '' | /*) ;; *) work=$(pwd)/$work ;; esac
case $fixtures in '' | /*) ;; *) fixtures=$(pwd)/$fixtures ;; esac
case $deps_tar in '' | /*) ;; *) deps_tar=$(pwd)/$deps_tar ;; esac
case $slot_dir in /*) ;; *) slot_dir=$(pwd)/$slot_dir ;; esac

n_stage=0
failed=0
slot=
blocked=

# ---- slot lock (same protocol as dmn_all.sh) ---------------------------------------------------
take_slot() {
	mkdir -p "$slot_dir"
	while [ -z "$slot" ]; do
		for _s in ${NTFSMAC_VM_SLOTS:-1 2}; do
			if mkdir "$slot_dir/slot-$_s" 2>/dev/null; then
				echo $$ >"$slot_dir/slot-$_s/pid"
				slot=$_s
				break
			fi
			_p=$(cat "$slot_dir/slot-$_s/pid" 2>/dev/null || true)
			if [ -n "$_p" ] && ! kill -0 "$_p" 2>/dev/null; then
				echo "run-suite: removing the stale slot-$_s (holder $_p is gone)" >&2
				rm -f "$slot_dir/slot-$_s/pid"
				rmdir "$slot_dir/slot-$_s" 2>/dev/null || true
			fi
		done
		[ -n "$slot" ] || sleep "${NTFSMAC_SLOT_WAIT:-30}"
	done
	echo "run-suite: holding slot-$slot" >&2
}
release_slot() {
	if [ -n "$slot" ]; then
		rm -f "$slot_dir/slot-$slot/pid"
		rmdir "$slot_dir/slot-$slot" 2>/dev/null || true
	fi
	slot=
}

on_exit() {
	_rc=$?
	release_slot
	[ "$dry" -eq 1 ] || echo "$_rc" >"$work/suite.rc"
}
trap on_exit EXIT
trap 'exit 130' INT TERM HUP

# ---- stage machinery (same stages.tsv format as scripts/test-all.sh) -----------------------------
record() { # NAME STATUS SECONDS NOTE [LOG]
	_note=$(printf '%s' "$4" | tr '\t\n' '  ')
	printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$_note" "${5:-}" >>"$work/stages.tsv"
}

skip() { # NAME DESCRIPTION NOTE
	n_stage=$((n_stage + 1))
	if [ "$dry" -eq 1 ]; then
		printf '%2d  %-22s %s  [%s]\n' "$n_stage" "$1" "$2" "$3"
		return 0
	fi
	printf '\n== stage %s: skipped (%s) ==\n' "$1" "$3" >&2
	record "$1" skip 0 "$3" ""
}

fail_stage() { # NAME NOTE: a stage that could not even start
	n_stage=$((n_stage + 1))
	printf '\n== stage %s: FAILED (%s) ==\n' "$1" "$2" >&2
	record "$1" fail 0 "$2" ""
	failed=1
}

stage() { # NAME DESCRIPTION CMD...   (NOTE_FILE, when set, names a file whose content becomes the note)
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
	if [ -n "${NOTE_FILE:-}" ] && [ -f "$NOTE_FILE" ]; then _note=$(tr '\n' ';' <"$NOTE_FILE" | sed 's/;$//; s/;/; /g'); fi
	NOTE_FILE=
	if [ "$_rc" -eq 0 ]; then
		record "$_name" pass "$((_t1 - _t0))" "$_note" "logs/$_name.log"
	else
		failed=1
		_note=$(grep . "$_log" | tail -n 1 | cut -c1-300 || true)
		record "$_name" fail "$((_t1 - _t0))" "exit $_rc${_note:+; $_note}" "logs/$_name.log"
	fi
	return 0
}

# harness_count ENV PATTERNS: sets hm_count to the number of cases of that env the patterns select.
# Returns 1 with hm_err set when run.py --list itself fails (unknown pattern, import error, ...):
# that is a failure of the stage, never a reason to skip it.
hm_count=0
hm_err=
harness_count() {
	hm_err=
	hm_count=0
	_o=$("$py" -B "$root/tests/harness/run.py" --env "$1" --list --cases "$2" 2>"$work/list.err") || {
		hm_err="case discovery failed (run.py --env $1 --list --cases $2): $(tail -n 1 "$work/list.err")"
		return 1
	}
	hm_count=$(printf '%s\n' "$_o" | grep -c . || true)
	return 0
}

# harness_stage NAME ENV PATTERNS DESCRIPTION CMD...: run a harness stage unless discovery fails or selects nothing
harness_stage() {
	_hn=$1
	_he=$2
	_hp=$3
	_hd=$4
	shift 4
	if [ -n "$blocked" ]; then
		skip "$_hn" "$_hd" "$blocked"
	elif [ "$dry" -eq 0 ] && [ -n "$_hp" ] && ! harness_count "$_he" "$_hp"; then
		fail_stage "$_hn" "$hm_err"
	elif [ "$dry" -eq 0 ] && [ -n "$_hp" ] && [ "$hm_count" -eq 0 ]; then
		skip "$_hn" "$_hd" "no case of env $_he matches $_hp"
	else
		stage "$_hn" "$_hd" "$@"
	fi
}

NOTE_FILE=

# ---- the plan ----------------------------------------------------------------------------------
if [ "$dry" -eq 0 ]; then
	mkdir -p "$work/logs" "$work/results" "$work/artifacts"
	[ -f "$work/stages.tsv" ] || : >"$work/stages.tsv"
	rm -f "$work/suite.rc"
	# Read-only checks first: no VM is started and no slot taken when something is missing.
	pre=
	command -v tart >/dev/null 2>&1 || pre="tart is not installed"
	if [ -z "$pre" ] && ! tart list --quiet 2>/dev/null | grep -qx ntfsmac-golden; then pre="the VM ntfsmac-golden does not exist"; fi
	if [ -z "$pre" ] && [ "$quick" -eq 0 ] && ! docker info >/dev/null 2>&1; then
		pre="docker is not reachable (the compat stage and the fixtures need it)"
	fi
	if [ -n "$pre" ]; then
		fail_stage vm-preflight "$pre"
		blocked="vm-preflight failed"
	else
		take_slot
	fi
fi

# integration (harness cases of env B)
if [ -n "$cases" ]; then icases=$cases; elif [ "$quick" -eq 1 ]; then icases='I-INT-*'; else icases=; fi
if [ -n "$icases" ]; then
	harness_stage integration B "$icases" \
		"tests/harness/run-env-b.sh --cases $icases (clone, build NTFS-3G in the guest, run, delete)" \
		sh "$root/tests/harness/run-env-b.sh" --out "$work/results" --cases "$icases"
else
	harness_stage integration B "" \
		"tests/harness/run-env-b.sh (clone, build NTFS-3G in the guest, run, delete)" \
		sh "$root/tests/harness/run-env-b.sh" --out "$work/results"
fi

# compat (cases of env host that drive guest and container)
if [ -n "$blocked" ]; then
	skip compat "host-side reference comparison" "$blocked"
elif [ "$quick" -eq 1 ] && [ -z "$cases" ]; then
	skip compat "host-side reference comparison" "skipped by --quick"
elif [ -n "$cases" ]; then
	harness_stage compat host "$cases" \
		"tests/harness/run-env-b.sh --orchestrate --cases $cases (C-REF-*: guest vs Linux container)" \
		sh "$root/tests/harness/run-env-b.sh" --orchestrate --out "$work/results" --cases "$cases"
else
	harness_stage compat host "" \
		"tests/harness/run-env-b.sh --orchestrate (C-REF-*: guest vs Linux container)" \
		sh "$root/tests/harness/run-env-b.sh" --orchestrate --out "$work/results"
fi

# daemon suites
if [ -n "$blocked" ]; then
	skip daemon "tests/integration/daemon/dmn_all.sh" "$blocked"
elif [ -n "$cases" ]; then
	skip daemon "tests/integration/daemon/dmn_all.sh" "not selected by --cases"
elif [ "$quick" -eq 1 ]; then
	skip daemon "tests/integration/daemon/dmn_all.sh" "skipped by --quick"
elif [ -z "$fixtures" ]; then
	skip daemon "tests/integration/daemon/dmn_all.sh" "no --fixtures directory"
elif [ "$dry" -eq 0 ] && [ ! -f "$root/build/release/src/daemon/ntfsmacd" ]; then
	fail_stage daemon "build/release/src/daemon/ntfsmacd is missing: cmake --build --preset release"
else
	NOTE_FILE=$work/artifacts/daemon/summary.txt
	if [ -n "$deps_tar" ]; then
		stage daemon "tests/integration/daemon/dmn_all.sh --fixtures DIR (every I-DMN driver, one guest each)" \
			sh "$root/tests/integration/daemon/dmn_all.sh" --fixtures "$fixtures" --out-dir "$work/artifacts/daemon" --deps-tar "$deps_tar"
	else
		stage daemon "tests/integration/daemon/dmn_all.sh --fixtures DIR (every I-DMN driver, one guest each)" \
			sh "$root/tests/integration/daemon/dmn_all.sh" --fixtures "$fixtures" --out-dir "$work/artifacts/daemon"
	fi
fi

# stress entry points registered for B
entries=$(sh "$root/scripts/test-all.sh" --list-stress B)
if [ -z "$entries" ]; then
	skip stress "tests/stress/*/run.sh entry points for B" "${blocked:-none registered}"
else
	for e in $entries; do
		if [ -n "$blocked" ]; then
			skip "stress-$e" "tests/stress/$e/run.sh" "$blocked"
		elif [ -n "$cases" ]; then
			skip "stress-$e" "tests/stress/$e/run.sh" "not selected by --cases"
		elif [ "$quick" -eq 1 ]; then
			skip "stress-$e" "tests/stress/$e/run.sh" "skipped by --quick"
		else
			stage "stress-$e" "tests/stress/$e/run.sh --out DIR" \
				sh "$root/tests/stress/$e/run.sh" --out "$work/artifacts/stress-$e"
		fi
	done
fi

[ "$dry" -eq 1 ] && exit 0
[ "$failed" -eq 0 ] || exit 1
exit 0
