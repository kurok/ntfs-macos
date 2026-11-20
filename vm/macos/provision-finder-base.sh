#!/usr/bin/env bash
# SPDX-License-Identifier: GPL-2.0-or-later
#
# provision-finder-base.sh - build "ntfsmac-golden-finder": a clone of ntfsmac-golden in which
# the guest agent may send Apple events to Finder (the Automation permission), so the Finder
# suite (M4 criterion 4.7) runs in clones of it with no click during the run. The screen-capture
# question that the suite's screenshots raise is answered here too.
#
# Usage: vm/macos/provision-finder-base.sh [--rebuild]
#   --rebuild  delete an existing ntfsmac-golden-finder first.
#
# Why a separate base: in a suite run the click on the Automation prompt was lost. The guest
# log showed the prompt raised, no answer for the whole wait, then "timeout was 1" and a deny
# entry, so later events were refused (-1743) with no new prompt. Here the prompt is asked
# again until it is answered: the deny entry is cleared with `tccutil reset AppleEvents` (the
# guest user's own entries only) before each try. The approval is stored in the base VM's
# disk, so every clone of it has it. ntfsmac-golden is only read (cloned), never changed.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
tart_sh=$here/tart.sh
SRC=golden
VM=golden-finder
TRIES=${NTFSMAC_FINDER_TRIES:-5}
WAIT=120

die() { echo "provision-finder-base: error: $*" >&2; exit 1; }
log() { echo "provision-finder-base: $*" >&2; }
g() { "$tart_sh" exec "$VM" -- "$@"; }

rebuild=0
case ${1:-} in
--rebuild) rebuild=1 ;;
'') ;;
*) die "unknown option: $1" ;;
esac

if tart list --quiet | grep -qx "ntfsmac-$VM"; then
	[ "$rebuild" = 1 ] || die "ntfsmac-$VM exists; pass --rebuild to make it again"
	"$tart_sh" delete "$VM"
fi
"$tart_sh" clone "$VM" --from "$SRC"
ok=0
cleanup() { [ "$ok" = 1 ] || { log "not approved: deleting ntfsmac-$VM"; "$tart_sh" delete "$VM" || true; }; }
trap cleanup EXIT
"$tart_sh" run "$VM" --graphics >/dev/null

# shellcheck disable=SC2016 # expanded in the guest, not here
g '[ "$(sysctl -n kern.hv_vmm_present)" = 1 ]' || die "the exec target is not a VM guest"

# ae SECONDS: one Apple event to Finder; prints osascript's answer or error, keeps its status.
ae() {
	g "perl -e 'alarm $(($1 + 10)); exec @ARGV or exit 127' osascript -e 'with timeout of $1 seconds' -e 'tell application \"Finder\" to get name of startup disk' -e 'end timeout' 2>&1"
}

i=1
while [ "$i" -le "$TRIES" ]; do
	g 'tccutil reset AppleEvents >/dev/null 2>&1' || true
	log ">>> ACTION NEEDED (try $i of $TRIES): in the VM window, click Allow on '\"tart-guest-agent\" wants access to control \"Finder\"' (waiting ${WAIT}s)"
	if out=$(ae "$WAIT"); then
		log "Finder answered: $out"
		break
	fi
	log "no answer on try $i: $out"
	i=$((i + 1))
done
[ "$i" -le "$TRIES" ] || die "Finder automation was not allowed after $TRIES tries"

# The entry must hold without a prompt: a second event answers at once.
out=$(ae 15) || die "a second Apple event failed ($out); the approval did not stick"
log "second Apple event answered at once: $out"

# The suite's evidence screenshots (screencapture) make macOS ask a second question, '"tart-guest-agent"
# is requesting to bypass the system private window picker and directly access your screen and audio',
# which covered the Finder window in a screenshot. Ask it here once, too.
g 'screencapture -x -t png /tmp/fb-shot.png' >/dev/null 2>&1 || true
log ">>> ACTION NEEDED: in the VM window, click Allow on '\"tart-guest-agent\" is requesting to bypass the system private window picker...' (waiting up to ${WAIT}s)"
# shellcheck disable=SC2016 # $HOME is the guest's, expanded there
approvals='"$HOME/Library/Group Containers/group.com.apple.replayd/ScreenCaptureApprovals.plist"'
t=0
until g "plutil -p $approvals 2>/dev/null" | grep -q tart-guest-agent; do
	[ "$t" -lt "$WAIT" ] || break
	sleep 5; t=$((t + 5))
done
if g "plutil -p $approvals 2>/dev/null" | grep -q tart-guest-agent; then
	log "screen capture approved for tart-guest-agent"
else
	log "WARNING: no screen-capture approval for tart-guest-agent found ($(g "plutil -p $approvals 2>&1" | head -3 | tr '\n' ' ')); the evidence screenshots may show the prompt"
fi
ok=1
"$tart_sh" stop "$VM"
log "ntfsmac-$VM is ready (stopped); clone it with: tart.sh clone NAME --from $VM"
