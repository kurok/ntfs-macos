#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Detached signature of a release's SHA256SUMS (T-M7-08, ADR-011 amendment 2026-11-26).
#
# A SHA256SUMS that sits next to the package on the release page proves only that the download is
# intact: whoever can replace the package there can replace SHA256SUMS too. This script signs
# SHA256SUMS with an SSH key that the owner holds off GitHub (ssh-keygen -Y sign, namespace
# ntfs-macos-release), and checks the signature against the public key committed in
# packaging/release-signers. A user checks it against that file from a clone of the repository, not
# against anything from the release page (docs/INSTALL.md).
#
#   sign:   scripts/sign-release.sh --key FILE [--label L] [--signers FILE] DIR
#   verify: scripts/sign-release.sh --verify [--signers FILE] DIR
#
# --key FILE      the owner's private key, or the PUBLIC key file of a key held in ssh-agent (ssh-keygen
#                 then asks the agent). There is no default, and a private key inside this repository is
#                 refused. The script never creates, copies or stores a key.
# --signers FILE  allowed-signers file (ssh-keygen(1) ALLOWED SIGNERS); default packaging/release-signers.
#                 Another file is for tests only: a signature checked against it proves nothing to users.
# --label L       sign only: when SHA256SUMS has no '# release L' line yet, add it as the first line
#                 (for a directory that release-local.sh did not write, e.g. the output of
#                 packaging/sign-and-notarize.sh); when it has one, it must be L.
# DIR             the release directory (dist/<label>/): SHA256SUMS and every file it names.
#
# SHA256SUMS: lines '<64 hex>  <plain file name>' and exactly one line '# release <label>' that binds
# the release label into the signed content (shasum -c skips it), so an old release's signed files
# cannot be passed off as another release. The file ntfs-macos-<label>.pkg must be listed.
#
# Sign: checks every line of DIR/SHA256SUMS against the files (shasum -a 256 -c), writes
# DIR/SHA256SUMS.sig, verifies it against the signers file, and prints the label and the fingerprint
# of the key that made the signature (from ssh-keygen -Y verify) for the release notes. Verify: the
# same checks without signing.
# Exit: 0 ok, 1 a check failed, 2 usage error, 3 no release key in the signers file (owner setup).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
namespace=ntfs-macos-release
principal=ntfs-macos-release

die() { printf 'sign-release: %s\n' "$1" >&2; exit "${2:-1}"; }
usage() {
	printf 'usage: %s --key FILE [--label L] [--signers FILE] DIR\n       %s --verify [--signers FILE] DIR\n' "$0" "$0" >&2
}

key='' signers=$root/packaging/release-signers mode=sign dir='' label=''
while [ $# -gt 0 ]; do
	case "$1" in
	--key) [ $# -ge 2 ] || { usage; die "--key needs a file" 2; }; key=$2; shift ;;
	--signers) [ $# -ge 2 ] || { usage; die "--signers needs a file" 2; }; signers=$2; shift ;;
	--label) [ $# -ge 2 ] || { usage; die "--label needs a value" 2; }; label=$2; shift ;;
	--verify) mode=verify ;;
	-h | --help) usage; exit 0 ;;
	-*) usage; die "unknown option: $1" 2 ;;
	*) [ -z "$dir" ] || { usage; die "only one DIR" 2; }; dir=$1 ;;
	esac
	shift
done
[ -n "$dir" ] || { usage; die "missing DIR" 2; }
[ -d "$dir" ] || die "not a directory: $dir" 2
[ -f "$dir/SHA256SUMS" ] || die "no SHA256SUMS in $dir" 2
command -v ssh-keygen >/dev/null 2>&1 || die "ssh-keygen not found" 2
if [ "$mode" = sign ]; then
	[ -n "$key" ] || { usage; die "missing --key (the release signing key; there is no default)" 2; }
	[ -f "$key" ] || die "key file not found: $key" 2
	keyabs=$(CDPATH='' cd -- "$(dirname -- "$key")" && pwd)/$(basename -- "$key")
	case "$keyabs" in
	"$root"/*) grep -q 'PRIVATE KEY' "$key" 2>/dev/null && die "refusing a private key inside the repository ($keyabs); keep it outside, see packaging/README.md" 2 ;;
	esac
else
	[ -z "$key" ] || die "--key is not used with --verify" 2
	[ -z "$label" ] || die "--label is not used with --verify" 2
fi
case $label in *[!A-Za-z0-9._+-]* | [!A-Za-z0-9]*) die "bad --label: $label" 2 ;; esac

# The signers file must have a key line for our principal that may sign in our namespace. A line is
# 'principals [options] keytype base64 [comment]' (ssh-keygen(1) ALLOWED SIGNERS); principals is a
# comma-separated list, and options, when present, restrict the namespaces only if they name some.
[ -f "$signers" ] || die "signers file not found: $signers" 2
if ! awk -v p="$principal" -v ns="$namespace" '
	/^[[:space:]]*(#|$)/ { next }
	{
		np = split($1, ps, ","); ok = 0
		for (i = 1; i <= np; i++) if (ps[i] == p) ok = 1
		if (!ok) next
		if ($2 ~ /^(ssh-|ecdsa-|sk-)/) { found = 1; exit }       # no options: any namespace
		if (index($2, "namespaces=") == 0) { found = 1; exit }    # options without a namespace limit
		o = $2; sub(/.*namespaces="/, "", o); sub(/".*/, "", o)
		n = split(o, nl, ",")
		for (i = 1; i <= n; i++) if (nl[i] == ns) { found = 1; exit }
	}
	END { exit !found }' "$signers"; then
	die "no release key in $signers yet: the owner adds the public key once (packaging/README.md, 'Release signing key')" 3
fi

# 1. the label line
sums=$dir/SHA256SUMS
have=$(sed -n 's/^# release //p' "$sums")
if [ -z "$have" ] && [ -n "$label" ]; then
	{ printf '# release %s\n' "$label"; cat "$sums"; } >"$sums.new" && mv "$sums.new" "$sums"
	have=$label
fi
[ "$(grep -c '^# release ' "$sums" || true)" = 1 ] ||
	die "$sums must have exactly one '# release <label>' line (sign with --label L to add it)"
[ -z "$label" ] || [ "$have" = "$label" ] || die "$sums is for release '$have', not '$label'"
case $have in *[!A-Za-z0-9._+-]* | [!A-Za-z0-9]*) die "bad release label in $sums: $have" ;; esac

# 2. the files match SHA256SUMS: every other line names a plain file in this directory, once
awk '/^# release [A-Za-z0-9][A-Za-z0-9._+-]*$/ { next }
	{ n = $0; sub(/^[0-9a-f]{64}  /, "", n)
	  if (n == $0 || n !~ /^[A-Za-z0-9][A-Za-z0-9._+-]*$/ || seen[n]++) { print "bad line " NR ": " $0; bad = 1 } }
	END { exit bad }' "$sums" >&2 ||
	die "$sums has a malformed line, a repeated name or a path that is not a plain file name"
grep -q "  ntfs-macos-$have\.pkg\$" "$sums" || die "$sums does not list ntfs-macos-$have.pkg, the package of release $have"
(cd "$dir" && shasum -a 256 -c SHA256SUMS) || die "the files in $dir do not match SHA256SUMS"

# 3. sign
if [ "$mode" = sign ]; then
	rm -f "$dir/SHA256SUMS.sig"
	ssh-keygen -q -Y sign -f "$key" -n "$namespace" "$dir/SHA256SUMS" || die "ssh-keygen -Y sign failed"
	[ -f "$dir/SHA256SUMS.sig" ] || die "ssh-keygen wrote no $dir/SHA256SUMS.sig"
fi

# 4. verify against the signers file (what a user runs, docs/INSTALL.md); the fingerprint is the one
# ssh-keygen reports for the key that made this signature
[ -f "$dir/SHA256SUMS.sig" ] || die "no SHA256SUMS.sig in $dir"
vout=$(ssh-keygen -Y verify -f "$signers" -I "$principal" -n "$namespace" -s "$dir/SHA256SUMS.sig" <"$sums" 2>&1) || {
	printf '%s\n' "$vout" >&2
	die "the signature of $sums does not verify against $signers"
}
fp=$(printf '%s\n' "$vout" | sed -n 's/^Good ".*" signature for .* with \(.*\) key \(SHA256:[^ ]*\).*/\1 \2/p')
[ -n "$fp" ] || die "unexpected ssh-keygen -Y verify output: $vout"
printf 'sign-release: %s: SHA256SUMS.sig verifies: release %s, principal %s, namespace %s, key %s\n' "$dir" "$have" "$principal" "$namespace" "$fp"
