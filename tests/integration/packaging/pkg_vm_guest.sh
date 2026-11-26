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
#           user-owned or group-writable, or is a symbolic link (also to a root-only directory,
#           with a root-owned or a user-owned /usr/local; audit M6-F01), and when the daemon of
#           the build tree has test seams.
# I-PKG-02: packaging/uninstall.sh refuses manifests that name paths outside the project and
#           removes nothing then; --dry-run removes nothing; a real run removes exactly the manifest
#           (the job, the right, the link, every file, every empty directory) and leaves foreign files, their
#           directory, the shared directories and the state alone; --purge-state removes the state;
#           the files left on the Mac equal those before the install; a package receipt of the older
#           kind is removed; a receipt path /usr/local/bin/ntfsmac (the only entry of /usr/local/bin)
#           follows the rule of the link: a regular file (earlier layout) is kept and named, a link to
#           exactly the CLI is removed, the same link in a user-owned /usr/local/bin is kept and named,
#           and the shared /usr/local/bin is never removed; a receipt with a path outside the project
#           is refused. With a user-owned /usr/local/libexec it removes nothing (also not in a dry
#           run) and names the directory; with --allow-unsafe-ancestors it removes the rest, keeps
#           and names every path below that directory, and exits 1 (audit M6-F05, issue #6).
#
# Prints PASS, FAIL and NOTE lines; exit 0 only if no FAIL.
set -u
PATH=/opt/homebrew/bin:/usr/local/bin:/sbin:/usr/sbin:$PATH
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
		find "$LIBEXEC" "$SUPPORT" \( -type f -o -type d -o -type l \) -print 2>/dev/null
		ls -d "$PLIST" 2>/dev/null
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
	{ [ -e "$LINK" ] || [ -L "$LINK" ]; } && { echo "  the link $LINK is still there"; bad=1; }
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
ANC=/usr/local/libexec
made=
[ -d "$ANC" ] || made="$made $ANC"
sudo -n mkdir -p "$ANC"
orig=$(stat -f '%Su:%Sg %Lp' "$ANC")
restore_anc() {
	if [ -n "$made" ]; then
		for m in $ANC; do case "$made" in *" $m"*) sudo -n rmdir "$m" 2>/dev/null ;; esac; done
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
echo "-- /usr/local/libexec a symbolic link to a root-only directory: the install is refused (audit M6-F01)"
TARGET=/private/var/db/ntfsmac-f01-target
had_anc=0
if [ -e "$ANC" ] || [ -L "$ANC" ]; then had_anc=1; sudo -n mv "$ANC" "$ANC.pkgvm-aside"; fi
sudo -n mkdir -m 755 "$TARGET"
sudo -n ln -s "$TARGET" "$ANC"
lorig=$(stat -f '%Su:%Sg %Lp' /usr/local)
sh packaging/dev-install.sh >"$work/anc3.out" 2>&1
rc=$?
tail -n 1 "$work/anc3.out" | cut -c1-260
if [ "$rc" -ne 0 ] && grep -q 'the directory "/usr/local/libexec" above "/usr/local/libexec/ntfs-macos" is a symbolic link' "$work/anc3.out"; then pass "a link at $ANC (root-owned /usr/local) is refused (exit $rc)"; else failf "link at $ANC: exit $rc"; fi
sudo -n chown "$(id -un)" /usr/local
sh packaging/dev-install.sh >"$work/anc4.out" 2>&1
rc=$?
tail -n 1 "$work/anc4.out" | cut -c1-260
if [ "$rc" -ne 0 ] && grep -q "the directory \"/usr/local\" above \"/usr/local/libexec/ntfs-macos\" is owned by $(id -un)" "$work/anc4.out"; then pass "a user-owned /usr/local holding that link is refused (exit $rc)"; else failf "user-owned /usr/local with the link: exit $rc"; fi
check "...nothing was built or installed, and nothing appeared in the link's target" sh -c "[ ! -e build/product ] && [ ! -e $PLIST ] && [ -z \"\$(ls -A $TARGET)\" ]"
check "...and no refusal suggests a command (audit M6-F06)" sh -c "! grep -q 'sudo chown' '$work/anc3.out' '$work/anc4.out' '$work/anc1.out' '$work/anc2.out'"
sudo -n chown "${lorig%%:*}" /usr/local
sudo -n chmod "${lorig#* }" /usr/local
sudo -n rm -f "$ANC"
sudo -n rmdir "$TARGET"
[ "$had_anc" -eq 0 ] || sudo -n mv "$ANC.pkgvm-aside" "$ANC"
check "/usr/local is back ($lorig)" [ "$(stat -f '%Su:%Sg %Lp' /usr/local)" = "$lorig" ]

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
check "BUILD-INFO says preset product" grep -q '^preset .*product' "$DOC/BUILD-INFO"
check "the link $LINK points to the CLI (/usr/local/bin is root-only in the guest)" [ "$(readlink "$LINK")" = "$CLI" ]

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
# T-M4-07 (criterion 4.6): every finding is "ok:", the macFUSE extension (approved in the golden
# image) included. The extension state is read from `pluginkit` as the console user; it is then
# disabled for that user, `doctor` must say "not approved", and it is enabled again.
problems=$(grep -c '^problem:' "$work/doctor.out" || true)
if [ "$problems" = 0 ] && grep -q '^ok: The macFUSE file system extension is enabled' "$work/doctor.out"; then pass "doctor is green: macFUSE installed, version ok, extension enabled, no conflicting driver, journal readable"; else failf "doctor reports $problems problem(s) or not the extension as enabled"; fi
EXT=io.macfuse.app.fsmodule.macfuse-local
echo "-- doctor with the macFUSE extension disabled for $(id -un) (pluginkit -e ignore)"
pluginkit -m -v -i "$EXT"
pluginkit -e ignore -i "$EXT"
sleep 2
pluginkit -m -v -i "$EXT"
"$CLI" doctor >"$work/doctor_off.out" 2>&1
cat "$work/doctor_off.out"
check "doctor says the extension is not approved" grep -q '^problem: The macFUSE file system extension is not approved' "$work/doctor_off.out"
echo "-- doctor with the extension enabled again (pluginkit -e use)"
pluginkit -e use -i "$EXT"
sleep 2
pluginkit -m -v -i "$EXT"
"$CLI" doctor >"$work/doctor_on.out" 2>&1
cat "$work/doctor_on.out"
check "doctor is green again" sh -c "! grep -q '^problem:' $work/doctor_on.out && grep -q '^ok: The macFUSE file system extension is enabled' $work/doctor_on.out"

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

echo "-- an ancestor that is not root-only: the uninstaller refuses and removes nothing (audit M6-F05, issue #6)"
sudo -n chown "$(id -un)" /usr/local/libexec
sudo -n sh "$LIBEXEC/uninstall.sh" --dry-run >"$work/warn.out" 2>&1
rc=$?
cat "$work/warn.out"
if [ "$rc" -ne 0 ] && grep -q "the directory \"/usr/local/libexec\" above .* is owned by $(id -un)" "$work/warn.out" && grep -q 'nothing was removed' "$work/warn.out"; then pass "the dry run refuses (exit $rc) and names /usr/local/libexec"; else failf "the dry run with a user-owned /usr/local/libexec: exit $rc"; fi
sudo -n sh "$LIBEXEC/uninstall.sh" >"$work/unsafe1.out" 2>&1
rc=$?
if [ "$rc" -ne 0 ] && grep -q 'nothing was removed' "$work/unsafe1.out"; then pass "the real run refuses (exit $rc)"; else failf "the real run with a user-owned /usr/local/libexec: exit $rc"; cat "$work/unsafe1.out"; fi
if installed_ok >/dev/null 2>&1 && job_loaded && right_is_ours; then pass "...and removed nothing: files, job and right are all still there"; else failf "the refused run changed something"; fi
check "...and suggests no command (audit M6-F06)" sh -c "! grep -q 'sudo chown' '$work/unsafe1.out'"
sudo -n sh "$LIBEXEC/uninstall.sh" --allow-unsafe-ancestors >"$work/unsafe2.out" 2>&1
rc=$?
cat "$work/unsafe2.out"
if [ "$rc" -eq 1 ]; then pass "--allow-unsafe-ancestors exits 1 (paths kept)"; else failf "--allow-unsafe-ancestors exits $rc"; fi
check "...it kept and named the files below /usr/local/libexec" sh -c "[ -f $CLI ] && [ -f $LIBEXEC/ntfsmacd ] && [ -f $LIBEXEC/foreign.txt ] && grep -q 'kept \"$CLI\": the directory \"/usr/local/libexec\" is owned by $(id -un)' '$work/unsafe2.out'"
check "...and removed the rest (job, plist, support directory)" sh -c "! sudo -n launchctl print system/$LABEL >/dev/null 2>&1 && [ ! -e $PLIST ] && [ ! -e '$SUPPORT' ]"
sudo -n chown root:wheel /usr/local/libexec
sh packaging/dev-install.sh --no-build >"$work/install-again.log" 2>&1 || { failf "the reinstall after the unsafe-ancestor case"; tail -n 10 "$work/install-again.log"; }
if out=$(installed_ok); then pass "the install is whole again"; else failf "the reinstall differs"; echo "$out"; fi
check "the daemon is up again" wait_for 30 test -S "$SOCK"
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
for p in "$CLI" "$PLIST" "$LIBEXEC/ntfsmacd" "$LIBEXEC/ntfsmac-worker" "$LIBEXEC/ntfsmac-probe" "$LIBEXEC/ntfsmac-volmark" "$LIBEXEC/mkntfs" "$LIBEXEC/uninstall.sh" "$LIBEXEC/manifest.txt" "$SUPPORT/cli-requirement" "$SUPPORT/sandbox/worker.sb" "$DOC/COPYING" "$LINK"; do
	[ ! -e "$p" ] && [ ! -L "$p" ] || failf "still there: $p"
done
nothing_left_out=$(manifest_paths file | while IFS= read -r p; do [ -e "$p" ] && echo "  left: $p"; done)
if [ -z "$nothing_left_out" ]; then pass "every file of the manifest is gone"; else failf "files left"; echo "$nothing_left_out"; fi
check "the empty project directories are gone (sandbox, doc, support)" sh -c "[ ! -e '$SUPPORT' ]"
check "the link $LINK is gone" sh -c "[ ! -e $LINK ] && [ ! -L $LINK ]"
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
mkdir -p "$work/pkgroot$LIBEXEC" "$work/pkgroot$DOC/LICENSES"
printf '#!/bin/sh\necho tool\n' >"$work/pkgroot$LIBEXEC/ntfsmac-probe"
chmod 755 "$work/pkgroot$LIBEXEC/ntfsmac-probe"
cp packaging/uninstall.sh packaging/manifest.txt "$work/pkgroot$LIBEXEC/"
echo licence >"$work/pkgroot$DOC/LICENSES/GPL"
echo text >"$work/pkgroot$DOC/COPYING"
pkgbuild --root "$work/pkgroot" --identifier org.ntfs-macos.pkg --version 0.0.1 --install-location / "$work/ok.pkg" >/dev/null 2>&1 || failf "pkgbuild of the test package"
sudo -n /usr/sbin/installer -pkg "$work/ok.pkg" -target / >"$work/pkg.out" 2>&1 || { failf "installer of the test package"; cat "$work/pkg.out"; }
check "the test package is installed with a receipt" pkgutil --pkg-info org.ntfs-macos.pkg
sudo -n sh "$LIBEXEC/uninstall.sh" >"$work/un4.out" 2>&1
rc=$?
cat "$work/un4.out"
if [ "$rc" -eq 0 ]; then pass "uninstall.sh removes a package install (exit 0)"; else failf "uninstall.sh on a package install exits $rc"; fi
check "the receipt is forgotten" sh -c "! pkgutil --pkg-info org.ntfs-macos.pkg >/dev/null 2>&1"
check "its files and directories (LICENSES included) are gone" sh -c "[ ! -e $LIBEXEC ] && [ ! -e '$SUPPORT' ]"

echo "-- receipts that name /usr/local/bin/ntfsmac follow the rule of the link"
# /usr/local/bin is moved aside, so that the package makes it with ntfsmac as its only entry
had_bin=0
if [ -d /usr/local/bin ]; then had_bin=1; sudo -n mv /usr/local/bin /usr/local/bin.pkgvm-aside; fi
# rpkg NAME: build and install a package whose receipt holds $LIBEXEC/ntfsmac-probe and usr/local/bin/ntfsmac
# as made in $work/NAME by the caller
rpkg() {
	mkdir -p "$work/$1$LIBEXEC"
	printf '#!/bin/sh\necho tool\n' >"$work/$1$LIBEXEC/ntfsmac-probe"
	chmod 755 "$work/$1$LIBEXEC/ntfsmac-probe"
	pkgbuild --root "$work/$1" --identifier org.ntfs-macos.pkg --version 0.0.1 --install-location / "$work/$1.pkg" >/dev/null 2>&1 || failf "pkgbuild of $1"
	sudo -n /usr/sbin/installer -pkg "$work/$1.pkg" -target / >/dev/null 2>&1 || failf "installer of $1"
}
# a) the earlier layout: the CLI as a regular file
mkdir -p "$work/r_file/usr/local/bin"
printf '#!/bin/sh\necho old cli\n' >"$work/r_file/usr/local/bin/ntfsmac"
chmod 755 "$work/r_file/usr/local/bin/ntfsmac"
rpkg r_file
check "a) /usr/local/bin holds only ntfsmac, a regular file of the receipt" sh -c "[ \"\$(ls -A /usr/local/bin)\" = ntfsmac ] && [ -f /usr/local/bin/ntfsmac ] && [ ! -L /usr/local/bin/ntfsmac ]"
sudo -n sh packaging/uninstall.sh >"$work/un6.out" 2>&1
rc=$?
cat "$work/un6.out"
if [ "$rc" -eq 0 ]; then pass "a) uninstall.sh exits 0"; else failf "a) uninstall.sh exits $rc"; fi
check "a) the regular file /usr/local/bin/ntfsmac is kept (not our link) and named" sh -c "[ -f /usr/local/bin/ntfsmac ] && grep -q 'kept /usr/local/bin/ntfsmac: it is not a symbolic link' '$work/un6.out'"
check "a) the rest of the receipt is gone and the receipt is forgotten" sh -c "[ ! -e $LIBEXEC ] && ! pkgutil --pkg-info org.ntfs-macos.pkg >/dev/null 2>&1"
check "a) the shared /usr/local/bin is kept" [ -d /usr/local/bin ]
sudo -n rm -f /usr/local/bin/ntfsmac
# b) a symbolic link to exactly the CLI, in a root-only /usr/local/bin: removed; the directory, empty now, is kept
mkdir -p "$work/r_link/usr/local/bin"
ln -s "$LIBEXEC/ntfsmac" "$work/r_link/usr/local/bin/ntfsmac"
rpkg r_link
check "b) the receipt installed the link" [ "$(readlink /usr/local/bin/ntfsmac)" = "$LIBEXEC/ntfsmac" ]
sudo -n sh packaging/uninstall.sh >"$work/un7.out" 2>&1
rc=$?
cat "$work/un7.out"
if [ "$rc" -eq 0 ]; then pass "b) uninstall.sh exits 0"; else failf "b) uninstall.sh exits $rc"; fi
check "b) the link is removed" sh -c "[ ! -e /usr/local/bin/ntfsmac ] && [ ! -L /usr/local/bin/ntfsmac ]"
check "b) the shared /usr/local/bin is kept although it is empty now" sh -c "[ -d /usr/local/bin ] && [ -z \"\$(ls -A /usr/local/bin)\" ]"
# c) the same link, but /usr/local/bin belongs to the user: kept and named
rpkg r_link
sudo -n chown "$(id -un)" /usr/local/bin
sudo -n sh packaging/uninstall.sh >"$work/un8.out" 2>&1
rc=$?
cat "$work/un8.out"
if [ "$rc" -eq 0 ]; then pass "c) uninstall.sh exits 0"; else failf "c) uninstall.sh exits $rc"; fi
check "c) the link in a user-owned /usr/local/bin is kept and named" sh -c "[ -L /usr/local/bin/ntfsmac ] && grep -q 'kept /usr/local/bin/ntfsmac: /usr/local/bin is not root-only' '$work/un8.out'"
check "c) the receipt is forgotten" sh -c "! pkgutil --pkg-info org.ntfs-macos.pkg >/dev/null 2>&1"
sudo -n rm -f /usr/local/bin/ntfsmac
sudo -n rmdir /usr/local/bin 2>/dev/null
[ "$had_bin" -eq 0 ] || sudo -n mv /usr/local/bin.pkgvm-aside /usr/local/bin

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
