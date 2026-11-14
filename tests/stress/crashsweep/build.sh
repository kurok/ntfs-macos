#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# build.sh - build the release and the fault-layer NTFS-3G inside Environment A and prove
# that the release build does not contain patch 0004 (T-M3-14).
#
# Run inside the container (the repo is /work, read-only), for example through
#   vm/linux/run.sh run -- sh /work/tests/stress/crashsweep/build.sh [OUTDIR]
# OUTDIR defaults to /tmp/crashsweep. Result: OUTDIR/release (series without the layer),
# OUTDIR/faultio (series plus --enable-faultio), OUTDIR/build-proof.txt.
#
# Proof for the release build (all must hold, else exit 1):
#   1. nm (every symbol, local ones included) of libntfs-3g.a, ntfs-3g, ntfsfix and mkntfs
#      has no "faultio" symbol, and strings finds no NTFSMAC_FAULT text in them;
#   2. unix_io.o of the release build disassembles (objdump -dr) and lists symbols (nm)
#      identically to unix_io.o built from the tarball with the series up to 0003 only,
#      same configure line, so patch 0004 changes no code without --enable-faultio;
#   3. positive control: the faultio build does contain the symbols and the text.
# Exit: 0 proof holds, 1 failure.
set -eu

work=${NTFSMAC_WORK:-/work}
out=${1:-/tmp/crashsweep}
proof=$out/build-proof.txt
jobs=$(nproc 2>/dev/null || echo 2)
die() { printf 'build.sh: error: %s\n' "$*" >&2; exit 1; }
say() { printf '%s\n' "$*" | tee -a "$proof" >&2; }

mkdir -p "$out"
: >"$proof"
cd "$out"

build() { # NAME [extra build-ntfs3g args]
	name=$1; shift
	sh "$work/scripts/build-ntfs3g.sh" --fuse internal --jobs "$jobs" \
		--prefix "$out/$name" --build-dir "$out/build-$name" "$@" >"$out/build-$name.log" 2>&1 \
		|| { tail -30 "$out/build-$name.log" >&2; die "build $name failed (log: $out/build-$name.log)"; }
}
build release
build faultio --configure-arg --enable-faultio

# Reference: series up to 0003 only, same configure line as the release build.
ref=$out/build-ref
rm -rf "$ref"
mkdir -p "$ref"
tarball=$(ls "$work"/third_party/cache/ntfs-3g_ntfsprogs-*.tgz)
tar -xzf "$tarball" -C "$ref" --strip-components=1
for p in "$work"/third_party/patches/ntfs-3g/[0-9][0-9][0-9][0-9]-*.patch; do
	case ${p##*/} in 0004-* | 0005-* | 0006-*) continue ;; esac
	(cd "$ref" && patch -p1 --batch --forward <"$p" >/dev/null) || die "reference patch failed: $p"
done
cfg=$(sh "$work/scripts/build-ntfs3g.sh" --fuse internal --prefix "$out/release" \
	--build-dir "$out/build-release" --print-configure | sed "s|$out/build-release/src/configure|./configure|")
# shellcheck disable=SC2086
(cd "$ref" && $cfg >"$out/build-ref.log" 2>&1 && make -C libntfs-3g libntfs_3g_la-unix_io.lo >>"$out/build-ref.log" 2>&1) \
	|| { tail -20 "$out/build-ref.log" >&2; die "reference build of unix_io.o failed"; }

fail=0
check_absent() { # FILE
	n=$(nm -a "$1" 2>/dev/null | grep -ic faultio || true)
	s=$(strings -a "$1" | grep -c 'NTFSMAC_FAULT' || true)
	say "release $(basename "$1"): faultio symbols=$n, NTFSMAC_FAULT strings=$s"
	[ "$n" = 0 ] && [ "$s" = 0 ] || fail=1
}
for f in "$out/release/lib/libntfs-3g.a" "$out/release/bin/ntfs-3g" "$out/release/bin/ntfsfix" \
	"$out/release/sbin/mkntfs"; do
	[ -f "$f" ] || die "missing $f"
	check_absent "$f"
done

objfile() { find "$1" -name '*unix_io.o' | head -n 1; }
rel_o=$(objfile "$out/build-release/src/libntfs-3g")
ref_o=$(objfile "$ref/libntfs-3g")
[ -n "$rel_o" ] && [ -n "$ref_o" ] || die "unix_io.o not found"
dis() { objdump -dr --no-show-raw-insn -j .text -j .rodata -j .data "$1" | sed '1,/^$/d'; }
nmx() { nm "$1" | awk '{ print $NF, $(NF-1) }' | sort; }
if dis "$rel_o" >"$out/unix_io.release.dis" && dis "$ref_o" >"$out/unix_io.ref.dis" \
	&& cmp -s "$out/unix_io.release.dis" "$out/unix_io.ref.dis" \
	&& [ "$(nmx "$rel_o" | sha256sum)" = "$(nmx "$ref_o" | sha256sum)" ]; then
	say "release unix_io.o equals the 0003-only reference: objdump -dr (.text .rodata .data) and nm identical ($(wc -l <"$out/unix_io.release.dis") lines)"
else
	say "FAIL: release unix_io.o differs from the 0003-only reference"
	diff "$out/unix_io.release.dis" "$out/unix_io.ref.dis" | head -20 | tee -a "$proof" >&2 || true
	fail=1
fi

fio_lib=$out/faultio/lib/libntfs-3g.a
n=$(nm -a "$fio_lib" | grep -ic faultio || true)
s=$(strings -a "$out/faultio/bin/ntfs-3g" | grep -c 'NTFSMAC_FAULT_PLAN' || true)
say "faultio libntfs-3g.a: faultio symbols=$n; ntfs-3g NTFSMAC_FAULT_PLAN strings=$s (positive control, must be >0)"
[ "$n" -gt 0 ] && [ "$s" -gt 0 ] || fail=1

if [ "$fail" = 0 ]; then say "build proof: PASS"; else say "build proof: FAIL"; fi
exit "$fail"
