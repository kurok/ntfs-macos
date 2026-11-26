#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Sign, notarize and staple an ntfs-macos package (T-M6-02, criterion 6.4, ADR-011 release stage).
# Takes the unsigned package that packaging/build-pkg.sh made (dist/<label>/ntfs-macos-<label>.pkg)
# and writes a signed, notarized and stapled copy, with the files that go with it, into another
# directory. The input directory is not changed.
#
# Steps:
#   1. pkgutil --expand-full      unpack the package into a scratch directory
#   2. codesign                   every Mach-O file of the payload: Developer ID Application,
#                                 hardened runtime, secure timestamp. A file whose `otool -L` lists
#                                 /usr/local/lib/libfuse* also gets the entitlement
#                                 com.apple.security.cs.disable-library-validation
#                                 (packaging/entitlements-libfuse.plist): without it, library
#                                 validation refuses macFUSE's libfuse and nothing mounts
#   3. pkgbuild, productbuild     build the component and the product again from the signed payload
#                                 with the original scripts, identifier, version and Distribution.
#                                 `pkgbuild --ownership recommended` gives root:wheel. (`pkgutil
#                                 --flatten-full` run as a normal user writes the user's uid into
#                                 the bill of materials and adds ._ files: do not use it.)
#   4. lsbom -p MUGf              the bill of materials (mode, owner, group, path) of the new
#                                 component must equal the original; if not, the script stops
#   5. productsign                Developer ID Installer signature on the package
#   6. xcrun notarytool submit    --keychain-profile NAME --wait; on a status other than Accepted
#                                 the script prints `notarytool log` for the submission and stops
#   7. xcrun stapler staple       and `stapler validate`
#   8. spctl -a -t install -vv    Gatekeeper's verdict on the result (must say "accepted")
#   9. output directory           ntfs-macos-<label>.pkg, uninstall.sh, manifest.txt, BUILD-INFO
#                                 (signature line changed) and a new SHA256SUMS
#
# Credentials are never stored. The names of the two Developer ID identities and the name of a
# notarytool keychain profile are passed as arguments, and the script only hands them to
# codesign, productsign and notarytool. It does not read, list, unlock or change a keychain
# (it does not run `security`), and it does not look at the identities before it uses them: a
# wrong name makes codesign or productsign fail at that step. Create the profile once with
# `xcrun notarytool store-credentials NAME` (docs/IMPLEMENTATION_PLAN.md, H8).
#
# Without the credentials or the tools the script exits 3 with a message and does nothing. The
# release can then be made unsigned (ADR-011, last row): see docs/INSTALL.md section "Install"
# and packaging/README.md.
#
# Run it on the release Mac, not in a test guest. It was not run against a real account: no
# Developer ID credentials exist yet (H8). `--dry-run` prints every command and runs none.
# If the files of the payload carry extended attributes that cannot be removed (for example
# com.apple.provenance on some hosts), pkgbuild adds ._ files, the bill of materials differs and
# the script refuses at step 4; run it where the expanded files carry none.
#
# Usage: packaging/sign-and-notarize.sh --pkg FILE --profile NAME
#            --app-identity "Developer ID Application: ..."
#            --installer-identity "Developer ID Installer: ..."
#            [--out-dir DIR] [--dry-run]
#   --pkg FILE              unsigned package, named ntfs-macos-<label>.pkg
#   --profile NAME          notarytool keychain profile
#   --app-identity NAME     signing identity for the binaries (codesign --sign)
#   --installer-identity N  signing identity for the package (productsign --sign)
#   --out-dir DIR           where the signed set goes (default: signed/ next to the input; its
#                           parent must exist; it must not be the input's directory and must not
#                           hold the package yet)
#   --dry-run               print the commands, run nothing; the package is not read and the
#                           tools are not looked for
# Exit: 0 done (or dry run printed), 1 a step failed, 2 usage error, 3 credentials or tools missing.
set -eu

PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

prog=${0##*/}
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ents=$here/entitlements-libfuse.plist
pkg=
profile=
app_id=
inst_id=
outdir=
dry=0
work=

usage() {
	cat >&2 <<USAGE
usage: $prog --pkg FILE --profile NAME --app-identity NAME --installer-identity NAME [--out-dir DIR] [--dry-run]
USAGE
}

die() { printf '%s: %s\n' "$prog" "$1" >&2; exit "${2:-1}"; }

unsigned_hint() {
	cat >&2 <<HINT
$prog: no signed release is possible without these. The unsigned path of ADR-011 stays open:
  ship the unsigned package from packaging/build-pkg.sh and say so. Gatekeeper blocks a
  double-click install of it; the supported way is 'sudo installer -pkg FILE -target /'
  (docs/INSTALL.md, packaging/README.md). Mark M6 as "complete, unsigned".
HINT
}

missing() {
	printf '%s: %s\n' "$prog" "$1" >&2
	unsigned_hint
	exit 3
}

cleanup() {
	if [ -n "$work" ]; then rm -rf "$work"; fi
}

while [ $# -gt 0 ]; do
	case $1 in
	--pkg | --profile | --app-identity | --installer-identity | --out-dir)
		[ $# -ge 2 ] || { usage; die "$1 needs a value" 2; }
		case $2 in
		-*) usage; die "$1 needs a value, got '$2'" 2 ;;
		esac
		case $1 in
		--pkg) pkg=$2 ;;
		--profile) profile=$2 ;;
		--app-identity) app_id=$2 ;;
		--installer-identity) inst_id=$2 ;;
		--out-dir) outdir=$2 ;;
		esac
		shift 2
		;;
	--dry-run) dry=1; shift ;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown argument: $1" 2 ;;
	esac
done

[ -n "$pkg" ] || { usage; die "--pkg is required" 2; }
name=${pkg##*/}
case $name in
ntfs-macos-*.pkg) ;;
*) die "--pkg must be named ntfs-macos-<label>.pkg: $name" 2 ;;
esac
indir=$(dirname -- "$pkg")

# The credentials. All three are required; none is read from the environment or a file.
need=
[ -n "$profile" ] || need="$need --profile"
[ -n "$app_id" ] || need="$need --app-identity"
[ -n "$inst_id" ] || need="$need --installer-identity"
if [ -n "$need" ]; then
	missing "missing credentials (give$need); they are passed as arguments and never stored"
fi
case $app_id in
"Developer ID Application:"*) ;;
*) die "--app-identity must be a 'Developer ID Application: ...' identity (is it swapped with the installer one?)" 2 ;;
esac
case $inst_id in
"Developer ID Installer:"*) ;;
*) die "--installer-identity must be a 'Developer ID Installer: ...' identity (is it swapped with the application one?)" 2 ;;
esac

[ -n "$outdir" ] || outdir=$indir/signed
outdir=${outdir%/}
[ -n "$outdir" ] || die "--out-dir must not be the root directory" 2
case $outdir in
*[!A-Za-z0-9._/+@=,-]*) die "--out-dir has a character that is not allowed: $outdir" 2 ;;
esac
[ "$outdir" != "$indir" ] || die "--out-dir must not be the directory of the input package" 2
[ -d "$(dirname -- "$outdir")" ] || die "the parent of --out-dir does not exist: $(dirname -- "$outdir")" 2

# run CMD ARGS...: print the command in dry-run mode, run it otherwise.
run() {
	if [ "$dry" = 1 ]; then
		printf '+'
		for a in "$@"; do printf " '%s'" "$a"; done
		printf '\n'
	else
		"$@"
	fi
}

if [ "$dry" = 0 ]; then
	for t in pkgutil pkgbuild productbuild productsign codesign lsbom otool spctl file find xattr; do
		command -v "$t" >/dev/null 2>&1 || missing "the tool '$t' is not available"
	done
	# notarytool and stapler come with Xcode or its command line tools (ADR-011).
	xcrun --find notarytool >/dev/null 2>&1 || missing "xcrun cannot find 'notarytool' (install Xcode or its command line tools, H8)"
	xcrun --find stapler >/dev/null 2>&1 || missing "xcrun cannot find 'stapler' (install Xcode or its command line tools, H8)"
	[ -f "$pkg" ] || die "no such package: $pkg" 1
	[ -f "$ents" ] || die "missing $ents" 1
	[ ! -e "$outdir/$name" ] || die "refusing to overwrite $outdir/$name" 1
	for f in uninstall.sh manifest.txt BUILD-INFO; do
		[ -f "$indir/$f" ] || die "the input directory lacks $f (it is copied to the output)" 1
	done
	work=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmac-sign.XXXXXX") || die "cannot make a scratch directory" 1
	trap cleanup EXIT
	trap 'exit 130' INT
	trap 'exit 143' HUP TERM
else
	work='<scratch dir>'
	printf '# dry run: nothing below is executed; no keychain is read\n'
fi

expanded=$work/expanded
comp=$expanded/component.pkg
build=$work/build
flat=$build/$name
signed=$work/signed.pkg
notary=$work/notary.txt

run pkgutil --expand-full "$pkg" "$expanded"

# Every Mach-O file of the payload gets the hardened runtime and a timestamp. `file -b` is run on
# each path on its own, so a name with ": " or a universal binary is handled. A file that links
# macFUSE's libfuse also gets the entitlement that turns library validation off.
if [ "$dry" = 1 ]; then
	printf '+ codesign --force --timestamp --options runtime --sign %s <each Mach-O file below %s/Payload>\n' \
		"'$app_id'" "$comp"
	printf '+ codesign --force --timestamp --options runtime --entitlements %s --sign %s <each of them whose otool -L lists /usr/local/lib/libfuse*>\n' \
		"'$ents'" "'$app_id'"
else
	n=0
	nfuse=0
	list=$work/files.list
	find "$comp/Payload" -type f >"$list"
	while IFS= read -r f; do
		case $(file -b "$f") in
		Mach-O*) ;;
		*) continue ;;
		esac
		if otool -L "$f" 2>/dev/null | grep -q '[[:space:]]/usr/local/lib/libfuse'; then
			run codesign --force --timestamp --options runtime --entitlements "$ents" --sign "$app_id" "$f"
			nfuse=$((nfuse + 1))
		else
			run codesign --force --timestamp --options runtime --sign "$app_id" "$f"
		fi
		n=$((n + 1))
	done <"$list"
	[ "$n" -gt 0 ] || die "no Mach-O file found in the payload of $pkg" 1
	[ "$nfuse" -gt 0 ] || die "no program of the payload links libfuse: the entitlement rule found nothing to apply to" 1
	printf '%s: signed %d binaries, %d with the library validation entitlement\n' "$prog" "$n" "$nfuse"
fi

# The installed BUILD-INFO names the signature of the package.
run sed -i '' 's/^signature .*/signature    Developer ID, notarized, stapled; every program with hardened runtime, libfuse users with disable-library-validation/' \
	"$comp/Payload/Library/Application Support/ntfs-macos/doc/BUILD-INFO"

# Build the component and the product again. Identifier and version come from the original.
if [ "$dry" = 1 ]; then
	ident='<identifier of PackageInfo>'
	ver='<version of PackageInfo>'
else
	ident=$(sed -n 's/.* identifier="\([^"]*\)".*/\1/p' "$comp/PackageInfo" | head -n 1)
	ver=$(sed -n 's/.* version="\([^"]*\)".*/\1/p' "$comp/PackageInfo" | head -n 1)
	[ -n "$ident" ] && [ -n "$ver" ] || die "cannot read identifier and version from PackageInfo" 1
	mkdir -p "$build"
fi
run xattr -cr "$comp/Payload"
run pkgbuild --root "$comp/Payload" --scripts "$comp/Scripts" --identifier "$ident" --version "$ver" \
	--install-location / --ownership recommended --filter '\.DS_Store$' "$build/component.pkg"
run productbuild --distribution "$expanded/Distribution" --package-path "$build" "$flat"

# The bill of materials must not change: same paths, modes, owners and groups.
if [ "$dry" = 1 ]; then
	printf '+ lsbom -p MUGf <Bom of the original component> and of the new one; refuse if they differ\n'
else
	mkdir -p "$work/new"
	pkgutil --expand "$flat" "$work/new/x"
	lsbom -p MUGf "$comp/Bom" | sort >"$work/bom.old"
	lsbom -p MUGf "$work/new/x/component.pkg/Bom" | sort >"$work/bom.new"
	if ! cmp -s "$work/bom.old" "$work/bom.new"; then
		diff "$work/bom.old" "$work/bom.new" | head -n 20 >&2 || true
		die "the bill of materials of the rebuilt package differs from the original (owners, modes or extra files); refusing" 1
	fi
	printf '%s: bill of materials identical (%d entries)\n' "$prog" "$(wc -l <"$work/bom.new" | tr -d ' ')"
fi

run productsign --sign "$inst_id" "$flat" "$signed"

# Notarize. On any status but Accepted, print the log of the submission.
if [ "$dry" = 1 ]; then
	printf "+ 'xcrun' 'notarytool' 'submit' '%s' '--keychain-profile' '%s' '--wait'   (the output is kept; the submission id is read from it)\n" "$signed" "$profile"
	printf "+ 'xcrun' 'notarytool' 'log' <submission id> '--keychain-profile' '%s'   (only if the status is not Accepted)\n" "$profile"
else
	rc=0
	xcrun notarytool submit "$signed" --keychain-profile "$profile" --wait >"$notary" 2>&1 || rc=$?
	cat "$notary"
	sid=$(sed -n 's/^[[:space:]]*id:[[:space:]]*//p' "$notary" | head -n 1)
	if [ "$rc" -ne 0 ] || ! grep -q 'status: Accepted' "$notary"; then
		if [ -n "$sid" ]; then
			printf '%s: notarization was not accepted; log of submission %s:\n' "$prog" "$sid" >&2
			xcrun notarytool log "$sid" --keychain-profile "$profile" >&2 || true
		fi
		die "notarization failed (submission ${sid:-unknown})" 1
	fi
fi
run xcrun stapler staple "$signed"
run xcrun stapler validate "$signed"
run spctl -a -t install -vv "$signed"

# The output set: the signed package under its own name, the files of the input, a new BUILD-INFO
# and SHA256SUMS.
run mkdir -p "$outdir"
run cp "$signed" "$outdir/$name"
run cp "$indir/uninstall.sh" "$indir/manifest.txt" "$outdir/"
if [ "$dry" = 1 ]; then
	printf '+ sed (signature line of BUILD-INFO) %s/BUILD-INFO > %s/BUILD-INFO\n' "$indir" "$outdir"
	printf '+ (cd %s && shasum -a 256 %s uninstall.sh manifest.txt > SHA256SUMS)\n' "$outdir" "$name"
	printf '# dry run done; the result would be in %s\n' "$outdir"
else
	sed 's/^signature .*/signature    Developer ID, notarized, stapled; every program with hardened runtime, libfuse users with disable-library-validation/' "$indir/BUILD-INFO" >"$outdir/BUILD-INFO"
	(cd "$outdir" && shasum -a 256 "$name" uninstall.sh manifest.txt >SHA256SUMS)
	printf '%s: wrote %s\n' "$prog" "$outdir"
fi
