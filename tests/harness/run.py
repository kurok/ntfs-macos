#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Harness CLI.

  run.py --env A|B|host [--cases ID,PATTERN..] [--out DIR] [--list]
         [--local] [--vm NAME] [--git-sha SHA --dirty true|false] [--host-os TEXT]

--local runs the cases in this process (what executes inside the container or the guest).
Without --local, env A goes through vm/linux/run.sh and env B through the tart guest
agent (--vm NAME, the VM must be running); the result and evidence are copied back.
Exit: 0 no fail/error (skips allowed), 1 a case failed or errored, 2 usage or setup error.
"""

import argparse
import os
import platform
import shlex
import subprocess
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
TESTS = os.path.dirname(HERE)
REPO = os.path.dirname(TESTS)
sys.path.insert(0, TESTS)
os.environ.setdefault("PYTHONDONTWRITEBYTECODE", "1")
sys.dont_write_bytecode = True

from harness import core, report, runner, transport, volume  # noqa: E402

DEFAULT_ROOTS = ("integration", "compatibility", "stress")
SHIP = ("harness",) + DEFAULT_ROOTS


def detect_os(t=None):
    t = t or transport.LocalTransport()
    r = t.run("sw_vers -productName 2>/dev/null && sw_vers -productVersion && sw_vers -buildVersion && uname -m")
    if r.rc == 0:
        n, v, b, a = r.out.decode().split()[:4]
        return "%s %s (%s) %s" % (n, v, b, a)
    try:
        with open("/etc/os-release", "r") as f:
            for line in f:
                if line.startswith("PRETTY_NAME="):
                    return line.split("=", 1)[1].strip().strip('"') + " " + platform.machine()
    except OSError:
        pass
    return "%s %s" % (platform.system(), platform.release())


def parse_args(argv):
    p = argparse.ArgumentParser(prog="run.py", description=__doc__.split("\n\n")[0])
    p.add_argument("--env", required=True, choices=core.ENVS)
    p.add_argument("--cases", default="", help="comma-separated ids or fnmatch patterns")
    p.add_argument("--out", default=os.path.join(TESTS, "results"), help="results directory")
    p.add_argument("--roots", default="", help="comma-separated case roots (default: integration,compatibility,stress)")
    p.add_argument("--list", action="store_true", help="list cases for the env and exit")
    p.add_argument("--local", action="store_true", help="run in this process")
    p.add_argument("--vm", default="", help="env B: VM name (already running)")
    p.add_argument("--git-sha", default=None)
    p.add_argument("--dirty", choices=("true", "false"), default=None)
    p.add_argument("--host-os", default=os.environ.get("NTFSMAC_HOST_OS"))
    p.add_argument("--tmp", default=None, help="scratch directory root (default: system temp)")
    p.add_argument("--quiet", action="store_true")
    return p.parse_args(argv)


def roots_of(args):
    names = [r for r in args.roots.split(",") if r] or list(DEFAULT_ROOTS)
    return [r if os.path.isabs(r) else os.path.join(TESTS, r) for r in names]


def run_local(args):
    cases = core.select(core.discover(roots_of(args)), args.cases, args.env)
    if args.list:
        for c in cases:
            print("%s\t%s\t%s" % (c.id, ",".join(c.envs), c.title))
        return 0
    sha, dirty = report.git_info(REPO, args.git_sha, None if args.dirty is None else args.dirty == "true")
    started = report.utc_now()
    rid = report.run_id(started, sha, args.env)
    t = transport.LocalTransport()
    results_dir = os.path.abspath(args.out)
    os.makedirs(results_dir, exist_ok=True)
    records = runner.run_cases(cases, args.env, rid, results_dir, t, tmp_root=args.tmp, quiet=args.quiet)
    finished = report.utc_now()
    local_os = detect_os(t)
    if args.env == "host":
        host_os, guest_os = local_os, None
    else:
        host_os, guest_os = args.host_os or "unknown (--host-os not given)", local_os
    macfuse = volume.detect_macfuse(t) if args.env in ("B", "host") else None
    ntfs3g = volume.detect_ntfs3g(t)
    doc = report.build_result(sha, dirty, args.env, host_os, guest_os, macfuse, ntfs3g, started, finished, records)
    path = report.write_result(doc, results_dir)
    sys.stderr.write("result: %s\n" % path)
    print(path)
    return 1 if any(r["status"] in ("fail", "error") for r in records) else 0


def forwarded(args, sha, dirty, host_os):
    out = ["--local", "--env", args.env, "--git-sha", sha, "--dirty", "true" if dirty else "false",
           "--host-os", host_os]
    if args.cases:
        out += ["--cases", args.cases]
    if args.roots:
        out += ["--roots", args.roots]
    if args.quiet:
        out += ["--quiet"]
    return out


def collect(out_bytes, args):
    """Unpack the remote results tree into args.out, return path of the result JSON."""
    import tempfile

    tmp = tempfile.mkdtemp(prefix="ntfsmac-collect-")
    try:
        transport.extract_tar(out_bytes, tmp)
        jsons = [f for f in os.listdir(tmp) if f.endswith(".json")]
        dest = os.path.abspath(args.out)
        os.makedirs(dest, exist_ok=True)
        for dp, _dn, fns in os.walk(tmp):
            for fn in fns:
                src = os.path.join(dp, fn)
                rel = os.path.relpath(src, tmp)
                dst = os.path.join(dest, rel)
                os.makedirs(os.path.dirname(dst), exist_ok=True)
                with open(src, "rb") as a, open(dst, "wb") as b:
                    b.write(a.read())
        return [os.path.join(dest, j) for j in jsons]
    finally:
        import shutil

        shutil.rmtree(tmp, ignore_errors=True)


def orchestrate(args):
    sha, dirty = report.git_info(REPO, args.git_sha, None if args.dirty is None else args.dirty == "true")
    host_os = args.host_os or detect_os()
    pyenv = "PYTHONDONTWRITEBYTECODE=1"
    if args.env == "A":
        t = transport.ContainerTransport(os.path.join(REPO, "vm", "linux", "run.sh"))
        remote_tests = "/work/tests"
        py = "python3"
    elif args.env == "B":
        if not args.vm:
            sys.stderr.write("env B without --local needs --vm NAME\n")
            return 2
        t = transport.VmTransport(args.vm, os.path.join(REPO, "vm", "macos", "tart.sh"))
        d = t.run("mktemp -d /tmp/ntfsmac-h.XXXXXX")
        if d.rc != 0:
            sys.stderr.write("cannot reach guest: %s\n" % d.err.decode("utf-8", "replace"))
            return 2
        remote_tests = d.out.decode().strip()
        t.put_tree(TESTS, SHIP, remote_tests)
        py = "$(if [ -x /usr/bin/python3 ]; then echo /usr/bin/python3; else echo python3; fi)"
    else:
        sys.stderr.write("env host runs with --local only\n")
        return 2
    outdir = "/tmp/ntfsmac-out.$$"
    inner = " ".join(shlex.quote(a) for a in forwarded(args, sha, dirty, host_os))
    script = ("rm -rf %(o)s; mkdir -p %(o)s; rc=0; %(env)s %(py)s %(t)s/harness/run.py %(inner)s --out %(o)s 1>&2 || rc=$?; "
              "tar -cf - -C %(o)s . ; rm -rf %(o)s; exit $rc") % {
        "o": outdir, "env": pyenv, "py": py, "t": remote_tests, "inner": inner}
    r = t.run(script, stream_stderr=True)
    if args.env == "B":
        t.run("rm -rf %s" % shlex.quote(remote_tests))
    if not r.out:
        sys.stderr.write("remote run produced no results (rc=%d)\n" % r.rc)
        return 2
    paths = collect(r.out, args)
    for p in paths:
        print(p)
    return r.rc


def main(argv=None):
    args = parse_args(sys.argv[1:] if argv is None else argv)
    try:
        if args.local or args.list or args.env == "host":
            return run_local(args)
        return orchestrate(args)
    except (core.DiscoveryError, RuntimeError, ValueError) as e:
        sys.stderr.write("run.py: %s\n" % e)
        return 2


if __name__ == "__main__":
    sys.exit(main())
