#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""report.py - write docs/PERFORMANCE.md from two result files of tests/perf/run.sh (T-M5-07, criterion 5.6).

Usage: report.py --a A/result.json --b B/result.json [--out docs/PERFORMANCE.md]    exit 1: see "Exit"
       report.py --template [--out FILE]     the document without numbers (what is committed until the criterion run)
       report.py selftest

A is NTFS-3G on Linux (the reference), B is this product in the macOS guest. Per phase: the median of both, the slowdown
(median rate of A divided by median rate of B, so above 1 means B is slower), and a flag when it is above 2 (criterion
5.6: regressions above 2x are explained). Each product phase of the sequential and random kinds also gets a ratio against
the baseline of its own environment (the same block sizes on what the file system sits on: A the image file, B the
buffered device node), and the baseline table is in the report. A flagged phase gets an entry under "Explanations" with the
text `PLACEHOLDER: explanation needed`. Entries a person has written (also several lines long, and for phases that are not
flagged any more) are kept when the file is generated again, keyed by the phase name AND by the run they were written
for: `- **seq_write** (P-SEQ-01) [run 1a2b3c4/1a2b3c4/9f8e7d6c]: text`. The run key is printed by the report ("Run key"): the
first 7 characters of the `git_sha` of A, of B and 8 hex characters of the SHA-256 of the canonical JSON of both result files (two runs of
one commit have two keys; a re-serialised copy keeps its key). An entry whose key is not the current run's (an entry from an earlier
run, one without a key: `unkeyed`) is printed as "(from run KEY, not re-confirmed)" and does NOT count as explained.
To confirm an entry for the current run, a person re-reads it against the numbers, changes the key in the brackets to the
current run key, and generates the report again; nothing else confirms an entry. The edit is a change in a commit that a
reviewer sees: it is the review that makes the confirmation count. The report says "comparison invalid"
when the parameters, the image or the commit (`git_sha`) of A and B differ, a tree was dirty or a case failed, "smoke
parameters" when they are below the criterion's, and that VM numbers are not hardware numbers.
Exit: 0 when the document was written and nothing is open; 1 when the comparison is invalid or a flagged phase (above 2x)
has a placeholder or an unconfirmed carried-over entry, or the results use smoke parameters (the document is written
anyway, so that it can be edited); an unreadable or invalid result file exits 1 with a message; 2 for usage errors. `--template` always exits 0.
"""
import argparse
import hashlib
import importlib.util
import io
import json
import os
import re
import sys

LIMIT = 2.0
FULL = {"reps": 5, "seq_mib": 1024, "rnd_mib": 1024, "rnd_ops": 16384, "small_files": 10000, "small_kib": 4,
        "dir_entries": [10000, 100000]}
FULL_EQ = {"block_kib": 1024, "seed": 20260, "fault": None}
PLACEHOLDER = "PLACEHOLDER: explanation needed"
GEN_CMD = "python3 tests/perf/report.py --a <A result.json> --b <B result.json> --out docs/PERFORMANCE.md"
ROWS = [("seq_write", "Sequential write"), ("seq_read", "Sequential read (cold)"), ("rnd_write", "Random 4 KiB write"),
        ("rnd_read", "Random 4 KiB read (cold)"), ("small_create", "Small files: create + write"),
        ("small_stat", "Small files: stat"), ("small_read", "Small files: read (cold)"),
        ("small_delete", "Small files: delete")]
BASE_PRIMARY = {"A": "file", "B": "disk"}
RAWK = [("seq_write", "seq write MiB/s"), ("seq_read", "seq read MiB/s"), ("rnd_write", "rnd 4K write ops/s"),
        ("rnd_read", "rnd 4K read ops/s")]


class ReportError(Exception):
    pass


def harness_validate(doc):
    """the validator of tests/harness/report.py (loaded by path: this file has the same module name)"""
    here = os.path.dirname(os.path.abspath(__file__))
    spec = importlib.util.spec_from_file_location("harness_report", os.path.join(here, "..", "harness", "report.py"))
    mod = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(mod)
    return mod.validate(doc)


def load(path):
    try:
        with open(path, "rb") as f:
            raw = f.read()
        doc = json.loads(raw.decode("utf-8"))
    except (OSError, ValueError) as e:
        raise ReportError("%s: cannot read as JSON: %s" % (path, e))
    errs = harness_validate(doc)
    if errs:
        raise ReportError("%s: not a valid result file: %s" % (path, "; ".join(errs[:4])))
    # the run id hashes the canonical JSON, so a copy that was re-serialised (indentation, key order) keeps its key
    canon = json.dumps(doc, sort_keys=True, separators=(",", ":"), ensure_ascii=True)
    out = {"doc": doc, "cases": {}, "data": {}, "raw": hashlib.sha256(canon.encode("utf-8")).hexdigest()}
    for c in doc["cases"]:
        out["cases"][c["id"]] = c
        m = re.search(r"; data=(\{.*\})$", c.get("note") or "")
        if m:
            try:
                out["data"][c["id"]] = json.loads(m.group(1))
            except ValueError as e:
                raise ReportError("%s: the data of case %s is not JSON: %s" % (path, c["id"], e))
    return out


def by_phase(r):
    return {d["phase"]: d for d in r["data"].values() if "phase" in d}


def case_of(r, phase):
    for cid, d in r["data"].items():
        if d.get("phase") == phase:
            return cid
    return None


def rows_for(a, b):
    rows = list(ROWS)
    sizes = []
    for r in (a, b):
        for d in r["data"].values():
            m = re.match(r"dir_(\d+)_list$", d.get("phase", ""))
            if m and int(m.group(1)) not in sizes:
                sizes.append(int(m.group(1)))
    for n in sorted(sizes):
        rows.append(("dir_%d_list" % n, "Directory of %d entries: readdir (cold)" % n))
        rows.append(("dir_%d_stat" % n, "Directory of %d entries: readdir + stat (cold)" % n))
    return rows


def fmt(v):
    if v is None:
        return "n/a"
    if abs(v) >= 100:
        return "%.0f" % v
    if abs(v) >= 10:
        return "%.1f" % v
    return "%.2f" % v


def mib(kib):
    return "n/a" if kib is None else fmt(kib / 1024.0)


def ratio(a, b):
    if not a or not b:
        return None
    return a / b


UNKEYED = "unkeyed"
CARRIED = re.compile(r"^\(from run [^)]*, not re-confirmed\) ?")


def run_key(a, b):
    """what an explanation is keyed to: the commits of A and B (7 characters each) and the 8 hex characters of
    sha256(A result.json + B result.json), so that two runs of one commit have two keys"""
    h = hashlib.sha256((a.get("raw") or "").encode() + (b.get("raw") or "").encode()).hexdigest()[:8]
    return "%s/%s/%s" % ((a["doc"].get("git_sha") or "?")[:7], (b["doc"].get("git_sha") or "?")[:7], h)


def keep_explanations(path):
    """phase name -> (run key, text; possibly several lines) of the entries a person has written; placeholders are dropped.
    An entry without a `[run KEY]` is keyed `unkeyed`. The "(from run ..., not re-confirmed)" the generator puts in front
    of a carried-over text is not part of the text."""
    kept = {}
    if not path or not os.path.exists(path):
        return kept
    with open(path, "r", encoding="utf-8") as f:
        txt = f.read()
    m = re.search(r"^## Explanations\n(.*?)(?=^## |\Z)", txt, re.S | re.M)
    if not m:
        return kept
    cur = None
    keys = {}
    for line in m.group(1).splitlines():
        mm = re.match(r"^- \*\*([A-Za-z0-9_]+)\*\*(?: \([^)]*\))?(?: \[run ([^\]]*)\])?: ?(.*)$", line)
        if mm:
            cur = mm.group(1)
            keys[cur] = (mm.group(2) or UNKEYED).strip() or UNKEYED
            kept[cur] = [CARRIED.sub("", mm.group(3))]
        elif line.startswith("Entries for phases that are not flagged"):
            cur = None
        elif cur is not None:
            kept[cur].append(line)
    out = {}
    for k, v in kept.items():
        text = "\n".join(v).rstrip()
        if text and not text.startswith("PLACEHOLDER"):
            out[k] = (keys[k], text)
    return out


def smoke_reasons(r):
    p = r["data"].get("P-RUN-01", {}).get("params")
    if not p:
        return ["no parameters recorded"]
    why = []
    for k, v in FULL.items():
        got = p.get(k)
        if k == "dir_entries":
            if sorted(got or []) != sorted(v):
                why.append("directory sizes %s (criterion %s)" % (got, v))
        elif got is None or got < v:
            why.append("%s %s (criterion %s)" % (k, got, v))
    for k, v in FULL_EQ.items():
        if p.get(k) != v:
            why.append("%s %s (criterion %s)" % (k, p.get(k), v))
    return why


def baseline_of(r, env, phase):
    """median of the baseline phase that matches a product phase, in the primary target of the environment"""
    t = BASE_PRIMARY[env]
    d = by_phase(r).get("raw_%s_%s" % (t, phase))
    return d["median"] if d else None


def render(a, b, prev):
    return render_ex(a, b, prev)[0]


def render_ex(a, b, prev):
    """(document, open): `open` lists what is still open, and is empty only when the report may be quoted as it is"""
    if a is None or b is None:
        return template(), []
    ph_a, ph_b = by_phase(a), by_phase(b)
    run_a, run_b = a["data"].get("P-RUN-01", {}), b["data"].get("P-RUN-01", {})
    meta_a, meta_b = run_a.get("meta", {}), run_b.get("meta", {})
    why = sorted(set(smoke_reasons(a) + smoke_reasons(b)))
    invalid = []
    rkey = run_key(a, b)
    if a["doc"].get("git_sha") != b["doc"].get("git_sha"):
        invalid.append("A and B are not the same commit (A %s, B %s)" % ((a["doc"].get("git_sha") or "?")[:7],
                                                                        (b["doc"].get("git_sha") or "?")[:7]))
    if run_a.get("params") != run_b.get("params"):
        invalid.append("the parameters of A and B differ")
    if meta_a.get("image_sha256") != meta_b.get("image_sha256"):
        invalid.append("the images of A and B differ")
    for env, r in (("A", a), ("B", b)):
        if r["doc"].get("dirty_tree"):
            invalid.append("the tree of the %s run was dirty" % env)
        bad = [c for c in r["doc"]["cases"] if c["status"] not in ("pass", "skip")]
        if bad:
            invalid.append("%s has failed cases: %s" % (env, ", ".join(c["id"] for c in bad)))
    out = ["# Performance (T-M5-07, criterion 5.6)", ""]
    out.append("<!-- generated by `%s`; do not edit the tables, edit only the entries under Explanations -->" % GEN_CMD)
    out.append("")
    if why:
        out += ["> **SMOKE PARAMETERS, NOT THE CRITERION RUN.** These tables come from a reduced run that checks the tooling "
                "(%s). Do not quote the numbers. The criterion run is `sh tests/perf/run.sh --env A --out DIR` and "
                "`sh tests/perf/run.sh --env B --out DIR` with the default parameters." % "; ".join(why), ""]
    if invalid:
        out += ["> **COMPARISON INVALID: %s.**" % "; ".join(invalid), ""]
    out += ["**VM numbers are not hardware numbers.** Environment B is a macOS guest (Virtualization framework, virtio "
            "block device behind an image file); Environment A is a Linux container (Docker VM on the same physical host, "
            "the image file on the container's file system). Both run on one physical host, but disk, cache and CPU "
            "scheduling are those of virtual machines. Read the ratios, not the absolute values, and do not compare them "
            "with a native Mac or a USB disk (Environment C). The reference is NTFS-3G on Linux with the same "
            "image bytes, `norecover` and the option list of the product.", ""]
    ca, cb = meta_a.get("cpus"), meta_b.get("cpus")
    ma, mb = int(meta_a.get("mem_kib") or 0) // 1024, int(meta_b.get("mem_kib") or 0) // 1024
    if str(ca) != str(cb) or ma != mb:
        out += ["**The machines differ in size:** A has %s CPUs and %d MiB, B has %s CPUs and %d MiB. The cache of the image "
                "file in A is far larger than in B, and A's whole-system CPU is counted over more CPUs." % (ca, ma, cb, mb), ""]
    out += ["## Storage layers", "",
            "The two file systems do not sit on the same kind of storage. A: `ntfs-3g` opens the image as a regular file "
            "(page cache of the container's kernel). B: the worker opens the buffered device node of the image attached "
            "with `hdiutil` (`/dev/diskN`, served by `diskimages-helper` from the image file, FSKit module in between). The "
            "baseline below runs the same block sizes (1 MiB sequential, 4 KiB random) on that storage without a file "
            "system, on scratch images (never the image of the file system), so that the cost of the storage stack can be "
            "told from the cost of the file system. Read phases are best-effort cold (`posix_fadvise` in A, `F_NOCACHE` in "
            "B); the layer below keeps its own cache. A `loop` target (loop device with direct I/O) is made only when the "
            "container may; see the skipped rows.", ""]
    out += ["### Baseline", "", "| Environment | Target | " + " | ".join(h for _k, h in RAWK) + " |",
            "|---|---|" + "---|" * len(RAWK)]
    skipped = []
    for env, r in (("A", a), ("B", b)):
        run = r["data"].get("P-RUN-01", {})
        for t in run.get("raw_targets", []):
            cells = []
            for k, _h in RAWK:
                d = by_phase(r).get("raw_%s_%s" % (t, k))
                cells.append("n/a" if not d else "%s (%s..%s)" % (fmt(d["median"]), fmt(d["min"]), fmt(d["max"])))
            mark = " (primary)" if BASE_PRIMARY[env] == t else ""
            out.append("| %s | %s%s | %s |" % (env, t, mark, " | ".join(cells)))
        for nm, rs in run.get("raw_skipped", []):
            skipped.append("%s %s: %s" % (env, nm, rs))
    out.append("")
    for x in skipped:
        out.append("- baseline target not available, %s" % x)
    if skipped:
        out.append("")
    out += ["## Method", ""]
    out += ["- One tool, `tests/perf/bench.py`, in both environments with the same parameters; the same fixture image "
            "(SHA-256 A `%s`, B `%s`)." % ((meta_a.get("image_sha256") or "?")[:16], (meta_b.get("image_sha256") or "?")[:16]),
            "- Parameters (A / B): %s / %s." % (json.dumps(run_a.get("params")), json.dumps(run_b.get("params"))),
            "- Every phase is repeated; the table shows the median (min..max in the data files). Writes end with `fsync` "
            "(the directory for create and delete); the time of that `fsync` is recorded separately (`fsync_s`) so that "
            "durability cost is visible. Every read is verified against what was written.",
            "- Cold cache: the volume is unmounted and mounted again before every read phase (`umount` / `mount` in A, "
            "`ntfsmac unmount` / `ntfsmac mount` in B); caches are not dropped system wide. This empties the file system "
            "caches and the worker's, not the cache of the image file one level down, which both environments keep.",
            "- Resources: CPU seconds (user + sys) and peak resident set of the worker (`ntfs-3g`) per phase, in B also of "
            "the macFUSE FSKit module processes (all instances) and of `diskimages-helper`; whole-system CPU: in A the busy "
            "time of all CPUs of the container VM (`/proc/stat`), in B the CPU time of every process (a process that exits "
            "during the phase is missed). Sampling every 50 ms (A) and 250 ms (B) with 10 ms CPU resolution: CPU of a phase "
            "shorter than 0.5 s is marked with `*` and is below the sampling resolution.",
            "- A: %s, %s, %s CPUs, %d MiB. B: %s, %s, macFUSE %s, %s CPUs, %d MiB." % (
                meta_a.get("ntfs3g") and "NTFS-3G " + meta_a["ntfs3g"], meta_a.get("kernel"), ca, ma,
                meta_b.get("ntfs3g") and "NTFS-3G " + meta_b["ntfs3g"], meta_b.get("guest_os"), meta_b.get("macfuse"), cb, mb),
            "- Results: `A %s` and `B %s` (dirty tree: A %s, B %s; result schema `ntfsmac-test-result/1`, cases `P-*`)." % (
                a["doc"]["git_sha"][:7], b["doc"]["git_sha"][:7], a["doc"].get("dirty_tree"), b["doc"].get("dirty_tree")),
            "- Run key: `%s` (the explanations below are keyed to it)." % rkey, ""]
    out += ["## Throughput and latency", "",
            "Slowdown is the median rate of A divided by the median rate of B: 1.0 is parity, above 1 means B (this product) "
            "is slower; above 2 is flagged and needs an explanation (criterion 5.6). `vs own baseline` is the rate of the "
            "baseline (A: file, B: buffered device node) divided by the rate of the file system on it: how many times "
            "slower the phase is than its storage, in that environment. `fsync` is the median time of the final `fsync`.", "",
            "| Case | Phase | Unit | A: NTFS-3G on Linux | B: this product | Slowdown | Flag | A vs own baseline | B vs own baseline | A fsync s | B fsync s |",
            "|---|---|---|---|---|---|---|---|---|---|---|"]
    flagged = []
    for key, title in rows_for(a, b):
        da, db = ph_a.get(key), ph_b.get(key)
        cid = case_of(a, key) or case_of(b, key) or key
        if da is None or db is None:
            out.append("| %s | %s | | %s | %s | | **missing** | | | | |" % (
                cid, title, "missing" if da is None else fmt(da["median"]), "missing" if db is None else fmt(db["median"])))
            continue
        ra = ratio(da["median"], db["median"])
        flag = ""
        if ra is None:
            flag = "**no ratio**"
        elif ra > LIMIT:
            flag = "**> 2x: explain**"
            flagged.append((key, cid, title, ra))
        ba, bb = baseline_of(a, "A", key), baseline_of(b, "B", key)
        out.append("| %s | %s | %s | %s (%s..%s) | %s (%s..%s) | %s | %s | %s | %s | %s | %s |" % (
            cid, title, da["unit"], fmt(da["median"]), fmt(da["min"]), fmt(da["max"]), fmt(db["median"]), fmt(db["min"]),
            fmt(db["max"]), fmt(ra), flag, fmt(ratio(ba, da["median"])), fmt(ratio(bb, db["median"])),
            fmt(da.get("fsync_s")), fmt(db.get("fsync_s"))))
    out.append("")
    out += ["## CPU and memory", "",
            "Median CPU seconds over the runs of the phase and the largest peak resident set seen. `Module` is the sum of "
            "the macFUSE FSKit module processes and `helper` is `diskimages-helper` (B only). `System` is the whole-system "
            "CPU described under Method. `*`: the phase is shorter than 0.5 s, the CPU is below the sampling resolution.", "",
            "| Phase | A worker CPU s | A worker RSS MiB | A system CPU s | B worker CPU s | B worker RSS MiB | B module CPU s | B module RSS MiB | B helper CPU s | B system CPU s |",
            "|---|---|---|---|---|---|---|---|---|---|"]
    for key, title in rows_for(a, b):
        x, y = ph_a.get(key) or {}, ph_b.get(key) or {}
        sa = "*" if (x.get("secs_median") or 0) < 0.5 else ""
        sb = "*" if (y.get("secs_median") or 0) < 0.5 else ""
        out.append("| %s | %s%s | %s | %s%s | %s%s | %s | %s%s | %s | %s%s | %s%s |" % (
            title, fmt(x.get("worker_cpu_s")), sa, mib(x.get("worker_rss_kib")), fmt(x.get("sys_cpu_s")), sa,
            fmt(y.get("worker_cpu_s")), sb, mib(y.get("worker_rss_kib")), fmt(y.get("module_cpu_s")), sb,
            mib(y.get("module_rss_kib")), fmt(y.get("helper_cpu_s")), sb, fmt(y.get("sys_cpu_s")), sb))
    out.append("")
    notes = []
    for env, r in (("A", a), ("B", b)):
        for c in r["doc"]["cases"]:
            if c["status"] != "pass":
                notes.append("- %s %s %s: %s" % (env, c["id"], c["status"], (c.get("note") or "").split("; data=")[0]))
    if notes:
        out += ["## Cases that did not pass", ""] + notes + [""]
    out += ["## Explanations", ""]
    flagged_keys = [f[0] for f in flagged]
    open_items = ["comparison invalid: " + x for x in invalid]
    if why:
        open_items.append("smoke parameters, not the criterion run (%s)" % "; ".join(why))
    if not flagged:
        out.append("No phase is more than 2x slower than the Linux reference in these results.")
    else:
        out.append("Every phase with a slowdown above 2 needs an entry here (criterion 5.6), keyed by the phase name and by "
                   "the run it was written for: `- **phase** (case) [run KEY]: text`. The run key of these results is "
                   "`%s`. The entries are written by a person (they may be several lines long). An entry whose key is not "
                   "`%s` is text from another run: it is printed as not re-confirmed and does not count as explained, and "
                   "this report exits 1 while one is left. To confirm an entry, re-read it against the numbers above and "
                   "change the key in its brackets to `%s`; a new flag gets a placeholder to replace." % (rkey, rkey, rkey))
        out.append("")
        for fk, cid, title, r in flagged:
            ent = prev.get(fk)
            if ent is None:
                out.append("- **%s** (%s) [run %s]: %s (%s, slowdown %.1f)" % (fk, cid, rkey, PLACEHOLDER, title, r))
                open_items.append("%s: placeholder, no explanation written for this run" % fk)
            elif ent[0] == rkey:
                out.append("- **%s** (%s) [run %s]: %s" % (fk, cid, rkey, ent[1]))
            else:
                out.append("- **%s** (%s) [run %s]: (from run %s, not re-confirmed) %s" % (fk, cid, ent[0], ent[0], ent[1]))
                open_items.append("%s: explanation is from run %s, not re-confirmed for %s" % (fk, ent[0], rkey))
    stale = [k for k in sorted(prev) if k not in flagged_keys]
    if stale:
        out += ["", "Entries for phases that are not flagged in these results (kept, with the run they were written for; "
                "they do not count towards anything here):", ""]
        for k in stale:
            out.append("- **%s** [run %s]: %s" % (k, prev[k][0], prev[k][1]))
    out.append("")
    return "\n".join(out), open_items


def template():
    return "\n".join([
        "# Performance (T-M5-07, criterion 5.6)", "",
        "<!-- template: no results yet. Generate with `%s` -->" % GEN_CMD, "",
        "> **NO RESULTS YET.** The criterion run has not been made. Run `sh tests/perf/run.sh --env A --out DIR "
        "--save-image IMG` and `sh tests/perf/run.sh --env B --out DIR --image IMG`, then the generator above. The tables "
        "will hold: sequential, random, small-file and directory-listing numbers for A (NTFS-3G on Linux) and B (this "
        "product), the slowdown with a flag above 2x, a raw-storage baseline for both environments and each phase's ratio "
        "against its own baseline, CPU and memory per phase, and the explanations of every flagged phase.", "",
        "**VM numbers are not hardware numbers.** Both environments are virtual machines on one physical host; read the "
        "ratios, not the absolute values.", ""])


def main(argv):
    ap = argparse.ArgumentParser()
    ap.add_argument("--a")
    ap.add_argument("--b")
    ap.add_argument("--out")
    ap.add_argument("--template", action="store_true")
    a = ap.parse_args(argv)
    open_items = []
    if a.template:
        txt = template()
    else:
        if not (a.a and a.b):
            ap.error("--a and --b are required (or --template)")
        try:
            txt, open_items = render_ex(load(a.a), load(a.b), keep_explanations(a.out))
        except ReportError as e:
            sys.stderr.write("report.py: error: %s\n" % e)
            return 1
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(txt)
    else:
        sys.stdout.write(txt)
    for x in open_items:
        sys.stderr.write("report.py: OPEN: %s\n" % x)
    if open_items:
        sys.stderr.write("report.py: exit 1: the report is written, but it cannot be quoted for criterion 5.6 yet "
                         "(see the Explanations section; to confirm an entry, set its run key to the current one)\n")
    return 1 if open_items else 0


def selftest():
    import tempfile

    def mk(rate, sha, fullp=False, extra=None, sha_commit="0" * 40, raw="r1"):
        phases = [("P-SEQ-01", "seq_write", "MiB/s"), ("P-SEQ-02", "seq_read", "MiB/s"), ("P-RND-01", "rnd_write", "ops/s"),
                  ("P-RND-02", "rnd_read", "ops/s"), ("P-SMALL-01", "small_create", "files/s"),
                  ("P-SMALL-02", "small_stat", "files/s"), ("P-SMALL-03", "small_read", "files/s"),
                  ("P-SMALL-04", "small_delete", "files/s"), ("P-DIR-01", "dir_10_list", "entries/s"),
                  ("P-DIR-02", "dir_10_stat", "entries/s"), ("P-RAW-01", "raw_t_seq_write", "MiB/s")]
        cases = []
        for cid, ph, u in phases:
            r = rate(ph)
            d = {"phase": ph, "unit": u, "median": r, "min": r, "max": r, "n": 1, "count": 1, "secs_median": 1}
            cases.append({"id": cid, "status": "pass", "duration_s": 1, "evidence": [], "note": "x; data=" + json.dumps(d)})
        params = dict(FULL, **FULL_EQ) if fullp else {"reps": 2, "seq_mib": 32, "rnd_mib": 32, "rnd_ops": 100,
                                                      "small_files": 10, "small_kib": 4, "dir_entries": [10], "block_kib": 1024,
                                                      "seed": 20260, "fault": None}
        params.update(extra or {})
        run = {"params": params, "platform": {}, "meta": {"image_sha256": sha}, "raw_targets": ["t"], "raw_skipped": [["loop", "no"]]}
        cases.append({"id": "P-RUN-01", "status": "pass", "duration_s": 0, "evidence": [], "note": "x; data=" + json.dumps(run)})
        doc = {"schema": "ntfsmac-test-result/1", "git_sha": sha_commit, "dirty_tree": False, "env": "B", "host_os": "h",
               "guest_os": None, "macfuse_version": None, "ntfs3g_version": None, "started": "2026-01-01T00:00:00Z",
               "finished": "2026-01-01T00:00:01Z", "cases": cases}
        return {"doc": doc, "cases": {c["id"]: c for c in cases}, "raw": raw,
                "data": {c["id"]: json.loads(c["note"].split("; data=")[1]) for c in cases}}

    def slow(ph):
        return 20.0 if ph == "seq_write" else 90.0

    def flat(ph):
        return 100.0

    # smoke parameters: the document says so, and it is not a result to quote: open item, exit 1 even with no flag
    a = mk(flat, "aa")
    b = mk(slow, "aa")
    txt, open_items = render_ex(a, b, {})
    assert "SMOKE PARAMETERS" in txt and "not hardware numbers" in txt and "### Baseline" in txt
    assert txt.count("> 2x: explain") == 1 and "PLACEHOLDER: explanation needed" in txt, txt
    assert "COMPARISON INVALID" not in txt and "baseline target not available, A loop" in txt
    assert any(x.startswith("smoke parameters") for x in open_items) and any("seq_write: placeholder" in x for x in open_items)
    assert any(x.startswith("smoke parameters") for x in render_ex(a, mk(flat, "aa"), {})[1]), "smoke, nothing flagged: still open"

    # the criterion parameters from here on
    fa = mk(flat, "aa", True)
    fb = mk(slow, "aa", True)
    ka = run_key(fa, fb)
    assert re.match(r"^0000000/0000000/[0-9a-f]{8}$", ka), ka
    txt, open_items = render_ex(fa, fb, {})
    assert "SMOKE" not in txt and "Run key: `%s`" % ka in txt and "- **seq_write** (P-SEQ-01) [run %s]: PLACEHOLDER" % ka in txt
    assert len(open_items) == 1 and "seq_write: placeholder" in open_items[0], open_items
    assert "COMPARISON INVALID" in render(fa, mk(flat, "bb", True), {})
    assert "COMPARISON INVALID" in render(fa, mk(flat, "aa", True, extra={"block_kib": 4}), {})
    # A and B of different commits: invalid, also when no phase is flagged
    t2, o2 = render_ex(fa, mk(flat, "aa", True, sha_commit="1" * 40), {})
    assert "COMPARISON INVALID: A and B are not the same commit (A 0000000, B 1111111)" in t2, t2
    assert o2 and "not the same commit" in o2[0], o2
    assert render_ex(fa, mk(flat, "aa", True), {})[1] == [], "criterion parameters, no flag, same commit: nothing open"
    # two runs of one commit have two keys
    kb = run_key(fa, mk(slow, "aa", True, raw="r2"))
    assert kb != ka and kb.startswith("0000000/0000000/"), (ka, kb)

    with tempfile.TemporaryDirectory() as t:
        pa, pb, p = (os.path.join(t, n) for n in ("A.json", "B.json", "P.md"))

        def put(path, r):
            with open(path, "w") as f:
                json.dump(r["doc"], f)

        def run_main():
            """main() on the files as they are; (rc, text of P.md, stderr)"""
            err = io.StringIO()
            old = sys.stderr
            sys.stderr = err
            try:
                rc = main(["--a", pa, "--b", pb, "--out", p])
            finally:
                sys.stderr = old
            text = ""
            if os.path.exists(p):
                with open(p) as f:
                    text = f.read()
            return rc, text, err.getvalue()

        def gen(body=None, ra=fa, rb=fb):
            """write the fake result files, put `body` (the Explanations entries) into P.md, run main"""
            put(pa, ra)
            put(pb, rb)
            if body is not None:
                with open(p, "w") as f:
                    f.write("# P\n\n## Explanations\n\n" + body + "\n")
            elif os.path.exists(p):
                os.remove(p)
            return run_main()

        def entries(text):
            return text.split("## Explanations\n", 1)[1].split("\n\nEntries for")[0]

        put(pa, fa)
        put(pb, fb)
        K = run_key(load(pa), load(pb))  # the key main sees: it hashes the canonical JSON of the files
        # a re-serialised copy (indented, keys sorted) of the same results keeps the key; other content does not
        with open(pa, "w") as f:
            json.dump(fa["doc"], f, indent=4, sort_keys=True)
        with open(pb, "w") as f:
            json.dump(fb["doc"], f, indent=2)
        assert run_key(load(pa), load(pb)) == K, "re-serialised copies must keep the run key"
        fb3 = mk(slow, "aa", True)
        fb3["doc"]["finished"] = "2026-01-01T00:00:05Z"
        put(pb, fb3)
        assert run_key(load(pa), load(pb)) != K
        put(pa, fa)
        put(pb, fb)

        # no explanation yet: a placeholder, exit 1, and again after regenerating from that file
        rc, t1, err = gen()
        assert rc == 1 and PLACEHOLDER in t1 and "OPEN: seq_write: placeholder" in err, (rc, err)
        assert keep_explanations(p) == {}
        rc, t1, _ = gen()
        assert rc == 1
        # a fresh explanation written for this run: explained, exit 0, kept on the next generation (multi-line too)
        rc, t1, err = gen("- **seq_write** (P-SEQ-01) [run %s]: FSKit round trips\n  per 1 MiB write\nsecond line" % K)
        assert rc == 0 and err == "", (rc, err)
        assert "FSKit round trips\n  per 1 MiB write\nsecond line" in t1 and "(from run" not in t1, t1
        assert keep_explanations(p)["seq_write"] == (K, "FSKit round trips\n  per 1 MiB write\nsecond line")
        # the same commit, another run (other result bytes, same numbers): the entry of the first run is carried over
        fb2 = mk(slow, "aa", True)
        fb2["doc"]["finished"] = "2026-01-01T00:00:02Z"
        put(pb, fb2)
        rc, t1b, err = run_main()
        assert rc == 1 and "(from run %s, not re-confirmed)" % K in t1b and "explanation is from run %s" % K in err, (rc, t1b)
        # carried over from another run: printed as not re-confirmed, does not count, exit 1
        rc, t1, err = gen("- **seq_write** (P-SEQ-01) [run abc1234/abc1234/00000000]: old text about the smoke run")
        assert rc == 1 and "(from run abc1234/abc1234/00000000, not re-confirmed) old text about the smoke run" in t1, (rc, t1)
        assert "OPEN: seq_write: explanation is from run abc1234/abc1234/00000000" in err, err
        # regenerating from that output changes nothing: the prefix is not stacked, the key stays, still exit 1
        rc, t3, _ = gen(entries(t1))
        assert rc == 1 and t3.count("not re-confirmed)") == 1 and t3.count("(from run") == 1, t3
        assert "[run abc1234/abc1234/00000000]" in t3
        # the person confirms it: the key is changed to the current run key, exit 0
        rc, t4, err = gen(entries(t3).replace("[run abc1234/abc1234/00000000]", "[run %s]" % K))
        assert rc == 0 and "(from run" not in t4 and "old text about the smoke run" in t4, (rc, t4, err)
        # an entry without a key (the format before keys existed) is "unkeyed", carried over, exit 1
        rc, t5, err = gen("- **seq_write** (P-SEQ-01): written about the smoke run")
        assert rc == 1 and "[run unkeyed]: (from run unkeyed, not re-confirmed) written about the smoke run" in t5, t5
        # a placeholder typed back in is still a placeholder
        rc, t6, _ = gen("- **seq_write** (P-SEQ-01) [run %s]: PLACEHOLDER: explanation needed" % K)
        assert rc == 1
        # different commits: invalid, exit 1
        rc, t7, err = gen("- **seq_write** (P-SEQ-01) [run x]: explained", fa, mk(slow, "aa", True, sha_commit="1" * 40))
        assert rc == 1 and "COMPARISON INVALID" in t7 and "not the same commit" in err, (rc, err)
        # no flagged phase, criterion parameters: exit 0, and the stale entries are kept with their keys, not counted
        rc, t8, err = gen("- **rnd_read** [run abc1234/abc1234/00000000]: an old entry", fa, mk(flat, "aa", True))
        assert rc == 0 and "No phase is more than 2x slower" in t8, (rc, err)
        assert "- **rnd_read** [run abc1234/abc1234/00000000]: an old entry" in t8
        rc, t9, _ = gen(None, fa, mk(flat, "aa", True))
        assert rc == 0
        # smoke parameters exit 1 even when nothing is flagged
        rc, t10, err = gen(None, mk(flat, "aa"), mk(flat, "aa"))
        assert rc == 1 and "OPEN: smoke parameters" in err, (rc, err)
        # an invalid result file (no git_sha) is a message and exit 1, not a traceback; so is a file that is not JSON
        bad = mk(flat, "aa", True)
        del bad["doc"]["git_sha"]
        put(pa, fa)
        put(pb, bad)
        rc, _t, err = run_main()
        assert rc == 1 and "not a valid result file" in err and "git_sha" in err, err
        with open(pb, "w") as f:
            f.write("{not json")
        rc, _t, err = run_main()
        assert rc == 1 and "cannot read as JSON" in err, err
        # the template always exits 0
        assert main(["--template", "--out", os.path.join(t, "T.md")]) == 0
    assert "NO RESULTS YET" in template()
    print("report.py selftest ok")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
