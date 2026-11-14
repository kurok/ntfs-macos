# SPDX-License-Identifier: GPL-2.0-or-later
"""Helpers of the xattr suite (I-XATTR-*, T-M3-08). Not a case module (no case declarations).

xattr calls for both platforms. macOS has no os.setxattr, so the calls go through libc by ctypes
(getxattr/setxattr/listxattr/removexattr, options 0, position 0); on Linux the os.* functions are used.
Every call returns a plain value or an errno number, never raises, so a case can assert on the errno.
Python 3.9, standard library only.
"""

import ctypes
import errno
import hashlib
import os
import subprocess
import sys
import unicodedata

DARWIN = sys.platform == "darwin"
# macOS: XATTR_CREATE 0x2, XATTR_REPLACE 0x4; Linux: 1 and 2.
XATTR_CREATE = 2 if DARWIN else 1
XATTR_REPLACE = 4 if DARWIN else 2

# errno numbers differ between macOS and Linux; the xattr "no such attribute" errno is ENOATTR (93)
# on macOS and ENODATA (61) on Linux.
ENOATTR = 93 if DARWIN else errno.ENODATA
ENOTSUP_SET = set(getattr(errno, n) for n in ("ENOTSUP", "EOPNOTSUPP") if hasattr(errno, n))

# Mount option sets (extra options after backend=fskit,local,norecover,uid,gid).
OPEN = "streams_interface=openxattr"                       # openxattr only
PRODUCT = "windows_names,noatime," + OPEN                  # the options ARCHITECTURE.md 3.3 proposes (M2-options)
XATTR_IF = "windows_names,noatime,streams_interface=xattr"  # what the design had before T-M2-04

# Values used by several cases. FinderInfo is 32 bytes; byte 9 is the low byte of the Finder flags and
# bits 1-3 are the colour label (0x0C = label 6, red). The last byte is set so the value is never all-zero.
QUAR = b"0083;5f5e1000;Safari;3B7C0D6F-0D5E-4C6B-8D63-0123456789AB"
FI_RED = bytes([0] * 9 + [0x0C] + [0] * 21 + [0xFF])
FI_ZERO = bytes(32)

if DARWIN:
    _libc = ctypes.CDLL(None, use_errno=True)
    _libc.getxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                               ctypes.c_uint32, ctypes.c_int]
    _libc.getxattr.restype = ctypes.c_ssize_t
    _libc.setxattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t,
                               ctypes.c_uint32, ctypes.c_int]
    _libc.listxattr.argtypes = [ctypes.c_char_p, ctypes.c_void_p, ctypes.c_size_t, ctypes.c_int]
    _libc.listxattr.restype = ctypes.c_ssize_t
    _libc.removexattr.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_int]
    _libc.copyfile.argtypes = [ctypes.c_char_p, ctypes.c_char_p, ctypes.c_void_p, ctypes.c_uint32]


def _b(s):
    return s if isinstance(s, bytes) else s.encode("utf-8")


def setx(path, name, value, flags=0):
    """0 on success, else the errno."""
    if DARWIN:
        r = _libc.setxattr(_b(path), _b(name), value, len(value), 0, flags)
        return 0 if r == 0 else ctypes.get_errno()
    try:
        os.setxattr(path, name, value, flags)
        return 0
    except OSError as e:
        return e.errno


def getx(path, name):
    """The value (bytes), or an errno (int)."""
    if DARWIN:
        n = _libc.getxattr(_b(path), _b(name), None, 0, 0, 0)
        if n < 0:
            return ctypes.get_errno()
        buf = ctypes.create_string_buffer(n + 1)
        n2 = _libc.getxattr(_b(path), _b(name), buf, n, 0, 0)
        if n2 < 0:
            return ctypes.get_errno()
        return buf.raw[:n2]
    try:
        return os.getxattr(path, name)
    except OSError as e:
        return e.errno


def getx_pos(path, name, size, position):
    """getxattr with a non-zero position (macOS; only the resource fork may use it). Bytes or errno."""
    buf = ctypes.create_string_buffer(size + 1)
    n = _libc.getxattr(_b(path), _b(name), buf, size, position, 0)
    return buf.raw[:n] if n >= 0 else ctypes.get_errno()


def listx(path):
    """List of names (str), or an errno (int)."""
    if DARWIN:
        n = _libc.listxattr(_b(path), None, 0, 0)
        if n < 0:
            return ctypes.get_errno()
        buf = ctypes.create_string_buffer(n + 1)
        n2 = _libc.listxattr(_b(path), buf, n, 0)
        if n2 < 0:
            return ctypes.get_errno()
        return [x.decode("utf-8", "replace") for x in buf.raw[:n2].split(b"\0") if x]
    try:
        return list(os.listxattr(path))
    except OSError as e:
        return e.errno


def removex(path, name):
    if DARWIN:
        r = _libc.removexattr(_b(path), _b(name), 0)
        return 0 if r == 0 else ctypes.get_errno()
    try:
        os.removexattr(path, name)
        return 0
    except OSError as e:
        return e.errno


def xmap(path):
    """{name: bytes} of all xattrs, names in NFC (macOS lists non-ASCII names in NFD); a value that
    cannot be read is recorded as 'errno N' (str)."""
    names = listx(path)
    if isinstance(names, int):
        return {"<list errno>": names}
    out = {}
    for n in names:
        v = getx(path, n)
        out[unicodedata.normalize("NFC", n)] = v if isinstance(v, bytes) else "errno %d" % v
    return out


def short(m):
    """Readable form of an xmap for logs: name -> len:sha8."""
    def one(v):
        return v if isinstance(v, str) else "%d:%s" % (len(v), hashlib.sha256(v).hexdigest()[:8])
    return dict((k, one(v)) for k, v in sorted(m.items()))


def ename(e):
    return "%s(%d)" % (errno.errorcode.get(e, "?"), e) if isinstance(e, int) else repr(e)


def blob(label, n):
    import random
    return random.Random(label).randbytes(n)


def sha(b):
    return hashlib.sha256(b).hexdigest()


def appledouble_files(root):
    """Relative paths of every ._* entry below root (the macOS root metadata dirs are skipped)."""
    out = []
    for dp, dn, fns in os.walk(root):
        if dp == root:
            dn[:] = [d for d in dn if d not in (".fseventsd", ".Spotlight-V100", ".Trashes", "System Volume Information")]
        for n in dn + fns:
            if n.startswith("._"):
                out.append(os.path.relpath(os.path.join(dp, n), root))
    return sorted(out)


def copyfile_all(src, dst):
    """copyfile(3) with COPYFILE_ALL (ACL | STAT | XATTR | DATA), what Finder-style copies call.
    Returns 0 or errno. macOS only."""
    COPYFILE_ALL = 15
    r = _libc.copyfile(_b(src), _b(dst), None, COPYFILE_ALL)
    return 0 if r == 0 else ctypes.get_errno()


def run(argv, timeout=120):
    """argv list -> (rc, text of stdout+stderr). A timeout kills the child and returns rc -9."""
    try:
        p = subprocess.run(argv, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, timeout=timeout)
        return p.returncode, p.stdout.decode("utf-8", "replace").strip()
    except subprocess.TimeoutExpired as e:
        return -9, "timeout: " + (e.stdout or b"").decode("utf-8", "replace")
