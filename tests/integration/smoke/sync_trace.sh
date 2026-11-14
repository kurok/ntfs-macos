#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M2-06 (U4): the sync trace. Which FUSE opcodes reach the NTFS-3G worker on the macFUSE FSKit
# backend for write, close, fsync, fcntl(F_FULLFSYNC), sync(2) and unmount, and how long after
# write() returns FUSE_WRITE arrives.
#
# Host side (default): clone a disposable tart VM from the golden VM, boot it, build NTFS-3G in the
# guest as tests/harness/run-env-b.sh does, compile sync_probe.c, run this same script in the guest
# with --guest, copy the logs to OUT, and always delete the VM (also on failure or interrupt).
# Guest side (--guest): everything happens on NTFS image FILES that the script creates with mkntfs
# in ~/ntfsmac/m206 (never a host disk, never the guest boot disk), mounted as the normal user
# with `-o backend=fskit,local,norecover,uid,gid,debug,no_detach` (norecover always; never
# recover, remove_hiberfile or fsname). libfuse2's debug output (`-o debug`) prints one
# "unique: N, opcode: NAME" line per request; each line is stamped with CLOCK_REALTIME as it is
# read, and sync_probe prints the same clock, so the two logs merge into one timeline.
# Mount points must be absolute paths (a relative one never mounts). No sudo anywhere.
#
# Sessions (each on a fresh image, each unmounted with /sbin/umount):
#   steps    sync_probe steps: create+write+close, fsync, F_FULLFSYNC, sync(2), clean-fd fsync,
#            directory fsync, rename, unlink, at 4 KiB, 1 MiB and 64 MiB
#   delay    sync_probe delay: write, hold the descriptor open, close; RUNS runs per size
#   dirty    dd 64 MiB, then umount at once (and again after sync); remount read-only and compare
#   enospc   fill a 24 MiB volume and look where the error shows (write, fsync, close, or never)
#
# Usage: sync_trace.sh [--out DIR] [--runs N] [--vm NAME]
#        sync_trace.sh --guest WORKDIR OUTDIR RUNS     (internal: the guest half)
#   --out   evidence directory (default tests/results/evidence/m206)
#   --runs  runs per size in the delay session (default 5)
#   --vm    use this running VM (short name) instead of cloning one; it is not deleted
# Exit: 0 all sessions ran, 1 a session failed, 2 setup error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)

# ---------------------------------------------------------------------------------------------
# Guest half
# ---------------------------------------------------------------------------------------------
guest_main() {
	work=$1
	out=$2
	runs=$3
	PATH=$HOME/ntfsmac/deps/bin:$HOME/ntfsmac/deps/sbin:/sbin:/usr/sbin:$PATH
	export PATH
	probe=$HOME/ntfsmac/sync_probe
	uidgid="uid=$(id -u),gid=$(id -g)"
	mkdir -p "${work:?}" "${out:?}"
	cd "$work"

	# Timestamp every line read from stdin with CLOCK_REALTIME (the clock sync_probe logs).
	stamp_lines() { perl -MTime::HiRes=time -e '$| = 1; while (<STDIN>) { printf "%.6f %s", time, $_ }'; }
	# A marker from the shell, in sync_probe's format.
	pmark() { perl -MTime::HiRes=time -e 'printf "P 0 %.9f %s\n", time, join(" ", @ARGV)' "$@" >>"$plog"; }
	mounted() { /sbin/mount | grep -F " on $mnt (" >/dev/null 2>&1; }
	worker_up() { pgrep -f "ntfs-3g .*$img" >/dev/null 2>&1; }

	mkimg() { # mkimg NAME MiB
		img=$work/$1.img
		rm -f "${img:?}"
		dd if=/dev/zero of="$img" bs=1m count=0 seek="$2" >/dev/null 2>&1
		mkntfs -F -Q -L "m206$1" "$img" >"$out/mkntfs-$1.log" 2>&1
	}

	# mount_img [EXTRA_OPTS]: debug mount of $img on $mnt; the FUSE log goes, stamped, to $flog.
	mount_img() {
		rm -rf "${mnt:?}"
		mkdir -p "$mnt"
		pmark mount_begin
		(ntfs-3g -o "backend=fskit,local,norecover,$uidgid,debug,no_detach${1:+,$1}" "$img" "$mnt" 2>&1 |
			stamp_lines >>"$flog") &
		i=0
		while [ "$i" -lt 60 ] && ! mounted; do
			sleep 0.5
			i=$((i + 1))
		done
		mounted || { echo "sync_trace(guest): mount of $img did not appear" >&2; return 1; }
		pmark mount_end
		sleep 2
	}

	umount_img() {
		pmark umount_begin
		urc=0
		/sbin/umount "$mnt" || urc=$?
		pmark umount_end "rc=$urc"
		i=0
		while [ "$i" -lt 60 ] && worker_up; do
			sleep 0.5
			i=$((i + 1))
		done
		if worker_up; then
			pmark worker_still_running_killing
			pkill -9 -f "ntfs-3g .*$img" || true
		else
			pmark worker_exited
		fi
		sleep 1
		return "$urc"
	}

	# merge NAME: probe markers (P) and stamped FUSE lines (F) -> one timeline, plus a table
	# "marker | opcodes that arrived before the next marker, with count and first offset".
	merge() {
		awk '$1 == "P" { printf "%s P", $3; for (i = 4; i <= NF; i++) printf " %s", $i; printf "\n" }' "$plog" >"$work/$1.p"
		awk '
			{ ts = $1; $1 = ""; sub(/^ /, "") }
			/^unique: [0-9]+, opcode:/ { print ts " F " $0; next }
			/^ +unique: [0-9]+, error/ { sub(/^ +/, ""); print ts " F    reply " $0; next }
			/^ +unique: [0-9]+, success/ { next }
			/^ / { next }
			/^(INIT|flags|max_)/ { next }
			{ print ts " F    " $0 }' "$flog" >"$work/$1.f"
		sort -n -k1,1 -s "$work/$1.p" "$work/$1.f" >"$work/$1.m"
		awk 'NR == 1 { t0 = $1 } { r = $1 - t0; $1 = ""; printf "+%.6f%s\n", r, $0 }' "$work/$1.m" >"$out/timeline-$1.txt"
		awk '
		function flush(   i, s) {
			if (ev == "") return
			s = ""
			for (i = 1; i <= n; i++) s = s sprintf(" %s x%d@+%.0fms", order[i], cnt[order[i]], (first[order[i]] - evt) * 1000)
			printf "%-66s |%s\n", ev, (n ? s : " -")
			n = 0
			delete cnt
			delete first
		}
		$2 == "P" { flush(); ev = substr($0, index($0, " P ") + 3); evt = $1; next }
		$2 == "F" && /opcode: / {
			for (i = 3; i <= NF; i++) if ($i == "opcode:") { op = $(i + 1); break }
			if (ev == "") { ev = "(before first marker)"; evt = $1 }
			if (!(op in cnt)) { order[++n] = op; first[op] = $1 }
			cnt[op]++
		}
		END { flush() }' "$work/$1.m" >"$out/opcodes-$1.txt"
	}

	session_begin() { # session_begin NAME MiB
		sname=$1
		mnt=$work/mnt-$1
		plog=$work/$1.plog
		flog=$work/$1.flog
		: >"$plog"
		: >"$flog"
		mkimg "$1" "$2"
	}
	session_end() {
		cp "$flog" "$out/fuse-$sname.log"
		cp "$plog" "$out/probe-$sname.log"
		merge "$sname"
	}

	{
		echo "date: $(date -u +%Y-%m-%dT%H:%M:%SZ)"
		sw_vers
		uname -a
		ntfs-3g --version 2>&1 | head -2
		ls -l /usr/local/lib/libfuse.2.dylib
	} >"$out/env.txt" 2>&1

	failed=0

	# ---- steps ----
	echo "sync_trace(guest): session steps" >&2
	session_begin steps 256
	mount_img
	"$probe" steps "$mnt" 1500 >>"$plog" || failed=1
	pmark pre_umount_idle
	sleep 3
	umount_img || failed=1
	session_end
	ntfsfix -n "$img" >"$out/ntfsfix-steps.log" 2>&1 || true

	# ---- delay ----
	echo "sync_trace(guest): session delay" >&2
	session_begin delay 256
	mount_img
	for sz in 4096 1048576 67108864; do
		n=1
		while [ "$n" -le "$runs" ]; do
			pmark run "size=$sz" "n=$n"
			"$probe" delay "$mnt" "$sz" 6000 >>"$plog" || failed=1
			n=$((n + 1))
		done
	done
	umount_img || failed=1
	session_end
	awk '
		BEGIN { print "size\trun\twrite_call_ms\tfirst_write_after_write_ms\tlast_write_after_write_ms\tn_write\tfirst_write_before_close\tflush_after_close_begin_ms\trelease_after_close_begin_ms" }
		$2 == "P" && $3 == "run" { split($4, a, "="); size = a[2]; split($5, b, "="); run = b[2]; nw = 0; first = ""; last = ""; flush = ""; rel = ""; cb = "" }
		$2 == "P" && $3 == "write_begin" { wb = $1; inrun = 1 }
		$2 == "P" && $3 == "write_end" { we = $1 }
		$2 == "P" && $3 == "close_begin" { cb = $1 }
		$2 == "F" && inrun && /opcode: WRITE/ { nw++; if (first == "") first = $1; last = $1 }
		$2 == "F" && inrun && /opcode: FLUSH/ && cb != "" && flush == "" { flush = $1 }
		$2 == "F" && inrun && /opcode: RELEASE/ && cb != "" && rel == "" { rel = $1 }
		$2 == "P" && $3 == "unlink_begin" && inrun {
			printf "%s\t%s\t%.1f\t%s\t%s\t%d\t%s\t%s\t%s\n", size, run, (we - wb) * 1000,
				(first == "" ? "-" : sprintf("%.1f", (first - we) * 1000)),
				(last == "" ? "-" : sprintf("%.1f", (last - we) * 1000)), nw,
				(first == "" ? "-" : (first < cb ? "yes" : "no")),
				(flush == "" ? "-" : sprintf("%.1f", (flush - cb) * 1000)),
				(rel == "" ? "-" : sprintf("%.1f", (rel - cb) * 1000))
			inrun = 0
		}' "$work/delay.m" >"$out/delay.tsv"

	# ---- dirty unmount ----
	echo "sync_trace(guest): session dirty" >&2
	dd if=/dev/urandom of="$work/src64.bin" bs=1m count=64 >/dev/null 2>&1
	for variant in plain synced; do
		session_begin "dirty-$variant" 256
		mount_img
		pmark dd_begin
		dd if="$work/src64.bin" of="$mnt/dirty.bin" bs=1m >"$out/dd-dirty-$variant.log" 2>&1 || failed=1
		pmark dd_end
		if [ "$variant" = synced ]; then
			pmark sync_begin
			sync
			pmark sync_end
			sleep 3
		fi
		umount_img || failed=1
		session_end
		# Remount read-only and compare the data with the source.
		plog=$work/$sname.chk.plog
		flog=$work/$sname.chk.flog
		: >"$plog"
		: >"$flog"
		mount_img ro
		{
			ls -l "$mnt/dirty.bin"
			if cmp "$work/src64.bin" "$mnt/dirty.bin"; then echo "CMP: identical"; else echo "CMP: DIFFERENT"; fi
		} >"$out/check-dirty-$variant.log" 2>&1 || true
		umount_img || failed=1
		cp "$flog" "$out/fuse-dirty-$variant-remount.log"
		ntfsfix -n "$img" >"$out/ntfsfix-dirty-$variant.log" 2>&1 || true
	done

	# ---- ENOSPC ----
	echo "sync_trace(guest): session enospc" >&2
	session_begin enospc 24
	mount_img
	"$probe" enospc "$mnt" 1048576 >>"$plog" || failed=1
	pmark pre_umount_idle
	sleep 2
	umount_img || failed=1
	session_end

	rm -f "${work:?}/src64.bin"
	for f in "$work"/*.img; do rm -f "${f:?}"; done
	return "$failed"
}

# ---------------------------------------------------------------------------------------------
# Host half
# ---------------------------------------------------------------------------------------------
if [ "${1:-}" = "--guest" ]; then
	[ $# -eq 4 ] || { echo "usage: sync_trace.sh --guest WORKDIR OUTDIR RUNS" >&2; exit 2; }
	guest_main "$2" "$3" "$4"
	exit $?
fi

root=$(CDPATH='' cd -- "$here/../../.." && pwd)
tart_sh=$root/vm/macos/tart.sh
out=$root/tests/results/evidence/m206
runs=5
vm=
while [ $# -gt 0 ]; do
	case $1 in
	--out) [ $# -ge 2 ] || { echo "sync_trace: --out needs a directory" >&2; exit 2; }; out=$2; shift 2 ;;
	--runs) [ $# -ge 2 ] || { echo "sync_trace: --runs needs a number" >&2; exit 2; }; runs=$2; shift 2 ;;
	--vm) [ $# -ge 2 ] || { echo "sync_trace: --vm needs a name" >&2; exit 2; }; vm=$2; shift 2 ;;
	-h | --help) sed -n '2,32p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	*) echo "sync_trace: unknown option: $1" >&2; exit 2 ;;
	esac
done
case $runs in '' | *[!0-9]* | 0) echo "sync_trace: --runs must be a positive number" >&2; exit 2 ;; esac

own_vm=0
if [ -z "$vm" ]; then
	vm=m206
	own_vm=1
fi
vmfull=ntfsmac-${vm#ntfsmac-}

# shellcheck disable=SC2329 # runs from the EXIT trap
cleanup() {
	if [ "$own_vm" = 1 ]; then
		"$tart_sh" delete "$vm" >&2 || echo "sync_trace: WARNING: could not delete $vm" >&2
	fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' "$root/third_party/deps.lock")
[ -n "$tarball" ] || { echo "sync_trace: no ntfs-3g entry in third_party/deps.lock" >&2; exit 2; }
[ -f "$root/third_party/cache/$tarball" ] || sh "$root/scripts/fetch-deps.sh" ntfs-3g >&2

if [ "$own_vm" = 1 ]; then
	"$tart_sh" clone "$vm" --from golden >&2
	"$tart_sh" run "$vm" >&2
fi

echo "sync_trace: building NTFS-3G and the probe in the guest" >&2
(cd "$root" && tar -cf - scripts/build-ntfs3g.sh scripts/fetch-deps.sh third_party/deps.lock \
	third_party/patches "third_party/cache/$tarball" tests/integration/smoke/sync_probe.c \
	tests/integration/smoke/sync_trace.sh) |
	tart exec -i "$vmfull" /bin/sh -c 'rm -rf ~/ntfsmac/tree && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree'
"$tart_sh" exec "$vm" -- \
	'cd ~/ntfsmac/tree && sh scripts/build-ntfs3g.sh --fuse external --jobs 4 --prefix ~/ntfsmac/deps --build-dir ~/ntfsmac/build 2>&1 | tail -2 && cc -Wall -Wextra -Werror -o ~/ntfsmac/sync_probe tests/integration/smoke/sync_probe.c' >&2 ||
	{ echo "sync_trace: guest build failed" >&2; exit 2; }

mkdir -p "$out"
rc=0
"$tart_sh" exec "$vm" -- \
	"rm -rf ~/ntfsmac/m206-out; sh ~/ntfsmac/tree/tests/integration/smoke/sync_trace.sh --guest \$HOME/ntfsmac/m206 \$HOME/ntfsmac/m206-out $runs" >&2 || rc=1
"$tart_sh" exec "$vm" -- 'cd ~/ntfsmac/m206-out && tar -cf - .' | tar -xf - -C "$out" ||
	{ echo "sync_trace: could not copy the logs back" >&2; exit 2; }
echo "sync_trace: logs in $out (guest sessions exit $rc)" >&2
exit "$rc"
