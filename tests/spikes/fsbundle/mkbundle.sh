#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-09 spike: build an experimental /Library/Filesystems/ntfs3g.fs in a GUEST.
# Never run on the host. Usage (guest, as root): [IMPL=kext] mkbundle.sh PROBE_ORDER [DEPS_DIR]
#   IMPL  value of FSImplementation: none (default), kext, userfs, userfs-kext
# The probe helper logs what Disk Arbitration passes it and recognises NTFS by its boot sector.
# The mount helper logs its argv and uid, applies a refusal rule with the engine's own probe
# (ntfs-3g.probe, so a dirty or hibernated volume is never mounted rw), opens the device
# itself and starts the pinned ntfs-3g (always norecover) on /dev/fd/9 with the FSKit backend.
# The log is world-writable because DA runs the mount helper as the console user.
set -eu
# Refuse to run anywhere but a virtual machine guest.
[ "$(sysctl -n kern.hv_vmm_present 2>/dev/null)" = 1 ] || { echo "$0: guest only (kern.hv_vmm_present is not 1)" >&2; exit 2; }
order=${1:?probe order}
deps=${2:-$HOME/ntfsmac/deps}
b=/Library/Filesystems/ntfs3g.fs
case ${IMPL:-none} in
none) impl_xml= ;;
kext) impl_xml='<key>FSImplementation</key><array><string>kext</string></array>' ;;
userfs) impl_xml='<key>FSImplementation</key><array><string>UserFS</string></array>' ;;
userfs-kext) impl_xml='<key>FSImplementation</key><array><string>UserFS</string><string>kext</string></array>' ;;
*) echo "bad IMPL" >&2; exit 2 ;;
esac
rm -rf "$b"
mkdir -p "$b/Contents/Resources"
cat > "$b/Contents/Info.plist" <<PL
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
<key>CFBundleIdentifier</key><string>test.ntfsmac.fs.ntfs3g</string>
<key>CFBundleName</key><string>ntfs3g</string>
<key>CFBundlePackageType</key><string>fs  </string>
<key>CFBundleShortVersionString</key><string>0.0.1</string>
<key>CFBundleVersion</key><string>0.0.1</string>
$impl_xml
<key>FSMediaTypes</key><dict>
 <key>Windows_NTFS</key><dict>
  <key>FSMediaProperties</key><dict><key>Content Hint</key><string>Windows_NTFS</string><key>Leaf</key><true/></dict>
  <key>FSProbeArguments</key><string>-p</string>
  <key>FSProbeExecutable</key><string>ntfs3g.util</string>
  <key>FSProbeOrder</key><integer>$order</integer>
 </dict>
 <key>EBD0A0A2-B9E5-4433-87C0-68B6B72699C7</key><dict>
  <key>FSMediaProperties</key><dict><key>Content Hint</key><string>EBD0A0A2-B9E5-4433-87C0-68B6B72699C7</string><key>Leaf</key><true/></dict>
  <key>FSProbeArguments</key><string>-p</string>
  <key>FSProbeExecutable</key><string>ntfs3g.util</string>
  <key>FSProbeOrder</key><integer>$order</integer>
 </dict>
 <key>Partitionless</key><dict>
  <key>FSMediaProperties</key><dict><key>Leaf</key><true/><key>Whole</key><true/></dict>
  <key>FSProbeArguments</key><string>-p</string>
  <key>FSProbeExecutable</key><string>ntfs3g.util</string>
  <key>FSProbeOrder</key><integer>$order</integer>
 </dict>
</dict>
<key>FSPersonalities</key><dict>
 <key>NTFS-3G</key><dict>
  <key>FSMountArguments</key><string></string>
  <key>FSMountExecutable</key><string>mount_ntfs3g</string>
  <key>FSName</key><string>NTFS-3G test</string>
 </dict>
</dict>
</dict></plist>
PL
cat > "$b/Contents/Resources/ntfs3g.util" <<UT
#!/bin/sh
# probe: ntfs3g.util -p DISK [removable|fixed] [readonly|writable]   (DA runs it as root)
log=/var/tmp/ntfs3g-fs.log; [ -e \$log ] || { : > \$log; chmod 666 \$log; }
echo "\$(date +%T) PROBE uid=\$(id -u) argv: \$*" >> \$log
[ "\$1" = -p ] || exit 1
[ -f /var/tmp/ntfs3g-decline ] && { echo "\$(date +%T) PROBE declined" >> \$log; exit 254; }
d=/dev/r\${2#/dev/}
magic=\$(dd if=\$d bs=512 count=1 2>/dev/null | dd bs=1 skip=3 count=8 2>/dev/null)
if [ "\$magic" != "NTFS    " ]; then echo "\$(date +%T) PROBE \$2 not ntfs" >> \$log; exit 254; fi
$deps/sbin/ntfslabel --force \$d 2>/dev/null | tr -d '\n'
echo "\$(date +%T) PROBE \$2 recognised" >> \$log
exit 255
UT
cat > "$b/Contents/Resources/mount_ntfs3g" <<MT
#!/bin/sh
# mount helper: DA runs it as the console user, not as root
log=/var/tmp/ntfs3g-fs.log
echo "\$(date +%T) MOUNT uid=\$(id -u) argv: \$*" >> \$log 2>/dev/null
for a; do dev=\$mp; mp=\$a; done
case \$dev in /dev/*) ;; *) dev=/dev/\$dev;; esac
if [ -f /var/tmp/ntfs3g-fake ]; then exit 0; fi
raw=/dev/r\${dev#/dev/}
if $deps/bin/ntfs-3g.probe --readwrite \$raw >> \$log 2>&1; then mode=rw
elif $deps/bin/ntfs-3g.probe --readonly \$raw >> \$log 2>&1; then mode=ro
else echo "\$(date +%T) MOUNT refuse" >> \$log; exit 1; fi
[ -f /var/tmp/ntfs3g-forcero ] && mode=ro
echo "\$(date +%T) MOUNT mode=\$mode dev=\$dev mp=\$mp" >> \$log
if [ \$mode = rw ]; then exec 9<>\$dev; else exec 9<\$dev; fi || { echo "open failed" >> \$log; exit 1; }
$deps/bin/ntfs-3g -o backend=fskit,local,norecover,noatime,windows_names,no_detach,\$mode,streams_interface=openxattr /dev/fd/9 \$mp >> \$log 2>&1 &
[ -f /var/tmp/ntfs3g-nowait ] && { echo "\$(date +%T) MOUNT nowait exit 0" >> \$log; exit 0; }
i=0
while [ \$i -lt 40 ]; do
  /sbin/mount | grep -q " on \$mp (" && { echo "\$(date +%T) MOUNT up after \$i x 0.25s" >> \$log; exit 0; }
  sleep 0.25; i=\$((i+1))
done
echo "\$(date +%T) MOUNT timeout" >> \$log; exit 1
MT
chmod 755 "$b/Contents/Resources/ntfs3g.util" "$b/Contents/Resources/mount_ntfs3g"
chown -R root:wheel "$b"
echo "bundle at $b, probe order $order, FSImplementation ${IMPL:-none}"
