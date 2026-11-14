# SPDX-License-Identifier: GPL-2.0-or-later
"""I-NAME-01..06 and I-NAME-08: file names on an NTFS volume (T-M3-05, IMPLEMENTATION_PLAN 3.4, U22, R-27).

I-NAME-01  NFC and NFD forms of one name on the same volume (B: one file; A: two files)
I-NAME-02  emoji, CJK, RTL, Cyrillic, Greek, combining marks, odd punctuation, in files and directories
I-NAME-03  255 UTF-16 units: ASCII and multi-byte, creation, listing, lookup, rename, 256 refused
I-NAME-04  windows_names: Windows-invalid names refused with EINVAL, valid look-alikes accepted
I-NAME-05  control for 04: without windows_names the same names are created and survive a remount
I-NAME-06  case-only pairs: distinct files, exact lookup, case-only rename
I-NAME-08  which layer converts: default mount against a `nonfconv` worker, and the worker's debug log

Environment B lists non-ASCII names in NFD while the bytes on disk are NFC (C-REF-01); name sets
are therefore compared in NFC (fsops.remount_check nfc=True) and the cases that are about the form
look at the raw bytes. The A-written pair case is I-NAME-U22 (u22_names.py).
"""

import errno
import os
import sys
import unicodedata

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))

import fsops  # noqa: E402
from fsops import blob, expect_errno, model_files, put, remount_check, slurp  # noqa: E402
from harness import Fail, case  # noqa: E402

NFC = "café"
NFD = unicodedata.normalize("NFD", NFC)


def _start(ctx, mib=128, extra=""):
    vol = fsops.open_volume(ctx, mib)
    fsops.mount(ctx, vol, extra=extra)
    return vol, vol.mnt


def _bytes_list(path):
    return sorted(n for n in os.listdir(os.fsencode(path)) if n != b".fseventsd")


def _form(raw):
    """'NFC', 'NFD' or 'both' (no change by either) for a listed name (bytes)."""
    s = raw.decode("utf-8", "surrogateescape")
    c, d = unicodedata.normalize("NFC", s), unicodedata.normalize("NFD", s)
    if c == d:
        return "same"
    return "NFC" if s == c else "NFD" if s == d else "mixed"


# ---------------------------------------------------------------- I-NAME-01

def _observe(ctx, label, fn, *args):
    """Log what fn(*args) returns or raises; never fails. For behaviour that is recorded, not required."""
    try:
        r = fn(*args)
        ctx.log("observed: %s -> %r" % (label, r))
        return r
    except OSError as e:
        ctx.log("observed: %s -> errno %d" % (label, e.errno))
        return e.errno


@case("I-NAME-01", envs=("A", "B"), title="NFC and NFD form of one name: B has one file, A two (reference)")
def i_name_01(ctx):
    vol, m = _start(ctx)
    j = lambda n: os.path.join(m, n)  # noqa: E731
    put(j(NFC), b"written as NFC")
    listed = _bytes_list(m)
    ctx.log("after creating the NFC name, the listing holds %s" % [(n.hex(), _form(n)) for n in listed])
    if ctx.env == "B":
        if len(listed) != 1 or unicodedata.normalize("NFC", listed[0].decode()) != NFC:
            raise Fail("expected one entry for the NFC name, got %r" % (listed,))
        # First lookup by the other form finds the same file (stat, read, exclusive create).
        if slurp(j(NFD)) != b"written as NFC" or os.stat(j(NFD)).st_ino != os.stat(j(NFC)).st_ino:
            raise Fail("lookup by the NFD form does not reach the file created with the NFC form")
        expect_errno(ctx, ("EEXIST",), open, j(NFD), "xb", desc="O_EXCL create by the NFD form")
        # Write by the other form; the NFD path sees it at once. What the NFC path shows is recorded:
        # macOS keeps one vnode per spelling, so it may serve stale data until the next mount (U22).
        put(j(NFD), b"rewritten via NFD")
        if slurp(j(NFD)) != b"rewritten via NFD":
            raise Fail("a write by the NFD form is not read back by the NFD form")
        stale = _observe(ctx, "read by NFC after a write by NFD (stale if it is the old text)", slurp, j(NFC))
        _observe(ctx, "stat size by NFC / by NFD", lambda: (os.stat(j(NFC)).st_size, os.stat(j(NFD)).st_size))
        if len(_bytes_list(m)) != 1:
            raise Fail("a write by the NFD form created a second entry: %r" % (_bytes_list(m),))
        # Directory: the second mkdir fails; a file made below the NFD path is on disk below the one directory.
        os.mkdir(j("d-" + NFC))
        expect_errno(ctx, ("EEXIST",), os.mkdir, j("d-" + NFD), desc="mkdir by the other form")
        put(j("d-" + NFD + "/inside"), b"in")
        _observe(ctx, "read d-NFC/inside after creating it below d-NFD", slurp, j("d-" + NFC + "/inside"))
        # Rename by one form; the old name by the other form is recorded too.
        os.rename(j(NFD), j("renamed-" + NFD))
        if slurp(j("renamed-" + NFC)) != b"rewritten via NFD":
            raise Fail("rename to an NFD name is not found by its NFC form")
        _observe(ctx, "stat of the old NFC name after a rename of the NFD name", os.stat, j(NFC))
        model = {"renamed-" + NFC: b"rewritten via NFD", "d-" + NFC + "/inside": b"in"}
        remount_check(ctx, vol, model_files(model), ["d-" + NFC], "I-NAME-01", nfc=True)
        # After the remount both forms reach the file, with the last data written.
        fsops.mount(ctx, vol, rw=False)
        try:
            for form in (NFC, NFD):
                if slurp(j("renamed-" + form)) != b"rewritten via NFD" or slurp(j("d-" + form + "/inside")) != b"in":
                    raise Fail("lookup by the %s form after the remount failed" % ("NFC" if form == NFC else "NFD"))
            expect_errno(ctx, ("ENOENT",), os.stat, j(NFC), desc="old name after the remount")
        finally:
            fsops.unmount(ctx, vol)
        ctx.log("stale read by NFC in the same session: %s" % (stale == b"written as NFC"))
    else:
        # Reference: Linux NTFS-3G stores the bytes it is given, the two forms are two names.
        put(j(NFD), b"written as NFD")
        listed = _bytes_list(m)
        if len(listed) != 2 or slurp(j(NFC)) != b"written as NFC" or slurp(j(NFD)) != b"written as NFD":
            raise Fail("on Linux the two forms must be two files, got %r" % (listed,))
        remount_check(ctx, vol, model_files({NFC: b"written as NFC", NFD: b"written as NFD"}), [], "I-NAME-01 (A)")


# ---------------------------------------------------------------- I-NAME-02

UNICODE_NAMES = [
    # emoji: single, ZWJ sequence, skin tone, flag, variation selector, keycap
    "\U0001F642.txt", "\U0001F468‍\U0001F469‍\U0001F467‍\U0001F466", "\U0001F44D\U0001F3FD",
    "\U0001F1FA\U0001F1E6", "heart-♥️", "1️⃣",
    # CJK and Hangul (Hangul composes: NFC syllables, NFD jamo)
    "日本語.txt", "中文测试", "한국어.txt",
    "ひらがな", "がぎぐ",
    # RTL: Hebrew, Arabic, mixed direction, with a right-to-left mark
    "שלום.txt", "مرحبا.txt", "report-שלום-2026.txt",
    "abc‏אבג", "السلام عليكم",
    # other scripts and accents
    "Привіт.txt", "αβγδ", "Ångström.txt", "naïve café",
    "äöüß", "Łódź", "أ",
    # combining marks in canonical order, and out of order (NFC reorders them)
    "q̣̇", "ạ̈", "ố",
    # spaces and punctuation that are legal everywhere
    " leading-space", "inner  double  space", ".hidden", "..two", "a b", "a　b", "it's", "#%&{}$!@=+,;()[]~^`.dat",
    "quote'and`tick",
]


@case("I-NAME-02", envs=("A", "B"), title="emoji, CJK, RTL and accented names as files and directories survive a remount")
def i_name_02(ctx):
    vol, m = _start(ctx)
    model, dirs = {}, set()
    nfc = lambda s: unicodedata.normalize("NFC", s)  # noqa: E731
    seen = set()
    for i, name in enumerate(UNICODE_NAMES):
        if nfc(name) in seen:
            raise Fail("test list holds two names with the same NFC form: %r" % name)
        seen.add(nfc(name))
        data = blob("name02:%d" % i, 100 + i * 37)
        put(os.path.join(m, name), data)
        if slurp(os.path.join(m, name)) != data:
            raise Fail("name %r (U+%s...) reads back other data" % (name, "%04X" % ord(name[0])))
        model[name] = data
        d = "dir-" + name
        os.mkdir(os.path.join(m, d))
        put(os.path.join(m, d, "child-" + name), data[:50])
        dirs.add(d)
        model[d + "/child-" + name] = data[:50]
    ctx.log("created %d names as files and as directories" % len(UNICODE_NAMES))
    listed = set(nfc(n.decode("utf-8", "surrogateescape")) for n in _bytes_list(m))
    missing = sorted(nfc(n) for n in model if "/" not in n and nfc(n) not in listed)
    if missing:
        raise Fail("%d names are not in the listing, first: %r" % (len(missing), missing[0]))
    forms = {}
    for n in _bytes_list(m):
        forms[_form(n)] = forms.get(_form(n), 0) + 1
    ctx.log("forms in the live listing: %s" % forms)
    remount_check(ctx, vol, model_files(model), dirs, "I-NAME-02", nfc=True)
    # Every name still opens by the form it was created with.
    fsops.mount(ctx, vol, rw=False)
    try:
        for name, data in model.items():
            if slurp(os.path.join(m, name)) != data:
                raise Fail("name %r does not open after the remount" % name)
    finally:
        fsops.unmount(ctx, vol)


# ---------------------------------------------------------------- I-NAME-03

@case("I-NAME-03", envs=("B",), title="255 UTF-16 units; 256 refused; names over 255 UTF-8 bytes are listed truncated and cannot be opened by that name")
def i_name_03(ctx):
    vol, m = _start(ctx)
    j = lambda n: os.path.join(m, n)  # noqa: E731
    emoji = "\U0001F642"
    cjk = "\u65E5"
    model, dirs = {}, set()
    # Phase 1: names of at most 255 UTF-8 bytes (and 255 UTF-16 units) behave like any other name.
    for label, name in (("ascii-255", "a" * 255), ("cjk-85 (255 bytes)", cjk * 85), ("emoji-63 (252 bytes)", emoji * 63)):
        data = blob("name03:" + label, 5000)
        put(j(name), data)
        if slurp(j(name)) != data:
            raise Fail("%s: created but not readable" % label)
        model[name] = data
    d = "d" * 255
    os.mkdir(j(d))
    dirs.add(d)
    put(j(d + "/" + "f" * 255), b"deep")
    model[d + "/" + "f" * 255] = b"deep"
    expect_errno(ctx, ("ENAMETOOLONG",), open, j("a" * 256), "xb", desc="256 ASCII units")
    expect_errno(ctx, ("ENAMETOOLONG",), open, j(emoji * 128), "xb", desc="128 emoji (256 units)")
    expect_errno(ctx, ("ENAMETOOLONG",), os.mkdir, j("d" * 256), desc="mkdir 256 units")
    expect_errno(ctx, ("ENAMETOOLONG",), os.rename, j("a" * 255), j("r" * 256), desc="rename to 256 units")
    os.rename(j("a" * 255), j("r" * 255))
    model["r" * 255] = model.pop("a" * 255)
    remount_check(ctx, vol, model_files(model), dirs, "I-NAME-03 (names up to 255 bytes)", nfc=True)

    # Phase 2: 255 UTF-16 units but more than 255 UTF-8 bytes. NTFS-3G stores them (the limit is in
    # units) and opens them by exact name, but its directory listing cuts the name to 255 bytes on Darwin
    # (src/ntfs-3g.c, "Returning file names larger than MAXNAMLEN"). Nothing listed can be opened.
    long_names = {
        "cjk-86 (258 B)": cjk * 86, "cjk-255 (765 B)": cjk * 255,
        "emoji-127 (508 B)": emoji * 127, "emoji-127+b (509 B)": emoji * 127 + "b",
    }
    fsops.mount(ctx, vol)
    os.mkdir(j("long"))
    data = {}
    for label, name in long_names.items():
        data[name] = blob("name03:" + label, 3000)
        put(j("long/" + name), data[name])
        if slurp(j("long/" + name)) != data[name]:
            raise Fail("%s: created but not readable by its name" % label)
    want = sorted(n.encode()[:255] for n in long_names.values())

    def check_listing(when):
        got = _bytes_list(j("long"))
        ctx.log("%s: listing of the directory holds %d entries, lengths %s; expected the names cut to 255 bytes" % (
            when, len(got), sorted(len(g) for g in got)))
        if got != want:
            raise Fail("%s: listing differs from the names cut to 255 bytes: %s" % (when, [g.hex()[:24] for g in got]))
        for g in got:
            try:
                os.stat(os.path.join(os.fsencode(j("long")), g))
            except OSError as e:
                # ENOENT for a cut name that is valid UTF-8, EILSEQ where the cut falls inside a character.
                if e.errno not in (errno.ENOENT, errno.EILSEQ):
                    raise Fail("%s: stat of a listed name: errno %d" % (when, e.errno))
            else:
                raise Fail("%s: a listed (cut) name can be opened; the behaviour changed, update R-27" % when)
        for name, content in data.items():
            if slurp(j("long/" + name)) != content:
                raise Fail("%s: a long name does not open by its exact name" % when)

    check_listing("live")
    fsops.unmount(ctx, vol)
    fsops.mount(ctx, vol, rw=False)
    try:
        check_listing("after remount")
    finally:
        fsops.unmount(ctx, vol)
    fsops.mount(ctx, vol)
    listed = [os.path.join(os.fsencode(j("long")), g) for g in _bytes_list(j("long"))]
    expect_errno(ctx, ("ENOENT", "EILSEQ"), os.unlink, listed[0], desc="unlink of a listed (cut) name")
    expect_errno(ctx, ("ENOTEMPTY", "EEXIST"), os.rmdir, j("long"), desc="rmdir of a directory whose listed names cannot be removed")
    for name in data:
        os.unlink(j("long/" + name))
    os.rmdir(j("long"))
    ctx.log("deleted the long names by exact name, then the directory")
    remount_check(ctx, vol, model_files(model), dirs, "I-NAME-03 (after the long names)", nfc=True)


# ---------------------------------------------------------------- I-NAME-04 / 05

WIN_BAD_FILES = [
    "a:b", "CON", "aux.txt", "trail.", "sp ", 'a"b', "lt<x", "gt>x", "a*b", "pipe|x", "q?x", "back\\slash",
    "NUL", "com1", "LPT9.txt", "con.txt", "Prn", "nul.tar.gz", "ctl\x01x", "two..", "x. ",
]
WIN_BAD_DIRS = ["dir.", "dir ", "d:ir", "AUX", "com5"]
WIN_OK_FILES = [" lead", "ok.txt", "console", "com10", "CON1", "nul_", "a.b", "..x", "lpt", "x.y.z", "dot.in.middle"]


@case("I-NAME-04", envs=("A", "B"), title="windows_names: ten-plus Windows-invalid names refused with EINVAL, valid ones kept")
def i_name_04(ctx):
    vol, m = _start(ctx, extra="windows_names")
    j = lambda n: os.path.join(m, n)  # noqa: E731
    for n in WIN_BAD_FILES:
        expect_errno(ctx, ("EINVAL",), open, j(n), "xb", desc="create %r" % n)
    for n in WIN_BAD_DIRS:
        expect_errno(ctx, ("EINVAL",), os.mkdir, j(n), desc="mkdir %r" % n)
    model = {}
    for n in WIN_OK_FILES:
        model[n] = blob("name04:" + n, 321)
        put(j(n), model[n])
    # Rename onto a forbidden name is refused too, and the source stays.
    for n in ("x:y", "CON", "end."):
        expect_errno(ctx, ("EINVAL",), os.rename, j("ok.txt"), j(n), desc="rename to %r" % n)
    left = set(os.fsdecode(n) for n in _bytes_list(m))
    if left != set(model):
        raise Fail("names on the volume differ from the accepted ones: extra %s, missing %s" % (
            sorted(left - set(model)), sorted(set(model) - left)))
    remount_check(ctx, vol, model_files(model), [], "I-NAME-04", extra="windows_names")


@case("I-NAME-05", envs=("A", "B"), title="control: without windows_names the same names are created and survive a remount")
def i_name_05(ctx):
    vol, m = _start(ctx)
    j = lambda n: os.path.join(m, n)  # noqa: E731
    model, dirs = {}, set()
    for n in WIN_BAD_FILES:
        if "\x01" in n:
            continue  # control characters are a different question (Linux and macOS both accept them; Finder shows them oddly)
        model[n] = blob("name05:" + n, 200)
        put(j(n), model[n])
    for n in WIN_BAD_DIRS:
        os.mkdir(j(n))
        dirs.add(n)
    remount_check(ctx, vol, model_files(model), dirs, "I-NAME-05")


# ---------------------------------------------------------------- I-NAME-06

@case("I-NAME-06", envs=("A", "B"), title="case-only pairs are distinct files and directories; case-only rename works")
def i_name_06(ctx):
    vol, m = _start(ctx)
    j = lambda n: os.path.join(m, n)  # noqa: E731
    model, dirs = {}, set()
    for n in ("Foo.txt", "foo.txt", "FOO.TXT"):
        model[n] = blob("name06:" + n, 1000 + len(n))
        put(j(n), model[n])
    for n in ("Foo.txt", "foo.txt", "FOO.TXT"):
        if slurp(j(n)) != model[n]:
            raise Fail("%s reads back the content of another case variant" % n)
    expect_errno(ctx, ("ENOENT",), os.stat, j("fOo.txt"), desc="a fourth case variant")
    expect_errno(ctx, ("EEXIST",), open, j("foo.txt"), "xb", desc="O_EXCL on the exact name")
    # A directory pair, with files inside that must not mix.
    for n in ("Dir", "dir"):
        os.mkdir(j(n))
        dirs.add(n)
        model[n + "/inner"] = blob("name06:inner:" + n, 77)
        put(j(n + "/inner"), model[n + "/inner"])
    # Case-only rename: the old spelling is gone, the new one is there.
    put(j("Case.txt"), b"case")
    os.rename(j("Case.txt"), j("case.txt"))
    expect_errno(ctx, ("ENOENT",), os.stat, j("Case.txt"), desc="old spelling after case-only rename")
    model["case.txt"] = b"case"
    listed = set(os.fsdecode(n) for n in _bytes_list(m))
    if "case.txt" not in listed or "Case.txt" in listed:
        raise Fail("listing after a case-only rename: %s" % sorted(listed))
    # Renaming one variant onto a different variant replaces only that one.
    put(j("Mix.txt"), b"mix-upper")
    put(j("mix.txt"), b"mix-lower")
    os.rename(j("Mix.txt"), j("mix.txt"))
    model["mix.txt"] = b"mix-upper"
    remount_check(ctx, vol, model_files(model), dirs, "I-NAME-06")


# ---------------------------------------------------------------- I-NAME-08

def _fresh(ctx, vol):
    vol.create()


def _read_or_errno(path):
    try:
        return slurp(path).decode("latin-1")
    except OSError as e:
        return "ERRNO%d" % e.errno


def _names_in(label, raws):
    return [{"NFC": "NFC", "NFD": "NFD"}.get(_form(r), _form(r)) for r in raws]


def _worker_names(log_bytes):
    """Paths the worker was asked about: bytes after 'LOOKUP /' and 'getattr /' lines that start with caf."""
    out = []
    for line in log_bytes.split(b"\n"):
        for tag in (b"LOOKUP /", b"getattr /"):
            if line.startswith(tag) and line[len(tag):].startswith(b"caf"):
                out.append((tag.decode().strip(), line[len(tag):]))
    return out


@case("I-NAME-08", envs=("B",), title="U22: default mount against a nonfconv worker, and the worker's debug log")
def i_name_08(ctx):
    vol = fsops.open_volume(ctx, 64)
    nfc_hex, nfd_hex = NFC.encode().hex(), NFD.encode().hex()
    table = []

    def disk_names():
        fsops.mount(ctx, vol, rw=False, extra="nonfconv")
        try:
            return _bytes_list(vol.mnt)
        finally:
            fsops.unmount(ctx, vol)

    def look(label, extra):
        fsops.mount(ctx, vol, rw=False, extra=extra)
        try:
            listed = _bytes_list(vol.mnt)
            r = {"listing": _names_in(label, listed),
                 "read_nfc": _read_or_errno(os.path.join(vol.mnt, NFC)),
                 "read_nfd": _read_or_errno(os.path.join(vol.mnt, NFD))}
        finally:
            fsops.unmount(ctx, vol)
        ctx.log("%s: %s" % (label, r))
        return r

    rows = {}
    for create_extra in ("", "nonfconv"):
        for forms in (("nfc",), ("nfd",), ("nfc", "nfd")):
            key = "%s|%s" % (create_extra or "default", "+".join(forms))
            _fresh(ctx, vol)
            fsops.mount(ctx, vol, extra=create_extra)
            for f in forms:
                put(os.path.join(vol.mnt, NFC if f == "nfc" else NFD), f.encode())
            fsops.unmount(ctx, vol)
            on_disk = [{nfc_hex: "NFC", nfd_hex: "NFD"}.get(n.hex(), n.hex()) for n in disk_names()]
            rows[key] = {"on_disk": on_disk, "default": look(key + " read by default mount", ""),
                         "nonfconv": look(key + " read by nonfconv mount", "nonfconv")}
            ctx.log("%s: on disk %s" % (key, on_disk))
    with open(ctx.evidence_file("matrix.txt"), "w", encoding="utf-8") as f:
        for k in sorted(rows):
            f.write("%s\n  on disk (bytes seen by nonfconv): %s\n  default mount: %s\n  nonfconv mount: %s\n" % (
                k, rows[k]["on_disk"], rows[k]["default"], rows[k]["nonfconv"]))

    bad = []

    def want(cond, msg):
        if not cond:
            bad.append(msg)

    # Names written through the default mount reach the disk as NFC, whichever form the program used.
    for key in ("default|nfc", "default|nfd", "default|nfc+nfd"):
        want(rows[key]["on_disk"] == ["NFC"], "%s: on disk %s, expected one NFC name" % (key, rows[key]["on_disk"]))
        want(rows[key]["default"]["listing"] == ["NFD"], "%s: default listing %s, expected NFD" % (key, rows[key]["default"]["listing"]))
        want(rows[key]["default"]["read_nfc"] == rows[key]["default"]["read_nfd"] and not rows[key]["default"]["read_nfc"].startswith("ERRNO"),
             "%s: the two forms do not reach the same file" % key)
        want(rows[key]["nonfconv"]["listing"] == ["NFC"], "%s: nonfconv listing %s" % (key, rows[key]["nonfconv"]["listing"]))
        want(rows[key]["nonfconv"]["read_nfc"] == rows[key]["default"]["read_nfc"] and rows[key]["nonfconv"]["read_nfd"].startswith("ERRNO"),
             "%s: nonfconv lookups: %s" % (key, rows[key]["nonfconv"]))
    # With nonfconv the bytes are stored as given and looked up exactly.
    want(rows["nonfconv|nfc"]["on_disk"] == ["NFC"], "nonfconv|nfc on disk %s" % rows["nonfconv|nfc"]["on_disk"])
    want(rows["nonfconv|nfd"]["on_disk"] == ["NFD"], "nonfconv|nfd on disk %s" % rows["nonfconv|nfd"]["on_disk"])
    want(sorted(rows["nonfconv|nfc+nfd"]["on_disk"]) == ["NFC", "NFD"], "nonfconv|nfc+nfd on disk %s" % rows["nonfconv|nfc+nfd"]["on_disk"])
    pair = rows["nonfconv|nfc+nfd"]["nonfconv"]
    want((pair["read_nfc"], pair["read_nfd"]) == ("nfc", "nfd"), "nonfconv pair lookups: %s" % pair)
    # The same NFD-on-disk name under the default mount: listed, but not reachable by either form.
    for key in ("nonfconv|nfd",):
        d = rows[key]["default"]
        want(d["listing"] == ["NFD"] and d["read_nfc"].startswith("ERRNO") and d["read_nfd"].startswith("ERRNO"),
             "%s under the default mount: %s" % (key, d))
    # The pair under the default mount: two identical-looking entries, both forms reach the NFC file only.
    d = rows["nonfconv|nfc+nfd"]["default"]
    want(d["listing"] == ["NFD", "NFD"] and d["read_nfc"] == "nfc" and d["read_nfd"] == "nfc",
         "pair under the default mount: %s" % d)

    # Worker debug log: the names it receives, with the default and with the nonfconv worker.
    _fresh(ctx, vol)
    fsops.mount(ctx, vol, extra="nonfconv")
    put(os.path.join(vol.mnt, NFC), b"nfc")
    put(os.path.join(vol.mnt, NFD), b"nfd")
    fsops.unmount(ctx, vol)
    received = {}
    for label, extra in (("default", ""), ("nonfconv", "nonfconv")):
        logp = ctx.evidence_file("worker-%s.log" % label)
        raw_log = ctx.work + "/worker-%s.raw" % label
        fsops.mount_debug(ctx, vol, raw_log, rw=False, extra=extra)
        try:
            for form, name in (("NFC", NFC), ("NFD", NFD)):
                _read_or_errno(os.path.join(vol.mnt, name))
                _bytes_list(vol.mnt)
        finally:
            fsops.unmount(ctx, vol)
        with open(raw_log, "rb") as f:
            data = f.read()
        names = _worker_names(data)
        received[label] = [(t, n.hex()) for t, n in names]
        with open(logp, "w", encoding="utf-8") as f:
            f.write("# worker debug log lines about names starting with 'caf' (bytes shown as hex; "
                    "NFC=%s, NFD=%s)\n" % (nfc_hex, nfd_hex))
            for t, n in names:
                f.write("%s /%s\n" % (t, n.hex()))
    ctx.log("worker received: %s" % received)
    for label in ("default", "nonfconv"):
        seq = [n for t, n in received[label] if t.startswith("LOOKUP")]
        want(nfc_hex in seq, "%s worker never saw the NFC bytes in a LOOKUP: %s" % (label, seq))
        want(nfd_hex in seq, "%s worker never saw the NFD bytes in a LOOKUP: %s" % (label, seq))
    if bad:
        for b in bad:
            ctx.log("UNEXPECTED " + b)
        raise Fail("%d observations differ from the recorded behaviour, first: %s" % (len(bad), bad[0]))
