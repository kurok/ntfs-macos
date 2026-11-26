#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-08: the part of attach_vm.sh that runs inside the macOS guest, as the guest user (who has
# passwordless sudo). Usage: attach_vm_guest.sh DIR, where DIR holds the sources (include,
# integration, src), bin/ntfsmacd and bin/ntfsmac (built on the host) and where NTFS-3G was built
# into ~/ntfsmac/deps by the host script. Two NTFS disks are attached: the fixtures `clean-64m-4k`
# (label clean64m4k) and `dirty-flag`.
#
# Prints PASS, FAIL and NOTE lines; exit 0 only if no FAIL.
set -u
# /sbin (mount) is not in the PATH of the guest exec shell
PATH=/sbin:/usr/sbin:/usr/bin:/bin:/usr/local/bin:$PATH
export PATH

d=${1:?directory}
fail=0
INSTALL=/usr/local/libexec/ntfs-macos
CLI=$INSTALL/ntfsmac
LABEL=org.ntfs-macos.ntfsmacd
PLIST=/Library/LaunchDaemons/$LABEL.plist
LOG=/var/log/ntfsmacd.log
CLEAN=clean64m4k
DIRTY=dirty-flag

pass() { echo "PASS: $*"; }
failf() { echo "FAIL: $*"; fail=$((fail + 1)); }
note() { echo "NOTE: $*"; }
contains() { case $1 in *"$2"*) return 0 ;; esac; return 1; }
check() { desc=$1; shift; if "$@"; then pass "$desc"; else failf "$desc"; fi; }

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

list() { "$CLI" list 2>&1; }
col() { list | awk -v l="$1" -v c="$2" '$2 == l { print $c }'; } # col LABEL N: BSD LABEL STATE MOUNT MOUNTPOINT REASON
state_of() { col "$1" 3; }
mount_of() { col "$1" 4; }
bsd_of() { col "$1" 1; }
reason_of() { col "$1" 6; }
has_state() { [ "$(state_of "$1")" = "$2" ]; }
# both volumes are known and none is in a transient state
settled() {
	list | awk 'NR > 1 { n++; if ($3 == "Probing" || $3 == "Deciding" || $3 == "Mounting" || $3 == "Unmounting") bad = 1 } END { exit (n >= 2 && !bad) ? 0 : 1 }'
}
mnt_line() { mount | grep -F "/Volumes/$1 " || true; }
logcat() { sudo -n cat "$LOG" 2>/dev/null; }
journal_count() { sudo -n sh -c 'ls /var/db/ntfs-macos/journal 2>/dev/null | grep -c "^[0-9A-F]\{16\}$"' || true; }
# Exact: a process whose executable is the worker (`ps` shows proc_pidpath), not any command line
# that mentions it (launchctl asuser and the --exec-as step carry the path in their arguments).
worker_pids() { ps -axo pid=,comm= | awk -v c="$INSTALL/ntfsmac-worker" '$2 == c { print $1 }'; }
worker_ppids() { ps -axo ppid=,comm= | awk -v c="$INSTALL/ntfsmac-worker" '$2 == c { print $1 }'; }
workers() { worker_pids | wc -l | tr -d ' '; }
daemon_pid() { sudo -n launchctl print system/$LABEL | sed -n 's/^[[:space:]]*pid = //p'; }
# every tracked worker pid is the worker itself and its parent is the daemon (kill-group, the
# parent walk of ADR-019 check 2 and the exit status depend on it)
workers_are_children() { dp=$(daemon_pid); [ -n "$dp" ] && [ "$(worker_ppids | grep -vx "$dp" | wc -l | tr -d ' ')" = 0 ] && [ "$(workers)" -ge 1 ]; }

echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u) gid $(id -g), console owner $(stat -f %Su /dev/console)"

# ---- tools: the probe and volmark against the guest NTFS-3G, the worker from the same build ----
cd "$d" || exit 2
export PKG_CONFIG_PATH=$HOME/ntfsmac/deps/lib/pkgconfig
work=$(cd -P "$(mktemp -d /tmp/attachvm.XXXXXX)" && pwd)
chmod 755 "$work"
ntfs="$(pkg-config --cflags libntfs-3g)"
nlibs="$(pkg-config --libs --static libntfs-3g)"
cc -std=c17 -O2 -I include -I src/probe -I src/common $ntfs -o "$work/ntfsmac-probe" \
	src/probe/main.c src/probe/cli.c src/probe/probe.c src/probe/devio.c src/probe/names.c src/probe/nfc.c \
	src/probe/state.c src/probe/text.c src/probe/json.c src/common/mftmirr.c src/common/state_rank.c \
	$nlibs -framework CoreFoundation || { echo "FAIL: probe did not build"; exit 2; }
cc -std=c17 -O2 -I include -I src/volmark -I src/common $ntfs -o "$work/ntfsmac-volmark" \
	src/volmark/main.c src/volmark/cli.c src/volmark/volmark.c src/volmark/syncmirror.c src/volmark/vdev.c src/volmark/args.c \
	src/common/mftmirr.c $nlibs -framework CoreFoundation || { echo "FAIL: volmark did not build"; exit 2; }

# ---- install at the manifest paths, root-owned ----
sudo -n install -d -m 755 -o root -g wheel "$INSTALL" "/Library/Application Support/ntfs-macos/sandbox"
sudo -n install -m 755 -o root -g wheel bin/ntfsmacd "$INSTALL/ntfsmacd"
# The privileged XPC service accepts only the CLI that the installer recorded (ADR-019 check 3):
# the CLI is signed ad hoc with the hardened runtime, and its cdhash is the recorded requirement.
cp bin/ntfsmac "$work/ntfsmac"
codesign --force -s - -o runtime "$work/ntfsmac" >/dev/null 2>&1 || failf "codesign of the CLI"
hash=$(codesign -d -vvv "$work/ntfsmac" 2>&1 | sed -n "s/^CDHash=//p")
sudo -n install -m 755 -o root -g wheel "$work/ntfsmac" "$CLI"
printf 'cdhash H"%s"\n' "$hash" >"$work/req"
sudo -n install -m 644 -o root -g wheel "$work/req" "/Library/Application Support/ntfs-macos/cli-requirement"
sudo -n install -m 755 -o root -g wheel "$work/ntfsmac-probe" "$INSTALL/ntfsmac-probe"
sudo -n install -m 755 -o root -g wheel "$work/ntfsmac-volmark" "$INSTALL/ntfsmac-volmark"
sudo -n install -m 755 -o root -g wheel "$HOME/ntfsmac/deps/bin/ntfs-3g" "$INSTALL/ntfsmac-worker"
sudo -n install -m 644 -o root -g wheel integration/sandbox/*.sb "/Library/Application Support/ntfs-macos/sandbox/"
# The job is the shipped plist. Without a user at the console the daemon cannot run a worker, so a
# guest with no login gets the test seam --console-uid (the shipped plist is otherwise unchanged).
uid=$(id -u)
if [ "$(stat -f %Su /dev/console)" = root ]; then
	note "no user is logged in at the console: the job gets --console-uid $uid and --child-log (test seams)"
	sed "s|<string>$INSTALL/ntfsmacd</string>|&<string>--console-uid</string><string>$uid</string><string>--child-log</string><string>/var/log/ntfsmacd-children.log</string>|" \
		integration/launchd/$LABEL.plist >"$work/job.plist"
else
	sed "s|<string>$INSTALL/ntfsmacd</string>|&<string>--child-log</string><string>/var/log/ntfsmacd-children.log</string>|" \
		integration/launchd/$LABEL.plist >"$work/job.plist"
fi
sudo -n install -m 644 -o root -g wheel "$work/job.plist" "$PLIST"
check "plist is valid" plutil -lint "$PLIST"

echo "== disks the guest sees"
diskutil list | sed -n '1,40p'

# ---- 0. a FUSE mount of the same user that exists BEFORE the daemon attaches anything ----
# (an sshfs, another macFUSE volume): the H1 watcher must never touch it (review item 2).
echo "== 0. a FUSE mount of the user before the attach"
pre=$work/pre.img
truncate -s 16m "$pre"
"$HOME/ntfsmac/deps/sbin/mkntfs" -F -Q -L pretrap "$pre" >/dev/null 2>&1 || failf "mkntfs of the pre-existing image"
mkdir -p "$HOME/pre"
"$HOME/ntfsmac/deps/bin/ntfs-3g" -o "backend=fskit,local,norecover,uid=$uid,gid=$(id -g),ro" "$pre" "$HOME/pre" >"$work/pre.log" 2>&1
check "the user's own FUSE mount is there before the daemon starts" wait_for 30 sh -c "mount | grep -qF \"$HOME/pre\""
premounts() { mount | grep -F macfuse | grep -vcF "$HOME/pre"; }

# ---- 1. start the job; both fixtures attach by themselves ----
echo "== 1. launchd job and automatic attach"
sudo -n launchctl bootstrap system "$PLIST" || failf "launchctl bootstrap"
check "the control socket appears" wait_for 30 test -S /var/run/ntfsmacd.sock
check "the privileged XPC service is registered with launchd" sh -c "sudo -n launchctl print system/$LABEL | grep -q org.ntfs-macos.ctl.privileged"
check "both volumes settle" wait_for 150 settled
echo "-- ntfsmac list"
list
echo "-- ntfsmac status (clean)"
"$CLI" status "$(bsd_of $CLEAN)" 2>&1 | sed -n '1,40p'
check "clean-64m-4k is Mounted" has_state $CLEAN Mounted
check "clean-64m-4k mounts rw" [ "$(mount_of $CLEAN)" = rw ]
check "dirty-flag is Mounted" has_state $DIRTY Mounted
check "dirty-flag mounts ro" [ "$(mount_of $DIRTY)" = ro ]
check "dirty-flag says why: dirty_flag" [ "$(reason_of $DIRTY)" = dirty_flag ]
echo "-- mount table"
mount | grep -F macfuse || true
check "the user's earlier FUSE mount survived the attach of both volumes" sh -c "mount | grep -qF \"$HOME/pre\""
check "the kernel table shows the clean volume as a macfuse mount that is not read-only" sh -c "mount | grep -F '/Volumes/$CLEAN ' | grep -F macfuse | grep -qv read-only"
check "the kernel table shows the dirty volume read-only" sh -c "mount | grep -F '/Volumes/$DIRTY ' | grep -F macfuse | grep -q read-only"
check "the clean volume holds a journal entry while it is mounted rw" [ "$(journal_count)" = 1 ]
check "two workers run" [ "$(workers)" -ge 2 ]
check "every worker process is the worker binary and a direct child of the daemon" workers_are_children
check "the chain of launchctl and --exec-as left no process behind" sh -c "! ps -axo comm= | grep -q '^/bin/launchctl$' || ! ps -axo args= | grep -v grep | grep -q 'launchctl asuser.*--exec-as'"
check "a file can be written on the rw volume" sh -c "echo m408 > '/Volumes/$CLEAN/m408.txt' && sync && cat '/Volumes/$CLEAN/m408.txt' | grep -q m408"
check "a file cannot be written on the ro volume" sh -c "! (echo x > '/Volumes/$DIRTY/m408.txt') 2>/dev/null"
echo "-- daemon log so far"
logcat | sed -n '1,60p'
check "the daemon logged the notifications" sh -c "sudo -n grep -q 'notification: ' $LOG"

# ---- 2. unmount, a clean session, mount again ----
echo "== 2. unmount of the rw volume (section 4.5), and mounting it again"
cb=$(bsd_of $CLEAN)
"$CLI" unmount "$cb" 2>&1
check "the clean volume returns to Appeared" wait_for 60 has_state $CLEAN Appeared
check "its mount point directory is gone" sh -c "[ ! -e '/Volumes/$CLEAN' ]"
check "the journal entry was removed by the clean session" [ "$(journal_count)" = 0 ]
check "the daemon told the user: ejected, all data written (the full text, so the drive flush was confirmed)" sh -c "sudo -n grep -q 'ejected, all data written\\.' $LOG"
check "no notification says that the drive did not confirm its cache flush" sh -c "! sudo -n grep -q 'did not confirm' $LOG"
check "the worker's drive flush is not refused by its sandbox" sh -c "! sudo -n grep -q 'DKIOCSYNCHRONIZE failed' /var/log/ntfsmacd-children.log"
"$CLI" mount "$cb" 2>&1
check "the clean volume mounts rw again" wait_for 90 sh -c "[ \"\$($CLI list | awk -v l=$CLEAN '\$2 == l { print \$3 \" \" \$4 }')\" = 'Mounted rw' ]"
check "the second session holds a journal entry again" [ "$(journal_count)" = 1 ]
"$CLI" unmount "$cb" 2>&1
check "and ends cleanly again" wait_for 60 has_state $CLEAN Appeared
check "with no journal entry" [ "$(journal_count)" = 0 ]

# ---- 3. stop and start the job ----
echo "== 3. stopping the job unmounts what it serves; starting it attaches again"
"$CLI" mount "$cb" 2>&1
wait_for 90 sh -c "[ \"\$($CLI list | awk -v l=$CLEAN '\$2 == l { print \$3 \" \" \$4 }')\" = 'Mounted rw' ]"
sudo -n launchctl bootout system/$LABEL
check "after the stop no macfuse mount is left" wait_for 60 sh -c "[ \"\$(mount | grep -F macfuse | grep -vcF $HOME/pre)\" = 0 ]"
check "after the stop no worker runs" [ "$(workers)" = 0 ]
check "the stop settled the rw session (no journal entry)" [ "$(journal_count)" = 0 ]
sudo -n launchctl bootstrap system "$PLIST" || failf "launchctl bootstrap (second)"
check "both volumes settle again" wait_for 150 settled
check "clean-64m-4k is Mounted rw again" sh -c "[ \"\$($CLI list | awk -v l=$CLEAN '\$2 == l { print \$3 \" \" \$4 }')\" = 'Mounted rw' ]"
check "dirty-flag is Mounted ro again" sh -c "[ \"\$($CLI list | awk -v l=$DIRTY '\$2 == l { print \$3 \" \" \$4 }')\" = 'Mounted ro' ]"

# ---- 4. H1: a second FUSE mount of the worker's user ----
echo "== 4. H1: another macFUSE mount owned by the user of the workers"
img=$work/stray.img
truncate -s 16m "$img"
"$HOME/ntfsmac/deps/sbin/mkntfs" -F -Q -L straytrap "$img" >/dev/null 2>&1 || failf "mkntfs of the stray image"
mkdir -p "$HOME/trap"
workers_before=$(workers)
note "workers before: $workers_before"
"$HOME/ntfsmac/deps/bin/ntfs-3g" -o "backend=fskit,local,norecover,uid=$uid,gid=$(id -g),ro" "$img" "$HOME/trap" >"$work/stray.log" 2>&1
stray_rc=$?
wait_for 30 sh -c "mount | grep -qF \"$HOME/trap\""
note "the stray mount (rc $stray_rc): $(mount | grep -F "$HOME/trap" | head -1)"
check "the daemon stops the workers (no worker left)" wait_for 30 sh -c "[ \"\$(ps -axo comm= | grep -cx $INSTALL/ntfsmac-worker)\" = 0 ]"
check "the stray mount is gone" wait_for 30 sh -c "! mount | grep -qF \"$HOME/trap\""
check "the clean volume is Failed with the reason stray_mount" wait_for 30 sh -c "[ \"\$($CLI list | awk -v l=$CLEAN '\$2 == l { print \$3 \" \" \$6 }')\" = 'Failed stray_mount' ]"
check "the dirty volume is Failed with the reason stray_mount" sh -c "[ \"\$($CLI list | awk -v l=$DIRTY '\$2 == l { print \$3 \" \" \$6 }')\" = 'Failed stray_mount' ]"
check "no macfuse mount is left but the user's earlier one" [ "$(premounts)" = 0 ]
check "the user's earlier FUSE mount is still there after the stray handling and the stop" sh -c "mount | grep -qF \"$HOME/pre\""
check "the killed rw session left its journal entry and marker (silence is never success)" [ "$(journal_count)" = 1 ]
check "the daemon told the user" sh -c "sudo -n grep -q 'mounted another file system' $LOG"
check "the mount point directories are gone" sh -c "[ ! -e '/Volumes/$CLEAN' ] && [ ! -e '/Volumes/$DIRTY' ]"
echo "-- ntfsmac list after the stray mount"
list
echo "-- journal"
sudo -n ls -la /var/db/ntfs-macos/journal 2>&1 | sed -n '1,10p'

# `ntfsmac status BSD`, asked again (up to 10 times, 1 s apart) while the daemon answers that the
# session journal is busy (reason journal_busy, T-M7-14: status waits at most 1 s for the journal).
status_of() {
	_n=0
	while :; do
		_o=$("$CLI" status "$1" 2>&1) || true
		case $_o in
		*"busy with the session journal"*) ;;
		*) printf '%s\n' "$_o"; return 0 ;;
		esac
		_n=$((_n + 1))
		[ "$_n" -lt 10 ] || { printf '%s\n' "$_o"; return 0; }
		sleep 1
	done
}
# ---- 5. the privileged request reaches the daemon (ROOT_PEER: sudo) ----
echo "== 5. sudo ntfsmac journal-clear reaches the daemon"
serial=$(status_of "$cb" | sed -n 's/^  serial: 0x//p')
serial=${serial#????????} # the CLI asks for the last 8 hex digits
note "serial of the clean volume: $serial"
check "the journal still holds the entry of the killed session" [ "$(journal_count)" = 1 ]
# The killed rw session left the dirty flag too, so the daemon refuses with its own reason (a fresh
# check must find the volume clean first): an answer of the daemon, over the privileged XPC service.
# The success of the operation on a clean volume is covered by journal_vm.sh (T-M4-14).
jc=$(sudo -n "$CLI" journal-clear --label "$CLEAN" --serial "$serial" "$cb" 2>&1)
jrc=$?
echo "$jc"
check "journal-clear was refused (exit $jrc)" test "$jrc" -ne 0
check "the refusal is the daemon's: the volume is not clean" contains "$jc" "The volume is not clean"
check "the refused request left the entry in place" [ "$(journal_count)" = 1 ]
check "no warning about a missing right" sh -c "! sudo -n grep -q 'right org.ntfs-macos.journal-modify' $LOG"

echo "== daemon log"
logcat | sed -n '1,200p'
echo "== worker and probe stderr"
sudo -n tail -n 20 /var/log/ntfsmacd-children.log 2>/dev/null || true

sudo -n launchctl bootout system/$LABEL >/dev/null 2>&1
sudo -n /usr/bin/pkill -9 -f "ntfs-3g.*stray.img" >/dev/null 2>&1
sudo -n /sbin/umount -f "$HOME/trap" >/dev/null 2>&1
sudo -n /sbin/umount -f "$HOME/pre" >/dev/null 2>&1
sudo -n rm -rf "$work" "$HOME/trap" "$HOME/pre"
echo "result: $fail check(s) failed"
[ "$fail" -eq 0 ]
