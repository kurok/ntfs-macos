# Architecture

Phase 1 output (2026-11-13). Companion documents: [TECHNICAL_DECISIONS.md](TECHNICAL_DECISIONS.md),
[IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md), [AGENT_TASKS.md](AGENT_TASKS.md),
[TEST_STRATEGY.md](TEST_STRATEGY.md), [RISK_REGISTER.md](RISK_REGISTER.md).

Fact labels used in all six documents:

- **[V:Sn]** verified against source `Sn` in section 12 (web page, upstream source file, or local SDK header).
- **[A]** assumption. Each assumption names the task that resolves it.
- **[U]** unknown, with "how to resolve".

## 0. Feasibility verdict

**Verdict: achievable, with two reductions in scope and one unproven link.**

| Question | Answer |
|---|---|
| Is read/write NTFS on macOS 27 with NTFS-3G over the macFUSE FSKit backend achievable? | Probably yes. Both named versions exist: macFUSE 5.5.0 (17 Oct 2026) and NTFS-3G 2026.9.28 (29 Sep 2026) [V:S1][V:S10]. macFUSE 5.5.0 is built with the macOS 27 SDK and its FSKit backend supports local volumes, read-only mounts, xattrs, preallocation and Finder eject [V:S1]. No source shows NTFS-3G itself running on that backend, so the end-to-end path is unproven until milestone M2. |
| Is "full durability" achievable? | No. FSKit has no per-file `fsync` callback, and on macOS 27 its close callback cannot return an error, so a failed flush never reaches `close(2)` [V:S1]. Durability is guaranteed only at volume-level sync points and at clean unmount (section 9). |
| Is Intel support achievable? | No, for macOS 27+. macOS 27 runs on Apple silicon only [V:S22, press coverage; confirm on Apple's compatibility page]. The release is arm64-only (ADR-009). |
| Is it "minimal installation"? | Partly. The user must install macFUSE separately and approve its file system extension once in System Settings. No kernel extension, no Reduced Security, no SIP change. |

**Blocked on the human** (details in IMPLEMENTATION_PLAN.md section 5):

1. Permission to install build and VM tools on the host (cmake, ninja, pkgconf, tart) and to start a container runtime.
2. One-time GUI approval of the macFUSE file system extension inside the macOS 27 test VM (cannot be scripted [A]).
3. Apple Developer Program membership, Developer ID certificates and full Xcode (for `notarytool`), needed only for M6 (signed, notarized package). Development and VM testing do not need them.
4. Disposable physical NTFS media and a Windows machine or VM for `chkdsk`, needed for Environment C.
5. A decision on the Paragon NTFS driver on the host before any hardware test on this Mac (section 8.4).

**Riskiest assumption:** that the libfuse2-based `ntfs-3g` binary can mount through the macFUSE FSKit backend at all.
macFUSE 5.1.0 documented `-o backend=fskit` for libfuse3 only, and a third party reported that libfuse2 rejects the option [V:S5][V:S9].
macFUSE 5.2.0 says libfuse (v2) now uses the new FSKit mount API, which suggests support, but does not say so explicitly [V:S6].
NTFS-3G requires `fuse >= 2.6.0` and has no libfuse3 support [V:S11].
Task T-M2-03 resolves this on day one of M2. The fallbacks are ordered in ADR-001.

## 1. Chosen architecture in one paragraph

Unmodified-in-substance **NTFS-3G 2026.9.28** (`ntfs-3g` FUSE server plus `libntfs-3g`) is the NTFS engine.
It runs as an **unprivileged, sandboxed worker process** on the **macFUSE 5.5.0 FSKit backend** (`-o backend=fskit,local`).
A small root **mount broker daemon** (`ntfsmacd`) watches Disk Arbitration, runs a **read-only safety probe** on every NTFS volume, decides `rw`, `ro` or `refuse` from a fixed policy table, opens the block device with exactly the access the decision allows, and passes the file descriptor to the worker.
The daemon never parses NTFS metadata. The worker never holds root. NTFS-3G is never started with `recover` or `remove_hiberfile`.
The fallback, if the FSKit backend cannot carry NTFS-3G, is a native FSKit module that links `libntfs-3g` directly (ADR-001, option D).

## 2. Component diagram

```
 User session (uid 501)                    System (root)                      Kernel / Apple
+----------------------+            +---------------------------+        +---------------------+
| Finder, apps, shell  |            | ntfsmacd (LaunchDaemon)   |        | diskarbitrationd    |
|   POSIX / NSFile API |            |  - DA session + callbacks |<------>|  disk appear/eject  |
+----------+-----------+            |  - volume state machine   |        +---------------------+
           | VFS                    |  - policy table           |
           v                        |  - session journal        |        +---------------------+
+----------------------+            |  - opens /dev/diskNsM     |------->| /dev/diskNsM (IOKit)|
| FSKit (fskitd, UBC)  |            |  - spawns probe + worker  |        +---------------------+
+----------+-----------+            +----+-----------------+----+
           | FSKit XPC                   | fd (O_RDONLY)   | fd (O_RDONLY or O_RDWR)
           v                             v                 v
+----------------------+       +----------------+  +-------------------------------+
| macFUSE FSModule     | FUSE  | ntfsmac-probe  |  | ntfsmac-worker = ntfs-3g      |
| (io.macfuse... appex)|<----->| (uid nobody,   |  | (uid = console user, sandbox) |
| closed source, signed| proto |  sandboxed)    |  |  libfuse (macFUSE, LGPL)      |
+----------------------+       |  libntfs-3g RO |  |  libntfs-3g (GPL-2.0+)        |
        ^                      +----------------+  +-------------------------------+
        | MFMount.framework XPC (io.macfuse.mount)         |
        +--------------------------------------------------+

 ntfsmac (CLI, user)  --- JSON over Unix socket /var/run/ntfsmacd.sock --->  ntfsmacd
```

Third-party components that this project does not build: macFUSE (FSModule appex, `MFMount.framework`, `libfuse`), FSKit, Disk Arbitration.

## 3. Components and boundaries

| Component | Path in repo | Runs as | Links | Responsibility | Must never |
|---|---|---|---|---|---|
| `ntfsmac-probe` | `src/probe/` | `nobody`, sandboxed, read-only fd | `libntfs-3g` (static) | Classify volume state. Print one JSON verdict. | Write to the device. Open any path except the inherited fd. |
| policy | `src/policy/` | library (pure C, no I/O) | none | Map probe verdict + user request + journal state to `rw` / `ro` / `refuse`. | Read the device or the environment. |
| session journal | `src/journal/` | library used by daemon | none | Host-side record of volumes mounted read/write and not yet cleanly unmounted. | Store anything except volume serial, label, timestamps, state. |
| `ntfsmacd` | `src/daemon/` | root (LaunchDaemon) | DiskArbitration, CoreFoundation | Detect disks, run probe, apply policy, create mount point, open device, spawn and supervise worker, journal, serve CLI. | Parse NTFS structures. Pass through a mount option outside the allow-list. |
| `ntfsmac-worker` | built from `third_party/ntfs-3g` + `third_party/patches/ntfs-3g/` | console user, sandboxed | macFUSE `libfuse`, `libntfs-3g` | Serve the file system. | Run as root. Receive `recover`, `remove_hiberfile`, `dev`, `suid`. |
| `ntfsmac` | `src/cli/` | user | none | `list`, `status`, `mount`, `unmount`, `doctor`, `repair` (explicit, confirmed). | Touch a device directly. |
| ntfsprogs | built from the same tarball | user, on explicit request | `libntfs-3g` | `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel`. Installed under `libexec`, not on `PATH`. | Run automatically. |
| packaging | `packaging/` | installer | none | `.pkg`, launchd plist, uninstaller, preflight checks. | Bundle macFUSE. Change security settings. |

### 3.1 Safety probe interface

Command: `ntfsmac-probe --fd <N> [--bsd-name diskNsM]`. Reads only. Never exits non-zero for a volume condition; exit status reports tool failure only.

Output (stdout, one JSON object, schema version 1):

```json
{
  "schema": 1,
  "is_ntfs": true,
  "ntfs_version": "3.1",
  "label": "DATA",
  "serial": "0x1A2B3C4D5E6F7788",
  "cluster_size": 4096,
  "sector_size": 512,
  "volume_flags": ["DIRTY"],
  "logfile": "clean | unclean | v2_fast_startup | unreadable",
  "hiberfile": "absent | inactive | hibernated | unreadable",
  "bitlocker": false,
  "state": "clean | dirty_flag | unclean_log | hibernated | fast_startup | unsupported | corrupt | unknown",
  "reasons": ["VOLUME_IS_DIRTY set in $Volume"]
}
```

| Exit | Meaning |
|---|---|
| 0 | Verdict printed. |
| 2 | Usage error. |
| 3 | I/O error reading the device (daemon treats the volume as `unknown`). |
| 4 | Internal error or timeout (daemon treats the volume as `unknown`). |

Checks, in order. Each uses `libntfs-3g` opened with `NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC`, which skips the hibernation and log checks inside the library and performs no writes [V:S11]:

| # | Check | Source of truth | Result |
|---|---|---|---|
| 1 | Boot sector OEM ID `NTFS    `; `-FVE-FS-` means BitLocker | bytes 3..10 of sector 0 | `unsupported` if not NTFS or BitLocker |
| 2 | `ntfs_mount` read-only succeeds | `libntfs-3g` | `corrupt` on failure |
| 3 | `$Volume` flags: `VOLUME_IS_DIRTY`, `VOLUME_MODIFIED_BY_CHKDSK`, upgrade/resize flags | `vol->flags` | `dirty_flag` |
| 4 | `$LogFile`: `ntfs_check_logfile` + `ntfs_is_logfile_clean`; restart page major version 2 | `logfile.h` API; version-2 rule is the same one `volume.c` applies [V:S11] | `unclean_log` or `fast_startup` |
| 5 | `/hiberfil.sys`: absent is safe; size < 4096 or header `hibr`/`HIBR` is hibernated | same rule as `ntfs_volume_check_hiberfile` [V:S11] | `hibernated` |
| 6 | Anything unexpected (unknown NTFS version, check could not run) | | `unknown` |

`state` is the most severe finding in the order `unsupported > corrupt > hibernated > fast_startup > unclean_log > dirty_flag > unknown > clean`.

### 3.2 Policy table

| Probe `state` | Journal says we left it open | Default action | With explicit `--read-write` request | Notes |
|---|---|---|---|---|
| `clean` | no | `rw` | `rw` | |
| `clean` | yes | `ro` | `ro` | Our previous session ended uncleanly. Message tells the user to run `chkdsk` on Windows. |
| `dirty_flag` | any | `ro` | `ro` | Cleared only by Windows `chkdsk`, or by the explicit `ntfsmac repair --clear-dirty` (wraps `ntfsfix -d`, typed confirmation, never automatic). |
| `unclean_log` | any | `ro` | `ro` | NTFS-3G's `recover` option would reset `$LogFile`; it is banned (ADR-005). |
| `hibernated`, `fast_startup` | any | `ro` | `ro` | `remove_hiberfile` is banned. Message: boot Windows, run `powercfg /h off`, shut down fully. |
| `unknown` | any | `ro` | `ro` | |
| `corrupt`, `unsupported` | any | `refuse` | `refuse` | Volume is left to the system. |

There is no override that turns an unsafe state into a read/write mount. The only path back to `rw` is to make the volume clean.
`ro` means both: the device fd is opened `O_RDONLY` by the daemon and the worker gets `-o ro`.

### 3.3 Worker invocation

```
ntfsmac-worker /dev/fd/<N> /Volumes/<name> -o <options>
```

| Option set | Options |
|---|---|
| Always | `backend=fskit,local,volname=<label>,fsname=<bsd name>,norecover,noatime,windows_names,big_writes,streams_interface=xattr,uid=<console uid>,gid=<console gid>,fmask=0133,dmask=0022` |
| Read-only | add `ro` |
| Opt-in (config) | `hide_hid_files`, `permissions` or `usermapping=<file>` (ADR-012) |
| Banned (daemon rejects) | `recover`, `remove_hiberfile`, `dev`, `suid`, `allow_root`, `efs_raw`, any option not in the allow-list |

Option semantics are from the NTFS-3G man page [V:S11]. `recover` is NTFS-3G's default, so `norecover` is mandatory on every invocation.
Whether `fsname`, `volname`, `big_writes` and `noatime` are accepted by the FSKit backend is [A]; macFUSE states that most kernel-handled options are not yet implemented there [V:S2]. T-M2-04 records the accepted set and this table is then corrected.

### 3.4 Daemon control protocol

Unix stream socket `/var/run/ntfsmacd.sock`, mode 0660, group `staff`. One JSON request per line, one JSON reply per line.
The daemon authenticates the peer with `LOCAL_PEERCRED` and accepts mount and unmount requests only from the console user or a member of `admin`.

| Request | Fields | Reply |
|---|---|---|
| `list` | none | array of volumes: `bsd`, `label`, `state`, `mount` (`rw`/`ro`/`none`), `mountpoint`, `reason` |
| `status` | `bsd` | one volume record plus last probe verdict |
| `mount` | `bsd`, `mode` (`auto`/`ro`) | result, mount point, effective mode, reason |
| `unmount` | `bsd`, `force` (bool) | result |
| `doctor` | none | macFUSE version, extension state, conflicting NTFS drivers found, journal entries |

### 3.5 Session journal

Directory `/var/db/ntfs-macos/journal/`, one file per volume serial, mode 0600 root.
Written and `F_FULLFSYNC`ed **before** the worker is started read/write; removed after the worker exits with status 0 following an unmount request.
A file that exists when the volume next appears means the previous read/write session did not end cleanly (policy row 2).
The journal is per host. A volume yanked from this Mac and then attached to another machine is covered by the on-disk dirty marker (ADR-006), not by the journal.

## 4. Data flows

### 4.1 Disk appears and is mounted

1. `diskarbitrationd` reports a new disk. `ntfsmacd` filters on `DAMediaContent` (`Windows_NTFS`, GPT basic-data GUID `EBD0A0A2-...`, or whole-disk media) and `DAMediaLeaf`.
2. Conflict check: if another third-party NTFS bundle is installed (for example `/Library/Filesystems/ufsd_NTFS.fs`), the daemon is in **passive mode** and stops here (section 8.4).
3. In active mode the daemon answers the Disk Arbitration mount-approval callback with a dissenter for this disk, so Apple's read-only NTFS driver does not auto-mount it first [A: T-M4-02 verifies the dissenter is honoured on macOS 27].
4. The daemon opens the device `O_RDONLY`, spawns `ntfsmac-probe` with that fd, and waits at most 20 s.
5. Policy (section 3.2) gives `rw`, `ro` or `refuse`. On `refuse` the dissent is withdrawn and the system handles the disk.
6. For `rw`: write the journal entry, then set the on-disk dirty marker (ADR-006, only when enabled). Open the device `O_RDWR`. For `ro`: open `O_RDONLY`.
7. Create `/Volumes/<label>` (unique suffix on collision), owned by the console user.
8. Spawn the worker as the console user inside the sandbox profile, with the fd inherited. Wait for the mount to appear (`statfs` on the mount point shows a new `f_fsid`) or for the worker to exit.
9. Record state `Mounted(rw|ro)`. Post a user notification that states the mode and, for `ro`, the reason.

### 4.2 Read

`read(2)` -> VFS -> FSKit (unified buffer cache; a cached page returns without reaching the module) -> macFUSE FSModule -> `FUSE_READ` -> `ntfs-3g` `ntfs_fuse_read` -> `libntfs-3g` runlist lookup -> `pread` on the device fd.
macFUSE 5.5.0 turns an empty `FUSE_READ` reply into `EIO` and zero-fills short-read tails to mitigate an FSKit corruption bug [V:S1][V:S7]. NTFS-3G must therefore never answer a non-EOF read with zero bytes; T-M3-07 tests reads at and across EOF.

### 4.3 Write

`write(2)` -> VFS -> FSKit (data may sit in the buffer cache; `write` returning does not mean the worker has seen it [A: T-M2-06 measures]) -> FSModule -> `FUSE_WRITE` -> `ntfs_fuse_write` -> `libntfs-3g` allocates clusters, updates MFT and bitmap -> `pwrite` on the device fd.
`libntfs-3g` has no write-back data cache of its own; it issues device writes synchronously inside the request [A: stated from NTFS-3G design knowledge; T-M3-10 confirms by tracing]. NTFS-3G does not journal; metadata updates are ordered writes, not transactions.

### 4.4 Sync

| Application call | What reaches the worker | Evidence |
|---|---|---|
| `fsync(fd)` | Not delivered per file. FSKit has no per-file fsync callback. | [V:S1] |
| `close(fd)` | `FUSE_FLUSH` (macFUSE 5.5.0). The error is logged and dropped on macOS 27. | [V:S1] |
| `sync(2)`, unmount, periodic system sync | FSKit `synchronize(flags:)` on the volume [V:S16]. Mapping to a FUSE opcode (`FUSE_FSYNC`/`FUSE_FSYNCDIR`) is [A]; T-M2-06 records it. | |
| `fcntl(F_FULLFSYNC)` | [U]. Resolve with T-M2-06. | |

Worker side: `ntfs_fuse_fsync` calls `ntfs_device_sync`, which is plain `fsync` on the device fd [V:S11]. On macOS plain `fsync` does not flush the drive write cache, so patch `0002` makes the Apple device-sync path call `fcntl(F_FULLFSYNC)` with a fall back to `fsync`.

### 4.5 Unmount and eject

1. Trigger: Finder eject, `diskutil unmount`, `umount`, or `ntfsmac unmount`. macFUSE 5.5.0 supports Finder unmount for `local` volumes [V:S1].
2. FSKit calls `synchronize`, then `unmount`, on the FSModule; the worker receives `FUSE_DESTROY`, calls `ntfs_umount`, syncs the device and exits 0.
3. The daemon sees the exit, runs `F_FULLFSYNC` on its own copy of the fd, clears the on-disk dirty marker (ADR-006), removes the journal entry, closes the fd, removes the mount point directory.
4. For "eject" of the physical disk the daemon then calls `DADiskEject` on the whole disk. Whether Finder's eject button on a macFUSE `local` volume also ejects the underlying media without daemon help is [U]; T-M4-03 resolves it.

A worker exit with non-zero status, or without a preceding unmount request, leaves the journal entry and the dirty marker in place.

## 5. Threading and locking model

| Layer | Model | Consequence |
|---|---|---|
| FSKit to FSModule | Requests arrive over NSXPC on a serial queue unless the module redispatches [V:S15]. macFUSE internals are closed; 5.5.0 notes "fast paths for uncontended locks" [V:S1]. | Out of our control. |
| Worker (`ntfs-3g`) | Single-threaded `fuse_loop`, not `fuse_loop_mt` [V:S11]. `libntfs-3g` is not thread-safe [A: consistent with the single-threaded loop]. | All file system operations are strictly serialized. No locking is added. The multi-threaded loop must not be enabled. |
| Daemon | One main dispatch queue for Disk Arbitration and socket events. One serial queue per volume, keyed by BSD name, running the state machine `Appeared -> Probing -> Deciding -> Mounting -> Mounted -> Unmounting -> Gone`, plus `Failed`. | No shared mutable state between volumes except the journal directory (one file per volume, atomic rename). |
| Probe | Single thread, hard timeout enforced by the daemon (SIGKILL at 20 s). | |
| Device exclusivity | The daemon holds the only open descriptors. The probe's read-only fd is closed before the worker's fd is opened. A second `mount` request for a volume not in state `Appeared` or `Failed` is rejected. | Two writers on one volume cannot happen through this product. Another driver (Paragon) is excluded by passive mode. |

Advisory locks: the FSKit backend does not forward `fcntl`/`flock` locks to the file system; they are local to the Mac [V:S15][V:S1, remote locking is kernel-backend only]. This is acceptable for a locally attached disk.

## 6. Security model

Threat: a malicious or corrupt NTFS volume (the user plugs in an untrusted USB drive). NTFS-3G had four security releases in 2026 fixing heap overflows, use-after-free and out-of-bounds bugs triggered by crafted volumes [V:S10]. The metadata parser must be assumed exploitable.

| Control | Detail |
|---|---|
| Privilege separation | Only `ntfsmacd` is root. It opens devices and creates `/Volumes` entries. It contains no NTFS parsing. |
| Unprivileged parser | Probe runs as `nobody`; worker runs as the console user. Neither can open another device. |
| Sandbox | Both run under a `sandbox_init` profile (`integration/sandbox/*.sb`): no network, no process exec, file access limited to the inherited fd, the mount point, and for the worker the Mach service `io.macfuse.mount` (required for sandboxed FUSE servers [V:S6]). `sandbox_init` is deprecated but functional [A: T-M4-06 verifies on macOS 27; if it fails, drop to uid separation only and record the residual risk]. |
| Least access | Read-only decisions get an `O_RDONLY` descriptor. A parser bug cannot write to a volume that policy marked unsafe. |
| Option allow-list | The daemon builds the worker command line itself. Client-supplied strings are limited to `bsd` and `mode`. |
| Client authentication | `LOCAL_PEERCRED` on the control socket; console user or `admin` only. |
| Mount flags | FSKit mounts are `nodev,nosuid` by construction [A]. `uid`/`gid`/`fmask`/`dmask` map every file to the console user; the execute bit is cleared on files (`fmask=0133`). |
| Supply chain | Dependencies pinned by version and SHA-256 in `third_party/deps.lock`; the macFUSE installer package signature is checked before use (IMPLEMENTATION_PLAN.md section 3). |
| No system weakening | No kext, no SIP change, no Reduced Security, no replaced Apple binary. The installer writes only to `/Library/LaunchDaemons`, `/usr/local`, `/Library/Application Support/ntfs-macos`, `/var/db/ntfs-macos`. |
| Hardening | Release build: hardened runtime, `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-ftrivial-auto-var-init=zero`. CI builds with ASan and UBSan (TEST_STRATEGY.md). |

Residual risk: a worker compromise gives the attacker the console user's identity inside the sandbox plus write access to that one volume.

## 7. Failure behaviour

| Event | Behaviour | Guarantee after the event |
|---|---|---|
| Device removed without eject | Device I/O returns `ENXIO`/`EIO`; the worker returns errors to FUSE; the daemon receives the Disk Arbitration disappearance, force-unmounts the mount point, kills the worker after 5 s. Journal entry and dirty marker remain. | Data not yet written by the worker is lost. Metadata may be mid-update. Next mount on this Mac is `ro` (journal). On Windows the dirty flag triggers a `chkdsk` prompt. |
| Worker crash or `SIGKILL` | macFUSE disconnects the volume (`daemon_timeout`, default 60 s [V:S4], supported on the FSKit backend since 5.5.0 [V:S1]); the daemon detects the exit immediately and force-unmounts. Journal and dirty marker remain. | Same as above. |
| `ntfsmacd` crash | launchd restarts it. On start it reconciles: for each journal entry, if a live worker and mount exist it re-adopts them [A: T-M4-05; if re-adoption is impractical, the daemon force-unmounts and the volume becomes `ro` on next mount]. | No data path goes through the daemon, so open files are unaffected. |
| macFUSE FSModule crash | Volume disappears or returns `EIO` [V:S7 describes this failure mode]. Worker exits non-zero. | Same as worker crash. |
| I/O error from the device | `libntfs-3g` returns the error; the application gets `EIO` on `read`/`write`. Errors on `close` are lost (FSKit limit). After the first write error the daemon marks the session failed, requests unmount, and the next mount is `ro`. | Earlier acknowledged writes are not rolled back. |
| Host panic or power loss | No clean unmount. Journal and dirty marker remain. | Same as device removal. |
| Volume full | `ENOSPC` from `libntfs-3g` on `write`. | No corruption expected; tested in T-M5-06. |
| Probe timeout or crash | State `unknown`, action `ro`. | No write happened. |
| macFUSE missing, too old, or extension not approved | Daemon reports it in `doctor` and in the notification; the disk is left to the system (Apple read-only driver). | No change to the volume. |

## 8. macOS integration

### 8.1 Disk Arbitration

The daemon is the integration point, not an FSKit module of our own (ADR-003). It uses the public DiskArbitration C API: `DARegisterDiskAppearedCallback`, `DARegisterDiskDisappearedCallback`, `DARegisterDiskMountApprovalCallback`, `DARegisterDiskUnmountApprovalCallback`, `DADiskUnmount`, `DADiskEject`.

Facts from this host (macOS 27.2.1) that shape the design [V:S17]:

| Driver | Type | Claims `Windows_NTFS` with `FSProbeOrder` |
|---|---|---|
| Paragon `ufsd_NTFS.fs` | kext-backed bundle in `/Library/Filesystems` | 1 |
| Apple `ntfs.fs` | system bundle, read-only | 1000 |
| Apple `com.apple.fskit.exfat` | FSKit module | 2000 (probes NTFS-typed partitions for exFAT) |

Kext-backed bundles are probed before FSKit modules, and Apple's read-only NTFS driver masks a third-party NTFS FSKit module in automount; Apple called this hard to change [V:S14]. A probe result of "usable but limited" suppresses automount [V:S13]. These are the reasons a native FSKit module would also need a Disk Arbitration helper.

### 8.2 Finder

`local` + `volname` make the volume appear as a local disk with an eject control [V:S1]. Custom volume icon: none in v1.
Finder writes `.DS_Store`, `._*` companions or xattrs. With `streams_interface=xattr`, xattrs are stored as NTFS named streams [V:S11]; T-M3-08 checks Finder copy, tags and quarantine attributes.

### 8.3 Mount and unmount commands

`ntfsmac mount|unmount`, Finder eject, `diskutil unmount <mountpoint>` and `umount` are supported. `diskutil mount diskNsM` invokes the system's own driver selection and is **not** routed to this product in v1; it is documented as unsupported.

### 8.4 Coexistence with Paragon NTFS and other drivers

| Situation | Behaviour |
|---|---|
| Installer finds `/Library/Filesystems/ufsd_NTFS.fs`, `tuxera_ntfs.fs`, or another non-Apple NTFS bundle | Warns, installs, sets passive mode. |
| Passive mode | No mount-approval dissent, no automount. `ntfsmac mount` works only on a volume that is currently unmounted, and refuses if the other driver has it mounted. |
| fuse-t present (as on this host) | No conflict: fuse-t registers its own FSKit module and is not an NTFS claimant [V:S17]. Our worker links macFUSE's `libfuse` by absolute path, never fuse-t's. |
| Tests | Never run mount tests on the developer host. All mount tests run in the macOS VM, which has no Paragon driver (TEST_STRATEGY.md section 2). |

This project never unloads, disables or removes another vendor's driver.

## 9. Durability guarantees

What the product **does** guarantee, once the tests named in TEST_STRATEGY.md pass:

| # | Guarantee | Mechanism |
|---|---|---|
| D1 | After a successful eject or unmount, every byte the applications wrote is on the device and the drive cache has been flushed. | FSKit `synchronize` + `unmount`, `ntfs_umount`, `F_FULLFSYNC` in worker and daemon. |
| D2 | A volume that policy classified as unsafe is never written, even if the NTFS parser misbehaves. | `O_RDONLY` descriptor. |
| D3 | A volume left without clean unmount by this product is mounted read-only on next attach to this Mac, and is flagged dirty for Windows. | Journal + dirty marker. |
| D4 | Windows hibernation state and `$LogFile` content are never discarded automatically. | `norecover` always; `remove_hiberfile` and `recover` banned by the option allow-list. |
| D5 | Data read back equals data written (hash equality across unmount and remount, and against Linux NTFS-3G). | Integrity suite. |

What the product **cannot** guarantee, and must not claim:

| # | Not guaranteed | Reason |
|---|---|---|
| N1 | `fsync(fd)` returning 0 means that file's data is on stable storage. | FSKit has no per-file fsync callback [V:S1]. |
| N2 | `close(fd)` returning 0 means the flush succeeded. | FSKit close cannot return errors on macOS 27 [V:S1]. |
| N3 | `write(2)` returning means the worker has received the data. | FSKit buffer cache [A]. |
| N4 | Atomicity or rollback of a multi-step metadata update after power loss or disconnect. | NTFS-3G does not replay or write the NTFS journal. |
| N5 | `F_FULLFSYNC` semantics for applications (databases, Time Machine-style tools). | [U] until T-M2-06. Until then documented as "not supported for workloads that need fsync durability". |
| N6 | Correct behaviour when a file is changed behind the mount. | FSKit is slow to observe remote changes; the 5.5.0 mitigation can return `EIO` [V:S1]. Not applicable while the daemon enforces exclusive access. |
| N7 | `mmap` write-back ordering and error reporting. | [U]; a report of `SIGBUS` on mapped files after a module crash exists [V:S7]. T-M3-11 tests mapped I/O. |
| N8 | POSIX and BSD advisory locks visible to another host. | Local only. |

User-facing documentation must carry N1 to N5 verbatim in a "Limitations" section.

## 10. Build and deployment view

| Artifact | Built by | Installed at |
|---|---|---|
| `ntfsmac-worker`, `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel` | NTFS-3G autotools build driven by `scripts/build-ntfs3g.sh`, `--with-fuse=external`, static `libntfs-3g` | `/usr/local/libexec/ntfs-macos/` |
| `ntfsmac-probe`, `ntfsmacd`, `ntfsmac` | CMake + Ninja (ADR-007) | `/usr/local/libexec/ntfs-macos/`, `/usr/local/bin/ntfsmac` |
| `org.ntfs-macos.ntfsmacd.plist` | static file | `/Library/LaunchDaemons/` |
| sandbox profiles, default config | static files | `/Library/Application Support/ntfs-macos/` |
| `ntfs-macos-<ver>.pkg`, `uninstall.sh` | `pkgbuild`/`productbuild` (present in base macOS) | release asset |

Target: arm64, deployment target macOS 27.0. External runtime prerequisite: macFUSE >= 5.5.0 installed by the user.

## 11. Open unknowns

| ID | Unknown | How to resolve | Blocks |
|---|---|---|---|
| U1 | Does macFUSE's libfuse2 accept `backend=fskit`? | T-M2-03 | Everything |
| U2 | Does a macOS 27 guest under Virtualization.framework load and run the macFUSE FSKit module? (kexts do not load in VMs [V:S3]; FSKit is user space, so expected yes.) | T-M2-02 | VM testing |
| U3 | Can the FSKit extension be enabled without the GUI (`pluginkit -e use`)? | T-M2-02 | VM automation |
| U4 | Which FUSE opcodes result from `fsync`, `F_FULLFSYNC`, `sync`, unmount? | T-M2-06 | Section 9 wording |
| U5 | Does `ntfs-3g` accept `/dev/fd/N` (device size and sector-size ioctls on a re-opened descriptor)? | T-M2-05; fallback patch `0003` | Privilege separation |
| U6 | Can the FUSE server run as the console user when started by a root daemon (FSKit modules are enabled per user; root-run `mount -F` fails [V:S14])? | T-M4-01 | Daemon design |
| U7 | Is a Disk Arbitration mount-approval dissenter still honoured for external NTFS disks on macOS 27? | T-M4-02; fallback: let Apple's driver mount read-only, then unmount and remount | Automount |
| U8 | Does Finder eject of the volume also eject the media? | T-M4-03 | UX |
| U9 | Does NTFS-3G already set `VOLUME_IS_DIRTY` during a read/write session? | T-M3-01 (source read + test) | ADR-006 |
| U10 | Raw (`/dev/rdisk`) versus buffered (`/dev/disk`) device node: alignment handling in `unix_io.c` on Darwin, and performance. | T-M3-10 | Performance, D1 |
| U11 | tart licence terms and macOS 27 guest support in the current release (only a secondary source found [V:S20]). | T-M1-04 | VM tooling |

## 12. Sources

| ID | Source |
|---|---|
| S1 | macFUSE releases and 5.5.0 notes: https://github.com/macfuse/macfuse/releases , https://github.com/macfuse/macfuse/releases/tag/macfuse-5.5.0 |
| S2 | macFUSE wiki, FUSE Backends: https://github.com/macfuse/macfuse/wiki/FUSE-Backends |
| S3 | macFUSE wiki, Getting Started (kext approval, VM limitation): https://github.com/macfuse/macfuse/wiki/Getting-Started |
| S4 | macFUSE wiki, Mount Options: https://github.com/macfuse/macfuse/wiki/Mount-Options |
| S5 | macFUSE 5.1.0 announcement: https://macfuse.github.io/2025/10/30/macfuse-5.1.0.html |
| S6 | macFUSE 5.2.0 notes: https://github.com/macfuse/macfuse/releases/tag/macfuse-5.2.0 |
| S7 | macFUSE issue 1188 (zero-byte READ reply): https://github.com/macfuse/macfuse/issues/1188 |
| S8 | macFUSE licence and open-source status: https://raw.githubusercontent.com/macfuse/macfuse/release/macfuse/LICENSE.txt , https://github.com/macfuse/macfuse/wiki/Open-Source-Status |
| S9 | jfuse issue 145 (libfuse2 rejects `backend=fskit`): https://github.com/cryptomator/jfuse/issues/145 |
| S10 | NTFS-3G releases and 2026.9.28 notes: https://github.com/tuxera/ntfs-3g/releases , https://github.com/tuxera/ntfs-3g/releases/tag/2026.9.28 |
| S11 | NTFS-3G source, branch `edge`: `libntfs-3g/volume.c`, `src/ntfs-3g.c`, `src/ntfs-3g.8.in`, `configure.ac` under https://raw.githubusercontent.com/tuxera/ntfs-3g/edge/ |
| S12 | NTFS-3G FAQ: https://github.com/tuxera/ntfs-3g/wiki/NTFS-3G-FAQ |
| S13 | Apple Developer Forums 786270 (FSModule probing and automount): https://developer.apple.com/forums/thread/786270 |
| S14 | Apple Developer Forums 788609 (FSKit and physical disks, NTFS kext precedence): https://developer.apple.com/forums/thread/788609 |
| S15 | Apple Developer Forums 831338 (FSKit versus FUSE gaps): https://developer.apple.com/forums/thread/831338 |
| S16 | FSKit headers in the local SDK: `/Library/Developer/CommandLineTools/SDKs/MacOSX27.2.sdk/System/Library/Frameworks/FSKit.framework/Headers/` (`FSVolume.h`, `FSResource.h`, `FSUnaryFileSystem.h`, `FSKitDefines.h`) |
| S17 | Read-only inspection of this host: `/System/Library/ExtensionKit/Extensions/com.apple.fskit.exfat.appex/Contents/Info.plist`, `/System/Library/Filesystems/ntfs.fs`, `/Library/Filesystems/ufsd_NTFS.fs`, `pluginkit -m -p com.apple.fskit.fsmodule`, `man mount`, `/Library/Application Support/fuse-t/LICENSE.rtf` |
| S18 | fuse-t: https://github.com/macos-fuse-t/fuse-t |
| S19 | FSKit-based NTFS products (vendor claims): https://apps.apple.com/ca/app/id6782636021 , https://iboysoft.com/ntfs-for-mac/iboysoft-ntfs-for-mac-fskit-based-user-space-driver.html |
| S20 | tart with macOS 27 guests (secondary source): https://www.starryhope.com/mac/tart-macos-linux-vms/ |
| S21 | GitHub Actions `xcode-27` image on macOS 27: https://github.blog/changelog/2026-10-15-xcode-27-runner-image-generally-available |
| S22 | macOS 27 drops Intel Macs (press): https://hothardware.com/news/apple-drops-intel-macs-macos-27 |
| S23 | WWDC26 session 224 (Virtualization, USB accessory access): https://developer.apple.com/videos/play/wwdc2026/224/ |
| S24 | Apple FSKit documentation: https://developer.apple.com/documentation/fskit |
