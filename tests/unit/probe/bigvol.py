#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Run time of the probe on a volume with very many directory entries (T-M3-02 acceptance:
1,000,000 entries inside the daemon's 20 s probe limit).

Runs INSIDE Environment A (the container; vm/linux/run.sh), nothing on the host. It
  1. makes a sparse image file with mkntfs and mounts it with the pinned NTFS-3G (internal FUSE),
  2. creates N entries (empty files and directories) through the mount: layout "dirs" is 1000
     directories of N/1000 - 1 files each, layout "flat" one directory of N - 1 files; half of the
     names are ASCII, half contain CJK characters (the NFC check then takes the table path),
  3. unmounts, hashes the image, runs the probe RUNS times (wall clock, peak memory of the children),
     hashes again,
  4. prints a JSON record and, with --out, writes it.
Exit: 0 and the run time inside --limit-s, 1 over the limit or an unexpected verdict, 2 setup failure.
"""
import argparse
import hashlib
import json
import os
import resource
import shutil
import subprocess
import sys
import time


def sha256(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(8 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def run(cmd, **kw):
    r = subprocess.run(cmd, capture_output=True, **kw)
    return r.returncode, (r.stdout + r.stderr).decode(errors="replace").strip()


def name_for(i):
    # CJK on odd indexes, ASCII on even ones
    return ("文件-%06d.txt" if i % 2 else "file-%06d.txt") % i


def populate(mnt, layout, n, log):
    t0 = time.monotonic()
    root = os.fsencode(mnt)
    made = 0
    if layout == "flat":
        d = os.path.join(root, b"flat")
        os.mkdir(d)
        made += 1
        for i in range(n - 1):
            fd = os.open(os.path.join(d, name_for(i).encode()), os.O_CREAT | os.O_WRONLY, 0o644)
            os.close(fd)
            made += 1
            if made % 50000 == 0:
                log("created %d entries in %.0f s" % (made, time.monotonic() - t0))
    else:
        ndirs = 1000
        per = n // ndirs - 1
        for dno in range(ndirs):
            d = os.path.join(root, ("d%04d" % dno).encode())
            os.mkdir(d)
            made += 1
            for i in range(per):
                fd = os.open(os.path.join(d, name_for(i).encode()), os.O_CREAT | os.O_WRONLY, 0o644)
                os.close(fd)
                made += 1
            if made % 50000 < per + 1:
                log("created %d entries in %.0f s" % (made, time.monotonic() - t0))
    return made, time.monotonic() - t0


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    ap.add_argument("--probe", required=True)
    ap.add_argument("--image", required=True, help="image file to create in the container")
    ap.add_argument("--layout", choices=["dirs", "flat"], default="dirs")
    ap.add_argument("--entries", type=int, default=1000000)
    ap.add_argument("--size-gib", type=int, default=0, help="image size, default from the entry count")
    ap.add_argument("--runs", type=int, default=3)
    ap.add_argument("--limit-s", type=float, default=20.0)
    ap.add_argument("--keep", action="store_true")
    ap.add_argument("--plant-nfd", action="store_true", help="one NFD name in the last directory: the walk must find it")
    ap.add_argument("--out")
    a = ap.parse_args()

    def log(msg):
        print(msg, file=sys.stderr, flush=True)

    size = (a.size_gib or max(1, (a.entries * 6 // 1000000) + 1)) << 30
    mnt = a.image + ".mnt"
    os.makedirs(mnt, exist_ok=True)
    with open(a.image, "wb") as f:
        f.truncate(size)
    rc, msg = run(["mkntfs", "-F", "-Q", "-T", "-L", "bigvol", "-c", "4096", "-s", "512", a.image])
    if rc != 0:
        log(msg)
        return 2
    rc, msg = run(["ntfs-3g", a.image, mnt, "-o", "rw,norecover,big_writes,locale=C.UTF-8"])
    if rc != 0:
        log(msg)
        return 2
    try:
        made, secs = populate(mnt, a.layout, a.entries, log)
        if a.plant_nfd:
            last = b"flat" if a.layout == "flat" else b"d%04d" % 999
            name = "cafe\u0301-planted.txt".encode()
            os.close(os.open(os.path.join(os.fsencode(mnt), last, name), os.O_CREAT | os.O_WRONLY, 0o644))
    finally:
        rc, msg = run(["umount", mnt])
        if rc != 0:
            log(msg)
    log("populated %d entries in %.0f s" % (made, secs))
    used = os.stat(a.image).st_blocks * 512
    h0 = sha256(a.image)
    runs = []
    verdict = None
    for r in range(a.runs):
        fd = os.open(a.image, os.O_RDONLY)
        cold = r % 2 == 0  # runs 1, 3, ...: the image's cached pages dropped first; run 2, ...: warm
        if cold:
            os.posix_fadvise(fd, 0, 0, os.POSIX_FADV_DONTNEED)
        t0 = time.monotonic()
        p = subprocess.run([a.probe, "--fd", str(fd), "--debug"], pass_fds=(fd,), capture_output=True)
        dt = time.monotonic() - t0
        os.close(fd)
        rss = resource.getrusage(resource.RUSAGE_CHILDREN).ru_maxrss
        if p.returncode != 0:
            log("probe exit %d: %s" % (p.returncode, p.stderr[:200]))
            return 1
        verdict = json.loads(p.stdout)
        scan = [ln for ln in p.stderr.decode().splitlines() if "name scan" in ln]
        runs.append({"seconds": round(dt, 3), "cold_cache": cold, "children_max_rss_kib": rss,
                     "scan": scan[-1] if scan else ""})
        log("run %d (%s cache): %.2f s, state %s" % (r + 1, "cold" if cold else "warm", dt, verdict["state"]))
    h1 = sha256(a.image)
    rec = {
        "layout": a.layout, "entries": made, "image_size_bytes": size, "image_allocated_bytes": used,
        "populate_seconds": round(secs, 1), "runs": runs,
        "max_seconds": max(r["seconds"] for r in runs), "limit_seconds": a.limit_s,
        "image_sha256_before": h0, "image_sha256_after": h1, "image_unchanged": h0 == h1,
        "state": verdict["state"], "reason_codes": verdict["reason_codes"], "nonnfc_count": verdict["nonnfc_count"],
        "uname": os.uname().machine, "cpus": os.cpu_count(),
    }
    print(json.dumps(rec, indent=2, sort_keys=True))
    if a.out:
        with open(a.out, "w") as f:
            json.dump(rec, f, indent=2, sort_keys=True)
            f.write("\n")
    if not a.keep:
        os.unlink(a.image)
    shutil.rmtree(mnt, ignore_errors=True)
    want = "names_not_nfc" if a.plant_nfd else "clean"
    ok = h0 == h1 and rec["max_seconds"] <= a.limit_s and verdict["state"] == want
    return 0 if ok else 1


if __name__ == "__main__":
    sys.exit(main())
