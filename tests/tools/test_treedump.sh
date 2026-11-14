#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Unit test for treedump: dump a small temp tree and compare with a known value.
# Usage: test_treedump.sh PATH-TO-TREEDUMP
set -eu

[ $# -eq 1 ] || { echo "usage: $0 TREEDUMP" >&2; exit 2; }
td=$1
tmp=$(mktemp -d "${TMPDIR:-/tmp}/treedump-test.XXXXXX")
trap 'rm -rf "$tmp"' EXIT HUP INT TERM

root=$tmp/root
mkdir -p "$root/sub/deeper" "$root/empty dir" "$root/\$Extend" \
	"$root/System Volume Information"
printf 'abc' >"$root/a.txt"
: >"$root/zero"
printf 'x' >"$root/sub/deeper/in side.txt"
printf 'hidden' >"$root/\$MFT"
printf 'svi' >"$root/System Volume Information/x"
# 1,000,000 'a': multi-block SHA-256 known answer.
head -c 1000000 /dev/zero | tr '\0' a >"$root/million"
# Non-ASCII name (UTF-8 e-acute) and a tab in a name.
eacute=$(printf '\303\251')
printf 'u' >"$root/caf$eacute.txt"
printf 't' >"$root/tab$(printf '\t')name"
ln -s a.txt "$root/link"

"$td" "$root" >"$tmp/got"

T=$(printf '\t')
cat >"$tmp/want" <<EOT
f${T}a.txt${T}3${T}ba7816bf8f01cfea414140de5dae2223b00361a396177a9cb410ff61f20015ad
f${T}caf${eacute}.txt${T}1${T}0bfe935e70c321c7ca3afc75ce0d0ca2f98b5422e008bb31c00c6d7f1f1c0ad6
d${T}empty dir
l${T}link${T}a.txt
f${T}million${T}1000000${T}cdc76e5c9914fb9281a1c7e284d73e67f1809a48a497200e046d39ccc7112cd0
d${T}sub
d${T}sub/deeper
f${T}sub/deeper/in side.txt${T}1${T}2d711642b726b04401627ca9fbac32f5c8530fb1903cc4db02258717921a4881
f${T}tab\\tname${T}1${T}e3b98a4da31a127d4bde6e43033f66ba274cab0eb7eb1c70ec41402bf6273dd8
f${T}zero${T}0${T}e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855
EOT

if ! cmp -s "$tmp/got" "$tmp/want"; then
	echo "FAIL: treedump output differs from the known value" >&2
	diff "$tmp/want" "$tmp/got" >&2 || true
	exit 1
fi
echo "treedump: OK"
