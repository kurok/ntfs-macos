# SPDX-License-Identifier: GPL-2.0-or-later
"""I-INT-01: write 100 seeded random files (1 KiB..64 MiB) to an NTFS volume, unmount,
remount, re-hash, compare (IMPLEMENTATION_PLAN criterion 2.3, first half).
I-INT-03: negative control for the same comparison: a one-byte on-disk change and a
corrupted expected hash must both be reported, or the comparison proves nothing.

Sizes are log-uniform between 1 KiB and 64 MiB (about 6 MiB mean, 600 MB per run); file 0 is
exactly 1 KiB and file 1 exactly 64 MiB so both ends are always covered. Environment
selection and mount/unmount are in harness.volume; env B skips until macFUSE is present.
Dev overrides: NTFSMAC_SEED, NTFSMAC_INT01_FILES, NTFSMAC_INT01_MAX (bytes).
"""

import os

from harness import Fail, case
from harness.seeded import (  # noqa: F401
    CHUNK, DEFAULT_SEED, KIB, MACOS_ROOT_METADATA, MIB, compare, file_stream, hash_tree, image_size as _image_size,
    plan_sizes, rel_name, write_files,
)


def _seed():
    return int(os.environ.get("NTFSMAC_SEED", DEFAULT_SEED))


@case("I-INT-01", envs=("A", "B"), title="100 random files survive unmount and remount (SHA-256)")
def i_int_01(ctx):
    seed = _seed()
    count = int(os.environ.get("NTFSMAC_INT01_FILES", "100"))
    max_size = int(os.environ.get("NTFSMAC_INT01_MAX", str(64 * MIB)))
    sizes = plan_sizes(seed, count, max_size)
    total = sum(sizes)
    ctx.log("seed=%d files=%d min=%d max=%d total=%d bytes" % (seed, count, min(sizes), max(sizes), total))
    vol = ctx.volume(_image_size(total))
    vol.mount(rw=True)
    expected = write_files(vol.mnt, seed, sizes)
    ctx.log("wrote %d files" % len(expected))
    vol.unmount()
    ctx.log("unmounted; remounting read-only")
    vol.mount(rw=False)
    actual = hash_tree(vol.mnt)
    vol.unmount()
    bad = compare(expected, actual)
    ctx.log("compared %d files, %d mismatches" % (len(expected), len(bad)))
    with open(ctx.evidence_file("sha256.txt"), "w", encoding="utf-8") as f:
        for rel in sorted(expected):
            f.write("%s  %d  %s\n" % (expected[rel][1], expected[rel][0], rel))
    if bad:
        for b in bad[:20]:
            ctx.log("MISMATCH " + b)
        raise Fail("%d of %d files differ after remount, first: %s" % (len(bad), len(expected), bad[0]))


@case("I-INT-03", envs=("A", "B"), title="negative control: the remount comparison catches corruption")
def i_int_03(ctx):
    seed = _seed()
    sizes = plan_sizes(seed + 1, 12, max_size=256 * KIB)
    vol = ctx.volume(_image_size(sum(sizes)))
    vol.mount(rw=True)
    expected = write_files(vol.mnt, seed + 1, sizes)
    vol.unmount()

    # Control 1: flip one byte of one file on disk, in place.
    victim = rel_name(3)
    vol.mount(rw=True)
    path = os.path.join(vol.mnt, victim)
    with open(path, "r+b") as f:
        f.seek(expected[victim][0] // 2)
        b = f.read(1)
        f.seek(-1, os.SEEK_CUR)
        f.write(bytes([b[0] ^ 0x01]))
    vol.unmount()
    vol.mount(rw=False)
    actual = hash_tree(vol.mnt)
    vol.unmount()
    bad = compare(expected, actual)
    ctx.log("control 1 (one byte flipped in %s): %s" % (victim, bad))
    if len(bad) != 1 or not bad[0].startswith(victim + ": sha256"):
        raise Fail("one flipped byte was not reported exactly once: %r" % (bad,))

    # Control 2: pristine data but a wrong expected hash for one other file.
    other = rel_name(5)
    tampered = dict(expected)
    tampered[other] = (expected[other][0], "0" * 64)
    clean_actual = dict(actual)
    clean_actual[victim] = expected[victim]  # pretend the victim was intact
    bad2 = compare(tampered, clean_actual)
    ctx.log("control 2 (expected hash corrupted for %s): %s" % (other, bad2))
    if len(bad2) != 1 or not bad2[0].startswith(other + ": sha256"):
        raise Fail("a corrupted expected hash was not reported exactly once: %r" % (bad2,))
    # And with nothing wrong the comparison is silent.
    if compare(expected, dict(expected)):
        raise Fail("comparison reports differences for identical inputs")
