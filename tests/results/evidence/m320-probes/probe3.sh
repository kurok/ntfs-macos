set -u
D=~/ntfsmac/deps/bin; S=~/ntfsmac/deps/sbin
W=/private/tmp/pw; rm -rf $W; mkdir -p $W/mnt; chmod 755 $W $W/mnt; cd $W
truncate -s 64m v.img; $S/mkntfs -F -Q -L t v.img >/dev/null 2>&1 || exit 9
M=$W/mnt
mnt() { cd $W; timeout 40 $D/ntfs-3g -o backend=fskit,local,norecover,"$1" $W/v.img $M; for i in $(seq 1 100); do /sbin/mount | grep -q " on /private/tmp/pw/mnt (" && break; sleep 0.3; done; /sbin/mount | grep " on /private/tmp/pw/mnt (" || { echo MOUNT FAILED; exit 1; }; }
um() { cd $W; sync; /sbin/umount $M; for i in $(seq 1 50); do pgrep -f "ntfs-3g.*v.img" >/dev/null || break; sleep 0.3; done; }
mnt uid=501,gid=20,fmask=0133,dmask=0022
echo hi > $M/f; mkdir $M/d; um
$D/ntfsinfo -F f -v $W/v.img > before.txt 2>&1; wc -l before.txt; grep -i -n "security\|attributes\|owner" before.txt | head
mnt uid=501,gid=20,fmask=0133,dmask=0022
chmod 600 $M/f; sudo -n chown 1234 $M/f; chmod 700 $M/d; um
$D/ntfsinfo -F f -v $W/v.img > after.txt 2>&1; diff before.txt after.txt && echo SAME
echo "=== sh -c exec as other"
sudo -n dscl . -create /Users/permtest; sudo -n dscl . -create /Users/permtest UniqueID 599; sudo -n dscl . -create /Users/permtest PrimaryGroupID 599; sudo -n dscl . -create /Users/permtest UserShell /bin/sh;  sudo -n dscl . -create /Users/permtest NFSHomeDirectory /var/empty
mnt permissions
cp /tmp/h $M/h; chmod 755 $M/h; echo secret > $M/z; chmod 000 $M/z; echo x > $M/y; chmod 600 $M/y; mkdir $M/dd; chmod 700 $M/dd; echo in > $M/dd/in
ls -ln $M
P="sudo -n -u permtest"
$P /bin/sh -c "$M/h"; echo "exec as other rc=$?"
$P cat $M/z; echo "cat z rc=$?"
$P cat $M/y; echo "cat y(600) rc=$?"
$P ls $M/dd; echo "ls dd(700) rc=$?"
$P sh -c "echo x > $M/y"; echo "write y(600) rc=$?"
$P stat -f '%u %g %Sp' $M/y
sudo -n ls -ln $M/y
cat $M/z; echo "cat z as admin rc=$?"
chmod 600 $M/z; chmod 644 $M/y; echo "chmod rc=$?"
um
sudo -n dscl . -delete /Users/permtest
mnt permissions
ls -ln $M; um
