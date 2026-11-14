# SPDX-License-Identifier: GPL-2.0-or-later
"""I-XATTR-09 (T-M3-08): xattrs written on macOS are NTFS named streams on Linux, and the other way round.

Host case driving both environments, like I-NAME-U22 (u22_names.py). The image makes three hops:

  phase A1 (Linux container, NTFS-3G there; `a_after_b.py phase-a1`)
      mkntfs, mount with the default interface (streams_interface=xattr, names as user.*), write streams the
      way Windows makes them (Zone.Identifier) and a plain one on a file and a directory, unmount, ntfsfix -n.
  host: image into the guest.
  phase B (macOS guest, `a_after_b.py phase-b`)
      mount with the product options (windows_names,noatime,streams_interface=openxattr); read what Linux
      wrote; write Apple xattrs (quarantine, FinderInfo, ResourceFork, an all-zero FinderInfo, lastuseddate,
      user.*, a 64 KiB and a 200000-byte value) on files and a directory, `ditto` a file with xattrs from
      APFS; add one to a Linux-made file; record every xattr map; unmount, ntfsfix -n.
  host: image back into the container.
  phase A2 (Linux; `a_after_b.py phase-a2`)
      read-only mounts with both interfaces: the names Linux lists, the values it reads; then
      `ntfscat -a DATA -n NAME` on the unmounted image, which reads the stream itself.
  host: judge. Expected: the stream names on disk are exactly the xattr names macOS used; the default Linux
      interface lists them as user.<name>; values are byte-identical; Linux-made streams show on macOS under
      their stream name; a value above 64 KiB is listed by Linux but cannot be read through the Linux xattr
      API (kernel limit), only by ntfscat; an all-zero FinderInfo is stored as 32 zero bytes.

Run (clones the VM, builds NTFS-3G in it, always deletes the VM):
    tests/harness/run-env-b.sh --orchestrate --cases I-XATTR-09
Without NTFSMAC_B_VM the case skips with a note.
"""

import argparse
import hashlib
import json
import os
import shlex
import shutil
import sys
import tempfile
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(os.path.dirname(HERE))
REPO = os.path.dirname(TESTS)
for p in (TESTS, os.path.join(TESTS, "integration", "functional"), HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import fsops  # noqa: E402
import xattr_ops as xo  # noqa: E402
from harness import core, transport, volume as hv  # noqa: E402
from harness.core import Fail, case  # noqa: E402

CASE_ID = "I-XATTR-09"
SHIP = ("harness", "integration/functional", "integration/xattr")
GUEST_PATH = 'PATH="$HOME/ntfsmac/deps/bin:$HOME/ntfsmac/deps/sbin:$PATH"'
EXIT_SKIP = 3
IMG_MIB = 128
ZONE = b"[ZoneTransfer]\r\nZoneId=3\r\n"
TEXT_ZONE = "zone.txt"


def fmt(v):
    return "%d:%s" % (len(v), hashlib.sha256(v).hexdigest())


def nfc(s):
    return unicodedata.normalize("NFC", s)


def mapfmt(path):
    """{name (NFC): 'len:sha256'} of the xattrs of path; unreadable values as 'errno N'."""
    out = {}
    names = xo.listx(path)
    if isinstance(names, int):
        return {"<list>": "errno %d" % names}
    for n in names:
        if n.startswith("system."):
            continue
        v = xo.getx(path, n)
        out[nfc(n)] = fmt(v) if isinstance(v, bytes) else "errno %d" % v
    return out


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def _phase_ctx(env, work):
    c = core.Case(CASE_ID, (env,), None, "xattr phase", None)
    return core.Ctx(c, env, work, os.path.join(work, "evidence"), transport.LocalTransport(), quiet=False)


def _os_string(t):
    r = t.run("sw_vers -productName && sw_vers -productVersion && sw_vers -buildVersion && uname -m")
    if r.rc == 0:
        n, v, b, a = r.out.decode().split()[:4]
        return "%s %s (%s) %s" % (n, v, b, a)
    r = t.run(". /etc/os-release 2>/dev/null && echo \"$PRETTY_NAME $(uname -m)\"")
    return r.out.decode().strip() if r.rc == 0 and r.out.strip() else "unknown"


# What the macOS phase writes: path -> {name: value}
UNI_NAME = "user.na\u00efve"  # typed in NFC; macOS lists it in NFD, the question is what NTFS stores
UNI_FILE = "uni.txt"


def raw_nonascii(path):
    """Hex of the raw bytes of every listed xattr name that has a non-ASCII character."""
    names = xo.listx(path)
    return sorted(n.encode("utf-8").hex() for n in names if isinstance(names, list) and any(ord(c) > 127 for c in n))


def mac_files():
    big64 = xo.blob("x09:64k", 64 * 1024)
    huge = xo.blob("x09:huge", 200000)
    return {
        "mac.txt": {
            "user.k": b"v1",
            "com.apple.quarantine": xo.QUAR,
            "com.apple.FinderInfo": xo.FI_RED,
            "com.apple.ResourceFork": xo.blob("x09:rf", 3000),
            "com.apple.lastuseddate#PS": b"\x02" * 16,
            "user.big": big64,
            "user.huge": huge,
        },
        "zero.txt": {"com.apple.FinderInfo": xo.FI_ZERO},
        "macdir": {"com.apple.FinderInfo": bytes([0] * 9 + [0x0A] + [0] * 21 + [0xFF]), "user.dk": b"dir"},
    }


# ---------------------------------------------------------------- phases

def phase_a1(work):
    ctx = _phase_ctx("A", work)
    try:
        vol = ctx.volume(IMG_MIB * 1024 * 1024)
        fsops.mount(ctx, vol, extra="streams_interface=xattr")
        m = vol.mnt
        z = os.path.join(m, TEXT_ZONE)
        with open(z, "wb") as f:
            f.write(b"downloaded on Linux")
        for n, v in (("user.Zone.Identifier", ZONE), ("user.note", b"made on linux")):
            e = xo.setx(z, n, v)
            if e:
                raise RuntimeError("Linux set %s: errno %d" % (n, e))
        d = os.path.join(m, "ldir")
        os.mkdir(d)
        xo.setx(d, "user.dnote", b"dir stream")
        doc = {"maps": {TEXT_ZONE: mapfmt(z), "ldir": mapfmt(d)}}
        fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        doc["image_sha256"] = sha256_file(vol.img)
        doc["ntfs3g_version"] = hv.detect_ntfs3g(ctx.transport)
        doc["os"] = _os_string(ctx.transport)
        return doc
    finally:
        ctx.close()


def _b_volume(ctx):
    vol = hv.MacosNtfs3g(ctx, 0)
    os.makedirs(vol.mnt, exist_ok=True)
    ctx.add_cleanup(vol.cleanup)
    return vol


def phase_b(work):
    ctx = _phase_ctx("B", work)
    try:
        ver = hv.detect_macfuse(ctx.transport)
        missing = [x for x in ("ntfs-3g", "mkntfs", "ntfsfix") if hv.guest_tool(x) is None]
        if ver is None or missing:
            sys.stderr.write("SKIP: macFUSE %s, missing tools %s\n" % (ver, missing))
            return EXIT_SKIP, None
        vol = _b_volume(ctx)
        if not os.path.exists(vol.img) or os.path.getsize(vol.img) < 1024 * 1024:
            raise RuntimeError("the image from Environment A is missing in the guest")
        doc = {"sha_in": sha256_file(vol.img)}
        fsops.mount(ctx, vol, extra=xo.PRODUCT)
        try:
            m = vol.mnt
            z = os.path.join(m, TEXT_ZONE)
            doc["linux_made_seen_on_mac"] = {TEXT_ZONE: mapfmt(z), "ldir": mapfmt(os.path.join(m, "ldir"))}
            doc["errors"] = {}
            for rel, attrs in mac_files().items():
                p = os.path.join(m, rel)
                if rel == "macdir":
                    os.mkdir(p)
                else:
                    with open(p, "wb") as f:
                        f.write(b"made on macOS: " + rel.encode())
                for n, v in attrs.items():
                    e = xo.setx(p, n, v)
                    if e:
                        doc["errors"]["%s %s" % (rel, n)] = e
            u = os.path.join(m, UNI_FILE)
            with open(u, "wb") as f:
                f.write(b"unicode xattr name")
            e = xo.setx(u, UNI_NAME, b"u")
            if e:
                doc["errors"]["uni.txt name"] = e
            doc["uni_raw_names_on_mac"] = raw_nonascii(u)
            # a file with xattrs copied from APFS by ditto
            src = os.path.join(work, "apfs-src.txt")
            with open(src, "wb") as f:
                f.write(b"apfs source")
            for n, v in (("user.k", b"v1"), ("com.apple.quarantine", xo.QUAR), ("com.apple.FinderInfo", xo.FI_RED)):
                xo.setx(src, n, v)
            rc, out = xo.run(["/usr/bin/ditto", src, os.path.join(m, "ditto.txt")])
            doc["ditto"] = {"rc": rc, "out": out}
            # an xattr added to a Linux-made file
            e = xo.setx(z, "com.apple.quarantine", xo.QUAR)
            if e:
                doc["errors"]["zone.txt quarantine"] = e
            doc["maps"] = dict((rel, mapfmt(os.path.join(m, rel)))
                               for rel in ("mac.txt", "zero.txt", "macdir", "ditto.txt", TEXT_ZONE, "ldir", UNI_FILE))
            doc["appledouble"] = xo.appledouble_files(m)
        finally:
            fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        doc["image_sha256"] = sha256_file(vol.img)
        doc["os"] = _os_string(ctx.transport)
        doc["macfuse_version"] = hv.detect_macfuse(ctx.transport)
        doc["ntfs3g_version"] = hv.detect_ntfs3g(ctx.transport)
        return 0, doc
    finally:
        ctx.close()


def phase_a2(work):
    ctx = _phase_ctx("A", work)
    try:
        vol = hv.LinuxNtfs3g(ctx, 0)
        os.makedirs(vol.mnt, exist_ok=True)
        ctx.add_cleanup(vol.cleanup)
        doc = {"image_sha256": sha256_file(vol.img)}
        fix = ctx.sh("ntfsfix -n %s" % shlex.quote(vol.img), check=False)
        doc["ntfsfix_rc"] = fix.rc
        paths = ["mac.txt", "zero.txt", "macdir", "ditto.txt", TEXT_ZONE, "ldir", UNI_FILE]
        for label, extra in (("openxattr", "streams_interface=openxattr"), ("default", "streams_interface=xattr")):
            fsops.mount(ctx, vol, rw=False, extra=extra)
            try:
                doc[label] = dict((rel, mapfmt(os.path.join(vol.mnt, rel))) for rel in paths)
                doc[label + "_listing"] = sorted(n for n in os.listdir(vol.mnt))
                doc[label + "_uni_raw_names"] = raw_nonascii(os.path.join(vol.mnt, UNI_FILE))
            finally:
                fsops.unmount(ctx, vol)
        # ntfscat on the unmounted image: reads each stream by its on-disk name
        ntfscat = {}
        for rel in paths:
            for name in doc["openxattr"][rel]:
                if rel == UNI_FILE:
                    continue
                r = ctx.transport.run("ntfscat -a DATA -n %s %s /%s" % (shlex.quote(name), shlex.quote(vol.img), shlex.quote(rel)))
                ntfscat["%s %s" % (rel, name)] = fmt(r.out) if r.rc == 0 else "rc %d" % r.rc
        doc["ntfscat"] = ntfscat
        doc["ntfs3g_version"] = hv.detect_ntfs3g(ctx.transport)
        doc["os"] = _os_string(ctx.transport)
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
    if b["errors"]:
        bad.append("macOS xattr writes failed: %s" % b["errors"])
    if b["ditto"]["rc"] != 0:
        bad.append("ditto of a file with xattrs onto the volume failed: %s" % b["ditto"])
    if b["appledouble"]:
        bad.append("._* files on the volume: %s" % b["appledouble"])
    # Linux-made streams as macOS sees them: stream name without the user. prefix
    for rel, m_linux in a1["maps"].items():
        want = dict((n[len("user."):], v) for n, v in m_linux.items())
        if b["linux_made_seen_on_mac"][rel] != want:
            bad.append("Linux-made streams of %s on macOS: %s, expected %s" % (rel, b["linux_made_seen_on_mac"][rel], want))
    facts.append("Linux wrote user.Zone.Identifier and user.note; macOS (openxattr) lists %s" % sorted(b["linux_made_seen_on_mac"][TEXT_ZONE]))
    # macOS-written names and values, as Linux reads them through the openxattr interface
    for rel in ("mac.txt", "zero.txt", "macdir", "ditto.txt", TEXT_ZONE, "ldir", UNI_FILE):
        mac = b["maps"][rel]
        lin = a2["openxattr"][rel]
        # The macOS read-back hides an all-zero FinderInfo (getxattr says ENOATTR): it is listed, not readable.
        mac_cmp = dict(mac)
        lin_cmp = dict(lin)
        if rel == "zero.txt":
            facts.append("zero.txt: macOS read-back %s; Linux sees %s" % (mac.get("com.apple.FinderInfo"), lin.get("com.apple.FinderInfo")))
            mac_cmp.pop("com.apple.FinderInfo", None)
            lin_cmp.pop("com.apple.FinderInfo", None)
            if lin.get("com.apple.FinderInfo") != fmt(xo.FI_ZERO):
                bad.append("zero.txt: the all-zero FinderInfo is not 32 zero bytes on Linux: %s" % lin.get("com.apple.FinderInfo"))
        # user.huge is above the Linux xattr limit: listed, unreadable by getxattr, readable by ntfscat
        if "user.huge" in lin_cmp:
            if not str(lin_cmp["user.huge"]).startswith("errno"):
                bad.append("%s user.huge: Linux read a 200000-byte value (%s)" % (rel, lin_cmp["user.huge"]))
            else:
                facts.append("%s: the 200000-byte value is listed on Linux, getxattr fails with %s" % (rel, lin_cmp["user.huge"]))
            lin_cmp.pop("user.huge")
            mac_cmp.pop("user.huge", None)
        if mac_cmp != lin_cmp:
            bad.append("%s: names and values differ between macOS and Linux (openxattr): macOS %s, Linux %s" % (rel, mac_cmp, lin_cmp))
    # the default Linux interface lists the same streams as user.<name>
    for rel, lin in a2["openxattr"].items():
        want = sorted("user." + n for n in lin)
        got = sorted(a2["default"][rel])
        if got != want:
            bad.append("%s: default Linux interface lists %s, expected %s" % (rel, got, want))
    nfc_hex = nfc(UNI_NAME).encode("utf-8").hex()
    nfd_hex = unicodedata.normalize("NFD", UNI_NAME).encode("utf-8").hex()
    on_disk = a2["openxattr_uni_raw_names"]
    facts.append("non-ASCII xattr name: macOS lists %s; Linux (openxattr) lists %s; NFC is %s, NFD is %s" % (
        b["uni_raw_names_on_mac"], on_disk, nfc_hex, nfd_hex))
    if on_disk not in ([nfc_hex], [nfd_hex]):
        bad.append("non-ASCII xattr name on Linux is neither NFC nor NFD: %s" % on_disk)
    facts.append("default Linux interface, mac.txt: %s" % sorted(a2["default"]["mac.txt"]))
    # ntfscat reads every stream by its on-disk name, byte for byte what macOS wrote
    for rel, attrs in mac_files().items():
        for n, v in attrs.items():
            got = a2["ntfscat"].get("%s %s" % (rel, n))
            if got != fmt(v):
                bad.append("ntfscat %s %s: %s, expected %s" % (rel, n, got, fmt(v)[:20]))
    for key, got in a2["ntfscat"].items():
        if got.startswith("rc "):
            bad.append("ntfscat %s: %s" % (key, got))
    return bad, facts


# ---------------------------------------------------------------- the case (host side)

def _tart_sh():
    return os.path.join(REPO, "vm", "macos", "tart.sh")


def _jdump(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, sort_keys=True)


@case(CASE_ID, envs=("host",), title="xattrs written on macOS are NTFS named streams on Linux; Linux-made streams are xattrs on macOS")
def i_xattr_09(ctx):
    vm_name = os.environ.get("NTFSMAC_B_VM")
    if not vm_name:
        ctx.skip("NTFSMAC_B_VM is not set: this case drives a running Environment B guest with NTFS-3G built "
                 "in it; run tests/harness/run-env-b.sh --orchestrate --cases I-XATTR-09")
    vm = transport.VmTransport(vm_name, _tart_sh())
    linux = transport.ContainerTransport(os.path.join(REPO, "vm", "linux", "run.sh"))
    d = vm.run("mktemp -d /tmp/ntfsmac-x09.XXXXXX")
    if d.rc != 0:
        raise RuntimeError("cannot reach the guest: %s" % d.err.decode("utf-8", "replace"))
    gdir = d.out.decode().strip()
    ctx.add_cleanup(lambda: vm.run("rm -rf %s" % shlex.quote(gdir)))
    vm.put_tree(TESTS, SHIP, gdir)
    build_root = os.path.join(REPO, "build", "x09")
    os.makedirs(build_root, exist_ok=True)
    hdir = tempfile.mkdtemp(prefix="run-", dir=build_root)
    ctx.add_cleanup(lambda: shutil.rmtree(hdir, ignore_errors=True))
    img1, img2 = os.path.join(hdir, "a.img"), os.path.join(hdir, "b.img")
    prog = "integration/xattr/a_after_b.py"

    tar_path = os.path.join(hdir, "a1.tar")
    r = linux.run_to_file("rm -rf /tmp/x09 && mkdir -p /tmp/x09 && PYTHONDONTWRITEBYTECODE=1 python3 /work/tests/%s "
                          "phase-a1 --dir /tmp/x09 1>&2 && tar -cf - -C /tmp/x09 vol.img a1.json" % prog, tar_path, timeout=900)
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

    r = linux.run_from_file("rm -rf /tmp/x09 && mkdir -p /tmp/x09 && cat > /tmp/x09/vol.img && "
                            "PYTHONDONTWRITEBYTECODE=1 python3 /work/tests/%s phase-a2 --dir /tmp/x09" % prog, img2, timeout=900)
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
        raise Fail("%d observations differ from the expected stream mapping, first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: stream names on disk are the xattr names macOS used, values are byte-identical on Linux")


def main(argv):
    p = argparse.ArgumentParser(prog="a_after_b.py", description="I-XATTR-09 phase programs (internal)")
    sub = p.add_subparsers(dest="phase", required=True)
    for name in ("phase-a1", "phase-b", "phase-a2"):
        s = sub.add_parser(name)
        s.add_argument("--dir", required=True)
    args = p.parse_args(argv)
    work = os.path.abspath(args.dir)
    os.makedirs(work, exist_ok=True)
    if args.phase == "phase-a1":
        doc = phase_a1(work)
        with open(os.path.join(work, "a1.json"), "w", encoding="utf-8") as f:
            json.dump(doc, f)
        return 0
    if args.phase == "phase-a2":
        sys.stdout.write(json.dumps(phase_a2(work)))
        return 0
    rc, doc = phase_b(work)
    if rc != 0:
        return rc
    sys.stdout.write(json.dumps(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
