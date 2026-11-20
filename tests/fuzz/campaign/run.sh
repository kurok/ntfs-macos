#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - the extended fuzz campaign (T-M5-09, M5 criterion: 8 CPU-hours of fuzzing, every crash classified)
# in Environment A (vm/linux/run.sh: container with the pinned toolchain, repo mounted read-only; nothing on
# the host, no sudo). The container is not network-isolated; /work is mounted read-only, so nothing a unit
# downloads could be written into the repository, and the build needs no download (the tarball is cached).
#
# Usage: tests/fuzz/campaign/run.sh [--budget-hours H | --budget-seconds S] [--jobs N] [--targets LIST]
#                                   [--shards LIST] [--no-patches] [--adopt] [--out DIR] [--dry-run]
#   --budget-hours H     total CPU budget over all units (default 8; fractions allowed)
#   --budget-seconds S   the same in seconds (smoke runs); overrides --budget-hours
#   --jobs N             containers at the same time (default min(5, Docker CPUs / 2): one libFuzzer process each;
#                        also limited so that jobs x RSS limit fits 85 percent of the Docker VM memory)
#   --targets LIST       comma list of probe,mountwalk,verdict,label,display,ctl (default all six)
#   --shards LIST        units per target, "mountwalk=4,probe=2" (default as shown; others 1). Shards of one
#                        target are independent processes with different seeds and their own corpus copy
#   --no-patches         probe and mountwalk on pristine NTFS-3G (no patch series): the known findings F1, F3 and
#                        F4 must then be recognised (self-test of the triage), not reported as regressions
#   --adopt              copy the minimised input of every NEW finding to tests/fuzz/findings/ (this edits the
#                        working tree; commit it deliberately)
#   --out DIR            result directory (default build/fuzz/campaign/<UTC stamp>)
#   --dry-run            print the plan and exit
#
# Plan: the budget is split equally over the units (targets x shards), so each unit is one process of
# budget / units seconds. With the defaults (10 units, 8 h): 2880 s = 48 min per unit, in two waves of five
# containers: about 2 x (48 min + 2 to 3 min engine build + triage) = 1 h 45 min wall time on 18 cores.
# Each unit (inner.sh): build with clang and ASan + UBSan, seed from the repository corpora and the reproducers
# in tests/fuzz/image/findings and tests/fuzz/findings, fuzz, replay every artifact, deduplicate by stack,
# minimise. Then triage.py merges all units into DIR/triage.md and DIR/triage.tsv: every signature is matched
# against tests/fuzz/campaign/known.tsv and classified (component, class; docs/SECURITY_FINDINGS.md section 2).
#
# Exit: 0 every signature is known and no known fixed finding came back, 1 a new finding or a regression, 2
# usage or setup error, 3 a unit failed to build or run.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
stamp=$(date -u +%Y%m%dT%H%M%SZ)
budget=28800
jobs=
targets="probe,mountwalk,verdict,label,display,ctl"
shards="mountwalk=4,probe=2"
nopatch=
adopt=
out=
dry=0
while [ $# -gt 0 ]; do
	case $1 in
	--budget-hours)
		[ $# -ge 2 ] || { echo "run.sh: --budget-hours needs a value" >&2; exit 2; }
		budget=$(awk -v h="$2" 'BEGIN { printf "%d", h * 3600 }'); shift ;;
	--budget-seconds) [ $# -ge 2 ] || { echo "run.sh: --budget-seconds needs a value" >&2; exit 2; }; budget=$2; shift ;;
	--jobs) [ $# -ge 2 ] || { echo "run.sh: --jobs needs a value" >&2; exit 2; }; jobs=$2; shift ;;
	--targets) [ $# -ge 2 ] || { echo "run.sh: --targets needs a value" >&2; exit 2; }; targets=$2; shift ;;
	--shards) [ $# -ge 2 ] || { echo "run.sh: --shards needs a value" >&2; exit 2; }; shards=$2; shift ;;
	--no-patches) nopatch=--no-patches ;;
	--adopt) adopt=--adopt ;;
	--out) [ $# -ge 2 ] || { echo "run.sh: --out needs a value" >&2; exit 2; }; out=$2; shift ;;
	--dry-run) dry=1 ;;
	-h | --help) sed -n '4,38p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "run.sh: unknown option: $1" >&2; exit 2 ;;
	esac
	shift
done
case $budget in '' | *[!0-9]*) echo "run.sh: budget must be a whole number of seconds" >&2; exit 2 ;; esac
[ "$budget" -gt 0 ] || { echo "run.sh: budget must be positive" >&2; exit 2; }
# the containers run in Docker's VM: its CPU count and memory limit the jobs, not the host's
dcpu=$(docker info --format '{{.NCPU}}' 2>/dev/null || echo 4)
dmem=$(docker info --format '{{.MemTotal}}' 2>/dev/null || echo 4294967296)
case $dcpu in '' | *[!0-9]*) dcpu=4 ;; esac
case $dmem in '' | *[!0-9]*) dmem=4294967296 ;; esac
dmem=$((dmem / 1048576))
if [ -z "$jobs" ]; then
	jobs=$((dcpu / 2))
	[ "$jobs" -le 5 ] || jobs=5
	[ "$jobs" -ge 1 ] || jobs=1
fi
case $jobs in '' | *[!0-9]*) echo "run.sh: --jobs must be a number" >&2; exit 2 ;; esac
# RSS limit of each libFuzzer process: an equal share of 85 percent of the VM memory, at most 2048 MB; fewer
# jobs rather than a limit below 768 MB (peak RSS seen in the smoke runs: 463 MB)
rss=$((dmem * 85 / 100 / jobs))
[ "$rss" -le 2048 ] || rss=2048
while [ "$rss" -lt 768 ] && [ "$jobs" -gt 1 ]; do
	jobs=$((jobs - 1))
	rss=$((dmem * 85 / 100 / jobs))
done
[ -n "$out" ] || out=$repo/build/fuzz/campaign/$stamp
case $out in /*) ;; *) out=$(pwd)/$out ;; esac

# unit list: "target shard"
plan=
for t in $(echo "$targets" | tr ',' ' '); do
	case $t in probe | mountwalk | verdict | label | display | ctl) ;; *) echo "run.sh: unknown target $t" >&2; exit 2 ;; esac
	n=$(echo ",$shards," | sed -n "s/.*,$t=\([0-9][0-9]*\),.*/\1/p")
	[ -n "$n" ] || n=1
	s=0
	while [ "$s" -lt "$n" ]; do
		plan="$plan $t:$s"
		s=$((s + 1))
	done
done
units=$(echo "$plan" | wc -w | tr -d ' ')
[ "$units" -gt 0 ] || { echo "run.sh: no units" >&2; exit 2; }
per=$((budget / units))
[ "$per" -ge 1 ] || { echo "run.sh: budget too small for $units units" >&2; exit 2; }
waves=$(((units + jobs - 1) / jobs))
echo "run.sh: Docker VM: $dcpu CPUs, $dmem MB; RSS limit $rss MB per process" >&2
echo "run.sh: $units units x $per s = $((units * per)) CPU-seconds, $jobs at a time ($waves wave(s)); results in $out" >&2
echo "run.sh: units:$plan" >&2
if [ "$dry" -eq 1 ]; then exit 0; fi
command -v docker >/dev/null 2>&1 || { echo "run.sh: docker not found" >&2; exit 2; }
mkdir -p "$out/units"

unit_run() {
	t=${1%:*}
	s=${1#*:}
	d=$out/units/$t-s$s
	mkdir -p "$d"
	rc=0
	# shellcheck disable=SC2086
	sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fuzz/campaign/inner.sh --target "$t" --shard "$s" \
		--seconds "$per" --rss-mb "$rss" $nopatch --tar >"$out/units/$t-s$s.tgz" 2>"$out/units/$t-s$s.log" || rc=$?
	if [ -s "$out/units/$t-s$s.tgz" ]; then tar -xzf "$out/units/$t-s$s.tgz" -C "$d" || true; fi
	echo "$rc" >"$out/units/$t-s$s.rc"
	rm -f "$out/units/$t-s$s.tgz"
}

pids=
live() {
	n=0
	np=
	for p in $pids; do
		if kill -0 "$p" 2>/dev/null; then n=$((n + 1)); np="$np $p"; fi
	done
	pids=$np
	echo "$n"
}
for u in $plan; do
	while [ "$(live)" -ge "$jobs" ]; do sleep 5; done
	echo "run.sh: start $u ($(date -u +%H:%M:%SZ))" >&2
	unit_run "$u" &
	pids="$pids $!"
done
# shellcheck disable=SC2086
wait $pids 2>/dev/null || true
wait || true

bad=0
for u in $plan; do
	t=${u%:*}
	s=${u#*:}
	rc=$(cat "$out/units/$t-s$s.rc" 2>/dev/null || echo 2)
	printf '%s-s%s: exit %s\n' "$t" "$s" "$rc"
	# exit 0: nothing found; exit 1 counts only when the unit says it found something; any other exit
	# (build failure, a signal, 137 from the container being killed) is a failed unit
	if [ "$rc" -ne 0 ]; then
		if [ "$rc" -ne 1 ] || ! grep -q '^RESULT: FINDING' "$out/units/$t-s$s/summary.txt" 2>/dev/null; then bad=1; fi
	fi
done
[ "$bad" -eq 0 ] || echo "run.sh: a unit failed to build or run (see $out/units/*.log)" >&2

res=0
python3 -I -B "$here/triage.py" --results "$out" --known "$here/known.tsv" --findings-dir "$repo/tests/fuzz/findings" \
	--plan "$(echo $plan)" --budget "$budget" $adopt || res=$?
if [ "$bad" -ne 0 ]; then exit 3; fi
exit "$res"
