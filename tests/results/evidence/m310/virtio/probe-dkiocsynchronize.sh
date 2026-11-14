export PATH=/sbin:/usr/sbin:$PATH
cd ~/ntfsmac/m310
grep -n "DKIOCSYNCHRONIZE\|dk_synchronize" $(xcrun --show-sdk-path)/usr/include/sys/disk.h | head
cat > ds.c <<'C'
#include <sys/disk.h>
#include <sys/ioctl.h>
#include <fcntl.h>
#include <stdio.h>
#include <string.h>
#include <errno.h>
#include <unistd.h>
int main(int argc, char **argv) {
	for (int i = 1; i < argc; i++) {
		int fd = open(argv[i], O_RDWR);
		if (fd < 0) { printf("%s open errno %d\n", argv[i], errno); continue; }
		dk_synchronize_t s; memset(&s, 0, sizeof s);
		int r = ioctl(fd, DKIOCSYNCHRONIZE, &s);
		printf("%s DKIOCSYNCHRONIZE(0,0,0) -> %d errno %d (sizeof %zu)\n", argv[i], r, r < 0 ? errno : 0, sizeof s);
		s.options = DK_SYNCHRONIZE_OPTION_BARRIER;
		r = ioctl(fd, DKIOCSYNCHRONIZE, &s);
		printf("%s DKIOCSYNCHRONIZE barrier -> %d errno %d\n", argv[i], r, r < 0 ? errno : 0);
		close(fd);
	}
	return 0;
}
C
cc -o ds ds.c && echo built
rm -f n.img; dd if=/dev/zero of=n.img bs=1m count=0 seek=64 2>/dev/null
D=$(hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage n.img 2>/dev/null | awk '{print $1; exit}')
./ds $D /dev/r${D#/dev/}
hdiutil detach $D >/dev/null 2>&1
V=""
for d in $(diskutil list | awk '/^\/dev\/disk[0-9]+ /{print $1}'); do diskutil info $d | grep -q "268435456 Bytes" && V=$d; done
echo virtio=$V
sudo -n ./ds $V /dev/r${V#/dev/}
