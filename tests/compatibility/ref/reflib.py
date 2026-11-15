# SPDX-License-Identifier: GPL-2.0-or-later
"""Helpers and phase programs of the reference comparison (C-REF-OP-01..12, T-M3-15).

Not a case module (no case declarations; the harness imports refcases.py). Python 3.9, standard library
only (it runs in the macOS guest and in the Linux container).

What one case does (refcases.py drives it from the host, like C-REF-01):

  phase-a  (Linux container)  fresh image, mkntfs, mount NTFS-3G with the product option set minus the two
           macOS-only tokens, replay the operation script, unmount, ntfsfix -n, then `dump`.
  phase-b  (macOS guest)      fresh image, mkntfs, mount with exactly the string src/options/options.c builds
           (tool optstr.c, compiled in the guest), replay the same script, unmount, ntfsfix -n.
           The image stays in the guest; the host streams it into a container.
  dump     (Linux container)  `dump` of a finished image: ntfsfix -n, read-only mount, tests/tools/treedump,
           and a full dump (type, size, SHA-256, link target, mode, link count and link group, allocated
           bytes, NTFS attribute flags, NTFS times, every named stream with length and SHA-256).
  judge    (host)             compare both results; every difference must be covered by an expected
           difference of expected-differences.md (rules ED-nn below), else the case fails.

Operation kinds (replay): mkdir, create, write, append, truncate, writezero, text, fsync, rename, unlink,
rmdir, link, symlink, setxattr, removexattr, utime, chattr, many, unlink_many, deep.
"""

import argparse
import hashlib
import json
import os
import random
import re
import shlex
import struct
import sys
import time
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
COMPAT = os.path.dirname(HERE)
TESTS = os.path.dirname(COMPAT)
REPO = os.path.dirname(TESTS)
for _p in (TESTS, os.path.join(TESTS, "integration", "functional"), os.path.join(TESTS, "integration", "xattr")):
    if _p not in sys.path:
        sys.path.insert(0, _p)

import fsops  # noqa: E402
import xattr_ops as xo  # noqa: E402
from harness import core, transport, volume as hv  # noqa: E402
from harness.seeded import MACOS_ROOT_METADATA  # noqa: E402

MIB = 1 << 20
EXIT_SKIP = 3
ATTR_BE = b"system.ntfs_attrib_be"
TIMES = b"system.ntfs_times"
FILE_SPARSE = 0x200
FILE_COMPRESSED = 0x800
EPOCH_DIFF = 11644473600          # seconds from 1601-01-01 to 1970-01-01
# Tokens of the builder's string that Linux NTFS-3G does not know (macOS FSKit backend selection).
MAC_ONLY_TOKENS = ("backend=fskit", "local")
DUMP_OPTS = "ro,noatime,streams_interface=openxattr"
MOUNT_DEADLINE = 60


def ops_path(n):
    return os.path.join(HERE, "ops-%02d.json" % n)


def load_ops(n):
    with open(ops_path(n), "r", encoding="utf-8") as f:
        return json.load(f)


# ---------------------------------------------------------------- replay

ERRNAME_MAP = {"ENODATA": "ENOATTR", "EOPNOTSUPP": "ENOTSUP"}


def errname(e):
    import errno
    n = errno.errorcode.get(e, "E%d" % e)
    return ERRNAME_MAP.get(n, n)


def gen(seed, idx, n):
    return random.Random("%s:%d" % (seed, idx)).randbytes(n)


def text_bytes(label, n):
    line = "%s line %%08d: the quick brown fox jumps over the lazy dog\n" % label
    count = n // len(line % 0) + 2
    return "".join(line % i for i in range(count)).encode()[:n]


class Replayer(object):
    def __init__(self, mnt, log=None):
        self.mb = os.fsencode(mnt)
        self.log = log or (lambda m: None)
        self.out = []

    def p(self, rel):
        return os.path.join(self.mb, rel.encode("utf-8"))

    def _pwrite(self, path, off, size, seed, chunk, flags):
        fd = os.open(path, flags, 0o666)
        try:
            pos, i = 0, 0
            while pos < size:
                n = min(chunk, size - pos)
                data = gen(seed, i, n)
                if flags & os.O_APPEND:
                    os.write(fd, data)
                else:
                    os.pwrite(fd, data, off + pos)
                pos += n
                i += 1
        finally:
            os.close(fd)

    def run_one(self, op):
        k = op["op"]
        w = os.O_WRONLY | os.O_CREAT
        if k == "mkdir":
            os.mkdir(self.p(op["path"]), 0o777)
        elif k == "create":
            self._pwrite(self.p(op["path"]), 0, op["size"], op["seed"], op.get("chunk", 7919), w | os.O_TRUNC)
        elif k == "write":
            self._pwrite(self.p(op["path"]), op["off"], op["size"], op["seed"], op.get("chunk", 7919), w)
        elif k == "append":
            self._pwrite(self.p(op["path"]), 0, op["size"], op["seed"], op.get("chunk", 7919), w | os.O_APPEND)
        elif k == "writezero":
            fd = os.open(self.p(op["path"]), os.O_WRONLY)
            try:
                os.pwrite(fd, b"\0" * op["size"], op["off"])
            finally:
                os.close(fd)
        elif k == "text":
            fd = os.open(self.p(op["path"]), w | os.O_TRUNC, 0o666)
            try:
                data = text_bytes(op["label"], op["size"])
                for i in range(0, len(data), 65536):
                    os.write(fd, data[i:i + 65536])
            finally:
                os.close(fd)
        elif k == "truncate":
            os.truncate(self.p(op["path"]), op["size"])
        elif k == "fsync":
            fd = os.open(self.p(op["path"]), os.O_RDWR)
            try:
                os.fsync(fd)
            finally:
                os.close(fd)
        elif k == "rename":
            os.rename(self.p(op["src"]), self.p(op["dst"]))
        elif k == "unlink":
            os.unlink(self.p(op["path"]))
        elif k == "rmdir":
            os.rmdir(self.p(op["path"]))
        elif k == "link":
            os.link(self.p(op["src"]), self.p(op["dst"]))
        elif k == "symlink":
            os.symlink(op["target"].encode("utf-8"), self.p(op["path"]))
        elif k == "setxattr":
            e = xo.setx(self.p(op["path"]), op["name"].encode("utf-8"), gen(op["seed"], 0, op["size"]))
            if e:
                raise OSError(e, "setxattr")
        elif k == "removexattr":
            e = xo.removex(self.p(op["path"]), op["name"].encode("utf-8"))
            if e:
                raise OSError(e, "removexattr")
        elif k == "utime":
            os.utime(self.p(op["path"]), ns=(op["atime_ns"], op["mtime_ns"]))
        elif k == "chattr":
            cur = xo.getx(self.p(op["path"]), ATTR_BE.decode())
            if not isinstance(cur, bytes) or len(cur) != 4:
                raise OSError(cur if isinstance(cur, int) else 5, "getxattr system.ntfs_attrib_be")
            val = struct.pack(">I", struct.unpack(">I", cur)[0] | op["or"])
            e = xo.setx(self.p(op["path"]), ATTR_BE.decode(), val)
            if e:
                raise OSError(e, "setxattr system.ntfs_attrib_be")
        elif k == "many":
            res = {}
            for i in range(op["count"]):
                size = op["min"] + (i * 37) % op["mod"]
                try:
                    self._pwrite(self.p(op["dir"] + "/" + op["fmt"] % i), 0, size, "%s:%d" % (op["seed"], i), 4099,
                                 w | os.O_TRUNC)
                except OSError as e:
                    res[errname(e.errno)] = res.get(errname(e.errno), 0) + 1
            if res:
                raise OSError(-1, json.dumps(res, sort_keys=True))
        elif k == "unlink_many":
            res = {}
            for i in range(op["start"], op["stop"], op["step"]):
                try:
                    os.unlink(self.p(op["dir"] + "/" + op["fmt"] % i))
                except OSError as e:
                    res[errname(e.errno)] = res.get(errname(e.errno), 0) + 1
            if res:
                raise OSError(-1, json.dumps(res, sort_keys=True))
        elif k == "deep":
            cur = op["base"]
            for d in range(op["depth"]):
                cur = cur + "/" + op["name"] % d
                os.mkdir(self.p(cur), 0o777)
                if (d + 1) % op["every"] == 0:
                    self._pwrite(self.p(cur + "/" + op["file"]), 0, op["fsize"], "%s:%d" % (op["seed"], d), 4099,
                                 w | os.O_TRUNC)
            self._pwrite(self.p(cur + "/" + op["file"]), 0, op["fsize"], op["seed"], 4099, w | os.O_TRUNC)
        else:
            raise ValueError("unknown op kind %r" % k)

    def run(self, ops):
        for i, op in enumerate(ops):
            try:
                self.run_one(op)
                rc = "ok"
            except OSError as e:
                rc = json.loads(e.strerror) if e.errno == -1 and e.strerror and e.strerror.startswith("{") \
                    else errname(e.errno)
                rc = rc if isinstance(rc, str) else "partial " + json.dumps(rc, sort_keys=True)
            self.out.append({"i": i, "op": op["op"], "path": op.get("path") or op.get("dst") or op.get("base") or "",
                             "rc": rc})
            if rc != "ok":
                self.log("op %d %s %s -> %s" % (i, op["op"], (op.get("path") or op.get("dst") or "")[:60], rc))
        return self.out


# ---------------------------------------------------------------- dump (runs on Linux, on a read-only mount)

def esc(b):
    out = []
    for c in b:
        if c == 0x5c:
            out.append("\\\\")
        elif c == 9:
            out.append("\\t")
        elif c == 10:
            out.append("\\n")
        elif c < 0x20 or c == 0x7f:
            out.append("\\x%02x" % c)
        else:
            out.append(chr(c))
    s = "".join(out)
    # keep valid UTF-8 readable, escape the rest
    try:
        return b"".join(x.encode("latin-1") for x in s).decode("utf-8")
    except (UnicodeDecodeError, UnicodeEncodeError):
        return "".join(chr(c) if c < 0x80 and 0x20 <= c != 0x7f and c != 0x5c else "\\x%02x" % c for c in b)


def sha_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(MIB)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def stream_map(path):
    out = {}
    try:
        names = os.listxattr(path, follow_symlinks=False)
    except OSError as e:
        return {"<list>": "errno %d" % e.errno}
    for n in names:
        if n.startswith("system."):
            continue
        try:
            v = os.getxattr(path, n, follow_symlinks=False)
            out[unicodedata.normalize("NFC", n)] = "%d:%s" % (len(v), hashlib.sha256(v).hexdigest())
        except OSError as e:
            out[unicodedata.normalize("NFC", n)] = "errno %d" % e.errno
    return out


def dump_tree(mnt):
    """{escaped path: entry} for everything below mnt (top-level NTFS metafiles skipped like treedump)."""
    ents = {}
    inos = {}

    def walk(dirb, relb, top):
        for nb in sorted(os.listdir(dirb)):
            if top and (nb.startswith(b"$") or nb.lower() == b"system volume information"):
                continue
            full = os.path.join(dirb, nb)
            rel = nb if not relb else relb + b"/" + nb
            st = os.lstat(full)
            e = {"nlink": st.st_nlink}
            for key, name in (("attrib", ATTR_BE), ("times", TIMES)):
                try:
                    v = os.getxattr(full, name, follow_symlinks=False)
                    if key == "attrib":
                        e["attrib"] = "0x%08x" % struct.unpack(">I", v)[0]
                    else:
                        c, m, a, ch = struct.unpack("<QQQQ", v[:32])
                        e["times"] = {"crtime": c, "mtime": m, "ctime": ch, "atime": a}
                except OSError as ex:
                    e[key] = "errno %d" % ex.errno
            import stat as S
            if S.S_ISDIR(st.st_mode):
                e["t"] = "d"
            elif S.S_ISREG(st.st_mode):
                e["t"] = "f"
                e["size"] = st.st_size
                e["sha"] = sha_file(full)
                e["blocks"] = st.st_blocks * 512
            elif S.S_ISLNK(st.st_mode):
                e["t"] = "l"
                e["target"] = esc(os.readlink(full))
            else:
                e["t"] = "o"
            e["xattrs"] = stream_map(full)
            if st.st_nlink > 1 and e["t"] != "d":
                inos.setdefault(st.st_ino, []).append(esc(rel))
            e["ino"] = st.st_ino
            ents[esc(rel)] = e
            if e["t"] == "d":
                walk(full, rel, False)

    walk(os.fsencode(mnt), b"", True)
    for e in ents.values():
        grp = inos.get(e.pop("ino"))
        e["group"] = sorted(grp)[0] if grp else None
    return ents


# ---------------------------------------------------------------- environment helpers

def _phase_ctx(env, work, case_id):
    c = core.Case(case_id, (env,), None, "reference comparison phase", None)
    return core.Ctx(c, env, work, os.path.join(work, "evidence"), transport.LocalTransport(), quiet=False)


def os_string(t):
    r = t.run("sw_vers -productName && sw_vers -productVersion && sw_vers -buildVersion && uname -m")
    if r.rc == 0:
        n, v, b, a = r.out.decode().split()[:4]
        return "%s %s (%s) %s" % (n, v, b, a)
    r = t.run(". /etc/os-release 2>/dev/null && echo \"$PRETTY_NAME $(uname -m)\"")
    return r.out.decode().strip() if r.rc == 0 and r.out.strip() else "unknown"


def sha256_path(path):
    return sha_file(path)


def build_optstr(ctx, repo, rw=True):
    """Compile optstr.c with the product's builder and return its option string for this user."""
    exe = os.path.join(ctx.work, "optstr")
    ctx.sh("cc -std=c17 -I %s -o %s %s %s" % (shlex.quote(os.path.join(repo, "include")), shlex.quote(exe),
                                                 shlex.quote(os.path.join(HERE, "optstr.c")),
                                                 shlex.quote(os.path.join(repo, "src", "options", "options.c"))))
    r = ctx.sh("%s %d %d %s" % (shlex.quote(exe), os.getuid(), os.getgid(), "rw" if rw else "ro"))
    s = r.out.decode().strip()
    for tok in s.split(","):
        key = tok.split("=")[0]
        if key in fsops.BANNED_OPTIONS:
            raise RuntimeError("the builder produced a banned option: %s" % tok)
    if "norecover" not in s.split(","):
        raise RuntimeError("the builder string has no norecover: %s" % s)
    return s


def linux_optstr(builder):
    """The builder's string without the macOS-only tokens (Linux NTFS-3G does not know them)."""
    return ",".join(t for t in builder.split(",") if t not in MAC_ONLY_TOKENS)


def mount_b(ctx, vol, optstr):
    """Environment B mount with exactly `optstr`: argv order as the daemon's (device, mount point, -o string)."""
    ctx.sh("%s %s %s -o %s" % (shlex.quote(hv.guest_tool("ntfs-3g")), shlex.quote(vol.img), shlex.quote(vol.mnt),
                                shlex.quote(optstr)), timeout=MOUNT_DEADLINE)
    fsops._wait_b_mount(ctx, vol)


def mount_a(ctx, vol, optstr):
    ctx.sh("ntfs-3g %s %s -o %s" % (shlex.quote(vol.img), shlex.quote(vol.mnt), shlex.quote(optstr)),
           timeout=MOUNT_DEADLINE)
    vol.mounted = True


def dump_image(ctx, vol, repo):
    """On Linux: ntfsfix -n, read-only mount, treedump, full dump. Leaves the volume unmounted."""
    doc = {}
    fix = ctx.sh("ntfsfix -n %s" % shlex.quote(vol.img), check=False)
    doc["ntfsfix"] = {"rc": fix.rc, "out": (fix.out + fix.err).decode("utf-8", "replace")}
    exe = os.path.join(ctx.work, "treedump")
    ctx.sh("cc -std=c17 -O2 -o %s %s" % (shlex.quote(exe), shlex.quote(os.path.join(TESTS, "tools", "treedump.c"))))
    os.makedirs(vol.mnt, exist_ok=True)
    mount_a(ctx, vol, DUMP_OPTS)
    try:
        doc["treedump"] = ctx.sh("%s %s" % (shlex.quote(exe), shlex.quote(vol.mnt))).out.decode(
            "utf-8", "surrogateescape")
        doc["dump"] = dump_tree(vol.mnt)
    finally:
        fsops.unmount(ctx, vol)
    # A value the Linux xattr API cannot return (above 64 KiB, E2BIG) is read by ntfscat from the image.
    for rel, e in doc["dump"].items():
        for name, v in e["xattrs"].items():
            if v == "errno 7":
                r = ctx.transport.run("ntfscat -a DATA -n %s %s /%s" % (shlex.quote(name), shlex.quote(vol.img),
                                                                         shlex.quote(rel)))
                if r.rc == 0:
                    e["xattrs"][name] = "%d:%s" % (len(r.out), hashlib.sha256(r.out).hexdigest())
    doc["image_sha256"] = sha_file(vol.img)
    doc["ntfs3g_version"] = hv.detect_ntfs3g(ctx.transport)
    return doc


# ---------------------------------------------------------------- phase programs

def phase_replay(env, n, work, repo):
    spec = load_ops(n)
    ctx = _phase_ctx(env, work, spec["id"])
    try:
        if env == "B":
            ver = hv.detect_macfuse(ctx.transport)
            missing = [x for x in ("ntfs-3g", "mkntfs", "ntfsfix") if hv.guest_tool(x) is None]
            if ver is None or missing:
                sys.stderr.write("SKIP: macFUSE %s, missing tools %s\n" % (ver, missing))
                return EXIT_SKIP, None
        else:
            for tool in ("ntfs-3g", "mkntfs", "ntfsfix", "cc", "umount"):
                if ctx.transport.run("command -v %s" % tool).rc != 0:
                    raise hv.VolumeError("%s not found in the Environment A container" % tool)
        # the builder's string is built in both environments; A drops the two macOS-only tokens
        builder = build_optstr(ctx, repo, True)
        optstr = builder if env == "B" else linux_optstr(builder)
        size = spec["image_mib"] * MIB
        vol = hv.MacosNtfs3g(ctx, size) if env == "B" else hv.LinuxNtfs3g(ctx, size)
        vol.create()
        ctx.log("mount options (%s): %s" % (env, optstr))
        t0 = time.time()
        (mount_b if env == "B" else mount_a)(ctx, vol, optstr)
        try:
            log = Replayer(vol.mnt, ctx.log).run(spec["ops"])
        finally:
            fsops.unmount(ctx, vol)
        t1 = time.time()
        doc = {"env": env, "optstr": optstr, "builder_optstr": builder, "oplog": log, "t_start": t0, "t_end": t1,
               "os": os_string(ctx.transport), "macfuse_version": hv.detect_macfuse(ctx.transport),
               "ntfs3g_version": hv.detect_ntfs3g(ctx.transport)}
        fsck_tool = hv.guest_tool("ntfsfix") if env == "B" else "ntfsfix"
        fix = ctx.sh("%s -n %s" % (shlex.quote(fsck_tool), shlex.quote(vol.img)), check=False)
        doc["ntfsfix_after_replay"] = {"rc": fix.rc, "out": (fix.out + fix.err).decode("utf-8", "replace")}
        if env == "A":
            doc.update(dump_image(ctx, vol, repo))
        else:
            doc["image_sha256"] = sha_file(vol.img)
            doc["image_size"] = os.path.getsize(vol.img)
        return 0, doc
    finally:
        ctx.close()


def phase_dump(n, work, repo):
    spec = load_ops(n)
    ctx = _phase_ctx("A", work, spec["id"])
    try:
        vol = hv.LinuxNtfs3g(ctx, 0)
        os.makedirs(vol.mnt, exist_ok=True)
        ctx.add_cleanup(vol.cleanup)
        sha_in = sha_file(vol.img)
        doc = dump_image(ctx, vol, repo)
        doc["image_sha256_in"] = sha_in
        doc["os"] = os_string(ctx.transport)
        return doc
    finally:
        ctx.close()


# ---------------------------------------------------------------- judge (pure)

ED_DOC = os.path.join(COMPAT, "expected-differences.md")
ED_ID_RE = re.compile(r"^\| (ED-\d+) \|", re.M)


def documented_ids(path=ED_DOC):
    try:
        with open(path, "r", encoding="utf-8") as f:
            return set(ED_ID_RE.findall(f.read()))
    except OSError:
        return set()


def nfc(s):
    return unicodedata.normalize("NFC", s)


def under_metadata(path):
    return any(path == m or path.startswith(m + "/") for m in MACOS_ROOT_METADATA)


def is_real_filetime(v, lo, hi):
    """A FILETIME that is a real wall-clock time between the unix seconds lo and hi (a second of slack)."""
    sec = v / 1e7 - EPOCH_DIFF
    return lo - 2 <= sec <= hi + 2


def is_raw_seconds(v, lo, hi):
    """B's creation time: the unix second of the create stored as a count of 100 ns ticks (ED-03)."""
    return lo - 2 <= v <= hi + 2


def tracked_paths(spec):
    return set(op["path"] for op in spec["ops"] if op["op"] == "utime")


def judge(spec, a, b, ids_doc=None):
    """Compare phase A (with its dump) and phase B (with the dump of its image taken on Linux).

    Returns (bad, explained, notes): `bad` are differences no expected difference covers;
    `explained` are (rule id, text) pairs; `notes` are facts for the log.
    """
    bad, explained, notes = [], [], []
    ids_doc = documented_ids() if ids_doc is None else ids_doc
    keys_explained = set()

    def ed(rule, text, key=None):
        if rule not in ids_doc:
            bad.append("%s is applied by the judge but not listed in expected-differences.md: %s" % (rule, text))
        else:
            explained.append((rule, text))
            if key is not None:
                keys_explained.add(key)

    # 1. images and option strings
    for label, d in (("A", a), ("B", b)):
        if d["ntfsfix"]["rc"] != 0:
            bad.append("ntfsfix -n on the image of %s exited %d: %s" % (label, d["ntfsfix"]["rc"],
                                                                         d["ntfsfix"]["out"][-200:]))

    def strip(s):  # uid and gid differ between the environments (root in the container, the console user in the guest)
        return ",".join(t for t in s.split(",") if not t.startswith(("uid=", "gid=")))

    if strip(a["builder_optstr"]) != strip(b["builder_optstr"]):
        bad.append("builder strings differ: A %s, B %s" % (a["builder_optstr"], b["builder_optstr"]))
    if b["optstr"] != b["builder_optstr"]:
        bad.append("B was not mounted with the builder's string: %s" % b["optstr"])
    # 2. operation results
    ed_ops = dict((int(k), v) for k, v in spec.get("ed_ops", {}).items())
    la, lb = a["oplog"], b["oplog"]
    if len(la) != len(lb):
        bad.append("operation logs differ in length: %d against %d" % (len(la), len(lb)))
    for x, y in zip(la, lb):
        if x["rc"] != y["rc"]:
            txt = "op %d %s %s: A %s, B %s" % (x["i"], x["op"], x["path"], x["rc"], y["rc"])
            if x["i"] in ed_ops:
                ed(ed_ops[x["i"]], txt)
            else:
                bad.append("result differs: " + txt)
    # 3. dumps
    da, db = a["dump"], b["dump"]
    tracked = tracked_paths(spec)
    ed_paths = spec.get("ed_paths", {})
    ka, kb = {}, {}
    for p, e in da.items():
        ka.setdefault(nfc(p), []).append((p, e))
    for p, e in db.items():
        if under_metadata(p):
            ed("ED-01", "%s: exists on B only (macOS creates it in the root of every mount)" % p)
            continue
        kb.setdefault(nfc(p), []).append((p, e))
    for key in sorted(set(ka) | set(kb)):
        if len(ka.get(key, ())) > 1 or len(kb.get(key, ())) > 1:
            bad.append("%s: several names with the same NFC form: A %s, B %s" % (
                key, [x[0] for x in ka.get(key, ())], [x[0] for x in kb.get(key, ())]))
            continue
        ea = ka[key][0] if key in ka else None
        eb = kb[key][0] if key in kb else None
        rules = list(ed_paths.get(key, ()))
        if ea is None or eb is None:
            side = "A only" if eb is None else "B only"
            if rules:
                ed(rules[0], "%s: %s" % (key, side), key)
            else:
                bad.append("%s: present on %s only" % (key, "A" if eb is None else "B"))
            continue
        (pa, xa), (pb, xb) = ea, eb
        if pa != pb:
            if pb == nfc(pb) and pa != nfc(pa):
                ed("ED-02", "%s: typed in a non-NFC form, stored as typed on A, in NFC on B" % key, key)
            else:
                bad.append("%s: names differ on disk: A %s, B %s" % (key, pa.encode("utf-8"), pb.encode("utf-8")))
        diffs = {}
        for f in ("t", "size", "sha", "target", "nlink", "group", "blocks", "attrib"):
            va, vb = xa.get(f), xb.get(f)
            if f == "group":
                va = nfc(va) if va else va
                vb = nfc(vb) if vb else vb
            if va != vb:
                diffs[f] = (va, vb)
        if xa["xattrs"] != xb["xattrs"]:
            diffs["xattrs"] = (xa["xattrs"], xb["xattrs"])
        ta, tb = xa.get("times"), xb.get("times")
        if isinstance(ta, dict) and isinstance(tb, dict):
            if not is_real_filetime(ta["crtime"], a["t_start"], a["t_end"]):
                bad.append("%s: creation time on A is not a time of the run: %s" % (key, ta["crtime"]))
            if is_raw_seconds(tb["crtime"], b["t_start"], b["t_end"]):
                bad.append("%s: creation time on B is the raw create second (1601-01-01 plus seconds): patch 0008 is not applied" % key)
            elif not is_real_filetime(tb["crtime"], b["t_start"], b["t_end"]):
                bad.append("%s: creation time on B fits neither the real nor the raw-seconds form: %s" % (key, tb["crtime"]))
            if key in tracked:
                for f in ("mtime", "atime"):
                    if ta[f] != tb[f]:
                        diffs[f] = (ta[f], tb[f])
        elif ta != tb:
            diffs["times"] = (ta, tb)
        if not diffs:
            continue
        covered = rules_cover(rules, xa, xb, diffs)
        if covered:
            for r in covered:
                ed(r, "%s: %s differ (listed for this path)" % (key, ", ".join(sorted(diffs))), key)
        else:
            for f, (va, vb) in sorted(diffs.items()):
                bad.append("%s: %s differs: A %s, B %s" % (key, f, str(va)[:160], str(vb)[:160]))
    # 4. treedump agrees (same tool on both images; names in NFC, macOS root metadata removed); only paths
    # that an expected difference already explains may differ
    tda, tdb = treedump_map(a["treedump"]), treedump_map(b["treedump"])
    for k in sorted(set(tda) | set(tdb)):
        if tda.get(k) != tdb.get(k) and k not in keys_explained:
            bad.append("treedump differs at %s: A %s, B %s" % (k, tda.get(k), tdb.get(k)))
    notes.append("%d entries on A, %d on B (macOS root metadata excluded); %d differences explained, %d not" % (
        len(da), len([p for p in db if not under_metadata(p)]), len(explained), len(bad)))
    return bad, explained, notes


def treedump_map(text):
    out = {}
    for line in unicodedata.normalize("NFC", text).splitlines():
        parts = line.split("\t")
        if len(parts) < 2 or under_metadata(parts[1]):
            continue
        out[parts[1]] = line
    return out


def xattr_rules(rules, xa, xb):
    """Expected-difference ids covering every differing stream name, or None if one name is not covered.
    ED-06: a value above 64 KiB exists on B only (Linux refuses it, kernel limit).
    ED-08: a stream name with ':' exists on B only (A is unpatched NTFS-3G; patch 0009 exempts xattr names)."""
    ma, mb = xa["xattrs"], xb["xattrs"]
    used = set()
    for name in sorted(set(ma) | set(mb)):
        if ma.get(name) == mb.get(name):
            continue
        if name not in ma and name in mb:
            length = int(str(mb[name]).split(":")[0]) if str(mb[name])[:1].isdigit() else 0
            if "ED-06" in rules and length > 65536:
                used.add("ED-06")
                continue
            if "ED-08" in rules and ":" in name:
                used.add("ED-08")
                continue
        return None
    return sorted(used)


def rules_cover(rules, xa, xb, diffs):
    """Expected-difference ids (declared for this path in the script) that explain all differing fields
    of the path, or None. The check is on the shape of the difference, not only on the path."""
    left = set(diffs)
    out = []
    if "xattrs" in left:
        r = xattr_rules(rules, xa, xb)
        if r is None:
            return None
        out += r
        left.discard("xattrs")
    if left == {"blocks"} and "ED-07" in rules and str(xa.get("attrib")) == str(xb.get("attrib")) \
            and int(str(xa["attrib"]), 16) & FILE_SPARSE and abs(xa["blocks"] - xb["blocks"]) <= 65536 \
            and xa["size"] == xb["size"] and xa["sha"] == xb["sha"]:
        return ["ED-07"]    # allocation of a sparse file differs by at most one compression unit (64 KiB)
    if left and "ED-05" in rules and left <= {"blocks", "attrib"} and xa.get("size") == xb.get("size") \
            and xa.get("sha") == xb.get("sha"):
        out.append("ED-05")     # dense write past EOF: same size and content, allocation and sparse flag differ
        left = set()
    return out if not left else None


# ---------------------------------------------------------------- command line (phase programs)

def main(argv):
    p = argparse.ArgumentParser(prog="reflib.py", description="C-REF-OP phase programs (internal)")
    sub = p.add_subparsers(dest="phase", required=True)
    for name in ("phase-a", "phase-b", "dump"):
        s = sub.add_parser(name)
        s.add_argument("--op", type=int, required=True)
        s.add_argument("--dir", required=True)
        s.add_argument("--repo", required=True)
    args = p.parse_args(argv)
    work = os.path.abspath(args.dir)
    os.makedirs(work, exist_ok=True)
    if args.phase == "dump":
        sys.stdout.write(json.dumps(phase_dump(args.op, work, args.repo)))
        return 0
    rc, doc = phase_replay("A" if args.phase == "phase-a" else "B", args.op, work, args.repo)
    if rc != 0:
        return rc
    sys.stdout.write(json.dumps(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
