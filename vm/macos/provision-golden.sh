#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# provision-golden.sh - build the golden macOS VM "ntfsmac-golden" (T-M2-02, docs/VM_SETUP.md):
# the pinned base image plus macFUSE 5.5.0, verified and approved for FSKit.
#
# Usage: vm/macos/provision-golden.sh [--resume]
#   --resume  reuse an existing ntfsmac-golden instead of refusing.
#
# Everything runs inside the guest through tart.sh (tart exec). Nothing is downloaded,
# installed or mounted on the host. One step needs a person (H3): turning macFUSE on in the
# guest's System Settings window. The script opens that pane and waits up to 15 minutes.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
repo=$(cd "$here/../.." && pwd)
tart_sh=$here/tart.sh
lock=$repo/third_party/deps.lock
SIGNER='Developer ID Installer: Benjamin Fleischer (3T5GSNBU6W)'
VM=golden

die() { echo "provision-golden: error: $*" >&2; exit 1; }
log() { echo "provision-golden: $*" >&2; }
g() { "$tart_sh" exec "$VM" -- "$@"; }

resume=0
case ${1:-} in
--resume) resume=1 ;;
'') ;;
*) die "unknown option: $1" ;;
esac

read -r _ mf_ver mf_url mf_sha < <(awk '$1 == "macfuse" { print; exit }' "$lock") ||
	die "no macfuse entry in $lock"
case $mf_url$mf_sha in *[!A-Za-z0-9:/._-]*) die "unexpected characters in the macfuse lock entry" ;; esac

if tart list --quiet | grep -qx "ntfsmac-$VM"; then
	[ "$resume" = 1 ] || die "ntfsmac-$VM exists; delete it or pass --resume"
else
	"$tart_sh" clone "$VM"
fi
"$tart_sh" run "$VM" --graphics >/dev/null

log "downloading and verifying macFUSE $mf_ver in the guest"
g "set -e; rm -rf /tmp/mf; mkdir -p /tmp/mf; cd /tmp/mf
curl -fsSL -o macfuse.dmg '$mf_url'
echo '$mf_sha  macfuse.dmg' | shasum -a 256 -c -
hdiutil attach -nobrowse -readonly -mountpoint /tmp/mf/vol macfuse.dmg >/dev/null
trap 'hdiutil detach /tmp/mf/vol >/dev/null 2>&1' EXIT
pkg='/tmp/mf/vol/Install macFUSE.pkg'
sig=\$(pkgutil --check-signature \"\$pkg\")
echo \"\$sig\" | sed -n 2,6p
echo \"\$sig\" | grep -qF 'Notarization: trusted by the Apple notary service'
echo \"\$sig\" | grep -qF '1. $SIGNER'
sudo -n /usr/sbin/installer -pkg \"\$pkg\" -target /" || die "macFUSE download, verification or install failed"

log "building the FSKit hello file system in the guest"
tart exec -i "ntfsmac-$VM" /bin/sh -c 'mkdir -p ~/ntfsmac && cat > ~/ntfsmac/fskit-hello.c' <"$here/fskit-hello.c"
g 'cd ~/ntfsmac && cc -Wall -Wextra -Werror -DFUSE_DARWIN_ENABLE_EXTENSIONS=0 -I/usr/local/include/fuse3 -o fskit-hello fskit-hello.c -L/usr/local/lib -lfuse3'

hello_ok() {
	# shellcheck disable=SC2016 # expanded in the guest shell
	g 'mkdir -p /tmp/hm; cd ~/ntfsmac; (./fskit-hello -f -o backend=fskit /tmp/hm >/tmp/hello.log 2>&1 &); sleep 6
r=1; [ "$(cat /tmp/hm/hello 2>/dev/null)" = "hello from fskit" ] && r=0
/sbin/umount /tmp/hm >/dev/null 2>&1; pkill -f "fskit-hello -f" >/dev/null 2>&1; exit $r'
}

if hello_ok; then
	log "macFUSE is already enabled"
else
	g 'open "x-apple.systempreferences:com.apple.LoginItems-Settings.extension"' || true
	cat >&2 <<'EOF'

  ACTION NEEDED (H3) in the ntfsmac-golden VM window:
  System Settings > General > Login Items & Extensions > File System Extensions:
  turn macFUSE on (both entries if two are listed). Password, if asked: admin
  (the image's public test login). Waiting up to 15 minutes...

EOF
	t0=$(date +%s)
	until hello_ok; do
		[ $(( $(date +%s) - t0 )) -lt 900 ] || die "macFUSE not enabled after 15 minutes"
		sleep 10
	done
	log "macFUSE enabled; hello mount works"
fi

g 'rm -rf /tmp/mf /tmp/hm; sync'
"$tart_sh" stop "$VM"
log "golden VM ready: clone test VMs with: vm/macos/tart.sh clone NAME --from $VM"
