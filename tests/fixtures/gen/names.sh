#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fixture generator v1 (T-M3-02): images with names that are, or are not, NFC
# (ADR-023, docs/TEST_STRATEGY.md 3.2: names-nfd, names-nfc, and three extras).
#
# Usage: tests/fixtures/gen/names.sh [--only NAME] [--export DIR]
#
# Run on the host: the script re-runs itself inside Environment A
# (vm/linux/run.sh, debian:13-slim with the pinned NTFS-3G and ntfsprogs).
# Images are made and mounted ONLY inside the container; nothing is attached or
# mounted on the host. Images live in the container's /tmp and vanish with it.
# Use --export DIR to copy images and name lists to the host (gzip stream,
# extracted under DIR, which should be below the git-ignored build/).
#
# For every entry of tests/fixtures/manifest.json whose generator is "names":
#   1. mkntfs an empty image of the given size, cluster and sector size (-T fixes
#      the time mkntfs seeds its serial and GUIDs from),
#   2. (recipe names:dirty only) set VOLUME_IS_DIRTY with tests/tools/setvolflags,
#   3. mount with ntfs-3g (internal FUSE, norecover) and create the names of the
#      recipe from byte strings. Linux NTFS-3G does no normalisation, so every
#      name is stored exactly as given, NFD included,
#   4. unmount, ntfsfix -n,
#   5. validate against the pinned NTFS-3G: "-o norecover,ro" on the image
#      (SHA-256 unchanged; every name reachable by its own bytes, stat and read),
#      and "-o norecover" on a COPY (mounts read/write).
# The expected name lists go to NTFSMAC_FIXTURE_OUT/NAME.nonnfc (the paths that are
# not NFC, UTF-8, one per line) and NAME.all (every path), for tests/unit/probe.
#
# Recipes:
#   names:nfd       22 names not in NFC (1 in a subdirectory, 1 directory) beside NFC twins
#   names:nfc       the same tree with every name in NFC, plus ASCII, emoji, CJK, Hangul
#   names:dirty     setvolflags DIRTY, then one NFD name
#   names:wide      names:nfd plus a directory of 300 NFC names (it has an index block)
#   names:longpath  six nested 100-character directories, one NFD name at the bottom
#   names:reparse   a directory with the reparse attribute (a junction) holding one NFD name
#   names:sysdir    a directory with the system attribute holding one ASCII file
#   names:deep      400 nested one-character directories, one NFD name at the bottom
# Env: NTFSMAC_FIXTURE_OUT (container output dir, default /tmp/ntfsmac-names).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")
manifest=tests/fixtures/manifest.json

die() { printf 'names.sh: error: %s\n' "$*" >&2; exit 1; }

only=
export_dir=
inner=0
tar_out=0
while [ $# -gt 0 ]; do
	case $1 in
	--only) [ $# -ge 2 ] || die "--only needs a name"; only=$2; shift ;;
	--export) [ $# -ge 2 ] || die "--export needs a dir"; export_dir=$2; shift ;;
	--inner) inner=1 ;;
	--tar) tar_out=1 ;;
	-h | --help) sed -n '4,40p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

if [ "$inner" -eq 0 ]; then
	args="--inner"
	[ -z "$only" ] || args="$args --only $only"
	if [ -n "$export_dir" ]; then
		mkdir -p "$export_dir"
		# shellcheck disable=SC2086
		sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/names.sh $args --tar |
			tar -xzf - -C "$export_dir"
		exit 0
	fi
	# shellcheck disable=SC2086
	exec sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/names.sh $args
fi

# ---- inside the container from here on ----
work=/work
out=${NTFSMAC_FIXTURE_OUT:-/tmp/ntfsmac-names}
mnt=/tmp/ntfsmac-names.mnt
bin=/tmp/ntfsmac-names-bin
prefix=${NTFS3G_PREFIX:-/opt/ntfs-3g}
for t in mkntfs ntfs-3g ntfsfix python3 cc sha256sum; do
	command -v "$t" >/dev/null 2>&1 || die "$t not found: run through Environment A"
done
mkdir -p "$out" "$mnt" "$bin"

# stdout carries the tar stream with --tar, so all messages go to stderr then.
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

log() { printf '%s\n' "$*"; }

san=
if [ "${NTFS_ASAN:-0}" = 1 ]; then san="-fsanitize=address,undefined -fno-omit-frame-pointer"; fi
# shellcheck disable=SC2086  # $san is a word list
cc -std=c17 -O2 -Wall -Wextra -Werror $san -isystem "$prefix/include" \
	-o "$bin/setvolflags" "$work/tests/tools/setvolflags.c" -L"$prefix/lib" -lntfs-3g ||
	die "building setvolflags failed"

# Creates the tree of recipe argv[2] below argv[1] and writes the lists argv[3].nonnfc
# and argv[3].all. Every path is bytes: nothing is converted by Python or by the mount.
cat >"$bin/populate.py" <<'PYEOF'
import os
import sys
import unicodedata as ud

root = os.fsencode(sys.argv[1])
recipe = sys.argv[2]
listbase = sys.argv[3]
all_paths = []
nonnfc = []


def nfc(s):
    return ud.normalize("NFC", s)


def nfd(s):
    return ud.normalize("NFD", s)


def put(rel, data=b"x"):
    """rel is a str; its UTF-8 is the name as stored."""
    b = rel.encode("utf-8")
    path = os.path.join(root, b)
    with open(path, "wb") as f:
        f.write(data)
    all_paths.append("/" + rel)
    if nfc(rel.split("/")[-1]) != rel.split("/")[-1]:
        nonnfc.append("/" + rel)


def mkdir(rel):
    os.mkdir(os.path.join(root, rel.encode("utf-8")))
    all_paths.append("/" + rel)
    if nfc(rel.split("/")[-1]) != rel.split("/")[-1]:
        nonnfc.append("/" + rel)


def base_tree(form):
    """form: the normalisation of the names that differ between the two fixtures."""
    f = nfd if form == "nfd" else nfc
    put("café-nfc.txt")                       # always NFC
    put(f("café-nfd.txt") if form == "nfd" else "café-second.txt")
    mkdir("sub")
    put("sub/" + f("Ångström-only.txt"))  # NFD-only name in a subdirectory
    mkdir("dir-é")                            # a pair of directories
    put("dir-é/x.txt")
    d2 = f("dir-é") if form == "nfd" else "dir-ü"
    mkdir(d2)
    put(d2 + "/y.txt")
    for i in range(19):
        put(f("nfd-%02d-ü.txt" % i) if form == "nfd" else "nfc-%02d-ü.txt" % i)


if recipe in ("names:nfd", "names:wide"):
    base_tree("nfd")
    if recipe == "names:wide":
        mkdir("wide")
        for i in range(300):
            put("wide/w-%03d-with-a-long-name-to-fill-the-index-block.txt" % i)
elif recipe == "names:nfc":
    base_tree("nfc")
    put("plain.txt")
    put("\U0001f600-emoji.txt")
    put("\U0001f468‍\U0001f469‍\U0001f467-family.txt")
    put("日本語-ファイル.txt")
    put("한글-파일.txt")
    put("документ.txt")
elif recipe == "names:dirty":
    put(nfd("café-dirty.txt"))
elif recipe == "names:longpath":
    rel = ""
    for i in range(6):
        rel += ("%d" % i) * 100 + "/"
        mkdir(rel.rstrip("/"))
    put(rel + nfd("é" * 40 + ".txt"))
elif recipe == "names:reparse":
    import struct
    j = "junc"
    mkdir(j)
    leaf = nfd("caf\u00e9-in-junction.txt")
    with open(os.path.join(root, (j + "/" + leaf).encode("utf-8")), "wb") as f:
        f.write(b"x")
    nonnfc.append("/" + j + "/" + leaf)  # not in `all`: a mount shows the junction as a link
    sub = "\\??\\C:\\target".encode("utf-16-le")
    pr = "C:\\target".encode("utf-16-le")
    data = struct.pack("<HHHH", 0, len(sub), len(sub) + 2, len(pr)) + sub + b"\0\0" + pr + b"\0\0"
    os.setxattr(os.path.join(root, j.encode()), "system.ntfs_reparse_data",
                struct.pack("<IHH", 0xA0000003, len(data), 0) + data)
    all_paths.remove("/" + j)  # a mount shows it as a dangling link: not reachable by stat
    put("plain.txt")
elif recipe == "names:sysdir":
    import struct
    mkdir("sysdir")
    put("sysdir/f.txt")
    p = os.path.join(root, b"sysdir")
    cur = struct.unpack(">I", os.getxattr(p, "system.ntfs_attrib_be"))[0]
    os.setxattr(p, "system.ntfs_attrib_be", struct.pack(">I", cur | 0x4))
elif recipe == "names:deep":
    rel = ""
    for i in range(400):
        rel += "a/"
        mkdir(rel.rstrip("/"))
    put(rel + nfd("caf\u00e9-deep.txt"))
else:
    sys.exit("unknown recipe " + recipe)

with open(listbase + ".nonnfc", "w", encoding="utf-8") as f:
    f.write("".join(p + "\n" for p in nonnfc))
with open(listbase + ".all", "w", encoding="utf-8") as f:
    f.write("".join(p + "\n" for p in all_paths))
print("%d names, %d not NFC" % (len(all_paths), len(nonnfc)))
PYEOF

# Every listed path must be reachable by its own bytes and readable.
cat >"$bin/reach.py" <<'PYEOF'
import os
import sys

root = os.fsencode(sys.argv[1])
bad = 0
n = 0
for line in open(sys.argv[2], encoding="utf-8"):
    rel = line.rstrip("\n")
    if not rel:
        continue
    p = os.path.join(root, rel.lstrip("/").encode("utf-8"))
    n += 1
    try:
        st = os.stat(p)
        if not os.path.isdir(p):
            with open(p, "rb") as f:
                f.read()
    except OSError as e:
        print("not reachable: %r: %s" % (rel, e))
        bad += 1
print("%d paths reachable by their own bytes, %d not" % (n - bad, bad))
sys.exit(1 if bad else 0)
PYEOF

python3 -I -c '
import json
import sys

m = json.load(open(sys.argv[1]))
for e in m["fixtures"]:
    if e.get("generator") != "names":
        continue
    print("\t".join(str(x) for x in (
        e["name"], e["size_bytes"], e["cluster_size"], e["sector_size"], e["label"], e["recipe"])))
' "$work/$manifest" >"$bin/fixtures.tsv"

mounted=0
# shellcheck disable=SC2329  # invoked through trap
cleanup() {
	if [ "$mounted" -eq 1 ]; then umount "$mnt" 2>/dev/null || true; fi
}
trap cleanup EXIT HUP INT TERM

do_mount() { # img opts
	ntfs-3g "$1" "$mnt" -o "$2" >"$out/mount.log" 2>&1 || { cat "$out/mount.log"; die "ntfs-3g mount failed ($2)"; }
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
while IFS=$tab read -r name size cluster sector label recipe; do
	[ -n "$name" ] || continue
	if [ -n "$only" ] && [ "$only" != "$name" ]; then continue; fi
	count=$((count + 1))
	bad=0
	img=$out/$name.img
	rm -f "$img"
	truncate -s "$size" "$img"
	mkntfs -F -Q -T -L "$label" -c "$cluster" -s "$sector" "$img" >"$out/$name.make.log" 2>&1 ||
		{ cat "$out/$name.make.log"; die "mkntfs failed for $name"; }
	if [ "$recipe" = names:dirty ]; then
		"$bin/setvolflags" "$img" --set DIRTY >>"$out/$name.make.log" 2>&1 || die "setvolflags failed for $name"
	fi
	do_mount "$img" rw,norecover
	python3 -I "$bin/populate.py" "$mnt" "$recipe" "$out/$name" >>"$out/$name.make.log" 2>&1 ||
		{ cat "$out/$name.make.log"; die "populate failed for $name"; }
	sync
	do_umount
	ntfsfix -n "$img" >"$out/$name.ntfsfix.log" 2>&1 || { cat "$out/$name.ntfsfix.log"; die "ntfsfix -n failed for $name"; }

	# Validate with the engine: read-only on the image (hash unchanged), read/write on a copy.
	h0=$(sha256sum "$img" | cut -d' ' -f1)
	do_mount "$img" norecover,ro
	python3 -I "$bin/reach.py" "$mnt" "$out/$name.all" >"$out/$name.reach.log" 2>&1 || { cat "$out/$name.reach.log"; bad=1; }
	do_umount
	h1=$(sha256sum "$img" | cut -d' ' -f1)
	[ "$h0" = "$h1" ] || { log "MISMATCH $name: image changed by a read-only mount"; bad=1; }
	cp "$img" "$img.rw"
	if ! ntfs-3g "$img.rw" "$mnt" -o norecover >"$out/$name.rw.log" 2>&1; then
		log "MISMATCH $name: read/write mount failed: $(tr '\n' ' ' <"$out/$name.rw.log")"
		bad=1
	else
		mounted=1
		touch "$mnt/.write-test" 2>/dev/null || { log "MISMATCH $name: not writable"; bad=1; }
		rm -f "$mnt/.write-test"
		do_umount
	fi
	rm -f "$img.rw"

	nn=$(wc -l <"$out/$name.nonnfc" | tr -d ' ')
	if [ "$bad" -eq 0 ]; then
		log "ok       $name ($nn names not NFC; $(tail -1 "$out/$name.reach.log"))"
	else
		log "MISMATCH $name"
		rc=1
	fi
done <"$bin/fixtures.tsv"
[ "$count" -gt 0 ] || die "no fixture generated (unknown --only name?)"
if [ "$tar_out" -eq 1 ]; then
	tar -czf - -C "$out" . >&3
fi
exit "$rc"
