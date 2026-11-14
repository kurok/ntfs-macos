#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Crash-consistency sweeper (T-M3-14, criterion 3.9, S-CRASH-*).

Runs INSIDE Environment A (use run.sh from the host). For each workload it
  1. runs the workload once under the fault-layer ntfs-3g with plan "count" (no fault) to learn the
     number of device writes W and to check that every durability claim holds without a fault;
  2. runs it again for cut points spread over 1..W with plan "cut:N" (or "short:N:K"): device write
     N+1 is never issued, ntfs-3g is SIGKILLed there (NTFSMAC_FAULT_KILL=1), as if power failed;
  3. checks the image afterwards with the release build (no fault layer), always with norecover.

Checks per cut point (details and the treatment of each outcome: README.md):
  mount   ntfs-3g -o ro,norecover mounts the image (a refusal is a failure)
  walk    (advisory, reported separately) every directory is listed and every file is read to the
          end without an error; a failure here is a damaged file or a dangling directory entry
          inside a non-atomic window, not a failure of criterion 3.9
  claims  every claim "this file prefix was durable" whose fsync returned before the cut and whose
          path was not changed by an operation that started before the cut still has the right hash
  fsck    ntfsfix -n exits 0 (any other exit status is a failure, message recorded)
  split   a failing cut point is classified "mirror_split" (class a, an upstream non-atomic window)
          only if ALL hold: the refusal is exactly "$MFTMirr does not match $MFT (record k)." with k in
          0..3 and nothing else failed; the write trace shows write N is $MFT record k and write N+1
          is $MFTMirr record k; and after copying $MFT record k over the mirror record on a scratch
          copy every strict check passes (mount, ntfsfix -n, all claims, walk). Else it stays a failure.
  rw      (informational) what ntfs-3g -o rw,norecover does with the image: mounted rw, fell back
          to read-only, or refused
Controls per workload: plan "count" (no fault, all claims must hold) and cut:0 (nothing reaches
the device: the image must be byte-identical to the fresh image, proving the layer cuts).
"""
import argparse
import hashlib
import json
import os
import random
import re
import shutil
import struct
import subprocess
import sys
import threading
import time
from concurrent.futures import ThreadPoolExecutor

MIRR_RE = re.compile(r"^\$MFTMirr does not match \$MFT \(record ([0-3])\)\.$")
ROOT = os.environ.get("CRASHSWEEP_ROOT", "/tmp/crashsweep")
NTFS3G_FAULT = ROOT + "/faultio/bin/ntfs-3g"
NTFS3G_REL = ROOT + "/release/bin/ntfs-3g"
NTFSFIX_REL = ROOT + "/release/bin/ntfsfix"
MKNTFS_REL = ROOT + "/release/sbin/mkntfs"
IMAGE_MIB = 64
MOUNT_OPTS = "rw,norecover,big_writes"
WORKLOAD_TIMEOUT = 90
HERE = os.path.abspath(__file__)


# ---------------------------------------------------------------------------------------------
# workloads (run in a child process against the mounted fault-layer volume)
# ---------------------------------------------------------------------------------------------
class Wl:
    """Workload helper. Writes an event log: claims (durable data) and touches (changes)."""

    def __init__(self, root, state, events):
        self.root = root
        self.state = state
        self.f = open(events, "a", buffering=1)

    def cnt(self):
        """Device writes issued so far (published by the fault layer); 0 before the first."""
        try:
            with open(self.state, "rb") as f:
                s = f.read(20)
            return int(s) if s else 0
        except (OSError, ValueError):
            return 0

    def p(self, rel):
        return os.path.join(self.root, rel)

    def ev(self, **kw):
        self.f.write(json.dumps(kw) + "\n")

    def touch(self, rel, prefix=False):
        """Call BEFORE an operation that changes rel (or everything below it)."""
        self.ev(k="touch", p=rel, pre=prefix, c=self.cnt())

    def sync_path(self, rel):
        fd = os.open(self.p(rel) if rel else self.root, os.O_RDONLY)
        try:
            os.fsync(fd)
        finally:
            os.close(fd)

    def claim(self, rel, n, sha, exact=True):
        self.ev(k="claim", p=rel, n=n, sha=sha, ex=exact, w=self.cnt())

    def mkdir(self, rel):
        parent = os.path.dirname(rel)
        os.mkdir(self.p(rel))
        self.sync_path(parent)

    def write(self, rel, data):
        with open(self.p(rel), "wb") as f:
            f.write(data)

    def durable(self, rel, data):
        """fsync file and parent directory, then claim the content."""
        self.sync_path(rel)
        self.sync_path(os.path.dirname(rel))
        self.claim(rel, len(data), hashlib.sha256(data).hexdigest())


def blob(seed, n):
    return random.Random(seed).randbytes(n)


def wl_smallfiles(w):
    rng = random.Random(101)
    for d in range(8):
        w.mkdir("d%d" % d)
    for i in range(140):
        rel = "d%d/f%03d.dat" % (i % 8, i)
        data = blob(1000 + i, rng.choice([0, 1, 100, 600, 800, 3000, 4096, 6000]))
        w.write(rel, data)
        if i % 7 == 6:
            w.durable(rel, data)
    for d in range(8):
        w.sync_path("d%d" % d)


def wl_seqwrite(w):
    w.write("big.bin", b"")
    w.sync_path("")
    h = hashlib.sha256()
    chunk = 256 * 1024
    total = 0
    with open(w.p("big.bin"), "wb") as f:
        for i in range(80):
            data = blob(5000 + i, chunk)
            f.write(data)
            h.update(data)
            total += chunk
            if i % 16 == 15:
                f.flush()
                os.fsync(f.fileno())
                w.claim("big.bin", total, h.copy().hexdigest(), exact=False)
    tail = blob(5999, 777)
    w.write("tail.txt", tail)
    w.durable("tail.txt", tail)


def wl_renamechurn(w):
    rng = random.Random(303)
    model = {}
    for d in ("a", "b"):
        w.mkdir(d)

    def create(rel, seed):
        data = blob(seed, rng.choice([50, 700, 900, 2500, 5000]))
        w.touch(rel)
        w.write(rel, data)
        model[rel] = data

    def sync_all():
        w.sync_path("a")
        w.sync_path("b")
        for rel, data in sorted(model.items()):
            w.claim(rel, len(data), hashlib.sha256(data).hexdigest())

    for i in range(40):
        create("a/f%02d" % i, 7000 + i)
    sync_all()
    nxt = 100
    for r in range(90):
        op = rng.choice(["rename", "move", "unlink", "create"])
        names = sorted(model)
        if op == "create" or not names:
            d = rng.choice("ab")
            create("%s/n%03d" % (d, nxt), 7000 + nxt)
            nxt += 1
        else:
            src = rng.choice(names)
            if op == "unlink":
                w.touch(src)
                os.unlink(w.p(src))
                del model[src]
            else:
                d = os.path.dirname(src) if op == "rename" else rng.choice("ab")
                dst = "%s/r%03d" % (d, nxt)
                nxt += 1
                w.touch(src)
                w.touch(dst)
                os.rename(w.p(src), w.p(dst))
                model[dst] = model.pop(src)
        if r % 10 == 9:
            sync_all()
    # directory rename: everything below changes its path
    w.touch("b", prefix=True)
    w.touch("c", prefix=True)
    os.rename(w.p("b"), w.p("c"))
    for rel in [k for k in model if k.startswith("b/")]:
        model["c/" + rel[2:]] = model.pop(rel)
    w.sync_path("")
    w.sync_path("c")
    sync_all_dirs = ("a", "c")
    for d in sync_all_dirs:
        w.sync_path(d)
    for rel, data in sorted(model.items()):
        w.claim(rel, len(data), hashlib.sha256(data).hexdigest())


def wl_overwrite_truncate(w):
    rng = random.Random(404)
    model = {}
    w.mkdir("t")
    for i in range(12):
        rel = "t/f%02d" % i
        data = blob(9000 + i, 40 * 1024)
        w.write(rel, data)
        model[rel] = bytearray(data)
    w.sync_path("t")
    for rel, data in sorted(model.items()):
        w.durable(rel, bytes(data))
    for r in range(60):
        rel = rng.choice(sorted(model))
        data = model[rel]
        op = rng.choice(["overwrite", "shrink", "extend", "append"])
        w.touch(rel)
        with open(w.p(rel), "r+b") as f:
            if op == "overwrite":
                off = rng.randrange(0, max(1, len(data) - 1))
                n = min(rng.choice([10, 512, 5000]), len(data) - off)
                patch = blob(20000 + r, n)
                f.seek(off)
                f.write(patch)
                data[off:off + n] = patch
            elif op == "shrink":
                n = rng.randrange(0, len(data) + 1)
                f.truncate(n)
                del data[n:]
            elif op == "extend":
                n = len(data) + rng.choice([1, 4096, 10000])
                f.truncate(n)
                data.extend(b"\0" * (n - len(data)))
            else:
                patch = blob(30000 + r, rng.choice([100, 3000]))
                f.seek(0, os.SEEK_END)
                f.write(patch)
                data.extend(patch)
        if r % 5 == 4:
            w.sync_path("t")
            for k, v in sorted(model.items()):
                w.sync_path(k)
                w.claim(k, len(v), hashlib.sha256(bytes(v)).hexdigest())


def wl_dirtree(w):
    # nested tree, then one directory that grows past one index block, then shrinks
    n = 0
    for a in range(4):
        pa = "n%d" % a
        w.mkdir(pa)
        for b in range(4):
            pb = "%s/m%d" % (pa, b)
            w.mkdir(pb)
            for c in range(3):
                pc = "%s/l%d" % (pb, c)
                w.mkdir(pc)
                rel = pc + "/leaf.txt"
                data = blob(40000 + n, 200 + n)
                n += 1
                w.write(rel, data)
                if n % 6 == 0:
                    w.durable(rel, data)
    w.mkdir("wide")
    names = {}
    for i in range(260):
        rel = "wide/%s-%03d.dat" % ("longname-for-index-growth-" + "x" * 30, i)
        data = blob(50000 + i, 30 + (i % 7) * 100)
        w.write(rel, data)
        names[rel] = data
        if i % 20 == 19:
            w.sync_path("wide")
            for k in sorted(names)[-20:]:
                w.sync_path(k)
                w.claim(k, len(names[k]), hashlib.sha256(names[k]).hexdigest())
    for i, rel in enumerate(sorted(names)):
        if i % 2 == 0:
            w.touch(rel)
            os.unlink(w.p(rel))
            del names[rel]
    w.sync_path("wide")
    for k, v in sorted(names.items()):
        w.claim(k, len(v), hashlib.sha256(v).hexdigest())


WORKLOADS = {
    "smallfiles": wl_smallfiles,
    "seqwrite": wl_seqwrite,
    "renamechurn": wl_renamechurn,
    "overwrite_truncate": wl_overwrite_truncate,
    "dirtree": wl_dirtree,
}


def child_main(name, mnt, state, events):
    w = Wl(mnt, state, events)
    WORKLOADS[name](w)
    w.ev(k="end", c=w.cnt())
    return 0


# ---------------------------------------------------------------------------------------------
# sweeper
# ---------------------------------------------------------------------------------------------
def run(cmd, env=None, timeout=120):
    try:
        r = subprocess.run(cmd, env=env, capture_output=True, text=True, timeout=timeout,
                           errors="replace")
        return r.returncode, (r.stdout + r.stderr).strip()
    except subprocess.TimeoutExpired:
        return -9, "timeout after %ds: %s" % (timeout, " ".join(cmd))


def pids_using(path):
    pids = []
    for d in os.listdir("/proc"):
        if d.isdigit():
            try:
                with open("/proc/%s/cmdline" % d, "rb") as f:
                    if path.encode() in f.read():
                        pids.append(int(d))
            except OSError:
                pass
    return [p for p in pids if p != os.getpid()]


def unmount(mnt, img):
    rc, msg = run(["umount", mnt], timeout=60)
    if rc != 0:
        run(["umount", "-l", mnt], timeout=30)
        for p in pids_using(img):
            try:
                os.kill(p, 9)
            except OSError:
                pass
    deadline = time.time() + 30
    while pids_using(img) and time.time() < deadline:
        time.sleep(0.05)
    for p in pids_using(img):
        try:
            os.kill(p, 9)
        except OSError:
            pass
    return rc, msg


def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def load_events(path):
    ev = []
    try:
        with open(path) as f:
            for line in f:
                try:
                    ev.append(json.loads(line))
                except ValueError:
                    pass  # torn last line of a killed workload
    except OSError:
        pass
    return ev


def live_claims(events, limit_claim, limit_touch):
    live = {}
    for e in events:
        if e["k"] == "claim":
            if e["w"] <= limit_claim:
                live[e["p"]] = e
        elif e["k"] == "touch" and e["c"] < limit_touch:
            p = e["p"]
            for k in list(live):
                if k == p or (e["pre"] and k.startswith(p + "/")):
                    del live[k]
    return live


def check_claims(mnt, claims):
    bad = []
    for p, e in sorted(claims.items()):
        full = os.path.join(mnt, p)
        try:
            size = os.stat(full).st_size
            if size < e["n"] or (e["ex"] and size != e["n"]):
                bad.append("%s: size %d, claimed %s%d" % (p, size, "" if e["ex"] else ">=", e["n"]))
                continue
            h = hashlib.sha256()
            left = e["n"]
            with open(full, "rb") as f:
                while left:
                    b = f.read(min(left, 1 << 20))
                    if not b:
                        break
                    h.update(b)
                    left -= len(b)
            if h.hexdigest() != e["sha"]:
                bad.append("%s: hash mismatch in first %d bytes" % (p, e["n"]))
        except OSError as ex:
            bad.append("%s: %s" % (p, ex.strerror or ex))
    return bad


def walk_tree(mnt):
    errs = []
    files = 0
    nbytes = 0

    def onerr(ex):
        errs.append("listdir %s: %s" % (ex.filename, ex.strerror))

    for dp, dn, fn in os.walk(mnt, onerror=onerr):
        for n in fn:
            fp = os.path.join(dp, n)
            try:
                with open(fp, "rb") as f:
                    while True:
                        b = f.read(1 << 20)
                        if not b:
                            break
                        nbytes += len(b)
                files += 1
            except OSError as ex:
                errs.append("read %s: %s" % (os.path.relpath(fp, mnt), ex.strerror))
    return files, nbytes, errs


def first_line(msg, n=200):
    for line in msg.splitlines():
        if line.strip():
            return line.strip()[:n]
    return ""


class Sweep:
    def __init__(self, args):
        self.a = args
        self.tmp = args.tmp
        self.base = os.path.join(self.tmp, "base.img")
        self.base_sha = None
        self.lock = threading.Lock()
        self.done = 0
        self.total = 0

    def make_base(self):
        with open(self.base, "wb") as f:
            f.truncate(IMAGE_MIB << 20)
        rc, msg = run([MKNTFS_REL, "-F", "-Q", "-L", "crash", "-s", "512", "-c", "4096", self.base])
        if rc != 0:
            sys.exit("mkntfs failed: %s" % msg)
        self.base_sha = sha_file(self.base)

    def point(self, wl, tag, plan, n, kind):
        """Run one cut point. kind: count | cut | short | zero."""
        d = os.path.join(self.tmp, "%s-%s" % (wl, tag))
        shutil.rmtree(d, ignore_errors=True)
        mnt = os.path.join(d, "mnt")
        vmnt = os.path.join(d, "vmnt")
        os.makedirs(mnt)
        os.makedirs(vmnt)
        img = os.path.join(d, "disk.img")
        state = os.path.join(d, "state")
        events = os.path.join(d, "events.jsonl")
        run(["cp", "--sparse=always", self.base, img])
        row = {"workload": wl, "tag": tag, "plan": plan, "n": n, "kind": kind, "fail": []}
        trace = os.path.join(d, "trace")
        env = dict(os.environ, NTFSMAC_FAULT_PLAN=plan, NTFSMAC_FAULT_STATE=state,
                   NTFSMAC_FAULT_TRACE=trace)
        if kind not in ("count", "skip"):
            env["NTFSMAC_FAULT_KILL"] = "1"  # power loss stops the process (see faultio.h)
        t0 = time.time()
        rc, msg = run([NTFS3G_FAULT, img, mnt, "-o", MOUNT_OPTS], env=env, timeout=60)
        if rc != 0:
            row["fail"].append("rw mount under fault layer failed: " + first_line(msg))
        else:
            wrc, wmsg = run([sys.executable, "-I", HERE, "_workload", wl, mnt, state, events],
                            timeout=WORKLOAD_TIMEOUT)
            row["workload_rc"] = wrc
            row["workload_hang"] = wrc == -9
            row["workload_msg"] = first_line(wmsg, 120)
            urc, umsg = unmount(mnt, img)
            row["umount_rc"] = urc
        row["writes_seen"] = Wl(mnt, state, os.devnull).cnt()
        row["rw_seconds"] = round(time.time() - t0, 2)
        self.verify(row, d, img, vmnt, events, n, kind)
        row["pass"] = not row["fail"]
        row["mirror_split"] = False
        if row["fail"] and kind == "cut":
            self.classify_mirror_split(row, d, img, vmnt, events, n, trace)
        keep = bool(row["fail"]) and not row["mirror_split"] and self.a.keep_failing
        if keep:
            os.makedirs(os.path.join(self.a.out, "failing-images"), exist_ok=True)
            dst = os.path.join(self.a.out, "failing-images", "%s-%s.img.gz" % (wl, tag))
            run(["sh", "-c", "gzip -1 < '%s' > '%s'" % (img, dst)], timeout=120)
            shutil.copy(events, os.path.join(self.a.out, "failing-images", "%s-%s.events" % (wl, tag)))
        shutil.rmtree(d, ignore_errors=True)
        with self.lock:
            self.done += 1
            if self.done % 20 == 0 or self.done == self.total:
                print("[%d/%d] %s" % (self.done, self.total, time.strftime("%H:%M:%S")),
                      file=sys.stderr, flush=True)
        return row

    def verify(self, row, d, img, vmnt, events, n, kind):
        if kind == "zero":
            same = sha_file(img) == self.base_sha
            row["image_identical_to_fresh"] = same
            if not same:
                row["fail"].append("cut:0 changed the image: the fault layer let a write through")
        # 1. read-only mount of the image with the release build (never the fault layer)
        rc, msg = run([NTFS3G_REL, img, vmnt, "-o", "ro,norecover"], timeout=60)
        row["mount_rc"] = rc
        row["mount_msg"] = first_line(msg)
        mounted = rc == 0
        if mounted:
            try:
                files, nbytes, errs = walk_tree(vmnt)
                row["files"] = files
                row["bytes"] = nbytes
                row["walk_clean"] = not errs
                if errs:
                    row["walk_errors"] = errs[:5]
                    row["walk_error_count"] = len(errs)
                    if kind in ("count", "zero"):
                        row["fail"].append("walk (control, must be clean): " + errs[0])
                if kind == "count":
                    lim_c = lim_t = 1 << 62
                elif kind == "skip":
                    lim_c, lim_t = n - 1, 1 << 62  # later writes persist: any later touch counts
                elif kind == "short":
                    lim_c, lim_t = n - 1, n
                else:
                    lim_c = lim_t = n
                claims = live_claims(load_events(events), lim_c, lim_t)
                row["claims"] = len(claims)
                bad = check_claims(vmnt, claims)
                if bad:
                    row["claims_bad"] = bad[:5]
                    row["fail"].append("claims: %d of %d broken, first: %s" % (len(bad), len(claims), bad[0]))
            finally:
                unmount(vmnt, img)
        else:
            row["fail"].append("ro,norecover mount refused (rc %d): %s" % (rc, first_line(msg)))
        # 2. informational: rw mount with norecover (on this temporary copy, after the checks)
        row["rw_norecover"] = self.rw_probe(img, vmnt)
        # 3. ntfsfix -n (no action)
        frc, fmsg = run([NTFSFIX_REL, "-n", img], timeout=120)
        row["ntfsfix_rc"] = frc
        row["ntfsfix_msg"] = first_line(fmsg)
        if frc != 0:
            lines = [ln.strip() for ln in fmsg.splitlines() if ln.strip()]
            row["ntfsfix_tail"] = lines[-4:]
            row["fail"].append("ntfsfix -n exit %d: %s" % (frc, " | ".join(lines[-2:])[:200]))

    def geometry(self):
        """(cluster size, record size, $MFT byte offset, $MFTMirr byte offset) from the fresh image."""
        with open(self.base, "rb") as f:
            d = f.read(512)
        bps, spc = struct.unpack_from("<HB", d, 11)
        mft, mir = struct.unpack_from("<QQ", d, 0x30)
        cpr = struct.unpack_from("<b", d, 0x40)[0]
        cs = bps * spc
        rs = (1 << -cpr) if cpr < 0 else cpr * cs
        return cs, rs, mft * cs, mir * cs

    @staticmethod
    def read_trace(path):
        t = {}
        try:
            with open(path) as f:
                for line in f:
                    p = line.split()
                    if len(p) == 4:
                        t[int(p[0])] = (int(p[1]), int(p[2]), p[3])
        except (OSError, ValueError):
            pass
        return t

    def classify_mirror_split(self, row, d, img, vmnt, events, n, trace):
        reasons = row["fail"]
        # (a) exactly the mirror refusal, nothing else failed
        m = MIRR_RE.match(row.get("mount_msg", ""))
        if not m or row.get("mount_rc") != 13 or len(reasons) != 2:
            return
        if not (reasons[0].startswith("ro,norecover mount refused")
                and reasons[1].startswith("ntfsfix -n exit")):
            return
        k = int(m.group(1))
        msg = "$MFTMirr does not match $MFT (record %d)." % k
        if msg not in " ".join(row.get("ntfsfix_tail", [])):
            return
        # (b) write N is $MFT record k, write N+1 (never issued) is $MFTMirr record k
        cs, rs, mft_off, mir_off = self.geometry()
        tr = self.read_trace(trace)
        a, b = tr.get(n), tr.get(n + 1)
        if not a or not b:
            return
        if (a[0], a[1]) != (mft_off + k * rs, rs) or (b[0], b[1]) != (mir_off + k * rs, rs):
            return
        row["mirror_split_k"] = k
        row["mirror_split_trace"] = {"N": list(a), "N+1": list(b)}
        # (c) on a scratch copy: $MFT record k over the mirror record, strict checks must pass
        scratch = os.path.join(d, "scratch.img")
        run(["cp", "--sparse=always", img, scratch])
        with open(scratch, "r+b") as f:
            f.seek(mft_off + k * rs)
            rec = f.read(rs)
            f.seek(mir_off + k * rs)
            f.write(rec)
        row2 = {"fail": []}
        self.verify(row2, d, scratch, vmnt, events, n, "cut")
        row["mirror_split_repaired_checks"] = {"fail": row2["fail"], "walk_clean": row2.get("walk_clean")}
        if not row2["fail"] and row2.get("walk_clean") is True:
            row["mirror_split"] = True

    def rw_probe(self, img, vmnt):
        rc, msg = run([NTFS3G_REL, img, vmnt, "-o", "rw,norecover"], timeout=60)
        if rc != 0:
            return "refused: " + first_line(msg, 120)
        mode = "mounted"
        with open("/proc/mounts") as f:
            for line in f:
                parts = line.split()
                if len(parts) > 3 and parts[1] == vmnt:
                    mode = "mounted-" + ("ro" if "ro" in parts[3].split(",") else "rw")
        unmount(vmnt, img)
        return mode

    def count_run(self, wl):
        row = self.point(wl, "count", "count", 0, "count")
        return row

    def run(self):
        a = self.a
        os.makedirs(a.out, exist_ok=True)
        self.make_base()
        names = a.workloads.split(",") if a.workloads else list(WORKLOADS)
        rows = []
        counts = {}
        # phase 1: count runs (also the no-fault control)
        with ThreadPoolExecutor(a.jobs) as ex:
            self.total = len(names)
            for wl, row in zip(names, ex.map(self.count_run, names)):
                counts[wl] = row["writes_seen"]
                rows.append(row)
        # phase 2: cut points + the cut:0 control
        tasks = []
        for wl in names:
            w_total = counts[wl]
            if w_total < 2:
                sys.exit("workload %s issued %d device writes: nothing to sweep" % (wl, w_total))
            k = min(a.cuts, w_total)
            pts = sorted({1 + (i * (w_total - 1)) // max(1, k - 1) for i in range(k)})
            if a.plan == "skip":
                tasks += [(wl, "k%d" % p, "skip:%d" % p, p, "skip") for p in pts]
            elif a.plan == "short":
                tasks += [(wl, "s%d" % p, "short:%d:%d" % (p, a.short_bytes), p, "short") for p in pts]
            else:
                tasks += [(wl, "c%d" % p, "cut:%d" % p, p, "cut") for p in pts]
            tasks.append((wl, "zero", "cut:0", 0, "zero"))
        self.total = len(tasks)
        self.done = 0
        with ThreadPoolExecutor(a.jobs) as ex:
            rows += list(ex.map(lambda t: self.point(*t), tasks))
        self.report(rows, counts)

    def report(self, rows, counts):
        a = self.a
        cols = ["workload", "tag", "plan", "kind", "writes_seen", "mount_rc", "files", "claims",
                "ntfsfix_rc", "rw_norecover", "walk_clean", "workload_rc", "pass", "mirror_split", "fail"]
        with open(os.path.join(a.out, "results.tsv"), "w") as f:
            f.write("\t".join(cols) + "\n")
            for r in rows:
                f.write("\t".join(" ; ".join(r[c]) if c == "fail" else str(r.get(c, "")) for c in cols) + "\n")
        with open(os.path.join(a.out, "results.jsonl"), "w") as f:
            for r in rows:
                f.write(json.dumps(r, sort_keys=True) + "\n")
        sweep = [r for r in rows if r["kind"] in ("cut", "short", "skip")]
        rwc = {}
        for r in sweep:
            key = (r.get("rw_norecover") or "n/a").split(":")[0]
            rwc[key] = rwc.get(key, 0) + 1
        per = {}
        for wl in counts:
            rs = [r for r in sweep if r["workload"] == wl]
            per[wl] = {"device_writes": counts[wl], "cut_points": len(rs),
                       "pass": sum(1 for r in rs if r["pass"]),
                       "fail": sum(1 for r in rs if not r["pass"]),
                       "mirror_split": sum(1 for r in rs if r.get("mirror_split")),
                       "walk_dirty": sum(1 for r in rs if r.get("walk_clean") is False),
                       "workload_hangs": sum(1 for r in rs if r.get("workload_hang"))}
        controls = [r for r in rows if r["kind"] in ("count", "zero")]
        summary = {
            "plan": a.plan, "short_bytes": a.short_bytes if a.plan == "short" else None,
            "cuts_per_workload_requested": a.cuts,
            "cut_points": len(sweep), "pass": sum(1 for r in sweep if r["pass"]),
            "fail": sum(1 for r in sweep if not r["pass"]),
            "mirror_split": sum(1 for r in sweep if r.get("mirror_split")),
            "unclassified_fail": sum(1 for r in sweep if not r["pass"] and not r.get("mirror_split")),
            "walk_dirty": sum(1 for r in sweep if r.get("walk_clean") is False),
            "strict_pass": sum(1 for r in sweep if r["pass"] and r.get("walk_clean", True)),
            "workload_hangs": sum(1 for r in sweep if r.get("workload_hang")),
            "rw_norecover_outcomes": rwc,
            "ntfsfix_nonzero": sum(1 for r in sweep if r.get("ntfsfix_rc") not in (0, None)),
            "mount_refused": sum(1 for r in sweep if r.get("mount_rc") not in (0, None)),
            "claims_checked": sum(r.get("claims", 0) for r in sweep),
            "claims_broken": sum(1 for r in sweep if r.get("claims_bad")),
            "controls": len(controls), "controls_pass": sum(1 for r in controls if r["pass"]),
            "per_workload": per, "mount_options": MOUNT_OPTS,
            "failures": [{"workload": r["workload"], "plan": r["plan"], "reasons": r["fail"]}
                         for r in rows if not r["pass"] and not r.get("mirror_split")],
        }
        with open(os.path.join(a.out, "summary.json"), "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
            f.write("\n")
        print(json.dumps({k: v for k, v in summary.items() if k != "failures"}, indent=2, sort_keys=True))
        for fl in summary["failures"][:20]:
            print("FAIL %s %s: %s" % (fl["workload"], fl["plan"], " ; ".join(fl["reasons"])), file=sys.stderr)
        # Gate (cut plan): controls all pass, no unclassified failure, no broken claim, no hang.
        # mirror_split and walk_dirty are reported, never failing; counts are not pinned.
        ok = (summary["controls_pass"] == summary["controls"] and summary["unclassified_fail"] == 0
              and summary["claims_broken"] == 0 and summary["workload_hangs"] == 0)
        summary["gate"] = "pass" if ok else "fail"
        with open(os.path.join(a.out, "summary.json"), "w") as f:
            json.dump(summary, f, indent=2, sort_keys=True)
            f.write("\n")
        print("gate: " + summary["gate"], file=sys.stderr)
        self.exit_code = 0 if ok else 1


def main():
    if len(sys.argv) > 1 and sys.argv[1] == "_workload":
        return child_main(*sys.argv[2:6])
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--mode", choices=["short", "full"], default="short",
                    help="short: 50 cut points per workload (CI); full: 110 per workload (550 in all)")
    ap.add_argument("--cuts", type=int, help="cut points per workload (overrides --mode)")
    ap.add_argument("--plan", choices=["cut", "short", "skip"], default="cut",
                    help="cut:N (default), torn write short:N:K, or skip:N (drop write N only); "
                         "short and skip are supplementary and not gated")
    ap.add_argument("--short-bytes", type=int, default=512, help="K for --plan short")
    ap.add_argument("--workloads", help="comma separated subset of: " + ",".join(WORKLOADS))
    ap.add_argument("--jobs", type=int, default=min(8, os.cpu_count() or 2))
    ap.add_argument("--out", default=ROOT + "/out")
    ap.add_argument("--tmp", default=ROOT + "/work")
    ap.add_argument("--keep-failing", action="store_true", help="gzip the image of every failing point into OUT/failing-images")
    args = ap.parse_args()
    if args.cuts is None:
        args.cuts = 110 if args.mode == "full" else 50
    for b in (NTFS3G_FAULT, NTFS3G_REL, NTFSFIX_REL, MKNTFS_REL):
        if not os.path.exists(b):
            sys.exit("missing %s: run build.sh first" % b)
    shutil.rmtree(args.tmp, ignore_errors=True)
    os.makedirs(args.tmp)
    sw = Sweep(args)
    sw.exit_code = 1
    sw.run()
    shutil.rmtree(args.tmp, ignore_errors=True)
    return sw.exit_code


if __name__ == "__main__":
    sys.exit(main())
