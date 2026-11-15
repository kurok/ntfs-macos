# SPDX-License-Identifier: GPL-2.0-or-later
"""I-BIG-COMP-01 (T-M3-09): NTFS-compressed files made on Linux, read and updated on macOS, read again on Linux.

Host case driving both environments, like I-XATTR-09 (a_after_b.py). The image makes three hops:

  phase A1 (Linux container, NTFS-3G there; `comp_a_after_b.py phase-a1`)
      mkntfs, mount, mark a directory for compression (system.ntfs_attrib_be, bit 0x800), create files in it
      (text, half random and half zeros, random, 100 B, 1 B, empty, 20 MiB of text, one in a subdirectory),
      and a plain file beside it. Linux reports each file's compressed flag, size, blocks and hash.
  host: image into the guest.
  phase B (macOS guest, `comp_a_after_b.py phase-b`)
      session 1 (product options, compression on): read every file (hash, blocks, compressed flag), then
      update them: overwrite in the middle, append, truncate down and up, overwrite whole, write into a
      1 B and an empty file, delete, rename, create new files in the compressed directory and beside it,
      a new subdirectory. Each operation records its errno; the file is read back and must equal either
      the expected new content (operation succeeded) or the old content (it failed).
      session 2 (`nocompression`): update a compressed file and create a file in the compressed directory.
      The on-disk $DATA attribute (compressed flag, sizes) of every file is read from the unmounted image.
  host: image back into the container.
  phase A2 (Linux; `comp_a_after_b.py phase-a2`)
      ntfsfix -n, read-only mount: every file must equal what the guest left; compressed flags and on-disk
      attributes; then a read-write mount, update a file the guest wrote and read it back.
  host: judge. Asserted: every file made on Linux reads back equal in B; no operation in B leaves a file
      that is neither the old nor the new content; Linux reads every file equal to the guest's final state;
      ntfsfix -n exit 0 at every hop. Recorded: which updates the guest accepts, whether new files in a
      compressed directory are compressed (compression on and off), blocks before and after.

Run (clones the VM, builds NTFS-3G in it, always deletes the VM):
    tests/harness/run-env-b.sh --orchestrate --cases I-BIG-COMP-01
Without NTFSMAC_B_VM the case skips with a note.
"""

import argparse
import hashlib
import json
import os
import shlex
import shutil
import struct
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
REPO = os.path.dirname(TESTS)
for p in (TESTS, os.path.join(TESTS, "integration", "functional"), os.path.join(TESTS, "integration", "xattr"),
          os.path.join(TESTS, "integration", "readpath"), HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import fsops  # noqa: E402
import readlib as rl  # noqa: E402
import xattr_ops as xo  # noqa: E402
from fsops import blob  # noqa: E402
from harness import core, transport, volume as hv  # noqa: E402
from harness.core import Fail, case  # noqa: E402

CASE_ID = "I-BIG-COMP-01"
SHIP = ("harness", "integration/functional", "integration/xattr", "integration/metadata", "integration/readpath",
        "integration/special")
GUEST_PATH = 'PATH="$HOME/ntfsmac/deps/bin:$HOME/ntfsmac/deps/sbin:$PATH"'
EXIT_SKIP = 3
IMG_MIB = 160
MIB = 1 << 20
ATTR = "system.ntfs_attrib_be"
CDIR = "cdir"


def sha(b):
    return hashlib.sha256(b).hexdigest()


def fmt(b):
    return None if b is None else "%d:%s" % (len(b), sha(b))


def text(label, n):
    line = ("%s line %%08d: the quick brown fox jumps over the lazy dog\n" % label)
    count = n // len(line % 0) + 2
    return "".join(line % i for i in range(count)).encode()[:n]


def linux_files():
    """rel path -> content, made in phase A1."""
    return {
        CDIR + "/z_text": text("zt", 2 * MIB),
        CDIR + "/z_mixed": blob("comp:mixed", MIB) + b"\0" * MIB,
        CDIR + "/z_rand": blob("comp:rand", MIB),
        CDIR + "/z_small": text("zs", 100),
        CDIR + "/z_one": b"Z",
        CDIR + "/z_empty": b"",
        CDIR + "/z_big": text("zb", 20 * MIB),
        CDIR + "/sub/z_nested": text("zn", 65536),
        "plain.txt": text("pl", 65536),
    }


def slurp(path):
    try:
        with open(path, "rb") as f:
            return f.read()
    except FileNotFoundError:
        return None


def attrib(path):
    v = xo.getx(path, ATTR)
    if isinstance(v, bytes) and len(v) == 4:
        return struct.unpack(">I", v)[0]
    return "errno %s" % v


def info(path):
    """size, hash, blocks and the compressed flag as the OS reports them."""
    b = slurp(path)
    a = attrib(path)
    st = os.stat(path)
    return {"size": st.st_size, "sha": sha(b), "blocks": st.st_blocks * 512,
            "attrib": a if isinstance(a, str) else "0x%x" % a,
            "compressed": bool(isinstance(a, int) and a & rl.FILE_COMPRESSED)}


def disk_info(img, names):
    view, _c = rl.disk_view(img, set(names))
    out = {}
    for nm, v in view.items():
        d = v["data"]
        out[nm] = {"attrs": "0x%x" % v["attrs"], "file_compressed": bool(v["attrs"] & rl.FILE_COMPRESSED),
                   "data": rl.fmt_data(d), "attr_compressed": bool(d and d["compressed"]),
                   "resident": bool(d and d["resident"])}
    return out


def _phase_ctx(env, work):
    c = core.Case(CASE_ID, (env,), None, "compressed files phase", None)
    return core.Ctx(c, env, work, os.path.join(work, "evidence"), transport.LocalTransport(), quiet=False)


def _os_string(t):
    r = t.run("sw_vers -productName && sw_vers -productVersion && sw_vers -buildVersion && uname -m")
    if r.rc == 0:
        n, v, b, a = r.out.decode().split()[:4]
        return "%s %s (%s) %s" % (n, v, b, a)
    r = t.run(". /etc/os-release 2>/dev/null && echo \"$PRETTY_NAME $(uname -m)\"")
    return r.out.decode().strip() if r.rc == 0 and r.out.strip() else "unknown"


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _b_volume(ctx):
    vol = hv.MacosNtfs3g(ctx, 0)
    os.makedirs(vol.mnt, exist_ok=True)
    ctx.add_cleanup(vol.cleanup)
    return vol


def tree_info(root, rels):
    return dict((r, info(os.path.join(root, r))) for r in rels if os.path.exists(os.path.join(root, r)))


# ---------------------------------------------------------------- phases

def phase_a1(work):
    ctx = _phase_ctx("A", work)
    try:
        vol = ctx.volume(IMG_MIB * MIB)
        fsops.mount(ctx, vol, extra="")
        m = vol.mnt
        doc = {"errors": {}}
        d = os.path.join(m, CDIR)
        os.mkdir(d)
        os.mkdir(os.path.join(d, "sub"))
        cur = attrib(d)
        doc["dir_attrib_before"] = cur if isinstance(cur, str) else "0x%x" % cur
        os.setxattr(d, ATTR, struct.pack(">I", (cur if isinstance(cur, int) else 0x10) | rl.FILE_COMPRESSED))
        os.setxattr(os.path.join(d, "sub"), ATTR, struct.pack(">I", 0x10 | rl.FILE_COMPRESSED))
        a = attrib(d)
        doc["dir_attrib_after"] = a if isinstance(a, str) else "0x%x" % a
        files = linux_files()
        for rel, data in files.items():
            with open(os.path.join(m, rel), "wb") as f:
                for i in range(0, len(data), 65536):
                    f.write(data[i:i + 65536])
        doc["files"] = tree_info(m, files)
        fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        doc["disk"] = disk_info(vol.img, [os.path.basename(r) for r in files])
        doc["image_sha256"] = sha256_file(vol.img)
        doc["ntfs3g_version"] = hv.detect_ntfs3g(ctx.transport)
        doc["os"] = _os_string(ctx.transport)
        return doc
    finally:
        ctx.close()


def phase_b(work):
    ctx = _phase_ctx("B", work)
    try:
        ver = hv.detect_macfuse(ctx.transport)
        missing = [x for x in ("ntfs-3g", "mkntfs", "ntfsfix") if hv.guest_tool(x) is None]
        if ver is None or missing:
            sys.stderr.write("SKIP: macFUSE %s, missing tools %s\n" % (ver, missing))
            return EXIT_SKIP, None
        vol = _b_volume(ctx)
        if not os.path.exists(vol.img) or os.path.getsize(vol.img) < MIB:
            raise RuntimeError("the image from Environment A is missing in the guest")
        doc = {"sha_in": sha256_file(vol.img), "ops": [], "session": {}}
        files = linux_files()
        state = dict(files)                      # rel -> bytes the file really holds now (None: gone)

        def run_op(label, rel, fn, expect):
            path = os.path.join(vol.mnt, rel)
            before = state.get(rel)
            try:
                fn(path)
                eno = 0
            except OSError as e:
                eno = e.errno
            actual = slurp(path)
            exp = expect(before) if expect else None
            consistent = (actual == exp) if eno == 0 else (actual == before)
            rec = {"label": label, "rel": rel, "errno": eno, "before": fmt(before), "after": fmt(actual),
                   "expected_if_ok": fmt(exp), "consistent": consistent}
            if eno == 0 and os.path.exists(path):
                rec["info"] = info(path)
            doc["ops"].append(rec)
            state[rel] = actual
            sys.stderr.write("op %s -> errno %d, %s\n" % (label, eno, "consistent" if consistent else "INCONSISTENT"))

        def pw(off, data):
            def f(path):
                fd = os.open(path, os.O_RDWR)
                try:
                    os.pwrite(fd, data, off)
                    os.fsync(fd)
                finally:
                    os.close(fd)
            return f

        def over(b, off, data):
            b = b + b"\0" * max(0, off - len(b))
            return b[:off] + data + b[off + len(data):]

        # session 1: compression on (the default)
        fsops.mount(ctx, vol, extra=rl.EXTRA_B)
        try:
            m = vol.mnt
            doc["read_in_b"] = tree_info(m, files)
            doc["dir_attrib_in_b"] = attrib(os.path.join(m, CDIR))
            if isinstance(doc["dir_attrib_in_b"], int):
                doc["dir_attrib_in_b"] = "0x%x" % doc["dir_attrib_in_b"]
            mid = blob("comp:b:mid", 70000)
            run_op("overwrite 70000 bytes in the middle of z_text", CDIR + "/z_text", pw(MIB + 5, mid),
                   lambda b: over(b, MIB + 5, mid))
            app = blob("comp:b:app", 300000)
            run_op("append 300000 random bytes to z_mixed", CDIR + "/z_mixed", pw(2 * MIB, app), lambda b: b + app)

            def trunc(n):
                return lambda path: os.truncate(path, n)
            run_op("truncate z_big to 5 MiB + 3", CDIR + "/z_big", trunc(5 * MIB + 3), lambda b: b[:5 * MIB + 3])
            run_op("truncate z_big up to 6 MiB", CDIR + "/z_big", trunc(6 * MIB), lambda b: b + b"\0" * (6 * MIB - len(b)))
            new = blob("comp:b:new", MIB)

            def rewrite(data):
                def f(path):
                    with open(path, "wb") as fh:
                        fh.write(data)
                return f
            run_op("rewrite z_rand (open O_TRUNC, write 1 MiB)", CDIR + "/z_rand", rewrite(new), lambda b: new)
            ten = b"0123456789"
            run_op("overwrite 10 bytes at 50 of z_small (100 B)", CDIR + "/z_small", pw(50, ten), lambda b: over(b, 50, ten))
            g = blob("comp:b:empty", 1000)
            run_op("write 1000 bytes into z_empty", CDIR + "/z_empty", rewrite(g), lambda b: g)
            run_op("append 10 bytes to z_one", CDIR + "/z_one", pw(1, ten), lambda b: b + ten)
            run_op("overwrite 4 bytes in z_nested", CDIR + "/sub/z_nested", pw(1000, b"ABCD"), lambda b: over(b, 1000, b"ABCD"))
            nt = text("new", MIB)
            run_op("create n_text (1 MiB text) in cdir", CDIR + "/n_text", rewrite(nt), lambda b: nt)
            run_op("create n_plain (1 MiB text) beside cdir", "n_plain", rewrite(nt), lambda b: nt)

            def mksub(path):
                os.mkdir(path)
                with open(os.path.join(path, "inner"), "wb") as fh:
                    fh.write(nt)
            run_op("mkdir cdir/newsub and create a file in it", CDIR + "/newsub/inner",
                   lambda path: mksub(os.path.dirname(path)), lambda b: nt)

            def rename(path):
                os.rename(path, path + "_renamed")
            nested = CDIR + "/sub/z_nested"
            old_content = state[nested]
            run_op("rename z_nested", nested, rename, None)
            renamed = slurp(os.path.join(vol.mnt, nested + "_renamed"))
            if doc["ops"][-1]["errno"] == 0 and renamed != old_content:
                doc["ops"][-1]["consistent"] = False
            state[nested + "_renamed"] = renamed
            run_op("unlink z_one", CDIR + "/z_one", lambda path: os.unlink(path), lambda b: None)
            doc["session"]["after_1"] = tree_info(m, [r for r, b in state.items() if b is not None])
        finally:
            fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        doc["disk_after_1"] = disk_info(vol.img, [os.path.basename(r) for r, b in state.items() if b is not None])
        # session 2: nocompression (existing compressed files can still be read and updated)
        fsops.mount(ctx, vol, extra=rl.EXTRA_B + ",nocompression")
        try:
            mid2 = blob("comp:b:mid2", MIB)
            run_op("[nocompression] overwrite 1 MiB at 500000 of z_mixed", CDIR + "/z_mixed", pw(500000, mid2),
                   lambda b: over(b, 500000, mid2))
            nt2 = text("nc", 512 * 1024)
            run_op("[nocompression] create n2_text in cdir", CDIR + "/n2_text", rewrite(nt2), lambda b: nt2)
            doc["session"]["after_2"] = tree_info(vol.mnt, [r for r, b in state.items() if b is not None])
        finally:
            fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        doc["disk_after_2"] = disk_info(vol.img, [os.path.basename(r) for r, b in state.items() if b is not None])
        doc["final"] = dict((r, fmt(b)) for r, b in state.items() if b is not None)
        doc["image_sha256"] = sha256_file(vol.img)
        doc["os"] = _os_string(ctx.transport)
        doc["macfuse_version"] = hv.detect_macfuse(ctx.transport)
        doc["ntfs3g_version"] = hv.detect_ntfs3g(ctx.transport)
        return 0, doc
    finally:
        ctx.close()


def phase_a2(work, final):
    ctx = _phase_ctx("A", work)
    try:
        vol = hv.LinuxNtfs3g(ctx, 0)
        os.makedirs(vol.mnt, exist_ok=True)
        ctx.add_cleanup(vol.cleanup)
        doc = {"image_sha256": sha256_file(vol.img)}
        fix = ctx.sh("ntfsfix -n %s" % shlex.quote(vol.img), check=False)
        doc["ntfsfix_rc"] = fix.rc
        doc["disk"] = disk_info(vol.img, [os.path.basename(r) for r in final])
        fsops.mount(ctx, vol, rw=False, extra="")
        try:
            doc["read_in_a"] = tree_info(vol.mnt, final)
        finally:
            fsops.unmount(ctx, vol)
        # Linux updates a file the guest wrote
        fsops.mount(ctx, vol, extra="")
        try:
            p = os.path.join(vol.mnt, CDIR + "/z_mixed")
            before = slurp(p)
            add = blob("comp:a2:add", 200000)
            with open(p, "ab") as f:
                f.write(add)
            ok = slurp(p) == before + add
            doc["a2_update"] = {"ok": ok, "info": info(p)}
        finally:
            fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        doc["os"] = _os_string(ctx.transport)
        doc["ntfs3g_version"] = hv.detect_ntfs3g(ctx.transport)
        return doc
    finally:
        ctx.close()


# ---------------------------------------------------------------- judge (pure)

def judge(a1, b, a2):
    bad, facts = [], []
    if b["sha_in"] != a1["image_sha256"]:
        bad.append("image changed on the way to the guest")
    if b["image_sha256"] != a2["image_sha256"]:
        bad.append("image changed on the way back to Linux")
    if a2["ntfsfix_rc"] != 0:
        bad.append("ntfsfix -n on the final image exited %d" % a2["ntfsfix_rc"])
    # what Linux made
    comp = dict((r, i["compressed"]) for r, i in a1["files"].items())
    facts.append("Linux: directory attributes %s -> %s; compressed flag per file %s" % (
        a1["dir_attrib_before"], a1["dir_attrib_after"], sorted((r, c) for r, c in comp.items())))
    for r in a1["files"]:
        if r.startswith(CDIR + "/") and not comp[r] and a1["files"][r]["size"] > 100:
            bad.append("Linux did not compress %s although its directory is marked" % r)
    if comp.get("plain.txt"):
        bad.append("plain.txt beside the compressed directory is compressed")
    for r, i in a1["files"].items():
        facts.append("Linux %s: %d bytes, %d allocated (%.0f%%)" % (r, i["size"], i["blocks"], 100.0 * i["blocks"] / max(1, i["size"])))
    # read in B equals Linux
    for r, i in a1["files"].items():
        got = b["read_in_b"].get(r)
        if got is None:
            bad.append("%s made on Linux is missing on macOS" % r)
        elif got["sha"] != i["sha"] or got["size"] != i["size"]:
            bad.append("%s made on Linux reads differently on macOS: %d bytes %s, expected %d bytes %s" % (
                r, got["size"], got["sha"][:12], i["size"], i["sha"][:12]))
        else:
            facts.append("macOS reads %s equal; blocks %d (Linux %d), compressed flag %s" % (r, got["blocks"], i["blocks"], got["attrib"]))
    facts.append("macOS reads the directory's attributes as %s" % b["dir_attrib_in_b"])
    # operations in B
    for op in b["ops"]:
        word = "accepted" if op["errno"] == 0 else "refused (errno %d)" % op["errno"]
        extra = ""
        if op.get("info"):
            extra = "; now %d bytes, %d allocated, attributes %s" % (op["info"]["size"], op["info"]["blocks"], op["info"]["attrib"])
        facts.append("B op '%s': %s%s" % (op["label"], word, extra))
        if not op["consistent"]:
            bad.append("B op '%s' on %s: errno %d, content is neither the old nor the expected new one (before %s, after %s, expected %s)" % (
                op["label"], op["rel"], op["errno"], op["before"], op["after"], op["expected_if_ok"]))
    # on-disk flags of what B wrote
    for label in ("disk_after_1", "disk_after_2"):
        for nm, d in sorted(b[label].items()):
            facts.append("%s %s: %s; attributes %s" % (label, nm, d["data"], d["attrs"]))
    # Linux reads B's final state
    for r, f in b["final"].items():
        got = a2["read_in_a"].get(r)
        if got is None:
            bad.append("%s is missing on Linux after macOS" % r)
            continue
        if "%d:%s" % (got["size"], got["sha"]) != f:
            bad.append("%s: Linux reads %d bytes %s, the guest left %s" % (r, got["size"], got["sha"][:12], f))
    extra_files = sorted(set(a2["read_in_a"]) - set(b["final"]))
    if extra_files:
        bad.append("files on Linux that the guest does not have: %s" % extra_files)
    for r, i in sorted(a2["read_in_a"].items()):
        facts.append("Linux after B, %s: %d bytes, %d allocated, compressed %s" % (r, i["size"], i["blocks"], i["compressed"]))
    if not a2["a2_update"]["ok"]:
        bad.append("Linux could not append to z_mixed after macOS wrote it")
    return bad, facts


# ---------------------------------------------------------------- the case (host side)

def _tart_sh():
    return os.path.join(REPO, "vm", "macos", "tart.sh")


def _jdump(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, sort_keys=True)


@case(CASE_ID, envs=("host",), title="NTFS-compressed files made on Linux: read in B equal, updates in B, read again on Linux")
def i_big_comp_01(ctx):
    vm_name = os.environ.get("NTFSMAC_B_VM")
    if not vm_name:
        ctx.skip("NTFSMAC_B_VM is not set: this case drives a running Environment B guest with NTFS-3G built "
                 "in it; run tests/harness/run-env-b.sh --orchestrate --cases I-BIG-COMP-01")
    vm = transport.VmTransport(vm_name, _tart_sh())
    linux = transport.ContainerTransport(os.path.join(REPO, "vm", "linux", "run.sh"))
    d = vm.run("mktemp -d /tmp/ntfsmac-comp.XXXXXX")
    if d.rc != 0:
        raise RuntimeError("cannot reach the guest: %s" % d.err.decode("utf-8", "replace"))
    gdir = d.out.decode().strip()
    ctx.add_cleanup(lambda: vm.run("rm -rf %s" % shlex.quote(gdir)))
    vm.put_tree(TESTS, SHIP, gdir)
    build_root = os.path.join(REPO, "build", "bigcomp")
    os.makedirs(build_root, exist_ok=True)
    hdir = tempfile.mkdtemp(prefix="run-", dir=build_root)
    ctx.add_cleanup(lambda: shutil.rmtree(hdir, ignore_errors=True))
    img1, img2 = os.path.join(hdir, "a.img"), os.path.join(hdir, "b.img")
    prog = "integration/special/comp_a_after_b.py"

    tar_path = os.path.join(hdir, "a1.tar")
    r = linux.run_to_file("rm -rf /tmp/bigcomp && mkdir -p /tmp/bigcomp && PYTHONDONTWRITEBYTECODE=1 python3 /work/tests/%s "
                          "phase-a1 --dir /tmp/bigcomp 1>&2 && tar -cf - -C /tmp/bigcomp vol.img a1.json" % prog, tar_path, timeout=900)
    ctx.log("phase A1 -> rc=%d\n%s" % (r.rc, r.err.decode("utf-8", "replace")[-3000:]))
    if r.rc != 0:
        raise RuntimeError("phase A1 failed (rc=%d)" % r.rc)
    with open(tar_path, "rb") as f:
        transport.extract_tar(f.read(), os.path.join(hdir, "a1"))
    os.remove(tar_path)
    shutil.move(os.path.join(hdir, "a1", "vol.img"), img1)
    with open(os.path.join(hdir, "a1", "a1.json"), "r", encoding="utf-8") as f:
        a1 = json.load(f)
    if sha256_file(img1) != a1["image_sha256"]:
        raise Fail("image changed in transit container -> host")

    py = "$(if [ -x /usr/bin/python3 ]; then echo /usr/bin/python3; else echo python3; fi)"
    cmd = ("cat > %s/vol.img && PYTHONDONTWRITEBYTECODE=1 %s %s %s/%s phase-b --dir %s" % (gdir, GUEST_PATH, py, gdir, prog, gdir))
    r = vm.run_from_file(cmd, img1, timeout=1800)
    ctx.log("phase B -> rc=%d\n%s" % (r.rc, r.err.decode("utf-8", "replace")[-6000:]))
    if r.rc == EXIT_SKIP:
        ctx.skip("phase B skipped in the guest: %s" % r.err.decode("utf-8", "replace").strip().splitlines()[-1])
    if r.rc != 0:
        raise RuntimeError("phase B failed in the guest (rc=%d)" % r.rc)
    b = json.loads(r.out.decode("utf-8"))
    ctx.meta.update({"guest_os": b["os"], "macfuse_version": b["macfuse_version"], "ntfs3g_version": b["ntfs3g_version"]})
    r = vm.run_to_file("cat %s/vol.img" % gdir, img2, timeout=900)
    if r.rc != 0:
        raise RuntimeError("copying the image out of the guest failed")
    if sha256_file(img2) != b["image_sha256"]:
        raise Fail("image changed in transit guest -> host")

    final = json.dumps(sorted(b["final"]))
    r = linux.run_from_file("rm -rf /tmp/bigcomp && mkdir -p /tmp/bigcomp && cat > /tmp/bigcomp/vol.img && "
                            "PYTHONDONTWRITEBYTECODE=1 python3 /work/tests/%s phase-a2 --dir /tmp/bigcomp --final %s" % (
                                prog, shlex.quote(final)), img2, timeout=900)
    ctx.log("phase A2 -> rc=%d\n%s" % (r.rc, r.err.decode("utf-8", "replace")[-3000:]))
    if r.rc != 0:
        raise RuntimeError("phase A2 failed (rc=%d)" % r.rc)
    a2 = json.loads(r.out.decode("utf-8"))
    for p in (img1, img2):
        os.remove(p)

    _jdump(ctx.evidence_file("phase-a1.json"), a1)
    _jdump(ctx.evidence_file("phase-b.json"), b)
    _jdump(ctx.evidence_file("phase-a2.json"), a2)
    bad, facts = judge(a1, b, a2)
    for f_ in facts:
        ctx.log("fact: " + f_)
    ctx.log("Linux: NTFS-3G %s on %s; macOS: %s, macFUSE %s, NTFS-3G %s" % (
        a1["ntfs3g_version"], a1["os"], b["os"], b["macfuse_version"], b["ntfs3g_version"]))
    if bad:
        for x in bad[:40]:
            ctx.log("MISMATCH " + x)
        raise Fail("%d observations differ from the expected result, first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: compressed files made on Linux read equal in B; no update left a file in a mixed state; "
            "Linux reads the guest's final state equal")


def main(argv):
    p = argparse.ArgumentParser(prog="comp_a_after_b.py", description="I-BIG-COMP-01 phase programs (internal)")
    sub = p.add_subparsers(dest="phase", required=True)
    for name in ("phase-a1", "phase-b", "phase-a2"):
        s = sub.add_parser(name)
        s.add_argument("--dir", required=True)
        if name == "phase-a2":
            s.add_argument("--final", required=True)
    args = p.parse_args(argv)
    work = os.path.abspath(args.dir)
    os.makedirs(work, exist_ok=True)
    if args.phase == "phase-a1":
        doc = phase_a1(work)
        with open(os.path.join(work, "a1.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        return 0
    if args.phase == "phase-a2":
        sys.stdout.write(json.dumps(phase_a2(work, json.loads(args.final))))
        return 0
    rc, doc = phase_b(work)
    if rc != 0:
        return rc
    sys.stdout.write(json.dumps(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
