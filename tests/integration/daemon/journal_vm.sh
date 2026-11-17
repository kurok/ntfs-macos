#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-14: run the session lifecycle and the privileged operations against the real probe,
# volmark and worker in a disposable macOS guest (Environment B). Covers I-DMN-22 (marker flow),
# I-DMN-25 a and c (binding), I-DMN-20 (evidence rule), and the module level of I-DMN-19 and
# I-DMN-21 (journal-clear and repair --clear-dirty on real probe verdicts). Not covered here:
# I-DMN-25 b with the real Disk Arbitration (needs the wired daemon, T-M4-11), the peer checks
# over XPC and the administrator password step of I-DMN-19 (U17: a person types the password;
# `tests/unit/ctl/vm/u17.sh` parts B3 and B4).
#
# Like tests/integration/daemon/sandbox_vm.sh: clone a tart VM from the golden VM, boot it,
# build NTFS-3G in the guest, copy the sources, build the driver, the real probe and volmark and
# the image tools with cc there, make NTFS image files in the guest, run the driver as root with
# the sandbox profiles in a root-owned directory, copy the output back, always delete the VM.
# Nothing touches a host disk. Every mount is of an image file inside the guest, with norecover.
#
# Usage: tests/integration/daemon/journal_vm.sh [--out FILE] [CASE]
#   CASE  one of rw, unclean, markfail, bind, evidence, clear, repair (default: all)
# Exit:  the driver's exit code (0 all checks passed), 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-journal-$$
out=
case_name=

while [ $# -gt 0 ]; do
	case "$1" in
	--out) [ $# -ge 2 ] || { echo "journal_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	-*) echo "journal_vm: unknown option $1" >&2; exit 2 ;;
	*) case_name=$1; shift ;;
	esac
done
case "$case_name" in
'' | rw | unclean | markfail | bind | evidence | clear | repair) ;;
*) echo "journal_vm: unknown case $case_name" >&2; exit 2 ;;
esac

cleanup() {
	"$tart_sh" delete "$name" >&2 || echo "journal_vm: WARNING: could not delete $name" >&2
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "journal_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { echo "journal_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

echo "journal_vm: building NTFS-3G in the guest" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball") |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
	{ echo "journal_vm: guest build failed" >&2; exit 2; }

echo "journal_vm: copying the sources" >&2
(cd "$root" && tar -cf - include integration/sandbox src/daemon src/journal src/common src/options \
	src/sanitize src/policy src/probe src/volmark tests/integration/daemon tests/tools) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/jvm && mkdir -p ~/ntfsmac/jvm && tar -xf - -C ~/ntfsmac/jvm'

# The guest script: build, make the images, run. Quoted so that nothing expands on the host.
guest='set -eu
d=$HOME/ntfsmac/jvm
export PKG_CONFIG_PATH=$HOME/ntfsmac/deps/lib/pkgconfig
work=$(cd -P "$(mktemp -d /tmp/journalvm.XXXXXX)" && pwd) # the real path: the profiles name programs by it
chmod 755 "$work" # `nobody` must be able to run the programs; the images stay mode 600, owned by root
cleanup() {
	sudo -n /usr/bin/pkill -9 -f "$work/" >/dev/null 2>&1 || true
	sudo -n rm -rf "$work"
}
trap cleanup EXIT
cd "$d"
ntfs="$(pkg-config --cflags libntfs-3g)"
nlibs="$(pkg-config --libs --static libntfs-3g)"
cc -std=c17 -Wall -Wextra -Werror -O2 -DNTFSMAC_TEST_SEAMS -I include -I src/daemon -I src/sanitize -I src/journal \
	-o "$work/journal_vm" tests/integration/daemon/journal_vm.c src/daemon/spawn.c src/daemon/status.c \
	src/daemon/errwatch.c src/daemon/errwatch_io.c src/daemon/session.c src/daemon/ctl_journal.c src/daemon/ctl_repair_mirror.c \
	src/daemon/ctl_parse.c src/daemon/da.c src/daemon/da_filter.c src/journal/journal.c src/common/log.c \
	src/options/options.c src/options/argv_check.c src/sanitize/label.c src/sanitize/display.c \
	src/sanitize/leaf.c src/sanitize/verdict.c src/common/label_cp.c src/policy/policy.c src/probe/state.c \
	src/common/state_rank.c -framework DiskArbitration -framework CoreFoundation
cc -std=c17 -Wall -Wextra -Werror -O2 -o "$work/jstub" tests/integration/daemon/journal_stub.c
mkdir "$work/stubs"
for k in exit1 sector0 hang; do ln "$work/jstub" "$work/stubs/jstub-$k"; done
cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -o "$work/ntfsmac-probe" \
	src/probe/main.c src/probe/cli.c src/probe/probe.c src/probe/devio.c src/probe/names.c src/probe/nfc.c \
	src/probe/state.c src/probe/text.c src/probe/json.c src/common/mftmirr.c src/common/state_rank.c \
	$nlibs -framework CoreFoundation
cc -std=c17 -O2 -I include -I src/volmark -I src/common $ntfs -o "$work/ntfsmac-volmark" \
	src/volmark/main.c src/volmark/cli.c src/volmark/volmark.c src/volmark/syncmirror.c src/volmark/vdev.c src/volmark/args.c \
	src/common/mftmirr.c $nlibs -framework CoreFoundation
for t in setvolflags logfile_mark; do
	cc -std=c17 -O2 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE=1 $ntfs -o "$work/$t" tests/tools/$t.c $nlibs -framework CoreFoundation
done
cp "$HOME/ntfsmac/deps/bin/ntfs-3g" "$work/ntfsmac-worker"
mk() { # mk FILE LABEL
	truncate -s 64m "$1"
	"$HOME/ntfsmac/deps/sbin/mkntfs" -F -Q -L "$2" "$1" >/dev/null 2>&1 || { echo "mkntfs failed"; exit 2; }
	"$work/logfile_mark" "$1" --init >/dev/null || { echo "logfile_mark --init failed"; exit 2; }
	chmod 600 "$1"
}
mk "$work/base.img" jvmvol
mk "$work/img2.img" jvmvol2

# The profiles go into a root-owned directory, as the spawn module requires.
sbx=$work/sandbox
sudo -n install -d -m 755 -o root -g wheel "$sbx"
sudo -n install -m 644 -o root -g wheel "$d"/integration/sandbox/*.sb "$sbx/"

echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u) gid $(id -g)"
rc=0
sudo -n "$work/journal_vm" "$work" "$(id -u)" "$(id -g)" "$sbx" "$work/ntfsmac-probe" "$work/ntfsmac-volmark" \
	"$work/ntfsmac-worker" "$work/stubs" "$work/setvolflags" "$work/logfile_mark" ${CASE:-} || rc=$?
echo "== worker log tail"
tail -n 12 "$work/worker.log" 2>/dev/null | sed "s/^/  log: /" || true
exit $rc'

rc=0
if [ -n "$out" ]; then
	"$tart_sh" exec "$name" -- "CASE=$case_name; export CASE; $guest" >"$out" 2>&1 || rc=$?
	cat "$out"
else
	"$tart_sh" exec "$name" -- "CASE=$case_name; export CASE; $guest" || rc=$?
fi
exit "$rc"
