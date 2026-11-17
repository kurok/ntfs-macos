#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-02 (T-M4-10): the manifest lint is tested with altered copies of the manifest. Each mutant
# must be refused; the unaltered manifest must pass. Reads files and writes only to a temporary
# directory; it never runs the installer or the uninstaller.
#
# Usage: manifest_lint_test.sh LINT ROOT
set -u

lint=${1:?usage: manifest_lint_test.sh LINT ROOT}
root=${2:?usage: manifest_lint_test.sh LINT ROOT}
manifest=$root/packaging/manifest.txt
tmp=$(mktemp -d "${TMPDIR:-/tmp}/manifest_lint_test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT
fail=0

sh "$lint" "$root" "$manifest" >/dev/null || { echo "FAIL: the unaltered manifest is refused"; exit 1; }
echo "PASS: the unaltered manifest passes"

# mutant NAME SED-SCRIPT: the altered manifest must be refused
mutant() {
	sed "$2" "$manifest" >"$tmp/m.txt"
	if cmp -s "$manifest" "$tmp/m.txt"; then echo "FAIL: mutant $1 changed nothing"; fail=1; return; fi
	if sh "$lint" "$root" "$tmp/m.txt" >"$tmp/out" 2>&1; then
		echo "FAIL: mutant $1 was accepted"; fail=1
	else
		echo "PASS: mutant $1 refused"
	fi
}
# add NAME LINE: the manifest plus one line must be refused
added() {
	{ cat "$manifest"; printf '%s\n' "$2"; } >"$tmp/m.txt"
	if sh "$lint" "$root" "$tmp/m.txt" >"$tmp/out" 2>&1; then
		echo "FAIL: mutant $1 was accepted"; fail=1
	else
		echo "PASS: mutant $1 refused"
	fi
}

mutant "a profile of the tree is missing" '/sandbox\/worker.sb$/d'
mutant "the plist is missing" '/org.ntfs-macos.ntfsmacd.plist$/d'
mutant "the worker is missing" '/ntfsmac-worker$/d'
mutant "the CLI is missing" '/usr\/local\/bin\/ntfsmac$/d'
mutant "the right is missing" '/^right /d'
mutant "the sandbox directory is missing" '/^dir .*\/sandbox$/d'
mutant "a program is not executable" 's|^file 0755 \(root wheel /usr/local/libexec/ntfs-macos/ntfsmac-probe\)|file 0644 \1|'
mutant "a profile is group-writable" 's|^file 0644 \(root wheel .*/worker.sb\)|file 0664 \1|'
mutant "a profile is owned by a user" 's|^file 0644 root wheel \(.*/probe.sb\)|file 0644 admin staff \1|'
mutant "the mode is not octal" 's|^file 0644 \(root wheel .*/volmark.sb\)|file 0x44 \1|'
mutant "an unknown type" 's|^statefile |tree     |'

added "a path outside the project" "file 0644 root wheel /etc/hosts"
added "a path in the shared bin directory" "file 0755 root wheel /usr/local/bin/ls"
added "a dir that is shared" "dir 0755 root wheel /usr/local/bin"
added "a dot-dot path" "file 0644 root wheel /usr/local/libexec/ntfs-macos/../../../etc/passwd"
added "a relative path" "file 0644 root wheel usr/local/libexec/ntfs-macos/x"
added "a double slash" "file 0644 root wheel /usr/local/libexec/ntfs-macos//x"
added "a duplicate" "file 0755 root wheel /usr/local/libexec/ntfs-macos/ntfsmacd"
added "a second right" "right - - - org.ntfs-macos.other"
added "a state path elsewhere" "statedir 0755 root wheel /var/db"
added "a system launchd plist" "file 0644 root wheel /Library/LaunchDaemons/com.apple.something.plist"
added "a file with no source in the installer" "file 0644 root wheel /usr/local/libexec/ntfs-macos/extra.bin"

exit "$fail"
