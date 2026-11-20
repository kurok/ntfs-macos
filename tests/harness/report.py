# SPDX-License-Identifier: GPL-2.0-or-later
"""Result file: build, name, write and validate (TEST_STRATEGY.md section 10); merge the stage
results of one scripts/test-all.sh run into one file (T-M5-01); generate the README status table.

  python3 tests/harness/report.py validate FILE...
  python3 tests/harness/report.py aggregate --env A|B|host --work DIR... [--out FILE] [--started TS]
  python3 tests/harness/report.py readme [--check] [--print]
  python3 tests/harness/report.py git-info

`python3 -m harness.report FILE...` (from tests/) still validates, as before.
"""

import fnmatch
import json
import os
import re
import subprocess
import sys
import time

SCHEMA = "ntfsmac-test-result/1"
TOP_KEYS = ("schema", "git_sha", "dirty_tree", "env", "host_os", "guest_os", "macfuse_version",
            "ntfs3g_version", "started", "finished", "cases")
CASE_KEYS = ("id", "status", "duration_s", "evidence", "note")
STATUSES = ("pass", "fail", "skip", "error")
ENV_VALUES = ("A", "B", "host")
TS_RE = re.compile(r"^\d{4}-\d{2}-\d{2}T\d{2}:\d{2}:\d{2}Z$")
SHA_RE = re.compile(r"^[0-9a-f]{40}$")
ID_RE = re.compile(r"^[A-Z][A-Z0-9]*(-[A-Z0-9]+)+$")


def utc_now():
    return time.strftime("%Y-%m-%dT%H:%M:%SZ", time.gmtime())


def _is_num(v):
    return isinstance(v, (int, float)) and not isinstance(v, bool)


def _opt_str(v):
    return v is None or (isinstance(v, str) and v.strip() != "")


def validate(doc):
    """Return a list of error strings; empty means the document is valid."""
    errs = []
    if not isinstance(doc, dict):
        return ["result is not an object"]
    for k in TOP_KEYS:
        if k not in doc:
            errs.append("missing key: %s" % k)
    for k in doc:
        if k not in TOP_KEYS:
            errs.append("unexpected key: %s" % k)
    if errs:
        return errs
    if doc["schema"] != SCHEMA:
        errs.append("schema must be %r" % SCHEMA)
    if not (isinstance(doc["git_sha"], str) and SHA_RE.match(doc["git_sha"])):
        errs.append("git_sha must be 40 lowercase hex digits")
    if not isinstance(doc["dirty_tree"], bool):
        errs.append("dirty_tree must be a boolean")
    if doc["env"] not in ENV_VALUES:
        errs.append("env must be one of %s" % ", ".join(ENV_VALUES))
    if not (isinstance(doc["host_os"], str) and doc["host_os"].strip()):
        errs.append("host_os must be a non-empty string")
    for k in ("guest_os", "macfuse_version", "ntfs3g_version"):
        if not _opt_str(doc[k]):
            errs.append("%s must be null or a non-empty string" % k)
    for k in ("started", "finished"):
        if not (isinstance(doc[k], str) and TS_RE.match(doc[k])):
            errs.append("%s must be YYYY-MM-DDTHH:MM:SSZ" % k)
    if not errs and doc["finished"] < doc["started"]:
        errs.append("finished is before started")
    cases = doc["cases"]
    if not isinstance(cases, list):
        errs.append("cases must be a list")
        return errs
    seen = set()
    for i, c in enumerate(cases):
        where = "cases[%d]" % i
        if not isinstance(c, dict):
            errs.append("%s is not an object" % where)
            continue
        for k in CASE_KEYS:
            if k not in c:
                errs.append("%s: missing key %s" % (where, k))
        for k in c:
            if k not in CASE_KEYS:
                errs.append("%s: unexpected key %s" % (where, k))
        if any(k not in c for k in CASE_KEYS):
            continue
        cid = c["id"]
        if not (isinstance(cid, str) and ID_RE.match(cid)):
            errs.append("%s: bad id %r" % (where, cid))
        elif cid in seen:
            errs.append("%s: duplicate id %s" % (where, cid))
        else:
            seen.add(cid)
        if c["status"] not in STATUSES:
            errs.append("%s: status must be one of %s" % (where, "/".join(STATUSES)))
        if not _is_num(c["duration_s"]) or c["duration_s"] < 0:
            errs.append("%s: duration_s must be a number >= 0" % where)
        ev = c["evidence"]
        if not isinstance(ev, list):
            errs.append("%s: evidence must be a list" % where)
        else:
            for p in ev:
                if not (isinstance(p, str) and p and not p.startswith("/") and ".." not in p.split("/")):
                    errs.append("%s: evidence path must be relative without '..': %r" % (where, p))
        note = c["note"]
        if not (note is None or isinstance(note, str)):
            errs.append("%s: note must be null or a string" % where)
        if c["status"] == "skip" and not (isinstance(note, str) and note.strip()):
            errs.append("%s: a skip needs a non-empty note" % where)
    return errs


def _git(repo, *args):
    r = subprocess.run(("git", "-c", "safe.directory=*", "-C", repo) + args, stdout=subprocess.PIPE,
                       stderr=subprocess.DEVNULL, check=False)
    if r.returncode != 0:
        return None
    return r.stdout.decode("utf-8", "replace")


def git_info(repo, sha=None, dirty=None):
    """(full sha, dirty bool). Explicit values win (used inside the container).
    Changes under tests/results/ do not make the tree dirty: results are outputs, not inputs."""
    if sha is None:
        out = _git(repo, "rev-parse", "HEAD")
        if out is None:
            raise RuntimeError("cannot read git HEAD; pass --git-sha and --dirty")
        sha = out.strip()
    if dirty is None:
        out = _git(repo, "status", "--porcelain", "--", ".", ":(exclude)tests/results")
        if out is None:
            raise RuntimeError("cannot read git status; pass --git-sha and --dirty")
        dirty = out.strip() != ""
    return sha, bool(dirty)


def short_sha(sha):
    return sha[:7]


def run_id(started, sha, env, suffix=""):
    """<date>-<short sha>-<env>[<suffix>]; suffix is "" or "-runN" for repeated runs."""
    return "%s-%s-%s%s" % (started[:10], short_sha(sha), env, suffix)


def free_suffix(out_dir, started, sha, env):
    """"" if no result or evidence of that run id exists in out_dir, else the first free "-runN" (N >= 2).
    Repeated runs of one commit on one day must not overwrite each other (M2 audit F-07)."""
    def taken(sfx):
        rid = run_id(started, sha, env, sfx)
        return (os.path.exists(os.path.join(out_dir, rid + ".json"))
                or os.path.exists(os.path.join(out_dir, "evidence", rid)))
    if not taken(""):
        return ""
    n = 2
    while taken("-run%d" % n):
        n += 1
    return "-run%d" % n


def build_result(sha, dirty, env, host_os, guest_os, macfuse_version, ntfs3g_version, started, finished, cases):
    return {
        "schema": SCHEMA,
        "git_sha": sha,
        "dirty_tree": bool(dirty),
        "env": env,
        "host_os": host_os,
        "guest_os": guest_os,
        "macfuse_version": macfuse_version,
        "ntfs3g_version": ntfs3g_version,
        "started": started,
        "finished": finished,
        "cases": cases,
    }


def write_result(doc, out_dir, rid=None):
    """Validate and write <rid>.json (default <date>-<short sha>-<env>); refuse an invalid document."""
    errs = validate(doc)
    if errs:
        raise ValueError("invalid result: " + "; ".join(errs))
    os.makedirs(out_dir, exist_ok=True)
    path = os.path.join(out_dir, (rid or run_id(doc["started"], doc["git_sha"], doc["env"])) + ".json")
    with open(path, "w", encoding="utf-8") as f:
        json.dump(doc, f, indent=2)
        f.write("\n")
    return path




# ---------------------------------------------------------------------------------------------
# Aggregation of one scripts/test-all.sh run (T-M5-01)
#
# A "work directory" is what test-all.sh and vm/macos/run-suite.sh leave behind:
#   stages.tsv          one line per stage: name, status (pass|fail|skip), seconds, note, log
#                       (tab-separated; the log is a path relative to the work directory, may be empty)
#   logs/               stage output
#   results/            result files of harness runs (run.py --out results/) with results/evidence/
#   artifacts/<stage>/  small extra evidence files of a stage (copied to the result's evidence)
# Every stage becomes one case "STAGE-<ENV>-<NAME>" in the one result file, so a failed or skipped
# stage is visible to everything that reads `cases` (the gate rule, the README table); the cases of
# the harness result files are merged in as they are. The schema does not change.
# ---------------------------------------------------------------------------------------------

STAGE_STATUSES = ("pass", "fail", "skip")
MAX_ARTIFACTS_PER_STAGE = 40


def _hostnames():
    names = set()
    try:
        import socket

        h = socket.gethostname()
        for n in (h, h.split(".")[0]):
            if len(n) >= 4 and n.lower() not in ("localhost", "localdomain"):
                names.add(n)
    except OSError:
        pass
    return sorted(names, key=len, reverse=True)


def scrub_text(text, repo=None, home=None):
    """Replace host paths and the machine name so that nothing under tests/results names the user or the
    machine: <repo>, <home>, <tmp>, <host>."""
    if repo:
        text = text.replace(repo.rstrip("/"), "<repo>")
    home = home if home is not None else os.path.expanduser("~")
    if home and home != "/":
        text = text.replace(home.rstrip("/"), "<home>")
    text = re.sub(r"/Users/[^/\s'\"]+", "<home>", text)
    text = re.sub(r"/(?:private/)?var/folders/[^/\s'\"]+/[^/\s'\"]+/[TC0-9a-z]", "<tmp>", text)
    text = re.sub(r"/private/tmp(?=[/\s'\"]|$)", "<tmp>", text)
    text = re.sub(r"(?<![\w<.-])/tmp(?=[/\s'\"]|$)", "<tmp>", text)
    for n in _hostnames():
        text = re.sub(r"(?<![\w-])%s(?![\w-])" % re.escape(n), "<host>", text, flags=re.I)
    return text


def scrub_bytes(data, repo=None):
    """scrub_text on bytes: invalid UTF-8 survives byte for byte (surrogateescape)."""
    return scrub_text(data.decode("utf-8", "surrogateescape"), repo).encode("utf-8", "surrogateescape")


ARCHIVE_EXT = (".gz", ".tgz", ".zip", ".tar", ".xz", ".bz2", ".zst", ".7z", ".img", ".dmg")
ARCHIVE_MAGIC = (b"\x1f\x8b", b"PK\x03\x04", b"\xfd7zXZ\x00", b"BZh", b"\x28\xb5\x2f\xfd", b"7z\xbc\xaf\x27\x1c")


def is_archive(path, head):
    return path.lower().endswith(ARCHIVE_EXT) or head.startswith(ARCHIVE_MAGIC)


def _copy_scrubbed(src, dst, repo):
    """Copy a file with host paths scrubbed on its bytes. Returns False, copying nothing, for an archive or
    disk image (compressed content cannot be scrubbed and images are not stored, section 10)."""
    with open(src, "rb") as f:
        data = f.read()
    if is_archive(src, data[:8]):
        return False
    os.makedirs(os.path.dirname(dst), exist_ok=True)
    with open(dst, "wb") as f:
        f.write(scrub_bytes(data, repo))
    return True


def stage_case_id(env, name):
    return re.sub(r"[^A-Z0-9]+", "-", ("STAGE-%s-%s" % (env, name)).upper()).strip("-")


def read_stages(work):
    path = os.path.join(work, "stages.tsv")
    out = []
    if not os.path.exists(path):
        return out
    with open(path, "r", encoding="utf-8") as f:
        for n, line in enumerate(f, 1):
            line = line.rstrip("\n")
            if not line:
                continue
            parts = line.split("\t")
            parts += [""] * (5 - len(parts))
            name, status, secs, note, log = parts[:5]
            if not name or status not in STAGE_STATUSES:
                raise ValueError("%s:%d: bad stage line" % (path, n))
            try:
                secs = float(secs)
            except ValueError:
                raise ValueError("%s:%d: bad seconds" % (path, n))
            out.append({"name": name, "status": status, "seconds": secs, "note": note, "log": log})
    return out


def _detect_host_os():
    try:
        r = subprocess.run("sw_vers -productName && sw_vers -productVersion && sw_vers -buildVersion && uname -m",
                           shell=True, stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
        if r.returncode == 0:
            n, v, b, a = r.stdout.decode().split()[:4]
            return "%s %s (%s) %s" % (n, v, b, a)
    except (OSError, ValueError):
        pass
    import platform

    return "%s %s" % (platform.system(), platform.release())


def aggregate(works, env, repo, out_path, sha, dirty, started, finished=None):
    """Merge the work directories (in order) into the one result file out_path; evidence goes to
    <dir of out_path>/evidence/<stem of out_path>/. Evidence is staged in a temporary directory and moved
    into place only after the JSON is written, so there is never an evidence directory without a result.
    The run is marked dirty when any merged result is dirty or was made at another commit.
    Returns the document (already written)."""
    import shutil
    import tempfile

    if env not in ENV_VALUES:
        raise ValueError("bad env %r" % env)
    out_path = os.path.abspath(out_path)
    out_dir = os.path.dirname(out_path)
    base = os.path.basename(out_path)
    rid = base[:-5] if base.endswith(".json") else base
    if os.path.exists(out_path):
        raise ValueError("%s exists already" % base)
    if os.path.exists(os.path.join(out_dir, "evidence", rid)):
        raise ValueError("evidence directory exists already: evidence/%s" % rid)
    os.makedirs(out_dir, exist_ok=True)
    stage_dir = tempfile.mkdtemp(prefix=".evidence-", dir=out_dir)
    try:
        return _aggregate(works, env, repo, out_path, out_dir, rid, stage_dir, sha, dirty, started, finished)
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)


def _aggregate(works, env, repo, out_path, out_dir, rid, stage_dir, sha, dirty, started, finished):
    stage_cases, merged = [], []
    seen = set()
    warnings = []
    dirty = bool(dirty)
    meta = {"host_os": None, "guest_os": None, "macfuse_version": None, "ntfs3g_version": None}

    def add_unique(c):
        if c["id"] in seen:
            raise ValueError("duplicate case id across stages: %s" % c["id"])
        seen.add(c["id"])

    def store(src, rest):
        """Copy src to evidence/<rid>/<rest>; the relative path, or None when it is not stored."""
        if not _copy_scrubbed(src, os.path.join(stage_dir, rest), repo):
            return None
        return "evidence/%s/%s" % (rid, rest)

    for work in works:
        for st in read_stages(work):
            cid = stage_case_id(env, st["name"])
            evidence, notes = [], []
            if st["log"]:
                src = os.path.join(work, st["log"])
                if os.path.isfile(src):
                    rel = store(src, "stages/%s.log" % st["name"])
                    if rel:
                        evidence.append(rel)
            art = os.path.join(work, "artifacts", st["name"])
            if os.path.isdir(art):
                files, not_copied = [], []
                for dp, dn, fns in os.walk(art):
                    dn.sort()
                    files.extend(os.path.join(dp, fn) for fn in sorted(fns))
                for src in files[:MAX_ARTIFACTS_PER_STAGE]:
                    sub = os.path.relpath(src, art).replace(os.sep, "/")
                    rel = store(src, "%s/%s" % (st["name"], sub))
                    if rel:
                        evidence.append(rel)
                    else:
                        not_copied.append(sub)
                if len(files) > MAX_ARTIFACTS_PER_STAGE:
                    notes.append("evidence capped: %d of %d files kept" % (MAX_ARTIFACTS_PER_STAGE, len(files)))
                if not_copied:
                    notes.append("not copied (archive or image): " + ", ".join(not_copied))
            note = scrub_text("; ".join(x for x in [st["note"].strip()] + notes if x), repo).strip()
            if st["status"] == "skip" and not note:
                note = "skipped"
            case = {"id": cid, "status": st["status"], "duration_s": round(st["seconds"], 3),
                    "evidence": evidence, "note": note or None}
            add_unique(case)
            stage_cases.append(case)
        rdir = os.path.join(work, "results")
        for fn in sorted(os.listdir(rdir)) if os.path.isdir(rdir) else []:
            if not fn.endswith(".json"):
                continue
            with open(os.path.join(rdir, fn), "r", encoding="utf-8") as f:
                sub = json.load(f)
            errs = validate(sub)
            if errs:
                raise ValueError("%s: invalid result: %s" % (fn, "; ".join(errs)))
            if sub["dirty_tree"]:
                dirty = True
                warnings.append("%s: tested a dirty tree" % fn)
            if sub["git_sha"] != sha:
                dirty = True
                warnings.append("%s: tested %s, this run is %s" % (fn, short_sha(sub["git_sha"]), short_sha(sha)))
            for k in meta:
                if meta[k] is None and sub.get(k):
                    meta[k] = sub[k]
            for c in sub["cases"]:
                c = dict(c)
                new_ev, missing = [], []
                for p in c["evidence"]:
                    src = os.path.join(rdir, p)
                    rest = p[len("evidence/"):] if p.startswith("evidence/") else p
                    rel = store(src, rest) if os.path.isfile(src) else None
                    if rel:
                        new_ev.append(rel)
                    else:
                        missing.append(p)
                c["evidence"] = new_ev
                note = c["note"] or ""
                if missing:
                    note = (note + "; " if note else "") + "evidence not stored: " + ", ".join(missing)
                c["note"] = scrub_text(note, repo) or None
                add_unique(c)
                merged.append(c)
    if warnings:
        case = {"id": stage_case_id(env, "provenance"), "status": "pass", "duration_s": 0.0, "evidence": [],
                "note": "result marked dirty: " + "; ".join(warnings)}
        add_unique(case)
        stage_cases.append(case)
    host_os = meta["host_os"] or _detect_host_os()
    doc = build_result(sha, dirty, env, scrub_text(host_os, repo), meta["guest_os"], meta["macfuse_version"],
                       meta["ntfs3g_version"], started, finished or utc_now(), stage_cases + merged)
    write_result(doc, out_dir, rid)
    try:
        if os.listdir(stage_dir):
            os.makedirs(os.path.join(out_dir, "evidence"), exist_ok=True)
            os.rename(stage_dir, os.path.join(out_dir, "evidence", rid))
    except OSError:
        os.unlink(out_path)
        raise
    return doc


def summarize(doc):
    """(text, ok): per-stage table and totals. ok is False when any case failed or errored, and also when
    nothing was verified (no case passed), so an all-skipped run can never look green."""
    lines = []
    counts = {s: 0 for s in STATUSES}
    for c in doc["cases"]:
        counts[c["status"]] += 1
        if c["id"].startswith("STAGE-"):
            lines.append("%-40s %-5s %7.1fs  %s" % (c["id"], c["status"].upper(), c["duration_s"], c["note"] or ""))
    lines.append("cases: %d pass, %d fail, %d error, %d skip" % (counts["pass"], counts["fail"], counts["error"],
                                                                 counts["skip"]))
    ok = counts["fail"] == 0 and counts["error"] == 0
    if counts["pass"] == 0:
        lines.append("nothing verified: no case and no stage passed")
        ok = False
    return "\n".join(lines), ok


# ---------------------------------------------------------------------------------------------
# README status table (TEST_STRATEGY.md section 10)
# ---------------------------------------------------------------------------------------------

BEGIN_RE = re.compile(r"^<!-- status-table:begin.*-->$", re.M)
END_MARK = "<!-- status-table:end -->"
BEGIN_MARK = "<!-- status-table:begin (generated by tests/harness/report.py readme; do not edit by hand) -->"
HW_RE = re.compile(r"^Hardware-verified:\s*(.+)$", re.M)
VM_ENVS = ("B", "host")


def load_results(results_dir):
    """[(filename, doc)] of the valid, clean-tree result files, oldest first."""
    docs = []
    if os.path.isdir(results_dir):
        for fn in sorted(os.listdir(results_dir)):
            if not fn.endswith(".json"):
                continue
            try:
                with open(os.path.join(results_dir, fn), "r", encoding="utf-8") as f:
                    doc = json.load(f)
            except (OSError, ValueError):
                sys.stderr.write("report: skipping unreadable %s\n" % fn)
                continue
            if validate(doc):
                sys.stderr.write("report: skipping invalid %s\n" % fn)
                continue
            if doc["dirty_tree"]:
                continue
            docs.append((fn, doc))
    docs.sort(key=lambda t: (t[1]["started"], t[0]))
    return docs


def case_status(docs, envs):
    """id -> status over results of the given envs: the newest status that is not a skip (a skip-only
    result never hides an earlier outcome); a case that only ever skipped is 'skip'."""
    out = {}
    for _fn, doc in docs:
        if doc["env"] not in envs:
            continue
        for c in doc["cases"]:
            if c["status"] != "skip" or c["id"] not in out:
                out[c["id"]] = c["status"]
    return out


def load_features(path):
    with open(path, "r", encoding="utf-8") as f:
        feats = json.load(f)
    ids = set()
    for ft in feats:
        if not (isinstance(ft.get("id"), str) and isinstance(ft.get("name"), str)
                and isinstance(ft.get("cases"), list) and ft["cases"]):
            raise ValueError("features.json: bad entry %r" % (ft,))
        if ft["id"] in ids:
            raise ValueError("features.json: duplicate id %s" % ft["id"])
        ids.add(ft["id"])
    return feats


def hardware_verified(results_dir):
    out = set()
    hw = os.path.join(results_dir, "hardware")
    if os.path.isdir(hw):
        for fn in sorted(os.listdir(hw)):
            if fn.endswith(".md"):
                with open(os.path.join(hw, fn), "r", encoding="utf-8") as f:
                    for m in HW_RE.finditer(f.read()):
                        out.update(x.strip() for x in m.group(1).split(",") if x.strip())
    return out


def _match(statuses, patterns):
    return {i: s for i, s in statuses.items() if any(fnmatch.fnmatchcase(i, p) for p in patterns)}


def feature_state(ft, vm, linux, hw):
    """(state, 'pass/total'). Order: Failing (VM), Failing (Linux), Verified (hardware), Verified (VM) (every
    matched case passed), Linux only (every matched Linux case passed), Partial (VM) (some passed, the rest
    skipped), Untested. A failure is never hidden by a pass elsewhere."""
    mv, ml = _match(vm, ft["cases"]), _match(linux, ft["cases"])

    def frac(m):
        return "%d/%d" % (sum(s == "pass" for s in m.values()), len(m))

    def bad(m):
        return any(s in ("fail", "error") for s in m.values())

    def full(m):
        return bool(m) and all(s == "pass" for s in m.values())

    if bad(mv):
        return "Failing (VM)", frac(mv)
    if bad(ml):
        return "Failing (Linux)", frac(ml)
    if ft["id"] in hw:
        return "Verified (hardware)", "hardware run"
    if full(mv):
        return "Verified (VM)", frac(mv)
    if full(ml):
        return "Linux only", frac(ml)
    if any(s == "pass" for s in mv.values()):
        return "Partial (VM)", frac(mv)
    return "Untested", "-"


def render_table(results_dir, features_path):
    docs = load_results(results_dir)
    vm, linux = case_status(docs, VM_ENVS), case_status(docs, ("A",))
    hw = hardware_verified(results_dir)
    rows = ["| Feature | State | Cases passing |", "|---|---|---|"]
    for ft in load_features(features_path):
        state, n = feature_state(ft, vm, linux, hw)
        rows.append("| %s | %s | %s |" % (ft["name"], state, n))
    src = ("Built by `python3 tests/harness/report.py readme` from the newest clean-tree result of each case in "
           "`tests/results/`.")
    return "\n".join(rows) + "\n\n" + src


def render_readme(text, block):
    """README text with the generated block between the markers replaced. ValueError if markers are absent."""
    m = BEGIN_RE.search(text)
    e = text.find(END_MARK)
    if not m or e < m.end():
        raise ValueError("README has no status-table:begin ... %s markers" % END_MARK)
    return text[:m.start()] + BEGIN_MARK + "\n" + block + "\n" + text[e:]


def cmd_readme(args):
    repo = args.repo
    results_dir = args.results_dir or os.path.join(repo, "tests", "results")
    features = args.features or os.path.join(repo, "tests", "harness", "features.json")
    readme = args.readme or os.path.join(repo, "README.md")
    block = render_table(results_dir, features)
    if args.print:
        print(block)
        return 0
    with open(readme, "r", encoding="utf-8") as f:
        old = f.read()
    new = render_readme(old, block)
    if args.check:
        if new != old:
            sys.stderr.write("README status table is stale: run python3 tests/harness/report.py readme\n")
            return 1
        print("README status table is current")
        return 0
    if new != old:
        with open(readme, "w", encoding="utf-8") as f:
            f.write(new)
        print("updated %s" % readme)
    else:
        print("%s already current" % readme)
    return 0


def cmd_validate(args):
    rc = 0
    for p in args.files:
        with open(p, "r", encoding="utf-8") as f:
            errs = validate(json.load(f))
        if errs:
            rc = 1
            print("%s: INVALID" % p)
            for e in errs:
                print("  " + e)
        else:
            print("%s: ok" % p)
    return rc


def cmd_aggregate(args):
    repo = args.repo
    sha, dirty = git_info(repo, args.git_sha, None if args.dirty is None else args.dirty == "true")
    started = args.started or utc_now()
    if args.out:
        out = os.path.abspath(args.out)
    else:
        d = os.path.join(repo, "tests", "results")
        out = os.path.join(d, run_id(started, sha, args.env, free_suffix(d, started, sha, args.env)) + ".json")
    doc = aggregate(args.work, args.env, repo, out, sha, dirty, started)
    text, ok = summarize(doc)
    sys.stderr.write(text + "\n")
    sys.stderr.write("result: %s%s\n" % (out, " (dirty tree: cannot satisfy a gate)" if doc["dirty_tree"] else ""))
    print(out)
    return 0 if ok else 1


def main(argv=None):
    import argparse

    argv = sys.argv[1:] if argv is None else list(argv)
    subs = ("validate", "aggregate", "readme", "git-info")
    if argv and argv[0] not in subs and not argv[0].startswith("-"):
        argv = ["validate"] + argv  # python3 -m harness.report FILE... (the original interface)
    here = os.path.dirname(os.path.abspath(__file__))
    default_repo = os.path.dirname(os.path.dirname(here))
    p = argparse.ArgumentParser(prog="report.py", description=__doc__.split("\n\n")[0])
    p.add_argument("--repo", default=default_repo, help=argparse.SUPPRESS)
    sp = p.add_subparsers(dest="cmd", required=True)
    v = sp.add_parser("validate", help="validate result files")
    v.add_argument("files", nargs="+")
    sp.add_parser("git-info", help="print '<sha> <true|false>' for the tree under test")
    a = sp.add_parser("aggregate", help="merge work directories into one result file")
    a.add_argument("--env", required=True, choices=ENV_VALUES)
    a.add_argument("--work", action="append", required=True, help="work directory (repeatable, in stage order)")
    a.add_argument("--out", default=None, help="result file (default tests/results/<date>-<sha>-<env>[-runN].json)")
    a.add_argument("--started", default=None, help="UTC start of the run, YYYY-MM-DDTHH:MM:SSZ")
    a.add_argument("--git-sha", default=None)
    a.add_argument("--dirty", choices=("true", "false"), default=None)
    r = sp.add_parser("readme", help="regenerate the README status table")
    r.add_argument("--check", action="store_true", help="exit 1 if the README table is stale; change nothing")
    r.add_argument("--print", action="store_true", help="print the table, change nothing")
    r.add_argument("--readme", default=None)
    r.add_argument("--results-dir", default=None)
    r.add_argument("--features", default=None)
    args = p.parse_args(argv)
    args.repo = os.path.abspath(args.repo)
    try:
        if args.cmd == "validate":
            return cmd_validate(args)
        if args.cmd == "git-info":
            sha, dirty = git_info(args.repo)
            print("%s %s" % (sha, "true" if dirty else "false"))
            return 0
        if args.cmd == "aggregate":
            return cmd_aggregate(args)
        return cmd_readme(args)
    except (ValueError, RuntimeError, OSError) as e:
        sys.stderr.write("report.py: %s\n" % e)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
