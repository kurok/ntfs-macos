# shellcheck shell=sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Version label shared by scripts/release-local.sh and scripts/make-source-release.sh (sourced, POSIX sh).
# Both run it in the repository root.
#
#   ntfsmac_project_version        print the version of CMakeLists.txt (empty when not found)
#   ntfsmac_make_label VER DIRTY   print the label: VER when HEAD carries the tag v<VER> and DIRTY is
#                                  empty, else <VER>-dev.<UTC date>.g<12-hex commit><DIRTY>
#                                  (DIRTY is ".dirty" or empty)

ntfsmac_project_version() {
	sed -n 's/^project(ntfs-macos VERSION \([0-9][0-9.]*\).*/\1/p' CMakeLists.txt
}

ntfsmac_make_label() {
	if [ "$(git tag --points-at HEAD | grep -cx "v$1")" -eq 1 ] && [ -z "$2" ]; then
		printf '%s\n' "$1"
	else
		printf '%s-dev.%s.g%s%s\n' "$1" "$(date -u +%Y%m%d)" "$(git rev-parse --short=12 HEAD)" "$2"
	fi
}
