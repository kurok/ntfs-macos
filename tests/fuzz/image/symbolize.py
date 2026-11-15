# SPDX-License-Identifier: GPL-2.0-or-later
"""Symbolize sanitizer stack frames with addr2line (T-M3-16). Standard library only.

Usage: python3 -I -B symbolize.py LOG > LOG.sym

The container has no llvm-symbolizer, so ASan, UBSan and LeakSanitizer print frames as
"#3 0xaaaabfb042cc  (/path/binary+0x4342cc) (BuildId: ...)". Each such frame is rewritten to
"#3 function file:line" (inlined callers on the lines below). Other lines pass through.
"""
import re
import subprocess
import sys

FRAME = re.compile(r"^(\s*#\d+) 0x[0-9a-f]+\s+\((/[^()\s]+)\+(0x[0-9a-f]+)\)(.*)$")


def resolve(binary, offsets):
    """addr2line over offsets of one binary; {offset: [(function, location)]}."""
    out = {}
    try:
        res = subprocess.run(["addr2line", "-f", "-C", "-i", "-p", "-e", binary] + offsets,
                             capture_output=True, text=True, timeout=120, check=False)
    except (OSError, subprocess.SubprocessError):
        return out
    # -p prints one "function at file:line" per frame, "(inlined by)" lines for inlined callers
    lines = res.stdout.splitlines()
    idx = -1
    for ln in lines:
        if not ln.startswith(" (inlined by)"):
            idx += 1
            if idx >= len(offsets):
                break
            out[offsets[idx]] = [ln]
        elif idx >= 0:
            out[offsets[idx]].append(ln.strip())
    return out


def main():
    with open(sys.argv[1], errors="replace") as f:
        text = f.read().splitlines()
    wanted = {}
    for ln in text:
        m = FRAME.match(ln)
        if m:
            wanted.setdefault(m.group(2), set()).add(m.group(3))
    table = {b: resolve(b, sorted(o)) for b, o in wanted.items()}
    for ln in text:
        m = FRAME.match(ln)
        if not m:
            print(ln)
            continue
        info = table.get(m.group(2), {}).get(m.group(3))
        if not info:
            print(ln)
            continue
        print("%s %s" % (m.group(1), info[0]))
        for extra in info[1:]:
            print("      %s" % extra)


if __name__ == "__main__":
    main()
