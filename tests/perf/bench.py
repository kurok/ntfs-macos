#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""bench.py - the one benchmark tool of the performance report (T-M5-07, P-*), used identically in
Environment A (NTFS-3G on Linux) and Environment B (this product in the macOS guest).

Python 3.6+, standard library only. Never run it on a physical disk: it writes into a directory it creates
(`perfbench`) below the mount point that the hook returns, or below --dir in the self-test.

Usage: bench.py --out FILE (--hook CMD | --dir DIR) [--label TEXT] [--reps N] [--seq-mib N] [--block-kib N]
                [--rnd-mib N] [--rnd-ops N] [--small-files N] [--small-kib N] [--dir-entries N,N] [--seed N]
                [--fault corrupt-read|short-read|drop-entry]

The hook is a command line, called as `CMD VERB`:
  mount     make sure the volume is mounted read-write, print the mount point (last line of stdout)
  remount   unmount, then mount again (the cold-cache step), print the mount point
  unmount   unmount
  pids      print the process ids of the worker (ntfs-3g), one per line
Cold cache: before every read phase the volume is unmounted and mounted again. That empties the file system level
caches (the kernel's cache of the volume, the worker's inode and cluster caches). It does not empty the cache of
the image FILE (the host side of the virtual disk, or the Linux page cache of the loop-less image file): neither
environment can do that without dropping caches system wide, which is not used.

Phases and units (every phase is repeated --reps times; median, min and max are reported):
  seq_write    write --seq-mib in blocks of --block-kib, fsync at the end            MiB/s
  seq_read     cold read of the same file, every block checked                       MiB/s
  rnd_write    --rnd-ops writes of 4 KiB at distinct random aligned offsets, fsync   ops/s
  rnd_read     cold read of the same offsets in another order, content checked       ops/s
  small_create --small-files files of --small-kib KiB (create, write, close), fsync of the directory   files/s
  small_stat   stat of every file (warm)                                             files/s
  small_read   cold read of every file, content checked                              files/s
  small_delete unlink of every file, fsync of the directory                          files/s
  dir_N_list   cold readdir of a directory with N empty files, names checked          entries/s
  dir_N_stat   cold readdir plus stat of every entry                                  entries/s
Resources per phase: CPU seconds (user+sys) and peak resident set of the worker (and, in the macOS guest, the
sum of all macFUSE FSKit module processes), sampled while the phase runs. CPU of a process that starts during
the phase counts from its first sample.
Every read is verified (a checksum or a name list), so a phase that returns wrong data is an error, not a number.
Exit: 0 every phase of every repetition finished, 1 otherwise (the output file is written either way), 2 usage.
"""
import argparse
import json
import os
import platform
import random
import shutil
import statistics
import subprocess
import sys
import threading
import time
import zlib

SCHEMA = "ntfsmac-perf/1"
MIB = 1 << 20
IS_LINUX = sys.platform.startswith("linux")


class PhaseError(Exception):
    pass


def parse_cpu(s):
    """ps time field [[DD-]HH:]MM:SS[.cc] -> seconds"""
    days = 0.0
    if "-" in s:
        d, s = s.split("-", 1)
        days = float(d)
    t = 0.0
    for part in s.split(":"):
        t = t * 60 + float(part)
    return days * 86400 + t


class Sampler(object):
    """Samples CPU and RSS while a phase runs: the worker pids, on macOS also the FSKit module processes and the
    diskimages-helper that serves the virtual disk, and the whole system (Linux: /proc/stat busy time of all CPUs of
    the container VM; macOS: the CPU time of every process, so a process that exits during the phase is missed)."""

    def __init__(self, interval):
        self.interval = interval
        self.pids = []
        self.stop_ev = threading.Event()
        self.th = None
        self.data = {}

    def _sample(self):
        out = {}
        if IS_LINUX:
            tck = float(os.sysconf("SC_CLK_TCK"))
            try:
                with open("/proc/stat") as f:
                    v = [int(x) for x in f.readline().split()[1:]]
                # user nice system idle iowait irq softirq steal
                busy = v[0] + v[1] + v[2] + v[5] + v[6] + (v[7] if len(v) > 7 else 0)
                out[("sys", 0)] = (busy / tck, 0)
            except (OSError, ValueError, IndexError):
                pass
            for p in self.pids:
                try:
                    with open("/proc/%d/stat" % p) as f:
                        rest = f.read().rsplit(")", 1)[1].split()
                    cpu = (int(rest[11]) + int(rest[12])) / tck
                    rss = 0
                    with open("/proc/%d/status" % p) as f:
                        for line in f:
                            if line.startswith("VmRSS:"):
                                rss = int(line.split()[1])
                    out[("worker", p)] = (cpu, rss)
                except (OSError, IndexError, ValueError):
                    pass
            return out
        try:
            txt = subprocess.run(["ps", "-axo", "pid=,rss=,time=,comm="], stdout=subprocess.PIPE,
                                 stderr=subprocess.DEVNULL, timeout=20).stdout.decode("utf-8", "replace")
        except (OSError, subprocess.SubprocessError):
            return out
        for line in txt.splitlines():
            f = line.split(None, 3)
            if len(f) < 4:
                continue
            try:
                pid, rss, cpu = int(f[0]), int(f[1]), parse_cpu(f[2])
            except ValueError:
                continue
            out[("sys", pid)] = (cpu, 0)
            if pid in self.pids:
                out[("worker", pid)] = (cpu, rss)
            elif "fsmodule.macfuse" in f[3]:
                out[("module", pid)] = (cpu, rss)
            elif "diskimages-helper" in f[3]:
                out[("helper", pid)] = (cpu, rss)
        return out

    def _merge(self, s):
        for k, (cpu, rss) in s.items():
            e = self.data.get(k)
            if e is None:
                self.data[k] = [cpu, cpu, rss]
            else:
                e[1] = cpu
                e[2] = max(e[2], rss)

    def _run(self):
        while not self.stop_ev.wait(self.interval):
            self._merge(self._sample())

    def start(self, pids):
        self.pids = list(pids)
        self.data = {}
        self.stop_ev.clear()
        self._merge(self._sample())
        self.th = threading.Thread(target=self._run)
        self.th.daemon = True
        self.th.start()

    def stop(self):
        self.stop_ev.set()
        self.th.join()
        self._merge(self._sample())
        res = {}
        for kind in ("worker", "module", "helper"):
            es = [v for k, v in self.data.items() if k[0] == kind]
            res[kind + "_cpu_s"] = round(sum(e[1] - e[0] for e in es), 3) if es else None
            res[kind + "_rss_kib"] = sum(e[2] for e in es) if es else None
        es = [v for k, v in self.data.items() if k[0] == "sys"]
        res["sys_cpu_s"] = round(sum(e[1] - e[0] for e in es), 3) if es else None
        return res


class Env(object):
    """The mount through the hook, or a plain directory (self-test)."""

    def __init__(self, hook, plain):
        self.hook = hook
        self.plain = plain

    def _call(self, verb):
        r = subprocess.run(self.hook + " " + verb, shell=True, stdout=subprocess.PIPE, universal_newlines=True)
        if r.returncode != 0:
            raise RuntimeError("hook %s failed with status %d" % (verb, r.returncode))
        return r.stdout

    def mount(self, verb="mount"):
        if self.plain:
            return self.plain
        lines = [x for x in self._call(verb).splitlines() if x.strip()]
        if not lines or not os.path.ismount(lines[-1].strip()):
            raise RuntimeError("hook %s printed no mount point, or it is not a mount" % verb)
        return lines[-1].strip()

    def remount(self):
        return self.mount("remount")

    def unmount(self):
        if not self.plain:
            self._call("unmount")

    def raw_targets(self, mib):
        """The baseline targets the hook prepares: [(name, path)], and [(name, reason)] for the ones it cannot make.
        The plain-directory mode has one target, a file in the directory."""
        if self.plain:
            return [("file", os.path.join(self.plain, "raw-file.bin"))], []
        ok, skipped = [], []
        for line in self._call("raw %d" % mib).splitlines():
            f = line.split(None, 2)
            if len(f) == 3 and f[0] == "TARGET":
                ok.append((f[1], f[2].strip()))
            elif len(f) >= 2 and f[0] == "SKIP":
                skipped.append((f[1], line.split(None, 2)[2].strip() if len(f) > 2 else ""))
        return ok, skipped

    def raw_done(self):
        if self.plain:
            try:
                os.unlink(os.path.join(self.plain, "raw-file.bin"))
            except OSError:
                pass
        else:
            self._call("rawdone")

    def pids(self):
        if self.plain:
            return []
        out = []
        for x in self._call("pids").split():
            if x.isdigit():
                out.append(int(x))
        return out


def block_pattern(seed, n):
    return random.Random(seed).getrandbits(8 * n).to_bytes(n, "little")


class Bench(object):
    def __init__(self, a):
        self.a = a
        self.env = Env(a.hook, a.dir)
        self.sampler = Sampler(0.05 if IS_LINUX else 0.25)
        self.blk = a.block_kib * 1024
        self.base_block = block_pattern(a.seed, self.blk)
        self.rnd_buf = block_pattern(a.seed + 1, 4096)
        self.small_buf = block_pattern(a.seed + 2, a.small_kib * 1024)
        self.root = None
        self.seq_ok = False
        self.raw_names = []
        self.raw_skipped = []
        self.rep_secs = []
        self.on_rep = None
        self.results = []
        self.errors = []

    # ---- helpers
    def path(self, *p):
        return os.path.join(self.root, *p)

    def mount_root(self, remount=False):
        mp = self.env.remount() if remount else self.env.mount()
        self.root = os.path.join(mp, "perfbench")
        os.makedirs(self.root, exist_ok=True)

    def block(self, i):
        return i.to_bytes(8, "little") + self.base_block[8:]

    def timed(self, rep, name, unit, count, fn, raw=False):
        """Run fn() once as phase `name` of repetition rep, with resource sampling. count is the amount of
        work in `unit` terms (MiB, ops, files, entries). fn may return the seconds its fsync took (fsync_s, part of
        the time). Errors become entries of the result, not exceptions."""
        rec = {"rep": rep, "phase": name, "unit": unit, "count": count}
        try:
            # never time a phase on the directory below a mount that is gone (it would measure the local disk)
            if not raw and not self.env.plain and not os.path.ismount(os.path.dirname(self.root)):
                raise RuntimeError("the volume is not mounted at %s" % os.path.dirname(self.root))
            self.sampler.start([] if raw else self.env.pids())
            t0 = time.perf_counter()
            fs = fn()
            secs = time.perf_counter() - t0
            rec.update(self.sampler.stop())
            rec["secs"] = round(secs, 6)
            rec["rate"] = round(count / secs, 3) if secs > 0 else None
            if fs is not None:
                rec["fsync_s"] = round(fs, 6)
        except Exception as e:  # noqa: BLE001 - recorded, the run goes on to the next phase or repetition
            try:
                self.sampler.stop()
            except Exception:  # noqa: BLE001
                pass
            rec["error"] = "%s: %s" % (type(e).__name__, e)
            self.errors.append("rep %d %s: %s" % (rep, name, rec["error"]))
        self.results.append(rec)
        sys.stderr.write("perf: rep %d %-22s %s\n" % (rep, name, ("ERROR " + rec["error"]) if "error" in rec else
                                                       "%.3f s (fsync %s s), %s %s, worker cpu %s s, sys cpu %s s" % (
                                                           rec["secs"], rec.get("fsync_s"), rec["rate"], unit,
                                                           rec.get("worker_cpu_s"), rec.get("sys_cpu_s"))))
        return "error" not in rec

    def fsync_timed(self, fd):
        t = time.perf_counter()
        os.fsync(fd)
        return time.perf_counter() - t

    def skipped(self, rep, name, unit, count, why):
        rec = {"rep": rep, "phase": name, "unit": unit, "count": count, "error": "skipped: " + why}
        self.results.append(rec)
        self.errors.append("rep %d %s: %s" % (rep, name, rec["error"]))

    # ---- phases
    def seq(self, rep):
        a = self.a
        nblk = a.seq_mib * MIB // self.blk
        f = self.path("seq.bin")
        expect = 0
        for i in range(nblk):
            expect = zlib.crc32(self.block(i), expect)

        def write():
            fd = os.open(f, os.O_WRONLY | os.O_CREAT | os.O_TRUNC, 0o644)
            try:
                for i in range(nblk):
                    b = self.block(i)
                    if os.write(fd, b) != len(b):
                        raise PhaseError("short write")
                return self.fsync_timed(fd)
            finally:
                os.close(fd)

        def read():
            crc = 0
            fd = os.open(f, os.O_RDONLY)
            try:
                total = 0
                while True:
                    b = os.read(fd, self.blk)
                    if not b:
                        break
                    if a.fault == "corrupt-read" and total == 0:
                        b = bytes([b[0] ^ 1]) + b[1:]
                    if a.fault == "short-read" and total >= self.blk and total + len(b) >= nblk * self.blk // 2:
                        break
                    crc = zlib.crc32(b, crc)
                    total += len(b)
            finally:
                os.close(fd)
            if total != nblk * self.blk:
                raise PhaseError("read %d bytes, wrote %d" % (total, nblk * self.blk))
            if crc != expect:
                raise PhaseError("checksum of the data read differs from the data written")

        ok = self.timed(rep, "seq_write", "MiB/s", nblk * self.blk / float(MIB), write)
        self.seq_ok = ok
        if not ok:
            self.skipped(rep, "seq_read", "MiB/s", nblk * self.blk / float(MIB), "seq_write failed")
            return False
        self.mount_root(True)
        return self.timed(rep, "seq_read", "MiB/s", nblk * self.blk / float(MIB), read)

    def rnd(self, rep):
        a = self.a
        f = self.path("seq.bin")
        nblk = a.seq_mib * MIB // 4096 if a.rnd_mib is None else a.rnd_mib * MIB // 4096
        ops = min(a.rnd_ops, nblk)
        if not self.seq_ok:
            self.skipped(rep, "rnd_write", "ops/s", ops, "seq_write failed, the file of the random phases is missing")
            self.skipped(rep, "rnd_read", "ops/s", ops, "seq_write failed, the file of the random phases is missing")
            return False
        rng = random.Random(a.seed + 10 + rep)
        offs = [o * 4096 for o in rng.sample(range(nblk), ops)]
        rorder = list(range(ops))
        rng.shuffle(rorder)

        def buf(i):
            return i.to_bytes(8, "little") + self.rnd_buf[8:]

        def write():
            fd = os.open(f, os.O_RDWR)
            try:
                for i, o in enumerate(offs):
                    if os.pwrite(fd, buf(i), o) != 4096:
                        raise PhaseError("short write")
                return self.fsync_timed(fd)
            finally:
                os.close(fd)

        def read():
            fd = os.open(f, os.O_RDONLY)
            try:
                for i in rorder:
                    b = os.pread(fd, 4096, offs[i])
                    if a.fault == "corrupt-read" and i == rorder[0]:
                        b = bytes([b[0] ^ 1]) + b[1:]
                    if b != buf(i):
                        raise PhaseError("content at offset %d differs from what was written" % offs[i])
            finally:
                os.close(fd)

        # the file of the sequential phase is the file of the random phases (it is rewritten in place)
        if not os.path.exists(f):
            with open(f, "wb") as fh:
                fh.truncate(nblk * 4096)
        elif os.path.getsize(f) < nblk * 4096:
            os.truncate(f, nblk * 4096)
        ok = self.timed(rep, "rnd_write", "ops/s", ops, write)
        if not ok:
            return False
        self.mount_root(True)
        f = self.path("seq.bin")
        return self.timed(rep, "rnd_read", "ops/s", ops, read)

    def small(self, rep):
        a = self.a
        n = a.small_files
        d = self.path("small")
        os.makedirs(d, exist_ok=True)

        def content(i):
            return i.to_bytes(8, "little") + self.small_buf[8:]

        def dirsync():
            fd = os.open(d, os.O_RDONLY)
            try:
                return self.fsync_timed(fd)
            finally:
                os.close(fd)

        def create():
            for i in range(n):
                fd = os.open(os.path.join(d, "f%06d" % i), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
                try:
                    b = content(i)
                    if os.write(fd, b) != len(b):
                        raise PhaseError("short write")
                finally:
                    os.close(fd)
            return dirsync()

        def stat():
            for i in range(n):
                st = os.stat(os.path.join(d, "f%06d" % i))
                if st.st_size != len(self.small_buf):
                    raise PhaseError("size of f%06d is %d" % (i, st.st_size))

        def read():
            for i in range(n):
                with open(os.path.join(d, "f%06d" % i), "rb") as fh:
                    b = fh.read()
                if a.fault == "corrupt-read" and i == 0:
                    b = bytes([b[0] ^ 1]) + b[1:]
                if b != content(i):
                    raise PhaseError("content of f%06d differs" % i)

        def delete():
            for i in range(n):
                os.unlink(os.path.join(d, "f%06d" % i))
            fs = dirsync()
            if os.listdir(d):
                raise PhaseError("files left after the delete")
            return fs

        if not self.timed(rep, "small_create", "files/s", n, create):
            self.skipped(rep, "small_stat", "files/s", n, "small_create failed")
            self.skipped(rep, "small_read", "files/s", n, "small_create failed")
            self.skipped(rep, "small_delete", "files/s", n, "small_create failed")
            return False
        if not self.timed(rep, "small_stat", "files/s", n, stat):
            return False
        self.mount_root(True)
        d = self.path("small")
        ok = self.timed(rep, "small_read", "files/s", n, read)
        if not ok:
            return False
        ok = self.timed(rep, "small_delete", "files/s", n, delete)
        os.rmdir(d)
        return ok

    def dirs(self, rep, n):
        a = self.a
        d = self.path("dir%d" % n)
        os.makedirs(d, exist_ok=True)
        names = ["e%07d" % i for i in range(n)]
        for nm in names:
            os.close(os.open(os.path.join(d, nm), os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644))
        fd = os.open(d, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)
        want = sorted(names)

        def lst():
            got = os.listdir(d)
            if a.fault == "drop-entry":
                got = got[1:]
            if sorted(got) != want:
                raise PhaseError("listing has %d names, expected %d (or the names differ)" % (len(got), n))

        def lst_stat():
            got = os.listdir(d)
            if a.fault == "drop-entry":
                got = got[1:]
            for nm in got:
                os.stat(os.path.join(d, nm))
            if len(got) != n:
                raise PhaseError("listing has %d names, expected %d" % (len(got), n))

        self.mount_root(True)
        d = self.path("dir%d" % n)
        ok = self.timed(rep, "dir_%d_list" % n, "entries/s", n, lst)
        self.mount_root(True)
        d = self.path("dir%d" % n)
        ok2 = self.timed(rep, "dir_%d_stat" % n, "entries/s", n, lst_stat)
        for nm in names:
            os.unlink(os.path.join(d, nm))
        os.rmdir(d)
        return ok and ok2

    def clean_root(self):
        if os.path.isdir(self.root):
            shutil.rmtree(self.root, ignore_errors=True)

    # ---- the baseline: the same block sizes on what the file system sits on (a scratch target the hook makes; the
    # image of the file system is never written). Read phases are best-effort cold: posix_fadvise(DONTNEED) on Linux,
    # F_NOCACHE on macOS; the device or image file below keeps its own cache.
    def raw(self, rep, name, path):
        a = self.a
        nblk = a.seq_mib * MIB // self.blk
        expect = 0
        for i in range(nblk):
            expect = zlib.crc32(self.block(i), expect)
        nr = a.seq_mib * MIB // 4096 if a.rnd_mib is None else a.rnd_mib * MIB // 4096
        ops = min(a.rnd_ops, nr)
        rng = random.Random(a.seed + 10 + rep)
        offs = [o * 4096 for o in rng.sample(range(nr), ops)]
        rorder = list(range(ops))
        rng.shuffle(rorder)

        def buf(i):
            return i.to_bytes(8, "little") + self.rnd_buf[8:]

        def cold(fd):
            try:
                if IS_LINUX:
                    os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
                else:
                    import fcntl
                    fcntl.fcntl(fd, 48, 1)  # F_NOCACHE
            except (OSError, AttributeError, ImportError):
                pass

        def seq_write():
            fd = os.open(path, os.O_WRONLY | os.O_CREAT)
            try:
                for i in range(nblk):
                    b = self.block(i)
                    if os.write(fd, b) != len(b):
                        raise PhaseError("short write")
                return self.fsync_timed(fd)
            finally:
                os.close(fd)

        def seq_read():
            fd = os.open(path, os.O_RDONLY)
            try:
                cold(fd)
                crc = 0
                for i in range(nblk):
                    b = os.read(fd, self.blk)
                    if a.fault == "corrupt-read" and i == 0:
                        b = bytes([b[0] ^ 1]) + b[1:]
                    crc = zlib.crc32(b, crc)
                    if len(b) != self.blk:
                        raise PhaseError("short read")
            finally:
                os.close(fd)
            if crc != expect:
                raise PhaseError("checksum of the data read differs from the data written")

        def rnd_write():
            fd = os.open(path, os.O_RDWR)
            try:
                for i, o in enumerate(offs):
                    if os.pwrite(fd, buf(i), o) != 4096:
                        raise PhaseError("short write")
                return self.fsync_timed(fd)
            finally:
                os.close(fd)

        def rnd_read():
            fd = os.open(path, os.O_RDONLY)
            try:
                cold(fd)
                for i in rorder:
                    b = os.pread(fd, 4096, offs[i])
                    if a.fault == "corrupt-read" and i == rorder[0]:
                        b = bytes([b[0] ^ 1]) + b[1:]
                    if b != buf(i):
                        raise PhaseError("content at offset %d differs from what was written" % offs[i])
            finally:
                os.close(fd)

        mb = nblk * self.blk / float(MIB)
        pre = "raw_%s_" % name
        if self.timed(rep, pre + "seq_write", "MiB/s", mb, seq_write, raw=True):
            self.timed(rep, pre + "seq_read", "MiB/s", mb, seq_read, raw=True)
        else:
            self.skipped(rep, pre + "seq_read", "MiB/s", mb, "seq_write failed")
        if self.timed(rep, pre + "rnd_write", "ops/s", ops, rnd_write, raw=True):
            self.timed(rep, pre + "rnd_read", "ops/s", ops, rnd_read, raw=True)
        else:
            self.skipped(rep, pre + "rnd_read", "ops/s", ops, "rnd_write failed")

    def run(self):
        a = self.a
        t_start = time.time()
        mib = max(a.seq_mib, a.rnd_mib or 0) + 16
        try:
            targets, self.raw_skipped = self.env.raw_targets(mib)
        except Exception as e:  # noqa: BLE001
            targets = []
            self.errors.append("baseline targets: %s" % e)
        self.raw_names = [n for n, _p in targets]
        self.mount_root()
        for rep in range(1, a.reps + 1):
            t_rep = time.time()
            for name, path in targets:
                try:
                    self.raw(rep, name, path)
                except Exception as e:  # noqa: BLE001
                    self.errors.append("rep %d baseline %s: %s: %s" % (rep, name, type(e).__name__, e))
            self.clean_root()
            os.makedirs(self.root, exist_ok=True)
            steps = [("seq", self.seq), ("rnd", self.rnd), ("small", self.small)]
            for n in a.dir_entries:
                steps.append(("dir%d" % n, lambda r, n=n: self.dirs(r, n)))
            self.seq_ok = False
            for nm, fn in steps:
                try:
                    fn(rep)
                except Exception as e:  # noqa: BLE001
                    self.errors.append("rep %d %s: %s: %s" % (rep, nm, type(e).__name__, e))
                    sys.stderr.write("perf: rep %d %s: %s\n" % (rep, nm, e))
                try:
                    if nm == "rnd":
                        os.unlink(self.path("seq.bin"))
                except OSError:
                    pass
            self.clean_root()
            self.rep_secs.append(time.time() - t_rep)
            done = time.time() - t_start
            if rep == 1:
                sys.stderr.write("perf: repetition 1 took %.0f s; estimate for %d repetitions: %.0f s\n" % (
                    self.rep_secs[0], a.reps, self.rep_secs[0] * a.reps))
            if self.on_rep:
                self.on_rep(False)
            if a.max_wall and rep < a.reps and done / rep * (rep + 1) > a.max_wall:
                self.errors.append("stopped after %d of %d repetitions: the next would pass the wall limit of %d s" % (
                    rep, a.reps, a.max_wall))
                sys.stderr.write("perf: %s\n" % self.errors[-1])
                break
        try:
            self.env.unmount()
        except Exception as e:  # noqa: BLE001
            self.errors.append("unmount: %s" % e)
        try:
            self.env.raw_done()
        except Exception as e:  # noqa: BLE001
            self.errors.append("rawdone: %s" % e)


def summarize(results, reps, expect_phases):
    phases = {}
    for name in expect_phases:
        rs = [r for r in results if r["phase"] == name and "error" not in r]
        errs = [r for r in results if r["phase"] == name and "error" in r]
        p = {"unit": None, "n": len(rs), "reps": reps, "errors": [r["error"] for r in errs]}
        if rs:
            p["unit"] = rs[0]["unit"]
            rates = [r["rate"] for r in rs]
            secs = [r["secs"] for r in rs]
            p["count"] = rs[0]["count"]
            p["median"] = round(statistics.median(rates), 3)
            p["min"] = min(rates)
            p["max"] = max(rates)
            p["secs_median"] = round(statistics.median(secs), 4)
            fv = [r["fsync_s"] for r in rs if r.get("fsync_s") is not None]
            p["fsync_s"] = round(statistics.median(fv), 4) if fv else None
            for k in ("worker_cpu_s", "module_cpu_s", "helper_cpu_s", "sys_cpu_s"):
                v = [r[k] for r in rs if r.get(k) is not None]
                p[k] = round(statistics.median(v), 3) if v else None
            for k in ("worker_rss_kib", "module_rss_kib", "helper_rss_kib"):
                v = [r[k] for r in rs if r.get(k) is not None]
                p[k] = max(v) if v else None
        phases[name] = p
    return phases


def parse_args(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--out", required=True)
    ap.add_argument("--hook")
    ap.add_argument("--dir")
    ap.add_argument("--label", default="")
    ap.add_argument("--reps", type=int, default=5)
    ap.add_argument("--seq-mib", type=int, default=256)
    ap.add_argument("--block-kib", type=int, default=1024)
    ap.add_argument("--rnd-mib", type=int, default=None, help="size of the file of the random phases (default: --seq-mib)")
    ap.add_argument("--rnd-ops", type=int, default=16384)
    ap.add_argument("--small-files", type=int, default=2000)
    ap.add_argument("--small-kib", type=int, default=4)
    ap.add_argument("--dir-entries", default="10000")
    ap.add_argument("--seed", type=int, default=20260)
    ap.add_argument("--meta", action="append", default=[], help="KEY=VALUE recorded in the result (versions, image hash)")
    ap.add_argument("--max-wall", type=int, default=0, help="stop after a repetition when the next would pass this many seconds")
    ap.add_argument("--emit-json", action="store_true", help="print `PERF-JSON {...}` on stdout after every repetition")
    ap.add_argument("--fault", choices=("corrupt-read", "short-read", "drop-entry"))
    a = ap.parse_args(argv)
    if bool(a.hook) == bool(a.dir):
        ap.error("exactly one of --hook and --dir")
    try:
        a.dir_entries = [int(x) for x in a.dir_entries.split(",") if x]
    except ValueError:
        ap.error("--dir-entries is a comma separated list of numbers")
    for k in ("reps", "seq_mib", "block_kib", "rnd_ops", "small_files", "small_kib"):
        if getattr(a, k) < 1:
            ap.error("--%s must be positive" % k.replace("_", "-"))
    if a.seq_mib * MIB % (a.block_kib * 1024):
        ap.error("--seq-mib must be a multiple of the block size")
    return a


def build_doc(a, b, started, t0, complete):
    expect = ["seq_write", "seq_read", "rnd_write", "rnd_read", "small_create", "small_stat", "small_read",
              "small_delete"]
    for n in a.dir_entries:
        expect += ["dir_%d_list" % n, "dir_%d_stat" % n]
    for t in b.raw_names:
        expect += ["raw_%s_%s" % (t, k) for k in ("seq_write", "seq_read", "rnd_write", "rnd_read")]
    return {"schema": SCHEMA, "label": a.label, "started": started, "wall_s": round(time.time() - t0, 1),
            "complete": bool(complete),
            "platform": {"system": platform.system(), "release": platform.release(), "machine": platform.machine(),
                         "python": platform.python_version(), "cpus": os.cpu_count()},
            "params": {"reps": a.reps, "seq_mib": a.seq_mib, "block_kib": a.block_kib,
                       "rnd_mib": a.rnd_mib if a.rnd_mib is not None else a.seq_mib, "rnd_ops": a.rnd_ops,
                       "small_files": a.small_files, "small_kib": a.small_kib, "dir_entries": a.dir_entries,
                       "seed": a.seed, "fault": a.fault},
            "raw_targets": b.raw_names, "raw_skipped": [list(x) for x in b.raw_skipped],
            "meta": dict(m.split("=", 1) for m in a.meta if "=" in m),
            "phases": summarize(b.results, a.reps, expect), "samples": b.results, "errors": b.errors}


def main(argv):
    a = parse_args(argv)
    b = Bench(a)
    started = time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())
    t0 = time.time()

    def write(complete):
        doc = build_doc(a, b, started, t0, complete)
        tmp = a.out + ".tmp"
        with open(tmp, "w") as f:
            json.dump(doc, f, indent=1)
            f.write("\n")
        os.replace(tmp, a.out)
        if a.emit_json:
            sys.stdout.write("PERF-JSON " + json.dumps(doc, separators=(",", ":")) + "\n")
            sys.stdout.flush()
        return doc

    b.on_rep = write
    b.run()
    doc = write(not b.errors and len(set(r["rep"] for r in b.results)) == a.reps)
    ok = not b.errors and all(p["n"] == a.reps for p in doc["phases"].values())
    sys.stderr.write("perf: %s, %d error(s)\n" % ("done" if ok else "INCOMPLETE", len(b.errors)))
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
