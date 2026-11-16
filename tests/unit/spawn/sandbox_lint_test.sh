#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-SBX-09: sandbox_lint.sh must catch what it claims to catch (T-M4-06). The real profiles pass; each
# mutated copy, which has one rule widened, split across lines, hidden after another rule, left
# without its comment or deleted, must fail with the message that names the fault.
#
# Usage: sandbox_lint_test.sh LINT DIR   (LINT is sandbox_lint.sh, DIR holds the real profiles)
# Exit:  0 all mutants were caught, 1 one was not, 2 usage error.
set -u

lint=${1:-}
dir=${2:-}
[ -f "$lint" ] && [ -d "$dir" ] || { echo "usage: sandbox_lint_test.sh LINT DIR" >&2; exit 2; }
tmp=$(mktemp -d "${TMPDIR:-/tmp}/lintmut.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT HUP INT TERM
fails=0
n=0

# fresh: a copy of the real profiles in $tmp/m
fresh() {
	rm -rf "$tmp/m"
	mkdir "$tmp/m"
	cp "$dir"/probe.sb "$dir"/volmark.sb "$dir"/worker.sb "$tmp/m/"
}

# after_default FILE TEXT: TEXT (with \n for line breaks) right after the `(deny default)` rule
after_default() {
	awk -v add="$2" '{ print } /^\(deny default\)$/ { gsub(/\\n/, "\n", add); print add }' "$1" > "$1.new" && mv "$1.new" "$1"
}

# expect NAME MESSAGE: the lint must exit 1 and print MESSAGE
expect() {
	n=$((n + 1))
	out=$(sh "$lint" "$tmp/m" 2>&1)
	rc=$?
	if [ "$rc" -ne 1 ] || ! printf '%s\n' "$out" | grep -q -- "$2"; then
		printf 'FAIL mutant %s: exit %s, wanted exit 1 and "%s"\n%s\n' "$1" "$rc" "$2" "$out" >&2
		fails=$((fails + 1))
	fi
}

fresh
out=$(sh "$lint" "$tmp/m" 2>&1) || { printf 'FAIL the real profiles do not pass:\n%s\n' "$out" >&2; fails=$((fails + 1)); }

fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow process*)'; expect worker-process-star 'allows every process operation'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file-read* (subpath "/"))'; expect worker-read-root 'whole file system'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file-read*\n  (subpath\n    "/"))'; expect worker-read-root-split 'whole file system'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file-write* (subpath "/private/tmp"))'; expect worker-write-star 'every file write'
fresh; sed -e 's|(allow file-read\* file-write-data (literal "/dev/null"))|(allow file-read* file-write* (literal "/dev/null"))|' "$tmp/m/worker.sb" > "$tmp/m/w" && mv "$tmp/m/w" "$tmp/m/worker.sb"; expect worker-null-write-star 'every file write'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file-write-data (literal "/private/tmp/x"))'; expect worker-write-data 'file write rule other than'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow iokit-open)'; expect worker-iokit 'iokit-open'
fresh; after_default "$tmp/m/probe.sb" ';; x\n(allow iokit-open (iokit-user-client-class "X"))'; expect probe-iokit 'iokit-open'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow sysctl*)'; expect worker-sysctl-star 'allows sysctl'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow sysctl-read)'; expect worker-sysctl-read 'allows sysctl'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow\n  process*)'; expect worker-process-split 'allows every process operation'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file-read-metadata (literal "/a")) (allow process*)'; expect worker-two-on-a-line 'FORM-LAYOUT'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file-read-metadata (literal "/a")) (allow process*)'; expect worker-two-on-a-line-op 'every process operation'
fresh; after_default "$tmp/m/worker.sb" ';; x\n (allow process*)'; expect worker-indented 'FORM-LAYOUT'
fresh; grep -v 'CTL_SOCKET_REAL"))))' "$tmp/m/worker.sb" > "$tmp/m/w" && mv "$tmp/m/w" "$tmp/m/worker.sb"; expect worker-no-real-socket-deny 'last three rules'
fresh; grep -v 'XPC_SERVICE")))' "$tmp/m/volmark.sb" > "$tmp/m/w" && mv "$tmp/m/w" "$tmp/m/volmark.sb"; expect volmark-no-xpc-deny 'last three rules'
fresh; printf '\n;; late\n(allow file-read-metadata (literal "/a"))\n' >> "$tmp/m/worker.sb"; expect worker-rule-after-denials 'last three rules'
fresh; after_default "$tmp/m/worker.sb" '(allow file-read-metadata (literal "/a"))'; expect worker-no-comment 'without a comment'
fresh; after_default "$tmp/m/probe.sb" ';; x\n(allow network-outbound)'; expect probe-network 'network allow'
fresh; after_default "$tmp/m/volmark.sb" ';; x\n(allow default)'; expect volmark-default 'allows by default'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow process-exec (literal "/bin/sh"))'; expect worker-exec-sh 'another program'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow process-exec (subpath "/bin"))'; expect worker-exec-subpath 'subpath or pattern'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow mach-lookup)'; expect worker-mach-all 'every Mach service'
fresh; after_default "$tmp/m/probe.sb" ';; x\n(allow process-fork)'; expect probe-fork 'allows fork'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow signal)'; expect worker-signal 'signal allow rule'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file*)'; expect worker-file-star 'every file operation'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow file-ioctl)'; expect worker-ioctl 'does not need'
fresh; after_default "$tmp/m/worker.sb" ';; x\n(allow network*)'; expect worker-network-star 'network allow rule'
fresh; sed -e 's|(param "XPC_SERVICE")|"io.example.privileged"|' "$tmp/m/worker.sb" > "$tmp/m/w" && mv "$tmp/m/w" "$tmp/m/worker.sb"; expect worker-xpc-name-written 'last three rules'

if [ "$fails" -ne 0 ]; then
	echo "sandbox_lint_test: $fails of $n mutants not caught" >&2
	exit 1
fi
echo "sandbox_lint_test: ok ($n mutants caught)"
