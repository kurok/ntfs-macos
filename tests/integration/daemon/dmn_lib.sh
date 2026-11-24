#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: shared guest code of the dmn_*_guest.sh suites. Sourced, never run. It builds the
# tools that need the guest's NTFS-3G, installs the daemon the way attach_vm_guest.sh does (the
# manifest paths, root-owned, the shipped launchd plist), and has the helpers to attach image
# FILES as virtual disks (hdiutil, inside the guest) and to read what the daemon says.
#
# The caller sets `d` (the directory with include/, integration/, src/, tests/, bin/ntfsmacd,
# bin/ntfsmac and fixtures/*.img) before sourcing. Nothing here touches a host disk: every image
# is a file in the guest and every mount is inside the guest, with norecover.
#
# Output vocabulary: PASS: / FAIL: / NOTE: lines; `fail` counts the failures.

# These suites mount, attach disks and install a daemon: a disposable guest only (H1)
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
# /sbin (mount) is not in the PATH of the guest exec shell
PATH=/sbin:/usr/sbin:/usr/bin:/bin:/usr/local/bin:$PATH
export PATH

fail=0
INSTALL=/usr/local/libexec/ntfs-macos
SBX="/Library/Application Support/ntfs-macos/sandbox"
CLI=/usr/local/bin/ntfsmac
LABEL=org.ntfs-macos.ntfsmacd
PLIST=/Library/LaunchDaemons/$LABEL.plist
LOG=/var/log/ntfsmacd.log
CLOG=/var/log/ntfsmacd-children.log
JDIR=/var/db/ntfs-macos/journal

pass() { echo "PASS: $*"; }
failf() { echo "FAIL: $*"; fail=$((fail + 1)); }
note() { echo "NOTE: $*"; }
contains() { case $1 in *"$2"*) return 0 ;; esac; return 1; }
# mount_diag BSD: when the volume is not Mounted, print what the daemon and the worker said
mount_diag() {
	[ "$(state_of "$1")" = Mounted ] && return 0
	echo "-- $1 is not Mounted (state '$(state_of "$1")', reason '$(reason_of "$1")'): daemon log, then the children"
	logcat | tail -40
	sudo -n tail -n 20 "$CLOG" 2>/dev/null
	echo "-- mount table (no apfs), /Volumes, the workers"
	mount | grep -v "apfs\|devfs\|^map "
	ls -la /Volumes
	ps -axo pid,ppid,etime,stat,args | grep -E "ntfsmac-worker|launchctl|exec-as" | grep -v grep | cut -c1-200
	return 0
}
# not_attached FILE: the image file is no longer a virtual disk
not_attached() { ! hdiutil info 2>/dev/null | grep -qF "$1"; }
# lacks TEXT PATTERN: TEXT does not contain PATTERN
lacks() { ! contains "$1" "$2"; }
# has_any TEXT PATTERN...: TEXT contains at least one PATTERN
has_any() {
	_t=$1
	shift
	for _p in "$@"; do contains "$_t" "$_p" && return 0; done
	return 1
}
# apple_mounted BSD: Apple's own (ntfs.fs, FSKit) mount of the disk, which the daemon does not hold back
# for a volume it refused: the mount table shows `ntfs`, not `macfuse`
apple_mounted() { mount | grep -F "/dev/$1 " | grep -F "(ntfs," || true; }
check() { desc=$1; shift; if "$@"; then pass "$desc"; else failf "$desc"; fi; }
# eq DESCRIPTION ACTUAL EXPECTED
eq() { if [ "$2" = "$3" ]; then pass "$1 ($2)"; else failf "$1: got '$2', expected '$3'"; fi; }

# wait_for SECONDS CMD...
wait_for() {
	t=$1
	shift
	i=0
	while [ "$i" -lt "$t" ]; do
		if "$@" >/dev/null 2>&1; then return 0; fi
		sleep 1
		i=$((i + 1))
	done
	return 1
}
# tmo SECONDS CMD...: run with a time limit (a dead mount can block for good)
tmo() {
	t=$1
	shift
	perl -e 'alarm shift; exec @ARGV or exit 127' "$t" "$@"
}

sha() { shasum -a 256 "$1" | awk '{ print $1 }'; }

# ---- what the daemon says ----
list() { "$CLI" list 2>&1; }
row() { list | awk -v b="$1" '$1 == b { print }'; } # BSD LABEL STATE MOUNT MOUNTPOINT REASON
# the fields come from `ntfsmac status` (a label or a mount point with a space breaks the columns of list)
state_of() { sget "$1" state; }
mount_of() { sget "$1" mount; }
mpoint_of() { sget "$1" mountpoint; }
reason_of() { sget "$1" reason; }
has_state() { [ "$(state_of "$1")" = "$2" ]; }
transient() { case $1 in Probing | Deciding | Mounting | Unmounting) return 0 ;; esac; return 1; }
# the volume reached an end state of the attach: Mounted, or Failed (refused or failed)
settled_row() { s=$(state_of "$1"); [ "$s" = Mounted ] || [ "$s" = Failed ]; }
logcat() { sudo -n cat "$LOG" 2>/dev/null; }
# Each fixture has a volume serial of its own (tests/fixtures/gen/setserial.py), which is the journal key,
# but a case that leaves an entry (a refused marker, a kill) must still not decide the next case on the
# same image.
reset_journal() { sudo -n sh -c "rm -f $JDIR/* $JDIR/cleared/* 2>/dev/null"; }
journal_count() { sudo -n sh -c "ls $JDIR 2>/dev/null | grep -c '^[0-9A-F]\{16\}\$'" || true; }
worker_pids() { ps -axo pid=,comm= | awk -v c="$INSTALL/ntfsmac-worker" '$2 == c { print $1 }'; }
workers() { worker_pids | wc -l | tr -d ' '; }
daemon_pid() { sudo -n launchctl print system/$LABEL 2>/dev/null | sed -n 's/^[[:space:]]*pid = //p'; }
kernel_mount_of() { mount | grep -F " on /Volumes/" | grep -F "$1" || true; }

# ---- images as virtual disks ----
# attach_image FILE: attach without the system's mount (-nomount): the daemon sees the Disk
# Arbitration event and does the rest. Prints the BSD name (disk7).
attach_image() {
	_o=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage "$1" 2>&1) || { echo "hdiutil: $_o" >&2; return 1; }
	printf '%s\n' "$_o" | awk '/^\/dev\/disk/ && !done { sub("^/dev/", "", $1); print $1; done = 1 }'
}
# attach_image_auto FILE: attach with the system's automount allowed (the race with Apple's
# read-only driver that the dissenter of T-M4-02 closes). Prints the BSD name.
attach_image_auto() {
	_o=$(hdiutil attach -imagekey diskimage-class=CRawDiskImage "$1" 2>&1) || { echo "hdiutil: $_o" >&2; return 1; }
	printf '%s\n' "$_o" | awk '/^\/dev\/disk/ && !done { sub("^/dev/", "", $1); print $1; done = 1 }'
}
# durable_write FILE TEXT: write TEXT to FILE and fsync the file and its directory, which is what an
# application does to be sure that data has reached the device (`sync(1)` is not that)
durable_write() {
	perl -MIO::Handle -e '
		my ($f, $t) = @ARGV;
		open(my $h, ">", $f) or die "open $f: $!";
		print $h "$t\n";
		$h->flush; $h->sync or die "fsync: $!";
		close($h);
		(my $dir = $f) =~ s{/[^/]*$}{};
		open(my $dh, "<", $dir) or die "open dir: $!";
		$dh->sync or die "fsync dir: $!";
	' "$1" "$2"
}
# yank_image FILE: take the virtual disk away from under its users, as an unplug does: kill the
# process that serves the image (diskimages-helper, the only holder of the image file)
yank_image() {
	_p=$(sudo -n lsof -t "$1" 2>/dev/null | head -1)
	[ -n "$_p" ] || { echo "yank_image: no process holds $1" >&2; return 1; }
	sudo -n kill -9 "$_p"
}
# detach_image BSD: an image that is still mounted is unmounted through the daemon first (hdiutil
# cannot detach a disk whose device the worker holds, and an image left attached would be found
# again by the next daemon start)
detach_image() {
	if [ "$(state_of "$1" 2>/dev/null)" = Mounted ]; then
		"$CLI" unmount "$1" >/dev/null 2>&1
		wait_for 60 has_state "$1" Appeared || true
	fi
	_detach_raw "$1"
}
# _detach_raw BSD: detach, then force-detach. On failure the answers of hdiutil, what `hdiutil info` lists for the
# disk and who holds it (lsof) are printed as NOTE lines, and the status is 1 (callers that only clean up ignore it).
_detach_raw() {
	_o1=$(tmo 60 hdiutil detach "/dev/$1" 2>&1) && return 0
	_o2=$(tmo 60 hdiutil detach "/dev/$1" -force 2>&1) && return 0
	hdiutil info 2>/dev/null | grep -qE "^/dev/$1[[:space:]]" || return 0 # gone after all
	{
		echo "detach of /dev/$1 failed: $_o1"
		echo "force-detach of /dev/$1 failed: $_o2"
		echo "hdiutil info:"; hdiutil info 2>&1 | grep -E "image-path|^/dev/$1([[:space:]]|s)"
		echo "lsof /dev/$1*:"; sudo -n lsof 2>/dev/null | grep -E "/dev/r?$1(s[0-9]+)?\$" || echo "(no holder found)"
	} | sed 's/^/NOTE: /'
	return 1
}
# every virtual disk of this run is detached at the end
detach_all() {
	hdiutil info 2>/dev/null | awk -v w="$work/" '/^image-path/ { p = $3 } /^\/dev\/disk[0-9]+[[:space:]]/ { if (index(p, w) == 1) print $1 }' |
		while read -r dev; do tmo 60 hdiutil detach "$dev" -force >/dev/null 2>&1 || true; done
}

# ---- tools ----
# build_tools: the probe, a probe stand-in that reports `clean`, volmark, setvolflags and
# logfile_mark against the guest's NTFS-3G, into $work/bin.
build_tools() {
	export PKG_CONFIG_PATH=$HOME/ntfsmac/deps/lib/pkgconfig
	mkdir -p "$work/bin"
	ntfs="$(pkg-config --cflags libntfs-3g)"
	nlibs="$(pkg-config --libs --static libntfs-3g)"
	psrc="src/probe/main.c src/probe/cli.c src/probe/probe.c src/probe/devio.c src/probe/names.c src/probe/nfc.c src/probe/state.c src/probe/text.c src/probe/json.c src/common/mftmirr.c src/common/state_rank.c"
	# shellcheck disable=SC2086
	cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -o "$work/bin/ntfsmac-probe" $psrc $nlibs -framework CoreFoundation ||
		{ echo "FAIL: probe did not build"; exit 2; }
	# The stand-in: cli.c calls dmn_stub_run instead of ntfsmac_probe_run; the stand-in runs the
	# real check and then reports `clean` whatever it found (I-DMN-23).
	cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -Dntfsmac_probe_run=dmn_stub_run -c \
		-o "$work/bin/cli_stub.o" src/probe/cli.c || { echo "FAIL: probe stand-in did not build"; exit 2; }
	# shellcheck disable=SC2086
	cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -o "$work/bin/ntfsmac-probe-stub" src/probe/main.c \
		"$work/bin/cli_stub.o" tests/integration/daemon/dmn_probe_stub.c src/probe/probe.c src/probe/devio.c \
		src/probe/names.c src/probe/nfc.c src/probe/state.c src/probe/text.c src/probe/json.c \
		src/common/mftmirr.c src/common/state_rank.c $nlibs -framework CoreFoundation ||
		{ echo "FAIL: probe stand-in did not link"; exit 2; }
	# the slow probe: the real verdict, 12 s late
	cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -DDMN_STUB_SLOW=12 -Dntfsmac_probe_run=dmn_stub_run -c \
		-o "$work/bin/cli_slow.o" src/probe/cli.c || { echo "FAIL: slow probe did not build"; exit 2; }
	cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -DDMN_STUB_SLOW=12 -c -o "$work/bin/stub_slow.o" \
		tests/integration/daemon/dmn_probe_stub.c || { echo "FAIL: slow probe stub did not build"; exit 2; }
	# shellcheck disable=SC2086
	cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -o "$work/bin/ntfsmac-probe-slow" src/probe/main.c \
		"$work/bin/cli_slow.o" "$work/bin/stub_slow.o" src/probe/probe.c src/probe/devio.c \
		src/probe/names.c src/probe/nfc.c src/probe/state.c src/probe/text.c src/probe/json.c \
		src/common/mftmirr.c src/common/state_rank.c $nlibs -framework CoreFoundation ||
		{ echo "FAIL: slow probe did not link"; exit 2; }
	# shellcheck disable=SC2086
	cc -std=c17 -O2 -I include -I src/volmark -I src/common $ntfs -o "$work/bin/ntfsmac-volmark" \
		src/volmark/main.c src/volmark/cli.c src/volmark/volmark.c src/volmark/syncmirror.c src/volmark/vdev.c src/volmark/args.c \
		src/common/mftmirr.c $nlibs -framework CoreFoundation || { echo "FAIL: volmark did not build"; exit 2; }
	# the client that does not refuse a caller that is not root (I-DMN-19, the spawned peer)
	cc -std=c17 -O2 -I include -I src/daemon -I src/cli -o "$work/bin/dmn-peer-client" tests/integration/daemon/dmn_peer_client.c \
		src/cli/authz.c src/cli/cli.c src/cli/client.c src/cli/cmd_journal.c src/cli/cmd_repair.c src/cli/json.c \
		src/cli/priv.c src/cli/reasons.c src/cli/render.c src/cli/req.c src/cli/xpc_client.c src/daemon/ctl_parse.c \
		-framework Security -framework CoreFoundation || { echo "FAIL: dmn-peer-client did not build"; exit 2; }
	for t in setvolflags logfile_mark; do
		# shellcheck disable=SC2086
		cc -std=c17 -O2 -D_DARWIN_C_SOURCE=1 $ntfs -o "$work/bin/$t" "tests/tools/$t.c" $nlibs -framework CoreFoundation ||
			{ echo "FAIL: $t did not build"; exit 2; }
	done
}

# install_daemon: the manifest paths, root-owned; the CLI signed ad hoc with the hardened runtime
# and its cdhash recorded as the requirement (ADR-019 check 3), as the installer does.
install_daemon() {
	sudo -n install -d -m 755 -o root -g wheel "$INSTALL" /usr/local/bin "$SBX"
	sudo -n install -m 755 -o root -g wheel bin/ntfsmacd "$INSTALL/ntfsmacd"
	cp bin/ntfsmac "$work/ntfsmac"
	codesign --force -s - -o runtime "$work/ntfsmac" >/dev/null 2>&1 || failf "codesign of the CLI"
	hash=$(codesign -d -vvv "$work/ntfsmac" 2>&1 | sed -n "s/^CDHash=//p")
	sudo -n install -m 755 -o root -g wheel "$work/ntfsmac" "$CLI"
	printf 'cdhash H"%s"\n' "$hash" >"$work/req"
	sudo -n install -m 644 -o root -g wheel "$work/req" "/Library/Application Support/ntfs-macos/cli-requirement"
	use_probe real
	sudo -n install -m 755 -o root -g wheel "$work/bin/ntfsmac-volmark" "$INSTALL/ntfsmac-volmark"
	sudo -n install -m 755 -o root -g wheel "$HOME/ntfsmac/deps/bin/ntfs-3g" "$INSTALL/ntfsmac-worker"
	sudo -n install -m 644 -o root -g wheel integration/sandbox/*.sb "$SBX/"
	uid=$(id -u)
	if [ "$(stat -f %Su /dev/console)" = root ]; then
		note "no user is logged in at the console: the job gets --console-uid $uid (test seam)"
		CONSOLE_ARGS="--console-uid $uid"
	else
		CONSOLE_ARGS=
	fi
}
# use_probe real|stub: which program answers as /usr/local/libexec/ntfs-macos/ntfsmac-probe
use_probe() {
	case $1 in
	real) sudo -n install -m 755 -o root -g wheel "$work/bin/ntfsmac-probe" "$INSTALL/ntfsmac-probe" ;;
	stub) sudo -n install -m 755 -o root -g wheel "$work/bin/ntfsmac-probe-stub" "$INSTALL/ntfsmac-probe" ;;
	slow) sudo -n install -m 755 -o root -g wheel "$work/bin/ntfsmac-probe-slow" "$INSTALL/ntfsmac-probe" ;;
	esac
}
# use_volmark real|FILE: which program answers as ntfsmac-volmark
use_volmark() {
	case $1 in
	real) sudo -n install -m 755 -o root -g wheel "$work/bin/ntfsmac-volmark" "$INSTALL/ntfsmac-volmark" ;;
	*) sudo -n install -m 755 -o root -g wheel "$1" "$INSTALL/ntfsmac-volmark" ;;
	esac
}

# start_daemon [DAEMON ARGS...]: the shipped plist plus the seams the guest needs (a child log,
# the console uid when nobody is logged in, and the arguments given).
start_daemon() {
	stop_daemon
	extra=
	for a in $CONSOLE_ARGS --child-log $CLOG "$@"; do extra="$extra<string>$a</string>"; done
	sed "s|<string>$INSTALL/ntfsmacd</string>|&$extra|" integration/launchd/$LABEL.plist >"$work/job.plist"
	sudo -n install -m 644 -o root -g wheel "$work/job.plist" "$PLIST"
	sudo -n rm -f "$LOG" "$CLOG"
	sudo -n launchctl bootstrap system "$PLIST" || failf "launchctl bootstrap"
	wait_for 30 test -S /var/run/ntfsmacd.sock || failf "the control socket did not appear"
}
stop_daemon() { sudo -n launchctl bootout system/$LABEL >/dev/null 2>&1 || true; }

# prepare: the work directory, the tools, the installation
prepare() {
	cd "$d" || exit 2
	work=$(cd -P "$(mktemp -d /tmp/dmnvm.XXXXXX)" && pwd)
	chmod 755 "$work"
	build_tools
	install_daemon
	stop_daemon
	reset_journal # the suites share one guest and one journal
	echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u), console owner $(stat -f %Su /dev/console)"
}
finish() {
	stop_daemon
	reset_journal
	detach_all
	sudo -n /usr/bin/pkill -9 -f "$work/" >/dev/null 2>&1
	sudo -n rm -rf "$work"
	echo "result: $fail check(s) failed"
	[ "$fail" -eq 0 ]
}

# fresh_image NAME: a private copy of fixture NAME in $work (the originals stay as generated)
fresh_image() {
	cp "$d/fixtures/$1.img" "$work/$1.img" && echo "$work/$1.img"
}

# ---- the same facts through `ntfsmac status`, for labels with spaces (list is column text) ----
sget() { "$CLI" status "$1" 2>&1 | sed -n "s/^  $2: //p" | head -1; } # sget BSD state|mount|mountpoint|reason
settled_s() { s=$(sget "$1" state); [ "$s" = Mounted ] || [ "$s" = Failed ]; }
