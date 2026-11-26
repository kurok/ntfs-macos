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

## Reproducibility (T-M6-04, criterion 6.2)

```sh
scripts/repro-check.sh                       # builds HEAD twice; the tree must be clean
scripts/repro-check.sh --allow-dirty         # the working tree instead (label ends in .dirty)
scripts/repro-check.sh --compare DIR_A DIR_B # only compare two outputs of build-pkg.sh (host, no VM)
```

The check packs the commit once on the host as `release-local.sh` packs it (`git archive` of the
commit's tree, or with `--allow-dirty` of the `scripts/lib/tree.sh` file set, with `tar.umask=022`,
`core.autocrlf=false`, `core.eol=lf`, plus the pinned NTFS-3G tarball; nothing is downloaded) and
builds that tar twice with `build-pkg.sh`, each time in a FRESH clone of the golden VM, as
`release-local.sh` does: same guest paths (`~/ntfsmac/tree`, `~/ntfsmac/work`),
same label `<version>-repro.g<commit>`, same `NTFSMAC_SOURCE_REV` and `SOURCE_DATE_EPOCH`. The two
builds run one after the other in one VM slot; each clone is deleted after its build. It then
compares, on the host, the `.pkg` files, the members of the product archive (`pkgutil --expand`)
and its xar table of contents, every payload file (`pkgutil --expand-full`: type, mode, sha256; a
Mach-O file that differs is compared again without its code signature, with `LC_UUID` zeroed, by its
debug-map entries together with every segment but `__LINKEDIT`, and by its strings, to name the cause;
another differing non-text file is `binary-content`), the Bom with and without times, the
package scripts, and `BUILD-INFO`, `uninstall.sh`, `manifest.txt` and `SHA256SUMS`. It prints one
row per item and exits 0 only when every difference has a cause of the allow-list in the script
(each rule has a comment there). Unit test: U-PKG-07 (`ctest -R packaging.repro_check`); the check
itself is I-PKG-07 in [docs/TEST_STRATEGY.md](../docs/TEST_STRATEGY.md).

What the build does for it (`build-pkg.sh`, `build-ntfs3g.sh`):

| Choice | Why |
|---|---|
| `SOURCE_DATE_EPOCH` = commit time of the built commit. `release-local.sh` (VM and host build) and `repro-check.sh` pass it with `NTFSMAC_SOURCE_REV`, since the extracted trees have no `.git`. Otherwise `build-pkg.sh` takes both from git only when the tree is the top of its own repository (an archive extracted inside another repository never borrows its commit), else from the `NTFSMAC_SOURCE_REV=` and `SOURCE_DATE_EPOCH=` lines of a source archive's `SOURCES.txt`, else the revision is `unknown` and the date is the current time, which `BUILD-INFO` then calls the build time. `--source-info` prints what it would use | one date for everything below; the compilers use it for `__DATE__`/`__TIME__` (not used by the product or NTFS-3G today) |
| `BUILD-INFO` line `source-date` (the commit time) instead of `built` (the build time) | the build time made `BUILD-INFO`, its Bom entry and the Payload differ |
| every payload file and both scripts get the time `SOURCE_DATE_EPOCH` (`touch -h -t`) before `pkgbuild` | `pkgbuild` copies modification times into the Bom and the cpio headers of Payload and Scripts (it writes 0 for device and inode numbers itself) |
| `ZERO_AR_DATE=1` (exported by both scripts; part of the `build-ntfs3g.sh` stamp, not of `--print-key`) | NTFS-3G builds with `-g`; ld writes the modification time of each object into the debug map (`N_OSO` stabs) of `ntfs-3g`, the ntfsprogs, the probe and volmark (these link libntfs-3g objects). Those dates changed the binary, so its `LC_UUID`, its signing identifier (derived from the UUID) and its cdhash |
| no linker option for the UUID | ld already makes `LC_UUID` a hash of the output; with equal input it is equal |
| no `-ffile-prefix-map`/`-fdebug-prefix-map`, no `-oso_prefix` | the build directory is the same in every clone; for NTFS-3G a `CFLAGS` value would replace its default `-g -O2` and change the deps key (`--print-key`) of `run-inputs.sh` |
| no sorting of the payload | `pkgbuild` has no option for the order; equal trees give the same order |

Result, 2026-11-26, commit d64f32d with these changes (`--allow-dirty`), golden VM macOS 27.2 with
macFUSE 5.5.0 (libfuse 2.9.9): 28 items identical, 3 differ, all allowed, exit 0. Identical: every
payload file (`ntfsmacd`, `ntfsmac`, `ntfsmac-worker`, `ntfsmac-probe`, `ntfsmac-volmark`, `mkntfs`,
`ntfsfix`, `ntfsinfo`, `ntfslabel`, the sandbox profiles, the plist, `cli-requirement`,
`BUILD-INFO`, `COPYING`, `README.md`, `manifest.txt`, `uninstall.sh`), so every `LC_UUID` and cdhash;
the Bom, PackageInfo, Payload and Scripts archives, `Distribution`, and `BUILD-INFO`,
`uninstall.sh` and `manifest.txt` of the output directory. The differences:

| Item | Cause (rule) |
|---|---|
| xar table of contents of the `.pkg` | `xar-toc-times`: productbuild records the archive creation time and, per member, the inode, device, atime, mtime, ctime and Finder creation time of its own scratch files; there is no option to fix them. It also records the uid, user, gid and group that ran the build (the same in every clone; a build by another user differs there too). Every member is byte-identical and the tables are equal without these fields |
| the `.pkg` file | the same (`xar-toc-times`); the table of contents is compressed and checksummed at the start of the file |
| `SHA256SUMS` | `sums-follow-pkg`: only the line of the `.pkg` |

Before these changes (same check, run at e7fc055, whose build-pkg.sh and build-ntfs3g.sh are those of
d64f32d; T-M6-05 changed only how release-local.sh packs the tree): 17 identical, 14 differ, none
allowed. The four ntfsprogs, the worker, the probe and volmark differed in 26 to 31 debug-map
timestamps each, and with them in `LC_UUID`, signing identifier and cdhash (`macho-oso-mtime`);
`BUILD-INFO` in the payload and in the output directory differed in the `built` line
(`buildinfo-date`); so did the Bom, the Payload, the xar table of contents, the `.pkg` and
`SHA256SUMS`. `ntfsmacd` and `ntfsmac` were already identical (no `-g` objects).

Limits: the absolute build directory (`/Users/admin/ntfsmac/work/...`) is in the debug map of the
programs that link NTFS-3G objects, so a build with another `--work` or user (or `--host`) differs
there. The toolchain is an input: equal results need the same golden VM (Xcode tools, SDK, macFUSE
headers). `release-local.sh` puts the UTC date into its label (`-dev.<date>.`), so two of its runs on
different days differ in the file name, `Distribution` and `BUILD-INFO`; `repro-check.sh` uses a
label without a date.

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
  `org.ntfs-macos.pkg.v2` or `org.ntfs-macos.pkg`, no developer install), when such a path is a
  symbolic link, and when a directory above an installed path is a symbolic link or is not
  root-only (those are `/usr/local/libexec`, `/usr/local`, `/usr`, `/Library` and their parents;
  a user-owned `/usr/local/bin` or `/usr/local/share` does not stop the install). The path is
  walked from `/` down and no link is followed, so a link at `/usr/local/libexec` is refused even
  when it points to a root-only directory, and so is a user-owned `/usr/local` that holds it
  (audit M6-F01). A refusal names the directory, quoted for reading only, and says in words what
  to change; it never suggests a command built from a path (audit M6-F06).
  **Owner decision 2026-11-26:** it also refuses when another NTFS driver is installed:
  `ufsd_NTFS.fs` of Paragon, `tuxera_ntfs.fs` of Tuxera, any other `*ntfs*.fs` in
  `/Library/Filesystems` except Apple's `ntfs.fs` and our own `ntfsmac*` names (the same rule as
  the daemon's conflict detection), or an `*ntfs*.kext` in `/Library/Extensions`. The earlier design
  warned, installed and set passive mode (ARCHITECTURE.md section 8.4); the daemon still goes
  passive when such a bundle is installed later (not for a kext-only driver: a known gap, ARCHITECTURE.md
  section 8.4).
- An install of ours is upgraded: preinstall stops the running daemon (it unmounts its volumes),
  and removes the files an earlier install of ours had and this package does not ship (the earlier
  `/usr/local/bin/ntfsmac` file and `/usr/local/share/doc/ntfs-macos` included), but only where
  every directory from `/` down to the file's directory is a real directory (no symbolic link) and
  root-only; anything else is left and named in the log. It then forgets the receipt
  `org.ntfs-macos.pkg` of the earlier layout when none of its files that this package does not
  ship is left; otherwise that receipt stays, and the log says to run
  `uninstall.sh --old-receipt` after fixing the directory (see "Package identifier" below). If the daemon does not stop within 70 s the install stops before any file is copied;
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

## Package identifier (owner decision 2026-11-26, audit M6-F02)

The package identifier is `org.ntfs-macos.pkg.v2`. The earlier layout used `org.ntfs-macos.pkg`:
the published pre-release `v0.1.0-dev.20261116` (tools in `/usr/local/libexec/ntfs-macos`,
documents and `LICENSES` in `/usr/local/share/doc/ntfs-macos`) and the earlier developer packages.
With one identifier the macOS Installer treats a new package as an upgrade of the old receipt and
deletes, as root and outside every check of preinstall, the old receipt's files that the new
package does not ship. Those lie in `/usr/local/share/doc`, which a user may own (Homebrew from an
Intel Mac), and a link planted there would point that deletion at another directory. With another
identifier the Installer has no old receipt to compare and deletes nothing. The old files are
removed by preinstall and by `uninstall.sh` instead, each only below a root-only directory chain
with no link on the way. preinstall and `uninstall.sh` recognise both receipts as ours; the unit
test U-PKG-03 (`packaging.payload`) keeps the names in step. Options the owner did not choose:
`pkgutil --forget` of the old receipt in preinstall (it relies on the order in which the Installer
reads receipts), and refusing such an upgrade. Whether to withdraw the pre-release is the owner's
separate decision; its own `uninstall.sh` follows links and should not be used.

Note for later releases: an upgrade from one `org.ntfs-macos.pkg.v2` package to the next is again
an upgrade of the same identifier, so the Installer again deletes, itself and outside preinstall's
checks, the files of the old receipt that the new package does not ship. Today every such file lies
in a root-only directory of the project. A release that drops a directory, or moves a path into a
directory a user may own, needs a preinstall refusal (or its own clean-up under root_only_dir)
for leftover `.v2` files there before it is published, or another identifier.

## Uninstall

Run the `uninstall.sh` of the release that you downloaded and checked against its `SHA256SUMS`
(it is next to the package in `dist/<label>/` and in the release assets):

```sh
shasum -a 256 -c SHA256SUMS
sudo sh ./uninstall.sh
```

The installed copy, `sudo /usr/local/libexec/ntfs-macos/uninstall.sh`, is a convenience. If a
directory above it is not root-only, a user could have replaced it, and its own checks cannot help
then (issue #6).

Before it changes anything, it checks every directory from `/` down to each path of the manifest
(`dir`, `file`, `statedir`, `statefile`) without following a symbolic link. When one is a link or
is not owned by root and protected from group and others, it names it and stops: nothing is
removed, also with `--dry-run`. `--allow-unsafe-ancestors` goes on, but a path is still removed only
when every directory from `/` down to it passes that check; every other path is kept and named,
its receipt is kept, and the exit status is 1 (audit M6-F05). Then it stops the daemon, removes the
right, the files of the package receipts `org.ntfs-macos.pkg.v2` and `org.ntfs-macos.pkg` (each
must lie below a `dir` of the manifest or be exactly a `file` or `link` path of the manifest; for
the earlier receipt also one of the exact files that the pre-release put in
`/usr/local/share/doc/ntfs-macos`; anything else refuses the whole run) and every path of the
manifest, removes `/usr/local/bin/ntfsmac` only when it is a symbolic link to exactly
`/usr/local/libexec/ntfs-macos/ntfsmac` and `/usr/local/bin` is root-only (the same rule when a
receipt names that path; anything else there, such as the CLI file of the earlier layout, is kept
and named in the output), then forgets each receipt of which no file was kept by the directory
check. A path kept by the rule of the link (a file or a foreign link at `/usr/local/bin/ntfsmac`,
or our link in a `/usr/local/bin` that is not root-only) does not keep the receipt: it is named,
and it is not a file that a later run of this script would remove. With `--purge-state` the state
directory itself must pass the same check before its tree is removed. It never removes a
shared directory (`/usr/local/bin`, `/usr/local/share/doc`, `/Library/LaunchDaemons`), even an
empty one. `--dry-run` lists what it would remove.

`--old-receipt` removes only the earlier layout (audit M6-F04): the files of the receipt
`org.ntfs-macos.pkg` that the current install does not use, under the same rules, and then that
receipt; the current install stays. It replaces the pre-release's own uninstaller. It does not go
with `--allow-unsafe-ancestors` (refused as a usage error): it removes no path of the manifest, so
the check that option relaxes does not run, and each path of the earlier layout is checked by
itself in any case.

## Developer install from source

To build and install the same set from a checkout instead of the package:

```sh
packaging/dev-install.sh        # as your own user; it calls sudo for the install steps
/usr/local/libexec/ntfs-macos/ntfsmac doctor
sudo /usr/local/libexec/ntfs-macos/uninstall.sh
```

`dev-install.sh` refuses to install unless every directory above an installed path is a real
directory (no symbolic link on the way, also not one to a root-only directory) owned by root and
not writable by group or others (a user-owned `/usr/local/libexec` would let a user plant a program
that runs as root); the uninstaller refuses in the same case (see "Uninstall"). The check is the
same text in `preinstall`, `postinstall`, `uninstall.sh` and `dev-install.sh` (a package script
cannot source a repository file), and the unit test U-PKG-08 (`packaging.root_only`) keeps the four
copies equal. It
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
(I-PKG-03..04), `pkg_preflight_vm.sh --pkg-dir dist/<label>` (I-PKG-05),
`pkg_preflight_vm.sh --pkg-dir dist/<label> --case userlocal` (I-PKG-06) and
`pkg_preflight_vm.sh --pkg-dir dist/<label> --case oldrel --old-pkg FILE` (I-PKG-06, the published
pre-release first; FILE is a local copy, refused unless its sha256 is the published one). Each
starts one VM; run them through the VM slot protocol when other runs use the VMs. Host-side
checks: `ctest -R packaging` (U-PKG-01..08).
