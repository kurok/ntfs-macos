#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# build-ntfs3g.sh - build static libntfs-3g (and ntfsprogs) from the pinned tarball.
#
# Takes the tarball that scripts/fetch-deps.sh verified (third_party/deps.lock, cache in
# third_party/cache/), unpacks it into a build directory, applies the patch series in
# third_party/patches/ntfs-3g/ in order (ADR-002; an empty series is fine), configures,
# builds and installs.
#
# FUSE: Linux uses the internal FUSE (--with-fuse=internal). macOS uses --with-fuse=external
# against the macFUSE libfuse, which must be installed (headers and fuse.pc under
# MACFUSE_PREFIX, default /usr/local). This script never installs macFUSE. The real macOS
# build runs in the VM (T-M2-03).
#
# Idempotent: a stamp keyed on tarball sha256, patch series hash and the configure line
# (with CFLAGS, LDFLAGS) makes a second run with nothing changed do no build work.
#
# --exec-prefix equals --prefix so nothing lands in /bin or /lib (upstream default).
#
# Usage: scripts/build-ntfs3g.sh [--prefix DIR] [--build-dir DIR] [--fuse internal|external]
#                                [--jobs N] [--force] [--print-configure] [--no-patches]
# --no-patches builds pristine upstream (no patch series), e.g. to re-check that U1 needs no patch
# (M2 audit F-06); never use it for a product build.
# Defaults: --prefix build/deps, --build-dir build/ntfs-3g-<fuse>.
# Env:   MACFUSE_PREFIX (default /usr/local), CFLAGS, LDFLAGS, DEPS_CACHE_DIR
# Exit:  0 built or up to date (or --print-configure), 1 error, 2 usage error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$here")
lock=$repo/third_party/deps.lock
cache=${DEPS_CACHE_DIR:-$repo/third_party/cache}
patch_dir=$repo/third_party/patches/ntfs-3g

die() { echo "build-ntfs3g: error: $*" >&2; exit 1; }
usage_err() { echo "build-ntfs3g: $*" >&2; echo "usage: build-ntfs3g.sh [--prefix DIR] [--build-dir DIR] [--fuse internal|external] [--jobs N] [--force] [--print-configure] [--no-patches]" >&2; exit 2; }
log() { printf 'build-ntfs3g: %s\n' "$*"; }

prefix=
build_dir=
fuse=
jobs=
force=0
no_patches=0
print_only=0

while [ $# -gt 0 ]; do
	case $1 in
	--prefix) [ $# -ge 2 ] || usage_err "--prefix needs a value"; prefix=$2; shift 2 ;;
	--build-dir) [ $# -ge 2 ] || usage_err "--build-dir needs a value"; build_dir=$2; shift 2 ;;
	--fuse) [ $# -ge 2 ] || usage_err "--fuse needs a value"; fuse=$2; shift 2 ;;
	--jobs) [ $# -ge 2 ] || usage_err "--jobs needs a value"; jobs=$2; shift 2 ;;
	--force) force=1; shift ;;
	--print-configure) print_only=1; shift ;;
	--no-patches) no_patches=1; shift ;;
	-h | --help) sed -n '2,26p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) usage_err "unknown option: $1" ;;
	esac
done

if [ -z "$fuse" ]; then
	case $(uname -s) in
	Linux) fuse=internal ;;
	*) fuse=external ;;
	esac
fi
case $fuse in internal | external) ;; *) usage_err "--fuse must be internal or external" ;; esac
case ${jobs:-1} in '' | *[!0-9]* | 0) usage_err "--jobs must be a positive integer" ;; esac

abspath() { # DIR -> absolute path (need not exist)
	case $1 in /*) printf '%s\n' "$1" ;; *) printf '%s/%s\n' "$(pwd)" "$1" ;; esac
}
[ -n "$prefix" ] || prefix=$repo/build/deps
[ -n "$build_dir" ] || build_dir=$repo/build/ntfs-3g-$fuse
prefix=$(abspath "$prefix")
build_dir=$(abspath "$build_dir")
case $prefix$build_dir in *[[:space:]]*) die "paths with whitespace are not supported" ;; esac

if [ -z "$jobs" ]; then
	jobs=$(nproc 2>/dev/null || sysctl -n hw.ncpu 2>/dev/null || echo 1)
fi

# The configure line. Same flags as vm/linux/Dockerfile, plus a static library.
macfuse_prefix=${MACFUSE_PREFIX:-/usr/local}
src=$build_dir/src
configure_args="--prefix=$prefix --exec-prefix=$prefix --with-fuse=$fuse --enable-static --disable-shared --enable-ntfsprogs --disable-ldconfig --disable-mount-helper --disable-plugins"
configure_env=
if [ "$fuse" = external ]; then
	configure_env="PKG_CONFIG_PATH=$macfuse_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}"
fi
configure_line="${configure_env:+$configure_env }$src/configure $configure_args"

if [ "$print_only" = 1 ]; then
	printf '%s\n' "$configure_line"
	exit 0
fi

if command -v sha256sum >/dev/null 2>&1; then
	sha256_stdin() { sha256sum | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
	sha256_stdin() { shasum -a 256 | cut -d' ' -f1; }
else
	die "need sha256sum or shasum"
fi

# --- preflight: external FUSE needs macFUSE (never installed by this script) ---
if [ "$fuse" = external ]; then
	pc_path=$macfuse_prefix/lib/pkgconfig${PKG_CONFIG_PATH:+:$PKG_CONFIG_PATH}
	if ! command -v pkg-config >/dev/null 2>&1 \
		|| ! PKG_CONFIG_PATH=$pc_path pkg-config --exists fuse; then
		cat >&2 <<MSG
build-ntfs3g: error: macFUSE headers are required for --with-fuse=external and were not found.
  Looked for the 'fuse' pkg-config module under: $macfuse_prefix/lib/pkgconfig
  This script never installs macFUSE. The real macOS build runs inside the VM (T-M2-03).
  If macFUSE lives elsewhere, set MACFUSE_PREFIX. Configure line it would use:
  $configure_line
MSG
		exit 1
	fi
fi

for t in tar patch make awk; do
	command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
done

# --- tarball from deps.lock, verified by fetch-deps.sh ---
[ -r "$lock" ] || die "lock file not readable: $lock"
url=$(awk '$1 == "ntfs-3g" { print $3 }' "$lock")
want=$(awk '$1 == "ntfs-3g" { print $4 }' "$lock")
[ -n "$url" ] && [ -n "$want" ] || die "no ntfs-3g entry in $lock"
tarball=$cache/${url##*/}
got=
[ ! -f "$tarball" ] || got=$(sha256_stdin <"$tarball")
if [ "$got" != "$want" ]; then
	# Missing or wrong digest: let fetch-deps.sh download and verify (needs curl, network).
	DEPS_CACHE_DIR=$cache sh "$here/fetch-deps.sh" ntfs-3g >/dev/null || die "fetch-deps.sh failed for ntfs-3g"
	[ -f "$tarball" ] || die "missing $tarball after fetch"
	got=$(sha256_stdin <"$tarball")
fi
[ "$got" = "$want" ] || die "tarball sha256 mismatch: $tarball"

# --- patch series: NNNN-*.patch in order; hash covers names and contents ---
series=
if [ "$no_patches" = 1 ]; then
	log "--no-patches: building pristine upstream without the patch series"
elif [ -d "$patch_dir" ]; then
	for p in "$patch_dir"/[0-9][0-9][0-9][0-9]-*.patch; do
		[ -f "$p" ] || continue
		case ${p##*/} in 0002-*) die "patch 0002 is withdrawn and its number is not reused: $p" ;; esac
		series="$series $p"
	done
fi
series_hash=$(
	for p in $series; do
		printf '%s %s\n' "${p##*/}" "$(sha256_stdin <"$p")"
	done | sha256_stdin
)

stamp_key=$(printf 'tarball=%s\nseries=%s\nconfigure=%s\nCFLAGS=%s\nLDFLAGS=%s\n' \
	"$want" "$series_hash" "$configure_line" "${CFLAGS:-}" "${LDFLAGS:-}")
stamp=$build_dir/.build-ntfs3g.stamp
lib=$prefix/lib/libntfs-3g.a

if [ "$force" = 0 ] && [ -f "$stamp" ] && [ -f "$lib" ] \
	&& [ "$(cat "$stamp")" = "$stamp_key" ]; then
	log "up to date (tarball $want, series ${series_hash%"${series_hash#????????}"}, fuse=$fuse); nothing to do"
	exit 0
fi

# --- unpack fresh, patch, configure, build, install ---
log "unpacking ${tarball##*/} into $src"
rm -f "$stamp"
rm -rf "$src"
mkdir -p "$src" "$prefix"
tar -xzf "$tarball" -C "$src" --strip-components=1

if [ -n "$series" ]; then
	for p in $series; do
		log "applying ${p##*/}"
		(cd "$src" && patch -p1 --batch --forward <"$p") || die "patch failed: ${p##*/}"
	done
else
	log "patch series is empty"
fi

log "configure: $configure_line"
(
	cd "$src"
	[ -z "$configure_env" ] || export "${configure_env?}"
	# shellcheck disable=SC2086
	./configure $configure_args
) || die "configure failed (see $src/config.log)"

log "make -j$jobs"
(cd "$src" && make -j"$jobs") || die "make failed"
log "make install"
(cd "$src" && make install) || die "make install failed"

[ -f "$lib" ] || die "expected static library missing: $lib"
printf '%s' "$stamp_key" >"$stamp"
log "done: $lib"
