<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Install and uninstall

> **Developer preview.** The current package cannot mount an NTFS volume yet. It installs the
> NTFS-3G worker, the safety probe, the volume-marker tool and four ntfsprogs tools for testing.
> Automatic mounting, the daemon and Finder integration come with Milestone 4. Do not use these
> tools on a drive that holds data you care about.

## Requirements

| Item | Requirement |
|---|---|
| Mac | Apple Silicon (arm64). Intel Macs are not supported by this package. |
| macOS | 27 or newer |
| macFUSE | 5.5.0 or newer, installed and turned on in System Settings. Get it from <https://macfuse.github.io/>. ntfs-macos does not bundle or download macFUSE. |

The installer stops with a message if one of these is missing.

## Download and check

1. Open the [Releases](https://github.com/kurok/ntfs-macos/releases) page and download, from the
   release you want:
   - `ntfs-macos-<label>.pkg`
   - `SHA256SUMS`
2. Check the download in Terminal, in the folder that holds both files:

   ```sh
   shasum -a 256 -c SHA256SUMS --ignore-missing
   ```

   The line for the `.pkg` must end with `OK`. If it does not, delete the file and do not install it.

## Install

The package is not signed with an Apple Developer ID and is not notarized. Gatekeeper therefore
blocks a double-click in Finder. Install it from Terminal:

```sh
sudo installer -pkg ntfs-macos-<label>.pkg -target /
```

What is installed:

| Path | Content |
|---|---|
| `/usr/local/libexec/ntfs-macos/` | `ntfsmac-worker` (NTFS-3G 2026.9.28 with this project's patch series), `ntfsmac-probe`, `ntfsmac-volmark`, `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel`, `uninstall.sh`, `manifest.txt` |
| `/usr/local/share/doc/ntfs-macos/` | `COPYING`, `LICENSES/`, `README.md`, `BUILD-INFO` |

The package adds nothing to `PATH`. It installs no daemon or LaunchDaemon, and it has no
`preinstall` or `postinstall` scripts.

To see which build is installed:

```sh
cat /usr/local/share/doc/ntfs-macos/BUILD-INFO
pkgutil --pkg-info org.ntfs-macos.pkg
```

## Uninstall

```sh
sudo /usr/local/libexec/ntfs-macos/uninstall.sh
```

The script removes only the files listed in the package receipt `org.ntfs-macos.pkg`, and only
below the directories that `manifest.txt` names (the two directories above). Then it removes the
receipt. It checks every path first and stops with nothing removed if one lies outside those
directories. To see what it would remove without removing anything, add `--dry-run`.

The same script and manifest remove a developer install made from source (see
[packaging/README.md](../packaging/README.md)): it also unloads the launchd job and removes the
authorization right, and it leaves the journal of unsafe volumes in `/var/db/ntfs-macos` unless you
add `--purge-state`.

macFUSE stays installed. Remove it with its own uninstaller if you no longer need it.

## Source code

The binaries are licensed under GPL-2.0-or-later. Each release lists the commit it was built
from (`source` in `BUILD-INFO`) and its tag. The corresponding source is:

- this repository at that tag, which includes the NTFS-3G patch series in
  `third_party/patches/ntfs-3g/` and the build scripts;
- the unmodified NTFS-3G release tarball, attached to the same GitHub release.

To build a package yourself, see [packaging/README.md](../packaging/README.md).

## Problems

Open an issue at <https://github.com/kurok/ntfs-macos/issues>. Include the contents of
`BUILD-INFO`, your macOS version and your macFUSE version. Do not post the details of a security
problem in a public issue; open an issue that asks for a private contact instead.
