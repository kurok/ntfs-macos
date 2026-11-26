#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# U-PKG-10 (T-M7-08): the detached signature of SHA256SUMS (scripts/sign-release.sh) and the rules
# around it. Host only, no VM. A throw-away ed25519 key is made in a temporary directory, used, and
# deleted; no real key, keychain or ssh-agent is touched (SSH_AUTH_SOCK is unset).
#
# Usage: release_signing_test.sh REPO_ROOT
set -eu

root=${1:?usage: release_signing_test.sh REPO_ROOT}
sign=$root/scripts/sign-release.sh
unset SSH_AUTH_SOCK
t=$(mktemp -d "${TMPDIR:-/tmp}/release-signing-test.XXXXXX")
trap 'rm -rf "$t"' EXIT

fail=0
pass() { printf 'PASS: %s\n' "$1"; }
bad() { printf 'FAIL: %s\n' "$1"; fail=1; }
# chk DESC CMD...: CMD must succeed
chk() { _d=$1; shift; if "$@"; then pass "$_d"; else bad "$_d"; fi; }
# expect RC DESC CMD...: CMD must exit with RC
expect() {
	_rc=$1 _d=$2; shift 2
	set +e; "$@" >"$t/out" 2>&1; _got=$?; set -e
	if [ "$_got" -eq "$_rc" ]; then pass "$_d"; else bad "$_d (exit $_got, want $_rc)"; sed 's/^/    /' "$t/out"; fi
}

ns=ntfs-macos-release
ssh-keygen -q -t ed25519 -N '' -C throwaway-test -f "$t/key"
ssh-keygen -q -t ed25519 -N '' -C throwaway-attacker -f "$t/other"
printf '# test signers\n%s namespaces="%s" %s\n' "$ns" "$ns" "$(cut -d' ' -f1,2 "$t/key.pub")" >"$t/signers"
: >"$t/empty-signers"

files='ntfs-macos-1.0.pkg uninstall.sh manifest.txt BUILD-INFO ntfs-macos-1.0-source.tar.gz'
mkrel() { # mkrel DIR [nolabel]: a release directory with SHA256SUMS as release-local.sh writes it
	rm -rf "$1"; mkdir -p "$1"
	printf 'package bytes\n' >"$1/ntfs-macos-1.0.pkg"
	printf '#!/bin/sh\n' >"$1/uninstall.sh"
	printf 'file x\n' >"$1/manifest.txt"
	printf 'revision x\n' >"$1/BUILD-INFO"
	printf 'source\n' >"$1/ntfs-macos-1.0-source.tar.gz"
	# shellcheck disable=SC2086 # files: fixed names
	(cd "$1" && { [ "${2:-}" = nolabel ] || echo '# release 1.0'; shasum -a 256 $files; } >SHA256SUMS)
}

# --- signing and verifying ---
r=$t/rel
mkrel "$r"
expect 0 "sign with a key from the signers file" sh "$sign" --key "$t/key" --signers "$t/signers" "$r"
chk "SHA256SUMS.sig written" test -s "$r/SHA256SUMS.sig"
chk "sign reports the verified signature" grep -q "SHA256SUMS.sig verifies" "$t/out"
fp=$(ssh-keygen -lf "$t/key.pub" | awk '{ print $2 }')
chk "sign prints the key fingerprint ($fp)" grep -qF "key ED25519 $fp" "$t/out"
chk "sign prints the release label" grep -qF "release 1.0," "$t/out"
expect 0 "--verify of the signed directory" sh "$sign" --verify --signers "$t/signers" "$r"
# the exact command a user runs (docs/INSTALL.md)
expect 0 "the user's ssh-keygen -Y verify command" sh -c \
	"ssh-keygen -Y verify -f '$t/signers' -I $ns -n $ns -s '$r/SHA256SUMS.sig' <'$r/SHA256SUMS'"

# --- a changed file fails ---
cp -R "$r" "$t/r1"
printf 'x' >>"$t/r1/ntfs-macos-1.0.pkg"
expect 1 "a changed package fails --verify" sh "$sign" --verify --signers "$t/signers" "$t/r1"

# --- a replaced package with a recomputed SHA256SUMS (the release-page attack) fails ---
cp -R "$r" "$t/r2"
printf 'evil\n' >"$t/r2/ntfs-macos-1.0.pkg"
# shellcheck disable=SC2086 # files: fixed names
(cd "$t/r2" && { echo '# release 1.0'; shasum -a 256 $files; } >SHA256SUMS)
expect 1 "replaced package and SHA256SUMS fail the signature" sh "$sign" --verify --signers "$t/signers" "$t/r2"
expect 255 "the user's command rejects the replaced SHA256SUMS" sh -c \
	"ssh-keygen -Y verify -f '$t/signers' -I $ns -n $ns -s '$t/r2/SHA256SUMS.sig' <'$t/r2/SHA256SUMS'"

# --- ... and re-signed with another key, it still fails ---
expect 1 "a signature by a key not in the signers file fails" sh "$sign" --key "$t/other" --signers "$t/signers" "$t/r2"

# --- a signature for another namespace fails ---
cp -R "$r" "$t/r3"
rm -f "$t/r3/SHA256SUMS.sig"
ssh-keygen -q -Y sign -f "$t/key" -n file "$t/r3/SHA256SUMS"
expect 1 "a signature in another namespace fails" sh "$sign" --verify --signers "$t/signers" "$t/r3"

# --- a missing file or a path in SHA256SUMS ---
cp -R "$r" "$t/r4"
rm "$t/r4/manifest.txt"
expect 1 "a file named in SHA256SUMS is missing" sh "$sign" --key "$t/key" --signers "$t/signers" "$t/r4"
mkrel "$t/r5"
printf '%s  ../escape\n' "$(shasum -a 256 "$t/r5/uninstall.sh" | cut -c1-64)" >>"$t/r5/SHA256SUMS"
expect 1 "a path outside the directory in SHA256SUMS is refused" sh "$sign" --key "$t/key" --signers "$t/signers" "$t/r5"

# --- the release label (replay and downgrade) ---
cp -R "$r" "$t/r7"
sed -i '' 's/^# release 1.0$/# release 2.0/' "$t/r7/SHA256SUMS"
expect 1 "a relabelled SHA256SUMS fails the signature" sh "$sign" --verify --signers "$t/signers" "$t/r7"
mkrel "$t/r8" nolabel
expect 1 "no '# release' line: refused" sh "$sign" --key "$t/key" --signers "$t/signers" "$t/r8"
expect 0 "--label adds the release line" sh "$sign" --key "$t/key" --label 1.0 --signers "$t/signers" "$t/r8"
chk "the release line is the first line" test "$(head -1 "$t/r8/SHA256SUMS")" = "# release 1.0"
mkrel "$t/r9"
expect 1 "--label differs from the release line: refused" sh "$sign" --key "$t/key" --label 1.1 --signers "$t/signers" "$t/r9"
mkrel "$t/r10"
printf '# release 1.0\n' >>"$t/r10/SHA256SUMS"
expect 1 "two release lines: refused" sh "$sign" --key "$t/key" --signers "$t/signers" "$t/r10"
mkrel "$t/r11"
sed -i '' 's/^# release 1.0$/# release 0.9/' "$t/r11/SHA256SUMS"
expect 1 "a label whose package is not listed: refused" sh "$sign" --key "$t/key" --signers "$t/signers" "$t/r11"
expect 2 "--label with --verify" sh "$sign" --verify --label 1.0 --signers "$t/signers" "$r"

# --- the signers file: other valid forms, and the fingerprint is that of the signing key only ---
pub=$(cut -d' ' -f1,2 "$t/key.pub")
opub=$(cut -d' ' -f1,2 "$t/other.pub")
printf '%s namespaces="%s" %s\n%s,someone namespaces="file,%s",valid-after="20200101" %s a comment\n' \
	"$ns" "$ns" "$opub" "$ns" "$ns" "$pub" >"$t/signers2"
mkrel "$t/r12"
expect 0 "signers: principal list, namespace list, more options, a comment" sh "$sign" --key "$t/key" --signers "$t/signers2" "$t/r12"
ofp=$(ssh-keygen -lf "$t/other.pub" | awk '{ print $2 }')
if grep -qF "$ofp" "$t/out"; then bad "only the signing key's fingerprint is printed"; else pass "only the signing key's fingerprint is printed"; fi
printf '%s %s\n' "$ns" "$pub" >"$t/signers3"
mkrel "$t/r13"
expect 0 "signers: a line without options" sh "$sign" --key "$t/key" --signers "$t/signers3" "$t/r13"
printf '%s namespaces="file" %s\n' "$ns" "$pub" >"$t/signers4"
mkrel "$t/r14"
expect 3 "signers: a key limited to another namespace is no release key" sh "$sign" --key "$t/key" --signers "$t/signers4" "$t/r14"

# --- usage and setup ---
mkrel "$t/r6"
expect 2 "no --key: refused (no default key)" sh "$sign" --signers "$t/signers" "$t/r6"
expect 2 "a key file that does not exist" sh "$sign" --key "$t/nokey" --signers "$t/signers" "$t/r6"
expect 3 "a signers file without a release key: owner setup needed" sh "$sign" --key "$t/key" --signers "$t/empty-signers" "$t/r6"
chk "nothing signed on a refusal" test ! -e "$t/r6/SHA256SUMS.sig"
expect 2 "--key with --verify" sh "$sign" --verify --key "$t/key" --signers "$t/signers" "$t/r6"

# --- release-local.sh: no package without --sign-key or --unsigned (run from a copy outside any repository,
#     so a broken check can never start a build) ---
mkdir -p "$t/x/scripts/lib"
cp "$root/scripts/release-local.sh" "$t/x/scripts/"
cp "$root"/scripts/lib/*.sh "$t/x/scripts/lib/"
expect 2 "release-local.sh without --sign-key or --unsigned" sh "$t/x/scripts/release-local.sh" --no-ci
chk "the refusal names --sign-key" grep -q -- "--sign-key" "$t/out"
expect 2 "release-local.sh with --sign-key and --unsigned" sh "$t/x/scripts/release-local.sh" --sign-key "$t/key" --unsigned
expect 2 "release-local.sh with a --sign-key that does not exist" sh "$t/x/scripts/release-local.sh" --sign-key "$t/nokey"
for o in --host --no-verify --allow-dirty; do
	expect 2 "release-local.sh refuses --sign-key with $o" sh "$t/x/scripts/release-local.sh" --sign-key "$t/key" "$o"
done

# --- the tree: the committed signers file, no private key, no key path default ---
chk "packaging/release-signers exists" test -f "$root/packaging/release-signers"
if (cd "$root" && git ls-files -z 2>/dev/null | xargs -0 grep -l -e '-----BEGIN OPENSSH PRIVATE KEY-----' -e '-----BEGIN [A-Z ]*PRIVATE KEY-----' 2>/dev/null |
	grep -v '^tests/unit/packaging/release_signing_test.sh$'); then
	bad "no private key in the tracked files"
else
	pass "no private key in the tracked files"
fi
if grep -nE '(key|sign_key)=[^ ]*(\.ssh|id_|\.pem|signing)' "$root/scripts/release-local.sh" "$sign"; then
	bad "no key path default in release-local.sh or sign-release.sh"
else
	pass "no key path default in release-local.sh or sign-release.sh"
fi

# --- doc lint: docs/INSTALL.md shows the signature check, before the install command ---
inst=$root/docs/INSTALL.md
vline=$(grep -n "ssh-keygen -Y verify -f release-signers -I $ns -n $ns -s SHA256SUMS.sig" "$inst" | head -1 | cut -d: -f1)
iline=$(grep -n 'sudo installer -pkg' "$inst" | head -1 | cut -d: -f1)
if [ -n "$vline" ] && [ -n "$iline" ] && [ "$vline" -lt "$iline" ]; then
	pass "docs/INSTALL.md: the signature check comes before the install command"
else
	bad "docs/INSTALL.md: the signature check (line '${vline}') must come before 'sudo installer -pkg' (line '${iline}')"
fi
chk "docs/INSTALL.md names SHA256SUMS.sig" grep -q 'SHA256SUMS.sig' "$inst"
chk "packaging/README.md shows the verify step" grep -q 'ssh-keygen -Y verify' "$root/packaging/README.md"
chk "the release notes template names the key fingerprint" grep -q 'Signing key fingerprint' "$root/docs/releases/TEMPLATE.md"

# --- every command of docs/INSTALL.md "Download and check" and packaging/README.md "Release signature"
#     runs as written: the owner's setup (with HOME in the temporary directory; the key is made without a
#     passphrase and git clone is replaced by a local copy, the only two changes), signing, then the user's
#     check in a folder with the downloaded files ---
# shcmds FILE HEADING: the lines of the ```sh blocks of the section HEADING (to the next heading of its level)
shcmds() {
	awk -v h="$2" '
		BEGIN { lvl = h; sub(/ .*/, "", lvl) }
		$0 == h { on = 1; next }
		on && /^#+ / { l = $0; sub(/ .*/, "", l); if (length(l) <= length(lvl)) exit }
		on && /^[[:space:]]*```sh[[:space:]]*$/ { blk = 1; next }
		on && blk && /^[[:space:]]*```[[:space:]]*$/ { blk = 0; next }
		on && blk { sub(/^[[:space:]]+/, ""); if ($0 != "") print }' "$1"
}
shcmds "$root/packaging/README.md" "## Release signature (T-M7-08, ADR-011 amendment 2026-11-26)" >"$t/readme.cmds"
shcmds "$inst" "## Download and check" >"$t/install.cmds"
chk "README section has commands" test -s "$t/readme.cmds"
chk "INSTALL section has commands" test -s "$t/install.cmds"
H=$t/home O=$t/owner U=$t/user
mkdir -p "$H/.ssh" "$O/packaging" "$U"
cp "$root/packaging/release-signers" "$O/packaging/release-signers"
# docrun DIR LINE: run one doc line in DIR
docrun() {
	(cd "$1" && HOME=$H sh -c "$2") >>"$t/doc.out" 2>&1 || { bad "doc command fails as written: $2"; sed 's/^/    /' "$t/doc.out"; return 0; }
	pass "doc command runs: $2"
}
: >"$t/doc.out"
# owner: key, signers line, fingerprint
while IFS= read -r l; do
	case $l in
	'ssh-keygen -t ed25519 '*) docrun "$O" "$l -N ''" ;;
	'printf '* | 'ssh-keygen -lf '*) docrun "$O" "$l" ;;
	esac
done <"$t/readme.cmds"
ofp=$(HOME=$H ssh-keygen -lf "$H/.ssh/ntfs-macos-release.pub" | awk '{ print $2 }')
chk "the README fingerprint command prints the key's fingerprint" grep -qF "$ofp" "$t/doc.out"
# a release signed with that key against that signers file
mkrel "$t/docrel"
expect 0 "sign with the key made by the README commands" sh "$sign" --key "$H/.ssh/ntfs-macos-release" --signers "$O/packaging/release-signers" "$t/docrel"
cp "$t/docrel/ntfs-macos-1.0.pkg" "$t/docrel/SHA256SUMS" "$t/docrel/SHA256SUMS.sig" "$U/"
mkdir -p "$U/ntfs-macos/packaging"
cp "$O/packaging/release-signers" "$U/ntfs-macos/packaging/"
: >"$t/doc.out"
while IFS= read -r l; do
	case $l in
	'git clone https://github.com/kurok/ntfs-macos.git') pass "doc command (replaced by a local copy): $l" ;;
	*) docrun "$U" "$l" ;;
	esac
done <"$t/install.cmds"
chk "the user's verify prints Good with the key's fingerprint" grep -qF "Good \"$ns\" signature for $ns with ED25519 key $ofp" "$t/doc.out"
chk "the user's check prints the release line" grep -qxF "# release 1.0" "$t/doc.out"
chk "the user's shasum check passes the package" grep -qxF "ntfs-macos-1.0.pkg: OK" "$t/doc.out"
: >"$t/doc.out"
while IFS= read -r l; do
	case $l in
	'ssh-keygen -Y verify '* | 'shasum '* | 'head '*) docrun "$U" "$l" ;;
	esac
done <"$t/readme.cmds"
# every line was handled: none of the doc lines is of a kind this test does not run
unk=$(grep -v -e '^ssh-keygen -t ed25519 ' -e '^printf ' -e '^ssh-keygen -lf ' -e '^ssh-keygen -Y verify ' -e '^shasum ' -e '^head ' "$t/readme.cmds" || true)
if [ -z "$unk" ]; then pass "README: every command is run"; else bad "README: commands this test does not run: $unk"; fi
unk=$(grep -v -e '^git clone ' -e '^cp ' -e '^ssh-keygen -Y verify ' -e '^shasum ' -e '^head ' "$t/install.cmds" || true)
if [ -z "$unk" ]; then pass "INSTALL: every command is run"; else bad "INSTALL: commands this test does not run: $unk"; fi

[ "$fail" -eq 0 ] && echo "release_signing_test: all passed"
exit "$fail"
