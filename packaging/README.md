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
| verify | a second fresh clone installs the package (preflight and postinstall run), checks that the daemon runs and `ntfsmac doctor`, runs each installed program, runs `uninstall.sh` and checks that nothing is left |

Result: `dist/<label>/` with `ntfs-macos-<label>.pkg`, `uninstall.sh`, `BUILD-INFO` and
`SHA256SUMS`. The label is `<version>` when `HEAD` carries the tag `v<version>`, else
`<version>-dev.<date>.g<commit>`. The working tree must be clean (`--allow-dirty` adds `.dirty` and
also sends the files that are not yet tracked and not ignored).

Options: `--no-ci`, `--no-verify`, `--keep-vm`, and `--host` to build on this Mac when macFUSE is
installed here (no verify stage).

## Install

Prerequisite: macFUSE 5.5.0 or newer, installed and turned on in System Settings.

```sh
sudo installer -pkg dist/<label>/ntfs-macos-<label>.pkg -target /
```

The package is not signed with a Developer ID or notarized, so a double-click in Finder is
blocked by Gatekeeper; use `installer` as above, or right-click > Open.

The package installs exactly the `dir` and `file` entries of [manifest.txt](manifest.txt), the same
set as `dev-install.sh`, with the manifest's modes and root:wheel. Everything root runs or reads is
in root-only directories; nothing goes into a directory that may belong to a user (a Homebrew
`/usr/local/bin` or `/usr/local/share`):

| Path | Content | Mode |
|---|---|---|
| `/usr/local/libexec/ntfs-macos/` | `ntfsmacd` (daemon), `ntfsmac` (the CLI, ad-hoc signed with the hardened runtime), `ntfsmac-worker` (NTFS-3G with the patch series), `ntfsmac-probe`, `ntfsmac-volmark`, `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel`, `uninstall.sh` (0755); `manifest.txt` (0644) | dir 0755 |
| `/Library/Application Support/ntfs-macos/` | `cli-requirement` (written by postinstall), `sandbox/probe.sb`, `sandbox/volmark.sb`, `sandbox/worker.sb`, `doc/COPYING`, `doc/README.md`, `doc/BUILD-INFO` (0644) | dirs 0755 (`.`, `sandbox`, `doc`) |
| `/Library/LaunchDaemons/org.ntfs-macos.ntfsmacd.plist` | the launchd job | 0644 |
| `/usr/local/bin/ntfsmac` | optional symbolic link to `/usr/local/libexec/ntfs-macos/ntfsmac`, made by postinstall (see below) | link |

The documents are in `/Library/Application Support/ntfs-macos/doc` rather than in
`/usr/local/libexec/ntfs-macos/doc`: `/Library` and `/Library/Application Support` are the system's
own root-owned directories on every Mac, Homebrew never changes them, and the project already owns
`/Library/Application Support/ntfs-macos` (the profiles and the requirement), while `libexec` is for
programs. There is no `LICENSES/` directory in the package (`COPYING` is the licence of everything
it ships). The daemon makes `/var/db/ntfs-macos` (journal) and launchd writes
`/var/log/ntfsmacd.log`; both are state that `uninstall.sh` keeps unless `--purge-state` is given.

The package scripts run as root and log to `/var/log/install.log` (lines `ntfs-macos preinstall:`
and `ntfs-macos postinstall:`):

- `scripts/preinstall` refuses the install, with the reason, on another volume than `/`, on macOS
  older than 27 or not arm64, without macFUSE, with macFUSE older than 5.5.0 (its
  `CFBundleShortVersionString`, else `CFBundleVersion`) or without its `libfuse.2.dylib`, when a
  path of the manifest exists although no install of ours is recorded (no receipt
  `org.ntfs-macos.pkg`, no developer install), when such a path is a symbolic link, and when a
  directory above an installed path is not root-only (those are `/usr/local/libexec`, `/usr/local`,
  `/usr`, `/Library` and their parents; a user-owned `/usr/local/bin` or `/usr/local/share` does
  not stop the install).
  **Owner decision 2026-11-26:** it also refuses when another NTFS driver is installed:
  `ufsd_NTFS.fs` of Paragon, `tuxera_ntfs.fs` of Tuxera, any other `*ntfs*.fs` in
  `/Library/Filesystems` except Apple's `ntfs.fs` and our own `ntfsmac*` names (the same rule as
  the daemon's conflict detection), or an `*ntfs*.kext` in `/Library/Extensions`. The earlier design
  warned, installed and set passive mode (ARCHITECTURE.md section 8.4); the daemon still goes
  passive when such a bundle is installed later (not for a kext-only driver: a known gap, ARCHITECTURE.md
  section 8.4).
- An install of ours is upgraded: preinstall stops the running daemon (it unmounts its volumes),
  and removes the files an earlier install of ours had and this package does not ship (the earlier
  `/usr/local/bin/ntfsmac` file and `/usr/local/share/doc/ntfs-macos` included), but only where the
  directory is root-only and reached without a symbolic link; anything else is left and named in
  the log. If the daemon does not stop within 70 s the install stops before any file is copied;
  unmount the NTFS volumes and run the installer again, or start the earlier daemon again with
  `sudo launchctl bootstrap system /Library/LaunchDaemons/org.ntfs-macos.ntfsmacd.plist`. If an
  upgrade fails later (in postinstall), run the installer again; it repeats every step.
- `scripts/postinstall` installs the authorization right `org.ntfs-macos.journal-modify`, records
  the cdhash of the installed CLI in `cli-requirement` (ADR-019 check 3; recorded at install so a
  re-signed payload stays valid), makes the link `/usr/local/bin/ntfsmac`, loads the job and waits
  until the daemon serves its socket. The link is made only when `/usr/local/bin` exists, is
  reached without a symbolic link, and it and every directory above it are owned by root and not
  writable by group or others, and only when nothing else is at that path. Otherwise it is skipped,
  and the log says to run `/usr/local/libexec/ntfs-macos/ntfsmac` by its full path or to add
  `/usr/local/libexec/ntfs-macos` to `PATH`. If something other than our link is at
  `/usr/local/bin/ntfsmac` (for example the CLI file of the earlier layout, which preinstall leaves
  when `/usr/local/bin` is not root-only), the last line of postinstall is a WARNING that says so,
  with the remedy: remove it by hand, or run the CLI by its full path.

Only the macFUSE checks (missing, too old) and the named drivers (Paragon, Tuxera) are also in the
installation check of `distribution.xml`, so only those reasons show in the Installer window and in
the output of `installer`. Every other refusal shows there as a failed install; the reason is in
`/var/log/install.log`.

## Uninstall

```sh
sudo /usr/local/libexec/ntfs-macos/uninstall.sh
```

It stops the daemon, removes the right, the files of the package receipt `org.ntfs-macos.pkg` (each
must lie below a `dir` of the manifest or be exactly a `file` or `link` path of the manifest;
anything else refuses the whole run) and every path of the manifest, removes `/usr/local/bin/ntfsmac`
only when it is a symbolic link to exactly `/usr/local/libexec/ntfs-macos/ntfsmac` and `/usr/local/bin`
is root-only (the same rule when a receipt names that path; anything else there, such as the CLI
file of the earlier layout, is kept and named in the output), then forgets the receipt. It never removes a shared directory (`/usr/local/bin`, `/Library/LaunchDaemons`), even an
empty one. `--dry-run` lists what it would remove.

## Developer install from source

To build and install the same set from a checkout instead of the package:

```sh
packaging/dev-install.sh        # as your own user; it calls sudo for the install steps
/usr/local/libexec/ntfs-macos/ntfsmac doctor
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
cdhash in `/Library/Application Support/ntfs-macos/cli-requirement` (ADR-019 check 3), makes the
link `/usr/local/bin/ntfsmac` under the same rule as postinstall, installs the authorization right
`org.ntfs-macos.journal-modify`, and loads the job
`org.ntfs-macos.ntfsmacd`. Running it again installs over the earlier install. Options: `--deps`,
`--build-dir`, `--jobs`, `--no-build`, `--no-load`, `--job-arg ARG` (a daemon argument in the installed
plist, for example `--automount-mode=ro`) and, for the VM test drivers only, `--allow-seams` (accept a build with
test seams, which the default refuses).

`uninstall.sh` removes exactly the paths of the manifest, boots out the job and removes the right.
It checks the whole manifest first and refuses (removing nothing) a path that is not one of the
project's own; it removes a directory of the manifest only when it is empty (never one that is a
symbolic link) and touches nothing in `/usr/local/bin` other than its own link `ntfsmac`. Only `--purge-state`
deletes recursively, and only `/var/db/ntfs-macos`. The journal (`/var/db/ntfs-macos`) and the daemon
log stay unless `--purge-state` is given; `--dry-run` lists what would go. The same script removes
a package installed with `installer` (it then removes the receipt's files, each of which must lie
below a `dir` of the manifest or be exactly a `file` or `link` path of the manifest).

Run the installer and the uninstaller only on a Mac you mean to change. The tests run them in a
disposable guest: `tests/integration/packaging/pkg_vm.sh` (I-PKG-01..02, developer install), and for
the package `tests/integration/packaging/pkg_release_vm.sh --pkg-dir dist/<label> --fixtures DIR`
(I-PKG-03..04), `pkg_preflight_vm.sh --pkg-dir dist/<label>` (I-PKG-05) and
`pkg_preflight_vm.sh --pkg-dir dist/<label> --case userlocal` (I-PKG-06). Each starts one VM;
run them through the VM slot protocol when other runs use the VMs. Host-side checks:
`ctest -R packaging` (U-PKG-01..04).
