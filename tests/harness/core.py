# SPDX-License-Identifier: GPL-2.0-or-later
"""Case registration, discovery and the per-case context."""

import fnmatch
import hashlib
import importlib.util
import os
import re
import sys
import time

ENVS = ("A", "B", "host")
CASE_ID_RE = re.compile(r"^[A-Z][A-Z0-9]*(-[A-Z0-9]+)+$")
MARKER_RE = re.compile(r"^@case\(", re.M)


class Skip(Exception):
    """Raise to record status skip; the message is the mandatory note."""


class Fail(AssertionError):
    """Raise to record status fail."""


class DiscoveryError(Exception):
    pass


class Case(object):
    def __init__(self, case_id, envs, func, title, source):
        self.id = case_id
        self.envs = tuple(envs)
        self.func = func
        self.title = title
        self.source = source

    def __repr__(self):
        return "Case(%s, envs=%s)" % (self.id, ",".join(self.envs))


_pending = []


def case(case_id, envs=("A", "B"), title=""):
    """Decorator declaring a test case: @case("I-INT-01", envs=("A", "B"))."""
    if not CASE_ID_RE.match(case_id):
        raise DiscoveryError("bad case id: %r" % (case_id,))
    for e in envs:
        if e not in ENVS:
            raise DiscoveryError("bad env %r for %s" % (e, case_id))

    def deco(func):
        _pending.append(Case(case_id, envs, func, title or (func.__doc__ or "").strip(), None))
        return func

    return deco


def discover(roots):
    """Import every .py under roots that declares cases; return cases sorted by id."""
    found = {}
    for root in roots:
        if not os.path.isdir(root):
            continue
        for dirpath, dirnames, filenames in os.walk(root):
            dirnames[:] = sorted(d for d in dirnames if d != "__pycache__" and not d.startswith("."))
            for fn in sorted(filenames):
                if not fn.endswith(".py"):
                    continue
                path = os.path.join(dirpath, fn)
                with open(path, "r", encoding="utf-8") as f:
                    text = f.read()
                if not MARKER_RE.search(text):
                    continue
                modname = "ntfsmac_case_" + hashlib.sha1(path.encode()).hexdigest()[:10]
                spec = importlib.util.spec_from_file_location(modname, path)
                mod = importlib.util.module_from_spec(spec)
                del _pending[:]
                sys.modules[modname] = mod
                try:
                    spec.loader.exec_module(mod)
                except Exception as e:
                    raise DiscoveryError("cannot import %s: %s: %s" % (path, type(e).__name__, e))
                for c in _pending:
                    c.source = path
                    if c.id in found:
                        raise DiscoveryError("duplicate case id %s in %s and %s" % (c.id, found[c.id].source, path))
                    found[c.id] = c
                del _pending[:]
    return [found[k] for k in sorted(found)]


def select(cases, patterns, env):
    """Filter by env and by comma-separated id patterns (fnmatch). Unmatched explicit pattern is an error."""
    pats = [p.strip() for p in patterns.split(",") if p.strip()] if patterns else []
    for p in pats:
        if not any(fnmatch.fnmatchcase(c.id, p) for c in cases):
            raise DiscoveryError("no case matches %r" % p)
    out = []
    for c in cases:
        if env not in c.envs:
            continue
        if pats and not any(fnmatch.fnmatchcase(c.id, p) for p in pats):
            continue
        out.append(c)
    return out


class Ctx(object):
    """Handed to each case function."""

    def __init__(self, case_obj, env, work, evidence_dir, transport, quiet=False):
        self.case = case_obj
        self.env = env
        self.work = work
        self.evidence_dir = evidence_dir
        self.transport = transport
        self.quiet = quiet
        self._cleanups = []
        # A case may fill in guest_os, macfuse_version, ntfs3g_version for the result file when it
        # drives another environment itself (env "host" cases); run.py then uses them.
        self.meta = {}
        os.makedirs(evidence_dir, exist_ok=True)
        self.log_path = os.path.join(evidence_dir, case_obj.id + ".log")
        self._log = open(self.log_path, "a", encoding="utf-8")

    def log(self, msg):
        line = "%s %s" % (time.strftime("%H:%M:%S", time.gmtime()), msg)
        self._log.write(line + "\n")
        self._log.flush()
        if not self.quiet:
            sys.stderr.write("[%s] %s\n" % (self.case.id, msg))
            sys.stderr.flush()

    def log_final(self, msg):
        """Append after close() (the log file is reopened for one line)."""
        with open(self.log_path, "a", encoding="utf-8") as f:
            f.write("%s %s\n" % (time.strftime("%H:%M:%S", time.gmtime()), msg))

    def skip(self, note):
        raise Skip(note)

    def fail(self, msg):
        raise Fail(msg)

    def sh(self, cmd, check=True, timeout=None):
        r = self.transport.run(cmd, timeout=timeout)
        self.log("$ %s -> rc=%d" % (cmd, r.rc))
        if r.out.strip():
            self.log(r.out.decode("utf-8", "replace").rstrip()[-2000:])
        if r.err.strip():
            self.log("stderr: " + r.err.decode("utf-8", "replace").rstrip()[-2000:])
        if check and r.rc != 0:
            raise RuntimeError("command failed (rc=%d): %s" % (r.rc, cmd))
        return r

    def evidence_file(self, suffix):
        """Path of an extra evidence file named <ID>.<suffix> (keep it small)."""
        return os.path.join(self.evidence_dir, "%s.%s" % (self.case.id, suffix))

    def add_cleanup(self, fn):
        self._cleanups.append(fn)

    def volume(self, size_bytes):
        from . import volume

        return volume.open_volume(self, size_bytes)

    def close(self):
        errs = []
        while self._cleanups:
            fn = self._cleanups.pop()
            try:
                fn()
            except Exception as e:  # cleanup must not hide the case result
                errs.append(e)
                self.log("cleanup error: %s: %s" % (type(e).__name__, e))
        self._log.close()
        return errs
