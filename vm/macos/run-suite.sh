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
#   guest-build  vm/macos/guest-build.sh: ONE clone that builds NTFS-3G and compiles every cc line of the VM
#                scripts (tests/integration/daemon/*_vm.sh) in the guest, so a missing header or a link
#                error fails in minutes; a failure skips every later stage. Full runs only (not --quick, --cases).
#   integration  tests/harness/run-env-b.sh: the harness cases of env B, run in the guest
#   compat       tests/harness/run-env-b.sh --orchestrate: the cases of env host that drive the guest
#                and the Linux container (C-REF-*, I-BIG-COMP-01, I-NAME-U22, I-XATTR-09)
#   daemon       tests/integration/daemon/dmn_all.sh: the I-DMN-* drivers, attach and Finder suites
#   stress-NAME  tests/stress/NAME/run.sh for every entry point registered for B
#                (scripts/test-all.sh --list-stress B)
#
# Lanes (default 2, --lanes 1 gives the old strictly sequential order): after guest-build the stages run in two
# lanes, each lane one guest at a time, so at most TWO guests exist (each lane holds one VM slot):
#     phase 1  guest-build                             (alone)
#     phase 2  integration, compat   ||   daemon       (about 2000 s ||  1865 s)
#     phase 3  stress-soak           ||   stress-kill, stress-leaks
#     phase 4  stress-parallel       ||   stress-disconnect
#     phase 5  any other registered stress entry, alone
# Soak and parallel are the heavy suites and never run together; each shares the host with a lighter one, and
# kill and disconnect (deadline driven) are never together either. Each stage has its own guest (name with the
# PID of its own script), its own log and artifacts directory; stages.tsv is written in plan order, not in
# completion order. A failing stage never stops the other lane; guest-build failing skips both. When the
# second VM slot is not free (another run holds it) the phase runs its lanes one after the other, never waiting
# for the slot (two runs waiting for each other's slot could never finish). INT/TERM stop every stage of
# both lanes and delete every guest.
#
# Usage: vm/macos/run-suite.sh --work DIR [--quick] [--cases GLOB] [--fixtures DIR] [--lanes 1|2]
#                              [--deps-tar FILE] [--no-guest-build] [--slot-dir DIR] [--dry-run]
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
#   --lanes N       1: every stage one after the other in plan order (debugging, timing comparisons);
#                   2 (default, or NTFSMAC_LANES): two lanes as above; only full runs use the second lane
#   --no-guest-build  do not run the guest-build stage
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
guest_build=1
slot_dir=${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}
dry=0
lanes=${NTFSMAC_LANES:-2}
while [ $# -gt 0 ]; do
	case $1 in
	--work) [ $# -ge 2 ] || die "--work needs a directory"; work=$2; shift ;;
	--quick) quick=1 ;;
	--cases) [ $# -ge 2 ] || die "--cases needs a pattern"; cases=$2; shift ;;
	--fixtures) [ $# -ge 2 ] || die "--fixtures needs a directory"; fixtures=$2; shift ;;
	--deps-tar) [ $# -ge 2 ] || die "--deps-tar needs a file"; deps_tar=$2; shift ;;
	--slot-dir) [ $# -ge 2 ] || die "--slot-dir needs a directory"; slot_dir=$2; shift ;;
	--lanes) [ $# -ge 2 ] || die "--lanes needs 1 or 2"; lanes=$2; shift ;;
	--no-guest-build) guest_build=0 ;;
	--dry-run) dry=1 ;;
	-h | --help) sed -n '4,55p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done
case $lanes in 1 | 2) ;; *) die "--lanes (or NTFSMAC_LANES) must be 1 or 2, not $lanes" ;; esac
[ "$dry" -eq 1 ] || [ -n "$work" ] || die "--work is required"
case $work in '' | /*) ;; *) work=$(pwd)/$work ;; esac
case $fixtures in '' | /*) ;; *) fixtures=$(pwd)/$fixtures ;; esac
case $deps_tar in '' | /*) ;; *) deps_tar=$(pwd)/$deps_tar ;; esac
case $slot_dir in /*) ;; *) slot_dir=$(pwd)/$slot_dir ;; esac

n_stage=0
failed=0
slot=
slot2=
blocked=
ph_conc=0 # 1 while the lanes of a phase run at the same time: their console output is only start and end lines
lane_tag=
terminating=0 # a signal was handled
waiting=0     # 1 while this shell waits for a stage or for the lanes: the stage or phase then records and exits itself

# ---- slot locks (same protocol as dmn_all.sh) ----------------------------------------------------
# grab_slot N: take slot-N when it is free or its holder is gone; 0 on success, 1 when someone else holds it
grab_slot() {
	if mkdir "$slot_dir/slot-$1" 2>/dev/null; then
		echo $$ >"$slot_dir/slot-$1/pid"
		return 0
	fi
	gs_p=$(cat "$slot_dir/slot-$1/pid" 2>/dev/null || true)
	if [ -n "$gs_p" ] && ! kill -0 "$gs_p" 2>/dev/null; then
		echo "run-suite: removing the stale slot-$1 (holder $gs_p is gone)" >&2
		rm -f "$slot_dir/slot-$1/pid"
		rmdir "$slot_dir/slot-$1" 2>/dev/null || true
		if mkdir "$slot_dir/slot-$1" 2>/dev/null; then
			echo $$ >"$slot_dir/slot-$1/pid"
			return 0
		fi
	fi
	return 1
}
# take_slot: wait for a first slot (the run holds it from the preflight to the end)
take_slot() {
	mkdir -p "$slot_dir"
	while [ -z "$slot" ]; do
		for ts_s in ${NTFSMAC_VM_SLOTS:-1 2}; do
			if grab_slot "$ts_s"; then
				slot=$ts_s
				break
			fi
		done
		[ -n "$slot" ] || sleep "${NTFSMAC_SLOT_WAIT:-30}"
		[ "$terminating" -eq 0 ] || exit 130
	done
	echo "run-suite: holding slot-$slot" >&2
}
# try_second_slot: the slot of the second lane. NEVER waits: two runs that each hold one slot and wait for another
# would never finish. 0 when this run holds a second slot now, 1 when it does not (the lanes then run one after the other)
try_second_slot() {
	[ -z "$slot2" ] || return 0
	for t2_s in ${NTFSMAC_VM_SLOTS:-1 2}; do
		[ "$t2_s" != "$slot" ] || continue
		if grab_slot "$t2_s"; then
			slot2=$t2_s
			echo "run-suite: holding slot-$slot2 as well (second lane)" >&2
			return 0
		fi
	done
	return 1
}
release_second_slot() {
	if [ -n "$slot2" ]; then
		rm -f "$slot_dir/slot-$slot2/pid"
		rmdir "$slot_dir/slot-$slot2" 2>/dev/null || true
	fi
	slot2=
}
release_slot() {
	release_second_slot
	if [ -n "$slot" ]; then
		rm -f "$slot_dir/slot-$slot/pid"
		rmdir "$slot_dir/slot-$slot" 2>/dev/null || true
	fi
	slot=
}

# ---- records: one file per stage, merged into stages.tsv in plan order --------------------------------
# Lanes run in subshells, so a stage cannot append to the shared stages.tsv in completion order (and its
# variables are lost): record() writes rec/NAME.tsv and flush_records() appends the files in the order of
# plan_names (then any other name), sets failed when a stage failed, and removes them.
plan_names=
flush_records() {
	[ -d "$work/rec" ] || return 0
	for fl_n in $plan_names; do
		fl_f=$work/rec/$fl_n.tsv
		[ -f "$fl_f" ] || continue
		cat "$fl_f" >>"$work/stages.tsv"
		rm -f "$fl_f"
	done
	for fl_f in "$work"/rec/*.tsv; do
		[ -f "$fl_f" ] || continue
		cat "$fl_f" >>"$work/stages.tsv"
		rm -f "$fl_f"
	done
	if awk -F '\t' '$2 == "fail" { f = 1 } END { exit !f }' "$work/stages.tsv"; then failed=1; fi
}

record() { # NAME STATUS SECONDS NOTE [LOG]
	rc_note=$(printf '%s' "$4" | tr '\t\n' '  ')
	printf '%s\t%s\t%s\t%s\t%s\n' "$1" "$2" "$3" "$rc_note" "${5:-}" >>"$work/rec/$1.tsv"
}

# ---- stopping everything (INT/TERM/HUP) -------------------------------------------------------------------
# tree_of PID...: tree_all = the PIDs and all their descendants
tree_of() {
	tree_all=$*
	tree_new=$*
	while [ -n "$tree_new" ]; do
		tree_next=
		for tree_p in $tree_new; do
			tree_next="$tree_next $(pgrep -P "$tree_p" 2>/dev/null | tr '\n' ' ')"
		done
		tree_next=$(echo $tree_next)
		tree_all="$tree_all $tree_next"
		tree_new=$tree_next
	done
}
# stage_roots: sr_roots = the PIDs of the commands of every running stage (their pid files)
stage_roots() {
	sr_roots=
	for sr_f in "$work"/pid/*; do
		[ -f "$sr_f" ] || continue
		sr_roots="$sr_roots $(cat "$sr_f" 2>/dev/null || true)"
	done
}
# gone_wait SECONDS PID...: 0 when none of the PIDs is alive any more, 1 after SECONDS
gone_wait() {
	gw_max=$1
	shift
	gw_i=0
	while [ "$gw_i" -lt "$gw_max" ]; do
		gw_any=0
		for gw_p in "$@"; do
			if kill -0 "$gw_p" 2>/dev/null; then
				gw_any=1
				break
			fi
		done
		[ "$gw_any" -eq 1 ] || return 0
		sleep 1
		gw_i=$((gw_i + 1))
	done
	return 1
}
# sweep_clones PID...: delete the guests named ntfsmac-*-PID of the given processes (every script names its guest after its
# own PID): a last resort when a script was killed before its own cleanup ran. Golden VMs never match.
sweep_clones() {
	command -v tart >/dev/null 2>&1 || return 0
	sw_list=$(tart list --quiet 2>/dev/null) || return 0
	for sw_p in "$@"; do
		for sw_v in $(printf '%s\n' "$sw_list" | grep -E "^ntfsmac-[a-z0-9-]+-$sw_p\$" | grep -v golden || true); do
			echo "run-suite: deleting the guest $sw_v that its script left behind" >&2
			sh "$here/tart.sh" delete "$sw_v" >&2 || echo "run-suite: WARNING: could not delete $sw_v" >&2
		done
	done
}
# stop_stages: TERM to every running stage command and all its descendants (their own traps delete their guests), wait for
# them to be gone, and only then return; the lane subshells see the stop file and start nothing more
stop_stages() {
	: >"$work/stop"
	ss_all=
	ss_seen=
	for ss_round in 1 2; do
		stage_roots
		# the second round signals only the stages that were not there in the first (not the cleanup of the others)
		ss_new=
		for ss_r in $sr_roots; do
			case " $ss_seen " in *" $ss_r "*) ;; *) ss_new="$ss_new $ss_r" ;; esac
		done
		ss_seen="$ss_seen $ss_new"
		tree_all=
		# shellcheck disable=SC2086
		[ -z "$ss_new" ] || tree_of $ss_new
		if [ -n "$tree_all" ]; then
			# shellcheck disable=SC2086
			kill -TERM $tree_all 2>/dev/null || true
			ss_all="$ss_all $tree_all"
		fi
		# a stage that was starting while the first round looked is found by the second
		[ "$ss_round" -eq 2 ] || sleep 1
	done
	# shellcheck disable=SC2086
	if ! gone_wait "${NTFSMAC_TERM_WAIT:-300}" $ss_all; then
		echo "run-suite: a stage did not stop in ${NTFSMAC_TERM_WAIT:-300} s: killing it" >&2
		# shellcheck disable=SC2086
		kill -KILL $ss_all 2>/dev/null || true
		sleep 1
	fi
	# shellcheck disable=SC2086
	sweep_clones $ss_all
	wait || true
}

on_exit() {
	oe_rc=$?
	trap '' INT TERM HUP
	[ "$dry" -eq 1 ] || flush_records
	release_slot
	[ "$dry" -eq 1 ] || echo "$oe_rc" >"$work/suite.rc"
}
# The signal is handled at once (stages run in the background, see stage()): every stage is stopped and gone before
# this returns. A stage or phase that was waiting then records its failure and exits 130 itself; anywhere else exit now.
on_signal() {
	trap '' INT TERM HUP
	terminating=1
	echo "run-suite: stopping every stage of both lanes and deleting their guests" >&2
	if [ "$dry" -eq 0 ]; then stop_stages; fi
	[ "$waiting" -eq 1 ] || exit 130
}
trap on_exit EXIT
trap on_signal INT TERM HUP

# ---- stage machinery (same stages.tsv format as scripts/test-all.sh) -----------------------------
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

# stage_block CMD...: runs in the background of the stage: the command's PID and exit status go to files (the PID is how
# a signal finds the command, see stop_stages)
stage_block() {
	"$@" 2>&1 </dev/null &
	sb_pid=$!
	echo "$sb_pid" >"$stg_pidf"
	# a signal that came before the pid file existed has not seen this command: stop it here
	if [ -e "$work/stop" ]; then kill -TERM "$sb_pid" 2>/dev/null || true; fi
	sb_rc=0
	wait "$sb_pid" || sb_rc=$?
	echo "$sb_rc" >"$stg_rcf"
}

stage() { # NAME DESCRIPTION CMD...   (NOTE_FILE, when set, names a file whose content becomes the note)
	stg_name=$1
	stg_desc=$2
	shift 2
	if [ -n "$blocked" ]; then
		skip "$stg_name" "$stg_desc" "$blocked"
		return 0
	fi
	n_stage=$((n_stage + 1))
	if [ "$dry" -eq 1 ]; then
		printf '%2d  %-22s %s\n' "$n_stage" "$stg_name" "$stg_desc"
		return 0
	fi
	printf '\n== [%s] stage %s: start%s: %s ==\n' "$(date +%H:%M:%S)" "$stg_name" "$lane_tag" "$stg_desc" >&2
	stg_log=$work/logs/$stg_name.log
	stg_rcf=$work/rc/$stg_name
	stg_pidf=$work/pid/$stg_name
	rm -f "$stg_rcf" "$stg_pidf"
	stg_t0=$(date +%s)
	if [ -e "$work/stop" ]; then # a signal arrived while this stage was being prepared: it is not started
		failed=1
		record "$stg_name" fail 0 "stopped by a signal before it started" ""
		return 0
	fi
	# The stage runs in the background and this shell waits for it, so that a signal is handled at once (a shell runs its trap
	# only after a foreground command ends). Concurrent lanes write their log file only; a lone stage also shows its output.
	if [ "$ph_conc" -eq 1 ]; then
		stage_block "$@" >"$stg_log" 2>&1 &
	else
		{ stage_block "$@" 2>&1 | tee "$stg_log" >&2; } &
	fi
	stg_bg=$!
	waiting=1
	wait "$stg_bg" || true
	waiting=0
	rm -f "$stg_pidf"
	stg_rc=$(cat "$stg_rcf" 2>/dev/null) || stg_rc=
	case $stg_rc in '' | *[!0-9]*) stg_rc=255 ;; esac
	stg_t1=$(date +%s)
	stg_note=
	if [ -n "${NOTE_FILE:-}" ] && [ -f "$NOTE_FILE" ]; then stg_note=$(tr '\n' ';' <"$NOTE_FILE" | sed 's/;$//; s/;/; /g'); fi
	NOTE_FILE=
	if [ "$stg_rc" -eq 0 ]; then
		record "$stg_name" pass "$((stg_t1 - stg_t0))" "$stg_note" "logs/$stg_name.log"
		printf '== [%s] stage %s: pass in %s s ==\n' "$(date +%H:%M:%S)" "$stg_name" "$((stg_t1 - stg_t0))" >&2
	else
		failed=1
		stg_note=$(grep . "$stg_log" | tail -n 1 | cut -c1-300 || true)
		record "$stg_name" fail "$((stg_t1 - stg_t0))" "exit $stg_rc${stg_note:+; $stg_note}" "logs/$stg_name.log"
		if [ "$ph_conc" -eq 1 ]; then
			printf -- '-- last lines of logs/%s.log:\n' "$stg_name" >&2
			tail -n 15 "$stg_log" >&2 || true
		fi
		printf '== [%s] stage %s: FAILED (exit %s) in %s s ==\n' "$(date +%H:%M:%S)" "$stg_name" "$stg_rc" "$((stg_t1 - stg_t0))" >&2
	fi
	[ "$terminating" -eq 0 ] || exit 130
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
	hm_errf=$work/list.$hs_name.err
	hm_out=$("$py" -B "$root/tests/harness/run.py" --env "$1" --list --cases "$2" 2>"$hm_errf") || {
		hm_err="case discovery failed (run.py --env $1 --list --cases $2): $(tail -n 1 "$hm_errf")"
		return 1
	}
	hm_count=$(printf '%s\n' "$hm_out" | grep -c . || true)
	return 0
}

# harness_stage NAME ENV PATTERNS DESCRIPTION CMD...: run a harness stage unless discovery fails or selects nothing
harness_stage() {
	hs_name=$1
	hs_env=$2
	hs_pat=$3
	hs_desc=$4
	shift 4
	if [ -n "$blocked" ]; then
		skip "$hs_name" "$hs_desc" "$blocked"
	elif [ "$dry" -eq 0 ] && [ -n "$hs_pat" ] && ! harness_count "$hs_env" "$hs_pat"; then
		fail_stage "$hs_name" "$hm_err"
	elif [ "$dry" -eq 0 ] && [ -n "$hs_pat" ] && [ "$hm_count" -eq 0 ]; then
		skip "$hs_name" "$hs_desc" "no case of env $hs_env matches $hs_pat"
	else
		stage "$hs_name" "$hs_desc" "$@"
	fi
}

NOTE_FILE=

# ---- the stages of the plan --------------------------------------------------------------------
# guest-build: one clone builds NTFS-3G and compiles every cc line of the VM scripts (a failure skips the rest, which
# would run for an hour before the same error showed). Full runs only: --quick and --cases select harness cases.
st_guest_build() {
	if [ "$guest_build" -eq 0 ]; then
		skip guest-build "vm/macos/guest-build.sh" "skipped by --no-guest-build"
	elif [ -n "$blocked" ]; then
		skip guest-build "vm/macos/guest-build.sh" "$blocked"
	elif [ "$quick" -eq 1 ] || [ -n "$cases" ]; then
		skip guest-build "vm/macos/guest-build.sh" "not run with --quick or --cases"
	else
		stage guest-build "vm/macos/guest-build.sh (one clone: build NTFS-3G, compile every cc line of the VM scripts)" \
			sh "$here/guest-build.sh" --out-dir "$work/artifacts/guest-build"
		if [ "$dry" -eq 0 ]; then
			flush_records
			if [ "$(awk -F '\t' '$1 == "guest-build" { s = $2 } END { print s }' "$work/stages.tsv")" != pass ]; then
				blocked="guest-build failed"
			fi
		fi
	fi
}

# integration (harness cases of env B)
st_integration() {
	if [ -n "$cases" ]; then si_cases=$cases; elif [ "$quick" -eq 1 ]; then si_cases='I-INT-*'; else si_cases=; fi
	if [ -n "$si_cases" ]; then
		harness_stage integration B "$si_cases" \
			"tests/harness/run-env-b.sh --cases $si_cases (clone, build NTFS-3G in the guest, run, delete)" \
			sh "$root/tests/harness/run-env-b.sh" --out "$work/results" --cases "$si_cases"
	else
		harness_stage integration B "" \
			"tests/harness/run-env-b.sh (clone, build NTFS-3G in the guest, run, delete)" \
			sh "$root/tests/harness/run-env-b.sh" --out "$work/results"
	fi
}

# compat (cases of env host that drive guest and container)
st_compat() {
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
}

# daemon suites
st_daemon() {
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
}

# stress entry E (tests/stress/E/run.sh), registered for B
st_stress() {
	if [ -n "$blocked" ]; then
		skip "stress-$1" "tests/stress/$1/run.sh" "$blocked"
	elif [ -n "$cases" ]; then
		skip "stress-$1" "tests/stress/$1/run.sh" "not selected by --cases"
	elif [ "$quick" -eq 1 ]; then
		skip "stress-$1" "tests/stress/$1/run.sh" "skipped by --quick"
	else
		stage "stress-$1" "tests/stress/$1/run.sh --out DIR" \
			sh "$root/tests/stress/$1/run.sh" --out "$work/artifacts/stress-$1"
	fi
}

# run_one NAME: the stage of the plan with that name (it runs, is skipped or fails; it records itself)
run_one() {
	case $1 in
	guest-build) st_guest_build ;;
	integration) st_integration ;;
	compat) st_compat ;;
	daemon) st_daemon ;;
	stress) skip stress "tests/stress/*/run.sh entry points for B" "${blocked:-none registered}" ;;
	stress-*) st_stress "${1#stress-}" ;;
	*) die "unknown stage $1" ;;
	esac
}

# ---- the plan and its lanes -----------------------------------------------------------------------
entries=$(sh "$root/scripts/test-all.sh" --list-stress B)
entries=$(echo $entries) # one line, single spaces
plan_names="guest-build integration compat daemon"
if [ -z "$entries" ]; then
	plan_names="$plan_names stress"
else
	for e in $entries; do plan_names="$plan_names stress-$e"; done
fi

# run_lane LANE NAME...: the stages of one lane, one after the other, until a signal stops the run
run_lane() {
	rl_lane=$1
	shift
	lane_tag=
	[ "$ph_conc" -eq 0 ] || lane_tag=" (lane $rl_lane)"
	for rl_name in "$@"; do
		[ ! -e "$work/stop" ] || break
		run_one "$rl_name"
	done
}

# run_phase "NAMES of lane 1" "NAMES of lane 2": both lanes at the same time when this run can hold a second slot (a
# lane is a subshell: it cannot change variables here, so failures are read back from the records), else one after the other
run_phase() {
	rp_one=$1
	rp_two=$2
	[ -n "$rp_one$rp_two" ] || return 0
	if [ -n "$rp_one" ] && [ -n "$rp_two" ] && try_second_slot; then
		ph_conc=1
		printf '\n== lanes: [%s] || [%s] ==\n' "$rp_one" "$rp_two" >&2
		# shellcheck disable=SC2086
		(run_lane 1 $rp_one) &
		rp_p1=$!
		# shellcheck disable=SC2086
		(run_lane 2 $rp_two) &
		rp_p2=$!
		waiting=1
		rp_s1=0
		rp_s2=0
		wait "$rp_p1" || rp_s1=$?
		wait "$rp_p2" || rp_s2=$?
		waiting=0
		[ "$terminating" -eq 0 ] || exit 130
		ph_conc=0
		lane_tag=
		release_second_slot # taken again by the next concurrent phase, if it is free then
		# a lane that crashed (a failing command under set -e) left its remaining stages unrecorded: that is a failure
		[ "$rp_s1" -eq 0 ] || { fail_stage lane-1 "lane 1 ended with status $rp_s1"; }
		[ "$rp_s2" -eq 0 ] || { fail_stage lane-2 "lane 2 ended with status $rp_s2"; }
		for rp_n in $rp_one $rp_two; do
			[ -f "$work/rec/$rp_n.tsv" ] || fail_stage "$rp_n" "not run (a lane ended before it)"
		done
	else
		if [ -n "$rp_one" ] && [ -n "$rp_two" ]; then
			echo "run-suite: no second VM slot is free: the lanes of this phase run one after the other" >&2
		fi
		# shellcheck disable=SC2086
		run_lane 1 $rp_one $rp_two
	fi
}

# stress_pick ENTRY...: sp_out = the stages of those of the ENTRYs that are registered for B
stress_pick() {
	sp_out=
	for sp_e in "$@"; do
		case " $entries " in *" $sp_e "*) sp_out="$sp_out stress-$sp_e" ;; esac
	done
	sp_out=$(echo $sp_out)
}

if [ "$dry" -eq 1 ]; then
	for pn in $plan_names; do run_one "$pn"; done
	exit 0
fi

mkdir -p "$work/logs" "$work/results" "$work/artifacts" "$work/rec" "$work/rc" "$work/pid"
[ -f "$work/stages.tsv" ] || : >"$work/stages.tsv"
rm -f "$work/suite.rc" "$work/stop"
echo $$ >"$work/suite.pid"
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
	flush_records
else
	take_slot
fi

# phase 1: guest-build, alone (a failure blocks every later stage of both lanes)
run_one guest-build
flush_records

if [ "$lanes" -ge 2 ] && [ "$quick" -eq 0 ] && [ -z "$cases" ] && [ -z "$blocked" ]; then
	# phase 2: the long functional suites, about 2000 s || 1865 s
	run_phase "integration compat" "daemon"
	flush_records
	if [ -z "$entries" ]; then
		run_one stress
	else
		# phases 3 and 4: the heavy suites (soak, parallel) never run together; each one shares the host with lighter ones, and
		# the deadline driven kill and disconnect are never together. Entries this list does not know run alone afterwards.
		stress_pick soak
		s3a=$sp_out
		stress_pick kill leaks
		s3b=$sp_out
		stress_pick parallel
		s4a=$sp_out
		stress_pick disconnect
		s4b=$sp_out
		s5=
		for e in $entries; do
			case $e in soak | kill | leaks | parallel | disconnect) ;; *) s5="$s5 stress-$e" ;; esac
		done
		# kill, disconnect and soak/parallel each run `cmake --build` of the release tree first: do it once here, so that two
		# of them never build at the same time
		if [ -f "$root/build/release/CMakeCache.txt" ]; then
			cmake --build "$root/build/release" >"$work/logs/stress-prebuild.log" 2>&1 ||
				echo "run-suite: cmake --build build/release failed (logs/stress-prebuild.log): the stress suites will report it" >&2
		fi
		run_phase "$s3a" "$s3b"
		run_phase "$s4a" "$s4b"
		run_phase "$(echo $s5)" ""
	fi
	flush_records
else
	# one lane: every stage in plan order
	for pn in $plan_names; do
		[ "$pn" != guest-build ] || continue
		run_one "$pn"
	done
	flush_records
fi

[ "$failed" -eq 0 ] || exit 1
exit 0
