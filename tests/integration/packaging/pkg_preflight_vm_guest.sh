#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M6-01: the part of pkg_preflight_vm.sh that runs inside a fresh clone of the golden VM, as the
# guest user (passwordless sudo). Usage: pkg_preflight_vm_guest.sh DIR [preflight|userlocal|oldrel],
# where DIR/pkg holds the built package, DIR/oldpkg the published pre-release package (oldrel only)
# and DIR/tree the reference files (packaging/manifest.txt, packaging/payload-check.sh,
# packaging/uninstall.sh). Everything here changes only the guest, which is deleted afterwards.
# The package's receipt is org.ntfs-macos.pkg.v2; org.ntfs-macos.pkg is the earlier layout (owner
# decision 2026-11-26, audit M6-F02).
#
# preflight, I-PKG-05: with each of the following, `installer -pkg` refuses with the reason (from
# the installation check of distribution.xml or from preinstall, in the install log) and installs
# nothing (no receipt, no file of the manifest, no job), and the preinstall script run on its own
# refuses with the reason:
#   macFUSE missing; macFUSE older than 5.5.0 (a stand-in bundle that says 5.4.2); a Paragon
#   (ufsd_NTFS.fs), a Tuxera (tuxera_ntfs.fs) and another NTFS bundle (Acme-NTFS.fs) (the refusal
#   of other drivers is the owner decision of 2026-11-26); an NTFS kernel extension in
#   /Library/Extensions; a foreign file at an installed path with no install of ours; a symbolic
#   link at /usr/local/libexec/ntfs-macos; a user-owned /usr/local/libexec (above programs root runs);
#   audit M6-F01: /usr/local/libexec a symbolic link to a root-only directory, with a root-owned and
#   with a user-owned /usr/local (nothing is installed, and nothing appears in the link's target).
# After each case the guest is put back. Then the real install works over an older package of ours
# in the earlier layout (receipt 0.0.1: the CLI as the file /usr/local/bin/ntfsmac, documents and
# LICENSES in /usr/local/share/doc/ntfs-macos, a file this package does not ship): it passes the
# preflight as an upgrade, removes those files, replaces the old CLI file by the link, and leaves
# exactly the manifest's set and receipt with a running daemon. Then a downgrade is refused (owner
# decision 2026-11-26, issue #10): with the receipt's version, and without a receipt with the
# installed BUILD-INFO's version, set to 99.0.0, the install changes nothing and the daemon keeps
# running; one patch above the real version is refused too, the earlier 0.1.0 is upgraded, and
# the same version installs again.
#
# userlocal, I-PKG-06: with a user-owned /usr/local/bin and /usr/local/share (a Homebrew moved from
# an Intel Mac) the install succeeds, postinstall skips the link and logs how to run the CLI,
# nothing in those directories changes, and the CLI works by its full path (doctor, and the
# privileged door answers it). With a root-owned /usr/local/bin that holds a foreign ntfsmac, the
# install leaves it alone, and uninstall.sh keeps it. Over an older package (receipt
# org.ntfs-macos.pkg) that has /usr/local/bin/ntfsmac in a user-owned /usr/local/bin, preinstall
# leaves the file and keeps that receipt, and the Installer does not delete the file either (the
# identifiers differ, audit M6-F02). An upgrade of a developer install of the earlier layout
# (CLI file /usr/local/bin/ntfsmac, no receipt) with a user-owned /usr/local/bin leaves that file,
# and the last line of postinstall is a WARNING that names it with the remedy; uninstall.sh keeps
# it and names it.
#
# oldrel (audit M6-F02, M6-F04): the published pre-release v0.1.0-dev.20261116 (receipt
# org.ntfs-macos.pkg; tools in /usr/local/libexec/ntfs-macos, documents and LICENSES in
# /usr/local/share/doc/ntfs-macos) is installed first, then this package: the upgrade removes the
# earlier layout and forgets its receipt, the Installer changes nothing outside the project's
# directories, and uninstall.sh removes everything. The pre-release alone is removed by the new
# uninstall.sh. With a user-owned /usr/local/share/doc whose ntfs-macos is replaced by a link to a
# root-owned directory with canary files, neither the upgrade, nor uninstall.sh, nor
# uninstall.sh --old-receipt deletes anything there; after the fix --old-receipt removes the rest.
#
# Prints PASS, FAIL and NOTE lines; exit 0 only if no FAIL.
# Test script run as the guest user: the output of sudo commands goes to the user's own files on purpose.
# shellcheck disable=SC2024,SC2015,SC2016,SC2329
set -u
PATH=/opt/homebrew/bin:/usr/local/bin:/sbin:/usr/sbin:/usr/bin:/bin:$PATH
export PATH

d=${1:?directory}
mode=${2:-preflight}
case $mode in preflight | userlocal | oldrel) ;; *) echo "usage: pkg_preflight_vm_guest.sh DIR [preflight|userlocal|oldrel]"; exit 2 ;; esac
cd "$d" || exit 2
fail=0
LIBEXEC=/usr/local/libexec/ntfs-macos
CLI=$LIBEXEC/ntfsmac
LINK=/usr/local/bin/ntfsmac
LABEL=org.ntfs-macos.ntfsmacd
PLIST=/Library/LaunchDaemons/$LABEL.plist
PKGID=org.ntfs-macos.pkg.v2
OLDID=org.ntfs-macos.pkg
MACFUSE=/Library/Filesystems/macfuse.fs
ASIDE=/Library/ntfsmac-test-macfuse.fs.aside
MANIFEST=$d/tree/packaging/manifest.txt
work=$(cd -P "$(mktemp -d /tmp/pkgpre.XXXXXX)" && pwd)
pkg=$(find "$d/pkg" -name 'ntfs-macos-*.pkg' | head -n 1)

pass() { echo "PASS: $*"; }
failf() { echo "FAIL: $*"; fail=$((fail + 1)); }
note() { echo "NOTE: $*"; }
check() { desc=$1; shift; if "$@"; then pass "$desc"; else failf "$desc"; fi; }
wait_for() { t=$1; shift; i=0; while [ "$i" -lt "$t" ]; do if "$@" >/dev/null 2>&1; then return 0; fi; sleep 1; i=$((i + 1)); done; return 1; }
job_loaded() { sudo -n launchctl print "system/$LABEL" >/dev/null 2>&1; }

[ -n "$pkg" ] || { failf "no package in $d/pkg"; exit 1; }
echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un); package ${pkg##*/}"
[ ! -e "$LIBEXEC" ] && ! pkgutil --pkg-info "$PKGID" >/dev/null 2>&1 && ! pkgutil --pkg-info "$OLDID" >/dev/null 2>&1 || { failf "the guest already has an install; use a fresh clone"; exit 1; }
macfuse_ver=$(plutil -extract CFBundleShortVersionString raw -o - "$MACFUSE/Contents/Info.plist" 2>/dev/null)
echo "macFUSE $macfuse_ver"
pkgutil --expand-full "$pkg" "$work/exp" >/dev/null || { failf "pkgutil --expand-full"; exit 1; }
PRE=$(find "$work/exp" -path '*/Scripts/preinstall' | head -n 1)
[ -n "$PRE" ] || { failf "no preinstall in the package"; exit 1; }
check "the package carries preinstall and postinstall, both executable" sh -c "[ -x '$PRE' ] && [ -x '${PRE%/preinstall}/postinstall' ]"

nothing_installed() {
	! pkgutil --pkg-info "$PKGID" >/dev/null 2>&1 && ! pkgutil --pkg-info "$OLDID" >/dev/null 2>&1 && [ ! -e "$LIBEXEC/ntfsmac-worker" ] && [ ! -e "$PLIST" ] && [ ! -e "/Library/Application Support/ntfs-macos" ] && ! job_loaded
}
# refused NAME TEXT: the installer refuses, gives TEXT (its own output or the install log), and installs nothing;
# the preinstall script on its own refuses with TEXT too
refused() {
	logmark=$(sudo -n wc -l </var/log/install.log | tr -d ' ')
	sudo -n /usr/sbin/installer -pkg "$pkg" -target / >"$work/inst.out" 2>&1
	rc=$?
	sudo -n tail -n "+$((logmark + 1))" /var/log/install.log >"$work/inst.log"
	echo "  installer (exit $rc): $(tr '\n' ' ' <"$work/inst.out" | cut -c1-300)"
	grep -h 'REFUSED' "$work/inst.log" | sed 's/^.*ntfs-macos preinstall/  install log: ntfs-macos preinstall/' | head -n 1
	if [ "$rc" -ne 0 ] && grep -q -F -- "$2" "$work/inst.out" "$work/inst.log"; then pass "$1: installer refuses: $2"; else failf "$1: installer exit $rc, reason '$2' not found"; fi
	check "$1: nothing was installed" nothing_installed
	sudo -n /bin/sh "$PRE" "$pkg" / / / >"$work/pre.out" 2>&1
	rc=$?
	echo "  preinstall alone (exit $rc): $(grep -m1 REFUSED "$work/pre.out" | cut -c1-300)"
	if [ "$rc" -ne 0 ] && grep -q -F -- "$2" "$work/pre.out"; then pass "$1: preinstall refuses: $2"; else failf "$1: preinstall exit $rc, reason '$2' not found"; fi
}
# pre_refused NAME TEXT: preinstall alone (the installation check of distribution.xml does not know the case)
pre_refused() {
	sudo -n /bin/sh "$PRE" "$pkg" / / / >"$work/pre.out" 2>&1
	rc=$?
	echo "  preinstall alone (exit $rc): $(grep -m1 REFUSED "$work/pre.out" | cut -c1-300)"
	if [ "$rc" -ne 0 ] && grep -q -F -- "$2" "$work/pre.out"; then pass "$1: preinstall refuses: $2"; else failf "$1: preinstall exit $rc, reason '$2' not found"; fi
	check "$1: nothing was installed" nothing_installed
}

want_set() { awk '/^[ \t]*(#|$)/ { next } $1 == "dir" || $1 == "file" { p = $0; for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p); sub(/^[ \t]+/, "", p); print p }' "$MANIFEST" | sort; }
have_set() { { find "$LIBEXEC" "/Library/Application Support/ntfs-macos" -print; ls -d "$PLIST"; } 2>/dev/null | sort; }
set_ok() { want_set >"$work/want"; have_set >"$work/have"; cmp -s "$work/want" "$work/have" && return 0; diff "$work/want" "$work/have" | sed 's/^/  /'; return 1; }
install_log() { # FILE: the ntfs-macos lines of the install log since logmark
	sudo -n tail -n "+$((logmark + 1))" /var/log/install.log | grep -E 'ntfs-macos (pre|post)install' | sed -E 's/^.*(ntfs-macos (pre|post)install:)/  \1/' >"$1"
	cat "$1"
}
pkg_install() { # OUT: installer -pkg, its output in OUT, the scripts' log lines in OUT.log; returns its exit status
	logmark=$(sudo -n wc -l </var/log/install.log | tr -d ' ')
	sudo -n /usr/sbin/installer -pkg "$pkg" -target / >"$1" 2>&1
	_rc=$?
	cat "$1"
	install_log "$1.log"
	return "$_rc"
}

if [ "$mode" = userlocal ]; then
	# =====================================================================================
	echo "== I-PKG-06: a user-owned /usr/local/bin and /usr/local/share (Homebrew moved from an Intel Mac)"
	# =====================================================================================
	made_bin=0 made_share=0
	[ -d /usr/local/bin ] || { made_bin=1; sudo -n mkdir -p /usr/local/bin; }
	[ -d /usr/local/share ] || { made_share=1; sudo -n mkdir -p /usr/local/share; }
	sudo -n chown "$(id -un):admin" /usr/local/bin /usr/local/share
	sudo -n chmod 775 /usr/local/bin /usr/local/share
	printf '#!/bin/sh\necho brew tool\n' >/usr/local/bin/brew-tool
	chmod 755 /usr/local/bin/brew-tool
	snap_user() { for p in /usr/local/bin /usr/local/share; do stat -f '%N %Su:%Sg %Lp' "$p"; find "$p" -mindepth 1 -print; done | sort; }
	snap_user >"$work/user.before"
	echo "/usr/local/bin: $(stat -f '%Su:%Sg %Lp' /usr/local/bin); /usr/local/share: $(stat -f '%Su:%Sg %Lp' /usr/local/share)"
	pkg_install "$work/u1.out"
	rc=$?
	if [ "$rc" -eq 0 ]; then pass "the install succeeds with a user-owned /usr/local/bin and /usr/local/share"; else failf "the install exits $rc"; fi
	check "postinstall skipped the link and says how to run the CLI" sh -c "grep -q 'no link /usr/local/bin/ntfsmac' '$work/u1.out.log' && grep -q 'run the CLI as $CLI' '$work/u1.out.log'"
	check "no /usr/local/bin/ntfsmac was made" sh -c "[ ! -e $LINK ] && [ ! -L $LINK ]"
	snap_user >"$work/user.after"
	if cmp -s "$work/user.before" "$work/user.after"; then pass "nothing in /usr/local/bin or /usr/local/share was written or changed (owner, mode, files)"; else failf "the user-owned directories changed"; diff "$work/user.before" "$work/user.after" | sed 's/^/  /'; fi
	if set_ok; then pass "exactly the manifest's set is installed"; else failf "the installed set differs"; fi
	check "the daemon runs" wait_for 30 test -S /var/run/ntfsmacd.sock
	"$CLI" doctor >"$work/doctor.out" 2>&1
	rc=$?
	cat "$work/doctor.out"
	if [ "$rc" -eq 0 ] && ! grep -q '^problem:' "$work/doctor.out"; then pass "the CLI works by its full path: $CLI doctor exits 0 with no problem"; else failf "$CLI doctor exits $rc or reports a problem"; fi
	sudo -n "$CLI" journal-clear --label x --serial 00000000 disk99s1 >"$work/priv.out" 2>&1
	check "sudo $CLI journal-clear is answered by the daemon (the recorded requirement is the CLI's)" grep -q "The daemon does not know this volume" "$work/priv.out"

	echo "-- a root-owned /usr/local/bin with a foreign ntfsmac in it: the install leaves it alone"
	sudo -n chown root:wheel /usr/local/bin
	sudo -n chmod 755 /usr/local/bin
	printf '#!/bin/sh\necho foreign\n' >"$work/foreign"
	sudo -n install -m 755 "$work/foreign" "$LINK"
	pkg_install "$work/u2.out"
	rc=$?
	if [ "$rc" -eq 0 ]; then pass "the install (an upgrade) succeeds"; else failf "the install exits $rc"; fi
	check "postinstall left the foreign /usr/local/bin/ntfsmac alone and said so" sh -c "grep -q 'exists and is not ours' '$work/u2.out.log' && cmp -s '$work/foreign' $LINK"
	sudo -n "$LIBEXEC/uninstall.sh" >"$work/un.out" 2>&1
	rc=$?
	cat "$work/un.out"
	if [ "$rc" -eq 0 ]; then pass "uninstall.sh exits 0"; else failf "uninstall.sh exits $rc"; fi
	check "the foreign /usr/local/bin/ntfsmac is kept, unchanged" cmp -s "$work/foreign" "$LINK"
	check "uninstall.sh said why it kept it" grep -q "kept $LINK" "$work/un.out"
	check "the user's file in /usr/local/bin is untouched" sh -c "[ \"\$(/usr/local/bin/brew-tool)\" = 'brew tool' ]"
	check "nothing of the manifest is left" sh -c "[ ! -e $LIBEXEC ] && [ ! -e '/Library/Application Support/ntfs-macos' ] && [ ! -e $PLIST ] && ! pkgutil --pkg-info $PKGID >/dev/null 2>&1"
	sudo -n rm -f "$LINK"

	echo "-- the Installer and an older package's file in a user-owned /usr/local/bin"
	sudo -n chown "$(id -un):admin" /usr/local/bin
	sudo -n chmod 775 /usr/local/bin
	mkdir -p "$work/old$LIBEXEC" "$work/old/usr/local/bin"
	printf '#!/bin/sh\necho old\n' >"$work/old$LIBEXEC/ntfsmac-probe"
	printf '#!/bin/sh\necho old cli\n' >"$work/old/usr/local/bin/ntfsmac"
	chmod 755 "$work/old$LIBEXEC/ntfsmac-probe" "$work/old/usr/local/bin/ntfsmac"
	pkgbuild --root "$work/old" --identifier "$OLDID" --version 0.0.1 --install-location / "$work/old.pkg" >/dev/null 2>&1 || failf "pkgbuild of the old package"
	sudo -n /usr/sbin/installer -pkg "$work/old.pkg" -target / >/dev/null 2>&1 || failf "install of the old package"
	pkg_install "$work/u3.out"
	rc=$?
	if [ "$rc" -eq 0 ]; then pass "the upgrade from an older package with /usr/local/bin/ntfsmac in its receipt succeeds"; else failf "the upgrade exits $rc"; fi
	check "preinstall left that file (user-owned directory) and said so" grep -q 'left /usr/local/bin/ntfsmac of the earlier install' "$work/u3.out.log"
	check "the Installer did not delete the old receipt's file in the user-owned /usr/local/bin (another identifier, M6-F02)" sh -c "[ -f $LINK ] && [ ! -L $LINK ]"
	check "preinstall kept the receipt $OLDID and says to run uninstall.sh --old-receipt" sh -c "pkgutil --pkg-info $OLDID >/dev/null 2>&1 && grep -q 'kept the receipt $OLDID of the earlier layout.*uninstall.sh --old-receipt' '$work/u3.out.log'"
	sudo -n "$LIBEXEC/uninstall.sh" >"$work/un3.out" 2>&1 || failf "uninstall after the receipt case"
	check "uninstall.sh kept the regular file /usr/local/bin/ntfsmac, named it, and forgot both receipts" sh -c "[ -f $LINK ] && grep -q 'kept $LINK' '$work/un3.out' && ! pkgutil --pkg-info $OLDID >/dev/null 2>&1 && ! pkgutil --pkg-info $PKGID >/dev/null 2>&1"
	sudo -n rm -f "$LINK"

	echo "-- an upgrade of a developer install of the earlier layout (CLI file /usr/local/bin/ntfsmac, no receipt) with a user-owned /usr/local/bin"
	sudo -n mkdir -p "$LIBEXEC"
	printf '# manifest of the earlier layout\nfile 0755 root wheel /usr/local/bin/ntfsmac\nfile 0644 root wheel %s/manifest.txt\n' "$LIBEXEC" >"$work/oldmanifest"
	sudo -n install -m 644 -o root -g wheel "$work/oldmanifest" "$LIBEXEC/manifest.txt"
	printf '#!/bin/sh\necho old cli\n' >"$LINK"
	chmod 755 "$LINK"
	cp "$LINK" "$work/oldcli"
	pkg_install "$work/u4.out"
	rc=$?
	if [ "$rc" -eq 0 ]; then pass "the upgrade of the earlier developer install succeeds"; else failf "the upgrade exits $rc"; fi
	check "preinstall saw the developer install and left the old CLI file in the user-owned /usr/local/bin" sh -c "grep -q 'upgrading the developer install' '$work/u4.out.log' && grep -q 'left /usr/local/bin/ntfsmac of the earlier install' '$work/u4.out.log' && cmp -s '$work/oldcli' $LINK"
	check "the last line of postinstall is a plain WARNING with the remedy" sh -c "tail -n 1 '$work/u4.out.log' | grep -q 'WARNING: /usr/local/bin/ntfsmac .*Remove it by hand (sudo rm /usr/local/bin/ntfsmac) or run $CLI by its full path'"
	check "the installed CLI works by its full path" sh -c "'$CLI' list >/dev/null 2>&1"
	sudo -n "$LIBEXEC/uninstall.sh" >"$work/un2.out" 2>&1
	rc=$?
	cat "$work/un2.out"
	if [ "$rc" -eq 0 ]; then pass "uninstall.sh exits 0"; else failf "uninstall.sh exits $rc"; fi
	check "uninstall.sh keeps the old CLI file and names it" sh -c "[ -f $LINK ] && grep -q 'kept $LINK' '$work/un2.out'"
	sudo -n rm -f "$LINK" /usr/local/bin/brew-tool
	sudo -n chown root:wheel /usr/local/bin
	sudo -n chmod 755 /usr/local/bin
	[ "$made_bin" -eq 0 ] || sudo -n rmdir /usr/local/bin
	[ "$made_share" -eq 0 ] || sudo -n rmdir /usr/local/share
	sudo -n rm -rf "$work"
	echo "result: $fail failure(s)"
	[ "$fail" -eq 0 ]
	exit
fi

if [ "$mode" = oldrel ]; then
	# =====================================================================================
	echo "== I-PKG-06 oldrel: the published pre-release v0.1.0-dev.20261116 (receipt $OLDID), then this package"
	# =====================================================================================
	OLDPKG=$(find "$d/oldpkg" -name 'ntfs-macos-*.pkg' | head -n 1)
	UNTREE=$d/tree/packaging/uninstall.sh
	OLDDOC=/usr/local/share/doc/ntfs-macos
	[ -n "$OLDPKG" ] && [ -f "$UNTREE" ] || { failf "no pre-release package in $d/oldpkg or no tree uninstall.sh"; exit 1; }
	echo "pre-release: ${OLDPKG##*/} sha256 $(shasum -a 256 "$OLDPKG" | awk '{print $1}')"
	# the places the Installer could touch (the system's /private/var/db changes on its own and is left
	# out; the canary there is checked by itself), without the project's own directories
	outside() {
		{
			{
				find /usr/local /Library/LaunchDaemons -print 2>/dev/null
				find "/Library/Application Support" -maxdepth 1 -print 2>/dev/null
			} | grep -v -e "^$LIBEXEC\(/\|\$\)" -e "^$OLDDOC\(/\|\$\)" -e "^/Library/Application Support/ntfs-macos\$" \
				-e '^/usr/local/share/doc/ntfs-macos.moved\(/\|$\)' -e "^$LINK\$" -e "^$PLIST\$"
		} | sort
	}
	old_install() {
		sudo -n /usr/sbin/installer -pkg "$OLDPKG" -target / >"$work/old.out" 2>&1 || { failf "install of the pre-release"; cat "$work/old.out"; }
		check "the pre-release is installed: receipt $OLDID 0.1.0, its tools and $OLDDOC/LICENSES" sh -c "pkgutil --pkg-info $OLDID | grep -q '^version: 0.1.0' && [ -x $LIBEXEC/ntfsmac-worker ] && [ -f $OLDDOC/COPYING ] && [ -f $OLDDOC/LICENSES/GPL-2.0-or-later.txt ]"
	}
	both_gone() { ! pkgutil --pkg-info "$PKGID" >/dev/null 2>&1 && ! pkgutil --pkg-info "$OLDID" >/dev/null 2>&1; }
	nothing_ours() { [ ! -e "$LIBEXEC" ] && [ ! -e "$OLDDOC" ] && [ ! -e "/Library/Application Support/ntfs-macos" ] && [ ! -e "$PLIST" ] && [ ! -L "$LINK" ] && ! job_loaded && both_gone; }

	echo "-- the pre-release, then this package (an upgrade across identifiers)"
	had_doc=1
	[ -d /usr/local/share/doc ] || had_doc=0
	outside >"$work/out.0"
	old_install
	outside >"$work/out.1"
	pkg_install "$work/o1.out"
	rc=$?
	if [ "$rc" -eq 0 ]; then pass "this package installs over the pre-release (exit 0)"; else failf "the upgrade exits $rc"; fi
	check "the preflight passed as an upgrade of the earlier layout" grep -q 'preflight passed (upgrading the package 0.1.0 of the earlier layout)' "$work/o1.out.log"
	check "preinstall removed the earlier layout's documents ($OLDDOC with LICENSES and the ._ files)" sh -c "[ ! -e $OLDDOC ] && [ ! -e $LIBEXEC/._uninstall.sh ]"
	check "the receipt $OLDID is forgotten and $PKGID is there" sh -c "! pkgutil --pkg-info $OLDID >/dev/null 2>&1 && pkgutil --pkg-info $PKGID >/dev/null 2>&1"
	pkgutil --files "$PKGID" >"$work/receipt.list"
	if out=$(sh tree/packaging/payload-check.sh "$MANIFEST" "$work/receipt.list"); then pass "the receipt $PKGID matches the manifest"; else failf "the receipt differs"; echo "$out"; fi
	if set_ok; then pass "exactly the manifest's set is installed"; else failf "the installed set differs"; fi
	check "the daemon runs" wait_for 30 test -S /var/run/ntfsmacd.sock
	outside >"$work/out.2"
	# the link /usr/local/bin/ntfsmac is the package's own (excluded above); nothing else outside changed
	if cmp -s "$work/out.1" "$work/out.2"; then pass "the Installer and the scripts changed nothing outside the project's directories"; else failf "something outside the project's directories changed"; diff "$work/out.1" "$work/out.2" | sed 's/^/  /'; fi
	sudo -n "$LIBEXEC/uninstall.sh" >"$work/o1.un" 2>&1
	rc=$?
	cat "$work/o1.un"
	if [ "$rc" -eq 0 ]; then pass "uninstall.sh exits 0"; else failf "uninstall.sh exits $rc"; fi
	check "nothing of either layout is left, both receipts are forgotten" nothing_ours

	echo "-- the pre-release alone, removed by the new uninstall.sh (M6-F04)"
	old_install
	sudo -n sh "$UNTREE" --manifest "$MANIFEST" >"$work/o2.un" 2>&1
	rc=$?
	cat "$work/o2.un"
	if [ "$rc" -eq 0 ]; then pass "uninstall.sh exits 0"; else failf "uninstall.sh exits $rc"; fi
	check "the pre-release is gone: tools, documents, LICENSES, receipt" nothing_ours
	outside >"$work/out.3"
	if cmp -s "$work/out.1" "$work/out.3"; then pass "nothing outside the project's directories changed"; else failf "something outside changed"; diff "$work/out.1" "$work/out.3" | sed 's/^/  /'; fi

	echo "-- a user-owned /usr/local/share/doc whose ntfs-macos is a link to a root-owned directory (M6-F02)"
	old_install
	CANARY=/private/var/db/ntfsmac-canary
	sudo -n mkdir -p "$CANARY/LICENSES"
	for f in COPYING README.md BUILD-INFO ._COPYING LICENSES/README.md LICENSES/GPL-2.0-or-later.txt; do echo canary | sudo -n tee "$CANARY/$f" >/dev/null; done
	(cd "$CANARY" && find . -print | sort) >"$work/canary.before"
	docorig=$(stat -f '%Su:%Sg %Lp' /usr/local/share/doc)
	sudo -n chown "$(id -un)" /usr/local/share/doc
	sudo -n mv "$OLDDOC" "$OLDDOC.moved"
	ln -s "$CANARY" "$OLDDOC"
	echo "/usr/local/share/doc: $(stat -f '%Su:%Sg %Lp' /usr/local/share/doc); $OLDDOC -> $(readlink "$OLDDOC")"
	canary_ok() { [ "$(cd "$CANARY" && find . -print | sort)" = "$(cat "$work/canary.before")" ] && [ "$(cat "$CANARY/COPYING")" = canary ]; }
	pkg_install "$work/o3.out"
	rc=$?
	if [ "$rc" -eq 0 ]; then pass "the upgrade succeeds"; else failf "the upgrade exits $rc"; fi
	check "the canary files behind the link are all there" canary_ok
	check "preinstall left the earlier documents, kept the receipt $OLDID and says to run uninstall.sh --old-receipt" sh -c "grep -q 'left $OLDDOC/COPYING of the earlier install' '$work/o3.out.log' && grep -q 'kept the receipt $OLDID of the earlier layout.*--old-receipt' '$work/o3.out.log' && pkgutil --pkg-info $OLDID >/dev/null 2>&1"
	sudo -n "$LIBEXEC/uninstall.sh" --old-receipt >"$work/o3.old" 2>&1
	rc=$?
	cat "$work/o3.old"
	if [ "$rc" -ne 0 ]; then pass "uninstall.sh --old-receipt exits $rc: it keeps what lies behind the link"; else failf "uninstall.sh --old-receipt exits 0"; fi
	check "...the receipt $OLDID is kept" pkgutil --pkg-info "$OLDID"
	check "...the canary files are all there" canary_ok
	if set_ok; then pass "...the current install is unchanged"; else failf "--old-receipt changed the current install"; fi
	check "...and it named the kept paths" grep -q "kept \"$OLDDOC/COPYING\": the directory \"/usr/local/share/doc\" is owned by $(id -un)" "$work/o3.old"
	sudo -n "$LIBEXEC/uninstall.sh" >"$work/o3.un" 2>&1
	rc=$?
	cat "$work/o3.un"
	if [ "$rc" -eq 1 ]; then pass "uninstall.sh exits 1: it removed the current install and kept the earlier documents (named)"; else failf "uninstall.sh exits $rc"; fi
	check "...the canary files are all there" canary_ok
	check "...the current install is gone, $PKGID forgotten, $OLDID kept" sh -c "[ ! -e '/Library/Application Support/ntfs-macos' ] && [ ! -e $PLIST ] && ! pkgutil --pkg-info $PKGID >/dev/null 2>&1 && pkgutil --pkg-info $OLDID >/dev/null 2>&1"
	echo "-- after the fix (the link replaced by the real directory, /usr/local/share/doc root-owned again)"
	rm -f "$OLDDOC"
	sudo -n mv "$OLDDOC.moved" "$OLDDOC"
	sudo -n chown "${docorig%%:*}" /usr/local/share/doc
	sudo -n chmod "${docorig#* }" /usr/local/share/doc
	sudo -n sh "$UNTREE" --manifest "$MANIFEST" --old-receipt >"$work/o4.un" 2>&1
	rc=$?
	cat "$work/o4.un"
	if [ "$rc" -eq 0 ]; then pass "uninstall.sh --old-receipt exits 0"; else failf "uninstall.sh --old-receipt exits $rc"; fi
	check "nothing of either layout is left, both receipts are forgotten" nothing_ours
	check "the canary files are all there" canary_ok
	sudo -n rm -rf "$CANARY"
	[ "$had_doc" -eq 1 ] || sudo -n rmdir /usr/local/share/doc /usr/local/share 2>/dev/null
	outside >"$work/out.4"
	if cmp -s "$work/out.0" "$work/out.4"; then pass "the guest outside the project's directories is as before the first install"; else note "outside differs from before the first install (shared parents the packages made):"; diff "$work/out.0" "$work/out.4" | sed 's/^/  /'; fi
	sudo -n rm -rf "$work"
	echo "result: $fail failure(s)"
	[ "$fail" -eq 0 ]
	exit
fi

echo "== I-PKG-05: preflight refusals"
echo "-- macFUSE missing"
sudo -n mv "$MACFUSE" "$ASIDE"
refused "macFUSE missing" "macFUSE"
echo "-- macFUSE too old (a stand-in bundle that says 5.4.2)"
sudo -n mkdir -p "$MACFUSE/Contents"
plutil -create xml1 "$work/Info.plist"
plutil -insert CFBundleIdentifier -string io.macfuse.filesystems.macfuse "$work/Info.plist"
plutil -insert CFBundleShortVersionString -string 5.4.2 "$work/Info.plist"
plutil -insert CFBundleVersion -string 5.4.2 "$work/Info.plist"
sudo -n cp "$work/Info.plist" "$MACFUSE/Contents/Info.plist"
refused "macFUSE 5.4.2" "5.4.2"
sudo -n rm -rf "$MACFUSE"
sudo -n mv "$ASIDE" "$MACFUSE"
check "macFUSE is back ($macfuse_ver)" [ "$(plutil -extract CFBundleShortVersionString raw -o - "$MACFUSE/Contents/Info.plist" 2>/dev/null)" = "$macfuse_ver" ]

echo "-- other NTFS drivers"
for b in "ufsd_NTFS.fs=Paragon NTFS" "tuxera_ntfs.fs=Tuxera NTFS"; do
	sudo -n mkdir -p "/Library/Filesystems/${b%%=*}/Contents"
	refused "${b%%=*}" "${b#*=}"
	sudo -n rm -rf "/Library/Filesystems/${b%%=*}"
done
sudo -n mkdir -p /Library/Filesystems/Acme-NTFS.fs/Contents
refused "Acme-NTFS.fs" "another NTFS driver"
sudo -n rm -rf /Library/Filesystems/Acme-NTFS.fs
sudo -n mkdir -p /Library/Extensions/acme_ntfs.kext/Contents
pre_refused "acme_ntfs.kext" "NTFS kernel extension"
sudo -n rm -rf /Library/Extensions/acme_ntfs.kext

echo "-- files in the way"
had_lx=1
[ -d /usr/local/libexec ] || had_lx=0
sudo -n mkdir -p "$LIBEXEC"
printf '#!/bin/sh\necho other\n' >"$work/other"
sudo -n install -m 755 "$work/other" "$LIBEXEC/ntfsmacd"
refused "a foreign $LIBEXEC/ntfsmacd" "no install of ntfs-macos is recorded"
check "...and the foreign file is unchanged" cmp -s "$work/other" "$LIBEXEC/ntfsmacd"
sudo -n rm -rf "$LIBEXEC"
sudo -n ln -s /tmp "$LIBEXEC"
pre_refused "a symbolic link at $LIBEXEC" "symbolic link"
sudo -n rm -f "$LIBEXEC"
orig=$(stat -f '%Su:%Sg %Lp' /usr/local/libexec)
sudo -n chown "$(id -un)" /usr/local/libexec
refused "a user-owned /usr/local/libexec (above the programs root runs)" "owned by $(id -un)"
sudo -n chown "${orig% *}" /usr/local/libexec
sudo -n chmod "${orig#* }" /usr/local/libexec

echo "-- audit M6-F01: /usr/local/libexec a symbolic link to a root-only directory"
TARGET=/private/var/db/ntfsmac-f01-target
sudo -n mkdir -m 755 "$TARGET"
sudo -n mv /usr/local/libexec /usr/local/libexec.pkgpre-aside
sudo -n ln -s "$TARGET" /usr/local/libexec
lorig=$(stat -f '%Su:%Sg %Lp' /usr/local)
echo "/usr/local: $lorig; /usr/local/libexec -> $(readlink /usr/local/libexec) ($(stat -f '%Su:%Sg %Lp' "$TARGET"))"
refused "a root-owned /usr/local, /usr/local/libexec a link to a root-only directory" 'the directory "/usr/local/libexec" above "/usr/local/libexec/ntfs-macos" is a symbolic link'
check "...and nothing appeared in the link's target" sh -c "[ -z \"\$(ls -A $TARGET)\" ]"
sudo -n chown "$(id -un)" /usr/local
refused "a user-owned /usr/local, /usr/local/libexec a link to a root-only directory (the case of the audit)" "the directory \"/usr/local\" above \"/usr/local/libexec/ntfs-macos\" is owned by $(id -un)"
check "...and nothing appeared in the link's target" sh -c "[ -z \"\$(ls -A $TARGET)\" ]"
sudo -n chown "${lorig%%:*}" /usr/local
sudo -n chmod "${lorig#* }" /usr/local
sudo -n rm -f /usr/local/libexec
sudo -n mv /usr/local/libexec.pkgpre-aside /usr/local/libexec
sudo -n rmdir "$TARGET"
check "/usr/local and /usr/local/libexec are back ($lorig, a real directory)" sh -c "[ \"\$(stat -f '%Su:%Sg %Lp' /usr/local)\" = '$lorig' ] && [ -d /usr/local/libexec ] && [ ! -L /usr/local/libexec ]"
[ "$had_lx" -eq 1 ] || sudo -n rmdir /usr/local/libexec

# =====================================================================================
echo "== I-PKG-05: an upgrade of an older package of ours"
# =====================================================================================
# an older package of ours in the earlier layout: the CLI as the file /usr/local/bin/ntfsmac, the
# documents and a LICENSES directory in /usr/local/share/doc/ntfs-macos, a file this package does
# not ship
OLDDOC=/usr/local/share/doc/ntfs-macos
mkdir -p "$work/old$LIBEXEC" "$work/old$OLDDOC/LICENSES" "$work/old/usr/local/bin"
printf '#!/bin/sh\necho old\n' >"$work/old$LIBEXEC/ntfsmac-probe"
printf '#!/bin/sh\necho old tool\n' >"$work/old$LIBEXEC/old-tool"
printf '#!/bin/sh\necho old cli\n' >"$work/old/usr/local/bin/ntfsmac"
chmod 755 "$work/old$LIBEXEC/ntfsmac-probe" "$work/old$LIBEXEC/old-tool" "$work/old/usr/local/bin/ntfsmac"
echo licence >"$work/old$OLDDOC/LICENSES/GPL-2.0-or-later.txt"
echo text >"$work/old$OLDDOC/COPYING"
pkgbuild --root "$work/old" --identifier "$OLDID" --version 0.0.1 --install-location / "$work/old.pkg" >/dev/null 2>&1 || failf "pkgbuild of the old package"
sudo -n /usr/sbin/installer -pkg "$work/old.pkg" -target / >/dev/null 2>&1 || failf "install of the old package"
check "the old package 0.0.1 ($OLDID) is installed, with /usr/local/bin/ntfsmac as a file" sh -c "pkgutil --pkg-info $OLDID | grep -q '^version: 0.0.1' && [ -f $LINK ] && [ ! -L $LINK ]"
pkg_install "$work/up.out"
rc=$?
if [ "$rc" -eq 0 ]; then pass "the release package installs over the old one (exit 0)"; else failf "the upgrade exits $rc"; fi
check "the preflight passed as an upgrade of package 0.0.1 of the earlier layout" grep -q 'preflight passed (upgrading the package 0.0.1 of the earlier layout)' "$work/up.out.log"
check "the receipt $OLDID of the earlier layout is forgotten" sh -c "! pkgutil --pkg-info $OLDID >/dev/null 2>&1 && grep -q 'forgot the receipt $OLDID' '$work/up.out.log'"
check "the old package's extra files are gone (old-tool, $OLDDOC with LICENSES)" sh -c "[ ! -e $LIBEXEC/old-tool ] && [ ! -e $OLDDOC ]"
check "the old CLI file was replaced by the link to $CLI" [ "$(readlink "$LINK")" = "$CLI" ]
pkgutil --files "$PKGID" >"$work/receipt.list"
if out=$(sh tree/packaging/payload-check.sh "$MANIFEST" "$work/receipt.list"); then pass "the receipt now matches the manifest"; else failf "the receipt differs"; echo "$out"; fi
if set_ok; then pass "exactly the manifest's set is installed"; else failf "the installed set differs"; fi
check "the probe is the new one, not the old script" sh -c "! grep -q 'echo old' $LIBEXEC/ntfsmac-probe"
check "the daemon runs" wait_for 30 test -S /var/run/ntfsmacd.sock

# =====================================================================================
echo "== I-PKG-05: a downgrade is refused (owner decision 2026-11-26, issue #10)"
# =====================================================================================
# The installed version is made newer than this package: first in the receipt (what pkgutil
# --pkg-info reads), then, with the receipt moved aside, in the installed BUILD-INFO (a developer
# install has no receipt). Each time the install is refused before anything changes.
RCPT=/var/db/receipts/$PKGID
BI="/Library/Application Support/ntfs-macos/doc/BUILD-INFO"
newv=$(pkgutil --pkg-info "$PKGID" | sed -n 's/^version: //p')
check "the package's preinstall carries its version $newv" grep -qx "new_version='$newv'" "$PRE"
have_set >"$work/dg.before"
dpid=$(sudo -n launchctl print "system/$LABEL" 2>/dev/null | sed -n 's/^[[:space:]]*pid = //p' | head -n 1)
sudo -n cp "$RCPT.plist" "$work/rcpt.plist"
sudo -n plutil -replace PackageVersion -string 99.0.0 "$RCPT.plist"
check "the receipt now says 99.0.0" sh -c "pkgutil --pkg-info $PKGID | grep -qx 'version: 99.0.0'"
logmark=$(sudo -n wc -l </var/log/install.log | tr -d ' ')
sudo -n /usr/sbin/installer -pkg "$pkg" -target / >"$work/dg.out" 2>&1
rc=$?
install_log "$work/dg.log"
want="this package is ntfs-macos $newv, older than the installed 99.0.0 (the receipt $PKGID)"
if [ "$rc" -ne 0 ] && grep -q -F -- "$want" "$work/dg.log"; then pass "installer refuses the downgrade: $want"; else failf "downgrade: installer exit $rc, reason '$want' not found"; fi
check "...the receipt still says 99.0.0" sh -c "pkgutil --pkg-info $PKGID | grep -qx 'version: 99.0.0'"
have_set >"$work/dg.after"
check "...the installed set is unchanged" cmp -s "$work/dg.before" "$work/dg.after"
check "...the daemon was not stopped (pid $dpid)" sh -c "[ -n '$dpid' ] && [ \"\$(sudo -n launchctl print system/$LABEL 2>/dev/null | sed -n 's/^[[:space:]]*pid = //p' | head -n 1)\" = '$dpid' ]"
check "...the install log has no line of a stop or a removal" sh -c "! grep -q -e 'stopping the running daemon' -e 'removing ' '$work/dg.log'"
sudo -n /bin/sh "$PRE" "$pkg" / / / >"$work/pre.out" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q -F -- "$want" "$work/pre.out"; then pass "preinstall alone refuses the downgrade"; else failf "downgrade: preinstall alone exit $rc"; fi
sudo -n cp "$work/rcpt.plist" "$RCPT.plist"
check "the receipt says $newv again" sh -c "pkgutil --pkg-info $PKGID | grep -qx 'version: $newv'"
echo "-- without a receipt: the installed BUILD-INFO"
sudo -n mkdir "$work/rcpt.aside"
sudo -n mv "$RCPT.plist" "$RCPT.bom" "$work/rcpt.aside/"
sudo -n cp "$BI" "$work/bi.orig"
sudo -n sed -i '' 's/^version .*/version      99.0.0/' "$BI"
check "no receipt, BUILD-INFO says 99.0.0" sh -c "! pkgutil --pkg-info $PKGID >/dev/null 2>&1 && grep -qx 'version      99.0.0' '$BI'"
sudo -n /bin/sh "$PRE" "$pkg" / / / >"$work/pre.out" 2>&1
rc=$?
want="this package is ntfs-macos $newv, older than the installed 99.0.0 (the installed BUILD-INFO)"
echo "  preinstall alone (exit $rc): $(grep -m1 REFUSED "$work/pre.out" | cut -c1-200)"
if [ "$rc" -ne 0 ] && grep -q -F -- "$want" "$work/pre.out"; then pass "preinstall refuses the downgrade of a developer install: $want"; else failf "BUILD-INFO downgrade: preinstall exit $rc, reason '$want' not found"; fi
sudo -n cp "$work/bi.orig" "$BI"
sudo -n mv "$work/rcpt.aside/$PKGID.plist" "$work/rcpt.aside/$PKGID.bom" /var/db/receipts/
check "receipt and BUILD-INFO are back ($newv)" sh -c "pkgutil --pkg-info $PKGID | grep -qx 'version: $newv' && grep -qx 'version      $newv' '$BI'"
echo "-- the real version next to this one: one patch newer is refused, the earlier 0.1.0 is upgraded"
nextv=$(printf '%s\n' "$newv" | awk -F. '{ $NF = $NF + 1; print }' OFS=.)
sudo -n plutil -replace PackageVersion -string "$nextv" "$RCPT.plist"
sudo -n /bin/sh "$PRE" "$pkg" / / / >"$work/pre.out" 2>&1
rc=$?
want="this package is ntfs-macos $newv, older than the installed $nextv (the receipt $PKGID)"
if [ "$rc" -ne 0 ] && grep -q -F -- "$want" "$work/pre.out"; then pass "preinstall refuses $newv over $nextv"; else failf "preinstall exit $rc over $nextv, reason '$want' not found"; fi
sudo -n plutil -replace PackageVersion -string 0.1.0 "$RCPT.plist"
pkg_install "$work/up01.out"
rc=$?
if [ "$rc" -eq 0 ]; then pass "$newv installs over an installed 0.1.0 (exit 0)"; else failf "the upgrade from 0.1.0 exits $rc"; fi
check "...the log names the installed 0.1.0 and this package $newv" grep -q "installed version 0.1.0 (the receipt $PKGID); this package is $newv" "$work/up01.out.log"
check "...the receipt says $newv again" sh -c "pkgutil --pkg-info $PKGID | grep -qx 'version: $newv'"
echo "-- the same version again: installed (a reinstall is not a downgrade)"
pkg_install "$work/re.out"
rc=$?
if [ "$rc" -eq 0 ]; then pass "the same version installs again (exit 0)"; else failf "the reinstall exits $rc"; fi
check "...the log names the installed version $newv" grep -q "installed version $newv (the receipt $PKGID); this package is $newv" "$work/re.out.log"
if set_ok; then pass "...exactly the manifest's set is installed"; else failf "the installed set differs after the reinstall"; fi
check "...the daemon runs" wait_for 30 test -S /var/run/ntfsmacd.sock

sudo -n rm -rf "$work"
echo "result: $fail failure(s)"
[ "$fail" -eq 0 ]
