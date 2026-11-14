# SPDX-License-Identifier: GPL-2.0-or-later
"""Command transports: local, container (Environment A), VM (Environment B).

All transports expose run(cmd, input=None, timeout=None) -> Result and derive
put_tree/get_tree from tar streams over stdin/stdout, so no scp or SSH is needed.
"""

import io
import os
import shlex
import subprocess
import tarfile

PRUNE = ("__pycache__",)


class Result(object):
    def __init__(self, rc, out, err):
        self.rc = rc
        self.out = out
        self.err = err


class Transport(object):
    def run(self, cmd, input=None, timeout=None, stream_stderr=False):
        raise NotImplementedError

    def _argv(self, cmd, stdin):
        """argv that runs `cmd` in the environment; stdin says whether stdin must carry data."""
        raise NotImplementedError

    def run_to_file(self, cmd, out_path, timeout=None):
        """Run cmd, stream its stdout into out_path (no copy in memory; for images). Result.out is empty."""
        with open(out_path, "wb") as f:
            p = subprocess.Popen(self._argv(cmd, False), stdin=subprocess.DEVNULL, stdout=f, stderr=subprocess.PIPE)
            try:
                _o, err = p.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                p.kill()
                _o, err = p.communicate()
                return Result(124, b"", (err or b"") + b"\ntimeout")
        return Result(p.returncode, b"", err or b"")

    def run_from_file(self, cmd, in_path, timeout=None):
        """Run cmd with in_path streamed to its stdin (no copy in memory); stdout and stderr are captured."""
        with open(in_path, "rb") as f:
            p = subprocess.Popen(self._argv(cmd, True), stdin=f, stdout=subprocess.PIPE, stderr=subprocess.PIPE)
            try:
                out, err = p.communicate(timeout=timeout)
            except subprocess.TimeoutExpired:
                p.kill()
                out, err = p.communicate()
                return Result(124, out or b"", (err or b"") + b"\ntimeout")
        return Result(p.returncode, out or b"", err or b"")

    def put_tree(self, local_root, names, remote_dir):
        """Copy local_root/<name> for each name into remote_dir (created)."""
        data = make_tar(local_root, names)
        r = self.run("mkdir -p %s && tar -xf - -C %s" % (shlex.quote(remote_dir), shlex.quote(remote_dir)), input=data)
        if r.rc != 0:
            raise RuntimeError("put_tree failed: %s" % r.err.decode("utf-8", "replace"))

    def get_tree(self, remote_dir, local_dir):
        r = self.run("tar -cf - -C %s ." % shlex.quote(remote_dir))
        if r.rc != 0:
            raise RuntimeError("get_tree failed: %s" % r.err.decode("utf-8", "replace"))
        extract_tar(r.out, local_dir)

    def put(self, local_path, remote_path):
        with open(local_path, "rb") as f:
            data = f.read()
        r = self.run("cat > %s" % shlex.quote(remote_path), input=data)
        if r.rc != 0:
            raise RuntimeError("put failed: %s" % r.err.decode("utf-8", "replace"))

    def get(self, remote_path, local_path):
        r = self.run("cat %s" % shlex.quote(remote_path))
        if r.rc != 0:
            raise RuntimeError("get failed: %s" % r.err.decode("utf-8", "replace"))
        with open(local_path, "wb") as f:
            f.write(r.out)


def _popen(argv, input, timeout, stream_stderr):
    p = subprocess.Popen(
        argv,
        stdin=subprocess.PIPE if input is not None else subprocess.DEVNULL,
        stdout=subprocess.PIPE,
        stderr=None if stream_stderr else subprocess.PIPE,
    )
    try:
        out, err = p.communicate(input, timeout=timeout)
    except subprocess.TimeoutExpired:
        p.kill()
        out, err = p.communicate()
        return Result(124, out or b"", (err or b"") + b"\ntimeout")
    return Result(p.returncode, out or b"", err or b"")


class LocalTransport(Transport):
    def _argv(self, cmd, stdin):
        return ["/bin/sh", "-c", cmd]

    def run(self, cmd, input=None, timeout=None, stream_stderr=False):
        return _popen(self._argv(cmd, input is not None), input, timeout, stream_stderr)


class ContainerTransport(Transport):
    """Environment A: vm/linux/run.sh run -- sh -c CMD (repo mounted read-only at /work)."""

    def __init__(self, run_sh):
        self.run_sh = run_sh

    def _argv(self, cmd, stdin):
        return [self.run_sh, "run", "--", "sh", "-c", cmd]

    def run(self, cmd, input=None, timeout=None, stream_stderr=False):
        return _popen(self._argv(cmd, input is not None), input, timeout, stream_stderr)


class VmTransport(Transport):
    """Environment B: tart guest agent. tart.sh exec for plain commands (it closes stdin);
    `tart exec -i` directly when stdin must be streamed. SSH is not used."""

    def __init__(self, vm, tart_sh, tart="tart"):
        self.vm = vm
        self.tart_sh = tart_sh
        self.tart = tart

    def _full(self):
        n = self.vm
        return n if n.startswith("ntfsmac-") else "ntfsmac-" + n

    def _argv(self, cmd, stdin):
        if not stdin:
            return [self.tart_sh, "exec", self.vm, "--", cmd]
        return [self.tart, "exec", "-i", self._full(), "/bin/sh", "-c", cmd]

    def run(self, cmd, input=None, timeout=None, stream_stderr=False):
        return _popen(self._argv(cmd, input is not None), input, timeout, stream_stderr)


def _skip_member(name):
    return any(part in PRUNE for part in name.split("/")) or name.endswith(".pyc")


def make_tar(root, names):
    buf = io.BytesIO()
    with tarfile.open(fileobj=buf, mode="w") as tf:
        for name in names:
            base = os.path.join(root, name)
            if os.path.isdir(base):
                for dp, dn, fns in os.walk(base):
                    dn[:] = sorted(d for d in dn if d not in PRUNE)
                    for fn in sorted(fns):
                        full = os.path.join(dp, fn)
                        arc = os.path.relpath(full, root)
                        if not _skip_member(arc):
                            ti = tf.gettarinfo(full, arc)
                            ti.uid = ti.gid = 0
                            ti.uname = ti.gname = ""
                            with open(full, "rb") as f:
                                tf.addfile(ti, f)
            elif os.path.isfile(base):
                ti = tf.gettarinfo(base, name)
                ti.uid = ti.gid = 0
                ti.uname = ti.gname = ""
                with open(base, "rb") as f:
                    tf.addfile(ti, f)
    return buf.getvalue()


def extract_tar(data, dest):
    """Extract regular files and directories only; refuse absolute or escaping names."""
    os.makedirs(dest, exist_ok=True)
    real = os.path.realpath(dest)
    with tarfile.open(fileobj=io.BytesIO(data), mode="r:") as tf:
        for m in tf.getmembers():
            name = os.path.normpath(m.name)
            if name.startswith("..") or os.path.isabs(name):
                raise RuntimeError("unsafe tar member: %s" % m.name)
            if not (m.isreg() or m.isdir()):
                continue
            target = os.path.join(real, name)
            if m.isdir():
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as f:
                f.write(tf.extractfile(m).read())
