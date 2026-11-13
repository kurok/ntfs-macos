#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Check build prerequisites. Prints what is missing with an install hint.
# Never installs anything. Exit 0 if all present, 1 otherwise.
set -eu

CMAKE_MIN="3.28"
missing=0

case "$(uname -s)" in
Darwin) hint_kind=brew ;;
*) hint_kind=apt ;;
esac

hint() { # tool brew-package apt-package
	if [ "$hint_kind" = brew ]; then
		printf '  install: brew install %s\n' "$2"
	else
		printf '  install: sudo apt-get install %s\n' "$3"
	fi
}

# version_ge A B: true if A >= B (dotted numeric versions).
version_ge() {
	[ "$(printf '%s\n%s\n' "$2" "$1" | sort -t. -k1,1n -k2,2n -k3,3n | head -n 1)" = "$2" ]
}

printf 'Checking prerequisites (nothing is installed by this script)\n'

if command -v cmake >/dev/null 2>&1; then
	v=$(cmake --version | sed -n '1s/^[^0-9]*\([0-9][0-9.]*\).*/\1/p')
	if version_ge "$v" "$CMAKE_MIN"; then
		printf 'ok       cmake %s\n' "$v"
	else
		printf 'TOO OLD  cmake %s (need >= %s)\n' "$v" "$CMAKE_MIN"
		hint cmake cmake cmake
		missing=1
	fi
else
	printf 'MISSING  cmake (need >= %s)\n' "$CMAKE_MIN"
	hint cmake cmake cmake
	missing=1
fi

if command -v ninja >/dev/null 2>&1; then
	printf 'ok       ninja %s\n' "$(ninja --version)"
else
	printf 'MISSING  ninja\n'
	hint ninja ninja ninja-build
	missing=1
fi

pkgconf_bin=
for t in pkgconf pkg-config; do
	if command -v "$t" >/dev/null 2>&1; then pkgconf_bin=$t; break; fi
done
if [ -n "$pkgconf_bin" ]; then
	printf 'ok       %s %s\n' "$pkgconf_bin" "$("$pkgconf_bin" --version)"
else
	printf 'MISSING  pkgconf\n'
	hint pkgconf pkgconf pkgconf
	missing=1
fi

cc_bin=
for t in "${CC:-}" cc clang gcc; do
	if [ -n "$t" ] && command -v "$t" >/dev/null 2>&1; then cc_bin=$t; break; fi
done
if [ -n "$cc_bin" ]; then
	printf 'ok       C compiler %s (%s)\n' "$cc_bin" "$("$cc_bin" --version | head -n 1)"
else
	printf 'MISSING  C compiler\n'
	if [ "$hint_kind" = brew ]; then
		printf '  install: xcode-select --install\n'
	else
		printf '  install: sudo apt-get install gcc\n'
	fi
	missing=1
fi

if [ "$missing" -ne 0 ]; then
	printf '\nSome prerequisites are missing. Install them yourself, then re-run.\n'
	exit 1
fi
printf '\nAll prerequisites present.\n'
