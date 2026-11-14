#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# inner.sh - runs inside Environment A (container): build, prove, sweep, stream the results.
# Called by run.sh; arguments go to crashsweep.py. stdout carries a gzip tar stream of the
# result directory, so every message goes to stderr.
# Exit: the sweeper's (0 all cut points pass, 1 a failing point or control, 2 setup failure).
set -eu

work=${NTFSMAC_WORK:-/work}
root=${CRASHSWEEP_ROOT:-/tmp/crashsweep}
export CRASHSWEEP_ROOT="$root"
exec 3>&1 1>&2

sh "$work/tests/stress/crashsweep/build.sh" "$root" || exit 2
mkdir -p "$root/out"
rc=0
python3 -I "$work/tests/stress/crashsweep/crashsweep.py" --keep-failing --out "$root/out" "$@" || rc=$?
cp "$root/build-proof.txt" "$root/out/build-proof.txt"
tar -czf - -C "$root/out" . >&3
exit "$rc"
