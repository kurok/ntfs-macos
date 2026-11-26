#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Developer installer (T-M4-10; docs/ARCHITECTURE.md section 10). Builds the product (CMake preset
# `product`: no test seams, check 4 of ADR-019 as the root-peer fallback) and NTFS-3G, installs
# every file of packaging/manifest.txt with the owner, group and mode the manifest gives, records
# the code requirement of the CLI (ADR-019 check 3), installs the authorization right
# org.ntfs-macos.journal-modify, and loads the launchd job org.ntfs-macos.ntfsmacd. packaging/
# uninstall.sh removes all of it again.
#
# Run it as your own user, not as root: the build must not leave root-owned files in the checkout;
# the install steps call sudo. Run it only on a Mac you mean to install on (this changes
# /usr/local, /Library and the launchd system domain). It installs over an earlier install: the
# job is booted out first and the files are replaced.
#
# The CLI is signed ad hoc with the hardened runtime, and the daemon accepts the privileged
# requests only from that exact build: the file /Library/Application Support/ntfs-macos/
# cli-requirement holds `cdhash H"<hash>"`. Installing a rebuilt CLI by hand without recording its
# new hash makes `journal-clear` and `repair` fail; run this script again instead.
#
# Prerequisites: macOS 27 on arm64, macFUSE 5.5.0 or newer (the worker links its libfuse), cmake,
# a C compiler. Ninja is used if present (the preset needs it); otherwise the same options are
# given to the Unix Makefiles generator.
#
# Usage: packaging/dev-install.sh [--deps DIR] [--build-dir DIR] [--jobs N] [--no-build] [--no-load]
#                                 [--job-arg ARG]... [--allow-seams]
#   --deps DIR       NTFS-3G install prefix (default build/deps); built there if it has no worker
#   --build-dir DIR  CMake build tree (default build/product)
#   --jobs N         parallel jobs (default 4)
#   --no-build       use the existing build tree and deps prefix as they are (still refused if the
#                    build is not a product build)
#   --no-load        install the files, the right and the plist but do not load the job
#   --job-arg ARG    add ARG to the daemon's arguments in the installed plist, after the program
#                    path (for example --automount-mode=ro); repeatable; the plist is then not the
#                    shipped file byte for byte
#   --allow-seams    for the VM test drivers only: accept a build tree with test seams (a daemon
#                    with --console-uid, --child-log, --journal-dir, no sandbox switch...), which
#                    the default refuses. The result is not a product install.
# Exit: 0 installed, 1 error, 2 usage error.
set -eu

# Privileged steps and checks use the system tools only. The build runs with the caller's PATH
# (cmake, ninja and pkg-config usually live in /opt/homebrew/bin).
user_path=$PATH
PATH=/usr/bin:/bin:/usr/sbin:/sbin
export PATH

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)

die() { printf 'dev-install: error: %s\n' "$1" >&2; exit "${2:-1}"; }
log() { printf 'dev-install: %s\n' "$*"; }
usage() { printf 'usage: %s [--deps DIR] [--build-dir DIR] [--jobs N] [--no-build] [--no-load] [--job-arg ARG] [--allow-seams]\n' "$0" >&2; }

deps=$root/build/deps
build=$root/build/product
jobs=4
do_build=1
do_load=1
allow_seams=0
job_args=
manifest=$here/manifest.txt
job=org.ntfs-macos.ntfsmacd
right=org.ntfs-macos.journal-modify
plist=/Library/LaunchDaemons/$job.plist
sock=/var/run/ntfsmacd.sock

while [ $# -gt 0 ]; do
	case "$1" in
	--deps) [ $# -ge 2 ] || { usage; die "--deps needs a directory" 2; }; deps=$2; shift ;;
	--build-dir) [ $# -ge 2 ] || { usage; die "--build-dir needs a directory" 2; }; build=$2; shift ;;
	--jobs) [ $# -ge 2 ] || { usage; die "--jobs needs a number" 2; }; jobs=$2; shift ;;
	--no-build) do_build=0 ;;
	--no-load) do_load=0 ;;
	--allow-seams) allow_seams=1 ;;
	--job-arg)
		[ $# -ge 2 ] || { usage; die "--job-arg needs a value" 2; }
		case $2 in '' | *[!A-Za-z0-9._=/:,@+-]*) die "--job-arg: only letters, digits and . _ = / : , @ + - are accepted: $2" 2 ;; esac
		job_args="$job_args$2
"
		shift
		;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" 2 ;;
	esac
	shift
done
case $jobs in '' | *[!0-9]*) die "--jobs needs a number" 2 ;; esac
case $deps$build in *[[:space:]]*) die "paths with whitespace are not supported" ;; esac

stage=$(mktemp -d "${TMPDIR:-/tmp}/dev-install.XXXXXX")
trap 'rm -rf "$stage"' EXIT
trap 'exit 130' INT TERM HUP

# ---- preflight ----
[ "$(uname -s)" = Darwin ] || die "this installs on macOS only"
[ "$(uname -m)" = arm64 ] || die "the product target is arm64 (ARCHITECTURE.md section 10)"
major=$(sw_vers -productVersion | sed 's/\..*//')
[ "$major" -ge 27 ] 2>/dev/null || die "macOS 27 or newer is required (this is $(sw_vers -productVersion))"
for t in codesign install launchctl security plutil sudo awk sed strings stat env mktemp; do
	command -v "$t" >/dev/null 2>&1 || die "missing tool: $t"
done
if [ "$do_build" -eq 1 ]; then
	(PATH=$user_path; command -v cmake >/dev/null 2>&1) || die "missing tool: cmake"
fi
if [ "$(id -u)" -eq 0 ] && [ "$do_build" -eq 1 ]; then
	die "run as your own user, not as root (the install steps call sudo); --no-build allows root"
fi
[ -f "$manifest" ] || die "manifest not found: $manifest"

priv() { if [ "$(id -u)" -eq 0 ]; then "$@"; else sudo "$@"; fi; }
[ "$(id -u)" -eq 0 ] || sudo -n true 2>/dev/null || sudo -v || die "sudo is needed for the install"

# ---- the paths the manifest may name: the same list as uninstall.sh ----
allowed() { # kind path
	case $1 in
	dir | file)
		case $2 in
		/usr/local/libexec/ntfs-macos | /usr/local/libexec/ntfs-macos/*) return 0 ;;
		"/Library/Application Support/ntfs-macos" | "/Library/Application Support/ntfs-macos/"*) return 0 ;;
		"/Library/LaunchDaemons/$job.plist") [ "$1" = file ] && return 0 ;;
		esac
		;;
	link) [ "$2" = /usr/local/bin/ntfsmac ] && return 0 ;;
	statedir) [ "$2" = /var/db/ntfs-macos ] && return 0 ;;
	statefile) [ "$2" = /var/log/ntfsmacd.log ] && return 0 ;;
	right) [ "$2" = "$right" ] && return 0 ;;
	esac
	return 1
}

# >>> root-only check (M6-F01). The same text in packaging/scripts/preinstall,
# packaging/scripts/postinstall, packaging/uninstall.sh and packaging/dev-install.sh: a package
# script cannot source a file of the repository, so each carries a copy, and the unit test
# packaging.root_only (tests/unit/packaging/root_only_test.sh) keeps the copies equal.
#
# root_only_chain PATH: 0 when every component of the absolute PATH that exists, from / down to
# PATH itself, is a real directory owned by root, not writable by group or others and without an
# access control entry that allows anything (`ls -lde` shows "allow"; a deny-only list is fine,
# audit M6 review). PATH must not hold an empty, "." or ".." component. A symbolic link is never
# followed: a link at any component fails, so the directory that holds a link is
# always checked as well (a user-owned /usr/local with /usr/local/libexec a link to a root-only
# directory fails at /usr/local). The components below the first missing one do not exist yet,
# which is allowed. On failure ro_bad is the component that failed and ro_why the reason.
root_only_chain() {
	ro_bad=
	ro_why=
	case $1 in
	/*) ;;
	*) ro_bad=$1; ro_why='is not an absolute path'; return 1 ;;
	esac
	if [ "$1" != / ]; then
		case "$1/" in
		*/../* | */./* | *//*) ro_bad=$1; ro_why='has an empty component or a component that is a dot or two dots'; return 1 ;;
		esac
	fi
	ro_rest=${1#/}
	ro_p=/
	while :; do
		if [ -L "$ro_p" ]; then ro_bad=$ro_p; ro_why='is a symbolic link'; return 1; fi
		[ -e "$ro_p" ] || return 0
		if [ ! -d "$ro_p" ]; then ro_bad=$ro_p; ro_why='is not a directory'; return 1; fi
		ro_st=$(/usr/bin/stat -f '%Su %Lp' "$ro_p") || { ro_bad=$ro_p; ro_why='cannot be read'; return 1; }
		if [ "${ro_st% *}" != root ] || [ $((0${ro_st#* } & 022)) -ne 0 ]; then
			ro_bad=$ro_p
			ro_why="is owned by ${ro_st% *} with mode ${ro_st#* }"
			return 1
		fi
		ro_acl=$(/bin/ls -lde "$ro_p") || { ro_bad=$ro_p; ro_why='cannot be read'; return 1; }
		case $ro_acl in
		*"
"*" allow "*)
			ro_bad=$ro_p
			ro_why='has an access control entry that allows changes to it (ls -le shows it)'
			return 1
			;;
		esac
		[ -n "$ro_rest" ] || return 0
		ro_c=${ro_rest%%/*}
		case $ro_rest in */*) ro_rest=${ro_rest#*/} ;; *) ro_rest= ;; esac
		case $ro_c in '' | '.' | '..') ro_bad=$1; ro_why='has an empty component or a component that is a dot or two dots'; return 1 ;; esac
		if [ "$ro_p" = / ]; then ro_p=/$ro_c; else ro_p=$ro_p/$ro_c; fi
	done
}
# root_only_dir DIR: 0 when DIR exists and root_only_chain accepts it (DIR included)
root_only_dir() {
	ro_bad=
	ro_why=
	[ -d "$1" ] || { ro_bad=$1; ro_why='is missing'; return 1; }
	root_only_chain "$1"
}
# ro_show PATH: PATH for a message, in double quotes, for reading only. Each character other than
# a letter, a digit, a space or / . _ , : @ + = - is shown as ?, so a name that a user chose
# cannot end the quotes or add a command or a line to the log.
ro_show() {
	/usr/bin/printf '"%s"' "$(/usr/bin/printf '%s' "$1" | LC_ALL=C /usr/bin/tr -c 'A-Za-z0-9 /._,:@+=-' '?')"
}
# <<< root-only check

entries=
nl='
'
n=0
while read -r kind mode owner group path || [ -n "$kind" ]; do
	n=$((n + 1))
	case $kind in '' | '#'*) continue ;; esac
	[ -n "$path" ] || die "$manifest:$n: malformed entry"
	case $kind in dir | file | statedir | statefile | right | link) ;; *) die "$manifest:$n: unknown entry type: $kind" ;; esac
	[ "$kind" = right ] || case $path in /*) ;; *) die "$manifest:$n: path is not absolute: $path" ;; esac
	case $path in *..* | *//* | */ | *[[:cntrl:]]*) die "$manifest:$n: refusing path: $path" ;; esac
	allowed "$kind" "$path" || die "$manifest:$n: refusing $kind outside the paths of ntfs-macos: $path"
	case $kind in
	dir | file)
		# no setuid, setgid or sticky bit (first digit 0), root:wheel only
		case $mode in 0[0-7][0-7][0-7]) ;; *) die "$manifest:$n: bad mode (setuid, setgid and sticky are refused): $mode" ;; esac
		{ [ "$owner" = root ] && [ "$group" = wheel ]; } || die "$manifest:$n: owner and group must be root wheel: $path"
		;;
	esac
	# Root runs or reads every dir and file: every directory above it must be a real directory that
	# only root can change, walked from / down without following a symbolic link (M6-F01). A
	# user-owned /usr/local/libexec (an old Homebrew, a `chown -R`), or a link there in a user-owned
	# /usr/local, would let a user process rename the directory and plant a binary. The optional
	# link is checked when it is made (step 6).
	case $kind in
	right | link) ;;
	*)
		an=${path%/*}
		# /var is the system's own link to private/var (the state entries): checked at its real path
		case $an in
		/var | /var/*)
			if [ "$(readlink /var)" = private/var ]; then an=/private$an; fi
			;;
		esac
		if ! root_only_chain "$an"; then
			die "the directory $(ro_show "$ro_bad") above $(ro_show "$path") $ro_why. Every directory above a path that this installs must be a real directory (not a symbolic link), owned by root and not writable by group or others, or a user could plant a program that runs as root. Replace a symbolic link by a real directory, or give the directory to root and remove the write permission of group and others, then run this again."
		fi
		;;
	esac
	entries="$entries$kind $mode $owner $group $path$nl"
done <"$manifest"

# ---- 1. NTFS-3G (worker and ntfsprogs) ----
if [ "$do_build" -eq 1 ] && [ ! -x "$deps/bin/ntfs-3g" ]; then
	log "building NTFS-3G (external FUSE) into $deps"
	env PATH="$user_path" sh "$root/scripts/build-ntfs3g.sh" --fuse external --jobs "$jobs" --prefix "$deps" --build-dir "$root/build/ntfs-3g-dev"
fi
[ -x "$deps/bin/ntfs-3g" ] || die "no NTFS-3G in $deps (run without --no-build)"

# ---- 2. the product build: no test seams ----
if [ "$do_build" -eq 1 ]; then
	log "configuring the product build in $build"
	if (PATH=$user_path; command -v ninja >/dev/null 2>&1); then
		env PATH="$user_path" PKG_CONFIG_PATH="$deps/lib/pkgconfig" cmake -S "$root" --preset product -B "$build"
	else
		# the options of the `product` preset in CMakePresets.json
		env PATH="$user_path" PKG_CONFIG_PATH="$deps/lib/pkgconfig" cmake -S "$root" -B "$build" -G "Unix Makefiles" \
			-DCMAKE_BUILD_TYPE=Release -DNTFSMAC_TEST_SEAMS=OFF -DNTFSMAC_AUTHZ_FALLBACK_ROOT=ON
	fi
	log "building"
	env PATH="$user_path" cmake --build "$build" --parallel "$jobs" --target ntfsmacd ntfsmac ntfsmac-probe ntfsmac-volmark
fi
if [ "$allow_seams" -eq 0 ]; then
	for v in NTFSMAC_TEST_SEAMS NTFSMAC_AUTHZ_FALLBACK_ROOT; do
		want=OFF
		[ "$v" = NTFSMAC_AUTHZ_FALLBACK_ROOT ] && want=ON
		grep -q "^$v:BOOL=$want\$" "$build/CMakeCache.txt" 2>/dev/null || die "$build is not a product build ($v is not $want); use the preset product"
	done
else
	log "WARNING: --allow-seams: this is a test install, not a product install"
fi
bin_daemon=$build/src/daemon/ntfsmacd
bin_cli=$build/src/cli/ntfsmac
bin_probe=$build/src/probe/ntfsmac-probe
bin_volmark=$build/src/volmark/ntfsmac-volmark
for f in "$bin_daemon" "$bin_cli" "$bin_probe" "$bin_volmark"; do
	[ -x "$f" ] || die "not built: $f"
done
# Two checks that do not trust the cache file: the strings of the binary, and its own usage text
# (the test seams add options to the usage line; `--bogus` makes the daemon print it and exit 2
# before it does anything else).
if [ "$allow_seams" -eq 0 ]; then
	strings "$bin_daemon" >"$stage/daemon.strings" || die "strings failed on $bin_daemon"
	[ -s "$stage/daemon.strings" ] || die "no strings in $bin_daemon"
	if grep -q -e '--console-uid' -e '--child-log' -e '--journal-dir' "$stage/daemon.strings"; then
		die "$bin_daemon contains test seams (--console-uid, --child-log or --journal-dir); build with the preset product"
	fi
	"$bin_daemon" --bogus >"$stage/daemon.usage" 2>&1 || true
	grep -q '^usage: ntfsmacd' "$stage/daemon.usage" || die "$bin_daemon printed no usage line for --bogus; cannot tell whether it has test seams"
	if grep -q -e '--console-uid' -e '--child-log' -e '--journal-dir' -e '--socket' -e '--no-marker' "$stage/daemon.usage"; then
		die "$bin_daemon lists test seams in its usage line; build with the preset product"
	fi
fi

# ---- 3. stage what is generated ----

# The privileged XPC service accepts only the CLI this build produced (ADR-019 check 3): signed ad
# hoc with the hardened runtime, and its cdhash is the recorded requirement.
cp "$bin_cli" "$stage/ntfsmac"
codesign --force -s - -o runtime "$stage/ntfsmac" >/dev/null 2>&1 || die "codesign of the CLI failed"
hash=$(codesign -d -vvv "$stage/ntfsmac" 2>&1 | sed -n 's/^CDHash=//p')
[ -n "$hash" ] || die "no cdhash for the CLI"
printf 'cdhash H"%s"\n' "$hash" >"$stage/cli-requirement"

version=$(sed -n 's/^project(ntfs-macos VERSION \([0-9][0-9.]*\).*/\1/p' "$root/CMakeLists.txt")
ntfs3g_ver=$(awk '$1 == "ntfs-3g" { print $2 }' "$root/third_party/deps.lock")
{
	printf 'install      developer install (packaging/dev-install.sh)\n'
	printf 'version      %s\n' "${version:-unknown}"
	printf 'source       %s\n' "$(git -C "$root" rev-parse HEAD 2>/dev/null || echo unknown)"
	printf 'built        %s\n' "$(date -u +%Y-%m-%dT%H:%M:%SZ)"
	printf 'build-host   macOS %s %s\n' "$(sw_vers -productVersion)" "$(uname -m)"
	printf 'ntfs-3g      %s (patch series third_party/patches/ntfs-3g)\n' "${ntfs3g_ver:-unknown}"
	if [ "$allow_seams" -eq 0 ]; then
		printf 'preset       product (no test seams)\n'
	else
		printf 'preset       test build (--allow-seams)\n'
	fi
	printf 'cli          ad-hoc signature, hardened runtime, cdhash %s\n' "$hash"
} >"$stage/BUILD-INFO"

# the right as ADR-019 and U17 define it: its own definition, class user, group admin, not shared,
# timeout 0 (a pointer to the built-in rule would inherit its timeout)
cat >"$stage/right.plist" <<'EOF'
<?xml version="1.0" encoding="UTF-8"?>
<!DOCTYPE plist PUBLIC "-//Apple//DTD PLIST 1.0//EN" "http://www.apple.com/DTDs/PropertyList-1.0.dtd">
<plist version="1.0"><dict>
	<key>class</key><string>user</string>
	<key>comment</key><string>Clear a session journal entry or repair an NTFS volume with ntfsmac.</string>
	<key>group</key><string>admin</string>
	<key>shared</key><false/>
	<key>timeout</key><integer>0</integer>
	<key>authenticate-user</key><true/>
	<key>version</key><integer>1</integer>
</dict></plist>
EOF

# the job plist: the shipped file, or with the --job-arg values after the program path
job_plist=$root/integration/launchd/$job.plist
if [ -n "$job_args" ]; then
	extra=
	while IFS= read -r a; do
		[ -n "$a" ] && extra="$extra<string>$a</string>"
	done <<EOF
$job_args
EOF
	sed "s|<string>/usr/local/libexec/ntfs-macos/ntfsmacd</string>|&$extra|" "$job_plist" >"$stage/job.plist"
	cmp -s "$job_plist" "$stage/job.plist" && die "could not add the job arguments to the plist"
	job_plist=$stage/job.plist
fi

find_tool() { # name: the NTFS-3G tool in the deps prefix
	for d in "$deps/bin" "$deps/sbin"; do
		[ -x "$d/$1" ] && { printf '%s\n' "$d/$1"; return 0; }
	done
	return 1
}

source_of() { # installed path: where the file comes from
	case $1 in
	/usr/local/libexec/ntfs-macos/ntfsmacd) echo "$bin_daemon" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac-worker) echo "$deps/bin/ntfs-3g" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac-probe) echo "$bin_probe" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac-volmark) echo "$bin_volmark" ;;
	/usr/local/libexec/ntfs-macos/mkntfs | /usr/local/libexec/ntfs-macos/ntfsfix | /usr/local/libexec/ntfs-macos/ntfsinfo | /usr/local/libexec/ntfs-macos/ntfslabel)
		find_tool "${1##*/}" ;;
	/usr/local/libexec/ntfs-macos/uninstall.sh) echo "$here/uninstall.sh" ;;
	/usr/local/libexec/ntfs-macos/manifest.txt) echo "$manifest" ;;
	/usr/local/libexec/ntfs-macos/ntfsmac) echo "$stage/ntfsmac" ;;
	"/Library/Application Support/ntfs-macos/cli-requirement") echo "$stage/cli-requirement" ;;
	"/Library/Application Support/ntfs-macos/sandbox/"*.sb) echo "$root/integration/sandbox/${1##*/}" ;;
	"/Library/LaunchDaemons/$job.plist") echo "$job_plist" ;;
	"/Library/Application Support/ntfs-macos/doc/COPYING") echo "$root/COPYING" ;;
	"/Library/Application Support/ntfs-macos/doc/README.md") echo "$root/README.md" ;;
	"/Library/Application Support/ntfs-macos/doc/BUILD-INFO") echo "$stage/BUILD-INFO" ;;
	*) return 1 ;;
	esac
}

# every file of the manifest must have a source before anything is changed
while read -r kind mode owner group path || [ -n "$kind" ]; do
	[ "$kind" = file ] || continue
	src=$(source_of "$path") || die "the installer has no source for $path (manifest and installer out of step)"
	[ -f "$src" ] || die "source of $path is missing: $src"
done <<EOF
$entries
EOF

# ---- 4. stop an earlier install, then install ----
if launchctl print "system/$job" >/dev/null 2>&1; then
	log "booting out the loaded job $job"
	priv launchctl bootout "system/$job" || die "could not boot out $job"
fi

# Paths of the earlier layout (the CLI as a file in /usr/local/bin, the documents in
# /usr/local/share/doc/ntfs-macos) that the installed manifest of an earlier install lists: removed
# when their directory is root-only, else left with a note (root does not delete in a directory a
# user can change).
old_manifest=/usr/local/libexec/ntfs-macos/manifest.txt
if [ -f "$old_manifest" ] && [ ! -L "$old_manifest" ] && [ "$(stat -f %Su "$old_manifest")" = root ]; then
	for lp in /usr/local/bin/ntfsmac /usr/local/share/doc/ntfs-macos/COPYING /usr/local/share/doc/ntfs-macos/README.md /usr/local/share/doc/ntfs-macos/BUILD-INFO; do
		grep -q -E "^file[[:space:]]+[0-7]+[[:space:]]+root[[:space:]]+wheel[[:space:]]+$lp\$" "$old_manifest" || continue
		{ [ -f "$lp" ] && [ ! -L "$lp" ]; } || continue
		if root_only_dir "${lp%/*}"; then
			priv rm -f "$lp"
			log "removed $lp (earlier layout)"
		else
			log "NOTE: left $lp of the earlier layout: ${lp%/*} is not root-only; remove it yourself"
		fi
	done
	if root_only_dir /usr/local/share/doc/ntfs-macos; then priv rmdir /usr/local/share/doc/ntfs-macos 2>/dev/null || true; fi
fi

log "installing the files of $(basename "$manifest")"
while read -r kind mode owner group path || [ -n "$kind" ]; do
	case $kind in
	dir)
		[ ! -L "$path" ] || die "$path is a symbolic link; remove it first"
		priv install -d -m "$mode" -o "$owner" -g "$group" "$path"
		;;
	file)
		src=$(source_of "$path")
		priv install -m "$mode" -o "$owner" -g "$group" "$src" "$path"
		;;
	esac
done <<EOF
$entries
EOF

# ---- 5. the authorization right (ADR-019) ----
log "installing the authorization right $right"
priv security authorizationdb write "$right" <"$stage/right.plist" >/dev/null 2>&1 || die "could not install the right $right"
priv security authorizationdb read "$right" >"$stage/right.read" 2>/dev/null || die "cannot read back the right $right"
for kv in class=user group=admin shared=false timeout=0; do
	got=$(plutil -extract "${kv%%=*}" raw -o - "$stage/right.read" 2>/dev/null || true)
	[ "$got" = "${kv#*=}" ] || die "the stored right $right has ${kv%%=*}='$got', not '${kv#*=}'"
done
# what the daemon also requires (ctl_sys.c real_right_definition): no `rule`, `allow-root` and
# `session-owner` absent or false, `authenticate-user` absent or true
if plutil -extract rule raw -o - "$stage/right.read" >/dev/null 2>&1; then die "the stored right $right has a rule key"; fi
for kv in allow-root=false session-owner=false authenticate-user=true; do
	got=$(plutil -extract "${kv%%=*}" raw -o - "$stage/right.read" 2>/dev/null || true)
	[ -z "$got" ] || [ "$got" = "${kv#*=}" ] || die "the stored right $right has ${kv%%=*}='$got', not '${kv#*=}'"
done

# ---- 6. the convenience link /usr/local/bin/ntfsmac (manifest kind `link`) ----
link=/usr/local/bin/ntfsmac
target=/usr/local/libexec/ntfs-macos/ntfsmac
if root_only_dir /usr/local/bin; then
	if [ -L "$link" ]; then
		if [ "$(readlink "$link")" = "$target" ]; then
			log "the link $link -> $target is in place"
		else
			log "NOTE: $link points to $(ro_show "$(readlink "$link")"), not to $target; left as it is"
		fi
	elif [ -e "$link" ]; then
		log "NOTE: $link exists and is not ours; left as it is. Run the CLI as $target"
	else
		priv ln -s "$target" "$link"
		log "linked $link -> $target"
	fi
else
	log "NOTE: /usr/local/bin is missing or not root-only (a Homebrew directory?): no link. Run the CLI as $target, or add /usr/local/libexec/ntfs-macos to PATH"
fi

# ---- 7. the launchd job ----
if [ "$do_load" -eq 1 ]; then
	plutil -lint "$plist" >/dev/null || die "the installed plist is not valid"
	log "loading $job"
	priv launchctl bootstrap system "$plist" || die "launchctl bootstrap failed"
	i=0
	while [ ! -S "$sock" ] && [ "$i" -lt 30 ]; do sleep 1; i=$((i + 1)); done
	if [ -S "$sock" ]; then
		log "the daemon is up ($sock)"
	else
		log "WARNING: no control socket after 30 s; see /var/log/ntfsmacd.log"
		priv tail -n 20 /var/log/ntfsmacd.log 2>/dev/null || true
	fi
else
	log "the job is not loaded (--no-load): sudo launchctl bootstrap system $plist"
fi

log "done. Check it with: $target doctor"
log "remove it with:     sudo /usr/local/libexec/ntfs-macos/uninstall.sh"
