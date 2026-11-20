#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run.sh - low-space test S-NOSPC-01 (T-M5-06, criterion 5.5), Environment B.
#
# A manual convenience only, NOT registered as a suite entry (no ENVS file): the S-NOSPC-* cases are
# declared with @case and run by the harness stage of scripts/test-all.sh / vm/macos/run-suite.sh,
# so registering this script would run them twice.
#
# Usage: tests/stress/enospc/run.sh --out DIR
#   --out DIR   result directory on the host (result file and evidence/)
#
# Clones a disposable VM from the golden VM, builds NTFS-3G in it, mounts a small NTFS image
# read/write (norecover, backend=fskit) and runs the cases S-NOSPC-01 (fill the volume: ENOSPC,
# pre-existing hashes intact, delete frees space, writing works again, volume clean after unmount)
# and S-NOSPC-02 (negative controls: the checks report a wrong errno, a damaged file and a dirty
# image). Everything goes through tests/harness/run-env-b.sh, which deletes its VM on exit. The
# cases are also declared for Environment A (tests/harness/run.py --env A --cases 'S-NOSPC-*').
# Size of the run: environment variables read by the case under `run.py --local` (a longer run
# inside a guest or container); the defaults are a 96 MiB image and 3 fill/free rounds:
#   NTFSMAC_NOSPC_MIB (image size), NTFSMAC_NOSPC_ROUNDS (fill/free cycles), NTFSMAC_NOSPC_FILES.
# Exit: 0 both cases passed, 1 a case failed or errored, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
out=
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "run.sh: --out needs a value" >&2; exit 2; }; out=$2; shift 2 ;;
	-h | --help) sed -n '4,23p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "run.sh: unknown option $1" >&2; exit 2 ;;
	esac
done
[ -n "$out" ] || { echo "run.sh: --out DIR is required" >&2; exit 2; }
case $out in /*) ;; *) out=$(pwd)/$out ;; esac
mkdir -p "$out"
rc=0
sh "$root/tests/harness/run-env-b.sh" --out "$out" --cases 'S-NOSPC-*' || rc=$?
echo "run.sh: results in $out (exit $rc)" >&2
exit "$rc"
