#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# T-M4-10: the part of pkg_vm.sh that runs inside the macOS guest, as the guest user (who has
# passwordless sudo). Usage: pkg_vm_guest.sh DIR, where DIR holds a copy of the source tree and
# (optionally) a prebuilt NTFS-3G prefix in DIR/build/deps. Everything here changes only the guest.
#
# I-PKG-01: packaging/dev-install.sh installs; every path of the manifest exists with the manifest's
#           owner, group and mode and nothing else is below the owned directories; the daemon has no
#           test seams; the recorded CLI requirement is the installed CLI's cdhash and the CLI has
#           the hardened runtime; the authorization right has its intended definition; the job is
#           loaded and the daemon answers; `ntfsmac doctor` runs; the privileged door accepts the
#           CLI; a second install over the first leaves the same state.
#           It refuses (before building anything) when a directory above an installed path is
#           user-owned or group-writable, and when the daemon of the build tree has test seams.
# I-PKG-02: packaging/uninstall.sh refuses manifests that name paths outside the project and
#           removes nothing then; --dry-run removes nothing; a real run removes exactly the manifest
#           (the job, the right, every file, every empty directory) and leaves foreign files, their
#           directory, the shared directories and the state alone; --purge-state removes the state;
#           the files left on the Mac equal those before the install; a package receipt of the older
#           kind is removed, and one with a path outside the project is refused.
#
# Prints PASS, FAIL and NOTE lines; exit 0 only if no FAIL.
set -u
PATH=/opt/homebrew/bin:/usr/local/bin:/sbin:/usr/sbin:$PATH
export PATH

d=${1:?directory}
cd "$d" || exit 2
fail=0
LIBEXEC=/usr/local/libexec/ntfs-macos
CLI=/usr/local/bin/ntfsmac
LABEL=org.ntfs-macos.ntfsmacd
PLIST=/Library/LaunchDaemons/$LABEL.plist
RIGHT=org.ntfs-macos.journal-modify
SUPPORT="/Library/Application Support/ntfs-macos"
SOCK=/var/run/ntfsmacd.sock
MANIFEST=$d/packaging/manifest.txt
work=$(cd -P "$(mktemp -d /tmp/pkgvm.XXXXXX)" && pwd)

pass() { echo "PASS: $*"; }
failf() { echo "FAIL: $*"; fail=$((fail + 1)); }
note() { echo "NOTE: $*"; }
check() { desc=$1; shift; if "$@"; then pass "$desc"; else failf "$desc"; fi; }
nocheck() { desc=$1; shift; if "$@"; then failf "$desc"; else pass "$desc"; fi; }
contains() { case $1 in *"$2"*) return 0 ;; esac; return 1; }

wait_for() { # SECONDS CMD...
	t=$1; shift; i=0
	while [ "$i" -lt "$t" ]; do
		if "$@" >/dev/null 2>&1; then return 0; fi
		sleep 1; i=$((i + 1))
	done
	return 1
}

job_loaded() { sudo -n launchctl print "system/$LABEL" >/dev/null 2>&1; }
right_group() { # the group of the stored right, empty when there is none
	sudo -n security authorizationdb read "$RIGHT" >"$work/right.read" 2>/dev/null || return 0
	plutil -extract group raw -o - "$work/right.read" 2>/dev/null || true
}
right_class() {
	sudo -n security authorizationdb read "$RIGHT" >"$work/right.read" 2>/dev/null || return 0
	plutil -extract class raw -o - "$work/right.read" 2>/dev/null || true
}
right_is_ours() { [ "$(right_class)" = user ] && [ "$(right_group)" = admin ]; }

# manifest entries: "kind mode owner group path" through the read loop below
manifest_paths() { # KIND...: the paths of those kinds
	awk -v kinds="$*" 'BEGIN { n = split(kinds, k, " "); for (i = 1; i <= n; i++) want[k[i]] = 1 }
		/^[ \t]*(#|$)/ { next }
		{ p = $0; for (i = 0; i < 4; i++) sub(/^[ \t]*[^ \t]+/, "", p); sub(/^[ \t]+/, "", p); sub(/[ \t]+$/, "", p)
		  if ($1 in want) print p }' "$MANIFEST"
}

# every dir and file of the manifest is there, with the owner, group and mode of the manifest
entries_match() {
	bad=0
	while read -r kind mode owner group path || [ -n "$kind" ]; do
		case $kind in dir | file) ;; *) continue ;; esac
		got=$(stat -f '%Lp %Su %Sg' "$path" 2>/dev/null) || { echo "  missing: $path"; bad=$((bad + 1)); continue; }
		want="${mode#0} $owner $group"
		[ "$got" = "$want" ] || { echo "  $path: $got, the manifest says $want"; bad=$((bad + 1)); }
		case $kind in
		file) { [ -f "$path" ] && [ ! -L "$path" ]; } || { echo "  not a regular file: $path"; bad=$((bad + 1)); } ;;
		dir) { [ -d "$path" ] && [ ! -L "$path" ]; } || { echo "  not a directory: $path"; bad=$((bad + 1)); } ;;
		esac
	done <"$MANIFEST"
	[ "$bad" -eq 0 ]
}

# the set of paths below the owned directories (and the two single files) equals the manifest's
installed_set() {
	{
		find "$LIBEXEC" /usr/local/share/doc/ntfs-macos "$SUPPORT" \( -type f -o -type d -o -type l \) -print 2>/dev/null
		ls -d "$CLI" "$PLIST" 2>/dev/null
	} | sort
}
set_equal() {
	manifest_paths dir file | sort >"$work/want.set"
	installed_set | grep -v '/foreign\.txt$' >"$work/have.set" || true # the foreign file of I-PKG-02 is not ours
	if cmp -s "$work/want.set" "$work/have.set"; then return 0; fi
	diff "$work/want.set" "$work/have.set" | sed 's/^/  /'
	return 1
}

# nothing of the project is left (the manifest's paths, the job, the right, the socket)
nothing_left() {
	bad=0
	manifest_paths dir file | while IFS= read -r p; do
		if [ -e "$p" ] || [ -L "$p" ]; then echo "  left: $p"; exit 1; fi
	done || bad=1
	job_loaded && { echo "  the job is still loaded"; bad=1; }
	right_is_ours && { echo "  the right is still there"; bad=1; }
	[ -S "$SOCK" ] && { echo "  the control socket is still there"; bad=1; }
	[ "$bad" -eq 0 ]
}

# what a call of `$@` leaves as installed: the manifest's set, the job and the right
installed_ok() { entries_match && set_equal; }

# a tree snapshot of the places the project writes to, without directories (shared parents such as
# /usr/local/share/doc may be made by the install and stay, as the manifest says)
snapshot() {
	{
		find /usr/local /Library/LaunchDaemons "$SUPPORT" /var/db/ntfs-macos -not -type d -print 2>/dev/null
		ls -d /var/log/ntfsmacd.log 2>/dev/null
	} | sort
}

echo "guest: $(sw_vers -productVersion) $(uname -m), user $(id -un) uid $(id -u); manifest $(grep -cv '^[[:space:]]*\(#\|$\)' "$MANIFEST") entries"

[ ! -e "$LIBEXEC" ] && [ ! -e "$CLI" ] && [ ! -e "$PLIST" ] || { failf "the guest already has an install; use a fresh clone"; exit 1; }
snapshot >"$work/snap.before"
note "before the install: $(wc -l <"$work/snap.before" | tr -d ' ') files below /usr/local, the launchd and support directories"

# =====================================================================================
echo "== I-PKG-01: dev-install.sh"
# =====================================================================================
echo "-- a directory above an installed path that is not root-only: the install is refused"
ANC=/usr/local/share/doc
made=
[ -d /usr/local/share ] || made="$made /usr/local/share"
[ -d "$ANC" ] || made="$made $ANC"
sudo -n mkdir -p "$ANC"
orig=$(stat -f '%Su:%Sg %Lp' "$ANC")
restore_anc() {
	if [ -n "$made" ]; then
		for m in $ANC /usr/local/share; do case "$made" in *" $m"*) sudo -n rmdir "$m" 2>/dev/null ;; esac; done
	else
		sudo -n chown "${orig% *}" "$ANC"
		sudo -n chmod "${orig#* }" "$ANC"
	fi
}
sudo -n chown "$(id -un)" "$ANC"
sh packaging/dev-install.sh >"$work/anc1.out" 2>&1
rc=$?
tail -n 3 "$work/anc1.out" | cut -c1-260
if [ "$rc" -ne 0 ] && grep -q "owned by $(id -un)" "$work/anc1.out"; then pass "a user-owned ancestor ($ANC) is refused (exit $rc)"; else failf "user-owned ancestor: exit $rc"; fi
sudo -n chown root:wheel "$ANC"
sudo -n chmod 775 "$ANC"
sh packaging/dev-install.sh >"$work/anc2.out" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q "mode 775" "$work/anc2.out"; then pass "a group-writable ancestor is refused (exit $rc)"; else failf "group-writable ancestor: exit $rc"; fi
check "...before anything was built or installed (no build tree, no install)" sh -c "[ ! -e build/product ] && [ ! -e $LIBEXEC ] && [ ! -e $PLIST ]"
restore_anc
echo "ancestor restored: $(stat -f '%Su:%Sg %Lp' "$ANC" 2>/dev/null || echo removed)"

sh packaging/dev-install.sh --jobs 4 >"$work/install1.log" 2>&1
rc=$?
echo "-- dev-install.sh (last 25 lines)"
tail -n 25 "$work/install1.log"
if [ "$rc" -eq 0 ]; then pass "dev-install.sh exits 0"; else failf "dev-install.sh exits $rc"; echo "-- the whole log"; cat "$work/install1.log"; exit 1; fi
contains "$(cat "$work/install1.log")" "configuring the product build" && pass "built with the product configuration" || failf "no product configuration in the log"
check "build tree is the product preset (test seams off, root fallback on)" sh -c "grep -q '^NTFSMAC_TEST_SEAMS:BOOL=OFF\$' build/product/CMakeCache.txt && grep -q '^NTFSMAC_AUTHZ_FALLBACK_ROOT:BOOL=ON\$' build/product/CMakeCache.txt"

echo "-- every path of the manifest, owner, group and mode"
if out=$(entries_match); then pass "all dirs and files of the manifest are installed with the manifest's owner, group and mode ($(manifest_paths dir file | wc -l | tr -d ' ') paths)"; else failf "manifest paths differ from the install"; echo "$out"; fi
if out=$(set_equal); then pass "the installed set below the owned directories equals the manifest (nothing extra)"; else failf "installed set differs from the manifest"; echo "$out"; fi
ls -lde "$LIBEXEC" "$LIBEXEC"/* "$CLI" "$PLIST" "$SUPPORT" "$SUPPORT"/* "$SUPPORT"/sandbox/* 2>&1 | sed 's/@ / /' | awk '{ $6 = $7 = $8 = ""; print }' | sed 's/  */ /g'
check "sandbox profiles are root:wheel 0644 (spawn.c refuses anything else)" sh -c "[ \"\$(stat -f '%Lp %Su %Sg' \"$SUPPORT\"/sandbox/*.sb | sort -u)\" = '644 root wheel' ]"
check "installed manifest and uninstaller are the ones of the source tree" sh -c "cmp -s $MANIFEST $LIBEXEC/manifest.txt && cmp -s packaging/uninstall.sh $LIBEXEC/uninstall.sh"
check "the shipped plist is installed unchanged" cmp -s integration/launchd/$LABEL.plist "$PLIST"
check "the plist is valid" plutil -lint "$PLIST"

echo "-- the product has no test seams"
nocheck "the installed daemon has none of --console-uid, --child-log, --journal-dir" sh -c "strings $LIBEXEC/ntfsmacd | grep -q -e '--console-uid' -e '--child-log' -e '--journal-dir'"
check "BUILD-INFO says preset product" grep -q '^preset .*product' /usr/local/share/doc/ntfs-macos/BUILD-INFO

echo "-- the CLI and its recorded code requirement (ADR-019 check 3)"
req=$(cat "$SUPPORT/cli-requirement" 2>/dev/null)
echo "recorded: $req"
hash=$(codesign -d -vvv "$CLI" 2>&1 | sed -n 's/^CDHash=//p')
check "cli-requirement is the cdhash of the installed CLI" [ "$req" = "cdhash H\"$hash\"" ]
check "the installed CLI has the hardened runtime" sh -c "codesign -d --verbose=2 $CLI 2>&1 | grep -q 'flags=.*runtime'"
check "the installed CLI is not the unsigned build output (re-signed, hash differs from the build tree's CLI)" sh -c "[ \"\$(codesign -d -vvv build/product/src/cli/ntfsmac 2>&1 | sed -n 's/^CDHash=//p')\" != \"$hash\" ]"

echo "-- the authorization right (ADR-019)"
sudo -n security authorizationdb read "$RIGHT" 2>/dev/null | grep -A1 -E '<key>(class|group|shared|timeout)</key>' | tr -d '\t' | tr '\n' ' '
echo
check "right $RIGHT: class user" [ "$(right_class)" = user ]
check "right $RIGHT: group admin" [ "$(right_group)" = admin ]
check "right $RIGHT: shared false" [ "$(plutil -extract shared raw -o - "$work/right.read" 2>/dev/null)" = false ]
check "right $RIGHT: timeout 0" [ "$(plutil -extract timeout raw -o - "$work/right.read" 2>/dev/null)" = 0 ]

echo "-- the launchd job"
check "the job is loaded" job_loaded
check "the control socket appears" wait_for 30 test -S "$SOCK"
check "the privileged XPC service is registered with launchd" sh -c "sudo -n launchctl print system/$LABEL | grep -q org.ntfs-macos.ctl.privileged"
check "the job runs the installed daemon" sh -c "sudo -n launchctl print system/$LABEL | grep -qF 'program = $LIBEXEC/ntfsmacd'"
check "the daemon reads no warning about a missing right or requirement" sh -c "! sudo -n grep -q -e 'right org.ntfs-macos.journal-modify' -e 'cli-requirement' /var/log/ntfsmacd.log"
echo "-- daemon log"
sudo -n sed -n '1,15p' /var/log/ntfsmacd.log 2>&1

echo "-- ntfsmac list and doctor"
"$CLI" list 2>&1 | sed -n '1,10p'
"$CLI" doctor >"$work/doctor.out" 2>&1
rc=$?
cat "$work/doctor.out"
if [ "$rc" -eq 0 ]; then pass "ntfsmac doctor exits 0"; else failf "ntfsmac doctor exits $rc"; fi
# findings print as "code: N" then "ok: yes|no"; code 3 is the macFUSE extension state, for which the
# daemon has no hook yet (T-M4-07 open item), so it is reported not ok on every Mac
notok=$(awk '/code:/ { c = $2 } /ok:/ && $2 == "no" { printf "%s ", c }' "$work/doctor.out")
if [ "$notok" = "3 " ] || [ -z "$notok" ]; then pass "doctor: macFUSE installed, version ok, no conflicting driver, journal readable (not ok: ${notok:-none})"; else failf "doctor reports not ok: $notok"; fi
[ "$notok" = "3 " ] && note "doctor code 3 (macFUSE extension state) is not ok: known open item of T-M4-07, not part of the install"

echo "-- the privileged door accepts the installed CLI (root peer, recorded requirement)"
sudo -n "$CLI" journal-clear --label x --serial 00000000 disk99s1 >"$work/priv.out" 2>&1
echo "sudo ntfsmac journal-clear (a volume that does not exist): $(tr '\n' ' ' <"$work/priv.out")"
check "the daemon itself answered: it does not know the volume (past checks 1 to 3 of ADR-019)" grep -q "The daemon does not know this volume" "$work/priv.out"
nocheck "...and it was not a peer refusal (reasons.c texts of checks 1 to 4)" sh -c "grep -q -e 'Only the user at the console' -e 'needs an administrator account' -e 'process that the NTFS daemon started' -e 'not the signed, installed copy' -e 'no administrator authorization' $work/priv.out"
"$CLI" journal-clear --label x --serial 00000000 disk99s1 >"$work/priv2.out" 2>&1
echo "ntfsmac journal-clear without sudo: $(tr '\n' ' ' <"$work/priv2.out")"
check "without sudo the CLI says to run the command with sudo" grep -q -i sudo "$work/priv2.out"

echo "-- installing again over the install"
sh packaging/dev-install.sh --no-build >"$work/install2.log" 2>&1
rc=$?
tail -n 6 "$work/install2.log"
if [ "$rc" -eq 0 ]; then pass "a second dev-install.sh exits 0"; else failf "a second dev-install.sh exits $rc"; fi
if out=$(installed_ok); then pass "the same state after the second install"; else failf "the second install changed the set"; echo "$out"; fi
check "the job is loaded again and the socket is back" wait_for 30 test -S "$SOCK"
check "the right is still ours" right_is_ours

echo "-- --job-arg: a daemon argument in the installed plist (what a VM driver or a person adds)"
sh packaging/dev-install.sh --no-build --job-arg --automount-mode=ro >"$work/install-arg.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then pass "dev-install.sh --job-arg exits 0"; else failf "dev-install.sh --job-arg exits $rc"; tail -n 8 "$work/install-arg.log"; fi
check "the installed plist has the argument after the program path" sh -c "grep -q '<string>$LIBEXEC/ntfsmacd</string><string>--automount-mode=ro</string>' $PLIST"
check "the plist with the argument is valid" plutil -lint "$PLIST"
check "launchd runs the daemon with the argument" sh -c "sudo -n launchctl print system/$LABEL | grep -q -e '--automount-mode=ro'"
check "the daemon is up with it" wait_for 30 test -S "$SOCK"
if out=$(installed_ok); then pass "the set is unchanged by --job-arg"; else failf "--job-arg changed the set"; echo "$out"; fi
if sh packaging/dev-install.sh --no-build --job-arg 'a b' >"$work/arg-bad.out" 2>&1; then failf "a --job-arg with a space was accepted"; else pass "a --job-arg with a space is refused (exit $?)"; fi
sh packaging/dev-install.sh --no-build >"$work/install-plain.log" 2>&1 || failf "dev-install.sh without --job-arg exits non-zero"
check "installing again without --job-arg restores the shipped plist" cmp -s integration/launchd/$LABEL.plist "$PLIST"
check "the daemon is up again" wait_for 30 test -S "$SOCK"

echo "-- dev-install.sh refuses a build with test seams (three independent checks)"
# a build tree of the right shape whose daemon is replaced; the cache says product in b and c
mkseam() { # NAME CACHE-SEAMS: a tree under $work/NAME with the real CLI, probe and volmark
	t=$work/$1
	mkdir -p "$t/src/daemon" "$t/src/cli" "$t/src/probe" "$t/src/volmark"
	printf 'NTFSMAC_TEST_SEAMS:BOOL=%s\nNTFSMAC_AUTHZ_FALLBACK_ROOT:BOOL=ON\n' "$2" >"$t/CMakeCache.txt"
	ln -s "$d/build/product/src/cli/ntfsmac" "$t/src/cli/ntfsmac"
	ln -s "$d/build/product/src/probe/ntfsmac-probe" "$t/src/probe/ntfsmac-probe"
	ln -s "$d/build/product/src/volmark/ntfsmac-volmark" "$t/src/volmark/ntfsmac-volmark"
}
refuse_build() { # NAME TEXT-THE-REFUSAL-MUST-CONTAIN
	sh packaging/dev-install.sh --no-build --build-dir "$work/$1" >"$work/$1.out" 2>&1
	rc=$?
	if [ "$rc" -ne 0 ] && grep -q "$2" "$work/$1.out"; then pass "$1: refused (exit $rc), reason: $2"; else failf "$1: exit $rc: $(tail -n 1 "$work/$1.out")"; fi
	tail -n 1 "$work/$1.out" | cut -c1-200
}
mkseam seam-cache ON
cp "$d/build/product/src/daemon/ntfsmacd" "$work/seam-cache/src/daemon/ntfsmacd"
refuse_build seam-cache "not a product build"
# the cache lies (says product), the binary has the seam options in its text
mkseam seam-strings OFF
printf '#!/bin/sh\n# usage: ntfsmacd [--console-uid UID] [--child-log FILE]\nexit 2\n' >"$work/seam-strings/src/daemon/ntfsmacd"
chmod 755 "$work/seam-strings/src/daemon/ntfsmacd"
refuse_build seam-strings "contains test seams"
# the cache lies and the text has no seam option, but the program's own usage line lists them
mkseam seam-usage OFF
printf '#!/bin/sh\nprintf "usage: ntfsmacd [--automount-mode=ro|auto] [%%s%%s UID]\\n" --console- uid >&2\nexit 2\n' >"$work/seam-usage/src/daemon/ntfsmacd"
chmod 755 "$work/seam-usage/src/daemon/ntfsmacd"
refuse_build seam-usage "usage line"
# a daemon that prints no usage at all cannot be judged: refused, not accepted
mkseam seam-mute OFF
printf '#!/bin/sh\nexit 2\n' >"$work/seam-mute/src/daemon/ntfsmacd"
chmod 755 "$work/seam-mute/src/daemon/ntfsmacd"
refuse_build seam-mute "printed no usage line"
check "the real product daemon prints its usage without seam options" sh -c "build/product/src/daemon/ntfsmacd --bogus 2>&1 | grep -q '^usage: ntfsmacd' && ! build/product/src/daemon/ntfsmacd --bogus 2>&1 | grep -q -e console-uid -e journal-dir"
check "...and none of the refusals changed anything (the installed set is as before)" installed_ok

# =====================================================================================
echo "== I-PKG-02: uninstall.sh"
# =====================================================================================
# things that must survive: a foreign file in a directory the project owns, a file in a shared directory,
# the journal (state), and a file that a bad manifest names
echo foreign | sudo -n tee "$LIBEXEC/foreign.txt" >/dev/null
echo decoy | sudo -n tee /usr/local/bin/ntfsmac-other >/dev/null
sudo -n mkdir -p /var/db/ntfs-macos
echo entry | sudo -n tee /var/db/ntfs-macos/keep-me >/dev/null
echo precious >"$work/outside"
sum_foreign=$(shasum -a 256 "$LIBEXEC/foreign.txt" | awk '{print $1}')

echo "-- manifests that name paths outside the project are refused, and nothing is removed"
refuse() { # NAME LINE
	{ cat "$MANIFEST"; printf '%s\n' "$2"; } >"$work/bad.txt"
	sudo -n sh "$LIBEXEC/uninstall.sh" --manifest "$work/bad.txt" >"$work/bad.out" 2>&1
	rc=$?
	if [ "$rc" -ne 0 ] && grep -q -i refus "$work/bad.out"; then pass "$1: refused (exit $rc)"; else failf "$1: exit $rc: $(cat "$work/bad.out")"; fi
	if installed_ok >/dev/null 2>&1 && job_loaded && right_is_ours && [ -f "$work/outside" ]; then pass "$1: nothing was removed"; else failf "$1: the install changed"; fi
}
refuse "a path outside the project" "file 0644 root wheel $work/outside"
refuse "a shared directory entry" "dir 0755 root wheel /usr/local/bin"
refuse "a dot-dot path" "file 0644 root wheel /usr/local/libexec/ntfs-macos/../../../..${work}/outside"
refuse "a relative path" "file 0644 root wheel usr/local/bin/ntfsmac"
refuse "a system plist" "file 0644 root wheel /Library/LaunchDaemons/com.apple.xpc.launchd.plist"
refuse "a wrong right" "right - - - com.apple.system.privilege.admin"
check "the decoy in /usr/local/bin is untouched" [ -f /usr/local/bin/ntfsmac-other ]

echo "-- a symbolic link in place of a project directory is refused"
sudo -n mv "$SUPPORT/sandbox" "$work/sandbox.moved"
sudo -n ln -s "$work/sandbox.moved" "$SUPPORT/sandbox"
sudo -n sh "$LIBEXEC/uninstall.sh" >"$work/link.out" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q -i 'symbolic link' "$work/link.out"; then pass "a symlinked project directory is refused (exit $rc)"; else failf "symlink case: exit $rc: $(cat "$work/link.out")"; fi
if [ -f "$CLI" ] && [ -f "$PLIST" ] && job_loaded; then pass "...before anything was removed (CLI, plist and job are still there)"; else failf "the symlink case removed something"; fi
sudo -n rm "$SUPPORT/sandbox"
sudo -n mv "$work/sandbox.moved" "$SUPPORT/sandbox"
check "the install is whole again" installed_ok

echo "-- an ancestor that is not root-only: the uninstaller warns"
sudo -n chown "$(id -un)" /usr/local/share/doc
sudo -n sh "$LIBEXEC/uninstall.sh" --dry-run >"$work/warn.out" 2>&1
sudo -n chown root:wheel /usr/local/share/doc
check "the dry run warns that /usr/local/share/doc is not root-only" grep -q "WARNING: /usr/local/share/doc is owned by $(id -un)" "$work/warn.out"
echo "-- --dry-run"
sudo -n sh "$LIBEXEC/uninstall.sh" --dry-run >"$work/dry.out" 2>&1
rc=$?
sed -n '1,12p' "$work/dry.out"
echo "..."
if [ "$rc" -eq 0 ]; then pass "--dry-run exits 0"; else failf "--dry-run exits $rc"; fi
check "--dry-run lists the files it would remove" grep -q "would remove $CLI" "$work/dry.out"
if installed_ok >/dev/null 2>&1 && job_loaded && right_is_ours; then pass "--dry-run removed nothing: files, job and right are all still there"; else failf "--dry-run changed something"; fi

echo "-- the uninstall (the installed copy, as a user would run it)"
sudo -n sh -c "$LIBEXEC/uninstall.sh" >"$work/un1.out" 2>&1
rc=$?
cat "$work/un1.out"
if [ "$rc" -eq 0 ]; then pass "uninstall.sh exits 0"; else failf "uninstall.sh exits $rc"; fi
check "the job is unloaded" sh -c "! sudo -n launchctl print system/$LABEL >/dev/null 2>&1"
check "the authorization right is removed" sh -c "! [ \"\$(sudo -n security authorizationdb read $RIGHT 2>/dev/null | plutil -extract group raw -o - - 2>/dev/null)\" = admin ]"
check "the control socket is gone" wait_for 20 sh -c "! [ -S $SOCK ]"
for p in "$CLI" "$PLIST" "$LIBEXEC/ntfsmacd" "$LIBEXEC/ntfsmac-worker" "$LIBEXEC/ntfsmac-probe" "$LIBEXEC/ntfsmac-volmark" "$LIBEXEC/mkntfs" "$LIBEXEC/uninstall.sh" "$LIBEXEC/manifest.txt" "$SUPPORT/cli-requirement" "$SUPPORT/sandbox/worker.sb" /usr/local/share/doc/ntfs-macos/COPYING; do
	[ ! -e "$p" ] && [ ! -L "$p" ] || failf "still there: $p"
done
nothing_left_out=$(manifest_paths file | while IFS= read -r p; do [ -e "$p" ] && echo "  left: $p"; done)
if [ -z "$nothing_left_out" ]; then pass "every file of the manifest is gone"; else failf "files left"; echo "$nothing_left_out"; fi
check "the empty project directories are gone (sandbox, support, doc)" sh -c "[ ! -e '$SUPPORT' ] && [ ! -e /usr/local/share/doc/ntfs-macos ]"
check "the directory with a foreign file is kept, with the file unchanged" sh -c "[ -d $LIBEXEC ] && [ \"\$(shasum -a 256 $LIBEXEC/foreign.txt | awk '{print \$1}')\" = $sum_foreign ]"
check "nothing else is in the kept directory" sh -c "[ \"\$(ls $LIBEXEC)\" = foreign.txt ]"
check "the decoy in /usr/local/bin is untouched" [ -f /usr/local/bin/ntfsmac-other ]
check "the journal (state) is kept without --purge-state" sudo -n test -f /var/db/ntfs-macos/keep-me
check "uninstall said that the state is kept" grep -q 'kept state /var/db/ntfs-macos' "$work/un1.out"
check "the file outside the project is untouched" [ -f "$work/outside" ]
check "/usr/local/bin and /Library/LaunchDaemons (shared) are still there" sh -c "[ -d /usr/local/bin ] && [ -d /Library/LaunchDaemons ]"

echo "-- running it again, from the source tree (nothing is installed)"
sudo -n sh packaging/uninstall.sh >"$work/un2.out" 2>&1
rc=$?
cat "$work/un2.out"
if [ "$rc" -eq 0 ]; then pass "a second uninstall exits 0"; else failf "a second uninstall exits $rc"; fi
check "the foreign file is still there" [ -f "$LIBEXEC/foreign.txt" ]

echo "-- install once more, then uninstall with --purge-state after the foreign file is gone"
sudo -n rm "$LIBEXEC/foreign.txt"
sudo -n rmdir "$LIBEXEC"
sh packaging/dev-install.sh --no-build >"$work/install3.log" 2>&1
rc=$?
if [ "$rc" -eq 0 ]; then pass "a reinstall after an uninstall exits 0"; else failf "a reinstall exits $rc"; tail -n 10 "$work/install3.log"; fi
if out=$(installed_ok); then pass "the reinstall is whole"; else failf "the reinstall differs"; echo "$out"; fi
check "the daemon is up again" wait_for 30 test -S "$SOCK"
check "the journal entry of the earlier install survived (state outlives an uninstall)" sudo -n test -f /var/db/ntfs-macos/keep-me
sudo -n sh packaging/uninstall.sh --purge-state >"$work/un3.out" 2>&1
rc=$?
cat "$work/un3.out"
if [ "$rc" -eq 0 ]; then pass "uninstall --purge-state exits 0"; else failf "uninstall --purge-state exits $rc"; fi
if out=$(nothing_left); then pass "nothing of the project is left (files, directories, job, right, socket)"; else failf "something is left"; echo "$out"; fi
check "the state is purged (journal directory and daemon log)" sh -c "sudo -n test ! -e /var/db/ntfs-macos && sudo -n test ! -e /var/log/ntfsmacd.log"
sudo -n rm -f /usr/local/bin/ntfsmac-other
snapshot >"$work/snap.after"
if cmp -s "$work/snap.before" "$work/snap.after"; then pass "the files on the guest equal those before the install (snapshot compare)"; else failf "the files on the guest differ from before the install"; diff "$work/snap.before" "$work/snap.after" | sed 's/^/  /'; fi

# =====================================================================================
echo "== I-PKG-02: a package receipt of the developer package (build-pkg.sh kind)"
# =====================================================================================
mkdir -p "$work/pkgroot$LIBEXEC" "$work/pkgroot/usr/local/share/doc/ntfs-macos/LICENSES"
printf '#!/bin/sh\necho tool\n' >"$work/pkgroot$LIBEXEC/ntfsmac-probe"
chmod 755 "$work/pkgroot$LIBEXEC/ntfsmac-probe"
cp packaging/uninstall.sh packaging/manifest.txt "$work/pkgroot$LIBEXEC/"
echo licence >"$work/pkgroot/usr/local/share/doc/ntfs-macos/LICENSES/GPL"
echo text >"$work/pkgroot/usr/local/share/doc/ntfs-macos/COPYING"
pkgbuild --root "$work/pkgroot" --identifier org.ntfs-macos.pkg --version 0.0.1 --install-location / "$work/ok.pkg" >/dev/null 2>&1 || failf "pkgbuild of the test package"
sudo -n /usr/sbin/installer -pkg "$work/ok.pkg" -target / >"$work/pkg.out" 2>&1 || { failf "installer of the test package"; cat "$work/pkg.out"; }
check "the test package is installed with a receipt" pkgutil --pkg-info org.ntfs-macos.pkg
sudo -n sh "$LIBEXEC/uninstall.sh" >"$work/un4.out" 2>&1
rc=$?
cat "$work/un4.out"
if [ "$rc" -eq 0 ]; then pass "uninstall.sh removes a package install (exit 0)"; else failf "uninstall.sh on a package install exits $rc"; fi
check "the receipt is forgotten" sh -c "! pkgutil --pkg-info org.ntfs-macos.pkg >/dev/null 2>&1"
check "its files and directories (LICENSES included) are gone" sh -c "[ ! -e $LIBEXEC ] && [ ! -e /usr/local/share/doc/ntfs-macos ]"

# a receipt that lists a path outside the directories of the manifest: refused, nothing removed
mkdir -p "$work/evilroot/usr/local/share/ntfs-macos-outside" "$work/evilroot$LIBEXEC"
echo keep >"$work/evilroot/usr/local/share/ntfs-macos-outside/file"
echo tool >"$work/evilroot$LIBEXEC/tool"
pkgbuild --root "$work/evilroot" --identifier org.ntfs-macos.pkg --version 0.0.1 --install-location / "$work/evil.pkg" >/dev/null 2>&1 || failf "pkgbuild of the second test package"
sudo -n /usr/sbin/installer -pkg "$work/evil.pkg" -target / >"$work/pkg2.out" 2>&1 || { failf "installer of the second test package"; cat "$work/pkg2.out"; }
sudo -n sh packaging/uninstall.sh >"$work/un5.out" 2>&1
rc=$?
cat "$work/un5.out"
if [ "$rc" -ne 0 ] && grep -q -i refus "$work/un5.out"; then pass "a receipt path outside the project directories is refused (exit $rc)"; else failf "receipt outside case: exit $rc"; fi
check "...and nothing was removed" sh -c "[ -f /usr/local/share/ntfs-macos-outside/file ] && [ -f $LIBEXEC/tool ]"
sudo -n pkgutil --forget org.ntfs-macos.pkg >/dev/null 2>&1
sudo -n rm -rf /usr/local/share/ntfs-macos-outside "$LIBEXEC"
snapshot >"$work/snap.after2"
if cmp -s "$work/snap.before" "$work/snap.after2"; then pass "the guest is as before the install"; else failf "files differ from before"; diff "$work/snap.before" "$work/snap.after2" | sed 's/^/  /'; fi

sudo -n rm -rf "$work"
echo "result: $fail failure(s)"
[ "$fail" -eq 0 ]
