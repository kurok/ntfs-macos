#!/bin/sh
# SPDX-License-Identifier: GPL-2.0-or-later
#
# Install a local pre-push hook that runs scripts/ci-local.sh --quick (ADR-015: CI runs on this
# machine, not on GitHub). A failing run blocks the push; `git push --no-verify` skips it.
# Refuses to overwrite a pre-push hook it did not write.
#
# Usage: scripts/install-git-hooks.sh [--remove]
# Exit:  0 done, 1 error, 2 usage error.
set -eu

root=$(git rev-parse --show-toplevel)
hooks=$(git rev-parse --git-path hooks)
hook=$hooks/pre-push
marker='# ntfs-macos: ci-local pre-push hook'

case "${1:-}" in
'') ;;
--remove)
	if [ -f "$hook" ] && grep -qF "$marker" "$hook"; then rm -f "$hook"; echo "removed $hook"; fi
	exit 0
	;;
*) echo "usage: $0 [--remove]" >&2; exit 2 ;;
esac

if [ -e "$hook" ] && ! grep -qF "$marker" "$hook"; then
	echo "install-git-hooks: $hook exists and was not written by this script; not touching it" >&2
	exit 1
fi
mkdir -p "$hooks"
cat >"$hook" <<EOF
#!/bin/sh
$marker
# Installed by scripts/install-git-hooks.sh. Skip once with: git push --no-verify
exec sh "\$(git rev-parse --show-toplevel)/scripts/ci-local.sh" --quick
EOF
chmod 0755 "$hook"
echo "installed $hook (runs scripts/ci-local.sh --quick before each push from $root)"
