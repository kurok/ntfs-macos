#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# gen_bases.sh - make the tiny base images of F-PROBE and F-MOUNT-WALK (T-M3-16).
#
# Usage: tests/fuzz/image/gen_bases.sh [--out DIR]
#   --out DIR  where the .nsb files go (default tests/fuzz/image/bases)
#
# Run on the host: the script re-runs itself inside Environment A (vm/linux/run.sh), where
# mkntfs makes the images and the container's ntfs-3g mounts them (image files only, nothing
# on the host). Each image is then packed into the sparse NSB1 form (nsb.py), a few hundred
# KiB instead of MiB, small enough to commit. The bases:
#   tree.nsb   2 MiB, 512-byte clusters and sectors: nested directories, resident and
#              non-resident files, sparse and compressed files, named streams, links,
#              extended attributes, a reparse point, NFD and NFC names, a 255 unit name
#   geom.nsb   4 MiB, 4096-byte clusters and sectors: a few files and one stream
#   dirty.nsb  tree.nsb with /hiberfil.sys ("hibr") and an unclean shutdown (the ntfs-3g
#              process is killed after a sync: dirty flag set, log not closed)
# Contents differ in timestamps from run to run; the committed files are the reference.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")

die() { printf 'gen_bases.sh: error: %s\n' "$*" >&2; exit 1; }

inner=0
out=$here/bases
while [ $# -gt 0 ]; do
	case $1 in
	--inner) inner=1 ;;
	--out) [ $# -ge 2 ] || die "--out needs a directory"; out=$2; shift ;;
	-h | --help) sed -n '4,19p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

if [ "$inner" -eq 0 ]; then
	mkdir -p "$out"
	tmp=$(mktemp -d "${TMPDIR:-/tmp}/fuzz-bases.XXXXXX")
	trap 'rm -rf "$tmp"' EXIT
	sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fuzz/image/gen_bases.sh --inner >"$tmp/bases.tgz" ||
		die "container run failed"
	tar -xzf "$tmp/bases.tgz" -C "$out"
	ls -l "$out"
	exit 0
fi

# ---- inside the container ---------------------------------------------------------------
work=/work/tests/fuzz/image
res=/tmp/bases-out
mkdir -p "$res"
exec 3>&1 1>&2

make_image() { # NAME SIZE SECTOR CLUSTER LABEL
	truncate -s "$2" "/tmp/$1.img"
	mkntfs -F -Q -T -s "$3" -c "$4" -L "$5" "/tmp/$1.img" >"/tmp/$1.mkntfs.log" 2>&1 ||
		{ cat "/tmp/$1.mkntfs.log"; exit 1; }
}

mount_rw() { # NAME
	mkdir -p "/tmp/$1.mnt"
	ntfs-3g "/tmp/$1.img" "/tmp/$1.mnt" -o rw,streams_interface=xattr,permissions,compression
}

pack() { # NAME
	python3 -I -B "$work/nsb.py" pack "/tmp/$1.img" "$res/$1.nsb"
}

make_image tree 2M 512 512 fuzztree
mount_rw tree
python3 -I -B "$work/populate.py" /tmp/tree.mnt full
sync
umount /tmp/tree.mnt
ntfsfix -n /tmp/tree.img >/dev/null
pack tree

make_image geom 4M 4096 4096 fuzzgeom
mount_rw geom
python3 -I -B "$work/populate.py" /tmp/geom.mnt light
sync
umount /tmp/geom.mnt
ntfsfix -n /tmp/geom.img >/dev/null
pack geom

make_image dirty 2M 512 512 fuzzdirty
mount_rw dirty
python3 -I -B "$work/populate.py" /tmp/dirty.mnt full --hiber
sync
pid=$(pidof ntfs-3g)
kill -9 "$pid"
sleep 1
umount -l /tmp/dirty.mnt || true
pack dirty

ls -l "$res" >&2
tar -czf - -C "$res" . >&3
