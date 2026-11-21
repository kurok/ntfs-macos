#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Test for scripts/ci-local.sh. Works on copies of the git-tracked tree in a
# temp dir; the real working tree and build/ are never touched.
# Registered with CTest (tests/unit/ci/CMakeLists.txt) as ci.local with --usage (the option and exit-code checks,
# and the quick-mode stage propagation with fake cmake and ctest on PATH: a few seconds, no build) and, when
# NTFSMAC_CI_BUILD_TESTS=ON, as ci.local_build without it (two minutes: three real configure-build-ctest runs of
# copies). ci-local.sh --quick runs ctest inside those copies, so the test skips
# there (NTFSMAC_CI_NESTED) instead of recursing into itself.
# Usage: tests/unit/ci/test_ci_local.sh [--usage]   (exit 0 = all pass, 77 = skipped with the reason printed)
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH='' cd -- "$here/../../.." && pwd)
usage_only=0
[ "${1:-}" != --usage ] || usage_only=1
skip() { echo "SKIP: $1"; exit 77; }
[ -z "${NTFSMAC_CI_NESTED:-}" ] || skip "inside a ci-local test run (its ctest would run this test again)"
command -v git >/dev/null 2>&1 || skip "git is not installed"
NTFSMAC_CI_NESTED=1
export NTFSMAC_CI_NESTED
work=$(mktemp -d "${TMPDIR:-/tmp}/ci-local-test.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

fails=0
check() { # description, condition-exit-code
	if [ "$2" -eq 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}

# copy_tree DEST (needs the tracked tree: only the real-build part below, not --usage): copy tracked and not-yet-tracked (non-ignored) files as they are in the working tree.
copy_tree() {
	mkdir -p "$1"
	(cd "$repo" && git ls-files --cached --others --exclude-standard | while IFS= read -r f; do
		[ -e "$f" ] && printf '%s\n' "$f"
	done | tar -cf - -T -) | (cd "$1" && tar -xf -)
	# the harness unit tests (harness.unit) read the git state of the tree they run in
	(cd "$1" && git init -q && git add -A && git -c user.name=t -c user.email=t@example.invalid commit -q -m copy)
}

run() { # dir args...; sets rc, output in $work/out
	_d=$1; shift
	rc=0
	"$_d/scripts/ci-local.sh" "$@" >"$work/out" 2>&1 || rc=$?
}

# 1. usage errors exit 2 (run from the real tree: they never build)
run "$repo" --bogus
if [ $rc -eq 2 ]; then r=0; else r=1; fi; check "unknown option exits 2" "$r"
run "$repo" --dry-run
if [ $rc -eq 0 ] && grep -q 'ci-local full plan' "$work/out"; then r=0; else r=1; fi; check "no --quick selects full mode (--dry-run only prints the plan)" "$r"
run "$repo" --verify-result "$work/missing.json"
if [ $rc -eq 2 ] && grep -q 'cannot read' "$work/out"; then r=0; else r=1; fi; check "--verify-result of a missing file exits 2 (read before HEAD is resolved)" "$r"
# the same without any repository: a copy with no .git, and one whose .git is a dangling gitdir file
for _v in nogit dangling; do
	mkdir -p "$work/$_v/scripts" "$work/$_v/tests"
	cp "$repo/scripts/ci-local.sh" "$work/$_v/scripts/"
	cp -R "$repo/tests/harness" "$work/$_v/tests/harness"
done
printf 'gitdir: /nonexistent/host/path/.git/worktrees/x\n' >"$work/dangling/.git"
for _v in nogit dangling; do
	run "$work/$_v" --verify-result "$work/missing.json"
	if [ $rc -eq 2 ] && grep -q 'cannot read' "$work/out"; then r=0; else r=1; sed 's/^/    | /' "$work/out" | head -5; fi; check "--verify-result of a missing file exits 2 in a copy with $_v" "$r"
done
run "$repo" --quick --preset
if [ $rc -eq 2 ]; then r=0; else r=1; fi; check "missing --preset value exits 2" "$r"

# 1b. quick mode: each failing stage fails the run (fake cmake and ctest on PATH, no build). A quick run that
# ignored a failed stage would pass here.
mkdir -p "$work/q/scripts" "$work/fakebin"
cp "$repo/scripts/ci-local.sh" "$work/q/scripts/"
printf '#!/bin/sh\n[ ! -e "$FAKE_DIR/boot-fail" ] || exit 1\nexit 0\n' >"$work/q/scripts/bootstrap.sh"
cat >"$work/fakebin/cmake" <<'FAKE'
#!/bin/sh
echo "cmake $*" >>"$FAKE_DIR/calls"
case " $* " in
*" --target "*) st=product ;;
*" --build "*) st=build ;;
*) st=configure ;;
esac
[ "$st" != "${FAKE_FAIL:-}" ] || { echo "fake cmake: $st fails"; exit 1; }
exit 0
FAKE
cat >"$work/fakebin/ctest" <<'FAKE'
#!/bin/sh
echo "ctest $*" >>"$FAKE_DIR/calls"
[ "ctest" != "${FAKE_FAIL:-}" ] || { echo "fake ctest: tests fail"; exit 1; }
exit 0
FAKE
chmod +x "$work/q/scripts/"*.sh "$work/fakebin/cmake" "$work/fakebin/ctest"
FAKE_DIR=$work
export FAKE_DIR
quick() { # FAIL-STAGE ("" = none)
	rc=0
	: >"$work/calls"
	PATH="$work/fakebin:$PATH" FAKE_FAIL="$1" "$work/q/scripts/ci-local.sh" --quick --build-dir "$work/q-build" >"$work/out" 2>&1 || rc=$?
}
quick ""
if [ $rc -eq 0 ] && grep -q 'ci-local --quick: PASSED' "$work/out" && [ "$(grep -c '^ctest ' "$work/calls")" -eq 1 ]; then r=0; else r=1; tail -12 "$work/out"; fi
check "quick mode with passing stages exits 0 and runs all stages (fake cmake and ctest)" "$r"
for st in configure build product ctest; do
	quick "$st"
	if [ $rc -eq 1 ] && grep -q "^$st  *FAIL " "$work/out" && grep -q 'ci-local --quick: FAILED' "$work/out"; then r=0; else r=1; tail -12 "$work/out"; fi
	check "quick mode: a failing $st stage exits 1" "$r"
	# no later stage ran: ctest is the last stage, so a failure before it must leave no ctest call
	if [ "$st" = ctest ] || ! grep -q '^ctest ' "$work/calls"; then r=0; else r=1; fi
	check "quick mode: after a failing $st stage no later stage ran" "$r"
done
touch "$work/boot-fail"
quick ""
if [ $rc -eq 1 ] && grep -q '^bootstrap  *FAIL ' "$work/out" && [ ! -s "$work/calls" ]; then r=0; else r=1; tail -12 "$work/out"; fi
check "quick mode: a failing bootstrap exits 1 and stops before cmake" "$r"
rm -f "$work/boot-fail"

if [ "$usage_only" -eq 1 ]; then
	if [ "$fails" -eq 0 ]; then echo "ALL PASSED (--usage: the build runs of the copies were not run)"; exit 0; fi
	echo "$fails FAILED"; exit 1
fi

# the real-build part copies the tracked tree: it needs the .git of the source tree
[ -e "$repo/.git" ] || skip "no .git in $repo (the real-build part copies the tracked tree)"

# 2. clean copy passes
copy_tree "$work/clean"
run "$work/clean" --quick --build-dir "$work/clean-build"
if [ $rc -eq 0 ]; then r=0; else r=1; tail -30 "$work/out"; fi; check "quick run on a clean copy exits 0" "$r"
if grep -c '^\(bootstrap\|configure\|build\|ctest\) .* ok ' "$work/out" | grep -qx 4; then r=0; else r=1; fi; check "summary lists all four stages" "$r"

# 3. compile error breaks the build -> exit 1
copy_tree "$work/broken"
printf '\nthis is not valid C;\n' >> "$work/broken/src/common/log.c"
run "$work/broken" --quick --build-dir "$work/broken-build"
if [ $rc -eq 1 ] && grep -q '^build .* FAIL ' "$work/out"; then r=0; else r=1; fi; check "broken build exits 1" "$r"
if ! grep -q 'not valid C' "$repo/src/common/log.c"; then r=0; else r=1; fi; check "real tree untouched by the broken-build run" "$r"

# 4. failing test -> exit 1
copy_tree "$work/failtest"
# make main() return 1 at once (brace is on the line after "int main(void)")
sed '/^int main(void)$/{n;s/^{$/{ if (1) return 1;/;}' "$work/failtest/src/common/test_log.c" > "$work/failtest/t.c" &&
	mv "$work/failtest/t.c" "$work/failtest/src/common/test_log.c"
grep -q 'if (1) return 1;' "$work/failtest/src/common/test_log.c"
run "$work/failtest" --quick --build-dir "$work/failtest-build"
if [ $rc -eq 1 ] && grep -q '^ctest .* FAIL ' "$work/out"; then r=0; else r=1; fi; check "failing test exits 1" "$r"

if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; exit 0; fi
echo "$fails FAILED"; exit 1
