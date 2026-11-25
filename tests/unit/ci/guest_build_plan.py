#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The plan of the `guest-build` stage of Environment B (vm/macos/guest-build.sh): every cc line of the VM scripts, in the guest.

check_guest_sources.py knows, for each VM script (tests/integration/daemon/*_vm.sh), the copy list of its sources and
its cc lines; check_guest_link.py builds the lines that need no library on the host. This script writes what the guest
needs to build ALL of them, the lines that use libntfs-3g, macFUSE or the FUSE stand-in included, so that a missing
header, a missing .c on a line or a link error fails in the first minutes of a B run, not after the integration suite:

  OUT/pack/build.sh      the guest script: for each VM script, cd into its directory and run every cc line (the flags
                         and libraries of libntfs-3g and fuse from pkg-config with PKG_CONFIG_PATH at ~/ntfsmac/deps,
                         as the VM scripts set it), print FAIL for every line that does not build, exit 1 if any
  OUT/pack/NAME/...      the files of the copy list of the script NAME, copied with tar as the script copies them

The guest unpacks OUT/pack into ~/ntfsmac/gbuild and runs `sh ~/ntfsmac/gbuild/build.sh`.
Usage: guest_build_plan.py REPO OUT [SCRIPT ...]     (default scripts: those of check_guest_sources.py;
       exit 0 = written, 1 = a script does not parse or its copy failed, 2 = usage)
"""
import os
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import check_guest_sources as gs  # noqa: E402

HEAD = """#!/bin/sh
# written by tests/unit/ci/guest_build_plan.py: every cc line of the VM scripts, built in the guest
set -u
root=$(cd "$(dirname "$0")" && pwd)
export PKG_CONFIG_PATH=$HOME/ntfsmac/deps/lib/pkgconfig
ntfs="$(pkg-config --cflags libntfs-3g)" || { echo "guest-build: pkg-config cannot find libntfs-3g (is ~/ntfsmac/deps there?)"; exit 2; }
nlibs="$(pkg-config --libs --static libntfs-3g)" || { echo "guest-build: pkg-config --libs libntfs-3g failed"; exit 2; }
rc=0
built=0
"""


def cc_line(entry, out):
    """the cc command of one entry as a shell line: flags, then the library flags, then the sources, quoted, -o, libraries last"""
    args = entry["args"]
    # the library flags go after the flags (-I paths first, as in the VM scripts) and before the first source
    k = next((i for i, a in enumerate(args) if a.endswith(".c")), len(args))
    words = ["cc"] + [shlex.quote(a) for a in args[:k]] + entry["pre"] + [shlex.quote(a) for a in args[k:]] + ["-o", out] + entry["post"]
    return " ".join(words)


def plan(repo, out, scripts):
    """-> (errors, number of cc lines); writes OUT/pack"""
    errors = []
    pack = os.path.join(out, "pack")
    body = [HEAD]
    total = 0
    for sc in scripts:
        with open(os.path.join(repo, sc), encoding="utf-8") as f:
            guest, tar, cc, perrs = gs.parse(f.read())
        errors += ["%s: %s" % (sc, e) for e in perrs]
        if not cc:
            errors.append("%s: no cc line found (the parser does not understand the script)" % sc)
        if perrs or not cc:
            continue
        name = os.path.basename(sc)[: -len(".sh")] if sc.endswith(".sh") else os.path.basename(sc)
        d = os.path.join(pack, name)
        os.makedirs(d, exist_ok=True)
        packed = subprocess.run(["tar", "-cf", "-", "-C", repo] + tar, capture_output=True)
        if packed.returncode != 0:
            errors.append("%s: tar of the copy list failed: %s" % (sc, packed.stderr.decode()[:300]))
            continue
        unpacked = subprocess.run(["tar", "-xf", "-", "-C", d], input=packed.stdout, capture_output=True)
        if unpacked.returncode != 0:
            errors.append("%s: untar failed: %s" % (sc, unpacked.stderr.decode()[:300]))
            continue
        body.append('cd "$root/%s" && mkdir -p .out || exit 2\n' % name)
        for i, entry in enumerate(cc):
            total += 1
            body.append('echo "== %s: cc line %d (%s)"\n' % (name, i + 1, " ".join(entry["srcs"][:1]) or "no source"))
            body.append("%s || { echo \"FAIL: %s: cc line %d\"; rc=1; }\n" % (cc_line(entry, ".out/o%d" % i), name, i + 1))
            body.append("built=$((built + 1))\n")
    body.append('echo "guest-build: $built cc lines run, exit $rc"\nexit $rc\n')
    os.makedirs(pack, exist_ok=True)
    with open(os.path.join(pack, "build.sh"), "w", encoding="utf-8") as f:
        f.write("".join(body))
    return errors, total


def main():
    if len(sys.argv) < 3:
        sys.stderr.write("usage: guest_build_plan.py REPO OUT [SCRIPT ...]\n")
        return 2
    repo, out = sys.argv[1], sys.argv[2]
    scripts = sys.argv[3:] or gs.DEFAULT_SCRIPTS
    errors, total = plan(repo, out, scripts)
    for e in errors:
        print("FAIL: " + e)
    if errors:
        return 1
    print("guest build plan: %d scripts, %d cc lines" % (len(scripts), total))
    return 0


if __name__ == "__main__":
    sys.exit(main())
