#!/bin/sh
# fetch-deps.sh - download pinned dependencies and verify their SHA-256.
#
# Reads third_party/deps.lock (name version url sha256 per line), downloads each artifact
# over HTTPS into the cache directory, and checks the digest. A cached file with the right
# digest is not downloaded again. On a mismatch the file is deleted and the script exits 1.
# Nothing is extracted or executed.
#
# Usage: scripts/fetch-deps.sh [--lock FILE] [--cache-dir DIR] [--allow-file-urls] [NAME...]
# Env:   DEPS_LOCK, DEPS_CACHE_DIR (overridden by the options)
# --allow-file-urls permits file:// URLs; it exists for the offline unit test only.
set -eu

script_dir=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
repo_root=$(dirname "$script_dir")
lock=${DEPS_LOCK:-$repo_root/third_party/deps.lock}
cache=${DEPS_CACHE_DIR:-$repo_root/third_party/cache}
allow_file=0

die() { echo "fetch-deps: error: $*" >&2; exit 1; }

while [ $# -gt 0 ]; do
    case $1 in
        --lock) [ $# -ge 2 ] || die "--lock needs a value"; lock=$2; shift 2 ;;
        --cache-dir) [ $# -ge 2 ] || die "--cache-dir needs a value"; cache=$2; shift 2 ;;
        --allow-file-urls) allow_file=1; shift ;;
        -h|--help) sed -n '2,13p' "$0" | sed 's/^# \{0,1\}//'; exit 0 ;;
        --) shift; break ;;
        -*) die "unknown option: $1" ;;
        *) break ;;
    esac
done
only="$*"

if command -v sha256sum >/dev/null 2>&1; then
    sha256_of() { sha256sum "$1" | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
    sha256_of() { shasum -a 256 "$1" | cut -d' ' -f1; }
else
    die "need sha256sum or shasum"
fi
command -v curl >/dev/null 2>&1 || die "need curl"

[ -r "$lock" ] || die "lock file not readable: $lock"
mkdir -p "$cache"

failed=0
count=0
while IFS= read -r line || [ -n "$line" ]; do
    # shellcheck disable=SC2086
    set -- $line
    [ $# -gt 0 ] || continue
    case $1 in '#'*) continue ;; esac
    [ $# -eq 4 ] || die "bad lock line (want: name version url sha256): $line"
    name=$1 version=$2 url=$3 want=$4

    if [ -n "$only" ]; then
        case " $only " in *" $name "*) ;; *) continue ;; esac
    fi

    case $url in
        https://*) ;;
        file://*) [ "$allow_file" = 1 ] || die "$name: non-HTTPS URL refused: $url" ;;
        *) die "$name: non-HTTPS URL refused: $url" ;;
    esac
    case $want in
        *[!0-9a-f]*) die "$name: sha256 must be 64 lowercase hex chars" ;;
    esac
    [ ${#want} -eq 64 ] || die "$name: sha256 must be 64 lowercase hex chars"

    file=${url##*/}
    case $file in ''|.|..|*[!A-Za-z0-9._+-]*) die "$name: unsafe file name in URL: $url" ;; esac
    dest=$cache/$file
    count=$((count + 1))

    if [ -f "$dest" ] && [ "$(sha256_of "$dest")" = "$want" ]; then
        echo "ok (cached)  $name $version  $file"
        continue
    fi
    [ -e "$dest" ] && rm -f "$dest"

    tmp=$dest.part.$$
    echo "fetching     $name $version  $url"
    if ! curl -fsSL --proto '=https,file' --proto-redir '=https' -o "$tmp" "$url"; then
        rm -f "$tmp"
        echo "fetch-deps: FAILED to download $name $version from $url" >&2
        failed=1
        continue
    fi
    got=$(sha256_of "$tmp")
    if [ "$got" != "$want" ]; then
        rm -f "$tmp" "$dest"
        echo "fetch-deps: CHECKSUM MISMATCH for $name $version ($file)" >&2
        echo "  expected: $want" >&2
        echo "  actual:   $got" >&2
        echo "  file deleted; refusing to use it" >&2
        failed=1
        continue
    fi
    mv "$tmp" "$dest"
    echo "ok (verified) $name $version  $file"
done < "$lock"

[ "$failed" = 0 ] || exit 1
[ "$count" -gt 0 ] || echo "fetch-deps: no artifacts selected" >&2
exit 0
