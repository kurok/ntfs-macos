# SPDX-License-Identifier: GPL-2.0-or-later
"""I-NAME-U22 (T-M3-05, ARCHITECTURE.md U22, R-27): names that differ only in Unicode normalisation.

Host case driving both environments, like C-REF-01. The image makes three hops:

  phase A1 (Linux container, NTFS-3G there; `u22_names.py phase-a1`)
      mkntfs, write groups of names that are canonically equivalent but different byte strings
      (NFC/NFD of café, a three-way Angstrom group, Hangul, kana, combining marks in two orders,
      a directory pair, an NFD-only name), unmount, ntfsfix -n, SHA-256.
  host: image into the guest.
  phase B (macOS guest, `u22_names.py phase-b`)
      1. default mount (NTFS-3G's Darwin nfconv on): raw listing; lookup of every spelling of every group.
      2. mount with `nonfconv`: the same.
      3. default and nonfconv mounts with `debug`: the paths the worker receives for those lookups.
      4. read/write default mount: create names on the Mac in NFC and in NFD form (files and
         directories), then unlink the listed NFD name of the café pair (what does it delete?).
      unmount, SHA-256 of the image.
  host: image back into the container.
  phase A2 (Linux; `u22_names.py phase-a2`)
      read-only mount, raw bytes of every name on disk (and treedump), ntfsfix -n.
  host: judge. The recorded rule (U22) is that the converting layer is NTFS-3G (nfconv); each
      observation is compared with what that rule predicts, and any difference fails the case.

Run (clones the VM, builds NTFS-3G in it, always deletes the VM):
    tests/harness/run-env-b.sh --orchestrate --cases I-NAME-U22
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
for p in (TESTS, HERE):
    if p not in sys.path:
        sys.path.insert(0, p)

import fsops  # noqa: E402
from harness import core, transport, volume as hv  # noqa: E402
from harness.core import Fail, case  # noqa: E402

CASE_ID = "I-NAME-U22"
SHIP = ("harness", "integration/functional", "tools")
GUEST_PATH = 'PATH="$HOME/ntfsmac/deps/bin:$HOME/ntfsmac/deps/sbin:$PATH"'
EXIT_SKIP = 3
IMG_MIB = 64

NFC = lambda s: unicodedata.normalize("NFC", s)  # noqa: E731
NFD = lambda s: unicodedata.normalize("NFD", s)  # noqa: E731

# (group id, kind f|d, [variant spellings]); each variant is written as its own entry.
GROUPS = [
    ("g1", "f", ["g1-café", "g1-café"]),
    ("g2", "f", ["g2-Å", "g2-Å", "g2-Å"]),
    ("g3", "f", ["g3-한", "g3-한"]),
    ("g4", "f", ["g4-が", "g4-が"]),
    ("g5", "f", ["g5-q̣̇", "g5-q̣̇"]),
    ("g6", "d", ["g6-é", "g6-é"]),
    ("g7", "f", ["g7-plain.txt"]),
    ("g8", "f", ["g8-ü-nfc-only"]),
    ("g9", "f", [NFD("g9-ü-nfd-only")]),
]
MAC_STRINGS = ["café", "Å", "한", "が", "q̣̇"]


def payload(spelling):
    return ("content:" + spelling.encode("utf-8").hex()).encode()


def _seen(root, spelling, kind):
    """Lookup spelling (str) below root (bytes path): content or 'ERRNO n'."""
    p = os.path.join(root, spelling.encode("utf-8"))
    if kind == "d":
        p = os.path.join(p, b"inner.txt")
    try:
        with open(p, "rb") as f:
            return f.read().decode("latin-1")
    except OSError as e:
        return "ERRNO%d" % e.errno


def _root_listing(root):
    return sorted(n.hex() for n in os.listdir(root) if n != b".fseventsd")


def _lookup_all(root):
    out = {}
    for gid, kind, variants in GROUPS:
        spellings = []
        for v in variants:
            for s in (v, NFC(v), NFD(v)):
                if s not in spellings:
                    spellings.append(s)
        out[gid] = dict((s.encode("utf-8").hex(), _seen(root, s, kind)) for s in spellings)
    return out


def _sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(1 << 20)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


# ---------------------------------------------------------------- phases

def _phase_ctx(env, work):
    c = core.Case(CASE_ID, (env,), None, "u22 phase", None)
    return core.Ctx(c, env, work, os.path.join(work, "evidence"), transport.LocalTransport(), quiet=False)


def _os_string(t):
    r = t.run("sw_vers -productName && sw_vers -productVersion && sw_vers -buildVersion && uname -m")
    if r.rc == 0:
        n, v, b, a = r.out.decode().split()[:4]
        return "%s %s (%s) %s" % (n, v, b, a)
    r = t.run(". /etc/os-release 2>/dev/null && echo \"$PRETTY_NAME $(uname -m)\"")
    return r.out.decode().strip() if r.rc == 0 and r.out.strip() else "unknown"


def phase_a1(work):
    ctx = _phase_ctx("A", work)
    try:
        vol = ctx.volume(IMG_MIB * 1024 * 1024)
        fsops.mount(ctx, vol)
        root = os.fsencode(vol.mnt)
        for gid, kind, variants in GROUPS:
            for v in variants:
                p = os.path.join(root, v.encode("utf-8"))
                if kind == "d":
                    os.mkdir(p)
                    p = os.path.join(p, b"inner.txt")
                fd = os.open(p, os.O_WRONLY | os.O_CREAT | os.O_EXCL, 0o644)
                os.write(fd, payload(v))
                os.close(fd)
        listing = _root_listing(root)
        fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        return {"image_sha256": _sha256_file(vol.img), "listing_on_linux": listing,
                "ntfs3g_version": hv.detect_ntfs3g(ctx.transport), "os": _os_string(ctx.transport)}
    finally:
        ctx.close()


def _b_volume(ctx):
    vol = hv.MacosNtfs3g(ctx, 0)
    os.makedirs(vol.mnt, exist_ok=True)
    ctx.add_cleanup(vol.cleanup)
    return vol


def _debug_names(raw_log):
    with open(raw_log, "rb") as f:
        data = f.read()
    out = []
    for line in data.split(b"\n"):
        if line.startswith(b"LOOKUP /") and not line.startswith(b"LOOKUP /._"):
            name = line[len(b"LOOKUP /"):].split(b"/")[0]  # libfuse logs the whole path; first component
            if any(b > 127 for b in name):
                out.append(name.hex())
    return out


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
        doc = {"sha_in": _sha256_file(vol.img)}
        # 1 and 2: listing and lookups, default and nonfconv.
        for label, extra in (("default", ""), ("nonfconv", "nonfconv")):
            fsops.mount(ctx, vol, rw=False, extra=extra)
            try:
                root = os.fsencode(vol.mnt)
                doc[label] = {"listing": _root_listing(root), "lookups": _lookup_all(root)}
            finally:
                fsops.unmount(ctx, vol)
        # 3: the worker's debug log, names it was asked to look up.
        for label, extra in (("default", ""), ("nonfconv", "nonfconv")):
            raw = os.path.join(work, "worker-%s.raw" % label)
            fsops.mount_debug(ctx, vol, raw, rw=False, extra=extra)
            try:
                _lookup_all(os.fsencode(vol.mnt))
            finally:
                fsops.unmount(ctx, vol)
            doc[label]["worker_lookups"] = sorted(set(_debug_names(raw)))
        # 4: names created on the Mac, then the hazard probe.
        fsops.mount(ctx, vol)
        try:
            root = os.fsencode(vol.mnt)
            for s in MAC_STRINGS:
                for tag, form in (("nfc", NFC(s)), ("nfd", NFD(s))):
                    name = "mac-%s-%s" % (tag, form)
                    with open(os.path.join(root, name.encode()), "wb") as f:
                        f.write(payload(name))
                    d = "macdir-%s-%s" % (tag, form)
                    os.mkdir(os.path.join(root, d.encode()))
                    with open(os.path.join(root, d.encode(), b"inner.txt"), "wb") as f:
                        f.write(payload(d))
            doc["mac_listing"] = _root_listing(root)
            # Hazard: unlink the name the listing shows for the café pair (NFD bytes).
            shown = [n for n in os.listdir(root) if n.startswith(b"g1-caf")]
            doc["g1_listed"] = [n.hex() for n in sorted(shown)]
            r = {}
            try:
                os.unlink(os.path.join(root, shown[0]))
                r["unlink_listed_name"] = "ok"
            except OSError as e:
                r["unlink_listed_name"] = "ERRNO%d" % e.errno
            r["g1_after_unlink"] = [n.hex() for n in sorted(x for x in os.listdir(root) if x.startswith(b"g1-caf"))]
            r["g1_lookups_after"] = dict(
                (s.encode().hex(), _seen(root, s, "f")) for s in ("g1-café", "g1-café"))
            doc["hazard"] = r
        finally:
            fsops.unmount(ctx, vol)
        fsops.fsck(ctx, vol)
        doc["image_sha256"] = _sha256_file(vol.img)
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
        image_sha = _sha256_file(vol.img)
        fix = ctx.sh("ntfsfix -n %s" % shlex.quote(vol.img), check=False)
        src = os.path.join(TESTS, "tools", "treedump.c")
        exe = os.path.join(work, "treedump")
        ctx.sh("cc -std=c17 -O2 -o %s %s" % (shlex.quote(exe), shlex.quote(src)))
        fsops.mount(ctx, vol, rw=False)
        try:
            root = os.fsencode(vol.mnt)
            names = []
            for dp, dn, fns in os.walk(root):
                for n in sorted(dn + fns):
                    full = os.path.join(dp, n)
                    rel = os.path.relpath(full, root)
                    names.append({"path_hex": rel.hex(), "dir": os.path.isdir(full)})
            contents = {}
            for dp, dn, fns in os.walk(root):
                for n in fns:
                    with open(os.path.join(dp, n), "rb") as f:
                        contents[os.path.relpath(os.path.join(dp, n), root).hex()] = f.read().decode("latin-1")
            treedump = ctx.sh("%s %s" % (shlex.quote(exe), shlex.quote(vol.mnt))).out.decode("utf-8", "surrogateescape")
        finally:
            fsops.unmount(ctx, vol)
        return {"image_sha256": image_sha, "names": names, "contents": contents, "treedump": treedump,
                "ntfsfix_rc": fix.rc, "ntfs3g_version": hv.detect_ntfs3g(ctx.transport), "os": _os_string(ctx.transport)}
    finally:
        ctx.close()


# ---------------------------------------------------------------- judge (pure)

def _disk_variants():
    """[(gid, kind, spelling)] for the variants Environment A wrote."""
    return [(gid, kind, v) for gid, kind, variants in GROUPS for v in variants]


def predict_lookup(mode, spelling, gid, kind):
    """Content (str) or 'ENOENT' the recorded rule predicts for a lookup of `spelling`.
    default (nfconv): the name is converted to NFC before it meets the disk; nonfconv: exact bytes."""
    key = NFC(spelling) if mode == "default" else spelling
    for g, k, v in _disk_variants():
        if g == gid and v == key:
            return payload(v).decode("latin-1")
    return "ENOENT"


def judge(a1, b, a2):
    """Return (problems, findings): findings are plain facts for the log, problems are deviations."""
    bad, facts = [], []
    if b["sha_in"] != a1["image_sha256"]:
        bad.append("image changed on the way to the guest")
    if b["image_sha256"] != a2["image_sha256"]:
        bad.append("image changed on the way back to Linux")
    if a2["ntfsfix_rc"] != 0:
        bad.append("ntfsfix -n on the final image exited %d" % a2["ntfsfix_rc"])
    on_disk = [v for _g, _k, v in _disk_variants()]
    # Listings.
    want_default = sorted(NFD(v).encode().hex() for v in on_disk)
    want_raw = sorted(v.encode().hex() for v in on_disk)
    if sorted(a1["listing_on_linux"]) != want_raw:
        bad.append("Linux does not list the names as written")
    if sorted(b["default"]["listing"]) != want_default:
        bad.append("default mount listing is not the NFD form of the names on disk")
    if sorted(b["nonfconv"]["listing"]) != want_raw:
        bad.append("nonfconv listing is not the bytes on disk")
    # Lookups.
    for mode in ("default", "nonfconv"):
        for gid, kind, variants in GROUPS:
            for hx, seen in b[mode]["lookups"][gid].items():
                s = bytes.fromhex(hx).decode("utf-8")
                got = "ENOENT" if seen == "ERRNO2" else seen
                want = predict_lookup(mode, s, gid, kind)
                if got != want:
                    bad.append("%s mount, group %s, lookup %r: got %s, rule predicts %s" % (mode, gid, s, seen, want))
    # Which pairs are unreachable under the default mount (facts for the note).
    for gid, kind, variants in GROUPS:
        for v in variants:
            if NFC(v) != v:
                facts.append("%s: %r is not NFC, listed but unreachable by name under the default mount" % (gid, v))
    # Worker log: the worker receives the spellings the program used, in both modes.
    spellings = set(hx for g in GROUPS for hx in b["default"]["lookups"][g[0]]
                    if any(c > 127 for c in bytes.fromhex(hx)))  # the worker log is filtered to non-ASCII names
    for mode in ("default", "nonfconv"):
        got = set(b[mode]["worker_lookups"])
        if got != spellings:
            bad.append("%s worker received other names than the program used: missing %s, extra %s" % (
                mode, sorted(spellings - got)[:3], sorted(got - spellings)[:3]))
    # Names made on the Mac: NFC on disk, whatever form was typed; content intact.
    disk = dict((bytes.fromhex(n["path_hex"]), n["dir"]) for n in a2["names"])
    nfc_count = 0
    for s in MAC_STRINGS:
        for prefix in ("mac-", "macdir-"):
            for tag, form in (("nfc", NFC(s)), ("nfd", NFD(s))):
                typed = "%s%s-%s" % (prefix, tag, form)
                if NFC(typed).encode() not in disk:
                    bad.append("Mac-created %r (typed in %s form) is not on disk as NFC" % (typed, tag.upper()))
                else:
                    nfc_count += 1
                if typed != NFC(typed) and typed.encode() in disk:
                    bad.append("Mac-created %r (typed in NFD form) is on disk in NFD" % typed)
                key = NFC(typed).encode() + (b"/inner.txt" if prefix == "macdir-" else b"")
                if a2["contents"].get(key.hex()) != payload(typed).decode("latin-1"):
                    bad.append("Mac-created %r: content differs on Linux" % typed)
    facts.append("%d of %d Mac-created names are NFC on disk" % (nfc_count, len(MAC_STRINGS) * 4))
    # Hazard probe: unlink of the listed (NFD) name of the café pair.
    h = b["hazard"]
    nfc_hex = "g1-café".encode().hex()
    nfd_hex = "g1-café".encode().hex()
    facts.append("unlink of the listed g1 name: %s; g1 entries on Linux afterwards: %s" % (
        h["unlink_listed_name"], sorted(bytes.fromhex(n["path_hex"]).decode() for n in a2["names"]
                                        if bytes.fromhex(n["path_hex"]).startswith(b"g1-caf"))))
    g1_disk = [bytes.fromhex(n["path_hex"]) for n in a2["names"] if bytes.fromhex(n["path_hex"]).startswith(b"g1-caf")]
    if h["unlink_listed_name"] != "ok" or g1_disk != [bytes.fromhex(nfd_hex)]:
        bad.append("unlink of the listed name did not remove the NFC-named file and keep the NFD-named one: %s, disk %s" % (
            h["unlink_listed_name"], [g.hex() for g in g1_disk]))
    if nfc_hex in [g.hex() for g in g1_disk]:
        bad.append("NFC-named g1 file still on disk")
    return bad, facts


# ---------------------------------------------------------------- the case (host side)

def _tart_sh():
    return os.path.join(REPO, "vm", "macos", "tart.sh")


def _jdump(path, obj):
    with open(path, "w", encoding="utf-8") as f:
        json.dump(obj, f, indent=1, sort_keys=True)


@case(CASE_ID, envs=("host",), title="U22: names that differ only in normalisation, written on Linux, read on macOS by both forms")
def i_name_u22(ctx):
    vm_name = os.environ.get("NTFSMAC_B_VM")
    if not vm_name:
        ctx.skip("NTFSMAC_B_VM is not set: this case drives a running Environment B guest with NTFS-3G built "
                 "in it; run tests/harness/run-env-b.sh --orchestrate --cases I-NAME-U22")
    vm = transport.VmTransport(vm_name, _tart_sh())
    linux = transport.ContainerTransport(os.path.join(REPO, "vm", "linux", "run.sh"))

    d = vm.run("mktemp -d /tmp/ntfsmac-u22.XXXXXX")
    if d.rc != 0:
        raise RuntimeError("cannot reach the guest: %s" % d.err.decode("utf-8", "replace"))
    gdir = d.out.decode().strip()
    ctx.add_cleanup(lambda: vm.run("rm -rf %s" % shlex.quote(gdir)))
    vm.put_tree(TESTS, SHIP, gdir)
    build_root = os.path.join(REPO, "build", "u22")
    os.makedirs(build_root, exist_ok=True)
    hdir = tempfile.mkdtemp(prefix="run-", dir=build_root)
    ctx.add_cleanup(lambda: shutil.rmtree(hdir, ignore_errors=True))
    img1, img2 = os.path.join(hdir, "a.img"), os.path.join(hdir, "b.img")
    prog = "integration/functional/u22_names.py"

    # Phase A1. A container lives for one call, so the image and the JSON leave it in one tar stream.
    tar_path = os.path.join(hdir, "a1.tar")
    r = linux.run_to_file("rm -rf /tmp/u22 && mkdir -p /tmp/u22 && PYTHONDONTWRITEBYTECODE=1 python3 /work/tests/%s "
                          "phase-a1 --dir /tmp/u22 1>&2 && tar -cf - -C /tmp/u22 vol.img a1.json" % prog, tar_path, timeout=900)
    ctx.log("phase A1 -> rc=%d\n%s" % (r.rc, r.err.decode("utf-8", "replace")[-3000:]))
    if r.rc != 0:
        raise RuntimeError("phase A1 failed (rc=%d)" % r.rc)
    with open(tar_path, "rb") as f:
        transport.extract_tar(f.read(), os.path.join(hdir, "a1"))
    os.remove(tar_path)
    shutil.move(os.path.join(hdir, "a1", "vol.img"), img1)
    with open(os.path.join(hdir, "a1", "a1.json"), "r", encoding="utf-8") as f:
        a1 = json.load(f)
    if fsops_sha(img1) != a1["image_sha256"]:
        raise Fail("image changed in transit container -> host")

    # Phase B.
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
    if fsops_sha(img2) != b["image_sha256"]:
        raise Fail("image changed in transit guest -> host")

    # Phase A2.
    r = linux.run_from_file("rm -rf /tmp/u22 && mkdir -p /tmp/u22 && cat > /tmp/u22/vol.img && "
                            "PYTHONDONTWRITEBYTECODE=1 python3 /work/tests/%s phase-a2 --dir /tmp/u22" % prog, img2, timeout=900)
    ctx.log("phase A2 -> rc=%d\n%s" % (r.rc, r.err.decode("utf-8", "replace")[-3000:]))
    if r.rc != 0:
        raise RuntimeError("phase A2 failed (rc=%d)" % r.rc)
    a2 = json.loads(r.out.decode("utf-8"))
    for p in (img1, img2):
        os.remove(p)

    _jdump(ctx.evidence_file("phase-a1.json"), a1)
    _jdump(ctx.evidence_file("phase-b.json"), b)
    _jdump(ctx.evidence_file("phase-a2.json"), dict((k, v) for k, v in a2.items() if k != "treedump"))
    with open(ctx.evidence_file("treedump-linux-final.txt"), "w", encoding="utf-8", errors="surrogateescape") as f:
        f.write(a2["treedump"])
    bad, facts = judge(a1, b, a2)
    for f_ in facts:
        ctx.log("fact: " + f_)
    ctx.log("Linux: NTFS-3G %s on %s; macOS: %s, macFUSE %s, NTFS-3G %s" % (
        a1["ntfs3g_version"], a1["os"], b["os"], b["macfuse_version"], b["ntfs3g_version"]))
    if bad:
        for x in bad[:40]:
            ctx.log("MISMATCH " + x)
        raise Fail("%d observations differ from the rule 'NTFS-3G nfconv converts', first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: every observation matches the rule (default mount: lookups are converted to NFC, listings to NFD; "
            "nonfconv: exact bytes; the worker receives the program's spelling in both)")


def fsops_sha(path):
    return _sha256_file(path)


def main(argv):
    p = argparse.ArgumentParser(prog="u22_names.py", description="I-NAME-U22 phase programs (internal)")
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
