#!/bin/sh
# Offline test for scripts/fetch-deps.sh. Uses file:// URLs via --allow-file-urls.
# Usage: tests/unit/fetch/test_fetch_deps.sh   (exit 0 = all pass)
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
fetch=$here/../../../scripts/fetch-deps.sh
work=$(mktemp -d "${TMPDIR:-/tmp}/fetch-deps-test.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

if command -v sha256sum >/dev/null 2>&1; then sum() { sha256sum "$1" | cut -d' ' -f1; }
else sum() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

fails=0
check() { # description, condition-exit-code
    if [ "$2" -eq 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}
# shellcheck disable=SC2120 # run takes optional extra fetch-deps.sh arguments
run() { # sets rc; output in $work/out
    rc=0
    "$fetch" --allow-file-urls --lock "$work/test.lock" --cache-dir "$work/cache" "$@" >"$work/out" 2>&1 || rc=$?
}

mkdir -p "$work/src"
printf 'hello artifact\n' > "$work/src/art.tgz"
good=$(sum "$work/src/art.tgz")
printf '# test lock\n\ntestdep 1.0 file://%s/src/art.tgz %s\n' "$work" "$good" > "$work/test.lock"

# 1. correct digest passes
run
if [ $rc -eq 0 ]; then r=0; else r=1; fi; check "correct digest exits 0" "$r"
if [ -f "$work/cache/art.tgz" ]; then r=0; else r=1; fi; check "artifact present in cache" "$r"
if [ "$(sum "$work/cache/art.tgz")" = "$good" ]; then r=0; else r=1; fi; check "cached file has the pinned digest" "$r"

# 2. second run uses the cache, no re-download
run
if [ $rc -eq 0 ]; then r=0; else r=1; fi; check "second run exits 0" "$r"
if grep -q 'ok (cached)' "$work/out"; then r=0; else r=1; fi; check "second run reports cached" "$r"
if ! grep -q '^fetching' "$work/out"; then r=0; else r=1; fi; check "second run does not fetch" "$r"

# 3. cached file corrupted -> re-downloaded and repaired
printf 'X' >> "$work/cache/art.tgz"
run
if [ $rc -eq 0 ] && grep -q 'ok (verified)' "$work/out"; then r=0; else r=1; fi; check "corrupt cache is re-fetched and passes" "$r"

# 4. flipped byte in the source: fails with exit 1 and leaves no file
rm -rf "$work/cache"
printf 'hellp artifact\n' > "$work/src/art.tgz"   # one byte changed
run
if [ $rc -eq 1 ]; then r=0; else r=1; fi; check "altered byte exits 1" "$r"
if grep -q 'CHECKSUM MISMATCH' "$work/out"; then r=0; else r=1; fi; check "mismatch message printed" "$r"
if [ ! -e "$work/cache/art.tgz" ]; then r=0; else r=1; fi; check "altered file removed from cache" "$r"
if [ -z "$(ls -A "$work/cache")" ]; then r=0; else r=1; fi; check "no partial files left" "$r"

# 5. altered byte already in the cache with the wrong digest is deleted on failure
printf 'hellp artifact\n' > "$work/cache/art.tgz"
run
if [ $rc -eq 1 ] && [ ! -e "$work/cache/art.tgz" ]; then r=0; else r=1; fi; check "bad cached + bad source exits 1 and removes file" "$r"

# 6. file:// refused without the test flag; http refused always
rc=0; "$fetch" --lock "$work/test.lock" --cache-dir "$work/cache" >"$work/out" 2>&1 || rc=$?
if [ $rc -eq 1 ] && grep -q 'non-HTTPS' "$work/out"; then r=0; else r=1; fi; check "file:// URL refused by default" "$r"
printf 'x 1 http://example.invalid/a.tgz %s\n' "$good" > "$work/http.lock"
rc=0; "$fetch" --allow-file-urls --lock "$work/http.lock" --cache-dir "$work/cache" >"$work/out" 2>&1 || rc=$?
if [ $rc -eq 1 ] && grep -q 'non-HTTPS' "$work/out"; then r=0; else r=1; fi; check "http:// URL refused" "$r"

# 7. the real lock file parses (format only, no network)
awk '!/^#/ && NF && NF != 4 { bad=1 } END { exit bad }' "$here/../../../third_party/deps.lock"
check "repo deps.lock has 4 fields per entry" "$?"

if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; exit 0; fi
echo "$fails FAILED"; exit 1
