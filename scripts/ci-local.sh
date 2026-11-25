#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Local CI runner (ADR-015, TEST_STRATEGY.md sections 9 and 10). Nothing is uploaded.
#
# Usage: scripts/ci-local.sh --quick [--preset NAME] [--build-dir DIR]
#        scripts/ci-local.sh [--only LIST] [--fuzz-seconds N] [--results-dir DIR] [--slot-dir DIR]
#                            [--fixtures DIR] [--lanes 1|2] [--serial] [--allow-dirty] [--dry-run]
#        scripts/ci-local.sh --verify-result FILE [--verify-result FILE]... [--commit REV]
#                            [--require-env LIST] [--require-case ID]... [--milestone NAME]
#
#   --quick          bootstrap check, configure, build, product build, ctest (T-M1-07). Before every push.
#   (no --quick)     FULL mode (T-M5-08), before each milestone gate and release. Takes hours. Stages, in
#                    order (it continues after a failure and exits 1 if any failed; A, linux-build-test and
#                    fuzz-short, the Docker stages, run next to package and B, the VM stages: see --serial):
#                      bootstrap         scripts/bootstrap.sh
#                      host              scripts/test-all.sh --env host (macOS build and unit tests, no mount)
#                      tsan              ThreadSanitizer unit tests (tsan preset, ctest -R ^tsan., U-TSAN-*)
#                      static-analysis   shellcheck, clang-tidy, cppcheck on project code, on the host
#                                        (clang-tidy: PATH first, then /opt/homebrew/opt/llvm/bin and
#                                        /usr/local/opt/llvm/bin; the stage note names the one used and the
#                                        directories searched; CI_LOCAL_LLVM_DIRS overrides them for tests only)
#                                        (-Wall -Wextra -Werror is part of every build, cmake/NtfsmacFlags.cmake)
#                      A                 scripts/test-all.sh --env A    (container: Linux suites, crash sweep)
#                      linux-build-test  project build and ctest in the container (presets linux and asan)
#                      fuzz-short        tests/fuzz/{image,sanitize}/run.sh --known-ok, --fuzz-seconds each (default 300)
#                      package           scripts/release-local.sh --no-ci: unsigned package built in a golden-VM
#                                        clone and installed in a second clone (takes a VM slot)
#                      B                 scripts/test-all.sh --env B    (macOS VM; takes a VM slot lock)
#                    Result files go to --results-dir (default tests/results): one per test-all run
#                    (<date>-<sha>-<env>.json) and <date>-<sha>-<env>-ci.json for the stages that ci-local
#                    itself runs (env host: bootstrap, tsan, static-analysis; env A: linux-build-test, fuzz-short;
#                    env B: package). Afterwards every file is checked with --verify-result --pre-commit
#                    against HEAD, requiring the environments of the selected stages. A dirty tree is
#                    refused (--allow-dirty: the results are then marked dirty and cannot satisfy a gate).
#   --only LIST      comma-separated subset of the stage names above
#   --dry-run        print the plan (with each test-all plan) and exit; nothing runs
#   --verify-result  TEST_STRATEGY.md section 10, "result for the exact commit". Repeatable. FILE is
#                    accepted when ALL hold: it is valid JSON without duplicate keys or NaN, valid for
#                    tests/harness/report.py validate, env is present, dirty_tree is false, no case failed
#                    or errored, at least one case passed, STAGE-<ENV>-* cases carry its own env, it is
#                    named <date>-<sha7>-<env>[-...].json for its own git_sha and env, it lies under
#                    tests/results/ and its content is committed unchanged in the commit under
#                    verification, every evidence path it names (relative to its own directory) is committed there too,
#                    and git_sha is that commit (HEAD, or --commit REV) or an ancestor with
#                    only the allowed set changed in between (tests/harness/report.py change_allowed):
#                    tests/results/, docs/audits/, CHECKPOINT.md, NEXT_ACTIONS.md, docs/SECURITY_FINDINGS.md,
#                    docs/UPSTREAM_REPORTS.md, new files under tests/fuzz/findings/ (existing ones identical),
#                    rows appended to tests/fuzz/campaign/known.tsv (its old content a prefix of the new), and
#                    the README status table (renames count as the change of both paths; the table must be
#                    what `report.py readme` renders from the results of the commit). --require-env
#                    host,A,B also demands, per environment, STAGE-<ENV>-* cases for every stage of that
#                    environment's `test-all.sh --env <ENV> --dry-run` plan and for the stages ci-local
#                    runs itself in that environment (host: bootstrap, tsan, static-analysis; A: linux-build-test,
#                    fuzz-short; B: package; the table is OWN_STAGES in tests/harness/report.py), every
#                    one with status pass (criterion 5.7). In gate mode (--require-env, --require-case or
#                    --milestone) every STAGE-* case of every environment in the files must pass. The only
#                    skips accepted are the ones the host plan itself declares as skipped by design: host
#                    `harness` and host `stress` (`test-all.sh --env host --dry-run` prints them with a
#                    [note]; the note must match exactly, and no other environment may skip). Every file
#                    must be one of three kinds: a test-all.sh result whose STAGE-<ENV>-MODE case has status
#                    pass and note `full` (any other mode, quick or cases=..., or no record rejects); a `-ci`
#                    file that holds only the own-stage cases; or an adopted criterion run (`report.py adopt`,
#                    M5-F05): <date>-<sha7>-<env>-<suite>.json whose STAGE-<ENV>-MODE case has the note
#                    `criterion:<suite>` and whose other cases are exactly the plain criterion ids of that
#                    suite (soak, parallel, leaks, kill, disconnect, fuzz; the table is CRITERION_SUITES in
#                    tests/harness/report.py), every one passing. A shortened or weaker run has ids with the
#                    suffix -SHORT and is rejected there (M5-F04). An adopted file is no substitute for the
#                    full run: each required environment still needs its full test-all result. A standalone
#                    result that is not adopted is not accepted. --pre-commit checks a freshly
#                    written result before it is committed (skips the path and committed-content checks;
#                    never use it for a gate). Every rejection prints its reason.
#   --require-case   CASE-ID that must be present and pass in the accepted files (repeatable; a skip,
#                    an absent case or a failed one rejects). Any case id, not only STAGE-* ones.
#   --milestone NAME the cases listed in tests/gate/NAME.cases (one id per line, # comments) are
#                    required like --require-case. Needs --require-env host,A,B. Unknown name,
#                    unreadable or empty list: exit 2. The list checks presence and status only, not
#                    the parameters of the run: the parameters are in the case ids. A criterion case has
#                    its plain id only when the run had the criterion parameters (6 h soak, 20 kill
#                    repetitions, ...); every other run writes it with the suffix -SHORT (M5-F04).
#   --lanes N        (full mode) stage B: 2 (default) runs its VM stages in two lanes, two guests at a time;
#                    1 runs them strictly one after the other (test-all.sh --lanes, docs/TEST_STRATEGY.md)
#   --serial         (full mode) no concurrency between the Docker stages (A, linux-build-test, fuzz-short) and
#                    the VM stages (package, B): every stage runs alone, in the order above, with its output on
#                    the console. Default: after bootstrap, host, tsan and static-analysis the two groups run at
#                    the same time, each group in the order above, one log per stage, and the Docker stages are
#                    capped at CI_LOCAL_DOCKER_CPUS CPUs per container (default 4, 0: no cap) so that the guests
#                    keep their cores. The results are the same (docs/TEST_STRATEGY.md). Use --serial to debug
#                    or to time a stage.
#   --fixtures DIR   (full mode) reuse fixture images. The run records the skip "reusing DIR", which the
#                    gate rejects: such a run cannot pass the gate, and ci-local prints a warning at the
#                    start. test-all.sh refuses a DIR whose images are not stamped (.fixtures-key, written by
#                    tests/fixtures/gen --export) with this checkout's key, and records the key and a
#                    manifest of the images in the stage note and evidence (M5-F07).
#   gate command:    scripts/ci-local.sh --verify-result tests/results/<file>.json ... \
#                        --require-env host,A,B --milestone M5
#
# Exit:  0 all stages passed / result accepted, 1 a stage failed / result rejected,
#        2 usage error, an option that does not apply to the mode, or unreadable input.
#
# Test seams (tests/unit/ci only): CI_LOCAL_TEST_ALL names a replacement for scripts/test-all.sh;
# CI_LOCAL_TEST_OWN=1 replaces the commands of the stages ci-local runs itself (they succeed, except the
# stages named in CI_LOCAL_TEST_OWN_FAIL); CI_LOCAL_LLVM_DIRS replaces the directories searched for
# clang-tidy (tests/unit/ci/test_ci_local_tools.sh only). With CI_LOCAL_TEST_OWN, a stub stage sleeps as
# CI_LOCAL_TEST_OWN_SLEEP says ("NAME:SECONDS ...") and appends "start NAME" / "end NAME" lines to the file
# CI_LOCAL_TEST_OWN_EVENTS (and "cpus NAME N" with the NTFS_LINUX_CPUS the stage runs with);
# CI_LOCAL_TEST_OWN_CLEANUP="NAME:SECONDS ..." makes the stub of NAME trap TERM, write to its output and
# take that long to clean up before it ends; CI_LOCAL_TEST_FORK_DELAY=SECONDS waits between the start of the
# two groups; CI_LOCAL_TEST_OWN_TAKE_SLOT="NAME ..." makes those stubs take a VM slot like package_stage;
# CI_LOCAL_TEST_DOCKER_NCPU=N stands for the CPU count of `docker info`. It is refused in full mode (exit 2) unless
# CI_LOCAL_TEST_OWN is set: a gate run must look where the documentation says.
#
# ThreadSanitizer: `scripts/ci-local.sh --quick --preset tsan` builds and runs every test with
# -fsanitize=thread (in build/tsan); the full-mode stage `tsan` runs only the U-TSAN-* drivers.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
py=${PYTHON:-python3}

die() { # message [exit-code]
	printf 'ci-local: %s\n' "$1" >&2
	exit "${2:-2}"
}

usage() {
	# the whole comment header from line 4 (no fixed end line: it drifted as the header grew)
	awk 'NR >= 4 { if ($0 !~ /^#/) exit; print }' "$0" | sed 's/^# \{0,1\}//' >&2
}

quick=0
preset=release
build_dir=
verify_files=
commit=
require_env=
require_cases=
milestone=
own_implied=1
pre_commit=0
only=
fuzz_seconds=300
results_dir=
slot_dir=
fixtures=
lanes=
serial=0
allow_dirty=0
dry=0
# options that belong to one mode only (checked after parsing)
full_opts=
verify_opts=
quick_opts=

while [ $# -gt 0 ]; do
	case "$1" in
	--quick) quick=1 ;;
	--verify-result)
		[ $# -ge 2 ] || { usage; die "--verify-result needs a file"; }
		verify_files="${verify_files}$2
"
		shift
		;;
	--commit)
		[ $# -ge 2 ] || { usage; die "--commit needs a revision"; }
		commit=$2; verify_opts="$verify_opts $1"; shift
		;;
	--require-env)
		[ $# -ge 2 ] || { usage; die "--require-env needs a list"; }
		require_env=$2; verify_opts="$verify_opts $1"; shift
		;;
	--require-case)
		[ $# -ge 2 ] || { usage; die "--require-case needs a case id"; }
		case ",$2," in *,,*) die "--require-case: empty case id" ;; esac
		require_cases="${require_cases:+$require_cases,}$2"; verify_opts="$verify_opts $1"; shift
		;;
	--milestone)
		[ $# -ge 2 ] || { usage; die "--milestone needs a name"; }
		milestone=$2; verify_opts="$verify_opts $1"; shift
		;;
	--pre-commit) pre_commit=1; verify_opts="$verify_opts $1" ;;
	--only)
		[ $# -ge 2 ] || { usage; die "--only needs a list"; }
		only=$2; full_opts="$full_opts $1"; shift
		;;
	--fuzz-seconds)
		[ $# -ge 2 ] || { usage; die "--fuzz-seconds needs a number"; }
		fuzz_seconds=$2; full_opts="$full_opts $1"; shift
		;;
	--results-dir)
		[ $# -ge 2 ] || { usage; die "--results-dir needs a directory"; }
		results_dir=$2; full_opts="$full_opts $1"; shift
		;;
	--slot-dir)
		[ $# -ge 2 ] || { usage; die "--slot-dir needs a directory"; }
		slot_dir=$2; full_opts="$full_opts $1"; shift
		;;
	--fixtures)
		[ $# -ge 2 ] || { usage; die "--fixtures needs a directory"; }
		fixtures=$2; full_opts="$full_opts $1"; shift
		;;
	--lanes)
		[ $# -ge 2 ] || { usage; die "--lanes needs 1 or 2"; }
		lanes=$2; full_opts="$full_opts $1"; shift
		;;
	--serial) serial=1; full_opts="$full_opts $1" ;;
	--allow-dirty) allow_dirty=1; full_opts="$full_opts $1" ;;
	--dry-run) dry=1; full_opts="$full_opts $1" ;;
	--preset)
		[ $# -ge 2 ] || { usage; die "--preset needs a value"; }
		preset=$2; quick_opts="$quick_opts $1"; shift
		;;
	--build-dir)
		[ $# -ge 2 ] || { usage; die "--build-dir needs a value"; }
		build_dir=$2; quick_opts="$quick_opts $1"; shift
		;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" ;;
	esac
	shift
done

case $fuzz_seconds in '' | *[!0-9]*) die "--fuzz-seconds needs a whole number" ;; esac
case $lanes in '' | 1 | 2) ;; *) die "--lanes needs 1 or 2, not $lanes" ;; esac
docker_cpus=${CI_LOCAL_DOCKER_CPUS:-4}
case $docker_cpus in '' | *[!0-9]*) die "CI_LOCAL_DOCKER_CPUS needs a whole number (0: no cap)" ;; esac
if [ -n "$verify_files" ]; then
	[ "$quick" -eq 0 ] || die "--verify-result and --quick cannot be combined"
	[ -z "$full_opts" ] || die "options of full mode do not apply to --verify-result:$full_opts"
	[ -z "$quick_opts" ] || die "options of --quick do not apply to --verify-result:$quick_opts"
elif [ "$quick" -eq 1 ]; then
	[ -z "$full_opts" ] || die "options of full mode do not apply to --quick:$full_opts"
	[ -z "$verify_opts" ] || die "options of --verify-result do not apply to --quick:$verify_opts"
else
	[ -z "$verify_opts" ] || die "options of --verify-result need --verify-result:$verify_opts"
	[ -z "$quick_opts" ] || die "options of --quick need --quick:$quick_opts"
fi

# ---- --verify-result (TEST_STRATEGY.md section 10) --------------------------------------------

# --milestone NAME: add the cases of tests/gate/NAME.cases to require_cases (fail closed: exit 2)
if [ -n "$milestone" ]; then
	case $milestone in '' | *[!A-Za-z0-9_-]*) die "--milestone: bad name '$milestone'" ;; esac
	_mf=$root/tests/gate/$milestone.cases
	{ [ -f "$_mf" ] && [ -r "$_mf" ]; } || die "--milestone: unknown milestone '$milestone' (no readable $_mf)"
	_ml=$(LC_ALL=C; export LC_ALL; sed 's/#.*//' "$_mf" | tr -s ' \t\r' '\n\n\n' | grep -v '^$' || true)
	[ -n "$_ml" ] || die "--milestone: $_mf lists no case"
	if (LC_ALL=C; export LC_ALL; printf '%s\n' "$_ml" | grep -qv '^[A-Z][A-Z0-9-]*$'); then die "--milestone: $_mf holds a malformed case id"; fi
	for _need in host A B; do
		case ",$require_env," in *",$_need,"*) ;; *) die "--milestone needs --require-env host,A,B (environment $_need is not required)" ;; esac
	done
	require_cases="${require_cases:+$require_cases,}$(printf '%s\n' "$_ml" | tr '\n' ',' | sed 's/,$//')"
fi

test_all=${CI_LOCAL_TEST_ALL:-$here/test-all.sh}

# the clang-tidy search path override is a test seam: refuse it when full mode would run the gate stages
if [ -n "${CI_LOCAL_LLVM_DIRS:-}" ] && [ "$quick" -eq 0 ] && [ -z "$verify_files" ] && [ -z "${CI_LOCAL_TEST_OWN:-}" ]; then
	die "CI_LOCAL_LLVM_DIRS is a test seam and is refused in full mode (unset it)"
fi

# verify_results FILE...: check the files together; prints one line per file and the reason of every
# rejection. Uses the globals commit, require_env, require_cases, pre_commit and test_all.
verify_results() {
	"$py" -B -I - "$root" "$commit" "$require_env" "$pre_commit" "$require_cases" "$test_all" "$own_implied" "$@" <<'PYEOF'
import json
import os
import re
import subprocess
import sys

root, rev, require, pre_commit, req_cases, test_all, own_implied = sys.argv[1:8]
pre_commit = pre_commit == "1"
own_implied = own_implied == "1"
files = sys.argv[8:]
sys.path.insert(0, os.path.join(root, "tests", "harness"))
import report  # noqa: E402

# The paths that may change since the tested commit are defined once, in tests/harness/report.py
# (change_allowed), and shared with the fuzz campaign re-triage.
# Only the host plan has skips by design (test-all.sh plan_host: harness, stress). They are read from the plan
# itself (`test-all.sh --env host --dry-run` prints a skipped stage as `... [note]`), so the note is never
# copied into this script. The stage names are pinned: a new skip in the host plan is not by design until it is
# added here after review.
SKIP_BY_DESIGN_ENV = "host"
SKIP_BY_DESIGN_STAGES = ("harness", "stress")
NAME_RE = re.compile(r"^\d{4}-\d{2}-\d{2}-([0-9a-f]{7})-(host|A|B)(-[A-Za-z0-9._-]+)?\.json$")


def git(*a, raw=False):
    r = subprocess.run(("git", "-c", "safe.directory=*", "-C", root) + a, stdout=subprocess.PIPE,
                       stderr=subprocess.PIPE, check=False)
    return r.returncode, (r.stdout if raw else r.stdout.decode("utf-8", "replace"))


def outside_table(text):
    """README text without the generated status table."""
    b = report.BEGIN_RE.search(text)
    e = text.find(report.END_MARK)
    if not b or e < 0:
        return text
    return text[:b.start()] + text[e + len(report.END_MARK):]


def table_is_current(commit):
    """The README status table of COMMIT equals what report.py renders from that commit's results."""
    import io
    import tarfile
    import tempfile
    rc, readme = git("show", "%s:README.md" % commit)
    if rc != 0:
        return False
    r = subprocess.run(("git", "-c", "safe.directory=*", "-C", root, "archive", commit, "tests/results",
                        "tests/harness/features.json"), stdout=subprocess.PIPE, stderr=subprocess.DEVNULL, check=False)
    if r.returncode != 0:
        return False
    with tempfile.TemporaryDirectory() as d:
        with tarfile.open(fileobj=io.BytesIO(r.stdout)) as t:
            t.extractall(d)
        try:
            block = report.render_table(os.path.join(d, "tests", "results"),
                                        os.path.join(d, "tests", "harness", "features.json"), root, commit)
        except RuntimeError:
            return False
    try:
        return report.render_readme(readme, block) == readme
    except ValueError:
        return False


# every file must be readable before the commit is resolved (a missing file is a usage error whatever the repository)
for f in files:
    try:
        with open(f, "rb"):
            pass
    except OSError as e:
        sys.stderr.write("ci-local: cannot read %s: %s\n" % (f, e.strerror))
        sys.exit(2)

rc, out = git("rev-parse", "--verify", "--quiet", (rev or "HEAD") + "^{commit}")
if rc != 0:
    sys.stderr.write("ci-local: cannot resolve the commit to verify: %s\n" % (rev or "HEAD"))
    sys.exit(2)
target = out.strip()
bad = 0
accepted = []


def reject(f, why):
    global bad
    bad += 1
    print("verify-result: REJECTED %s: %s" % (f, why))


def changed_outside(tested):
    """Paths changed between the tested commit and the target that the rule does not allow, or None.
    Renames are not detected, so a rename counts as the deletion and the creation of its two paths."""
    rc, out = git("diff-tree", "-r", "-z", "--no-renames", "--name-only", tested, target, raw=True)
    if rc != 0:
        return None
    paths = [p.decode("utf-8", "surrogateescape") for p in out.split(b"\0") if p]
    bad_paths = []
    readme_changed = False
    for p in paths:
        if not report.disallowed_changes(
                [p], lambda which, q: report.git_blob(root, tested if which == "old" else target, q)):
            continue
        if p == "README.md":
            readme_changed = True
            _, a = git("show", "%s:README.md" % tested)
            _, b = git("show", "%s:README.md" % target)
            if outside_table(a) == outside_table(b):
                continue
        bad_paths.append(p)
    if readme_changed and "README.md" not in bad_paths and not table_is_current(target):
        bad_paths.append("README.md (status table is not what report.py renders from the results)")
    return bad_paths


def listed(items):
    return ", ".join(items[:8]) + (", ..." if len(items) > 8 else "")


def no_nan(c):
    raise ValueError("%s is not allowed" % c)


def no_dup(pairs):
    d = {}
    for k, v in pairs:
        if k in d:
            raise ValueError("duplicate key %r" % k)
        d[k] = v
    return d


for f in files:
    try:
        with open(f, "r", encoding="utf-8") as fh:
            doc = json.load(fh, parse_constant=no_nan, object_pairs_hook=no_dup)
    except OSError as e:
        sys.stderr.write("ci-local: cannot read %s: %s\n" % (f, e.strerror))
        sys.exit(2)
    except ValueError as e:
        reject(f, "not valid JSON (%s)" % e)
        continue
    if not isinstance(doc, dict):
        reject(f, "schema invalid: result is not an object")
        continue
    if doc.get("env") in (None, ""):
        reject(f, "environment missing: no env in the result")
        continue
    errs = report.validate(doc)
    if errs:
        reject(f, "schema invalid: " + "; ".join(errs[:5]) + (" (+%d more)" % (len(errs) - 5) if len(errs) > 5 else ""))
        continue
    if doc["dirty_tree"]:
        reject(f, "dirty_tree is true: a result from an uncommitted tree cannot satisfy a gate")
        continue
    cases = doc["cases"]
    failed = [c["id"] for c in cases if c["status"] in ("fail", "error")]
    if failed:
        reject(f, "%d case(s) failed or errored: %s" % (len(failed), listed(failed)))
        continue
    if not any(c["status"] == "pass" and not report.is_mode_case(c["id"]) for c in cases):
        reject(f, "no case passed: nothing was verified")
        continue
    own = "STAGE-%s-" % doc["env"].upper()
    foreign = [c["id"] for c in cases if c["id"].startswith("STAGE-") and not c["id"].startswith(own)]
    if foreign:
        reject(f, "case(s) of another environment in an env %s result: %s" % (doc["env"], listed(foreign)))
        continue
    tested = doc["git_sha"]
    m = NAME_RE.match(os.path.basename(f))
    if not m:
        reject(f, "file name must be <date>-<sha7>-<env>[-...].json")
        continue
    if m.group(1) != tested[:7] or m.group(2) != doc["env"]:
        reject(f, "file name %s does not match git_sha %s and env %s" % (os.path.basename(f), tested[:7], doc["env"]))
        continue
    if not pre_commit:
        real = os.path.realpath(f)
        rel = os.path.relpath(real, os.path.realpath(root))
        if rel.startswith("..") or not rel.startswith("tests/results/"):
            reject(f, "the result must lie under tests/results/ of this repository")
            continue
        rc, blob = git("show", "%s:%s" % (target, rel), raw=True)
        if rc != 0:
            reject(f, "%s is not committed in %s" % (rel, target[:12]))
            continue
        with open(f, "rb") as fh:
            if fh.read() != blob:
                reject(f, "%s differs from the copy committed in %s" % (rel, target[:12]))
                continue
        # the evidence paths are relative to the directory of the result file; each must be committed with it
        # (one `git ls-tree` of the result directory per file; a directory path is not a committed file)
        missing_ev = []
        if any(c["evidence"] for c in cases):
            lrc, lout = git("ls-tree", "-r", "--name-only", "-z", target, "--", os.path.dirname(rel), raw=True)
            if lrc != 0:
                reject(f, "cannot list the tree of %s" % target[:12])
                continue
            committed = set(x.decode("utf-8", "surrogateescape") for x in lout.split(b"\0") if x)
            for c in cases:
                for p in c["evidence"]:
                    if os.path.normpath(os.path.join(os.path.dirname(rel), p)) not in committed and p not in missing_ev:
                        missing_ev.append(p)
        if missing_ev:
            reject(f, "%d evidence path(s) named by the result are not committed in %s: %s"
                   % (len(missing_ev), target[:12], listed(missing_ev)))
            continue
    if tested != target:
        rc, _ = git("cat-file", "-e", tested + "^{commit}")
        if rc != 0:
            reject(f, "git_sha %s is not a commit of this repository" % tested[:12])
            continue
        rc, _ = git("merge-base", "--is-ancestor", tested, target)
        if rc != 0:
            reject(f, "git_sha %s is neither the commit under verification (%s) nor its ancestor"
                   % (tested[:12], target[:12]))
            continue
        paths = changed_outside(tested)
        if paths is None:
            reject(f, "cannot diff %s..%s" % (tested[:12], target[:12]))
            continue
        if paths:
            reject(f, "%d path(s) changed since the tested commit %s outside the allowed set: %s"
                   % (len(paths), tested[:12], listed(paths)))
            continue
    doc["_file"] = f
    accepted.append(doc)
    n_pass = sum(1 for c in cases if c["status"] == "pass")
    n_skip = sum(1 for c in cases if c["status"] == "skip")
    print("verify-result: ok %s: env %s, tested %s, covers %s, %d pass, %d skip%s"
          % (f, doc["env"], tested[:12], target[:12], n_pass, n_skip,
             " (list the skips in the milestone audit)" if n_skip else ""))


def plan_stages(env):
    try:
        r = subprocess.run(("sh", test_all, "--env", env, "--dry-run"), stdout=subprocess.PIPE,
                           stderr=subprocess.STDOUT, check=False, timeout=300)
    except (OSError, subprocess.SubprocessError):
        return None
    if r.returncode != 0:
        return None
    text = r.stdout.decode("utf-8", "replace")
    names = re.findall(r"^\s*\d+ {2}(\S+)", text, re.M)
    skips = {}
    if env == SKIP_BY_DESIGN_ENV:
        for n, note in re.findall(r"^\s*\d+ {2}(\S+) .*? {2}\[([^\]]+)\]\s*$", text, re.M):
            if n in SKIP_BY_DESIGN_STAGES:
                skips[report.stage_case_id(env, n)] = note
    return (names, skips) if names else None


if not bad and (require or req_cases):
    required = [c for c in req_cases.split(",") if c]
    occ = {}  # case id -> every case of that id in the accepted files (all must be ok)
    for d in accepted:
        for c in d["cases"]:
            occ.setdefault(c["id"], []).append(c)
    own_ids = {}  # env -> case ids of the stages ci-local runs itself
    for e, n in report.OWN_STAGES:
        own_ids.setdefault(e, set()).add(report.stage_case_id(e, n))

    def describe(cs):
        return ", ".join("%s%s" % (c["status"], " (%s)" % c["note"] if c.get("note") else "") for c in cs)

    # every file is a full test-all result (with its mode record), a -ci file of own stages only, or an adopted
    # criterion run (`report.py adopt`: the mode record has the note criterion:<suite>, the cases are exactly the
    # plain criterion ids of that suite, M5-F04 and M5-F05)
    for f, d in [(x["_file"], x) for x in accepted]:
        env = d["env"]
        mode_id = report.stage_case_id(env, "mode")
        modes = [c for c in d["cases"] if c["id"] == mode_id]
        mnote = (modes[0].get("note") or "") if modes else ""
        if modes and modes[0]["status"] == "pass" and mnote.startswith(report.CRITERION_NOTE):
            suite = mnote[len(report.CRITERION_NOTE):]
            why = report.criterion_problems(d, suite)
            if report.criterion_suite_of(d) != suite:
                why.append("the cases do not belong to suite %s" % suite)
            nm = NAME_RE.match(os.path.basename(f))
            if not nm or nm.group(3) != "-" + suite:
                why.append("the file name must be <date>-<sha7>-<env>-%s.json" % suite)
            if why:
                bad += 1
                print("verify-result: REJECTED %s: not a criterion run of %s: %s" % (f, suite, "; ".join(why)))
        elif modes:
            if modes[0]["status"] != "pass" or mnote != "full":
                bad += 1
                print("verify-result: REJECTED %s: not a full run (%s: %s)" % (f, mode_id, describe(modes)))
        elif not all(c["id"] in own_ids.get(env, ()) for c in d["cases"]):
            bad += 1
            print("verify-result: REJECTED %s: no run-mode record (%s) and not a -ci file of own stages or an adopted "
                  "criterion run: a standalone result cannot show that the run was a full one (M5-F05)" % (f, mode_id))
    present = sorted({d["env"] for d in accepted})
    plans = {}
    for env in dict.fromkeys([e for e in require.split(",") if e] + present):
        plans[env] = plan_stages(env)
        if plans[env] is None:
            bad += 1
            print("verify-result: REJECTED: cannot read the stage plan of `test-all.sh --env %s --dry-run`" % env)
    # every STAGE case of every environment present must pass; a skip only where the plan declares it by design
    for env in present:
        if plans.get(env) is None:
            continue
        by_design = plans[env][1]
        notpass = []
        for d in accepted:
            if d["env"] != env:
                continue
            for c in d["cases"]:
                if not c["id"].startswith("STAGE-") or c["status"] == "pass":
                    continue
                if c["status"] == "skip" and by_design.get(c["id"]) == (c.get("note") or ""):
                    continue
                notpass.append("%s=%s" % (c["id"], describe([c])))
        if notpass:
            bad += 1
            print("verify-result: REJECTED: environment %s: stage case(s) not passed (a skip is accepted only "
                  "where the plan declares it by design): %s" % (env, listed(notpass)))

    for env in [e for e in require.split(",") if e]:
        docs = [d for d in accepted if d["env"] == env]
        if not docs:
            bad += 1
            print("verify-result: REJECTED: no accepted result for environment %s (accepted: %s)"
                  % (env, ", ".join(sorted({d["env"] for d in accepted})) or "none"))
            continue
        if plans.get(env) is None:
            continue
        mode_id = report.stage_case_id(env, "mode")
        if mode_id not in occ:
            bad += 1
            print("verify-result: REJECTED: environment %s has no run-mode record (%s): "
                  "a result without it cannot show that the run was a full one" % (env, mode_id))
        elif not any(c["status"] == "pass" and (c.get("note") or "") == "full" for c in occ[mode_id]):
            bad += 1
            print("verify-result: REJECTED: environment %s has no full test-all run (%s with the note full; an "
                  "adopted criterion run is not one)" % (env, mode_id))
        need = [report.stage_case_id(env, n) for n in plans[env][0]]
        if own_implied:
            need += [report.stage_case_id(e, n) for e, n in report.OWN_STAGES if e == env]
        need = list(dict.fromkeys(need))
        missing = [c for c in need if c not in occ]
        if missing:
            bad += 1
            print("verify-result: REJECTED: environment %s lacks the stage case(s): %s" % (env, listed(missing)))
    for c in dict.fromkeys(required):
        cs = occ.get(c)
        if not cs:
            bad += 1
            print("verify-result: REJECTED: required case %s is absent from the accepted files" % c)
        elif not all(x["status"] == "pass" for x in cs):
            bad += 1
            print("verify-result: REJECTED: required case %s did not pass: %s"
                  % (c, describe([x for x in cs if x["status"] != "pass"])))
print("verify-result: %s" % ("FAILED" if bad else "PASSED"))
sys.exit(1 if bad else 0)
PYEOF
}

if [ -n "$verify_files" ]; then
	set --
	while IFS= read -r _f; do
		[ -z "$_f" ] || set -- "$@" "$_f"
	done <<VEOF
$verify_files
VEOF
	_rc=0
	verify_results "$@" || _rc=$?
	exit "$_rc"
fi

# ---- full mode (TEST_STRATEGY.md section 9) ---------------------------------------------------
all_stages="bootstrap host tsan static-analysis A linux-build-test fuzz-short package B"
# environment each stage's result is recorded under
stage_env() {
	case $1 in
	host | A | B) echo "$1" ;;
	*) printf '%s\n' "$own_table" | awk -v n="$1" '$2 == n { print $1; exit }' ;;
	esac
}
if [ "$quick" -eq 0 ]; then
	sel=$all_stages
	if [ -n "$only" ]; then
		sel=$(printf '%s' "$only" | tr ',' ' ')
		set -- $sel
		[ $# -gt 0 ] || die "--only: no stage named (stages: $all_stages)"
		for _s in "$@"; do
			case " $all_stages " in *" $_s "*) ;; *) die "--only: unknown stage '$_s' (stages: $all_stages)" ;; esac
		done
	fi
	want() { case " $sel " in *" $1 "*) return 0 ;; esac; return 1; }
	# sel_of STAGE...: the selected ones among them, in the order given
	sel_of() {
		_so=
		for _sx in "$@"; do
			if want "$_sx"; then _so="${_so:+$_so }$_sx"; fi
		done
		printf '%s' "$_so"
	}
	# the stages that run before the two groups (local builds, CPU heavy), the Docker group and the VM group
	grp_pre=$(sel_of bootstrap host tsan static-analysis)
	grp_d=$(sel_of A linux-build-test fuzz-short)
	grp_v=$(sel_of package B)
	concurrent=0
	if [ "$serial" -eq 0 ] && [ -n "$grp_d" ] && [ -n "$grp_v" ]; then concurrent=1; fi
	[ -n "$results_dir" ] || results_dir=$root/tests/results
	case $results_dir in /*) ;; *) results_dir=$(pwd)/$results_dir ;; esac
	case $slot_dir in '' | /*) ;; *) slot_dir=$(pwd)/$slot_dir ;; esac
	case $fixtures in '' | /*) ;; *) fixtures=$(pwd)/$fixtures ;; esac
	if [ -n "$fixtures" ]; then
		printf 'ci-local: WARNING: --fixtures %s: the B result records the skip "reusing ..." for the fixtures, which the gate rejects. test-all.sh also refuses a directory whose images are not stamped for this checkout (M5-F07). This run cannot pass the gate.\n' "$fixtures" >&2
	fi
	[ -n "$slot_dir" ] || slot_dir=${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}

	# the environments (and the stages ci-local runs itself) the final verification demands; the table of
	# own stages is report.OWN_STAGES, the same one --require-env uses
	own_table=$("$py" -B "$root/tests/harness/report.py" own-stages) || die "cannot read the table of own stages"
	for _n in $(printf '%s\n' "$own_table" | awk '{print $2}'); do
		case " $all_stages " in *" $_n "*) ;; *) die "own stage '$_n' of report.py is not a stage of ci-local" ;; esac
	done
	need_envs=
	need_cases=
	for _s in $all_stages; do
		want "$_s" || continue
		_e=$(stage_env "$_s")
		[ -n "$_e" ] || continue
		case ",$need_envs," in *",$_e,"*) ;; *) need_envs="${need_envs:+$need_envs,}$_e" ;; esac
		case $_s in
		host | A | B) ;;
		*) need_cases="${need_cases:+$need_cases,}$("$py" -B -c 'import sys; sys.path.insert(0, sys.argv[1]); import report; print(report.stage_case_id(sys.argv[2], sys.argv[3]))' "$root/tests/harness" "$_e" "$_s")" ;;
		esac
	done

	if [ "$dry" -eq 1 ]; then
		printf 'ci-local full plan: stages=%s fuzz-seconds=%s results-dir=%s\n' "$sel" "$fuzz_seconds" "$results_dir"
		n=0
		for _s in $all_stages; do
			want "$_s" || continue
			n=$((n + 1))
			case $_s in
			bootstrap) _d="scripts/bootstrap.sh, recorded in env host" ;;
			host | A | B) _d="scripts/test-all.sh --env $_s" ;;
			tsan) _d="tsan preset: cmake --preset tsan, build, ctest -R ^tsan. (U-TSAN-*), recorded in env host" ;;
			static-analysis) _d="shellcheck (-S error), clang-tidy, cppcheck on project code, on the host, recorded in env host" ;;
			linux-build-test) _d="project build and ctest in the container (presets linux, asan), recorded in env A" ;;
			fuzz-short) _d="tests/fuzz/{image,sanitize}/run.sh --known-ok --skip-security --seconds $fuzz_seconds (container), recorded in env A" ;;
			package) _d="scripts/release-local.sh --no-ci in golden-VM clones (VM slot), recorded in env B" ;;
			esac
			printf '%2d  %-16s %s\n' "$n" "$_s" "$_d"
			case $_s in
			host | A | B) "$test_all" --env "$_s" --dry-run 2>&1 | sed 's/^/      /' || true ;;
			esac
		done
		if [ "$concurrent" -eq 1 ]; then
			printf 'concurrency: after %s, the Docker stages (%s) run next to the VM stages (%s), each group in plan order; Docker cap: %s CPUs per container (0: none)\n' \
				"${grp_pre:-nothing}" "$grp_d" "$grp_v" "$docker_cpus"
		elif [ "$serial" -eq 1 ]; then
			printf 'concurrency: none (--serial), the stages run one after the other\n'
		else
			printf 'concurrency: none, the selection has only one of the two groups\n'
		fi
		printf 'then: --verify-result --pre-commit on every result file against HEAD, --require-env %s\n' "$need_envs"
		printf 'dry run: nothing was run, no result was written\n'
		exit 0
	fi

	cd "$root"
	_gi=$("$py" -B "$root/tests/harness/report.py" git-info) || die "cannot read the git state"
	set -- $_gi
	git_sha=$1
	git_dirty=$2
	if [ "$git_dirty" = true ] && [ "$allow_dirty" -eq 0 ]; then
		die "the working tree is dirty: the results could not satisfy a gate (commit first, or pass --allow-dirty)"
	fi
	mkdir -p "$results_dir" || die "cannot create $results_dir"
	work=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmac-ci-local.XXXXXX") || die "cannot create a work directory"
	for _e in host A B; do
		mkdir -p "$work/$_e/logs"
		: >"$work/$_e/stages.tsv"
	done
	# per stage, one file each: its summary line (sum), its result files (res), its console log when run
	# concurrently (live); the stages of the two groups run in different processes and share nothing else
	mkdir -p "$work/sum" "$work/res" "$work/live"
	slot_held=
	cleanup() {
		# the slots taken by this run's stages: take_slot writes $$, the pid of this shell also in the
		# subshells of the groups and of the stages, so a slot whose stage was stopped by a signal is
		# released here (the VM suites of B hold their own slots under their own pids)
		for _cd in "$slot_dir"/slot-*; do
			[ -f "$_cd/pid" ] || continue
			[ "$(cat "$_cd/pid" 2>/dev/null || true)" = "$$" ] || continue
			rm -f "$_cd/pid"
			rmdir "$_cd" 2>/dev/null || true
		done
		rm -rf "$work"
	}
	trap cleanup EXIT
	trap 'exit 130' INT TERM HUP
	started=$(date -u +%Y-%m-%dT%H:%M:%SZ)
	day=$(date -u +%Y-%m-%d)
	short=$(printf '%.7s' "$git_sha")
	result_files=
	failed=0
	summary=
	interrupted=0

	note_result() { # NAME STATUS SECONDS: the summary line of the stage
		printf '%-16s %-5s %5ss\n' "$1" "$2" "$3" >"$work/sum/$1"
	}

	# free_name SUFFIX: a result path under results_dir that does not exist yet
	free_name() {
		_n=$results_dir/$day-$short-$1.json
		_i=2
		while [ -e "$_n" ]; do
			_n=$results_dir/$day-$short-$1-run$_i.json
			_i=$((_i + 1))
		done
		printf '%s' "$_n"
	}

	run_test_all() { # ENV
		_env=$1
		_out=$(free_name "$_env")
		printf '\n== ci-local stage %s: test-all --env %s ==\n' "$_env" "$_env"
		set -- --env "$_env" --out "$_out"
		[ "$_env" != B ] || set -- "$@" --slot-dir "$slot_dir"
		[ "$_env" != B ] || [ -z "$fixtures" ] || set -- "$@" --fixtures "$fixtures"
		[ "$_env" != B ] || [ -z "$lanes" ] || set -- "$@" --lanes "$lanes"
		_t0=$(date +%s)
		_rc=0
		"$test_all" "$@" || _rc=$?
		_t1=$(date +%s)
		# 3 = every stage passed but the result is marked dirty: expected only under --allow-dirty with a tree
		# that was dirty at the start; the final verification rejects such a result for a gate anyway
		if [ "$_rc" -eq 3 ] && [ "$git_dirty" = true ]; then _rc=0; fi
		if [ -f "$_out" ]; then
			printf '%s\n' "$_out" >>"$work/res/$_env"
		elif [ "$_rc" -eq 0 ]; then
			printf 'ci-local: test-all --env %s exited 0 but wrote no result file\n' "$_env"
			_rc=1
		fi
		if [ "$_rc" -eq 0 ]; then note_result "$_env" ok "$((_t1 - _t0))"; else note_result "$_env" FAIL "$((_t1 - _t0))"; fi
	}

	# own_stage ENV NAME CMD...: a stage ci-local runs itself; recorded like a test-all stage. The note is
	# the last line "NOTE: ..." of its output (the last line on failure).
	own_stage() {
		_oe=$1; _on=$2; shift 2
		printf '\n== ci-local stage %s ==\n' "$_on"
		if [ -n "${CI_LOCAL_TEST_OWN:-}" ]; then set -- stub_own; fi
		_log=$work/$_oe/logs/$_on.log
		rm -f "$work/rc-$_on"
		_t0=$(date +%s)
		{
			_rc=0
			"$@" 2>&1 </dev/null || _rc=$?
			echo "$_rc" >"$work/rc-$_on"
		} | tee "$_log"
		_rc=$(cat "$work/rc-$_on" 2>/dev/null) || _rc=
		case $_rc in '' | *[!0-9]*) _rc=255 ;; esac
		_t1=$(date +%s)
		_note=$(grep '^NOTE: ' "$_log" | tail -n 1 | cut -c7-300 | tr '\t' ' ' || true)
		if [ "$_rc" -eq 0 ]; then
			printf '%s\tpass\t%s\t%s\tlogs/%s.log\n' "$_on" "$((_t1 - _t0))" "$_note" "$_on" >>"$work/$_oe/stages.tsv"
			note_result "$_on" ok "$((_t1 - _t0))"
		else
			[ -n "$_note" ] || _note=$(grep . "$_log" | tail -n 1 | tr '\t' ' ' | cut -c1-300 || true)
			printf '%s\tfail\t%s\texit %s; %s\tlogs/%s.log\n' "$_on" "$((_t1 - _t0))" "$_rc" "$_note" "$_on" >>"$work/$_oe/stages.tsv"
			note_result "$_on" FAIL "$((_t1 - _t0))"
		fi
	}

	stub_own() {
		[ -z "${CI_LOCAL_TEST_OWN_EVENTS:-}" ] || echo "start $_on" >>"$CI_LOCAL_TEST_OWN_EVENTS"
		[ -z "${CI_LOCAL_TEST_OWN_EVENTS:-}" ] || echo "cpus $_on ${NTFS_LINUX_CPUS:-none}" >>"$CI_LOCAL_TEST_OWN_EVENTS"
		echo "NOTE: stub stage $_on"
		case " ${CI_LOCAL_TEST_OWN_TAKE_SLOT:-} " in *" $_on "*) take_slot ;; esac
		for _ss in ${CI_LOCAL_TEST_OWN_SLEEP:-}; do
			case $_ss in
			"$_on":*)
				_sc=0
				for _cs in ${CI_LOCAL_TEST_OWN_CLEANUP:-}; do
					case $_cs in "$_on":*) _sc=${_cs#*:} ;; esac
				done
				if [ "$_sc" -gt 0 ]; then
					# a stage like release-local.sh: a child with a TERM trap that writes to its output and starts
					# a process of its own to clean up (which a second TERM from the sweeper would kill)
					cat >"$work/stubclean.sh" <<'CLEAN'
ev=$1; secs=$2; name=$3; wait_s=$4
on_term() {
	echo cleaning; echo cleaning >&2; echo after-cleanup
	sh -c 'sleep "$1"; echo "cleaned $2" >>"$3"' sh "$secs" "$name" "$ev"
	exit 143
}
trap on_term TERM
sleep "$wait_s" &
wait
CLEAN
					sh "$work/stubclean.sh" "${CI_LOCAL_TEST_OWN_EVENTS:-/dev/null}" "$_sc" "$_on" "${_ss#*:}" || true
				else
					sleep "${_ss#*:}"
				fi
				;;
			esac
		done
		release_slot
		[ -z "${CI_LOCAL_TEST_OWN_EVENTS:-}" ] || echo "end $_on" >>"$CI_LOCAL_TEST_OWN_EVENTS"
		case ",${CI_LOCAL_TEST_OWN_FAIL:-}," in *",$_on,"*) echo "stub failure"; return 1 ;; esac
		return 0
	}

	tsan_stage() {
		cmake --preset tsan >/dev/null || return 1
		cmake --build --preset tsan || return 1
		ctest --preset tsan -R '^tsan\.' || return 1
		echo "NOTE: U-TSAN-* under ThreadSanitizer (build/tsan)"
	}

	static_analysis() {
		_rc=0
		# recorded evidence scripts under tests/results are not code under test
		if command -v shellcheck >/dev/null 2>&1; then
			echo "shellcheck -x -S error"
			find scripts vm packaging tests -name '*.sh' -not -path 'tests/results/*' -print0 | xargs -0 shellcheck -x -S error || _rc=1
		else
			echo "static-analysis: shellcheck is not installed (brew install shellcheck)"; _rc=1
		fi
		# clang-tidy: on PATH, else in the keg-only Homebrew llvm (neither prefix is required to exist)
		_tidy=
		if command -v clang-tidy >/dev/null 2>&1; then
			_tidy=$(command -v clang-tidy)
		else
			_dirs=${CI_LOCAL_LLVM_DIRS:-/opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin}
			_dirs_note="PATH, $_dirs${CI_LOCAL_LLVM_DIRS:+ (CI_LOCAL_LLVM_DIRS override)}"
			for _d in $_dirs; do
				if [ -x "$_d/clang-tidy" ]; then _tidy=$_d/clang-tidy; break; fi
			done
		fi
		if [ -n "$_tidy" ]; then
			echo "clang-tidy: using $_tidy (searched: ${_dirs_note:-PATH})"
			echo "clang-tidy (clang-diagnostic-*, clang-analyzer-*; files of the compile database under src/)"
			_cdb=$root/build/static
			cmake --preset release -B "$_cdb" -DCMAKE_EXPORT_COMPILE_COMMANDS=ON >/dev/null || _rc=1
			_srcs=$(sed -n 's/^ *"file": *"\(.*\)",\{0,1\}$/\1/p' "$_cdb/compile_commands.json" 2>/dev/null | grep "^$root/src/" | sort -u || true)
			if [ -z "$_srcs" ]; then
				echo "static-analysis: no src/ file in $_cdb/compile_commands.json"; _rc=1
			else
				# shellcheck disable=SC2086
				# Off, as checks that do not fit this project:
				#  insecureAPI.DeprecatedOrUnsafeBufferHandling  wants the C11 Annex K *_s functions, which macOS lacks
				#  optin.performance.Padding                      struct layout is not a performance goal here
				# optin.performance.GCDAntipattern stays on: the one semaphore wait (the CLI waits for an XPC
				# reply) carries an inline NOLINT with its reason
				_checks='-*,clang-diagnostic-*,clang-analyzer-*'
				_checks="$_checks,-clang-analyzer-security.insecureAPI.DeprecatedOrUnsafeBufferHandling"
				_checks="$_checks,-clang-analyzer-optin.performance.Padding"
				"$_tidy" -p "$_cdb" --checks="$_checks" --warnings-as-errors='*' $_srcs || _rc=1
			fi
		else
			echo "static-analysis: clang-tidy is not installed (brew install llvm; searched: ${_dirs_note:-PATH, ${CI_LOCAL_LLVM_DIRS:-/opt/homebrew/opt/llvm/bin /usr/local/opt/llvm/bin}})"; _rc=1
		fi
		if command -v cppcheck >/dev/null 2>&1; then
			echo "cppcheck"
			# __has_feature is a clang built-in that cppcheck does not know; -D fixes the configuration to
			# one, so --force makes it check the others too (the macOS branches included)
			cppcheck --error-exitcode=1 --enable=warning,performance,portability --inline-suppr --quiet \
				'-D__has_feature(x)=0' --force -I include src || _rc=1
		else
			echo "static-analysis: cppcheck is not installed (brew install cppcheck)"; _rc=1
		fi
		[ "$_rc" -eq 0 ] || echo "static-analysis: FAILED"
		[ "$_rc" -ne 0 ] || echo "NOTE: clang-tidy used: $_tidy (searched: ${_dirs_note:-PATH})"
		return "$_rc"
	}

	container_ready() { # NAME
		docker info >/dev/null 2>&1 || { echo "$1: docker is not reachable (Environment A needs it)"; return 1; }
		docker image inspect ntfs-macos-linux:release >/dev/null 2>&1 ||
			{ echo "$1: image ntfs-macos-linux:release is missing: run 'vm/linux/run.sh build' yourself"; return 1; }
	}

	# project build and unit tests in the container; the repo is mounted read-only, so build under /tmp
	linux_build_test() {
		container_ready linux-build-test || return 1
		_rc=0
		_skipped=
		for _p in linux asan; do
			echo "== linux-build-test: preset $_p"
			echo "NOTE: fuzz.findings.F1/F3/F4/F8 are not run here: the image of Environment A links pristine NTFS-3G, where these replays fail by design (they pass against the patched library in the A-unit asan stage, tests/unit/probe/inner.sh)"
			_o=$work/A/linux-$_p.out
			_prc=0
			sh "$root/vm/linux/run.sh" run -- bash -c \
				"set -e; cmake --preset $_p -S /work -B /tmp/b-$_p -DNTFSMAC_NTFS3G_PATCHED=OFF >/dev/null && cmake --build /tmp/b-$_p && ctest --test-dir /tmp/b-$_p --output-on-failure -V" \
				>"$_o" 2>&1 || _prc=$?
			cat "$_o"
			# a test that skips a branch prints "SKIP: <reason>" (ctest -V shows it); count them (M5-F09)
			_sk=$(grep -c 'SKIP: ' "$_o" || true)
			_skipped="${_skipped:+$_skipped; }preset $_p: ${_sk:-0} SKIP line(s)"
			if [ "$_prc" -ne 0 ]; then
				_rc=1
				_err=$(grep -m1 'error:' "$_o" | cut -c1-240 || true); [ -n "$_err" ] || _err=$(grep -m1 -E 'FAILED|Failed' "$_o" | cut -c1-240 || true)
				echo "NOTE: linux build or test FAILED with preset $_p: ${_err:-exit $_prc (see the log)}"
			fi
		done
		[ "$_rc" -ne 0 ] || echo "NOTE: skipped tests, counted from SKIP: lines ($_skipped); fuzz.findings.F1/F3/F4/F8 not run here (pristine NTFS-3G, run in A-unit asan)"
		return "$_rc"
	}

	fuzz_short() {
		container_ready fuzz-short || return 1
		_stamp=$(date -u +%Y%m%dT%H%M%SZ)
		_fo=$root/build/fuzz/ci/$_stamp
		mkdir -p "$_fo"
		_rc=0
		# --skip-security leaves out the security descriptor reads (NTFS-3G engine faults F1, F3 in
		# tests/fuzz/TRIAGE.md). --known-ok: a target that fails only on findings of tests/fuzz/campaign/known.tsv
		# (open engine leaks such as F2, F5 in the corpus replay; matched by shortcheck.py, the matcher of the campaign)
		# passes and its ids are named in the note; any artifact, bad run exit, unknown leak or regression blocks the stage
		sh "$root/tests/fuzz/image/run.sh" --known-ok --skip-security --seconds "$fuzz_seconds" --out "$_fo/image" || _rc=1
		sh "$root/tests/fuzz/sanitize/run.sh" --known-ok --seconds "$fuzz_seconds" --out "$_fo/sanitize" || _rc=1
		_known=$(cat "$_fo"/image/*.known "$_fo"/sanitize/*.known 2>/dev/null | tr ',' '\n' | sort -u | sort -V | tr '\n' ',' | sed 's/,$//;s/,/, /g')
		echo "NOTE: known findings seen: ${_known:-none}; left out: image run --skip-security (F1/F3, covered by the replays); fuzz output in build/fuzz/ci/$_stamp"
		return "$_rc"
	}

	# take_slot / release_slot: the VM slot protocol of vm/macos/run-suite.sh (mkdir slot-N, pid file)
	take_slot() {
		mkdir -p "$slot_dir"
		while [ -z "$slot_held" ]; do
			for _s in ${NTFSMAC_VM_SLOTS:-1 2}; do
				if mkdir "$slot_dir/slot-$_s" 2>/dev/null; then
					echo $$ >"$slot_dir/slot-$_s/pid"
					slot_held=$_s
					break
				fi
				_p=$(cat "$slot_dir/slot-$_s/pid" 2>/dev/null || true)
				if [ -n "$_p" ] && ! kill -0 "$_p" 2>/dev/null; then
					rm -f "$slot_dir/slot-$_s/pid"
					rmdir "$slot_dir/slot-$_s" 2>/dev/null || true
				fi
			done
			[ -n "$slot_held" ] || sleep "${NTFSMAC_SLOT_WAIT:-30}"
		done
		echo "package: holding VM slot-$slot_held"
	}
	release_slot() {
		[ -z "$slot_held" ] || { rm -f "$slot_dir/slot-$slot_held/pid"; rmdir "$slot_dir/slot-$slot_held" 2>/dev/null || true; }
		slot_held=
	}

	# the unsigned package, built in a clone of the golden VM and installed in a second clone
	package_stage() {
		command -v tart >/dev/null 2>&1 || { echo "package: tart is not installed"; return 1; }
		tart list --quiet 2>/dev/null | grep -qx ntfsmac-golden || { echo "package: the VM ntfsmac-golden does not exist"; return 1; }
		_tb=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
		[ -n "$_tb" ] && [ -f "$root/third_party/cache/$_tb" ] ||
			{ echo "package: the pinned NTFS-3G tarball is missing: run 'sh scripts/fetch-deps.sh ntfs-3g' yourself (ci-local never downloads it)"; return 1; }
		set --
		[ "$allow_dirty" -eq 0 ] || set -- --allow-dirty
		take_slot
		_rc=0
		sh "$here/release-local.sh" --no-ci "$@" || _rc=$?
		release_slot
		[ "$_rc" -ne 0 ] || echo "NOTE: package built and installed in a VM clone; output in dist/ (git-ignored)"
		return "$_rc"
	}

	# aggregate_own ENV: one result file from the stages ci-local ran itself in $work/ENV
	aggregate_own() {
		[ -s "$work/$1/stages.tsv" ] || return 0
		_out=$(free_name "$1-ci")
		"$py" -B "$root/tests/harness/report.py" aggregate --env "$1" --work "$work/$1" --started "$started" \
			--git-sha "$git_sha" --dirty "$git_dirty" --out "$_out" >/dev/null || true
		if [ -f "$_out" ]; then
			result_files="${result_files}$_out
"
		else
			echo "ci-local: no result file was written for the stages run in env $1"
			failed=1
		fi
	}

	run_stage() { # NAME: one stage, its output on stdout
		case $1 in
		bootstrap) own_stage host bootstrap "$here/bootstrap.sh" ;;
		host) run_test_all host ;;
		tsan) own_stage host tsan tsan_stage ;;
		static-analysis) own_stage host static-analysis static_analysis ;;
		A) run_test_all A ;;
		linux-build-test) own_stage A linux-build-test linux_build_test ;;
		fuzz-short) own_stage A fuzz-short fuzz_short ;;
		package) own_stage B package package_stage ;;
		B) run_test_all B ;;
		esac
	}

	# group_stage GROUP STAGE: run it with its output in its own log; the console gets a start and an end line
	# (and the tail of the log of a failed stage), each written in one piece. `|| true` suspends set -e inside
	# the stage, which is how every stage ran before as well (own_stage and run_test_all test each status).
	group_stage() {
		_gl=$work/live/$2.log
		printf 'ci-local: [%s] start %s (log %s)\n' "$1" "$2" "$_gl"
		run_stage "$2" >"$_gl" 2>&1 || true
		_gsum=$(cat "$work/sum/$2" 2>/dev/null || true)
		if [ -z "$_gsum" ]; then
			printf 'ci-local: [%s] end   %s: LOST (no result recorded)\n' "$1" "$2"
			return 0
		fi
		set -- "$1" "$2" $_gsum
		if [ "$4" = ok ]; then
			printf 'ci-local: [%s] end   %s: ok in %s\n' "$1" "$2" "$5"
		else
			printf 'ci-local: [%s] end   %s: FAIL in %s; last lines of %s:\n%s\n' "$1" "$2" "$5" "$_gl" "$(tail -n 15 "$_gl" | sed 's/^/    | /')"
		fi
	}

	# tree_of PID: PID and its descendants, the top first, without the output filters of the pipelines
	# (tee, sed, ...): a stage that is stopped writes its output through them while it cleans up, and a
	# filter killed with it would turn that into a SIGPIPE that cuts the cleanup short
	tree_of() {
		ps -A -o pid= -o ppid= -o comm= | awk -v root="$1" '
			function base(c) { sub(/.*\//, "", c); return c }
			{ kid[$2] = kid[$2] " " $1; name[$1] = base($3) }
			END {
				n = 0; q[n++] = root
				for (i = 0; i < n; i++) {
					if (!(name[q[i]] in skip)) print q[i]
					m = split(kid[q[i]], a, " "); for (j = 1; j <= m; j++) q[n++] = a[j]
				}
			}
			BEGIN { split("tee sed awk cat tail head grep tr cut sort", f, " "); for (k in f) skip[f[k]] = 1 }'
	}
	# stamp PID: the start time of a process, so that a pid that was reused is not taken for the old process
	stamp() { ps -o lstart= -p "$1" 2>/dev/null | tr ' ' '_'; }
	children_of() { ps -A -o pid= -o ppid= | awk -v p="$1" '$2 == p { print $1 }'; }
	# sweep_stop PID...: (run in a subshell) PID... are the group processes, children of this shell. The first
	# pass sends TERM to every process of their trees. Later passes (every second) send it only to children of
	# the groups that were not there before (a stage that started in the window of the signal), with their
	# trees: what a stopped stage starts itself while it cleans up (release-local.sh deleting its clone) is
	# never signalled again. It ends when the groups and every process it stopped are gone; what is left
	# after CI_LOCAL_STOP_WAIT seconds gets KILL.
	sweep_stop() {
		_main=$$
		_t=0
		_seen=
		_first=1
		while :; do
			_live=0
			_roots=
			for _r in "$@"; do
				if [ "$(ps -o ppid= -p "$_r" 2>/dev/null | tr -d ' ')" = "$_main" ]; then
					_live=1
					_roots="$_roots $_r"
				fi
			done
			for _r in $_roots; do
				if [ "$_first" -eq 1 ]; then _top=$_r; else _top=$(children_of "$_r"); fi
				for _c in $_top; do
					case " $_seen " in *" $_c@"*) continue ;; esac
					_seen="$_seen $_c@$(stamp "$_c")"
					for _p in $(tree_of "$_c"); do
						case " $_seen " in *" $_p@"*) ;; *) _seen="$_seen $_p@$(stamp "$_p")" ;; esac
						kill -TERM "$_p" 2>/dev/null || true
					done
				done
			done
			_first=0
			_left=
			for _e in $_seen; do
				if [ -n "${_e#*@}" ] && [ "$(stamp "${_e%%@*}")" = "${_e#*@}" ]; then _left="$_left ${_e%%@*}"; _live=1; fi
			done
			[ "$_live" -eq 1 ] || break
			if [ "$_t" -ge "${CI_LOCAL_STOP_WAIT:-300}" ]; then
				for _p in $_left; do kill -KILL "$_p" 2>/dev/null || true; done
				break
			fi
			sleep 1
			_t=$((_t + 1))
		done
	}
	# INT, TERM or HUP while the two groups run: no stage starts any more (the stop file) and the sweeper
	# stops what runs; the parent waits for the groups and for the sweeper, which ends when every process
	# it stopped is gone. While the groups are being started the signal only sets the flags (a background
	# job started by the handler would replace $! before it is read); the sweeper starts once, after both pids are known.
	pid_d=
	pid_v=
	sweepers=
	forking=0
	stop_groups() {
		[ -z "$sweepers" ] || return 0
		# shellcheck disable=SC2086
		sweep_stop $pid_d $pid_v &
		sweepers=$!
	}
	sig_stop() {
		[ "$interrupted" -eq 0 ] || return 0
		interrupted=1
		: >"$work/stop"
		printf '\nci-local: interrupted: stopping the Docker and VM stages\n'
		[ "$forking" -eq 1 ] || stop_groups
	}
	# wait_for PID...: until each has ended (wait is cut short by a trapped signal: ask again; a pid that is
	# not a child of this shell makes wait fail at once: stop then)
	wait_for() {
		for _wp in "$@"; do
			while kill -0 "$_wp" 2>/dev/null; do
				_wr=0
				wait "$_wp" 2>/dev/null || _wr=$?
				[ "$_wr" -gt 128 ] || break
			done
		done
	}

	for _s in $grp_pre; do run_stage "$_s"; done

	if [ "$concurrent" -eq 1 ]; then
		printf '\n== ci-local: Docker stages (%s) next to VM stages (%s); one log per stage ==\n' "$grp_d" "$grp_v"
		# the cap cannot exceed the CPUs Docker has (a larger --cpus is refused by docker run)
		if [ "$docker_cpus" -ne 0 ]; then
			if [ -n "${CI_LOCAL_TEST_OWN:-}" ]; then _dn=${CI_LOCAL_TEST_DOCKER_NCPU:-}; else _dn=$(docker info --format '{{.NCPU}}' 2>/dev/null || true); fi
			case $_dn in
			'' | *[!0-9]*) ;;
			*)
				if [ "$docker_cpus" -gt "$_dn" ]; then
					printf 'ci-local: WARNING: CI_LOCAL_DOCKER_CPUS=%s is more than the %s CPUs of Docker: using %s
' "$docker_cpus" "$_dn" "$_dn"
					docker_cpus=$_dn
				fi
				;;
			esac
		fi
		trap sig_stop INT TERM HUP
		forking=1
		(
			trap 'exit 143' INT TERM HUP
			if [ -z "${NTFS_LINUX_CPUS:-}" ] && [ "$docker_cpus" -ne 0 ]; then NTFS_LINUX_CPUS=$docker_cpus; export NTFS_LINUX_CPUS; fi
			for _s in $grp_d; do
				[ ! -e "$work/stop" ] || break
				group_stage D "$_s"
			done
		) &
		pid_d=$!
		if [ -n "${CI_LOCAL_TEST_OWN:-}" ] && [ -n "${CI_LOCAL_TEST_FORK_DELAY:-}" ]; then sleep "$CI_LOCAL_TEST_FORK_DELAY"; fi
		(
			trap 'exit 143' INT TERM HUP
			for _s in $grp_v; do
				[ ! -e "$work/stop" ] || break
				group_stage V "$_s"
			done
		) &
		pid_v=$!
		forking=0
		# a signal while the groups were started only set the flags: stop them now (no stage starts after the
		# stop file exists, but the first stage of a group may have started just before it)
		if [ "$interrupted" -ne 0 ]; then stop_groups; fi
		wait_for "$pid_d" "$pid_v"
		# shellcheck disable=SC2086
		wait_for $sweepers
		trap 'exit 130' INT TERM HUP
		# the logs of the concurrent stages, whole and in plan order (nothing interleaved)
		for _s in $grp_d $grp_v; do
			if [ -f "$work/live/$_s.log" ]; then
				printf '\n== ci-local stage %s: log of the concurrent run ==\n' "$_s"
				cat "$work/live/$_s.log"
			fi
		done
	else
		for _s in $grp_d $grp_v; do run_stage "$_s"; done
	fi

	# the summary and the result files, in plan order whatever the order of completion
	for _s in $all_stages; do
		want "$_s" || continue
		if [ -s "$work/sum/$_s" ]; then
			_l=$(cat "$work/sum/$_s")
		else
			_l=$(printf '%-16s %-5s %5s ' "$_s" NOTRUN -)
		fi
		summary="${summary}${_l}
"
		set -- $_l
		[ "$2" = ok ] || failed=1
		if [ -s "$work/res/$_s" ]; then
			result_files="${result_files}$(cat "$work/res/$_s")
"
		fi
	done
	if [ "$interrupted" -eq 1 ]; then
		printf '\n== summary ==\n%s' "$summary"
		printf 'ci-local: INTERRUPTED\n'
		exit 130
	fi
	aggregate_own host
	aggregate_own A
	aggregate_own B

	printf '\n== summary ==\n%s' "$summary"
	if [ -n "$result_files" ]; then
		printf '\n== verify-result (HEAD %s) ==\n' "$short"
		require_env=$need_envs
		require_cases=$need_cases
		own_implied=0 # only the own stages that were selected (need_cases), not all of them
		pre_commit=1
		set --
		while IFS= read -r _f; do [ -z "$_f" ] || set -- "$@" "$_f"; done <<VEOF2
$result_files
VEOF2
		verify_results "$@" || failed=1
	fi
	if [ "$failed" -ne 0 ]; then
		printf 'ci-local: FAILED\n'
		exit 1
	fi
	printf 'ci-local: PASSED\n'
	exit 0
fi

# ---- quick mode (T-M1-07) ---------------------------------------------------------------------
[ -n "$build_dir" ] || build_dir=$root/build/$preset

summary=
failed=0

# stage NAME CMD...: run CMD, record result and seconds, stop at first failure.
stage() {
	_name=$1; shift
	printf '\n== stage: %s ==\n' "$_name"
	_t0=$(date +%s)
	_rc=0
	"$@" || _rc=$?
	_t1=$(date +%s)
	if [ "$_rc" -eq 0 ]; then _res=ok; else _res=FAIL; failed=1; fi
	summary="${summary}$(printf '%-12s %-5s %4ss' "$_name" "$_res" "$((_t1 - _t0))")
"
	[ "$_rc" -eq 0 ] || finish
}

finish() {
	printf '\n== summary ==\n%s' "$summary"
	if [ "$failed" -ne 0 ]; then
		printf 'ci-local --quick: FAILED\n'
		exit 1
	fi
	printf 'ci-local --quick: PASSED\n'
	exit 0
}

cd "$root"
stage bootstrap "$here/bootstrap.sh"
stage configure cmake --preset "$preset" -B "$build_dir"
stage build cmake --build "$build_dir"
# T-M4-06: the daemon library must also build without the test seams (the shipping configuration).
stage product sh -c 'cmake --preset product -B "$1" >/dev/null && cmake --build "$1" --target ntfsmac_daemon' sh "$root/build/product"
stage ctest ctest --test-dir "$build_dir" --output-on-failure
finish
