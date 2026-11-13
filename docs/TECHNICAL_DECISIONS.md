# Technical decisions

ADR-style records. Source tags `[V:Sn]`, `[A]`, `[U]` are defined in [ARCHITECTURE.md](ARCHITECTURE.md) (labels at the top, sources in section 12).
Status values: **Accepted**, **Accepted, gated** (a named task can overturn it), **Deferred**.

| ADR | Topic | Decision | Status |
|---|---|---|---|
| 001 | Backend | macFUSE 5.5.0 FSKit backend; native FSKit port is the fallback | Accepted, gated by T-M2-03/04 |
| 002 | NTFS engine | NTFS-3G 2026.9.28, minimal patch series | Accepted |
| 003 | Disk Arbitration integration | Root daemon with DA callbacks | Accepted, gated by T-M4-02 |
| 004 | Privilege model | Root broker passes a device fd to unprivileged sandboxed workers | Accepted, gated by T-M2-05, T-M4-01 |
| 005 | Unsafe volume states | Own read-only probe; `norecover` always; never `remove_hiberfile` | Accepted |
| 006 | Unclean-session marker | Host journal now; on-disk dirty flag after Opus review | Accepted (journal), gated (flag) |
| 007 | Build system | CMake + Ninja for our code; upstream autotools for NTFS-3G; C11 only | Accepted |
| 008 | Test VM tooling | tart for macOS guests; Docker or Podman for Linux | Accepted, gated by T-M1-04 |
| 009 | Intel support | Not supported; arm64 only | Accepted |
| 010 | Licence | GPL-2.0-or-later for the whole repository | Accepted |
| 011 | Signing and distribution | Developer ID + notarized `.pkg`; unsigned developer builds until credentials exist | Accepted |
| 012 | Permissions, xattrs, names | uid/gid mapping, xattrs as named streams, `windows_names` | Accepted |
| 013 | macFUSE distribution | Prerequisite, never bundled | Accepted |
| 014 | Device sync | `F_FULLFSYNC` at sync points | Accepted |

---

## ADR-001 Backend: how NTFS-3G reaches the macOS VFS

**Context.** The brief prefers the FSKit user-space backend and allows the kernel backend only if FSKit cannot deliver.

**Options.**

| | Option | For | Against |
|---|---|---|---|
| A | NTFS-3G on **macFUSE FSKit backend** | No kext, no Reduced Security. macFUSE's module is already signed and notarized, so this project needs no restricted entitlement and no Xcode to develop [V:S3]. NTFS-3G's FUSE glue is reused unchanged. 5.5.0 targets the macOS 27 SDK, adds Finder eject for `local` volumes, read-only mounts, `FUSE_FLUSH` on close, a corruption mitigation [V:S1]. Works in VMs in principle (user space). | FSKit backend is not at parity: no FUSE notification API, most kernel mount options missing, slower than the kext [V:S2]. No per-file fsync; close errors lost [V:S1]. libfuse2 support for `backend=fskit` is unproven [V:S5][V:S6][V:S9]. Closed-source module. User must install macFUSE and approve its extension. Volume is not owned by Disk Arbitration as a block-device file system, so automount needs our daemon. |
| B | NTFS-3G on **macFUSE kernel backend** | Full feature set, best performance, real fsync path, 15 years of use [V:S2]. | Apple silicon needs Recovery Mode and Reduced Security to allow third-party kexts [V:S3]. Kexts do not load in VMs, so Environment B could not test it [V:S3]. Conflicts with "no unnecessary kernel modifications". |
| C | NTFS-3G on **fuse-t** (NFS or its FSKit module) | Already on the host (1.2.9, module `org.fuset.fskit-srv.module`) [V:S17]. libfuse-compatible. | Closed source. Binary licence: free for non-commercial use, commercial use or bundling needs a paid licence [V:S17]. NFS backend has NFS client caching semantics. Not named by the brief. A second closed dependency with a smaller user base. |
| D | **Native FSKit module** linking `libntfs-3g` (no FUSE layer): `FSUnaryFileSystem` + `FSBlockDeviceResource` | Removes the macFUSE dependency. FSKit hands the module the block device, so no root broker is needed [V:S16]. Proper `fsck`/format hooks (`FSManageableResourceMaintenanceOperations`). Commercial products ship this design (vendor claims) [V:S19]. | New code: every `FSVolume` operation must be re-implemented against `libntfs-3g` (the equivalent of `src/ntfs-3g.c`, about 4,500 lines of data-path glue), which is exactly where data-loss bugs come from. Needs full Xcode and the restricted entitlement `com.apple.developer.fskit.fsmodule`, authorised by a provisioning profile, so an Apple Developer Program membership is needed even to run a development build [A: stated in an Apple forum answer surfaced by a web search and not opened directly; whether a Developer ID profile can carry the entitlement is U]. Automount is still masked by Apple's read-only NTFS kext, and third-party modules had permission failures on physical disks in 2025 with no fixed release named [V:S14]. Same FSKit durability limits as A. `libntfs-3g` is GPL, which is incompatible with Mac App Store distribution terms [A]. |

**Decision.** Option **A**. It is the brief's preference, it has the least new data-path code, and it is the only option that can start today without a paid developer account or Xcode.

**Gate and fallback order.** T-M2-03 and T-M2-04 must show a read/write mount with hash-verified remount in the macOS 27 VM.

1. If libfuse2 rejects `backend=fskit`: port the FUSE glue of `ntfs-3g.c` to the libfuse3 high-level API (task T-M2-03b, Opus-reviewed; mechanical signature changes in about 40 callbacks). libfuse3 3.18.3 ships with macFUSE 5.5.0 and is documented to support the option [V:S1][V:S5].
2. If the FSKit backend itself cannot carry NTFS-3G correctly (data mismatch, hangs, no usable sync point) after 3 Sonnet attempts and 1 Opus analysis: stop, write `CHECKPOINT.md`, and ask the human to choose between option D (needs Xcode + Developer Program) and option B as an opt-in power-user mode. Do not start D without that decision.
3. Option C is used only as an optional comparison target in tests, never shipped.

**Consequences.** The probe, policy, journal, CLI and test suite are backend-independent and are reused if option D is taken later. Durability limits N1 to N5 in ARCHITECTURE.md section 9 are permanent for A and D.

## ADR-002 NTFS engine

**Context.** The engine must be mature, open source, and read/write.

| Option | Assessment |
|---|---|
| NTFS-3G 2026.9.28 | Exists; stable release of 28 Sep 2026; preceded in 2026 by two security releases (2026.7.7, 2026.9.18) and 2026.2.25 with CVE fixes [V:S10]. Actively hardened against crafted volumes. Darwin is a supported `configure` target: external FUSE by default, `fuse >= 2.6.0`, CoreFoundation-based Unicode normalisation (`nfconv`) on by default [V:S11]. Has macFUSE-specific hooks (`getxtimes`, `setcrtime`, `setchgtime`, xattr `position`) [V:S11]. |
| Linux `ntfs3` (Paragon, GPL-2.0) | Kernel code tied to Linux VFS and page cache. Porting is a rewrite. Rejected. |
| Apple `ntfs.fs` | Read-only. Not usable. |
| New implementation | Forbidden by the brief. |

**Decision.** NTFS-3G **2026.9.28**, source tarball pinned by SHA-256 (IMPLEMENTATION_PLAN.md section 3). ntfsprogs are built from the same tarball.

**Patch policy.** Patches live in `third_party/patches/ntfs-3g/NNNN-*.patch`, are applied by `scripts/build-ntfs3g.sh`, and each has a header with rationale, upstream status, and the test that covers it. Planned series:

| Patch | Purpose | Needed |
|---|---|---|
| 0001 | Build fixes for macOS 27 + macFUSE 5.5.0 (pkg-config module name, header paths, warnings) | Only if T-M2-03 shows a need |
| 0002 | `F_FULLFSYNC` in the Darwin device-sync path | Yes (ADR-014) |
| 0003 | Accept an inherited descriptor as the device | Only if `/dev/fd/N` fails (T-M2-05) |
| 0004 | Test-only fault-injection device layer, compiled with `--enable-faultio`, absent from release builds | Yes (TEST_STRATEGY.md section 7) |
| 0005 | libfuse3 port of `ntfs-3g.c` | Only per ADR-001 fallback 1 |

Anything touching allocation, runlists, MFT or index code is out of scope for this project and goes upstream as a bug report instead.

**Consequences.** We inherit NTFS-3G semantics: no journal replay, single-threaded, POSIX namespace by default. Security updates require a new pin and a full test run.

## ADR-003 Disk Arbitration integration

**Context.** With option A the mounted volume is a FUSE volume, not a block-device file system chosen by `diskarbitrationd`. Something must notice disks and start mounts.

| Option | For | Against |
|---|---|---|
| Root LaunchDaemon with DA callbacks (chosen) | Public API. Can block Apple's read-only automount with a mount-approval dissenter and mount read/write instead. Natural place for the root-only device open. | A long-running root process. Dissenter behaviour on macOS 27 is [A] (T-M4-02). |
| Legacy bundle `/Library/Filesystems/ntfs3g.fs` with probe and mount executables | `diskarbitrationd` itself runs our probe and mount helper; `diskutil mount` works. Paragon uses this mechanism on this host [V:S17]. | Legacy mechanism; `mount -F` exists for "developers migrating from a traditional Filesystems bundle to FSKit" [V:S17], which signals its direction. DA expects the helper to mount the device node itself; a FUSE mount behind it is [U]. Probe-order races with Apple's `ntfs.fs` (1000) and other vendors (Paragon uses 1). |
| LaunchAgent in the user session | No root daemon. | Cannot open `/dev/diskN` (root-owned) without a privileged helper anyway. |
| Manual CLI only | Simplest. | Fails the "removable-drive support" and Finder goals. |

**Decision.** LaunchDaemon. The legacy bundle is evaluated as a time-boxed spike (T-M4-09) and adopted only if it gives `diskutil mount` support without a regression.

**Consequences.** Passive mode when another NTFS driver is present (ARCHITECTURE.md section 8.4). With no GUI session, automount is off and only root or `admin` can mount through the CLI.

## ADR-004 Privilege model

**Context.** Whole-disk and partition nodes are owned by root, so something privileged must open them. NTFS-3G's parser handles hostile input and had many memory-safety fixes in 2026 [V:S10]. `ntfs-3g` refuses to run setuid with external FUSE [V:S11].

| Option | Assessment |
|---|---|
| Run `ntfs-3g` as root (historical macOS practice) | A parser exploit is a root compromise. FSKit modules are enabled per user and root-run FSKit mounts failed for others [V:S14]. Rejected. |
| `chown` the device node to the user | Described by Apple as a testing hack that must not ship [V:S14]. Rejected. |
| Root broker opens the device and passes the fd to an unprivileged, sandboxed worker (chosen) | Parser never has root. Read-only decisions are enforced by the descriptor mode. macFUSE 5.2.0+ supports unprivileged and sandboxed servers through its XPC mount API [V:S6]. |

**Decision.** Broker + fd passing. Worker uid is the console user, so the user's approval of the macFUSE extension applies.

**Consequences.** Requires U5 and U6 to resolve positively. If the sandbox profile cannot be made to work, uid separation alone remains and the residual risk is recorded (R-07).

## ADR-005 Handling hibernated, Fast Startup and dirty volumes

**Context.** NTFS-3G's own behaviour [V:S11]:

- `recover` is the **default**: on an unclean `$LogFile` it calls `ntfs_logfile_reset()` and mounts read/write, discarding the Windows journal.
- `norecover` refuses instead.
- A hibernated volume (`hiberfil.sys` with `hibr` header, or shorter than 4096 bytes) fails with `EPERM`; `remove_hiberfile` deletes the file and continues.
- A `$LogFile` restart page of version 2.0 (Windows 8+ Fast Startup metadata cache) returns `EPERM` and is rejected even with `recover`.
- When `NTFS_MNT_MAY_RDONLY` is set (read/write requested, no `remove_hiberfile`), the library falls back to read-only instead of failing.

**Decision.**

1. The decision is made **before** NTFS-3G starts, by `ntfsmac-probe` and the policy table. NTFS-3G's internal fallback is defence in depth, not the mechanism.
2. Every worker invocation carries `norecover`.
3. `remove_hiberfile` and `recover` are not reachable through any interface of this product, including configuration. The daemon's option builder has no code path that emits them, and a unit test asserts it.
4. Unsafe state means read-only with an `O_RDONLY` descriptor.
5. Manual repair is exposed only as `ntfsmac repair --clear-dirty <bsd>` (wrapper for `ntfsfix -d`), requires typing the volume label, prints that this is not a consistency check, and is never suggested as the first remedy. There is no wrapper for hibernation-file removal.

**Consequences.** Users of dual-boot or Fast Startup machines get read-only mounts until Windows is shut down fully. This is intended.

## ADR-006 Marking an unclean session

**Context.** If the disk is pulled during a read/write session, nothing on the volume says so unless NTFS-3G or this product sets `VOLUME_IS_DIRTY`. Whether NTFS-3G sets it is [U9].

| Option | For | Against |
|---|---|---|
| Host journal only | No extra on-disk writes. | Invisible to Windows and to other Macs. |
| On-disk dirty flag set by the worker (patch) | One process. | Changes NTFS-3G's mount path. |
| On-disk dirty flag set by a separate short `libntfs-3g` step before the worker starts and cleared after it exits cleanly (chosen for phase 2) | NTFS-3G unpatched. Uses the library's own `ntfs_volume_write_flags`. Windows offers `chkdsk` after an unclean detach. | Two extra short open/close cycles per session. A crash between clear and journal removal must be handled (order: clear flag, full sync, then remove journal). |

**Decision.** Phase 1 (M3): host journal only. Phase 2 (M3 exit): add the on-disk flag via the separate step, after T-M3-01 establishes NTFS-3G's own behaviour and an Opus review of the write ordering (T-M3-12). If NTFS-3G already sets and clears the flag, phase 2 is dropped.

**Consequences.** After any unclean detach the volume is read-only on this product until Windows `chkdsk` (or the explicit repair command) clears the flag. Documented prominently.

## ADR-007 Build system and language

**Context.** Host has Command Line Tools only (no `xcodebuild`); Homebrew is present. Core logic must also build on Linux for Environment A.

| Option | Assessment |
|---|---|
| Xcode project | Needs full Xcode; not buildable on Linux. Rejected for A; would be required for ADR-001 option D. |
| SwiftPM | Works with CLT, but XCTest availability without Xcode is uncertain and Linux parity adds toolchain weight. Rejected. |
| Plain Make | Fine for small trees; weak for presets, sanitizer variants and cross-platform tests. |
| CMake >= 3.28 + Ninja, CTest (chosen) | Presets for `release`, `asan`, `ubsan`, `fuzz`, `linux`. Runs with CLT. |

**Decision.** C11 for all project code. CMake + Ninja + CTest. NTFS-3G is built with its own `configure`/`make` from the release tarball (which ships a generated `configure`, so autoconf is not required unless patch 0001 touches `configure.ac`), driven by `scripts/build-ntfs3g.sh` into `build/deps/`. Test drivers are POSIX `sh` and Python 3 standard library only.

**Consequences.** `cmake`, `ninja`, `pkgconf` must be installed on the host (human approval, IMPLEMENTATION_PLAN.md section 5). No Swift, no Objective-C in v1.

## ADR-008 Test VM tooling

| Option | Assessment |
|---|---|
| tart | CLI over Virtualization.framework; macOS and Linux guests; `tart clone` is a copy-on-write copy usable as a disposable snapshot; `--disk` attaches extra disk images as block devices [A: from tart documentation knowledge; T-M1-04 verifies on the current release]. macOS 27 guest images reported, need a macOS 26+ host for the ASIF format [V:S20, secondary]. Licence and current maintainer are [U11]. |
| lume (trycua) | MIT-licensed alternative with a similar feature set [A]. Fallback. |
| UTM | GUI-first; scripting is weaker. Fallback for a Windows 11 ARM guest in Environment C. |
| Own Virtualization.framework runner | Needs Xcode. Rejected. |
| Docker or Podman (Linux only) | Environment A. Docker CLI is present, daemon stopped [preflight]. |

**Decision.** tart for Environment B; Docker (or Podman) for Environment A, with a tart Linux guest as fallback if no container runtime can be started. T-M1-04 confirms tart's licence permits this use at zero cost and that a macOS 27 guest boots; otherwise switch to lume.

**Known VM limits.** Kexts do not load in VMs [V:S3] (irrelevant to option A). USB hot-unplug is simulated by force-detaching the disk image; macOS 27 adds USB accessory passthrough for guests [V:S23] but tool support is [U]. Real unplug, real drive write caches and real Finder-with-hardware behaviour need Environment C.

## ADR-009 Intel support

**Context.** The brief asks for Intel "where technically feasible" and a minimum OS of macOS 27.

**Facts.** macOS 26 was the last release for Intel Macs; macOS 27 is Apple silicon only [V:S22, press; confirm on Apple's page]. GitHub's macOS 27 runner image is arm64 only [V:S21].

**Decision.** arm64 only. No x86_64 slice is built or tested. Code stays portable C so that a later decision to support macOS 26 on Intel is a build-matrix change, not a rewrite.

**Consequences.** "Intel support" is reported as not applicable in the release-readiness report.

## ADR-010 Licence of this repository

**Facts.**

| Component | Licence | Source |
|---|---|---|
| `libntfs-3g`, `ntfs-3g`, ntfsprogs | GPL-2.0-or-later (file headers: "either version 2 of the License, or (at your option) any later version") | [V:S11] |
| macFUSE `libfuse` | LGPL | [V:S8] |
| macFUSE binaries (FSModule, kext, frameworks) | BSD-style terms plus: binary redistribution bundled with commercial software, including automated download or installation, needs written permission. Kext and other parts closed source since 3.9. | [V:S8] |
| fuse-t binary | Free for non-commercial use; commercial use or bundling needs a licence | [V:S17] |

`ntfsmac-probe` links `libntfs-3g` statically and is therefore a GPL work. The worker is NTFS-3G itself. The daemon and CLI link neither, but ship in the same package and share headers.

| Option | Assessment |
|---|---|
| GPL-2.0-or-later for everything (chosen) | One licence, compatible with NTFS-3G, lets patches go upstream unchanged. |
| GPL-2.0-only | No benefit; narrows compatibility. |
| GPL-3.0-or-later | Allowed by "or later", but patches could not go back to a GPL-2.0+ upstream without relicensing. |
| Permissive for daemon/CLI, GPL for probe | Two licences to track for no user benefit. |

**Decision.** **GPL-2.0-or-later** for all code in this repository. `LICENSES/` carries GPL-2.0, LGPL-2.1 (libfuse notice), and the macFUSE licence text as a third-party notice. Source files carry `SPDX-License-Identifier: GPL-2.0-or-later`.

**Consequences.** Every binary release must offer corresponding source, including the exact NTFS-3G tarball and patch series. Mac App Store distribution is out of scope.

## ADR-011 Signing, notarization, distribution

**Facts.** Option A needs no restricted entitlement: the only FSKit module involved is macFUSE's, which is signed and notarized by its developer [V:S3]. macFUSE supports unsigned FUSE server binaries [V:S1, 5.5.0 fixed a crash with unsigned binaries]. `pkgbuild`, `productbuild`, `codesign` are part of base macOS; `notarytool` and `stapler` come with Xcode [A].

**Decision.**

| Stage | Signing |
|---|---|
| Development and VM tests (M2 to M5) | Ad-hoc signatures (the arm64 linker default). LaunchDaemon installed with `sudo` inside the VM. |
| Release (M6) | Developer ID Application for binaries with hardened runtime; Developer ID Installer for the `.pkg`; notarize with `notarytool`; staple. Credentials stay in the human's keychain and are passed by keychain profile name, never stored in the repo or CI secrets without the human's decision. |
| If no credentials are available | Ship an unsigned `.pkg` plus build-from-source instructions, state clearly that Gatekeeper will block a double-click install, and mark M6 as "complete, unsigned". |

**Consequences.** Xcode and a paid Developer Program membership are human blockers for a notarized release only.

## ADR-012 Ownership, permissions, extended attributes, names

| Topic | Decision | Reason |
|---|---|---|
| Ownership | All files appear owned by the console user (`uid=`, `gid=`), `dmask=0022`, `fmask=0133`. | Matches how macOS treats removable FAT/exFAT volumes (Apple's exFAT module sets `FSVolumeAlwaysIgnoreOwnership` [V:S17]). NTFS ACLs are not modified. |
| Full POSIX mapping | Opt-in only: `permissions` or `usermapping=` with a `.NTFS-3G/UserMapping` file [V:S11]. Not tested in v1 beyond a smoke test. | Mapping errors can lock Windows users out of files. |
| Extended attributes | `streams_interface=xattr`: macOS xattrs are stored as NTFS named data streams [V:S11]. | Keeps Finder metadata on the volume without `._` files. T-M3-08 verifies; if Finder copy fails, fall back to `streams_interface=none` and document xattrs as unsupported. |
| Alternate data streams | Readable and writable through the xattr interface. `streams_interface=windows` (`file:stream` syntax) is not offered. | `:` handling in macOS paths is error-prone. |
| File names | `windows_names` on: creation of names Windows cannot open is refused [V:S11]. `nfconv` normalisation stays at NTFS-3G's Darwin default [V:S11]. Case: NTFS-3G is case-sensitive in the POSIX namespace; no `ignore_case` (that is `lowntfs-3g` only) [V:S11]. | Interoperability first. The case-sensitivity difference from Windows is documented and tested (two names differing only in case). |
| Timestamps | `noatime`. Creation time through the macFUSE `getxtimes`/`setcrtime` hooks [V:S11]; whether the FSKit backend forwards them is [A], covered by T-M3-06. | |
| Compression, EFS | Read and update of compressed files as NTFS-3G supports; no new compressed files (`nocompression` is considered in T-M3-09); EFS files unreadable (no `efs_raw`). | |

## ADR-013 macFUSE as a prerequisite, not a bundled component

**Decision.** The installer does not contain macFUSE and does not download it. Preflight checks that macFUSE >= 5.5.0 is installed and that its FSKit extension is enabled, and otherwise explains what to do and links to the official download.

**Reason.** The macFUSE licence restricts bundling and automated installation in a commercial context [V:S8]; this project is non-commercial, but a prerequisite model removes any ambiguity for forks and redistributors, keeps macFUSE updates in its own updater, and keeps closed binaries out of a GPL source release.

**Consequences.** Two installs for the user. The minimum version is enforced at daemon start and reported by `ntfsmac doctor`.

## ADR-014 Device sync

**Decision.** At every sync point (FUSE fsync/fsyncdir, flush on close, unmount, and the daemon's post-exit step) the device descriptor gets `fcntl(F_FULLFSYNC)`, falling back to `fsync` when the device rejects it. The `sync` mount option of NTFS-3G (synchronous device writes) is evaluated in T-M3-10 and enabled if the throughput cost is below 30 percent on the sequential-write benchmark.

**Reason.** Guarantee D1 depends on the drive cache being flushed; plain `fsync` on macOS does not do that.

**Consequences.** Slower close on spinning USB disks. Measured and reported in the benchmark output.
