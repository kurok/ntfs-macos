#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-06: test the sandbox profiles of integration/sandbox/ in a disposable macOS guest
# (Environment B). Covers I-SBX-01 to I-SBX-12 (docs/TEST_STRATEGY.md section 7).
#
# Like tests/integration/daemon/spawn_vm.sh: clone a tart VM from the golden VM, boot it, build
# NTFS-3G in the guest, copy the sources, build the drivers and the real probe and volmark there
# with cc, make an NTFS image file in the guest, run the drivers as root, copy the output back,
# and always delete the VM. Nothing touches a host disk. Every mount is of an image file inside
# the guest, with norecover.
#
# The case fuse2 starts a program that serves a second file system with libfuse (sandbox_fuse.c),
# on a directory in the work directory, and takes the mount away again with a forced unmount.
#
# In the guest the script also registers a launchd job that owns the Mach service of the
# privileged requests (the name of ctl_proto.h), so that the look-up is refused by the profile
# and not by a missing service, and removes it at the end. sandbox_vm.c makes the control
# socket, a TCP listener and the UserMapping files and removes them.
#
# Usage: tests/integration/daemon/sandbox_vm.sh [--out FILE] [CASE]
#   CASE  probe, volmark, worker, worker-um, desc, desc-broad, tree, broad, fail, fuse2
#                                                   (sandbox_vm: I-SBX-01 to I-SBX-05 and I-SBX-08 to I-SBX-12)
#         rw, ro, priv                              (spawn_vm --sandbox: I-SBX-06, the real worker
#                                                    mounts rw and ro and the stand-ins start)
#         real                                      (real probe and volmark: I-SBX-07)
#         default: all of them
# Exit:  0 all checks passed, 1 a check failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-sandbox-$$
out=
case_name=

while [ $# -gt 0 ]; do
	case "$1" in
	--out) [ $# -ge 2 ] || { echo "sandbox_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	-*) echo "sandbox_vm: unknown option $1" >&2; exit 2 ;;
	*) case_name=$1; shift ;;
	esac
done
case "$case_name" in
'' | probe | volmark | worker | worker-um | desc | desc-broad | tree | broad | fail | fuse2 | rw | ro | priv | real) ;;
*) echo "sandbox_vm: unknown case $case_name" >&2; exit 2 ;;
esac

cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "sandbox_vm: WARNING: could not delete $name" >&2
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "sandbox_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { echo "sandbox_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }
service=$(sed -n 's/^#define NTFSMAC_CTL_XPC_SERVICE "\(.*\)"$/\1/p' "$root/include/ntfsmac/ctl_proto.h")
[ -n "$service" ] || { echo "sandbox_vm: no NTFSMAC_CTL_XPC_SERVICE in include/ntfsmac/ctl_proto.h" >&2; exit 2; }

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

echo "sandbox_vm: building NTFS-3G in the guest" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball") |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
	{ echo "sandbox_vm: guest build failed" >&2; exit 2; }

echo "sandbox_vm: copying the sources" >&2
(cd "$root" && tar -cf - include integration/sandbox src/daemon/spawn.c src/daemon/spawn_config.h \
	src/daemon/internal.h src/daemon/status.c src/options src/sanitize src/common src/policy \
	src/probe src/volmark tests/integration/daemon tests/unit/spawn/spawn_stub.c) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/sbx && mkdir -p ~/ntfsmac/sbx && tar -xf - -C ~/ntfsmac/sbx'

# The guest script: build, make the image, register the Mach service, run. Quoted so that
# nothing expands on the host.
guest='set -eu
d=$HOME/ntfsmac/sbx
export PKG_CONFIG_PATH=$HOME/ntfsmac/deps/lib/pkgconfig
work=$(cd -P "$(mktemp -d /tmp/sandboxvm.XXXXXX)" && pwd) # the real path: the profiles name programs by it
chmod 755 "$work" # `nobody` must be able to run the stand-ins; the image stays mode 000
label=org.ntfs-macos.sandbox-test
cleanup() {
	sudo -n /bin/launchctl bootout system/$label >/dev/null 2>&1 || true
	sudo -n /usr/bin/pkill -9 -f "$work/" >/dev/null 2>&1 || true
	sudo -n rm -rf "$work"
}
trap cleanup EXIT
cd "$d"
cflags="-std=c17 -Wall -Wextra -Werror -O2 -DNTFSMAC_TEST_SEAMS -I include -I src/daemon -I src/sanitize -I tests/integration/daemon"
cc $cflags -o "$work/spawn_vm" tests/integration/daemon/spawn_vm.c src/daemon/spawn.c src/daemon/status.c \
	src/options/options.c src/options/argv_check.c src/sanitize/label.c src/sanitize/display.c \
	src/sanitize/leaf.c src/sanitize/verdict.c src/common/label_cp.c src/policy/policy.c
cc $cflags -o "$work/sandbox_vm" tests/integration/daemon/sandbox_vm.c src/daemon/spawn.c src/daemon/status.c \
	src/options/options.c src/options/argv_check.c src/sanitize/label.c src/sanitize/display.c \
	src/sanitize/leaf.c src/sanitize/verdict.c src/common/label_cp.c src/policy/policy.c src/probe/state.c \
	src/common/state_rank.c
cc $cflags -o "$work/sandbox_stub" tests/integration/daemon/sandbox_stub.c
cc -std=c17 -Wall -Wextra -Werror -O2 $(pkg-config --cflags fuse) -o "$work/sandbox_fuse" tests/integration/daemon/sandbox_fuse.c $(pkg-config --libs fuse)
cc -std=c17 -Wall -Wextra -Werror -O2 -o "$work/spawn_stub" tests/unit/spawn/spawn_stub.c
cc -std=c17 -O2 -I include -I src/probe -I src/common $(pkg-config --cflags libntfs-3g) -o "$work/ntfsmac-probe" \
	src/probe/main.c src/probe/cli.c src/probe/probe.c src/probe/devio.c src/probe/names.c src/probe/nfc.c \
	src/probe/state.c src/probe/text.c src/probe/json.c src/common/mftmirr.c src/common/state_rank.c \
	$(pkg-config --libs --static libntfs-3g) -framework CoreFoundation
cc -std=c17 -O2 -I include -I src/volmark -I src/common $(pkg-config --cflags libntfs-3g) -o "$work/ntfsmac-volmark" \
	src/volmark/main.c src/volmark/cli.c src/volmark/volmark.c src/volmark/vdev.c src/volmark/args.c \
	src/common/mftmirr.c $(pkg-config --libs --static libntfs-3g) -framework CoreFoundation
cp "$HOME/ntfsmac/deps/bin/ntfs-3g" "$work/ntfsmac-worker"
img=$work/ntfs.img
truncate -s 64m "$img"
"$HOME/ntfsmac/deps/sbin/mkntfs" -F -Q -L sandboxvm "$img" >/dev/null 2>&1 || { echo "mkntfs failed"; exit 2; }
chmod 000 "$img"

# The profiles go into a root-owned directory, as the spawn module requires.
sbx=$work/sandbox
sudo -n install -d -m 755 -o root -g wheel "$sbx"
sudo -n install -m 644 -o root -g wheel "$d"/integration/sandbox/*.sb "$sbx/"

# A launchd job that owns the Mach service; nothing starts it, the name only has to resolve.
plist=$work/$label.plist
printf "%s\n" "<?xml version=\"1.0\" encoding=\"UTF-8\"?>" \
	"<!DOCTYPE plist PUBLIC \"-//Apple//DTD PLIST 1.0//EN\" \"http://www.apple.com/DTDs/PropertyList-1.0.dtd\">" \
	"<plist version=\"1.0\"><dict>" \
	"<key>Label</key><string>$label</string>" \
	"<key>ProgramArguments</key><array><string>/usr/bin/true</string></array>" \
	"<key>MachServices</key><dict><key>$SERVICE</key><true/></dict>" \
	"</dict></plist>" > "$work/plist.tmp"
sudo -n install -m 644 -o root -g wheel "$work/plist.tmp" "$plist"
sudo -n /bin/launchctl bootstrap system "$plist"

echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u) gid $(id -g)"
uid=$(id -u); gid=$(id -g)
rc=0
run_sbx() {
	sudo -n "$work/sandbox_vm" "$sbx" "$work/sandbox_stub" "$uid" "$gid" "$work" "$@" || rc=$?
}
run_spawn() {
	sudo -n "$work/spawn_vm" --sandbox "$sbx" "$img" "$work/ntfsmac-worker" "$work/spawn_stub" "$uid" "$gid" "$work" "$1" || rc=$?
}
case "${CASE:-}" in
"") run_sbx; run_sbx fuse2 "$work/sandbox_fuse"; run_spawn priv; run_spawn rw; run_spawn ro; run_sbx real "$work/ntfsmac-probe" "$work/ntfsmac-volmark" "$img" ;;
probe | volmark | worker | worker-um | desc | desc-broad | tree | broad | fail) run_sbx "$CASE" ;;
fuse2) run_sbx fuse2 "$work/sandbox_fuse" ;;
rw | ro | priv) run_spawn "$CASE" ;;
real) run_sbx real "$work/ntfsmac-probe" "$work/ntfsmac-volmark" "$img" ;;
esac
exit $rc'

rc=0
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "CASE=$case_name; SERVICE=$service; export CASE SERVICE; $guest" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "CASE=$case_name; SERVICE=$service; export CASE SERVICE; $guest" || rc=$?
fi
exit "$rc"
