#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Build, on the macOS host, what the guest scripts build in the VM (the part that needs no libntfs-3g or macFUSE).

For each VM script of check_guest_sources.py: copy its tar list (the one that feeds the guest build directory)
into a temporary directory with tar, as the guest gets it, then run every cc line of the script that does not
use `pkg-config` or the libntfs-3g variables ($ntfs, $nlibs), in that directory, with the same flags and
with -o pointed into the temporary directory. A script that builds no line, a parse error, and a skipped line that holds daemon or journal sources
(or common sources on a line with no probe, volmark or test tool file) (they need no guest library, so they must be built here) fail too. A missing file in the copy list, a missing .c on the cc line (an
undefined symbol at link time, such as ntfsmac_log from src/common/log.c) or a compile error fails the test.
The lines that need libntfs-3g or macFUSE (the probe, volmark, test tools and the FUSE stand-in) are counted
and skipped: they need the guest's headers and libraries.
Usage: check_guest_link.py REPO [SCRIPT ...]    (exit 0 = all built, 1 = a build failed, 2 = usage,
       77 = skipped: not a macOS host, where the frameworks of the cc lines do not exist)
"""
import os
import subprocess
import sys
import tempfile

OWN_DIRS = ("src/daemon/", "src/journal/", "src/common/")
TOOL_DIRS = ("src/probe/", "src/volmark/", "tests/")
HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
sys.dont_write_bytecode = True
import check_guest_sources as gs  # noqa: E402


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: check_guest_link.py REPO [SCRIPT ...]\n")
        return 2
    if sys.platform != "darwin":
        print("SKIP: the cc lines of the guest scripts link macOS frameworks; this host is %s" % sys.platform)
        return 77
    repo = sys.argv[1]
    scripts = sys.argv[2:] or gs.DEFAULT_SCRIPTS
    failures, built, skipped = [], 0, 0
    with tempfile.TemporaryDirectory(prefix="guestlink.") as tmp:
        for sc in scripts:
            with open(os.path.join(repo, sc), encoding="utf-8") as f:
                guest, tar, cc, perrs = gs.parse(f.read())
            failures += ["%s: %s" % (sc, e) for e in perrs]
            if not cc:
                failures.append("%s: no cc line found (the parser does not understand the script)" % sc)
            if perrs or not cc:
                continue  # already reported above; the script is not skipped silently
            d = os.path.join(tmp, os.path.basename(sc))
            os.makedirs(d)
            pack = subprocess.run(["tar", "-cf", "-", "-C", repo] + tar, capture_output=True)
            if pack.returncode != 0:
                failures.append("%s: tar of the copy list failed: %s" % (sc, pack.stderr.decode()[:300]))
                continue
            unpack = subprocess.run(["tar", "-xf", "-", "-C", d], input=pack.stdout, capture_output=True)
            if unpack.returncode != 0:
                failures.append("%s: untar failed: %s" % (sc, unpack.stderr.decode()[:300]))
                continue
            built_here = 0
            for i, entry in enumerate(cc):
                if entry["external"]:
                    # the libntfs-3g tools (probe, volmark, test tools) legitimately take src/common files
                    tool = any(x.startswith(TOOL_DIRS) for x in entry["srcs"])
                    own = [x for x in entry["srcs"] if x.startswith(OWN_DIRS) and
                           not (tool and x.startswith("src/common/"))]
                    if own:
                        failures.append("%s: a cc line skipped as external (libntfs-3g or macFUSE) holds project "
                                        "sources of the daemon library: %s" % (sc, " ".join(own)))
                    skipped += 1
                    continue
                cmd = ["cc"] + entry["args"] + ["-o", os.path.join(d, "out%d" % i)]
                r = subprocess.run(cmd, cwd=d, capture_output=True, text=True)
                if r.returncode != 0:
                    failures.append("%s: cc %s failed:\n%s" % (sc, " ".join(entry["args"][:8]), r.stderr[:800]))
                else:
                    built += 1
                    built_here += 1
            if built_here == 0:
                failures.append("%s: no cc line was built (every one skipped as external, or none found)" % sc)
    for e in failures:
        print("FAIL: " + e)
    if failures:
        return 1
    print("guest link: %d cc lines built from the copied lists, %d skipped (libntfs-3g or macFUSE)" % (built, skipped))
    return 0


if __name__ == "__main__":
    sys.exit(main())
