# SPDX-License-Identifier: GPL-2.0-or-later
"""Leak classification for S-LEAK-01 (T-M5-03, criterion 5.4). Host side, standard library only.

  classify.py patched-map --tarball FILE --patches DIR --out FILE
  classify.py classify --map FILE --leaks FILE [--project-image NAME]...
  classify.py finalize --dir OUT [--repo ROOT] [--started TS] [--project-image NAME]...
  classify.py selftest

`leaks` (Apple) run against the worker that was started with MallocStackLogging=1 prints, for every
leaked root, the allocation stack with the image, symbol and source file:line of each frame:

    STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in fuse_new_common>':
    3   ntfs-3g       0x10048ce94 main + 1392  ntfs-3g.c:4604
    1   libfuse.2.dylib   0x1005d9258 fuse_new_common + 668

The worker is the NTFS-3G program with libntfs-3g linked in statically, so "project code" and
"engine" live in the same image and are told apart by SOURCE LINE, using the patch series
(third_party/patches/ntfs-3g) applied to the pinned tarball:

  project  a frame of the worker image is at a source line that a patch added or changed (R1), or
           the allocation site (the innermost frame of the worker image) is in a function in which
           a patch added or changed a line (R2), or a frame is in an image named by --project-image
           (the daemon and the CLI, src/)
  engine   no project frame, and the stack has a frame in the worker image (upstream NTFS-3G and
           the libraries linked into it) or in libfuse
  platform no frame in the worker image or in libfuse (Apple libraries only)

Rule R2 makes the classification conservative: a leak allocated in a function a patch touched is a
project leak even when the leaking line itself is upstream, so a patch cannot hide a leak behind
an unchanged line. The run fails on any project leak, on a leak without a stack (it cannot be
classified, so zero project leaks is not proven), and on a failed control (see leaks_guest.sh).
"""

import argparse
import difflib
import json
import os
import re
import subprocess
import sys
import tempfile

HERE = os.path.dirname(os.path.abspath(__file__))
REPO = os.path.dirname(os.path.dirname(os.path.dirname(HERE)))
sys.path.insert(0, os.path.join(REPO, "tests", "harness"))

WORKER_IMAGE = "ntfs-3g"
ENGINE_LIBS = ("libfuse.2.dylib", "libfuse.dylib", "libfuse3.4.dylib")
# only the project's own lines count; generated and build-only files are listed but never in a frame
FRAME_RE = re.compile(r"^\s*(\d+)\s+(\S+)\s+(0x[0-9a-fA-F]+)\s+(.*?)\s*$")
SRC_RE = re.compile(r"^(.*?)\s+([\w.+\-]+\.\w+):(\d+)$")
SYM_RE = re.compile(r"^(.*?)(?:\s+\+\s+(\d+))?$")
HEADER_RE = re.compile(r"^STACK OF (\d+) INSTANCES? OF '(.*)':\s*$")
TOTAL_RE = re.compile(r"Process \d+: (\d+) leaks? for (\d+) total leaked bytes")
NODES_RE = re.compile(r"Process \d+: (\d+) nodes malloced for (\d+) KB")


# ---- the patch series: changed lines and touched functions ------------------------------------

def _functions(lines):
    """[(name, first, last)] of the function definitions of a C file in GNU style (the opening
    brace of a function at column 0, its header above it, the closing brace at column 0)."""
    out = []
    i = 0
    while i < len(lines):
        if lines[i].startswith("{"):
            j = i - 1
            while j >= 0 and (lines[j][:1] in (" ", "\t") or not lines[j].strip()) and i - j < 40:
                j -= 1
            name = None
            k = j
            while k >= 0 and i - k < 40:
                m = re.search(r"([A-Za-z_]\w*)\s*\(", lines[k])
                if m and not lines[k][:1] in (" ", "\t", "#", "/", "*"):
                    name = m.group(1)
                    break
                k -= 1
                if k >= 0 and lines[k].rstrip().endswith((";", "}", "*/")):
                    break
            end = i
            while end < len(lines) and not lines[end].startswith("}"):
                end += 1
            if name:
                out.append((name, i + 1 - (i - k), end + 1))
            i = end
        i += 1
    return out


def changed_lines(old, new):
    """(ranges, touch): [first, last] lines of the new file that a patch added or changed, and the
    one-line ranges next to a pure deletion (a patch that only removes a free() has no new line, but
    it changed the function around the gap, so the function is listed as touched, R2)."""
    ranges, touch = [], []
    for tag, _i1, _i2, j1, j2 in difflib.SequenceMatcher(None, old, new, autojunk=False).get_opcodes():
        if tag in ("replace", "insert"):
            ranges.append([j1 + 1, j2])
        elif tag == "delete":
            touch.append([max(j1, 1), max(j1, 1)])
            touch.append([j1 + 1, j1 + 1])
    return ranges, touch


def build_patched_map(tarball, patch_dir):
    """Apply the series to the pinned tarball as scripts/build-ntfs3g.sh does and diff the result
    against the pristine tree: {basename: {"lines": [[a, b]...], "functions": {name: [first, last]}}}
    in the numbering of the PATCHED file (the one the worker was built from)."""
    patch_dir = os.path.abspath(patch_dir)
    series = sorted(f for f in os.listdir(patch_dir) if re.match(r"^\d{4}-.*\.patch$", f))
    with tempfile.TemporaryDirectory(prefix="leakmap.") as tmp:
        pristine, patched = os.path.join(tmp, "a"), os.path.join(tmp, "b")
        for d in (pristine, patched):
            os.mkdir(d)
            subprocess.run(["tar", "-xzf", tarball, "-C", d, "--strip-components=1"], check=True)
        files = []
        for p in series:
            with open(os.path.join(patch_dir, p), "r", encoding="utf-8", errors="replace") as f:
                text = f.read()
            for m in re.finditer(r"^\+\+\+ b/(\S+)", text, re.M):
                if m.group(1) not in files:
                    files.append(m.group(1))
            r = subprocess.run(["patch", "-p1", "--batch", "--forward", "-i", os.path.join(patch_dir, p)],
                               cwd=patched, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, check=False)
            if r.returncode != 0:
                raise RuntimeError("patch %s does not apply: %s" % (p, r.stdout.decode("utf-8", "replace")))
        res = {}
        for rel in files:
            if not rel.endswith((".c", ".h")):
                continue
            new_path = os.path.join(patched, rel)
            if not os.path.isfile(new_path):
                continue
            with open(new_path, "r", encoding="utf-8", errors="replace") as f:
                new = f.read().split("\n")
            old = []
            if os.path.isfile(os.path.join(pristine, rel)):
                with open(os.path.join(pristine, rel), "r", encoding="utf-8", errors="replace") as f:
                    old = f.read().split("\n")
            ranges, touch = changed_lines(old, new)
            funcs = {}
            for name, first, last in _functions(new):
                if any(a <= last and b >= first for a, b in ranges + touch):
                    funcs[name] = [first, last]
            ent = res.setdefault(os.path.basename(rel), {"lines": [], "functions": {}})
            ent["lines"].extend(ranges)
            ent["functions"].update(funcs)
    return {"series": series, "files": res}


# ---- parsing `leaks` -------------------------------------------------------------------------

def parse_leaks(text):
    """{"leaks": n, "bytes": n, "roots": [{"label", "instances", "frames": [...]}], "stacks": bool}"""
    total = None
    m = TOTAL_RE.search(text)
    if m:
        total = (int(m.group(1)), int(m.group(2)))
    elif re.search(r"Process \d+: 0 leaks for 0 total leaked bytes", text):
        total = (0, 0)
    roots = []
    cur = None
    for line in text.split("\n"):
        h = HEADER_RE.match(line)
        if h:
            cur = {"label": h.group(2), "instances": int(h.group(1)), "frames": []}
            roots.append(cur)
            continue
        if cur is None:
            continue
        if line.startswith("===="):
            cur = None
            continue
        f = FRAME_RE.match(line)
        if not f:
            continue
        idx, image, addr, rest = int(f.group(1)), f.group(2), f.group(3), f.group(4)
        src = SRC_RE.match(rest)
        file = line_no = None
        if src:
            rest, file, line_no = src.group(1), src.group(2), int(src.group(3))
        sym = SYM_RE.match(rest.strip())
        cur["frames"].append({"index": idx, "image": image, "symbol": (sym.group(1) if sym else rest).strip(),
                              "file": file, "line": line_no})
    return {"leaks": total[0] if total else None, "bytes": total[1] if total else None,
            "roots": roots, "parsed_total": total is not None}


def in_ranges(ranges, n):
    return any(a <= n <= b for a, b in ranges)


# Frames that are not the allocation site: the allocators' own wrappers and the C library's string
# helpers that allocate for their caller. The site is the first frame that is none of these.
WORKER_ALLOC_WRAPPERS = ("ntfs_malloc", "ntfs_calloc", "ntfs_realloc", "ntfs_alloc", "ntfs_strdup",
                         "ntfs_ucsndup", "ntfs_ucsdup", "ntfs_mbsndup", "ntfs_dup")
LIBC_ALLOC_HELPERS = ("strdup", "strndup", "asprintf", "vasprintf", "calloc", "realloc", "reallocf",
                      "malloc", "valloc", "posix_memalign", "aligned_alloc", "_vasprintf")


def _is_wrapper(f, worker_image):
    if f["image"].startswith("libsystem_malloc"):
        return True
    if f["image"].startswith("libsystem_c") and f["symbol"] in LIBC_ALLOC_HELPERS:
        return True
    if f["image"] == worker_image and (f["symbol"] in WORKER_ALLOC_WRAPPERS
                                       or re.match(r"^ntfs_\w*(dup|alloc)\w*$", f["symbol"])):
        return True
    return False


def classify_root(root, pmap, project_images=(), worker_image=WORKER_IMAGE):
    """(class, reason) for one parsed root; frames ordered as printed (highest index = outermost)."""
    files = pmap.get("files", {})
    frames = sorted(root["frames"], key=lambda f: f["index"])  # index 0 = innermost
    for f in frames:
        if f["image"] in project_images:
            return "project", "frame in %s (%s)" % (f["image"], f["symbol"])
    worker = [f for f in frames if f["image"] == worker_image]
    for f in worker:
        ent = files.get(f["file"] or "")
        if ent and f["line"] is not None and in_ranges(ent["lines"], f["line"]):
            return "project", "R1: %s at %s:%d is a line added or changed by the patch series" % (
                f["symbol"], f["file"], f["line"])
    if worker:
        f = worker[0]
        # R2: the allocation site, skipping the allocator wrappers, is in the worker image and in a function a
        # patch changed. If a library frame (libfuse) is the site, the library allocated and the worker frames
        # are just its callers (R1 still covers a patched call line).
        site = next((x for x in frames if not _is_wrapper(x, worker_image)), None)
        if site is not None and site["image"] == worker_image:
            ent = files.get(site["file"] or "")
            if ent and site["symbol"] in ent["functions"]:
                return "project", "R2: allocation site %s (%s:%s) is a function the patch series changed" % (
                    site["symbol"], site["file"], site["line"])
            if not site["file"]:
                for fn, ent2 in files.items():
                    if site["symbol"] in ent2["functions"]:
                        return "project", "R2: allocation site %s (no line info) is a function the patch series changed (%s)" % (
                            site["symbol"], fn)
        return "engine", "worker image frames only at unpatched lines (innermost %s %s:%s)" % (
            f["symbol"], f["file"], f["line"])
    if any(f["image"] in ENGINE_LIBS for f in frames):
        return "engine", "libfuse"
    return "platform", "no frame in the worker image or libfuse"


def classify_text(text, pmap, project_images=(), worker_image=WORKER_IMAGE):
    p = parse_leaks(text)
    problems = []
    if p["leaks"] is None:
        problems.append("the leaks output has no summary line (leaks did not complete)")
    out_roots = []
    for r in p["roots"]:
        cls, why = classify_root(r, pmap, project_images, worker_image)
        inner = [f for f in sorted(r["frames"], key=lambda f: f["index"]) if "malloc" not in f["image"]]
        out_roots.append({"label": r["label"], "instances": r["instances"], "class": cls, "reason": why,
                          "site": ["%s %s%s" % (f["image"], f["symbol"],
                                                (" %s:%d" % (f["file"], f["line"])) if f["file"] else "")
                                   for f in inner[:4]]})
    if p["leaks"]:
        if not p["roots"]:
            problems.append("%d leak(s) reported without any allocation stack (MallocStackLogging was not in "
                            "effect): they cannot be classified" % p["leaks"])
        elif any(not r["frames"] for r in p["roots"]):
            problems.append("a leaked root has no frames")
    wf = [f for r in p["roots"] for f in r["frames"] if f["image"] == worker_image]
    if wf and not any(f["file"] for f in wf):
        problems.append("frames of %s were found but none has a parsed source file:line: the patched-line rule "
                        "is blind (object files missing, or the output format changed)" % worker_image)
    n_proj = sum(r["instances"] for r in out_roots if r["class"] == "project")
    return {"leaks": p["leaks"], "bytes": p["bytes"], "roots": out_roots, "project_instances": n_proj,
            "problems": problems}


# ---- finalize: one result for the run ---------------------------------------------------------

def _read(path):
    with open(path, "r", encoding="utf-8", errors="replace") as f:
        return f.read()


def finalize(d, repo, started=None, project_images=()):
    import report  # tests/harness/report.py

    with open(os.path.join(d, "patched-map.json"), "r", encoding="utf-8") as f:
        pmap = json.load(f)
    meta = {}
    mp = os.path.join(d, "meta.txt")
    if os.path.isfile(mp):
        for line in _read(mp).splitlines():
            if "=" in line:
                k, v = line.split("=", 1)
                meta[k.strip()] = v.strip()
    scans = []
    for name in sorted(os.listdir(d)):
        if re.match(r"^leaks-worker-scan\d+\.txt$", name):
            c = classify_text(_read(os.path.join(d, name)), pmap, tuple(project_images))
            c["file"] = name
            scans.append(c)
    # control: the pipeline sees a leak with a stack, and a project rule turns it into a FAIL
    control = {"ok": False, "detail": "no control output"}
    cp = os.path.join(d, "leaks-control.txt")
    if os.path.isfile(cp):
        ct = _read(cp)
        w = "leakctl"
        base = classify_text(ct, {"files": {}}, (), w)
        r1 = classify_text(ct, {"files": {"leakctl.c": {"lines": [[24, 24]], "functions": {}}}}, (), w)
        r2 = classify_text(ct, {"files": {"leakctl.c": {"lines": [[1, 1]], "functions": {"ctl_leak_site": [19, 29]}}}}, (), w)
        seen = [r for r in base["roots"] if any("ctl_leak_site" in x for x in r["site"])]
        m1 = [re.search(r"R1: ctl_leak_site at leakctl\.c:(\d+)", r["reason"]) for r in r1["roots"]]
        m1 = [m for m in m1 if m and 19 <= int(m.group(1)) <= 29]
        m2 = [r for r in r2["roots"] if r["reason"].startswith("R2: allocation site ctl_leak_site")]
        ok = bool(seen) and not base["problems"] and base["project_instances"] == 0 and bool(m1) and bool(m2)
        control = {"ok": ok,
                   "detail": "leaks found %d root(s) allocated in ctl_leak_site; classified with no rule: %d project (expected 0); "
                             "line rule 'leakctl.c:24 is patched': %d R1 hit(s) with a parsed frame leakctl.c:19..29 (expected >= 1); "
                             "function rule 'ctl_leak_site is patched': %d R2 hit(s) (expected >= 1)%s"
                             % (len(seen), base["project_instances"], len(m1), len(m2),
                                ("; problems: " + "; ".join(base["problems"])) if base["problems"] else "")}
    problems = [p for s in scans for p in s["problems"]]
    if not scans:
        problems.append("no leaks scan of the worker was produced")
    if meta.get("lineinfo") != "1":
        problems.append("the worker has no source line information in the guest (object files of the build are "
                        "missing): the patched-line rule cannot be applied")
    if meta.get("worker_exit") not in ("0",):
        problems.append("the worker did not exit cleanly after the unmount (worker_exit=%s)" % meta.get("worker_exit"))
    if meta.get("worker_alive") == "0":
        problems.append("the worker died during the run")
    by = {s["file"]: s for s in scans}
    s2, s3 = by.get("leaks-worker-scan2.txt"), by.get("leaks-worker-scan3.txt")
    if s2 and s3 and (s2["leaks"] is not None and s3["leaks"] is not None) and s3["leaks"] > s2["leaks"]:
        problems.append("the leak count grows with the workload: %d after iteration 1, %d after the last" %
                        (s2["leaks"], s3["leaks"]))
    workload_ok = meta.get("workload_rc") == "0"
    proj = sum(s["project_instances"] for s in scans)
    engine = {}
    for s in scans:
        for r in s["roots"]:
            if r["class"] != "project":
                key = "%s: %s" % (r["class"], r["label"])
                engine[key] = max(engine.get(key, 0), r["instances"])
    status_main = "pass" if (scans and proj == 0 and not problems and workload_ok and control["ok"]) else "fail"
    notes = []
    notes.append("%d scan(s) of the worker after %s workload iteration(s); project leaks: %d" %
                 (len(scans), meta.get("iterations", "?"), proj))
    notes.append("engine and platform leaks: " + ("; ".join("%s (x%d)" % (k, v) for k, v in sorted(engine.items())) or "none"))
    if not workload_ok:
        notes.append("the functional workload FAILED (rc %s)" % meta.get("workload_rc"))
    if not control["ok"]:
        notes.append("control failed: " + control["detail"])
    notes.extend(problems)
    for s in scans:
        for r in s["roots"]:
            if r["class"] == "project":
                notes.append("PROJECT LEAK in %s: %s [%s]" % (s["file"], r["label"], r["reason"]))
    with open(os.path.join(d, "classification.json"), "w", encoding="utf-8") as f:
        json.dump({"series": pmap.get("series"), "scans": scans, "control": control, "meta": meta}, f, indent=1)
        f.write("\n")
    lines = ["S-LEAK-01 %s" % status_main.upper(), ""]
    lines += notes
    lines += ["", "control: %s" % control["detail"], ""]
    for s in scans:
        lines.append("%s: %s leak(s), %s bytes, %d root(s)" % (s["file"], s["leaks"], s["bytes"], len(s["roots"])))
        for r in s["roots"]:
            lines.append("  [%s] x%d %s" % (r["class"], r["instances"], r["label"]))
            lines.append("      %s" % r["reason"])
            for site in r["site"]:
                lines.append("      at %s" % site)
    with open(os.path.join(d, "summary.txt"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")
    sha, dirty = report.git_info(repo)
    now = report.utc_now()
    ev = [n for n in ("summary.txt", "classification.json", "patched-map.json", "meta.txt", "leaks-control.txt",
                      "workload.log", "worker.log") if os.path.isfile(os.path.join(d, n))]
    ev += sorted(n for n in os.listdir(d) if re.match(r"^leaks-worker-scan\d+\.txt$", n))
    cases = [
        {"id": "S-LEAK-01", "status": status_main, "duration_s": float(meta.get("seconds", 0) or 0),
         "evidence": ev, "note": "; ".join(notes)},
        {"id": "S-LEAK-01-CONTROL", "status": "pass" if control["ok"] else "fail", "duration_s": 0.0,
         "evidence": [n for n in ("leaks-control.txt",) if n in ev], "note": control["detail"]},
    ]
    doc = report.build_result(sha, dirty, "B", meta.get("host_os") or "macOS (host)", meta.get("guest_os"),
                              meta.get("macfuse"), meta.get("ntfs3g"), started or now, now, cases)
    errs = report.validate(doc)
    if errs:
        raise RuntimeError("result is invalid: " + "; ".join(errs))
    with open(os.path.join(d, "result.json"), "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=1)
        f.write("\n")
    print("\n".join(lines))
    return 0 if status_main == "pass" and control["ok"] else 1


# ---- selftest ---------------------------------------------------------------------------------

_SAMPLE = """leaks Report Version: 4.0, multi-line stacks
Process 1: 3000 nodes malloced for 35399 KB
Process 1: %(n)d leaks for %(b)d total leaked bytes.

%(body)s
Binary Images:
"""

_ENGINE = """STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in fuse_new_common>':
4   dyld                                  0x188100d7c start + 6652
3   ntfs-3g                               0x10048ce94 main + 1392  ntfs-3g.c:4604
2   ntfs-3g                               0x10048d4f0 mount_fuse + 384  ntfs-3g.c:4319
1   libfuse.2.dylib                       0x1005d9258 fuse_new_common + 668
0   libsystem_malloc.dylib                0x189b0089c _malloc_zone_malloc_instrumented_or_legacy + 152
====
    1 (160 bytes) ROOT LEAK: <malloc in fuse_new_common 0x7b14c983c0> [160]
"""

_PATCHED_LINE = """STACK OF 2 INSTANCES OF 'ROOT LEAK: <malloc in ntfs_malloc>':
3   ntfs-3g                               0x1004 ntfs_fuse_setxattr + 90  ntfs-3g.c:4030
2   ntfs-3g                               0x1005 ntfs_malloc + 20  malloc.c:55
0   libsystem_malloc.dylib                0x189b0089c _malloc_zone_malloc_instrumented_or_legacy + 152
====
"""

_PATCHED_FUNC = """STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in ntfs_valid_descr>':
2   ntfs-3g                               0x1004 ntfs_valid_descr + 90  acls.c:500
0   libsystem_malloc.dylib                0x189b0089c _malloc_zone_malloc_instrumented_or_legacy + 152
====
"""

_LIB_UNDER_PATCHED = """STACK OF 1 INSTANCE OF 'ROOT LEAK: <non-object from libfuse.2.dylib>':
3   ntfs-3g                               0x1004 ntfs_fuse_setxattr + 90  ntfs-3g.c:4090
2   libfuse.2.dylib                       0x1005 fuse_session_loop + 252
0   libsystem_malloc.dylib                0x189b0089c _malloc_zone_malloc_instrumented_or_legacy + 152 
====
"""

_WRAPPED = """STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in ntfs_malloc>':
5   ntfs-3g                               0x1002 main + 12  ntfs-3g.c:100
4   ntfs-3g                               0x1003 ntfs_fuse_setxattr + 90  ntfs-3g.c:4090
3   ntfs-3g                               0x1004 ntfs_malloc + 20  malloc.c:55
2   libsystem_c.dylib                     0x1005 strdup + 20
0   libsystem_malloc.dylib                0x189b0089c _malloc_zone_malloc_instrumented_or_legacy + 152 
====
"""

_PLATFORM = """STACK OF 1 INSTANCE OF 'ROOT LEAK: <malloc in CFString>':
2   CoreFoundation                        0x1004 __CFStringCreate + 90
0   libsystem_malloc.dylib                0x189b0089c _malloc_zone_malloc_instrumented_or_legacy + 152
====
"""

_MAP = {"series": ["x"], "files": {"ntfs-3g.c": {"lines": [[4020, 4040]], "functions": {"ntfs_fuse_setxattr": [4000, 4100]}},
                                   "acls.c": {"lines": [[10, 12]], "functions": {"ntfs_valid_descr": [480, 520]}}}}


def selftest():
    n = 0

    def ok(cond, what):
        nonlocal n
        n += 1
        if not cond:
            raise AssertionError("selftest: " + what)

    def run(body, leaks, nbytes, **kw):
        return classify_text(_SAMPLE % {"body": body, "n": leaks, "b": nbytes}, _MAP, **kw)

    r = run("", 0, 0)
    ok(r["leaks"] == 0 and r["project_instances"] == 0 and not r["problems"], "zero leaks is clean")
    r = run(_ENGINE, 1, 160)
    ok([x["class"] for x in r["roots"]] == ["engine"] and r["project_instances"] == 0, "libfuse leak is engine")
    ok(r["roots"][0]["site"][0].startswith("libfuse.2.dylib fuse_new_common"), "innermost non-malloc frame is the site")
    r = run(_ENGINE + _PATCHED_LINE, 3, 400)
    ok(r["project_instances"] == 2 and "R1" in r["roots"][1]["reason"], "frame at a patched line is a project leak (R1)")
    r = run(_PATCHED_FUNC, 1, 40)
    ok(r["project_instances"] == 1 and "R2" in r["roots"][0]["reason"], "allocation in a patched function (R2)")
    r = run(_WRAPPED, 1, 64)
    ok(r["roots"][0]["class"] == "project" and "R2" in r["roots"][0]["reason"] and "ntfs_fuse_setxattr" in r["roots"][0]["reason"],
       "R2 looks through ntfs_malloc and strdup to the allocation site")
    old = ["int f(void)", "{", "\tchar *p = malloc(1);", "\tfree(p);", "\treturn 0;", "}"]
    new = ["int f(void)", "{", "\tchar *p = malloc(1);", "\treturn 0;", "}"]
    rg, tc = changed_lines(old, new)
    ok(rg == [] and tc and _functions(new)[0][0] == "f" and any(a <= 4 <= b for a, b in tc), "a deletion touches its function")
    r = run(re.sub(r"  \S+\.c:\d+", "", _ENGINE), 1, 160)
    ok(any("none has a parsed source file:line" in x for x in r["problems"]), "worker frames without file:line are a problem")
    r = run(_LIB_UNDER_PATCHED, 1, 160)
    ok(r["roots"][0]["class"] == "engine", "libfuse allocation called from a function a patch touched is engine")
    r = run(_PLATFORM, 1, 32)
    ok(r["roots"][0]["class"] == "platform", "Apple-only stack is platform")
    r = run(_ENGINE, 1, 160, project_images=("libfuse.2.dylib",))
    ok(r["project_instances"] == 1, "--project-image makes a whole image project code")
    r = run("", 4, 128)
    ok(r["problems"] and "without any allocation stack" in r["problems"][0], "leaks without stacks fail (not classifiable)")
    r = classify_text("garbage", _MAP)
    ok(r["problems"] and "no summary line" in r["problems"][0], "truncated output is a problem")
    ok(parse_leaks(_SAMPLE % {"body": _ENGINE, "n": 1, "b": 160})["roots"][0]["frames"][1]["line"] == 4604, "frame line parsed")
    # the function scanner on GNU-style C
    src = ["#include <x.h>", "static int foo(int a,", "\t\tint b)", "{", "\treturn a;", "}", "", "int bar(void)", "{", "}"]
    fn = _functions(src)
    ok([f[0] for f in fn] == ["foo", "bar"] and fn[0][2] == 6, "function scanner")
    print("classify selftest: %d checks passed" % n)
    return 0


def main(argv):
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[0])
    sub = ap.add_subparsers(dest="cmd", required=True)
    a = sub.add_parser("patched-map")
    a.add_argument("--tarball", required=True)
    a.add_argument("--patches", required=True)
    a.add_argument("--out", required=True)
    c = sub.add_parser("classify")
    c.add_argument("--map", required=True)
    c.add_argument("--leaks", required=True)
    c.add_argument("--project-image", action="append", default=[])
    f = sub.add_parser("finalize")
    f.add_argument("--dir", required=True)
    f.add_argument("--repo", default=REPO)
    f.add_argument("--started")
    f.add_argument("--project-image", action="append", default=[], help="treat every frame of this image as project code (a test of the FAIL path)")
    sub.add_parser("selftest")
    args = ap.parse_args(argv)
    if args.cmd == "selftest":
        return selftest()
    if args.cmd == "patched-map":
        m = build_patched_map(args.tarball, args.patches)
        with open(args.out, "w", encoding="utf-8") as fh:
            json.dump(m, fh, indent=1)
            fh.write("\n")
        for fn, ent in sorted(m["files"].items()):
            print("%s: %d changed range(s), functions: %s" % (fn, len(ent["lines"]), ", ".join(sorted(ent["functions"])) or "-"))
        return 0
    if args.cmd == "classify":
        with open(args.map, "r", encoding="utf-8") as fh:
            pmap = json.load(fh)
        r = classify_text(_read(args.leaks), pmap, tuple(args.project_image))
        print(json.dumps(r, indent=1))
        return 1 if (r["project_instances"] or r["problems"]) else 0
    return finalize(args.dir, args.repo, args.started, args.project_image)


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
