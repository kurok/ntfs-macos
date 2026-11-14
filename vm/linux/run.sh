#!/bin/sh
# run.sh - Environment A (Linux container with pinned NTFS-3G + ntfsprogs).
#
# Usage: vm/linux/run.sh build [--asan]
#        vm/linux/run.sh shell [--asan] [-- CMD...]
#        vm/linux/run.sh selftest [--asan]
#        vm/linux/run.sh run [--asan] -- CMD...
#
# The repo is mounted read-only at /work. Images live in the container's /tmp only.
# The container gets /dev/fuse and CAP_SYS_ADMIN, nothing else (no --privileged).
# Env: NTFS_LINUX_APPARMOR_UNCONFINED=1 adds --security-opt apparmor=unconfined.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(dirname "$(dirname "$here")")
cache=$repo/third_party/cache
lock=$repo/third_party/deps.lock

die() { echo "run.sh: error: $*" >&2; exit 1; }
command -v docker >/dev/null 2>&1 || die "docker not found"

[ $# -ge 1 ] || die "usage: run.sh build|shell|selftest|run [--asan]"
cmd=$1; shift
asan=0
while [ $# -gt 0 ]; do
    case $1 in
        --asan) asan=1; shift ;;
        --) shift; break ;;
        *) die "unknown option: $1" ;;
    esac
done
if [ "$asan" = 1 ]; then tag=ntfs-macos-linux:asan; else tag=ntfs-macos-linux:release; fi

lock_field() { awk -v n=ntfs-3g '$1 !~ /^#/ && $1 == n { print $'"$1"' }' "$lock"; }

do_build() {
    sh "$repo/scripts/fetch-deps.sh" ntfs-3g
    url=$(lock_field 3); sha=$(lock_field 4)
    file=${url##*/}
    [ -f "$cache/$file" ] || die "missing $cache/$file after fetch"
    # Minimal build context: Dockerfile + the verified tarball only.
    ctx=$(mktemp -d "${TMPDIR:-/tmp}/ntfs-linux-ctx.XXXXXX")
    trap 'rm -rf "$ctx"' EXIT
    cp "$here/Dockerfile" "$ctx/Dockerfile"
    cp "$cache/$file" "$ctx/$file"
    docker build --build-arg ASAN="$asan" --build-arg NTFS3G_FILE="$file" \
        --build-arg NTFS3G_SHA256="$sha" -t "$tag" "$ctx"
}

docker_run() {
    flags=""
    [ "${NTFS_LINUX_APPARMOR_UNCONFINED:-0}" = 1 ] && flags="--security-opt apparmor=unconfined"
    tty=""
    [ -t 0 ] && [ -t 1 ] && tty="-t"
    # shellcheck disable=SC2086
    docker run --rm -i $tty --device /dev/fuse --cap-add SYS_ADMIN $flags \
        -v "$repo":/work:ro "$@"
}

selftest_script() {
cat <<'INNER'
set -eu
img=/tmp/selftest.img
mnt=/tmp/selftest.mnt
mkdir -p "$mnt"
ntfs-3g --version | head -1
rm -f /tmp/asan/*
truncate -s 64M "$img"
mkntfs -F -Q -L selftest "$img" >/tmp/mkntfs.log 2>&1 || { cat /tmp/mkntfs.log; exit 1; }
ntfs-3g "$img" "$mnt" -o rw,big_writes 2>&1
mkdir "$mnt/dir"
head -c 3000000 /dev/urandom > "$mnt/dir/blob.bin"
printf 'hello ntfs\n' > "$mnt/hello.txt"
before=$(cd "$mnt" && sha256sum hello.txt dir/blob.bin)
sync
umount "$mnt"
ntfs-3g "$img" "$mnt" -o ro 2>&1
after=$(cd "$mnt" && sha256sum hello.txt dir/blob.bin)
[ -d "$mnt/dir" ] || { echo "FAIL: dir missing"; exit 1; }
[ "$before" = "$after" ] || { echo "FAIL: hash mismatch"; echo "$before"; echo "$after"; exit 1; }
echo "$after"
umount "$mnt"
ntfsfix -n "$img" >/tmp/ntfsfix.log 2>&1 || { cat /tmp/ntfsfix.log; echo "FAIL: ntfsfix -n"; exit 1; }
sleep 1
if [ "${NTFS_ASAN:-0}" = 1 ]; then
    if ls /tmp/asan/* >/dev/null 2>&1; then
        echo "FAIL: sanitizer reports:"; cat /tmp/asan/*; exit 1
    fi
    echo "ASan/UBSan: clean"
fi
echo "selftest OK"
INNER
}

case $cmd in
    build) do_build ;;
    shell) docker_run "$tag" bash "$@" ;;
    run) [ $# -gt 0 ] || die "run needs -- CMD"; docker_run "$tag" "$@" ;;
    selftest) selftest_script | docker_run "$tag" bash -s ;;
    *) die "unknown subcommand: $cmd" ;;
esac
