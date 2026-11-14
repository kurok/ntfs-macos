# SPDX-License-Identifier: GPL-2.0-or-later
"""C-REF-01: an NTFS image written on macOS (Environment B: guest-built NTFS-3G on macFUSE FSKit)
is read on Linux (Environment A: pinned NTFS-3G in the container) and every file hash is equal
(IMPLEMENTATION_PLAN criterion 2.3, second half).

One case cannot live in one environment, so C-REF-01 is an env "host" case: it runs on the macOS
host, drives the Environment B guest through the tart guest agent and the Environment A container
through vm/linux/run.sh, and runs this same file as a small phase program on each side:

  phase B (in a clone of the golden VM, `cross_read.py phase-b`):
      mkntfs an image, mount it rw with the macOS adapter (backend=fskit,local,norecover),
      write the seeded set (100 files 1 KiB..64 MiB, the generator of I-INT-01) plus directories,
      an empty directory, a zero-length file and Unicode names, record SHA-256 per file, unmount
      cleanly, remount read-only and hash and treedump what macOS reads back, unmount, sync,
      SHA-256 of the image, write manifest.json.
  host: stream the image out of the guest into build/ (git-ignored), check its SHA-256, stream it
      into the container, always delete the host copy.
  phase A (in the container, `cross_read.py phase-a`):
      SHA-256 of the image, `ntfsfix -n`, read-only NTFS-3G mount, hash every file, treedump.
  host: compare. Pass needs: image SHA-256 equal at every hop, Linux hashes equal to what macOS
      wrote (size and SHA-256, no missing and no extra file, directories present), macOS read-back
      equal to what was written, treedump of both sides equal, and `ntfsfix -n` exit 0.
      The root `.fseventsd` that macOS creates in every mount is ignored.

Run (clones the VM, builds NTFS-3G in it, always deletes the VM):
    tests/harness/run-env-b.sh --orchestrate --cases C-REF-01
Without NTFSMAC_B_VM (set by run-env-b.sh to a running, built VM) the case skips with a note.
Dev override: NTFSMAC_SEED.
"""

import argparse
import hashlib
import json
import os
import random
import shlex
import shutil
import sys
import tempfile
import time
import unicodedata

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
REPO = os.path.dirname(TESTS)
if TESTS not in sys.path:
    sys.path.insert(0, TESTS)

from harness import core, transport, volume  # noqa: E402
from harness.core import Fail, case  # noqa: E402
from harness.seeded import DEFAULT_SEED, MACOS_ROOT_METADATA, MIB, compare, hash_tree, plan_sizes, write_files  # noqa: E402

CASE_ID = "C-REF-01"
NFILES = 100
SHIP = ("harness", "compatibility", "tools")
GUEST_PATH = 'PATH="$HOME/ntfsmac/deps/bin:$HOME/ntfsmac/deps/sbin:$PATH"'
EXIT_SKIP = 3

# Extra entries beyond the seeded files: Unicode names in NFC (what the generator writes; a
# normalising layer would show up as a missing or extra file), nesting, an empty file, an empty directory.
EXTRA_FILES = (
    ("uni/café.txt", 4097),
    ("uni/日本語/файл.bin", 70000),
    ("uni/emoji \U0001F642.bin", 513),
    ("uni/with space/trailing.dot.txt", 100),
    ("deep/a/b/c/d/leaf.txt", 1),
    ("zero-length", 0),
)
EXTRA_DIRS = ("empty-dir", "uni/empty sub dir")


def _seed():
    return int(os.environ.get("NTFSMAC_SEED", DEFAULT_SEED))


def sha256_file(path):
    h = hashlib.sha256()
    with open(path, "rb") as f:
        while True:
            b = f.read(MIB)
            if not b:
                break
            h.update(b)
    return h.hexdigest()


def write_extras(root, seed):
    """Write EXTRA_FILES and EXTRA_DIRS; return ({rel: (size, sha256)}, [dirs])."""
    out = {}
    for rel, size in EXTRA_FILES:
        assert unicodedata.normalize("NFC", rel) == rel
        path = os.path.join(root, rel)
        os.makedirs(os.path.dirname(path), exist_ok=True)
        data = random.Random("extra:%d:%s" % (seed, rel)).randbytes(size)
        with open(path, "wb") as f:
            f.write(data)
        out[rel] = (size, hashlib.sha256(data).hexdigest())
    for rel in EXTRA_DIRS:
        os.makedirs(os.path.join(root, rel), exist_ok=True)
    return out, list(EXTRA_DIRS)


def list_dirs(root):
    out = []
    for dp, dn, _fns in os.walk(root):
        if dp == root:
            dn[:] = [d for d in dn if d not in MACOS_ROOT_METADATA]
        for d in dn:
            out.append(os.path.relpath(os.path.join(dp, d), root).replace(os.sep, "/"))
    return sorted(out)


def nfc_keys(files):
    """({NFC name: value}, [raw names that were not NFC]). macOS lists names of this volume
    in NFD (observed in C-REF-01: caf\u00e9 comes back as cafe + U+0301) while the bytes on disk are NFC
    (Linux lists NFC); a name written in NFC is compared by its NFC form on the macOS side."""
    out, changed = {}, []
    for k, v in files.items():
        n = unicodedata.normalize("NFC", k)
        if n != k:
            changed.append(k)
        out[n] = v
    return out, sorted(changed)


def filter_treedump(text):
    """Normalise a treedump for comparison: names to NFC (see nfc_keys) and drop the root
    .fseventsd lines (fields are TAB separated, path second)."""
    keep = []
    for line in unicodedata.normalize("NFC", text).splitlines():
        path = line.split("\t")[1] if "\t" in line else ""
        if any(path == m or path.startswith(m + "/") for m in MACOS_ROOT_METADATA):
            continue
        keep.append(line)
    return "\n".join(keep) + ("\n" if keep else "")


def judge(manifest, phase_a, image_sha_host):
    """Pure comparison. Returns (list of failure strings, dict of counts)."""
    bad = []
    expected = {k: tuple(v) for k, v in manifest["expected"].items()}
    readback = {k: tuple(v) for k, v in manifest["readback"].items()}
    linux = {k: tuple(v) for k, v in phase_a["files"].items()}
    for label, sha in (("host copy", image_sha_host), ("Linux copy", phase_a["image_sha256"])):
        if sha != manifest["image_sha256"]:
            bad.append("image SHA-256 differs between guest and %s: %s != %s" % (label, sha, manifest["image_sha256"]))
    bad += ["Linux read: " + b for b in compare(expected, linux)]
    bad += ["macOS read-back: " + b for b in compare(expected, readback)]
    missing_dirs = sorted(set(manifest["dirs"]) - set(phase_a["dirs"]))
    bad += ["Linux read: directory missing: %s" % d for d in missing_dirs]
    if filter_treedump(manifest["treedump"]) != filter_treedump(phase_a["treedump"]):
        bad.append("treedump differs between macOS read-back and Linux read")
    if phase_a["ntfsfix"]["rc"] != 0:
        bad.append("ntfsfix -n on Linux exited %d" % phase_a["ntfsfix"]["rc"])
    return bad, {"files_expected": len(expected), "files_linux": len(linux), "dirs_expected": len(manifest["dirs"])}


# ---- phase programs (run inside the guest and the container) ----

def _phase_ctx(env, work):
    c = core.Case(CASE_ID, (env,), None, "cross-read phase", None)
    return core.Ctx(c, env, work, os.path.join(work, "evidence"), transport.LocalTransport(), quiet=False)


def _build_treedump(ctx):
    src = os.path.join(TESTS, "tools", "treedump.c")
    exe = os.path.join(ctx.work, "treedump")
    ctx.sh("cc -std=c17 -O2 -o %s %s" % (shlex.quote(exe), shlex.quote(src)))
    return exe


def _treedump(ctx, exe, root):
    return ctx.sh("%s %s" % (shlex.quote(exe), shlex.quote(root))).out.decode("utf-8", "surrogateescape")


def _os_string(t):
    r = t.run("sw_vers -productName && sw_vers -productVersion && sw_vers -buildVersion && uname -m")
    if r.rc == 0:
        n, v, b, a = r.out.decode().split()[:4]
        return "%s %s (%s) %s" % (n, v, b, a)
    r = t.run(". /etc/os-release 2>/dev/null && echo \"$PRETTY_NAME $(uname -m)\"")
    return r.out.decode().strip() if r.rc == 0 and r.out.strip() else "unknown"


def phase_b(out_dir, seed):
    os.makedirs(out_dir, exist_ok=True)
    ctx = _phase_ctx("B", out_dir)
    try:
        sizes = plan_sizes(seed, NFILES)
        total = sum(sizes) + sum(s for _r, s in EXTRA_FILES)
        img_size = ((int(total * 1.06) + 64 * MIB) // MIB + 1) * MIB
        ctx.log("seed=%d files=%d total=%d bytes image=%d bytes" % (seed, NFILES, total, img_size))
        try:
            vol = ctx.volume(img_size)
        except core.Skip as e:
            ctx.log("skip: %s" % e)
            sys.stderr.write("SKIP: %s\n" % e)
            return EXIT_SKIP
        exe = _build_treedump(ctx)
        vol.mount(rw=True)
        expected = write_files(vol.mnt, seed, sizes)
        extras, extra_dirs = write_extras(vol.mnt, seed)
        expected.update(extras)
        dirs = sorted(set(list_dirs(vol.mnt)))
        ctx.log("wrote %d files, %d directories" % (len(expected), len(dirs)))
        vol.unmount()
        vol.mount(rw=False)
        readback, nfd_listed = nfc_keys(hash_tree(vol.mnt))
        ctx.log("macOS listed %d names in a non-NFC form: %s" % (len(nfd_listed), nfd_listed))
        treedump = _treedump(ctx, exe, vol.mnt)
        vol.unmount()
        ctx.sh("sync")
        manifest = {
            "seed": seed,
            "expected": expected,
            "dirs": dirs,
            "readback": readback,
            "macos_non_nfc_names": nfd_listed,
            "treedump": treedump,
            "image_size": os.path.getsize(vol.img),
            "image_sha256": sha256_file(vol.img),
            "os": _os_string(ctx.transport),
            "macfuse_version": volume.detect_macfuse(ctx.transport),
            "ntfs3g_version": volume.detect_ntfs3g(ctx.transport),
        }
        with open(os.path.join(out_dir, "manifest.json"), "w", encoding="utf-8") as f:
            json.dump(manifest, f)
        ctx.log("image sha256 %s" % manifest["image_sha256"])
        return 0
    finally:
        errs = ctx.close()
        if errs:
            sys.stderr.write("phase-b cleanup errors: %s\n" % errs)


def phase_a(work):
    ctx = _phase_ctx("A", work)
    try:
        for tool in ("ntfs-3g", "ntfsfix", "cc", "umount"):
            if ctx.transport.run("command -v %s" % tool).rc != 0:
                raise volume.VolumeError("%s not found in the Environment A container" % tool)
        vol = volume.LinuxNtfs3g(ctx, 0)
        os.makedirs(vol.mnt, exist_ok=True)
        ctx.add_cleanup(vol.cleanup)
        image_sha = sha256_file(vol.img)
        fix = ctx.sh("ntfsfix -n %s" % shlex.quote(vol.img), check=False)
        exe = _build_treedump(ctx)
        vol.mount(rw=False)
        files = hash_tree(vol.mnt)
        dirs = list_dirs(vol.mnt)
        treedump = _treedump(ctx, exe, vol.mnt)
        vol.unmount()
        return {
            "image_sha256": image_sha,
            "files": files,
            "dirs": dirs,
            "treedump": treedump,
            "ntfsfix": {"rc": fix.rc, "out": (fix.out + fix.err).decode("utf-8", "replace")},
            "os": _os_string(ctx.transport),
            "ntfs3g_version": volume.detect_ntfs3g(ctx.transport),
        }
    finally:
        ctx.close()


# ---- the case (host side) ----

def _tart_sh():
    return os.path.join(REPO, "vm", "macos", "tart.sh")


def _log_remote(ctx, label, r):
    ctx.log("%s -> rc=%d" % (label, r.rc))
    err = r.err.decode("utf-8", "replace").strip()
    if err:
        ctx.log("%s stderr:\n%s" % (label, err[-6000:]))


@case(CASE_ID, envs=("host",), title="image written on macOS (NTFS-3G on macFUSE FSKit) reads identically on Linux")
def c_ref_01(ctx):
    vm_name = os.environ.get("NTFSMAC_B_VM")
    if not vm_name:
        ctx.skip("NTFSMAC_B_VM is not set: this case drives a running Environment B guest with NTFS-3G built "
                 "in it; run tests/harness/run-env-b.sh --orchestrate --cases C-REF-01")
    seed = _seed()
    vm = transport.VmTransport(vm_name, _tart_sh())
    linux = transport.ContainerTransport(os.path.join(REPO, "vm", "linux", "run.sh"))

    d = vm.run("mktemp -d /tmp/ntfsmac-xr.XXXXXX")
    if d.rc != 0:
        raise RuntimeError("cannot reach the guest: %s" % d.err.decode("utf-8", "replace"))
    gdir = d.out.decode().strip()
    ctx.add_cleanup(lambda: vm.run("rm -rf %s" % shlex.quote(gdir)))
    vm.put_tree(TESTS, SHIP, gdir)

    build_root = os.path.join(REPO, "build", "xread")
    os.makedirs(build_root, exist_ok=True)
    hdir = tempfile.mkdtemp(prefix="run-", dir=build_root)
    ctx.add_cleanup(lambda: shutil.rmtree(hdir, ignore_errors=True))
    host_img = os.path.join(hdir, "vol.img")

    # Phase B in the guest.
    t0 = time.monotonic()
    py = "$(if [ -x /usr/bin/python3 ]; then echo /usr/bin/python3; else echo python3; fi)"
    cmd = "PYTHONDONTWRITEBYTECODE=1 %s %s %s/compatibility/cross_read.py phase-b --out %s/out --seed %d" % (
        GUEST_PATH, py, gdir, gdir, seed)
    r = vm.run(cmd, timeout=3600)
    _log_remote(ctx, "phase B (guest)", r)
    if r.rc == EXIT_SKIP:
        ctx.skip("phase B skipped in the guest: %s" % r.err.decode("utf-8", "replace").strip().splitlines()[-1])
    if r.rc != 0:
        raise RuntimeError("phase B failed in the guest (rc=%d)" % r.rc)
    ctx.log("phase B took %.0f s" % (time.monotonic() - t0))
    m = vm.run("cat %s/out/manifest.json" % gdir)
    if m.rc != 0:
        raise RuntimeError("cannot read the manifest from the guest")
    manifest = json.loads(m.out.decode("utf-8"))
    ctx.meta.update({"guest_os": manifest["os"], "macfuse_version": manifest["macfuse_version"],
                     "ntfs3g_version": manifest["ntfs3g_version"]})

    # Image out of the guest (already unmounted and synced in phase B).
    t0 = time.monotonic()
    r = vm.run_to_file("cat %s/out/vol.img" % gdir, host_img, timeout=3600)
    _log_remote(ctx, "copy image out", r)
    if r.rc != 0:
        raise RuntimeError("copying the image out of the guest failed (rc=%d)" % r.rc)
    host_sha = sha256_file(host_img)
    ctx.log("image on host: %d bytes, sha256 %s (%.0f s)" % (os.path.getsize(host_img), host_sha,
                                                              time.monotonic() - t0))
    if os.path.getsize(host_img) != manifest["image_size"] or host_sha != manifest["image_sha256"]:
        raise Fail("image changed in transit guest -> host (guest %s, host %s)" % (manifest["image_sha256"], host_sha))

    # Phase A in the container.
    t0 = time.monotonic()
    acmd = ("rm -rf /tmp/xr && mkdir -p /tmp/xr && cat > /tmp/xr/vol.img && "
            "PYTHONDONTWRITEBYTECODE=1 python3 /work/tests/compatibility/cross_read.py phase-a --dir /tmp/xr")
    r = linux.run_from_file(acmd, host_img, timeout=3600)
    _log_remote(ctx, "phase A (container)", r)
    if r.rc != 0:
        raise RuntimeError("phase A failed in the container (rc=%d)" % r.rc)
    phase_a_doc = json.loads(r.out.decode("utf-8"))
    ctx.log("phase A took %.0f s; Environment A NTFS-3G %s, %s" % (
        time.monotonic() - t0, phase_a_doc["ntfs3g_version"], phase_a_doc["os"]))
    os.remove(host_img)  # no image survives the run

    # Evidence: hashes, treedumps, ntfsfix output (small text; images are not stored).
    with open(ctx.evidence_file("sha256-linux.txt"), "w", encoding="utf-8") as f:
        for rel in sorted(phase_a_doc["files"]):
            size, sha = phase_a_doc["files"][rel]
            f.write("%s  %d  %s\n" % (sha, size, rel))
    for side, text in (("macos", manifest["treedump"]), ("linux", phase_a_doc["treedump"])):
        with open(ctx.evidence_file("treedump-%s.txt" % side), "w", encoding="utf-8", errors="surrogateescape") as f:
            f.write(text)
    with open(ctx.evidence_file("ntfsfix-linux.txt"), "w", encoding="utf-8") as f:
        f.write("ntfsfix -n exit %d\n%s" % (phase_a_doc["ntfsfix"]["rc"], phase_a_doc["ntfsfix"]["out"]))
    ctx.log("image sha256 (guest, host, Linux): %s %s %s" % (manifest["image_sha256"], host_sha,
                                                              phase_a_doc["image_sha256"]))

    bad, counts = judge(manifest, phase_a_doc, host_sha)
    ctx.log("observation: macOS lists %d name(s) in NFD, the bytes on disk are NFC (Linux lists them as written): %s" % (
        len(manifest.get("macos_non_nfc_names", [])), manifest.get("macos_non_nfc_names", [])))
    ctx.log("compared: %d expected files, %d read on Linux, %d directories, %d problems; ntfsfix -n rc=%d" % (
        counts["files_expected"], counts["files_linux"], counts["dirs_expected"], len(bad),
        phase_a_doc["ntfsfix"]["rc"]))
    if bad:
        for b in bad[:30]:
            ctx.log("MISMATCH " + b)
        raise Fail("%d problems, first: %s" % (len(bad), bad[0]))
    ctx.log("PASS: %d files equal on Linux and macOS" % counts["files_linux"])


def main(argv):
    p = argparse.ArgumentParser(prog="cross_read.py", description="C-REF-01 phase programs (internal)")
    sub = p.add_subparsers(dest="phase", required=True)
    b = sub.add_parser("phase-b", help="in the macOS guest: write the image, write manifest.json")
    b.add_argument("--out", required=True)
    b.add_argument("--seed", type=int, default=_seed())
    a = sub.add_parser("phase-a", help="in the Linux container: read DIR/vol.img, print JSON on stdout")
    a.add_argument("--dir", required=True)
    args = p.parse_args(argv)
    if args.phase == "phase-b":
        return phase_b(os.path.abspath(args.out), args.seed)
    doc = phase_a(os.path.abspath(args.dir))
    sys.stdout.write(json.dumps(doc))
    return 0


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
