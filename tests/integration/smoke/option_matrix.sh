#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M2-04: NTFS-3G mount option matrix on macFUSE's FSKit backend.
#
# Runs IN THE macOS GUEST (Environment B) as an unprivileged user, on copies of one NTFS image FILE
# that the caller made (never a host disk, never a block device). For every option of
# docs/ARCHITECTURE.md section 3.3 and the extra NTFS-3G / FUSE / macFUSE options of T-M2-04 it
# mounts a fresh copy of the image with
#     backend=fskit,local,norecover,<option under test>
# waits for the mount by polling, probes the effect of the option, unmounts, waits for the driver to
# exit, and logs: exact ntfs-3g exit status and output, the `mount` line, and EVIDENCE lines. The
# classification (accepted and effective / accepted but ignored / rejected) is made from this log
# and written to docs/spikes/M2-options.md and integration/mount-options.json.
#
# Safety: norecover is in every mount. recover, remove_hiberfile and fsname are never passed. A mount
# that does not appear within the deadline gets its ntfs-3g process killed with SIGKILL (SIGTERM is
# ignored in the no-mount case). The script checks at the end that no ntfs-3g process of its own is left.
#
# Usage: option_matrix.sh [-w WORKDIR] [-t SECONDS] IMAGE
# Env:   NTFS3G, NTFSFIX, MKNTFS, NTFSINFO, NTFSLS (default: the same names from PATH)
# Exit:  0 if the matrix ran and no process was left behind, 1 if something was left behind, 2 setup error.
# The probes are called by name from run_case, and `ls` output is what is under test.
# shellcheck disable=SC2329,SC2012
set -u

PATH=/sbin:/usr/sbin:$PATH
export PATH

wait_s=15
work=
img_src=
ntfs3g=${NTFS3G:-ntfs-3g}
ntfsfix=${NTFSFIX:-ntfsfix}
mkntfs=${MKNTFS:-mkntfs}
ntfsinfo=${NTFSINFO:-ntfsinfo}
ntfsls=${NTFSLS:-ntfsls}

die() { echo "option_matrix: error: $*" >&2; exit 2; }

while [ $# -gt 0 ]; do
	case $1 in
	-w) [ $# -ge 2 ] || die "-w needs a directory"; work=$2; shift 2 ;;
	-t) [ $# -ge 2 ] || die "-t needs seconds"; wait_s=$2; shift 2 ;;
	-h | --help) sed -n '2,24p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
	-*) die "unknown option: $1" ;;
	*) [ -z "$img_src" ] || die "one IMAGE only"; img_src=$1; shift ;;
	esac
done
[ -n "$img_src" ] || die "usage: option_matrix.sh [-w WORKDIR] [-t SECONDS] IMAGE"
[ -f "$img_src" ] || die "not a regular file: $img_src"
[ "$(id -u)" != 0 ] || die "must run as a normal user, not root"
case $img_src in /*) ;; *) img_src=$(pwd)/$img_src ;; esac
command -v "$ntfs3g" >/dev/null 2>&1 || [ -x "$ntfs3g" ] || die "ntfs-3g not found: $ntfs3g"

own_work=0
if [ -z "$work" ]; then
	work=$(mktemp -d "${TMPDIR:-/tmp}/ntfsmac-optmx.XXXXXX") || die "mktemp failed"
	own_work=1
fi
mkdir -p "$work" || die "cannot create $work"
work=$(cd "$work" && pwd -P) || die "bad work dir"
img=$work/var.img
mnt=$work/mnt
out=$work/ntfs3g.out
mkdir -p "$mnt"
base=backend=fskit,local,norecover
uid=$(id -u)
gid=$(id -g)
left=0

say() { printf '%s\n' "$*"; }
ev() { printf 'EVIDENCE: %s\n' "$*"; }
mounted() { /sbin/mount | grep -F " on $mnt (" >/dev/null 2>&1; }
mount_line() { /sbin/mount | grep -F " on $mnt (" | sed "s#$work#WORK#g"; }
driver_pids() { pgrep -f "ntfs-3g .*$img" 2>/dev/null; }
kill_driver() {
	p=$(driver_pids)
	[ -z "$p" ] && return 0
	say "  killing stray ntfs-3g (SIGKILL): $p"
	# shellcheck disable=SC2086 # word splitting of the pid list is intended
	kill -9 $p 2>/dev/null
	sleep 0.5
}

# shellcheck disable=SC2329 # runs from the EXIT trap
cleanup() {
	if mounted; then /sbin/umount "$mnt" >/dev/null 2>&1 || /sbin/umount -f "$mnt" >/dev/null 2>&1; fi
	kill_driver >/dev/null 2>&1
	if [ "$own_work" = 1 ]; then rm -rf "$work"; fi
}
trap cleanup EXIT
trap 'exit 130' INT TERM HUP

cur_src=$img_src
fresh_image() {
	rm -f "$img"
	cp -c "$cur_src" "$img" 2>/dev/null || cp "$cur_src" "$img" || die "cannot copy the image"
}
sha_img() { shasum -a 256 <"$img" | cut -d' ' -f1; }

poll() { # poll SECONDS CMD...: true if CMD succeeds within SECONDS
	s=$1
	shift
	i=0
	while [ "$i" -lt $((s * 5)) ]; do
		"$@" && return 0
		sleep 0.2
		i=$((i + 1))
	done
	return 1
}
no_driver() { [ -z "$(driver_pids)" ]; }
mount_or_dead() { mounted || { [ -n "$fgpid" ] && ! kill -0 "$fgpid" 2>/dev/null; }; }

# do_mount OPTS [fg]: mount IMAGE on $mnt with -o OPTS. Sets mrc (ntfs-3g exit status) and mstate
# (mounted, rejected, no-mount). With fg=1 the driver runs in the foreground (no_detach, background job)
# so that its output after daemon() is visible.
fgpid=
mrc=
mstate=
do_mount() {
	o=$1
	fg=${2:-0}
	fgpid=
	: >"$out"
	if [ "$fg" = 1 ]; then
		say "+ ntfs-3g -o $o,no_detach WORK/var.img WORK/mnt   (foreground job)"
		"$ntfs3g" -o "$o,no_detach" "$img" "$mnt" >"$out" 2>&1 &
		fgpid=$!
		poll "$wait_s" mount_or_dead
		if mounted; then mstate=mounted; mrc=running; return 0; fi
		if kill -0 "$fgpid" 2>/dev/null; then
			mstate=no-mount
			mrc=running
			kill -9 "$fgpid" 2>/dev/null
			kill_driver
		else
			wait "$fgpid"
			mrc=$?
			mstate=rejected
		fi
		return 1
	fi
	say "+ ntfs-3g -o $o WORK/var.img WORK/mnt"
	"$ntfs3g" -o "$o" "$img" "$mnt" >"$out" 2>&1
	mrc=$?
	if [ "$mrc" != 0 ]; then mstate=rejected; return 1; fi
	# ntfs-3g calls daemon() and returns before the mount is visible: poll, and give up early
	# if the daemonised driver is gone (1 s without a process) or at the deadline.
	gone=0
	i=0
	while [ "$i" -lt $((wait_s * 5)) ]; do
		if mounted; then mstate=mounted; return 0; fi
		if no_driver; then gone=$((gone + 1)); else gone=0; fi
		[ "$gone" -ge 5 ] && break
		sleep 0.2
		i=$((i + 1))
	done
	mstate=no-mount
	if [ "$gone" -ge 5 ]; then mstate=driver-exited; fi
	kill_driver
	return 1
}

do_umount() {
	sync
	if mounted; then
		/sbin/umount "$mnt" || /sbin/umount -f "$mnt" || say "  umount failed"
	fi
	if ! poll 10 no_driver; then say "  driver did not exit after umount"; kill_driver; fi
	if mounted; then say "  STILL MOUNTED after umount"; left=1; fi
}

show_out() { sed "s#$work#WORK#g" "$out" | sed 's/^/  | /'; }

summary() { # summary NAME OPTS
	say "SUMMARY|$1|$2|rc=$mrc|$mstate"
	say ""
}

# ---- probes (run with the mount in place, no arguments) ----
t=$mnt/t
mk_t() { mkdir -p "$t"; }

p_basic() {
	mk_t
	printf 'hello\n' >"$t/f.txt" && ev "write+read: $(cat "$t/f.txt")"
	ev "ls -ln file: $(ls -ln "$t/f.txt" | sed 's/  */ /g')"
	ev "ls -ldn dir: $(ls -ldn "$t" | sed 's/  */ /g')"
}
p_owner() { p_basic; }
p_ro() {
	mk_t 2>/dev/null
	if touch "$mnt/ro-write-test" 2>"$work/err"; then ev "write on this mount: ALLOWED"; rm -f "$mnt/ro-write-test"; else ev "write on this mount: REFUSED ($(cat "$work/err"))"; fi
}
p_names() {
	mk_t
	for n in 'a:b' 'CON' 'aux.txt' 'trail.' 'sp ' 'q?x' 'a"b' 'lt<x' 'a*b' 'pipe|x'; do
		if (: >"$t/$n") 2>"$work/err"; then
			ev "create '$n': OK"
			rm -f "$t/$n"
		else
			ev "create '$n': FAILED ($(sed 's#.*: ##' "$work/err" | head -1))"
		fi
	done
}
p_xattr() {
	mk_t
	f=$t/x.txt
	printf 'hi\n' >"$f"
	for n in user.test com.test.x com.apple.metadata:test com.apple.quarantine com.apple.ResourceFork system.test; do
		if r=$(xattr -w "$n" val "$f" 2>&1); then ev "xattr -w $n: OK"; else ev "xattr -w $n: FAILED ($r)"; fi
	done
	# FinderInfo must be exactly 32 bytes
	if r=$(xattr -wx com.apple.FinderInfo 0000000000000000000000000000000000000000000000000000000000000000 "$f" 2>&1); then ev "xattr -wx com.apple.FinderInfo (32 bytes): OK"; else ev "xattr -wx com.apple.FinderInfo (32 bytes): FAILED ($r)"; fi
	ev "xattr -l: $(xattr -l "$f" 2>&1 | tr '\n' ';')"
	ev "xattr -p user.test: $(xattr -p user.test "$f" 2>&1)"
	ev "ls -l (size of main stream): $(stat -f '%z' "$f")"
}
p_xattr_again() {
	ev "after remount, xattr -l: $(xattr -l "$t/x.txt" 2>&1 | tr '\n' ';')"
}
atime_file() { find "$mnt" -maxdepth 2 -type f ! -path '*/.fseventsd/*' -size +0 | sort | head -1; }
p_atime() {
	# An existing file of the image, read after mount: a file written in this mount may be served
	# from the page cache without a FUSE read (a first version of this probe had that flaw).
	f=$(atime_file)
	[ -n "$f" ] || { ev "no regular file in the image"; return; }
	printf '%s\n' "${f#"$mnt"}" >"$work/atime.rel"
	touch -a -t 200001010000 "$f"
	b=$(stat -f '%Sa' -t '%Y-%m-%d %H:%M' "$f")
	cat "$f" >/dev/null
	sleep 1
	a=$(stat -f '%Sa' -t '%Y-%m-%d %H:%M' "$f")
	ev "file ${f#"$mnt"}: atime after touch -a -t 200001010000: $b; after a read: $a"
}
p_atime_ref() {
	f=$(atime_file)
	printf '%s\n' "${f#"$mnt"}" >"$work/atime.rel"
	ev "reference: file ${f#"$mnt"}, listed only, not read or touched"
}
post_atime() {
	rel=$(cat "$work/atime.rel")
	ev "after unmount, ntfsinfo -F $rel: $("$ntfsinfo" -F "$rel" "$img" 2>&1 | grep -i 'access' | head -3 | sed 's/^[ \t]*//' | tr '\n' ';')"
}
p_volname() {
	ev "mount line: $(mount_line)"
	ev "diskutil info: $(diskutil info "$mnt" 2>&1 | grep -E 'Volume Name|Mount Point|Device Node|File System Personality|Type \(Bundle\)' | sed 's/  */ /g' | sed "s#$work#WORK#" | tr '\n' ';')"
	ev "df: $(df -h "$mnt" | tail -1 | sed "s#$work#WORK#" | sed 's/  */ /g')"
	ev "/Volumes entries: $(ls /Volumes | tr '\n' ',')"
	ev "getattrlist ATTR_VOL_NAME: $(python3 -I - "$mnt" 2>&1 <<'PY'
import ctypes, sys
libc = ctypes.CDLL(None, use_errno=True)
class AL(ctypes.Structure):
    _fields_ = [("bitmapcount", ctypes.c_ushort), ("reserved", ctypes.c_ushort), ("commonattr", ctypes.c_uint32),
                ("volattr", ctypes.c_uint32), ("dirattr", ctypes.c_uint32), ("fileattr", ctypes.c_uint32),
                ("forkattr", ctypes.c_uint32)]
al = AL(5, 0, 0, 0x80000002, 0, 0, 0)  # ATTR_VOL_INFO | ATTR_VOL_NAME
buf = ctypes.create_string_buffer(1024)
r = libc.getattrlist(sys.argv[1].encode(), ctypes.byref(al), buf, 1024, 0)
if r != 0:
    print("getattrlist failed, errno", ctypes.get_errno())
else:
    off = int.from_bytes(buf.raw[4:8], "little", signed=True)
    ln = int.from_bytes(buf.raw[8:12], "little")
    print(repr(buf.raw[4 + off:4 + off + ln].split(b"\0")[0].decode("utf-8", "replace")))
PY
)"
}
p_iosize() {
	mk_t
	dd if=/dev/zero of="$t/io.bin" bs=1m count=8 2>/dev/null
	ev "stat -f %k (preferred I/O size) on file: $(stat -f '%k' "$t/io.bin"); on mount root: $(stat -f '%k' "$mnt")"
	ev "df block size line: $(df "$mnt" | tail -1 | sed "s#$work#WORK#" | sed 's/  */ /g')"
}
p_appledouble() {
	mk_t
	s=$work/src.txt
	printf 'x\n' >"$s"
	xattr -w com.apple.test 1 "$s" 2>/dev/null
	cp -p "$s" "$t/ad.txt" 2>"$work/err"
	ev "cp -p of a file with an xattr: rc=$? $(cat "$work/err")"
	ev "directory listing: $(ls -a "$t" | tr '\n' ',')"
	ev "xattrs on the copy: $(xattr -l "$t/ad.txt" 2>&1 | tr '\n' ';')"
	ev "AppleDouble '._ad.txt' present: $([ -e "$t/._ad.txt" ] && echo yes || echo no)"
	rm -f "$s"
}
p_hidden() {
	mk_t
	printf 'x\n' >"$t/hid.txt"
	printf 'y\n' >"$t/vis.txt"
	if r=$(xattr -wx system.ntfs_attrib_be 00000002 "$t/hid.txt" 2>&1); then ev "set DOS hidden attribute via system.ntfs_attrib_be: OK"; else ev "set DOS hidden attribute: FAILED ($r)"; fi
	ev "listing: $(ls -a "$t" | tr '\n' ',')"
	ev "stat of hid.txt still works: $(stat -f '%z' "$t/hid.txt" 2>&1)"
}
p_perm() {
	mk_t
	printf 'x\n' >"$t/p.txt"
	if r=$(chmod 600 "$t/p.txt" 2>&1); then ev "chmod 600: OK"; else ev "chmod 600: FAILED ($r)"; fi
	ev "ls -ln after chmod: $(ls -ln "$t/p.txt" | sed 's/  */ /g')"
}
p_perm_again() {
	ev "after remount, ls -ln: $(ls -ln "$t/p.txt" | sed 's/  */ /g')"
}
p_nonempty() {
	ev "listing of mount point: $(ls -a "$mnt" | tr '\n' ',')"
	if [ -e "$mnt/preexisting-marker" ]; then ev "pre-existing local file visible: yes"; else ev "pre-existing local file visible: no (hidden by the mount)"; fi
}
pre_nonempty() { : >"$mnt/preexisting-marker"; }
post_nonempty() { rm -f "$mnt/preexisting-marker"; }
p_defperm() {
	mk_t
	printf 'x\n' >"$t/d.txt"
	ev "ls -ln: $(ls -ln "$t/d.txt" | sed 's/  */ /g')"
	if r=$(cat "$t/d.txt" 2>&1); then ev "read of a mode-000 file owned by uid 0 as uid $uid: ALLOWED"; else ev "read as uid $uid: REFUSED ($r)"; fi
	if (: >"$t/d2.txt") 2>"$work/err"; then ev "create in a directory with mode 000: ALLOWED"; else ev "create in dir: REFUSED ($(sed 's#.*: ##' "$work/err" | head -1))"; fi
}
p_sync() {
	ev "mount line: $(mount_line)"
	case $(mount_line) in *synchronous*) ev "mount flags include synchronous: yes" ;; *) ev "mount flags include synchronous: no" ;; esac
}

# run_case NAME OPTS [PROBE [PRE [POST [PROBE2 [fg]]]]]
run_case() {
	name=$1
	opts=$2
	probe=${3:-p_basic}
	pre=${4:-}
	post=${5:-}
	probe2=${6:-}
	fg=${7:-0}
	say "=== CASE $name: -o $opts"
	fresh_image
	[ -z "$pre" ] || "$pre"
	if do_mount "$opts" "$fg"; then
		say "mount: $(mount_line)"
		"$probe"
		do_umount
		[ -z "$post" ] || "$post"
		if [ -n "$probe2" ]; then
			say "-- remount with the same options"
			if do_mount "$opts" "$fg"; then
				"$probe2"
				do_umount
			else
				say "  remount: $mstate (rc=$mrc)"
				show_out
			fi
		fi
	else
		say "RESULT: $mstate (ntfs-3g exit status $mrc)"
		show_out
		if [ "$mstate" = no-mount ] || [ "$mstate" = driver-exited ]; then
			say "-- diagnosis: same options in the foreground"
			do_mount "$opts" 1 || true
			say "  foreground: $mstate (rc=$mrc)"
			show_out
		fi
	fi
	[ -z "$post" ] || [ "$mstate" = mounted ] || "$post"
	do_umount
	summary "$name" "$opts"
}

echo "option_matrix: $(date -u +%Y-%m-%dT%H:%M:%SZ) user=$(id -un) uid=$uid gid=$gid"
echo "host: $(sw_vers -productName) $(sw_vers -productVersion) ($(sw_vers -buildVersion)) $(uname -m); macFUSE: $(defaults read /Library/Filesystems/macfuse.fs/Contents/Info CFBundleShortVersionString 2>/dev/null)"
"$ntfs3g" --version 2>&1 | head -1
echo "image: $img_src ($(stat -f %z "$img_src") bytes); work dir: $work"
echo

# ================= baseline and section 3.3 "Always" row =================
run_case baseline "$base,uid=$uid,gid=$gid"
run_case always-set "$base,noatime,windows_names,big_writes,streams_interface=xattr,uid=$uid,gid=$gid,fmask=0133,dmask=0022"

# backend and local
run_case backend-missing "local,norecover,uid=$uid,gid=$gid"
run_case backend-bogus "backend=bogus,local,norecover,uid=$uid,gid=$gid"
run_case local-absent "backend=fskit,norecover,uid=$uid,gid=$gid" p_volname

# uid, gid, fmask, dmask, umask
run_case uid-gid-other "$base,uid=1234,gid=4321"
run_case fmask-dmask "$base,uid=$uid,gid=$gid,fmask=0133,dmask=0022"
run_case fmask-dmask-strict "$base,uid=$uid,gid=$gid,fmask=0177,dmask=0077"
run_case umask "$base,uid=$uid,gid=$gid,umask=0027"
run_case uid-not-a-number "$base,uid=abc,gid=$gid"
run_case no-uid-gid "$base"

# windows_names
run_case windows_names-off "$base,uid=$uid,gid=$gid" p_names
run_case windows_names-on "$base,uid=$uid,gid=$gid,windows_names" p_names

# streams_interface
run_case streams-none "$base,uid=$uid,gid=$gid,streams_interface=none" p_xattr
run_case streams-xattr "$base,uid=$uid,gid=$gid,streams_interface=xattr" p_xattr "" "" p_xattr_again
run_case streams-openxattr "$base,uid=$uid,gid=$gid,streams_interface=openxattr" p_xattr "" "" p_xattr_again
run_case user_xattr "$base,uid=$uid,gid=$gid,user_xattr" p_xattr
run_case streams-default "$base,uid=$uid,gid=$gid" p_xattr
run_case streams-windows "$base,uid=$uid,gid=$gid,streams_interface=windows" p_xattr
run_case streams-bogus "$base,uid=$uid,gid=$gid,streams_interface=bogus"
run_case streams-noval "$base,uid=$uid,gid=$gid,streams_interface"

# noatime and friends
run_case atime-reference "$base,uid=$uid,gid=$gid" p_atime_ref "" post_atime
run_case noatime "$base,uid=$uid,gid=$gid,noatime" p_atime "" post_atime
run_case atime "$base,uid=$uid,gid=$gid,atime" p_atime "" post_atime
run_case relatime "$base,uid=$uid,gid=$gid,relatime" p_atime "" post_atime

# big_writes. NTFS-3G applies it only on volumes of at least SAFE_CAPACITY_FOR_BIG_WRITES (4 GiB,
# include/ntfs-3g/param.h), so a sparse 5 GiB image is made for this part. `debug` asks libfuse2 to log
# each request; whether the FSKit backend produces such a log is part of what is recorded.
bw_probe() {
	mk_t
	dd if=/dev/zero of="$t/bw.bin" bs=1m count=8 2>/dev/null
	sync
	sleep 1
	ev "trace lines in the driver output: $(wc -l <"$out" | tr -d ' ')"
	ev "lines mentioning write: $(grep -ci 'write' "$out")"
	ev "sample write lines: $(grep -i 'write' "$out" | sed -n '2p;12p;22p' | cut -c1-110 | tr '\n' '|')"
	ev "FUSE write request sizes, count x bytes (libfuse2 -odebug 'write[fh] N bytes'): $(grep '^ *write\[' "$out" | sed 's/^ *write\[[0-9]*\] \([0-9]*\) bytes.*/\1/' | sort -n | uniq -c | sort -rn | head -4 | sed 's/  */ /g' | tr '\n' ';')"
	ev "max_write seen: $(grep -o 'max_write=[0-9a-fx]*' "$out" | head -1)"
	ev "first lines of driver output: $(head -4 "$out" | cut -c1-120 | tr '\n' '|')"
}
bw_time() {
	mk_t
	for n in 1 2 3; do
		t0=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
		dd if=/dev/zero of="$t/bw$n.bin" bs=1m count=256 2>/dev/null
		sync
		t1=$(perl -MTime::HiRes=time -e 'printf "%.3f", time')
		ev "256 MiB dd + sync, run $n: $(perl -e "printf '%.2f', $t1 - $t0") s"
	done
}
big=$work/big.src
rm -f "$big"
dd if=/dev/zero of="$big" bs=1 count=0 seek=5g 2>/dev/null
if "$mkntfs" -F -Q -L bigvol "$big" >"$work/mkntfs.out" 2>&1; then
	say "5 GiB sparse test image made with mkntfs (-F -Q)"
	cur_src=$big
	run_case big_writes-on-5GiB-debug "$base,uid=$uid,gid=$gid,big_writes,debug" bw_probe "" "" "" 1
	run_case big_writes-off-5GiB-debug "$base,uid=$uid,gid=$gid,debug" bw_probe "" "" "" 1
	run_case big_writes-on-5GiB-timing "$base,uid=$uid,gid=$gid,big_writes" bw_time
	run_case big_writes-off-5GiB-timing "$base,uid=$uid,gid=$gid" bw_time
	cur_src=$img_src
else
	say "mkntfs of the 5 GiB image failed; big_writes not measured"
	sed 's/^/  | /' "$work/mkntfs.out"
fi
rm -f "$big"
echo

# opt-in rows
run_case volname "$base,uid=$uid,gid=$gid,volname=NTFSMAC" p_volname
run_case no-volname "$base,uid=$uid,gid=$gid" p_volname
run_case ro "$base,uid=$uid,gid=$gid,ro" p_ro
run_case rdonly "$base,uid=$uid,gid=$gid,rdonly" p_ro
run_case rw-explicit "$base,uid=$uid,gid=$gid,rw" p_ro
run_case hide_hid_files-off "$base,uid=$uid,gid=$gid" p_hidden
run_case hide_hid_files-on "$base,uid=$uid,gid=$gid,hide_hid_files" p_hidden
run_case permissions-off "$base,uid=$uid,gid=$gid" p_perm "" "" p_perm_again
run_case permissions-on "$base,uid=$uid,gid=$gid,permissions" p_perm "" "" p_perm_again
run_case permissions-only "$base,permissions" p_perm "" "" p_perm_again

um=$work/UserMapping
cat >"$um" <<EOF
$uid::S-1-5-21-3141592653-589793238-462843383-1000
:$gid:S-1-5-21-3141592653-589793238-462843383-513
::S-1-5-21-3141592653-589793238-462843383-10000
EOF
run_case usermapping-file "$base,uid=$uid,gid=$gid,usermapping=$um" p_perm "" "" p_perm_again
run_case usermapping-only "$base,usermapping=$um" p_perm "" "" p_perm_again
run_case usermapping-missing "$base,uid=$uid,gid=$gid,usermapping=$work/does-not-exist"

# banned row (dev, suid, allow_root, efs_raw); recover, remove_hiberfile, fsname are never passed
run_case dev "$base,uid=$uid,gid=$gid,dev"
run_case suid "$base,uid=$uid,gid=$gid,suid"
run_case allow_root "$base,uid=$uid,gid=$gid,allow_root"
run_case efs_raw "$base,uid=$uid,gid=$gid,efs_raw"

# options NTFS-3G adds itself, and engine options of interest
run_case allow_other "$base,uid=$uid,gid=$gid,allow_other"
run_case nonempty-on-empty-dir "$base,uid=$uid,gid=$gid,nonempty"
run_case nonempty-on-busy-dir "$base,uid=$uid,gid=$gid" p_nonempty pre_nonempty post_nonempty
run_case default_permissions "$base,uid=$uid,gid=$gid,default_permissions"
run_case defperm-effect-with "$base,uid=0,gid=0,fmask=0777,dmask=0777" p_defperm
run_case no_def_opts "$base,uid=0,gid=0,fmask=0777,dmask=0777,no_def_opts" p_defperm
run_case blkdev "$base,uid=$uid,gid=$gid,blkdev"
run_case blksize "$base,uid=$uid,gid=$gid,blksize=4096"
run_case bogus-unknown-token "$base,uid=$uid,gid=$gid,bogusopt"
run_case bogus-unknown-kv "$base,uid=$uid,gid=$gid,bogusopt=1"
run_case empty-token "$base,,uid=$uid,gid=$gid"

# macFUSE-specific
run_case noowners "$base,uid=$uid,gid=$gid,noowners"
run_case noappledouble-off "$base,uid=$uid,gid=$gid,streams_interface=none" p_appledouble
run_case noappledouble-on "$base,uid=$uid,gid=$gid,streams_interface=none,noappledouble" p_appledouble
run_case noappledouble-on-xattr "$base,uid=$uid,gid=$gid,streams_interface=xattr,noappledouble" p_appledouble
run_case noapplexattr "$base,uid=$uid,gid=$gid,streams_interface=xattr,noapplexattr" p_xattr
run_case nobrowse "$base,uid=$uid,gid=$gid,nobrowse" p_volname
run_case daemon_timeout "$base,uid=$uid,gid=$gid,daemon_timeout=10"
run_case daemon_timeout-bogus "$base,uid=$uid,gid=$gid,daemon_timeout=abc"
run_case iosize-default "$base,uid=$uid,gid=$gid" p_iosize
run_case iosize-65536 "$base,uid=$uid,gid=$gid,iosize=65536" p_iosize
run_case iosize-1048576 "$base,uid=$uid,gid=$gid,iosize=1048576" p_iosize
run_case iosize-bogus "$base,uid=$uid,gid=$gid,iosize=abc"
run_case sync "$base,uid=$uid,gid=$gid,sync" p_sync
run_case async "$base,uid=$uid,gid=$gid,async" p_sync

# echo_case NAME OPTS: foreground mount only to read the engine's own report of what it passed on
# to libfuse ("Mount options:" line). The daemonised mount prints nothing.
echo_case() {
	say "=== ECHO $1: -o $2"
	fresh_image
	do_mount "$2" 1
	say "  state=$mstate"
	grep -E '^(Cmdline options|Mount options|Mounted|WARNING)' "$out" | sed "s#$work#WORK#g" | sed 's/^/  | /'
	do_umount
	say ""
}
for o in volname=NTFSMAC nobrowse noappledouble noapplexattr dev suid blkdev blksize=4096 daemon_timeout=10 iosize=65536 sync async rdonly allow_other nonempty default_permissions bogusopt big_writes; do
	echo_case "echo-$o" "$base,uid=$uid,gid=$gid,$o"
done
echo_case echo-usermapping-missing "$base,uid=$uid,gid=$gid,usermapping=$work/does-not-exist"
echo_case echo-usermapping-file "$base,uid=$uid,gid=$gid,usermapping=$um"
echo_case echo-permissions "$base,permissions"

# noop_case NAME OPTS: mount, change nothing, unmount, and compare the image hash. Shows which
# mounts really open the volume read-only (ro) and which only make the kernel mount read-only (rdonly).
noop_case() {
	say "=== NOOP $1: -o $2"
	fresh_image
	h0=$(sha_img)
	say "  root before: $("$ntfsls" "$img" 2>&1 | tr '\n' ' ')"
	do_mount "$2" 1
	say "  state=$mstate; mount: $(mount_line)"
	grep -E '^(Mounted|Mount options)' "$out" | sed "s#$work#WORK#g" | sed 's/^/  | /'
	sleep 1
	do_umount
	h1=$(sha_img)
	say "  root after:  $("$ntfsls" "$img" 2>&1 | tr '\n' ' ')"
	if [ "$h0" = "$h1" ]; then say "  EVIDENCE: image unchanged by a mount with no writes"; else say "  EVIDENCE: image CHANGED by a mount with no writes"; fi
	say ""
}
noop_case rw-noop "$base,uid=$uid,gid=$gid"
noop_case ro-noop "$base,uid=$uid,gid=$gid,ro"
noop_case rdonly-noop "$base,uid=$uid,gid=$gid,rdonly"

# sync: the plain umount was busy once in the first run; repeat to see whether that is the option.
run_case sync-repeat-1 "$base,uid=$uid,gid=$gid,sync" p_sync
run_case sync-repeat-2 "$base,uid=$uid,gid=$gid,sync" p_sync
run_case async-repeat-1 "$base,uid=$uid,gid=$gid,async" p_sync
run_case fmask-dmask-without-uid "$base,fmask=0133,dmask=0022"
run_case noappledouble-on-openxattr "$base,uid=$uid,gid=$gid,streams_interface=openxattr,noappledouble" p_appledouble
run_case noapplexattr-openxattr "$base,uid=$uid,gid=$gid,streams_interface=openxattr,noapplexattr" p_xattr

# norecover on a volume left dirty by a killed driver: the volume flag is set while mounted
# read/write and cleared at unmount; SIGKILL leaves it set. Only norecover and ro are used on it.
say "=== CASE norecover-dirty-volume (volume left dirty by SIGKILL of the driver)"
fresh_image
if do_mount "$base,uid=$uid,gid=$gid"; then
	: >"$mnt/dirty-marker"
	kill_driver
	/sbin/umount -f "$mnt" >/dev/null 2>&1
	poll 5 no_driver
	say "  ntfsfix -n after the kill:"
	"$ntfsfix" -n "$img" 2>&1 | sed 's/^/  | /'
	say "  rw mount with norecover on the dirty volume:"
	do_mount "$base,uid=$uid,gid=$gid" 0
	say "  state=$mstate rc=$mrc"
	show_out
	[ "$mstate" != mounted ] || do_umount
	say "  ro mount with norecover on the dirty volume:"
	do_mount "$base,uid=$uid,gid=$gid,ro" 0
	say "  state=$mstate rc=$mrc"
	show_out
	[ "$mstate" != mounted ] || do_umount
else
	say "  setup mount failed: $mstate"
fi
do_umount
summary norecover-dirty-volume "$base,uid=$uid,gid=$gid"

# daemon_timeout in action: stop the driver (SIGSTOP) and see whether macFUSE tears the mount down.
say "=== CASE daemon_timeout-effect (SIGSTOP the driver, daemon_timeout=3)"
fresh_image
if do_mount "$base,uid=$uid,gid=$gid,daemon_timeout=3"; then
	p=$(driver_pids)
	say "  driver pid(s): $p"
	# shellcheck disable=SC2086 # word splitting of the pid list is intended
	kill -STOP $p
	( ls "$mnt" >/dev/null 2>&1 & echo $! >"$work/ls.pid" )
	sleep 8
	if mounted; then say "  after 8 s: still mounted"; else say "  after 8 s: mount is gone"; fi
	# shellcheck disable=SC2086
	kill -CONT $p 2>/dev/null
	kill -9 "$(cat "$work/ls.pid" 2>/dev/null)" 2>/dev/null
	sleep 1
fi
kill_driver
if mounted; then /sbin/umount -f "$mnt" >/dev/null 2>&1; fi
do_umount
summary daemon_timeout-effect "$base,uid=$uid,gid=$gid,daemon_timeout=3"

# ---- final check: nothing left behind ----
if mounted; then say "LEFT BEHIND: $mnt is still mounted"; left=1; fi
if [ -n "$(driver_pids)" ]; then say "LEFT BEHIND: ntfs-3g process(es): $(driver_pids)"; kill_driver; left=1; fi
if [ "$left" = 0 ]; then say "option_matrix: done, nothing left behind"; exit 0; fi
say "option_matrix: done, SOMETHING WAS LEFT BEHIND"
exit 1
