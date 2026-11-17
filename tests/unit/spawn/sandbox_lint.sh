#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-SBX-08: rules that the sandbox profiles of integration/sandbox/ must keep (T-M4-06). A profile
# is easy to widen by one line, so the shape that review relied on is checked on every build:
#
#   * deny by default, first, and no `(allow default`;
#   * every rule starts a line and ends its line, so a rule cannot be split across lines or
#     hidden after another one; the checks below read each rule whole, as one string;
#   * every allow rule has a comment above it in the same block (a block ends at a blank line);
#   * an allow rule may use only the operations the profile needs (a list per profile), so
#     `(allow process*)`, `file*`, `iokit-open`, `sysctl*`, `network*`, `signal` and the like fail;
#     `(allow file-read* (subpath "/"))` fails; no profile writes a file, except that the worker
#     writes the data of /dev/null;
#   * probe and volmark have no fork, no exec but their own program and no Mach lookup; the worker
#     has no bare `(allow mach-lookup)`;
#   * the denials of ADR-019 (control socket under both spellings, privileged XPC service) exist
#     and are the last three rules, so they win over everything above them;
#   * the socket path and the service name are parameters, never written in a profile (one
#     definition, in include/ntfsmac/ctl_proto.h);
#   * the worker's UserMapping rule is conditional and takes its path from a parameter.
#
# sandbox_lint_test.sh proves each of these with a mutated copy of the profiles.
#
# Usage: sandbox_lint.sh DIR   (DIR holds probe.sb, volmark.sb, worker.sb)
# Exit:  0 all rules hold, 1 a rule is broken, 2 usage error.
set -u

dir=${1:-}
[ -d "$dir" ] || { echo "usage: sandbox_lint.sh DIR" >&2; exit 2; }
fails=0

bad() { # file message
	printf 'FAIL %s: %s\n' "$1" "$2" >&2
	fails=$((fails + 1))
}

# forms FILE: one line per top-level form, with the newlines and runs of blanks inside it turned
# into single spaces. Comments are cut off first. A form that does not start at the beginning of
# a line, or that shares its last line with another form, is reported on stderr as FORM-LAYOUT.
forms() {
	sed -e 's/;.*$//' "$1" | awk '
		{
			line = $0
			n = length(line)
			for (i = 1; i <= n; i++) {
				c = substr(line, i, 1)
				if (instr) {
					cur = cur c
					if (c == "\"" && substr(line, i - 1, 1) != "\\") instr = 0
					continue
				}
				if (depth == 0) {
					if (c ~ /[ \t]/) continue
					if (c != "(") { printf "FORM-LAYOUT line %d: text outside a rule\n", NR > "/dev/stderr"; continue }
					if (i != 1 && substr(line, 1, i - 1) !~ /^[ \t]*$/) { printf "FORM-LAYOUT line %d: a rule after another rule\n", NR > "/dev/stderr" }
					if (i != 1) { printf "FORM-LAYOUT line %d: a rule that does not start the line\n", NR > "/dev/stderr" }
					cur = ""
				}
				if (c == "\"") instr = 1
				if (c == "(") depth++
				if (c == ")") depth--
				cur = cur c
				if (depth == 0) {
					gsub(/[ \t]+/, " ", cur)
					gsub(/\( /, "(", cur)
					gsub(/ \)/, ")", cur)
					print cur
					cur = ""
					rest = substr(line, i + 1)
					if (rest !~ /^[ \t]*$/) printf "FORM-LAYOUT line %d: a rule is followed by more text on its line\n", NR > "/dev/stderr"
				}
			}
			if (depth > 0) cur = cur " "
		}
		END { if (depth != 0) print "FORM-LAYOUT: unbalanced parentheses" > "/dev/stderr" }
	'
}

# allowed_ops NAME: the operations an allow rule of that profile may name.
allowed_ops() {
	case "$1" in
	probe) echo "process-exec file-read* file-ioctl" ;;
	volmark) echo "process-exec file-read* file-ioctl" ;;
	worker) echo "process-exec process-fork file-read* file-read-data file-read-metadata file-write-data file-ioctl mach-lookup" ;;
	esac
}

for name in probe volmark worker; do
	f=$dir/$name.sb
	if [ ! -f "$f" ]; then
		bad "$name.sb" "missing"
		continue
	fi
	# The layout of the rules; every message of the parser is a failure.
	layout=$(forms "$f" 2>&1 >/dev/null)
	if [ -n "$layout" ]; then
		printf '%s\n' "$layout" | while IFS= read -r l; do printf 'FAIL %s: %s\n' "$name.sb" "$l" >&2; done
		fails=$((fails + 1))
	fi
	rules=$(forms "$f" 2>/dev/null)

	printf '%s\n' "$rules" | head -n 1 | grep -q '^(version 1)$' || bad "$name.sb" "does not start with (version 1)"
	printf '%s\n' "$rules" | sed -n 2p | grep -q '^(deny default)$' || bad "$name.sb" "(deny default) is not the first rule"
	printf '%s\n' "$rules" | grep -q '(allow default' && bad "$name.sb" "allows by default"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*network' && bad "$name.sb" "has a network allow rule"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*signal' && bad "$name.sb" "has a signal allow rule"
	printf '%s\n' "$rules" | grep -q '(allow *)' && bad "$name.sb" "has an allow rule without an operation"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*process\*' && bad "$name.sb" "allows every process operation"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*iokit' && bad "$name.sb" "allows iokit-open"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*file\*' && bad "$name.sb" "allows every file operation"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*sysctl' && bad "$name.sb" "allows sysctl"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*file-read\*[^()]*(subpath "/")' && bad "$name.sb" "allows reading the whole file system"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*file-write\*' && bad "$name.sb" "allows every file write"

	# Only the operations the profile needs.
	ok=$(allowed_ops "$name")
	ops=$(printf '%s\n' "$rules" | grep -o '(allow[^()]*' | sed 's/^(allow *//' | tr ' ' '\n' | grep -v '^$' | sort -u)
	for op in $ops; do
		case " $ok " in
		*" $op "*) ;;
		*) bad "$name.sb" "allows the operation $op, which this profile does not need" ;;
		esac
	done
	# The one file write there is: the data of /dev/null, in the worker.
	printf '%s\n' "$rules" | grep '(allow[^()]*file-write' | grep -v '^(allow file-read\* file-write-data (literal "/dev/null"))$' \
		| grep -q . && bad "$name.sb" "has a file write rule other than the data of /dev/null"

	# Every allow rule (and the conditional that holds one) has a comment line above it in its block.
	awk -v f="$name.sb" '
		/^[[:space:]]*$/ { have = 0; next }
		/^[[:space:]]*;/ { have = 1; next }
		/^\(allow/ || /^\(if/ {
			if (!have) { printf "FAIL %s: line %d: rule without a comment above it: %s\n", f, NR, $0 > "/dev/stderr"; bad = 1 }
			next
		}
		END { exit bad }
	' "$f" || fails=$((fails + 1))

	# The last three rules are the denials of ADR-019, in this order.
	last=$(printf '%s\n' "$rules" | tail -n 3)
	want='(deny network-outbound (remote unix-socket (path-literal (param "CTL_SOCKET"))))
(deny network-outbound (remote unix-socket (path-literal (param "CTL_SOCKET_REAL"))))
(deny mach-lookup (global-name (param "XPC_SERVICE")))'
	[ "$last" = "$want" ] || bad "$name.sb" "the last three rules are not the denials of the control socket (both spellings) and the XPC service"

	# One definition of the two names, in ctl_proto.h.
	grep -q 'ntfsmacd\.sock\|ctl\.privileged' "$f" && bad "$name.sb" "writes the socket path or the XPC service name; use the parameters"
done

# probe and volmark: nothing but their own program, the root directory and device ioctls.
for name in probe volmark; do
	f=$dir/$name.sb
	[ -f "$f" ] || continue
	rules=$(forms "$f" 2>/dev/null)
	printf '%s\n' "$rules" | grep -q '(allow[^()]*process-fork' && bad "$name.sb" "allows fork"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*mach-lookup' && bad "$name.sb" "allows a Mach lookup"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*file-write' && bad "$name.sb" "allows a file write"
	n=$(printf '%s\n' "$rules" | grep -c '(allow[^()]*process-exec')
	[ "$n" -eq 1 ] || bad "$name.sb" "has $n process-exec rules, expected 1 (its own program)"
	printf '%s\n' "$rules" | grep '(allow[^()]*process-exec' | grep -q '(param "EXEC_PATH")' || bad "$name.sb" "its process-exec rule is not for EXEC_PATH"
done

# worker
f=$dir/worker.sb
if [ -f "$f" ]; then
	rules=$(forms "$f" 2>/dev/null)
	printf '%s\n' "$rules" | grep -q '(allow[^()]*mach-lookup)' && bad worker.sb "allows every Mach service"
	printf '%s\n' "$rules" | grep -q '(allow[^()]*file-read\*)' && bad worker.sb "allows reading every file"
	printf '%s\n' "$rules" | grep -q 'UserMapping' && bad worker.sb "writes the UserMapping path; use the parameter"
	printf '%s\n' "$rules" | grep -q '(if (string=? (param "USERMAPPING") "yes")' || bad worker.sb "the UserMapping rule is not conditional on the option"
	printf '%s\n' "$rules" | grep -q '(literal (param "USERMAPPING_PATH"))' || bad worker.sb "no UserMapping rule that takes its path from USERMAPPING_PATH"
	# The one device ioctl: DKIOCSYNCHRONIZE (_IO "d" 22) on disk device nodes, as in volmark.sb.
	printf '%s\n' "$rules" | grep '(allow[^()]*file-ioctl' \
		| grep -v '^(allow file-ioctl (require-all (regex #"^/dev/r?disk\[0-9\]+(s\[0-9\]+)?\$") (ioctl-command (_IO "d" 22))))$' \
		| grep -q . && bad worker.sb "has a file-ioctl rule other than DKIOCSYNCHRONIZE on disk device nodes"
	# Starting a program: only EXEC_PATH and the macFUSE helper, by literal path.
	printf '%s\n' "$rules" | grep -o '(allow process-exec[^()]*([a-z]* [^()]*))' \
		| grep -v 'EXEC_PATH\|macfuse.app/Contents/MacOS/macfuse"' | grep -q . && bad worker.sb "allows starting another program"
	# Every process-exec rule must name a literal: a subpath or a regex would start more programs.
	printf '%s\n' "$rules" | grep '(allow[^()]*process-exec' | grep -q '(subpath\|(regex\|(prefix' && bad worker.sb "starts programs by subpath or pattern"
fi

if [ "$fails" -ne 0 ]; then
	echo "sandbox_lint: $fails rule(s) broken" >&2
	exit 1
fi
echo "sandbox_lint: ok"
