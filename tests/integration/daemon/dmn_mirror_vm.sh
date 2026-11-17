#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-16: I-DMN-27, `repair --sync-mirror` end to end at module level, against the real probe,
# volmark and sandbox in a disposable macOS guest (Environment B), with the privileged request of
# src/daemon/ctl_repair_mirror.c and the real journal and spawn modules. The images (mirror-split,
# mirror-split-dirty, mirror-newer and a split with an unclean log) are made by Environment A
# (tests/fixtures/gen/mirror.sh and unsafe.sh; nothing is mounted on the host) and copied into the
# guest; the clean volume is made in the guest. Not covered here: Disk Arbitration (a fake
# description, as in journal_vm.sh), the XPC door and the five checks of ADR-019 (the dispatcher
# tests of tests/unit/ctl), and the notification text (T-M4-08).
#
# Like tests/integration/daemon/journal_vm.sh: clone a tart VM from the golden VM, boot it, build
# NTFS-3G in the guest, copy the sources, build the driver and the real probe and volmark with cc
# there, run the driver as root with the sandbox profiles in a root-owned directory, copy the output
# back, always delete the VM. Every device is an image file inside the guest. No mount is made.
#
# ntfsfix audit: a sampler in the guest lists every process whose executable is called ntfsfix during
# the run (there must be none), and the two product binaries are searched for the name.
#
# Usage: tests/integration/daemon/dmn_mirror_vm.sh [--out FILE] [--images DIR] [CASE]
#   CASE          one of refuse, repair, volmark (default: all)
#   --images DIR  a directory with split.img, splitdirty.img, newer.img and splitlog.img (default:
#                 made with Environment A into a temporary directory)
# Exit:  the driver's exit code (0 all checks passed), 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
name=ntfsmac-m416-$$
out=
case_name=
images=
tmpimg=

while [ $# -gt 0 ]; do
	case "$1" in
	--out) [ $# -ge 2 ] || { echo "dmn_mirror_vm: --out needs a file" >&2; exit 2; }; out=$2; shift 2 ;;
	--images) [ $# -ge 2 ] || { echo "dmn_mirror_vm: --images needs a directory" >&2; exit 2; }; images=$2; shift 2 ;;
	-*) echo "dmn_mirror_vm: unknown option $1" >&2; exit 2 ;;
	*) case_name=$1; shift ;;
	esac
done
case "$case_name" in
'' | refuse | repair | volmark) ;;
*) echo "dmn_mirror_vm: unknown case $case_name" >&2; exit 2 ;;
esac

cleanup() {
	if [ -n "${vm_started:-}" ]; then
		"$tart_sh" delete "$name" >&2 || echo "dmn_mirror_vm: WARNING: could not delete $name" >&2
	fi
	[ -z "$tmpimg" ] || rm -rf "$tmpimg"
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "dmn_mirror_vm: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || { echo "dmn_mirror_vm: $root/third_party/cache/$tarball is missing; run scripts/fetch-deps.sh ntfs-3g" >&2; exit 2; }

# The images, from Environment A (a container; nothing is attached or mounted here).
if [ -z "$images" ]; then
	tmpimg=$(mktemp -d "${TMPDIR:-/tmp}/dmn-mirror-img.XXXXXX")
	images=$tmpimg
	echo "dmn_mirror_vm: making the images in Environment A" >&2
	sh "$root/tests/fixtures/gen/mirror.sh" --only mirror-newer --export "$tmpimg/m1" >&2
	sh "$root/tests/fixtures/gen/mirror.sh" --only mirror-split-dirty --export "$tmpimg/m2" >&2
	sh "$root/tests/fixtures/gen/unsafe.sh" --only unclean-log --export "$tmpimg/u" >&2
	cp "$tmpimg/m1/mirror-split.img" "$tmpimg/split.img"
	cp "$tmpimg/m1/mirror-newer.img" "$tmpimg/newer.img"
	cp "$tmpimg/m2/mirror-split-dirty.img" "$tmpimg/splitdirty.img"
	cp "$tmpimg/u/unclean-log.img" "$tmpimg/splitlog.img"
	# the split of record 0 put on the unclean-log image (a raw edit of the image file)
	python3 -I -c 'import sys; sys.path.insert(0, sys.argv[1]); import ntfsrec; ntfsrec.synth_split(sys.argv[2])' \
		"$root/tests/unit/probe" "$tmpimg/splitlog.img"
fi
for f in split splitdirty newer splitlog; do
	[ -f "$images/$f.img" ] || { echo "dmn_mirror_vm: $images/$f.img is missing" >&2; exit 2; }
done

vm_started=1
"$tart_sh" clone "$name" --from golden >&2
"$tart_sh" run "$name" >&2

echo "dmn_mirror_vm: building NTFS-3G in the guest" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball") |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
"$tart_sh" exec "$name" -- 'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1' >&2 ||
	{ echo "dmn_mirror_vm: guest build failed" >&2; exit 2; }

echo "dmn_mirror_vm: copying the sources and the images" >&2
(cd "$root" && tar -cf - include integration/sandbox src/daemon src/journal src/common src/options \
	src/sanitize src/policy src/probe src/volmark tests/integration/daemon tests/tools) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/dmv && mkdir -p ~/ntfsmac/dmv && tar -xf - -C ~/ntfsmac/dmv'
(cd "$images" && tar -czf - split.img splitdirty.img newer.img splitlog.img) |
	tart exec -i "$name" /bin/sh -c 'rm -rf ~/ntfsmac/dmvimg && mkdir -p ~/ntfsmac/dmvimg && tar -xzf - -C ~/ntfsmac/dmvimg'

# The guest script: build, make the clean image, run. Quoted so that nothing expands on the host.
guest='set -eu
d=$HOME/ntfsmac/dmv
export PKG_CONFIG_PATH=$HOME/ntfsmac/deps/lib/pkgconfig
work=$(cd -P "$(mktemp -d /tmp/dmnmirror.XXXXXX)" && pwd) # the real path: the profiles name programs by it
chmod 755 "$work" # `nobody` must be able to run the programs; the images stay mode 600, owned by root
sampler=
cleanup() {
	[ -z "$sampler" ] || { kill "$sampler"; wait "$sampler"; } >/dev/null 2>&1 || true
	sudo -n /usr/bin/pkill -9 -f "$work/" >/dev/null 2>&1 || true
	sudo -n rm -rf "$work"
}
trap cleanup EXIT
cd "$d"
ntfs="$(pkg-config --cflags libntfs-3g)"
nlibs="$(pkg-config --libs --static libntfs-3g)"
cc -std=c17 -Wall -Wextra -Werror -O2 -DNTFSMAC_TEST_SEAMS -I include -I src/daemon -I src/sanitize -I src/journal \
	-o "$work/dmn_mirror_vm" tests/integration/daemon/dmn_mirror_vm.c src/daemon/spawn.c src/daemon/status.c \
	src/daemon/errwatch.c src/daemon/errwatch_io.c src/daemon/session.c src/daemon/ctl_journal.c src/daemon/ctl_repair_mirror.c \
	src/daemon/ctl_parse.c src/daemon/da.c src/daemon/da_filter.c src/journal/journal.c src/common/log.c \
	src/options/options.c src/options/argv_check.c src/sanitize/label.c src/sanitize/display.c \
	src/sanitize/leaf.c src/sanitize/verdict.c src/common/label_cp.c src/policy/policy.c src/probe/state.c \
	src/common/state_rank.c -framework DiskArbitration -framework CoreFoundation
cc -std=c17 -Wall -Wextra -Werror -O2 -o "$work/jstub" tests/integration/daemon/journal_stub.c
mkdir "$work/stubs"
for k in exit1 sector0; do ln "$work/jstub" "$work/stubs/jstub-$k"; done
cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -o "$work/ntfsmac-probe" \
	src/probe/main.c src/probe/cli.c src/probe/probe.c src/probe/devio.c src/probe/names.c src/probe/nfc.c \
	src/probe/state.c src/probe/text.c src/probe/json.c src/common/mftmirr.c src/common/state_rank.c \
	$nlibs -framework CoreFoundation
cc -std=c17 -Wall -Wextra -O2 -I include -I src/volmark -I src/common $ntfs -o "$work/ntfsmac-volmark" \
	src/volmark/main.c src/volmark/cli.c src/volmark/volmark.c src/volmark/syncmirror.c src/volmark/vdev.c \
	src/volmark/args.c src/common/mftmirr.c $nlibs -framework CoreFoundation
for t in logfile_mark; do
	cc -std=c17 -O2 -Wall -Wextra -Werror -D_DARWIN_C_SOURCE=1 $ntfs -o "$work/$t" tests/tools/$t.c $nlibs -framework CoreFoundation
done
cp "$HOME/ntfsmac/deps/bin/ntfs-3g" "$work/ntfsmac-worker"
for f in split splitdirty newer splitlog; do
	cp "$HOME/ntfsmac/dmvimg/$f.img" "$work/$f.img"
	chmod 600 "$work/$f.img"
done
truncate -s 64m "$work/clean.img"
"$HOME/ntfsmac/deps/sbin/mkntfs" -F -Q -L cleanvol "$work/clean.img" >/dev/null 2>&1 || { echo "mkntfs failed"; exit 2; }
"$work/logfile_mark" "$work/clean.img" --init >/dev/null || { echo "logfile_mark --init failed"; exit 2; }
chmod 600 "$work/clean.img"

# The profiles go into a root-owned directory, as the spawn module requires.
sbx=$work/sandbox
sudo -n install -d -m 755 -o root -g wheel "$sbx"
sudo -n install -m 644 -o root -g wheel "$d"/integration/sandbox/*.sb "$sbx/"

echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u) gid $(id -g)"

# ntfsfix audit, static part: the two binaries never name it.
for b in ntfsmac-probe ntfsmac-volmark; do
	n=$(grep -c -a ntfsfix "$work/$b" || true)
	echo "ntfsfix audit: $b mentions ntfsfix in $n line(s) of its bytes (text from libntfs-3g, not run):"
	grep -a -o ".\{0,50\}ntfsfix.\{0,50\}" "$work/$b" | tr -c "[:print:]\n" "?" | sed "s/^/  | /" || true
done
# ... and the dynamic part: a sampler lists any process whose executable is called that.
: >"$work/ntfsfix.seen"
(while :; do ps -axo comm= | grep -E "(^|/)ntfsfix$" >>"$work/ntfsfix.seen" 2>/dev/null || true; sleep 0.05; done) &
sampler=$!

rc=0
sudo -n "$work/dmn_mirror_vm" "$work" "$(id -u)" "$(id -g)" "$sbx" "$work/ntfsmac-probe" "$work/ntfsmac-volmark" \
	"$work/ntfsmac-worker" "$work/stubs" ${CASE:-} || rc=$?
{ kill "$sampler"; wait "$sampler"; } >/dev/null 2>&1 || true
sampler=
seen=$(grep -c . "$work/ntfsfix.seen" || true)
echo "ntfsfix audit: $seen sample(s) saw an ntfsfix process"
if [ "$seen" != 0 ]; then rc=1; fi
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
