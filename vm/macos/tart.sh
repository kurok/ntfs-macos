#!/usr/bin/env bash
# tart.sh - Environment B wrapper: disposable macOS guests on tart.
#
# Usage: vm/macos/tart.sh pull
#        vm/macos/tart.sh clone NAME [--from SRC]   (SRC e.g. golden, the macFUSE-approved VM)
#        vm/macos/tart.sh run NAME [--graphics] [--disk FILE[:opts]]...   (opts: ro, sync=none, comma-separated)
#        vm/macos/tart.sh exec NAME -- CMD...   (alias: ssh; transport from NTFSMAC_TRANSPORT=exec|ssh)
#        vm/macos/tart.sh stop NAME
#        vm/macos/tart.sh delete NAME
#        vm/macos/tart.sh selftest
#
# NAME is the short name; the VM is called "${VM_PREFIX}NAME" (prefix not doubled).
# Only VMs with that prefix are ever cloned, stopped or deleted. No sudo, no host settings.
# State (logs, pid files, scratch disk images) lives in $NTFSMAC_STATE
# (default ~/.cache/ntfsmac-vm), never in the repo.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
# shellcheck source=versions.env
. "$here/versions.env"

STATE=${NTFSMAC_STATE:-$HOME/.cache/ntfsmac-vm}
IMAGE="$GUEST_IMAGE_REPO@$GUEST_IMAGE_DIGEST"
SSH_WAIT=${NTFSMAC_SSH_WAIT:-240}
# Guest transport: "exec" (tart guest agent over virtio, no host network; default) or "ssh".
TRANSPORT=${NTFSMAC_TRANSPORT:-exec}
case $TRANSPORT in exec|ssh) ;; *) echo "tart.sh: error: NTFSMAC_TRANSPORT must be exec or ssh" >&2; exit 1 ;; esac
mkdir -p "$STATE"

die() { echo "tart.sh: error: $*" >&2; exit 1; }
log() { echo "tart.sh: $*" >&2; }

command -v tart >/dev/null 2>&1 || die "tart not found (brew install openai/tools/tart)"
[ "$(uname -s)" = Darwin ] && [ "$(uname -m)" = arm64 ] || die "needs an Apple silicon Mac"

vmname() {
    local n=${1:-}
    [ -n "$n" ] || die "missing NAME"
    case $n in
        */*|*:*|*@*|*' '*) die "bad VM name: $n" ;;
    esac
    case $n in
        "$VM_PREFIX"*) printf '%s' "$n" ;;
        *) printf '%s%s' "$VM_PREFIX" "$n" ;;
    esac
}

vm_exists() { tart list --quiet 2>/dev/null | grep -qx -- "$1"; }
not() { ! "$@"; }
base_present() { tart list | grep -qF -- "$GUEST_IMAGE_REPO"; }
now() { perl -MTime::HiRes=time -e 'printf "%.2f\n", time'; }
free_kb() { df -k "${TART_HOME:-$HOME/.tart}" | awk 'NR==2{print $4}'; }

# Run ssh with the image's public test password via SSH_ASKPASS (no sshpass needed).
askpass=$STATE/askpass.sh
mk_askpass() {
    printf '#!/bin/sh\necho "%s"\n' "$GUEST_PASS" > "$askpass"
    chmod 700 "$askpass"
}
guest_ssh() { # guest_ssh IP [ssh-flags] -- CMD...  (flags before IP not supported)
    local ip=$1; shift
    mk_askpass
    SSH_ASKPASS=$askpass SSH_ASKPASS_REQUIRE=force DISPLAY=none \
    ssh -o StrictHostKeyChecking=no -o UserKnownHostsFile=/dev/null -o LogLevel=ERROR \
        -o PreferredAuthentications=password,keyboard-interactive -o PubkeyAuthentication=no \
        -o ConnectTimeout=5 -o NumberOfPasswordPrompts=1 "$GUEST_USER@$ip" "$@"
}

vm_ip() { tart ip --wait 5 "$1" 2>/dev/null; }

# guest VM CMD...: run CMD (joined with spaces, as ssh does) in the guest's shell.
# exec needs the tart guest agent in the image; ssh needs host-to-guest networking,
# which macOS Local Network privacy can block for the calling app.
guest() {
    local vm=$1; shift
    if [ "$TRANSPORT" = exec ]; then
        tart exec "$vm" /bin/sh -c "$*" </dev/null
    else
        local ip; ip=$(vm_ip "$vm") || { echo "tart.sh: $vm has no IP" >&2; return 255; }
        guest "$vm" "$*" </dev/null
    fi
}

cmd_pull() {
    local v
    v=$(tart --version)
    [ "$v" = "$TART_VERSION" ] || log "warning: tart is $v, pinned $TART_VERSION"
    log "pulling $IMAGE"
    tart pull "$IMAGE"
}

cmd_clone() { # clone NAME [--from SRC]  (SRC: another ntfsmac- VM, e.g. golden; default the pinned image)
    local vm; vm=$(vmname "${1:-}"); shift || true
    local src=$IMAGE
    if [ "${1:-}" = --from ]; then
        [ $# -ge 2 ] || die "--from needs a VM name"
        src=$(vmname "$2")
        vm_exists "$src" || die "source VM $src does not exist"
    fi
    vm_exists "$vm" && die "$vm already exists"
    tart clone "$src" "$vm"
    log "cloned $src -> $vm"
}

cmd_run() {
    local vm; vm=$(vmname "${1:-}"); shift || true
    local args=() gfx=(--no-graphics)
    while [ $# -gt 0 ]; do
        case $1 in
            --disk) [ $# -ge 2 ] || die "--disk needs FILE[:opts]"
                    f=${2%%:*}
                    case $f in /dev/*) die "refusing a block device: $f" ;; esac
                    [ -f "$f" ] || die "disk image not found: $f"
                    args+=(--disk "$2"); shift 2 ;;
            --graphics) gfx=(); shift ;;  # open a VM window (needed for one-time GUI approvals, H3)
            *) die "unknown option: $1" ;;
        esac
    done
    vm_exists "$vm" || die "$vm does not exist (clone first)"
    local pidf=$STATE/$vm.pid logf=$STATE/$vm.log
    if [ -f "$pidf" ] && kill -0 "$(cat "$pidf")" 2>/dev/null; then die "$vm already running"; fi
    nohup tart run "${gfx[@]+"${gfx[@]}"}" --no-audio --no-clipboard "${args[@]+"${args[@]}"}" "$vm" \
        >"$logf" 2>&1 </dev/null &
    echo $! > "$pidf"
    log "started $vm (pid $(cat "$pidf"), log $logf); waiting for guest ($TRANSPORT)"
    local ip="" t0; t0=$(date +%s)
    while :; do
        kill -0 "$(cat "$pidf")" 2>/dev/null || { tail -5 "$logf" >&2; die "tart run exited early"; }
        [ -n "$ip" ] || ip=$(vm_ip "$vm" || true)
        # perl alarm: tart exec can hang when no agent answers yet.
        if { perl -e 'alarm shift; exec @ARGV or exit 127' 15 "$0" exec "$vm" -- true >/dev/null 2>&1; } 2>/dev/null; then break; fi
        if [ $(( $(date +%s) - t0 )) -ge "$SSH_WAIT" ]; then
            if [ "$TRANSPORT" = exec ]; then
                die "no guest agent after ${SSH_WAIT}s (ip='${ip}'). The image needs the tart guest agent (macos-*-base images have it); see vm/macos/README.md"
            fi
            die "no SSH after ${SSH_WAIT}s (ip='${ip}'). If the lease exists but nothing answers, the calling app probably lacks macOS Local Network permission; use NTFSMAC_TRANSPORT=exec or see vm/macos/README.md"
        fi
        sleep 3
    done
    log "$vm up at $ip after $(( $(date +%s) - t0 ))s"
    echo "$ip"
}

cmd_ssh() {
    local vm; vm=$(vmname "${1:-}"); shift || true
    [ "${1:-}" = "--" ] && shift
    guest "$vm" "$@"
}

cmd_stop() {
    local vm; vm=$(vmname "${1:-}")
    local pidf=$STATE/$vm.pid
    # Clean guest shutdown first: a plain `tart stop` was seen to lose writes made seconds
    # before it (T-M2-02). Best effort; tart stop below forces it after its timeout.
    if [ "$TRANSPORT" = exec ]; then
        { perl -e 'alarm shift; exec @ARGV or exit 127' 20 tart exec "$vm" /bin/sh -c 'sync; sudo -n /sbin/shutdown -h now' >/dev/null 2>&1; } 2>/dev/null || true
        if [ -f "$pidf" ]; then
            for _ in $(seq 1 60); do kill -0 "$(cat "$pidf")" 2>/dev/null || break; sleep 1; done
        fi
    fi
    tart stop "$vm" 2>/dev/null || true
    if [ -f "$pidf" ]; then
        local p; p=$(cat "$pidf")
        for _ in $(seq 1 30); do kill -0 "$p" 2>/dev/null || break; sleep 1; done
        kill -0 "$p" 2>/dev/null && kill "$p" 2>/dev/null || true
        rm -f "$pidf"
    fi
    log "stopped $vm"
}

cmd_delete() {
    local vm; vm=$(vmname "${1:-}")
    case $vm in "$VM_PREFIX"?*) ;; *) die "refusing to delete without prefix $VM_PREFIX" ;; esac
    vm_exists "$vm" || { log "$vm not present"; return 0; }
    cmd_stop "$vm" || true
    tart delete "$vm"
    rm -f "$STATE/$vm.log" "$STATE/$vm.pid"
    log "deleted $vm"
}

cmd_selftest() {
    local vm="${VM_PREFIX}selftest-$$"
    local d1=$STATE/$vm-rw.img d2=$STATE/$vm-ro.img
    cleanup() {
        set +e
        cmd_delete "$vm" >/dev/null 2>&1
        rm -f "$d1" "$d2"
    }
    trap cleanup EXIT
    local fail=0
    chk() { if [ "$1" = 0 ]; then echo "PASS: $2"; else echo "FAIL: $2"; fail=1; fi; }
    # check DESC CMD...: record PASS/FAIL without tripping set -e.
    check() { local d=$1; shift; if "$@"; then chk 0 "$d"; else chk 1 "$d"; fi; }

    echo "== pull (pinned digest) =="
    cmd_pull
    tart list --quiet | grep -qF "$GUEST_IMAGE_DIGEST" || tart list | grep -F "$GUEST_IMAGE_REPO" || true

    echo "== clone as snapshot =="
    local f0 f1 t0 t1
    f0=$(free_kb); t0=$(now)
    cmd_clone "$vm"
    t1=$(now); f1=$(free_kb)
    echo "clone time: $(echo "$t1 - $t0" | bc) s; host free space change: $(( (f0 - f1) / 1024 )) MiB"
    chk 0 "clone created ($vm)"

    echo "== scratch disks =="
    dd if=/dev/zero of="$d1" bs=1m count=0 seek=256 2>/dev/null   # sparse 256 MiB
    dd if=/dev/zero of="$d2" bs=1m count=0 seek=64 2>/dev/null    # sparse 64 MiB
    ls -ls "$d1" "$d2"

    echo "== boot headless with extra disks =="
    local ip
    ip=$(cmd_run "$vm" --disk "$d1" --disk "$d2:ro" | tail -1)
    echo "guest IP: $ip"
    echo "-- sw_vers"; local sw; sw=$(guest "$vm" sw_vers); echo "$sw"
    echo "-- uname -m"; local arch; arch=$(guest "$vm" uname -m); echo "$arch"
    check "guest is macOS 27.x" grep -q 'ProductVersion:[[:space:]]*27\.' <<<"$sw"
    check "guest arch arm64" [ "$arch" = arm64 ]

    echo "-- FSKit probe"
    guest "$vm" 'systemextensionsctl list 2>&1 | head -20' || true
    guest "$vm" 'echo "pluginkit fskit.fsmodule:"; pluginkit -m -A -p com.apple.fskit.fsmodule 2>&1 | head -20; echo "(exit $?)"' || true
    guest "$vm" 'ls /System/Library/Frameworks/FSKit.framework >/dev/null 2>&1 && echo "FSKit.framework: present" || echo "FSKit.framework: not on disk (dyld cache only?)"; ls -d /System/Library/ExtensionKit/Extensions/*FS* /Library/Filesystems/* 2>/dev/null | head; echo "fskitd:"; pgrep -lf fskitd || true'

    echo "-- diskutil list"
    guest "$vm" 'diskutil list'
    local found_rw found_ro
    # shellcheck disable=SC2016 # expanded in the guest shell, not here
    found_rw=$(guest "$vm" 'for d in $(diskutil list | awk "/^\\/dev\\/disk[0-9]+ /{print \$1}"); do diskutil info "$d" | grep -q "268435456 Bytes" && echo "$d"; done; true' | head -1)
    # shellcheck disable=SC2016 # expanded in the guest shell, not here
    found_ro=$(guest "$vm" 'for d in $(diskutil list | awk "/^\\/dev\\/disk[0-9]+ /{print \$1}"); do diskutil info "$d" | grep -q "67108864 Bytes" && echo "$d"; done; true' | head -1)
    echo "rw disk device: ${found_rw:-none}; ro disk device: ${found_ro:-none}"
    check "guest sees 256 MiB rw disk as block device" [ -n "$found_rw" ]
    check "guest sees 64 MiB ro disk as block device" [ -n "$found_ro" ]
    if [ -n "$found_ro" ]; then
        guest "$vm" "diskutil info $found_ro | grep -E 'Read-Only|Device Location|Protocol'"
        check "ro disk reported read-only" guest "$vm" "diskutil info $found_ro | grep -E 'Media Read-Only|Read-Only Media' | grep -q Yes"
    fi
    [ -z "$found_rw" ] || guest "$vm" "diskutil info $found_rw | grep -E 'Read-Only|Device Location|Protocol'"

    echo "== stop and delete clone; base must survive =="
    cmd_stop "$vm"
    cmd_delete "$vm"
    check "clone deleted" not vm_exists "$vm"
    check "base image still present" base_present
    trap - EXIT
    rm -f "$d1" "$d2"
    if [ "$fail" = 0 ]; then echo "SELFTEST OK"; else echo "SELFTEST FAILED"; return 1; fi
}

sub=${1:-}; [ $# -gt 0 ] && shift
case $sub in
    pull) cmd_pull ;;
    clone) cmd_clone "$@" ;;
    run) cmd_run "$@" ;;
    ssh|exec) cmd_ssh "$@" ;;
    stop) cmd_stop "$@" ;;
    delete) cmd_delete "$@" ;;
    selftest) cmd_selftest ;;
    *) sed -n '2,12p' "$0"; exit 2 ;;
esac
