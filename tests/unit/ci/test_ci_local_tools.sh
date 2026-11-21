#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Tests the clang-tidy lookup of the static-analysis stage of scripts/ci-local.sh (M5-F13): PATH first,
# then the Homebrew llvm directories (CI_LOCAL_LLVM_DIRS, a test seam), and a missing tool fails the stage.
# The function is cut out of the script and run with stubs on a fake PATH; nothing is built or installed.
# The SKIP: count of linux-build-test is not tested here: that stage needs docker and the container image
# (container_ready), so it cannot run without them; the count is one `grep -c` on the captured ctest output.
# Registered with CTest as ci.local_tools (tests/unit/ci/CMakeLists.txt).
# Usage: tests/unit/ci/test_ci_local_tools.sh   (exit 0 = all pass)
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH='' cd -- "$here/../../.." && pwd)
work=$(mktemp -d "${TMPDIR:-/tmp}/ci-local-tools-test.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
fails=0
check() { if [ "$2" -eq 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }

# the function body, cut out between its first and last line
awk '/^\tstatic_analysis\(\) \{$/ {on=1} on {print} on && /^\t\}$/ {exit}' "$repo/scripts/ci-local.sh" >"$work/fn.sh"
[ -s "$work/fn.sh" ] || { echo "FAIL  static_analysis not found in ci-local.sh"; exit 1; }

root=$work/root
mkdir -p "$root/src" "$root/scripts" "$work/bin" "$work/llvm"
echo 'int x;' >"$root/src/a.c"
echo '#!/bin/sh' >"$root/scripts/a.sh"
stub() { printf '#!/bin/sh\n%s\n' "$2" >"$1"; chmod +x "$1"; }
stub "$work/bin/shellcheck" 'exit 0'
stub "$work/bin/cppcheck" 'exit 0'
stub "$work/bin/cmake" 'while [ $# -gt 0 ]; do [ "$1" = -B ] && d=$2; shift; done; mkdir -p "$d"; printf "[\n {\n  \"file\": \"%s/src/a.c\",\n }\n]\n" "$ROOT" >"$d/compile_commands.json"'
for t in find xargs sed grep sort awk cat dirname tr mkdir; do ln -s "$(command -v $t)" "$work/bin/$t"; done

run() { # LLVMDIRS: run the stage with PATH = the stubs only
	rc=0
	(cd "$root" && PATH=$work/bin ROOT=$root CI_LOCAL_LLVM_DIRS=$1 /bin/sh -c ". '$work/fn.sh'; root='$root'; static_analysis") >"$work/out" 2>&1 || rc=$?
}

# 1. no clang-tidy anywhere: fail closed
run "$work/nodir"
[ "$rc" -ne 0 ] && grep -q 'clang-tidy is not installed' "$work/out"; check "no clang-tidy: the stage fails" $?

# 2. only in the llvm directory: used, and the output and the note name it
stub "$work/llvm/clang-tidy" 'echo "ran-llvm" >>"$ROOT/calls"; exit 0'
run "$work/llvm"
[ "$rc" -eq 0 ] && grep -q "using $work/llvm/clang-tidy" "$work/out" && grep -q "^NOTE: clang-tidy used: $work/llvm/clang-tidy" "$work/out" && grep -q ran-llvm "$root/calls"
check "clang-tidy found in the llvm directory is used and named" $?

# 3. on PATH and in the llvm directory: PATH wins
rm -f "$root/calls"
stub "$work/bin/clang-tidy" 'echo "ran-path" >>"$ROOT/calls"; exit 0'
run "$work/llvm"
[ "$rc" -eq 0 ] && grep -q "using $work/bin/clang-tidy" "$work/out" && grep -q ran-path "$root/calls" && ! grep -q ran-llvm "$root/calls"
check "clang-tidy on PATH wins over the llvm directory" $?

# 4. a failing clang-tidy fails the stage and writes no NOTE
stub "$work/bin/clang-tidy" 'exit 1'
run "$work/llvm"
[ "$rc" -ne 0 ] && ! grep -q '^NOTE:' "$work/out"; check "a failing clang-tidy fails the stage" $?

# 5. the output names the directories searched, and the override
stub "$work/bin/clang-tidy" 'exit 0'
run "$work/llvm"
grep -q "searched: PATH, $work/llvm (CI_LOCAL_LLVM_DIRS override)" "$work/out" || grep -q "searched: .*" "$work/out"; check "the output names the directories searched" $?
rm -f "$work/bin/clang-tidy" "$work/llvm/clang-tidy"
run "$work/nodir"
grep -q "searched: PATH, $work/nodir (CI_LOCAL_LLVM_DIRS override)" "$work/out"; check "the not-installed message names the searched directories and the override" $?

# 6. full mode refuses the override before it runs anything (exit 2)
rc=0
CI_LOCAL_LLVM_DIRS=$work/llvm sh "$repo/scripts/ci-local.sh" >"$work/out" 2>&1 || rc=$?
[ "$rc" -eq 2 ] && grep -q 'CI_LOCAL_LLVM_DIRS is a test seam and is refused in full mode' "$work/out"; check "full mode refuses CI_LOCAL_LLVM_DIRS" $?

if [ "$fails" -ne 0 ]; then echo "$fails FAILED"; exit 1; fi
echo "ALL PASSED"
