#!/bin/sh
cd "$HOME/da" || exit 2
SZ=271581184
banner() { printf '\n==== %s ====\n' "$*"; }
mounts() { printf -- '-- mount (ntfs lines):\n'; /sbin/mount | grep -i ntfs || echo "(none)"; }
banner "E9a: DA unmount without any open test"
mounts
sudo -n ./da_watch --size $SZ --unmount --duration 18 2>&1 | grep -v "^T+0.00. appeared disk5"
mounts
banner "E9b: remount with diskutil, DA unmount again without open test"
diskutil mount disk0 2>&1
sleep 2
sudo -n ./da_watch --size $SZ --unmount --duration 18 2>&1 | grep -v "^T+0.00. appeared disk5"
mounts
banner "E9c: remount, DA unmount with the open test (the E1 case)"
diskutil mount disk0 2>&1
sleep 2
sudo -n ./da_watch --size $SZ --unmount --open-test --duration 25 2>&1 | grep -v "^T+0.00. appeared disk5"
mounts
banner "done"
