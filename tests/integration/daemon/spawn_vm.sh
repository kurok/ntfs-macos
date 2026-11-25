#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-01: run the spawn module against the real worker in a disposable macOS guest
# (Environment B). Covers U6, U20, I-DMN-09 and I-DMN-26 (b and c) at module level; the
# full daemon versions of I-DMN-09 and I-DMN-26 need the wired daemon (T-M4-08).
#
# Like tests/harness/run-env-b.sh: clone a tart VM from the golden VM, boot it, build NTFS-3G
# in the guest (scripts/build-ntfs3g.sh, with the patch series), copy the sources of the
# module and its driver, build the driver there with cc, make an NTFS image file in the guest,
# run the driver as root, copy the output back, and always delete the VM.
# Nothing touches a host disk. Every mount is of an image file inside the guest, with norecover.
#
# With --sandbox the three children run under the profiles of integration/sandbox/ (T-M4-06),
# copied into a root-owned directory of the guest as the spawn module requires. The default
# cases are then priv, rw, ro and u20: stop and nofskit start a shell wrapper, which the worker
# profile does not allow to exec. The sandbox tests proper are in sandbox_vm.sh.
#
# Usage: tests/integration/daemon/spawn_vm.sh [--out FILE] [--guest NAME] [--sandbox] [CASE]
#   CASE  one of priv, rw, ro, u20, stop, nofskit (default: all)
#   --guest NAME  run in that running guest instead of a fresh clone (dmn_all.sh shares one between the
#                 module-level drivers): it is neither cloned nor deleted here, and NTFS-3G is built only when the
#                 guest has no build of its own yet (~/ntfsmac/deps-built names the guest)
# Exit:  the driver's exit code (0 all checks passed), 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-spawn-$$
shared=0 # 1: --guest, a running guest of dmn_all.sh shared with other drivers (not cloned, not deleted here)
out=
case_name=
sandbox=0

while [ $# -gt 0 ]; do
	case "$1" in
	--out) [ $# -ge 2 ] || { echo "spawn_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	--guest) [ $# -ge 2 ] || { echo "spawn_vm: --guest needs a guest name" >&2; exit 2; }; name=$2; shared=1; shift 2 ;;
	--sandbox) sandbox=1; shift ;;
	-*) echo "spawn_vm: unknown option $1" >&2; exit 2 ;;
	*) case_name=$1; shift ;;
	esac
done

cleanup() {
	[ "$shared" -eq 1 ] || "$tart_sh" delete "$name" >&2 || echo "spawn_vm: WARNING: could not delete $name" >&2
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "spawn_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { echo "spawn_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

if [ "$shared" -eq 0 ]; then
	"$tart_sh" clone "$name" --from golden >&2
	"$tart_sh" run "$name" >&2
fi

# In a shared guest the first driver builds NTFS-3G and stamps ~/ntfsmac/deps-built with the guest name; the
# others use that build (the same command, sources and patches; the drivers only read ~/ntfsmac/deps).
if [ "$shared" -eq 1 ] && "$tart_sh" exec "$name" -- "test \"\$(cat ~/ntfsmac/deps-built 2>/dev/null)\" = '$name'" 2>/dev/null; then
	echo "spawn_vm: NTFS-3G is already built in the shared guest $name" >&2
else
	echo "spawn_vm: building NTFS-3G in the guest" >&2
	(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
		third_party/patches "third_party/cache/$tarball") |
		tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
	"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
		{ echo "spawn_vm: guest build failed" >&2; exit 2; }
	[ "$shared" -eq 0 ] || "$tart_sh" exec "$name" -- "echo '$name' >~/ntfsmac/deps-built" >&2
fi

echo "spawn_vm: copying the sources of the module and the driver" >&2
(cd "$root" && tar -cf - include src/daemon/spawn.c src/daemon/spawn_config.h src/daemon/internal.h \
	src/daemon/spawn_holder.c src/daemon/spawn_holder.h src/daemon/usermap.c src/daemon/usermap.h \
	src/daemon/usermap_file.c src/daemon/usermap_file.h src/daemon/status.c \
	src/options src/sanitize src/common/label_cp.c src/common/label_cp_data.h src/common/log.c \
	src/policy integration/sandbox tests/integration/daemon/spawn_vm.c tests/unit/spawn/spawn_stub.c) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/spawn && mkdir -p ~/ntfsmac/spawn && tar -xf - -C ~/ntfsmac/spawn'

# The guest script: build, make the image, run. Quoted so that nothing expands on the host.
guest='set -eu
d=$HOME/ntfsmac/spawn
work=$(cd -P "$(mktemp -d /tmp/spawnvm.XXXXXX)" && pwd) # the real path: a profile names the worker by it
chmod 755 "$work" # `nobody` must be able to run the stand-ins; the image stays mode 000
cd "$d"
cc -std=c17 -Wall -Wextra -Werror -O2 -DNTFSMAC_TEST_SEAMS -I include -I src/daemon -I src/sanitize \
	-o "$work/spawn_vm" tests/integration/daemon/spawn_vm.c src/daemon/spawn.c \
	src/daemon/spawn_holder.c src/daemon/usermap.c src/daemon/usermap_file.c src/daemon/status.c \
	src/options/options.c src/options/argv_check.c src/sanitize/label.c src/sanitize/display.c \
	src/sanitize/leaf.c src/sanitize/verdict.c src/common/label_cp.c src/common/log.c src/policy/policy.c
cc -std=c17 -Wall -Wextra -Werror -O2 -o "$work/spawn_stub" tests/unit/spawn/spawn_stub.c
cp "$HOME/ntfsmac/deps/bin/ntfs-3g" "$work/ntfsmac-worker"
img=$work/ntfs.img
truncate -s 64m "$img"
"$HOME/ntfsmac/deps/sbin/mkntfs" -F -Q -L spawnvm "$img" >/dev/null 2>&1 || { echo "mkntfs failed"; exit 2; }
chmod 000 "$img"
sbx=
if [ "${SANDBOX:-0}" = 1 ]; then
	sbx=$work/sandbox
	sudo -n install -d -m 755 -o root -g wheel "$sbx"
	sudo -n install -m 644 -o root -g wheel "$d"/integration/sandbox/*.sb "$sbx/"
fi
echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u) gid $(id -g)"
rc=0
cases=${CASE:-}
[ -z "$cases" ] && [ -n "$sbx" ] && cases="priv rw ro u20" # stop and nofskit start a shell wrapper
for c in ${cases:-""}; do
	sudo -n "$work/spawn_vm" ${sbx:+--sandbox "$sbx"} "$img" "$work/ntfsmac-worker" "$work/spawn_stub" "$(id -u)" "$(id -g)" "$work" $c || rc=$?
done
sudo -n /usr/bin/pkill -9 -f "$work/" >/dev/null 2>&1 || true
chmod 600 "$img"; sudo -n rm -rf "$work"
exit $rc'

rc=0
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "CASE=$case_name; SANDBOX=$sandbox; export CASE SANDBOX; $guest" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "CASE=$case_name; SANDBOX=$sandbox; export CASE SANDBOX; $guest" || rc=$?
fi
exit "$rc"
