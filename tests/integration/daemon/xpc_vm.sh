#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-14: the XPC door of the privileged requests with real peers, in a disposable macOS guest
# (Environment B): the real listener (src/daemon/ctl_xpc.c) started by launchd under the product's
# Mach service name, the real ntfsmac CLI (src/cli) as its client, the code requirement of check
# 3 from a root-owned file as the installer writes it, and fake operations that log what reached
# them (tests/integration/daemon/xpc_vm.c). It needs no NTFS-3G and no image.
#
# What it shows: a message reaches the operation with the uid and pid taken from its audit token;
# the connection of a CLI that does not satisfy the requirement (a re-signed copy, a build without
# the hardened runtime) is refused and no operation runs; the default strategy of check 4 (the
# external form) refuses a request without a form, and the ADR-019 fallback (a root peer) accepts
# `sudo ntfsmac journal-clear`; the typed confirmation can come from the terminal.
# What it does not and must not do: type or supply a password. A non-root CLI would show the
# system password dialog, so every case here runs the CLI as root, and the external form of
# check 4 with a real grant is the person's step (tests/unit/ctl/vm/u17.sh, parts B3 and B4).
#
# Usage: tests/integration/daemon/xpc_vm.sh [--out FILE]
# Exit:  0 all checks passed, 1 a check failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-xpc-$$
out=

while [ $# -gt 0 ]; do
	case "$1" in
	--out) [ $# -ge 2 ] || { echo "xpc_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	*) echo "xpc_vm: unknown argument $1" >&2; exit 2 ;;
	esac
done

cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "xpc_vm: WARNING: could not delete $name" >&2
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

echo "xpc_vm: copying the sources" >&2
(cd "$root" && tar -cf - include src/daemon src/cli tests/integration/daemon/xpc_vm.c) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/xpc && mkdir -p ~/ntfsmac/xpc && tar -xf - -C ~/ntfsmac/xpc'

# The guest script. Quoted so that nothing expands on the host.
guest='set -eu
d=$HOME/ntfsmac/xpc
work=$(cd -P "$(mktemp -d /tmp/xpcvm.XXXXXX)" && pwd)
chmod 755 "$work"
label=org.ntfs-macos.xpc-test
reqdir="/Library/Application Support/ntfs-macos"
cleanup() {
	sudo -n /bin/launchctl bootout system/$label >/dev/null 2>&1 || true
	sudo -n rm -f "$reqdir/cli-requirement"
	sudo -n rm -rf "$work"
}
trap cleanup EXIT
cd "$d"
fails=0
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }
check() { # check DESCRIPTION COMMAND... (true means pass)
	desc=$1; shift
	if "$@"; then pass "$desc"; else fail "$desc"; fi
}

cc -std=c17 -Wall -Wextra -Werror -O2 -I include -I src/daemon -o "$work/xpc_vm" tests/integration/daemon/xpc_vm.c \
	src/daemon/ctl.c src/daemon/ctl_peer.c src/daemon/ctl_sys.c src/daemon/ctl_parse.c src/daemon/ctl_xpc.c \
	src/daemon/status.c -framework Security -framework CoreFoundation
cc -std=c17 -Wall -Wextra -Werror -O2 -I include -I src/daemon -I src/cli -o "$work/ntfsmac" \
	src/cli/authz.c src/cli/cli.c src/cli/client.c src/cli/cmd_journal.c src/cli/cmd_repair.c src/cli/json.c \
	src/cli/priv.c src/cli/reasons.c src/cli/render.c src/cli/req.c src/cli/xpc_client.c src/cli/main.c \
	src/daemon/ctl_parse.c -framework Security -framework CoreFoundation
cp "$work/ntfsmac" "$work/ntfsmacA"; codesign --force -s - -o runtime "$work/ntfsmacA" 2>&1
cp "$work/ntfsmac" "$work/ntfsmacB"; codesign --force -s - "$work/ntfsmacB" 2>&1
cp "$work/ntfsmac" "$work/ntfsmacC"; codesign --force -s - -o runtime -i org.example.other "$work/ntfsmacC" 2>&1
hashA=$(codesign -d -vvv "$work/ntfsmacA" 2>&1 | sed -n "s/^CDHash=//p")
echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un); requirement: cdhash of the installed CLI (A)"

# The requirement the installer records (ADR-019 check 3), root-owned.
printf "cdhash H\"%s\"\n" "$hashA" > "$work/req"
sudo -n install -d -m 755 -o root -g wheel "$reqdir"
sudo -n install -m 644 -o root -g wheel "$work/req" "$reqdir/cli-requirement"

log=$work/xpc.log
start_daemon() { # start_daemon MODE
	sudo -n /bin/launchctl bootout system/$label >/dev/null 2>&1 || true
	: > "$log"; chmod 666 "$log"
	printf "%s\n" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
		"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">" \
		"<plist version=\"1.0\"><dict>" \
		"<key>Label</key><string>$label</string>" \
		"<key>ProgramArguments</key><array><string>$work/xpc_vm</string><string>$log</string><string>$1</string></array>" \
		"<key>MachServices</key><dict><key>org.ntfs-macos.ctl.privileged</key><true/></dict>" \
		"<key>StandardErrorPath</key><string>$work/daemon.err</string>" \
		"</dict></plist>" > "$work/plist.tmp"
	sudo -n install -m 644 -o root -g wheel "$work/plist.tmp" "$work/$label.plist"
	sudo -n /bin/launchctl bootstrap system "$work/$label.plist"
}
nlog() { wc -l < "$log" | tr -d " "; }
has() { printf "%s" "$1" | grep -qi -- "$2"; }
runq() { rc=0; o=$("$@" 2>&1) || rc=$?; } # never lets a failing command end the script

# 1. the ADR-019 fallback: a root peer is accepted, with the identity from the audit token
start_daemon root
runq sudo -n "$work/ntfsmacA" journal-clear --label "My Disk" --serial 0000beef disk4s1
check "fallback: sudo ntfsmac journal-clear is accepted (exit $rc)" test "$rc" -eq 0
check "fallback: the reply is shown" has "$o" cleared
check "the operation saw uid 0 from the audit token" grep -q "^journal_clear uid=0 " "$log"
check "the operation got the typed label and the upper-cased serial" grep -q "label=My Disk serial=0000BEEF" "$log"
check "the CLI said that it ran as root (no dialog)" has "$o" "running as root"
runq sudo -n "$work/ntfsmacA" repair --clear-dirty --label L --serial 00000001 disk5s1
check "fallback: sudo ntfsmac repair --clear-dirty is accepted (exit $rc)" test "$rc" -eq 0
check "the repair request carries the action" grep -q "^repair uid=0 .*bsd=disk5s1 action=clear_dirty" "$log"
rc=0; o=$(printf "My Disk\n0000BEEF\n" | sudo -n "$work/ntfsmacA" journal-clear disk4s1 2>&1) || rc=$?
check "the typed confirmation can come from the terminal (exit $rc)" test "$rc" -eq 0
n=$(nlog)

# 2. a CLI that does not satisfy the requirement never reaches the operation
runq sudo -n "$work/ntfsmacC" journal-clear --label L --serial 00000001 disk4s1
check "a re-signed copy of the CLI is refused (exit $rc)" test "$rc" -ne 0
echo "  output: $o"
runq sudo -n "$work/ntfsmacB" journal-clear --label L --serial 00000001 disk4s1
check "a CLI without the hardened runtime is refused (exit $rc)" test "$rc" -ne 0
echo "  output: $o"
check "no operation ran for either" test "$(nlog)" = "$n"

# 3. the default strategy: root is not enough, the request needs the external form
start_daemon external
runq sudo -n "$work/ntfsmacA" journal-clear --label L --serial 00000001 disk4s1
check "default strategy: a request without a form is refused (exit $rc)" test "$rc" -ne 0
# no right is installed in this guest, and the right is checked before the form: the refusal names it
check "the refusal names the missing right (it is checked before the form)" has "$o" "right of ntfsmac is missing"
echo "  output: $o"
check "no operation ran" test "$(nlog)" = "0"

# 4. no service: the CLI says so
sudo -n /bin/launchctl bootout system/$label >/dev/null 2>&1 || true
runq sudo -n "$work/ntfsmacA" journal-clear --label L --serial 00000001 disk4s1
check "with the service gone the CLI says the daemon is not running (exit $rc)" has "$o" "not running"
echo "== daemon stderr"
cat "$work/daemon.err" 2>/dev/null | sed "s/^/  /" || true
echo "== result: $fails failure(s)"
[ "$fails" -eq 0 ]'

rc=0
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "$guest" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "$guest" || rc=$?
fi
exit "$rc"
