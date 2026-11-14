# SPDX-License-Identifier: GPL-2.0-or-later
"""Result file: build, name, write and validate (TEST_STRATEGY.md section 10)."""

import json
import os
import re
import subprocess
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


def main(argv=None):
    """python3 -m harness.report FILE... : validate result files."""
    import sys

    argv = sys.argv[1:] if argv is None else argv
    rc = 0
    for p in argv:
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


if __name__ == "__main__":
    raise SystemExit(main())
