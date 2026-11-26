#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build the release package ntfs-macos-<label>.pkg (T-M6-01, criteria 6.1 and 6.3; ADR-011 unsigned
# path, ADR-015: the output stays in local dist/). The payload is exactly the `dir` and `file`
# entries of packaging/manifest.txt, with the manifest's modes; the Installer gives them root:wheel
# (pkgbuild --ownership recommended). That is the same set, at the same paths, that
# packaging/dev-install.sh installs:
#   the daemon, the NTFS-3G engine as ntfsmac-worker, the ntfsprogs, the probe, volmark, the CLI
#   (all in /usr/local/libexec/ntfs-macos), the sandbox profiles, the CLI requirement and the
#   documents (in /Library/Application Support/ntfs-macos), the launchd plist, the uninstaller and
#   the manifest. The manifest's `link` /usr/local/bin/ntfsmac is not in the payload: postinstall
#   makes it when /usr/local/bin is root-only.
# The package scripts packaging/scripts/preinstall (preflight: macFUSE version, other NTFS drivers,
# files in the way, an earlier install of ours) and packaging/scripts/postinstall (the
# authorization right, the CLI requirement of ADR-019 check 3, the launchd job) run as root.
#
# The CLI requirement file is a placeholder in the payload: postinstall records the cdhash of the
# CLI as installed, so that a later re-signing of the payload (packaging/sign-and-notarize.sh)
# cannot leave a stale requirement. Until postinstall has run, the placeholder is not a valid
# requirement and the daemon refuses the privileged requests (fail closed).
#
# The product is built with the options of the CMake preset `product` (no test seams, ADR-019
# check 4 as the root-peer fallback), from the pinned NTFS-3G of third_party/deps.lock (the tarball
# must be in third_party/cache; nothing is downloaded here).
#
# Must run on macOS arm64 with macFUSE installed (the engine links macFUSE's libfuse). The host
# usually has no macFUSE; scripts/release-local.sh runs this script inside a disposable clone of
# the golden VM.
#
# Usage: packaging/build-pkg.sh [--label LABEL] [--out DIR] [--work DIR] [--jobs N] [--no-test]
#        packaging/build-pkg.sh --plan
#   --label  version label used in the file name (default: CMake project version)
#   --out    output directory (default: dist/<label>)
#   --work   scratch directory for deps, build and payload (default: build/pkg)
#   --no-test  skip the smoke run of the built binaries
#   --plan   print "TYPE MODE PATH <- SOURCE" for every payload entry and exit; builds nothing
#            and runs on any system (the unit test U-PKG-03 compares it with the manifest)
# Env:   MACFUSE_PREFIX (default /usr/local)
# Exit:  0 package built, 1 error, 2 usage error.
set -eu
umask 022

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)

die() { printf 'build-pkg: error: %s\n' "$1" >&2; exit "${2:-1}"; }
log() { printf 'build-pkg: %s\n' "$*"; }
usage() { printf 'usage: %s [--label LABEL] [--out DIR] [--work DIR] [--jobs N] [--no-test] | --plan\n' "$0" >&2; }

label=
out=
work=
jobs=4
run_tests=1
plan=0
while [ $# -gt 0 ]; do
	case "$1" in
	--label) [ $# -ge 2 ] || { usage; die "--label needs a value" 2; }; label=$2; shift ;;
	--out) [ $# -ge 2 ] || { usage; die "--out needs a value" 2; }; out=$2; shift ;;
	--work) [ $# -ge 2 ] || { usage; die "--work needs a value" 2; }; work=$2; shift ;;
	--jobs) [ $# -ge 2 ] || { usage; die "--jobs needs a value" 2; }; jobs=$2; shift ;;
	--no-test) run_tests=0 ;;
	--plan) plan=1 ;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" 2 ;;
	esac
	shift
done
case $jobs in '' | *[!0-9]*) die "--jobs needs a number" 2 ;; esac

manifest=$here/manifest.txt
job=org.ntfs-macos.ntfsmacd
pkg_id=org.ntfs-macos.pkg
[ -f "$manifest" ] || die "manifest not found: $manifest"

find_tool() { # name: the NTFS-3G tool in the deps prefix (bin or sbin, as NTFS-3G installs it)
	for _d in "$deps/bin" "$deps/sbin"; do
		[ -x "$_d/$1" ] && { printf '%s\n' "$_d/$1"; return 0; }
	done
	printf '%s\n' "$deps/{bin,sbin}/$1"
}

# ---- where every payload file comes from (the counterpart of source_of in dev-install.sh) ----
# deps: the NTFS-3G prefix; build: the CMake tree; stage: files generated here
source_of() { # installed path: the source file
	case $1 in
	/usr/local/libexec/ntfs-macos/ntfsmacd) echo "$build/src/daemon/ntfsmacd" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac-worker) echo "$deps/bin/ntfs-3g" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac-probe) echo "$build/src/probe/ntfsmac-probe" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac-volmark) echo "$build/src/volmark/ntfsmac-volmark" ;;
	/usr/local/libexec/ntfs-macos/mkntfs | /usr/local/libexec/ntfs-macos/ntfsfix | /usr/local/libexec/ntfs-macos/ntfsinfo | /usr/local/libexec/ntfs-macos/ntfslabel)
		find_tool "${1##*/}" ;;
	/usr/local/libexec/ntfs-macos/uninstall.sh) echo "$here/uninstall.sh" ;;
	/usr/local/libexec/ntfs-macos/manifest.txt) echo "$manifest" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac) echo "$stage/ntfsmac" ;;
	"/Library/Application Support/ntfs-macos/cli-requirement") echo "$stage/cli-requirement" ;;
	"/Library/Application Support/ntfs-macos/sandbox/"*.sb) echo "$root/integration/sandbox/${1##*/}" ;;
	/Library/LaunchDaemons/"$job".plist) echo "$root/integration/launchd/$job.plist" ;;
	"/Library/Application Support/ntfs-macos/doc/COPYING") echo "$root/COPYING" ;;
	"/Library/Application Support/ntfs-macos/doc/README.md") echo "$root/README.md" ;;
	"/Library/Application Support/ntfs-macos/doc/BUILD-INFO") echo "$stage/BUILD-INFO" ;;
	*) return 1 ;;
	esac
}

# the dir and file entries of the manifest, "kind mode path" (the path may hold spaces)
entries=$(awk '/^[ \t]*(#|$)/ { next } $1 == "dir" || $1 == "file" {
	p = $0; for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p); sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p)
	print $1 " " $2 " " p }' "$manifest")
[ -n "$entries" ] || die "the manifest lists no dir or file"

if [ "$plan" -eq 1 ]; then
	deps='<deps>' build='<build>' stage='<stage>'
	rc=0
	while read -r kind mode path; do
		if [ "$kind" = dir ]; then
			printf '%s %s %s\n' "$kind" "$mode" "$path"
		elif src=$(source_of "$path"); then
			printf '%s %s %s <- %s\n' "$kind" "$mode" "$path" "$src"
		else
			echo "build-pkg: no source for $path" >&2
			rc=1
		fi
	done <<EOF
$entries
EOF
	exit "$rc"
fi

# --- preflight ---
[ "$(uname -s)" = Darwin ] || die "packages are built on macOS only"
[ "$(uname -m)" = arm64 ] || die "the product target is arm64 (ARCHITECTURE.md section 10)"
for t in pkgbuild productbuild pkgutil codesign cmake shasum otool strings; do
	command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
done
macfuse_prefix=${MACFUSE_PREFIX:-/usr/local}
[ -f "$macfuse_prefix/lib/pkgconfig/fuse.pc" ] ||
	die "macFUSE libfuse not found ($macfuse_prefix/lib/pkgconfig/fuse.pc); build in the golden VM (scripts/release-local.sh)"

version=$(sed -n 's/^project(ntfs-macos VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
[ -n "$version" ] || die "cannot read the project version from CMakeLists.txt"
[ -n "$label" ] || label=$version
case $label in *[!A-Za-z0-9._+-]*) die "label has characters outside [A-Za-z0-9._+-]: $label" 2 ;; esac
[ -n "$work" ] || work=$root/build/pkg
[ -n "$out" ] || out=$root/dist/$label
case $work$out in *[[:space:]]*) die "paths with whitespace are not supported" ;; esac
mkdir -p "$work" "$out"
work=$(CDPATH='' cd -- "$work" && pwd)
out=$(CDPATH='' cd -- "$out" && pwd)

deps=$work/deps
build=$work/product
stage=$work/stage
payload=$work/root
scripts=$work/scripts
name=ntfs-macos-$label
rm -rf "$stage" "$payload" "$scripts"
mkdir -p "$stage" "$scripts"

# --- 1. NTFS-3G engine and static libntfs-3g, patched (ADR-002), from the pinned tarball ---
log "building NTFS-3G (external FUSE) into $deps"
sh "$root/scripts/build-ntfs3g.sh" --fuse external --jobs "$jobs" --prefix "$deps" --build-dir "$work/ntfs-3g"

# --- 2. the product build: the options of the preset `product`, linked against that libntfs-3g ---
log "configuring the product build in $build"
if command -v ninja >/dev/null 2>&1; then
	PKG_CONFIG_PATH=$deps/lib/pkgconfig cmake -S "$root" --preset product -B "$build"
else
	PKG_CONFIG_PATH=$deps/lib/pkgconfig cmake -S "$root" -B "$build" -G "Unix Makefiles" \
		-DCMAKE_BUILD_TYPE=Release -DNTFSMAC_TEST_SEAMS=OFF -DNTFSMAC_AUTHZ_FALLBACK_ROOT=ON
fi
# Product targets only: the fixture tools under tests/tools are for Environment A, and the unit
# tests run in ci-local.sh on the host.
cmake --build "$build" --parallel "$jobs" --target ntfsmacd ntfsmac ntfsmac-probe ntfsmac-volmark

# The same three checks as dev-install.sh: the cache, the strings of the daemon, its usage line.
grep -q '^NTFSMAC_TEST_SEAMS:BOOL=OFF$' "$build/CMakeCache.txt" || die "$build has test seams on"
grep -q '^NTFSMAC_AUTHZ_FALLBACK_ROOT:BOOL=ON$' "$build/CMakeCache.txt" || die "$build has the root fallback off"
strings "$build/src/daemon/ntfsmacd" >"$stage/daemon.strings"
if grep -q -e '--console-uid' -e '--child-log' -e '--journal-dir' "$stage/daemon.strings"; then
	die "the daemon contains test seams"
fi
"$build/src/daemon/ntfsmacd" --bogus >"$stage/daemon.usage" 2>&1 || true
grep -q '^usage: ntfsmacd' "$stage/daemon.usage" || die "the daemon printed no usage line for --bogus"
if grep -q -e '--console-uid' -e '--child-log' -e '--journal-dir' -e '--socket' -e '--no-marker' "$stage/daemon.usage"; then
	die "the daemon lists test seams in its usage line"
fi
rm -f "$stage/daemon.strings" "$stage/daemon.usage"

if [ "$run_tests" -eq 1 ]; then
	log "smoke test of the release binaries"
	"$build/src/probe/ntfsmac-probe" --version
	_rc=0; "$build/src/volmark/ntfsmac-volmark" >/dev/null 2>&1 || _rc=$?
	[ "$_rc" -eq 2 ] || die "ntfsmac-volmark without arguments should exit 2 (usage), got $_rc"
fi

# --- 3. generated files ---
# The CLI: ad-hoc signature with the hardened runtime (ADR-019 check 3). Its cdhash is recorded by
# postinstall, not here.
cp "$build/src/cli/ntfsmac" "$stage/ntfsmac"
codesign --force -s - -o runtime "$stage/ntfsmac"
printf '# not a requirement: the postinstall script of the package replaces this line with the cdhash of /usr/local/libexec/ntfs-macos/ntfsmac\n' \
	>"$stage/cli-requirement"

macfuse_ver=$(PKG_CONFIG_PATH=$macfuse_prefix/lib/pkgconfig pkg-config --modversion fuse 2>/dev/null || echo unknown)
ntfs3g_ver=$(awk '$1 == "ntfs-3g" { print $2 }' "$root/third_party/deps.lock")
{
	printf 'package      %s\n' "$name"
	printf 'identifier   %s\n' "$pkg_id"
	printf 'version      %s\n' "$version"
	printf 'source       %s\n' "${NTFSMAC_SOURCE_REV:-$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)}"
	printf 'built        %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'build-host   macOS %s %s\n' "$(sw_vers -productVersion)" "$(uname -m)"
	printf 'ntfs-3g      %s (patch series third_party/patches/ntfs-3g)\n' "${ntfs3g_ver:-unknown}"
	printf 'libfuse      %s (macFUSE, not bundled)\n' "$macfuse_ver"
	printf 'preset       product (no test seams)\n'
	printf 'signature    ad-hoc (unsigned developer build, ADR-011); cli with hardened runtime\n'
} >"$stage/BUILD-INFO"

# --- 4. stage the payload: exactly the manifest's dirs and files, with its modes ---
mkdir -p "$payload"
while read -r kind mode path; do
	case $kind in
	dir) install -d -m "$mode" "$payload$path" ;;
	file)
		src=$(source_of "$path") || die "no source for $path (manifest and build-pkg.sh out of step)"
		[ -f "$src" ] || die "source of $path is missing: $src"
		install -d -m 0755 "$payload${path%/*}"
		# cp -X: no extended attributes (a source tree copied with tar carries them, and pkgbuild
		# would add each as an AppleDouble ._ file to the payload)
		cp -X "$src" "$payload$path"
		chmod "$mode" "$payload$path"
		;;
	esac
done <<EOF
$entries
EOF

# Ad-hoc signatures for the programs in libexec (ADR-011). No hardened runtime there: library
# validation would refuse macFUSE's Developer ID libfuse in an ad-hoc signed worker. The CLI keeps
# the signature with the hardened runtime made above.
for f in "$payload/usr/local/libexec/ntfs-macos"/*; do
	case ${f##*/} in uninstall.sh | manifest.txt | ntfsmac) continue ;; esac
	codesign --force --sign - "$f"
done

# Refuse a payload that links anything outside the system, macFUSE or itself.
for f in "$payload/usr/local/libexec/ntfs-macos"/*; do
	case ${f##*/} in uninstall.sh | manifest.txt) continue ;; esac
	otool -L "$f" | tail -n +2 | awk '{print $1}' >"$stage/libs"
	while read -r lib; do
		case $lib in
		/usr/lib/* | /System/Library/* | /usr/local/lib/libfuse*.dylib) ;;
		*) die "${f##*/} links an unexpected library: $lib" ;;
		esac
	done <"$stage/libs"
done
rm -f "$stage/libs"

xattr -cr "$payload" 2>/dev/null || true
if [ -n "$(xattr -lr "$payload" 2>/dev/null)" ]; then
	xattr -lr "$payload" >&2
	die "the staged payload still has extended attributes"
fi

# the staged tree holds the manifest's paths and their parents, nothing else
(cd "$payload" && find . -print) >"$stage/staged.list"
sh "$here/payload-check.sh" "$manifest" "$stage/staged.list" || die "the staged payload differs from the manifest"

# --- 5. component package with the scripts, and the product archive ---
install -m 0755 "$here/scripts/preinstall" "$scripts/preinstall"
install -m 0755 "$here/scripts/postinstall" "$scripts/postinstall"
log "building $name.pkg"
pkgbuild --root "$payload" --scripts "$scripts" --identifier "$pkg_id" --version "$version" \
	--install-location / --ownership recommended --filter '\.DS_Store$' \
	"$work/component.pkg"
pkgutil --payload-files "$work/component.pkg" >"$stage/payload.list"
sh "$here/payload-check.sh" "$manifest" "$stage/payload.list" || die "the package payload differs from the manifest"
sed -e "s/@VERSION@/$version/g" -e "s/@LABEL@/$label/g" -e "s/@PKG_ID@/$pkg_id/g" \
	"$here/distribution.xml" >"$work/distribution.xml"
rm -f "$out/$name.pkg"
productbuild --distribution "$work/distribution.xml" --package-path "$work" "$out/$name.pkg"

install -m 0755 "$here/uninstall.sh" "$out/uninstall.sh"
install -m 0644 "$manifest" "$out/manifest.txt"
cp "$stage/BUILD-INFO" "$out/BUILD-INFO"
(cd "$out" && shasum -a 256 "$name.pkg" uninstall.sh manifest.txt >SHA256SUMS)

log "payload ($(grep -c . "$stage/payload.list") entries; manifest: $(printf '%s\n' "$entries" | grep -c .) dirs and files):"
sed 's/^/  /' "$stage/payload.list"
log "size: $(stat -f %z "$out/$name.pkg") bytes"
log "done: $out/$name.pkg"
