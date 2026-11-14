#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Run the harness in Environment B: clone a disposable tart VM, boot it, run the
# cases in the guest through the tart guest agent, copy the result back, and
# always delete the VM (also on failure or interrupt).
#
# Usage: tests/harness/run-env-b.sh [--out DIR] [run.py options, e.g. --cases I-INT-01]
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

"$tart_sh" clone "$name" >&2
"$tart_sh" run "$name" >&2
python3 "$here/run.py" --env B --vm "$name" "$@"
