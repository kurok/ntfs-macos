#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
# Test for scripts/run-inputs.sh and for its use by the suites (audit M5-F07): a deps tar and a fixtures
# directory carry a key, a different, missing or unreadable key refuses, a fixtures image is checked by its own
# stamp line and sha256. Fake tars, fake directories and a throw-away git repository in a temp dir; no VM, no
# Docker. The fixtures part needs git and is skipped, with a message, when git is missing.
# Usage: tests/unit/ci/test_run_inputs.sh   (exit 0 = all pass)
#   RUN_INPUTS=FILE tests the script at FILE instead (RUN_INPUTS_ROOT is set to the checkout), for a mutation check.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
repo=$(CDPATH='' cd -- "$here/../../.." && pwd)
ri=${RUN_INPUTS:-$repo/scripts/run-inputs.sh}
work=$(mktemp -d "${TMPDIR:-/tmp}/run-inputs-test.XXXXXX")
trap 'rm -rf "$work"' EXIT INT TERM
unset RUN_INPUTS_ROOT

fails=0
check() { # description, condition-exit-code
	if [ "$2" -eq 0 ]; then echo "PASS  $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi
}
ok() { # description, command...: the command must succeed
	_d=$1; shift
	if "$@" >"$work/out" 2>&1; then check "$_d" 0; else check "$_d" 1; sed 's/^/      /' "$work/out"; fi
}
refused() { # description, command...: the command must fail with exit 1
	_d=$1; shift
	_rc=0
	"$@" >"$work/out" 2>&1 || _rc=$?
	if [ "$_rc" -eq 1 ]; then check "$_d" 0; else check "$_d (exit $_rc)" 1; sed 's/^/      /' "$work/out"; fi
}
say() { grep -q "$1" "$work/out" && r=0 || r=1; check "$2" "$r"; }
# R: run-inputs against the real checkout (deps), F: against the throw-away repository (fixtures)
R() { RUN_INPUTS_ROOT=$repo sh "$ri" "$@"; }
F() { RUN_INPUTS_ROOT=$work/repo sh "$ri" "$@"; }

zeros=0000000000000000000000000000000000000000000000000000000000000000
ones=1111111111111111111111111111111111111111111111111111111111111111

# ================================================================== deps key and deps tars
k=$(R deps-key)
case $k in *[!0-9a-f]* | '') bad=1 ;; *) bad=0 ;; esac
[ ${#k} -eq 64 ] || bad=1
check "deps-key is 64 hex digits" "$bad"
[ "$(R deps-key)" = "$k" ] && r=0 || r=1; check "deps-key is stable" "$r"
[ "$(sh "$repo/scripts/build-ntfs3g.sh" --print-key --fuse external --prefix /elsewhere --build-dir /else/where)" = "$k" ] && r=0 || r=1
check "the key does not depend on --prefix and --build-dir" "$r"
[ "$(CFLAGS=-O0 LDFLAGS=-L/x sh "$repo/scripts/build-ntfs3g.sh" --print-key --fuse external)" = "$k" ] && r=0 || r=1
check "the key ignores the host CFLAGS and LDFLAGS (the guest builds run with them unset)" "$r"
grep -B1 'for t in tar patch make awk' "$repo/scripts/build-ntfs3g.sh" | grep -q 'print_key" = 0' && r=0 || r=1
check "--print-key exits before the tool-presence loop (the loop is gated)" "$r"

mkdir -p "$work/t/deps/lib"
echo lib >"$work/t/deps/lib/libntfs-3g.a"
(cd "$work/t" && tar -cf "$work/plain.tar" deps)
cp "$work/plain.tar" "$work/sealed.tar"
ok "deps-seal appends the key" R deps-seal "$work/sealed.tar" "$k" dmn
ok "a sealed tar passes deps-check" R deps-check "$work/sealed.tar"
ok "a sealed tar passes with the current key and its layout given" R deps-check "$work/sealed.tar" --key "$k" --layout dmn
refused "a sealed tar is refused for another key" R deps-check "$work/sealed.tar" --key "$ones"
refused "a tar of the dmn layout is refused for the pkg layout" R deps-check "$work/sealed.tar" --layout pkg
say 'dmn layout' "the refusal names the layouts"
cp "$work/plain.tar" "$work/pkg.tar"
R deps-seal "$work/pkg.tar" "$k" pkg
ok "a pkg tar passes for the pkg layout" R deps-check "$work/pkg.tar" --layout pkg
refused "a pkg tar is refused for dmn (the default)" R deps-check "$work/pkg.tar"
refused "a tar without a key is refused" R deps-check "$work/plain.tar"
say 'no deps/.ntfsmac-deps-key' "the refusal says that the key is missing"
refused "a sealed tar is not sealed twice" R deps-seal "$work/sealed.tar" "$k" dmn
refused "a tar with two key members is refused" sh -c '
	cp "$1" "$2"; mkdir -p "$3/deps"; printf "%s dmn\n" "$4" >"$3/deps/.ntfsmac-deps-key"
	(cd "$3" && tar -rf "$2" deps/.ntfsmac-deps-key); RUN_INPUTS_ROOT="$6" sh "$5" deps-check "$2"' sh "$work/sealed.tar" "$work/twice.tar" "$work/t2" "$ones" "$ri" "$repo"
# a key of another commit (made without the real key, as an older checkout would have)
mkdir -p "$work/t3/deps"
printf '%s dmn\n' "$zeros" >"$work/t3/deps/.ntfsmac-deps-key"
(cd "$work/t3" && tar -cf "$work/other.tar" deps)
refused "a tar made for a different key is refused" R deps-check "$work/other.tar"
say 'REFUSED' "the refusal is announced"
printf 'not-a-key\n' >"$work/t3/deps/.ntfsmac-deps-key"
(cd "$work/t3" && tar -cf "$work/malformed.tar" deps)
refused "a tar with a malformed key is refused" R deps-check "$work/malformed.tar"
echo "this is not a tar" >"$work/garbage.tar"
refused "an unreadable tar (not a tar) is refused" R deps-check "$work/garbage.tar"
refused "a missing tar is refused" R deps-check "$work/missing.tar"
gzip -c "$work/sealed.tar" >"$work/sealed.tar.gz"
refused "deps-seal refuses a compressed tar (it cannot be appended to)" R deps-seal "$work/sealed.tar.gz" "$k" dmn
# nothing outside deps/ may ride along
mkdir -p "$work/t4/deps" "$work/t4/other"
printf '%s dmn\n' "$k" >"$work/t4/deps/.ntfsmac-deps-key"
echo x >"$work/t4/other/evil"
(cd "$work/t4" && tar -cf "$work/outside.tar" deps other)
refused "a tar with a member outside deps/ is refused (even with a good key)" R deps-check "$work/outside.tar"
say 'outside deps/' "the refusal names the member outside deps/"
echo x >"$work/t4/evil-top"
(cd "$work/t4" && rm -rf other && tar -cf "$work/outside2.tar" deps evil-top)
refused "a top-level extra file is refused too" R deps-check "$work/outside2.tar"
# links must stay inside deps/
mkdir -p "$work/t5/deps/lib"
printf '%s dmn\n' "$k" >"$work/t5/deps/.ntfsmac-deps-key"
echo x >"$work/t5/deps/lib/real"
ln -s real "$work/t5/deps/lib/inside"
ln -s ../lib/real "$work/t5/deps/lib/inside2"
(cd "$work/t5" && tar -cf "$work/links-ok.tar" deps)
ok "relative symlinks that stay inside deps/ are accepted" R deps-check "$work/links-ok.tar"
ln -s ../../outside "$work/t5/deps/lib/escape"
(cd "$work/t5" && tar -cf "$work/links-up.tar" deps)
refused "a symlink that climbs out of deps/ is refused" R deps-check "$work/links-up.tar"
say 'points outside deps' "the refusal names the link"
rm "$work/t5/deps/lib/escape"
ln -s /etc/passwd "$work/t5/deps/lib/abs"
(cd "$work/t5" && tar -cf "$work/links-abs.tar" deps)
refused "an absolute symlink is refused" R deps-check "$work/links-abs.tar"
rm "$work/t5/deps/lib/abs"
ln -s ../.. "$work/t5/deps/lib/updir"
(cd "$work/t5" && tar -cf "$work/links-up2.tar" deps)
refused "a symlink to the directory above deps/ is refused" R deps-check "$work/links-up2.tar"
info=$(R deps-info "$work/sealed.tar")
echo "$info" | grep -q "^deps_tar_key=$k\$" && r=0 || r=1; check "deps-info shows the stored key" "$r"
if command -v shasum >/dev/null 2>&1; then want=$(shasum -a 256 "$work/sealed.tar" | cut -d' ' -f1); else want=$(sha256sum "$work/sealed.tar" | cut -d' ' -f1); fi
echo "$info" | grep -q "^deps_tar_sha256=$want\$" && r=0 || r=1; check "deps-info shows the hash of the tar" "$r"
R deps-info "$work/plain.tar" | grep -q '^deps_tar_key=none$' && r=0 || r=1; check "deps-info says none for a tar without a key" "$r"
# N5: the key sealed is the one the caller computed before the tree was copied, not one read at seal time
cp "$work/plain.tar" "$work/early.tar"
R deps-seal "$work/early.tar" "$ones" dmn
refused "a tar sealed with a key computed earlier is refused when the tree changed since (key differs)" R deps-check "$work/early.tar"

# ================================================================== fixtures (a throw-away git repository)
if command -v git >/dev/null 2>&1; then
	mkdir -p "$work/repo"
	(
		cd "$work/repo"
		for p in tests/fixtures/gen/x.sh tests/fixtures/manifest.json tests/tools/t.c tests/stress/crashsweep/build.sh src/s.c include/i.h third_party/patches/p third_party/deps.lock vm/linux/Dockerfile scripts/build-ntfs3g.sh CMakeLists.txt cmake/c.cmake; do
			mkdir -p "$(dirname "$p")"
			echo "v1 $p" >"$p"
		done
		git init -q .
		git add -A
		git -c user.name=t -c user.email=t@example.invalid commit -q -m one
	)
	commit() { (cd "$work/repo" && git add -A && git -c user.name=t -c user.email=t@example.invalid commit -q -m "$1"); }
	fk=$(F fixtures-key)
	[ ${#fk} -eq 64 ] && r=0 || r=1; check "fixtures-key is 64 hex digits" "$r"
	[ "$(F fixtures-key)" = "$fk" ] && r=0 || r=1; check "fixtures-key is stable" "$r"
	# every input the generators depend on changes the key; a dirty tree is keyed on the contents, not refused
	prev=$fk
	for p in tests/fixtures/gen/x.sh tests/fixtures/manifest.json tests/tools/t.c tests/stress/crashsweep/build.sh src/s.c include/i.h third_party/patches/p third_party/deps.lock vm/linux/Dockerfile scripts/build-ntfs3g.sh CMakeLists.txt cmake/c.cmake; do
		echo "v2 $p" >"$work/repo/$p"
		ok "an uncommitted change in $p still gives a key" F fixtures-key
		dk=$(F fixtures-key)
		[ "$(F fixtures-dirty)" = 1 ] && r=0 || r=1; check "fixtures-dirty says 1 with an uncommitted change in $p" "$r"
		[ "$dk" != "$prev" ] && r=0 || r=1; check "the dirty key of $p differs from the clean key before" "$r"
		[ "$(F fixtures-key)" = "$dk" ] && r=0 || r=1; check "the dirty key of $p is stable for the same content" "$r"
		commit "change $p"
		now=$(F fixtures-key)
		[ "$(F fixtures-dirty)" = 0 ] && r=0 || r=1; check "fixtures-dirty says 0 after the commit of $p" "$r"
		[ "$now" != "$prev" ] && r=0 || r=1; check "a committed change in $p gives another key" "$r"
		[ "$dk" != "$now" ] && r=0 || r=1; check "a dirty key never equals the clean key of the same content ($p)" "$r"
		prev=$now
	done
	fk=$prev
	echo untracked >"$work/repo/src/new.c"
	ok "an untracked file under src/ still gives a key" F fixtures-key
	uk=$(F fixtures-key)
	[ "$uk" != "$fk" ] && [ "$(F fixtures-dirty)" = 1 ] && r=0 || r=1; check "an untracked file makes the tree dirty and changes the key" "$r"
	echo other >"$work/repo/src/new.c"
	[ "$(F fixtures-key)" != "$uk" ] && r=0 || r=1; check "the content of an untracked file is part of the dirty key" "$r"
	rm "$work/repo/src/new.c"
	[ "$(F fixtures-key)" = "$fk" ] && [ "$(F fixtures-dirty)" = 0 ] && r=0 || r=1; check "removing it gives the clean key back" "$r"
	rm "$work/repo/CMakeLists.txt"
	[ "$(F fixtures-key)" != "$fk" ] && [ "$(F fixtures-dirty)" = 1 ] && r=0 || r=1; check "a deleted tracked file makes the tree dirty" "$r"
	(cd "$work/repo" && git checkout -q -- CMakeLists.txt)
	echo other >"$work/repo/docs-only.txt"
	ok "a file outside the covered paths does not matter" F fixtures-key
	[ "$(F fixtures-key)" = "$fk" ] && [ "$(F fixtures-dirty)" = 0 ] && r=0 || r=1; check "and does not make the tree dirty" "$r"
	rm "$work/repo/docs-only.txt"
	refused "no git checkout: no fixtures key" sh -c 'RUN_INPUTS_ROOT="$2" sh "$1" fixtures-key' sh "$ri" "$work"

	# an export: a tgz with two images and a list; the stamp lists the images
	mkdir -p "$work/exp1"
	printf 'image one' >"$work/exp1/a.img"
	printf 'image two' >"$work/exp1/b.img"
	printf 'names' >"$work/exp1/a.list"
	(cd "$work/exp1" && tar -czf "$work/e1.tgz" ./a.img ./b.img ./a.list)
	mkdir -p "$work/fx"
	tar -xzf "$work/e1.tgz" -C "$work/fx"
	refused "an unstamped directory is refused" F fixtures-check "$work/fx" a
	say 'no .fixtures-key' "the refusal says there is no stamp"
	ok "fixtures-stamp writes one line per image extracted" F fixtures-stamp "$work/fx" "$work/e1.tgz"
	[ "$(wc -l <"$work/fx/.fixtures-key" | tr -d ' ')" -eq 2 ] && r=0 || r=1; check "the stamp has a line for a.img and b.img only (not the list)" "$r"
	grep -q "^a.img [0-9a-f]\{64\} $fk\$" "$work/fx/.fixtures-key" && r=0 || r=1; check "a stamp line is: name sha256 key" "$r"
	ok "a stamped image passes" F fixtures-check "$work/fx" a
	ok "both stamped images pass" F fixtures-check "$work/fx" a b
	ok "no names: every image is checked" F fixtures-check "$work/fx"
	refused "an image that is not in the stamp is refused" sh -c 'echo c >"$2/c.img"; RUN_INPUTS_ROOT="$3" sh "$1" fixtures-check "$2" c' sh "$ri" "$work/fx" "$work/repo"
	say 'not listed exactly once' "the refusal says the image is not listed"
	refused "no names with an unlisted image in the directory is refused" F fixtures-check "$work/fx"
	rm "$work/fx/c.img"
	refused "a named image that is missing is refused" F fixtures-check "$work/fx" nothere
	refused "another key is refused" F fixtures-check "$work/fx" --key "$ones" a
	# an image edited after it was stamped (S1)
	printf 'edited' >"$work/fx/a.img"
	refused "an image edited after stamping is refused (sha256)" F fixtures-check "$work/fx" a
	say 'changed after it was stamped' "the refusal names the edit"
	ok "the other image still passes" F fixtures-check "$work/fx" b
	printf 'image one' >"$work/fx/a.img"
	ok "restoring the bytes passes again" F fixtures-check "$work/fx" a
	# a partial re-export under a new key re-stamps only what it extracted; the old image keeps its old key (S1)
	echo "v3" >"$work/repo/src/s.c"
	commit "move on"
	fk2=$(F fixtures-key)
	[ "$fk2" != "$fk" ] && r=0 || r=1; check "the tree moved on: another key" "$r"
	refused "the old stamp is refused under the new key" F fixtures-check "$work/fx" a
	mkdir -p "$work/exp2"
	printf 'image one new' >"$work/exp2/a.img"
	(cd "$work/exp2" && tar -czf "$work/e2.tgz" ./a.img)
	tar -xzf "$work/e2.tgz" -C "$work/fx"
	ok "a partial re-export stamps what it extracted" F fixtures-stamp "$work/fx" "$work/e2.tgz"
	ok "the re-exported image passes under the new key" F fixtures-check "$work/fx" a
	refused "the image it did not extract keeps its old key and is refused" F fixtures-check "$work/fx" b
	refused "the whole directory is refused while one image is old" F fixtures-check "$work/fx"
	say 'made for fixtures key' "the refusal names the key"
	printf '%s\n' "$fk2" >"$work/fx/.fixtures-key"
	refused "the old one-line (directory) stamp format is refused" F fixtures-check "$work/fx" a
	# the manifest
	F fixtures-stamp "$work/fx" "$work/e1.tgz"
	F fixtures-manifest "$work/fx" a >"$work/man"
	grep -q '^a.img [0-9a-f]\{64\} 13$' "$work/man" && r=0 || r=1; check "the manifest holds name, hash and size" "$r"
	[ "$(F fixtures-manifest "$work/fx" | wc -l | tr -d ' ')" -eq 2 ] && r=0 || r=1; check "the manifest of a directory lists every image" "$r"
	refused "the manifest of a missing image fails" F fixtures-manifest "$work/fx" nothere

	# record: inputs.txt, with the keys read at the start (N2)
	rm -rf "$work/fx"
	mkdir -p "$work/fx"
	tar -xzf "$work/e1.tgz" -C "$work/fx"
	F fixtures-stamp "$work/fx" "$work/e1.tgz"
	refused "snapshot --deps fails when the deps key cannot be computed (fake tree)" F snapshot "$work/snapx" --deps
	[ ! -e "$work/snapx" ] && r=0 || r=1; check "a failed snapshot writes nothing" "$r"
	ok "snapshot takes the deps key now" R snapshot "$work/snap1" --deps
	ok "snapshot takes the fixtures key now" F snapshot "$work/snap2f" --fixtures
	cat "$work/snap1" "$work/snap2f" >"$work/snap"
	ok "record with the sealed tar and the fixtures" F record "$work/rec/inputs.txt" --snapshot "$work/snap" --deps-tar "$work/sealed.tar" --fixtures "$work/fx" --names a
	grep -q "^deps_tar_sha256=$want\$" "$work/rec/inputs.txt" && r=0 || r=1; check "inputs.txt holds the deps tar hash" "$r"
	grep -q "^deps_tar_key=$k\$" "$work/rec/inputs.txt" && r=0 || r=1; check "inputs.txt holds the deps key" "$r"
	grep -q "^fixtures_key=$fk2\$" "$work/rec/inputs.txt" && r=0 || r=1; check "inputs.txt holds the fixtures key" "$r"
	grep -q '^fixtures_manifest_sha256=[0-9a-f]\{64\}$' "$work/rec/inputs.txt" && r=0 || r=1; check "inputs.txt holds the manifest hash" "$r"
	grep -q '^fixture.a.img=' "$work/rec/inputs.txt" && r=0 || r=1; check "inputs.txt lists the image used" "$r"
	grep -q 'b.img' "$work/rec/inputs.txt" && r=1 || r=0; check "only the images named are in the manifest" "$r"
	: >"$work/meta.txt"
	grep -E '^(deps_tar|fixtures?[_.])' "$work/rec/inputs.txt" >>"$work/meta.txt"
	grep -q "^deps_tar_sha256=$want\$" "$work/meta.txt" && grep -q '^fixtures_manifest_sha256=' "$work/meta.txt" && r=0 || r=1
	check "the line filter the suites use puts the hashes into meta.txt" "$r"
	# the tree changes after the snapshot: the record still uses the keys of the start and does not read the tree again
	echo "late" >"$work/repo/src/s.c"
	ok "record after the tree became dirty still passes with the snapshot" F record "$work/rec6/inputs.txt" --snapshot "$work/snap" --fixtures "$work/fx" --names a
	refused "record without a snapshot reads the dirty tree (another key than the stamp) and refuses" F record "$work/rec7/inputs.txt" --fixtures "$work/fx" --names a
	# a dirty run (ci-local --allow-dirty): the start record gets the keys appended, the images are stamped with the dirty key
	printf 'git_sha=%s\ndirty=true\n' "$ones" >"$work/prov.txt"
	ok "snapshot appends to an existing start record on a dirty tree" F snapshot "$work/prov.txt" --fixtures
	head -n 2 "$work/prov.txt" | grep -q '^dirty=true$' && grep -q '^fixtures_dirty=1$' "$work/prov.txt" && r=0 || r=1
	check "the earlier lines stay and fixtures_dirty=1 is recorded" "$r"
	F snapshot "$work/prov.txt" --fixtures
	[ "$(grep -c '^fixtures_key=' "$work/prov.txt")" -eq 2 ] && r=0 || r=1; check "a second snapshot appends again (readers take the last)" "$r"
	mkdir -p "$work/fxd"
	tar -xzf "$work/e1.tgz" -C "$work/fxd"
	ok "a generator run on the dirty tree stamps with the dirty key" F fixtures-stamp "$work/fxd" "$work/e1.tgz"
	ok "the dirty-stamped images pass on the same dirty tree" F fixtures-check "$work/fxd" a b
	ok "record on the dirty tree with its own snapshot passes" F record "$work/rec10/inputs.txt" --snapshot "$work/prov.txt" --fixtures "$work/fxd" --names a
	grep -q '^fixtures_dirty=1$' "$work/rec10/inputs.txt" && r=0 || r=1; check "inputs.txt says fixtures_dirty=1" "$r"
	refused "dirty-stamped images are refused once the tree is clean again" sh -c '(cd "$2" && git checkout -q -- src/s.c); RUN_INPUTS_ROOT="$2" sh "$1" fixtures-check "$3" a' sh "$ri" "$work/repo" "$work/fxd"
	refused "clean-stamped images (fx) are refused for a dirty key" sh -c 'echo late2 >"$2/src/s.c"; RUN_INPUTS_ROOT="$2" sh "$1" fixtures-check "$3" a' sh "$ri" "$work/repo" "$work/fx"
	(cd "$work/repo" && git checkout -q -- src/s.c)
	refused "record with a snapshot lacking the fixtures key refuses" sh -c 'RUN_INPUTS_ROOT="$3" sh "$1" snapshot "$2" --deps; RUN_INPUTS_ROOT="$3" sh "$1" record "$4" --snapshot "$2" --fixtures "$5" --names a' sh "$ri" "$work/snap2" "$repo" "$work/rec8/inputs.txt" "$work/fx"
	refused "record refuses fixtures stamped with another key" sh -c 'sed "s/ [0-9a-f]\{64\}\$/ $2/" "$3/.fixtures-key" >"$3/k.new" && mv "$3/k.new" "$3/.fixtures-key"; RUN_INPUTS_ROOT="$4" sh "$1" record "$5" --fixtures "$3" --names a' sh "$ri" "$zeros" "$work/fx" "$work/repo" "$work/rec3/inputs.txt"
	[ ! -e "$work/rec3/inputs.txt" ] && r=0 || r=1; check "a refused record writes nothing" "$r"
fi

# ================================================================== record without fixtures
refused "record refuses a deps tar with another key" R record "$work/rec2/inputs.txt" --deps-tar "$work/other.tar"
[ ! -e "$work/rec2/inputs.txt" ] && r=0 || r=1; check "a refused deps record writes nothing" "$r"
ok "record with no tar says none (NTFS-3G built in the guest)" R record "$work/rec4/inputs.txt" --deps-tar "$work/not-made-yet.tar"
grep -q '^deps_tar=none$' "$work/rec4/inputs.txt" && r=0 || r=1; check "inputs.txt says deps_tar=none" "$r"
ok "record --deps-none (perf in Environment A: no guest)" R record "$work/rec9/inputs.txt" --deps-none
grep -q '^deps_tar=n/a' "$work/rec9/inputs.txt" && r=0 || r=1; check "inputs.txt says n/a, not none, without a guest" "$r"
ok "record with a seed tar only records it (the leak suite rebuilds)" R record "$work/rec5/inputs.txt" --deps-tar-seed "$work/other.tar"
grep -q '^deps_tar=other.tar (seed only' "$work/rec5/inputs.txt" && r=0 || r=1; check "a seed is marked as seed only" "$r"

# ================================================================== the result note (tests/harness/inputs.py)
if command -v python3 >/dev/null 2>&1; then
	ok "inputs.py selftest" python3 -B "$repo/tests/harness/inputs.py" selftest
	python3 -B - "$repo" "$work/rec" <<'EOF' && r=0 || r=1
import os, sys
sys.path.insert(0, sys.argv[1] + "/tests/harness")
import inputs
if not os.path.exists(os.path.join(sys.argv[2], "inputs.txt")):
    sys.exit(0)  # the fixtures part was skipped (no git)
cases = inputs.annotate([{"id": "S-X", "status": "pass", "note": "n", "evidence": []}], sys.argv[2])
assert "deps tar sha256" in cases[0]["note"] and "fixtures key" in cases[0]["note"], cases
assert cases[0]["evidence"] == ["inputs.txt"]
EOF
	check "a case note carries the deps tar hash and the fixtures key" "$r"
fi

# ================================================================== B1: --fixtures-unkeyed is for perf alone
# a copy of dmn_vm.sh in a fake tree with a fake tart that only logs: it shows how far a call gets
v=$work/vt
mkdir -p "$v/tests/integration/daemon" "$v/vm/macos" "$v/scripts" "$v/third_party/cache" "$v/bld/src/daemon" "$v/bld/src/cli" "$v/fx"
cp "$repo/tests/integration/daemon/dmn_vm.sh" "$v/tests/integration/daemon/"
cp "$repo/scripts/run-inputs.sh" "$v/scripts/"
printf '#!/bin/sh\necho "tart.sh $*" >>"%s/tart.log"\nexit 3\n' "$work" >"$v/vm/macos/tart.sh"
echo 'ntfs-3g v http://example.invalid/f.tgz 00' >"$v/third_party/deps.lock"
chmod +x "$v/vm/macos/tart.sh"
: >"$v/third_party/cache/f.tgz"
: >"$v/bld/src/daemon/ntfsmacd"
: >"$v/bld/src/cli/ntfsmac"
printf 'img' >"$v/fx/perf-ntfs.img"
printf 'img' >"$v/fx/clean-64m-4k.img"
dv() { sh "$v/tests/integration/daemon/dmn_vm.sh" --build-dir "$v/bld" --fixtures "$v/fx" "$@"; }
: >"$work/tart.log"
_rc=0; dv perf >"$work/out" 2>&1 || _rc=$?
[ "$_rc" -eq 2 ] && [ ! -s "$work/tart.log" ] && r=0 || r=1; check "perf without --fixtures-unkeyed and an unstamped directory is refused before any guest (exit $_rc)" "$r"
say 'fixtures refused' "the refusal is the fixtures one"
_rc=0; dv --fixtures-unkeyed perf >"$work/out" 2>&1 || _rc=$?
[ "$_rc" -eq 3 ] && grep -q 'clone' "$work/tart.log" && r=0 || r=1; check "perf with --fixtures-unkeyed gets past the fixtures gate (the fake tart is reached, exit $_rc)" "$r"
: >"$work/tart.log"
_rc=0; dv --fixtures-unkeyed kill >"$work/out" 2>&1 || _rc=$?
[ "$_rc" -eq 2 ] && [ ! -s "$work/tart.log" ] && r=0 || r=1; check "--fixtures-unkeyed with the suite kill is refused (exit $_rc)" "$r"
say 'only for the suite perf' "the refusal says it is perf only"
_rc=0; dv --fixtures-unkeyed perf kill >"$work/out" 2>&1 || _rc=$?
[ "$_rc" -eq 2 ] && [ ! -s "$work/tart.log" ] && r=0 || r=1; check "--fixtures-unkeyed with perf and another suite is refused (exit $_rc)" "$r"
_rc=0; dv --fixtures-unkeyed >"$work/out" 2>&1 || _rc=$?
[ "$_rc" -eq 2 ] && [ ! -s "$work/tart.log" ] && r=0 || r=1; check "--fixtures-unkeyed with the default suites is refused (exit $_rc)" "$r"
# nobody else can reach the flag
users=$(cd "$repo" && grep -rl --include='*.sh' --include='*.py' -e '--fixtures-unkeyed' scripts tests vm 2>/dev/null | grep -v 'tests/unit/ci/test_run_inputs.sh' | LC_ALL=C sort | tr '\n' ' ')
[ "$users" = "tests/integration/daemon/dmn_vm.sh tests/perf/run.sh " ] && r=0 || r=1; check "only dmn_vm.sh and tests/perf/run.sh mention --fixtures-unkeyed ($users)" "$r"
grep -q -e '--fixtures "$tmp/fx" --fixtures-unkeyed' "$repo/tests/perf/run.sh" && r=0 || r=1; check "perf passes the flag with its own directory" "$r"

# ================================================================== wiring
for f in tests/integration/daemon/dmn_vm.sh tests/integration/daemon/attach_vm.sh tests/integration/packaging/pkg_vm.sh \
	tests/stress/soak/mix_host.sh tests/stress/disconnect/run.sh tests/stress/kill/run.sh tests/perf/run.sh; do
	grep -q 'run-inputs.sh" deps-check' "$repo/$f" && r=0 || r=1; check "$f checks the deps tar before anything starts" "$r"
done
for f in tests/integration/daemon/dmn_vm.sh tests/integration/daemon/attach_vm.sh tests/integration/packaging/pkg_vm.sh; do
	grep -q 'run-inputs.sh" deps-seal' "$repo/$f" && r=0 || r=1; check "$f seals a tar it saves" "$r"
	grep -q 'run-inputs.sh" deps-key' "$repo/$f" && r=0 || r=1; check "$f reads the deps key before the tree is copied" "$r"
done
grep -q 'env -u CFLAGS -u LDFLAGS sh ~/ntfsmac/pkg/tests' "$repo/tests/integration/packaging/pkg_vm.sh" && r=0 || r=1
check "pkg_vm.sh runs its guest build with CFLAGS and LDFLAGS unset" "$r"
grep -q -e '--exclude inputs.txt' "$repo/tests/stress/leaks/run.sh" && grep -q 'cmp -s "$out/inputs.txt"' "$repo/tests/stress/leaks/run.sh" && r=0 || r=1
check "the leak suite keeps the guest output from replacing inputs.txt (exclude and compare)" "$r"
grep -q 'cmp -s "$out/provenance.txt"' "$repo/tests/stress/leaks/run.sh" && r=0 || r=1
check "the leak suite keeps the guest output from replacing provenance.txt, which holds the input keys" "$r"
for f in tests/stress/soak/mix_host.sh tests/stress/disconnect/run.sh tests/stress/kill/run.sh tests/stress/leaks/run.sh; do
	grep -q 'snapshot "$out/provenance.txt"' "$repo/$f" && r=0 || r=1; check "$f appends the input keys to provenance.txt (one start record)" "$r"
done
grep -q 'layout pkg' "$repo/tests/integration/packaging/pkg_vm.sh" && grep -q 'layout dmn' "$repo/tests/integration/daemon/dmn_vm.sh" && r=0 || r=1
check "pkg_vm.sh and dmn_vm.sh use different layouts" "$r"
for f in tests/integration/daemon/dmn_vm.sh tests/integration/daemon/attach_vm.sh; do
	grep -q 'run-inputs.sh" fixtures-check' "$repo/$f" && r=0 || r=1; check "$f checks the fixtures stamp" "$r"
	grep -q 'env -u CFLAGS -u LDFLAGS sh scripts/build-ntfs3g.sh' "$repo/$f" && r=0 || r=1; check "$f builds with CFLAGS and LDFLAGS unset (as the key assumes)" "$r"
done
for f in tests/stress/soak/mix_host.sh tests/stress/disconnect/run.sh tests/stress/kill/run.sh tests/stress/leaks/run.sh tests/perf/run.sh; do
	grep -q 'run-inputs.sh" record' "$repo/$f" && r=0 || r=1; check "$f records inputs.txt" "$r"
	grep -q 'run-inputs.sh" snapshot' "$repo/$f" && r=0 || r=1; check "$f takes its keys at the start" "$r"
done
for f in tests/stress/soak/mix_host.sh tests/stress/disconnect/run.sh tests/stress/leaks/run.sh; do
	grep -q "grep -E '^(deps_tar|fixtures?\[_.\])'" "$repo/$f" && r=0 || r=1; check "$f copies the hashes into meta.txt" "$r"
done
grep -q 'run-inputs.sh" record' "$repo/scripts/test-all.sh" && r=0 || r=1; check "test-all.sh records the manifest of reused fixtures" "$r"
for g in clean labels mirror names unsafe vmk; do
	grep -q 'run-inputs.sh" fixtures-stamp "$export_dir" "$tgz"' "$repo/tests/fixtures/gen/$g.sh" && r=0 || r=1; check "gen/$g.sh stamps the images of its export" "$r"
done
for f in tests/stress/soak/mix_result.py tests/stress/disconnect/disc_result.py tests/stress/kill/result.py tests/stress/leaks/classify.py tests/perf/result.py; do
	grep -q 'inputs.annotate' "$repo/$f" && r=0 || r=1; check "$f puts the inputs into the case notes" "$r"
done

if [ "$fails" -eq 0 ]; then echo "run-inputs tests: all passed"; exit 0; fi
echo "run-inputs tests: $fails failed" >&2
exit 1
