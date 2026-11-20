# SPDX-License-Identifier: GPL-2.0-or-later
"""S-NOSPC-01: low disk space (IMPLEMENTATION_PLAN criterion 5.5, TEST_STRATEGY section 6).

Known files with recorded SHA-256 are written first. Then the volume is filled until writes fail,
and the case checks, in this order:
  1. the failure is errno ENOSPC (not EIO, not a hang: every phase has a deadline), also for the
     file whose write was cut short (the bytes the kernel accepted are exactly the bytes on disk);
  2. the pre-existing files still read back with their recorded hashes while the volume is full;
  3. deleting files gives the space back (statvfs) and a new file can be written and read back;
  4. this is repeated for several rounds (fill, verify, free, write);
  5. after the unmount the image's dirty flag in $Volume is clear and `ntfsfix -n` finds nothing
     to do; a read-only remount shows every surviving file with its hash; the flag is clear again.

S-NOSPC-02 are the negative controls: a wrong errno, a damaged file, a hang, a dirty image and a
damaged $MFTMirr (seen by ntfsfix only) are
each reported by the very checks S-NOSPC-01 uses (a test that cannot fail proves nothing).

Environment: harness.volume (B: guest-built ntfs-3g, backend=fskit, norecover; A: Linux ntfs-3g).
Size of the run, for `run.py --local` only (the harness does not forward the environment to a guest):
NTFSMAC_NOSPC_MIB (image, default 96), NTFSMAC_NOSPC_ROUNDS (default 3), NTFSMAC_NOSPC_FILES (default 24).
"""

import errno
import hashlib
import os
import random
import shlex
import shutil
import signal
import struct

from harness import Fail, case
from harness.seeded import KIB, MIB, compare, hash_tree
from harness.volume import guest_tool

SEED = 20261120
VOLUME_IS_DIRTY = 0x0001
FILL_CHUNK = 256 * KIB      # one write() call while filling
FILL_FILE = 4 * MIB         # size of each fill file
# Deadline of one fill phase: a hang is a failure, not a long wait. SIGALRM cannot interrupt a
# syscall stuck uninterruptibly in FUSE; the harness's own timeout is the backstop for that case.
PHASE_SECONDS = 240


# ---- checks (shared by S-NOSPC-01 and the negative controls of S-NOSPC-02) ---------------------

class Hang(Fail):
    pass


class deadline(object):
    """Context manager: raise Hang if the block runs longer than `seconds` (SIGALRM; POSIX only)."""

    def __init__(self, seconds, what):
        self.seconds = seconds
        self.what = what

    def _fire(self, _sig, _frm):
        raise Hang("%s did not finish within %d s (hang)" % (self.what, self.seconds))

    def __enter__(self):
        self.old = signal.signal(signal.SIGALRM, self._fire)
        signal.setitimer(signal.ITIMER_REAL, self.seconds)
        return self

    def __exit__(self, *exc):
        signal.setitimer(signal.ITIMER_REAL, 0)
        signal.signal(signal.SIGALRM, self.old)
        return False


def require_enospc(err, where):
    """The failure of a write on a full volume must be exactly ENOSPC."""
    if not isinstance(err, OSError) or err.errno != errno.ENOSPC:
        code = errno.errorcode.get(getattr(err, "errno", None), repr(err))
        raise Fail("%s: write failed with %s (%s), expected ENOSPC" % (where, code, err))


def content(tag, index, size):
    """Deterministic bytes of one file, in FILL_CHUNK pieces."""
    rng = random.Random("%s:%d:%d" % (tag, SEED, index))
    left = size
    while left > 0:
        n = min(FILL_CHUNK, left)
        yield rng.randbytes(n)
        left -= n


def write_known(path, tag, index, size):
    """Write a whole file; return (size, sha256)."""
    h = hashlib.sha256()
    with open(path, "wb") as f:
        for b in content(tag, index, size):
            h.update(b)
            f.write(b)
    return size, h.hexdigest()


def fill_until_full(root, tag, first_index, log):
    """Write fill files until a write fails. Returns ({relpath: (size, sha256)}, errno of the failure
    that ended the fill, bytes written). The size and hash of the last (short) file are those of the
    bytes write() reported as accepted. Anything but ENOSPC raises Fail."""
    expected = {}
    total = 0
    index = first_index
    os.makedirs(os.path.join(root, "fill"), exist_ok=True)
    failure = None
    while failure is None:
        rel = "fill/%s-%05d.bin" % (tag, index)
        path = os.path.join(root, rel)
        h = hashlib.sha256()
        written = 0
        fd = None
        unspecified = False     # a delayed error from close(): the content on disk is not defined
        try:
            fd = os.open(path, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            for b in content(tag, index, FILL_FILE):
                view = memoryview(b)
                while view:
                    n = os.write(fd, view)   # a short count is accepted data; the next call fails
                    h.update(view[:n])
                    written += n
                    view = view[n:]
        except OSError as e:
            failure = e
            require_enospc(e, "%s after %d bytes of this file, %d in all" % (rel, written, total + written))
        finally:
            if fd is not None:
                try:
                    os.close(fd)
                except OSError as e:
                    # close() may report a delayed write error; it must be ENOSPC too
                    unspecified = True
                    if failure is None:
                        failure = e
                    require_enospc(e, "close of " + rel)
        if unspecified:
            # the bytes of this file are not defined: leave it out of the hash set and delete it
            log("%s: close() failed with ENOSPC, content unspecified, deleted" % rel)
            try:
                os.unlink(path)
            except OSError as e:
                if e.errno != errno.ENOENT:
                    raise Fail("cannot remove %s after a failed close: %s" % (rel, e))
        elif os.path.exists(path):
            expected[rel] = (written, h.hexdigest())
        total += written
        index += 1
        if failure is None and index - first_index > 100000:
            raise Fail("fill never failed after %d files" % (index - first_index))
    log("fill ended after %d files (%d bytes) with %s" % (len(expected), total, errno.errorcode[failure.errno]))
    return expected, failure.errno, total


def top_up_small(root, tag, log):
    """With the big fill done, write 4 KiB files until even those fail (fills the last clusters).
    Returns the new expected entries; the failure must be ENOSPC."""
    expected = {}
    os.makedirs(os.path.join(root, "small"), exist_ok=True)
    for i in range(100000):
        rel = "small/%s-%05d.bin" % (tag, i)
        data = random.Random("%s-small:%d:%d" % (tag, SEED, i)).randbytes(4 * KIB)
        try:
            with open(os.path.join(root, rel), "wb") as f:
                f.write(data)
        except OSError as e:
            require_enospc(e, rel)
            # a file that was created but could not be written fully is not part of the data set
            try:
                os.unlink(os.path.join(root, rel))
            except OSError as e2:
                if e2.errno != errno.ENOENT:
                    raise Fail("cannot remove the failed %s: %s" % (rel, e2))
            log("small-file fill ended after %d files with ENOSPC" % len(expected))
            return expected
        expected[rel] = (len(data), hashlib.sha256(data).hexdigest())
    raise Fail("small-file fill never failed")


def free_bytes(root):
    st = os.statvfs(root)
    return st.f_bavail * st.f_frsize


def read_volume_flags(img):
    """The 16-bit volume flags of $Volume ($VOLUME_INFORMATION) of an NTFS image file, read without
    mounting it (boot sector -> $MFT -> record 3, update sequence applied). Raises Fail on a layout
    that is not NTFS."""
    return _volume_flags(img)[0]


def set_volume_flags(img, flags):
    """Test fixture: overwrite the volume flags of an image file (the field sits far from the end of
    the first 512-byte sector of the record, so no update-sequence fix-up is touched)."""
    off = _volume_flags(img)[1]
    with open(img, "r+b") as f:
        f.seek(off)
        f.write(struct.pack("<H", flags))


def _volume_flags(img):
    """(flags, absolute file offset of the flags field)."""
    with open(img, "rb") as f:
        boot = f.read(512)
        if boot[3:11] != b"NTFS    ":
            raise Fail("%s: no NTFS boot sector" % img)
        bps, spc = struct.unpack_from("<HB", boot, 0x0B)
        mft_lcn, = struct.unpack_from("<Q", boot, 0x30)
        cpm, = struct.unpack_from("<b", boot, 0x40)
        cluster = bps * spc
        recsize = (1 << -cpm) if cpm < 0 else cpm * cluster
        rec_pos = mft_lcn * cluster + 3 * recsize
        f.seek(rec_pos)
        rec = bytearray(f.read(recsize))
    if rec[:4] != b"FILE":
        raise Fail("%s: $Volume record has no FILE signature" % img)
    usa_off, usa_n = struct.unpack_from("<HH", rec, 4)
    usa = bytes(rec[usa_off:usa_off + 2 * usa_n])
    for i in range(1, usa_n):        # the update sequence protects every 512-byte block, whatever bps is
        end = i * 512
        if end > len(rec):
            break
        rec[end - 2:end] = usa[2 * i:2 * i + 2]
    off, = struct.unpack_from("<H", rec, 0x14)
    while off + 16 <= len(rec):
        atype, alen = struct.unpack_from("<II", rec, off)
        if atype == 0xFFFFFFFF or alen == 0:
            break
        if atype == 0x70:
            if rec[off + 8] != 0:
                raise Fail("%s: $VOLUME_INFORMATION is non-resident" % img)
            voff, = struct.unpack_from("<H", rec, off + 0x14)
            flags, = struct.unpack_from("<H", rec, off + voff + 10)
            if off + voff + 10 >= 510:
                raise Fail("%s: volume flags beyond the first sector of the record" % img)
            return flags, rec_pos + off + voff + 10
        off += alen
    raise Fail("%s: $VOLUME_INFORMATION not found in $Volume" % img)


def ntfsfix_check(ctx, img):
    """`ntfsfix -n IMG` (no writes): returns (rc, output). A missing tool is a failure: ntfsprogs is
    built in the guest (run-env-b.sh) and is in the Environment A image."""
    tool = guest_tool("ntfsfix") or shutil.which("ntfsfix")
    if tool is None:
        raise Fail("ntfsfix not found: the cleanliness check cannot run")
    r = ctx.sh("%s -n %s" % (shlex.quote(tool), shlex.quote(img)), check=False)
    return r.rc, (r.out + r.err).decode("utf-8", "replace")


# ntfsfix -n prints these when it finds something it would repair (it cannot, with -n)
NTFSFIX_DAMAGE = ("FAILED", "Correcting differences", "Error", "error")


def require_clean_image(ctx, img, where):
    """Cleanliness of an unmounted image, without the daemon: the dirty bit of $Volume is clear and
    `ntfsfix -n` exits 0 with no repair message (exit code and output are both checked).

    The dirty bit alone proves little: libntfs-3g does not set or clear VOLUME_IS_DIRTY during a
    mount (NTFS-3G 2026.9.28 source: only ntfsfix, mkntfs, ntfsresize and similar tools touch it;
    ntfsfix.c says so in a porting note), so after a session it only guards against dirtiness the
    image already had. What shows the session ended cleanly is the umount exit code, the driver
    having exited (harness.volume.unmount) and this ntfsfix check ($MFT against $MFTMirr, alternate
    boot sector)."""
    flags = read_volume_flags(img)
    ctx.log("%s: $Volume flags 0x%04x" % (where, flags))
    if flags & VOLUME_IS_DIRTY:
        raise Fail("%s: the image is left dirty (volume flags 0x%04x)" % (where, flags))
    rc, text = ntfsfix_check(ctx, img)
    if rc != 0:
        raise Fail("%s: ntfsfix -n exit %d: %s" % (where, rc, text.strip()[-300:]))
    hits = [w for w in NTFSFIX_DAMAGE if w in text]
    if hits:
        raise Fail("%s: ntfsfix -n reports %s: %s" % (where, hits, text.strip()[-300:]))


def corrupt_mftmirr(img):
    """Test fixture: change the sequence number of record 0 in $MFTMirr only, so that it differs from
    $MFT record 0 (the dirty flag stays clear)."""
    with open(img, "r+b") as f:
        boot = f.read(512)
        bps, spc = struct.unpack_from("<HB", boot, 0x0B)
        mirr_lcn, = struct.unpack_from("<Q", boot, 0x38)
        f.seek(mirr_lcn * bps * spc + 0x10)
        v = f.read(2)
        f.seek(mirr_lcn * bps * spc + 0x10)
        f.write(bytes([v[0] ^ 0x5A, v[1]]))


# ---- S-NOSPC-01 ----------------------------------------------------------------------------------

def _params():
    return (int(os.environ.get("NTFSMAC_NOSPC_MIB", "96")) * MIB,
            int(os.environ.get("NTFSMAC_NOSPC_ROUNDS", "3")),
            int(os.environ.get("NTFSMAC_NOSPC_FILES", "24")))


def _verify(root, expected, where, ctx, only=None):
    actual = hash_tree(root)
    if only is not None:
        actual = {k: v for k, v in actual.items() if k in only}
        expected = {k: v for k, v in expected.items() if k in only}
    bad = compare(expected, actual)
    ctx.log("%s: %d files compared, %d mismatches" % (where, len(expected), len(bad)))
    if bad:
        for b in bad[:20]:
            ctx.log("MISMATCH " + b)
        raise Fail("%s: %d mismatches, first: %s" % (where, len(bad), bad[0]))


@case("S-NOSPC-01", envs=("A", "B"), title="fill the volume: ENOSPC, earlier files intact, space reusable, clean after unmount")
def s_nospc_01(ctx):
    size, rounds, nfiles = _params()
    vol = ctx.volume(size)
    vol.mount(rw=True)
    root = vol.mnt
    ctx.log("image %d MiB, %d rounds, %d pre-existing files" % (size // MIB, rounds, nfiles))

    # Pre-existing files: sizes from 1 KiB to 512 KiB, one in a subdirectory tree, hashes recorded.
    rng = random.Random("sizes:%d" % SEED)
    pre = {}
    os.makedirs(os.path.join(root, "pre", "sub"), exist_ok=True)
    for i in range(nfiles):
        n = KIB if i == 0 else rng.randint(KIB, 512 * KIB)
        rel = "pre/%s%03d.bin" % ("sub/" if i % 3 == 0 else "", i)
        pre[rel] = write_known(os.path.join(root, rel), "pre", i, n)
    os.sync()
    _verify(root, pre, "before the fill", ctx)
    free0 = free_bytes(root)
    ctx.log("free after the pre-existing files: %d bytes" % free0)

    live = dict(pre)         # every file that must exist and match at the end
    next_index = 0
    for rnd in range(1, rounds + 1):
        tag = "r%d" % rnd
        with deadline(PHASE_SECONDS, "fill round %d" % rnd):
            filled, err, total = fill_until_full(root, tag, next_index, ctx.log)
            next_index += len(filled) + 1
            small = top_up_small(root, tag, ctx.log)
        if err != errno.ENOSPC:
            raise Fail("round %d: fill ended with errno %s" % (rnd, err))
        if total < free0 // 2:
            raise Fail("round %d: only %d bytes written before ENOSPC, of %d free: the volume was not filled"
                       % (rnd, total, free0))
        live.update(filled)
        live.update(small)
        left = free_bytes(root)
        ctx.log("round %d: full, free %d bytes" % (rnd, left))
        if left > 1 * MIB:
            raise Fail("round %d: ENOSPC with %d bytes still free" % (rnd, left))

        # The volume is full: no earlier file may have changed, and the short file is what was accepted.
        with deadline(PHASE_SECONDS, "verify on the full volume (round %d)" % rnd):
            _verify(root, live, "round %d, volume full" % rnd, ctx)

        # Creating things on a full volume may succeed (MFT slack) or fail: only ENOSPC is allowed.
        # A probe that exists but cannot be removed (ENOSPC) is removed again after the space is freed.
        leftovers = []
        for probe in ("probe-dir-%d" % rnd, "probe-file-%d" % rnd):
            p = os.path.join(root, probe)
            try:
                if probe.startswith("probe-dir"):
                    os.mkdir(p)
                else:
                    open(p, "wb").close()
            except OSError as e:
                require_enospc(e, "creating %s on the full volume" % probe)
                continue
            try:
                (os.rmdir if probe.startswith("probe-dir") else os.unlink)(p)
            except OSError as e:
                require_enospc(e, "removing %s on the full volume" % probe)
                leftovers.append(p)

        # Delete: every fill file of this round (and the small files of the round), keep the rest.
        before = free_bytes(root)
        victims = [k for k in live if k.startswith("fill/%s-" % tag) or k.startswith("small/%s-" % tag)]
        freed = sum(live[k][0] for k in victims)
        for k in victims:
            os.unlink(os.path.join(root, k))
            del live[k]
        os.sync()
        after = free_bytes(root)
        ctx.log("round %d: deleted %d files (%d bytes), free %d -> %d" % (rnd, len(victims), freed, before, after))
        if after - before < freed * 0.9:
            raise Fail("round %d: deleting %d bytes freed only %d" % (rnd, freed, after - before))

        # Writing works again, with a hash, and the old files are still right.
        for p in leftovers:
            (os.rmdir if os.path.isdir(p) else os.unlink)(p)
        again = "again/r%d.bin" % rnd
        os.makedirs(os.path.join(root, "again"), exist_ok=True)
        live[again] = write_known(os.path.join(root, again), "again", rnd, 2 * MIB)
        os.sync()
        _verify(root, live, "round %d, after freeing space and writing again" % rnd, ctx)
        # the next fill starts from this point; the next free0 is what is free now
        free0 = free_bytes(root)

    vol.unmount()
    require_clean_image(ctx, vol.img, "after the unmount")
    vol.mount(rw=False)
    try:
        _verify(root, live, "read-only remount", ctx)
    finally:
        vol.unmount()
    require_clean_image(ctx, vol.img, "after the read-only remount")
    with open(ctx.evidence_file("sha256.txt"), "w", encoding="utf-8") as f:
        for rel in sorted(live):
            f.write("%s  %d  %s\n" % (live[rel][1], live[rel][0], rel))
    ctx.log("PASS: %d files intact; image clean" % len(live))


# ---- S-NOSPC-02: negative controls ----------------------------------------------------------------

def _must_fail(what, fn):
    try:
        fn()
    except Fail:
        return
    raise Fail("control not detected: %s" % what)


@case("S-NOSPC-02", envs=("A", "B"), title="negative controls: wrong errno, damaged file, hang and dirty image are reported")
def s_nospc_02(ctx):
    # 1. an errno other than ENOSPC is rejected, ENOSPC accepted
    for code in (errno.EIO, errno.EDQUOT, errno.EROFS, errno.EFBIG):
        _must_fail("errno %s" % errno.errorcode[code], lambda c=code: require_enospc(OSError(c, "x"), "control"))
    _must_fail("a non-OSError", lambda: require_enospc(ValueError("x"), "control"))
    require_enospc(OSError(errno.ENOSPC, "x"), "control")
    ctx.log("control 1: only ENOSPC passes")

    # 2. a hang is reported
    def hang():
        import time
        with deadline(1, "control sleep"):
            time.sleep(5)
    _must_fail("a hang", hang)
    ctx.log("control 2: the deadline fires")

    # 3. the comparison reports a damaged and a missing pre-existing file
    exp = {"a": (10, "0" * 64), "b": (10, "1" * 64)}
    if compare(exp, dict(exp)):
        raise Fail("control 3: identical trees reported as different")
    if len(compare(exp, {"a": (10, "0" * 64), "b": (10, "2" * 64)})) != 1:
        raise Fail("control 3: a changed hash was not reported exactly once")
    if len(compare(exp, {"a": (10, "0" * 64)})) != 1:
        raise Fail("control 3: a missing file was not reported")
    ctx.log("control 3: damage is reported")

    # 4. a dirty image is reported: the image of a clean unmounted volume with the dirty bit set
    #    (what a session that never reached a clean unmount leaves; ntfs-3g's own flag write is not
    #    visible in the image while mounted, so the bit is set in a copy).
    vol = ctx.volume(32 * MIB)
    vol.mount(rw=True)
    with open(os.path.join(vol.mnt, "x.bin"), "wb") as f:
        f.write(b"x" * 4096)
    vol.unmount()
    require_clean_image(ctx, vol.img, "after unmount")
    bad = os.path.join(ctx.work, "dirty.img")
    shutil.copyfile(vol.img, bad)
    set_volume_flags(bad, read_volume_flags(bad) | VOLUME_IS_DIRTY)
    ctx.log("control 4: flags of the copy with the dirty bit set: 0x%04x" % read_volume_flags(bad))
    _must_fail("a dirty image", lambda: require_clean_image(ctx, bad, "dirty copy"))
    ctx.log("control 4: a dirty image is reported, the unmounted image passes")

    # 5. the ntfsfix part can fail on its own: $MFTMirr record 0 differs from $MFT record 0, the dirty
    #    flag is clear, so only ntfsfix -n can see it.
    mm = os.path.join(ctx.work, "mirror.img")
    shutil.copyfile(vol.img, mm)
    corrupt_mftmirr(mm)
    if read_volume_flags(mm) & VOLUME_IS_DIRTY:
        raise Fail("control 5: the fixture is dirty, it must differ only in $MFTMirr")
    rc, text = ntfsfix_check(ctx, mm)
    ctx.log("control 5: ntfsfix -n on the $MFTMirr copy: exit %d, %r" % (rc, text.strip()[-200:]))
    _must_fail("a damaged $MFTMirr", lambda: require_clean_image(ctx, mm, "mirror copy"))
    ctx.log("control 5: a damaged $MFTMirr is reported by ntfsfix")
