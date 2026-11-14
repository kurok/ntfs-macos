set -u
D=~/ntfsmac/deps/bin; S=~/ntfsmac/deps/sbin
W=/private/tmp/pw; rm -rf $W; mkdir -p $W/mnt; chmod 755 $W $W/mnt; cd $W
truncate -s 64m v.img; $S/mkntfs -F -Q -L t v.img >/dev/null 2>&1 || exit 9
M=$W/mnt
mnt() { cd $W; timeout 40 $D/ntfs-3g -o backend=fskit,local,norecover,"$1" $W/v.img $M; for i in $(seq 1 100); do /sbin/mount | grep -q " on /private/tmp/pw/mnt (" && break; sleep 0.3; done; /sbin/mount | grep " on /private/tmp/pw/mnt (" || { echo MOUNT FAILED; exit 1; }; }
um() { cd $W; sync; /sbin/umount $M; for i in $(seq 1 50); do pgrep -f "ntfs-3g.*v.img" >/dev/null || break; sleep 0.3; done; }
which cc clang
echo '#include <stdio.h>
int main(void){puts("native ran");return 0;}' > /tmp/h.c; cc -o /tmp/h /tmp/h.c && /tmp/h
echo "=== exec"
mnt uid=501,gid=20,fmask=0133,dmask=0022
cp /tmp/h $M/h; ls -ln $M/h; $M/h; echo "native rc=$?"; chmod +x $M/h; $M/h; echo "native after chmod rc=$?"
printf '#!/bin/sh\necho script ran\n' > $M/s.sh; $M/s.sh; echo "script rc=$?"; 
um
echo "=== exec ro"
mnt uid=501,gid=20,fmask=0133,dmask=0022,ro
$M/h; echo "native ro rc=$?"; $M/s.sh; echo "script ro rc=$?"; um
echo "=== other user"
sudo -n dscl . -create /Users/permtest; sudo -n dscl . -create /Users/permtest UniqueID 599; sudo -n dscl . -create /Users/permtest PrimaryGroupID 599; sudo -n dscl . -create /Users/permtest UserShell /bin/sh;  sudo -n dscl . -create /Users/permtest NFSHomeDirectory /var/empty; sudo -n dscl . -create /Users/permtest RealName permtest
id permtest
mnt uid=501,gid=20,fmask=0133,dmask=0022
echo secret > $M/readable; echo secret > $M/z; chmod 000 $M/z; mkdir $M/dd
P="sudo -n -u permtest"
$P id
$P ls -ln $M; echo "ls rc=$?"
$P cat $M/readable; echo "cat rc=$?"
$P cat $M/z; echo "cat z rc=$?"
$P sh -c "echo x >> $M/readable"; echo "append rc=$?"
$P sh -c "echo x > $M/new"; echo "create rc=$?"
$P mkdir $M/dd/sub; echo "mkdir rc=$?"
$P rm $M/readable; echo "rm rc=$?"
$P $M/h; echo "exec h rc=$?"
ls -ln $M
um
sudo -n dscl . -delete /Users/permtest; id permtest
