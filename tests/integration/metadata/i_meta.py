# SPDX-License-Identifier: GPL-2.0-or-later
"""I-META-01..07 (T-M3-06, IMPLEMENTATION_PLAN 3.6, R-09): timestamps and attribute flags.

I-META-01  times of new files and directories: macOS view vs on-disk $STANDARD_INFORMATION, precision,
           creation time shown and stored
I-META-02  explicit times (touch -t, -m, -a, utimensat with ns and UTIME_OMIT, range, cp -p), remount
I-META-03  atime updates on read and readdir under the mount options atime, relatime, noatime, default
I-META-04  creation time: setattrlist ATTR_CMN_CRTIME, system.ntfs_times, cp -p from APFS
I-META-05  mtime, ctime and atime effects of write, truncate, rename, move, unlink, create, link, chmod, read
I-META-06  read-only attribute (FILE_ATTRIBUTE_READONLY) against chflags uchg and chmod
I-META-07  hidden flag (FILE_ATTRIBUTE_HIDDEN) against chflags hidden and dot-files; hide_* options

The on-disk values come from the unmounted image (metalib.RawVolume reads the MFT: $STANDARD_INFORMATION
and $FILE_NAME times and the file flags), never from a tool's rounded text. The cases record what the
backend does and assert it, so a new macFUSE or NTFS-3G that changes it fails the case and has to be looked
at. Environment B is the product; Environment A (Linux, kernel FUSE) is the reference. Expectations that
differ are named constants. Python 3.9, standard library only.
"""

import os
import struct
import sys
import time

_here = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, _here)
sys.path.insert(0, os.path.join(_here, "..", "functional"))

import fsops  # noqa: E402
import metalib as ml  # noqa: E402
from fsops import put  # noqa: E402
from harness import Fail, case  # noqa: E402
from harness.seeded import MACOS_ROOT_METADATA  # noqa: E402

TOUCH = "/usr/bin/touch" if ml.IS_MAC else "touch"
PRODUCT_B = "noatime,windows_names,streams_interface=openxattr,fmask=0133,dmask=0022"
PRODUCT_A = "noatime,fmask=0133,dmask=0022"

# Environment B: the backend calls setcrtime after every create, and NTFS-3G's macOS code stores
# tv_sec as the raw FILETIME (seconds as 100 ns ticks): $STANDARD_INFORMATION creation time is
# 1601-01-01 00:0x. Environment A stores the real time. $FILE_NAME creation time is right in both.
SI_CRTIME_IS_RAW_SECONDS = {"A": False, "B": True}
# Environment B shows the creation time as 1970-01-01 (getxtimes is not used by the backend).
BIRTH_SHOWN = {"A": None, "B": 0}


def product(ctx):
    return PRODUCT_B if ctx.env == "B" else PRODUCT_A


def new_volume(ctx, mib=64):
    return fsops.open_volume(ctx, mib)


def line(ctx, label, v):
    ctx.log("%s: %s" % (label, ml.fmt_view(v) if v else "missing"))


def need(ctx, bad, what):
    for b in bad[:20]:
        ctx.log("MISMATCH " + b)
    if bad:
        raise Fail("%s: %d problems, first: %s" % (what, len(bad), bad[0]))


def listing(mnt, sub=""):
    names = sorted(os.listdir(os.path.join(mnt, sub)))
    return [n for n in names if not (sub == "" and n in MACOS_ROOT_METADATA)]


# ---------------------------------------------------------------- I-META-01

@case("I-META-01", envs=("A", "B"), title="times of new entries: view, on-disk values, precision, creation time")
def i_meta_01(ctx):
    vol = new_volume(ctx)
    fsops.mount(ctx, vol)
    m = vol.mnt
    t0 = ml.ts(time.time()) - 2 * 10 ** 9
    ctx.sh("echo one > %s/f1" % m)
    time.sleep(1.2)
    ctx.sh("echo two >> %s/f1" % m)
    ctx.sh("mkdir %s/d1" % m)
    ctx.sh("echo three > %s/d1/f2" % m)
    put(m + "/d1/f3", b"x" * 100000)
    time.sleep(0.2)
    t1 = ml.ts(time.time()) + 2 * 10 ** 9
    names = ["f1", "d1", "d1/f2", "d1/f3"]
    live = ml.snap(m, names)
    for n in names:
        line(ctx, "live " + n, live[n])
    if ml.IS_MAC:
        try:
            ctx.log("getattrlist f1: %s" % ml.getattrlist_times(m + "/f1"))
        except OSError as e:
            ctx.log("getattrlist f1 failed: %s" % e)
    else:
        ctx.sh("stat -c 'birth=%%w mtime=%%y' %s/f1" % m, check=False)
    ents, after = ml.cycle(ctx, vol, names)
    obs = {}
    bad = []
    # cross-check of the raw MFT reader against ntfsinfo (whole seconds, floor of the FILETIME)
    for nm in ("f1", "d1"):
        tt = ml.ntfsinfo_times(ctx, vol, "/" + nm)
        si = ml.disk(ents, nm)[1]
        raw = [si["crtime"], si["mtime"], si["ctime"], si["atime"]]
        for (label, secs), ftv in zip(tt, raw):
            ctx.log("ntfsinfo %s: %s = %d (raw reader: %d)" % (nm, label, secs, ftv // ml.FT_PER_SEC - ml.EPOCH_DELTA))
            if secs != ftv // ml.FT_PER_SEC - ml.EPOCH_DELTA:
                bad.append("ntfsinfo and the raw MFT reader disagree on %s of %s: %d against %d" % (
                    label, nm, secs, ftv // ml.FT_PER_SEC - ml.EPOCH_DELTA))
    for n in names:
        base = os.path.basename(n)
        rn, si, fn = ml.disk(ents, base)
        v = after[n]
        line(ctx, "after remount " + n, v)
        ctx.log("disk %s (record %d) SI: %s" % (n, rn, ml.fmt_si(si)))
        ctx.log("disk %s (record %d) FN: %s" % (n, rn, ml.fmt_si(fn)))
        d = ml.si_ns(si)
        obs[n] = {"view": v, "si": si, "fn": fn}
        # the view after a remount is the on-disk value, to the nanosecond
        for k in ("mtime", "ctime", "atime"):
            if v[k] != d[k]:
                bad.append("%s %s: view %s, disk %s" % (n, k, ml.fmt_ns(v[k]), ml.fmt_ns(d[k])))
            if v[k] % 100:
                bad.append("%s %s view %s is finer than 100 ns" % (n, k, ml.fmt_ns(v[k])))
        for k in ("mtime", "ctime"):
            if not t0 <= d[k] <= t1:
                bad.append("%s on-disk %s %s is not near the clock" % (n, k, ml.fmt_ns(d[k])))
        # $FILE_NAME times are written at create and are not updated by writes
        fnn = ml.si_ns({"crtime": fn["crtime"], "mtime": fn["mtime"], "ctime": fn["ctime"], "atime": fn["atime"]})
        if not t0 <= fnn["crtime"] <= t1:
            bad.append("%s $FILE_NAME creation time %s is not near the clock" % (n, ml.fmt_ns(fnn["crtime"])))
        # creation time in $STANDARD_INFORMATION
        cr = si["crtime"]
        if SI_CRTIME_IS_RAW_SECONDS[ctx.env]:
            if not t0 // 10 ** 9 <= cr <= t1 // 10 ** 9:
                bad.append("%s SI creation FILETIME %d is not the create time in seconds" % (n, cr))
            ctx.log("%s SI creation time is %s: the create time in seconds stored as 100 ns ticks" % (n, ml.fmt_ns(d["crtime"])))
        else:
            if not t0 <= d["crtime"] <= t1:
                bad.append("%s SI creation time %s is not near the clock" % (n, ml.fmt_ns(d["crtime"])))
        # creation time as the OS shows it
        want = BIRTH_SHOWN[ctx.env]
        if v["birth"] != want or live[n]["birth"] != want:
            bad.append("%s birth time shown: live %s, after remount %s, expected %s" % (
                n, live[n]["birth"], v["birth"], want))
    # write after create: $STANDARD_INFORMATION mtime moves, $FILE_NAME mtime stays
    f1 = obs["f1"]
    gap = (f1["si"]["mtime"] - f1["fn"]["mtime"]) / 1e7
    ctx.log("f1: SI mtime minus FN mtime = %.2f s (FN times are not kept up to date)" % gap)
    if gap < 1.0:
        bad.append("f1: SI mtime is %.2f s after FN mtime, expected at least 1 s" % gap)
    # sub-100 ns and sub-microsecond digits seen (log only)
    sub_us = sum(1 for n in names for k in ("mtime", "ctime", "atime") if after[n][k] % 1000)
    ctx.log("view values with digits below a microsecond: %d of %d" % (sub_us, 3 * len(names)))
    live_diff = ["%s %s" % (n, k) for n in names for k in ("mtime", "ctime", "atime")
                 if live[n][k] != after[n][k]]
    ctx.log("live view differs from the remounted view in %d values: %s" % (len(live_diff), live_diff[:6]))
    ml.dump_json(ctx, "observations.json", obs)
    fsops.fsck(ctx, vol)
    need(ctx, bad, "I-META-01")


# ---------------------------------------------------------------- I-META-02

BASE = ml.ymd(2010, 1, 1) * 10 ** 9
T1 = ml.ymd(2001, 1, 2, 3, 4, 5) * 10 ** 9
T2 = ml.ymd(2002, 2, 2, 2, 2, 2) * 10 ** 9
T3 = ml.ymd(2003, 3, 3, 3, 3, 3) * 10 ** 9
NS_A = 1111111111987654321
NS_M = 1234567890123456789
RANGE = [
    ("r_1601", -11644473600 * 10 ** 9),                       # FILETIME 0
    ("r_1950", ml.ymd(1950, 6, 15, 12) * 10 ** 9 + 250000000),
    ("r_neg_half", -500000000),                                # 1969-12-31 23:59:59.5
    ("r_2106", ml.ymd(2106, 2, 7, 6, 28, 16) * 10 ** 9),       # first second past 32 bits
    ("r_2200", ml.ymd(2200, 1, 1) * 10 ** 9 + 999999900),
]
# Settings whose stored result differs from the request, per environment. The key is the case name,
# the value the reason. Empty means the request is stored as asked (to 100 ns).
KNOWN_SET_DIFFS = {
    "A": {},
    "B": {
        "touch_m.atime": "a change of mtime alone (touch -m, UTIME_OMIT for atime) moves atime to now",
        "omit_a.atime": "a change of mtime alone (touch -m, UTIME_OMIT for atime) moves atime to now",
        "touch_a.atime": "a change of atime alone (touch -a, UTIME_OMIT for mtime) is dropped, nothing reaches the disk",
        "omit_m.atime": "a change of atime alone (touch -a, UTIME_OMIT for mtime) is dropped, nothing reaches the disk",
    },
}
# Views that differ from the on-disk value after a remount: times before 1970 with a fraction of a second
# show rounded up to the next whole second (the disk value is exact).
# The same in A: NTFS-3G's conversion of FILETIME to timespec, not the macOS backend.
KNOWN_VIEW_DIFFS = {
    "A": {"r_1950.mtime", "r_1950.atime", "r_neg_half.mtime", "r_neg_half.atime"},
    "B": {"r_1950.mtime", "r_1950.atime", "r_neg_half.mtime", "r_neg_half.atime"},
}


@case("I-META-02", envs=("A", "B"), title="explicit mtime and atime (touch, utimensat, cp -p) round trip across remount")
def i_meta_02(ctx):
    vol = new_volume(ctx)
    fsops.mount(ctx, vol)
    m = vol.mnt
    ref = os.path.join(ctx.work, "src-apfs")
    with open(ref, "wb") as f:
        f.write(b"cp -p source\n")
    # source: atime 2005-05-05 05:05:05.123456789, mtime 2006-06-06 06:06:06.987654321
    ml.utimens(ref, ml.ymd(2005, 5, 5, 5, 5, 5) * 10 ** 9 + 123456789, ml.ymd(2006, 6, 6, 6, 6, 6) * 10 ** 9 + 987654321)
    cases = ["touch_t", "touch_m", "touch_a", "ns_both", "omit_a", "omit_m", "touch_now", "cp_p"] + [r[0] for r in RANGE]
    for n in cases:
        if n != "cp_p":
            ctx.sh("echo x > %s/%s" % (m, n))
    ctx.sh("cp -p %s %s/cp_p" % (ref, m))
    ctx.sh("cp %s %s/cp_plain" % (ref, m))
    for n in cases:
        if n not in ("cp_p",):
            ml.utimens(m + "/" + n, BASE, BASE)
    fixture_end = ml.ts(time.time())
    time.sleep(1.1)
    ctx.sh("TZ=UTC %s -t 200101020304.05 %s/touch_t" % (TOUCH, m))
    ctx.sh("TZ=UTC %s -m -t 200202020202.02 %s/touch_m" % (TOUCH, m))
    ctx.sh("TZ=UTC %s -a -t 200303030303.03 %s/touch_a" % (TOUCH, m))
    ml.utimens(m + "/ns_both", NS_A, NS_M)
    ml.utimens(m + "/omit_a", None, NS_M)
    ml.utimens(m + "/omit_m", NS_A, None)
    t_now0 = ml.ts(time.time())
    ctx.sh("%s %s/touch_now" % (TOUCH, m))
    for n, t in RANGE:
        try:
            ml.utimens(m + "/" + n, t, t)
        except OSError as e:
            ctx.log("utimens %s failed: %s" % (n, e))
    # (name, wanted mtime, wanted atime); None = not asked, the fixture value BASE stays
    t_now1 = ml.ts(time.time()) + 10 ** 9
    want = {
        "touch_t": (T1, T1), "touch_m": (T2, BASE), "touch_a": (BASE, T3),
        "ns_both": (ml.trunc100(NS_M), ml.trunc100(NS_A)),
        "omit_a": (ml.trunc100(NS_M), BASE), "omit_m": (BASE, ml.trunc100(NS_A)),
        "cp_p": (ml.ymd(2006, 6, 6, 6, 6, 6) * 10 ** 9 + 987654300, ml.ymd(2005, 5, 5, 5, 5, 5) * 10 ** 9 + 123456700),
    }
    for n, t in RANGE:
        want[n] = (ml.trunc100(t), ml.trunc100(t))
    names = cases + ["cp_plain"]
    live = ml.snap(m, names)
    for n in names:
        line(ctx, "live " + n, live[n])
    ents, after = ml.cycle(ctx, vol, names)
    bad = []
    view_diffs = set()
    for b in ml.view_vs_disk(ents, after):
        ctx.log("VIEW DIFF " + b)
        view_diffs.add(b.split(":")[0].replace(" ", "."))
    table = {}
    diffs = []
    for n in names:
        v = after[n]
        si = ents[n][1]
        d = ml.si_ns(si)
        line(ctx, "after remount " + n, v)
        table[n] = {"want": want.get(n), "mtime": v["mtime"], "atime": v["atime"], "ctime": v["ctime"],
                    "disk": d}
        if n in want:
            wm, wa = want[n]
            if d["mtime"] != wm:
                diffs.append("%s.mtime: asked %s, on disk %s" % (n, ml.fmt_ns(wm), ml.fmt_ns(d["mtime"])))
            if d["atime"] != wa:
                diffs.append("%s.atime: asked %s, on disk %s" % (n, ml.fmt_ns(wa), ml.fmt_ns(d["atime"])))
        # ctime cannot be set: it is the time of the change
        if n not in ("cp_p", "cp_plain") and not fixture_end <= v["ctime"] <= t_now1:
            bad.append("%s ctime %s is not the time of the change" % (n, ml.fmt_ns(v["ctime"])))
    ctx.log("touch (no argument): mtime %s, atime %s, clock was %s" % (
        ml.fmt_ns(after["touch_now"]["mtime"]), ml.fmt_ns(after["touch_now"]["atime"]), ml.fmt_ns(t_now0)))
    if not t_now0 - 2 * 10 ** 9 <= after["touch_now"]["mtime"] <= t_now1:
        bad.append("touch_now mtime is not now")
    cpplain = after["cp_plain"]
    ctx.log("cp without -p: mtime %s (source %s)" % (ml.fmt_ns(cpplain["mtime"]), ml.fmt_ns(ml.ymd(2005, 5, 5, 5, 5, 5) * 10 ** 9)))
    known = KNOWN_SET_DIFFS[ctx.env]
    for d in diffs:
        ctx.log("SET DIFF %s" % d)
    got_keys = set(d.split(":")[0] for d in diffs)
    bad += ["unexpected: %s" % d for d in diffs if d.split(":")[0] not in known]
    bad += ["expected difference did not happen: %s (%s)" % (k, known[k]) for k in sorted(set(known) - got_keys)]
    kv = KNOWN_VIEW_DIFFS[ctx.env]
    bad += ["unexpected view difference: %s" % k for k in sorted(view_diffs - kv)]
    bad += ["expected view difference did not happen: %s" % k for k in sorted(kv - view_diffs)]
    ml.dump_json(ctx, "observations.json", table)
    fsops.fsck(ctx, vol)
    need(ctx, bad, "I-META-02")


# ---------------------------------------------------------------- I-META-03

OLD = ml.ymd(2020, 1, 1) * 10 ** 9
# Effect of a read on the on-disk atime: True when it moves to the read time.
ATIME_UPDATES = {
    "A": {"noatime": False, "atime": True, "relatime": True, "default": True},
    "B": {"noatime": False, "atime": True, "relatime": True, "default": True},
}


# Does the mounted view show the atime update at once? B: no, stat keeps the old value (the disk has the
# new one, T-M2-04). A: yes.
ATIME_LIVE_SHOWN = {"A": True, "B": False}


@case("I-META-03", envs=("A", "B"), title="atime on read and readdir under noatime, atime, relatime and the default")
def i_meta_03(ctx):
    vol = new_volume(ctx)
    opts = [("noatime", "noatime"), ("atime", "atime"), ("relatime", "relatime"), ("default", ""),
            ("product", product(ctx))]
    fsops.mount(ctx, vol)
    m = vol.mnt
    for tag, _o in opts:
        ctx.sh("echo content > %s/fa_%s; mkdir %s/da_%s; echo x > %s/da_%s/inner" % (m, tag, m, tag, m, tag))
    time.sleep(0.2)
    for tag, _o in opts:
        for n in ("fa_%s" % tag, "da_%s" % tag, "da_%s/inner" % tag):
            ml.utimens(m + "/" + n, OLD, OLD)
    fsops.unmount(ctx, vol)
    ents = ml.disk_entries(vol.img)
    bad = []
    res = {}
    for tag, _o in opts:
        for n in ("fa_%s" % tag, "da_%s" % tag):
            a = ml.si_ns(ents[n][1])["atime"]
            if a != OLD:
                bad.append("fixture %s atime %s is not the set value" % (n, ml.fmt_ns(a)))
    for tag, o in opts:
        fsops.mount(ctx, vol, extra=o)
        mm = vol.mnt
        t_read = ml.ts(time.time())
        with open(mm + "/fa_%s" % tag, "rb") as f:
            f.read()
        os.listdir(mm + "/da_%s" % tag)
        os.stat(mm + "/fa_%s" % tag)
        time.sleep(1.1)
        live_f = ml.view(mm + "/fa_%s" % tag)
        live_d = ml.view(mm + "/da_%s" % tag)
        fsops.unmount(ctx, vol)
        e = ml.disk_entries(vol.img)
        df = ml.si_ns(e["fa_%s" % tag][1])
        dd = ml.si_ns(e["da_%s" % tag][1])
        upd_f = df["atime"] != OLD
        upd_d = dd["atime"] != OLD
        ctx.log("%s (-o %s): file atime live %s, disk %s (updated: %s); dir atime live %s, disk %s (updated: %s); file mtime disk %s" % (
            tag, o or "-", ml.fmt_ns(live_f["atime"]), ml.fmt_ns(df["atime"]), upd_f,
            ml.fmt_ns(live_d["atime"]), ml.fmt_ns(dd["atime"]), upd_d, ml.fmt_ns(df["mtime"])))
        res[tag] = {"file_updated": upd_f, "dir_updated": upd_d, "file_atime_disk": df["atime"],
                    "file_atime_live": live_f["atime"]}
        if upd_f and not t_read - 2 * 10 ** 9 <= df["atime"] <= t_read + 10 * 10 ** 9:
            bad.append("%s: atime on disk %s is not the read time" % (tag, ml.fmt_ns(df["atime"])))
        if tag in ATIME_UPDATES[ctx.env] and upd_f != ATIME_UPDATES[ctx.env][tag]:
            bad.append("%s: file atime updated on disk = %s, expected %s" % (tag, upd_f, ATIME_UPDATES[ctx.env][tag]))
        if df["mtime"] != OLD:
            bad.append("%s: a read changed the mtime" % tag)
        # product options are noatime: nothing may move
        if tag == "product" and (upd_f or upd_d):
            bad.append("product options (noatime) changed an atime on disk")
        # relation of the live view to the disk
        if live_f["atime"] != df["atime"]:
            ctx.log("%s: the live atime differs from the disk (live %s)" % (tag, ml.fmt_ns(live_f["atime"])))
        if upd_f and (live_f["atime"] == df["atime"]) != ATIME_LIVE_SHOWN[ctx.env]:
            bad.append("%s: atime update shown by stat = %s, expected %s" % (
                tag, live_f["atime"] == df["atime"], ATIME_LIVE_SHOWN[ctx.env]))
    ml.dump_json(ctx, "observations.json", res)
    fsops.fsck(ctx, vol)
    need(ctx, bad, "I-META-03")


# ---------------------------------------------------------------- I-META-04

CR_REQ = 1234567890 * 10 ** 9 + 123456789
CR_XATTR = ml.ymd(2012, 12, 12, 12, 12, 12) * 10 ** 9 + 345678900


@case("I-META-04", envs=("A", "B"), title="creation time: setattrlist, system.ntfs_times, cp -p from APFS")
def i_meta_04(ctx):
    vol = new_volume(ctx)
    fsops.mount(ctx, vol)
    m = vol.mnt
    bad = []
    for n in ("c_attrlist", "c_xattr", "c_cp", "c_ref"):
        ctx.sh("echo x > %s/%s" % (m, n))
    obs = {}
    if ml.IS_MAC:
        try:
            ml.setattrlist_crtime(m + "/c_attrlist", CR_REQ)
            r = "ok"
        except OSError as e:
            r = "errno %d" % e.errno
        ctx.log("setattrlist(ATTR_CMN_CRTIME) on c_attrlist: %s" % r)
        obs["setattrlist"] = r
        try:
            ctx.log("getattrlist after: %s" % ml.getattrlist_times(m + "/c_attrlist"))
        except OSError as e:
            ctx.log("getattrlist failed: %s" % e)
        # a source on APFS with a creation time in the past, copied with cp -p
        src = os.path.join(ctx.work, "apfs-src")
        with open(src, "wb") as f:
            f.write(b"x")
        ml.setattrlist_crtime(src, CR_REQ)
        ctx.log("APFS source birth time: %s" % ml.fmt_ns(ml.view(src)["birth"]))
        ctx.sh("cp -p %s %s/c_cp" % (src, m))
    got = ml.xattr_get(m + "/c_xattr", "system.ntfs_times")
    if not isinstance(got, bytes) or len(got) != 32:
        bad.append("system.ntfs_times read gave %r" % (got,))
        got = b"\0" * 32
    ft = struct.unpack("<QQQQ", got)
    ctx.log("system.ntfs_times of a new file: creation %s mtime %s ctime %s atime %s" % tuple(
        ml.fmt_ns(ml.ft_to_ns(x)) for x in ft))
    new = struct.pack("<QQQQ", ml.ns_to_ft(CR_XATTR), ft[1], ft[2], ft[3])
    r = ml.xattr_set(m + "/c_xattr", "system.ntfs_times", new)
    ctx.log("setxattr system.ntfs_times (creation time only changed): %s" % r)
    obs["xattr_set"] = r
    if r != "ok":
        bad.append("setting system.ntfs_times failed: %s" % r)
    live_view = ml.view(m + "/c_xattr")
    ctx.log("live view after xattr set: %s" % ml.fmt_view(live_view))
    names = ["c_attrlist", "c_xattr", "c_cp", "c_ref"]
    ents, after = ml.cycle(ctx, vol, names)
    # read the creation time through the xattr in a read-only session
    fsops.mount(ctx, vol, rw=False)
    try:
        times_after = {}
        for n in names:
            g = ml.xattr_get(vol.mnt + "/" + n, "system.ntfs_times")
            times_after[n] = struct.unpack("<QQQQ", g)[0] if isinstance(g, bytes) and len(g) == 32 else g
    finally:
        fsops.unmount(ctx, vol)
    for n in names:
        rn, si, fn = ml.disk(ents, n)
        ctx.log("%s: SI creation %s (FILETIME %d), FN creation %s, view birth %s, xattr creation %s" % (
            n, ml.fmt_ns(ml.ft_to_ns(si["crtime"])), si["crtime"], ml.fmt_ns(ml.ft_to_ns(fn["crtime"])),
            ml.fmt_ns(after[n]["birth"]) if after[n]["birth"] is not None else "-", times_after[n]))
        obs[n] = {"si_crtime_ft": si["crtime"], "fn_crtime_ft": fn["crtime"], "xattr_crtime_ft": times_after[n],
                  "birth": after[n]["birth"]}
        if after[n]["birth"] != BIRTH_SHOWN[ctx.env]:
            bad.append("%s birth time shown %s, expected %s" % (n, after[n]["birth"], BIRTH_SHOWN[ctx.env]))
        if times_after[n] != si["crtime"]:
            bad.append("%s: xattr creation %s differs from the disk %s" % (n, times_after[n], si["crtime"]))
    # xattr route: stored exactly, to 100 ns
    if ents["c_xattr"][1]["crtime"] != ml.ns_to_ft(CR_XATTR):
        bad.append("c_xattr: SI creation FILETIME %d, asked %d" % (ents["c_xattr"][1]["crtime"], ml.ns_to_ft(CR_XATTR)))
    if ml.IS_MAC:
        # setattrlist route: the backend stores tv_sec as raw FILETIME ticks (nsec dropped)
        got = ents["c_attrlist"][1]["crtime"]
        want = CR_REQ // 10 ** 9
        ctx.log("setattrlist route: asked %s, stored FILETIME %d (%s)" % (ml.fmt_ns(CR_REQ), got, ml.fmt_ns(ml.ft_to_ns(got))))
        if SI_CRTIME_IS_RAW_SECONDS[ctx.env]:
            if got != want:
                bad.append("c_attrlist: SI creation FILETIME %d, expected the raw seconds %d" % (got, want))
        # cp -p does not carry the APFS birth time: the file has the create-time garbage of every new file
        gotc = ents["c_cp"][1]["crtime"]
        ctx.log("cp -p route: stored FILETIME %d (%s), the APFS birth time was not carried" % (gotc, ml.fmt_ns(ml.ft_to_ns(gotc))))
        now_s = int(time.time())
        if not now_s - 600 <= gotc <= now_s + 5:
            bad.append("c_cp: SI creation FILETIME %d is not the create time in seconds" % gotc)
    ml.dump_json(ctx, "observations.json", obs)
    fsops.fsck(ctx, vol)
    need(ctx, bad, "I-META-04")


# ---------------------------------------------------------------- I-META-05

def changed(before, after, mark):
    """'m+ c+ a-' style: which of mtime, ctime, atime moved (ctime relative to the mark)."""
    out = []
    out.append("m+" if after["mtime"] != before["mtime"] else "m-")
    out.append("c+" if after["ctime"] >= mark else "c-")
    out.append("a+" if after["atime"] != before["atime"] else "a-")
    return " ".join(out)


# Expected (mtime, ctime, atime) moves of the touched objects. Values are 'm+ c+ a-' strings.
# Key: operation:object. Set from the measurements; `?` is not asserted.
OP_EXPECT = {
    "A": {
        "write": "m+ c+ a-", "truncate": "m+ c+ a-",
        "rename (same dir): file": "m- c+ a-", "move to dB: file": "m- c+ a-",
        "link: file": "m- c+ a-",
        "chmod: file": "m- c- a-", "read: file": "m- c- a+", "utimens same value: file": "m- c+ a-",
        "dA after rename, unlink, link": "m+ c+ a-", "dB after move, create, mkdir": "m+ c+ a-",
    },
    "B": {
        "write": "m+ c+ a-", "truncate": "m+ c+ a-",
        "rename (same dir): file": "m- c+ a-", "move to dB: file": "m- c+ a-",
        "link: file": "m- c- a-",          # link() gives EIO but creates the name; ctime moves on disk, the live view is stale
        "chmod: file": "m- c- a-", "read: file": "m- c- a-", "utimens same value: file": "m- c+ a-",
        "dA after rename, unlink, link": "m+ c+ a-", "dB after move, create, mkdir": "m+ c+ a-",
    },
}


# Environment B: link() fails with EIO (also seen by I-XATTR-07).
LINK = {"A": "ok", "B": "EIO"}


@case("I-META-05", envs=("A", "B"), title="mtime, ctime, atime effects of write, truncate, rename, unlink, link, chmod, read")
def i_meta_05(ctx):
    vol = new_volume(ctx)
    fsops.mount(ctx, vol)
    m = vol.mnt
    for d in ("dA", "dB"):
        ctx.sh("mkdir %s/%s" % (m, d))
    files = ["f_w", "f_t", "f_r", "f_m", "f_u", "f_l", "f_c", "f_rd", "f_o"]
    for n in files:
        ctx.sh("echo content > %s/dA/%s" % (m, n))
    objs = ["dA", "dB"] + ["dA/" + n for n in files]
    for n in objs:
        ml.utimens(m + "/" + n, OLD, OLD)
    mark = ml.ts(time.time())
    time.sleep(1.2)
    mark += 10 ** 9
    before = ml.snap(m, objs)
    with open(m + "/dA/f_w", "ab") as f:
        f.write(b"more")
    os.truncate(m + "/dA/f_t", 3)
    os.rename(m + "/dA/f_r", m + "/dA/f_r2")
    os.rename(m + "/dA/f_m", m + "/dB/f_m")
    os.unlink(m + "/dA/f_u")
    link = ml.outcome(os.link, m + "/dA/f_l", m + "/dA/f_l2")
    ctx.log("link(): %s" % link)
    os.chmod(m + "/dA/f_c", 0o600)
    with open(m + "/dA/f_rd", "rb") as f:
        f.read()
    with open(m + "/dB/created", "wb") as f:
        f.write(b"x")
    os.mkdir(m + "/dB/subdir")
    os.utime(m + "/dA/f_o", ns=(OLD, OLD))      # same value again: ctime still moves
    after_names = ["dA", "dB", "dA/f_w", "dA/f_t", "dA/f_r2", "dB/f_m", "dA/f_l", "dA/f_c",
                   "dA/f_rd", "dA/f_o", "dB/created", "dB/subdir"]
    after_names.append("dA/f_l2")
    if link != LINK[ctx.env]:
        raise Fail("link() gave %s, expected %s" % (link, LINK[ctx.env]))
    live = ml.snap(m, after_names)
    moves = {}
    rows = [("write", "dA/f_w", "dA/f_w"), ("truncate", "dA/f_t", "dA/f_t"),
            ("rename (same dir): file", "dA/f_r", "dA/f_r2"), ("move to dB: file", "dA/f_m", "dB/f_m"),
            ("link: file", "dA/f_l", "dA/f_l"), ("chmod: file", "dA/f_c", "dA/f_c"),
            ("read: file", "dA/f_rd", "dA/f_rd"), ("utimens same value: file", "dA/f_o", "dA/f_o"),
            ("dA after rename, unlink, link", "dA", "dA"), ("dB after move, create, mkdir", "dB", "dB")]
    for label, b, a in rows:
        moves[label] = changed(before[b], live[a], mark)
        ctx.log("%-34s %s" % (label, moves[label]))
    bad = []
    # directories must show the namespace changes; a read must not move mtime or ctime
    exp = OP_EXPECT[ctx.env]
    for label, got in moves.items():
        if label in exp and exp[label] != got:
            bad.append("%s: moves %s, expected %s" % (label, got, exp[label]))
    ents, aft = ml.cycle(ctx, vol, after_names)
    ctx.log("names on disk: %s" % sorted(ents))
    # B: link() reports EIO, but the second name is created on disk and shows after a remount
    if "f_l2" not in ents or ents["f_l2"][0] != ents["f_l"][0]:
        bad.append("link(): the second name f_l2 is not a name of the same record on disk (link() gave %s)" % link)
    if aft["dA/f_l2"] is None:
        bad.append("link(): f_l2 is not visible after the remount")
    ctx.log("link(): f_l2 live view %s, after remount %s" % ("present" if live["dA/f_l2"] else "absent",
                                                           "present" if aft["dA/f_l2"] else "absent"))
    bad += ml.view_vs_disk(ents, aft)
    for n in after_names:
        d = ml.si_ns(ents[os.path.basename(n)][1])
        if live[n] and any(live[n][k] != d[k] for k in ("mtime", "ctime", "atime")):
            ctx.log("live view of %s differs from disk: %s vs %s" % (n, ml.fmt_view(live[n]), ml.fmt_si(ents[os.path.basename(n)][1])))
    # NTFS: hard links share the record, so f_l and f_l2 show the same times
    if aft["dA/f_l"] and aft["dA/f_l2"]:
        for k in ("mtime", "ctime", "atime"):
            if aft["dA/f_l"][k] != aft["dA/f_l2"][k]:
                bad.append("hard links f_l and f_l2 differ in %s" % k)
    ml.dump_json(ctx, "observations.json", moves)
    fsops.fsck(ctx, vol)
    need(ctx, bad, "I-META-05")


# ---------------------------------------------------------------- I-META-06 and I-META-07 helpers

UF_IMMUTABLE, UF_HIDDEN = 0x2, 0x8000


def set_attrib(path, flags):
    return ml.xattr_set(path, "system.ntfs_attrib", struct.pack("<I", flags))


def get_attrib(path):
    g = ml.xattr_get(path, "system.ntfs_attrib")
    return struct.unpack("<I", g)[0] if isinstance(g, bytes) and len(g) == 4 else g


def chflags(ctx, flag, path):
    rc, out = ml.run(["/usr/bin/chflags", flag, path])
    ctx.log("chflags %s %s -> rc %d %s" % (flag, os.path.basename(path), rc, out))
    return "ok" if rc == 0 else "rc%d" % rc


def mode_flags(path):
    st = os.lstat(path)
    return "%04o flags=%s" % (st.st_mode & 0o7777, getattr(st, "st_flags", "-"))


def probe_ops(m, name):
    """What a write, truncate, rename (and back) and chmod do to a file."""
    p = m + "/" + name
    out = {}

    def append():
        with open(p, "ab") as f:
            f.write(b"x")

    out["append"] = ml.outcome(append)
    out["truncate"] = ml.outcome(os.truncate, p, 0)
    out["rename"] = ml.outcome(os.rename, p, p + ".mv")
    if out["rename"] == "ok":
        os.rename(p + ".mv", p)
    out["chmod"] = ml.outcome(os.chmod, p, 0o600)
    return out


def attrs_on_disk(ents, name):
    return ents[name][1]["flags"]


# ---------------------------------------------------------------- I-META-06

# Is a stored FILE_ATTRIBUTE_READONLY enforced (append, truncate, rename, unlink refused)?
# Environment B: no, every operation succeeds (modes are display values, I-PERM-05).
RO_ENFORCED = {"A": False, "B": False}
# Is the root record $MFT (record 0) listed with show_sys_files? Environment B drops the entry
# (d_ino 0 reads as a deleted entry).
MFT_LISTED = {"A": True, "B": False}


@case("I-META-06", envs=("A", "B"), title="read-only attribute: FILE_ATTRIBUTE_READONLY against chflags uchg and chmod")
def i_meta_06(ctx):
    vol = new_volume(ctx)
    fsops.mount(ctx, vol, extra=product(ctx))
    m = vol.mnt
    kinds = ["plain", "xattr_ro", "chmod444"] + (["uchg"] if ml.IS_MAC else [])
    obs = {"live": {}, "after": {}}
    for k in kinds:
        ctx.sh("echo data > %s/k_%s; echo data > %s/k_%s_del" % (m, k, m, k))
    ctx.sh("mkdir %s/dk_xattr_ro; mkdir %s/dk_uchg" % (m, m))
    obs["set"] = {}
    obs["set"]["xattr_ro"] = set_attrib(m + "/k_xattr_ro", 0x21)
    obs["set"]["chmod444"] = ml.outcome(os.chmod, m + "/k_chmod444", 0o444)
    obs["set"]["dir_xattr_ro"] = set_attrib(m + "/dk_xattr_ro", 0x21)
    if ml.IS_MAC:
        obs["set"]["uchg"] = chflags(ctx, "uchg", m + "/k_uchg")
        obs["set"]["dir_uchg"] = chflags(ctx, "uchg", m + "/dk_uchg")
    ctx.log("set results: %s" % obs["set"])
    for n in ["k_" + k for k in kinds] + ["dk_xattr_ro", "dk_uchg"]:
        ctx.log("live %s: %s" % (n, mode_flags(m + "/" + n)))
    # the _del twins carry the same attribute for the unlink test
    set_attrib(m + "/k_xattr_ro_del", 0x21)
    os.chmod(m + "/k_chmod444_del", 0o444)
    if ml.IS_MAC:
        chflags(ctx, "uchg", m + "/k_uchg_del")
    for k in kinds:
        obs["live"][k] = probe_ops(m, "k_" + k)
        obs["live"][k]["unlink"] = ml.outcome(os.unlink, m + "/k_%s_del" % k)
        ctx.log("live ops on %s: %s" % (k, obs["live"][k]))
    obs["live"]["dir_xattr_ro"] = {"create_inside": ml.outcome(os.mkdir, m + "/dk_xattr_ro/sub"),
                                   "rmdir_self": "-"}
    obs["live"]["dir_uchg"] = {"create_inside": ml.outcome(os.mkdir, m + "/dk_uchg/sub")} if ml.IS_MAC else {}
    ctx.log("live ops on directories: %s" % ({k: v for k, v in obs["live"].items() if k.startswith("dir_")},))
    if ml.IS_MAC:
        chflags(ctx, "nouchg", m + "/k_uchg")
        chflags(ctx, "nouchg", m + "/dk_uchg")
    names = ["k_" + k for k in kinds] + ["dk_xattr_ro", "dk_uchg"]
    bad = []
    for k in ("xattr_ro", "chmod444", "uchg"):
        if k in obs["live"] and not RO_ENFORCED[ctx.env]:
            refused = dict((op, r) for op, r in obs["live"][k].items() if r != "ok")
            if refused:
                bad.append("%s: operations refused %s, expected none (no enforcement)" % (k, refused))
    fsops.unmount(ctx, vol)
    ents = ml.disk_entries(vol.img)
    obs["disk"] = {}
    for n in names:
        fl = attrs_on_disk(ents, n)
        obs["disk"][n] = fl
        ctx.log("disk %s: flags 0x%x%s" % (n, fl, " READONLY" if fl & ml.FA_READONLY else ""))
    if not obs["disk"]["k_xattr_ro"] & ml.FA_READONLY:
        bad.append("system.ntfs_attrib 0x21 did not reach the disk")
    if obs["set"]["xattr_ro"] != "ok":
        bad.append("system.ntfs_attrib set gave %s" % obs["set"]["xattr_ro"])
    for n in ("k_plain", "k_chmod444", "k_uchg", "dk_uchg"):
        if n in obs["disk"] and obs["disk"][n] & ml.FA_READONLY:
            bad.append("%s: READONLY reached the disk" % n)
    # second session: the stored attribute against what the OS shows and allows
    fsops.mount(ctx, vol, extra=product(ctx))
    try:
        for n in names:
            ctx.log("second session %s: %s attrib=%s" % (n, mode_flags(m + "/" + n) if os.path.lexists(m + "/" + n) else "gone",
                                                       get_attrib(m + "/" + n) if os.path.lexists(m + "/" + n) else "-"))
        ctx.sh("/bin/ls -lO %s" % m if ml.IS_MAC else "ls -l %s" % m, check=False)
        shown = {}
        for n in names:
            if os.path.lexists(m + "/" + n):
                st = os.lstat(m + "/" + n)
                shown[n] = (st.st_mode & 0o7777, getattr(st, "st_flags", None))
        obs["after"]["shown"] = shown
        for k in kinds:
            if os.path.lexists(m + "/k_" + k):
                obs["after"][k] = probe_ops(m, "k_" + k)
        ctx.log("second session ops: %s" % obs["after"])
    finally:
        fsops.unmount(ctx, vol)
    ml.dump_json(ctx, "observations.json", obs)
    ents2 = ml.disk_entries(vol.img)
    ctx.log("flags after the second session: %s" % dict((n, hex(attrs_on_disk(ents2, n))) for n in names if n in ents2))
    for n in names:
        if n in ents2 and attrs_on_disk(ents2, n) != attrs_on_disk(ents, n):
            bad.append("%s: flags changed on disk in the second session" % n)
    # permissions opt-in: does chmod reach FILE_ATTRIBUTE_READONLY?
    ml.mount_raw(ctx, vol, "permissions")
    try:
        ctx.sh("echo data > %s/p_file; echo data > %s/p_back" % (m, m))
        r1 = ml.outcome(os.chmod, m + "/p_file", 0o444)
        r2 = ml.outcome(os.chmod, m + "/p_back", 0o444)
        r3 = ml.outcome(os.chmod, m + "/p_back", 0o644)
        ctx.log("permissions mount: chmod 0444 %s, chmod 0444 then 0644 %s/%s; modes now %s / %s" % (
            r1, r2, r3, mode_flags(m + "/p_file"), mode_flags(m + "/p_back")))
        obs["permissions"] = {"chmod444": r1, "back": [r2, r3]}
        ctx.sh("echo data > %s/p_ops" % m)
        os.chmod(m + "/p_ops", 0o444)
        obs["permissions"]["ops_on_0444"] = probe_ops(m, "p_ops")
        ctx.log("permissions mount, operations on the 0444 file: %s" % obs["permissions"]["ops_on_0444"])
    finally:
        fsops.unmount(ctx, vol)
    ents3 = ml.disk_entries(vol.img)
    for n in ("p_file", "p_back"):
        ctx.log("permissions mount, disk %s: flags 0x%x" % (n, attrs_on_disk(ents3, n)))
    if not attrs_on_disk(ents3, "p_file") & ml.FA_READONLY:
        bad.append("permissions: chmod 0444 did not set READONLY on disk")
    if attrs_on_disk(ents3, "p_back") & ml.FA_READONLY:
        bad.append("permissions: chmod 0644 did not clear READONLY on disk")
    obs["permissions"]["disk"] = dict((n, attrs_on_disk(ents3, n)) for n in ("p_file", "p_back"))
    ml.dump_json(ctx, "observations.json", obs)
    fsops.fsck(ctx, vol)
    need(ctx, bad, "I-META-06")


# ---------------------------------------------------------------- I-META-07

@case("I-META-07", envs=("A", "B"), title="hidden flag: FILE_ATTRIBUTE_HIDDEN against chflags hidden, dot-files and the hide_* options")
def i_meta_07(ctx):
    vol = new_volume(ctx)
    fsops.mount(ctx, vol)
    m = vol.mnt
    obs = {}
    for n in ("h_chflags", ".dotfile", "h_attr", "s_attr", "plain"):
        ctx.sh("echo data > %s/%s" % (m, n))
    ctx.sh("mkdir %s/.dotdir; mkdir %s/h_dir" % (m, m))
    if ml.IS_MAC:
        obs["chflags_hidden"] = chflags(ctx, "hidden", m + "/h_chflags")
        obs["chflags_hidden_dir"] = chflags(ctx, "hidden", m + "/h_dir")
        ctx.log("live h_chflags: %s" % mode_flags(m + "/h_chflags"))
    obs["set_hidden"] = set_attrib(m + "/h_attr", 0x22)
    set_attrib(m + "/s_attr", 0x24)
    set_attrib(m + "/h_dir", 0x22) if not ml.IS_MAC else None
    names = ["h_chflags", ".dotfile", "h_attr", "s_attr", "plain", ".dotdir", "h_dir"]
    live_list = listing(m)
    ctx.log("live listing, default mount: %s" % live_list)
    fsops.unmount(ctx, vol)
    ents = ml.disk_entries(vol.img)
    obs["disk_default"] = dict((n, attrs_on_disk(ents, n)) for n in names)
    for n in names:
        ctx.log("disk %s: flags 0x%x" % (n, obs["disk_default"][n]))
    bad = []
    if ml.IS_MAC and obs["chflags_hidden"] == "ok" and obs["disk_default"]["h_chflags"] & ml.FA_HIDDEN:
        bad.append("chflags hidden now reaches FILE_ATTRIBUTE_HIDDEN (expected it to be lost)")
    if obs["disk_default"][".dotfile"] & ml.FA_HIDDEN or obs["disk_default"][".dotdir"] & ml.FA_HIDDEN:
        bad.append("a dot-name got FILE_ATTRIBUTE_HIDDEN without hide_dot_files")
    if not obs["disk_default"]["h_attr"] & ml.FA_HIDDEN:
        bad.append("system.ntfs_attrib 0x22 did not reach the disk")
    if not obs["disk_default"]["s_attr"] & ml.FA_SYSTEM:
        bad.append("system.ntfs_attrib 0x24 did not reach the disk")
    # what a remounted default mount shows for entries that carry HIDDEN or SYSTEM
    fsops.mount(ctx, vol, rw=False)
    try:
        obs["listing_default"] = listing(m)
        obs["shown_default"] = dict((n, mode_flags(m + "/" + n)) for n in names if os.path.lexists(m + "/" + n))
        ctx.log("remount, default: listing %s" % obs["listing_default"])
        ctx.log("remount, default: mode and flags %s" % obs["shown_default"])
        ctx.sh("/bin/ls -lOa %s" % m if ml.IS_MAC else "ls -la %s" % m, check=False)
        if ml.IS_MAC:
            for n in names:
                st = os.lstat(m + "/" + n)
                if st.st_flags & UF_HIDDEN:
                    bad.append("%s shows UF_HIDDEN (expected the flag is not mapped)" % n)
    finally:
        fsops.unmount(ctx, vol)
    for n in names:
        if n not in obs["listing_default"]:
            bad.append("%s is not listed on a default mount" % n)
    # hide_dot_files: new dot-names get FILE_ATTRIBUTE_HIDDEN
    fsops.mount(ctx, vol, extra="hide_dot_files")
    ctx.sh("echo data > %s/.newdot; mkdir %s/.newdotdir; echo data > %s/vis" % (m, m, m))
    obs["listing_hide_dot_files"] = listing(m)
    ctx.log("hide_dot_files: listing %s" % obs["listing_hide_dot_files"])
    fsops.unmount(ctx, vol)
    ents = ml.disk_entries(vol.img)
    for n in (".newdot", ".newdotdir", "vis"):
        obs["disk_hide_dot_files." + n] = attrs_on_disk(ents, n)
        ctx.log("hide_dot_files, disk %s: flags 0x%x" % (n, attrs_on_disk(ents, n)))
    if not (attrs_on_disk(ents, ".newdot") & ml.FA_HIDDEN and attrs_on_disk(ents, ".newdotdir") & ml.FA_HIDDEN):
        bad.append("hide_dot_files did not set HIDDEN on new dot-names")
    if attrs_on_disk(ents, "vis") & ml.FA_HIDDEN:
        bad.append("hide_dot_files set HIDDEN on a name without a dot")
    hidden_names = sorted(n for n in ents if n in names + [".newdot", ".newdotdir", "vis"] and attrs_on_disk(ents, n) & ml.FA_HIDDEN)
    ctx.log("names with HIDDEN on disk: %s" % hidden_names)
    # hide_hid_files: names with HIDDEN are not listed (and not found)
    fsops.mount(ctx, vol, rw=False, extra="hide_hid_files")
    try:
        obs["listing_hide_hid_files"] = listing(m)
        ctx.log("hide_hid_files: listing %s" % obs["listing_hide_hid_files"])
        direct = {}
        for n in hidden_names:
            direct[n] = ml.outcome(os.stat, m + "/" + n)
        obs["direct_stat_hidden"] = direct
        ctx.log("hide_hid_files: stat of each hidden name: %s" % direct)
        ctx.sh("/bin/ls -lOa %s" % m if ml.IS_MAC else "ls -la %s" % m, check=False)
    finally:
        fsops.unmount(ctx, vol)
    for n in hidden_names:
        if n in obs["listing_hide_hid_files"]:
            bad.append("hide_hid_files lists the hidden name %s" % n)
    for n in ("plain", "vis", "s_attr", ".dotfile"):
        if n not in obs["listing_hide_hid_files"]:
            bad.append("hide_hid_files hides %s, which has no HIDDEN" % n)
    # show_sys_files: the NTFS system files show in the root
    fsops.mount(ctx, vol, rw=False, extra="show_sys_files")
    try:
        obs["listing_show_sys_files"] = listing(m)
        ctx.log("show_sys_files: listing %s" % obs["listing_show_sys_files"])
    finally:
        fsops.unmount(ctx, vol)
    if ("$MFT" in obs["listing_show_sys_files"]) != MFT_LISTED[ctx.env]:
        bad.append("show_sys_files: $MFT listed = %s, expected %s" % ("$MFT" in obs["listing_show_sys_files"], MFT_LISTED[ctx.env]))
    if "$AttrDef" not in obs["listing_show_sys_files"]:
        bad.append("show_sys_files does not list $AttrDef")
    if "$MFT" in obs["listing_default"]:
        bad.append("a default mount lists $MFT")
    ml.dump_json(ctx, "observations.json", obs)
    fsops.fsck(ctx, vol)
    need(ctx, bad, "I-META-07")
