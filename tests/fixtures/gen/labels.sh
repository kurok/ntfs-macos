#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Fixture generator v1 (T-M3-19): hostile-label images (docs/TEST_STRATEGY.md 3.3) and the
# label lists that tests/unit/sanitize and I-DMN-18 (T-M4-03) use.
#
# Usage: tests/fixtures/gen/labels.sh [--only NAME] [--export DIR]
#
# Run on the host: the script re-runs itself inside Environment A
# (vm/linux/run.sh, debian:13-slim with the pinned NTFS-3G and ntfsprogs).
# Images are made and mounted ONLY inside the container; nothing is attached or
# mounted on the host. Images live in the container's /tmp and vanish with it.
# Use --export DIR to copy images and lists to the host (gzip stream, extracted
# under DIR, which should be below the git-ignored build/).
#
# Steps:
#   1. make the base image clean-64m-4k with tests/fixtures/gen/clean.sh (its own check
#      against the manifest's treedump hash applies),
#   2. for each label of the table below: copy the base, set the label with the pinned
#      ntfslabel (LC_ALL=C.UTF-8), read it back, run ntfsfix -n, mount read-only with
#      ntfs-3g and compare the treedump hash with the base (the image is otherwise clean),
#   3. compile src/sanitize/label.c, read each label back from the image's $Volume the way
#      the probe does (UTF-16 to UTF-8) and run the label sanitiser on it: the leaf must be
#      the expected leaf of the table (written here from ADR-016, not taken from the code).
# NAME.leaf (expected leaf), NAME.label (the label bytes as read back) and labels.tsv (name,
# label hex, serial hex, expected leaf; the input of the I-DMN-18 harness) go to
# NTFSMAC_FIXTURE_OUT. The two images label-dup-a and label-dup-b carry the same label `DATA`.
#
# Hostile labels that ntfslabel cannot write (invalid UTF-8, a lone surrogate, NUL) are
# not images: $Volume holds UTF-16, and the probe turns a lone surrogate or NUL into U+FFFD
# (src/probe/text.c) before the label reaches the parser. Those inputs are in tests/unit/sanitize
# and in the fuzz corpus.
# Env: NTFSMAC_FIXTURE_OUT (container output dir, default /tmp/ntfsmac-labels).
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$(dirname "$here")")")

die() { printf 'labels.sh: error: %s\n' "$*" >&2; exit 1; }

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
	-h | --help) sed -n '4,36p' "$0"; exit 0 ;;
	*) die "unknown option: $1" ;;
	esac
	shift
done

if [ "$inner" -eq 0 ]; then
	args="--inner"
	[ -z "$only" ] || args="$args --only $only"
	if [ -n "$export_dir" ]; then
		mkdir -p "$export_dir"
		# A file, not a pipe: the status of the run in Environment A must not be lost.
		tgz=$(mktemp "${TMPDIR:-/tmp}/fixgen.XXXXXX") || exit 1
		# shellcheck disable=SC2086
		if ! sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/labels.sh $args --tar >"$tgz" ||
			! tar -xzf "$tgz" -C "$export_dir"; then
			rm -f "$tgz"
			printf 'labels.sh: error: the run in Environment A or the unpacking failed\n' >&2
			exit 1
		fi
		# stamp every image this export extracted (name, sha256, fixtures key of this checkout): a later run reusing
		# DIR refuses images from another version or edited since (M5-F07)
		sh "$repo/scripts/run-inputs.sh" fixtures-stamp "$export_dir" "$tgz" || { rm -f "$tgz"; exit 1; }
		rm -f "$tgz"
		exit 0
	fi
	# shellcheck disable=SC2086
	exec sh "$repo/vm/linux/run.sh" run -- sh /work/tests/fixtures/gen/labels.sh $args
fi

# ---- inside the container from here on ----
work=/work
out=${NTFSMAC_FIXTURE_OUT:-/tmp/ntfsmac-labels}
mnt=/tmp/ntfsmac-labels.mnt
bin=/tmp/ntfsmac-labels-bin
for t in ntfslabel ntfs-3g ntfsfix python3 cc sha256sum; do
	command -v "$t" >/dev/null 2>&1 || die "$t not found: run through Environment A"
done
mkdir -p "$out" "$mnt" "$bin"

# stdout carries the tar stream with --tar, so all messages go to stderr then.
if [ "$tar_out" -eq 1 ]; then exec 3>&1 1>&2; fi

log() { printf '%s\n' "$*"; }

# 1. base image
NTFSMAC_FIXTURE_OUT=$out sh "$work/tests/fixtures/gen/clean.sh" --inner --only clean-64m-4k ||
	die "the base image clean-64m-4k could not be made"
base=$out/clean-64m-4k.img
[ -f "$base" ] || die "missing $base"
base_sha=$(sha256sum "$out/clean-64m-4k.treedump" | cut -d' ' -f1)

# the sanitiser, compiled on its own (no library, no cmake)
cat >"$bin/leaf.c" <<'CEOF'
#include "sanitize.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* stdin: serial in hex, then the label in hex (may be empty); stdout: the leaf. */
int main(int argc, char **argv)
{
	unsigned char lab[2048];
	char out[NTFSMAC_LABEL_PART_SIZE];
	size_t n = 0, i;
	unsigned long long serial;

	if (argc != 3)
		return 2;
	serial = strtoull(argv[1], NULL, 16);
	for (i = 0; argv[2][i] && argv[2][i + 1] && n < sizeof(lab); i += 2) {
		char h[3] = { argv[2][i], argv[2][i + 1], 0 };

		lab[n++] = (unsigned char)strtoul(h, NULL, 16);
	}
	if (ntfsmac_label_sanitize((const char *)lab, n, serial, out, sizeof(out)) == 0)
		return 1;
	puts(out);
	return 0;
}
CEOF
cc -std=c17 -O2 -Wall -Wextra -Werror -I"$work/include" -I"$work/src/sanitize" -o "$bin/leaf" "$bin/leaf.c" \
	"$work/src/sanitize/label.c" "$work/src/common/label_cp.c" || die "building the sanitiser tool failed"
cc -std=c17 -O2 -Wall -Wextra -Werror -o "$bin/treedump" "$work/tests/tools/treedump.c" ||
	die "building treedump failed"

cat >"$bin/labels.py" <<'PYEOF'
# Makes one hostile-label image per table row and checks it. Every label is bytes.
import os
import shutil
import struct
import subprocess
import sys

base, out, only, binp, mnt, want_sha = sys.argv[1:7]
DOTS = "é" * 128
# name, label (str), expected leaf (from the ADR-016 table: "NTFS-@" is the fallback, @ = serial: also for a label in which nothing allowed survives)
TABLE = [
    ("label-comma", "X,remove_hiberfile", "X_remove_hiberfile"),
    ("label-fuseopt", "X,allow_other,uid=0", "X_allow_other_uid_0"),
    ("label-dotdot", "../../etc/x", "_._.._etc_x"),
    ("label-dot", ".hidden", "_hidden"),
    ("label-backslash", "a\\,b", "a__b"),
    ("label-newline", "a\nb", "a_b"),
    ("label-bidi", "abc‮txt.exe", "abc_txt.exe"),
    ("label-long", DOTS, "é" * 32),
    ("label-empty", "", "NTFS-@"),
    ("label-dup-a", "DATA", "DATA"),
    ("label-dup-b", "DATA", "DATA"),
    # extras
    ("label-equals", "X=1", "X_1"),
    ("label-hyphen", "-e x", "_e x"),  # never an option to a program (audit M4 F-01)
    ("label-slash", "a/b:c", "a_b_c"),
    ("label-esc", "a\x1b[31mred", "a__31mred"),
    ("label-trailing", "name. . ", "name"),
    ("label-spaces", "   ", "NTFS-@"),
    ("label-lookalike", "x／y：z", "x_y_z"),
    ("label-c1", "a\u0085b", "a_b"),
    ("label-private", "ab", "a_b"),
    ("label-astral", "\U0001F600 x", "\U0001F600 x"),
    ("label-dotsonly", "...", "NTFS-@"),
    ("label-allrepl", ",,,", "NTFS-@"),
    ("label-nbsp", "a\u00a0b", "a_b"),
    ("label-mark-first", "\u0301x", "_x"),
    ("label-mark-inside", "e\u0301x", "e\u0301x"),
    ("label-varsel", "x\ufe0fy", "x_y"),
    ("label-hangul-filler", "a\u3164b", "a_b"),
]
env = dict(os.environ, LC_ALL="C.UTF-8", PYTHONUTF8="1")


def run(cmd, **kw):
    return subprocess.run(cmd, env=env, capture_output=True, **kw)


def sh(cmd):
    p = run(cmd)
    if p.returncode != 0:
        sys.stderr.write("labels.py: %r failed (%d): %s\n" % (cmd, p.returncode, p.stderr.decode("utf-8", "replace")))
        sys.exit(1)
    return p.stdout


def read_label(img):
    """$Volume's label as the probe sees it: ntfslabel prints it in the locale charset (UTF-8)."""
    return sh(["ntfslabel", img]).rstrip(b"\n")


with open(base, "rb") as f:
    base_serial = struct.unpack_from("<Q", f.read(512), 0x48)[0]
tsv = []
count = 0
for idx, (name, label, leaf) in enumerate(TABLE, 1):
    if only and only != name:
        continue
    count += 1
    img = os.path.join(out, name + ".img")
    shutil.copyfile(base, img)
    # A serial of its own (primary and backup boot sector): two images with the same serial are
    # the clone case of ADR-020, and label-dup-a and label-dup-b are mounted together.
    new_serial = (base_serial + idx * 0x0101010101010101) & 0xFFFFFFFFFFFFFFFF
    with open(img, "r+b") as f:
        size = os.fstat(f.fileno()).st_size
        for off in (0x48, size - 512 + 0x48):
            f.seek(off)
            f.write(struct.pack("<Q", new_serial))
    if label == "":
        p = run(["ntfslabel", "--force", img, ""])
    else:
        p = run(["ntfslabel", "--force", "--", img, label.encode("utf-8")])  # "--": a label may start with "-"
    if p.returncode != 0:
        sys.stderr.write("labels.py: ntfslabel failed for %s: %s\n" % (name, p.stderr.decode("utf-8", "replace")))
        sys.exit(1)
    got = read_label(img)
    if got != label.encode("utf-8"):
        sys.stderr.write("labels.py: %s: label reads back as %r, want %r\n" % (name, got, label.encode("utf-8")))
        sys.exit(1)
    sh(["ntfsfix", "-n", img])
    with open(img, "rb") as f:
        sector0 = f.read(512)
    serial = struct.unpack_from("<Q", sector0, 0x48)[0]
    # the image is otherwise clean: same treedump as the base
    sh(["ntfs-3g", img, mnt, "-o", "ro,norecover,locale=C.UTF-8"])
    try:
        tree = sh([os.path.join(binp, "treedump"), mnt])
    finally:
        sh(["umount", mnt])
    import hashlib
    if hashlib.sha256(tree).hexdigest() != want_sha:
        sys.stderr.write("labels.py: %s: tree differs from the base image\n" % name)
        sys.exit(1)
    # the sanitiser on the label as read back
    want = leaf.replace("@", "%08X" % (serial & 0xFFFFFFFF)).encode("utf-8")
    cp = run([os.path.join(binp, "leaf"), "%X" % serial, got.hex()])
    leaf_got = cp.stdout.rstrip(b"\n")
    if cp.returncode != 0 or leaf_got != want:
        sys.stderr.write("labels.py: %s: leaf %r, want %r\n" % (name, leaf_got, want))
        sys.exit(1)
    with open(os.path.join(out, name + ".leaf"), "wb") as f:
        f.write(want + b"\n")
    with open(os.path.join(out, name + ".label"), "wb") as f:
        f.write(got)
    tsv.append("%s\t%s\t%016X\t%s" % (name, got.hex() or "-", serial, want.decode("utf-8")))
    print("ok       %s serial %016X leaf %r" % (name, serial, want.decode("utf-8")))
if count == 0:
    sys.stderr.write("labels.py: no fixture generated (unknown --only name?)\n")
    sys.exit(1)
if not only:
    with open(os.path.join(out, "labels.tsv"), "w", encoding="utf-8") as f:
        f.write("\n".join(tsv) + "\n")
PYEOF

mounted=0
# shellcheck disable=SC2329  # invoked through trap
cleanup() {
	if [ "$mounted" -eq 1 ]; then umount "$mnt" 2>/dev/null || true; fi
}
trap cleanup EXIT HUP INT TERM

export LC_ALL=C.UTF-8 PYTHONUTF8=1
python3 -I "$bin/labels.py" "$base" "$out" "$only" "$bin" "$mnt" "$base_sha" || die "label generation failed"
if [ "$tar_out" -eq 1 ]; then
	tar -czf - -C "$out" . >&3
fi
