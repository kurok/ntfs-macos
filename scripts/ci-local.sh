#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Local CI runner (ADR-015, TEST_STRATEGY.md sections 9 and 10). Nothing is uploaded.
#
# Usage: scripts/ci-local.sh --quick [--preset NAME] [--build-dir DIR]
#        scripts/ci-local.sh [--only LIST] [--fuzz-seconds N] [--results-dir DIR] [--slot-dir DIR]
#                            [--fixtures DIR] [--allow-dirty] [--dry-run]
#        scripts/ci-local.sh --verify-result FILE [--verify-result FILE]... [--commit REV]
#                            [--require-env LIST]
#
#   --quick          bootstrap check, configure, build, product build, ctest (T-M1-07). Before every push.
#   (no --quick)     FULL mode (T-M5-08), before each milestone gate and release. Takes hours. Stages, in
#                    order (it continues after a failure and exits 1 if any failed):
#                      bootstrap         scripts/bootstrap.sh
#                      host              scripts/test-all.sh --env host (macOS build and unit tests, no mount)
#                      tsan              ThreadSanitizer unit tests (tsan preset, ctest -R ^tsan., U-TSAN-*)
#                      static-analysis   shellcheck, clang-tidy, cppcheck on project code, on the host
#                                        (-Wall -Wextra -Werror is part of every build, cmake/NtfsmacFlags.cmake)
#                      A                 scripts/test-all.sh --env A    (container: Linux suites, crash sweep)
#                      linux-build-test  project build and ctest in the container (presets linux and asan)
#                      fuzz-short        tests/fuzz/{image,sanitize}/run.sh, --fuzz-seconds each (default 300)
#                      package           scripts/release-local.sh --no-ci: unsigned package built in a golden-VM
#                                        clone and installed in a second clone (takes a VM slot)
#                      B                 scripts/test-all.sh --env B    (macOS VM; takes a VM slot lock)
#                    Result files go to --results-dir (default tests/results): one per test-all run
#                    (<date>-<sha>-<env>.json) and <date>-<sha>-<env>-ci.json for the stages that ci-local
#                    itself runs (env host: tsan, static-analysis; env A: linux-build-test, fuzz-short;
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
#                    verification, and git_sha is that commit (HEAD, or --commit REV) or an ancestor with
#                    only tests/results/, docs/audits/, CHECKPOINT.md, NEXT_ACTIONS.md and the README status
#                    table changed in between (renames count as the change of both paths; the table must be
#                    what `report.py readme` renders from the results of the commit). --require-env
#                    host,A,B also demands, per environment, STAGE-<ENV>-* cases for every stage of that
#                    environment's `test-all.sh --env <ENV> --dry-run` plan (pass, or skip with a note)
#                    (criterion 5.7). --pre-commit checks a freshly written result before it is committed
#                    (skips the path and committed-content checks; never use it for a gate).
#                    Every rejection prints its reason.
#
# Exit:  0 all stages passed / result accepted, 1 a stage failed / result rejected,
#        2 usage error, an option that does not apply to the mode, or unreadable input.
#
# Test seams (tests/unit/ci only): CI_LOCAL_TEST_ALL names a replacement for scripts/test-all.sh;
# CI_LOCAL_TEST_OWN=1 replaces the commands of the stages ci-local runs itself (they succeed, except the
# stages named in CI_LOCAL_TEST_OWN_FAIL).
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
	sed -n '4,56p' "$0" | sed 's/^# \{0,1\}//' >&2
}

quick=0
preset=release
build_dir=
verify_files=
commit=
require_env=
require_cases=
pre_commit=0
only=
fuzz_seconds=300
results_dir=
slot_dir=
fixtures=
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
		require_cases="${require_cases:+$require_cases,}$2"; verify_opts="$verify_opts $1"; shift
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

test_all=${CI_LOCAL_TEST_ALL:-$here/test-all.sh}

# verify_results FILE...: check the files together; prints one line per file and the reason of every
# rejection. Uses the globals commit, require_env, require_cases, pre_commit and test_all.
verify_results() {
	"$py" -B -I - "$root" "$commit" "$require_env" "$pre_commit" "$require_cases" "$test_all" "$@" <<'PYEOF'
import json
import os
import re
import subprocess
import sys

root, rev, require, pre_commit, req_cases, test_all = sys.argv[1:7]
pre_commit = pre_commit == "1"
files = sys.argv[7:]
sys.path.insert(0, os.path.join(root, "tests", "harness"))
import report  # noqa: E402

ALLOWED_PREFIX = ("tests/results/", "docs/audits/")
ALLOWED_FILES = ("CHECKPOINT.md", "NEXT_ACTIONS.md")
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
        block = report.render_table(os.path.join(d, "tests", "results"),
                                    os.path.join(d, "tests", "harness", "features.json"))
    try:
        return report.render_readme(readme, block) == readme
    except ValueError:
        return False


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
        if p.startswith(ALLOWED_PREFIX) or p in ALLOWED_FILES:
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
    if not any(c["status"] == "pass" for c in cases):
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
    names = re.findall(r"^\s*\d+ {2}(\S+)", r.stdout.decode("utf-8", "replace"), re.M)
    return names or None


if not bad and require:
    extra = [c for c in req_cases.split(",") if c]
    for env in [e for e in require.split(",") if e]:
        docs = [d for d in accepted if d["env"] == env]
        if not docs:
            bad += 1
            print("verify-result: REJECTED: no accepted result for environment %s (accepted: %s)"
                  % (env, ", ".join(sorted({d["env"] for d in accepted})) or "none"))
            continue
        status = {}
        for d in docs:
            for c in d["cases"]:
                status[c["id"]] = c["status"]
        plan = plan_stages(env)
        if plan is None:
            bad += 1
            print("verify-result: REJECTED: cannot read the stage plan of `test-all.sh --env %s --dry-run`" % env)
            continue
        need = [report.stage_case_id(env, n) for n in plan]
        need += [c for c in extra if c.startswith("STAGE-%s-" % env.upper()) and c not in need]
        missing = [c for c in need if status.get(c) not in ("pass", "skip")]
        if missing:
            bad += 1
            print("verify-result: REJECTED: environment %s lacks the stage case(s): %s" % (env, listed(missing)))
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
	host | tsan | static-analysis) echo host ;;
	A | linux-build-test | fuzz-short) echo A ;;
	B | package) echo B ;;
	*) echo "" ;;
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
	[ -n "$results_dir" ] || results_dir=$root/tests/results
	case $results_dir in /*) ;; *) results_dir=$(pwd)/$results_dir ;; esac
	case $slot_dir in '' | /*) ;; *) slot_dir=$(pwd)/$slot_dir ;; esac
	case $fixtures in '' | /*) ;; *) fixtures=$(pwd)/$fixtures ;; esac
	[ -n "$slot_dir" ] || slot_dir=${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}

	# the environments (and the stages ci-local runs itself) the final verification demands
	need_envs=
	need_cases=
	for _s in $all_stages; do
		want "$_s" || continue
		_e=$(stage_env "$_s")
		[ -n "$_e" ] || continue
		case ",$need_envs," in *",$_e,"*) ;; *) need_envs="${need_envs:+$need_envs,}$_e" ;; esac
		case $_s in
		host | A | B | bootstrap) ;;
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
			bootstrap) _d="scripts/bootstrap.sh" ;;
			host | A | B) _d="scripts/test-all.sh --env $_s" ;;
			tsan) _d="tsan preset: cmake --preset tsan, build, ctest -R ^tsan. (U-TSAN-*), recorded in env host" ;;
			static-analysis) _d="shellcheck (-S error), clang-tidy, cppcheck on project code, on the host, recorded in env host" ;;
			linux-build-test) _d="project build and ctest in the container (presets linux, asan), recorded in env A" ;;
			fuzz-short) _d="tests/fuzz/{image,sanitize}/run.sh --skip-security --seconds $fuzz_seconds (container), recorded in env A" ;;
			package) _d="scripts/release-local.sh --no-ci in golden-VM clones (VM slot), recorded in env B" ;;
			esac
			printf '%2d  %-16s %s\n' "$n" "$_s" "$_d"
			case $_s in
			host | A | B) "$test_all" --env "$_s" --dry-run 2>&1 | sed 's/^/      /' || true ;;
			esac
		done
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
	slot_held=
	cleanup() {
		if [ -n "$slot_held" ]; then
			rm -f "$slot_dir/slot-$slot_held/pid"
			rmdir "$slot_dir/slot-$slot_held" 2>/dev/null || true
		fi
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

	note_result() { # NAME STATUS SECONDS
		summary="${summary}$(printf '%-16s %-5s %5ss' "$1" "$2" "$3")
"
		[ "$2" = ok ] || failed=1
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
		_t0=$(date +%s)
		_rc=0
		"$test_all" "$@" || _rc=$?
		_t1=$(date +%s)
		if [ -f "$_out" ]; then
			result_files="${result_files}$_out
"
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
		rm -f "$work/rc"
		_t0=$(date +%s)
		{
			_rc=0
			"$@" 2>&1 </dev/null || _rc=$?
			echo "$_rc" >"$work/rc"
		} | tee "$_log"
		_rc=$(cat "$work/rc" 2>/dev/null) || _rc=
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
		echo "NOTE: stub stage $_on"
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
		if command -v clang-tidy >/dev/null 2>&1; then
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
				clang-tidy -p "$_cdb" --checks="$_checks" --warnings-as-errors='*' $_srcs || _rc=1
			fi
		else
			echo "static-analysis: clang-tidy is not installed (brew install llvm)"; _rc=1
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
		for _p in linux asan; do
			echo "== linux-build-test: preset $_p"
			echo "NOTE: fuzz.findings.F1/F3/F4 are not run here: the image of Environment A links pristine NTFS-3G, where these replays fail by design (they pass against the patched library in the A-unit asan stage, tests/unit/probe/inner.sh)"
			_o=$work/A/linux-$_p.out
			_prc=0
			sh "$root/vm/linux/run.sh" run -- bash -c \
				"set -e; cmake --preset $_p -S /work -B /tmp/b-$_p -DNTFSMAC_NTFS3G_PATCHED=OFF >/dev/null && cmake --build /tmp/b-$_p && ctest --test-dir /tmp/b-$_p --output-on-failure" \
				>"$_o" 2>&1 || _prc=$?
			cat "$_o"
			if [ "$_prc" -ne 0 ]; then
				_rc=1
				_err=$(grep -m1 'error:' "$_o" | cut -c1-240 || true); [ -n "$_err" ] || _err=$(grep -m1 -E 'FAILED|Failed' "$_o" | cut -c1-240 || true)
				echo "NOTE: linux build or test FAILED with preset $_p: ${_err:-exit $_prc (see the log)}"
			fi
		done
		return "$_rc"
	}

	fuzz_short() {
		container_ready fuzz-short || return 1
		_stamp=$(date -u +%Y%m%dT%H%M%SZ)
		_fo=$root/build/fuzz/ci/$_stamp
		mkdir -p "$_fo"
		_rc=0
		# --skip-security leaves out the security descriptor reads (NTFS-3G engine faults F1, F3 in
		# tests/fuzz/TRIAGE.md); the stage blocks on a finding anywhere else
		sh "$root/tests/fuzz/image/run.sh" --skip-security --seconds "$fuzz_seconds" --out "$_fo/image" || _rc=1
		sh "$root/tests/fuzz/sanitize/run.sh" --seconds "$fuzz_seconds" --out "$_fo/sanitize" || _rc=1
		echo "NOTE: fuzz output (corpus, logs, artifacts) in build/fuzz/ci/$_stamp"
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

	if want bootstrap; then
		_t0=$(date +%s)
		_rc=0
		printf '\n== ci-local stage bootstrap ==\n'
		"$here/bootstrap.sh" || _rc=1
		if [ "$_rc" -eq 0 ]; then _res=ok; else _res=FAIL; fi
		note_result bootstrap "$_res" "$(($(date +%s) - _t0))"
	fi
	if want host; then run_test_all host; fi
	if want tsan; then own_stage host tsan tsan_stage; fi
	if want static-analysis; then own_stage host static-analysis static_analysis; fi
	if want A; then run_test_all A; fi
	if want linux-build-test; then own_stage A linux-build-test linux_build_test; fi
	if want fuzz-short; then own_stage A fuzz-short fuzz_short; fi
	if want package; then own_stage B package package_stage; fi
	if want B; then run_test_all B; fi
	aggregate_own host
	aggregate_own A
	aggregate_own B

	printf '\n== summary ==\n%s' "$summary"
	if [ -n "$result_files" ]; then
		printf '\n== verify-result (HEAD %s) ==\n' "$short"
		require_env=$need_envs
		require_cases=$need_cases
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
