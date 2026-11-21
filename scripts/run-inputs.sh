#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# run-inputs.sh - tie the pre-built inputs of a VM run to the commit under test (audit M5-F07).
#
# A run may reuse a deps tar (--deps-tar: the guest's NTFS-3G prefix, instead of building it) and a fixtures
# directory (--fixtures DIR: image files, instead of generating them). Neither says what made it. This script
# gives both a key, refuses an input whose key differs from the checkout's (fail closed: a missing,
# unreadable or malformed key refuses too, no fallback to "build anyway", no override), and records what was
# used.
#
# What a key covers, exactly:
#   deps key      scripts/build-ntfs3g.sh --print-key --fuse external: the pinned tarball digest (deps.lock), the
#                 patch series (names and contents), the fuse mode and the configure flags without the paths.
#                 CFLAGS and LDFLAGS are NOT part of it: the guest builds run with them unset
#                 (`env -u CFLAGS -u LDFLAGS`) and the key is computed as for unset ones. NOT covered: the
#                 golden image (macOS SDK, macFUSE headers, clang): a tar made on another golden image still passes.
#                 No cheap identity of the tart image exists without starting a VM, so none is included.
#                 The key does not depend on the install prefix; the seal therefore also names the layout the
#                 tar was built for (dmn = ~/ntfsmac/deps, pkg = ~/ntfsmac/pkg/build/deps): a tar made by
#                 pkg_vm.sh is refused by dmn_vm.sh and the other way round.
#   fixtures key  everything the generators read or run (full input set, checked against tests/fixtures/gen/*.sh):
#                   tests/fixtures      the generators, their helpers (*_make.py) and manifest.json
#                   tests/tools         treedump, setvolflags, logfile_mark, txf_make, hiber_make.sh (built or run by them)
#                   tests/stress/crashsweep   build.sh and the fault layer (mirror.sh, vmk.sh)
#                   src, include        the product sources they compile (src/sanitize, src/common/label_cp.c, src/volmark ...)
#                   third_party/patches, third_party/deps.lock, scripts/build-ntfs3g.sh   the engine they mount with
#                   vm/linux            the container they run in (Environment A: Dockerfile, run.sh)
#                   CMakeLists.txt, cmake   vmk.sh configures and builds the project with cmake
#                 On a clean tree the key is the SHA-256 of the HEAD tree/blob hashes of those paths. On an unclean tree
#                 (uncommitted or untracked files in them) it is the SHA-256 of "dirty" and the path and content hash of every
#                 file `git ls-files -c -o --exclude-standard` lists there, and `fixtures_dirty=1` is recorded: such a run
#                 goes on and its result is marked dirty by the provenance of the suite (the gate rejects it); a dirty key
#                 never equals a clean one. Refused (no key) outside a git checkout. NOT covered: the Docker image content
#                 beyond what vm/linux builds from, the host, and files read from outside those paths.
#
# Where a key lives: in the artifact, and it is checked where the artifact is used.
#   deps tar      member deps/.ntfsmac-deps-key, one line "KEY LAYOUT"; every member must be under deps/
#   fixtures dir  DIR/.fixtures-key, one line "NAME.img SHA256 KEY" per image the generators exported (a partial
#                 re-export stamps only what it extracted); a check names the images a run uses: each must be
#                 listed with the current key and with the SHA-256 of the file as it is now
#
# Why the key sits in the tar and not "seed, then always run build-ntfs3g.sh" (the leak suite's rule): the
# deps tars of the daemon suites hold only the prefix (no build tree, no stamp), so a rebuild after seeding would
# be a full build and the tar would save nothing. The leak suite keeps its rule (its tar holds the build tree)
# and records the seed hash only.
#
# Usage: scripts/run-inputs.sh deps-key
#        scripts/run-inputs.sh deps-seal TAR KEY [LAYOUT]    append the key (computed by the caller BEFORE the
#                                                            inputs were copied to the guest) to an uncompressed tar
#        scripts/run-inputs.sh deps-check TAR [--key K] [--layout L]   (default: current key, layout dmn); every member under
#                                                     deps/, no symlink or hardlink that points outside deps/ or is absolute
#        scripts/run-inputs.sh deps-info TAR                 deps_tar_sha256=, deps_tar_key= (or "none")
#        scripts/run-inputs.sh fixtures-key                  the key, and: fixtures-dirty prints 1 on an unclean tree, else 0
#        scripts/run-inputs.sh fixtures-stamp DIR TGZ [KEY]  add a line for every top-level *.img in TGZ (the export)
#        scripts/run-inputs.sh fixtures-check DIR [--key K] [NAME...]   (default: every *.img in DIR)
#        scripts/run-inputs.sh fixtures-manifest DIR [NAME...]   "NAME.img sha256 bytes" lines
#        scripts/run-inputs.sh snapshot FILE [--deps] [--fixtures]   APPEND the keys (deps_key=, fixtures_key=, fixtures_dirty=) to
#                                                     FILE now (the start record of a run, e.g. provenance.txt)
#        scripts/run-inputs.sh record OUT [--snapshot FILE] [--deps-tar TAR] [--deps-tar-seed TAR]
#                                         [--deps-none] [--fixtures DIR [--names "N N"]]
#                                                     --deps-none: no guest (perf in Environment A): "n/a", not "none"
#                                                     check, then write key=value lines to OUT (inputs.txt). With
#                                                     --snapshot the checks use the keys read at the start of the run,
#                                                     and the working tree is not read again. --deps-tar: a missing
#                                                     file is "none" (built in the guest), an existing one must pass.
#                                                     --deps-tar-seed: recorded only (the build re-runs).
# Exit:  0 ok, 1 refused or failed, 2 usage error.
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=${RUN_INPUTS_ROOT:-$(dirname "$here")}
dkey=.ntfsmac-deps-key
fkey=.fixtures-key
fix_paths="tests/fixtures tests/tools tests/stress/crashsweep src include third_party/patches third_party/deps.lock vm/linux scripts/build-ntfs3g.sh CMakeLists.txt cmake"

die() { echo "run-inputs: $*" >&2; exit 1; }
usage_err() { echo "run-inputs: $*" >&2; echo "usage: run-inputs.sh deps-key | deps-seal TAR KEY [LAYOUT] | deps-check TAR [--key K] [--layout L] | deps-info TAR | fixtures-key | fixtures-dirty | fixtures-stamp DIR TGZ [KEY] | fixtures-check DIR [--key K] [NAME...] | fixtures-manifest DIR [NAME...] | snapshot FILE [--deps] [--fixtures] | record OUT [...]" >&2; exit 2; }

if command -v sha256sum >/dev/null 2>&1; then
	sha256_stdin() { sha256sum | cut -d' ' -f1; }
elif command -v shasum >/dev/null 2>&1; then
	sha256_stdin() { shasum -a 256 | cut -d' ' -f1; }
else
	die "need sha256sum or shasum"
fi
sha256_file() { sha256_stdin <"$1"; }

is_key() { case $1 in *[!0-9a-f]* | '') return 1 ;; esac; [ ${#1} -eq 64 ]; }
short() { printf '%s' "${1%"${1#????????????}"}"; }

deps_key() { sh "$root/scripts/build-ntfs3g.sh" --print-key --fuse external; }

fixtures_dirty() { # prints 1 when the tree has uncommitted or untracked files under the input paths, else 0
	(
		cd "$root"
		git rev-parse --git-dir >/dev/null 2>&1 || { echo "run-inputs: no git checkout in $root: the fixtures key needs git" >&2; exit 1; }
		# shellcheck disable=SC2086
		d=$(git status --porcelain --untracked-files=all -- $fix_paths 2>/dev/null) || { echo "run-inputs: git status failed" >&2; exit 1; }
		if [ -n "$d" ]; then echo 1; else echo 0; fi
	)
}

fixtures_key() {
	(
		cd "$root"
		dirty=$(fixtures_dirty) || exit 1
		lines=
		if [ "$dirty" = 0 ]; then
			for p in $fix_paths; do
				h=$(git rev-parse "HEAD:$p" 2>/dev/null) || { echo "run-inputs: $p is not in HEAD" >&2; exit 1; }
				lines="$lines$p $h
"
			done
		else
			# content of the files as they are: tracked (a deleted one counts as missing) and untracked, not ignored
			# shellcheck disable=SC2086
			list=$(git ls-files -c -o --exclude-standard -- $fix_paths | LC_ALL=C sort -u) || { echo "run-inputs: git ls-files failed" >&2; exit 1; }
			lines="dirty
"
			for f in $list; do
				if [ -f "$f" ]; then h=$(sha256_file "$f"); else h=missing; fi
				lines="$lines$f $h
"
			done
		fi
		printf '%s' "$lines" | sha256_stdin
	)
}

# --- deps tars
deps_stored() { # TAR: print "KEY LAYOUT", or say why not and return 1
	[ -f "$1" ] && [ -r "$1" ] || { echo "run-inputs: deps tar $1 is missing or not readable" >&2; return 1; }
	_list=$(tar -tf "$1" 2>/dev/null) || { echo "run-inputs: deps tar $1 is unreadable (not an uncompressed tar?)" >&2; return 1; }
	# every member under deps/: nothing else may ride along (absolute paths, .., other directories)
	_bad=$(printf '%s\n' "$_list" | awk '$0 != "deps" && $0 != "deps/" && substr($0, 1, 5) != "deps/" { print; exit } /(^|\/)\.\.(\/|$)/ { print; exit }')
	[ -z "$_bad" ] || { echo "run-inputs: deps tar $1 has a member outside deps/: $_bad" >&2; return 1; }
	# a link must stay inside deps/: no absolute target, no target that climbs out of it
	_vl=$(tar -tvf "$1" 2>/dev/null | awk '
		function inside(base, tgt,    n, i, parts, out, k) {
			if (substr(tgt, 1, 1) == "/") return 0
			n = split(base "/" tgt, parts, "/"); k = 0
			for (i = 1; i <= n; i++) {
				if (parts[i] == "" || parts[i] == ".") continue
				if (parts[i] == "..") { if (k == 0) return 0; k--; continue }
				out[++k] = parts[i]
			}
			return k >= 1 && out[1] == "deps"
		}
		{
			name = ""; tgt = ""
			if (match($0, / -> /)) { tgt = substr($0, RSTART + 4); name = substr($0, 1, RSTART - 1) }
			else if (match($0, / link to /)) { tgt = substr($0, RSTART + 9); name = substr($0, 1, RSTART - 1) }
			else next
			n = split(name, f, " "); name = f[n]
			sub(/\/[^\/]*$/, "", name)
			if (match($0, / link to /)) { if (!inside("", tgt)) { print $0; exit } }
			else if (!inside(name, tgt)) { print $0; exit }
		}')
	[ -z "$_vl" ] || { echo "run-inputs: deps tar $1 has a link that points outside deps/ or is absolute: $_vl" >&2; return 1; }
	_raw=$(tar -xOf "$1" "deps/$dkey" 2>/dev/null) || { echo "run-inputs: deps tar $1 has no deps/$dkey (made before the key existed, or by another tool)" >&2; return 1; }
	case $_raw in *'
'*) echo "run-inputs: $1 holds more than one key line" >&2; return 1 ;; esac
	set -- $_raw
	[ $# -eq 2 ] && is_key "$1" || { echo "run-inputs: the key in the tar is malformed" >&2; return 1; }
	printf '%s %s\n' "$1" "$2"
}

deps_check() { # TAR [KEY LAYOUT]
	_t=$1
	_want=${2:-}
	_lay=${3:-dmn}
	[ -n "$_want" ] || _want=$(deps_key) || return 1
	is_key "$_want" || { echo "run-inputs: the expected key is malformed" >&2; return 1; }
	_st=$(deps_stored "$_t") || return 1
	set -- $_st
	if [ "$1" != "$_want" ]; then
		echo "run-inputs: REFUSED: $_t was made for deps key $(short "$1"), this checkout has $(short "$_want") (rebuild it; there is no override)" >&2
		return 1
	fi
	if [ "$2" != "$_lay" ]; then
		echo "run-inputs: REFUSED: $_t was built for the $2 layout, this run needs $_lay (a pkg_vm tar is not a dmn_vm tar)" >&2
		return 1
	fi
}

# --- fixtures
stamp_line() { # DIR NAME.img: the stamp line of an image, or nothing
	[ -f "$1/$fkey" ] || return 0
	awk -v n="$2" '$1 == n' "$1/$fkey"
}

fixtures_check() { # DIR KEY NAME...   (NAME without .img; none = every image)
	_d=$1
	_want=$2
	shift 2
	[ -d "$_d" ] || { echo "run-inputs: fixtures directory $_d is missing" >&2; return 1; }
	[ -f "$_d/$fkey" ] && [ -r "$_d/$fkey" ] || {
		echo "run-inputs: REFUSED: $_d has no $fkey (not made by the generators of this checkout, or before the stamp existed)" >&2
		return 1
	}
	[ -n "$_want" ] || _want=$(fixtures_key) || return 1
	is_key "$_want" || { echo "run-inputs: the expected key is malformed" >&2; return 1; }
	if [ $# -eq 0 ]; then
		set -- $(cd "$_d" && ls | sed -n 's/\.img$//p' | LC_ALL=C sort)
		[ $# -gt 0 ] || { echo "run-inputs: no *.img files in $_d" >&2; return 1; }
	fi
	for _n in "$@"; do
		[ -f "$_d/$_n.img" ] || { echo "run-inputs: REFUSED: $_d/$_n.img is missing" >&2; return 1; }
		_l=$(stamp_line "$_d" "$_n.img")
		case $_l in '' | *'
'*) echo "run-inputs: REFUSED: $_n.img is not listed exactly once in $_d/$fkey" >&2; return 1 ;; esac
		set -- $_l
		[ $# -eq 3 ] && is_key "$2" && is_key "$3" || { echo "run-inputs: REFUSED: the stamp line of $_n.img is malformed" >&2; return 1; }
		[ "$3" = "$_want" ] || {
			echo "run-inputs: REFUSED: $_n.img was made for fixtures key $(short "$3"), this checkout has $(short "$_want") (regenerate it)" >&2
			return 1
		}
		[ "$2" = "$(sha256_file "$_d/$_n.img")" ] || {
			echo "run-inputs: REFUSED: $_n.img changed after it was stamped (sha256 differs)" >&2
			return 1
		}
	done
}

manifest() { # DIR [NAME...]
	_d=$1
	shift
	[ -d "$_d" ] || die "fixtures directory $_d is missing"
	if [ $# -eq 0 ]; then
		set -- $(cd "$_d" && ls | sed -n 's/\.img$//p' | LC_ALL=C sort)
		[ $# -gt 0 ] || die "no *.img files in $_d"
	fi
	for _n in "$@"; do
		[ -f "$_d/$_n.img" ] || die "$_d/$_n.img is missing"
		printf '%s.img %s %s\n' "$_n" "$(sha256_file "$_d/$_n.img")" "$(wc -c <"$_d/$_n.img" | tr -d ' ')"
	done
}

cmd=${1:-}
[ -n "$cmd" ] || usage_err "a command is required"
shift
case $cmd in
deps-key) [ $# -eq 0 ] || usage_err "deps-key takes no argument"; deps_key ;;
fixtures-key) [ $# -eq 0 ] || usage_err "fixtures-key takes no argument"; fixtures_key ;;
fixtures-dirty) [ $# -eq 0 ] || usage_err "fixtures-dirty takes no argument"; fixtures_dirty ;;
deps-seal)
	[ $# -ge 2 ] && [ $# -le 3 ] || usage_err "deps-seal needs a tar, a key and optionally a layout"
	tar_f=$1
	k=$2
	lay=${3:-dmn}
	is_key "$k" || die "the key to seal is malformed"
	case $lay in dmn | pkg) ;; *) usage_err "the layout is dmn or pkg" ;; esac
	[ -f "$tar_f" ] || die "$tar_f is missing"
	# never append to a tar that has a key already: it would hold two and be refused from then on
	if tar -tf "$tar_f" 2>/dev/null | grep -qx "deps/$dkey"; then die "$tar_f already holds a key; it is not sealed again"; fi
	t=$(mktemp -d "${TMPDIR:-/tmp}/run-inputs.XXXXXX")
	mkdir -p "$t/deps"
	printf '%s %s\n' "$k" "$lay" >"$t/deps/$dkey"
	# an uncompressed tar can be appended to; a compressed one cannot, and is refused here rather than left unsealed
	if ! (cd "$t" && tar -rf "$tar_f" "deps/$dkey") 2>/dev/null; then
		rm -rf "$t"
		die "could not append the key to $tar_f (not an uncompressed tar?)"
	fi
	rm -rf "$t"
	deps_check "$tar_f" "$k" "$lay" || exit 1
	;;
deps-check)
	[ $# -ge 1 ] || usage_err "deps-check needs a tar"
	t=$1
	shift
	k= lay=dmn
	while [ $# -gt 0 ]; do
		case $1 in
		--key) [ $# -ge 2 ] || usage_err "--key needs a value"; k=$2; shift 2 ;;
		--layout) [ $# -ge 2 ] || usage_err "--layout needs a value"; lay=$2; shift 2 ;;
		*) usage_err "unknown option: $1" ;;
		esac
	done
	deps_check "$t" "$k" "$lay" || exit 1
	;;
deps-info)
	[ $# -eq 1 ] || usage_err "deps-info needs a tar"
	[ -f "$1" ] || die "$1 is missing"
	echo "deps_tar_sha256=$(sha256_file "$1")"
	st=$(deps_stored "$1" 2>/dev/null) && echo "deps_tar_key=${st%% *}" || echo "deps_tar_key=none"
	;;
fixtures-stamp)
	[ $# -ge 2 ] && [ $# -le 3 ] || usage_err "fixtures-stamp needs a directory, the export tgz and optionally a key"
	d=$1
	tgz=$2
	[ -d "$d" ] || die "$d is missing"
	[ -f "$tgz" ] || die "$tgz is missing"
	k=${3:-}
	[ -n "$k" ] || k=$(fixtures_key) || exit 1
	is_key "$k" || die "the key to stamp is malformed"
	names=$(tar -tzf "$tgz" | sed 's|^\./||' | grep -E '^[^/]+\.img$' || true)
	buf=$(mktemp "${TMPDIR:-/tmp}/run-inputs.XXXXXX")
	[ ! -f "$d/$fkey" ] || cp "$d/$fkey" "$buf"
	for n in $names; do
		[ -f "$d/$n" ] || { rm -f "$buf"; die "$d/$n was not extracted"; }
		# replace only the lines of the images this export extracted; the others keep the key they were made with
		awk -v n="$n" '$1 != n' "$buf" >"$buf.new"
		printf '%s %s %s\n' "$n" "$(sha256_file "$d/$n")" "$k" >>"$buf.new"
		mv "$buf.new" "$buf"
	done
	cp "$buf" "$d/$fkey"
	rm -f "$buf"
	;;
fixtures-check)
	[ $# -ge 1 ] || usage_err "fixtures-check needs a directory"
	d=$1
	shift
	k=
	if [ "${1:-}" = --key ]; then [ $# -ge 2 ] || usage_err "--key needs a value"; k=$2; shift 2; fi
	fixtures_check "$d" "$k" "$@" || exit 1
	;;
fixtures-manifest) [ $# -ge 1 ] || usage_err "fixtures-manifest needs a directory"; manifest "$@" ;;
snapshot)
	[ $# -ge 1 ] || usage_err "snapshot needs a file"
	out=$1
	shift
	want_d=0 want_f=0
	for a in "$@"; do
		case $a in --deps) want_d=1 ;; --fixtures) want_f=1 ;; *) usage_err "unknown option: $a" ;; esac
	done
	buf=$(mktemp "${TMPDIR:-/tmp}/run-inputs.XXXXXX")
	if [ "$want_d" -eq 1 ]; then
		dk=$(deps_key) || { rm -f "$buf"; exit 1; }
		echo "deps_key=$dk" >>"$buf"
	fi
	if [ "$want_f" -eq 1 ]; then
		fk=$(fixtures_key) || { rm -f "$buf"; exit 1; }
		fd=$(fixtures_dirty) || { rm -f "$buf"; exit 1; }
		echo "fixtures_key=$fk" >>"$buf"
		echo "fixtures_dirty=$fd" >>"$buf"
	fi
	mkdir -p "$(dirname "$out")"
	cat "$buf" >>"$out"
	rm -f "$buf"
	;;
record)
	[ $# -ge 1 ] || usage_err "record needs an output file"
	out=$1
	shift
	tar_f= seed_f= fx= names= snap= nodeps=0
	while [ $# -gt 0 ]; do
		case $1 in
		--snapshot) [ $# -ge 2 ] || usage_err "--snapshot needs a file"; snap=$2; shift 2 ;;
		--deps-tar) [ $# -ge 2 ] || usage_err "--deps-tar needs a file"; tar_f=$2; shift 2 ;;
		--deps-none) nodeps=1; shift ;;
		--deps-tar-seed) [ $# -ge 2 ] || usage_err "--deps-tar-seed needs a file"; seed_f=$2; shift 2 ;;
		--fixtures) [ $# -ge 2 ] || usage_err "--fixtures needs a directory"; fx=$2; shift 2 ;;
		--names) [ $# -ge 2 ] || usage_err "--names needs a list"; names=$2; shift 2 ;;
		*) usage_err "unknown option: $1" ;;
		esac
	done
	buf=$(mktemp "${TMPDIR:-/tmp}/run-inputs.XXXXXX")
	trap 'rm -f "$buf"' EXIT
	snap_get() { # NAME: the key read at the start of the run; refuse when it was not taken
		[ -f "$snap" ] || die "snapshot $snap is missing"
		v=$(sed -n "s/^$1=//p" "$snap" | tail -n 1)
		is_key "$v" || die "the snapshot has no $1 (take it with snapshot ... --deps/--fixtures at the start of the run)"
		printf '%s\n' "$v"
	}
	if [ -n "$tar_f" ] && [ -f "$tar_f" ]; then
		wk=
		[ -z "$snap" ] || wk=$(snap_get deps_key)
		deps_check "$tar_f" "$wk" dmn || exit 1
		{ echo "deps_tar=${tar_f##*/}"; sh "$0" deps-info "$tar_f"; } >>"$buf"
	elif [ -n "$seed_f" ] && [ -f "$seed_f" ]; then
		{ echo "deps_tar=${seed_f##*/} (seed only: the build re-runs)"; sh "$0" deps-info "$seed_f"; } >>"$buf"
	elif [ "$nodeps" -eq 1 ]; then
		echo "deps_tar=n/a (no guest in this environment)" >>"$buf"
	else
		echo "deps_tar=none" >>"$buf"
	fi
	if [ -n "$fx" ]; then
		if [ -n "$snap" ]; then
			fk=$(snap_get fixtures_key)
			fd=$(sed -n 's/^fixtures_dirty=//p' "$snap" | tail -n 1)
		else
			fk=$(fixtures_key) || exit 1
			fd=$(fixtures_dirty) || exit 1
		fi
		case $fd in 0 | 1) ;; *) die "no fixtures_dirty in the snapshot" ;; esac
		# shellcheck disable=SC2086
		fixtures_check "$fx" "$fk" $names || exit 1
		echo "fixtures_dir=$fx" >>"$buf"
		echo "fixtures_key=$fk" >>"$buf"
		echo "fixtures_dirty=$fd" >>"$buf"
		# shellcheck disable=SC2086
		m=$(manifest "$fx" $names) || exit 1
		printf '%s\n' "$m" | while read -r n h b; do echo "fixture.$n=$h ($b bytes)"; done >>"$buf"
		echo "fixtures_manifest_sha256=$(printf '%s\n' "$m" | sha256_stdin)" >>"$buf"
	fi
	mkdir -p "$(dirname "$out")"
	cp "$buf" "$out"
	;;
*) usage_err "unknown command: $cmd" ;;
esac
