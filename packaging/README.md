# Packaging

Local, unsigned developer packages (ADR-011 unsigned path). Packages are built in the git-ignored
`dist/` directory. Only the owner uploads a package, by hand, as a GitHub pre-release (ADR-015,
amended 2026-11-16); user instructions are in [docs/INSTALL.md](../docs/INSTALL.md).

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

## Developer install from source

For a Mac on which you want the daemon, the CLI and the launchd job (the package above has none of
them yet), build and install from a checkout:

```sh
packaging/dev-install.sh        # as your own user; it calls sudo for the install steps
ntfsmac doctor
sudo /usr/local/libexec/ntfs-macos/uninstall.sh
```

`dev-install.sh` refuses to install unless every directory above an installed path is a real
directory owned by root and not writable by group or others (a user-owned `/usr/local/libexec`
would let a user plant a program that runs as root); the uninstaller warns about the same. It
needs macOS 27 on arm64, macFUSE 5.5.0 or newer and cmake. It builds NTFS-3G (into
`build/deps`) and the product with the CMake preset `product` (no test seams; check 4 of ADR-019 is
the root-peer fallback, so `journal-clear` and `repair` are run with `sudo`), then installs every
path of [manifest.txt](manifest.txt) with the owner and mode the manifest gives (root:wheel; the
sandbox profiles 0644), signs the installed CLI ad hoc with the hardened runtime and records its
cdhash in `/Library/Application Support/ntfs-macos/cli-requirement` (ADR-019 check 3), installs the
authorization right `org.ntfs-macos.journal-modify`, and loads the job
`org.ntfs-macos.ntfsmacd`. Running it again installs over the earlier install. Options: `--deps`,
`--build-dir`, `--jobs`, `--no-build`, `--no-load`, `--job-arg ARG` (a daemon argument in the installed
plist, for example `--automount-mode=ro`) and, for the VM test drivers only, `--allow-seams` (accept a build with
test seams, which the default refuses).

`uninstall.sh` removes exactly the paths of the manifest, boots out the job and removes the right.
It checks the whole manifest first and refuses (removing nothing) a path that is not one of the
project's own; it removes a directory of the manifest only when it is empty (never one that is a
symbolic link) and touches nothing in `/usr/local/bin` other than `ntfsmac`. Only `--purge-state`
deletes recursively, and only `/var/db/ntfs-macos`. The journal (`/var/db/ntfs-macos`) and the daemon
log stay unless `--purge-state` is given; `--dry-run` lists what would go. The same script removes
a package installed with `installer` (it then removes the receipt's files, which must all lie in the
manifest's directories).

Run the installer and the uninstaller only on a Mac you mean to change. The tests run them in a
disposable guest: `tests/integration/packaging/pkg_vm.sh` (I-PKG-01..02).
