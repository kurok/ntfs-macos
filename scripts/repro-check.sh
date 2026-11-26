#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Reproducibility check (T-M6-04, criterion 6.2): two builds of the release package from the same
# clean commit must give byte-identical binaries, or every difference must be listed and explained.
#
# Build mode (default): the commit HEAD (the working tree must be clean) is packed ONCE on the host,
# as scripts/release-local.sh packs it (git archive of the commit's tree with tar.umask=022,
# core.autocrlf=false, core.eol=lf, plus the pinned NTFS-3G tarball of third_party/cache; nothing is
# downloaded), and the same tar is built twice with packaging/build-pkg.sh, each
# build in its own FRESH clone of the golden VM, at the same guest paths (~/ntfsmac/tree,
# ~/ntfsmac/work), with the same label, NTFSMAC_SOURCE_REV and SOURCE_DATE_EPOCH (the commit time).
# The two builds run one after the other in one VM slot (vm/macos/run-suite.sh slot protocol), so
# they are minutes apart. Each clone is deleted when its build is copied back, and on any exit.
# Then both results are compared as in --compare.
#
# Compare mode (--compare A B, host only, no VM): A and B are two output directories of build-pkg.sh
# (ntfs-macos-<label>.pkg, BUILD-INFO, uninstall.sh, manifest.txt, SHA256SUMS). It compares
#   - the files of the directories, the .pkg included (sha256);
#   - the members of the product archive (pkgutil --expand: Distribution, component Bom,
#     PackageInfo, Payload archive, Scripts) and the xar table of contents (xar --dump-toc);
#   - the expanded payload file by file (pkgutil --expand-full): type, mode and sha256; a Mach-O file
#     that differs is compared again without its code signature (codesign --remove-signature on a
#     copy) and with its LC_UUID zeroed, then by its symbol table and strings to name the cause;
#   - the bills of materials (lsbom with and without the modification times);
#   - the package scripts (preinstall, postinstall).
# It prints one row per item (identical or differs) and, for each difference, the classified cause.
#
# Exit: 0 when every difference has a cause in the allow-list below, 1 otherwise (a difference
#       without a cause, or with a cause that is not allowed), 2 usage or setup error.
#
# Usage: scripts/repro-check.sh [--work DIR] [--keep-work] [--allow-dirty]
#        scripts/repro-check.sh --compare DIR_A DIR_B [--work DIR]
#   --work DIR   scratch and result directory (default build/repro); the two builds land in DIR/a
#                and DIR/b, their logs in DIR/build-a.log and DIR/build-b.log
#   --keep-work  keep the expanded packages in DIR/cmp (default: removed after the comparison)
#   --allow-dirty  build the working tree instead of HEAD: the tracked files as they are plus the files
#                not yet tracked and not ignored (as scripts/release-local.sh --allow-dirty sends them);
#                SOURCE_DATE_EPOCH is still the time of HEAD and the label ends in .dirty
# Env: NTFSMAC_VM_SLOT_DIR, NTFSMAC_VM_SLOTS, NTFSMAC_SLOT_WAIT (the slot protocol of run-suite.sh)
set -eu

here=$(CDPATH='' cd -- "$(dirname -- "$0")" && pwd)
root=$(CDPATH='' cd -- "$here/.." && pwd)
tart_sh=$root/vm/macos/tart.sh
# shellcheck source=lib/tree.sh
. "$here/lib/tree.sh"

die() { printf 'repro-check: %s\n' "$1" >&2; exit "${2:-1}"; }
log() { printf '\n== repro-check: %s ==\n' "$*"; }
usage() { printf 'usage: %s [--work DIR] [--keep-work] [--allow-dirty] | --compare DIR_A DIR_B [--work DIR]\n' "$0" >&2; }

# ---- allow-list -------------------------------------------------------------------------------------
# Causes that are accepted. Every other cause (and an unclassified difference) fails the check.
#   xar-toc-times   The product archive is a xar file whose table of contents records, for each member,
#                   the inode, device, atime, mtime, ctime and Finder creation time of the temporary
#                   file productbuild read, and the archive's creation time. pkgbuild and productbuild
#                   take these from their own scratch files and have no option to fix them. The rule
#                   holds only when every member of the archive is byte-identical and the tables of
#                   contents are equal once those fields are removed (the heap checksum at the start
#                   of the file is the checksum of the table and follows it).
#   sums-follow-pkg SHA256SUMS differs only in the line of the .pkg, and the .pkg itself differs only
#                   for allowed causes.
# Causes that are named but NOT allowed (build-pkg.sh removes them; they come back only if that
# regresses):
#   bom-mtime       Bom equal apart from the modification times (payload mtimes not clamped to
#                   SOURCE_DATE_EPOCH).
#   payload-mtime   the Payload or Scripts archive differs, its expanded files and the Bom without
#                   times are equal (cpio header mtimes, same fix).
#   buildinfo-date  BUILD-INFO differs only in its date line (SOURCE_DATE_EPOCH not passed).
#   macho-signature the Mach-O code is equal without the code signature.
#   macho-uuid      the Mach-O code is equal once LC_UUID is zeroed (a random linker UUID).
#   macho-oso-mtime only the debug-map (N_OSO) timestamps differ, and the UUID and signature that
#                   follow them (ZERO_AR_DATE=1 not in effect): the symbol tables are equal with the
#                   N_OSO values masked, and every segment but __LINKEDIT is byte-identical.
#   macho-path      the strings differ in an absolute path (a build directory that differs).
#   date-string     a text file, or the strings of a Mach-O, differ only in what looks like a date.
#   macho-code      any other Mach-O difference; binary-content: a differing file that is neither
#                   Mach-O nor text; content: any other text difference.
allowed_causes='xar-toc-times sums-follow-pkg'

work=
keep_work=0
allow_dirty=0
cmp_a=
cmp_b=
mode=build
while [ $# -gt 0 ]; do
	case "$1" in
	--work) [ $# -ge 2 ] || { usage; die "--work needs a value" 2; }; work=$2; shift ;;
	--keep-work) keep_work=1 ;;
	--allow-dirty) allow_dirty=1 ;;
	--compare)
		[ $# -ge 3 ] || { usage; die "--compare needs two directories" 2; }
		mode=compare cmp_a=$2 cmp_b=$3; shift 2 ;;
	-h | --help) usage; exit 0 ;;
	*) usage; die "unknown option: $1" 2 ;;
	esac
	shift
done
[ "$(uname -s)" = Darwin ] || die "needs macOS (pkgutil, xar, lsbom, codesign)" 2
for t in pkgutil xar lsbom codesign otool nm strings shasum file perl cmp; do
	command -v "$t" >/dev/null 2>&1 || die "missing tool: $t" 2
done
[ -n "$work" ] || work=$root/build/repro
mkdir -p "$work"
work=$(CDPATH='' cd -- "$work" && pwd)

# ---- comparison ---------------------------------------------------------------------------------------
sha() { shasum -a 256 "$1" | cut -c1-64; }
is_macho() { file -b "$1" | grep -q '^Mach-O'; }
is_allowed() { case " $allowed_causes " in *" $1 "*) return 0 ;; esac; return 1; }

rows=
n_same=0 n_diff=0 n_bad=0
# row ITEM [CAUSE]: no cause = identical; a cause ends in "(allowed)" or "(NOT allowed)"
row() {
	if [ $# -lt 2 ]; then
		printf '%-66s %-9s %s\n' "$1" identical - >>"$rows"
		n_same=$((n_same + 1))
		return
	fi
	n_diff=$((n_diff + 1))
	if is_allowed "$2"; then
		printf '%-66s %-9s %s (allowed)\n' "$1" differs "$2" >>"$rows"
	else
		printf '%-66s %-9s %s (NOT allowed)\n' "$1" differs "$2" >>"$rows"
		n_bad=$((n_bad + 1))
	fi
	[ -z "${3:-}" ] || printf '%s\n' "$3" | sed 's/^/      | /' >>"$rows"
}

# date_only FILE_A FILE_B: 0 when the files differ only in lines whose difference is a date or time
date_only() {
	diff "$1" "$2" | grep '^[<>]' | sed -E \
		-e 's/[0-9]{4}-[0-9]{2}-[0-9]{2}([T ][0-9]{2}:[0-9]{2}(:[0-9]{2})?(Z|[+-][0-9]{2}:?[0-9]{2})?)?/<DATE>/g' \
		-e 's/(Jan|Feb|Mar|Apr|May|Jun|Jul|Aug|Sep|Oct|Nov|Dec) +[0-9]{1,2} +[0-9]{4}/<DATE>/g' \
		-e 's/[0-9]{2}:[0-9]{2}:[0-9]{2}/<TIME>/g' -e 's/^[<>] //' | sort | uniq -c | awk '$1 % 2 { bad = 1 } END { exit bad }'
}

# zero_uuid FILE: zero the 16 bytes of LC_UUID of a thin 64-bit Mach-O (in place)
zero_uuid() {
	perl -e '
		open(my $f, "+<", $ARGV[0]) or die "open: $!\n"; binmode $f;
		read($f, my $h, 32) == 32 or die "short\n";
		my ($magic, undef, undef, undef, $n) = unpack("V5", $h);
		$magic == 0xfeedfacf or exit 3;
		my $o = 32;
		for (1 .. $n) {
			seek($f, $o, 0); read($f, my $lc, 8);
			my ($cmd, $size) = unpack("V2", $lc);
			if ($cmd == 0x1b) { seek($f, $o + 8, 0); print $f "\0" x 16; }
			$o += $size;
		}' "$1"
}

# segs_equal A B: 0 when the two thin 64-bit Mach-O files have the same segments and every segment
# other than __LINKEDIT (symbol table, strings, signature) is byte-identical
segs_equal() {
	perl -e '
		sub segs {
			open(my $f, "<", $_[0]) or die "open: $!\n"; binmode $f;
			local $/; my $d = <$f>;
			my ($magic, undef, undef, undef, $n) = unpack("V5", $d);
			$magic == 0xfeedfacf or exit 3;
			my ($o, @r) = (32);
			for (1 .. $n) {
				my ($cmd, $size) = unpack("V2", substr($d, $o, 8));
				if ($cmd == 0x19) {
					my $name = unpack("Z16", substr($d, $o + 8, 16));
					my (undef, undef, $off, $len) = unpack("Q<4", substr($d, $o + 24, 32));
					push @r, $name . "=" . ($name eq "__LINKEDIT" ? "" : substr($d, $off, $len));
				}
				$o += $size;
			}
			return join("\0", @r);
		}
		exit(segs($ARGV[0]) eq segs($ARGV[1]) ? 0 : 1);' "$1" "$2"
}

# macho_cause A B TMP: classify two differing Mach-O files; prints "cause|detail"
macho_cause() {
	mc_a=$3/a.bin mc_b=$3/b.bin
	cp "$1" "$mc_a"; cp "$2" "$mc_b"; chmod u+w "$mc_a" "$mc_b"
	mc_detail="uuid $(otool -l "$1" | awk '$1 == "uuid" { print $2 }') / $(otool -l "$2" | awk '$1 == "uuid" { print $2 }')"
	codesign --remove-signature "$mc_a" 2>/dev/null || true
	codesign --remove-signature "$mc_b" 2>/dev/null || true
	if cmp -s "$mc_a" "$mc_b"; then echo "macho-signature|$mc_detail"; return; fi
	zero_uuid "$mc_a" && zero_uuid "$mc_b" || true
	if cmp -s "$mc_a" "$mc_b"; then echo "macho-uuid|$mc_detail"; return; fi
	# the symbol tables with the N_OSO values (the object file mtimes) masked
	nm -ap "$1" >"$3/nm.a" 2>/dev/null || true
	nm -ap "$2" >"$3/nm.b" 2>/dev/null || true
	sed -E 's/^[0-9a-f]+( - [0-9a-f]+ [0-9a-f]+ +OSO )/0\1/' "$3/nm.a" >"$3/nm.ma"
	sed -E 's/^[0-9a-f]+( - [0-9a-f]+ [0-9a-f]+ +OSO )/0\1/' "$3/nm.b" >"$3/nm.mb"
	mc_segs=differ
	segs_equal "$mc_a" "$mc_b" && mc_segs=equal
	if ! cmp -s "$3/nm.a" "$3/nm.b" && cmp -s "$3/nm.ma" "$3/nm.mb" && [ "$mc_segs" = equal ]; then
		echo "macho-oso-mtime|$mc_detail; $(diff "$3/nm.a" "$3/nm.b" | grep -c '^<') OSO entries differ"
		return
	fi
	strings -a "$1" | sort >"$3/s.a"
	strings -a "$2" | sort >"$3/s.b"
	if ! cmp -s "$3/s.a" "$3/s.b"; then
		if diff "$3/s.a" "$3/s.b" | grep '^[<>]' | grep -q '^[<>] /'; then
			echo "macho-path|$mc_detail; $(diff "$3/s.a" "$3/s.b" | grep '^[<>] /' | head -n 2 | tr '\n' ' ')"
			return
		fi
		if date_only "$3/s.a" "$3/s.b"; then echo "date-string|$mc_detail"; return; fi
	fi
	mc_n=$(cmp -l "$mc_a" "$mc_b" 2>/dev/null | wc -l | tr -d ' ')
	echo "macho-code|$mc_detail; $mc_n bytes differ without signature and UUID; segments other than __LINKEDIT $mc_segs"
}

# text_cause A B: classify two differing non-Mach-O files (a binary one is binary-content)
text_cause() {
	if { [ -s "$1" ] && ! grep -Iq . "$1"; } || { [ -s "$2" ] && ! grep -Iq . "$2"; }; then
		echo "binary-content|$(cmp "$1" "$2" 2>/dev/null | head -n 1); $(cmp -l "$1" "$2" 2>/dev/null | wc -l | tr -d ' ') bytes differ"
		return
	fi
	case ${1##*/} in
	BUILD-INFO)
		if [ "$(diff "$1" "$2" | grep '^[<>]' | grep -cv '^[<>] \(built\|source-date\) ')" -eq 0 ]; then
			echo "buildinfo-date|$(diff "$1" "$2" | grep '^[<>]' | tr '\n' ' ')"
			return
		fi ;;
	esac
	if date_only "$1" "$2"; then echo "date-string|"; return; fi
	echo "content|$(diff "$1" "$2" 2>/dev/null | grep '^[<>]' | head -n 4 | tr '\n' ' ')"
}

# toc_norm PKG OUT: the xar table of contents without the fields of the allow rule xar-toc-times
toc_norm() {
	xar --dump-toc="$2.raw" -f "$1" >/dev/null 2>&1 || die "xar cannot read $1"
	sed -E -e '/<(creation-time|atime|mtime|ctime|inode|deviceno|time|nanoseconds)>/d' \
		-e '/<\/?FinderCreateTime>/d' "$2.raw" >"$2"
}

compare() { # DIR_A DIR_B
	ca=$1 cb=$2
	[ -d "$ca" ] && [ -d "$cb" ] || die "not a directory: $ca or $cb" 2
	pa=$(cd "$ca" && ls ntfs-macos-*.pkg 2>/dev/null || true)
	pb=$(cd "$cb" && ls ntfs-macos-*.pkg 2>/dev/null || true)
	[ -n "$pa" ] && [ "$(printf '%s\n' "$pa" | wc -l)" -eq 1 ] || die "$ca holds not exactly one ntfs-macos-*.pkg" 2
	[ "$pa" = "$pb" ] || die "the packages have different names: $pa, $pb" 2
	cmpd=$work/cmp
	rm -rf "$cmpd"
	mkdir -p "$cmpd/tmp"
	rows=$cmpd/rows
	: >"$rows"
	pkg_ok=1 # 1 while every difference inside the package is allowed

	# 1. the members of the product archive (Scripts comes as a directory)
	pkgutil --expand "$ca/$pa" "$cmpd/xa" || die "pkgutil --expand failed for $ca/$pa"
	pkgutil --expand "$cb/$pb" "$cmpd/xb" || die "pkgutil --expand failed for $cb/$pb"
	pkgutil --expand-full "$ca/$pa" "$cmpd/fa" || die "pkgutil --expand-full failed for $ca/$pa"
	pkgutil --expand-full "$cb/$pb" "$cmpd/fb" || die "pkgutil --expand-full failed for $cb/$pb"
	comp=$(cd "$cmpd/xa" && find . -maxdepth 1 -name "*.pkg" | sort | head -n 1)
	comp=${comp#./}
	[ -n "$comp" ] || die "no component package in $pa"

	members_same=1
	(cd "$cmpd/xa" && find . \( -type f -o -type l \) ! -name '._*' | sort) >"$cmpd/members.a"
	(cd "$cmpd/xb" && find . \( -type f -o -type l \) ! -name '._*' | sort) >"$cmpd/members.b"
	sort -u "$cmpd/members.a" "$cmpd/members.b" >"$cmpd/members"
	while IFS= read -r m; do
		m=${m#./}
		item="pkg:$m"
		if [ ! -e "$cmpd/xa/$m" ] || [ ! -e "$cmpd/xb/$m" ]; then
			row "$item" only-in-one "present in $([ -e "$cmpd/xa/$m" ] && echo A || echo B) only"
			members_same=0 pkg_ok=0
			continue
		fi
		if cmp -s "$cmpd/xa/$m" "$cmpd/xb/$m"; then row "$item"; continue; fi
		members_same=0 pkg_ok=0
		case $m in
		*/Bom)
			lsbom -p fmugsc "$cmpd/xa/$m" >"$cmpd/tmp/bom.a"; lsbom -p fmugsc "$cmpd/xb/$m" >"$cmpd/tmp/bom.b"
			if cmp -s "$cmpd/tmp/bom.a" "$cmpd/tmp/bom.b"; then
				row "$item" bom-mtime "$(lsbom -p ft "$cmpd/xa/$m" | head -n 1) / $(lsbom -p ft "$cmpd/xb/$m" | head -n 1)"
			else
				row "$item" bom-content "$(diff "$cmpd/tmp/bom.a" "$cmpd/tmp/bom.b" | grep '^[<>]' | head -n 4)"
			fi ;;
		*/Payload)
			lsbom -p fmugsc "$cmpd/xa/$comp/Bom" >"$cmpd/tmp/bom.a"; lsbom -p fmugsc "$cmpd/xb/$comp/Bom" >"$cmpd/tmp/bom.b"
			if cmp -s "$cmpd/tmp/bom.a" "$cmpd/tmp/bom.b" &&
				diff -r "$cmpd/fa/$comp/Payload" "$cmpd/fb/$comp/Payload" >/dev/null 2>&1; then
				row "$item" payload-mtime "expanded files and Bom without times are equal"
			else
				row "$item" payload-content "see the payload rows"
			fi ;;
		*)
			if is_macho "$cmpd/xa/$m"; then c=$(macho_cause "$cmpd/xa/$m" "$cmpd/xb/$m" "$cmpd/tmp")
			else c=$(text_cause "$cmpd/xa/$m" "$cmpd/xb/$m"); fi
			row "$item" "${c%%|*}" "${c#*|}" ;;
		esac
	done <"$cmpd/members"

	# 2. the expanded payload, file by file
	(cd "$cmpd/fa/$comp/Payload" && find . ! -name '._*' | sort) >"$cmpd/payload.a"
	(cd "$cmpd/fb/$comp/Payload" && find . ! -name '._*' | sort) >"$cmpd/payload.b"
	sort -u "$cmpd/payload.a" "$cmpd/payload.b" >"$cmpd/payload"
	while IFS= read -r p; do
		[ "$p" != . ] || continue
		fa=$cmpd/fa/$comp/Payload/${p#./} fb=$cmpd/fb/$comp/Payload/${p#./}
		item="payload:${p#.}"
		if { [ ! -e "$fa" ] && [ ! -L "$fa" ]; } || { [ ! -e "$fb" ] && [ ! -L "$fb" ]; }; then
			row "$item" only-in-one "present in $([ -e "$fa" ] && echo A || echo B) only"; pkg_ok=0; continue
		fi
		ma=$(stat -f '%HT %Lp' "$fa") mb=$(stat -f '%HT %Lp' "$fb")
		if [ "$ma" != "$mb" ]; then row "$item" mode "$ma / $mb"; pkg_ok=0; continue; fi
		[ -f "$fa" ] && [ ! -L "$fa" ] || continue # directories and links: type and mode only
		if cmp -s "$fa" "$fb"; then row "$item"; continue; fi
		pkg_ok=0
		if is_macho "$fa"; then c=$(macho_cause "$fa" "$fb" "$cmpd/tmp")
		else c=$(text_cause "$fa" "$fb"); fi
		row "$item" "${c%%|*}" "sha256 $(sha "$fa" | cut -c1-16) / $(sha "$fb" | cut -c1-16); ${c#*|}"
	done <"$cmpd/payload"

	# 3. the xar table of contents and the .pkg file itself
	toc_norm "$ca/$pa" "$cmpd/toc.a"
	toc_norm "$cb/$pb" "$cmpd/toc.b"
	toc_cause=
	if cmp -s "$cmpd/toc.a.raw" "$cmpd/toc.b.raw"; then
		row "pkg:xar table of contents"
	elif cmp -s "$cmpd/toc.a" "$cmpd/toc.b"; then
		toc_cause=xar-toc-times
		row "pkg:xar table of contents" xar-toc-times \
			"$(diff "$cmpd/toc.a.raw" "$cmpd/toc.b.raw" | grep '^[<>]' | sed -E 's/^([<>]) *<([a-zA-Z-]+)>.*/\2/' | sort | uniq -c | tr -s ' ' | tr '\n' ';')"
	else
		row "pkg:xar table of contents" toc-content "$(diff "$cmpd/toc.a" "$cmpd/toc.b" | grep '^[<>]' | head -n 4)"
		pkg_ok=0
	fi
	if cmp -s "$ca/$pa" "$cb/$pb"; then
		row "dist:$pa"
	elif [ "$pkg_ok" -eq 1 ] && [ "$members_same" -eq 1 ] && [ "$toc_cause" = xar-toc-times ]; then
		# allowed only when the table of contents itself differs only in its times
		row "dist:$pa" xar-toc-times "sha256 $(sha "$ca/$pa" | cut -c1-16) / $(sha "$cb/$pb" | cut -c1-16); every member equal"
	else
		row "dist:$pa" pkg-content "sha256 $(sha "$ca/$pa" | cut -c1-16) / $(sha "$cb/$pb" | cut -c1-16); see the rows above"
	fi

	# 4. the other files of the output directories
	for f in BUILD-INFO uninstall.sh manifest.txt SHA256SUMS; do
		if [ ! -f "$ca/$f" ] || [ ! -f "$cb/$f" ]; then row "dist:$f" only-in-one "missing in A or B"; continue; fi
		if cmp -s "$ca/$f" "$cb/$f"; then row "dist:$f"; continue; fi
		if [ "$f" = SHA256SUMS ]; then
			if [ "$(diff "$ca/$f" "$cb/$f" | grep '^[<>]' | grep -cv " $pa\$")" -eq 0 ] &&
				grep -q "^dist:$pa .*identical\|^dist:$pa .*(allowed)" "$rows"; then
				row "dist:$f" sums-follow-pkg "only the line of $pa"
			else
				row "dist:$f" content "$(diff "$ca/$f" "$cb/$f" | grep '^[<>]' | head -n 4)"
			fi
			continue
		fi
		c=$(text_cause "$ca/$f" "$cb/$f")
		row "dist:$f" "${c%%|*}" "${c#*|}"
	done

	printf '%-66s %-9s %s\n' ITEM RESULT CAUSE
	cat "$rows"
	printf '\nrepro-check: %s identical, %s differ, %s of them without an allowed cause\n' "$n_same" "$n_diff" "$n_bad"
	[ "$keep_work" -eq 1 ] || rm -rf "$cmpd"
	if [ "$n_bad" -eq 0 ]; then
		echo "repro-check: PASSED (every difference has an allowed cause: $allowed_causes)"
		return 0
	fi
	echo "repro-check: FAILED"
	return 1
}

if [ "$mode" = compare ]; then
	case $cmp_a in /*) ;; *) cmp_a=$(pwd)/$cmp_a ;; esac
	case $cmp_b in /*) ;; *) cmp_b=$(pwd)/$cmp_b ;; esac
	compare "$cmp_a" "$cmp_b"
	exit $?
fi

# ---- build mode ---------------------------------------------------------------------------------------
cd "$root"
git rev-parse --git-dir >/dev/null 2>&1 || die "not a git checkout" 2
dirty=
if [ -n "$(git status --porcelain --untracked-files=no)" ]; then
	[ "$allow_dirty" -eq 1 ] || die "uncommitted changes to tracked files; the check builds a clean commit (or pass --allow-dirty)" 2
	dirty=.dirty
fi
command -v tart >/dev/null 2>&1 || die "tart not found (vm/macos/README.md)" 2
version=$(sed -n 's/^project(ntfs-macos VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt)
[ -n "$version" ] || die "cannot read the project version from CMakeLists.txt" 2
rev=$(git rev-parse HEAD)
sde=$(git log -1 --format=%ct "$rev")
label=$version-repro.g$(git rev-parse --short=12 HEAD)$dirty
tarball=$(awk '$1 == "ntfs-3g" { n = split($3, a, "/"); print a[n] }' third_party/deps.lock)
[ -n "$tarball" ] || die "no ntfs-3g entry in third_party/deps.lock" 2
[ -f "third_party/cache/$tarball" ] || die "third_party/cache/$tarball is missing (this script never downloads; scripts/fetch-deps.sh)" 2
printf 'repro-check: commit %s, SOURCE_DATE_EPOCH %s, label %s, work %s\n' "$rev" "$sde" "$label" "$work"

rm -rf "$work/a" "$work/b" "$work/cmp" "$work/src.tar"
mkdir -p "$work/a" "$work/b"
# One source tar for both builds, made as scripts/release-local.sh makes it (same tree, same git archive
# settings): the commit's tree, or with --allow-dirty the working-tree file set of scripts/lib/tree.sh,
# plus the verified tarball.
if [ "$allow_dirty" -eq 1 ]; then
	tree=$(ntfsmac_worktree_tree 1 "$work") || die "cannot make the tree of the working-tree files" 2
	rm -f "$work/tree-index" "$work/tree-list"
else
	tree=$(git rev-parse "$rev^{tree}")
fi
git -c tar.umask=022 -c core.autocrlf=false -c core.eol=lf archive --format=tar \
	--prefix=third_party/cache/ --add-file="$root/third_party/cache/$tarball" --prefix= "$tree" >"$work/src.tar" ||
	die "git archive of the tree failed"

# ---- VM slot (the protocol of vm/macos/run-suite.sh: mkdir slot-N, pid file with this shell's pid) ----
slot_dir=${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}
case $slot_dir in /*) ;; *) slot_dir=$(pwd)/$slot_dir ;; esac
slot=
terminating=0
pid_gone() {
	case $1 in '' | *[!0-9]*) return 0 ;; esac
	kill -0 "$1" 2>/dev/null && return 1
	ps -p "$1" >/dev/null 2>&1 && return 1
	return 0
}
grab_slot_claim() {
	mv "$1" "$gs_d/pid.$$" 2>/dev/null || return 1
	gs_x=$(cat "$gs_d/pid.$$" 2>/dev/null || true)
	if [ "$gs_x" = "$2" ]; then
		if (set -C; echo $$ >"$gs_d/pid") 2>/dev/null; then
			rm -f "$gs_d/pid.$$"
			return 0
		fi
		rm -f "$gs_d/pid.$$"
		return 1
	fi
	[ -e "$1" ] || mv "$gs_d/pid.$$" "$1" 2>/dev/null || true
	return 1
}
grab_slot() {
	gs_d=$slot_dir/slot-$1
	if mkdir "$gs_d" 2>/dev/null; then
		echo $$ >"$gs_d/pid"
		return 0
	fi
	if [ -e "$gs_d/pid" ]; then
		gs_p=$(cat "$gs_d/pid" 2>/dev/null || true)
		if [ -n "$gs_p" ]; then
			pid_gone "$gs_p" || return 1
		else
			[ -n "$(find "$gs_d/pid" -mmin +1 2>/dev/null)" ] || return 1
		fi
		grab_slot_claim "$gs_d/pid" "$gs_p" || return 1
		echo "repro-check: removed the stale slot-$1 (holder '$gs_p' is gone)" >&2
		return 0
	fi
	gs_k=
	for gs_c in "$gs_d"/pid.*; do
		[ -f "$gs_c" ] || continue
		gs_k=1
		if ! pid_gone "${gs_c##*/pid.}" && [ -z "$(find "$gs_c" -mmin +1 2>/dev/null)" ]; then
			return 1
		fi
	done
	for gs_c in "$gs_d"/pid.*; do
		[ -f "$gs_c" ] || continue
		gs_p=$(cat "$gs_c" 2>/dev/null || true)
		if [ -n "$gs_p" ] && pid_gone "$gs_p"; then
			grab_slot_claim "$gs_c" "$gs_p" || return 1
			echo "repro-check: removed the stale slot-$1 (claim of a gone run, holder $gs_p is gone)" >&2
			return 0
		fi
		[ ! -e "$gs_d/pid" ] && mv "$gs_c" "$gs_d/pid" 2>/dev/null || true
		return 1
	done
	[ -z "$gs_k" ] || return 1
	[ -n "$(find "$gs_d" -maxdepth 0 -mmin +1 2>/dev/null)" ] || return 1
	(set -C; echo $$ >"$gs_d/pid") 2>/dev/null || return 1
	echo "repro-check: took the abandoned slot-$1 (no pid file)" >&2
	return 0
}
release_slot() {
	[ -n "$slot" ] || return 0
	if [ "$(cat "$slot_dir/slot-$slot/pid" 2>/dev/null || true)" = "$$" ]; then
		rm -f "$slot_dir/slot-$slot/pid"
		rmdir "$slot_dir/slot-$slot" 2>/dev/null || true
	fi
	slot=
}
take_slot() {
	mkdir -p "$slot_dir"
	while [ -z "$slot" ]; do
		for ts_s in ${NTFSMAC_VM_SLOTS:-1 2}; do
			if grab_slot "$ts_s"; then slot=$ts_s; break; fi
		done
		[ -n "$slot" ] || sleep "${NTFSMAC_SLOT_WAIT:-30}"
		[ "$terminating" -eq 0 ] || exit 130
	done
	echo "repro-check: holding slot-$slot" >&2
}

vms=
cleanup() {
	for vm in $vms; do
		tart list --quiet 2>/dev/null | grep -qx "$vm" || continue
		"$tart_sh" delete "$vm" >/dev/null 2>&1 || echo "repro-check: WARNING: could not delete $vm" >&2
	done
	release_slot
}
trap cleanup EXIT
trap 'terminating=1; exit 130' INT TERM HUP

gexec() { # vm command-string: run in the guest with Homebrew on PATH
	tart exec "$1" /bin/sh -c "export PATH=/opt/homebrew/bin:/usr/local/bin:\$PATH; $2"
}

take_slot
for side in a b; do
	vm=ntfsmac-repro-$side-$$
	log "build $side in a fresh clone $vm"
	vms="$vms $vm"
	"$tart_sh" clone "$vm" --from golden >&2
	"$tart_sh" run "$vm" >&2
	tart exec -i "$vm" /bin/sh -c 'rm -rf ~/ntfsmac && mkdir -p ~/ntfsmac/tree && tar -xf - -C ~/ntfsmac/tree' <"$work/src.tar"
	if ! gexec "$vm" "cd ~/ntfsmac/tree && NTFSMAC_SOURCE_REV='$rev$dirty' SOURCE_DATE_EPOCH='$sde' sh packaging/build-pkg.sh --label '$label' --out ~/ntfsmac/dist --work ~/ntfsmac/work 2>&1" >"$work/build-$side.log"; then
		tail -n 30 "$work/build-$side.log" >&2
		die "build $side failed (log: $work/build-$side.log)"
	fi
	gexec "$vm" 'cd ~/ntfsmac/dist && tar -cf - .' | tar -xf - -C "$work/$side"
	(cd "$work/$side" && shasum -a 256 -c SHA256SUMS >/dev/null) || die "the copied build $side does not match its checksums"
	"$tart_sh" delete "$vm" >/dev/null 2>&1 || echo "repro-check: WARNING: could not delete $vm" >&2
done
cleanup
trap - EXIT

log "compare $work/a and $work/b"
compare "$work/a" "$work/b"
