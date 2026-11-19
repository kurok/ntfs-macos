#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fixture generator v1 (T-M3-02): stale-$MFTMirr images, docs/TEST_STRATEGY.md 3.2
# (mirror-split, mirror-newer, mirror-torn, mirror-extra), two extras for
# U-PROBE-MIRROR-03 (mirror-split-dirty, mirror-split-hiber) and the failing cut
# points of the crash sweep for U-PROBE-MIRROR-05 (sweep-WORKLOAD-cN).
#
# Usage: tests/fixtures/gen/mirror.sh [--only NAME|sweep] [--export DIR]
#
# Run on the host: the script re-runs itself inside Environment A
# (vm/linux/run.sh, debian:13-slim with the pinned NTFS-3G and ntfsprogs).
# Images are made and mounted ONLY inside the container; nothing is attached or
# mounted on the host. Images live in the container's /tmp and vanish with it.
# Use --export DIR to copy them to the host (gzip stream, extracted under DIR,
# which should be below the git-ignored build/). Images are never committed.
#
# How: tests/stress/crashsweep/build.sh builds the release and the --enable-faultio
# NTFS-3G (patch 0004, T-M3-14) into CRASHSWEEP_ROOT (default /tmp/crashsweep; skipped
# when it is already there). tests/fixtures/gen/mirror_make.py then runs the sweep's
# workloads on a fresh mkntfs image under the fault build with plan cut:N, N taken from
# the write trace (write N is $MFT record 0, write N+1 would be $MFTMirr record 0), and
# derives the other images. Each image is checked against the pinned NTFS-3G: the
# read-only mount is refused with the expected message, and where the strict rule of
# ARCHITECTURE.md section 3.7 must accept it, copying $MFT record 0 over the mirror on a
# scratch copy gives a read-only mount and `ntfsfix -n` exit 0.
# Env: NTFSMAC_FIXTURE_OUT (container output dir, default /tmp/ntfsmac-mirror),
#      CRASHSWEEP_ROOT.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")

die() { printf 'mirror.sh: error: %s\n' "$*" >&2; exit 1; }

only=
export_dir=
inner=0
tar_out=0
while [ $# -gt 0 ]; do
	case $1 in
	--only) [ $# -ge 2 ] || die "--only needs a name"; only=$2; shift ;;
	--export) [ $# -ge 2 ] || die "--export needs a dir"; export_dir=$2; shift ;;
	--inner) inner=1 ;;
	--tar) tar_out=1 ;;
	-h | --help) sed -n '4,30p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

if [ "$inner" -eq 0 ]; then
	args="--inner"
	[ -z "$only" ] || args="$args --only $only"
	if [ -n "$export_dir" ]; then
		mkdir -p "$export_dir"
		# A file, not a pipe: the status of the run in Environment A must not be lost.
		tgz=$(mktemp "${TMPDIR:-/tmp}/fixgen.XXXXXX") || exit 1
		# shellcheck disable=SC2086
		if ! sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/mirror.sh $args --tar >"$tgz" ||
			! tar -xzf "$tgz" -C "$export_dir"; then
			rm -f "$tgz"
			printf 'mirror.sh: error: the run in Environment A or the unpacking failed\n' >&2
			exit 1
		fi
		rm -f "$tgz"
		exit 0
	fi
	# shellcheck disable=SC2086
	exec sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/mirror.sh $args
fi

# ---- inside the container from here on ----
work=/work
out=${NTFSMAC_FIXTURE_OUT:-/tmp/ntfsmac-mirror}
root=${CRASHSWEEP_ROOT:-/tmp/crashsweep}
bin=/tmp/ntfsmac-mirror-bin
prefix=${NTFS3G_PREFIX:-/opt/ntfs-3g}
for t in mkntfs ntfs-3g ntfsfix python3 cc; do
	command -v "$t" >/dev/null 2>&1 || die "$t not found: run through Environment A"
done
mkdir -p "$out" "$bin"

# stdout carries the tar stream with --tar, so all messages go to stderr then.
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

export CRASHSWEEP_ROOT="$root"
if [ ! -x "$root/faultio/bin/ntfs-3g" ] || [ ! -x "$root/release/bin/ntfs-3g" ]; then
	sh "$work/tests/stress/crashsweep/build.sh" "$root" >"$out/build-crashsweep.log" 2>&1 ||
		{ tail -20 "$out/build-crashsweep.log"; die "crashsweep build failed (log: $out/build-crashsweep.log)"; }
fi

san=
if [ "${NTFS_ASAN:-0}" = 1 ]; then san="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
# shellcheck disable=SC2086  # $san is a word list
cc -std=c17 -O2 -Wall -Wextra -Werror $san -isystem "$prefix/include" \
	-o "$bin/setvolflags" "$work/tests/tools/setvolflags.c" -L"$prefix/lib" -lntfs-3g ||
	die "building setvolflags failed"

export LC_ALL=C.UTF-8
rc=0
if [ -n "$only" ]; then
	python3 -I "$work/tests/fixtures/gen/mirror_make.py" --out "$out" --setvolflags "$bin/setvolflags" --only "$only" || rc=$?
else
	python3 -I "$work/tests/fixtures/gen/mirror_make.py" --out "$out" --setvolflags "$bin/setvolflags" || rc=$?
fi
if [ "$tar_out" -eq 1 ]; then
	tar -czf - -C "$out" . >&3
fi
exit "$rc"
