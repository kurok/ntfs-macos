# SPDX-License-Identifier: GPL-2.0-or-later
"""Result file: build, name, write and validate (TEST_STRATEGY.md section 10); merge the stage
results of one scripts/test-all.sh run into one file (T-M5-01); generate the README status table.

  python3 tests/harness/report.py validate FILE...
  python3 tests/harness/report.py aggregate --env A|B|host --work DIR... [--out FILE] [--started TS]
  python3 tests/harness/report.py adopt --env A|B --dir DIR [--out tests/results/] [--suite NAME]
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

# The one set of paths that may change between the commit a result was recorded at (T) and the commit that relies on
# it (G). Used by `ci-local.sh --verify-result` (the gate) and by the fuzz campaign re-triage
# (campaign_result.retriage_bad); define it only here.
#   - tests/results/, docs/audits/, CHECKPOINT.md, NEXT_ACTIONS.md, docs/SECURITY_FINDINGS.md and
#     docs/UPSTREAM_REPORTS.md: records and documents, not code under test; any change is allowed.
#   - tests/fuzz/findings/ (minimised inputs): replayed by ctest (fuzz.findings.*) and by the campaign, so an input
#     that exists at T must be byte-identical at G (no edit, no deletion). Only a new file may be added.
#   - tests/fuzz/campaign/known.tsv: read at run time by the fuzz-short stage (first matching row wins) and by the
#     campaign triage. Only APPENDING rows is allowed: T's content must be a byte prefix of G's, so no row that
#     existed at T is edited, reordered, shadowed by a row inserted above, or removed. The unit tests of the
#     classifiers use a frozen copy (tests/fuzz/campaign/testdata/known.tsv), not the live file.
#     Caveat: an appended row can still classify a signature that an earlier pass counted as unclassified; the
#     next triage run, not the gate, judges that.
# README.md is handled separately by the gate (only outside its status table, which must match report.py).
ALLOWED_CHANGE_FILES = ("CHECKPOINT.md", "NEXT_ACTIONS.md", "docs/SECURITY_FINDINGS.md",
                        "docs/UPSTREAM_REPORTS.md")  # exact paths, any change
ALLOWED_CHANGE_DIRS = ("docs/audits/", "tests/results/")  # prefixes, any change
ADD_ONLY_DIRS = ("tests/fuzz/findings/",)  # prefixes: a file may be added, an existing one must stay identical
APPEND_ONLY_FILES = ("tests/fuzz/campaign/known.tsv",)  # exact paths: G's content starts with T's content


def change_allowed(path, old, new):
    """True when the change of PATH (repo-relative, forward slashes) from OLD to NEW (bytes at T and at G, None when
    the file does not exist there) does not invalidate a recorded result."""
    if path in ALLOWED_CHANGE_FILES or path.startswith(ALLOWED_CHANGE_DIRS):
        return True
    if path.startswith(ADD_ONLY_DIRS):
        return old is None or old == new
    if path in APPEND_ONLY_FILES:
        # old must end at a row boundary, so G cannot extend T's last row in place
        return old is None or (new is not None and new.startswith(old) and (old == b"" or old.endswith(b"\n")))
    return False


def disallowed_changes(paths, blob):
    """The PATHS whose change is not allowed. blob(rev_name, path) returns the bytes of the path at "old" (T) or
    "new" (G), or None when it does not exist there."""
    return [p for p in paths if not change_allowed(p, blob("old", p), blob("new", p))]


def git_blob(repo, rev, path):
    """Bytes of PATH at REV, or None when it does not exist there."""
    r = subprocess.run(("git", "-c", "safe.directory=*", "-C", repo, "cat-file", "blob", "%s:%s" % (rev, path)),
                       stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    return r.stdout if r.returncode == 0 else None


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


def write_provenance(path, repo):
    """Record the commit and the tree state at the START of a run: DIR/provenance.txt (git_sha=, dirty=). The suite's
    run.sh calls it (`report.py provenance --out FILE`) first, then checks that the build tree belongs to this
    checkout (`report.py build-check`) and rebuilds it with `cmake --build`, so that the prebuilt host binaries the
    suite tests (ntfsmacd, ntfsmac) come from the recorded commit. The guest copy and the guest build follow."""
    sha, dirty = git_info(repo)
    with open(path, "w", encoding="utf-8") as f:
        f.write("git_sha=%s\ndirty=%s\n" % (sha, "true" if dirty else "false"))
    return sha, dirty


def run_state(run_dir, repo):
    """(git_sha, dirty, notes) of a run that wrote DIR/provenance.txt at its start (M5-F04 review). The sha is the one
    at the START. The result is dirty when the start record is missing or unusable, the tree was dirty at the start,
    HEAD at the end differs from the start, or the tree is dirty at the end. A missing record gives the end sha, dirty."""
    end_sha, end_dirty = git_info(repo)
    kv = {}
    try:
        with open(os.path.join(run_dir, "provenance.txt"), "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
    except OSError:
        pass
    start = kv.get("git_sha", "")
    if not SHA_RE.match(start):
        return end_sha, True, ["no usable start record (provenance.txt): the commit that was tested is not known"]
    notes, dirty = [], False
    if kv.get("dirty") != "false":
        dirty = True
        notes.append("the tree was dirty (or its state unknown) when the run started")
    if start != end_sha:
        dirty = True
        notes.append("HEAD moved during the run: started at %s, ended at %s" % (start[:12], end_sha[:12]))
    if end_dirty:
        dirty = True
        notes.append("the tree was dirty at the end of the run")
    return start, dirty, notes


def build_check(build, repo):
    """None when BUILD is a CMake build tree of REPO (CMAKE_HOME_DIRECTORY of its cache is the repository root),
    else the reason. A stale or foreign tree would make the suite test binaries of another source tree."""
    cache = os.path.join(build, "CMakeCache.txt")
    try:
        with open(cache, "r", encoding="utf-8", errors="replace") as f:
            for line in f:
                if line.startswith("CMAKE_HOME_DIRECTORY:"):
                    home = line.split("=", 1)[1].strip()
                    if os.path.realpath(home) == os.path.realpath(repo):
                        return None
                    return "%s was configured for %s, not for this checkout %s" % (build, home, repo)
    except OSError:
        return "%s has no CMakeCache.txt: it is not a CMake build tree (cmake --preset release)" % build
    return "%s/CMakeCache.txt has no CMAKE_HOME_DIRECTORY" % build


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


# Stages that scripts/ci-local.sh runs itself, per environment (recorded in <date>-<sha>-<env>-ci.json). The one
# list for full mode and for `ci-local.sh --verify-result --require-env`. `bootstrap` is in it: full mode always
# runs it first, so a gate result without STAGE-HOST-BOOTSTRAP (M5-F12) is one from a run that skipped it.
OWN_STAGES = (("host", "bootstrap"), ("host", "tsan"), ("host", "static-analysis"), ("A", "linux-build-test"), ("A", "fuzz-short"),
              ("B", "package"))


# `report.py aggregate` exit status: 0 everything passed on a clean tree, 1 a case or stage failed or nothing passed,
# 2 usage error or the result could not be written, EXIT_DIRTY everything passed but the result is marked dirty
# (dirty tree, HEAD moved, or a sub-result from another commit): not usable for a gate (M5-F12).
EXIT_DIRTY = 3


def stage_case_id(env, name):
    return re.sub(r"[^A-Z0-9]+", "-", ("STAGE-%s-%s" % (env, name)).upper()).strip("-")


def is_mode_case(cid):
    """The run-mode record of test-all.sh: STAGE-<ENV>-MODE (a bookkeeping case, not a verification)."""
    return re.match(r"^STAGE-(HOST|A|B)-MODE$", cid) is not None


# Criterion runs (stress suites at the criterion parameters, the fuzz campaign): a standalone DIR/result.json is
# adopted into tests/results/ by `report.py adopt` (M5-F05). The adopted file carries the one marker that only `adopt`
# writes: the case STAGE-<ENV>-MODE with status pass and the note `criterion:<suite>`. `ci-local.sh --verify-result`
# accepts such a file when its cases are exactly the plain criterion ids of the suite (and its run case): a run that
# was shortened or weaker has the ids with the suffix -SHORT and is refused (M5-F04).
CRITERION_NOTE = "criterion:"
CRITERION_SUITES = {
    "soak": {"env": "B", "cases": ("S-SOAK-01", "S-SOAK-01-CONTROL"), "also": ("S-SOAK-RUN",)},
    "parallel": {"env": "B", "cases": ("S-PAR-01", "S-PAR-01-CONTROL"), "also": ("S-PAR-RUN",)},
    "leaks": {"env": "B", "cases": ("S-LEAK-01", "S-LEAK-01-CONTROL"), "also": ()},
    "kill": {"env": "B", "cases": ("S-KILL-01", "S-KILL-02", "S-KILL-03", "S-KILL-04", "S-KILL-05", "S-KILL-RUN"), "also": ()},
    "disconnect": {"env": "B", "cases": ("S-DISC-01", "S-DISC-02", "S-DISC-03", "S-DISC-04", "S-DISC-05"),
                   "also": ("S-DISC-RUN",)},
    "fuzz": {"env": "A", "cases": ("F-CAMPAIGN-01",), "also": ()},
}


def criterion_suite_of(doc):
    """The suite whose first case is in doc, or None (also None when the cases belong to more than one suite)."""
    ids = {c["id"] for c in doc["cases"]}
    found = [n for n, v in CRITERION_SUITES.items() if v["cases"][0] in ids]
    return found[0] if len(found) == 1 else None


def criterion_problems(doc, suite):
    """Why doc is not the result of a criterion run of SUITE (empty list: it is). The mode record is not looked at.
    Every non-bookkeeping case must be one of the suite's plain ids and pass, and every id of the suite must be there."""
    spec = CRITERION_SUITES.get(suite)
    if spec is None:
        return ["unknown criterion suite %r" % (suite,)]
    pr = []
    if doc["env"] != spec["env"]:
        pr.append("suite %s runs in environment %s, the result is for %s" % (suite, spec["env"], doc["env"]))
    cases = [c for c in doc["cases"] if not is_mode_case(c["id"])]
    allowed = set(spec["cases"]) | set(spec["also"])
    foreign = [c["id"] for c in cases if c["id"] not in allowed]
    if foreign:
        pr.append("case(s) that are not plain criterion cases of %s (a shortened or weaker run has the suffix -SHORT): %s"
                  % (suite, ", ".join(foreign[:8])))
    have = {c["id"] for c in cases}
    missing = [i for i in spec["cases"] if i not in have]
    if missing:
        pr.append("criterion case(s) missing: %s" % ", ".join(missing))
    notpass = ["%s=%s" % (c["id"], c["status"]) for c in cases if c["status"] != "pass"]
    if notpass:
        pr.append("case(s) not passed: %s" % ", ".join(notpass[:8]))
    return pr


def _retriage_matches(src_dir, result_sha, fuzzed_sha):
    """A fuzz result that a re-triage wrote at another commit: DIR/retriage.txt names that commit (git_sha=) and the
    fuzzed one (fuzzed=); both must equal the result's sha and the start record's sha."""
    kv = {}
    try:
        with open(os.path.join(src_dir, "retriage.txt"), "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    kv[k.strip()] = v.strip()
    except OSError:
        return False
    return bool(fuzzed_sha) and kv.get("git_sha") == result_sha and kv.get("fuzzed") == fuzzed_sha


def _inside(base, path):
    """True when realpath(path) is below realpath(base) (no symlink in the chain leads out of it)."""
    b = os.path.realpath(base)
    r = os.path.realpath(path)
    return r == b or r.startswith(b + os.sep)


def adopt(src_dir, env, out_dir, repo, suite=None):
    """Turn the standalone result DIR/result.json of a criterion run into a result file under out_dir that
    `ci-local.sh --verify-result` can accept (M5-F05): <date>-<sha7>-<env>-<suite>.json plus the evidence copied to
    <out_dir>/evidence/<same name>/ (paths rewritten, host paths scrubbed). Refuses (ValueError) an invalid, dirty
    or sha-less source, a source that is not a plain criterion run of one suite, and an existing target.
    Returns the path of the result file."""
    import shutil
    import tempfile

    src = os.path.join(src_dir, "result.json")
    prov = os.path.join(src_dir, "provenance.txt")
    if os.path.isfile(prov):
        pkv = {}
        with open(prov, "r", encoding="utf-8") as f:
            for line in f.read().splitlines():
                if "=" in line:
                    k, v = line.split("=", 1)
                    pkv[k.strip()] = v.strip()
        try:
            with open(src, "r", encoding="utf-8") as f:
                rsha = json.load(f).get("git_sha")
        except (OSError, ValueError, AttributeError):
            rsha = None
        if rsha is not None and pkv.get("git_sha") != rsha and not _retriage_matches(src_dir, rsha, pkv.get("git_sha")):
            raise ValueError("%s: git_sha %s differs from the start record in provenance.txt (%s): a reused or mixed "
                             "directory" % (src, str(rsha)[:12], pkv.get("git_sha", "none")[:12]))
        if os.path.isfile(src) and os.path.getmtime(src) < os.path.getmtime(prov):
            raise ValueError("%s is older than provenance.txt: a reused or interrupted directory (run again into a "
                             "fresh --out)" % src)
    try:
        with open(src, "r", encoding="utf-8") as f:
            doc = json.load(f)
    except (OSError, ValueError) as e:
        raise ValueError("cannot read %s: %s" % (src, e))
    errs = validate(doc)
    if errs:
        raise ValueError("%s is invalid: %s" % (src, "; ".join(errs[:5])))
    if doc["dirty_tree"]:
        raise ValueError("%s was made on a dirty tree: a criterion run must be repeated on a clean tree" % src)
    if doc["env"] != env:
        raise ValueError("%s is for environment %s, not %s" % (src, doc["env"], env))
    if any(c["id"].startswith("STAGE-") for c in doc["cases"]):
        raise ValueError("%s has STAGE-* cases: it is not the standalone result of one criterion run" % src)
    found = criterion_suite_of(doc)
    if suite is None:
        suite = found
    if suite is None or found != suite:
        raise ValueError("%s: cannot tell the criterion suite (%s)" % (src, "found %s, asked %s" % (found, suite) if found or suite else "no criterion case"))
    pr = criterion_problems(doc, suite)
    if pr:
        raise ValueError("%s is not a passing criterion run of %s: %s" % (src, suite, "; ".join(pr)))
    rid = "%s-%s-%s-%s" % (doc["started"][:10], short_sha(doc["git_sha"]), env, suite)
    out_dir = os.path.abspath(out_dir)
    out_path = os.path.join(out_dir, rid + ".json")
    if os.path.exists(out_path) or os.path.exists(os.path.join(out_dir, "evidence", rid)):
        raise ValueError("%s exists already (a criterion run of this commit, day and suite is adopted once)" % rid)
    os.makedirs(out_dir, exist_ok=True)
    stage_dir = tempfile.mkdtemp(prefix=".evidence-", dir=out_dir)
    try:
        cases = []
        for c in doc["cases"]:
            c = dict(c)
            new_ev, missing = [], []
            for p in c["evidence"]:
                sp = os.path.join(src_dir, p)
                if os.path.islink(sp) or not _inside(src_dir, sp):
                    raise ValueError("evidence %r of %s is a symlink or lies outside %s: refused" % (p, c["id"], src_dir))
                if os.path.isfile(sp) and _copy_scrubbed(sp, os.path.join(stage_dir, p), repo):
                    new_ev.append("evidence/%s/%s" % (rid, p))
                else:
                    missing.append(p)
            c["evidence"] = new_ev
            note = c["note"] or ""
            if missing:
                note = (note + "; " if note else "") + "evidence not stored: " + ", ".join(missing)
            c["note"] = scrub_text(note, repo) or None
            cases.append(c)
        cases.append({"id": stage_case_id(env, "mode"), "status": "pass", "duration_s": 0.0, "evidence": [],
                      "note": CRITERION_NOTE + suite})
        new = dict(doc)
        new["cases"] = cases
        new["host_os"] = scrub_text(doc["host_os"], repo)
        write_result(new, out_dir, rid)
        try:
            if os.listdir(stage_dir):
                os.makedirs(os.path.join(out_dir, "evidence"), exist_ok=True)
                os.rename(stage_dir, os.path.join(out_dir, "evidence", rid))
        except OSError:
            os.unlink(out_path)
            raise
    finally:
        shutil.rmtree(stage_dir, ignore_errors=True)
    return out_path


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
        # result files: work/results/*.json (harness runs) and artifacts/<stage>/result.json (a stress entry
        # point that writes its own result in this schema; its evidence paths are relative to that directory
        # and are kept under the stage's name, where the stage's artifacts are copied as well)
        sources = []
        rdir0 = os.path.join(work, "results")
        for fn in sorted(os.listdir(rdir0)) if os.path.isdir(rdir0) else []:
            if fn.endswith(".json"):
                sources.append((rdir0, fn, ""))
        adir0 = os.path.join(work, "artifacts")
        for sd in sorted(os.listdir(adir0)) if os.path.isdir(adir0) else []:
            if os.path.isfile(os.path.join(adir0, sd, "result.json")):
                sources.append((os.path.join(adir0, sd), "result.json", sd + "/"))
        for rdir, fn, prefix in sources:
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
                    rest = prefix + (p[len("evidence/"):] if p.startswith("evidence/") else p)
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
    # the run-mode record (STAGE-<ENV>-MODE, written by test-all.sh) verifies nothing
    verified = sum(1 for c in doc["cases"] if c["status"] == "pass" and not is_mode_case(c["id"]))
    if verified == 0:
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


def ancestor_check(repo, head):
    """A predicate sha -> bool: is the commit an ancestor of HEAD (or equal), in the git repository REPO.
    RuntimeError when git cannot resolve HEAD (fail closed: no result is then silently trusted)."""
    if _git(repo, "rev-parse", "--verify", "--quiet", head + "^{commit}") is None:
        raise RuntimeError("cannot resolve %s in %s: the table needs git to tell which results belong to this tree" % (head, repo))
    cache = {}

    def check(sha):
        if sha not in cache:
            r = subprocess.run(("git", "-c", "safe.directory=*", "-C", repo, "merge-base", "--is-ancestor", sha, head),
                               stdout=subprocess.DEVNULL, stderr=subprocess.DEVNULL, check=False)
            cache[sha] = r.returncode == 0  # 1: not an ancestor; 128: unknown commit or not an ancestor
        return cache[sha]
    return check


def load_results(results_dir, is_ancestor=None):
    """[(filename, doc)] of the valid, clean-tree result files, oldest first. With IS_ANCESTOR (a predicate on
    a commit id), results of a commit that is not an ancestor of HEAD are left out (M5-F11)."""
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
            if is_ancestor is not None and not is_ancestor(doc["git_sha"]):
                sys.stderr.write("report: ignoring %s: commit %s is not an ancestor of HEAD\n" % (fn, short_sha(doc["git_sha"])))
                continue
            docs.append((fn, doc))
    docs.sort(key=lambda t: (t[1]["started"], t[0]))
    return docs


def is_short_case(cid):
    """A shortened stress or perf run writes `<criterion id>-SHORT` (M5-F04): not a criterion result."""
    return cid.endswith("-SHORT")


def case_status_ex(docs, envs):
    """id -> (status, git_sha, started) over results of the given envs: the newest status that is not a skip (a
    skip-only result never hides an earlier outcome); a case that only ever skipped is 'skip'. A `-SHORT` case
    is never taken: a shortened run must not decide a row."""
    out = {}
    for _fn, doc in docs:
        if doc["env"] not in envs:
            continue
        for c in doc["cases"]:
            if is_short_case(c["id"]):
                continue
            if c["status"] != "skip" or c["id"] not in out:
                out[c["id"]] = (c["status"], doc["git_sha"], doc["started"])
    return out


def case_status(docs, envs):
    """id -> status (see case_status_ex)."""
    return {i: v[0] for i, v in case_status_ex(docs, envs).items()}


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


HW_NAME_RE = re.compile(r"^(\d{4}-\d{2}-\d{2})-([0-9a-f]{7})\.md$")


def hardware_verified(results_dir, is_ancestor=None):
    """feature id -> the 7-digit commit of the newest hardware result file that signs it off. Only files named
    `<UTC date>-<sha7>.md` count (docs/HARDWARE_TEST_PROTOCOL.md section 11); a file with another name, or with
    IS_ANCESTOR given and a commit that is not an ancestor of HEAD, is ignored."""
    out = {}
    hw = os.path.join(results_dir, "hardware")
    if os.path.isdir(hw):
        for fn in sorted(os.listdir(hw)):
            if not fn.endswith(".md"):
                continue
            m = HW_NAME_RE.match(fn)
            if not m:
                sys.stderr.write("report: ignoring hardware result %s: the name is not <UTC date>-<sha7>.md\n" % fn)
                continue
            if is_ancestor is not None and not is_ancestor(m.group(2)):
                sys.stderr.write("report: ignoring hardware result %s: commit %s is not an ancestor of HEAD\n" % (fn, m.group(2)))
                continue
            with open(os.path.join(hw, fn), "r", encoding="utf-8") as f:
                for m2 in HW_RE.finditer(f.read()):
                    for x in m2.group(1).split(","):
                        if x.strip():
                            out[x.strip()] = m.group(2)  # sorted by name: the newest file wins
    return out


def _match(statuses, patterns):
    return {i: s for i, s in statuses.items() if not is_short_case(i) and any(fnmatch.fnmatchcase(i, p) for p in patterns)}


def feature_commit(ft, vmx, linuxx, hw):
    """The commit shown for a row: the commit of the side that decided its state, in the order of
    feature_state. A failing case decides (the newest failing result, VM before Linux); then the hardware
    file's commit; then the newest result of the VM cases, else of the Linux cases. '-' when there is none."""
    def matched(side):
        return [v for i, v in side.items() if not is_short_case(i)
                and any(fnmatch.fnmatchcase(i, p) for p in ft["cases"])]

    def newest(hits):
        return short_sha(max(hits, key=lambda v: v[2])[1])
    mv, ml = matched(vmx), matched(linuxx)
    for hits in (mv, ml):
        bad = [v for v in hits if v[0] in ("fail", "error")]
        if bad:
            return newest(bad)
    if ft["id"] in hw:
        return hw[ft["id"]]
    for hits in (mv, ml):
        if hits:
            return newest(hits)
    return "-"


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


def render_table(results_dir, features_path, repo=None, head="HEAD"):
    """The status table. With REPO, results and hardware files whose commit is not an ancestor of HEAD are left
    out (M5-F11); without it nothing is filtered (unit tests of the rendering only)."""
    anc = ancestor_check(repo, head) if repo else None
    docs = load_results(results_dir, anc)
    vmx, linuxx = case_status_ex(docs, VM_ENVS), case_status_ex(docs, ("A",))
    vm, linux = case_status(docs, VM_ENVS), case_status(docs, ("A",))
    hw = hardware_verified(results_dir, anc)
    rows = ["| Feature | State | Cases passing | Commit |", "|---|---|---|---|"]
    for ft in load_features(features_path):
        state, n = feature_state(ft, vm, linux, hw)
        rows.append("| %s | %s | %s | %s |" % (ft["name"], state, n, feature_commit(ft, vmx, linuxx, hw)))
    src = ("Built by `python3 tests/harness/report.py readme` from the newest clean-tree result of each case in "
           "`tests/results/`. The last column is the commit of the newest result behind the row (for "
           "`Verified (hardware)`: the commit in the hardware result file name); results of a commit that is not "
           "an ancestor of `HEAD`, and shortened `-SHORT` runs, are left out, so a row older than the code it "
           "describes shows an old commit.")
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
    try:
        block = render_table(results_dir, features, repo)
    except RuntimeError as e:
        sys.stderr.write("report.py: %s\n" % e)
        return 2
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
    if not ok:
        return 1
    # every stage passed but the tree is dirty (or a sub-result came from another commit): not a success a gate can use
    return EXIT_DIRTY if doc["dirty_tree"] else 0


def cmd_adopt(args):
    out = args.out or os.path.join(args.repo, "tests", "results")
    try:
        path = adopt(args.dir, args.env, out, args.repo, args.suite)
    except ValueError as e:
        sys.stderr.write("report.py adopt: refused: %s\n" % e)
        return 1
    sys.stderr.write("adopted: %s (commit it with its evidence directory, then run the gate: "
                     "scripts/ci-local.sh --verify-result ... --milestone M5)\n" % path)
    print(path)
    return 0


def main(argv=None):
    import argparse

    argv = sys.argv[1:] if argv is None else list(argv)
    subs = ("validate", "aggregate", "adopt", "readme", "git-info", "provenance", "build-check", "own-stages")
    if argv and argv[0] not in subs and not argv[0].startswith("-"):
        argv = ["validate"] + argv  # python3 -m harness.report FILE... (the original interface)
    here = os.path.dirname(os.path.abspath(__file__))
    default_repo = os.path.dirname(os.path.dirname(here))
    p = argparse.ArgumentParser(prog="report.py", description=__doc__.split("\n\n")[0])
    p.add_argument("--repo", default=default_repo, help=argparse.SUPPRESS)
    sp = p.add_subparsers(dest="cmd", required=True)
    v = sp.add_parser("validate", help="validate result files")
    v.add_argument("files", nargs="+")
    sp.add_parser("own-stages", help="print '<env> <stage>' for each stage ci-local.sh runs itself")
    sp.add_parser("git-info", help="print '<sha> <true|false>' for the tree under test")
    bc = sp.add_parser("build-check", help="exit 1 unless --build is a CMake build tree of this checkout")
    bc.add_argument("--build", required=True)
    pv = sp.add_parser("provenance", help="write the start state of a run (git_sha=, dirty=) to FILE")
    pv.add_argument("--out", required=True)
    a = sp.add_parser("aggregate", help="merge work directories into one result file")
    a.add_argument("--env", required=True, choices=ENV_VALUES)
    a.add_argument("--work", action="append", required=True, help="work directory (repeatable, in stage order)")
    a.add_argument("--out", default=None, help="result file (default tests/results/<date>-<sha>-<env>[-runN].json)")
    a.add_argument("--started", default=None, help="UTC start of the run, YYYY-MM-DDTHH:MM:SSZ")
    a.add_argument("--git-sha", default=None)
    a.add_argument("--dirty", choices=("true", "false"), default=None)
    ad = sp.add_parser("adopt", help="adopt the result.json of a criterion run into tests/results/ (refuses a dirty, "
                       "sha-less, invalid, shortened or failed run)")
    ad.add_argument("--env", required=True, choices=("A", "B"), help="B for the stress suites, A for the fuzz campaign")
    ad.add_argument("--dir", required=True, help="directory with result.json and its evidence (the run's --out)")
    ad.add_argument("--out", default=None, help="results directory (default tests/results)")
    ad.add_argument("--suite", default=None, choices=sorted(CRITERION_SUITES), help="default: found from the cases")
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
        if args.cmd == "build-check":
            why = build_check(os.path.abspath(args.build), args.repo)
            if why:
                sys.stderr.write("report.py build-check: %s\n" % why)
                return 1
            return 0
        if args.cmd == "provenance":
            sha, dirty = write_provenance(args.out, args.repo)
            print("%s %s" % (sha, "true" if dirty else "false"))
            return 0
        if args.cmd == "own-stages":
            for e, n in OWN_STAGES:
                print("%s %s" % (e, n))
            return 0
        if args.cmd == "aggregate":
            return cmd_aggregate(args)
        if args.cmd == "adopt":
            return cmd_adopt(args)
        return cmd_readme(args)
    except (ValueError, RuntimeError, OSError) as e:
        sys.stderr.write("report.py: %s\n" % e)
        return 2


if __name__ == "__main__":
    raise SystemExit(main())
