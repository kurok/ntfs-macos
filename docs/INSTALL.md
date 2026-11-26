<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Install and uninstall

This page is for people who install the release package `ntfs-macos-<label>.pkg`. Read
[LIMITATIONS.md](LIMITATIONS.md) first: it lists what the product does not guarantee, durability
and access control included.

> **Pre-release.** The package is verified in virtual machines and on disk images. It has not been
> run against real USB drives yet (`docs/HARDWARE_TEST_PROTOCOL.md`). Keep a copy of every volume
> you care about until it has.

## Requirements

| Item | Requirement |
|---|---|
| Mac | Apple silicon (arm64). Intel Macs are not supported by this package. |
| macOS | 27 or newer |
| macFUSE | 5.5.0 or newer, installed, with its file system extension turned on in System Settings (General > Login Items & Extensions). Get it from <https://macfuse.github.io/>. ntfs-macos does not bundle or download macFUSE. |
| Other NTFS drivers | A Paragon or Tuxera driver, another `*ntfs*.fs` file system bundle or an `*ntfs*.kext` makes the installer refuse (owner decision of 2026-11-26; see "If the installer refuses"). Apple's own read-only NTFS driver is not a conflict. |

The installer stops with a reason if a requirement is not met (see "If the installer refuses"). It
checks that macFUSE is present and new enough; it does not check that the macFUSE extension is
approved (see "First use"). An upgrade over an install of this package works.

## Download and check

1. Open the [Releases](https://github.com/kurok/ntfs-macos/releases) page and download, from the
   release you want, into one folder:
   - `ntfs-macos-<label>.pkg`
   - `SHA256SUMS`
   - `SHA256SUMS.sig`
2. Get the project's release key, `packaging/release-signers`, from a clone of the repository, not
   from the release page (a release from before 2026-11-26 has no `SHA256SUMS.sig`; see below). If
   you already have a clone or a copy of the file from an earlier release, use that copy instead: it
   is the stronger check (see "What the signature proves" below).

   ```sh
   git clone https://github.com/kurok/ntfs-macos.git
   cp ntfs-macos/packaging/release-signers .
   ```

3. Check the signature, then the checksums, in Terminal, in the folder that holds the files:

   ```sh
   ssh-keygen -Y verify -f release-signers -I ntfs-macos-release -n ntfs-macos-release -s SHA256SUMS.sig < SHA256SUMS
   shasum -a 256 -c SHA256SUMS --ignore-missing
   head -1 SHA256SUMS
   ```

   - The first command must print
     `Good "ntfs-macos-release" signature for ntfs-macos-release with ED25519 key SHA256:...`. The
     `SHA256:...` part is the key fingerprint: it must be the one the release notes name under
     "Signing key fingerprint", and the same as for the releases you checked before.
   - The line for the `.pkg` must end with `OK`.
   - The last command prints `# release <label>`. That label, and the `<label>` in the name of the
     `.pkg` line, must be the release you chose to install (the version on the release page and in
     its notes). The label is part of the signed file, so an older signed release cannot be passed
     off as a newer one under another name.

   If any of this fails, delete the files and do not install.

### What the signature proves

| Check | Proves | Does not prove |
|---|---|---|
| `shasum -c SHA256SUMS` alone | the download is complete and not damaged | anything about who made it: whoever can replace the package on the release page can replace `SHA256SUMS` with it |
| the signature of `SHA256SUMS` | `SHA256SUMS`, and so every file it lists (the package, `uninstall.sh`, `manifest.txt`, `BUILD-INFO` and the source archive), and its release label were signed with the key in your `release-signers` | that this key is the owner's; see below |
| `pkgutil --check-signature` / `spctl` | only for a release that says it is signed with an Apple Developer ID (see below) | |

**Where the key comes from (trust on first use).** The owner keeps the private key off GitHub, but
the public key and the release notes that name its fingerprint are both on GitHub. Whoever controls
the GitHub account could change both together, and a first-time user who takes the key from a fresh
clone cannot tell. The owner has not yet published the fingerprint at a place that the GitHub account
does not control (owner decision pending: ADR-011, amendment 2026-11-26, and packaging/README.md
"Release signing key"). Until then the check protects you only if you already have an earlier copy
of `release-signers` (or noted its fingerprint) and the key has not changed since; on a first
download it proves no more than that the files belong together.

**What no check here can detect:** an attacker who controls the release page can keep showing an
older, genuinely signed release as the newest one ("frozen latest"). Each file checks out; only a
statement of the current version from a place off GitHub could show that a newer one exists, and
there is none yet (same owner decision).

The package is still not signed with an Apple Developer ID; the signature above is a separate check
that needs no Apple account. A release without `SHA256SUMS.sig` (the pre-release `v0.1.0-dev.20261116`)
can be checked only with `shasum`, which proves no more than the first row.

## Install

### Gatekeeper and the unsigned package (ADR-011)

Until a release says it is signed and notarized, the package is **not signed with an Apple Developer
ID and not notarized**. Gatekeeper therefore **blocks a double-click install** in Finder: macOS shows
a dialog that it cannot verify the package and does not open the installer (the exact wording
depends on the macOS release). That is expected for this package; it does not mean the download is
damaged.

The supported way to install it is the `installer` tool in Terminal. `installer` does not run the
Gatekeeper check that a double-click does: `sudo installer -pkg ... -target /` installed the unsigned
package with the `com.apple.quarantine` attribute set (exit 0, checked in a virtual machine on
macOS 27.2, test I-PKG-03; log in
`tests/results/evidence/pkg-release/2026-11-26-63ff4ef/i-pkg-03-04.txt`).
Check the download first (previous section), then:

```sh
sudo installer -pkg ntfs-macos-<label>.pkg -target /
```

Do not remove the quarantine flag or turn Gatekeeper off to make a double-click work; that weakens
the Mac for every program. If you do not want to install a binary you cannot verify through Apple,
build the package yourself (see "Build from source" below).

A release that is signed and notarized is installed the same way, and a double-click also works.
Check which kind you have with `spctl -a -t install -vv ntfs-macos-<label>.pkg` (`accepted` means
signed and notarized). The release notes say which it is.

### What is installed

| Path | Content |
|---|---|
| `/usr/local/libexec/ntfs-macos/` (0755) | `ntfsmacd` (the daemon), `ntfsmac` (the command line tool, ad hoc signed with the hardened runtime), `ntfsmac-worker` (NTFS-3G with this project's patch series), `ntfsmac-probe`, `ntfsmac-volmark`, `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel`, `uninstall.sh` (all 0755), and `manifest.txt` (0644) |
| `/Library/Application Support/ntfs-macos/` (0755) | `cli-requirement` (0644; written by the package's postinstall script from the installed CLI's cdhash) |
| `/Library/Application Support/ntfs-macos/sandbox/` (0755) | `probe.sb`, `volmark.sb`, `worker.sb` (0644) |
| `/Library/Application Support/ntfs-macos/doc/` (0755) | `COPYING`, `README.md`, `BUILD-INFO` (0644). There is no `LICENSES/` directory |
| `/Library/LaunchDaemons/org.ntfs-macos.ntfsmacd.plist` (0644) | the launchd job that runs the daemon as root |
| `/usr/local/bin/ntfsmac` | **optional** symbolic link to `/usr/local/libexec/ntfs-macos/ntfsmac`. The postinstall script makes it only when `/usr/local/bin` is owned by root and not writable by others and nothing exists at that path. Otherwise the install still succeeds and has no link (the install log says so) |
| authorization right `org.ntfs-macos.journal-modify` | in the system authorization database, not a file |
| `/var/db/ntfs-macos/`, `/var/log/ntfsmacd.log` | kept by uninstall unless `--purge-state`; made by the daemon at run time: the journal of volumes that were not unmounted cleanly, and the daemon log |

Everything installed is owned by root:wheel. Nothing is installed in `/usr/local/share`. The daemon
runs as root from launchd, because only root may open a disk device. It opens a volume read-only
first, and starts the NTFS-3G worker in a sandbox.

An upgrade from the earlier layout (a preview package, such as the pre-release
`v0.1.0-dev.20261116`) removes the old `/usr/local/bin/ntfsmac` file and
`/usr/local/share/doc/ntfs-macos`, but only where every directory on the way is a real directory
owned by root and not writable by others. This package has the identifier `org.ntfs-macos.pkg.v2`;
the earlier layout used `org.ntfs-macos.pkg`. When the old files were all removed, that old receipt
is forgotten. When some had to stay (for example in a `/usr/local/share/doc` that your user owns),
the install log names them and the old receipt stays: check those files, then run
`sudo /usr/local/libexec/ntfs-macos/uninstall.sh --old-receipt`. Do not run the pre-release's own
`uninstall.sh`; it follows symbolic links.

**Running `ntfsmac`.** The commands in this page are written as `ntfsmac`. That works when the link
`/usr/local/bin/ntfsmac` exists (check with `ls -l /usr/local/bin/ntfsmac`). If it does not, run the
tool by its full path, `/usr/local/libexec/ntfs-macos/ntfsmac`, or put the directory on your PATH
for the shell you use, for example in `~/.zshrc`:

```sh
export PATH="$PATH:/usr/local/libexec/ntfs-macos"
```

(That directory also holds the daemon and the engine tools; the link is the cleaner choice.) With
`sudo`, use the full path, because `sudo` does not read your `~/.zshrc`.

To see which build is installed:

```sh
cat "/Library/Application Support/ntfs-macos/doc/BUILD-INFO"
pkgutil --pkg-info org.ntfs-macos.pkg.v2
```

### If the installer refuses

The package checks the Mac before it copies anything and stops with a reason in these cases:

| Reason | What to do |
|---|---|
| macOS below 27, or not Apple silicon (arm64) | This Mac is not supported. |
| The target is not the startup volume | Install onto the startup volume (`-target /`). |
| macFUSE missing, below 5.5.0, or `/usr/local/lib/libfuse.2.dylib` missing | Install or update macFUSE from <https://macfuse.github.io/>, turn its extension on, run the installer again. |
| A Paragon or Tuxera NTFS driver, another `*ntfs*.fs` bundle, or an `*ntfs*.kext` is installed. The installer refuses (owner decision of 2026-11-26): two drivers could write to one volume | Remove the other driver with its own uninstaller, then install again. This project never removes another vendor's driver. |
| A path of the package already belongs to other software, a symbolic link stands at one of its paths, or a directory above them (`/usr/local/libexec`, `/usr/local`, `/Library/Application Support`, `/Library/LaunchDaemons` and their parents) is a symbolic link or is not root-only (owned by root and not writable by group or others). A link is refused even when it points to a root-only directory. A user-owned `/usr/local/bin` does not block the install; the package then makes no `ntfsmac` link | Do not force it. A user-writable `/usr/local/libexec`, or a link there in a user-writable `/usr/local`, would let a user plant a program that runs as root. The message names the directory; replace a link by a real directory, or give the directory to root and remove the write permission of group and others, or move the other software, then install again. |

An upgrade over an install of this package is not refused.

## First use

1. **Approve macFUSE once.** After you install macFUSE, turn its file system extension on in System
   Settings > General > Login Items & Extensions. Mounts fail with "File system extension not enabled"
   until you do.
2. **Check the installation.**

   ```sh
   ntfsmac doctor
   ```

   Every finding should be ok. A finding that says the macFUSE extension is not approved is step 1.
3. **Attach an NTFS drive.** When the daemon runs it takes over the drive before Apple's own
   read-only NTFS driver does, probes it (read-only) and mounts it under `/Volumes`. A clean volume
   mounts read/write. A volume that is dirty, hibernated (Windows Fast Startup), has an unclean log,
   or was left open by an earlier session of this product mounts read-only. Eject in Finder, or
   with `ntfsmac unmount`.
4. **Commands.** `ntfsmac` talks to the daemon; BSD names such as `disk4s1` come from `list`.

   | Command | What it does |
   |---|---|
   | `ntfsmac list` | show NTFS volumes |
   | `ntfsmac status <bsd>` | show one volume and its last check |
   | `ntfsmac mount [--ro] <bsd>` | mount a volume (read-only with `--ro`) |
   | `ntfsmac unmount [--force] <bsd>` | unmount a volume |
   | `ntfsmac doctor` | check the installation |
   | `sudo ntfsmac journal-clear [--label TEXT --serial HEX8] <bsd>` | clear a session journal entry (administrator) |
   | `sudo ntfsmac repair --clear-dirty [--label TEXT --serial HEX8] <bsd>` | clear the dirty flag (administrator) |
   | `sudo ntfsmac repair --sync-mirror [--label TEXT --serial HEX8] <bsd>` | repair a stale MFT mirror (administrator) |

   `journal-clear` and `repair` ask for the label shown by `list` and the last 8 hex digits of the
   serial. They need `sudo` in the current release. Use them only when you know why a volume is
   read-only; Windows `chkdsk` is the safer way to make a volume clean again.
5. **Always unmount or eject before you unplug.** Unmount writes all data the file system
   received and asks the drive to flush; whether the drive confirmed it is shown by the session
   report ([LIMITATIONS.md](LIMITATIONS.md), N1 to N5). If unmount says the volume is busy, close the
   programs that use it; `--force` unmounts anyway, and the volume may then be mounted read-only next time.

`diskutil mount` is not routed to this product; use `ntfsmac mount`.

## Uninstall

Use the `uninstall.sh` that came with the release you downloaded, after checking it against the
release's `SHA256SUMS` (previous sections), from the directory that holds both:

```sh
shasum -a 256 -c SHA256SUMS
sudo sh ./uninstall.sh
```

The installed copy, `sudo /usr/local/libexec/ntfs-macos/uninstall.sh`, does the same and is a
convenience. If a directory above it were not root-only, another user could have replaced it, so
the checked copy is the safer one.

The script reads the manifest next to it and removes exactly the paths that it lists: the files
above, the launchd job (it is booted out first) and the authorization right. It checks every path
first and stops with nothing removed if one lies outside the directories that the manifest names,
or if a directory above one of them is a symbolic link or is not owned by root and protected from
other users. It names that directory; fix it, then run the script again.
`--allow-unsafe-ancestors` removes the rest anyway, but never a path below such a directory: those
are kept and named, and the script exits 1. A directory is removed only if it is empty afterwards.
For a package installed with `installer` it also removes the files of the package receipt
(`org.ntfs-macos.pkg.v2`, and `org.ntfs-macos.pkg` of the earlier layout, including the
pre-release's documents in `/usr/local/share/doc/ntfs-macos`) and then each receipt. Add
`--dry-run` to see what it would remove without removing anything. `--old-receipt` removes only
what is left of the earlier layout and keeps the current install.

The journal of unsafe volumes (`/var/db/ntfs-macos`) and the daemon log stay unless you add
`--purge-state`. Keep them if you may mount a volume again: the journal is what makes the next
mount read-only after an unclean session.

macFUSE stays installed. Remove it with its own uninstaller if you no longer need it.

## Build from source

Use this if you want a package or an install you built yourself, which is also the way around the
Gatekeeper block on an unsigned package.

Prerequisites: a Mac with macOS 27 on Apple silicon, macFUSE 5.5.0 or newer installed (the worker
links its `libfuse`), Xcode command line tools, CMake 3.28 or newer, and Ninja (optional). Check with
`scripts/bootstrap.sh`; it installs nothing.

```sh
git clone https://github.com/kurok/ntfs-macos.git
cd ntfs-macos
scripts/bootstrap.sh                 # checks the prerequisites
scripts/fetch-deps.sh ntfs-3g        # downloads the pinned NTFS-3G tarball and checks its SHA-256
packaging/dev-install.sh             # builds, installs and loads the job; run as your own user
ntfsmac doctor
```

`dev-install.sh` calls `sudo` for the install steps and installs the layout of
`packaging/manifest.txt`. To build the `.pkg` instead of installing, use `packaging/build-pkg.sh`
(output in `dist/<label>/`); `scripts/release-local.sh` does that in a disposable virtual machine and
tests the result. Details of both, and of the unsigned path, are in
[packaging/README.md](../packaging/README.md).

The binaries are licensed under GPL-2.0-or-later. A release lists the commit it was built from
(`source` in `BUILD-INFO`) and its tag. The corresponding source is the source release archive
`ntfs-macos-<label>-source.tar.gz`, which is published next to every binary package (criterion 6.5,
T-M6-05). It holds, under one directory `ntfs-macos-<label>-source/`:

- the file set that the build got, as one git tree (its id is in `SOURCES.txt`): the release commit's
  tree, or, for a build with `--allow-dirty` and for a host build, the tree of the tracked files, the
  force-added ignored files and the new files that are not ignored, with their contents as they were
  when the build started (the sources, the build scripts, the documents),
- the NTFS-3G patch series in `third_party/patches/ntfs-3g/`,
- the unmodified NTFS-3G release tarball in `third_party/cache/`, the one pinned in
  `third_party/deps.lock`,
- `SOURCES.txt`, which states the GPL licence and the corresponding-source statement and lists the
  base commit, the tree id, whether the tree differs from the commit (dirty) with the changed paths,
  the tarball digest, the patches and the git version.

macFUSE (libfuse and its headers) is a system prerequisite and is not part of the archive; install
macFUSE 5.5.0 or newer from <https://macfuse.github.io/> first. The build finds it with
`pkg-config fuse`.

`scripts/make-source-release.sh` makes the archive, checks the tarball against its pin (it never
downloads), and writes the archive and its line in `SHA256SUMS` into `dist/<label>/`.
`scripts/release-local.sh` makes that tree once before it builds, gives it to the build (sent to the
VM, or extracted and built on the host, never the live working tree), and archives the same tree id
after the build for the commit it read at the start. An edit made during the build changes neither.
If the archive fails, or `HEAD` moved, it removes the package, so a package never exists without its
source. A tree that differs from the commit gets the label suffix `.dirty` in the label, `BUILD-INFO`
and `SOURCES.txt`. `packaging/sign-and-notarize.sh` checks the archive against its `SHA256SUMS` line,
copies it into the signed output set and lists it in the new `SHA256SUMS`, so the signed release has
it too. The same tree and the same git version give the same bytes. To build from it:

```sh
tar -xzf ntfs-macos-<label>-source.tar.gz
cd ntfs-macos-<label>-source
shasum -a 256 third_party/cache/ntfs-3g_ntfsprogs-*.tgz      # equals the pin in third_party/deps.lock
# base commit, its time and the label are in SOURCES.txt; the label names the package
NTFSMAC_SOURCE_REV=<base commit> SOURCE_DATE_EPOCH=<commit time> packaging/build-pkg.sh --label <label>
```

The archive is not a git clone; `NTFSMAC_SOURCE_REV` makes `BUILD-INFO` of the package name the base
commit, and `SOURCE_DATE_EPOCH` (the commit time) is its date and the time of every file in the
package. Without the two variables `build-pkg.sh` reads both from `SOURCES.txt`; it never takes them
from a git repository that merely contains the extracted directory. A build from the archive gives the
same bytes as the released package only with the same work directory and user (the absolute build
path is in the debug map of the programs that link NTFS-3G) and the same toolchain image (the golden
VM: Xcode tools, SDK, macFUSE headers); see "Reproducibility" in
[packaging/README.md](../packaging/README.md). `packaging/dev-install.sh` reads the commit from git and records `unknown` here, so use
`build-pkg.sh` for a build from the archive.

## Problems

Open an issue at <https://github.com/kurok/ntfs-macos/issues>. Include the contents of
`BUILD-INFO`, your macOS version, your macFUSE version and the output of `ntfsmac doctor`. Do not
post the details of a security problem in a public issue; open an issue that asks for a private
contact instead.
