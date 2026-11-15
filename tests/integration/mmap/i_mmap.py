# SPDX-License-Identifier: GPL-2.0-or-later
"""I-MMAP-01..07 (T-M3-11, ARCHITECTURE.md N7 and D1, R-05): mapped I/O on the FSKit backend.

I-MMAP-01  mapped read of a file written with write(2), and write(2) after a mapped read seen through the live mapping
I-MMAP-02  mapped write then msync(MS_SYNC) / msync(MS_ASYNC), read(2) back, hashes after remount
I-MMAP-03  mapped write with no msync, munmap, close, idle, clean unmount: is the data on disk after remount
I-MMAP-04  unmount (umount, then forced unmount if it refuses) while pages are dirty and the mapping is live
I-MMAP-05  a mapping that extends a file (ftruncate up, write through the map), access past EOF, shrink under a map
I-MMAP-06  which FUSE opcodes mapped writes produce and when (msync, munmap, close, fsync, F_FULLFSYNC, sync(2), idle)
I-MMAP-07  kill -9 of the worker with a live dirty mapping: SIGBUS or error, state of the mount, data on remount

Environment B only. Files are made on images the case creates in the guest; ntfs-3g runs in the foreground
with `-o backend=fskit,local,norecover,debug,no_detach` and its output is time-stamped; the device calls of
the worker are logged by the syncspy interpose library of I-SYNC-01 (built here the same way). The mapped
accesses are made by the C helper mmaptool.c, which keeps one descriptor and one MAP_SHARED mapping and
survives a worker kill. Observations go to <ID>.observations.json next to the case log. Cases fail only on
a broken invariant (data differs, a command hangs); what the backend does is recorded, not judged.
"""

import json
import os
import sys
import time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, os.path.join(os.path.dirname(os.path.dirname(os.path.abspath(__file__))), "functional"))

import mmaplib  # noqa: E402
from fsops import blob  # noqa: E402
from harness import Fail, case  # noqa: E402
from mmaplib import PAGE, Tool, Worker, put, sha, slurp, summarize, verify_image  # noqa: E402

KIB = 1024
MIB = KIB * KIB


class Run(object):
    """One case run: the volume, the built tools, the current worker, observation log."""

    def __init__(self, ctx, mib=256, spy=True):
        mmaplib.need_guest(ctx)
        self.ctx = ctx
        self.exe, self.spylib = mmaplib.build(ctx)
        self.use_spy = spy
        self.vol = ctx.volume(mib * MIB)
        self.w = None
        self.obs = {"page_size": PAGE, "steps": [], "notes": []}
        self.tools = []

    def mount(self, tag="main"):
        self.w = Worker(self.ctx, self.vol, tag, spy=self.spylib if self.use_spy else None)
        self.w.start()
        return self.w

    def path(self, name):
        return os.path.join(self.w.mnt, name)

    def tool(self):
        t = Tool(self.ctx, self.exe)
        self.tools.append(t)
        return t

    def step(self, label, fn, settle=1.0, tool_label=None):
        """Run fn, wait `settle` seconds, record the FUSE requests and device calls in the window."""
        w = self.w
        t0 = time.time()
        ret = fn()
        t1 = time.time()
        time.sleep(settle)
        s = summarize(w, t0, t1 + settle, tmark=t1)
        row = {"step": label, "ret": ret if isinstance(ret, (str, int, bool, type(None))) else str(ret),
               "call_ms": round((t1 - t0) * 1000.0, 1)}
        row.update(s)
        self.obs["steps"].append(row)
        self.ctx.log("STEP %-44s ret=%s %.0f ms | WRITE x%d %d B (%d B before return), device writes %d syncs %d | %s" % (
            label, row["ret"], row["call_ms"], s["write_requests"], s["write_bytes"], s["write_bytes_during"],
            s["device_writes"], s["device_syncs"], " ".join(s["fuse"]) if len(s["fuse"]) < 12 else
            " ".join(s["fuse"][:6]) + " ... (%d)" % len(s["fuse"])))
        return ret

    def note(self, text):
        self.obs["notes"].append(text)
        self.ctx.log("NOTE " + text)

    def finish(self):
        for t in self.tools:
            t.kill()
        with open(self.ctx.evidence_file("observations.json"), "w") as f:
            json.dump(self.obs, f, indent=1, sort_keys=True)


def ok(reply):
    return reply.startswith("ok")


def must(ctx, reply, what):
    if not ok(reply):
        raise Fail("%s: %s" % (what, reply))
    return reply


def save_worker_log(r, w, suffix="worker-debug.log"):
    with open(r.ctx.evidence_file(suffix), "a", encoding="utf-8", errors="replace") as f:
        for t, text in w.lines[-2000:]:
            f.write("%.3f %s\n" % (t, text))


def open_map(r, t, name, size, mode="rw"):
    must(r.ctx, t.cmd("open %s %s" % (r.path(name), mode)), "open " + name)
    must(r.ctx, t.cmd("map %d %s" % (size, mode)), "map " + name)


def remount_fresh(r, tag):
    """Clean unmount, then a new worker on the same image."""
    save_worker_log(r, r.w)
    r.w.stop()
    r.mount(tag)


# ---------------------------------------------------------------------------------------------
@case("I-MMAP-01", envs=("B",), title="mapped read of a file written with write(2); write(2) after a mapped read is seen")
def i_mmap_01(ctx):
    r = Run(ctx, spy=False)
    r.mount()
    expected = {}
    # A: write(2), close, then a mapped read of the whole file, at sizes around the page size.
    for i, size in enumerate((1, 4095, 4096, 4097, PAGE - 1, PAGE, PAGE + 1, 65536 + 13, 3 * MIB + 7)):
        name = "a%02d.bin" % i
        data = blob("mmap01a:%d" % i, size)
        put(r.path(name), data)
        expected[name] = data
        t = r.tool()
        open_map(r, t, name, size, "ro")
        rep, got = t.mget(0, size)
        if got != data:
            raise Fail("%s (%d bytes): mapped read differs from what write(2) wrote (%s)" % (name, size, rep))
        t.quit()
    r.note("A: 9 files from 1 byte to 3 MiB+7: mapped read equals write(2) data")
    # B: mapped read first, then write(2) from another process / from the mapping's own descriptor,
    # then read through the same live mapping.
    for size in (4096, PAGE + 5, MIB + 13):
        tag = "b%d" % size
        a = blob("mmap01b:%d" % size, size)
        put(r.path(tag + "-other.bin"), a)
        put(r.path(tag + "-own.bin"), a)
        put(r.path(tag + "-trunc.bin"), a)
        third = size // 3
        for kind in ("other", "own", "trunc"):
            name = "%s-%s.bin" % (tag, kind)
            t = r.tool()
            open_map(r, t, name, size, "ro" if kind != "own" else "rw")
            rep, got = t.mget(0, size)
            if got != a:
                raise Fail("%s: first mapped read differs" % name)
            if kind == "trunc":
                new = blob("mmap01c:%d" % size, size)
            else:
                new = a[:third] + blob("mmap01n:%d:%s" % (size, kind), third) + a[2 * third:]
            t0 = time.time()
            if kind == "other":
                with open(r.path(name), "r+b") as f:  # another descriptor in this process, the mapper is another process
                    f.seek(third)
                    f.write(new[third:2 * third])
            elif kind == "own":
                must(ctx, t.pwrite(third, new[third:2 * third]), "pwrite through the mapper's own descriptor")
            else:
                put(r.path(name), new)  # open(O_TRUNC), write the whole new content
            seen = None
            while time.time() - t0 < 5.0:
                rep, got = t.mget(0, size)
                if got == new:
                    seen = round((time.time() - t0) * 1000.0)
                    break
                time.sleep(0.05)
            r.obs["steps"].append({"step": "B %s size %d: write(2) then mapped read" % (kind, size),
                                   "seen_after_ms": seen})
            ctx.log("B %s size %d: new data seen through the live mapping after %s ms" % (kind, size, seen))
            if seen is None:
                raise Fail("%s: mapping still shows old data 5 s after write(2)" % name)
            expected[name] = new
            t.quit()
    # C: mapped write in a RW mapping, then read(2) of the same file by write(2) path (coherence the other way)
    data = blob("mmap01d", MIB)
    put(r.path("c.bin"), data)
    t = r.tool()
    open_map(r, t, "c.bin", MIB, "rw")
    mapped = blob("mmap01e", 5000)
    must(ctx, t.mput(777, mapped), "mapped write")
    want = data[:777] + mapped + data[777 + 5000:]
    if slurp(r.path("c.bin")) != want:
        raise Fail("read(2) does not see a mapped write on a live mapping")
    expected["c.bin"] = want
    t.cmd("msync sync")
    t.quit()
    r.w.stop()
    res = verify_image(ctx, r.vol, expected, "01")
    r.obs["verify"] = res
    r.finish()
    bad = dict((k, v) for k, v in res["files"].items() if v != "ok")
    if res["mount"] != "ok" or bad or res["fsck_rc"] != 0:
        raise Fail("after remount: mount %s, fsck rc %s, bad files %s" % (res["mount"], res["fsck_rc"], bad))


# ---------------------------------------------------------------------------------------------
@case("I-MMAP-02", envs=("B",), title="mapped write, msync(MS_SYNC/MS_ASYNC), read(2), hashes equal after remount")
def i_mmap_02(ctx):
    r = Run(ctx)
    r.mount()
    expected, alts, problems = {}, {}, []
    for mode in ("sync", "async"):
        for size in (4096, MIB + 13, 8 * MIB):
            name = "ms-%s-%d.bin" % (mode, size)
            old = blob("mmap02a:%s:%d" % (mode, size), size)
            new = blob("mmap02b:%s:%d" % (mode, size), size)
            put(r.path(name), old)
            time.sleep(0.5)
            t = r.tool()
            r.step("%s: open+map" % name, lambda: (must(ctx, t.cmd("open %s rw" % r.path(name)), "open"),
                                                    must(ctx, t.cmd("map %d rw" % size), "map"))[1], settle=0.5)
            r.step("%s: mapped write" % name, lambda: must(ctx, t.mput(0, new), "mput"))
            before = 0  # the mapped write alone sent nothing to the worker (checked in I-MMAP-06)
            rep = r.step("%s: msync %s" % (name, mode), lambda: t.cmd("msync %s" % mode))
            row = r.obs["steps"][-1]
            if not ok(rep):
                problems.append("%s: msync %s returned %s" % (name, mode, rep))
            if mode == "sync" and row["write_bytes_during"] < size - before:
                problems.append("%s: msync(MS_SYNC) returned with %d of %d bytes at the worker" % (
                    name, before + row["write_bytes_during"], size))
            same = r.step("%s: read(2) after msync" % name, lambda: slurp(r.path(name)) == new, settle=0.2)
            if not same:
                problems.append("%s: read(2) after msync differs" % name)
            r.step("%s: munmap" % name, lambda: t.cmd("munmap"))
            r.step("%s: close" % name, lambda: t.cmd("close"))
            t.quit()
            expected[name] = new
            alts[name] = {"old": old}
    r.step("clean unmount", lambda: r.w.stop(), settle=0.5)
    save_worker_log(r, r.w)
    res = verify_image(ctx, r.vol, expected, "02", alts)
    r.obs["verify"] = res
    r.finish()
    bad = dict((k, v) for k, v in res["files"].items() if v != "ok")
    if res["mount"] != "ok" or bad or res["fsck_rc"] != 0:
        problems.append("after remount: mount %s, fsck rc %s, bad files %s" % (res["mount"], res["fsck_rc"], bad))
    if problems:
        raise Fail("; ".join(problems))


# ---------------------------------------------------------------------------------------------
@case("I-MMAP-03", envs=("B",), title="mapped write without msync, munmap, close, idle, clean unmount: data after remount")
def i_mmap_03(ctx):
    r = Run(ctx)
    r.mount()
    expected, alts, tools, problems = {}, {}, {}, []
    sizes = (4096, MIB + 13, 8 * MIB)
    for size in sizes:
        name = "nosync-%d.bin" % size
        old = blob("mmap03a:%d" % size, size)
        new = blob("mmap03b:%d" % size, size)
        put(r.path(name), old)
        expected[name] = new
        alts[name] = {"old": old}
        tools[name] = (r.tool(), size, new)
    time.sleep(1.0)
    for name, (t, size, new) in sorted(tools.items()):
        must(ctx, t.cmd("open %s rw" % r.path(name)), "open")
        must(ctx, t.cmd("map %d rw" % size), "map")
    r.step("mapped write, 3 files, no msync", lambda: [must(ctx, t.mput(0, new), "mput") for t, size, new in tools.values()])
    r.step("idle 5 s with mapping live", lambda: time.sleep(5), settle=0)
    for name, (t, size, new) in sorted(tools.items()):
        if slurp(r.path(name)) != new:
            problems.append("%s: read(2) does not see the mapped write" % name)
    r.step("munmap (3 files)", lambda: [t.cmd("munmap") for t, _s, _n in tools.values()], settle=2.0)
    r.step("close (3 files)", lambda: [t.cmd("close") for t, _s, _n in tools.values()], settle=2.0)
    for name, (t, size, new) in sorted(tools.items()):
        if slurp(r.path(name)) != new:
            problems.append("%s: read(2) after munmap+close does not see the mapped write" % name)
        t.quit()
    r.step("idle 40 s, no application activity", lambda: time.sleep(40), settle=0)
    t0 = time.time()
    r.step("clean unmount (umount)", lambda: r.w.stop(), settle=0.0)
    r.obs["unmount_window"] = summarize(r.w, t0, time.time() + 0.5)
    save_worker_log(r, r.w)
    res = verify_image(ctx, r.vol, expected, "03", alts)
    r.obs["verify"] = res
    r.finish()
    ctx.log("after remount: %s" % res["files"])
    bad = dict((k, v) for k, v in res["files"].items() if v != "ok")
    if res["mount"] != "ok" or res["fsck_rc"] != 0:
        problems.append("after remount: mount %s, fsck rc %s" % (res["mount"], res["fsck_rc"]))
    if bad:
        problems.append("mapped data lost or changed after a clean unmount: %s" % bad)
    if problems:
        raise Fail("; ".join(problems))


# ---------------------------------------------------------------------------------------------
def unmount_variant(r, variant):
    """Dirty mapping, then umount; forced unmount when it refuses. Returns the observation dict."""
    ctx = r.ctx
    r.vol.create()  # a fresh image: the previous variant may have left a dirty one
    size = MIB + 13
    old = blob("mmap04a:" + variant, size)
    new = blob("mmap04b:" + variant, size)
    r.mount("u-" + variant)
    put(r.path("d.bin"), old)
    put(r.path("e.bin"), old)  # a second, untouched file
    t = r.tool()
    open_map(r, t, "d.bin", size, "rw")
    must(ctx, t.mput(0, new), "mput")
    if variant == "map-only":
        must(ctx, t.cmd("close"), "close fd, mapping stays")
    time.sleep(1.0)
    ob = {"variant": variant}
    w = r.w
    final = new
    stage = "umount"
    if variant != "force-first":
        t0 = time.time()
        rc, out = w.umount()
        ob["umount"] = {"rc": rc, "out": out, "still_mounted": bool(w.mounted())}
        ob["umount"].update(summarize(w, t0, time.time() + 0.5))
        if variant == "redirty" and w.mounted():
            # the refused umount pushed the first data; dirty the pages again, then force
            final = blob("mmap04c:" + variant, size)
            must(ctx, t.mput(0, final), "mput again")
            time.sleep(1.0)
            ob["redirtied_after_refused_umount"] = True
        if w.mounted():
            t1 = time.time()
            rc, out = w.diskutil_force()
            ob["diskutil_force"] = {"rc": rc, "out": out, "still_mounted": bool(w.mounted())}
            ob["diskutil_force"].update(summarize(w, t1, time.time() + 0.5))
            stage = "diskutil unmount force"
    if w.mounted():
        t1 = time.time()
        rc, out = w.umount(force=True)
        ob["umount_f"] = {"rc": rc, "out": out, "still_mounted": bool(w.mounted())}
        ob["umount_f"].update(summarize(w, t1, time.time() + 0.5))
        stage = "umount -f"
    ob["unmounted_by"] = stage if not w.mounted() else None
    ob["worker_exited"] = w.wait_exit(timeout=20) if not w.mounted() else None
    ob["worker_rc"] = w.proc.returncode
    # What the live mapping does now.
    after = {}
    after["mget"], _ = t.mget(0, size, "u")
    after["mtouch"] = t.cmd("mtouch %d" % (size - 1), timeout=20)
    after["mput"] = t.mput(0, new[:4096], "u2")
    after["msync"] = t.cmd("msync sync", timeout=20)
    after["munmap"] = t.cmd("munmap", timeout=20)
    after["close"] = t.cmd("close", timeout=20) if variant != "map-only" else "(fd already closed)"
    ob["mapper_after_unmount"] = after
    t.quit()
    save_worker_log(r, w, "worker-debug-%s.log" % variant)
    w.cleanup()
    res = verify_image(ctx, r.vol, {"d.bin": final, "e.bin": old}, "04-" + variant,
                       {"d.bin": {"old": old, "first-write": new}})
    ob["verify"] = res
    ctx.log("variant %s: %s" % (variant, json.dumps(ob, sort_keys=True)))
    return ob


@case("I-MMAP-04", envs=("B",), title="unmount with dirty pages and a live mapping: umount, forced unmount, data after remount")
def i_mmap_04(ctx):
    r = Run(ctx)
    problems = []
    r.obs["variants"] = {}
    for variant in ("fd+map", "map-only", "force-first", "redirty"):
        ob = unmount_variant(r, variant)
        r.obs["variants"][variant] = ob
        if ob["unmounted_by"] is None:
            problems.append("%s: nothing unmounted the volume" % variant)
        if "TIMEOUT" in json.dumps(ob["mapper_after_unmount"]):
            problems.append("%s: the mapping's owner hung after the unmount" % variant)
        if ob["worker_exited"] is False:
            problems.append("%s: worker did not exit after the unmount" % variant)
        vf = ob["verify"]
        r.obs.setdefault("data_after_forced_unmount", {})[variant] = vf["files"].get("d.bin")
        if vf["mount"] != "ok" or vf["fsck_rc"] != 0:
            problems.append("%s: image after the unmount: mount %s, fsck rc %s" % (variant, vf["mount"], vf["fsck_rc"]))
    r.finish()
    if problems:
        raise Fail("; ".join(problems))


# ---------------------------------------------------------------------------------------------
@case("I-MMAP-05", envs=("B",), title="mapping that extends a file; access past EOF; truncate under a mapping")
def i_mmap_05(ctx):
    r = Run(ctx)
    r.mount()
    expected, problems = {}, []
    # A: ftruncate up through the descriptor, then write through the map, msync, read(2).
    for size in (4096, PAGE + 100, MIB + 13):
        name = "ext-%d.bin" % size
        new = blob("mmap05a:%d" % size, size)
        t = r.tool()
        must(ctx, t.cmd("open %s create" % r.path(name)), "create")
        r.step("%s: ftruncate up" % name, lambda: must(ctx, t.cmd("ftruncate %d" % size), "ftruncate"), settle=0.5)
        must(ctx, t.cmd("map %d rw" % size), "map")
        r.step("%s: mapped write over the new range" % name, lambda: must(ctx, t.mput(0, new), "mput"))
        if t.cmd("fsize") != "ok %d" % size:
            problems.append("%s: size after ftruncate is not %d" % (name, size))
        r.step("%s: msync sync" % name, lambda: t.cmd("msync sync"))
        if slurp(r.path(name)) != new:
            problems.append("%s: read(2) after mapped write into a grown file differs" % name)
        expected[name] = new
        t.cmd("munmap")
        t.cmd("close")
        t.quit()
    # B: the file is created empty, grown by ftruncate, written through the map, NO msync, munmap, close.
    name = "ext-nosync.bin"
    size = MIB + 13
    new = blob("mmap05b", size)
    t = r.tool()
    must(ctx, t.cmd("open %s create" % r.path(name)), "create")
    must(ctx, t.cmd("ftruncate %d" % size), "ftruncate")
    must(ctx, t.cmd("map %d rw" % size), "map")
    must(ctx, t.mput(0, new), "mput")
    r.step("%s: munmap" % name, lambda: t.cmd("munmap"), settle=2.0)
    r.step("%s: close" % name, lambda: t.cmd("close"), settle=2.0)
    t.quit()
    expected[name] = new
    # C: mapping longer than the file: bytes after EOF in the last page, then whole pages after EOF.
    name = "pastEOF.bin"
    size = PAGE + PAGE // 2
    data = blob("mmap05c", size)
    put(r.path(name), data)
    t = r.tool()
    open_map(r, t, name, 3 * PAGE, "rw")
    inpage = t.cmd("mtouch %d" % (size + 10))
    farpage = t.cmd("mtouch %d" % (2 * PAGE + 1))
    wr_inpage = t.mput(size + 10, b"TAIL" * 8, "c1")
    wr_farpage = t.mput(2 * PAGE + 1, b"FAR!" * 8, "c2")
    r.obs["pastEOF"] = {"read_in_last_page": inpage, "read_whole_page_after_eof": farpage,
                        "write_in_last_page_after_eof": wr_inpage, "write_whole_page_after_eof": wr_farpage}
    t.cmd("msync sync")
    r.obs["pastEOF"]["fsize_after_msync"] = t.cmd("fsize")
    r.note("past EOF: %s" % r.obs["pastEOF"])
    t.cmd("munmap")
    t.cmd("close")
    t.quit()
    if os.path.getsize(r.path(name)) != size:
        problems.append("%s: size changed by mapped writes after EOF: %d" % (name, os.path.getsize(r.path(name))))
    expected[name] = data
    # D: truncate under a live mapping: shrink, access the cut-off pages, grow back, bytes must be zero.
    name = "shrink.bin"
    size = 4 * PAGE
    data = blob("mmap05d", size)
    put(r.path(name), data)
    t = r.tool()
    open_map(r, t, name, size, "rw")
    must(ctx, t.mput(0, data[:size]), "mput")
    t.cmd("msync sync")
    must(ctx, t.cmd("ftruncate %d" % PAGE), "shrink")
    shrink = {"read_cut_page": t.cmd("mtouch %d" % (2 * PAGE + 5)), "read_kept_page": t.cmd("mtouch 5"),
              "write_cut_page": t.mput(3 * PAGE, b"X" * 16, "d1")}
    t.cmd("ftruncate %d" % size)
    rep, got = t.mget(PAGE, size - PAGE, "d2")
    shrink["regrown_bytes_zero"] = (got is not None and got == b"\0" * (size - PAGE))
    shrink["regrown_via_read2_zero"] = (slurp(r.path(name))[PAGE:] == b"\0" * (size - PAGE))
    r.obs["shrink"] = shrink
    r.note("shrink under a mapping: %s" % shrink)
    if not shrink["regrown_via_read2_zero"]:
        problems.append("%s: bytes beyond the shrunk size are not zero after growing again (read(2))" % name)
    if not shrink["regrown_bytes_zero"]:
        problems.append("%s: bytes beyond the shrunk size are not zero after growing again (mapped read: %s)" % (name, rep))
    t.cmd("msync sync")
    t.cmd("munmap")
    t.cmd("close")
    t.quit()
    expected[name] = data[:PAGE] + b"\0" * (size - PAGE)
    r.w.stop()
    save_worker_log(r, r.w)
    res = verify_image(ctx, r.vol, expected, "05")
    r.obs["verify"] = res
    r.finish()
    bad = dict((k, v) for k, v in res["files"].items() if v != "ok")
    ctx.log("after remount: %s" % res["files"])
    if res["mount"] != "ok" or res["fsck_rc"] != 0 or bad:
        problems.append("after remount: mount %s, fsck rc %s, bad files %s" % (res["mount"], res["fsck_rc"], bad))
    if problems:
        raise Fail("; ".join(problems))


# ---------------------------------------------------------------------------------------------
@case("I-MMAP-06", envs=("B",), title="FUSE opcodes of mapped writes and when they arrive (msync, munmap, close, fsync, sync, idle)")
def i_mmap_06(ctx):
    r = Run(ctx)
    r.mount()
    size = MIB
    scen = {}
    names = ("s1-idle", "s2-msync-async", "s3-msync-sync", "s4-fsync", "s5-fullfsync", "s6-sync2", "s7-munmap",
             "s8-munmap-close", "s9-close-map-live", "s10-read2")
    for n in names:
        old = blob("mmap06a:" + n, size)
        new = blob("mmap06b:" + n, size)
        put(r.path(n + ".bin"), old)
        scen[n] = {"old": old, "new": new, "tool": None}
    time.sleep(1.0)

    def dirty(n):
        t = r.tool()
        scen[n]["tool"] = t
        open_map(r, t, n + ".bin", size, "rw")
        r.step("%s: mapped write 1 MiB" % n, lambda: must(ctx, t.mput(0, scen[n]["new"]), "mput"), settle=2.0)
        return t

    t = dirty("s1-idle")
    r.step("s1-idle: idle 40 s (mapping live, fd open)", lambda: time.sleep(40), settle=0)
    t = dirty("s2-msync-async")
    r.step("s2: msync(MS_ASYNC)", lambda: t.cmd("msync async"), settle=3.0)
    t = dirty("s3-msync-sync")
    r.step("s3: msync(MS_SYNC)", lambda: t.cmd("msync sync"), settle=2.0)
    t = dirty("s4-fsync")
    r.step("s4: fsync(fd), mapping live", lambda: t.cmd("fsync"), settle=2.0)
    t = dirty("s5-fullfsync")
    r.step("s5: fcntl(F_FULLFSYNC), mapping live", lambda: t.cmd("fullfsync"), settle=2.0)
    t = dirty("s6-sync2")
    r.step("s6: sync(2), mapping live, fd open", lambda: os.sync(), settle=3.0)
    t = dirty("s7-munmap")
    r.step("s7: munmap, fd open", lambda: t.cmd("munmap"), settle=3.0)
    r.step("s7: close", lambda: t.cmd("close"), settle=3.0)
    t = dirty("s8-munmap-close")
    r.step("s8: munmap", lambda: t.cmd("munmap"), settle=1.0)
    r.step("s8: close", lambda: t.cmd("close"), settle=1.0)
    r.step("s8: idle 40 s after close", lambda: time.sleep(40), settle=0)
    t = dirty("s9-close-map-live")
    r.step("s9: close(fd), mapping stays", lambda: t.cmd("close"), settle=3.0)
    r.step("s9: idle 20 s", lambda: time.sleep(20), settle=0)
    t = dirty("s10-read2")
    r.step("s10: read(2) of the whole file, mapping live", lambda: slurp(r.path("s10-read2.bin")) == scen["s10-read2"]["new"],
           settle=2.0)
    # No read(2) of the files here: a read pushes dirty mapped pages to the worker (s10).
    r.step("clean umount with live mappings of s1..s6, s9", lambda: r.w.umount(), settle=0.0)
    t0 = time.time()
    still = r.w.mounted()
    ctx.log("mounted after umount: %s" % still)
    r.obs["umount_with_live_maps_still_mounted"] = bool(still)
    for n in names:
        tl = scen[n]["tool"]
        if tl is not None and tl.proc.poll() is None:
            tl.cmd("msync sync", timeout=20)
            tl.cmd("munmap", timeout=20)
            tl.cmd("close", timeout=20)
            tl.quit()
    if r.w.mounted():
        t1 = time.time()
        r.step("umount again after the mappings are gone", lambda: r.w.umount(), settle=0.0)
    r.w.wait_exit(timeout=30)
    save_worker_log(r, r.w)
    expected = dict((n + ".bin", scen[n]["new"]) for n in names)
    alts = dict((n + ".bin", {"old": scen[n]["old"]}) for n in names)
    res = verify_image(ctx, r.vol, expected, "06", alts)
    r.obs["verify"] = res
    r.finish()
    ctx.log("after remount: %s" % res["files"])
    problems = []
    bad = dict((k, v) for k, v in res["files"].items() if v != "ok")
    if res["mount"] != "ok" or res["fsck_rc"] != 0:
        problems.append("after remount: mount %s, fsck rc %s" % (res["mount"], res["fsck_rc"]))
    if bad:
        problems.append("data lost or changed after the unmount: %s" % bad)
    if problems:
        raise Fail("; ".join(problems))


# ---------------------------------------------------------------------------------------------
def kill_variant(r, variant):
    ctx = r.ctx
    r.vol.create()  # a fresh image: the previous variant killed a worker
    npages = 8
    size = npages * PAGE
    old = blob("mmap07a:" + variant, size)
    p1 = blob("mmap07b:" + variant, PAGE)
    p2 = blob("mmap07c:" + variant, PAGE)
    # a fresh mount for the file, so that its pages are not in the cache when it is mapped
    r.mount("k0-" + variant)
    put(r.path("k.bin"), old)
    put(r.path("other.bin"), old)
    remount_fresh(r, "k-" + variant)
    w = r.w
    t = r.tool()
    open_map(r, t, "k.bin", size, "rw")
    ob = {"variant": variant}
    exp_new = bytearray(old)
    if variant in ("dirty", "synced-then-dirty"):
        must(ctx, t.mput(0, p1), "mput page 0")
        exp_new[0:PAGE] = p1
        if variant == "synced-then-dirty":
            must(ctx, t.cmd("msync sync"), "msync")
        must(ctx, t.mput(PAGE, p2), "mput page 1")
        exp_new[PAGE:2 * PAGE] = p2
    else:  # clean: only read pages 0 and 1
        t.mget(0, 2 * PAGE, "k")
    time.sleep(0.3)  # short, so that the 30 s syncer tick is unlikely to fall between the write and the kill
    t_kill = time.time()
    pid = w.pid()
    w.kill()
    ob["killed_worker_pid"] = pid
    ob["worker_rc"] = w.proc.returncode
    ob["still_mounted_after_kill"] = bool(w.mounted())
    # The mapper's view, every command with a 25 s deadline.
    a = {}
    a["read_dirty_resident_page0"] = t.mget(0, PAGE, "k0")[0]
    a["read_dirty_resident_page1"] = t.mget(PAGE, PAGE, "k1")[0]
    a["read_untouched_page5"] = t.cmd("mtouch %d" % (5 * PAGE), 25)
    a["read_untouched_page7"] = t.cmd("mtouch %d" % (7 * PAGE), 25)
    a["write_resident_page0"] = t.mput(0, p2, "k2")
    a["write_untouched_page6"] = t.mput(6 * PAGE, p2, "k3")
    a["msync_sync"] = t.cmd("msync sync", 25)
    a["fsync"] = t.cmd("fsync", 25)
    a["pread_page3"] = t.pread(3 * PAGE, 100)[0]
    a["pwrite_page3"] = t.pwrite(3 * PAGE, b"Z" * 100)
    a["fsize"] = t.cmd("fsize", 25)
    a["munmap"] = t.cmd("munmap", 25)
    a["close"] = t.cmd("close", 25)
    ob["mapper_after_kill"] = a
    ob["ls_mount_dir"] = w.run(["/bin/ls", w.mnt], timeout=20)
    ob["stat_file"] = w.run(["/usr/bin/stat", "-f", "%z", os.path.join(w.mnt, "k.bin")], timeout=20)
    ob["read_file_with_cat"] = w.run(["/bin/cat", os.path.join(w.mnt, "other.bin")], timeout=20)[0]
    ob["seconds_to_run_probes"] = round(time.time() - t_kill, 1)
    t.quit()
    rc, out = w.umount()
    ob["umount"] = {"rc": rc, "out": out, "still_mounted": bool(w.mounted())}
    if w.mounted():
        rc, out = w.diskutil_force()
        ob["diskutil_force"] = {"rc": rc, "out": out, "still_mounted": bool(w.mounted())}
    if w.mounted():
        rc, out = w.umount(force=True)
        ob["umount_f"] = {"rc": rc, "out": out, "still_mounted": bool(w.mounted())}
    ob["unmounted"] = not w.mounted()
    save_worker_log(r, w, "worker-debug-kill-%s.log" % variant)
    w.cleanup()
    res = verify_image(ctx, r.vol, {"k.bin": bytes(exp_new), "other.bin": old}, "07-" + variant,
                       {"k.bin": {"old": old}}, {"k.bin": {"old": old, "p1": p1, "p2": b"\xff" * PAGE + p2}})
    ob["verify"] = res
    ctx.log("variant %s: %s" % (variant, json.dumps(ob, sort_keys=True)))
    return ob


@case("I-MMAP-07", envs=("B",), title="kill -9 of the worker with a live dirty mapping: SIGBUS or error, mount state, data on remount")
def i_mmap_07(ctx):
    r = Run(ctx)
    r.obs["variants"] = {}
    problems = []
    for variant in ("dirty", "synced-then-dirty", "clean"):
        ob = kill_variant(r, variant)
        r.obs["variants"][variant] = ob
        if "TIMEOUT" in json.dumps(ob["mapper_after_kill"]):
            problems.append("%s: a mapper command hung after the kill" % variant)
        if not ob["unmounted"]:
            problems.append("%s: the volume could not be unmounted after the kill" % variant)
    r.finish()
    if problems:
        raise Fail("; ".join(problems))
