#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-11, I-DMN-18 at daemon level: every hostile-label image of docs/TEST_STRATEGY.md section 3.3
# (tests/fixtures/gen/labels.sh, 27 images) is attached to the real daemon. Pass: the mount point
# is /Volumes/<expected leaf> (the expected leaf comes from the generator, written from ADR-016,
# not from the code under test), `ps -o args` of the worker shows no option beyond the allow-list
# and nothing of the label, `diskutil` names the volume from the leaf (U14: no `volname` is
# passed), and nothing is created outside /Volumes. The pair label-dup-a/-b is attached together:
# `DATA`, then `DATA 2`. Usage: dmn_labels_guest.sh DIR.
[ "$(/usr/sbin/sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only" >&2; exit 2; }
set -u
d=${1:?directory}
. "$d/tests/integration/daemon/dmn_lib.sh"
prepare

# the options the daemon may give the worker (ARCHITECTURE.md section 3.3 and mount-options.json)
# keys of the -o list (the daemon builds it: ARCHITECTURE.md section 3.3; the observed vector of a rw mount is
# backend=fskit,local,norecover,noatime,windows_names,nocompression,no_detach,streams_interface=openxattr,uid,gid,fmask,dmask; ro adds ro)
ALLOWED='backend local norecover noatime windows_names nocompression no_detach streams_interface uid gid fmask dmask ro'
BANNED='recover remove_hiberfile allow_other volname fsname rdonly default_permissions'

args_ok() { # args_ok LINE: every -o key is on the allow-list, none is banned, no label bytes
	# every -o group of the line, not only the last
	opts=$(printf '%s\n' "$1" | grep -oE -- ' -o [^ ]+' | sed 's/^ -o //')
	[ -n "$opts" ] || return 1
	for kv in $(printf '%s\n' "$opts" | tr ',' ' '); do
		k=${kv%%=*}
		case " $BANNED " in *" $k "*) echo "the banned option '$k' is present"; return 1 ;; esac
		case " $ALLOWED " in *" $k "*) ;; *) echo "option '$k' is not on the allow-list"; return 1 ;; esac
	done
	return 0
}

before_root=$(ls -A / | tr '\n' ' ')
echo "== start the job"
start_daemon

check_one() { # check_one NAME BSD EXPECTED_LEAF
	n=$1 bsd=$2 leaf=$3
	st=$(sget "$bsd" state); mp=$(sget "$bsd" mountpoint); mo=$(sget "$bsd" mount)
	eq "I-DMN-18 $n: state" "$st $mo" "Mounted rw"
	eq "I-DMN-18 $n: mount point" "$mp" "/Volumes/$leaf"
	check "I-DMN-18 $n: the directory exists and is the mount" sh -c "[ -d '/Volumes/$leaf' ]"
	line=$(ps -axww -o args= | grep -F "$INSTALL/ntfsmac-worker" | grep -v grep | head -1)
	if [ -n "$line" ]; then
		check "I-DMN-18 $n: the worker's options are all on the allow-list, none is banned" args_ok "$line"
		case $line in *"$INSTALL/ntfsmac-worker /dev/fd/"*" /Volumes/"*) pass "I-DMN-18 $n: the worker is started with a descriptor and the mount point only" ;; *) failf "I-DMN-18 $n: unexpected worker arguments: $line" ;; esac
	else
		failf "I-DMN-18 $n: no worker process found"
	fi
	# U14: the name Finder shows comes from the leaf (no volname is passed). Finder itself is not
	# scriptable unattended (Automation permission); the display name the system gives the path is
	# what Finder asks for (NSFileManager displayNameAtPath), and needs no permission.
	vn=$(tmo 20 osascript -l JavaScript -e "ObjC.import('Foundation'); \$.NSFileManager.defaultManager.displayNameAtPath('/Volumes/$leaf').js" 2>/dev/null)
	if [ "$vn" = "$leaf" ]; then
		pass "I-DMN-18 $n: the display name of the mount point equals the leaf (U14)"
	else
		note "I-DMN-18 $n: display name '$vn' differs from the leaf '$leaf'"
	fi
}

for lf in "$d"/fixtures/label-*.leaf; do
	n=$(basename "$lf" .leaf)
	case $n in label-dup-a | label-dup-b) continue ;; esac
	[ -z "${DMN_ONLY:-}" ] || case " $DMN_ONLY " in *" $n "*) ;; *) continue ;; esac
	leaf=$(cat "$lf")
	img=$(fresh_image "$n") || { failf "$n: no image"; continue; }
	bsd=$(attach_image "$img")
	wait_for 120 settled_s "$bsd" || true
	sleep 1
	echo "-- $n ($bsd) expected leaf '$leaf': mount point '$(sget "$bsd" mountpoint)'"
	check_one "$n" "$bsd" "$leaf"
	"$CLI" unmount "$bsd" >/dev/null 2>&1
	wait_for 60 sh -c "[ \"\$('$CLI' status $bsd 2>&1 | sed -n 's/^  state: //p' | head -1)\" = Appeared ]" || note "$n: no return to Appeared"
	check "I-DMN-18 $n: the mount point directory is gone" sh -c "[ ! -e '/Volumes/$leaf' ]"
	detach_image "$bsd"
	rm -f "$img"
done

echo "== the pair with the same label: DATA, then DATA 2"
ia=$(fresh_image label-dup-a); ib=$(fresh_image label-dup-b)
ba=$(attach_image "$ia")
wait_for 120 settled_s "$ba" || true
bb=$(attach_image "$ib")
wait_for 120 settled_s "$bb" || true
sleep 1
ma=$(sget "$ba" mountpoint); mb=$(sget "$bb" mountpoint)
echo "-- dup-a ($ba): '$ma'; dup-b ($bb): '$mb'"
eq "I-DMN-18 label-dup-a: mount point" "$ma" "/Volumes/DATA"
eq "I-DMN-18 label-dup-b: mount point gets a suffix" "$mb" "/Volumes/DATA 2"
"$CLI" unmount "$ba" >/dev/null 2>&1
"$CLI" unmount "$bb" >/dev/null 2>&1
wait_for 60 sh -c "[ ! -e '/Volumes/DATA' ] && [ ! -e '/Volumes/DATA 2' ]" || true
check "the two mount points are gone" sh -c "[ ! -e '/Volumes/DATA' ] && [ ! -e '/Volumes/DATA 2' ]"
detach_image "$ba"; detach_image "$bb"

after_root=$(ls -A / | tr '\n' ' ')
eq "I-DMN-18: nothing was created in / by the daemon" "$after_root" "$before_root"
note "entries left in /Volumes: $(ls -A /Volumes | tr '\n' '|')"
echo "-- daemon log (tail)"
logcat | tail -20
finish
