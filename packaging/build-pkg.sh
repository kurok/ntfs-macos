#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Build an installable, unsigned developer package ntfs-macos-<label>.pkg (ADR-011 unsigned path,
# ADR-015: output stays in local dist/). Minimal precursor of T-M6-01: it packages the parts that
# exist today (NTFS-3G engine as ntfsmac-worker, ntfsprogs, ntfsmac-probe, ntfsmac-volmark) under
# /usr/local/libexec/ntfs-macos/. No daemon, no LaunchDaemon, no preinstall/postinstall scripts:
# nothing runs as root at install time except the Installer copying files.
#
# Must run on macOS arm64 with macFUSE installed (the engine links macFUSE's libfuse). The host
# usually has no macFUSE; scripts/release-local.sh runs this script inside a disposable clone of
# the golden VM.
#
# Usage: packaging/build-pkg.sh [--label LABEL] [--out DIR] [--work DIR] [--jobs N] [--no-test]
#   --label  version label used in the file name (default: CMake project version)
#   --out    output directory (default: dist/<label>)
#   --no-test  skip the smoke run of the built binaries
#   --work   scratch directory for deps, build and payload (default: build/pkg)
# Env:   MACFUSE_PREFIX (default /usr/local)
# Exit:  0 package built, 1 error, 2 usage error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)

die() { printf 'build-pkg: error: %s\n' "$1" >&2; exit "${2:-1}"; }
log() { printf 'build-pkg: %s\n' "$*"; }
usage() { printf 'usage: %s [--label LABEL] [--out DIR] [--work DIR] [--jobs N] [--no-test]\n' "$0" >&2; }

label=
out=
work=
jobs=4
run_tests=1
while [ $# -gt 0 ]; do
	case "$1" in
	--label) [ $# -ge 2 ] || { usage; die "--label needs a value" 2; }; label=$2; shift ;;
	--out) [ $# -ge 2 ] || { usage; die "--out needs a value" 2; }; out=$2; shift ;;
	--work) [ $# -ge 2 ] || { usage; die "--work needs a value" 2; }; work=$2; shift ;;
	--jobs) [ $# -ge 2 ] || { usage; die "--jobs needs a value" 2; }; jobs=$2; shift ;;
	--no-test) run_tests=0 ;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" 2 ;;
	esac
	shift
done

# --- preflight ---
[ "$(uname -s)" = Darwin ] || die "packages are built on macOS only"
[ "$(uname -m)" = arm64 ] || die "the product target is arm64 (ARCHITECTURE.md section 10)"
for t in pkgbuild productbuild codesign cmake shasum; do
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

pkg_id=org.ntfs-macos.pkg
libexec=usr/local/libexec/ntfs-macos
docdir=usr/local/share/doc/ntfs-macos
deps=$work/deps
build=$work/cmake
payload=$work/root
name=ntfs-macos-$label

# --- 1. NTFS-3G engine and static libntfs-3g, patched (ADR-002) ---
log "building NTFS-3G (external FUSE) into $deps"
sh "$root/scripts/build-ntfs3g.sh" --fuse external --jobs "$jobs" --prefix "$deps" --build-dir "$work/ntfs-3g"

# --- 2. CMake parts, linked against that libntfs-3g ---
gen=
if command -v ninja >/dev/null 2>&1; then gen=Ninja; else gen="Unix Makefiles"; fi
log "configuring CMake ($gen) in $build"
PKG_CONFIG_PATH=$deps/lib/pkgconfig cmake -S "$root" -B "$build" -G "$gen" -DCMAKE_BUILD_TYPE=Release
# Product targets only: the libntfs-3g fixture tools under tests/tools are built for Environment A
# (Linux) and are not part of the package. Unit tests run in ci-local.sh on the host.
cmake --build "$build" --parallel "$jobs" --target ntfsmac-probe ntfsmac-volmark
if [ "$run_tests" -eq 1 ]; then
	log "smoke test of the release binaries"
	"$build/src/probe/ntfsmac-probe" --version
	_rc=0; "$build/src/volmark/ntfsmac-volmark" >/dev/null 2>&1 || _rc=$?
	[ "$_rc" -eq 2 ] || die "ntfsmac-volmark without arguments should exit 2 (usage), got $_rc"
fi

# --- 3. stage the payload ---
find_one() { # name dir...: print the first regular file called name below the dirs
	_n=$1; shift
	find "$@" -type f -name "$_n" -perm -u+x 2>/dev/null | head -n 1
}
rm -rf "$payload"
mkdir -p "$payload/$libexec" "$payload/$docdir"
install_bin() { # source-name dest-name dirs...
	_src=$1 _dst=$2; shift 2
	_f=$(find_one "$_src" "$@")
	[ -n "$_f" ] || die "built binary not found: $_src"
	install -m 0755 "$_f" "$payload/$libexec/$_dst"
}
install_bin ntfs-3g ntfsmac-worker "$deps/bin" "$deps/sbin"
for p in mkntfs ntfsfix ntfsinfo ntfslabel; do
	install_bin "$p" "$p" "$deps/bin" "$deps/sbin"
done
install_bin ntfsmac-probe ntfsmac-probe "$build/src/probe"
install_bin ntfsmac-volmark ntfsmac-volmark "$build/src/volmark"
install -m 0755 "$here/uninstall.sh" "$payload/$libexec/uninstall.sh"

install -m 0644 "$root/COPYING" "$root/README.md" "$payload/$docdir/"
cp -R "$root/LICENSES" "$payload/$docdir/LICENSES"
find "$payload/$docdir/LICENSES" -type f -exec chmod 0644 {} +

# Unsigned developer build: ad-hoc signatures only (ADR-011). No hardened runtime: library
# validation would refuse macFUSE's Developer ID libfuse in an ad-hoc signed worker.
for f in "$payload/$libexec"/*; do
	[ "${f##*/}" = uninstall.sh ] && continue
	codesign --force --sign - "$f"
done

# Refuse a payload that links anything outside the system, macFUSE or itself.
for f in "$payload/$libexec"/*; do
	[ "${f##*/}" = uninstall.sh ] && continue
	otool -L "$f" | tail -n +2 | awk '{print $1}' | while read -r lib; do
		case $lib in
		/usr/lib/* | /System/Library/* | /usr/local/lib/libfuse*.dylib) ;;
		*) die "${f##*/} links an unexpected library: $lib" ;;
		esac
	done
done

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
	printf 'signature    ad-hoc (unsigned developer build, ADR-011)\n'
} >"$payload/$docdir/BUILD-INFO"
chmod 0644 "$payload/$docdir/BUILD-INFO"

# --- 4. component package and product archive ---
log "building $name.pkg"
pkgbuild --root "$payload" --identifier "$pkg_id" --version "$version" \
	--install-location / --ownership recommended --filter '\.DS_Store$' \
	"$work/component.pkg"
sed -e "s/@VERSION@/$version/g" -e "s/@LABEL@/$label/g" -e "s/@PKG_ID@/$pkg_id/g" \
	"$here/distribution.xml" >"$work/distribution.xml"
rm -f "$out/$name.pkg"
productbuild --distribution "$work/distribution.xml" --package-path "$work" "$out/$name.pkg"

install -m 0755 "$here/uninstall.sh" "$out/uninstall.sh"
cp "$payload/$docdir/BUILD-INFO" "$out/BUILD-INFO"
(cd "$out" && shasum -a 256 "$name.pkg" uninstall.sh >SHA256SUMS)

log "payload:"
pkgutil --payload-files "$out/$name.pkg" | sed 's/^/  /'
log "done: $out/$name.pkg"
