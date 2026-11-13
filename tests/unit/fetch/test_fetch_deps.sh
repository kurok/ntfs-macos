#!/bin/sh
# Offline test for scripts/fetch-deps.sh. Uses file:// URLs via --allow-file-urls.
# Usage: tests/unit/fetch/test_fetch_deps.sh   (exit 0 = all pass)
set -eu

here=$(CDPATH= cd -- "$(dirname -- "$0")" && pwd)
fetch=$here/../../../scripts/fetch-deps.sh
work=$(mktemp -d "${TMPDIR:-/tmp}/fetch-deps-test.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM

if command -v sha256sum >/dev/null 2>&1; then sum() { sha256sum "$1" | cut -d' ' -f1; }
else sum() { shasum -a 256 "$1" | cut -d' ' -f1; }; fi

fails=0
check() { # description, condition-exit-code
    if [ "$2" -eq 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}
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
check "correct digest exits 0" "$([ $rc -eq 0 ]; echo $?)"
check "artifact present in cache" "$([ -f "$work/cache/art.tgz" ]; echo $?)"
check "cached file has the pinned digest" "$([ "$(sum "$work/cache/art.tgz")" = "$good" ]; echo $?)"

# 2. second run uses the cache, no re-download
run
check "second run exits 0" "$([ $rc -eq 0 ]; echo $?)"
check "second run reports cached" "$(grep -q 'ok (cached)' "$work/out"; echo $?)"
check "second run does not fetch" "$(! grep -q '^fetching' "$work/out"; echo $?)"

# 3. cached file corrupted -> re-downloaded and repaired
printf 'X' >> "$work/cache/art.tgz"
run
check "corrupt cache is re-fetched and passes" "$([ $rc -eq 0 ] && grep -q 'ok (verified)' "$work/out"; echo $?)"

# 4. flipped byte in the source: fails with exit 1 and leaves no file
rm -rf "$work/cache"
printf 'hellp artifact\n' > "$work/src/art.tgz"   # one byte changed
run
check "altered byte exits 1" "$([ $rc -eq 1 ]; echo $?)"
check "mismatch message printed" "$(grep -q 'CHECKSUM MISMATCH' "$work/out"; echo $?)"
check "altered file removed from cache" "$([ ! -e "$work/cache/art.tgz" ]; echo $?)"
check "no partial files left" "$([ -z "$(ls -A "$work/cache")" ]; echo $?)"

# 5. altered byte already in the cache with the wrong digest is deleted on failure
printf 'hellp artifact\n' > "$work/cache/art.tgz"
run
check "bad cached + bad source exits 1 and removes file" "$([ $rc -eq 1 ] && [ ! -e "$work/cache/art.tgz" ]; echo $?)"

# 6. file:// refused without the test flag; http refused always
rc=0; "$fetch" --lock "$work/test.lock" --cache-dir "$work/cache" >"$work/out" 2>&1 || rc=$?
check "file:// URL refused by default" "$([ $rc -eq 1 ] && grep -q 'non-HTTPS' "$work/out"; echo $?)"
printf 'x 1 http://example.invalid/a.tgz %s\n' "$good" > "$work/http.lock"
rc=0; "$fetch" --allow-file-urls --lock "$work/http.lock" --cache-dir "$work/cache" >"$work/out" 2>&1 || rc=$?
check "http:// URL refused" "$([ $rc -eq 1 ] && grep -q 'non-HTTPS' "$work/out"; echo $?)"

# 7. the real lock file parses (format only, no network)
awk '!/^#/ && NF && NF != 4 { bad=1 } END { exit bad }' "$here/../../../third_party/deps.lock"
check "repo deps.lock has 4 fields per entry" "$?"

if [ "$fails" -eq 0 ]; then echo "ALL PASSED"; exit 0; fi
echo "$fails FAILED"; exit 1
