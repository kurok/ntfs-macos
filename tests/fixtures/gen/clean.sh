#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fixture generator v1 (T-M2-07): clean NTFS images, populated from a seed.
#
# Usage: tests/fixtures/gen/clean.sh [--only NAME] [--export DIR] [--print]
#
# Run on the host: the script re-runs itself inside Environment A
# (vm/linux/run.sh, debian:13-slim with the pinned NTFS-3G and ntfsprogs).
# Images are made and mounted ONLY inside the container; nothing is attached or
# mounted on the host. Images live in the container's /tmp and vanish with it.
# Use --export DIR to copy them to the host (gzip stream, extracted under DIR,
# which should be below the git-ignored build/).
#
# For every entry of tests/fixtures/manifest.json whose generator is "clean":
#   1. truncate an image of the given size, mkntfs with the given cluster and
#      sector size (-T fixes the time mkntfs seeds its serial/GUIDs from),
#   2. mount with ntfs-3g (internal FUSE) and write the tree derived from seed,
#   3. unmount, ntfsfix -n, remount read-only, run treedump,
#   4. compare the treedump SHA-256 with the manifest.
# --print only prints "name sha256" lines and does not compare (use it to fill
# in the manifest after changing a fixture).
#
# Reproducibility is defined at the treedump level. Images are NOT bit-identical
# across generations: ntfs-3g stamps file times with the wall clock while the
# tree is written (see docs/spikes or the T-M2-07 report).
# Env: NTFSMAC_FIXTURE_OUT (container output dir, default /tmp/ntfsmac-fixtures).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
manifest=tests/fixtures/manifest.json

die() { printf 'clean.sh: error: %s\n' "$*" >&2; exit 1; }

only=
export_dir=
print_only=0
inner=0
tar_out=0
while [ $# -gt 0 ]; do
	case $1 in
	--only) [ $# -ge 2 ] || die "--only needs a name"; only=$2; shift ;;
	--export) [ $# -ge 2 ] || die "--export needs a dir"; export_dir=$2; shift ;;
	--print) print_only=1 ;;
	--inner) inner=1 ;;
	--tar) tar_out=1 ;;
	-h | --help) sed -n '4,28p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

if [ "$inner" -eq 0 ]; then
	# Host side: hand over to Environment A.
	args="--inner"
	[ -z "$only" ] || args="$args --only $only"
	[ "$print_only" -eq 0 ] || args="$args --print"
	if [ -n "$export_dir" ]; then
		mkdir -p "$export_dir"
		# shellcheck disable=SC2086
		sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/clean.sh $args --tar |
			tar -xzf - -C "$export_dir"
		exit 0
	fi
	# shellcheck disable=SC2086
	exec sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/clean.sh $args
fi

# ---- inside the container from here on ----
work=/work
out=${NTFSMAC_FIXTURE_OUT:-/tmp/ntfsmac-fixtures}
mnt=/tmp/ntfsmac-fixture.mnt
bin=/tmp/ntfsmac-fixture-bin
for t in mkntfs ntfs-3g ntfsfix python3 cc; do
	command -v "$t" >/dev/null 2>&1 || die "$t not found: run through Environment A"
done
mkdir -p "$out" "$mnt" "$bin"

# stdout carries the tar stream with --tar, so all messages go to stderr then.
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

log() { printf '%s\n' "$*"; }

cc -std=c17 -O2 -Wall -Wextra -Werror -o "$bin/treedump" "$work/tests/tools/treedump.c"

cat >"$bin/populate.py" <<'PYEOF'
# Writes a deterministic tree below argv[1]; contents derive from seed argv[2].
import os
import random
import sys

root = sys.argv[1]
seed = int(sys.argv[2])
cluster = int(sys.argv[3])
size = int(sys.argv[4])
rng = random.Random(seed)


def put(rel, data):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        f.write(data)


def textish(n):
    line = "seed %d line %%d\n" % seed
    out = []
    i = 0
    total = 0
    while total < n:
        s = line % i
        out.append(s)
        total += len(s)
        i += 1
    return "".join(out).encode()[:n]


def big(rel, n):
    path = os.path.join(root, rel)
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "wb") as f:
        left = n
        while left > 0:
            k = min(left, 1 << 20)
            f.write(rng.randbytes(k))
            left -= k


os.makedirs(os.path.join(root, "empty"), exist_ok=True)
put("docs/readme.txt", textish(1200))
put("docs/nested/a/b/c/deep.txt", textish(300))
put("with spaces/a file  with   spaces.txt", rng.randbytes(777))
put("with spaces/ leading space", rng.randbytes(10))
put("unicode/café-nfc.txt", rng.randbytes(50))
put("unicode/café-nfd.txt", rng.randbytes(51))
put("unicode/файл-кир.txt", rng.randbytes(2000))
put("unicode/文件-中文.txt", rng.randbytes(5000))
put("unicode/emoji-\U0001f600-\U0001d518.txt", rng.randbytes(64))
put("unicode/Ünïcödé dir/åßç.bin", rng.randbytes(4097))
put("long/" + "x" * 250 + ".txt", rng.randbytes(5))
for i in range(40):
    n = rng.choice([0, 1, 7, 100, 500, 900, 1500, 5000, 20000])
    put("small/s%03d.bin" % i, rng.randbytes(n))
for tag, n in (("cm1", cluster - 1), ("c", cluster), ("cp1", cluster + 1), ("c2", 2 * cluster)):
    put("boundary/%s.bin" % tag, rng.randbytes(n))
for i in range(300):
    put("many/m%04d.txt" % i, ("m%d\n" % i).encode())
big("large/big.bin", size // 16)
put("large/text.txt", textish(300000))

# Sparse-ish file: data at start, middle and end with holes in between.
sp = os.path.join(root, "sparse")
os.makedirs(sp, exist_ok=True)
with open(os.path.join(sp, "holey.bin"), "wb") as f:
    f.write(rng.randbytes(4096))
    f.seek(3 << 20)
    f.write(rng.randbytes(4096))
    f.seek(6 << 20)
    f.truncate(6 << 20)
with open(os.path.join(sp, "allhole.bin"), "wb") as f:
    f.truncate(1 << 20)

os.symlink("docs/readme.txt", os.path.join(root, "link-to-readme"))
PYEOF

python3 -I -c '
import json
import sys

m = json.load(open(sys.argv[1]))
for e in m["fixtures"]:
    if e.get("generator") != "clean":
        continue
    print("\t".join(str(x) for x in (
        e["name"], e["size_bytes"], e["cluster_size"], e["sector_size"],
        e["seed"], e["label"], e.get("treedump_sha256", ""))))
' "$work/$manifest" >"$bin/fixtures.tsv"

mounted=0
# shellcheck disable=SC2329  # invoked through trap
cleanup() {
	if [ "$mounted" -eq 1 ]; then umount "$mnt" 2>/dev/null || true; fi
}
trap cleanup EXIT HUP INT TERM

do_mount() { # img mode
	ntfs-3g "$1" "$mnt" -o "$2,locale=C.UTF-8" || die "ntfs-3g mount failed ($2)"
	mounted=1
}
do_umount() {
	umount "$mnt" || die "umount failed"
	mounted=0
}

export LC_ALL=C.UTF-8 PYTHONUTF8=1
tab=$(printf '\t')
rc=0
count=0
while IFS=$tab read -r name size cluster sector seed label want; do
	[ -n "$name" ] || continue
	if [ -n "$only" ] && [ "$only" != "$name" ]; then continue; fi
	count=$((count + 1))
	img=$out/$name.img
	rm -f "$img"
	truncate -s "$size" "$img"
	# -T: fake time 0 so mkntfs' time-seeded serial and GUIDs are fixed.
	mkntfs -F -Q -T -L "$label" -c "$cluster" -s "$sector" "$img" >"$out/$name.mkntfs.log" 2>&1 ||
		{ cat "$out/$name.mkntfs.log"; die "mkntfs failed for $name"; }
	do_mount "$img" rw,big_writes
	python3 -I "$bin/populate.py" "$mnt" "$seed" "$cluster" "$size" ||
		die "populate failed for $name"
	sync
	do_umount
	ntfsfix -n "$img" >"$out/$name.ntfsfix.log" 2>&1 ||
		{ cat "$out/$name.ntfsfix.log"; die "ntfsfix -n failed for $name"; }
	do_mount "$img" ro
	"$bin/treedump" "$mnt" >"$out/$name.treedump" || die "treedump failed for $name"
	do_umount
	got=$(sha256sum "$out/$name.treedump" | cut -d' ' -f1)
	lines=$(wc -l <"$out/$name.treedump" | tr -d ' ')
	if [ "$print_only" -eq 1 ]; then
		log "$name $got"
	elif [ "$got" = "$want" ]; then
		log "ok       $name $got ($lines entries)"
	else
		log "MISMATCH $name got $got want ${want:-<none>}"
		rc=1
	fi
done <"$bin/fixtures.tsv"
[ "$count" -gt 0 ] || die "no fixture generated (unknown --only name?)"
if [ "$tar_out" -eq 1 ]; then
	tar -czf - -C "$out" . >&3
fi
exit "$rc"
