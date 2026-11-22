# SPDX-License-Identifier: GPL-2.0-or-later
"""Crash signatures and known-finding matching for the fuzz campaign (T-M5-09).

Python standard library only. Used inside the container (inner.sh) and on the host (triage.py).

Usage:
  python3 -I -B fuzzsig.py sig LOG                 print one signature line (TSV) for the first
                                                   sanitizer or fuzzer event of the log, or nothing
  python3 -I -B fuzzsig.py leaks LOG               one signature line per distinct leak
  python3 -I -B fuzzsig.py ubsan LOG               distinct UBSan locations, "file:line message"
  python3 -I -B fuzzsig.py match KNOWN SIGLINE     print the id of the known finding, or "-"

A signature line has the fields (TAB separated):
  key        stable text: kind + the top three frames, or the oracle message
  kind       asan:<type>, leak, ubsan, oracle, timeout, oom, slow, crash (deadly signal), other
  access     READ, WRITE or - (ASan access line)
  component  engine (libntfs-3g), project (src/, tests/fuzz/) or unknown: the first frame that is
             neither runtime nor libc
  caller     the first frame of the other component, or -
  frames     the top frames, "function@file:line", joined with " < "

The log must have been through tests/fuzz/image/symbolize.py (the container has no llvm-symbolizer).
Frames of the sanitizer runtime, libFuzzer and libc are skipped.
"""
import re
import sys

FRAME_SYM = re.compile(r"^\s*#(\d+) (?!0x)(.+?) at (\S+)\s*$")  # symbolize.py output
FRAME_ASAN = re.compile(r"^\s*#(\d+) 0x[0-9a-f]+ in (\S+) (\S+)")  # sanitizer's own symbolizer
FRAME_RAW = re.compile(r"^\s*#(\d+) 0x[0-9a-f]+\s+\((\S+)\)")
FRAME_INL = re.compile(r"^\s+(\S.*?) at (\S+)\s*$")  # "(inlined by)" continuation line

SKIP_PATH = ("compiler-rt", "libFuzzer", "/lib/", "/usr/lib", "libc.", "libclang_rt", "asan_", "sanitizer_",
             "ubsan_", "lsan_", "interception", "fuzzer/", "FuzzerLoop", "FuzzerDriver", "FuzzerMain",
             "FuzzerInternal", "FuzzerUtil", "FuzzerIO")
SKIP_FUNC = ("___interceptor", "__asan", "__sanitizer", "__interceptor", "fuzzer::", "__libc", "_start", "LLVMFuzzerTestOneInput",
             "main", "malloc", "calloc", "realloc", "free", "operator new", "__ubsan", "ntfs_malloc",
             "ntfs_calloc", "ntfs_realloc", "strdup", "memcpy", "memmove", "memset", "strlen", "strcmp", "__GI_", "posix_memalign")


def short(loc):
    """file:line:col -> file:line with the directory reduced to its last component under the tree."""
    loc = loc.split(" (", 1)[0]
    parts = loc.split(":")
    path = parts[0]
    for marker in ("libntfs-3g/", "/work/", "ntfs-3g-"):
        if marker in path:
            path = path.split(marker, 1)[1]
            if marker == "ntfs-3g-":
                path = path.split("/", 1)[-1]
            break
    line = parts[1] if len(parts) > 1 else "0"
    return "%s:%s" % (path, line)


def component(path):
    if "libntfs-3g" in path or "ntfs-3g-" in path or "/b-eng" in path:
        return "engine"
    if path.startswith(("src/", "tests/fuzz/", "tests/unit/")) or "/work/" in path:
        return "project"
    return "unknown"


def parse_frames(lines, start):
    """Frames after line `start`; stops at the first line that is not a frame or continuation."""
    frames = []
    i = start
    while i < len(lines):
        ln = lines[i]
        m = FRAME_SYM.match(ln)
        if m:
            frames.append((m.group(2), m.group(3)))
        else:
            m = FRAME_ASAN.match(ln)
            if m:
                frames.append((m.group(2), m.group(3)))
            elif FRAME_RAW.match(ln):
                frames.append(("?", FRAME_RAW.match(ln).group(2)))
            elif frames and ln.startswith("      ") and FRAME_INL.match(ln):
                # inlined caller: keep both, the inner frame first is already there
                m = FRAME_INL.match(ln)
                frames.append((m.group(1), m.group(2)))
            elif frames and ln.strip() == "":
                break
            elif frames and not ln.lstrip().startswith("#"):
                break
            elif not frames and i - start > 12:
                break
        i += 1
    return frames


def useful(frames):
    out = []
    for func, loc in frames:
        func = func.split("(", 1)[0]
        if any(s in loc for s in SKIP_PATH) or any(func.startswith(s) for s in SKIP_FUNC):
            continue
        out.append((func, short(loc), component(loc.split(":")[0])))
    return out


def build(kind, access, frames, oracle=None):
    fr = useful(frames)
    top = fr[:3]
    comp = "unknown"
    caller = "-"
    for func, loc, c in fr:
        if comp == "unknown" and c != "unknown":
            comp = c
        elif comp != "unknown" and c not in ("unknown", comp):
            caller = "%s@%s" % (func, loc)
            break
    text = " < ".join("%s@%s" % (f, l) for f, l, _ in top) or "-"
    if oracle is not None:
        key = "%s: %s" % (kind, oracle)
        comp = "project"
        text = text if text != "-" else oracle
    else:
        key = "%s%s %s%s" % (kind, " " + access if access and access != "-" else "",
                             " < ".join(f for f, _, _ in top) or "-", " @" + top[0][1] if top else "")
    return "\t".join([key, kind, access or "-", comp, caller, text])


def events(text):
    """Yield (kind, access, frames, oracle) for the sanitizer and fuzzer events in a log."""
    lines = text.splitlines()
    n = len(lines)
    i = 0
    while i < n:
        ln = lines[i]
        m = re.search(r"ERROR: AddressSanitizer: (\S+)", ln)
        if m:
            t = m.group(1)
            if t == "requested":
                t = "allocation-size-too-big"
            kind = "asan:" + t
            access = "-"
            for j in range(i + 1, min(i + 6, n)):
                a = re.match(r"^(READ|WRITE) of size", lines[j])
                if a:
                    access = a.group(1)
                    break
            frames = parse_frames(lines, i + 1)
            yield (kind, access, frames, None)
            i += 1
            continue
        m = re.search(r"ERROR: LeakSanitizer", ln)
        if m:
            i += 1
            continue
        if re.match(r"^Direct leak of", ln):
            yield ("leak", "-", parse_frames(lines, i + 1), None)
            i += 1
            continue
        m = re.search(r"ERROR: libFuzzer: (timeout|out-of-memory)", ln)
        if m:
            kind = "timeout" if m.group(1) == "timeout" else "oom"
            frames = parse_frames(lines, i + 1)
            yield (kind, "-", frames, None)
            i += 1
            continue
        m = re.search(r"ERROR: libFuzzer: deadly signal", ln)
        if m:
            yield ("crash", "-", parse_frames(lines, i + 1), None)
            i += 1
            continue
        m = re.search(r"oracle failure: (.*)$", ln)
        if m:
            msg = re.sub(r"\(line \d+\)", "", m.group(1)).strip()
            yield ("oracle", "-", [], msg)
            i += 1
            continue
        m = re.search(r"^(\S+?):(\d+):(\d+): runtime error: (.*)$", ln)
        if m:
            msg = re.sub(r"-?\d+(\.\d+)?|0x[0-9a-f]+", "N", m.group(4))
            msg = re.sub(r"'[^']*'", "'T'", msg)
            frames = [("ubsan", "%s:%s" % (m.group(1), m.group(2)))]
            yield ("ubsan", "-", frames, msg)
            i += 1
            continue
        i += 1


def signature(path):
    """Signature line (TSV text) of the replay: the first fatal event. Recoverable UBSan lines (the engine is built with
    -fsanitize-recover=undefined, so they are printed before whatever happens next) count only when no
    fatal event follows; a benign UBSan line must never hide a crash. None if the log has no event."""
    text = open(path, errors="replace").read()
    ub = None
    for kind, access, frames, oracle in events(text):
        if kind == "ubsan":
            if ub is None:
                ub = (frames, oracle)
            continue
        return build(kind, access, frames, oracle)
    if ub is not None:
        file, line = ub[0][0][1].rsplit(":", 1)
        loc = short(file + ":" + line)
        return "\t".join(["ubsan %s %s" % (loc, ub[1]), "ubsan", "-", component(file), "-", loc])
    return None


def cmd_sig(path):
    s = signature(path)
    if s is not None:
        print(s)


def leak_signatures(path):
    """Yield (signature line, leaked bytes) for each distinct direct leak of the log. Indirect leaks are skipped on
    purpose: LeakSanitizer reports an indirect leak only together with the direct leak that holds it, and the
    direct leak's allocation stack is the signature (the root cause)."""
    lines = open(path, errors="replace").read().splitlines()
    seen = set()
    for i, ln in enumerate(lines):
        if re.match(r"^(Direct|Indirect) leak of", ln):
            if ln.startswith("Indirect"):
                continue
            size = re.search(r"leak of (\d+) byte", ln)
            frames = parse_frames(lines, i + 1)
            sig = build("leak", "-", frames)
            if sig not in seen:
                seen.add(sig)
                yield sig, (size.group(1) if size else "0")


def cmd_leaks(path):
    for sig, size in leak_signatures(path):
        print(sig + "\t" + size)


def cmd_ubsan(path):
    seen = {}
    for ln in open(path, errors="replace"):
        m = re.search(r"(\S+?):(\d+):\d+: runtime error: (.*)$", ln)
        if m:
            msg = re.sub(r"-?\d+(\.\d+)?|0x[0-9a-f]+", "N", m.group(3))
            msg = re.sub(r"'[^']*'", "'T'", msg).strip()
            key = "%s %s" % (short(m.group(1) + ":" + m.group(2)), msg)
            seen[key] = seen.get(key, 0) + 1
    for key, n in sorted(seen.items()):
        print("%d\t%s" % (n, key))


def load_known(path):
    rows = []
    for ln in open(path):
        if ln.startswith("#") or not ln.strip():
            continue
        f = ln.rstrip("\n").split("\t")
        if len(f) < 8:
            continue
        rows.append(dict(zip(("id", "kind_re", "frame_re", "component", "status", "fixed_by", "klass", "ref"), f)))
    return rows


def match_known(rows, key, kind, text):
    """First known row whose kind pattern matches the kind and whose frame pattern matches the frames
    text ("function@file:line < function@file:line", or the oracle message, or "file:line" for UBSan).
    The frame patterns are anchored at the top frame in known.tsv. None if no row matches."""
    for r in rows:
        if re.search(r["kind_re"], kind) and re.search(r["frame_re"], text):
            return r
    return None


def main():
    if len(sys.argv) < 3:
        print(__doc__)
        return 2
    cmd = sys.argv[1]
    if cmd == "sig":
        cmd_sig(sys.argv[2])
    elif cmd == "leaks":
        cmd_leaks(sys.argv[2])
    elif cmd == "ubsan":
        cmd_ubsan(sys.argv[2])
    elif cmd == "match":
        f = sys.argv[3].split("\t")
        r = match_known(load_known(sys.argv[2]), f[0], f[1], f[-1])
        print(r["id"] if r else "-")
    else:
        return 2
    return 0


if __name__ == "__main__":
    sys.exit(main())
