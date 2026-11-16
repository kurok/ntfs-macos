# Packaging

Local, unsigned developer packages (ADR-011 unsigned path). Nothing is uploaded: packages
stay in the git-ignored `dist/` directory (ADR-015).

## Build a package

```sh
scripts/release-local.sh
```

| Stage | What happens |
|---|---|
| ci | `scripts/ci-local.sh --quick` on the host |
| build | `packaging/build-pkg.sh` in a disposable clone of the golden VM (macOS 27 + macFUSE) |
| verify | a second fresh clone installs the package, runs each installed binary, runs `uninstall.sh` and checks that nothing is left |

Result: `dist/<label>/` with `ntfs-macos-<label>.pkg`, `uninstall.sh`, `BUILD-INFO` and
`SHA256SUMS`. The label is `<version>` when `HEAD` carries the tag `v<version>`, else
`<version>-dev.<date>.g<commit>`. The working tree must be clean (`--allow-dirty` adds `.dirty`).

Options: `--no-ci`, `--no-verify`, `--keep-vm`, and `--host` to build on this Mac when macFUSE is
installed here (no verify stage).

## Install

Prerequisite: macFUSE 5.5.0 or newer, installed and turned on in System Settings. The installer
refuses to run without it, and on macOS older than 27 or on Intel Macs.

```sh
sudo installer -pkg dist/<label>/ntfs-macos-<label>.pkg -target /
```

The package is not signed with a Developer ID or notarized, so a double-click in Finder is
blocked by Gatekeeper; use `installer` as above, or right-click > Open.

Installed files:

| Path | Content |
|---|---|
| `/usr/local/libexec/ntfs-macos/` | `ntfsmac-worker` (NTFS-3G with the patch series), `ntfsmac-probe`, `ntfsmac-volmark`, `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel`, `uninstall.sh` |
| `/usr/local/share/doc/ntfs-macos/` | `COPYING`, `LICENSES/`, `README.md`, `BUILD-INFO` |

Nothing is put on `PATH`, no daemon or LaunchDaemon is installed, and the package has no
`preinstall` or `postinstall` scripts. The daemon, CLI and automount come with M4 and the
release package with T-M6-01.

## Uninstall

```sh
sudo /usr/local/libexec/ntfs-macos/uninstall.sh
```

It removes only the files listed in the package receipt `org.ntfs-macos.pkg`, and only below the
two directories above, then forgets the receipt. `--dry-run` lists what it would remove.
