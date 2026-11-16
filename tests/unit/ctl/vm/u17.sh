#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U17 and the signature part of ADR-019 (T-M4-04), run INSIDE a disposable
# macOS VM only. It changes the guest's authorization database, so never run
# it on a host. The VM is cloned from the golden image and deleted afterwards.
#
# Usage (in the guest):  sh u17.sh PATH-TO-ctl_vm_probe [PART ...]
#   parts: A B1 B2 (default: all)
# Needs: sudo without a password (true for the test login of the image),
#        codesign, security, launchctl.
#
# Parts:
#   A   check 3: code requirement and hardened runtime, on ad-hoc builds
#   B1  check 4 mechanics: an external form crosses from an unprivileged
#       process to root, with a right of class allow
#   B2  check 4 with the right as the package must install it (class user,
#       group admin, shared false, timeout 0): what the daemon does with
#       references that lack the right, and what the prompt does without a
#       GUI session
# Do not add a part that answers a password prompt: a person must do that.
set -u

probe=${1:?usage: sh u17.sh PATH-TO-ctl_vm_probe [PART ...]}
shift
parts=${*:-A B1 B2}
t=$(mktemp -d /tmp/u17.XXXXXX) || exit 2
trap 'rm -rf "$t"' EXIT

say() { printf '\n== %s\n' "$*"; }

say "guest"
sw_vers -productVersion
uname -m
if launchctl print "gui/$(id -u)" >/dev/null 2>&1; then echo "gui session of this shell: present"; else echo "gui session of this shell: absent"; fi
cp "$probe" "$t/p"

part_A() {
	say "A. check 3: signature requirement and hardened runtime"
	cp "$t/p" "$t/clientA"; codesign --force -s - -o runtime "$t/clientA" 2>&1
	cp "$t/p" "$t/clientB"; codesign --force -s - "$t/clientB" 2>&1
	cp "$t/p" "$t/clientC"; codesign --force -s - -o runtime -i org.example.other "$t/clientC" 2>&1
	cdhash() { codesign -d -vvv "$1" 2>&1 | sed -n 's/^CDHash=//p'; }
	hashA=$(cdhash "$t/clientA"); hashB=$(cdhash "$t/clientB"); hashC=$(cdhash "$t/clientC")
	echo "cdhash A (ad-hoc, hardened runtime): $hashA"
	echo "cdhash B (ad-hoc, no runtime): $hashB"
	echo "cdhash C (re-signed, other identifier, runtime): $hashC"

	# sigcase NAME CLIENT HASH-IN-REQUIREMENT
	sigcase() {
		name=$1; client=$2; hash=$3
		printf 'cdhash H"%s"\n' "$hash" >"$t/req"
		chmod 644 "$t/req"
		rm -f "$t/sock"
		"$t/p" sig-server "$t/req" "$t/sock" >"$t/out" 2>&1 &
		srv=$!
		n=0; while [ ! -S "$t/sock" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
		"$client" sig-client "$t/sock" 3 &
		cli=$!
		wait $srv
		wait $cli 2>/dev/null
		printf '%s: ' "$name"; tr '\n' ' ' <"$t/out"; echo
	}
	sigcase "client A, requirement = cdhash of A (want YES)" "$t/clientA" "$hashA"
	sigcase "client B, requirement = cdhash of B, no runtime (want NO)" "$t/clientB" "$hashB"
	sigcase "client C, requirement = cdhash of A, re-signed copy (want NO)" "$t/clientC" "$hashA"
	sigcase "client A, requirement = cdhash of C (want NO)" "$t/clientA" "$hashC"
}

part_B1() {
	say "B1. right of class allow: the form crosses to a root process"
	sudo -n security authorizationdb write org.ntfs-macos.test-allow allow >/dev/null 2>&1
	echo "built-in rule authenticate-admin-nonshared (what a right that names it inherits):"
	sudo -n security authorizationdb read authenticate-admin-nonshared 2>/dev/null |
		grep -A1 -E '<key>(class|timeout|shared|group)</key>' | tr -d '\t' | tr '\n' ' '
	echo
	rm -f "$t/form"
	"$t/p" authz-make org.ntfs-macos.test-allow copy "$t/form" 6 >"$t/mk" 2>&1 &
	mk=$!
	n=0; while [ ! -s "$t/form" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
	echo "client side:"; sed 's/^/  /' "$t/mk"
	echo "root, first check (reference alive):"
	sudo -n "$t/p" authz-check org.ntfs-macos.test-allow "$(cat "$t/form")" 2>&1 | sed 's/^/  /'
	echo "root, second check with the same form:"
	sudo -n "$t/p" authz-check org.ntfs-macos.test-allow "$(cat "$t/form")" 2>&1 | sed 's/^/  /'
	wait $mk
	echo "root, check after the client freed its reference:"
	sudo -n "$t/p" authz-check org.ntfs-macos.test-allow "$(cat "$t/form")" 2>&1 | sed 's/^/  /'
	sudo -n security authorizationdb remove org.ntfs-macos.test-allow >/dev/null 2>&1
}

part_B2() {
	# The right as the package must install it: its own definition with timeout 0.
	# Pointing at the built-in rule would inherit its timeout (see B1).
	cat >"$t/right.plist" <<'EOF2'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>class</key><string>user</string>
	<key>comment</key><string>Clear a session journal entry or repair an NTFS volume with ntfsmac.</string>
	<key>group</key><string>admin</string>
	<key>shared</key><false/>
	<key>timeout</key><integer>0</integer>
	<key>authenticate-user</key><true/>
	<key>version</key><integer>1</integer>
</dict></plist>
EOF2
	say "B2. real right: class user, group admin, shared false, timeout 0"
	sudo -n security authorizationdb write org.ntfs-macos.journal-modify <"$t/right.plist" >/dev/null 2>&1
	sudo -n security authorizationdb read org.ntfs-macos.journal-modify 2>/dev/null |
		grep -A1 -E '<key>(class|timeout|shared|group)</key>' | tr -d '\t' | tr '\n' ' '
	echo

	echo "CLI side, interaction not allowed:"
	rm -f "$t/form3"
	"$t/p" authz-make org.ntfs-macos.journal-modify copy "$t/form3" 1 2>&1 | sed 's/^/  /'

	echo "root checks a reference that does not hold the right (no prompt may appear):"
	rm -f "$t/form4"
	"$t/p" authz-make org.ntfs-macos.journal-modify none "$t/form4" 4 >"$t/mk4" 2>&1 &
	mk4=$!
	n=0; while [ ! -s "$t/form4" ] && [ $n -lt 50 ]; do sleep 0.1; n=$((n + 1)); done
	sudo -n "$t/p" authz-check org.ntfs-macos.journal-modify "$(cat "$t/form4")" 2>&1 | sed 's/^/  /'
	wait $mk4
	echo "root checks a made-up form:"
	sudo -n "$t/p" authz-check org.ntfs-macos.journal-modify \
		00112233445566778899aabbccddeeff00112233445566778899aabbccddeeff 2>&1 | sed 's/^/  /'

	# The prompt itself. A job in the system launchd domain has no GUI session,
	# like a login over SSH: the call must fail, not wait for a dialog.
	echo "prompt, no GUI session (a job in the system domain; waits at most 25 s):"
	rm -f "$t/job.out" "$t/job.err" "$t/form2"
	sudo -n launchctl submit -l org.example.u17 -o "$t/job.out" -e "$t/job.err" -- \
		"$t/p" authz-make org.ntfs-macos.journal-modify prompt "$t/form2" 1
	n=0; while ! grep -q copy_rights "$t/job.out" 2>/dev/null && [ $n -lt 125 ]; do sleep 0.2; n=$((n + 1)); done
	sed 's/^/  /' "$t/job.out" 2>/dev/null
	sed 's/^/  err: /' "$t/job.err" 2>/dev/null
	grep -q copy_rights "$t/job.out" 2>/dev/null || echo "  no answer within 25 s (the call is waiting for a prompt)"
	sudo -n launchctl remove org.example.u17 >/dev/null 2>&1
	sudo -n security authorizationdb remove org.ntfs-macos.journal-modify >/dev/null 2>&1
}

for p in $parts; do "part_$p"; done
say "done"
