#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M6-01: the part of pkg_release_vm.sh that runs inside a fresh clone of the golden VM, as the
# guest user (passwordless sudo). Usage: pkg_release_vm_guest.sh DIR, where DIR/pkg holds the
# built package (ntfs-macos-*.pkg) and DIR/tree the files of the source tree that the checks
# compare with (packaging/manifest.txt, packaging/uninstall.sh, packaging/payload-check.sh,
# integration/launchd/*.plist). The fixture clean-64m-4k is attached to the guest as a disk.
# Everything here changes only the guest; mounts are made by the installed daemon, always with
# norecover.
#
# I-PKG-03: `installer -pkg` of the package with the quarantine attribute set (a NOTE says whether
#           the command-line installer accepts it; if not, it is installed without the attribute)
#           installs; `pkgutil --files org.ntfs-macos.pkg.v2` agrees with the manifest;
#           every dir and file of the manifest is there with its owner, group and mode and nothing
#           else is below the owned directories; the installed manifest, uninstaller and plist are
#           the tree's; the daemon has no test seams; the recorded requirement is the cdhash of
#           the installed CLI /usr/local/libexec/ntfs-macos/ntfsmac (written by postinstall) and the
#           CLI has the hardened runtime; postinstall made the link /usr/local/bin/ntfsmac; the
#           right reads back as defined; the job is loaded and the daemon serves its socket and
#           XPC service; every installed program runs; `ntfsmac doctor` is green; the privileged
#           door answers the installed CLI; the fixture volume is attached and mounted rw by the
#           daemon with norecover (no recover, no remove_hiberfile), a file written there reads
#           back, `ntfsmac unmount` and `ntfsmac mount` work; the shared parent directories keep
#           their owner and mode; installing the package again (an upgrade of our own install,
#           with the volume mounted) passes the preflight, gives the same state and mounts the
#           volume again.
# I-PKG-04: the installed packaging/uninstall.sh removes it: the job is unloaded, no macFUSE mount
#           is left, the right is removed, the receipt is forgotten, every dir and file of the
#           manifest is gone; nothing else was touched (the non-directory files below /usr/local,
#           /Library/LaunchDaemons, /Library/Filesystems, /Library/Extensions and the support
#           directory equal those before the install; the state is kept, then removed by
#           --purge-state).
#
# Prints PASS, FAIL and NOTE lines; exit 0 only if no FAIL.
# Test script run as the guest user: the output of sudo commands goes to the user's own files on purpose.
# shellcheck disable=SC2024,SC2012,SC2009,SC2015
set -u
PATH=/opt/homebrew/bin:/usr/local/bin:/sbin:/usr/sbin:/usr/bin:/bin:$PATH
export PATH

d=${1:?directory}
cd "$d" || exit 2
fail=0
LIBEXEC=/usr/local/libexec/ntfs-macos
CLI=$LIBEXEC/ntfsmac
LINK=/usr/local/bin/ntfsmac
DOC="/Library/Application Support/ntfs-macos/doc"
LABEL=org.ntfs-macos.ntfsmacd
PLIST=/Library/LaunchDaemons/$LABEL.plist
RIGHT=org.ntfs-macos.journal-modify
SUPPORT="/Library/Application Support/ntfs-macos"
SOCK=/var/run/ntfsmacd.sock
LOG=/var/log/ntfsmacd.log
PKGID=org.ntfs-macos.pkg.v2
MANIFEST=$d/tree/packaging/manifest.txt
VOL=clean64m4k
work=$(cd -P "$(mktemp -d /tmp/pkgrel.XXXXXX)" && pwd)
pkg=$(ls "$d"/pkg/ntfs-macos-*.pkg 2>/dev/null | head -n 1)

pass() { echo "PASS: $*"; }
failf() { echo "FAIL: $*"; fail=$((fail + 1)); }
note() { echo "NOTE: $*"; }
check() { desc=$1; shift; if "$@"; then pass "$desc"; else failf "$desc"; fi; }
nocheck() { desc=$1; shift; if "$@"; then failf "$desc"; else pass "$desc"; fi; }
wait_for() { # SECONDS CMD...
	t=$1; shift; i=0
	while [ "$i" -lt "$t" ]; do
		if "$@" >/dev/null 2>&1; then return 0; fi
		sleep 1; i=$((i + 1))
	done
	return 1
}

job_loaded() { sudo -n launchctl print "system/$LABEL" >/dev/null 2>&1; }
right_key() { sudo -n security authorizationdb read "$RIGHT" 2>/dev/null | plutil -extract "$1" raw -o - - 2>/dev/null || true; }
right_is_ours() { [ "$(right_key class)" = user ] && [ "$(right_key group)" = admin ]; }
manifest_paths() { # KIND...
	awk -v kinds="$*" 'BEGIN { n = split(kinds, k, " "); for (i = 1; i <= n; i++) want[k[i]] = 1 }
		/^[ \t]*(#|$)/ { next }
		{ p = $0; for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p); sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p)
		  if ($1 in want) print p }' "$MANIFEST"
}
entries_match() {
	bad=0
	while read -r kind mode owner group path || [ -n "$kind" ]; do
		case $kind in dir | file) ;; *) continue ;; esac
		got=$(stat -f '%Lp %Su %Sg' "$path" 2>/dev/null) || { echo "  missing: $path"; bad=$((bad + 1)); continue; }
		[ "$got" = "${mode#0} $owner $group" ] || { echo "  $path: $got, the manifest says ${mode#0} $owner $group"; bad=$((bad + 1)); }
		case $kind in
		file) { [ -f "$path" ] && [ ! -L "$path" ]; } || { echo "  not a regular file: $path"; bad=$((bad + 1)); } ;;
		dir) { [ -d "$path" ] && [ ! -L "$path" ]; } || { echo "  not a directory: $path"; bad=$((bad + 1)); } ;;
		esac
	done <"$MANIFEST"
	[ "$bad" -eq 0 ]
}
set_equal() {
	manifest_paths dir file | sort >"$work/want.set"
	{
		find "$LIBEXEC" "$SUPPORT" \( -type f -o -type d -o -type l \) -print 2>/dev/null
		ls -d "$PLIST" 2>/dev/null
	} | sort >"$work/have.set"
	cmp -s "$work/want.set" "$work/have.set" && return 0
	diff "$work/want.set" "$work/have.set" | sed 's/^/  /'
	return 1
}
installed_ok() { entries_match && set_equal; }
receipt_ok() {
	pkgutil --files "$PKGID" >"$work/receipt.list" 2>/dev/null || { echo "  no receipt"; return 1; }
	sh tree/packaging/payload-check.sh "$MANIFEST" "$work/receipt.list"
}
# files (not directories) below the places the project writes to, and the shared parents' owner and mode
snapshot() {
	{
		find /usr/local /Library/LaunchDaemons /Library/Filesystems /Library/Extensions "$SUPPORT" /var/db/ntfs-macos -not -type d -print 2>/dev/null
		ls -d "$LOG" 2>/dev/null
	} | sort
}
# every directory below /usr/local and /Library/LaunchDaemons, and the first level of
# /Library/Application Support, with owner, group and mode: the shared parents included, also the
# empty ones (a removed directory is a line that is missing afterwards)
dirsnap() {
	{
		find /usr/local /Library/LaunchDaemons -type d 2>/dev/null
		find "/Library/Application Support" -maxdepth 1 -type d 2>/dev/null
	} | sort | while IFS= read -r p; do printf '%s %s\n' "$p" "$(stat -f '%Su:%Sg %Lp' "$p")"; done
}
# kept_dirs BEFORE AFTER: every directory of BEFORE is in AFTER with the same owner, group and mode
kept_dirs() {
	_miss=$(awk 'NR == FNR { a[$0] = 1; next } !($0 in a) { print "  changed or removed: " $0 }' "$2" "$1")
	[ -z "$_miss" ] && return 0
	echo "$_miss"
	return 1
}

list() { "$CLI" list 2>&1; }
col() { list | awk -v l="$1" -v c="$2" '$2 == l { print $c }'; } # BSD LABEL STATE MOUNT MOUNTPOINT REASON
state_of() { col "$1" 3; }
mount_of() { col "$1" 4; }
bsd_of() { col "$1" 1; }
vol_mounted_rw() { [ "$(state_of $VOL)" = Mounted ] && [ "$(mount_of $VOL)" = rw ] && mount | grep -F "/Volumes/$VOL " | grep -qF macfuse; }
worker_args() { ps -axo args= | grep -F "$LIBEXEC/ntfsmac-worker" | grep -v grep; }
macfuse_mounts() { mount | grep -cF macfuse || true; }
console=$(stat -f %Su /dev/console)

echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u), console owner $console"
echo "package: ${pkg##*/} ($(stat -f %z "$pkg" 2>/dev/null) bytes); manifest $(grep -cv '^[[:space:]]*\(#\|$\)' "$MANIFEST") entries"
[ -n "$pkg" ] || { failf "no package in $d/pkg"; exit 1; }
[ ! -e "$LIBEXEC" ] && [ ! -e "$LINK" ] && [ ! -e "$PLIST" ] && ! pkgutil --pkg-info "$PKGID" >/dev/null 2>&1 ||
	{ failf "the guest already has an install; use a fresh clone"; exit 1; }
[ "$console" != root ] || note "nobody is logged in at the console: the product daemon cannot start a worker, so the mount checks will fail"
echo "-- macFUSE: $(plutil -extract CFBundleShortVersionString raw -o - /Library/Filesystems/macfuse.fs/Contents/Info.plist 2>/dev/null)"
echo "-- disks"
diskutil list external 2>/dev/null | sed -n '1,12p'
mount | grep -i -e ntfs -e "/Volumes/$VOL" || true
snapshot >"$work/snap.before"
dirsnap >"$work/dirs.before"
note "before the install: $(wc -l <"$work/snap.before" | tr -d ' ') files below /usr/local, the launchd, file system, extension and support directories"

# =====================================================================================
echo "== I-PKG-03: install from the package"
# =====================================================================================
# The package as a browser leaves it: the quarantine attribute on an unsigned package. INSTALL.md
# depends on whether the command-line installer accepts it; the answer is printed as a NOTE and
# the test goes on without the attribute if it does not.
xattr -w com.apple.quarantine "0083;$(printf '%x' "$(date +%s)");Safari;$(uuidgen)" "$pkg"
echo "quarantine attribute on the package: $(xattr -p com.apple.quarantine "$pkg" 2>&1)"
pkgutil --check-signature "$pkg" 2>&1 | sed 's/^/  /'
logmark=$(sudo -n wc -l </var/log/install.log | tr -d ' ')
sudo -n /usr/sbin/installer -pkg "$pkg" -target / >"$work/install1.out" 2>&1
rc=$?
cat "$work/install1.out"
if [ "$rc" -eq 0 ]; then
	note "QUARANTINE: sudo installer -pkg accepts the quarantined, unsigned package on macOS $(sw_vers -productVersion) (exit 0)"
	pass "installer -pkg of the quarantined package exits 0"
else
	note "QUARANTINE: sudo installer -pkg refuses the quarantined, unsigned package on macOS $(sw_vers -productVersion) (exit $rc); installing without the attribute"
	sudo -n tail -n "+$((logmark + 1))" /var/log/install.log | grep -i -E 'quarantine|gatekeeper|signature|trust|assess' | tail -n 10 | sed 's/^/  /'
	xattr -d com.apple.quarantine "$pkg"
	logmark=$(sudo -n wc -l </var/log/install.log | tr -d ' ')
	sudo -n /usr/sbin/installer -pkg "$pkg" -target / >"$work/install1.out" 2>&1
	rc=$?
	cat "$work/install1.out"
	if [ "$rc" -eq 0 ]; then pass "installer -pkg exits 0 (without the quarantine attribute)"; else failf "installer -pkg exits $rc"; fi
fi
xattr -d com.apple.quarantine "$pkg" 2>/dev/null || true
sudo -n tail -n "+$((logmark + 1))" /var/log/install.log | grep -E 'ntfs-macos (pre|post)install' | sed -E 's/^.*(ntfs-macos (pre|post)install:)/  \1/' >"$work/scripts1.log"
cat "$work/scripts1.log"
check "the preflight passed (install log)" grep -q 'preinstall: preflight passed$' "$work/scripts1.log"
check "postinstall recorded the CLI requirement and started the daemon (install log)" sh -c "grep -q 'recorded the requirement' '$work/scripts1.log' && grep -q 'is running' '$work/scripts1.log'"
[ "$rc" -eq 0 ] || { echo "-- install log"; sudo -n tail -n 60 /var/log/install.log; echo "result: $fail failure(s)"; exit 1; }
pkgutil --pkg-info "$PKGID"

echo "-- the receipt, the manifest and the installed files"
if out=$(receipt_ok); then pass "pkgutil --files $PKGID matches the manifest ($(grep -c . "$work/receipt.list") receipt paths)"; else failf "the receipt differs from the manifest"; echo "$out"; fi
if out=$(entries_match); then pass "every dir and file of the manifest is installed with its owner, group and mode ($(manifest_paths dir file | wc -l | tr -d ' ') paths)"; else failf "manifest paths differ from the install"; echo "$out"; fi
if out=$(set_equal); then pass "the installed set below the owned directories equals the manifest (nothing extra)"; else failf "installed set differs from the manifest"; echo "$out"; fi
ls -lde "$LIBEXEC" "$LIBEXEC"/* "$CLI" "$PLIST" "$SUPPORT" "$SUPPORT"/* "$SUPPORT"/sandbox/* "$DOC" "$DOC"/* "$LINK" 2>&1 |
	awk '{ $2 = $5 = $6 = $7 = $8 = ""; print }' | sed 's/  */ /g'
check "the installed manifest and uninstaller are the tree's" sh -c "cmp -s '$MANIFEST' $LIBEXEC/manifest.txt && cmp -s tree/packaging/uninstall.sh $LIBEXEC/uninstall.sh"
check "the installed plist is the shipped one" cmp -s "tree/integration/launchd/$LABEL.plist" "$PLIST"
nocheck "the installed daemon has none of the test-seam options" sh -c "strings $LIBEXEC/ntfsmacd | grep -q -e '--console-uid' -e '--child-log' -e '--journal-dir'"
check "BUILD-INFO says preset product" grep -q '^preset .*product' "$DOC/BUILD-INFO"
check "postinstall made the link $LINK -> $CLI (/usr/local/bin is root-only in the guest)" [ "$(readlink "$LINK")" = "$CLI" ]
check "the link works: ntfsmac by its name on PATH runs the installed CLI" sh -c "PATH=/usr/local/bin:/usr/bin:/bin ntfsmac list >/dev/null 2>&1"
dirsnap >"$work/dirs.after"
if out=$(kept_dirs "$work/dirs.before" "$work/dirs.after"); then pass "every directory that existed below /usr/local, /Library/LaunchDaemons and /Library/Application Support kept its owner and mode"; else failf "a directory that existed before changed"; echo "$out"; fi

echo "-- the CLI requirement (ADR-019 check 3), recorded by postinstall"
req=$(cat "$SUPPORT/cli-requirement" 2>/dev/null)
hash=$(codesign -d -vvv "$CLI" 2>&1 | sed -n 's/^CDHash=//p')
echo "recorded: $req"
check "cli-requirement is the cdhash of the installed CLI" [ "$req" = "cdhash H\"$hash\"" ]
check "the installed CLI has the hardened runtime" sh -c "codesign -d --verbose=2 $CLI 2>&1 | grep -q 'flags=.*runtime'"
check "no temporary requirement file is left" sh -c "! ls -a '$SUPPORT' | grep -q '^\.cli-requirement'"

echo "-- the authorization right (ADR-019)"
check "right $RIGHT: class user, group admin, shared false, timeout 0" sh -c "[ '$(right_key class) $(right_key group) $(right_key shared) $(right_key timeout)' = 'user admin false 0' ]"

echo "-- the launchd job"
check "the job is loaded" job_loaded
check "the control socket is there" wait_for 30 test -S "$SOCK"
check "the privileged XPC service is registered" sh -c "sudo -n launchctl print system/$LABEL | grep -q org.ntfs-macos.ctl.privileged"
check "the job runs the installed daemon" sh -c "sudo -n launchctl print system/$LABEL | grep -qF 'program = $LIBEXEC/ntfsmacd'"

echo "-- every installed program runs (exit below 126: no dyld or signature failure)"
for p in "ntfsmac-worker --help" "ntfsmac-probe --version" "ntfsmac-volmark" "mkntfs --version" "ntfsfix --version" "ntfsinfo --version" "ntfslabel --version"; do
	# shellcheck disable=SC2086 # the program and its argument
	"$LIBEXEC"/$p >/dev/null 2>&1
	prc=$?
	if [ "$prc" -lt 126 ]; then pass "runs: ${p%% *} (exit $prc)"; else failf "does not run: ${p%% *} (exit $prc)"; fi
done
for f in "$LIBEXEC"/* "$CLI"; do
	case $f in *.sh | */manifest.txt) continue ;; esac
	codesign --verify "$f" 2>/dev/null || failf "signature of $f does not verify"
done

echo "-- ntfsmac doctor"
"$CLI" doctor >"$work/doctor.out" 2>&1
rc=$?
cat "$work/doctor.out"
if [ "$rc" -eq 0 ] && ! grep -q '^problem:' "$work/doctor.out"; then pass "ntfsmac doctor exits 0 with no problem"; else failf "ntfsmac doctor exits $rc or reports a problem"; fi
sudo -n "$CLI" journal-clear --label x --serial 00000000 disk99s1 >"$work/priv.out" 2>&1
check "sudo ntfsmac journal-clear is answered by the daemon (the privileged door accepts the installed CLI)" grep -q "The daemon does not know this volume" "$work/priv.out"

echo "-- the fixture volume: attached and mounted by the daemon"
if wait_for 150 vol_mounted_rw; then pass "$VOL is Mounted rw by the daemon (macfuse, /Volumes/$VOL)"; else failf "$VOL is not mounted rw"; fi
list
mount | grep -F macfuse || true
worker_args >"$work/worker.args"
sed 's/^/  worker: /' "$work/worker.args"
check "a worker serves the volume with norecover" grep -q 'norecover' "$work/worker.args"
nocheck "...and with no recover and no remove_hiberfile option" grep -q -E '(^|[ ,=])(recover|remove_hiberfile)([ ,]|$)' "$work/worker.args"
check "a file written on the volume reads back" sh -c "echo m6pkg > '/Volumes/$VOL/m6pkg.txt' && sync && [ \"\$(cat '/Volumes/$VOL/m6pkg.txt')\" = m6pkg ]"
bsd=$(bsd_of $VOL)
dev=$(mount | awk -v m="/Volumes/$VOL" '$3 == m { sub("^/dev/", "", $1); print $1 }')
note "ntfsmac list names the volume $bsd; the kernel mount table names $dev"
diskutil list 2>&1 | sed -n '1,40p'
"$CLI" status "$bsd" 2>&1 | sed -n '1,30p'
"$CLI" unmount "$bsd" >"$work/unmount.out" 2>&1
echo "ntfsmac unmount $bsd (exit $?): $(tr '\n' ' ' <"$work/unmount.out")"
check "ntfsmac unmount $bsd: the volume is unmounted" wait_for 60 sh -c "! mount | grep -qF '/Volumes/$VOL '"
# the daemon settles the session after the mount is gone (journal, notification): mount after that
check "...and the daemon has settled it (state Appeared)" wait_for 60 sh -c "[ \"\$('$CLI' list 2>&1 | awk -v l=$VOL '\$2 == l { print \$3 }')\" = Appeared ]"
list
"$CLI" mount "$bsd" >"$work/mount.out" 2>&1
echo "ntfsmac mount $bsd (exit $?): $(tr '\n' ' ' <"$work/mount.out")"
if ! wait_for 90 vol_mounted_rw; then
	list
	echo "-- daemon log (last 40 lines)"
	sudo -n tail -n 40 "$LOG"
fi
check "ntfsmac mount $bsd: mounted rw again" vol_mounted_rw
check "the file is still there" sh -c "[ \"\$(cat '/Volumes/$VOL/m6pkg.txt' 2>/dev/null)\" = m6pkg ]"
worker_args | grep -q norecover && pass "the new worker has norecover too" || failf "the new worker has no norecover"

echo "-- installing the package again: an upgrade of our own install, with the volume mounted"
logmark=$(sudo -n wc -l </var/log/install.log | tr -d ' ')
sudo -n /usr/sbin/installer -pkg "$pkg" -target / >"$work/install2.out" 2>&1
rc=$?
cat "$work/install2.out"
sudo -n tail -n "+$((logmark + 1))" /var/log/install.log | grep -E 'ntfs-macos (pre|post)install' | sed -E 's/^.*(ntfs-macos (pre|post)install:)/  \1/' >"$work/scripts2.log"
cat "$work/scripts2.log"
if [ "$rc" -eq 0 ]; then pass "the second install exits 0"; else failf "the second install exits $rc"; fi
check "the preflight saw the earlier package and stopped its daemon" sh -c "grep -q 'preflight passed (upgrading the package' '$work/scripts2.log' && grep -q 'stopping the running daemon' '$work/scripts2.log'"
if out=$(installed_ok); then pass "the same installed state after the upgrade"; else failf "the upgrade changed the installed set"; echo "$out"; fi
if out=$(receipt_ok); then pass "the receipt still matches the manifest"; else failf "the receipt differs after the upgrade"; echo "$out"; fi
check "the daemon is up again" wait_for 30 test -S "$SOCK"
check "the requirement is still the installed CLI's" sh -c "[ \"\$(cat '$SUPPORT/cli-requirement')\" = 'cdhash H\"$(codesign -d -vvv "$CLI" 2>&1 | sed -n 's/^CDHash=//p')\"' ]"
check "the right is still ours" right_is_ours
if wait_for 150 vol_mounted_rw; then pass "$VOL is mounted rw again by the new daemon"; else failf "$VOL is not mounted again after the upgrade"; list; fi
check "the file written before the upgrade is there" sh -c "[ \"\$(cat '/Volumes/$VOL/m6pkg.txt' 2>/dev/null)\" = m6pkg ]"

# =====================================================================================
echo "== I-PKG-04: packaging/uninstall.sh (the installed copy)"
# =====================================================================================
sudo -n "$LIBEXEC/uninstall.sh" >"$work/un1.out" 2>&1
rc=$?
cat "$work/un1.out"
if [ "$rc" -eq 0 ]; then pass "uninstall.sh exits 0"; else failf "uninstall.sh exits $rc"; fi
check "the job is unloaded" sh -c "! sudo -n launchctl print system/$LABEL >/dev/null 2>&1"
check "the control socket is gone" wait_for 20 sh -c "! [ -S $SOCK ]"
check "no macFUSE mount is left (the daemon unmounted the volume when it stopped)" wait_for 60 sh -c "[ \"\$(mount | grep -cF macfuse)\" = 0 ]"
nocheck "the right is removed" right_is_ours
check "the receipt is forgotten" sh -c "! pkgutil --pkg-info $PKGID >/dev/null 2>&1"
check "the link $LINK is gone" sh -c "[ ! -e $LINK ] && [ ! -L $LINK ]"
left=$(manifest_paths dir file | while IFS= read -r p; do if [ -e "$p" ] || [ -L "$p" ]; then echo "  left: $p"; fi; done)
if [ -z "$left" ]; then pass "every dir and file of the manifest is gone"; else failf "manifest paths are left"; echo "$left"; fi
snapshot | grep -v -e '^/var/db/ntfs-macos/' -e "^$LOG\$" >"$work/snap.after"
grep -v -e '^/var/db/ntfs-macos/' -e "^$LOG\$" "$work/snap.before" >"$work/snap.before.nostate"
if cmp -s "$work/snap.before.nostate" "$work/snap.after"; then
	pass "nothing else was touched: the files below /usr/local, /Library/LaunchDaemons, /Library/Filesystems, /Library/Extensions and the support directory equal those before the install"
else
	failf "files differ from before the install"; diff "$work/snap.before.nostate" "$work/snap.after" | sed 's/^/  /'
fi
dirsnap >"$work/dirs.final"
if out=$(kept_dirs "$work/dirs.before" "$work/dirs.final"); then pass "every directory that existed before the install is still there with its owner and mode (empty shared parents included)"; else failf "a directory that existed before was removed or changed"; echo "$out"; fi
awk 'NR == FNR { a[$0] = 1; next } !($0 in a) { print "NOTE: directory made by the install and kept (shared, never removed): " $0 }' "$work/dirs.before" "$work/dirs.final"
check "the state is kept without --purge-state" grep -q 'kept state' "$work/un1.out"
sudo -n sh tree/packaging/uninstall.sh --purge-state >"$work/un2.out" 2>&1
rc=$?
cat "$work/un2.out"
if [ "$rc" -eq 0 ]; then pass "uninstall.sh --purge-state exits 0"; else failf "uninstall.sh --purge-state exits $rc"; fi
snapshot >"$work/snap.final"
if cmp -s "$work/snap.before" "$work/snap.final"; then pass "after --purge-state the files equal those before the install (state included)"; else failf "files differ after --purge-state"; diff "$work/snap.before" "$work/snap.final" | sed 's/^/  /'; fi
diskutil list external 2>/dev/null | grep -q -i ntfs && pass "the fixture disk is still there (only unmounted)" || note "the fixture disk is not listed as NTFS"

sudo -n rm -rf "$work"
echo "result: $fail failure(s)"
[ "$fail" -eq 0 ]
