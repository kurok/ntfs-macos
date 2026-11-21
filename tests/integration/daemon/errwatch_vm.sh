#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-13: run the session channel watcher against the real worker in a disposable macOS guest
# (Environment B). Covers U16 (the descriptor table of a live worker), I-DMN-24 (a) with a
# faultio worker and (b) without the channel, and the happy path, at module level. The daemon
# versions of I-DMN-24 (c) to (f) need the wired daemon (T-M4-08, T-M4-14).
#
# Like tests/integration/daemon/spawn_vm.sh: clone a tart VM from the golden VM, boot it, build
# NTFS-3G twice in the guest (release, and with --enable-faultio in another prefix), copy the
# sources of the modules and the driver, build the driver there with cc, make an NTFS image file
# in the guest, run the driver as root, copy the output back, and always delete the VM.
# Nothing touches a host disk. Every mount is of an image file inside the guest, with norecover.
#
# Usage: tests/integration/daemon/errwatch_vm.sh [--out FILE] [CASE]
#   CASE  one of clean, nochan, fault (default: all)
# Exit:  the driver's exit code (0 all checks passed), 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-errwatch-$$
out=
case_name=

while [ $# -gt 0 ]; do
	case "$1" in
	--out) [ $# -ge 2 ] || { echo "errwatch_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	-*) echo "errwatch_vm: unknown option $1" >&2; exit 2 ;;
	*) case_name=$1; shift ;;
	esac
done

cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "errwatch_vm: WARNING: could not delete $name" >&2
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "errwatch_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { echo "errwatch_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

echo "errwatch_vm: building NTFS-3G (release and faultio) in the guest" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball") |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
	{ echo "errwatch_vm: guest build failed" >&2; exit 2; }
if [ -z "$case_name" ] || [ "$case_name" = fault ]; then
	"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps-fio --build-dir ~/ntfsmac/build-fio --configure-arg --enable-faultio 2>&1' >&2 ||
		{ echo "errwatch_vm: guest faultio build failed" >&2; exit 2; }
fi

echo "errwatch_vm: copying the sources of the modules and the driver" >&2
(cd "$root" && tar -cf - include src/daemon/spawn.c src/daemon/spawn_config.h src/daemon/internal.h \
	src/daemon/spawn_holder.c src/daemon/spawn_holder.h src/daemon/usermap.c src/daemon/usermap.h \
	src/daemon/usermap_file.c src/daemon/usermap_file.h src/daemon/status.c src/daemon/errwatch.c src/daemon/errwatch_io.c \
	src/options src/sanitize \
	src/common/label_cp.c src/common/label_cp_data.h src/common/log.c src/policy tests/integration/daemon/errwatch_vm.c) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/errwatch && mkdir -p ~/ntfsmac/errwatch && tar -xf - -C ~/ntfsmac/errwatch'

# The guest script: build, make the image, run. Quoted so that nothing expands on the host.
guest='set -eu
d=$HOME/ntfsmac/errwatch
work=$(mktemp -d /tmp/errwatchvm.XXXXXX)
chmod 755 "$work" # the console user must be able to run the wrapper; the image stays mode 000
cd "$d"
cc -std=c17 -Wall -Wextra -Werror -O2 -DNTFSMAC_TEST_SEAMS -I include -I src/daemon -I src/sanitize \
	-o "$work/errwatch_vm" tests/integration/daemon/errwatch_vm.c src/daemon/spawn.c \
	src/daemon/spawn_holder.c src/daemon/usermap.c src/daemon/usermap_file.c src/daemon/status.c \
	src/daemon/errwatch.c src/daemon/errwatch_io.c \
	src/options/options.c src/options/argv_check.c src/sanitize/label.c src/sanitize/display.c \
	src/sanitize/leaf.c src/sanitize/verdict.c src/common/label_cp.c src/common/log.c src/policy/policy.c
cp "$HOME/ntfsmac/deps/bin/ntfs-3g" "$work/ntfsmac-worker"
if [ -x "$HOME/ntfsmac/deps-fio/bin/ntfs-3g" ]; then
	cp "$HOME/ntfsmac/deps-fio/bin/ntfs-3g" "$work/ntfsmac-worker-fio"
fi
# The faultio worker is started through this script, which reads the plan from the work directory.
cat > "$work/worker-fault" <<EOS
#!/bin/sh
NTFSMAC_FAULT_PLAN=\$(cat "$work/plan")
NTFSMAC_FAULT_STATE=$work/fault-state
export NTFSMAC_FAULT_PLAN NTFSMAC_FAULT_STATE
exec "$work/ntfsmac-worker-fio" "\$@"
EOS
chmod 755 "$work/worker-fault"
: > "$work/fault-state"
chmod 666 "$work/fault-state"
img=$work/ntfs.img
truncate -s 64m "$img"
"$HOME/ntfsmac/deps/sbin/mkntfs" -F -Q -L errwatchvm "$img" >/dev/null 2>&1 || { echo "mkntfs failed"; exit 2; }
chmod 000 "$img"
echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u) gid $(id -g)"
rc=0
sudo -n "$work/errwatch_vm" "$img" "$work/ntfsmac-worker" "$work/worker-fault" "$(id -u)" "$(id -g)" "$work" ${CASE:-} || rc=$?
sudo -n /usr/bin/pkill -9 -f "$work/" >/dev/null 2>&1 || true
chmod 600 "$img"; rm -rf "$work"
exit $rc'

rc=0
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "CASE=$case_name; export CASE; $guest" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "CASE=$case_name; export CASE; $guest" || rc=$?
fi
exit "$rc"
