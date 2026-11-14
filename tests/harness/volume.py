# SPDX-License-Identifier: GPL-2.0-or-later
"""Pluggable NTFS volume adapters. A case asks ctx.volume(size) and gets the adapter
for its environment, or a Skip with a note when the environment cannot mount yet."""

import os
import re
import shlex
import time

MACFUSE_BUNDLE = "/Library/Filesystems/macfuse.fs"


class VolumeError(Exception):
    pass


def detect_macfuse(transport):
    """Version string of the installed macFUSE, or None. Runs in the environment of `transport`."""
    r = transport.run("test -d %s && /usr/bin/defaults read %s/Contents/Info CFBundleShortVersionString" %
                      (MACFUSE_BUNDLE, MACFUSE_BUNDLE))
    if r.rc != 0:
        return None
    v = r.out.decode("utf-8", "replace").strip()
    return v or "unknown"


def detect_ntfs3g(transport):
    r = transport.run("ntfs-3g --version 2>&1 | head -1")
    text = r.out.decode("utf-8", "replace").strip()
    m = re.search(r"ntfs-3g\s+(\S+)", text)
    return m.group(1) if r.rc == 0 and m else None


class Volume(object):
    """Abstract volume on an image file. mount/unmount are the pluggable parts."""

    def __init__(self, ctx, size_bytes):
        self.ctx = ctx
        self.size = size_bytes
        self.img = os.path.join(ctx.work, "vol.img")
        self.mnt = os.path.join(ctx.work, "mnt")
        self.mounted = False

    def create(self):
        raise NotImplementedError

    def mount(self, rw=True):
        raise NotImplementedError

    def unmount(self):
        raise NotImplementedError

    def cleanup(self):
        if self.mounted:
            try:
                self.unmount()
            except Exception as e:
                self.ctx.log("cleanup unmount failed: %s" % e)


class LinuxNtfs3g(Volume):
    """Environment A: ntfs-3g with internal FUSE on a loop-free image file."""

    def create(self):
        c = self.ctx
        os.makedirs(self.mnt, exist_ok=True)
        with open(self.img, "wb") as f:
            f.truncate(self.size)
        c.sh("mkntfs -F -Q -L ntfsmac %s" % shlex.quote(self.img))
        c.add_cleanup(self.cleanup)

    def mount(self, rw=True):
        mode = "rw,big_writes" if rw else "ro"
        self.ctx.sh("ntfs-3g %s %s -o %s" % (shlex.quote(self.img), shlex.quote(self.mnt), mode))
        self.mounted = True

    def _driver_running(self):
        for pid in os.listdir("/proc"):
            if not pid.isdigit():
                continue
            try:
                with open("/proc/%s/cmdline" % pid, "rb") as f:
                    args = f.read().split(b"\0")
            except OSError:
                continue
            if args and os.path.basename(args[0].decode("utf-8", "replace")) == "ntfs-3g" and \
                    self.img.encode() in args:
                return True
        return False

    def unmount(self):
        self.ctx.sh("sync")
        self.ctx.sh("umount %s" % shlex.quote(self.mnt))
        self.mounted = False
        # The driver finishes writing back after umount returns; the image is not
        # safe to reopen until the process is gone.
        deadline = time.time() + 30
        while self._driver_running():
            if time.time() > deadline:
                raise VolumeError("ntfs-3g still running 30 s after umount")
            time.sleep(0.2)


def open_volume(ctx, size_bytes):
    """Return a created, unmounted Volume for ctx.env, or raise Skip with a note."""
    from .core import Skip

    t = ctx.transport
    if ctx.env == "A":
        for tool in ("ntfs-3g", "mkntfs", "umount"):
            if t.run("command -v %s" % tool).rc != 0:
                raise VolumeError("%s not found in the Environment A container" % tool)
        vol = LinuxNtfs3g(ctx, size_bytes)
        vol.create()
        return vol
    if ctx.env == "B":
        ver = detect_macfuse(t)
        if ver is None:
            raise Skip("macFUSE is not installed in the guest (checked %s); it is installed by T-M2-02, "
                       "no NTFS mount is possible yet" % MACFUSE_BUNDLE)
        raise Skip("macFUSE %s is present, but the Environment B mount adapter is not written yet "
                   "(lands with T-M2-03)" % ver)
    raise Skip("no volume adapter for env %s" % ctx.env)
