# SPDX-License-Identifier: GPL-2.0-or-later
"""I-PERM-01..07 (T-M3-20, IMPLEMENTATION_PLAN 3.16, ADR-012, limitations N9 and N10).

I-PERM-01  owner and group of every entry (touch, mkdir, cp, and entries from an earlier session)
I-PERM-02  modes 0644 and 0755 from fmask/dmask, also after remounts, no change on disk
I-PERM-03  chmod outcomes on a file and a directory, three runs, no NTFS change
I-PERM-04  chown outcomes (console user and root), no NTFS change
I-PERM-05  execute bit: script and native binary, read-write and read-only, directories (N9)
I-PERM-06  opt-in `permissions` and `usermapping=` smoke (ADR-012)
I-PERM-07  a second local account on a mount made by the console user (N10), Environment B only

The cases record what the backend does and assert it, so a change of behaviour (a new macFUSE, a
different NTFS-3G) fails the case and has to be looked at. Environment B is the product; Environment A
(Linux, kernel FUSE) is the reference, where the kernel does enforce modes. Expectations that differ
between the two are named constants below. Python 3.9, standard library only.
"""

import errno
import hashlib
import os
import shlex
import shutil
import stat
import subprocess
import sys

sys.path.insert(0, os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "functional"))

import fsops  # noqa: E402
from fsops import blob, put, sha, slurp  # noqa: E402
from harness import Fail, case, volume as hv  # noqa: E402

MIB = 1024 * 1024

# Product options of ADR-012 beyond what fsops.mount adds in B (backend, local, norecover, uid, gid).
PRODUCT_EXTRA_B = "noatime,windows_names,streams_interface=openxattr,fmask=0133,dmask=0022"
MAPPED_FILE, MAPPED_DIR = 0o644, 0o755
SID_BASE = "S-1-5-21-3141592653-589793238-462843383"

# Environment B: the execute bit is not checked, so a file with mode 0644 runs (N9 premise refuted).
# Environment A: the kernel checks it, so the same file gives EACCES.
EXEC_RUNS = {"A": False, "B": True}


# ---------------------------------------------------------------- helpers

def mount_product(ctx, vol, rw=True):
    """The product mount line: uid/gid of the console user, fmask=0133, dmask=0022."""
    if ctx.env == "B":
        fsops.mount(ctx, vol, rw=rw, extra=PRODUCT_EXTRA_B)
    else:
        fsops.mount(ctx, vol, rw=rw, extra="noatime,uid=%d,gid=%d,fmask=0133,dmask=0022" % (os.getuid(), os.getgid()))


def mount_opts(ctx, vol, opts, rw=True):
    """Mount with exactly `opts` after the mandatory ones: no uid and gid are added (the `permissions`
    opt-in works only without uid, gid, fmask, dmask, T-M2-04). Deadline and SIGKILL as fsops.mount."""
    fsops._check_extra(opts)
    if ctx.env == "B":
        o = "backend=fskit,local,norecover,%s%s" % (opts, "" if rw else ",ro")
        r = ctx.sh("%s -o %s %s %s" % (shlex.quote(hv.guest_tool("ntfs-3g")), o, shlex.quote(vol.img),
                                        shlex.quote(vol.mnt)), timeout=60)
        fsops._wait_b_mount(ctx, vol)
    else:
        r = ctx.sh("ntfs-3g %s %s -o %s,%s" % (shlex.quote(vol.img), shlex.quote(vol.mnt),
                                               "rw" if rw else "ro", opts), timeout=60)
        vol.mounted = True
    return (r.out + r.err).decode("utf-8", "replace")


def outcome(fn, *args):
    try:
        fn(*args)
        return "ok"
    except OSError as e:
        return errno.errorcode.get(e.errno, str(e.errno))


def perm_of(path):
    st = os.lstat(path)
    return st.st_uid, st.st_gid, stat.S_IMODE(st.st_mode)


def fmt(t):
    return "%d:%d:%04o" % t


def walk_entries(root):
    """Every entry below root (and root itself) as {rel: (uid, gid, mode)}."""
    out = {".": perm_of(root)}
    for dp, dn, fn in os.walk(root):
        for n in dn + fn:
            p = os.path.join(dp, n)
            out[os.path.relpath(p, root)] = perm_of(p)
    return out


def sudo_prefix():
    return [] if (os.getuid() == 0) else ["/usr/bin/sudo", "-n"]


def as_root(argv):
    return subprocess.run(sudo_prefix() + list(argv), stdin=subprocess.DEVNULL, stdout=subprocess.PIPE,
                          stderr=subprocess.PIPE, timeout=60)


def root_chown(path, uid, gid):
    code = ("import os,sys,errno\n"
            "try:\n os.chown(sys.argv[1], int(sys.argv[2]), int(sys.argv[3])); print('ok')\n"
            "except OSError as e:\n print(errno.errorcode.get(e.errno, e.errno))\n")
    r = as_root([sys.executable, "-c", code, path, str(uid), str(gid)])
    if r.returncode != 0:
        raise Fail("root helper failed: %s" % r.stderr.decode("utf-8", "replace")[:200])
    return r.stdout.decode().strip()


def ntfsinfo(ctx, vol, name):
    """ntfsinfo -v of a root-level entry on the unmounted image (attributes, security descriptor)."""
    if vol.mounted:
        raise RuntimeError("ntfsinfo needs the volume unmounted")
    tool = hv.guest_tool("ntfsinfo") if ctx.env == "B" else "ntfsinfo"
    r = ctx.transport.run("%s -F %s -v %s" % (shlex.quote(tool), shlex.quote(name), shlex.quote(vol.img)))
    text = (r.out + r.err).decode("utf-8", "replace")
    if r.rc != 0 or "Dumping" not in text:
        raise Fail("ntfsinfo -F %s failed (rc %d): %s" % (name, r.rc, text[-200:]))
    return text


def same_text(ctx, what, before, after):
    if before != after:
        b, a = before.splitlines(), after.splitlines()
        diff = [x for x in a if x not in b][:6] + [x for x in b if x not in a][:6]
        for d in diff:
            ctx.log("NTFS DIFF %s: %s" % (what, d.strip()))
        raise Fail("%s: ntfsinfo output changed on disk" % what)
    ctx.log("%s: ntfsinfo -v identical before and after (%d lines)" % (what, len(before.splitlines())))


def security_info(text):
    """'Security ID N' of $STANDARD_INFORMATION and the owner SID if the record holds the descriptor."""
    sid, owner = None, None
    for line in text.splitlines():
        if "Security ID" in line and sid is None:
            sid = line.split(":", 1)[1].strip()
        if "Owner SID" in line and owner is None:
            owner = line.split(":", 1)[1].strip()
    return "security id %s, owner SID %s" % (sid, owner)


def new_volume(ctx, mib=64):
    return fsops.open_volume(ctx, mib)


def system_binary():
    for p in ("/usr/bin/true", "/bin/true"):
        if os.path.exists(p):
            return p
    raise Fail("no /usr/bin/true or /bin/true to copy")


def run_exec(path):
    """Execute path directly. Returns ('ran', rc, output) or ('error', ERRNO, '')."""
    try:
        r = subprocess.run([path], stdin=subprocess.DEVNULL, stdout=subprocess.PIPE, stderr=subprocess.PIPE,
                           timeout=30)
        return "ran", r.returncode, r.stdout.decode("utf-8", "replace").strip()
    except OSError as e:
        return "error", errno.errorcode.get(e.errno, str(e.errno)), ""


# ---------------------------------------------------------------- I-PERM-01

@case("I-PERM-01", envs=("A", "B"), title="owner and group of every entry is the console user (touch, mkdir, cp, earlier session)")
def i_perm_01(ctx):
    vol = new_volume(ctx)
    uid, gid = os.getuid(), os.getgid()
    mount_product(ctx, vol)
    m = vol.mnt
    ctx.sh("touch %s" % shlex.quote(m + "/touched.txt"))
    ctx.sh("mkdir %s" % shlex.quote(m + "/dir1"))
    ctx.sh("mkdir -p %s" % shlex.quote(m + "/dir1/sub/deeper"))
    ctx.sh("cp /etc/hosts %s" % shlex.quote(m + "/copied.txt"))
    ctx.sh("cp -p /etc/hosts %s" % shlex.quote(m + "/dir1/copied-p.txt"))
    ctx.sh("cp -R %s %s" % (shlex.quote(m + "/dir1"), shlex.quote(m + "/dir1-copy")))
    put(m + "/dir1/sub/data.bin", blob("perm01", 100000))
    live = walk_entries(m)
    fsops.unmount(ctx, vol)
    # Second session: every entry now comes from disk, not from this session's cache.
    mount_product(ctx, vol)
    try:
        later = walk_entries(m)
        if ctx.env == "B":
            for who, argv in (("root", ["/usr/bin/stat", "-f", "%u:%g", m + "/copied.txt"]),):
                r = as_root(argv)
                ctx.log("view as %s: %s" % (who, r.stdout.decode().strip()))
    finally:
        fsops.unmount(ctx, vol)
    for label, ents in (("same session", live), ("after remount", later)):
        bad = [(k, fmt(v)) for k, v in sorted(ents.items()) if (v[0], v[1]) != (uid, gid)]
        for k, v in sorted(ents.items()):
            ctx.log("%s: %-24s %s" % (label, k, fmt(v)))
        if bad:
            raise Fail("%s: %d entries not %d:%d, first %s" % (label, len(bad), uid, gid, bad[0]))
    names = sorted(k for k in later if k != ".")
    ctx.log("%d entries checked in each of two sessions, all %d:%d" % (len(names) + 1, uid, gid))
    if "touched.txt" not in later or "dir1-copy/sub/deeper" not in later:
        raise Fail("expected entries missing after remount")


# ---------------------------------------------------------------- I-PERM-02

@case("I-PERM-02", envs=("A", "B"), title="modes 0644 for files and 0755 for directories from fmask/dmask, stable across remounts")
def i_perm_02(ctx):
    vol = new_volume(ctx)
    mount_product(ctx, vol)
    m = vol.mnt
    put(m + "/f1", blob("perm02:1", 5000))
    ctx.sh("touch %s" % shlex.quote(m + "/f2"))
    os.mkdir(m + "/d1")
    os.mkdir(m + "/d1/d2")
    ctx.sh("cp /etc/hosts %s" % shlex.quote(m + "/d1/f3"))
    fd = os.open(m + "/f4", os.O_CREAT | os.O_WRONLY, 0o777)  # a requested mode is not honoured
    os.close(fd)
    os.mkdir(m + "/d3", 0o700)
    fsops.unmount(ctx, vol)
    info0 = {n: ntfsinfo(ctx, vol, n) for n in ("f1", "d1", "f4", "d3")}
    for label, rw in (("remount rw", True), ("remount ro", False), ("remount rw again", True)):
        mount_product(ctx, vol, rw=rw)
        try:
            ents = walk_entries(m)
        finally:
            fsops.unmount(ctx, vol)
        for k, v in sorted(ents.items()):
            if k == ".fseventsd" or k.startswith(".fseventsd/"):
                ctx.log("%s: %-12s %s (macOS metadata, not asserted)" % (label, k, fmt(v)))
                continue
            isdir = k == "." or k in ("d1", "d1/d2", "d3")
            want = MAPPED_DIR if isdir else MAPPED_FILE
            ctx.log("%s: %-12s %s" % (label, k, fmt(v)))
            if v[2] != want:
                raise Fail("%s: %s mode %04o, expected %04o" % (label, k, v[2], want))
    for n, text in info0.items():
        same_text(ctx, n, text, ntfsinfo(ctx, vol, n))
        ctx.log("%s: %s" % (n, security_info(text)))
    fsops.fsck(ctx, vol)
    # Reference without masks (log only): what the engine shows by default.
    if ctx.env == "B":
        mount_opts(ctx, vol, "uid=%d,gid=%d" % (os.getuid(), os.getgid()))
        try:
            ctx.log("without fmask/dmask: root %s, f1 %s, d1 %s" % (
                fmt(perm_of(m)), fmt(perm_of(m + "/f1")), fmt(perm_of(m + "/d1"))))
        finally:
            fsops.unmount(ctx, vol)


# ---------------------------------------------------------------- I-PERM-03

CHMODS = (("0600", lambda cur: 0o600), ("0755", lambda cur: 0o755), ("+x", lambda cur: cur | 0o111),
          ("0777", lambda cur: 0o777), ("0000", lambda cur: 0))


@case("I-PERM-03", envs=("A", "B"), title="chmod on a file and a directory: same outcome on three runs, mode stays mapped, no NTFS change")
def i_perm_03(ctx):
    vol = new_volume(ctx)
    mount_product(ctx, vol)
    m = vol.mnt
    put(m + "/f03", blob("perm03", 3000))
    os.mkdir(m + "/d03")
    fsops.unmount(ctx, vol)
    before = {n: ntfsinfo(ctx, vol, n) for n in ("f03", "d03")}
    runs = []
    for run in (1, 2, 3):
        sig = []
        mount_product(ctx, vol)
        try:
            for name, want in (("f03", MAPPED_FILE), ("d03", MAPPED_DIR)):
                p = m + "/" + name
                for label, fn in CHMODS:
                    cur = stat.S_IMODE(os.lstat(p).st_mode)
                    out = outcome(os.chmod, p, fn(cur))
                    after = stat.S_IMODE(os.lstat(p).st_mode)
                    sig.append((name, label, out, "%04o" % after))
                    ctx.log("run %d: chmod %s %s -> %s, mode now %04o" % (run, label, name, out, after))
                    if after != want:
                        raise Fail("run %d: %s after chmod %s is %04o, expected the mapped %04o" % (
                            run, name, label, after, want))
        finally:
            fsops.unmount(ctx, vol)
        mount_product(ctx, vol, rw=False)
        try:
            for name, want in (("f03", MAPPED_FILE), ("d03", MAPPED_DIR)):
                mode = perm_of(m + "/" + name)[2]
                sig.append((name, "remount", "-", "%04o" % mode))
                ctx.log("run %d: after remount %s mode %04o" % (run, name, mode))
                if mode != want:
                    raise Fail("run %d: %s after remount is %04o, expected %04o" % (run, name, mode, want))
        finally:
            fsops.unmount(ctx, vol)
        runs.append(sig)
    if not (runs[0] == runs[1] == runs[2]):
        raise Fail("chmod outcomes differ between runs")
    outs = sorted(set(s[2] for s in runs[0] if s[1] != "remount"))
    ctx.log("chmod outcomes seen: %s; identical on three runs; mode never left the mapped value" % outs)
    for n, text in before.items():
        same_text(ctx, n, text, ntfsinfo(ctx, vol, n))
    fsops.fsck(ctx, vol)


# ---------------------------------------------------------------- I-PERM-04

@case("I-PERM-04", envs=("A", "B"), title="chown by the console user and by root: outcome recorded, ownership unchanged, no NTFS change")
def i_perm_04(ctx):
    vol = new_volume(ctx)
    uid, gid = os.getuid(), os.getgid()
    mount_product(ctx, vol)
    m = vol.mnt
    put(m + "/f04", blob("perm04", 3000))
    os.mkdir(m + "/d04")
    fsops.unmount(ctx, vol)
    before = {n: ntfsinfo(ctx, vol, n) for n in ("f04", "d04")}
    runs = []
    for run in (1, 2, 3):
        sig = []
        mount_product(ctx, vol)
        try:
            for name in ("f04", "d04"):
                p = m + "/" + name
                for who, to_u, to_g in (("user", uid + 1000, gid + 1000), ("user", 0, 0), ("user", uid, gid),
                                        ("root", 1234, 4321), ("root", 0, 0)):
                    if who == "user":
                        out = outcome(os.chown, p, to_u, to_g)
                    else:
                        out = root_chown(p, to_u, to_g)
                    now = perm_of(p)
                    sig.append((name, who, to_u, to_g, out, now[:2]))
                    ctx.log("run %d: %s chown %d:%d %s -> %s, now %d:%d" % (run, who, to_u, to_g, name, out, now[0], now[1]))
                    if now[:2] != (uid, gid):
                        raise Fail("run %d: %s owner is %d:%d after %s chown, expected %d:%d" % (
                            run, name, now[0], now[1], who, uid, gid))
        finally:
            fsops.unmount(ctx, vol)
        mount_product(ctx, vol, rw=False)
        try:
            for name in ("f04", "d04"):
                now = perm_of(m + "/" + name)[:2]
                ctx.log("run %d: after remount %s owner %d:%d" % (run, name, now[0], now[1]))
                if now != (uid, gid):
                    raise Fail("run %d: %s after remount owned by %d:%d" % (run, name, now[0], now[1]))
        finally:
            fsops.unmount(ctx, vol)
        runs.append(sig)
    if not (runs[0] == runs[1] == runs[2]):
        raise Fail("chown outcomes differ between runs")
    ctx.log("chown outcomes seen: %s" % sorted(set(s[4] for s in runs[0])))
    for n, text in before.items():
        same_text(ctx, n, text, ntfsinfo(ctx, vol, n))
    fsops.fsck(ctx, vol)


# ---------------------------------------------------------------- I-PERM-05

@case("I-PERM-05", envs=("A", "B"), title="execute bit (N9): script and native binary, read-write and read-only, directories traversable")
def i_perm_05(ctx):
    vol = new_volume(ctx)
    mount_product(ctx, vol)
    m = vol.mnt
    script = b"#!/bin/sh\necho script-ran\n"
    put(m + "/run.sh", script)
    shutil.copyfile(system_binary(), m + "/native")
    cc = shutil.which("cc")
    have_cc = cc is not None and ctx.env == "B"
    if have_cc:
        src = os.path.join(ctx.work, "hello.c")
        with open(src, "w") as f:
            f.write('#include <stdio.h>\nint main(void){puts("native-ran");return 0;}\n')
        r = ctx.sh("%s -o %s %s" % (shlex.quote(cc), shlex.quote(m + "/hello"), shlex.quote(src)), check=False)
        have_cc = r.rc == 0
    os.mkdir(m + "/dx")
    put(m + "/dx/inner.txt", b"inner\n")
    names = ["run.sh", "native"] + (["hello"] if have_cc else [])
    expect = EXEC_RUNS[ctx.env]

    def check(label):
        for n in names:
            p = m + "/" + n
            mode_before = stat.S_IMODE(os.lstat(p).st_mode)
            ch = "-" if label.startswith("ro") else outcome(os.chmod, p, mode_before | 0o111)
            mode_after = stat.S_IMODE(os.lstat(p).st_mode)
            res = run_exec(p)
            ctx.log("%s: %s mode %04o, chmod +x %s, mode %04o, exec -> %s" % (label, n, mode_before, ch, mode_after, res))
            if mode_after & 0o111:
                raise Fail("%s: %s has an execute bit (%04o), the mapping should have cleared it" % (label, n, mode_after))
            if expect:
                if res[0] != "ran" or res[1] != 0:
                    raise Fail("%s: %s did not run (%s); behaviour changed, N9 text needs review" % (label, n, res))
            elif res != ("error", "EACCES", ""):
                raise Fail("%s: %s: expected EACCES on this environment, got %s" % (label, n, res))
        # Directories stay traversable and writable by the owner.
        d = m + "/dx"
        if "inner.txt" not in os.listdir(d) or slurp(d + "/inner.txt") != b"inner\n":
            raise Fail("%s: directory not traversable" % label)
        cwd = os.getcwd()
        try:
            os.chdir(d)
        finally:
            os.chdir(cwd)
        if label.startswith("rw"):
            put(d + "/new-%s.txt" % label.split()[-1], b"x")

    try:
        check("rw session 1")
    finally:
        fsops.unmount(ctx, vol)
    for label, rw in (("rw session 2", True), ("ro session", False)):
        mount_product(ctx, vol, rw=rw)
        try:
            check(label)
        finally:
            fsops.unmount(ctx, vol)
    ctx.log("execute bit: %s on %s" % ("not checked, files without x run" if expect else "checked, EACCES", ctx.env))
    fsops.fsck(ctx, vol)


# ---------------------------------------------------------------- I-PERM-06

def _digest(path):
    return hashlib.sha256(slurp(path)).hexdigest()


def _smoke(ctx, vol, label, opts):
    """Mount with `opts`, write two files and a directory, hash, remount with the same options, hash."""
    m = vol.mnt
    big = blob("perm06:" + label, MIB + 4097)
    small = blob("perm06s:" + label, 777)
    said = mount_opts(ctx, vol, opts)
    if "Bad " in said or "Invalid" in said or "rror" in said:
        raise Fail("%s: the engine complained at mount: %s" % (label, said.strip()[:200]))
    ctx.log("%s: engine said at mount: %s" % (label, said.strip().replace("\n", " | ") or "(nothing)"))
    try:
        put(m + "/big.bin", big)
        put(m + "/small.bin", small)
        os.mkdir(m + "/dir")
        put(m + "/dir/in.bin", small)
        ctx.log("%s: created, ls: big %s small %s dir %s" % (
            label, fmt(perm_of(m + "/big.bin")), fmt(perm_of(m + "/small.bin")), fmt(perm_of(m + "/dir"))))
        ch = outcome(os.chmod, m + "/small.bin", 0o600)
        ctx.log("%s: chmod 0600 small.bin -> %s, mode now %04o (recorded, not asserted)" % (
            label, ch, perm_of(m + "/small.bin")[2]))
        if slurp(m + "/big.bin") != big or slurp(m + "/small.bin") != small:
            raise Fail("%s: read-back differs in the live mount" % label)
    finally:
        fsops.unmount(ctx, vol)
    mount_opts(ctx, vol, opts, rw=False)
    try:
        if _digest(m + "/big.bin") != sha(big) or _digest(m + "/small.bin") != sha(small) \
                or _digest(m + "/dir/in.bin") != sha(small):
            raise Fail("%s: hash differs after remount" % label)
        ctx.log("%s: hashes equal after remount; small.bin mode %04o" % (label, perm_of(m + "/small.bin")[2]))
    finally:
        fsops.unmount(ctx, vol)
    fsops.fsck(ctx, vol)
    return ntfsinfo(ctx, vol, "small.bin")


def _mapping_text():
    # An implicit pattern (empty uid and gid) needs a last component of 1000 or more (ntfs_valid_pattern).
    return "%d:%d:%s-1000\n::%s-10000\n" % (os.getuid(), os.getgid(), SID_BASE, SID_BASE)


@case("I-PERM-06", envs=("A", "B"), title="opt-in permissions and usermapping= smoke: mount succeeds, file written and read back with equal hash")
def i_perm_06(ctx):
    results = {}
    # 1. permissions alone (default mapping).
    vol = new_volume(ctx)
    results["permissions"] = security_info(_smoke(ctx, vol, "permissions", "permissions"))
    # 2. permissions with an on-volume .NTFS-3G/UserMapping file.
    vol = new_volume(ctx)
    mount_product(ctx, vol)
    try:
        os.mkdir(vol.mnt + "/.NTFS-3G")
        put(vol.mnt + "/.NTFS-3G/UserMapping", _mapping_text().encode())
    finally:
        fsops.unmount(ctx, vol)
    results["permissions + on-volume UserMapping"] = security_info(_smoke(ctx, vol, "ondisk-map", "permissions"))
    # 3. usermapping=<absolute path> (the product path /usr/local/etc/ntfs-macos is a worker-sandbox matter;
    #    here a file in the case's work directory, no space in the path).
    mapfile = os.path.join(ctx.work, "UserMapping")
    with open(mapfile, "w") as f:
        f.write(_mapping_text())
    os.chmod(mapfile, 0o644)
    vol = new_volume(ctx)
    results["usermapping=file"] = security_info(_smoke(ctx, vol, "usermapping", "usermapping=" + mapfile))
    for k, v in results.items():
        ctx.log("security of small.bin: %-40s %s" % (k, v))
    if os.path.exists(mapfile):
        os.unlink(mapfile)


# ---------------------------------------------------------------- I-PERM-07

OTHER = "ntfsmacperm"
OTHER_UID = 561


def _dscl(*args):
    return subprocess.run(sudo_prefix() + ["/usr/bin/dscl", "."] + list(args), stdin=subprocess.DEVNULL,
                          stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)


def in_vm():
    r = subprocess.run(["/usr/sbin/sysctl", "-n", "kern.hv_vmm_present"], stdin=subprocess.DEVNULL,
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, timeout=10)
    return r.returncode == 0 and r.stdout.strip() == b"1"


def make_other_user(ctx):
    # Accounts are created only in a guest, never on a host Mac.
    if not in_vm():
        raise Fail("I-PERM-07 creates a local account and runs only inside a VM (kern.hv_vmm_present is not 1)")
    if _dscl("-read", "/Users/" + OTHER).returncode == 0:
        _dscl("-delete", "/Users/" + OTHER)
    if _dscl("-search", "/Users", "UniqueID", str(OTHER_UID)).stdout.strip():
        raise Fail("uid %d already in use in the guest" % OTHER_UID)
    ctx.add_cleanup(lambda: drop_other_user(ctx))
    for key, val in (("UniqueID", str(OTHER_UID)), ("PrimaryGroupID", str(OTHER_UID)), ("UserShell", "/bin/sh"),
                     ("NFSHomeDirectory", "/var/empty"), ("RealName", "ntfsmac perm test")):
        r = _dscl("-create", "/Users/" + OTHER, key, val)
        if r.returncode != 0:
            raise Fail("dscl create %s failed: %s" % (key, r.stderr.decode()[:200]))
    ctx.log("created throwaway account %s uid %d (no password, used through sudo -u only)" % (OTHER, OTHER_UID))


def drop_other_user(ctx):
    if _dscl("-read", "/Users/" + OTHER).returncode == 0:
        r = _dscl("-delete", "/Users/" + OTHER)
        ctx.log("deleted account %s: rc %d" % (OTHER, r.returncode))
    if _dscl("-read", "/Users/" + OTHER).returncode == 0:
        raise Fail("account %s still exists after delete" % OTHER)


def as_other(script):
    r = subprocess.run(["/usr/bin/sudo", "-n", "-u", OTHER, "/bin/sh", "-c", script], stdin=subprocess.DEVNULL,
                       cwd="/", stdout=subprocess.PIPE, stderr=subprocess.PIPE, timeout=60)
    return r.returncode, r.stdout.decode("utf-8", "replace").strip(), r.stderr.decode("utf-8", "replace").strip()


# Operation -> expected (rc == 0). Measured on macOS 27.2, macFUSE 5.5.0 (docs/spikes/M3-permissions.md).
EXPECT_OTHER_PRODUCT = {
    "stat": True, "list": True, "read": True, "read locked": True, "append": True, "append locked": True,
    "create": True, "mkdir": True, "mkdir in dir": True, "rename": True, "delete": True, "exec": True,
}


def _other_ops(ctx, m):
    """Run the operations as the other account. Returns {op: (rc, stdout, stderr)}."""
    q = shlex.quote
    ops = (
        ("stat", "/usr/bin/stat -f '%%u:%%g:%%Sp' %s" % q(m + "/plain.txt")),
        ("list", "/bin/ls -ln %s" % q(m)),
        ("read", "/bin/cat %s" % q(m + "/plain.txt")),
        ("read locked", "/bin/cat %s" % q(m + "/locked.txt")),
        ("append", "echo more >> %s" % q(m + "/plain.txt")),
        ("append locked", "echo more >> %s" % q(m + "/locked.txt")),
        ("create", "echo new > %s" % q(m + "/by-other.txt")),
        ("mkdir", "/bin/mkdir %s" % q(m + "/other-dir")),
        ("mkdir in dir", "/bin/mkdir %s" % q(m + "/adir/other-sub")),
        ("rename", "/bin/mv %s %s" % (q(m + "/rename-me.txt"), q(m + "/renamed.txt"))),
        ("delete", "/bin/rm %s" % q(m + "/delete-me.txt")),
        ("exec", q(m + "/bin-true")),
    )
    out = {}
    for name, script in ops:
        rc, so, se = as_other(script)
        out[name] = (rc, so, se)
        ctx.log("other account: %-16s rc=%d %s%s" % (name, rc, so.replace("\n", " | ")[:140],
                                                    (" ERR " + se[:140]) if se else ""))
    return out


def _seed_other(ctx, m):
    put(m + "/plain.txt", b"plain\n")
    put(m + "/locked.txt", b"locked\n")
    os.chmod(m + "/locked.txt", 0)
    put(m + "/rename-me.txt", b"r\n")
    put(m + "/delete-me.txt", b"d\n")
    os.mkdir(m + "/adir")
    shutil.copyfile(system_binary(), m + "/bin-true")


@case("I-PERM-07", envs=("B",), title="a second, non-admin local account reads, writes and lists on a mount owned by the console user (N10)")
def i_perm_07(ctx):
    make_other_user(ctx)
    # The mount point must be reachable by the other account: the harness work directory is under the
    # console user's private TMPDIR.
    base = "/private/tmp/ntfsmac-perm07-%d" % os.getpid()
    os.makedirs(base + "/mnt")
    os.chmod(base, 0o755)
    os.chmod(base + "/mnt", 0o755)
    ctx.add_cleanup(lambda: shutil.rmtree(base, ignore_errors=True))
    vol = new_volume(ctx)
    vol.mnt = base + "/mnt"

    # Pass 1: the product mount line.
    mount_product(ctx, vol)
    m = vol.mnt
    try:
        _seed_other(ctx, m)
        ctx.log("console user view (chmod 0 on locked.txt is a no-op on this mount, the real mode-000 file is in pass 2): plain %s, locked %s, adir %s" % (
            fmt(perm_of(m + "/plain.txt")), fmt(perm_of(m + "/locked.txt")), fmt(perm_of(m + "/adir"))))
        r = as_root(["/usr/bin/stat", "-f", "%u:%g", m + "/plain.txt"])
        ctx.log("root view of plain.txt: %s" % r.stdout.decode().strip())
        res = _other_ops(ctx, m)
        rc, so, se = as_other("/usr/bin/stat -f '%u:%g' " + shlex.quote(m + "/plain.txt"))
        ctx.log("other account sees owner of an admin-made file as %s" % so)
        rc, so, se = as_other("/bin/chmod 600 %s" % shlex.quote(m + "/plain.txt"))
        ctx.log("other account chmod 600 plain.txt: rc=%d %s (recorded)" % (rc, se))
        live = {k: os.path.exists(m + "/" + k) for k in ("by-other.txt", "other-dir", "adir/other-sub", "renamed.txt",
                                                         "rename-me.txt", "delete-me.txt")}
        content = slurp(m + "/plain.txt")
    finally:
        fsops.unmount(ctx, vol)
    for op, want in EXPECT_OTHER_PRODUCT.items():
        got = res[op][0] == 0
        if got != want:
            raise Fail("other account, %s: succeeded=%s, expected %s (N10 text needs review)" % (op, got, want))
    if res["exec"][1] != "" or res["read locked"][1] != "locked":
        raise Fail("other account read or exec output not as expected")
    exp = {"by-other.txt": True, "other-dir": True, "adir/other-sub": True, "renamed.txt": True,
           "rename-me.txt": False, "delete-me.txt": False}
    if live != exp:
        raise Fail("state after the other account's operations differs: %s" % live)
    if content != b"plain\nmore\n":
        raise Fail("append by the other account not in the file: %r" % content)
    # The changes are on disk: remount read-only and look.
    mount_product(ctx, vol, rw=False)
    try:
        after = sorted(n for n in os.listdir(m) if n != ".fseventsd")
        ctx.log("after remount: %s" % after)
        if slurp(m + "/plain.txt") != b"plain\nmore\n" or "delete-me.txt" in after or "by-other.txt" not in after:
            raise Fail("other account's writes are not on disk after remount")
        # Read-only mount: writes by the other account fail.
        rc, so, se = as_other("echo x > %s" % shlex.quote(m + "/ro-try.txt"))
        ctx.log("read-only mount, other account create: rc=%d %s" % (rc, se))
        if rc == 0:
            raise Fail("create succeeded on a read-only mount")
    finally:
        fsops.unmount(ctx, vol)

    # Pass 2: `permissions` opt-in, modes are stored. Recorded and asserted the same way: no enforcement.
    vol = new_volume(ctx)
    vol.mnt = base + "/mnt"
    mount_opts(ctx, vol, "permissions")
    try:
        put(m + "/mine.txt", b"mine\n")
        os.chmod(m + "/mine.txt", 0o600)
        put(m + "/locked.txt", b"locked\n")
        os.chmod(m + "/locked.txt", 0)
        os.mkdir(m + "/private")
        os.chmod(m + "/private", 0o700)
        put(m + "/private/in.txt", b"in\n")
        ctx.log("permissions mount, console view: mine %s locked %s private %s" % (
            fmt(perm_of(m + "/mine.txt")), fmt(perm_of(m + "/locked.txt")), fmt(perm_of(m + "/private"))))
        outs = {}
        for name, script in (("read 0600", "/bin/cat %s" % shlex.quote(m + "/mine.txt")),
                             ("write 0600", "echo x >> %s" % shlex.quote(m + "/mine.txt")),
                             ("read mode-000", "/bin/cat %s" % shlex.quote(m + "/locked.txt")),
                             ("append mode-000", "echo x >> %s" % shlex.quote(m + "/locked.txt")),
                             ("list 0700 dir", "/bin/ls %s" % shlex.quote(m + "/private")),
                             ("delete in 0700 dir", "/bin/rm %s" % shlex.quote(m + "/private/in.txt")),
                             ("create in 0700 dir", "echo x > %s" % shlex.quote(m + "/private/o.txt"))):
            rc, so, se = as_other(script)
            outs[name] = rc
            ctx.log("permissions mount, other account: %-20s rc=%d %s %s" % (name, rc, so[:60], se[:100]))
    finally:
        fsops.unmount(ctx, vol)
    bad = [k for k, rc in outs.items() if rc != 0]
    if bad:
        raise Fail("permissions mount: other account refused for %s; N10 text needs review" % bad)
    drop_other_user(ctx)
