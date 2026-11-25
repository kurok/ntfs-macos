#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The plan of the `guest-build` stage (tests/unit/ci/guest_build_plan.py, vm/macos/guest-build.sh): it must be made from every
# VM script of check_guest_sources.py, the guest script must parse, hold one cc line for every line the scripts
# build, put the libntfs-3g and fuse flags and libraries from pkg-config around the lines that use them, and
# copy each script's files into its own directory. No guest is needed.
# Usage: test_guest_plan.sh REPO
set -eu
repo=${1:?usage: test_guest_plan.sh REPO}
here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
py=${PYTHON:-python3}
t=$(mktemp -d "${TMPDIR:-/tmp}/test_guest_plan.XXXXXX")
trap 'rm -rf "$t"' EXIT
fails=0
ok() { printf 'PASS  %s\n' "$1"; }
bad() { printf 'FAIL  %s\n' "$1"; fails=$((fails + 1)); }

"$py" -I -B "$here/guest_build_plan.py" "$repo" "$t/out" >"$t/log" 2>&1 && ok "the plan is made for every VM script" || { cat "$t/log"; bad "the plan is made"; }
b=$t/out/pack/build.sh
[ -f "$b" ] || { bad "build.sh exists"; exit 1; }
sh -n "$b" && ok "build.sh parses" || bad "build.sh parses"

# one directory per script, with its copy list
for s in spawn_vm sandbox_vm errwatch_vm journal_vm dmn_mirror_vm xpc_vm; do
	[ -d "$t/out/pack/$s" ] && ok "pack/$s exists" || bad "pack/$s exists"
done
[ -f "$t/out/pack/journal_vm/src/daemon/spawn.c" ] && ok "the copy list of journal_vm is unpacked" || bad "the copy list of journal_vm is unpacked"

# every cc line the scripts have is in build.sh: the count of the parser equals the count of lines
n_plan=$(grep -c '^cc ' "$b")
n_parse=$("$py" -I -B -c '
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, sys.argv[1])
import check_guest_sources as gs
n = 0
for sc in gs.DEFAULT_SCRIPTS:
    with open(sys.argv[2] + "/" + sc) as f:
        n += len(gs.parse(f.read())[2])
print(n)' "$here" "$repo")
[ "$n_plan" -eq "$n_parse" ] && [ "$n_plan" -gt 0 ] && ok "build.sh has all $n_plan cc lines" || bad "build.sh has $n_plan cc lines, the scripts $n_parse"

# the library flags of the lines that use them
grep -q "^cc .*\$ntfs .*\$nlibs ||" "$b" && ok "the lines with \$ntfs and \$nlibs keep them" || bad "the lines with \$ntfs and \$nlibs keep them"
grep -q "^cc .* \$(pkg-config --cflags fuse) .* \$(pkg-config --libs fuse) ||" "$b" && ok "the FUSE stand-in line keeps its pkg-config words" || bad "the FUSE stand-in line keeps its pkg-config words"
grep -q 'PKG_CONFIG_PATH=\$HOME/ntfsmac/deps/lib/pkgconfig' "$b" && ok "pkg-config looks at the guest's deps" || bad "pkg-config looks at the guest's deps"
# a failing line is reported and the script exits 1 after the others ran
grep -q 'rc=1; }' "$b" && grep -q '^exit \$rc' "$b" && ok "a failing line is reported, the others still run" || bad "a failing line is reported, the others still run"

[ "$fails" -eq 0 ] || { echo "$fails failed"; exit 1; }
echo "guest plan: all passed"
