# shellcheck shell=sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# The tree of the file set a build gets (sourced, POSIX sh; run in the repository root). Shared by
# scripts/release-local.sh and scripts/make-source-release.sh so that the build input and the source
# archive are the same git tree.
#
#   ntfsmac_worktree_tree OTHERS TMPDIR   print the id of a tree (written to the object database) made
#       from `git ls-files --cached [--others] --exclude-standard`, read from the real index, with the
#       working-tree contents of those files: tracked files (also force-added ignored ones), and with
#       OTHERS=1 the new files that are not ignored. A tracked file that is deleted is absent.
#       third_party/cache is left out. No line-ending or filemode conversion (autocrlf and safecrlf
#       off, filemode on). A directory in the list (an untracked nested repository) fails the call, with a message.
#       TMPDIR holds the temporary index and list; the real index is not touched.

ntfsmac_worktree_tree() {
	_others=
	[ "$1" -eq 1 ] && _others=--others
	_idx=$2/tree-index
	_lst=$2/tree-list
	rm -f "${_idx:?}"
	# shellcheck disable=SC2086 # the option or nothing
	git ls-files -z --cached $_others --exclude-standard -- . ':!third_party/cache' >"$_lst" || return 1
	if tr '\0' '\n' <"$_lst" | grep -q '/$'; then
		printf 'tree.sh: an untracked nested repository (or a directory) is in the file set: %s\n' "$(tr '\0' '\n' <"$_lst" | grep '/$' | head -1)" >&2
		return 1
	fi
	GIT_INDEX_FILE=$_idx git -c core.autocrlf=false -c core.safecrlf=false -c core.fileMode=true \
		update-index --add --remove -z --stdin <"$_lst" || return 1
	GIT_INDEX_FILE=$_idx git write-tree
}
