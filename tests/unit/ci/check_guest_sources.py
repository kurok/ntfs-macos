#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The VM scripts that build daemon code in the guest must copy and compile every source the build needs.

Those scripts hold hand-written lists: a `tar -cf - <files>` run under `cd "$root"` that copies the sources
to the guest, and `cc ... <files>` lines that build them there. A module that gains a new file (the spawn
module got spawn_holder.{c,h}, usermap.{c,h} and usermap_file.{c,h}) breaks the guest build, and the failure
shows only in the full run in the VM. This check finds the gap on the host. For each script:
  - only the tar whose `tar -xf - -C ~/ntfsmac/DIR` goes to the guest build directory of the script
    (`d=$HOME/ntfsmac/DIR`, where the cc lines run) counts; the other tars (NTFS-3G build inputs, images)
    are ignored;
  (a) every local `#include "..."` of every .c file on a cc line, followed recursively through the headers it
      finds (looked up next to the including file, then in the -I paths of that cc line, relative to the
      repository root as in the guest), must be inside the tar list (a listed file, or a file under a listed
      directory); the .c files must be copied too;
  (b) simpler rule instead of a full symbol check: every header of src/daemon reached that way, which has a
      sibling src/daemon/X.c, requires that X.c on the same cc line. A full check (which .c defines a function
      that a listed object calls) needs a C parser and the final link; the sibling rule covers the way modules
      are split here (X.h declares what X.c defines). The link itself is checked by check_guest_link.py
      (ci.guest_link, macOS host), which builds the same cc lines.
A `$var` token in a tar list or a cc line is an error unless it is a known flag variable ($cflags, $ntfs,
$nlibs) or the variable of a `for` loop of the script; so is a script without a guest directory, or with no
or two tar lists for it. A cc line is any `cc ` command in a line (after `&&`, `;`, `(`, `do`, ...); a line that names a src/ or tests/
.c file and is neither a cc line, a tar list nor a comment is an error (a compiler written as `command cc`,
`xcrun cc`, `$CC` or `clang` would otherwise escape the check).
Running the check also runs a self-test: the known-bad fixture guest_sources_bad.txt must be rejected with
the expected findings (and the checker's own parsing, for example of a `)` inside a tar list, must hold).
Usage: check_guest_sources.py REPO [SCRIPT ...]      (exit 0 = no gap, 1 = gap, 2 = usage;
       default scripts: the VM scripts below)
"""
import os
import re
import sys

DEFAULT_SCRIPTS = [
    "tests/integration/daemon/spawn_vm.sh",
    "tests/integration/daemon/sandbox_vm.sh",
    "tests/integration/daemon/errwatch_vm.sh",
    "tests/integration/daemon/journal_vm.sh",
    "tests/integration/daemon/dmn_mirror_vm.sh",
    "tests/integration/daemon/xpc_vm.sh",
]
INC_RE = re.compile(r'^\s*#\s*include\s+"([^"]+)"', re.M)
SRC_C_RE = re.compile(r"\b(?:src|tests)/[\w./-]*\.c\b")
CC_RE = re.compile(r"(?:^|[;&(|]|\bdo\b|\bthen\b)\s*cc\s(.*)$")
# variables that stand for the flags and libraries of libntfs-3g (a line using them needs the guest)
EXTERNAL_VARS = ("$ntfs", "$nlibs")
HERE = os.path.dirname(os.path.abspath(__file__))
FIXTURE = os.path.join(HERE, "guest_sources_bad.txt")


def logical_lines(text):
    """Lines joined at a trailing backslash or pipe (`tar ... |` and the `tart exec` line are one command)."""
    out, cur = [], ""
    for line in text.split("\n"):
        r = line.rstrip()
        if line.endswith("\\"):
            cur += line[:-1] + " "
        elif r.endswith("|") and not r.endswith("||"):
            cur += line + " "
        else:
            out.append(cur + line)
            cur = ""
    if cur:
        out.append(cur)
    return out


def parse(text):
    """-> (guest dir, tar entries of the tar that feeds it, cc entries, errors).
    A cc entry is {line, srcs, incs, args (the arguments without -o and its value), external, pre, post (the
    library flags and libraries of the line as shell words: $ntfs, $nlibs or an inline $(pkg-config ...))}."""
    cflags, loops, errs = {}, {}, []
    tars, cc, guest = [], [], None
    for ln in logical_lines(text):
        s = ln.strip()
        if s.startswith("#"):
            continue
        m = re.match(r'^(\w+)="([^"$]*)"$', s)
        if m:
            cflags[m.group(1)] = m.group(2)
        m = re.match(r"^d=\$HOME/ntfsmac/(\w+)$", s)
        if m:
            guest = m.group(1)
        m = re.match(r"^for (\w+) in ([^;]*); do\b", s)
        if m:
            loops[m.group(1)] = m.group(2).split()
        elif s == "done":
            loops = {}
        m = re.search(r'\(cd "\$root" && tar -cf - (.*?)\)\s*\|.*tar -xf - -C ~/ntfsmac/(\w+)', s)
        if m:
            tars.append((m.group(2), m.group(1).split()))
            continue
        m = CC_RE.search(s)
        if not m:
            f = SRC_C_RE.search(s)
            if f:
                errs.append("a line names %s but is not parsed as a cc line (command cc, xcrun cc, $CC, clang ...?): %s"
                            % (f.group(0), s[:70]))
            continue
        rest = m.group(1)
        external = "pkg-config" in rest or any(v in rest for v in EXTERNAL_VARS)
        for k, v in cflags.items():
            rest = rest.replace("$" + k, v)
        # what a guest build adds around the arguments (guest_build_plan.py): the flags of the libraries before them,
        # their libraries after them; $ntfs and $nlibs are set by the guest script, $(pkg-config ...) is inline
        pre = [v for v in EXTERNAL_VARS[:1] if v in rest]
        post = [v for v in EXTERNAL_VARS[1:] if v in rest]
        for pm in re.finditer(r"\$\(pkg-config ([^)]*)\)", rest):
            (post if "--libs" in pm.group(1) else pre).append(pm.group(0))
        rest = re.sub(r"\"?\$\([^)]*\)\"?", " ", rest)  # $(pkg-config ...)
        rest = re.split(r"&&|\|\||;|\|", rest)[0].rstrip().rstrip(")")
        toks = [t.strip('"') for t in rest.split()]
        args, skip = [], False
        for t in toks:
            if skip:
                skip = False
            elif t == "-o":
                skip = True
            elif t not in EXTERNAL_VARS:
                args.append(t)
        variants = [[]]
        for t in args:
            m = re.search(r"\$(\w+)", t)
            if not m:
                variants = [v + [t] for v in variants]
            elif m.group(1) in loops:
                variants = [v + [t.replace(m.group(0), x)] for v in variants for x in loops[m.group(1)]]
            else:
                errs.append("a cc line has the token %s, which is not a known flag variable: %s" % (t, rest[:70]))
                variants = [v + [t] for v in variants]
        for a in variants:
            srcs = [t for t in a if t.endswith(".c")]
            incs = []
            for i, t in enumerate(a):
                if t == "-I" and i + 1 < len(a):
                    incs.append(a[i + 1])
                elif t.startswith("-I") and len(t) > 2:
                    incs.append(t[2:])
            cc.append({"line": rest, "srcs": srcs, "incs": incs, "args": a, "external": external,
                       "pre": pre, "post": post})
    if guest is None:
        errs.append("no `d=$HOME/ntfsmac/DIR` guest directory assignment found")
    mine = [t for d, t in tars if d == guest]
    if guest is not None and len(mine) != 1:
        errs.append("%d tar lists extract into ~/ntfsmac/%s (need exactly one)" % (len(mine), guest))
    tar = mine[0] if len(mine) == 1 else []
    for t in tar:
        if "$" in t:
            errs.append("the tar list has the token %s with a variable" % t)
    return guest, [t for t in tar if "$" not in t], cc, errs


def covered(path, tar):
    p = os.path.normpath(path)
    return any(p == os.path.normpath(t) or p.startswith(os.path.normpath(t) + "/") for t in tar)


def resolve(repo, inc, from_file, incs):
    for d in [os.path.dirname(from_file)] + incs:
        cand = os.path.normpath(os.path.join(d, inc))
        if os.path.isfile(os.path.join(repo, cand)):
            return cand
    return None


def check_script(repo, script):
    with open(os.path.join(repo, script), encoding="utf-8") as f:
        guest, tar, cc, perrs = parse(f.read())
    errs = ["%s: %s" % (script, e) for e in perrs]
    if not cc:
        return errs + ["%s: no cc line found (the parser does not understand the script)" % script]
    for entry in cc:
        line, srcs, incs = entry["line"], entry["srcs"], entry["incs"]
        seen, reached_daemon_headers = set(), set()
        todo = list(srcs)
        for s in srcs:
            if not os.path.isfile(os.path.join(repo, s)):
                errs.append("%s: cc source %s does not exist" % (script, s))
        while todo:
            f = todo.pop()
            if f in seen or not os.path.isfile(os.path.join(repo, f)):
                continue
            seen.add(f)
            if not covered(f, tar):
                errs.append("%s: %s is not in the tar list" % (script, f))
            with open(os.path.join(repo, f), encoding="utf-8", errors="replace") as fh:
                body = fh.read()
            for inc in INC_RE.findall(body):
                h = resolve(repo, inc, f, incs)
                if h is None:
                    continue  # not a file of this repository (a system or generated header)
                if os.path.dirname(h) == "src/daemon" and h.endswith(".h"):
                    reached_daemon_headers.add(h)
                todo.append(h)
        for h in sorted(reached_daemon_headers):
            c = h[:-2] + ".c"
            if os.path.isfile(os.path.join(repo, c)) and c not in srcs:
                errs.append("%s: %s is included but %s is not on the cc line (%s)" % (
                    script, h, c, " ".join(line.split()[:6])))
    return sorted(set(errs))


def selftest(repo):
    """The bad fixture must be rejected, for each of the reasons it was written for."""
    errs = check_script(repo, FIXTURE)
    text = "\n".join(errs)
    want = [
        "src/daemon/spawn_holder.h is not in the tar list",  # the header the first tar lacks
        "src/daemon/spawn_holder.c is not on the cc line",  # the sibling rule
        "$mystery/y.c, which is not a known flag variable",  # an unknown variable
    ]
    unwanted = [
        "src/daemon/internal.h is not in the tar list",  # after a `)` in the list: it is listed
        "src/daemon/spawn_config.h is not in the tar list",
        "tar lists extract into",  # the other tar (dir `other`) must not count or confuse
    ]
    bad = ["selftest: the fixture was not rejected for: " + w for w in want if w not in text]
    bad += ["selftest: the fixture was rejected wrongly: " + w for w in unwanted if w in text]
    return bad


def main():
    if len(sys.argv) < 2:
        sys.stderr.write("usage: check_guest_sources.py REPO [SCRIPT ...]\n")
        return 2
    repo = sys.argv[1]
    scripts = sys.argv[2:] or DEFAULT_SCRIPTS
    bad = selftest(repo)
    for s in scripts:
        bad += check_script(repo, s)
    for e in bad:
        print("FAIL: " + e)
    if bad:
        return 1
    print("guest sources: %d scripts, every header is copied and every sibling .c is compiled" % len(scripts))
    return 0


if __name__ == "__main__":
    sys.exit(main())
