#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the harness in Environment B: clone a disposable tart VM from the golden VM
# (macOS 27.2 + macFUSE 5.5.0 with the FSKit extension approved, docs/VM_SETUP.md),
# boot it, build NTFS-3G in the guest against macFUSE's libfuse2
# (scripts/build-ntfs3g.sh --fuse external, about 15 s, into ~/ntfsmac/deps),
# run the cases in the guest through the tart guest agent, copy the result back,
# and always delete the VM (also on failure or interrupt).
#
# Usage: tests/harness/run-env-b.sh [--orchestrate] [--out DIR] [run.py options, e.g. --cases I-INT-01]
#   --orchestrate  run.py runs on the host with --env host (cases that drive the guest and the Linux
#                  container themselves, e.g. C-REF-01); the VM name is passed in NTFSMAC_B_VM.
# Env:   NTFSMAC_B_BUILD_LOG=FILE  also write the guest build log to FILE
# Exit:  run.py's exit code (0 ok, 1 case failed or errored, 2 setup error).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-harness-$$

cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "run-env-b: WARNING: could not delete $name" >&2
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

# The pinned NTFS-3G tarball, verified by fetch-deps.sh (only a download if it is missing).
tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "run-env-b: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || sh "$root/scripts/fetch-deps.sh" ntfs-3g >&2

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

# Build NTFS-3G in the guest from the same script and patch series the host uses.
echo "run-env-b: building NTFS-3G in the guest" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball") |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
build_cmd='cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1'
if [ -n "${NTFSMAC_B_BUILD_LOG:-}" ]; then
	"$tart_sh" exec "$name" -- "$build_cmd" >"$NTFSMAC_B_BUILD_LOG" || {
		tail -20 "$NTFSMAC_B_BUILD_LOG" >&2
		echo "run-env-b: guest build failed (log: $NTFSMAC_B_BUILD_LOG)" >&2
		exit 2
	}
else
	"$tart_sh" exec "$name" -- "$build_cmd" >&2 || { echo "run-env-b: guest build failed" >&2; exit 2; }
fi

if [ "${1:-}" = "--orchestrate" ]; then
	shift
	NTFSMAC_B_VM=$name python3 "$here/run.py" --env host --local --vm "$name" "$@"
else
	python3 "$here/run.py" --env B --vm "$name" "$@"
fi
