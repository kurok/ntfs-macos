#!/bin/sh
# Guest-side experiments for T-M4-02. Run inside a disposable clone only.
cd "$HOME/da" || exit 2
SZ_VIRTIO=271581184
SZ_RAW2=269484032
SZ_GPT_PART=268435456

banner() { printf '\n==== %s ====\n' "$*"; }
mounts() { printf -- '-- mount (ntfs lines):\n'; /sbin/mount | grep -i ntfs || echo "(none)"; }

attach() { # attach IMG -> prints whole-disk dev in $DEV
	hdiutil attach -imagekey diskimage-class=CRawDiskImage "$1" >att.txt 2>/dev/null
	cat att.txt
	DEV=$(awk 'NR==1 { print $1 }' att.txt)
}
detach() {
	hdiutil detach "$DEV" -force >det.txt 2>&1; grep -v deprecated det.txt
}

banner "guest"
sw_vers
uname -m
id -u
sudo -n true && echo "sudo -n ok"

banner "build"
cc -std=c17 -Wall -Wextra -I src/daemon -I include -o da_watch \
	tests/integration/daemon/da_watch.c src/daemon/da.c src/daemon/da_filter.c src/daemon/status.c \
	-framework DiskArbitration -framework CoreFoundation && echo build-ok

gunzip -c raw2.img.gz >raw2.img
gunzip -c gpt.img.gz >gpt.img

banner "cleanup of stale image disks from the first run"
for d in disk7 disk8; do hdiutil detach /dev/$d -force 2>&1 | grep -v deprecated; done
sleep 2
banner "E2: hot attach, no dissent (raw NTFS image, size $SZ_RAW2)"
sudo -n ./da_watch --size $SZ_RAW2 --open-test --duration 22 >e2.log 2>&1 &
sleep 3
attach raw2.img
sleep 14
mounts
sleep 6
cat e2.log
detach
sleep 2

banner "E3: dissent from disk_appeared, three attach cycles"
sudo -n ./da_watch --size $SZ_RAW2 --dissent --open-test --duration 70 >e3.log 2>&1 &
sleep 3
for n in 1 2 3; do
	echo "-- cycle $n"
	attach raw2.img
	sleep 10
	mounts
	diskutil info "$DEV" 2>&1 | grep -i "mounted\|mount point\|file system\|content"
	detach
	sleep 4
done
sleep 12
cat e3.log

banner "E4: dissent, then withdraw after 8 s (hand back)"
sudo -n ./da_watch --size $SZ_RAW2 --dissent --handback 8 --open-test --duration 34 >e4.log 2>&1 &
sleep 3
attach raw2.img
sleep 6
echo "-- before withdraw"
mounts
sleep 14
echo "-- after withdraw"
mounts
sleep 8
cat e4.log
detach
sleep 2

banner "E5: GPT disk (partition Windows basic data), no dissent, no size filter"
sudo -n ./da_watch --duration 20 >e5.log 2>&1 &
sleep 3
attach gpt.img
sleep 12
mounts
for d in "$DEV" "${DEV}s1"; do diskutil info "$d" 2>&1 | grep -i "device identifier\|content (iocontent)\|partition type\|file system personality"; done
sleep 7
cat e5.log
detach
sleep 2

banner "E6: GPT disk, dissent on the partition (size $SZ_GPT_PART)"
sudo -n ./da_watch --size $SZ_GPT_PART --dissent --duration 24 >e6.log 2>&1 &
sleep 3
attach gpt.img
sleep 12
mounts
sleep 8
cat e6.log
detach
sleep 3
banner "done"
