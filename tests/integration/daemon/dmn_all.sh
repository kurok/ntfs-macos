#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11: a run of the whole I-DMN-* range that is runnable now (docs/TEST_STRATEGY.md section 4):
# every guest driver of the range, with a summary of exit codes and PASS/FAIL counts and the I-DMN ids
# each driver covers. Nothing here touches a host disk; every guest is a clone of the golden VM that is
# deleted at the end.
#
#   spawn_vm.sh                 I-DMN-09, I-DMN-26 b and c (module level, U6, U20)
#   spawn_vm.sh --sandbox       I-SBX-06 (the sandboxed worker mounts)
#   sandbox_vm.sh               I-SBX-* (I-DMN-09 adjacent)
#   errwatch_vm.sh              I-DMN-24 a and b (module level, U16)
#   journal_vm.sh               I-DMN-19..22, I-DMN-25 a and c (module level)
#   xpc_vm.sh                   the XPC door of I-DMN-19 with fake operations
#   dmn_mirror_vm.sh            I-DMN-27 (module level)
#   attach_vm.sh                the T-M4-08 manual attach (needs --fixtures)
#   dmn_vm.sh classes kill unmount labels priv slow u15 finder
#                               I-DMN-01..08, 10..13, 14..16, 17, 18, 19..22, 23, 25 b, 27 at daemon
#                               level, U15, Finder
#
# Guests (CI speedup step 5; docs/TEST_STRATEGY.md section 4, "Whole range"). Default (--share-guests 1):
#   shared guest   spawn, spawn-sbx, errwatch, journal, mirror run one after the other in ONE guest (each with
#                  --guest NAME), and NTFS-3G is built there once. They are module-level drivers: none installs,
#                  starts or kills the daemon, none registers a launchd job or changes the system sandbox (their
#                  profiles go into their own root-owned work directory), and each one works in its own mktemp
#                  directory under /tmp, its own source directory under ~/ntfsmac, its own mount points under
#                  /Volumes, kills only the processes of its own work directory (pkill -f WORK/) and checks only its
#                  own mount point. They read ~/ntfsmac/deps and never write it. Proven reset: right after the boot
#                  the guest state is recorded (/Volumes, the mount table, the attached images, the NTFS-3G, worker,
#                  FUSE and driver processes, the org.ntfs-macos launchd jobs, the product's install paths and the
#                  drivers' work directories); after every driver it must be the same again (three looks, 5 s
#                  apart). The two services macFUSE starts at the first mount of a boot and keeps (its FSKit module
#                  and its launchservice daemon) get a second baseline, their pids, taken after the first driver that
#                  starts them (one instance each, none stopped or a zombie); later drivers must leave exactly those
#                  pids, so a second instance, a restart or a wedged one counts as a difference. When the state
#                  differs, or the driver failed, that guest is deleted and the next driver of the group gets a new
#                  one. When the shared guest cannot be cloned or booted, every driver of the group clones its own.
#   own guest      sandbox   registers a launchd job that owns the product's Mach service, attaches a raw disk and
#                            serves a second FUSE file system: plays with launchd;
#                  xpc       registers launchd jobs under the product's Mach service and writes
#                            /Library/Application Support/ntfs-macos/cli-requirement: plays with launchd;
#                  attach    installs and starts the daemon (launchd), and its fixtures are disks given at boot;
#                  dmn       installs, kills, restarts and uninstalls the daemon, needs a pristine guest.
#                  Each of these keeps a fresh clone of its own, as before.
#   two guests     the jobs (the shared group counts as one) are taken from one list, longest first: dmn, the
#                  shared group, sandbox, attach, xpc. A second guest runs next to the first only when a second
#                  slot is free: it is tried (never waited for) when the run starts and again before each job of
#                  the first lane, as the lanes of vm/macos/run-suite.sh do. So at most two guests exist, each
#                  holding a slot. With --deps-tar and no tar yet, the first of attach and dmn builds NTFS-3G
#                  and saves the tar; the other uses it when it is saved by then, else builds without saving.
# --share-guests 0 (or NTFSMAC_DMN_SHARE=0) is the old order: every driver its own fresh guest, one after the
# other, in the order of DRIVER. The result is the same in both modes: DIR/DRIVER.txt, DIR/DRIVER.log and the
# summary lines `DRIVER exit N  PASS N  FAIL N` in the order of DRIVER, in DIR/summary.txt (rewritten after every
# driver, so an interrupt keeps the lines of the drivers that ended) and at the end of the output.
# Signals: the drivers run in the background of their lane and are waited for, so INT, TERM or HUP to this script
# alone (not only to its process group) is handled at once: every driver of both lanes and all its commands get
# TERM, their traps delete their guests, then the shared guest is deleted and the slots are released.
#
# Usage: tests/integration/daemon/dmn_all.sh --fixtures DIR [--deps-tar FILE] [--out-dir DIR]
#                                           [--slot-dir DIR] [--lane-slot-dir DIR] [--share-guests 0|1]
#                                           [--lanes 1|2] [DRIVER...]
#   --fixtures   the image files (tests/fixtures/gen/*.sh --export DIR), for attach_vm.sh and dmn_vm.sh
#   --out-dir    where each driver's output goes (default: a temporary directory, printed)
#   --slot-dir   a directory for the slot locks slot-1 and slot-2, taken with mkdir around each guest
#                (a `pid` file in the lock names the holder; a lock whose holder is gone is removed);
#                default: none, and then the caller holds whatever lock it needs
#   --lane-slot-dir  without --slot-dir: the caller holds one slot of DIR for the first guest; a second guest
#                runs only when the other slot of DIR is free (taken without waiting). Without this option and
#                without --slot-dir there is never a second guest.
#   --share-guests   1 (default, or NTFSMAC_DMN_SHARE): the guest sharing and the two guests above; 0: the old order
#   --lanes      2 (default, or NTFSMAC_DMN_LANES): a second guest when a slot is free; 1: one guest at a time
#   DRIVER       a subset of: spawn spawn-sbx sandbox errwatch journal xpc mirror attach dmn
# Exit: 0 every driver passed, 1 a driver failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
fx=
deps_tar=
outdir=
slotdir=
laneslots=
share=${NTFSMAC_DMN_SHARE:-1}
lanes=${NTFSMAC_DMN_LANES:-2}
drivers=
while [ $# -gt 0 ]; do
	case "$1" in
	--fixtures) fx=$2; shift 2 ;;
	--deps-tar) deps_tar=$2; shift 2 ;;
	--out-dir) outdir=$2; shift 2 ;;
	--slot-dir) slotdir=$2; shift 2 ;;
	--lane-slot-dir) laneslots=$2; shift 2 ;;
	--share-guests) share=$2; shift 2 ;;
	--lanes) lanes=$2; shift 2 ;;
	-*) echo "dmn_all: unknown option $1" >&2; exit 2 ;;
	*) drivers="$drivers $1"; shift ;;
	esac
done
[ -n "$drivers" ] || drivers="spawn spawn-sbx sandbox errwatch journal xpc mirror attach dmn"
[ -n "$fx" ] || { echo "dmn_all: --fixtures is required" >&2; exit 2; }
case $share in 0 | 1) ;; *) echo "dmn_all: --share-guests (or NTFSMAC_DMN_SHARE) must be 0 or 1, not $share" >&2; exit 2 ;; esac
case $lanes in 1 | 2) ;; *) echo "dmn_all: --lanes (or NTFSMAC_DMN_LANES) must be 1 or 2, not $lanes" >&2; exit 2 ;; esac
for dr in $drivers; do
	case $dr in
	spawn | spawn-sbx | sandbox | errwatch | journal | xpc | mirror | attach | dmn) ;;
	*) echo "dmn_all: unknown driver $dr" >&2; exit 2 ;;
	esac
done
[ -z "$slotdir" ] || mkdir -p "$slotdir"
[ -n "$outdir" ] || outdir=$(mktemp -d "${TMPDIR:-/tmp}/dmn_all.XXXXXX")
mkdir -p "$outdir"
echo "dmn_all: output in $outdir"
# run state (job claims, results, guest state records): not in the output directory, gone at the end
state=$(mktemp -d "${TMPDIR:-/tmp}/dmn_all-state.XXXXXX")
mkdir "$state/claim" "$state/res" "$state/share"

# The slot lock is a directory with a file `pid` (the process that holds it); the protocol is the one of
# vm/macos/run-suite.sh. A lock whose holder is gone is taken over by claiming its pid file (the directory never
# moves); a lock is released only when its pid file holds this shell's pid.
slot=
# pid_gone PID: 0 when no process has that pid. kill -0 alone would call a live process of another user dead
# (EPERM), so ps -p is asked too: a process is gone only when both fail.
pid_gone() {
	case $1 in '' | *[!0-9]*) return 0 ;; esac
	kill -0 "$1" 2>/dev/null && return 1
	ps -p "$1" >/dev/null 2>&1 && return 1
	return 0
}
# grab_slot_claim SRC PID: take over a stale slot of dir gs_d by claiming its pid file SRC (the slot dir itself never
# moves, so the number of slots can never be exceeded): rename SRC to pid.$$ (atomic, one contender wins), check it
# still holds the stale PID, then create the pid file (noclobber: it must not exist). A file whose content is not the
# stale PID (a live run's pid) is never deleted: it is put back, or left where it is.
grab_slot_claim() {
	mv "$1" "$gs_d/pid.$$" 2>/dev/null || return 1
	gs_x=$(cat "$gs_d/pid.$$" 2>/dev/null || true)
	if [ "$gs_x" = "$2" ]; then
		if (set -C; echo $$ >"$gs_d/pid") 2>/dev/null; then
			rm -f "$gs_d/pid.$$"
			return 0
		fi
		rm -f "$gs_d/pid.$$" # the stale content only; somebody else has created the pid file
		return 1
	fi
	[ -e "$1" ] || mv "$gs_d/pid.$$" "$1" 2>/dev/null || true
	return 1
}
# grab_slot N: take slot-N when it is free or its holder is gone; 0 on success, 1 when someone else holds it.
# Stale: the pid file names a dead process, or is empty and more than a minute old (a crash between its creation
# and its write); or there is no pid file and a pid.N claim file of a dead claimer N, or more than a minute old (a
# crash after the rename; N may be a reused pid) holding a dead pid; or an empty slot more than a minute old (a
# crash between mkdir and the pid file).
grab_slot() {
	gs_d=$slotdir/slot-$1
	if mkdir "$gs_d" 2>/dev/null; then
		echo $$ >"$gs_d/pid"
		return 0
	fi
	if [ -e "$gs_d/pid" ]; then
		gs_p=$(cat "$gs_d/pid" 2>/dev/null || true)
		if [ -n "$gs_p" ]; then
			pid_gone "$gs_p" || return 1
		else
			[ -n "$(find "$gs_d/pid" -mmin +1 2>/dev/null)" ] || return 1
		fi
		grab_slot_claim "$gs_d/pid" "$gs_p" || return 1
		echo "dmn_all: removed the stale slot-$1 (holder '$gs_p' is gone)" >&2
		return 0
	fi
	gs_k=
	for gs_c in "$gs_d"/pid.*; do
		[ -f "$gs_c" ] || continue
		gs_k=1
		# a claim in progress (its claimer is alive and the file is recent): leave the slot alone
		if ! pid_gone "${gs_c##*/pid.}" && [ -z "$(find "$gs_c" -mmin +1 2>/dev/null)" ]; then
			return 1
		fi
	done
	for gs_c in "$gs_d"/pid.*; do
		[ -f "$gs_c" ] || continue
		gs_p=$(cat "$gs_c" 2>/dev/null || true)
		if [ -n "$gs_p" ] && pid_gone "$gs_p"; then
			grab_slot_claim "$gs_c" "$gs_p" || return 1
			echo "dmn_all: removed the stale slot-$1 (claim of a gone run, holder $gs_p is gone)" >&2
			return 0
		fi
		# the claimer died holding a live pid: give the pid file back
		[ ! -e "$gs_d/pid" ] && mv "$gs_c" "$gs_d/pid" 2>/dev/null || true
		return 1
	done
	[ -z "$gs_k" ] || return 1
	# an empty slot that is more than a minute old
	[ -n "$(find "$gs_d" -maxdepth 0 -mmin +1 2>/dev/null)" ] || return 1
	(set -C; echo $$ >"$gs_d/pid") 2>/dev/null || return 1
	echo "dmn_all: took the abandoned slot-$1 (no pid file)" >&2
	return 0
}
# release_one N: give slot-N back, but only when its pid file holds this shell's pid
release_one() {
	[ "$(cat "$slotdir/slot-$1/pid" 2>/dev/null || true)" = "$$" ] || return 0
	rm -f "$slotdir/slot-$1/pid"
	rmdir "$slotdir/slot-$1" 2>/dev/null || true
}
take_slot() {
	[ -n "$slotdir" ] || return 0
	while [ -z "$slot" ]; do
		for s in ${NTFSMAC_VM_SLOTS:-1 2}; do
			if grab_slot "$s"; then
				slot=$s
				break
			fi
		done
		[ -n "$slot" ] || sleep 30
	done
}
release_slot() {
	[ -z "$slot" ] || release_one "$slot"
	slot=
}

# ---- the second slot (two guests at once): tried, never waited for --------------------------------------------------
# A second guest needs a slot of its own: from --slot-dir (the run takes all its slots there), or, when the caller holds
# the first slot, from --lane-slot-dir. Two runs that each hold one slot and wait for another would never finish, so a
# busy slot means one guest at a time, never a wait.
slot2=
sdir2=${slotdir:-$laneslots}
try_second_slot() {
	[ -z "$slot2" ] || return 0
	[ -n "$sdir2" ] || return 1
	ts_keep=$slotdir
	slotdir=$sdir2
	mkdir -p "$slotdir" 2>/dev/null || true
	for ts_s in ${NTFSMAC_VM_SLOTS:-1 2}; do
		[ "$ts_s" != "$slot" ] || continue
		if grab_slot "$ts_s"; then
			slot2=$ts_s
			break
		fi
	done
	slotdir=$ts_keep
	[ -n "$slot2" ]
}
release_second_slot() {
	[ -n "$slot2" ] || return 0
	rs_keep=$slotdir
	slotdir=$sdir2
	release_one "$slot2"
	slotdir=$rs_keep
	slot2=
}

# ---- drivers -----------------------------------------------------------------------------------------------------
# deps_args DRIVER: depsargs for attach and dmn. A tar that is there at the start is used by both (each checks its key);
# without one, the first of the two builds NTFS-3G and saves it, and the other uses it only once it is complete. A
# writer that ended without saving it hands the job on (deps-writer-done; mv: one taker): the next one builds and saves
# it, as in the old order, where dmn saved the tar when attach had not.
deps_pre=0
[ -z "$deps_tar" ] || [ ! -f "$deps_tar" ] || deps_pre=1
deps_writer=0
deps_args() {
	depsargs=
	deps_writer=0
	[ -n "$deps_tar" ] || return 0
	if [ "$deps_pre" -eq 1 ] || [ -e "$state/deps-saved" ]; then
		depsargs="--deps-tar $deps_tar"
	elif mkdir "$state/deps-writer" 2>/dev/null || mv "$state/deps-writer-done" "$state/deps-taken.$1" 2>/dev/null; then
		depsargs="--deps-tar $deps_tar"
		deps_writer=1
	else
		echo "dmn_all: $1 builds NTFS-3G itself and saves nothing: $deps_tar is being made by the driver in the other guest"
	fi
}
# driver_cmd DRIVER OUT: cmd, the command of that driver
driver_cmd() {
	case $1 in
	spawn) cmd="sh $here/spawn_vm.sh --out $2" ;;
	spawn-sbx) cmd="sh $here/spawn_vm.sh --sandbox --out $2" ;;
	sandbox) cmd="sh $here/sandbox_vm.sh --out $2" ;;
	errwatch) cmd="sh $here/errwatch_vm.sh --out $2" ;;
	journal) cmd="sh $here/journal_vm.sh --out $2" ;;
	xpc) cmd="sh $here/xpc_vm.sh --out $2" ;;
	mirror) cmd="sh $here/dmn_mirror_vm.sh --out $2" ;;
	attach) deps_args attach; cmd="sh $here/attach_vm.sh --fixtures $fx $depsargs --out $2" ;;
	dmn) deps_args dmn; cmd="sh $here/dmn_vm.sh --fixtures $fx $depsargs --out $2 classes kill unmount labels priv slow u15 finder" ;;
	esac
}
summary=$outdir/summary.txt
rd_pid=
# write_summary TAG: summary.txt from the lines of the drivers that have ended, in driver order (atomic: written beside
# it and renamed, so the two guests' lanes never mix their writes); kept on an interrupt
write_summary() {
	ws_t=$state/summary.$1.tmp
	: >"$ws_t"
	for ws_d in $drivers; do
		[ ! -f "$state/res/$ws_d" ] || cat "$state/res/$ws_d" >>"$ws_t"
	done
	mv "$ws_t" "$summary"
}
# kill_tree PID: TERM to PID and all its descendants (a driver waits for its own foreground command before its trap runs;
# its commands get TERM as well, as from vm/macos/run-suite.sh stop_stages, and the driver's trap deletes its guest)
kill_tree() {
	kt_all=$1
	kt_new=$1
	while [ -n "$kt_new" ]; do
		kt_next=
		for kt_p in $kt_new; do
			kt_next="$kt_next $(ps -axo pid=,ppid= | awk -v p="$kt_p" '$2 == p { print $1 }')"
		done
		kt_new=$(echo $kt_next)
		kt_all="$kt_all $kt_new"
	done
	# shellcheck disable=SC2086
	kill -TERM $kt_all 2>/dev/null || true
}
# run_driver DRIVER [GUEST]: run it (in GUEST, a shared guest, when given); its summary line goes to the output and to
# the state; rd_rc is its exit status
run_driver() {
	rd_out=$outdir/$1.txt
	deps_writer=0
	driver_cmd "$1" "$rd_out"
	[ -z "${2:-}" ] || cmd="$cmd --guest $2"
	echo "dmn_all: === $1: $cmd"
	rd_t0=$(date +%s)
	rd_rc=0
	# in the background and waited for, so that a signal to this shell is handled at once (kill_tree in on_exit)
	# shellcheck disable=SC2086
	$cmd >"$outdir/$1.log" 2>&1 </dev/null &
	rd_pid=$!
	wait "$rd_pid" || rd_rc=$?
	rd_pid=
	if [ "$deps_writer" -eq 1 ]; then
		if [ -f "$deps_tar" ]; then : >"$state/deps-saved"; else mkdir "$state/deps-writer-done" 2>/dev/null || true; fi
	fi
	rd_p=$(grep -c '^PASS' "$rd_out" 2>/dev/null || true)
	rd_f=$(grep -c '^FAIL' "$rd_out" 2>/dev/null || true)
	rd_line=$(printf '%-10s exit %s  PASS %s  FAIL %s' "$1" "$rd_rc" "${rd_p:-0}" "${rd_f:-0}")
	echo "$rd_line"
	echo "$rd_line" >"$state/res/$1.tmp" && mv "$state/res/$1.tmp" "$state/res/$1"
	write_summary "$1"
	echo "dmn_all: $1 took $(($(date +%s) - rd_t0)) s${2:+ in the shared guest $2}"
}

# ---- the shared guest --------------------------------------------------------------------------------------------
# guest_state NAME: what a driver could leave behind that another one would see (see the header), as text. macFUSE's own
# two services (its FSKit module io.macfuse.app.fsmodule.macfuse-local and
# /Library/PrivilegedHelperTools/io.macfuse.app.launchservice.daemon) are started by the system at the first macFUSE mount
# of a boot and kept (seen in the run of 2026-11-25: the only difference after spawn). They are listed apart, after
# "-- macfuse", with their pids: share_clean compares them with a second baseline taken after the first driver.
guest_state() {
	"$tart_sh" exec "$1" -- 'echo "-- volumes"; ls -A /Volumes
echo "-- mounts"; mount | sort
echo "-- images"; hdiutil info 2>/dev/null | grep "^image-path" | sort
echo "-- processes"; ps -axo comm= | grep -E "ntfs-3g|ntfsmac|fuse|_vm\$|sandbox_|jstub|spawn_stub" |
	grep -v -e "/io\.macfuse\.app\.fsmodule\.macfuse-local\$" -e "^/Library/PrivilegedHelperTools/io\.macfuse\.app\.launchservice\.daemon\$" | sort
echo "-- launchd"; sudo -n /bin/launchctl print system 2>/dev/null | grep -o "org\.ntfs-macos[A-Za-z0-9._-]*" | sort -u
echo "-- installed"; ls -d /Library/LaunchDaemons/org.ntfs-macos.* /usr/local/libexec/ntfs-macos "/Library/Application Support/ntfs-macos" /var/db/ntfs-macos /usr/local/bin/ntfsmac 2>/dev/null
echo "-- work"; ls -d /tmp/spawnvm.* /tmp/errwatchvm.* /tmp/journalvm.* /tmp/dmnmirror.* /tmp/sandboxvm.* /tmp/xpcvm.* 2>/dev/null
echo "-- macfuse"; ps -axo pid=,stat=,comm= | grep -E "/io\.macfuse\.app\.fsmodule\.macfuse-local\$|^ *[0-9]+ +[A-Za-z+<>]+ +/Library/PrivilegedHelperTools/io\.macfuse\.app\.launchservice\.daemon\$" |
	awk "{ print \$1, \$3, (\$2 ~ /^[TZ]/ ? \"wedged:\" \$2 : \"ok\") }" | sort -k2
true'
}
# mf_ok FILE: the macFUSE part of a state is acceptable as a baseline: at most one instance of each service, none
# stopped or a zombie (a `wedged:` line)
mf_ok() {
	! grep -q " wedged:" "$1" && [ -z "$(awk '{ print $2 }' "$1" | sort | uniq -d)" ]
}
g=
share_n=0
share_broken=0
# share_up: clone and boot a shared guest and record its state; g is its name, empty when that failed
share_up() {
	share_n=$((share_n + 1))
	g=ntfsmac-share$share_n-$$ # ends in this run's pid: vm/macos/run-suite.sh sweep_clones finds it after a kill
	echo "dmn_all: shared guest $g: clone and boot"
	if "$tart_sh" clone "$g" --from golden >>"$outdir/share.log" 2>&1 && "$tart_sh" run "$g" >>"$outdir/share.log" 2>&1 &&
		guest_state "$g" >"$state/share/$g.base" 2>>"$outdir/share.log"; then
		return 0
	fi
	echo "dmn_all: WARNING: the shared guest $g could not be set up (share.log): each remaining driver of the group clones its own"
	share_broken=1
	share_down
	return 1
}
share_down() {
	[ -n "$g" ] || return 0
	"$tart_sh" delete "$g" >>"$outdir/share.log" 2>&1 || echo "dmn_all: WARNING: could not delete $g" >&2
	g=
}
# share_clean: 0 when the shared guest's state is again the one recorded after its boot, and macFUSE's services are those
# recorded after the first driver (their second baseline: same pids, one instance each, none stopped; a second instance,
# a restart or a wedged one retires the guest)
share_clean() {
	sc_i=0
	b=$state/share/$g
	sed '/^-- macfuse$/,$d' "$b.base" >"$b.main"
	while :; do
		if guest_state "$g" >"$b.now" 2>>"$outdir/share.log"; then
			sed '/^-- macfuse$/,$d' "$b.now" >"$b.nmain"
			sed '1,/^-- macfuse$/d' "$b.now" >"$b.nmf"
			if cmp -s "$b.main" "$b.nmain"; then
				if [ ! -s "$b.mf" ]; then # none yet: the first driver that starts them sets the baseline
					if mf_ok "$b.nmf"; then
						cp "$b.nmf" "$b.mf"
						[ ! -s "$b.mf" ] || echo "dmn_all: macFUSE services of $g after its first driver (second baseline): $(tr '\n' ';' <"$b.mf")"
						return 0
					fi
				elif cmp -s "$b.mf" "$b.nmf"; then
					return 0
				fi
			fi
		fi
		sc_i=$((sc_i + 1))
		[ "$sc_i" -lt 3 ] || break
		sleep 5
	done
	echo "dmn_all: the state of $g differs from the one after its boot (or its macFUSE services from their baseline):"
	diff "$b.main" "$b.nmain" 2>/dev/null | sed 's/^/dmn_all:   /' || true
	if [ -f "$b.mf" ]; then
		diff "$b.mf" "$b.nmf" 2>/dev/null | sed 's/^/dmn_all:   macfuse /' || true
	else
		sed 's/^/dmn_all:   macfuse (one instance each, none stopped, expected) /' "$b.nmf" 2>/dev/null || true
	fi
	return 1
}
# run_shared: the drivers of the shared group, one after the other, in one guest while it stays clean
run_shared() {
	g=
	for rs_dr in $share_drivers; do
		[ ! -e "$state/stop" ] || break
		if [ -n "$g" ] && ! share_clean; then
			echo "dmn_all: $g is deleted; the next driver gets a fresh shared guest"
			share_down
		fi
		[ -n "$g" ] || [ "$share_broken" -eq 1 ] || share_up || true
		run_driver "$rs_dr" "$g"
		if [ -n "$g" ] && [ "$rd_rc" -ne 0 ]; then
			echo "dmn_all: $rs_dr failed in $g: that guest is deleted, the next driver gets a fresh one"
			share_down
		fi
	done
	share_down
}

# ---- jobs and lanes ----------------------------------------------------------------------------------------------
share_drivers=
own=
for dr in $drivers; do
	case $dr in
	spawn | spawn-sbx | errwatch | journal | mirror) share_drivers="$share_drivers $dr" ;;
	*) own="$own $dr" ;;
	esac
done
set -- $share_drivers
[ $# -ge 2 ] || { own="$own $share_drivers"; share_drivers=; }
# longest first (run of 2026-11-25, seconds: dmn 1461..1477, the shared group about 300 with its boot, sandbox 74,
# attach 122, xpc 40; docs/TEST_STRATEGY.md section 4)
jobs=
for j in dmn shared sandbox attach journal mirror errwatch spawn spawn-sbx xpc; do
	if [ "$j" = shared ]; then
		[ -z "$share_drivers" ] || jobs="$jobs shared"
	else
		case " $own " in *" $j "*) jobs="$jobs $j" ;; esac
	fi
done
unclaimed() {
	uc_n=0
	for uc_j in $jobs; do [ -d "$state/claim/$uc_j" ] || uc_n=$((uc_n + 1)); done
	echo "$uc_n"
}
# run_lane N: take the next job nobody has taken (mkdir is the claim) until none is left
run_lane() {
	for rl_j in $jobs; do
		[ "$1" -ne 1 ] || maybe_lane2
		[ ! -e "$state/stop" ] || break
		mkdir "$state/claim/$rl_j" 2>/dev/null || continue
		if [ "$rl_j" = shared ]; then run_shared; else run_driver "$rl_j"; fi
	done
}
lane2_pid=
# maybe_lane2: start the second lane when it is not running, at least two jobs are left and a second slot is free now
maybe_lane2() {
	[ "$lanes" -eq 2 ] || return 0
	if [ -n "$lane2_pid" ]; then
		[ -e "$state/lane2.done" ] || return 0
		wait "$lane2_pid" || true
		lane2_pid=
		slot2= # released by the lane itself
		rm -f "$state/lane2.done"
	fi
	[ "$(unclaimed)" -ge 2 ] || return 0
	try_second_slot || return 0
	echo "dmn_all: holding slot-$slot2 as well: a second guest runs next to the first"
	(
		slot= # the first lane's slot is not this lane's to release
		lane2_exit() {
			trap '' INT TERM HUP
			[ -z "$rd_pid" ] || { kill_tree "$rd_pid"; wait "$rd_pid" 2>/dev/null || true; }
			share_down
			release_second_slot
			: >"$state/lane2.done"
		}
		trap lane2_exit EXIT
		trap 'exit 130' INT TERM HUP
		run_lane 2
	) &
	lane2_pid=$!
}

on_exit() {
	oe_rc=$?
	trap '' INT TERM HUP
	: >"$state/stop"
	# the drivers of both lanes and all their commands get TERM (their traps delete their guests), then are waited for
	[ -z "$rd_pid" ] || kill_tree "$rd_pid"
	[ -z "$lane2_pid" ] || kill_tree "$lane2_pid"
	[ -z "$rd_pid" ] || wait "$rd_pid" 2>/dev/null || true
	[ -z "$lane2_pid" ] || wait "$lane2_pid" 2>/dev/null || true
	share_down
	release_second_slot
	release_slot
	# the lines of the drivers that ended stay in summary.txt (the final one below has every driver)
	[ "$oe_rc" -eq 0 ] || [ "$oe_rc" -eq 1 ] || write_summary exit 2>/dev/null || true
	rm -rf "$state"
	exit "$oe_rc"
}
trap on_exit EXIT
trap 'exit 130' INT TERM HUP

t_start=$(date +%s)
if [ "$share" -eq 0 ]; then
	# the old order: every driver its own guest, one after the other, a slot around each
	for dr in $drivers; do
		take_slot
		run_driver "$dr"
		release_slot
	done
else
	echo "dmn_all: jobs, longest first:$(echo "$jobs" | sed "s/ shared/ shared($(echo $share_drivers | tr ' ' ','))/")"
	take_slot
	run_lane 1
	release_slot # this lane is done: the slot is free for others while the second lane ends
	if [ -n "$lane2_pid" ]; then
		wait "$lane2_pid" || true
		lane2_pid=
		slot2=
	fi
fi

bad=0
for dr in $drivers; do
	if [ -f "$state/res/$dr" ]; then
		case $(cat "$state/res/$dr") in *" exit 0 "*) ;; *) bad=1 ;; esac
	else
		# never run (a lane ended before it): a setup error
		printf '%-10s exit %s  PASS %s  FAIL %s\n' "$dr" 2 0 0 >"$state/res/$dr"
		bad=1
	fi
done
write_summary final
echo "dmn_all: wall time $(($(date +%s) - t_start)) s"
echo "dmn_all: summary, in driver order ($summary):"
cat "$summary"
exit "$bad"
