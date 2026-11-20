#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - performance benchmarks, one environment per call (T-M5-07, criterion 5.6, P-*).
#
# Usage: tests/perf/run.sh --env A|B --out DIR [--smoke] [--reps N] [--seq-mib N] [--rnd-ops N] [--small-files N]
#                          [--dir-entries N,N] [--image-mib N] [--image FILE] [--deps-tar FILE] [--build-dir DIR]
#                          [--deadline SECONDS] [--bench-args "..."]
#   --env A    NTFS-3G (`norecover`, the product's option list) in the Linux container (vm/linux/run.sh), no VM
#   --env B    this product (daemon, sandboxed worker, macFUSE FSKit) in a clone of ntfsmac-golden
#   --out DIR  result.json (ntfsmac-test-result/1: P-RUN-01, P-SEQ-01..02, P-RND-01..02, P-SMALL-01..04,
#              P-DIR-01.., P-RES-01), bench.json (every sample), summary.txt, driver.log
#   --smoke    reduced parameters (32 MiB, 1,000 random ops, 200 small files, 1,000 entries, 2 repetitions) for a
#              check of the tooling: the numbers are NOT the criterion run
#   defaults (the criterion run): 5 repetitions, sequential 1 GiB in 1 MiB blocks with fsync at the end, 16,384
#              random 4 KiB reads and writes over a 1 GiB file, 10,000 small files of 4 KiB, directories of 10,000 and
#              100,000 entries. Every phase is repeated; median, minimum and maximum are reported.
#   --image FILE  a raw NTFS image (default: made by mkntfs in Environment A, sized from the parameters). A and B must
#              run on the SAME image file: make it with the first call (`--save-image FILE` keeps the raw image) and pass
#              it to the second (`--image FILE`). The SHA-256 of the image each side used is in both results.
#   --save-image FILE  keep the raw image that was made or given
#   --deps-tar FILE  tar of the guest's ~/ntfsmac/deps (B), as for the other stress entries
# Both environments run the same tests/perf/bench.py with the same parameters on the same image bytes; B must run on the
# same physical host as A (a VM is not the hardware: see docs/PERFORMANCE.md). B takes no VM slot here: the caller holds
# it. A clone named ntfsmac-perf-PID is deleted on every exit path, by name as a last resort; there is no --keep.
# Then: python3 tests/perf/report.py --a A/result.json --b B/result.json --out docs/PERFORMANCE.md
# Time: smoke about 5 minutes in A and about 12 minutes in B (a guest with --deps-tar); the full run is hours, mostly
# the 100,000-entry directory and the 1 GiB files in B (not measured yet). Exit: 0 every case passed, 1 a case failed,
# 2 setup error. Perf is NOT a stress entry (no ENVS file): scripts/test-all.sh does not run it; see tests/perf/README.md.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
py=${PYTHON:-python3}
env_name=
out=
smoke=0
reps=
seq=
ops=
small=
dirs=
image_mib=
image=
save_image=
deps_tar=
build=$root/build/release
deadline=
extra=
need() { [ $# -ge 2 ] || { echo "perf: $1 needs a value" >&2; exit 2; }; }
while [ $# -gt 0 ]; do
	case $1 in
	--env) need "$@"; env_name=$2; shift 2 ;;
	--out) need "$@"; out=$2; shift 2 ;;
	--smoke) smoke=1; shift ;;
	--reps) need "$@"; reps=$2; shift 2 ;;
	--seq-mib) need "$@"; seq=$2; shift 2 ;;
	--rnd-ops) need "$@"; ops=$2; shift 2 ;;
	--small-files) need "$@"; small=$2; shift 2 ;;
	--dir-entries) need "$@"; dirs=$2; shift 2 ;;
	--image-mib) need "$@"; image_mib=$2; shift 2 ;;
	--image) need "$@"; image=$2; shift 2 ;;
	--save-image) need "$@"; save_image=$2; shift 2 ;;
	--deps-tar) need "$@"; deps_tar=$2; shift 2 ;;
	--build-dir) need "$@"; build=$2; shift 2 ;;
	--deadline) need "$@"; deadline=$2; shift 2 ;;
	--bench-args) need "$@"; extra=$2; shift 2 ;;
	-h | --help) sed -n '4,36p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "perf: unknown option $1" >&2; exit 2 ;;
	esac
done
case $env_name in A | B) ;; *) echo "perf: --env A or B is required" >&2; exit 2 ;; esac
[ -n "$out" ] || { echo "perf: --out is required" >&2; exit 2; }
if [ "$smoke" -eq 1 ]; then
	: "${reps:=2}" "${seq:=32}" "${ops:=1000}" "${small:=200}" "${dirs:=1000}"
else
	: "${reps:=5}" "${seq:=1024}" "${ops:=16384}" "${small:=10000}" "${dirs:=10000,100000}"
fi
for v in "$reps" "$seq" "$ops" "$small"; do
	case $v in '' | *[!0-9]* | 0) echo "perf: reps, seq-mib, rnd-ops and small-files must be positive numbers" >&2; exit 2 ;; esac
done
case $dirs in '' | *[!0-9,]*) echo "perf: --dir-entries is a comma separated list of numbers" >&2; exit 2 ;; esac
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
case $image in '' | /*) ;; *) image=$(pwd)/$image ;; esac
case $save_image in '' | /*) ;; *) save_image=$(pwd)/$save_image ;; esac
case $deps_tar in '' | /*) ;; *) deps_tar=$(pwd)/$deps_tar ;; esac
case $build in /*) ;; *) build=$(pwd)/$build ;; esac
mkdir -p "$out"
"$py" -B "$here/result.py" selftest >&2 || { echo "perf: result.py selftest failed" >&2; exit 2; }

# the image: the sequential and random file, the small files (data + MFT record) and the largest directory
maxdir=0
for n in $(echo "$dirs" | tr ',' ' '); do [ "$n" -gt "$maxdir" ] && maxdir=$n; done
if [ -z "$image_mib" ]; then
	image_mib=$(((seq + small * 5 / 1024 + maxdir * 2 / 1024 + 160 + 63) / 64 * 64))
	[ "$image_mib" -ge 128 ] || image_mib=128
fi
if [ -z "$deadline" ]; then
	if [ "$env_name" = A ]; then
		[ "$smoke" -eq 1 ] && deadline=900 || deadline=14400
	else
		[ "$smoke" -eq 1 ] && deadline=2700 || deadline=28800
	fi
fi
# the benchmark stops after a repetition when the next one (estimated from the ones done) would pass this
maxwall=$((deadline - 600))
[ "$maxwall" -gt 0 ] || maxwall=0
bench_args="--reps $reps --seq-mib $seq --rnd-ops $ops --small-files $small --dir-entries $dirs --max-wall $maxwall $extra"
started=$("$py" -B -c 'import time; print(time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime()))')
tmp=$(mktemp -d "${TMPDIR:-/tmp}/perf.XXXXXX")
name=ntfsmac-perf-$$
cleanup() {
	[ "$env_name" != B ] || "$tart_sh" delete "$name" >/dev/null 2>&1 || true
	rm -rf "$tmp"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# ---- the image: made in Environment A (mkntfs, 4 KiB clusters, quick format) or given; the same bytes go to A and B
if [ -n "$image" ]; then
	[ -f "$image" ] || { echo "perf: $image is missing" >&2; exit 2; }
	raw=$image
else
	echo "perf: making a ${image_mib} MiB image in Environment A" >&2
	sh "$root/vm/linux/run.sh" run -- sh -c "set -e; truncate -s ${image_mib}M /tmp/p.img; mkntfs -F -Q -L perf /tmp/p.img >/dev/null 2>&1; gzip -1 -c /tmp/p.img" </dev/null >"$tmp/made.gz" ||
		{ echo "perf: the image could not be made in Environment A" >&2; exit 2; }
	gunzip -c "$tmp/made.gz" >"$tmp/perf-ntfs.img" && rm -f "$tmp/made.gz"
	raw=$tmp/perf-ntfs.img
fi
[ -s "$raw" ] || { echo "perf: empty image" >&2; exit 2; }
[ -z "$save_image" ] || cp "$raw" "$save_image"

if [ "$env_name" = A ]; then
	gz=$tmp/perf-ntfs.img.gz
	gzip -1 -c "$raw" >"$gz"
	echo "perf: A: $bench_args; deadline ${deadline} s" >&2
	rc=0
	# `timeout` inside the container: the container is removed (--rm) when the shell ends, also on the deadline
	sh "$root/vm/linux/run.sh" run -- timeout -s KILL "$deadline" env "PERF_BENCH_ARGS=$bench_args" \
		sh /work/tests/perf/env_a.sh <"$gz" >"$out/bench.raw" 2>"$out/driver.log" || rc=$?
	tail -n 3 "$out/driver.log" >&2 || true
	sed -n 's/^PERF-JSON //p' "$out/bench.raw" | tail -n 1 >"$out/bench.in.json"
	rm -f "$out/bench.raw"
	[ -s "$out/bench.in.json" ] || { echo "perf: no benchmark result (status $rc); see $out/driver.log" >&2; exit 2; }
else
	for f in "$build/src/daemon/ntfsmacd" "$build/src/cli/ntfsmac"; do
		[ -f "$f" ] || { echo "perf: $f is missing; build first (cmake --build --preset release)" >&2; exit 2; }
	done
	mkdir -p "$tmp/fx"
	ln -s "$raw" "$tmp/fx/perf-ntfs.img"
	echo "perf: B: $bench_args; guest $name; host deadline ${deadline} s" >&2
	rc=0
	PERF_ARGS=$bench_args PERF_LABEL="B: this product in a macOS guest" \
		perl -e '
my ($t, @cmd) = @ARGV;
my $pid = fork();
defined $pid or exit 127;
if (!$pid) { setpgrp(0, 0); exec @cmd; exit 127 }
my $why = 0;
$SIG{ALRM} = sub { if ($why) { kill "KILL", -$pid } else { $why = 124; kill "TERM", -$pid; alarm 180 } };
for my $s (qw(INT TERM HUP)) { $SIG{$s} = sub { $why ||= 130; kill "TERM", -$pid; alarm 180 } }
alarm $t;
my $r;
do { $r = waitpid($pid, 0) } while ($r == -1 && $!{EINTR});
exit($why) if $why;
exit($? >> 8) if ($? & 127) == 0;
exit(128 + ($? & 127));
' "$deadline" \
		sh "$root/tests/integration/daemon/dmn_vm.sh" --name "$name" --build-dir "$build" --fixtures "$tmp/fx" ${deps_tar:+--deps-tar "$deps_tar"} \
		--out "$out/guest.log" perf >"$out/driver.log" 2>&1 || rc=$?
	tail -n 3 "$out/driver.log" >&2 || true
	[ "$rc" -ne 124 ] || echo "perf: the host deadline of ${deadline} s expired" >&2
	[ -f "$out/guest.log" ] || : >"$out/guest.log"
	sed -n 's/^PERF-JSON //p' "$out/guest.log" | tail -n 1 >"$out/bench.in.json"
	[ -s "$out/bench.in.json" ] || { echo "perf: no benchmark result in the guest log (status $rc); see $out/guest.log" >&2; exit 2; }
fi
"$py" -B -c 'import json, sys; json.dump(json.load(open(sys.argv[1])), open(sys.argv[2], "w"), indent=1)' "$out/bench.in.json" "$out/bench.json" ||
	{ echo "perf: the benchmark result is not JSON" >&2; exit 2; }
rm -f "$out/bench.in.json"
host_os="macOS $(sw_vers -productVersion 2>/dev/null || echo ?) $(uname -m)"
"$py" -B "$here/result.py" --bench "$out/bench.json" --env "$env_name" --out "$out" --repo "$root" --started "$started" \
	--bench-rc "$rc" --meta "host_os=$host_os"
