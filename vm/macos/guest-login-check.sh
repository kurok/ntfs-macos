#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# guest-login-check.sh - does a running guest accept a network login? (T-M7-17)
#
# Usage: vm/macos/guest-login-check.sh [--close] [--isolated] NAME
#
# The base image has the public login admin/admin, passwordless sudo, and Remote Login (sshd, TCP 22)
# and Screen Sharing (TCP 5900) on (seen 2026-11-26 in a clone of ntfsmac-golden). Anyone who reaches
# the guest's address could take it over. scripts/release-local.sh runs this before it trusts a build
# guest.
#
#   --close     first turn Remote Login, Screen Sharing, Remote Management and the Kerberos KDC off in
#               this running guest (launchctl disable and bootout of com.openssh.sshd,
#               com.apple.screensharing, com.apple.RemoteDesktop.agent and com.apple.Kerberos.kdc). A
#               change to the clone only: refused for the golden VMs.
#   --isolated  also require that the guest has no network: tart ip finds no address and the guest has no
#               default route over a real interface (vm/macos/tart.sh run --no-network).
#
# Checks: (1) in the guest, no TCP socket listens on an address other than loopback (netstat), any
# port, not only TCP 22 (Remote Login), 5900 (Screen Sharing) and 3283 (Remote Management); (2) from
# the host, when tart ip gives an address, a TCP connection to 22, 5900 and 3283 is refused (nc).
# Check 2 alone could pass wrongly when macOS Local Network privacy blocks this host process; check 1
# does not depend on the network. A guest command that fails or prints nothing usable is an error
# (exit 2), never a pass. NAME is the short or the full VM name.
# Exit: 0 no network login possible, 1 the guest listens beyond loopback, accepts a connection or is
# not isolated, 2 usage or setup error (also: a guest command failed).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
# shellcheck source=versions.env
. "$here/versions.env"

die() { printf 'guest-login-check: %s\n' "$1" >&2; exit "${2:-1}"; }
usage() { printf 'usage: %s [--close] [--isolated] NAME\n' "$0" >&2; }

close=0 isolated=0 vm=
while [ $# -gt 0 ]; do
	case "$1" in
	--close) close=1 ;;
	--isolated) isolated=1 ;;
	-h | --help) usage; exit 0 ;;
	-*) usage; die "unknown option: $1" 2 ;;
	*) [ -z "$vm" ] || { usage; die "only one NAME" 2; }; vm=$1 ;;
	esac
	shift
done
[ -n "$vm" ] || { usage; die "missing NAME" 2; }
case $vm in "$VM_PREFIX"*) ;; *) vm=$VM_PREFIX$vm ;; esac
command -v tart >/dev/null 2>&1 || die "tart not found" 2

ports='22 5900 3283'
g() { tart exec "$vm" /bin/sh -c "$1" </dev/null; }
g true >/dev/null 2>&1 || die "$vm is not running or has no guest agent" 2

if [ "$close" -eq 1 ]; then
	case $vm in "${VM_PREFIX}golden" | "${VM_PREFIX}golden-"*) die "refusing to change a golden VM ($vm); clone it first" 2 ;; esac
	# shellcheck disable=SC2016 # expanded in the guest
	g 'for s in com.openssh.sshd com.apple.screensharing com.apple.RemoteDesktop.agent com.apple.Kerberos.kdc; do
		sudo -n /bin/launchctl disable "system/$s" || exit 1
		sudo -n /bin/launchctl bootout "system/$s" >/dev/null 2>&1 || true
	done' || die "could not turn the network logins off in $vm"
	printf 'guest-login-check: %s: Remote Login, Screen Sharing, Remote Management and the Kerberos KDC turned off (this clone only)\n' "$vm"
fi

fail=0
# gout CMD [MARK]: the output of CMD in the guest. CMD's exit status is printed by the guest after its
# output; exit 2 when tart exec fails, the status line is missing (no answer, a cut-off output) or not 0,
# or MARK is given and no line matches it. A failed or empty tart exec never reads as "nothing found".
# (netstat -an -p tcp prints nothing at all, not even its header, when the guest has no TCP socket.)
gout() {
	_o=$(g "$1; echo \"__gout_rc=\$?\"" 2>&1) || die "'$1' in $vm failed: $_o" 2
	_l=$(printf '%s\n' "$_o" | tail -1)
	[ "$_l" = __gout_rc=0 ] || die "'$1' in $vm failed or gave no complete output (${_l:-nothing}): $_o" 2
	_o=$(printf '%s\n' "$_o" | sed '$d')
	[ -z "${2:-}" ] || printf '%s\n' "$_o" | grep -q "$2" || die "'$1' in $vm gave no '$2' line: $_o" 2
	printf '%s\n' "$_o"
}

# 1. listeners in the guest: any TCP socket in LISTEN on an address other than loopback
ns=$(gout 'netstat -an -p tcp')
listen=$(printf '%s\n' "$ns" | awk '$1 ~ /^tcp/ && $NF == "LISTEN" && $4 !~ /^(127\.|::1\.|localhost\.)/ && $4 !~ /%lo0\./ { print $1, $4 }')
if [ -n "$listen" ]; then
	printf 'guest-login-check: FAIL %s listens on TCP beyond loopback (a network login or other service):\n' "$vm" >&2
	printf '%s\n' "$listen" | sed 's/^/    /' >&2
	fail=1
else
	printf 'guest-login-check: ok   %s: no TCP socket listens beyond loopback\n' "$vm"
fi

# 2. from the host, at the guest's address
ip=$(tart ip --wait 3 "$vm" 2>/dev/null || true)
if [ -n "$ip" ]; then
	for p in $ports; do
		if nc -z -G 3 "$ip" "$p" >/dev/null 2>&1; then
			printf 'guest-login-check: FAIL %s accepts a connection at %s:%s from this host\n' "$vm" "$ip" "$p" >&2
			fail=1
		else
			printf 'guest-login-check: ok   %s:%s refused or unreachable\n' "$ip" "$p"
		fi
	done
else
	printf 'guest-login-check: ok   %s has no address on the host network (tart ip)\n' "$vm"
fi

# 3. isolation
if [ "$isolated" -eq 1 ]; then
	if [ -n "$ip" ]; then
		printf 'guest-login-check: FAIL %s has the address %s; it must have no network (tart.sh run --no-network)\n' "$vm" "$ip" >&2
		fail=1
	fi
	# a default route over a real interface (macOS keeps IPv6 default routes over its own utun tunnels, which
	# lead nowhere outside the guest; those do not count)
	rt=$(gout 'netstat -rn' '^Routing tables')
	routes=$(printf '%s\n' "$rt" | awk '$1 == "default" && $4 !~ /^(utun|lo)[0-9]/ { print $1, $2, $4 }')
	if [ -n "$routes" ]; then
		printf 'guest-login-check: FAIL %s has a default route:\n' "$vm" >&2
		printf '%s\n' "$routes" | sed 's/^/    /' >&2
		fail=1
	else
		printf 'guest-login-check: ok   %s has no default route (other than over its own utun tunnels)\n' "$vm"
	fi
fi
exit "$fail"
