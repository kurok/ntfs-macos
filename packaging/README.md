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

## Source release archive (T-M6-05)

The GPL requires the corresponding source with every binary. `scripts/release-local.sh` therefore
runs `scripts/make-source-release.sh` on the host after the package build and puts
`ntfs-macos-<label>-source.tar.gz` into `dist/<label>/` next to the package, with its line in
`SHA256SUMS`. Publish the two together (ADR-015).

```sh
scripts/make-source-release.sh [--label L] [--out DIR] [--rev COMMIT] [--tree TREE] [--allow-dirty] [--worktree]
```

- The archive is the corresponding source of what was built: one git tree. `release-local.sh` makes
  the tree before the build (`scripts/lib/tree.sh`: the commit's tree, or with `--allow-dirty` and for
  the host build the file set of `git ls-files --cached --others --exclude-standard` with the
  working-tree contents, force-added ignored files included, no line-ending conversion, an untracked
  nested repository refused), gives it to the build (sent to the VM, or extracted and built on the host,
  so no cached CMake state of the working tree and no later edit reaches the build), and passes the same
  id as `--tree`. Standalone, the script uses the commit's tree, or with `--allow-dirty` or `--worktree`
  the working-tree set. Plus the pinned NTFS-3G tarball from `third_party/cache` and `SOURCES.txt`
  (licence, corresponding-source statement, base commit, tree id, dirty flag and changed paths, git and
  git version and the built-in gzip, the macFUSE prerequisite), under `ntfs-macos-<label>-source/`.
- A tree that differs from the commit is dirty: the label gets `.dirty` (also for untracked files on
  the host), and `BUILD-INFO` and `SOURCES.txt` agree.
- Standalone it refuses uncommitted changes to tracked files unless `--allow-dirty`, and an untracked
  `*.patch` in the patch directory unless the working-tree set is archived. `--rev` is the commit
  that was built; the script refuses if `HEAD` is another commit. `release-local.sh` passes it.
- It verifies the tarball against `third_party/deps.lock` and refuses on a mismatch. It never
  downloads: if the tarball is missing it prints `scripts/fetch-deps.sh ntfs-3g` and exits 1. It also
  checks that every patch the series README lists as applied is in the archive.
- Same tree and same git version give the same bytes: the commit time on every entry, owner 0/0,
  modes with umask 022, compressed by git's built-in gzip (`tar.tar.gz.command=git archive gzip`, no
  name, no time, not the system gzip).
- `--rev` is strict on purpose: if `HEAD` is not the commit that was built, the script refuses, even when
  only a later commit was added.
- If it fails in `release-local.sh`, the package is removed from `dist/<label>/`.
- `sign-and-notarize.sh` requires the archive and `SHA256SUMS` next to the unsigned package, checks the
  archive against its line (`shasum -a 256 -c`), and copies it, with its line, into the signed output.
- The label logic is `scripts/lib/label.sh`, shared with `release-local.sh`; `--label` overrides it.
  A default label has the current date; pass `--label` for a reproducible name.
- Test: U-PKG-06 (`ctest -R packaging.source_release`).

## Install

Prerequisite: macFUSE 5.5.0 or newer, installed and turned on in System Settings.

```sh
sudo installer -pkg dist/<label>/ntfs-macos-<label>.pkg -target /
```

The package is not signed with a Developer ID or notarized, so Gatekeeper blocks a double-click
in Finder; use `installer` as above (see "Unsigned packages" below).

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

## Unsigned packages (ADR-011)

Until Developer ID credentials exist (H8, `docs/IMPLEMENTATION_PLAN.md`), every package from
`build-pkg.sh` is unsigned. That is the documented unsigned path of ADR-011 (last row of its
table): ship the unsigned `.pkg` with build-from-source instructions, say that Gatekeeper blocks a
double-click install, and mark M6 as "complete, unsigned" (criterion 6.4 is then recorded as
unsigned). The supported install is `sudo installer -pkg FILE -target /`, as in
[docs/INSTALL.md](../docs/INSTALL.md); the release notes must say that the package is unsigned.

## Signed release (T-M6-02, not run yet)

`sign-and-notarize.sh` turns the unsigned package into a signed, notarized and stapled one. It
needs Xcode or its command line tools (`notarytool`, `stapler`), a Developer ID Application and a Developer ID
Installer certificate in the keychain of the Mac that runs it, and a `notarytool` keychain profile
made once with `xcrun notarytool store-credentials NAME` (H8). The names are arguments; nothing is
stored in the repository, in a file or in the environment, and the script never reads or changes a
keychain itself.

```sh
packaging/sign-and-notarize.sh --pkg dist/<label>/ntfs-macos-<label>.pkg \
    --profile NAME \
    --app-identity "Developer ID Application: ..." \
    --installer-identity "Developer ID Installer: ..." [--out-dir DIR] [--dry-run]
```

| Step | Command |
|---|---|
| unpack | `pkgutil --expand-full` |
| sign each Mach-O file of the payload | `codesign --force --timestamp --options runtime --sign <Application identity>`; a file whose `otool -L` lists `/usr/local/lib/libfuse*` (the worker) also gets `--entitlements packaging/entitlements-libfuse.plist` (`com.apple.security.cs.disable-library-validation`), because with library validation the signed worker refuses macFUSE's libfuse |
| rebuild | `pkgbuild --ownership recommended` and `productbuild` from the signed payload, with the original scripts, identifier, version and Distribution. `pkgutil --flatten-full` is not used: run as a normal user it writes the user's uid into the bill of materials and adds `._` files |
| check | `lsbom -p MUGf` of the new component must equal the original; otherwise the script stops |
| sign the package | `productsign --sign <Installer identity>` |
| notarize | `xcrun notarytool submit --keychain-profile NAME --wait`; on a status other than Accepted it prints `notarytool log` for the submission and stops |
| staple | `xcrun stapler staple`, then `stapler validate` |
| Gatekeeper check | `spctl -a -t install -vv` (criterion 6.4) |
| output | `ntfs-macos-<label>.pkg`, `uninstall.sh`, `manifest.txt`, `BUILD-INFO` (signature line changed), the source archive `ntfs-macos-<label>-source.tar.gz` (required in the input directory with its `SHA256SUMS` line, checked, copied unchanged) and a new `SHA256SUMS` in `--out-dir` (default `signed/` next to the input) |

`--dry-run` prints these commands and runs none. Without a profile or an identity (or without the
tools) the script exits 3 and points to the unsigned path above; exit 2 is a usage error and exit 1
a failed step. The input directory is not changed. Unit test:
`tests/unit/packaging/sign_and_notarize_test.sh` (U-PKG-05, ctest `packaging.sign_and_notarize`)
checks the argument handling and the dry run only. Checked on the host without signing: the
rebuild gives root:wheel for every entry (`--ownership recommended`), whereas `pkgutil
--flatten-full` as a normal user gave the user's uid and `._` files. A host where the expanded files
carry `com.apple.provenance` (it cannot be removed with `xattr`) still gets `._` files from
`pkgbuild`; the bill-of-materials check then stops the script, so run it where the files carry no
such attribute. Not yet checked against a real account: that the signed worker starts with the
entitlement, and that the signed CLI agrees with the code requirement that postinstall records
(`cli-requirement`, ADR-019 check 3). Do both in the first real run, in a disposable guest, before
anything is published.

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
`ctest -R packaging` (U-PKG-01..05).
