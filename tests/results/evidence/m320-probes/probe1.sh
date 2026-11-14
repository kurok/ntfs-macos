set -u
D=~/ntfsmac/deps/bin; S=~/ntfsmac/deps/sbin
W=/private/tmp/pw; rm -rf $W; mkdir -p $W/mnt; cd $W
truncate -s 64m v.img; $S/mkntfs -F -Q -L t v.img >/dev/null 2>&1 || exit 9
M=$W/mnt
mnt() { cd $W; timeout 40 $D/ntfs-3g -o backend=fskit,local,norecover,"$1" $W/v.img $M; for i in $(seq 1 100); do /sbin/mount | grep -q " on /private/tmp/pw/mnt (" && break; sleep 0.3; done; /sbin/mount | grep " on /private/tmp/pw/mnt (" || { echo MOUNT FAILED; exit 1; }; }
um() { cd $W; sync; /sbin/umount $M; for i in $(seq 1 50); do pgrep -f "ntfs-3g.*v.img" >/dev/null || break; sleep 0.3; done; }
echo "=== product options"
mnt uid=501,gid=20,fmask=0133,dmask=0022
cd $M; touch f; mkdir d; echo hi > f; printf '#!/bin/sh\necho ran\n' > s.sh
ls -lne; 
for m in 600 755 000 4755; do chmod $m f; echo "chmod $m rc=$?"; ls -ln f; done
chmod +x s.sh; echo "chmod+x rc=$?"; ls -ln s.sh; ./s.sh; echo "exec rc=$?"; sh s.sh
chmod 700 d; echo "chmod d rc=$?"; ls -lnd d
chown 0 f; echo "chown 0 rc=$?"; sudo -n chown 0 f; echo "sudo chown 0 rc=$?"; sudo -n chown 1234:4321 f; echo "sudo chown 1234 rc=$?"; chown 501:20 f; echo "chown self rc=$?"
ls -ln f
um
mnt uid=501,gid=20,fmask=0133,dmask=0022; ls -ln $M; um
echo "=== permissions only"
mnt permissions
cd $M; touch p; ls -ln p; chmod 600 p; echo rc=$?; ls -ln p; sudo -n chown 1234:4321 p; echo "chown rc=$?"; ls -ln p; printf '#!/bin/sh\necho ran\n' > s2.sh; chmod 755 s2.sh; ./s2.sh; echo "exec rc=$?"; mkdir d2; ls -lnd d2; um
mnt permissions; ls -ln $M; um
