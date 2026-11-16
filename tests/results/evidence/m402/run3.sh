#!/bin/sh
# Guest-side experiment 3 for T-M4-02: dissent on a persistent disk (the virtio disk, size 271581184).
cd "$HOME/da" || exit 2
SZ=271581184
banner() { printf '\n==== %s ====\n' "$*"; }
mounts() { printf -- '-- mount (ntfs lines):\n'; /sbin/mount | grep -i ntfs || echo "(none)"; }

banner "E7: persistent disk, dissent active, explicit mount request, then withdraw at 14 s"
mounts
sudo -n ./da_watch --size $SZ --dissent --handback 14 --duration 40 >e7.log 2>&1 &
sleep 4
echo "-- diskutil mount disk0 while dissent is active"
diskutil mount disk0 2>&1
echo "rc=$?"
mounts
sleep 14
echo "-- after withdraw"
mounts
sleep 10
cat e7.log

banner "E8: time of a plain diskutil unmount, then explicit mount without dissent"
mounts
s=$(date +%s)
diskutil unmount disk0 2>&1
e=$(date +%s)
echo "diskutil unmount took $((e - s)) s"
mounts
sudo -n ./da_watch --size $SZ --duration 14 >e8.log 2>&1 &
sleep 3
diskutil mount disk0 2>&1
sleep 10
mounts
cat e8.log
banner "done"
