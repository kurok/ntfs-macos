# Technical decisions

ADR-style records. Source tags `[V:Sn]`, `[A]`, `[U]` are defined in [ARCHITECTURE.md](ARCHITECTURE.md) (labels at the top, sources in section 12).
Status values: **Accepted**, **Accepted, gated** (a named task can overturn it), **Deferred**.

| ADR | Topic | Decision | Status |
|---|---|---|---|
| 001 | Backend | macFUSE 5.5.0 FSKit backend; native FSKit port is the fallback | Accepted, gated by the M2 gate (T-M2-03/04 done; audit T-M2-10) |
| 002 | NTFS engine | NTFS-3G 2026.9.28, minimal patch series | Accepted |
| 003 | Disk Arbitration integration | Root daemon with DA callbacks | Accepted, gated by T-M4-02 |
| 004 | Privilege model | Root broker passes a device fd to unprivileged sandboxed workers | Accepted, gated by T-M4-01 (T-M2-05 passed with patch 0003) |
| 005 | Unsafe volume states | Own read-only probe; `norecover` always; never `remove_hiberfile` | Accepted |
| 006 | Unclean-session marker | Host journal plus on-disk dirty flag set by `ntfsmac-volmark` around every read/write session (default plan); `FORENSIC` library mount so only the `$Volume` record is written; stated crash outcome per write | Accepted; flag enabled when T-M3-12 passes |
| 007 | Build system | CMake + Ninja for our code; upstream autotools for NTFS-3G; C11 only | Accepted |
| 008 | Test VM tooling | tart for macOS guests; Docker or Podman for Linux | Accepted, gated by T-M1-04 |
| 009 | Intel support | Not supported; arm64 only | Accepted |
| 010 | Licence | GPL-2.0-or-later for the whole repository | Accepted |
| 011 | Signing and distribution | Developer ID + notarized `.pkg`; unsigned developer builds until credentials exist | Accepted |
| 012 | Permissions, xattrs, names | uid/gid mapping (display only, not access control), xattrs as named streams through `streams_interface=openxattr`, `windows_names` | Accepted |
| 013 | macFUSE distribution | Prerequisite, never bundled | Accepted |
| 014 | Device sync | Upstream `ntfs_fsync()` already uses `F_FULLFSYNC`; its silent `fsync` fallback is reported, not hidden; patch 0002 withdrawn | Accepted |
| 015 | Local-only CI | `scripts/ci-local.sh`; no GitHub-hosted CI, no published binaries | Accepted |
| 016 | Disk-derived strings | Sanitised label as mount-point leaf only, never in the option string (`volname` dropped); final argv validator; strict probe-verdict parser with a grammar for every field; file names only from parsed integers; reason codes instead of text | Accepted |
| 017 | Clearing the session journal; repair | Four clearing paths; evidence rule off until verified; repair through `ntfsmac-volmark`, not `ntfsfix` | Accepted (evidence rule gated by U12; U13 confirmed by T-M3-01) |
| 018 | Device-error reporting and clean-session proof | Patch 0006 session channel: positive `S` and `U` records, `E` and `W` records, bounded, `FD_CLOEXEC`; silence is never clean | Accepted, gated by T-M3-18 |
| 019 | Authorising journal clearing and repair | Peer is not a spawned process, is the signed CLI, and an administrator authenticates per request through Authorization Services; typed confirmation is only a guard against mistakes | Accepted, gated by T-M4-04 (U17) |
| 020 | Binding the probed volume to the opened device | Probe descriptor reused for `ro`; for `rw` same device number and boot-area hash, expected serial and size checked by volmark, no other mount | Accepted, gated by T-M4-02 (U18) |
| 021 | Stale `$MFTMirr` after an interrupted `$MFT` update | Probe state `mirror_split` under a strict rule; policy `refuse`; explicit `ntfsmac repair --sync-mirror` copies only the differing records from `$MFT` to `$MFTMirr`; never `ntfsfix`, never automatic | Accepted (owner decision 2026-11-14, option C); repair gated by T-M4-16 |

---

## ADR-001 Backend: how NTFS-3G reaches the macOS VFS

**Context.** The brief prefers the FSKit user-space backend and allows the kernel backend only if FSKit cannot deliver.

**Options.**

| | Option | For | Against |
|---|---|---|---|
| A | NTFS-3G on **macFUSE FSKit backend** | No kext, no Reduced Security. macFUSE's module is already signed and notarized, so this project needs no restricted entitlement and no Xcode to develop [V:S3]. NTFS-3G's FUSE glue is reused unchanged. 5.5.0 targets the macOS 27 SDK, adds Finder eject for `local` volumes, read-only mounts, `FUSE_FLUSH` on close, a corruption mitigation [V:S1]. Works in VMs in principle (user space). | FSKit backend is not at parity: no FUSE notification API, most kernel mount options missing, slower than the kext [V:S2]. No per-file fsync; close errors lost [V:S1] (T-M2-06 later showed that `fsync` does arrive per file as `FUSE_FSYNC` and that `close` sends no flush at all). libfuse2 support for `backend=fskit` is unproven [V:S5][V:S6][V:S9]. Closed-source module. User must install macFUSE and approve its extension. Volume is not owned by Disk Arbitration as a block-device file system, so automount needs our daemon. |
| B | NTFS-3G on **macFUSE kernel backend** | Full feature set, best performance, real fsync path, 15 years of use [V:S2]. | Apple silicon needs Recovery Mode and Reduced Security to allow third-party kexts [V:S3]. Kexts do not load in VMs, so Environment B could not test it [V:S3]. Conflicts with "no unnecessary kernel modifications". |
| C | NTFS-3G on **fuse-t** (NFS or its FSKit module) | Already on the host (1.2.9, module `org.fuset.fskit-srv.module`) [V:S17]. libfuse-compatible. | Closed source. Binary licence: free for non-commercial use, commercial use or bundling needs a paid licence [V:S17]. NFS backend has NFS client caching semantics. Not named by the brief. A second closed dependency with a smaller user base. |
| D | **Native FSKit module** linking `libntfs-3g` (no FUSE layer): `FSUnaryFileSystem` + `FSBlockDeviceResource` | Removes the macFUSE dependency. FSKit hands the module the block device, so no root broker is needed [V:S16]. Proper `fsck`/format hooks (`FSManageableResourceMaintenanceOperations`). Commercial products ship this design (vendor claims) [V:S19]. | New code: every `FSVolume` operation must be re-implemented against `libntfs-3g` (the equivalent of `src/ntfs-3g.c`, about 4,500 lines of data-path glue), which is exactly where data-loss bugs come from. Needs full Xcode and the restricted entitlement `com.apple.developer.fskit.fsmodule`, authorised by a provisioning profile, so an Apple Developer Program membership is needed even to run a development build [A: stated in an Apple forum answer surfaced by a web search and not opened directly; whether a Developer ID profile can carry the entitlement is U]. Automount is still masked by Apple's read-only NTFS kext, and forum posters reported permission failures for third-party modules on physical disks in 2025 with no fixed release named [V:S14]. Same FSKit durability limits as A. `libntfs-3g` is GPL, which is incompatible with Mac App Store distribution terms [A]. |

**Decision.** Option **A**. It is the brief's preference, it has the least new data-path code, and it is the only option that can start today without a paid developer account or Xcode.

**Gate and fallback order.** T-M2-03 and T-M2-04 must show a read/write mount with hash-verified remount in the macOS 27 VM.

1. If libfuse2 rejects `backend=fskit`: port the FUSE glue of `ntfs-3g.c` to the libfuse3 high-level API (task T-M2-03b, Opus-reviewed; mechanical signature changes in about 40 callbacks). libfuse3 3.18.3 ships with macFUSE 5.5.0 and is documented to support the option [V:S1][V:S5].
2. If the FSKit backend itself cannot carry NTFS-3G correctly (data mismatch, hangs, no usable sync point) after 3 Sonnet attempts and 1 Opus analysis: stop, write `CHECKPOINT.md`, and ask the human to choose between option D (needs Xcode + Developer Program) and option B as an opt-in power-user mode. Do not start D without that decision.
3. Option C is used only as an optional comparison target in tests, never shipped.

**Results so far (2026-11-14).** libfuse2 accepts `backend=fskit`, so fallback 1 (T-M2-03b) is not needed; `I-SMOKE-01` passed 3 of 3, `I-INT-01` passed in Environment B and `C-REF-01` 3 of 3 (`docs/spikes/M2-backend.md`, `tests/compatibility/cross_read.py`). Whether the gate is passed is decided by the M2 audit (T-M2-10).

**Consequences.** The probe, policy, journal, CLI and test suite are backend-independent and are reused if option D is taken later. Durability limits N1 to N5 in ARCHITECTURE.md section 9 are permanent for A and D.

## ADR-002 NTFS engine

**Context.** The engine must be mature, open source, and read/write.

| Option | Assessment |
|---|---|
| NTFS-3G 2026.9.28 | Exists; stable release, tagged 28 Sep 2026 and published 29 Sep 2026 UTC; preceded in 2026 by two security releases (2026.7.7, 2026.9.18) and 2026.2.25 with CVE fixes [V:S10]. Actively hardened against crafted volumes. Darwin is a supported `configure` target: external FUSE by default, `fuse >= 2.6.0`, CoreFoundation-based Unicode normalisation (`nfconv`) on by default [V:S11]. Has macFUSE-specific hooks (`getxtimes`, `setcrtime`, `setchgtime`, xattr `position`) [V:S11]. |
| Linux `ntfs3` (Paragon, GPL-2.0) | Kernel code tied to Linux VFS and page cache. Porting is a rewrite. Rejected. |
| Apple `ntfs.fs` | Read-only. Not usable. |
| New implementation | Forbidden by the brief. |

**Decision.** NTFS-3G **2026.9.28**, source tarball pinned by SHA-256 (IMPLEMENTATION_PLAN.md section 3). ntfsprogs are built from the same tarball.

**Patch policy.** Patches live in `third_party/patches/ntfs-3g/NNNN-*.patch`, are applied by `scripts/build-ntfs3g.sh`, and each has a header with rationale, upstream status, and the test that covers it. Planned series:

| Patch | Purpose | Needed |
|---|---|---|
| 0001 | Build fixes for macOS 27 + macFUSE 5.5.0 (pkg-config module name, header paths, warnings) | No: T-M2-03 built with 0 errors and 17 upstream warnings, no change |
| 0002 | Withdrawn. Its premise was wrong: `libntfs-3g/unix_io.c` `ntfs_fsync()` already calls `fcntl(F_FULLFSYNC)` on `__APPLE__` and falls back to `fsync` [V:S25]. The number is not reused. | No (ADR-014) |
| 0003 | Accept an inherited descriptor as the device | **Applied** (T-M2-05, reviewed, review changes applied): unpatched NTFS-3G fails three ways with `/dev/fd/N` for a worker without device permission (ARCHITECTURE.md section 11, U5). macOS only (`__APPLE__`); the Linux build is unchanged. The `unix_io.c` part is library-wide: every `libntfs-3g` user (`ntfs-3g`, `ntfsfix`, `mkntfs` and the other ntfsprogs, `ntfs-3g.probe`, `ntfsmac-probe`, `ntfsmac-volmark`) accepts `/dev/fd/N` by duplicating the held descriptor, and the library exports one new symbol (`ntfs_device_inherited_fd`). It never widens the access mode. `lowntfs-3g` is not patched and must not be shipped. |
| 0004 | Test-only fault-injection device layer, compiled with `--enable-faultio`, absent from release builds | Yes (TEST_STRATEGY.md section 6) |
| 0005 | libfuse3 port of `ntfs-3g.c` | No: ADR-001 fallback 1 not needed (T-M2-03) |
| 0006 | Session channel on `NTFSMAC_ERRFD` (ADR-018): `E` and `W` records from `libntfs-3g/unix_io.c`, `S` and `U` records and the `umount` `E` record from `src/ntfs-3g.c` (`main`, `ntfs_close`) | Yes |

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

**Consequences.** Passive mode when another NTFS driver is present (ARCHITECTURE.md section 8.4). With no GUI session, automount is off and only root or `admin` can mount through the CLI. The claim must hold, or Apple's automount must be unmounted, before the probe opens the device: while `ntfs.fs` has the disk mounted, even a root `O_RDONLY` open fails with `EBUSY` (T-M2-05, U21).

## ADR-004 Privilege model

**Context.** Whole-disk and partition nodes are owned by root, so something privileged must open them. NTFS-3G's parser handles hostile input and had many memory-safety fixes in 2026 [V:S10]. `ntfs-3g` refuses to run setuid with external FUSE [V:S11].

| Option | Assessment |
|---|---|
| Run `ntfs-3g` as root (historical macOS practice) | A parser exploit is a root compromise. Forum posters report that FSKit modules are enabled per user and that root-run FSKit mounts failed [V:S14]; this is not an Apple statement (the Apple engineer says he is unsure about root mounts), so [A] until U6 is resolved. Rejected mainly on the parser-exploit ground. |
| `chown` the device node to the user | A forum poster in the same thread calls it a testing hack that must not ship [V:S14]; that is a user's view, not Apple's. Rejected on our own grounds: it changes system device ownership persistently. |
| Root broker opens the device and passes the fd to an unprivileged, sandboxed worker (chosen) | Parser never has root. Read-only decisions are enforced by the descriptor mode. macFUSE 5.2.0 notes say libfuse already uses the new FSKit mount API and name the Mach exception `io.macfuse.mount` that sandboxed servers need [V:S6]; they do not say unprivileged servers are supported, so that is [A] until T-M4-01. |

**Decision.** Broker + fd passing. Worker uid is the console user, so the user's approval of the macFUSE extension applies.

**Consequences.** Requires U5 and U6 to resolve positively. U5 is resolved with patch 0003 (T-M2-05): a root parent opens the device, and a child without device permission mounts it read/write and read-only from the inherited descriptor. U6 remains (T-M4-01). If the sandbox profile cannot be made to work, uid separation alone remains and the residual risk is recorded (R-07).

## ADR-005 Handling hibernated, Fast Startup and dirty volumes

**Context.** NTFS-3G's own behaviour [V:S11]:

- `recover` is the **default**: on an unclean `$LogFile` it calls `ntfs_logfile_reset()` and mounts read/write, discarding the Windows journal.
- `norecover` refuses instead.
- `ntfs-3g` passes `NTFS_MNT_MAY_RDONLY` whenever `remove_hiberfile` is absent, so for the driver every refusal below becomes a read-only mount: T-M3-04 observed it for `unclean-log`, `log-v2` and the `hibr`/`HIBR`/short hibernation fixtures (exact messages in `tests/fixtures/manifest.json`); T-M3-01 observed it for an unclean log in A and B, with `$LogFile` byte-identical.
- A hibernated volume (`hiberfil.sys` with `hibr` header, or shorter than 4096 bytes) fails with `EPERM`; `remove_hiberfile` deletes the file and continues.
- A `$LogFile` restart page of version 2.0 (Windows 8+ Fast Startup metadata cache) returns `EPERM` and is rejected even with `recover`.
- When `NTFS_MNT_MAY_RDONLY` is set (read/write requested, no `remove_hiberfile`), the library falls back to read-only instead of failing.

**Decision.**

1. The decision is made **before** NTFS-3G starts, by `ntfsmac-probe` and the policy table. NTFS-3G's internal fallback is defence in depth, not the mechanism. The worker's exit status does not show that fallback, so the daemon compares the mount's `MNT_RDONLY` flag with its decision after every mount (ARCHITECTURE.md section 4.1 step 8).
2. Every worker invocation carries `norecover`.
3. `remove_hiberfile` and `recover` are not reachable through any interface of this product, including configuration. The daemon's option builder has no code path that emits them, a unit test asserts it, and the independent argv validator (ADR-016) rejects them if they ever appear.
4. Unsafe state means read-only with an `O_RDONLY` descriptor.
5. Manual repair is exposed only as `ntfsmac repair --clear-dirty <bsd>`, implemented by `ntfsmac-volmark clear-repair` (ADR-017), and `ntfsmac repair --sync-mirror <bsd>`, implemented by `ntfsmac-volmark sync-mirror` (ADR-021), never by `ntfsfix`. The rest of this item describes `--clear-dirty`. It requires the peer checks and the administrator prompt of ADR-019, typing the volume label and serial, is refused unless the probe state is exactly `dirty_flag`, prints that this is not a consistency check, and is never suggested as the first remedy. There is no wrapper for hibernation-file removal.
6. Disk-derived strings (the label) never enter the worker option string unsanitised (ADR-016).

**What this decision does not guarantee.** The probe is the parser that produces the verdict. A probe that is fooled into `clean` defeats decisions 1 and 4; the engine's own checks (with `norecover`) and the volmark step's own hibernation and `$LogFile` checks are a second line, not a guarantee (ARCHITECTURE.md section 9, D2).

**Consequences.** Users of dual-boot or Fast Startup machines get read-only mounts until Windows is shut down fully. This is intended.

## ADR-006 Marking an unclean session

**Context.** If the disk is pulled during a read/write session, nothing on the volume says so unless NTFS-3G or this product sets `VOLUME_IS_DIRTY`. The NTFS-3G 2026.9.28 source settles U9, and T-M3-01 confirmed it by test in A and B: neither `libntfs-3g` nor `src/ntfs-3g.c` ever sets or clears the flag; `ntfs_volume_write_flags()` is called only by the ntfsprogs tools `ntfsfix`, `ntfsresize`, `ntfsmove` and `ntfsck` (`ntfsfix` also through its own `OLD_ntfs_volume_set_flags`), and `mkntfs` builds `$Volume` itself (ARCHITECTURE.md section 3.6) [V:S25]. NTFS-3G with `norecover` does not write `$LogFile` either (U13, confirmed by T-M3-01). So without this product's own marker, an unclean session leaves no trace on the disk, and guarantee D3 holds only on the Mac that ran the session.

| Option | For | Against |
|---|---|---|
| Host journal only | No extra on-disk writes. | Invisible to Windows and to other Macs. D3 is host-only. |
| On-disk dirty flag set by the worker (patch) | One process. | Changes NTFS-3G's mount path; a worker crash before the flag is written leaves no marker. |
| On-disk dirty flag set by a separate short `libntfs-3g` step (`ntfsmac-volmark`) before the worker starts and cleared after it exits cleanly (chosen) | NTFS-3G unpatched for this. Uses the library's own `ntfs_volume_write_flags`. Volmark's own hibernation and `$LogFile` checks are also a second check against a misreported unsafe state. Windows checks the volume after an unclean detach. | Two extra short mount cycles per session (each a few `$MFT` reads and one record write). A second NTFS parser holding a writable descriptor. Windows also checks a volume that was merely moved without a clean eject (intended). |

**Decision.** The on-disk marker is the **default plan** (phase 2 is no longer optional). Component, ordering, uid, sandbox, descriptor and failure handling are specified in ARCHITECTURE.md section 3.6:

1. `ntfsmac-volmark` runs as `nobody` in its own sandbox profile, receives only the daemon's `O_RDWR` device descriptor, and only for `rw` decisions.
2. `set`: after the journal entry is written and synced and the device is bound to the probed volume (ADR-020), before the worker starts; `F_FULLFSYNC` and a read-back before exit 0. If `set` fails for any reason, the session is mounted `ro`.
3. `clear`: only after a clean session proven by the positive records of ADR-018 (`S rw`, `U 0`, no `E`), and a successful daemon `F_FULLFSYNC`; journal entry removed only after `clear` exits 0.
4. Only the `VOLUME_IS_DIRTY` bit is changed; all other bits are preserved, and volmark refuses a volume with bits outside `VOLUME_FLAGS_MASK`, which `ntfs_volume_write_flags()` would drop [V:S25].
5. Volmark mounts with `NTFS_MNT_FORENSIC` and no `RDONLY`. In `volume.c` `ntfs_device_mount` that skips the check block that also contains `fix_txf_data()`, so volmark writes nothing but the `$Volume` record; the `$TXF_DATA` fix-up moves to the worker's mount, after the flag is set (audit M1-recheck N-03). Volmark runs the hibernation and `$LogFile` refusals itself with the public functions `ntfs_volume_check_hiberfile()`, `ntfs_check_logfile()` and `ntfs_is_logfile_clean()` plus the restart-page 2.0 test [V:S25].
6. The flag write is two writes: `ntfs_mft_records_write()` (`mft.c`) writes record 3 to `$MFT`, then to `$MFTMirr`. Every NTFS-3G mount compares the two. A cut between or inside them leaves a volume that NTFS-3G refuses (probe `corrupt`, policy `refuse`), not one that mounts read-only. No write order avoids it. This is accepted as a stated residual risk (R-33, U15). A cut between the two writes is now classified `mirror_split` and repaired by the explicit `repair --sync-mirror` (ADR-021); a torn copy stays `corrupt`. The worker opens the same window for every MFT growth (T-M3-14), so it is not specific to volmark. For volmark the window is two 1 KiB writes and one flush per session edge, the data region is never touched, the daemon never `SIGKILL`s volmark between its `w` and `d` progress marks, the notification names the cause, and `automount_mode=ro` removes the exposure for users who mostly read. The crash outcome of every cut point is the table in ARCHITECTURE.md section 3.6, tested by `U-VMK-07`.

T-M3-12 (Opus) implements it with the kill-at-every-step tests; until it passes, the step is disabled, the journal records `marker=none`, and D3 is documented as "host-only". T-M3-01 still confirms the source finding by test; if it unexpectedly shows NTFS-3G setting the flag, the coordinator re-opens this ADR rather than dropping the marker.

**Interaction with Windows.** A volume with `VOLUME_IS_DIRTY` set makes Windows run `autochk` at boot (fixed disk) or offer "Scan and fix" (removable disk). Because the flag is set for the whole read/write session, this happens after every session that did not end with a clean eject on the Mac. This is the intended behaviour, and it is the only way a Windows user learns that the volume may be inconsistent.

**Consequences.** After any unclean detach the volume is read-only on this product, on any Mac, until Windows `chkdsk` (or `ntfsmac repair --clear-dirty`, ADR-017) clears the flag and the journal entry is cleared (ADR-017). Documented prominently.

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

**Facts.** Option A needs no restricted entitlement: the only FSKit module involved is macFUSE's, which is signed and notarized by its developer [V:S3]. Unsigned FUSE server binaries are [A] until T-M2-03 shows an ad-hoc-signed worker mounting on arm64: the 5.5.0 notes only say that a crash when mounting with unsigned binaries on Intel Macs was fixed [V:S1], and this product is arm64-only. `pkgbuild`, `productbuild`, `codesign` are part of base macOS; `notarytool` and `stapler` come with Xcode [A].

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
| Ownership | All files appear owned by the console user (`uid=`, `gid=`), `dmask=0022`, `fmask=0133`. On this backend the mount is always `noowners`: it shows the mounting user whatever `uid`/`gid` say, and modes are not enforced (a mode-000 file is readable by the mounting user). `uid`, `gid`, `fmask` and `dmask` are kept because they set the engine's defaults and the shown modes [V: T-M2-04]. | Matches how macOS treats removable FAT/exFAT volumes (Apple's exFAT module sets `FSVolumeAlwaysIgnoreOwnership` [V:S17]). NTFS ACLs are not modified. Consequences: modes are metadata, not access control (ARCHITECTURE.md section 9, N10); the execute bit is cleared and `chmod +x` does not persist, but whether that stops execution is open (N9; tests `I-PERM-*`). Relaxing `fmask` is a later, explicit decision. |
| Full POSIX mapping | Opt-in only: `permissions` or `usermapping=/usr/local/etc/ntfs-macos/UserMapping` (a path without a space, so `argv_check()` rule 3 holds) [V:S11, S25]. `permissions` works only without `uid`, `gid`, `fmask` and `dmask` (with them `chmod` returns success and changes nothing), so it replaces those four. A missing `usermapping` file is accepted silently, so the daemon checks the file before the spawn [V: T-M2-04]. Not tested in v1 beyond a smoke test (`I-PERM-06`). | Mapping errors can lock Windows users out of files. |
| Extended attributes | `streams_interface=openxattr`: macOS xattrs of every namespace are stored as NTFS named data streams and survive a remount [V:S11; V: T-M2-04]. `xattr` is not used: it stores only `user.*` names and fails every `com.apple.*` name with `ENOTSUP`. | Keeps Finder metadata on the volume without `._` files (`cp -p` copies the xattr and makes no `._` file). Known gap: a `com.apple.FinderInfo` write returns success but cannot be read back (T-M2-04). T-M3-08 verifies FinderInfo, quarantine and resource fork; if Finder copy fails, fall back to `streams_interface=none` and document xattrs as unsupported. |
| Alternate data streams | Readable and writable through the xattr interface. `streams_interface=windows` (`file:stream` syntax) is not offered. | `:` handling in macOS paths is error-prone. |
| File names | `windows_names` on: creation of names Windows cannot open is refused with `EINVAL` [V:S11; V: T-M2-04, ten names such as `a:b`, `CON`, `trail.`]. `nfconv` normalisation stays at NTFS-3G's Darwin default [V:S11]. macOS lists non-ASCII names in NFD while the bytes on disk are NFC (T-M2-09); which layer converts and how normalisation-only pairs behave is U22. Case: NTFS-3G is case-sensitive in the POSIX namespace; no `ignore_case` (that is `lowntfs-3g` only) [V:S11]. | Interoperability first. The case-sensitivity difference from Windows is documented and tested (two names differing only in case). |
| Timestamps | `noatime` (it acts on disk; the backend does not show atime changes in `stat`, T-M2-04). Creation time through the macFUSE `getxtimes`/`setcrtime` hooks [V:S11]; T-M2-03 saw a birth time of 1970-01-01 for new files through the backend, so creation time is not exposed there [A until T-M3-06]. | |
| Compression, EFS | Read and update of compressed files as NTFS-3G supports; no new compressed files (`nocompression` is considered in T-M3-09); EFS files unreadable (no `efs_raw`). | |

## ADR-013 macFUSE as a prerequisite, not a bundled component

**Decision.** The installer does not contain macFUSE and does not download it. Preflight checks that macFUSE >= 5.5.0 is installed and that its FSKit extension is enabled, and otherwise explains what to do and links to the official download.

**Reason.** The macFUSE licence restricts bundling and automated installation in a commercial context [V:S8]; this project is non-commercial, but a prerequisite model removes any ambiguity for forks and redistributors, keeps macFUSE updates in its own updater, and keeps closed binaries out of a GPL source release.

**Consequences.** Two installs for the user. The minimum version is enforced at daemon start and reported by `ntfsmac doctor`.

## ADR-014 Device sync

**Facts** [V:S25]. In `libntfs-3g/unix_io.c`, `ntfs_fsync()` issues `fcntl(fd, F_FULLFSYNC)` on `__APPLE__`; if that call fails for any reason it calls plain `fsync(fd)` and returns that result, with no message. Both the device sync operation (`ntfs_device_unix_io_sync()`) and the close path (`ntfs_device_unix_io_close()`, when the device is dirty) use it. The earlier plan for a patch 0002 that adds `F_FULLFSYNC` rested on a wrong premise (audit M1-recheck N-06).

What reaches the worker [V: T-M2-06]: an application's `fsync` and `F_FULLFSYNC` both arrive as the same `FUSE_FSYNC`, which `ntfs_fuse_fsync()` turns into a whole-device sync through `ntfs_fsync()`; `sync(2)` sends `FUSE_FSYNC` only for open files; unmount sends only `FUSE_DESTROY`, whose `ntfs_umount()` syncs the device. So the drive flush is the worker's own doing at those points, never something the application's `F_FULLFSYNC` asks for.

**Decision.**

1. No patch adds `F_FULLFSYNC`; patch 0002 is withdrawn.
2. The fallback is made visible: patch 0006 writes `W fullfsync <errno>` on the session channel whenever `ntfs_fsync()` falls back (ADR-018). It does not change the return value, because failing every sync on a drive that lacks `F_FULLFSYNC` would make such drives unusable without making them safer.
3. The daemon's own post-exit sync and volmark's step 5 use `fcntl(F_FULLFSYNC)` with the same fallback and record whether it was used.
4. A clean report says the drive cache was flushed only when no `W` record arrived and neither the daemon nor volmark fell back (ARCHITECTURE.md section 4.5, D1).
5. The `sync` mount option of NTFS-3G (synchronous device writes) is evaluated in T-M3-10 and enabled if the throughput cost is below 30 percent on the sequential-write benchmark. T-M2-04 found that it sets no synchronous flag on the FSKit mount, so any effect is engine-side only; it stays out of the allow-list until T-M3-10 shows one.

**Reason.** Guarantee D1 depends on the drive cache being flushed; plain `fsync` on macOS does not do that, and a silent fallback would let D1 be claimed when it is false.

**Consequences.** Slower close on spinning USB disks (from upstream behaviour, measured in T-M3-10). Drives that reject `F_FULLFSYNC` get a weaker "all data written" report. One patch less to carry (R-06).

## ADR-015 Local-only CI and build artifacts

**Context.** The owner decided on 2026-11-13 that, for the start of the project, nothing runs on GitHub: no GitHub Actions workflows, no macOS or Linux hosted runners, and no binaries uploaded as release assets or packages.

**Decision.** All continuous integration runs locally through `scripts/ci-local.sh` (TEST_STRATEGY.md section 9) in Environment A (local container), Environment B (local macOS VM) and the host (build only). Result files are committed under `tests/results/`. Builds, packages and release candidates stay in git-ignored `build/` and `dist/` on the local machine. The repository holds source, docs and test results only.

**Consequences.** No automatic checks on push or on outside pull requests; the coordinator runs `ci-local.sh --quick` before every push once T-M1-07 has created the minimal script (until then: `scripts/bootstrap.sh` and a manual `cmake --build` and `ctest`), and the full run at each milestone gate (T-M5-08 completes the script). No hosted-CI cost (risk R-22). Human action H4 is withdrawn. Publishing CI or binaries later needs a new decision by the owner.


## ADR-016 Disk-derived strings in the root daemon

**Context.** The volume label is read from `$Volume` on an attached disk, which the threat model treats as hostile (ARCHITECTURE.md section 6). The earlier draft put it in the worker's `-o` string as `volname=<label>` and used it as `/Volumes/<label>`, created by root. `ntfs-3g` splits `-o` on every `,` with no escape character and passes unknown tokens to libfuse, which splits on `,` again and treats `\` as an escape [V:S25]. A label `X,remove_hiberfile` would become an engine option, `X,allow_other` a FUSE option, and `../../x` a path outside `/Volumes`. The label also travels from the probe, itself a parser of hostile data, to root as JSON text.

| Option | Assessment |
|---|---|
| Escape the label inside `-o` | Not possible: `ntfs-3g`'s own `strsep` split has no escape, so `\,` still splits. Rejected. |
| Never use the label; name every mount point `NTFS-<serial>` | Safe, but every volume looks alike in Finder. Kept as the fallback name. |
| Sanitise to an allow-listed leaf, use it only as the mount-point name, and re-validate the final argv (chosen) | Keeps a readable name; the allow-list excludes every byte that is special to either option parser or to paths. |

**Decision.**

1. The daemon parses the probe output with a strict parser in `src/sanitize/`: at most 64 KiB, valid UTF-8, no `\u0000`, no duplicate keys, known keys and enum values only, and the per-field grammar of ARCHITECTURE.md section 3.1 (types, ranges, lengths, character sets; `serial` and `logfile_lsn` exactly `0x` and 16 upper-case hex digits). Any failure gives state `unknown` (read-only).
   - `serial` and `logfile_lsn` are converted to 64-bit integers and the input strings are discarded. Every file name and every displayed serial is formatted by the daemon from the integer (journal entry `<16 hex>`, tombstone `cleared/<16 hex>-<UTC>`), and created relative to an `O_NOFOLLOW` directory descriptor (audit M1-recheck N-02).
   - The probe sends no human text. `reasons` is replaced by `reason_codes`, an array of enum codes from `include/ntfsmac/probe.h`; the daemon and the CLI map codes to their own fixed text, so no probe byte can reach a terminal, a notification or a log line except the sanitised label.
2. The label sanitiser maps the raw label to `<leaf>`:

| Rule | Detail |
|---|---|
| Decoding | Strict UTF-8 (no overlong forms, no surrogates). Invalid input: the whole label is replaced by the fallback. |
| ASCII allow-list | `A`-`Z`, `a`-`z`, `0`-`9`, space, `-`, `_`, `.`, `(`, `)`, `+`. Every other ASCII byte, including `,` `=` `\` `/` `:` `"` `'` and all of `0x00`-`0x1F` and `0x7F`, is replaced by `_`. |
| Non-ASCII | Allowed except: C1 controls U+0080-U+009F; format and bidirectional controls (U+00AD, U+061C, U+180E, U+200B-U+200F, U+202A-U+202E, U+2060-U+206F, U+FEFF, U+FFF9-U+FFFB); U+2028, U+2029; private use (U+E000-U+F8FF, planes 15 and 16); noncharacters (U+FDD0-U+FDEF, U+xFFFE, U+xFFFF); slash look-alikes (U+2044, U+2215, U+29F8, U+FF0F) and the colon look-alike U+FF1A. Each is replaced by `_`. Non-ASCII UTF-8 bytes are all `0x80` or above, so they cannot collide with an option or path metacharacter. |
| Length | The label part is at most 64 bytes of UTF-8, cut at a code-point boundary (NTFS allows 128 UTF-16 units). The created mount-point leaf is at most 67 bytes: the label part plus an optional collision suffix ` 2` to ` 99` (below). Both are far inside the 255-byte name limit. |
| Path component | In this order: leading spaces removed; a leading `.` becomes `_`; trailing spaces and dots removed. The result is never empty (else fallback), never `.` or `..`, never starts with `.`, and never contains `/` or NUL (both outside the allow-list). |
| Fallback | Empty result: `NTFS-<last 8 hex digits of the serial>`. |
| Collisions | `/Volumes/<leaf>` is created with `mkdirat` relative to an `O_NOFOLLOW` descriptor of `/Volumes`; on `EEXIST` the daemon tries `<leaf> 2` to `<leaf> 99`; it never reuses an existing directory and removes only the directory it created (device and inode checked). |

3. The sanitised leaf is the only form of the label that leaves the sanitiser: mount point, journal, `list` replies, CLI output and notifications. The raw label is not logged except hex-encoded in debug logs. No other probe string leaves the parser at all (decision 1).
4. `argv_check()` re-parses the complete worker vector before every spawn and refuses it unless it matches the allow-list and per-key grammars (ARCHITECTURE.md section 3.3, rules 1 to 7). It is independent of the option builder: different source file (`src/options/argv_check.c` next to `options.c`, T-M3-03), written against the same specification, and fuzzed. Its text rules 1 to 6 (`ntfsmac_argv_check_text()`) do no I/O; rule 7 adds the one `fcntl(F_GETFL)` (`ntfsmac_argv_check()`). T-M3-19 adds the verdict parser and the label sanitiser in `src/sanitize/` and reuses these two functions; it does not duplicate them.

**No label in the option string.** `volname` is not passed: the FSKit backend ignores it (T-M2-04, U14), so the label never enters `-o`, and `argv_check()` rule 3 allows only bytes `0x21..0x7E` there. The leaf is a separate `argv` element. The path metacharacters are `/`, NUL and, in Finder, `:`; none is in the sanitiser's output alphabet.

**Accepted display risk.** Look-alikes other than slash and colon stay allowed: Cyrillic or full-width letters that imitate another name (`Macintosh HD`), full-width comma and similar. All such bytes are `0x80` or above, so no option or path injection follows (rules 2 and 3 of `argv_check()` hold), but notifications and the typed confirmation of ADR-017 and ADR-019 can show an attacker-chosen name. The confirmation also asks for the last 8 hex digits of the serial, which the label does not control, and `list` shows the BSD name; no further filtering is done because a confusables table is never complete. NFC and NFD forms of one name differ in bytes but collide in APFS name comparison; the `mkdirat` `EEXIST` rule gives the second one a suffix (`U-LABEL-08`).

**Consequences.** Some labels display differently from Windows (for example `Backup,2026` becomes `Backup_2026`). Tested by `U-LABEL-*`, `U-ARGV-*`, `U-VERDICT-*` (including `U-VERDICT-05` for hostile serials and reason codes), `F-SANITIZE-*` and `I-DMN-18` (TEST_STRATEGY.md section 4). Task T-M3-19; the journal naming rule is tested in T-M3-13 (`U-JRN-*`).

## ADR-017 Clearing the session journal; repair

**Context.** A journal entry is created for every read/write session and removed only after a clean end of that session. A read-only session can never produce that clean end. Without another way to remove the entry, a volume that once ended uncleanly stays read-only on this Mac forever, even after Windows `chkdsk` (audit M1, C2). Any clearing path must not let an unsafe volume become read/write without the user knowing.

| Option | Assessment |
|---|---|
| Expire entries after a time | Silent; says nothing about the volume. Rejected. |
| Clear when the probe says `clean` | A drive that loses our dirty-flag write after power loss would probe `clean` with nothing checked. Rejected as the only rule. |
| Clear on evidence that another system checked the volume: our flag was set and is now clear, and the `$LogFile` sequence number moved (chosen, gated) | Matches what Windows `chkdsk` does; does not fire on a lost flag write alone. Needs U12 and U13 confirmed (U13 confirmed by T-M3-01). |
| Explicit, confirmed user command (chosen) | Always available; the user takes the decision with a stated warning. |

**Decision.** Four clearing paths, specified in ARCHITECTURE.md section 3.5: (1) clean end of our own session; (2) the external-check evidence rule, shipped disabled until the Windows step of T-M5-10 confirms U12 (T-M3-01 confirmed U13), and even then only effective from the next decision, never on the attach where it fires; (3) `ntfsmac journal-clear`, allowed only when a fresh probe returns exactly `clean` and no live session owns the entry (`session_id`), with a stronger warning for `write_error` and for the lost-flush pattern (`marker=set`, flag clear, LSN unchanged); (4) a successful `ntfsmac repair --clear-dirty`. Paths 3 and 4 are control requests `journal_clear` and `repair` (ARCHITECTURE.md section 3.4), authorised as in ADR-019 (not a spawned process, signed CLI, administrator password per request), with the sanitised label and the last 8 hex digits of the serial typed by the user as a guard against mistakes. Cleared entries are kept as tombstones for `doctor`.

`repair --clear-dirty`:

| Item | Rule |
|---|---|
| Allowed when | Fresh probe `state` exactly `dirty_flag`, `logfile` `clean`, no hibernation, and `VOLUME_IS_DIRTY` set with no other flag except the informational `VOLUME_MODIFIED_BY_CHKDSK` and `VOLUME_MOUNTED_ON_NT4`; volume not mounted. Otherwise refused with the reason (run Windows `chkdsk`). |
| Leftover `chkdsk` flags | A lone `VOLUME_MODIFIED_BY_CHKDSK` does not make a volume `dirty_flag` (ARCHITECTURE.md section 3.1 check 3), so a `chkdsk` that leaves it set does not trap the volume read-only: `journal-clear` (fresh probe `clean`) is available. No NTFS-3G or ntfsprogs code clears that bit (`layout.h` defines it; `ntfsinfo.c` only prints it) [V:S25], so treating it as dirty would have recreated the M1 deadlock (audit M1-recheck N-04). `H-USB-08` records the flags `chkdsk /f` leaves; `H-USB-09` checks that a plain Windows mount leaves our flag set. |
| Who writes | `ntfsmac-volmark clear-repair`, `nobody`, sandboxed, on a daemon-opened `O_RDWR` descriptor (ARCHITECTURE.md section 3.6). `ntfsfix` is not run: besides clearing the flag it also empties `$LogFile` and rewrites `$MFTMirr` and `$UpCase` when it finds differences [V:S25, `ntfsprogs/ntfsfix.c`]; in its repair path it sets `VOLUME_IS_DIRTY` and resets `$LogFile` unconditionally (`ntfsfix.c:295`, `:330-340`) [V: T-M3-14 triage]. `ntfsfix` is not a product repair path for any state (ADR-021). |
| Warning | Always: "this does not check or repair the file system". If a journal entry exists: "this Mac left the volume open on <date>; files written in that session may be incomplete or damaged; Windows `chkdsk` is strongly preferred". The user must type the confirmation after the warning. |
| After success | Journal entry removed (path 4). The volume is not remounted automatically; the next `mount` decides with a fresh probe. |

**What this does not guarantee.** The probe output that feeds every gate comes from untrusted data. A crafted volume can satisfy the gates; it could equally probe `clean` with no journal entry, so these paths add no new exposure against a hostile disk. They protect only against our own unclean sessions.

**Consequences.** The user has a defined way back to read/write after `chkdsk`. Tests `U-POL-*` (gates, including the informational-flag cases), `U-JRN-*`, `I-DMN-19`, `I-DMN-20`, `I-DMN-21`, `H-USB-08`, `H-USB-09`. Task T-M4-14.

## ADR-018 How the daemon learns of a device write error, and how a session proves it ended cleanly

**Context.** The architecture promised that the daemon "marks the session failed after the first write error", but the worker is stock `ntfs-3g`, FSKit drops close errors on macOS 27 [V:S1], and nothing reported device errors to the daemon (audit M1, C6). The first version of this decision called a session clean on "worker exit 0 and no error record". The re-check showed that this fails open (audit M1-recheck N-01): `src/ntfs-3g.c` `main()` sets `err = 0` after `fuse_loop()` returns, whatever happened, and `ntfs_close()` only logs a failed `ntfs_umount()` [V:S25]; and a channel that is not set, not patched, lost or leaked is also silent. The channel was also unbounded and could leak into other processes (N-07).

| Option | Assessment |
|---|---|
| Worker exit status only | Exit status is 0 after every `fuse_loop()` return [V:S25]. Not evidence. Kept as a necessary condition. |
| Parse the worker's log | NTFS-3G log text is not a stable interface, and not every device failure is logged with a distinguishable message [A]. Rejected. |
| FUSE-level error counters | Not available from the closed macFUSE FSModule. Rejected. |
| Error records only, clean when none arrive | Fails open on every channel failure (N-01). Rejected. |
| Small patch writing error records plus positive start and clean-unmount records to an inherited descriptor (chosen) | Device layer plus two hooks in `src/ntfs-3g.c`; no change to allocation, MFT or index code; independent of FSKit; silence means unclean. |

**Decision.** Patch 0006, specified in ARCHITECTURE.md section 7:

1. Records `S rw|ro` (after `mount_fuse()` succeeded in `main()`), `E <op> <errno>` (failed or short device write, failed device sync or close in `unix_io.c`, failed `ntfs_umount()` in `ntfs_close()`), `W fullfsync <errno>` (the silent fallback in `ntfs_fsync()`, ADR-014) and `U <n>` (in `ntfs_close()`, only when `ntfs_umount()` returned 0; `n` counts the `E` records attempted).
2. A session is clean only on positive proof: `S rw` first, `U 0` last, no `E`, channel within bounds and at end-of-file after the worker exit, unmount seen before the exit, session not re-adopted after a daemon restart, exit status 0. Anything else, including silence, is unclean and keeps the journal entry and the on-disk marker.
3. Bounds: records at most 32 bytes, at most 64 per session, strict grammar; on a violation the session is unclean and the daemon keeps draining into a fixed buffer without closing the read end. No unbounded buffer exists in the root daemon.
4. Descriptor hygiene: daemon descriptors `O_CLOEXEC`; every spawn uses `POSIX_SPAWN_CLOEXEC_DEFAULT` with explicit file actions; the worker sets `FD_CLOEXEC` on the channel and writes non-blocking. End-of-file must arrive within 2 s of the worker exit.

**What this does not guarantee.** A compromised worker can forge `S rw` and `U 0`. That only makes its own volume look clean, which it can already achieve by writing that device; it is the residual risk of section 6 and D2. Errors that never reach the worker remain invisible. For `write(2)` data T-M2-06 found no FSKit buffer and the worker's error comes back from `write()` itself; `mmap` writes are not traced yet (N7, T-M3-11).

**Consequences.** One more patch to carry and re-base on each NTFS-3G update, now touching `src/ntfs-3g.c` as well as `unix_io.c`; Opus review at merge (R-06). A broken channel turns every session unclean, so a packaging or spawn mistake shows up as "read-only next time", which `I-DMN-24` tests. Tests `I-ERR-01..07`, `I-DMN-24`. Tasks T-M3-18, T-M4-13.

## ADR-019 Authorising journal clearing and repair

**Context.** `journal_clear` and `repair` let an unsafe volume reach `rw` on a later attach. They were gated by `admin` membership (`LOCAL_PEERCRED`) and by typing the sanitised label and the last 8 hex digits of the serial. Both values are published by `list` and `status` to every peer, and on a typical Mac the console user is an administrator, so every process of that user, including a compromised worker, passed the check (audit M1-recheck N-05). The sandbox that would stop the worker is [A] (T-M4-06), and the stated fallback is uid separation only.

| Option | Assessment |
|---|---|
| Typed confirmation only | Not a boundary: the values are public. Kept as a guard against mistakes. |
| Root-only requests (`sudo ntfsmac ...`) | Stops other users' processes but not a compromised process of an admin user who has a cached `sudo` ticket; also makes the normal path need a terminal. Rejected as the only control. |
| Refuse peers that are spawned processes, require the signed CLI, and require a per-request administrator authentication validated by the daemon (chosen) | Each part closes one path: the worker and its children; any other process of the user; and any process without a human present. Uses public APIs. |

**Decision.** For `journal_clear` and `repair` the daemon requires, in this order (ARCHITECTURE.md section 3.4):

1. Peer uid in `admin` (`LOCAL_PEERCRED`).
2. Peer is not a process the daemon spawned, nor a descendant of one. Every spawn gets its own process group; the daemon checks the peer's process group and parent chain (`LOCAL_PEERPID`, `proc_pidinfo`).
3. Peer executable is the installed, hardened-runtime `ntfsmac` CLI, checked from the peer's audit token against the code requirement recorded at install time (Developer ID team for release builds, exact cdhash for ad-hoc development builds).
4. A fresh administrator authentication: the CLI acquires the right `org.ntfs-macos.journal-modify` (rule `authenticate-admin-nonshared`, timeout 0, created by the package's `postinstall` with `AuthorizationRightSet` and removed by the uninstaller) and sends the external form; the daemon calls `AuthorizationCopyRights` on it itself. No cached credential can satisfy the rule.
5. The typed label and serial, and the policy gates.

The worker sandbox also denies `connect()` to the control socket; the boundary no longer depends on it. Plain `mount` and `unmount` keep the console-user-or-`admin` rule: a `rw` mount only follows a fresh `clean` probe with no journal entry, so it cannot turn an unsafe volume writable.

**Gate.** T-M4-04 verifies on macOS 27 that the daemon can validate the external form, how the prompt behaves without a GUI session (U17), and that the code-requirement check works for ad-hoc development builds [A]. If Authorization Services cannot be used from the daemon, the fallback is a root-only request (`sudo`) plus checks 2 and 3, recorded as a residual risk in R-34.

**Consequences.** One password prompt per `journal-clear` or `repair`. The package installs one authorization right (a new entry, not a change to an existing system right; no security setting is lowered). Tests: `I-DMN-19`, `I-DMN-21` and ctl tests in T-M4-04 cover refusal of a spawned-process peer, an unsigned or re-signed client, and a missing or cached authorization. Audit T-M4-12 checks this boundary. Risk R-34.

## ADR-020 Binding the probed volume to the opened device

**Context.** The probe verdict (serial, total sectors, state) decided the mode, but the daemon then opened the device again and volmark and the worker never re-checked that it was the same volume (audit M1-recheck N-08). A disk swapped between the two opens, a BSD name reused by another device, or another driver mounting the disk in between would make the journal entry name one volume while another is flagged and written.

| Option | Assessment |
|---|---|
| Open once `O_RDWR` and give the probe a read-only view of it | Not possible: on macOS, opening `/dev/fd/N` shares the open file description, so the probe would hold a writable descriptor and D2 would be lost [V: T-M2-05]. It is worse than assumed: the re-opened descriptor keeps the original access mode (an `O_RDONLY` request on an `O_RDWR` descriptor gives `O_RDWR`), and an unprivileged process cannot re-open `/dev/fd/N` at all without permission on the device (`EACCES`). Rejected. |
| Re-run the probe on the writable descriptor | Gives a parser of hostile data a writable descriptor. Rejected. |
| Reuse the probe descriptor for `ro`; for `rw` open the writable descriptor while the probe's is still open and compare device number and a hash of the first 4096 bytes; volmark checks the expected serial and size; Disk Arbitration shows no other mount (chosen) | No NTFS parsing in the daemon (a hash compare only); the checks that need NTFS parsing run in the unprivileged volmark. |

**Decision.** ARCHITECTURE.md section 4.1 step 6: (1) `ro` sessions use the probe's own descriptor; (2) for `rw`, `st_rdev` of both descriptors equal and the first 4096 bytes hash to the value read before the probe ran, then the probe descriptor is closed; (3) volmark gets `--expect-serial` and `--expect-sectors`, formatted from the parsed verdict, and exits 15 on a mismatch with the boot sector; (4) the hash is checked again after volmark; (5) before any `O_RDWR` open, the Disk Arbitration description shows no mount path for the disk [A: U18]. Any mismatch gives `ro` or `Failed`, never `rw`.

**What this does not guarantee.** A device whose firmware answers the same reads differently at different times defeats any binding; it can equally change its content during a session. That is the hostile-disk exposure of D2.

**Device exclusivity.** The binding and the Disk Arbitration dissenter (ADR-003) are what keep the device exclusive. The advisory lock that NTFS-3G takes on its duplicate of the descriptor gives no lasting exclusivity: `ntfs-3g` calls `daemon()` after opening the volume, and POSIX locks do not survive the fork (T-M2-05, patch 0003 header).

**Consequences.** One extra 4 KiB read per attach and one per volmark run. Tests `I-DMN-25` (swap between probe and mount, simulated by re-attaching another image under the same BSD name; and a volume already mounted by another driver) and `U-VMK-09` (volmark with wrong expected values). Tasks T-M4-14, T-M3-12, T-M4-02. Risk R-35.

## ADR-021 Stale `$MFTMirr` after an interrupted `$MFT` update

**Context.** The crash sweep (T-M3-14, `docs/spikes/M3-crashsweep-triage.md`) found one class of failure in all 18 failing cut points of 550: the cut fell between NTFS-3G's write of `$MFT` record 0 and its write of `$MFTMirr` record 0. `ntfs_mft_record_init()` syncs record 0 for every new MFT record beyond the initialized size (`mft.c:1349-1350`), and `ntfs_mft_records_write()` writes the two copies as two device writes with no flush between (`mft.c:189`, `:204`). Any interruption between them (power loss, unplug, a crash or `SIGKILL` of the worker) leaves `$MFT` record 0 one step ahead of the mirror. Every NTFS-3G mount, read-only too, then refuses the volume (`volume.c:1038-1046`), so the probe says `corrupt` and the policy refuses it, although the data is intact: after copying either copy over the other every image mounted, walked cleanly and kept all synced files. In all 18 images `$MFT` was the newer copy (USN one higher; data and initialized size one record larger). The window opens on 8.0 % of device writes in the `smallfiles` workload, so this is not rare. The same window exists for record 3 in every volmark flag write (ADR-006, R-33). Upstream `ntfsfix` repairs the mirror but also sets `VOLUME_IS_DIRTY` and resets `$LogFile` unconditionally (`ntfsfix.c:295`, `:330-340`), which D4 forbids.

| Option (triage section 2) | Assessment |
|---|---|
| A. Keep `refuse`; remedy Windows `chkdsk` | Safe and simple, but a common crash locks the user out on the Mac; D3 needs an exception anyway. |
| B. Distinct state with a read-only mount | Restores D3, but needs an engine patch that tolerates the mismatch, and a mirror-newer case would read through a stale record 0. Not chosen. |
| C. Distinct state, refused, plus an explicit repair command (chosen, owner decision 2026-11-14) | No engine patch. The volume comes back without Windows, through a writer much narrower than `ntfsfix`. The writer acts on a volume the product has classified as damaged, so its gates are strict. |
| D. Automatic repair on attach | Violates "no silent auto-repair". Rejected. |

**Decision.**

1. The probe reports state `mirror_split` with reason code `MFTMIRR_STALE` only when the strict rule of ARCHITECTURE.md section 3.7 holds: both copies of records 0 to 3 are valid multi-sector records with valid fix-ups; only records 0 to 3 differ; each difference is confined to what the triage observed (the USN, and for record 0 the `$DATA` sizes, highest VCN and run list of exactly one `ntfs_mft_record_init()` step; for record 3 only the `VOLUME_IS_DIRTY` bit, accepted once T-M3-12 has recorded it); the `$MFT` USN is the successor of the mirror's; and a read-only overlay view that reads the mirror records from `$MFT` mounts. Anything else, including a torn `$MFT` record, a mirror that is newer, or equal USNs with different bytes, stays `corrupt`. In the severity order `mirror_split` ranks below `corrupt` and above `hibernated`; checks 3 to 5 run on the overlay view, so the verdict also carries their reason codes.
2. Policy: `mirror_split` gives `refuse` in both columns, never `ro` and never `rw`. The notification says that the file table and its mirror differ, that the data region was not touched, and names `ntfsmac repair --sync-mirror` and Windows `chkdsk`.
3. `ntfsmac repair --sync-mirror <bsd>` is the only product remedy. Control request `repair` with `action` `sync_mirror`, authorised exactly as ADR-017 and ADR-019 (peer not a spawned process, signed CLI, administrator password per request, typed sanitised label and last 8 hex digits of the serial). Gate (ARCHITECTURE.md section 3.2): fresh probe exactly `mirror_split`, no reason code other than `MFTMIRR_STALE` and `FLAG_DIRTY`, `logfile` `clean`, `hiberfile` `absent` or `inactive`, volume not mounted.
4. The writer is `ntfsmac-volmark sync-mirror` (ARCHITECTURE.md section 3.7): it re-checks the binding and the whole rule itself, then writes, for each differing record only, the raw on-disk bytes of the `$MFT` copy over the `$MFTMirr` copy; then `F_FULLFSYNC`, a read-back compare and an ordinary read-only mount. It never writes `$MFT`, never writes `$LogFile`, never sets or clears a flag, and refuses (nothing written) whenever the evidence does not show that `$MFT` is the newer valid copy.
5. After the write the daemon runs a fresh probe and reports its state. It does not remount and does not remove a journal entry: the next attach goes through the policy table, so a volume left with `VOLUME_IS_DIRTY` comes back `ro`.
6. `ntfsfix`, with or without `-n`, is never a product repair or check path for this state (`ntfsfix -n` prints "OK" for corrections it did not write and attempts a read/write `ntfs_mount`, `ntfsfix.c:1649`).

**What this does not guarantee.** USN order is evidence, not proof: a drive cache that persists the second write and loses the first can leave a mirror that is newer; the rule then refuses (M5), which is safe. A crafted volume can satisfy the rule; it could equally probe `clean`, so the exposure is that of D2. A cut inside the repair's own record write tears the mirror copy, which gives `corrupt` (only `chkdsk` repairs it); a rerun after any other cut is safe. The repair restores mountability, not consistency: it is not a file system check, and the CLI says so.

**Consequences.** D3 gets a stated exception (ARCHITECTURE.md section 9) and limitation N12. One more volmark mode and one more `repair` action. R-33 is widened and R-36 added. Tests `U-PROBE-MIRROR-*`, `U-VMK-11..14`, `I-DMN-27`, `H-USB-10`. Tasks T-M3-02 (detection), T-M4-16 (repair). An upstream report ("a read-only mount refuses a volume whose `$MFTMirr` is one sync behind `$MFT`", with the decoded image) is still to be drafted (criterion 3.9); sending it is a human decision.
