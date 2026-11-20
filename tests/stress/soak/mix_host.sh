#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# mix_host.sh - host side of the soak (S-SOAK-01, criterion 5.2) and parallel (S-PAR-01, criterion 5.3) suites
# (T-M5-02). Environment B. Not called directly: tests/stress/soak/run.sh and tests/stress/parallel/run.sh
# set the kind and give the options on. Usage: mix_host.sh soak|par --out DIR [options]; see those scripts.
#
# One guest is cloned from ntfsmac-golden (tests/integration/daemon/dmn_vm.sh --keep: NTFS-3G, daemon, probe,
# volmark, fixture), the suite is copied in and mix_guest.sh runs in it under a host-side deadline (perl alarm);
# the evidence is fetched and mix_result.py writes result.json. The guest is deleted on every exit path. This
# script takes no VM slot: the caller holds it. Nothing is mounted on the host.
# Exit: 0 every case passed, 1 a case failed, 2 setup error.
set -eu

kind=${1:?kind}
shift
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
py=${PYTHON:-python3}
tag=$kind
case $kind in
soak)
	# registered default: 10 min (the baseline point is a sixth of the run, as 1 h of 6 h)
	duration=600 verify=120 rss_every=10 workers=1 hang=60 full_d=21600 full_v=600 full_r=30 smoke_d=180 smoke_v=30 smoke_r=5
	full_hint="--full (6 h)"
	;;
par)
	duration=300 verify=60 rss_every=10 workers=16 hang=60 full_d=1800 full_v=120 full_r=10 smoke_d=120 smoke_v=30 smoke_r=10
	full_hint="--full (30 min)"
	;;
*) echo "mix_host: kind must be soak or par" >&2; exit 2 ;;
esac
usage() {
	cat <<EOF
Usage: run.sh --out DIR [--duration S | --full | --smoke] [--baseline S] [--verify-every S] [--rss-every S]
              [--workers N] [--hang-limit S] [--controls LIST] [--fault KIND] [--fixtures DIR] [--build-dir DIR]
              [--deps-tar FILE] [--keep]
  --out DIR          result directory: result.json (ntfsmac-test-result/1), plan.txt, meta.txt, runs.tsv, guest.log,
                     per run directory: summary.json, run.log, rss.tsv (soak), workers.json, remount.json, state.tar.gz
  --duration S       seconds of the real run (default $duration)
  --full             the criterion run: $full_d s ($full_hint); --smoke: $smoke_d s
  --baseline S       soak: the point of the RSS baseline, seconds from the start (default: a sixth of the duration,
                     as 1 h of 6 h); the rule is growth of the peak after it below 10 percent
  --verify-every S   seconds between full verifications of the whole tree (default $verify)
  --rss-every S      soak: seconds between samples of the worker's RSS (default $rss_every)
  --workers N        parallel: writer processes (default $workers; the criterion is 16)
  --hang-limit S     longest single operation or progress gap tolerated (default $hang; the criterion is 60)
  --controls LIST    negative controls, short runs on their own mount (default: "corrupt" for soak, "corrupt stall"
                     for parallel; "" for none)
  --fault KIND       corrupt|stall: inject that fault into the REAL run (the suite must then FAIL); a test seam
  --fixtures DIR     directory with clean-64m-4k.img (default: generated in Environment A, needs Docker)
  --build-dir DIR    CMake tree with src/daemon/ntfsmacd and src/cli/ntfsmac (default build/release)
  --deps-tar FILE    tar of the guest's ~/ntfsmac/deps (as dmn_vm.sh), used instead of building NTFS-3G when it exists
  --keep             keep the guest (delete it yourself with vm/macos/tart.sh delete NAME)
EOF
}
out= fx= build=$root/build/release deps_tar= keep=0 fault= baseline= mode=
if [ "$kind" = soak ]; then controls=corrupt; else controls="corrupt stall"; fi
need() { [ $# -ge 2 ] || { echo "$tag: $1 needs a value" >&2; exit 2; }; }
while [ $# -gt 0 ]; do
	case $1 in
	--out) need "$@"; out=$2; shift 2 ;;
	--duration) need "$@"; duration=$2; mode=custom; shift 2 ;;
	--full) duration=$full_d verify=$full_v rss_every=$full_r mode=full; shift ;;
	--smoke) duration=$smoke_d verify=$smoke_v rss_every=$smoke_r mode=smoke; shift ;;
	--baseline) need "$@"; baseline=$2; shift 2 ;;
	--verify-every) need "$@"; verify=$2; shift 2 ;;
	--rss-every) need "$@"; rss_every=$2; shift 2 ;;
	--workers) need "$@"; workers=$2; shift 2 ;;
	--hang-limit) need "$@"; hang=$2; shift 2 ;;
	--controls) need "$@"; controls=$2; shift 2 ;;
	--fault) need "$@"; fault=$2; shift 2 ;;
	--fixtures) need "$@"; fx=$2; shift 2 ;;
	--build-dir) need "$@"; build=$2; shift 2 ;;
	--deps-tar) need "$@"; deps_tar=$2; shift 2 ;;
	--keep) keep=1; shift ;;
	-h | --help) usage; exit 0 ;;
	*) echo "$tag: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$out" ] || { echo "$tag: --out is required" >&2; exit 2; }
for v in duration verify rss_every workers hang; do
	eval "val=\$$v"
	case $val in '' | *[!0-9]* | 0) echo "$tag: $v must be a positive whole number of seconds/processes" >&2; exit 2 ;; esac
done
[ -n "$baseline" ] || baseline=$((duration / 6))
case $baseline in '' | *[!0-9]*) echo "$tag: --baseline must be a whole number" >&2; exit 2 ;; esac
if [ "$kind" = soak ]; then
	[ "$baseline" -gt 0 ] && [ "$baseline" -lt "$duration" ] || { echo "$tag: the baseline point must be inside the run (0 < $baseline < $duration)" >&2; exit 2; }
	[ "$workers" -eq 1 ] || { echo "$tag: the soak has one worker process" >&2; exit 2; }
	[ $((duration - baseline)) -ge $((rss_every * 4)) ] || { echo "$tag: fewer than 4 RSS samples fit after the baseline: lower --rss-every or lengthen the run" >&2; exit 2; }
fi
[ "$verify" -lt "$duration" ] || { echo "$tag: --verify-every must be shorter than the run" >&2; exit 2; }
for c in $controls; do
	case $kind:$c in soak:corrupt | par:corrupt | par:stall) ;; *) echo "$tag: unknown control $c for $kind" >&2; exit 2 ;; esac
done
case $fault in '' | corrupt | stall) ;; *) echo "$tag: --fault must be corrupt or stall" >&2; exit 2 ;; esac
[ "$kind:$fault" != soak:stall ] || { echo "$tag: no stall seam in the soak" >&2; exit 2; }
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
case $fx in '' | /*) ;; *) fx=$(pwd)/$fx ;; esac
case $build in /*) ;; *) build=$(pwd)/$build ;; esac
case $deps_tar in '' | /*) ;; *) deps_tar=$(pwd)/$deps_tar ;; esac
mkdir -p "$out"

"$py" -B "$here/mixlib.py" selftest >&2 || { echo "$tag: mixlib.py selftest failed" >&2; exit 2; }
"$py" -B "$here/mix_result.py" selftest >&2 || { echo "$tag: mix_result.py selftest failed" >&2; exit 2; }
for f in "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac"; do
	[ -f "$f" ] || { echo "$tag: $f is missing (cmake --build --preset release, or --build-dir)" >&2; exit 2; }
done

tmp=$(mktemp -d "${TMPDIR:-/tmp}/$tag.XXXXXX")
name=ntfsmac-$tag-$$
dpid=
started=$($py -B -c 'import time; print(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))')
cleanup() {
	trap '' INT TERM HUP
	if [ -n "$dpid" ]; then
		kill "$dpid" 2>/dev/null || true
		wait "$dpid" 2>/dev/null || true
	fi
	if [ "$keep" -eq 0 ]; then
		"$tart_sh" delete "$name" >&2 || echo "$tag: WARNING: could not delete $name" >&2
	else
		echo "$tag: guest $name kept (--keep): delete it with vm/macos/tart.sh delete $name" >&2
	fi
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

if [ -z "$fx" ]; then
	fx=$tmp/fixtures
	echo "$tag: generating clean-64m-4k in Environment A" >&2
	sh "$root/tests/fixtures/gen/clean.sh" --only clean-64m-4k --export "$fx" >&2 || { echo "$tag: fixture generation failed" >&2; exit 2; }
fi
[ -f "$fx/clean-64m-4k.img" ] || { echo "$tag: $fx/clean-64m-4k.img is missing" >&2; exit 2; }

dargs=
[ -z "$deps_tar" ] || dargs="--deps-tar $deps_tar"
# shellcheck disable=SC2086
sh "$root/tests/integration/daemon/dmn_vm.sh" --keep --name "$name" --build-dir "$build" --fixtures "$fx" $dargs kill >"$tmp/dmn_vm.log" 2>&1 &
dpid=$!
wait "$dpid" || true
dpid=
if ! grep -q 'is up with everything copied in' "$tmp/dmn_vm.log"; then
	tail -n 20 "$tmp/dmn_vm.log" >&2
	echo "$tag: the guest $name is not ready" >&2
	exit 2
fi

{
	echo "kind=$kind"
	echo "duration=$duration"
	echo "baseline=$baseline"
	echo "verify=$verify"
	echo "rss_every=$rss_every"
	echo "workers=$workers"
	echo "hang=$hang"
	echo "controls=$controls"
	echo "fault=$fault"
	echo "mode=${mode:-default}"
} >"$out/plan.txt"
# the deadline: the real run, the controls (at most 30 s each), the mount cycles (about 2 min each, 600 s allowed) and the verification
ncyc=1
for c in $controls; do ncyc=$((ncyc + 1)); done
budget=$((duration + ncyc * 700 + 400))

echo "$tag: copying the suite into $name" >&2
(cd "$root" && tar -cf - tests/stress/soak) |
	tart exec -i "$name" /bin/sh -c 'mkdir -p ~/ntfsmac/dmn && tar -xf - -C ~/ntfsmac/dmn'

echo "$tag: running in the guest ($duration s, workers $workers, baseline $baseline s, controls: ${controls:-none}${fault:+, fault $fault})" >&2
grc=0
perl -e 'alarm shift; exec @ARGV or exit 127' "$budget" sh "$tart_sh" exec "$name" -- "rm -rf ~/mixout; MIX_DURATION=$duration MIX_BASELINE=$baseline MIX_VERIFY=$verify MIX_RSS_EVERY=$rss_every MIX_WORKERS=$workers MIX_HANG=$hang MIX_CONTROLS='$controls' MIX_FAULT='$fault' sh ~/ntfsmac/dmn/tests/stress/soak/mix_guest.sh $kind ~/ntfsmac/dmn ~/mixout 2>&1" >"$out/guest.log" || grc=$?
tail -n 40 "$out/guest.log" >&2
perl -e 'alarm shift; exec @ARGV or exit 127' 300 tart exec "$name" /bin/sh -c 'cd ~/mixout 2>/dev/null && tar -cf - .' | tar -xf - -C "$out" ||
	echo "$tag: WARNING: could not fetch the evidence from the guest" >&2
echo "$tag: guest script exit $grc" >&2
echo "guest_rc=$grc" >>"$out/meta.txt"
echo "host_os=macOS $(sw_vers -productVersion 2>/dev/null || echo host)" >>"$out/meta.txt"

"$py" -B "$here/mix_result.py" finalize --dir "$out" --repo "$root" --started "$started"
