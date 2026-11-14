export PATH=/sbin:/usr/sbin:$PATH
D=""
for d in $(diskutil list | awk '/^\/dev\/disk[0-9]+ /{print $1}'); do diskutil info $d | grep -q "268435456 Bytes" && D=$d; done
echo "found: $D"; diskutil info $D | egrep "Protocol|Virtual|Block Size|Device Location|Media Name"
R=/dev/r${D#/dev/}
sudo -n python3 - $D $R <<'PY'
import os, sys, fcntl, time
for n in sys.argv[1:]:
    fd=os.open(n, os.O_RDWR)
    os.pwrite(fd, b"x"*1048576, 8*1048576)
    for what in ("F_FULLFSYNC","fsync","F_FULLFSYNC-after-write"):
        if what.endswith("write"): os.pwrite(fd, b"y"*4096, 9*1048576)
        t=time.time()
        try:
            if what.startswith("F_"): r=fcntl.fcntl(fd, fcntl.F_FULLFSYNC)
            else: os.fsync(fd); r=0
            print(n, what, "ok", r, "%.2f ms"%((time.time()-t)*1000))
        except OSError as e:
            print(n, what, "errno", e.errno, "%.2f ms"%((time.time()-t)*1000))
    os.close(fd)
PY
