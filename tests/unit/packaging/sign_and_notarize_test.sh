#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-05 (T-M6-02): argument handling and dry run of packaging/sign-and-notarize.sh. It never
# signs, notarizes or reads a keychain: a real run is refused here (no credentials are given) or
# is a dry run, which executes nothing. The tests use fake identity names.
#
# Usage: sign_and_notarize_test.sh SCRIPT
# Exit: 0 all passed, 1 a check failed, 2 usage error.
set -u

script=${1:?usage: sign_and_notarize_test.sh SCRIPT}
[ -f "$script" ] || { echo "no script: $script"; exit 2; }
fail=0
pass=0

APP='Developer ID Application: Test Name (TEST000000)'
INST='Developer ID Installer: Test Name (TEST000000)'
tmp=$(mktemp -d "${TMPDIR:-/tmp}/sign-test.XXXXXX") || exit 2
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

# check NAME WANT_EXIT PATTERN COMMAND...: the exit status must equal WANT_EXIT and the combined
# output must match the grep pattern (use . for any).
check() {
	name=$1 want=$2 pat=$3
	shift 3
	"$@" >"$tmp/out" 2>&1
	rc=$?
	if [ "$rc" != "$want" ]; then
		echo "FAIL: $name: exit $rc, want $want"
		sed 's/^/    /' "$tmp/out"
		fail=$((fail + 1))
	elif ! grep -q -- "$pat" "$tmp/out"; then
		echo "FAIL: $name: output lacks '$pat'"
		sed 's/^/    /' "$tmp/out"
		fail=$((fail + 1))
	else
		pass=$((pass + 1))
	fi
}
# absent NAME PATTERN: the last output must not match.
absent() {
	if grep -q -- "$2" "$tmp/out"; then
		echo "FAIL: $1: output has '$2'"
		fail=$((fail + 1))
	else
		pass=$((pass + 1))
	fi
}

check no-args 2 'usage:' sh "$script"
check unknown-arg 2 'unknown argument' sh "$script" --bogus
check not-a-pkg 2 'must be named ntfs-macos' sh "$script" --pkg x.txt --profile p --app-identity "$APP" --installer-identity "$INST"
check wrong-name 2 'must be named ntfs-macos' sh "$script" --pkg other.pkg --profile p --app-identity "$APP" --installer-identity "$INST"
check value-missing 2 'needs a value' sh "$script" --pkg
check value-is-option 2 'needs a value' sh "$script" --pkg --profile
check help 0 'usage:' sh "$script" --help

# Missing credentials: exit 3, names the missing arguments, points to the unsigned path.
check no-credentials 3 'missing credentials' sh "$script" --pkg ntfs-macos-x.pkg
check no-credentials-names 3 '--profile --app-identity --installer-identity' sh "$script" --pkg ntfs-macos-x.pkg
check no-credentials-hint 3 'ADR-011' sh "$script" --pkg ntfs-macos-x.pkg
check no-credentials-gatekeeper 3 'Gatekeeper blocks' sh "$script" --pkg ntfs-macos-x.pkg
check no-profile 3 '--profile' sh "$script" --pkg ntfs-macos-x.pkg --app-identity "$APP" --installer-identity "$INST"
check no-installer 3 '--installer-identity' sh "$script" --pkg ntfs-macos-x.pkg --profile p --app-identity "$APP"
# A dry run does not excuse a missing credential.
check dry-no-credentials 3 'missing credentials' sh "$script" --dry-run --pkg ntfs-macos-x.pkg

# Swapped identities are refused.
check swapped-app 2 'Developer ID Application' sh "$script" --pkg ntfs-macos-x.pkg --profile p --app-identity "$INST" --installer-identity "$INST"
check swapped-inst 2 'Developer ID Installer' sh "$script" --pkg ntfs-macos-x.pkg --profile p --app-identity "$APP" --installer-identity "$APP"

# Real mode with a missing package or missing tools must run nothing: exit 1 (no package) or 3
# (a tool such as notarytool is not installed here), and nothing is written.
sh "$script" --pkg "$tmp/in/ntfs-macos-x.pkg" --profile p --app-identity "$APP" --installer-identity "$INST" --out-dir "$tmp/o" >"$tmp/out" 2>&1
rc=$?
case $rc in
1 | 3) pass=$((pass + 1)) ;;
*) echo "FAIL: real run without the package: exit $rc, want 1 or 3"; fail=$((fail + 1)) ;;
esac
if [ -e "$tmp/o" ]; then
	echo "FAIL: real run wrote $tmp/o"
	fail=$((fail + 1))
else
	pass=$((pass + 1))
fi

# --out-dir: the parent must exist, and it must not be the input's directory.
check outdir-no-parent 2 'parent of --out-dir' sh "$script" --dry-run --pkg "$tmp/ntfs-macos-x.pkg" --profile p --app-identity "$APP" --installer-identity "$INST" --out-dir "$tmp/no/such/dir"
check outdir-is-input 2 'must not be the directory' sh "$script" --dry-run --pkg "$tmp/ntfs-macos-x.pkg" --profile p --app-identity "$APP" --installer-identity "$INST" --out-dir "$tmp"

# Dry run: every step is printed in order, nothing is executed, the package need not exist.
check dry-run 0 'dry run done' sh "$script" --dry-run --pkg "$tmp/ntfs-macos-x.pkg" --profile myprofile --app-identity "$APP" --installer-identity "$INST"
cp "$tmp/out" "$tmp/dry.txt"
# has TEXT DESCRIPTION: the dry-run output must hold the fixed string TEXT.
has() {
	if grep -qF -- "$1" "$tmp/dry.txt"; then
		pass=$((pass + 1))
	else
		echo "FAIL: dry run lacks $2 ($1)"
		fail=$((fail + 1))
	fi
}
has "'--expand-full'" 'expand'
has "--options runtime" 'hardened runtime'
has "--entitlements" 'entitlements for libfuse programs'
has "entitlements-libfuse.plist" 'entitlements file'
has "/usr/local/lib/libfuse" 'libfuse rule'
has "'--ownership' 'recommended'" 'pkgbuild ownership'
has "'productbuild'" 'productbuild'
has "lsbom -p MUGf" 'bill of materials check'
has "'productsign' '--sign' '$INST'" 'productsign identity'
has "'--keychain-profile' 'myprofile' '--wait'" 'notarytool submit'
has "'notarytool' 'log'" 'notarytool log on failure'
has "'stapler' 'staple'" 'staple'
has "'stapler' 'validate'" 'stapler validate'
has "'-a' '-t' 'install' '-vv'" 'spctl'
has "$tmp/signed/ntfs-macos-x.pkg" 'output name ntfs-macos-<label>.pkg in signed/'
has "SHA256SUMS" 'SHA256SUMS'
has "BUILD-INFO" 'BUILD-INFO'
# The flatten that writes the user's uid must not be used.
if grep -q -- 'flatten-full' "$tmp/dry.txt"; then
	echo "FAIL: dry run uses flatten-full"
	fail=$((fail + 1))
else
	pass=$((pass + 1))
fi
# Order of the main steps by line number.
prev=0
ok=1
for w in "'--expand-full'" "options runtime" "'pkgbuild'" "'productbuild'" "lsbom" "'productsign'" "'notarytool' 'submit'" "'staple'" "'spctl'" "SHA256SUMS"; do
	n=$(grep -nF -- "$w" "$tmp/dry.txt" | sed -n '1s/:.*//p')
	if [ -z "$n" ] || [ "$n" -le "$prev" ]; then
		ok=0
		echo "FAIL: dry run step '$w' out of order (line ${n:-none})"
	fi
	prev=${n:-$prev}
done
if [ "$ok" = 1 ]; then pass=$((pass + 1)); else fail=$((fail + 1)); fi
# The dry run never calls the keychain tool.
if grep -q '^+.*security' "$tmp/dry.txt"; then
	echo "FAIL: dry run uses security"
	fail=$((fail + 1))
else
	pass=$((pass + 1))
fi
# --out-dir is honoured.
check dry-outdir 0 "$tmp/custom/ntfs-macos-x.pkg" sh "$script" --dry-run --pkg "$tmp/ntfs-macos-x.pkg" --profile p --app-identity "$APP" --installer-identity "$INST" --out-dir "$tmp/custom"

# The entitlements file is a valid plist with the one entitlement (plutil exists on macOS only).
ents=$(dirname -- "$script")/entitlements-libfuse.plist
if [ -f "$ents" ]; then
	pass=$((pass + 1))
else
	echo "FAIL: no $ents"
	fail=$((fail + 1))
fi
if grep -q 'com.apple.security.cs.disable-library-validation' "$ents"; then
	pass=$((pass + 1))
else
	echo "FAIL: entitlement missing in $ents"
	fail=$((fail + 1))
fi
if command -v plutil >/dev/null 2>&1; then
	if plutil -lint "$ents" >/dev/null 2>&1; then pass=$((pass + 1)); else echo "FAIL: plutil -lint $ents"; fail=$((fail + 1)); fi
fi

# The script file itself must not hold a credential-looking literal.
if grep -q 'TEST000000\|keychain-password' "$script"; then
	echo "FAIL: script holds a credential literal"
	fail=$((fail + 1))
else
	pass=$((pass + 1))
fi

echo "sign_and_notarize_test: $pass passed, $fail failed"
[ "$fail" = 0 ]
