#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""report.py - write docs/PERFORMANCE.md from two result files of tests/perf/run.sh (T-M5-07, criterion 5.6).

Usage: report.py --a A/result.json --b B/result.json [--out docs/PERFORMANCE.md]
       report.py --template [--out FILE]     the document without numbers (what is committed until the criterion run)
       report.py selftest

A is NTFS-3G on Linux (the reference), B is this product in the macOS guest. Per phase: the median of both, the slowdown
(median rate of A divided by median rate of B, so above 1 means B is slower), and a flag when it is above 2 (criterion
5.6: regressions above 2x are explained). Each product phase of the sequential and random kinds also gets a ratio against
the baseline of its own environment (the same block sizes on what the file system sits on: A the image file, B the
buffered device node), and the baseline table is in the report. A flagged phase gets an entry under "Explanations" with the
text `PLACEHOLDER: explanation needed`; entries a person has written (also several lines long, and for phases that are not
flagged any more) are kept when the file is generated again, keyed by the phase name. The report says "comparison
invalid" when the parameters or the image of A and B differ, "smoke parameters" when they are below the criterion's, and
that VM numbers are not hardware numbers.
"""
import argparse
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


def load(path):
    with open(path, "r", encoding="utf-8") as f:
        doc = json.load(f)
    out = {"doc": doc, "cases": {}, "data": {}}
    for c in doc["cases"]:
        out["cases"][c["id"]] = c
        m = re.search(r"; data=(\{.*\})$", c.get("note") or "")
        if m:
            out["data"][c["id"]] = json.loads(m.group(1))
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


def keep_explanations(path):
    """phase name -> text (possibly several lines) of the entries a person has written; placeholders are dropped."""
    kept = {}
    if not path or not os.path.exists(path):
        return kept
    with open(path, "r", encoding="utf-8") as f:
        txt = f.read()
    m = re.search(r"^## Explanations\n(.*?)(?=^## |\Z)", txt, re.S | re.M)
    if not m:
        return kept
    cur = None
    for line in m.group(1).splitlines():
        mm = re.match(r"^- \*\*([A-Za-z0-9_]+)\*\*(?: \([^)]*\))?: ?(.*)$", line)
        if mm:
            cur = mm.group(1)
            kept[cur] = [mm.group(2)]
        elif line.startswith("Entries for phases that are not flagged"):
            cur = None
        elif cur is not None:
            kept[cur].append(line)
    out = {}
    for k, v in kept.items():
        text = "\n".join(v).rstrip()
        if text and not text.startswith("PLACEHOLDER"):
            out[k] = text
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
    if a is None or b is None:
        return template()
    ph_a, ph_b = by_phase(a), by_phase(b)
    run_a, run_b = a["data"].get("P-RUN-01", {}), b["data"].get("P-RUN-01", {})
    meta_a, meta_b = run_a.get("meta", {}), run_b.get("meta", {})
    why = sorted(set(smoke_reasons(a) + smoke_reasons(b)))
    invalid = []
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
                a["doc"]["git_sha"][:7], b["doc"]["git_sha"][:7], a["doc"].get("dirty_tree"), b["doc"].get("dirty_tree")), ""]
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
    if not flagged:
        out.append("No phase is more than 2x slower than the Linux reference in these results.")
    else:
        out.append("Every phase with a slowdown above 2 needs an entry here (criterion 5.6), keyed by the phase name. The "
                   "entries are written by a person (they may be several lines long); the generator keeps them and writes a "
                   "placeholder for a new flag.")
        out.append("")
        for key, cid, title, r in flagged:
            out.append("- **%s** (%s): %s" % (key, cid, prev.get(key, "%s (%s, slowdown %.1f)" % (PLACEHOLDER, title, r))))
    stale = [k for k in sorted(prev) if k not in flagged_keys]
    if stale:
        out += ["", "Entries for phases that are not flagged in these results (kept; check that they still apply):", ""]
        for k in stale:
            out.append("- **%s**: %s" % (k, prev[k]))
    out.append("")
    return "\n".join(out)


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
    if a.template:
        txt = template()
    else:
        if not (a.a and a.b):
            ap.error("--a and --b are required (or --template)")
        txt = render(load(a.a), load(a.b), keep_explanations(a.out))
    if a.out:
        with open(a.out, "w", encoding="utf-8") as f:
            f.write(txt)
    else:
        sys.stdout.write(txt)
    return 0


def selftest():
    import tempfile

    def mk(rate, sha, fullp=False, extra=None):
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
        return {"doc": {"git_sha": "0" * 40, "dirty_tree": False, "cases": cases}, "cases": {c["id"]: c for c in cases},
                "data": {c["id"]: json.loads(c["note"].split("; data=")[1]) for c in cases}}

    a = mk(lambda ph: 100.0, "aa")
    b = mk(lambda ph: 20.0 if ph == "seq_write" else 90.0, "aa")
    txt = render(a, b, {})
    assert "SMOKE PARAMETERS" in txt and "not hardware numbers" in txt and "### Baseline" in txt
    assert txt.count("> 2x: explain") == 1 and "PLACEHOLDER: explanation needed" in txt, txt
    assert "COMPARISON INVALID" not in txt and "baseline target not available, A loop" in txt
    assert "COMPARISON INVALID" in render(a, mk(lambda ph: 100.0, "bb"), {})
    assert "COMPARISON INVALID" in render(a, mk(lambda ph: 100.0, "aa", extra={"block_kib": 4}), {})
    with tempfile.TemporaryDirectory() as t:
        p = os.path.join(t, "P.md")
        with open(p, "w") as f:
            f.write(txt.replace(PLACEHOLDER + " (Sequential write, slowdown 5.0)", "FSKit round trips\n  per 1 MiB write\n"
                                "second line") + "\n- **rnd_read**: an old entry\n")
        kept = keep_explanations(p)
        assert kept.get("seq_write", "").endswith("second line") and "rnd_read" in kept, kept
        t2 = render(a, b, kept)
        assert "FSKit round trips\n  per 1 MiB write" in t2 and "- **rnd_read**: an old entry" in t2
        # generate again from the regenerated file: the same entries are kept
        with open(p, "w") as f:
            f.write(t2)
        assert keep_explanations(p) == kept
    assert "SMOKE" not in render(mk(lambda ph: 100.0, "aa", True), mk(lambda ph: 100.0, "aa", True), {})
    assert "NO RESULTS YET" in template()
    print("report.py selftest ok")
    return 0


if __name__ == "__main__":
    if len(sys.argv) > 1 and sys.argv[1] == "selftest":
        sys.exit(selftest())
    sys.exit(main(sys.argv[1:]))
