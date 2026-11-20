# SPDX-License-Identifier: GPL-2.0-or-later
"""mixlib.py - mixed-workload library of the soak and parallel suites (T-M5-02, S-SOAK-01, S-PAR-01).

Runs in the macOS guest against a mounted NTFS volume (and on any directory, for the host selftest).
Standard library only (guest /usr/bin/python3, 3.9).

Every worker owns a private tree (MP/KIND/wNN) and keeps a complete model of it in memory: every
file's bytes and every directory. The operations are sequential and random writes, small files, rename
(also over an existing name), truncate (shrink and extend), mmap writes, directory churn (mkdir, rmdir,
directory rename, the ENOTEMPTY error path), deletes and readdir. After each operation the touched file
is sometimes read back; every `verify_every` seconds, and at the end, the whole tree is read and compared
byte for byte with the model (names, sizes, contents). A worker stops at its first mismatch or unexpected
error and reports it.

The parallel kind adds shared directories (MP/par/shared/sK): each worker creates files there under a name
that carries its content key and size (content = SHAKE-256(key, size)), published by rename of a temporary
name, renames them (only the generation suffix changes), deletes them, and reads and verifies the files
of the OTHER workers by name; the listing of a shared directory must hold exactly the worker's own names.

Hang detection: every operation is timed; a slot per worker in an anonymous shared mapping holds the start
of the operation in flight and the end of the last one. The parent (cmd_run) treats "now - last progress >
hang_limit" for any worker as a hang, kills the workers and reports it; the longest single operation is
also compared with the limit at the end.

At the end each worker writes a manifest (state/man-NN.tsv: F path size sha256, D path); `verify` walks the
volume after a remount and compares it with the union of the manifests (extra and missing entries too).
"""
import argparse
import errno
import hashlib
import json
import math
import mmap
import os
import random
import re
import signal
import struct
import subprocess
import sys
import time

OPS = ["create", "seqwrite", "randwrite", "truncate", "rename", "mmap", "delete", "mkdir", "rmdir", "dirrename",
       "readdir", "readback", "verify", "sh_create", "sh_rename", "sh_delete", "sh_read", "sh_list", "setup", "stall"]
OPI = {n: i for i, n in enumerate(OPS)}
SLOT = 64  # bytes per worker slot: begin, last, maxlat, ops, opcode, done, -, -  (8 doubles)


class Mismatch(Exception):
    pass


# ---------------------------------------------------------------------------------------------
# pure functions (selftest, host side of the result)
# ---------------------------------------------------------------------------------------------
def rss_growth(samples, baseline_s, limit_pct=10.0, min_after=3, max_lag_s=60.0):
    """The soak rule of criterion 5.2: samples [(t_s, rss_kb)] in time order, t_s from the start of the run.
    Baseline = the first sample at or after baseline_s (the end of hour 1). Growth = (peak after the baseline
    - baseline) * 100 / baseline, the peak over every sample from the baseline on. status: pass when the growth
    is below limit_pct (equal fails: "below 10 percent"), fail when not, error when the rule cannot be applied
    (no baseline sample, the baseline sample later than max_lag_s after the point, fewer than min_after samples
    after it, a baseline of 0)."""
    base = None
    for t, r in samples:
        if t >= baseline_s:
            base = (t, r)
            break
    if base is None:
        return {"status": "error", "why": "no sample at or after the baseline point %.0f s" % baseline_s}
    if base[0] - baseline_s > max_lag_s:
        return {"status": "error", "why": "first sample after the baseline point is %.0f s late" % (base[0] - baseline_s)}
    if base[1] <= 0:
        return {"status": "error", "why": "baseline RSS is %s kB" % base[1]}
    after = [(t, r) for t, r in samples if t > base[0]]
    if len(after) < min_after:
        return {"status": "error", "why": "only %d sample(s) after the baseline (need %d)" % (len(after), min_after)}
    peak = max(r for _, r in after)
    final = after[-1][1]
    growth = (peak - base[1]) * 100.0 / base[1]
    return {"status": "pass" if growth < limit_pct else "fail", "baseline_kb": base[1], "baseline_t": base[0],
            "peak_kb": peak, "final_kb": final, "growth_pct": round(growth, 3),
            "final_growth_pct": round((final - base[1]) * 100.0 / base[1], 3), "limit_pct": limit_pct,
            "samples_after": len(after)}


def shake(key, n):
    return hashlib.shake_256(key.encode()).digest(n) if n else b""


def shared_key(name):
    return name.rsplit(".g", 1)[0]


def shared_size(name):
    m = re.search(r"-z(\d+)$", shared_key(name))
    return int(m.group(1)) if m else None


def sha_hex(b):
    return hashlib.sha256(b).hexdigest()


def bucket(lat):
    """log2 bucket of a latency in milliseconds: 0 = under 1 ms, k = under 2**k ms"""
    ms = lat * 1000.0
    return 0 if ms < 1 else min(int(math.log2(ms)) + 1, 24)


def hist_percentile(hist, q):
    """upper bound (seconds) of the bucket that holds the q-quantile"""
    tot = sum(hist)
    if not tot:
        return 0.0
    need = q * tot
    acc = 0
    for k, c in enumerate(hist):
        acc += c
        if acc >= need:
            return (2 ** k) / 1000.0
    return (2 ** (len(hist) - 1)) / 1000.0


# ---------------------------------------------------------------------------------------------
# shared slots
# ---------------------------------------------------------------------------------------------
class Slots:
    def __init__(self, n):
        self.m = mmap.mmap(-1, SLOT * max(n, 1))

    def get(self, i):
        return tuple(struct.unpack_from("d", self.m, i * SLOT + 8 * k)[0] for k in range(8))

    def put(self, i, v):
        # one aligned 8-byte store per field: a reader never sees a half-written double (a 64-byte store of the whole
        # slot, as one memcpy or as eight stores of one pack_into, was measured to tear). The fields are not read
        # as a consistent set: the hang decision rests on field 1 (`last`) alone, confirmed by a second poll.
        for k in range(8):
            struct.pack_into("d", self.m, i * SLOT + 8 * k, v[k])


# ---------------------------------------------------------------------------------------------
# the worker
# ---------------------------------------------------------------------------------------------
class Worker:
    def __init__(self, cfg, wid, slots):
        self.c = cfg
        self.wid = wid
        self.slots = slots
        self.rng = random.Random(cfg["seed"] * 1000 + wid)
        self.root = os.path.join(cfg["mp"], cfg["kind"], "w%02d" % wid)
        self.files = {}  # rel -> bytearray
        self.dirs = {""}
        self.total = 0
        self.nname = 0
        self.sh = {}  # rel (sK/name) -> size: own shared files
        self.shn = 0
        self.ops = {}
        self.lat = {}
        self.hist = [0] * 25
        self.slow = []  # (secs, op)
        self.mism = []
        self.errs = []
        self.verifies = 0
        self.bytes_written = 0
        self.bytes_verified = 0
        self.stall_done = False
        self.t0 = time.time()
        self.cur = [0.0, time.time(), 0.0, 0.0, 0.0, 0.0, 0.0, 0.0]
        self.slots.put(wid, self.cur)

    # ---- timing ----
    def timed(self, op, fn, *a):
        t = time.time()
        c = self.cur
        c[0] = t
        c[4] = float(OPI[op])
        self.slots.put(self.wid, c)
        try:
            st = self.c.get("stall")
            if st and st[0] == self.wid and not self.stall_done and t - self.t0 >= st[1]:
                self.stall_done = True
                time.sleep(st[2])  # a simulated stall inside the timed window (control)
            return fn(*a)
        finally:
            t2 = time.time()
            d = t2 - t
            c[0] = 0.0
            c[1] = t2
            c[2] = max(c[2], d)
            c[3] += 1
            self.slots.put(self.wid, c)
            self.ops[op] = self.ops.get(op, 0) + 1
            self.lat[op] = max(self.lat.get(op, 0.0), d)
            self.hist[bucket(d)] += 1
            if d > 1.0:
                self.slow.append((round(d, 3), op))
                self.slow = sorted(self.slow, reverse=True)[:5]

    # ---- helpers ----
    def ap(self, rel):
        return os.path.join(self.root, rel) if rel else self.root

    def rb(self, n):
        return self.rng.getrandbits(8 * n).to_bytes(n, "little") if n else b""

    def name(self, p):
        self.nname += 1
        return "%s%06d" % (p, self.nname)

    def pick_file(self):
        return self.rng.choice(sorted(self.files)) if self.files else None

    def pick_dir(self):
        return self.rng.choice(sorted(self.dirs))

    @staticmethod
    def join(d, n):
        return d + "/" + n if d else n

    def put_model(self, rel, data):
        old = self.files.get(rel)
        if old is not None:
            self.total -= len(old)
        self.files[rel] = bytearray(data)
        self.total += len(data)

    def fix_total(self):
        self.total = sum(len(v) for v in self.files.values())

    def read_all(self, path):
        with open(path, "rb") as f:
            return f.read()

    def cmp(self, rel, got):
        exp = self.files[rel]
        if got != bytes(exp):
            off = next((i for i in range(min(len(got), len(exp))) if got[i] != exp[i]), min(len(got), len(exp)))
            raise Mismatch("%s: content differs (model %d bytes, disk %d bytes, first difference at offset %d)" % (rel, len(exp), len(got), off))

    def readback(self, rel):
        self.cmp(rel, self.read_all(self.ap(rel)))
        self.bytes_verified += len(self.files[rel])

    def maybe_readback(self, rel):
        if rel in self.files and self.rng.random() < 0.3:
            self.readback(rel)

    # ---- private operations ----
    def op_create(self):
        d = self.pick_dir()
        rel = self.join(d, self.name("f"))
        n = self.rng.choice((0, 1, 17, 100, 511, 512, 513, 1000, 4095, 4096, 4097, 6000, 8192))
        data = self.rb(n)
        fd = os.open(self.ap(rel), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        try:
            self.wall(fd, data)
        finally:
            os.close(fd)
        self.put_model(rel, data)
        self.bytes_written += n
        self.maybe_readback(rel)

    def wall(self, fd, data, off=None):
        mv = memoryview(data)
        pos = 0
        while pos < len(data):
            w = os.write(fd, mv[pos:pos + 65536]) if off is None else os.pwrite(fd, mv[pos:pos + 65536], off + pos)
            pos += w

    def op_seqwrite(self):
        cands = [r for r, v in self.files.items() if len(v) < self.c["filecap"] // 2]
        if cands and self.rng.random() < 0.7:
            rel = self.rng.choice(sorted(cands))
        else:
            rel = self.join(self.pick_dir(), self.name("big"))
            os.close(os.open(self.ap(rel), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644))
            self.put_model(rel, b"")
        cur = len(self.files[rel])
        n = min(self.rng.choice((4096, 65536, 65536 * 2, 65536 * 4)), self.c["filecap"] - cur)
        data = self.rb(n)
        fd = os.open(self.ap(rel), os.O_WRONLY)
        try:
            os.lseek(fd, cur, os.SEEK_SET)
            self.wall(fd, data)
        finally:
            os.close(fd)
        self.files[rel].extend(data)
        self.total += n
        self.bytes_written += n
        self.maybe_readback(rel)

    def op_randwrite(self):
        rel = self.pick_file()
        if rel is None:
            return self.op_create()
        cur = len(self.files[rel])
        off = self.rng.randint(0, min(cur + 8192, self.c["filecap"] - 1))
        n = self.rng.randint(1, min(16384, self.c["filecap"] - off))
        data = self.rb(n)
        fd = os.open(self.ap(rel), os.O_WRONLY)
        try:
            self.wall(fd, data, off)
        finally:
            os.close(fd)
        m = self.files[rel]
        if off > len(m):
            m.extend(b"\0" * (off - len(m)))
        m[off:off + n] = data
        self.fix_total()
        self.bytes_written += n
        self.maybe_readback(rel)

    def op_truncate(self):
        rel = self.pick_file()
        if rel is None:
            return self.op_create()
        cur = len(self.files[rel])
        new = self.rng.randint(0, min(max(cur * 2, 1), self.c["filecap"]))
        os.truncate(self.ap(rel), new)
        m = self.files[rel]
        if new < len(m):
            del m[new:]
        else:
            m.extend(b"\0" * (new - len(m)))
        self.fix_total()
        self.maybe_readback(rel)

    def op_rename(self):
        src = self.pick_file()
        if src is None:
            return self.op_create()
        if len(self.files) > 1 and self.rng.random() < 0.3:
            dst = self.rng.choice(sorted(r for r in self.files if r != src))  # over an existing file
        else:
            dst = self.join(self.pick_dir(), self.name("r"))
        os.rename(self.ap(src), self.ap(dst))
        data = self.files.pop(src)
        self.files[dst] = data
        self.fix_total()
        if src not in self.files and os.path.exists(self.ap(src)):
            raise Mismatch("%s still exists after the rename to %s" % (src, dst))
        self.maybe_readback(dst)

    def op_mmap(self):
        cands = [r for r, v in self.files.items() if 0 < len(v) <= 1 << 20]
        if not cands:
            return self.op_create()
        rel = self.rng.choice(sorted(cands))
        m = self.files[rel]
        fd = os.open(self.ap(rel), os.O_RDWR)
        try:
            mm = mmap.mmap(fd, len(m))
            try:
                if mm[:] != bytes(m):
                    raise Mismatch("%s: mmap read differs from the model before the write" % rel)
                off = self.rng.randint(0, len(m) - 1)
                n = self.rng.randint(1, min(8192, len(m) - off))
                data = self.rb(n)
                mm[off:off + n] = data
                mm.flush()
                m[off:off + n] = data
                self.bytes_written += n
            finally:
                mm.close()
        finally:
            os.close(fd)
        self.maybe_readback(rel)

    def op_delete(self):
        rel = self.pick_file()
        if rel is None:
            return self.op_create()
        os.unlink(self.ap(rel))
        self.total -= len(self.files.pop(rel))
        if os.path.exists(self.ap(rel)):
            raise Mismatch("%s still exists after unlink" % rel)

    def op_mkdir(self):
        cands = [d for d in self.dirs if d.count("/") < 3]
        if len(self.dirs) >= 40 or not cands:
            return self.op_rmdir()
        rel = self.join(self.rng.choice(sorted(cands)), self.name("d"))
        os.mkdir(self.ap(rel))
        self.dirs.add(rel)

    def nonempty(self, d):
        p = d + "/"
        return any(r.startswith(p) for r in self.files) or any(x.startswith(p) for x in self.dirs)

    def op_rmdir(self):
        empty = [d for d in self.dirs if d and not self.nonempty(d)]
        full = [d for d in self.dirs if d and self.nonempty(d)]
        if full and self.rng.random() < 0.3:  # the error path: rmdir of a non-empty directory
            d = self.rng.choice(sorted(full))
            try:
                os.rmdir(self.ap(d))
            except OSError as e:
                if e.errno not in (errno.ENOTEMPTY, errno.EEXIST):
                    raise Mismatch("rmdir of non-empty %s: errno %s, expected ENOTEMPTY" % (d, e.errno))
                return
            raise Mismatch("rmdir of non-empty directory %s succeeded" % d)
        if not empty:
            return self.op_mkdir()
        d = self.rng.choice(sorted(empty))
        os.rmdir(self.ap(d))
        self.dirs.discard(d)

    def op_dirrename(self):
        cands = [d for d in self.dirs if d]
        if not cands:
            return self.op_mkdir()
        src = self.rng.choice(sorted(cands))
        parents = [d for d in self.dirs if d != src and not d.startswith(src + "/") and (d.count("/") + src.count("/") - src.count("/")) < 3]
        if not parents:
            return
        dst = self.join(self.rng.choice(sorted(parents)), self.name("d"))
        depth_sub = max([x.count("/") - src.count("/") for x in self.dirs if x.startswith(src + "/")] + [0])
        if dst.count("/") + depth_sub > 4:
            return
        os.rename(self.ap(src), self.ap(dst))
        p = src + "/"
        self.dirs = {(dst + x[len(src):] if (x == src or x.startswith(p)) else x) for x in self.dirs}
        self.files = {(dst + r[len(src):] if r.startswith(p) else r): v for r, v in self.files.items()}

    def check_dir(self, d):
        want = set()
        p = d + "/" if d else ""
        for r in self.files:
            if r.startswith(p) and "/" not in r[len(p):]:
                want.add(r[len(p):])
        for x in self.dirs:
            if x and x.startswith(p) and "/" not in x[len(p):]:
                want.add(x[len(p):])
        got = set(os.listdir(self.ap(d)))
        if d == "":
            pass
        if got != want:
            raise Mismatch("readdir of '%s': missing %s, extra %s" % (d or ".", sorted(want - got)[:5], sorted(got - want)[:5]))

    def op_readdir(self):
        self.check_dir(self.pick_dir())

    def op_readback(self):
        rel = self.pick_file()
        if rel is None:
            return self.op_create()
        self.readback(rel)
        st = os.stat(self.ap(rel))
        if st.st_size != len(self.files[rel]):
            raise Mismatch("%s: stat size %d, model %d" % (rel, st.st_size, len(self.files[rel])))

    # ---- shared operations ----
    def sdir(self, k):
        return os.path.join(self.c["mp"], self.c["kind"], "shared", "s%d" % k)

    def shared_bytes(self):
        return sum(self.sh.values())

    def op_sh_create(self):
        if len(self.sh) >= self.c["shared_max"]:
            return self.op_sh_delete()  # the shared set is bounded: a create at the limit becomes a delete
        k = self.rng.randrange(self.c["nshared"])
        self.shn += 1
        n = self.rng.choice((0, 100, 1000, 4096, 5000, 12000))
        key = "w%02d-t%06d-z%d" % (self.wid, self.shn, n)
        final = key + ".g0.dat"
        tmp = ".tmp-" + key
        data = shake(key, n)
        fd = os.open(os.path.join(self.sdir(k), tmp), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
        try:
            self.wall(fd, data)
        finally:
            os.close(fd)
        os.rename(os.path.join(self.sdir(k), tmp), os.path.join(self.sdir(k), final))
        self.sh["s%d/%s" % (k, final)] = n
        self.bytes_written += n

    def op_sh_rename(self):
        if not self.sh:
            return self.op_sh_create()
        rel = self.rng.choice(sorted(self.sh))
        k, name = rel.split("/")
        base, g = name.rsplit(".g", 1)
        new = "%s.g%d.dat" % (base, int(g.split(".")[0]) + 1)
        os.rename(os.path.join(self.sdir(int(k[1:])), name), os.path.join(self.sdir(int(k[1:])), new))
        self.sh[k + "/" + new] = self.sh.pop(rel)

    def op_sh_delete(self):
        if not self.sh:
            return
        rel = self.rng.choice(sorted(self.sh))
        k, name = rel.split("/")
        os.unlink(os.path.join(self.sdir(int(k[1:])), name))
        del self.sh[rel]

    def check_shared_file(self, path, name, own):
        n = shared_size(name)
        try:
            got = self.read_all(path)
        except OSError as e:
            if e.errno == errno.ENOENT and not own:
                return  # another worker renamed or deleted it between the listing and the open
            raise
        if got != shake(shared_key(name), n):
            raise Mismatch("shared %s: content differs from its name-derived content (%d bytes on disk, %s expected)" % (name, len(got), n))
        self.bytes_verified += len(got)

    def op_sh_read(self):
        k = self.rng.randrange(self.c["nshared"])
        names = [x for x in os.listdir(self.sdir(k)) if x.endswith(".dat") and not x.startswith(".tmp-")]
        for name in self.rng.sample(names, min(3, len(names))):
            own = ("s%d/%s" % (k, name)) in self.sh
            self.check_shared_file(os.path.join(self.sdir(k), name), name, own)

    def op_sh_list(self):
        for k in range(self.c["nshared"]):
            pref = "w%02d-" % self.wid
            got = set(x for x in os.listdir(self.sdir(k)) if x.startswith(pref))
            want = set(r.split("/")[1] for r in self.sh if r.startswith("s%d/" % k))
            if got != want:
                raise Mismatch("shared dir s%d: own names missing %s, unknown own-prefix names %s" % (k, sorted(want - got)[:5], sorted(got - want)[:5]))

    # ---- verification ----
    def verify_all(self):
        for d in sorted(self.dirs):
            self.timed("verify", self.check_dir, d)
        for rel in sorted(self.files):
            self.timed("verify", self.readback, rel)
        if self.c["nshared"]:
            self.timed("sh_list", self.op_sh_list)
            for rel in sorted(self.sh):
                k, name = rel.split("/")
                self.timed("sh_read", self.check_shared_file, os.path.join(self.sdir(int(k[1:])), name), name, True)
        self.verifies += 1

    def inject_corrupt(self):
        """control: flip one byte of a file on the volume behind the model's back"""
        cands = [r for r, v in self.files.items() if len(v) > 0]
        if not cands:
            return
        rel = sorted(cands)[0]
        fd = os.open(self.ap(rel), os.O_RDWR)
        try:
            b = os.pread(fd, 1, 0)
            os.pwrite(fd, bytes([b[0] ^ 0x55]), 0)
            os.fsync(fd)
        finally:
            os.close(fd)

    def manifest(self, path):
        lines = []
        rt = os.path.join(self.c["kind"], "w%02d" % self.wid)
        lines.append("D\t%s" % rt)
        for d in sorted(self.dirs):
            if d:
                lines.append("D\t%s/%s" % (rt, d))
        for r in sorted(self.files):
            lines.append("F\t%s/%s\t%d\t%s" % (rt, r, len(self.files[r]), sha_hex(bytes(self.files[r]))))
        for r in sorted(self.sh):
            k, name = r.split("/")
            n = shared_size(name)
            lines.append("F\t%s/shared/%s/%s\t%d\t%s" % (self.c["kind"], k, name, n, sha_hex(shake(shared_key(name), n))))
        with open(path + ".tmp", "w") as f:
            f.write("\n".join(lines) + "\n")
        os.rename(path + ".tmp", path)

    # ---- main loop ----
    def table(self):
        t = [(12, self.op_create), (10, self.op_seqwrite), (12, self.op_randwrite), (6, self.op_truncate),
             (8, self.op_rename), (6, self.op_mmap), (7, self.op_delete), (4, self.op_mkdir), (3, self.op_rmdir),
             (2, self.op_dirrename), (3, self.op_readdir), (4, self.op_readback)]
        if self.c["nshared"]:
            t += [(12, self.op_sh_create), (6, self.op_sh_rename), (6, self.op_sh_delete), (8, self.op_sh_read), (2, self.op_sh_list)]
        return t

    def step(self):
        if os.getppid() != self.c["parent"]:
            os._exit(4)  # the parent is gone (killed): do not keep writing to the volume
        if self.total + self.shared_bytes() > self.c["cap"]:
            fn = self.rng.choice((self.op_delete, self.op_delete, self.op_truncate) + ((self.op_sh_delete,) if self.sh else ()))
        else:
            tab = self.table()
            x = self.rng.random() * sum(w for w, _ in tab)
            for w, fn in tab:
                x -= w
                if x < 0:
                    break
        self.timed(fn.__name__[3:], fn)

    def run(self):
        c = self.c
        try:
            self.timed("setup", os.mkdir, self.root)
            next_verify = self.t0 + c["verify_every"]
            while time.time() < c["stop_at"]:
                if time.time() >= next_verify:
                    self.verify_all()
                    next_verify += c["verify_every"]  # a fixed grid: the time a verification takes does not move the next one
                self.step()
            if c.get("corrupt") and self.wid == 0:
                self.timed("setup", self.inject_corrupt)
            self.verify_all()
        except Mismatch as e:
            self.mism.append(str(e))
        except Exception as e:  # noqa: BLE001 - every unexpected error is a failure, with its errno
            self.errs.append("%s: %r" % (type(e).__name__, e))
        ok = not self.mism and not self.errs
        if not self.errs:  # after a mismatch the model is still the truth: the remount check must see the disk differ from it
            self.manifest(os.path.join(c["state"], "man-%02d.tsv" % self.wid))
        res = {"id": self.wid, "ok": ok, "ops": self.ops, "ops_total": sum(self.ops.values()), "lat_max_by_op": {k: round(v, 4) for k, v in self.lat.items()},
               "lat_max_s": round(self.cur[2], 4), "hist": self.hist, "slow": self.slow, "mismatches": self.mism, "errors": self.errs,
               "verifies": self.verifies, "bytes_written": self.bytes_written, "bytes_verified": self.bytes_verified,
               "files": len(self.files), "dirs": len(self.dirs) - 1, "shared_files": len(self.sh), "seconds": round(time.time() - self.t0, 3)}
        with open(os.path.join(c["state"], "worker-%02d.json" % self.wid), "w") as f:
            json.dump(res, f)
        return 0 if ok else 1


# ---------------------------------------------------------------------------------------------
# parent
# ---------------------------------------------------------------------------------------------
def ps_rss(pid):
    """(status, rss_kb): status "ok", "gone" (ps answered and the process is not there) or "err" (ps timed out or
    printed something else: says nothing about the worker)"""
    try:
        o = subprocess.run(["ps", "-o", "rss=", "-p", str(pid)], stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=10).stdout.decode().strip()
        return ("ok", int(o)) if o else ("gone", 0)
    except (ValueError, subprocess.TimeoutExpired, OSError):
        return ("err", 0)


def stalled(now, v, prev_last, t_start, limit):
    """hang condition for one slot read v: no progress for limit seconds or more, judged on a sane `last` (a torn or
    zero read never counts) that is also the one the previous poll saw (two consecutive polls)"""
    last = v[1]
    return last >= t_start and now - last >= limit and last == prev_last


def kill_group(signum=None, frame=None):
    try:
        os.killpg(os.getpgrp(), signal.SIGKILL)
    except OSError:
        pass
    os._exit(137)


def cmd_run(a):
    kind = a.kind
    try:
        os.setpgrp()  # own process group: a timeout kill (SIGALRM of tmo) or SIGTERM takes the workers with it
    except OSError:
        pass
    signal.signal(signal.SIGTERM, kill_group)
    signal.signal(signal.SIGALRM, kill_group)
    os.makedirs(a.state, exist_ok=True)
    cfg = {"mp": a.mp, "kind": kind, "seed": a.seed, "verify_every": a.verify_every, "state": a.state,
           "cap": a.cap, "filecap": a.filecap, "shared_max": a.shared_max, "nshared": a.shared if kind == "par" else 0, "corrupt": a.inject_corrupt,
           "stall": None}
    if a.inject_stall:
        w, at, secs = a.inject_stall.split(":")
        cfg["stall"] = (int(w), float(at), float(secs))
    cfg["parent"] = os.getpid()
    n = a.workers
    root = os.path.join(a.mp, kind)
    os.mkdir(root)
    man = ["D\t%s" % kind]
    for k in range(cfg["nshared"]):
        os.makedirs(os.path.join(root, "shared", "s%d" % k), exist_ok=True)
    if cfg["nshared"]:
        man.append("D\t%s/shared" % kind)
        man += ["D\t%s/shared/s%d" % (kind, k) for k in range(cfg["nshared"])]
    with open(os.path.join(a.state, "man-parent.tsv"), "w") as f:
        f.write("\n".join(man) + "\n")
    slots = Slots(n)
    t_start = time.time()
    cfg["stop_at"] = t_start + a.duration
    pids = {}
    for i in range(n):
        slots.put(i, [0.0, t_start, 0.0, 0.0, 0.0, 0.0, 0.0, 0.0])
        pid = os.fork()
        if pid == 0:
            try:
                code = Worker(cfg, i, slots).run()
            except BaseException:  # noqa: BLE001
                code = 3
            os._exit(code)
        pids[i] = pid
    failures = []
    exited = {}
    prev_last = {}
    ps_errors = 0
    samples = []
    rss_f = open(os.path.join(a.state, "rss.tsv"), "w") if a.rss_pid else None
    next_rss = 0.0
    hang = None
    hard = t_start + a.duration + a.grace
    while len(exited) < n:
        now = time.time()
        t = now - t_start
        if a.rss_pid and t >= next_rss:
            st_, r = ps_rss(a.rss_pid)
            if st_ == "err":
                st_, r = ps_rss(a.rss_pid)  # one retry: a ps that timed out says nothing about the worker
            if st_ == "err":
                ps_errors += 1
            elif st_ == "gone":
                failures.append({"kind": "worker_died", "detail": "the worker %d is gone at %.0f s" % (a.rss_pid, t)})
                a.rss_pid = 0
            else:
                samples.append((round(t, 2), r))
                rss_f.write("%.2f\t%d\n" % (t, r))
                rss_f.flush()
            next_rss += a.rss_every
        for i, pid in pids.items():
            if i in exited:
                continue
            try:
                p, st = os.waitpid(pid, os.WNOHANG)
            except ChildProcessError:
                p, st = pid, 0
            if p:
                exited[i] = st
        for i in range(n):
            if i in exited:
                continue
            v = slots.get(i)
            sd = stalled(now, v, prev_last.get(i), t_start, a.hang_limit)
            prev_last[i] = v[1]
            if sd and hang is None:
                hang = {"worker": i, "op": OPS[int(v[4])] if v[0] else "between operations", "seconds": round(now - v[1], 1), "at_s": round(t, 1)}
        if hang is not None:
            failures.append({"kind": "hang", "detail": "worker %d: no progress for %.1f s (limit %.0f s) in %s at %.0f s" % (
                hang["worker"], hang["seconds"], a.hang_limit, hang["op"], hang["at_s"])})
            break
        if now > hard:
            failures.append({"kind": "timeout", "detail": "workers still running %.0f s after the planned end" % a.grace})
            break
        time.sleep(0.5)
    killed = []
    for i, pid in pids.items():
        if i not in exited:
            try:
                os.kill(pid, signal.SIGKILL)
                killed.append(i)
            except OSError:
                pass
    for i in killed:
        end = time.time() + 10
        while time.time() < end:
            try:
                p, st = os.waitpid(pids[i], os.WNOHANG)
            except ChildProcessError:
                break
            if p:
                break
            time.sleep(0.2)
    if rss_f:
        rss_f.close()
    workers = []
    for i in range(n):
        p = os.path.join(a.state, "worker-%02d.json" % i)
        if os.path.isfile(p):
            with open(p) as f:
                workers.append(json.load(f))
        elif i not in killed:
            failures.append({"kind": "worker_crash", "detail": "worker %d exited (status %s) without a report" % (i, exited.get(i))})
    mism = [m for w in workers for m in w["mismatches"]]
    errs = [m for w in workers for m in w["errors"]]
    for m in mism[:10]:
        failures.append({"kind": "mismatch", "detail": m})
    for m in errs[:10]:
        failures.append({"kind": "error", "detail": m})
    lat_max = max([w["lat_max_s"] for w in workers] + [0.0])
    if lat_max >= a.hang_limit and hang is None:
        failures.append({"kind": "hang", "detail": "an operation took %.1f s (limit %.0f s)" % (lat_max, a.hang_limit)})
    hist = [sum(w["hist"][k] for w in workers) for k in range(25)]
    summary = {"kind": kind, "workers": n, "duration_planned_s": a.duration, "duration_actual_s": round(time.time() - t_start, 2),
               "verify_every_s": a.verify_every, "hang_limit_s": a.hang_limit, "baseline_s": a.baseline,
               "ops_total": sum(w["ops_total"] for w in workers), "mismatches": len(mism), "errors": len(errs),
               "verifies": sum(w["verifies"] for w in workers), "verifies_min_per_worker": (min(w["verifies"] for w in workers) if workers else 0),
               "bytes_written": sum(w["bytes_written"] for w in workers), "bytes_verified": sum(w["bytes_verified"] for w in workers),
               "lat_max_s": lat_max, "lat_p50_s": hist_percentile(hist, 0.5), "lat_p99_s": hist_percentile(hist, 0.99),
               "lat_p999_s": hist_percentile(hist, 0.999),
               "slowest": sorted([(s, o, w["id"]) for w in workers for s, o in w["slow"]], reverse=True)[:5],
               "ops_by_kind": {k: sum(w["ops"].get(k, 0) for w in workers) for k in OPS if any(w["ops"].get(k) for w in workers)},
               "worker_ok": [w["ok"] for w in workers], "hang": hang, "injected": {"corrupt": bool(a.inject_corrupt), "stall": a.inject_stall or ""}}
    if samples or a.baseline:
        summary["rss_samples"] = len(samples)
    if a.baseline and kind == "soak":
        r = rss_growth(samples, a.baseline)
        summary["rss"] = r
        if r["status"] != "pass":
            failures.append({"kind": "rss_growth" if r["status"] == "fail" else "rss_error", "detail": json.dumps(r)})
    summary["ps_errors"] = ps_errors
    summary["failures"] = failures
    summary["verdict"] = "fail" if failures else "pass"
    with open(os.path.join(a.state, "summary.json"), "w") as f:
        json.dump(summary, f, indent=1)
    print("%s: %s ops=%d verifies=%d mismatches=%d errors=%d lat_max=%.3fs p99<=%.3fs%s" % (
        kind, summary["verdict"].upper(), summary["ops_total"], summary["verifies"], len(mism), len(errs), lat_max, summary["lat_p99_s"],
        (" rss=" + json.dumps(summary["rss"])) if "rss" in summary else ""))
    for fl in failures:
        print("  FAIL %s: %s" % (fl["kind"], fl["detail"][:300]))
    return 1 if failures else 0


def sha_file(p):
    h = hashlib.sha256()
    with open(p, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def cmd_verify(a):
    """compare the tree under MP/KIND with the manifests in the state directory"""
    expf, expd = {}, set()
    for n in sorted(os.listdir(a.state)):
        if n.startswith("man-") and n.endswith(".tsv"):
            with open(os.path.join(a.state, n)) as f:
                for line in f.read().splitlines():
                    c = line.split("\t")
                    if c[0] == "D":
                        expd.add(c[1])
                    elif c[0] == "F":
                        expf[c[1]] = (int(c[2]), c[3])
    if not expf:
        print("verify: no manifest entries in %s" % a.state)
        return 2
    gotf, gotd = {}, set()
    bad = []
    top = os.path.join(a.mp, a.kind)
    gotd.add(a.kind)
    for dp, dns, fns in os.walk(top):
        rel = os.path.relpath(dp, a.mp)
        for d in dns:
            gotd.add(rel + "/" + d)
        for fn in fns:
            r = rel + "/" + fn
            try:
                gotf[r] = (os.path.getsize(os.path.join(dp, fn)), sha_file(os.path.join(dp, fn)))
            except OSError as e:
                bad.append("%s: %s" % (r, e))
    for r in sorted(expf):
        if r not in gotf:
            bad.append("missing file %s" % r)
        elif gotf[r] != expf[r]:
            bad.append("%s: size/sha256 differ (disk %s, expected %s)" % (r, gotf[r], expf[r]))
    bad += ["extra file %s" % r for r in sorted(set(gotf) - set(expf))]
    bad += ["missing directory %s" % d for d in sorted(expd - gotd)]
    bad += ["extra directory %s" % d for d in sorted(gotd - expd)]
    res = {"files": len(expf), "dirs": len(expd), "bytes": sum(v[0] for v in expf.values()), "mismatches": len(bad), "detail": bad[:20]}
    with open(a.out, "w") as f:
        json.dump(res, f, indent=1)
    print("verify: %d files, %d directories, %d bytes, %d mismatch(es)" % (res["files"], res["dirs"], res["bytes"], len(bad)))
    for b in bad[:10]:
        print("  BAD " + b)
    return 1 if bad else 0


def selftest_local():
    """short runs on a temporary directory (no NTFS): slot reads are never torn, the shared set is bounded, the
    verification grid holds, and a TERM of the driver takes its workers with it"""
    import shutil
    import tempfile
    # 1. a writer alternating two doubles with no byte in common, a reader that must only ever see one of them whole
    sl = Slots(1)
    A, B = 1.0, -2.5e300
    pid = os.fork()
    if pid == 0:
        end = time.time() + 1.5
        while time.time() < end:
            sl.put(0, [A] * 8)
            sl.put(0, [B] * 8)
        os._exit(0)
    bad = n = 0
    end = time.time() + 1.5
    while time.time() < end:
        v = sl.get(0)
        n += 1
        if any(x not in (A, B, 0.0) for x in v):
            bad += 1
    os.waitpid(pid, 0)
    assert n > 1000 and bad == 0, "torn slot reads: %d of %d" % (bad, n)
    tmp = tempfile.mkdtemp(prefix="mixself.")
    try:
        mp = os.path.join(tmp, "mp")
        os.mkdir(mp)
        me = os.path.abspath(__file__)
        r = subprocess.run([sys.executable, "-B", me, "run", "--kind", "par", "--mp", mp, "--state", os.path.join(tmp, "st"), "--duration", "6",
                            "--workers", "4", "--verify-every", "1", "--shared-max", "5", "--hang-limit", "20"], stdout=subprocess.PIPE, stderr=subprocess.STDOUT)
        assert r.returncode == 0, r.stdout.decode()
        sm = json.load(open(os.path.join(tmp, "st", "summary.json")))
        for i in range(4):
            w = json.load(open(os.path.join(tmp, "st", "worker-%02d.json" % i)))
            assert w["shared_files"] <= 5, "shared files not bounded: %d" % w["shared_files"]
            assert w["verifies"] >= 6, "verification grid: %d verifications in 6 s with a 1 s period" % w["verifies"]
        assert sm["verifies_min_per_worker"] >= 6
        left = sum(len(f) for _, _, f in os.walk(os.path.join(mp, "par", "shared")))
        assert left <= 4 * 5, "%d files left in the shared directories" % left
        # 2. a TERM of the driver leaves no worker behind
        mp2 = os.path.join(tmp, "mp2")
        os.mkdir(mp2)
        st2 = os.path.join(tmp, "st2")
        pr = subprocess.Popen([sys.executable, "-B", me, "run", "--kind", "par", "--mp", mp2, "--state", st2, "--duration", "60", "--workers", "3"],
                              stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL)
        time.sleep(2.5)
        alive = subprocess.run(["pgrep", "-f", st2], stdout=subprocess.PIPE).stdout.split()
        assert len(alive) >= 4, "driver and workers expected, found %s" % alive
        pr.terminate()
        pr.wait()
        time.sleep(0.5)
        alive = subprocess.run(["pgrep", "-f", st2], stdout=subprocess.PIPE).stdout.split()
        assert not alive, "workers left after TERM: %s" % alive
    finally:
        shutil.rmtree(tmp, ignore_errors=True)


def selftest():
    # the RSS rule on synthetic samples (the "injected RSS growth" control, and its boundaries)
    def ser(base, later, n=10, step=10.0, start=0.0):
        s = [(start + i * step, 1000) for i in range(0, 4)]
        s.append((60.0, base))
        s += [(60.0 + (i + 1) * step, later) for i in range(n)]
        return s
    r = rss_growth(ser(1000, 1000), 60)
    assert r["status"] == "pass" and r["growth_pct"] == 0 and r["baseline_kb"] == 1000, r
    r = rss_growth(ser(1000, 1099), 60)
    assert r["status"] == "pass" and r["growth_pct"] == 9.9, r
    r = rss_growth(ser(1000, 1100), 60)
    assert r["status"] == "fail" and r["growth_pct"] == 10.0, r  # exactly 10 percent is not "below 10 percent"
    r = rss_growth(ser(1000, 1500), 60)
    assert r["status"] == "fail", r
    # growth before the baseline does not count; a spike after it does, even when the end is back at baseline
    s = [(0.0, 100), (30.0, 900), (60.0, 1000)] + [(70.0 + i, 1000) for i in range(5)]
    assert rss_growth(s, 60)["status"] == "pass"
    s = [(60.0, 1000), (70.0, 1000), (80.0, 1200), (90.0, 1000), (100.0, 1000)]
    r = rss_growth(s, 60)
    assert r["status"] == "fail" and r["final_growth_pct"] == 0.0, r
    # the baseline is the first sample at or after the point, not an earlier one
    s = [(50.0, 5000), (61.0, 1000), (70.0, 1000), (80.0, 1050), (90.0, 1050)]
    r = rss_growth(s, 60)
    assert r["status"] == "pass" and r["baseline_kb"] == 1000 and r["baseline_t"] == 61.0, r
    # the rule cannot be applied: reported as error, never as pass
    assert rss_growth([(0.0, 1), (10.0, 1)], 60)["status"] == "error"
    assert rss_growth([(60.0, 1000), (70.0, 1000)], 60)["status"] == "error"
    assert rss_growth([(200.0, 1000), (210.0, 1000), (220.0, 1000), (230.0, 1000)], 60)["status"] == "error"
    assert rss_growth([(60.0, 0), (70.0, 1), (80.0, 1), (90.0, 1)], 60)["status"] == "error"
    # names, buckets, percentiles
    assert shared_key("w01-t000002-z100.g3.dat") == "w01-t000002-z100" and shared_size("w01-t000002-z100.g3.dat") == 100
    assert len(shake("k", 5000)) == 5000 and shake("k", 0) == b""
    assert bucket(0.0004) == 0 and bucket(0.001) == 1 and bucket(0.0035) == 2 and bucket(100000) == 24
    h = [0] * 25
    h[0] = 99
    h[10] = 1
    assert hist_percentile(h, 0.5) == 0.001 and hist_percentile(h, 0.999) == 1.024
    # the hang condition: a torn or zero read never counts, a stale `last` counts only when two polls agree
    assert not stalled(100.0, [0.0, 0.0] + [0.0] * 6, None, 10.0, 60.0)
    assert not stalled(100.0, [0.0, 1795189510.0] + [0.0] * 6, 1795189510.0, 10.0, 60.0)
    assert not stalled(100.0, [0.0, 30.0] + [0.0] * 6, None, 10.0, 60.0)  # first sighting only
    assert not stalled(100.0, [0.0, 30.0] + [0.0] * 6, 31.0, 10.0, 60.0)  # progress between the polls
    assert stalled(100.0, [0.0, 30.0] + [0.0] * 6, 30.0, 10.0, 60.0)
    assert stalled(90.0, [0.0, 30.0] + [0.0] * 6, 30.0, 10.0, 60.0)  # exactly the limit is a hang
    selftest_local()
    print("mixlib selftest: ok")
    return 0


def main():
    ap = argparse.ArgumentParser()
    sp = ap.add_subparsers(dest="cmd", required=True)
    r = sp.add_parser("run")
    r.add_argument("--kind", choices=("soak", "par"), required=True)
    r.add_argument("--mp", required=True)
    r.add_argument("--state", required=True)
    r.add_argument("--duration", type=float, required=True)
    r.add_argument("--workers", type=int, default=1)
    r.add_argument("--verify-every", type=float, default=60.0)
    r.add_argument("--baseline", type=float, default=0.0, help="soak: the baseline point of the RSS rule, seconds from the start")
    r.add_argument("--rss-pid", type=int, default=0)
    r.add_argument("--rss-every", type=float, default=10.0)
    r.add_argument("--hang-limit", type=float, default=60.0)
    r.add_argument("--grace", type=float, default=600.0)
    r.add_argument("--seed", type=int, default=1)
    r.add_argument("--cap", type=int, default=8 << 20, help="model bytes per worker")
    r.add_argument("--filecap", type=int, default=1 << 20)
    r.add_argument("--shared", type=int, default=4)
    r.add_argument("--shared-max", type=int, default=40, help="own files per worker in the shared directories")
    r.add_argument("--inject-corrupt", action="store_true")
    r.add_argument("--inject-stall", default="", help="WORKER:AT_SECONDS:STALL_SECONDS")
    v = sp.add_parser("verify")
    v.add_argument("--kind", choices=("soak", "par"), required=True)
    v.add_argument("--mp", required=True)
    v.add_argument("--state", required=True)
    v.add_argument("--out", required=True)
    sp.add_parser("selftest")
    a = ap.parse_args()
    if a.cmd == "selftest":
        return selftest()
    if a.cmd == "verify":
        return cmd_verify(a)
    return cmd_run(a)


if __name__ == "__main__":
    sys.exit(main())
