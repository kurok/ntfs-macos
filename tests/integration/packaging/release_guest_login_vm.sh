#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M7-17: I-REL-01, the network-login check of the release build guest (vm/macos/guest-login-check.sh)
# against a real guest. One FRESH clone of the golden VM on the default shared network, which is the
# case the check must catch:
#   1. the golden VM's defaults (Remote Login and Screen Sharing on): the check FAILS;
#   2. --isolated on this networked clone: FAILS (it has an address and a route);
#   3. --close: the logins are turned off in the clone, and the check PASSES;
#   4. Remote Login turned on again on purpose (launchctl enable and bootstrap): the check FAILS again.
# The isolated case (tart.sh run --no-network, --close --isolated passes) is what every
# scripts/release-local.sh run does before its build. The VM is always deleted; the golden VM is only
# cloned.
#
# This script starts one VM. Run it through the VM slot protocol (vm/macos/run-suite.sh) when other
# runs may use the VMs.
#
# Usage: tests/integration/packaging/release_guest_login_vm.sh
# Exit: 0 all checks as expected, 1 a check was not, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
check=$root/vm/macos/guest-login-check.sh
name=ntfsmac-rellogin-$$

command -v tart >/dev/null 2>&1 || { echo "release_guest_login_vm: tart not found" >&2; exit 2; }
# shellcheck disable=SC2329 # called by the EXIT trap
cleanup() { "$tart_sh" delete "$name" >/dev/null 2>&1 || echo "release_guest_login_vm: WARNING: could not delete $name" >&2; }
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

fail=0
# expect RC DESC ARGS...: guest-login-check.sh ARGS must exit RC
expect() {
	_rc=$1 _d=$2; shift 2
	echo "-- guest-login-check.sh $* (want exit $_rc)"
	set +e; sh "$check" "$@" 2>&1; _got=$?; set -e
	if [ "$_got" -eq "$_rc" ]; then echo "PASS: $_d"; else echo "FAIL: $_d (exit $_got, want $_rc)"; fail=1; fi
}

expect 1 "the golden VM's defaults: a network login is open, the check fails" "$name"
expect 1 "a networked guest is not isolated" --isolated "$name"
expect 0 "after --close nothing listens and the check passes" --close "$name"
echo "-- turning Remote Login on again in the clone"
tart exec "$name" /bin/sh -c 'sudo -n /bin/launchctl enable system/com.openssh.sshd &&
	sudo -n /bin/launchctl bootstrap system /System/Library/LaunchDaemons/ssh.plist' </dev/null || {
	echo "release_guest_login_vm: could not turn Remote Login on again" >&2
	exit 2
}
expect 1 "Remote Login allowed on purpose: the check fails" "$name"

[ "$fail" -eq 0 ] && echo "release_guest_login_vm: PASSED"
exit "$fail"
