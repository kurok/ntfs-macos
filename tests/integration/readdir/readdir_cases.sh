#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# I-RDIR-01..05 (patch 0014, ADR-027): directory listing through NTFS-3G on macFUSE's FSKit backend.
# A regression check for macFUSE and NTFS-3G updates: the FSKit module serves several enumerations
# from one FUSE directory handle and rejected a READDIR reply that ends inside an entry (EINVAL).
#
# Runs IN THE macOS GUEST (Environment B), as an unprivileged user, on an NTFS image FILE that the
# caller made (never a host disk); tests/integration/readdir/readdir_vm.sh does all of it in a
# disposable clone. Cases, each on its own directory of the mounted image:
#   I-RDIR-01  readdir + unlink of every entry, then rmdir (git and busybox rm -r do this): the
#              directory must end empty and rmdir succeed. Also run while another process lists the
#              same directory in a loop with `ls -f` and with getattrlistbulk (Finder): with macFUSE
#              5.5.0 this leaves files (the FSKit module continues every enumeration of a directory
#              from one shared FUSE handle's position, not from the caller's cookie; ADR-027), so it
#              is reported as KNOWN and does not fail the run unless -s is given; a run where it no
#              longer leaves files prints NOW-PASSES (a macFUSE fix to check). A run is KNOWN only
#              when all of these hold: readdir reported no error; every unlink failure was ENOENT
#              (a name listed twice through the shared position); the final listing worked; rmdir
#              failed with ENOTEMPTY; and 0 < files left < n/4. Anything else fails the run, as does
#              any failed listing of the lister while the directory exists (EINVAL would be a
#              regression). The alone case always fails when it leaves files
#   I-RDIR-02  seekdir to a telldir cookie after reading to the end lists the same tail again
#   I-RDIR-03  two descriptors on one directory, one hands off to the other (sequential and
#              interleaved, 2 KiB and 16 KiB getdirentries buffers): both list every name once
#   I-RDIR-04  a directory growing from INDEX_ROOT only to an index allocation during a listing: the
#              names present before are listed once, no error
#   I-RDIR-05  `ls -f` and getattrlistbulk of a directory with more than 10240 bytes of listing
#              (unpatched NTFS-3G: EINVAL)
# The mount uses the product's option set with norecover; recover, remove_hiberfile and fsname are
# never used. Exit 0 only if every check passed, 1 if one failed, 2 on a setup error.
#
# Usage: readdir_cases.sh [-m MOUNTPOINT] [-n "SIZES"] [-r REPEATS] [-s] IMAGE
#   -n  entry counts for I-RDIR-01 (default "1000 10000")
#   -r  runs per concurrent lister and size for I-RDIR-01 (default 3)
#   -s  strict: the concurrent-lister runs of I-RDIR-01 fail the run when they leave files
# Env:  NTFS3G (default: ntfs-3g from PATH), DIRTESTS (default: dirtests next to this script),
#       NTFSMAC_RDIR_WAIT (seconds to wait for the mount, default 30)
set -u

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
ntfs3g=${NTFS3G:-ntfs-3g}
dt=${DIRTESTS:-$here/dirtests}
wait_s=${NTFSMAC_RDIR_WAIT:-30}
sizes="1000 10000"
reps=3
strict=0
mnt=
img=
fails=0
PATH=$PATH:/sbin:/usr/sbin

die() { echo "readdir_cases: error: $*" >&2; exit 2; }
pass() { echo "PASS: $*"; }
fail() { echo "FAIL: $*"; fails=$((fails + 1)); }

while [ $# -gt 0 ]; do
	case $1 in
	-m) [ $# -ge 2 ] || die "-m needs a mount point"; mnt=$2; shift 2 ;;
	-n) [ $# -ge 2 ] || die "-n needs sizes"; sizes=$2; shift 2 ;;
	-r) [ $# -ge 2 ] || die "-r needs a number"; reps=$2; shift 2 ;;
	-s) strict=1; shift ;;
	-h | --help) sed -n '2,38p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option: $1" ;;
	*) [ -z "$img" ] || die "one IMAGE only"; img=$1; shift ;;
	esac
done
[ -n "$img" ] || die "usage: readdir_cases.sh [-m MOUNTPOINT] [-n SIZES] [-r REPEATS] [-s] IMAGE"
[ -f "$img" ] || die "not a regular file: $img"
[ -x "$dt" ] || die "$dt is missing (cc -O2 -o dirtests dirtests.c)"
[ "$(id -u)" != 0 ] || die "must run as a normal user, not root"
case $reps in '' | *[!0-9]* | 0) die "-r must be a positive number" ;; esac
case $img in /*) ;; *) img=$(pwd)/$img ;; esac
[ -n "$mnt" ] || mnt=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmac-rdir.XXXXXX") || die "mktemp failed"
mkdir -p "$mnt"
mnt=$(cd "$mnt" && pwd -P) || die "cannot resolve the mount point"	# `mount` shows the physical path

opts="backend=fskit,local,norecover,noatime,windows_names,nocompression,no_detach,streams_interface=openxattr,uid=$(id -u),gid=$(id -g),fmask=0133,dmask=0022"
"$ntfs3g" -o "$opts" "$img" "$mnt" >"${TMPDIR:-/tmp}/ntfsmac-rdir-worker.log" 2>&1 &
wpid=$!
i=0
until mount | grep -q " on $mnt "; do
	i=$((i + 1))
	[ $i -le "$wait_s" ] || { cat "${TMPDIR:-/tmp}/ntfsmac-rdir-worker.log" >&2; kill -9 $wpid 2>/dev/null; die "the mount did not appear"; }
	sleep 1
done

# known OUT N: true when the output of `dirtests unlink` on N entries is the known macFUSE defect
known() {
	k_left=$(echo "$1" | sed -n 's/.*, left \([0-9]*\),.*/\1/p')
	k_errs=$(echo "$1" | sed -n 's/.*unlink failures [0-9]* (\([^)]*\)).*/\1/p')	# none, or ENOENT:k[,...]
	case $k_errs in none | ENOENT:[0-9]*) ;; *) return 1 ;; esac
	case $k_errs in *,*) return 1 ;; esac	# another errno besides ENOENT
	case $1 in *"readdir error none"*"final listing ok"*"rmdir Directory not empty"*) ;; *) return 1 ;; esac
	[ -n "$k_left" ] && [ "$k_left" -gt 0 ] && [ $((k_left * 4)) -lt "$2" ]
}

mk() { # mk DIR COUNT NAMELEN
	mkdir -p "$1" && python3 -c "import os,sys; [open(os.path.join(sys.argv[1], ('%05d' % k).ljust(int(sys.argv[3]), 'q')), 'w').close() for k in range(int(sys.argv[2]))]" "$@"
}

# I-RDIR-01
for n in $sizes; do
	d=$mnt/unlink-alone-$n
	mk "$d" "$n" 58
	if out=$("$dt" unlink "$d"); then pass "I-RDIR-01 unlink $n alone ($out)"; else fail "I-RDIR-01 unlink $n alone ($out)"; fi
	for lister in ls bulk; do
		bad=0 other=0 r=1 last= lerr=$mnt/.lister-errors
		: >"$lerr"
		while [ $r -le "$reps" ]; do
			d=$mnt/unlink-$lister-$n-$r
			mk "$d" "$n" 58
			# the lister notes every listing that fails while the directory still exists (EINVAL would
			# be a regression, not the known defect)
			(while [ -d "$d" ]; do
				if [ $lister = ls ]; then ls -f "$d" >/dev/null 2>&1 || { [ -d "$d" ] && echo "$lister" >>"$lerr"; }
				else "$dt" bulk "$d" >/dev/null 2>&1 || { [ -d "$d" ] && echo "$lister" >>"$lerr"; }; fi
			done) &
			lp=$!
			if ! out=$("$dt" unlink "$d"); then
				bad=$((bad + 1))
				last=$out
				known "$out" "$n" || { other=$((other + 1)); echo "NOTE: not the known defect: $out"; }
			fi
			echo "INFO: I-RDIR-01 $lister n=$n run $r: $out"
			pkill -P $lp 2>/dev/null	# the lister's current ls or dirtests
			kill $lp 2>/dev/null
			wait $lp 2>/dev/null
			[ ! -d "$d" ] || { find "$d" -type f -delete 2>/dev/null; rmdir "$d" 2>/dev/null; }
			r=$((r + 1))
		done
		nlerr=$(wc -l <"$lerr" | tr -d ' ')
		rm -f "$lerr"
		if [ "$nlerr" -gt 0 ] || [ $other -gt 0 ]; then
			fail "I-RDIR-01 unlink $n with a concurrent $lister lister: $other run(s) failed otherwise than the known defect, $nlerr failed listing(s) (last: $last)"
		elif [ $bad -eq 0 ]; then
			pass "I-RDIR-01 unlink $n with a concurrent $lister lister: $reps of $reps runs empty"
			[ $strict -eq 1 ] || echo "NOW-PASSES: the concurrent-lister case no longer leaves files (macFUSE fixed? see ADR-027)"
		elif [ $strict -eq 1 ]; then
			fail "I-RDIR-01 unlink $n with a concurrent $lister lister: $bad of $reps runs left files (last: $last)"
		else
			echo "KNOWN: I-RDIR-01 unlink $n with a concurrent $lister lister: $bad of $reps runs left files (macFUSE; last: $last)"
		fi
	done
done

# I-RDIR-02, -03
d=$mnt/seek
mk "$d" 2000 58
for k in 10 500 1500; do
	if out=$("$dt" seekeof "$d" $k); then pass "I-RDIR-02 seekdir after the end, k=$k"; else fail "I-RDIR-02 seekdir after the end, k=$k ($out)"; fi
done
for bs in 2048 16384; do
	if out=$("$dt" handoff "$d" $bs); then pass "I-RDIR-03 handoff, buffer $bs"; else fail "I-RDIR-03 handoff, buffer $bs ($(echo "$out" | tr '\n' ' '))"; fi
done

# I-RDIR-04
d=$mnt/grow
mk "$d" 3 8
if out=$("$dt" grow "$d" 2048 400); then pass "I-RDIR-04 growth from INDEX_ROOT only"; else fail "I-RDIR-04 growth from INDEX_ROOT only ($out)"; fi

# I-RDIR-05: 116 names of 58 bytes make a listing just over 10240 bytes
d=$mnt/big
mk "$d" 400 58
if ls -f "$d" >/dev/null 2>&1; then pass "I-RDIR-05 ls -f of 400 names"; else fail "I-RDIR-05 ls -f of 400 names"; fi
if out=$("$dt" bulk "$d") && [ "$out" = "bulk: 400 names" ]; then pass "I-RDIR-05 getattrlistbulk of 400 names"; else fail "I-RDIR-05 getattrlistbulk of 400 names ($out)"; fi

# unmount; a very large session can take minutes (macFUSE), so the wait is bounded
umount "$mnt" >/dev/null 2>&1 &
up=$!
i=0
while kill -0 $up 2>/dev/null && [ $i -lt 300 ]; do sleep 1; i=$((i + 1)); done
if kill -0 $up 2>/dev/null; then
	echo "NOTE: unmount still running after 300 s (known macFUSE slowness after many items)"
else
	i=0
	while kill -0 $wpid 2>/dev/null && [ $i -lt 30 ]; do sleep 1; i=$((i + 1)); done
fi
echo "readdir_cases: $fails failed check(s)"
[ $fails -eq 0 ]
