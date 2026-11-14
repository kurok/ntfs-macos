#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Test for scripts/ci-local.sh. Works on copies of the git-tracked tree in a
# temp dir; the real working tree and build/ are never touched.
# Not registered with CTest: ci-local.sh runs ctest, so a CTest entry would
# recurse into itself. Run it directly.
# Usage: tests/unit/ci/test_ci_local.sh   (exit 0 = all pass)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH= cd -- "$here/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ci-local-test.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

fails=0
check() { # description, condition-exit-code
	if [ "$2" -eq 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}

# copy_tree DEST: copy tracked and not-yet-tracked (non-ignored) files as they are in the working tree.
copy_tree() {
	mkdir -p "$1"
	(cd "$repo" && git ls-files --cached --others --exclude-standard | while IFS= read -r f; do
		[ -e "$f" ] && printf '%s\n' "$f"
	done | tar -cf - -T -) | (cd "$1" && tar -xf -)
}

run() { # dir args...; sets rc, output in $work/out
	_d=$1; shift
	rc=0
	"$_d/scripts/ci-local.sh" "$@" >"$work/out" 2>&1 || rc=$?
}

# 1. usage errors exit 2 (run from the real tree: they never build)
run "$repo" --bogus
check "unknown option exits 2" "$([ $rc -eq 2 ]; echo $?)"
run "$repo"
check "no --quick exits 2" "$([ $rc -eq 2 ] && grep -q 'not implemented' "$work/out"; echo $?)"
run "$repo" --verify-result x
check "--verify-result exits 2" "$([ $rc -eq 2 ] && grep -q 'not implemented' "$work/out"; echo $?)"
run "$repo" --quick --preset
check "missing --preset value exits 2" "$([ $rc -eq 2 ]; echo $?)"

# 2. clean copy passes
copy_tree "$work/clean"
run "$work/clean" --quick --build-dir "$work/clean-build"
check "quick run on a clean copy exits 0" "$([ $rc -eq 0 ]; echo $?)"
check "summary lists all four stages" "$(grep -c '^\(bootstrap\|configure\|build\|ctest\) .* ok ' "$work/out" | grep -qx 4; echo $?)"

# 3. compile error breaks the build -> exit 1
copy_tree "$work/broken"
printf '\nthis is not valid C;\n' >> "$work/broken/src/common/log.c"
run "$work/broken" --quick --build-dir "$work/broken-build"
check "broken build exits 1" "$([ $rc -eq 1 ] && grep -q '^build .* FAIL ' "$work/out"; echo $?)"
check "real tree untouched by the broken-build run" "$(! grep -q 'not valid C' "$repo/src/common/log.c"; echo $?)"

# 4. failing test -> exit 1
copy_tree "$work/failtest"
# make main() return 1 at once (brace is on the line after "int main(void)")
sed '/^int main(void)$/{n;s/^{$/{ if (1) return 1;/;}' "$work/failtest/src/common/test_log.c" > "$work/failtest/t.c" &&
	mv "$work/failtest/t.c" "$work/failtest/src/common/test_log.c"
grep -q 'if (1) return 1;' "$work/failtest/src/common/test_log.c"
run "$work/failtest" --quick --build-dir "$work/failtest-build"
check "failing test exits 1" "$([ $rc -eq 1 ] && grep -q '^ctest .* FAIL ' "$work/out"; echo $?)"

if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; exit 0; fi
echo "$fails FAILED"; exit 1
