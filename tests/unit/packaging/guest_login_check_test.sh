#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-11 (T-M7-17): vm/macos/guest-login-check.sh against a stub `tart` (no VM): a guest command
# that fails or prints nothing is an error (exit 2), never a pass; any TCP listener beyond loopback
# fails, on any port; a default route over a real interface fails --isolated, utun routes do not;
# --close is refused for a golden VM. The real guest is I-REL-01.
#
# Usage: guest_login_check_test.sh REPO_ROOT
set -eu

root=${1:?usage: guest_login_check_test.sh REPO_ROOT}
check=$root/vm/macos/guest-login-check.sh
t=$(mktemp -d "${TMPDIR:-/tmp}/guest-login-check-test.XXXXXX")
trap 'rm -rf "$t"' EXIT
mkdir "$t/bin"

# the stub: tart ip (no address unless FAKE_IP) and tart exec VM /bin/sh -c CMD
cat >"$t/bin/tart" <<'EOF'
#!/bin/sh
case $1 in
ip) [ -n "${FAKE_IP:-}" ] && { echo "$FAKE_IP"; exit 0; }; echo "no IP address found" >&2; exit 1 ;;
exec)
	[ "${FAKE_DOWN:-0}" = 1 ] && { echo "no guest agent" >&2; exit 1; }
	cmd=$5
	# guest-login-check.sh appends '; echo "__gout_rc=$?"' to the commands whose output it reads
	rc_line=0
	case $cmd in *'; echo "__gout_rc=$?"') cmd=${cmd%'; echo "__gout_rc=$?"'}; rc_line=1 ;; esac
	case $cmd in
	true) exit 0 ;;
	'for s in '*) echo "$cmd" >"$FAKE_DIR/closed"; exit 0 ;;
	'netstat -an -p tcp') v=${FAKE_NS:-clean}; f=ns ;;
	'netstat -rn') v=${FAKE_RT:-none}; f=rt ;;
	*) echo "stub tart: unexpected command: $cmd" >&2; exit 99 ;;
	esac
	case $v in
	fail) echo "tart exec: connection lost" >&2; exit 1 ;;   # tart exec itself fails
	cmdfail) [ "$rc_line" = 1 ] && echo "__gout_rc=1"; exit 0 ;;   # netstat fails in the guest
	empty) exit 0 ;;                                          # no output at all, not even the status line
	none-at-all) [ "$rc_line" = 1 ] && echo "__gout_rc=0"; exit 0 ;;   # netstat prints nothing: no socket
	*) cat "$FAKE_DIR/$f.$v"; [ "$rc_line" = 1 ] && echo "__gout_rc=0"; exit 0 ;;
	esac
	;;
*) echo "stub tart: unexpected: $*" >&2; exit 99 ;;
esac
EOF
chmod +x "$t/bin/tart"

hdr='Active Internet connections (including servers)
Proto Recv-Q Send-Q  Local Address          Foreign Address        (state)'
printf '%s\n%s\n%s\n%s\n' "$hdr" \
	'tcp4       0      0  127.0.0.1.631          *.*                    LISTEN' \
	'tcp6       0      0  ::1.631                *.*                    LISTEN' \
	'tcp4       0      0  10.0.0.5.22            10.0.0.9.50000         ESTABLISHED' >"$t/ns.clean"
printf '%s\n%s\n' "$hdr" 'tcp4       0      0  *.22                   *.*                    LISTEN' >"$t/ns.ssh"
printf '%s\n%s\n' "$hdr" 'tcp6       0      0  *.8080                 *.*                    LISTEN' >"$t/ns.other"
printf '%s\n%s\n' "$hdr" 'tcp4       0      0  192.168.64.3.88        *.*                    LISTEN' >"$t/ns.addr"
rhdr='Routing tables

Internet:
Destination        Gateway            Flags               Netif Expire
127                127.0.0.1          UCS                   lo0
169.254            link#7             UCS                   en0      !'
printf '%s\n' "$rhdr" >"$t/rt.none"
printf '%s\n%s\n' "$rhdr" 'default            fe80::%utun0       UGcIg               utun0' >"$t/rt.utun"
printf '%s\n%s\n' "$rhdr" 'default            192.168.64.1       UGScg                 en0' >"$t/rt.en0"

fail=0
expect() { # RC DESC ENV... -- ARGS...
	_rc=$1 _d=$2; shift 2
	_env=
	while [ "$1" != -- ]; do _env="$_env $1"; shift; done
	shift
	set +e
	# shellcheck disable=SC2086 # _env: VAR=value words made here
	env PATH="$t/bin:$PATH" FAKE_DIR="$t" $_env sh "$check" "$@" >"$t/out" 2>&1
	_got=$?
	set -e
	if [ "$_got" -eq "$_rc" ]; then echo "PASS: $_d"; else echo "FAIL: $_d (exit $_got, want $_rc)"; sed 's/^/    /' "$t/out"; fail=1; fi
}

expect 0 "only loopback listeners, no address: ok" -- t1
expect 2 "tart exec fails: error, not ok" FAKE_NS=fail -- t1
expect 2 "netstat fails in the guest: error, not ok" FAKE_NS=cmdfail -- t1
expect 2 "no output and no status line: error, not ok" FAKE_NS=empty -- t1
expect 0 "netstat succeeds with no TCP socket at all (no header either): ok" FAKE_NS=none-at-all -- t1
expect 2 "the guest agent does not answer: error" FAKE_DOWN=1 -- t1
expect 1 "sshd listens on *.22: fails" FAKE_NS=ssh -- t1
expect 1 "a listener on another port (*.8080): fails" FAKE_NS=other -- t1
expect 1 "a listener on a non-loopback address: fails" FAKE_NS=addr -- t1
expect 0 "--isolated, no default route: ok" FAKE_RT=none -- --isolated t1
expect 0 "--isolated, only a utun default route: ok" FAKE_RT=utun -- --isolated t1
expect 1 "--isolated, a default route over en0: fails" FAKE_RT=en0 -- --isolated t1
expect 2 "--isolated, tart exec of netstat -rn fails: error, not ok" FAKE_RT=fail -- --isolated t1
expect 2 "--isolated, netstat -rn fails in the guest: error, not ok" FAKE_RT=cmdfail -- --isolated t1
expect 2 "--isolated, no output and no status line: error, not ok" FAKE_RT=empty -- --isolated t1
expect 2 "--isolated, netstat -rn without its header: error, not ok" FAKE_RT=none-at-all -- --isolated t1
expect 2 "--close on the golden VM: refused" -- --close golden
expect 2 "--close on a golden-* VM: refused" -- --close ntfsmac-golden-finder
if [ -e "$t/closed" ]; then echo "FAIL: nothing was turned off in a golden VM"; fail=1; else echo "PASS: nothing was turned off in a golden VM"; fi
expect 0 "--close on a clone, then nothing listens: ok" -- --close t1
if grep -q com.openssh.sshd "$t/closed" 2>/dev/null && grep -q com.apple.screensharing "$t/closed"; then
	echo "PASS: --close turns sshd and screen sharing off"
else
	echo "FAIL: --close turns sshd and screen sharing off"; fail=1
fi

[ "$fail" -eq 0 ] && echo "guest_login_check_test: all passed"
exit "$fail"
