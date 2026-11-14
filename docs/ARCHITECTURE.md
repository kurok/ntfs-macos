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
| Is read/write NTFS on macOS 27 with NTFS-3G over the macFUSE FSKit backend achievable? | Probably yes. Both named versions exist: macFUSE 5.5.0 (17 Oct 2026) and NTFS-3G 2026.9.28 (tagged 28 Sep 2026, published 29 Sep 2026 UTC) [V:S1][V:S10]. macFUSE 5.5.0 is built with the macOS 27 SDK and its FSKit backend supports local volumes, read-only mounts, xattrs, preallocation and Finder eject [V:S1]. No source shows NTFS-3G itself running on that backend, so the end-to-end path is unproven until milestone M2. |
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
The daemon never parses NTFS metadata. Three unprivileged processes do: the probe (read-only), the short-lived `ntfsmac-volmark` step that sets and clears the on-disk dirty flag around every read/write session (ADR-006), and the worker. The worker never holds root. NTFS-3G is never started with `recover` or `remove_hiberfile`.
Strings that come from the disk (the volume label) are untrusted: the daemon sanitises them and validates the final worker argument vector before any spawn (ADR-016).
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
| FSKit (fskitd, UBC)  |            |  - spawns probe, volmark  |        +---------------------+
|                      |            |    and worker             |
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

Not drawn: `ntfsmac-volmark`, a short-lived process (uid `nobody`, sandboxed) that receives the same `O_RDWR` descriptor as the worker, only for `rw` decisions, once before the worker starts and once after it exits cleanly (section 3.6, ADR-006).

Third-party components that this project does not build: macFUSE (FSModule appex, `MFMount.framework`, `libfuse`), FSKit, Disk Arbitration.

## 3. Components and boundaries

| Component | Path in repo | Runs as | Links | Responsibility | Must never |
|---|---|---|---|---|---|
| `ntfsmac-probe` | `src/probe/` | `nobody`, sandboxed, read-only fd | `libntfs-3g` (static) | Classify volume state. Print one JSON verdict. | Write to the device. Open any path except the inherited fd. |
| `ntfsmac-volmark` | `src/volmark/` | `nobody`, sandboxed (`integration/sandbox/volmark.sb`), inherited `O_RDWR` fd, timeout 20 s before its first write, no `SIGKILL` during the flag write (section 3.6) | `libntfs-3g` (static, unpatched for sync: upstream `ntfs_fsync` already uses `F_FULLFSYNC`, section 4.4) | Set `VOLUME_IS_DIRTY` before a read/write session and clear it after a clean one; also the only writer for `ntfsmac repair --clear-dirty` (section 3.6, ADR-006, ADR-017). | Run for an `ro` decision. Touch any `$Volume` flag bit other than `VOLUME_IS_DIRTY`. Write anything except the `$MFT` and `$MFTMirr` copies of the `$Volume` record. Mount with `NTFS_MNT_RECOVER`, `NTFS_MNT_MAY_RDONLY`, `NTFS_MNT_IGNORE_HIBERFILE` or `NTFS_MNT_RDONLY` for a write step. Open any path. |
| policy | `src/policy/` | library (pure C, no I/O) | none | Map probe verdict + user request + journal state to `rw` / `ro` / `refuse` and a journal action (`keep`/`remove`); decide whether `journal-clear` and `repair` requests are allowed. | Read the device or the environment. |
| sanitize | `src/sanitize/` | library (pure C, no I/O) used by the daemon | none | Strict parser for the probe's JSON verdict; volume-label sanitiser; final worker argv validator (ADR-016). | Pass an input byte through without checking it against the allow-list. |
| session journal | `src/journal/` | library used by daemon | none | Host-side record of volumes mounted read/write and not yet cleanly unmounted (section 3.5). | Store anything except the fields listed in section 3.5. |
| `ntfsmacd` | `src/daemon/` | root (LaunchDaemon) | DiskArbitration, CoreFoundation | Detect disks, run probe, apply policy, create mount point, open device, bind the probed volume to the opened device (section 4.1, ADR-020), spawn volmark and worker, supervise worker and its session channel, journal, serve CLI. | Parse NTFS structures (comparing raw bytes by hash is not parsing). Pass through a mount option outside the allow-list. Use a disk-derived string that has not passed the sanitiser. Build a path or file name from a probe string (only from parsed integers, ADR-016). |
| `ntfsmac-worker` | built from `third_party/ntfs-3g` + `third_party/patches/ntfs-3g/` | console user, sandboxed | macFUSE `libfuse`, `libntfs-3g` | Serve the file system. Write the session records `S`, `E`, `W` and `U` on the inherited session descriptor (patch 0006, section 7). | Run as root. Receive `recover`, `remove_hiberfile`, `dev`, `suid`. Pass the session descriptor to any child (`FD_CLOEXEC`, section 7). |
| `ntfsmac` | `src/cli/` | user | none | `list`, `status`, `mount`, `unmount`, `doctor`, `journal-clear` and `repair` (both explicit, typed confirmation plus an administrator password prompt validated by the daemon, ADR-019). Maps probe reason codes to text (ADR-016). | Touch a device directly. Print a disk-derived string other than the sanitised label. |
| ntfsprogs | built from the same tarball | user, on explicit request | `libntfs-3g` | `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel`. Installed under `libexec`, not on `PATH`. | Run automatically. Be invoked by the daemon or the CLI (repair uses `ntfsmac-volmark`, ADR-017). |
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
  "total_sectors": 131071,
  "cluster_size": 4096,
  "sector_size": 512,
  "volume_flags": ["DIRTY"],
  "logfile": "clean | unclean | v2_fast_startup | unreadable",
  "logfile_lsn": "0x0000000001A2B3C4",
  "hiberfile": "absent | inactive | hibernated | unreadable",
  "bitlocker": false,
  "state": "clean | dirty_flag | unclean_log | hibernated | fast_startup | unsupported | corrupt | unknown",
  "reason_codes": ["FLAG_DIRTY"]
}
```

`logfile_lsn` is the `current_lsn` of the newer valid `$LogFile` restart area, or `null` when `logfile` is `unreadable`. It is used only as evidence that another system mounted the volume (section 3.5).

Every field is derived from untrusted on-disk data by a parser that must be assumed exploitable (section 6). `label` in particular is attacker-controlled text. The daemon parses the output with the strict parser in `src/sanitize/` (at most 64 KiB, UTF-8 only, no `\u0000`, no duplicate keys, every enum checked; any failure means state `unknown`) and never uses `label` before the sanitiser in ADR-016 has run.

**Field grammar (schema 1).** Every key below is required and no other key is allowed. Numbers are JSON integers without sign, fraction, exponent or leading zero. Nesting depth is at most 2. Any violation gives state `unknown` (ADR-016, `U-VERDICT-*`).

| Key | Type and grammar | Use in the daemon |
|---|---|---|
| `schema` | integer, exactly `1` | |
| `is_ntfs`, `bitlocker` | `true` or `false` | |
| `ntfs_version` | string `^[1-3]\.[0-9]$`, or `null` | display only, from the parsed digits |
| `label` | string, at most 512 bytes of UTF-8 (128 UTF-16 units), no `\u0000` | only as input to the label sanitiser |
| `serial` | string `^0x[0-9A-F]{16}$` | parsed to a 64-bit integer; every later use (journal file name, tombstone name, `confirm_serial`, display) is re-formatted by the daemon from that integer as 16 upper-case hex digits; the input string is discarded |
| `total_sectors` | integer, 1 to 2^63 - 1 | journal entry and device binding (section 4.1) |
| `cluster_size` | integer, a power of two from 512 to 2097152 | |
| `sector_size` | integer, one of 512, 1024, 2048, 4096 | |
| `volume_flags` | array of at most 9 distinct strings from `DIRTY`, `RESIZE_LOG_FILE`, `UPGRADE_ON_MOUNT`, `MOUNTED_ON_NT4`, `DELETE_USN_UNDERWAY`, `REPAIR_OBJECT_ID`, `CHKDSK_UNDERWAY`, `MODIFIED_BY_CHKDSK`, `UNKNOWN_BITS` | gates in section 3.2 |
| `logfile`, `hiberfile`, `state` | enum strings as shown above | policy |
| `logfile_lsn` | string `^0x[0-9A-F]{16}$`, or `null`; `null` if and only if `logfile` is `unreadable` | parsed to a 64-bit integer for the evidence rule |
| `reason_codes` | array of at most 16 distinct codes from the fixed list in `include/ntfsmac/probe.h` (for example `NOT_NTFS`, `BITLOCKER`, `MOUNT_FAILED`, `FLAG_DIRTY`, `FLAG_CHKDSK_UNDERWAY`, `FLAG_PENDING_WORK`, `FLAG_UNKNOWN_BITS`, `LOG_UNCLEAN`, `LOG_V2`, `LOG_UNREADABLE`, `HIBER_SIGNATURE`, `HIBER_WAKE`, `HIBER_SHORT`, `HIBER_UNREADABLE`, `VERSION_UNKNOWN`, `CHECK_FAILED`) | mapped to text by the daemon and the CLI from their own tables; no probe-supplied text is ever shown |

Cross-field rule: `state` `clean` with a non-empty `reason_codes`, or a non-`clean` state with an empty one, gives `unknown`.

| Exit | Meaning |
|---|---|
| 0 | Verdict printed. |
| 2 | Usage error. |
| 3 | I/O error reading the device (daemon treats the volume as `unknown`). |
| 4 | Internal error or timeout (daemon treats the volume as `unknown`). |

Checks, in order. Each uses `libntfs-3g` opened with `NTFS_MNT_RDONLY | NTFS_MNT_FORENSIC`, which skips the hibernation and log checks inside the library and performs no writes [V:S11]. (In `volume.c` that check block is guarded by `!(flags & (RDONLY | FORENSIC))`, so `RDONLY` alone already skips it; `FORENSIC` additionally skips the `$TXF_DATA` fix-up. Both flags are passed so that the probe does not depend on that detail. This is not a reason to believe `FORENSIC` is required for safety.)

| # | Check | Source of truth | Result |
|---|---|---|---|
| 1 | Boot sector OEM ID `NTFS    `; `-FVE-FS-` means BitLocker | bytes 3..10 of sector 0 | `unsupported` if not NTFS or BitLocker |
| 2 | `ntfs_mount` read-only succeeds | `libntfs-3g` | `corrupt` on failure |
| 3 | `$Volume` flags. `VOLUME_IS_DIRTY` (0x0001), `VOLUME_CHKDSK_UNDERWAY` (0x4000), the pending-work flags `RESIZE_LOG_FILE`, `UPGRADE_ON_MOUNT`, `DELETE_USN_UNDERWAY`, `REPAIR_OBJECT_ID`, and any bit outside `VOLUME_FLAGS_MASK` (0xc03f) give `dirty_flag`. `VOLUME_MODIFIED_BY_CHKDSK` (0x8000) and `VOLUME_MOUNTED_ON_NT4` (0x0008) alone are informational: listed in `volume_flags`, no state change (decision for N-04: `chkdsk` may leave 0x8000 set and no NTFS-3G or ntfsprogs code clears it, `include/ntfs-3g/layout.h` `VOLUME_FLAGS`; `ntfsprogs/ntfsinfo.c` only prints it; re-checked against real `chkdsk` output in `H-USB-08`, U12) | `vol->flags` | `dirty_flag` |
| 4 | `$LogFile`: `ntfs_check_logfile` + `ntfs_is_logfile_clean`; restart page version 2.0 (major 2 and minor 0) | `logfile.h` API; the 2.0 rule is the one `volume.c` applies [V:S11] | `unclean_log` or `fast_startup` |
| 5 | `/hiberfil.sys` (root directory only; matched ignoring case, see below): absent is safe; size < 4096 or header `hibr`/`HIBR` is hibernated; header `wake`/`WAKE` (a resume in progress) is also treated as `hibernated` | the `hibr`/`HIBR` and size rule of `ntfs_volume_check_hiberfile` [V:S11]; the engine does not recognise `wake`/`WAKE` (decision: the probe is deliberately stricter than the engine, which can only make a volume read-only). The engine looks the name up with `ntfs_inode_lookup_by_name()` on a volume that `ntfs_device_mount` leaves case sensitive (`NVolSetCaseSensitive`; `ntfs-3g` never calls `ntfs_set_ignore_case`), so `HIBERFIL.SYS` is "absent" to it. The probe therefore lists the root with `ntfs_readdir()` and applies the header rule to every entry equal to `hiberfil.sys` under `ntfs_names_are_equal()` with the volume's `$UpCase` [A: U19, T-M3-02; V:S25 `volume.c` `ntfs_hiberfile_open`] | `hibernated` |
| 6 | Anything unexpected (unknown NTFS version, check could not run) | | `unknown` |

`state` is the most severe finding in the order `unsupported > corrupt > hibernated > fast_startup > unclean_log > dirty_flag > unknown > clean`.

### 3.2 Policy table

| Probe `state` | Journal entry for this serial (section 3.5) | `mode=auto` (default) | `mode=ro` request | Notes |
|---|---|---|---|---|
| `clean` | none | `rw` | `ro` | `mode=ro` only narrows: it can turn `rw` into `ro`, never the reverse. |
| `clean` | present, and the external-check evidence rule of section 3.5 holds | `ro`, journal action `remove` | `ro` | The volume was checked on another system after our unclean session. This attach stays read-only; the next `mount` request (or the next attach) is decided again with no entry. The notification says so. |
| `clean` | present, evidence rule does not hold | `ro` | `ro` | Our previous session ended uncleanly. Message: run `chkdsk` on Windows, then re-attach; or, if the user accepts the risk, `ntfsmac journal-clear` (section 3.5). |
| `dirty_flag` | any | `ro` | `ro` | Cleared only by Windows `chkdsk`, or by the explicit `ntfsmac repair --clear-dirty` (ADR-017: typed confirmation, `admin` peer with the ADR-019 checks and administrator prompt, never automatic). With a journal entry present, the message says that this Mac left the volume open and that writes from that session may be incomplete. |
| `unclean_log` | any | `ro` | `ro` | NTFS-3G's `recover` option would reset `$LogFile`; it is banned (ADR-005). |
| `hibernated`, `fast_startup` | any | `ro` | `ro` | `remove_hiberfile` is banned. Message: boot Windows, run `powercfg /h off`, shut down fully. |
| `unknown` | any | `ro` | `ro` | |
| `corrupt`, `unsupported` | any | `refuse` | `refuse` | Volume is left to the system. |

There is no override that turns an unsafe state into a read/write mount. The only path back to `rw` is to make the volume clean.
`ro` means both: the device fd is opened `O_RDONLY` by the daemon and the worker gets `-o ro`.
Removing a journal entry (by the evidence rule, `journal-clear` or `repair`) never changes the mode of an existing mount. It only affects the next decision, which still goes through this table with a fresh probe.
A `rw` decision becomes `ro` if `ntfsmac-volmark set` fails (section 3.6).

The policy library also exposes the two gate functions used by the control requests in section 3.4, so that their rules are unit-tested with the table:

| Gate | Allowed only when |
|---|---|
| `journal-clear` | A journal entry exists; a probe run by the daemon for this request returns `state` exactly `clean`; the volume is not mounted read/write; no live session owns the entry (no volume, on any BSD name, is `Mounting` or `Mounted` with the entry's `session_id`; a second disk with the same serial, such as a clone, therefore cannot clear the entry of a live `rw` session); the peer passed the checks of section 3.4 and the administrator prompt (ADR-019). |
| `repair --clear-dirty` | A probe run for this request returns `state` exactly `dirty_flag`, `logfile` `clean`, `hiberfile` `absent` or `inactive`, `volume_flags` containing `DIRTY` and otherwise only the informational `MODIFIED_BY_CHKDSK` or `MOUNTED_ON_NT4` (section 3.1 check 3); the volume is not mounted (the CLI asks the user to unmount the read-only mount first); the peer passed the checks of section 3.4 and the administrator prompt (ADR-019). |

A volume with only the informational flags set probes `clean`, so a `chkdsk` that clears `VOLUME_IS_DIRTY` but leaves `VOLUME_MODIFIED_BY_CHKDSK` set does not trap the volume in `ro` (audit M1-recheck N-04). Any other leftover flag gives `dirty_flag`, which `repair` refuses; the remedy is a further Windows `chkdsk`. `H-USB-08` records the flags `chkdsk /f` really leaves; if it shows a flag this rule does not cover, the coordinator revisits check 3 before the M5 gate.

### 3.3 Worker invocation

```
ntfsmac-worker /dev/fd/<N> /Volumes/<leaf> -o <options>
```

The daemon starts the worker with `posix_spawn` and a fixed five-element argument vector (no shell, no `-o` other than the one shown). `<leaf>` is the mount-point name produced by the label sanitiser (ADR-016); it is a separate argument, never part of the option string.

| Option set | Options |
|---|---|
| Always | `backend=fskit,local,norecover,noatime,windows_names,big_writes,streams_interface=xattr,uid=<console uid>,gid=<console gid>,fmask=0133,dmask=0022` |
| Volume name | `volname=<leaf>`, only if T-M2-04 shows the FSKit backend needs it to name the volume (U14). The value is the same sanitised leaf as the mount point, never the raw label. If the backend names the volume from the mount point, `volname` is not passed at all. |
| Read-only | add `ro` |
| Opt-in (config) | `hide_hid_files`, `permissions` or `usermapping=/usr/local/etc/ntfs-macos/UserMapping` (fixed path without a space, root-owned, mode 0644; ADR-012) |
| Banned (daemon rejects) | `recover`, `remove_hiberfile`, `dev`, `suid`, `allow_root`, `efs_raw`, `fsname`, any option not in the allow-list |

Option semantics are from the NTFS-3G man page [V:S11]. `recover` is NTFS-3G's default, so `norecover` is mandatory on every invocation.
`fsname` is removed from the earlier draft: `ntfs-3g` rejects it as an unsupported option and builds `fsname=` itself from the device argument, escaping `,` and `\` [V:S25, `src/ntfs-3g_common.c` option table and `parse_mount_options`].
Whether `volname`, `big_writes` and `noatime` are accepted by the FSKit backend is [A]; macFUSE states that most kernel-handled options are not yet implemented there [V:S2]. T-M2-04 records the accepted set and this table is then corrected.

How the option string is parsed, which is why disk-derived text must never reach it unchecked [V:S25]: `ntfs-3g` splits `-o` on every `,` with `strsep` (no escape character), splits each token at the first `=`, consumes the tokens it knows, and passes every unknown token through to libfuse, which splits again on `,` and treats `\` as an escape. A label `X,remove_hiberfile` would therefore become a separate engine option, and `X,allow_other` a separate FUSE option.

**Final argv validator.** After the option builder runs and before `posix_spawn`, `argv_check()` in `src/sanitize/` re-parses the complete vector the same way the engine does and refuses the spawn unless all of the following hold. On refusal the volume goes to state `Failed` with reason `internal: worker arguments rejected`; nothing is "repaired" and no mount happens.

| # | Rule |
|---|---|
| 1 | Exactly five elements: the worker path from the install manifest, `/dev/fd/<N>` with `N` decimal 3..255, `/Volumes/<leaf>`, `-o`, the option string. |
| 2 | `<leaf>` satisfies every label rule of ADR-016 and is the name the daemon created in step 7 of section 4.1. |
| 3 | The option string is at most 1024 bytes and contains no `\`, no NUL and no byte below `0x20` or equal to `0x7F`. Outside the `volname` value every byte is in `0x21..0x7E`; the `volname` value obeys rule 2 (it may contain spaces and non-ASCII UTF-8, never `,` `=` `\` `/` `:`). |
| 4 | Splitting on `,` gives no empty token; each token splits at the first `=` into a key from the allow-list and, for keys that take one, a value with no `=`. No key appears twice. |
| 5 | Values match per-key grammars: `uid`/`gid` decimal equal to the console user; `fmask=0133`, `dmask=0022` exactly; `streams_interface` is `xattr` or `none`; `backend=fskit`; `volname` equals `<leaf>` byte for byte; `usermapping` is exactly `/usr/local/etc/ntfs-macos/UserMapping`. The path has no space, so rule 3 is not relaxed; the engine uses an absolute value as the file name as it is (`libntfs-3g/security.c` `ntfs_build_mapping`, set from `src/ntfs-3g_common.c` `parse_mount_options`) [V:S25]. |
| 6 | `norecover` is present; `ro` is present if and only if the decision is `ro`; no banned key is present. |
| 7 | The descriptor's access mode (`fcntl(F_GETFL)`) is `O_RDONLY` when the decision is `ro`, and `O_RDWR` only when it is `rw`. |

The option builder is the only producer of the vector; `argv_check()` is the independent re-check. Both are unit-tested against each other: every builder output for every policy row validates, and every single-token mutation of it is rejected (TEST_STRATEGY.md section 4, `U-ARGV-*`).
`ntfs-3g` itself then appends `allow_other,nonempty,` and `fsname=` to what it passes to libfuse; those come from the engine, not from the disk, and are accepted.

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
| `journal_clear` | `bsd`, `confirm_label` (the sanitised label as shown by `list`), `confirm_serial` (last 8 hex digits of the serial) | `cleared` or `refused` with reason |
| `repair` | `bsd`, `action` (only `clear_dirty`), `confirm_label`, `confirm_serial` | `repaired` or `refused` with reason |

`journal_clear` and `repair` are accepted only when all of the following hold (ADR-019). The typed label and serial are a guard against mistakes, not a security boundary: `list` and `status` publish both to every peer.

| # | Check | Done by the daemon with |
|---|---|---|
| 1 | Peer uid is a member of `admin`. | `LOCAL_PEERCRED` |
| 2 | Peer is not a process this daemon spawned (probe, volmark, worker) or one of their descendants. Every spawned process gets its own process group (`POSIX_SPAWN_SETPGROUP`); a peer whose process group, or whose parent chain, reaches a live spawned process is refused. | `LOCAL_PEERPID`, `proc_pidinfo` [A: T-M4-04] |
| 3 | Peer executable is the installed `ntfsmac` CLI: its code signature satisfies the requirement recorded at install time (Developer ID team for release builds; the exact cdhash from the install manifest for ad-hoc development builds), and it runs with the hardened runtime, so another process of the same user cannot inject code into it. | `LOCAL_PEERTOKEN` audit token, `SecCodeCreateWithAuditToken`, `SecCodeCheckValidity` [A: T-M4-04] |
| 4 | An administrator authenticated for this one request. The CLI obtains the right `org.ntfs-macos.journal-modify` (rule `authenticate-admin-nonshared`, timeout 0, installed by the package) through Authorization Services, which shows the system password prompt; it sends the external form with the request, and the daemon re-checks the right itself with `AuthorizationCopyRights` before acting. | Authorization Services [A: U17, T-M4-04] |
| 5 | Volume is attached and passes the gate in section 3.2; both confirmation fields match. | policy gates |

The worker sandbox profile also denies `connect()` to `/var/run/ntfsmacd.sock` explicitly (T-M4-06); this is defence in depth, because checks 2 to 4 hold without the sandbox. A `rw` mount of a volume never needs check 4: a volume reaches `rw` only through a fresh `clean` probe with no journal entry, and the only ways to remove an entry without a clean session (paths 3 and 4 in section 3.5) pass check 4.

The CLI collects the typed confirmation after printing what the action does and does not do (ADR-017), then triggers the password prompt. Labels shown by `list`, by the CLI and in notifications are always the sanitised form (ADR-016); reasons are shown as text the CLI or daemon maps from `reason_codes`.

### 3.5 Session journal

Directory `/var/db/ntfs-macos/journal/`, one file per volume serial, mode 0600 root. Files are written to a temporary name, `F_FULLFSYNC`ed, renamed, and the directory is `fsync`ed.

File names are built only by the daemon, from the serial parsed as a 64-bit integer (section 3.1 grammar): entry `<16 upper-case hex digits>`, temporary `.<16 hex>.tmp`, tombstone `cleared/<16 hex>-<YYYYMMDDTHHMMSSZ>`. No byte of the probe's `serial` string is used. All opens and renames are relative to a descriptor of the journal directory opened with `O_DIRECTORY | O_NOFOLLOW` (`openat`, `renameat`, `O_NOFOLLOW | O_EXCL` for the temporary file). `U-JRN-*` and `U-VERDICT-05` test hostile serials.

Entry fields (nothing else is stored): `serial`; sanitised `label`; `total_sectors`; `session_id`; `created` and `updated` timestamps; `logfile_lsn_at_start` (from the probe run that allowed `rw`); `marker` = `pending` | `set` | `failed` | `none` (section 3.6); `state` = `open` | `write_error`.

| When | Journal change |
|---|---|
| Before the device is opened `O_RDWR` | Entry created: `state=open`, `marker=pending` (or `none` while the marker step is disabled, see ADR-006). |
| `volmark set` succeeded | `marker=set`. Only then is the worker started. |
| `volmark set` failed | `marker=failed`; the session continues as `ro` (section 3.6). |
| The worker reported a device write or sync error, or the session channel broke its grammar or bounds (section 7) | `state=write_error`. |
| Clean end of session (section 4.5) | Entry removed. |

**How an entry is removed.** Exactly four ways, none of them by time or by count:

| # | Path | Conditions | Effect on this attach |
|---|---|---|---|
| 1 | Clean end of our own session (section 4.5 step 3) | The positive clean-session proof of section 7 (records `S rw` and `U 0` in order, no `E`, channel within bounds and at EOF, session not re-adopted, unmount seen before the worker exit, worker exit 0); daemon `F_FULLFSYNC` succeeded; `volmark clear` exit 0 (or `marker=none`). Entry `state` is `open`. | Normal unmount. |
| 2 | External-check evidence (automatic) | All of: entry `marker=set`; fresh probe `state` exactly `clean` (so `VOLUME_IS_DIRTY`, which we set, is now clear); probe `logfile_lsn` present and different from `logfile_lsn_at_start`; same `serial` and `total_sectors`. Disabled by default until U12 and U13 are resolved (below). | Stays `ro`; the clear applies to the next decision. Notification: "checked on another system; re-attach or `ntfsmac mount` for read/write". |
| 3 | `ntfsmac journal-clear` (explicit) | Gate in section 3.2 (fresh probe exactly `clean`), peer checks and administrator prompt of section 3.4 (ADR-019), label and serial typed. Also the live-session condition of section 3.2. Allowed for any `marker` and `state`, with a stronger warning when `state=write_error`, and the same stronger warning when `marker=set`, the fresh probe is `clean` and its `logfile_lsn` equals `logfile_lsn_at_start` (nothing on Windows touched the log, so the cleared flag may be a lost flush and no tool has checked the volume): "the dirty marker is gone but nothing shows that this volume was checked". | Unchanged; the next `mount` decides again. |
| 4 | `ntfsmac repair --clear-dirty` succeeded | Gate in section 3.2 (exactly `dirty_flag`), peer checks and administrator prompt of section 3.4 (ADR-019), label and serial typed (ADR-017). | Unchanged; the next `mount` decides again. |

Removal renames the entry into `/var/db/ntfs-macos/journal/cleared/<serial>-<UTC time>` (kept for `doctor`, at most 32 kept) instead of deleting it, and logs which path removed it.

Why rule 2 needs the log sequence number and not only the cleared flag: a drive that ignores cache flushes can lose our `VOLUME_IS_DIRTY` write, after which the flag is clear although nothing checked the volume. Windows writes `$LogFile` whenever it mounts a volume read/write, including `chkdsk`, so a changed `current_lsn` together with the cleared flag is evidence that Windows mounted the volume and cleared the flag (chkdsk or autochk clears it; plain Windows mounts do not) [A: U12]. Rule 2 is safe only if the negative half holds: a plain Windows read/write mount that replays or advances the log but does not run `chkdsk` must leave our `VOLUME_IS_DIRTY` set, because NTFS-3G's writes are not in the Windows log and only `chkdsk` checks them. `H-USB-09` tests exactly that; rule 2 stays disabled unless both `H-USB-08` and `H-USB-09` pass. NTFS-3G with `norecover` does not write `$LogFile` [A: U13; `ntfs_logfile_reset` is called only with `recover`, V:S25]. Until T-M3-01 (U13) and the Windows step of T-M5-10 (U12) confirm both, rule 2 is compiled in but disabled by configuration, and the only ways out are rules 1, 3 and 4. Rule 2 can never produce `rw` on the attach where it fires.

What the evidence rule cannot do: the probe is itself a parser of untrusted data, so a crafted volume can fake a clean flag and a new LSN. That is not a new exposure, because the same volume can equally fake `clean` without any journal entry; the journal protects against our own unclean sessions, not against a hostile disk (section 9, D2).

Key collisions: the key is the NTFS serial, which clones and copies of one image share. A clone of a volume with an open entry is therefore also mounted `ro` until cleared. This errs towards read-only and is accepted.

The journal is per host. Another machine learns about an unclean session only through the on-disk dirty marker (section 3.6, ADR-006).

### 3.6 On-disk dirty marker (`ntfsmac-volmark`)

NTFS-3G 2026.9.28 never sets or clears `VOLUME_IS_DIRTY`: neither `libntfs-3g` nor `src/ntfs-3g.c` writes it; only the ntfsprogs tools `ntfsfix`, `ntfsresize`, `ntfsmove` and `mkntfs` change it, through `ntfs_volume_write_flags()` [V:S25]. Without this step nothing on the disk records an unclean session (U9).

Command: `ntfsmac-volmark set|clear|clear-repair --fd 3 --expect-serial <16 hex digits> --expect-sectors <decimal>`. The two expected values are formatted by the daemon from the parsed probe verdict (section 3.1), never copied from probe text. Runs as `nobody` under `integration/sandbox/volmark.sb` (no network, no exec, no file system access, no Mach services; only the inherited descriptor 3, which is the daemon's `O_RDWR` device descriptor, the same one the worker later receives). It accesses the device through the same descriptor-backed device layer as the probe, never by path. Exit 0 means every step below succeeded; any other status or a timeout is a failure.

| Step | `set` (before the worker) | `clear` (after a clean session) | `clear-repair` (ADR-017) |
|---|---|---|---|
| 1 | `ntfs_device_mount` read/write with `NTFS_MNT_FORENSIC` only (no `RDONLY`, `RECOVER`, `MAY_RDONLY` or `IGNORE_HIBERFILE`). In `libntfs-3g/volume.c` `ntfs_device_mount` the hibernation, `$LogFile` and `fix_txf_data()` block runs only when neither `RDONLY` nor `FORENSIC` is set, so this mount performs no write at all; in particular the `$TXF_DATA` fix-up (truncate and rewrite of the root directory's `$TXF_DATA`) is left to the worker, which runs it after the flag is set [V:S25]. Volmark then runs the same refusals itself through the public API: `ntfs_volume_check_hiberfile(vol, 1)` (`include/ntfs-3g/volume.h`) and `ntfs_check_logfile()` plus `ntfs_is_logfile_clean()` (`include/ntfs-3g/logfile.h`) with the restart-page 2.0 test that the static `ntfs_volume_check_logfile()` in `volume.c` applies. Any refusal: exit 10, nothing written. | same | same |
| 2 | Binding (ADR-020): read sector 0 through the device layer; `volume_serial_number` and `number_of_sectors` (`NTFS_BOOT_SECTOR`, `layout.h`) must equal the expected values, else exit 15, nothing written. | same | same |
| 3 | Read `vol->flags` (raw on-disk value, `volume.c` line 1181). If `VOLUME_IS_DIRTY` is already set, or any bit outside `VOLUME_FLAGS_MASK` (0xc03f) is set (`ntfs_volume_write_flags` would drop it): stop, exit 11. | If `VOLUME_IS_DIRTY` is not set: stop, exit 13 (anomaly: someone changed the volume during our session). | If `VOLUME_IS_DIRTY` is not set, or any flag other than it and the informational `MODIFIED_BY_CHKDSK` and `MOUNTED_ON_NT4` is set: stop, exit 13. |
| 4 | Print `w` on stdout, then `ntfs_volume_write_flags(vol, flags \| VOLUME_IS_DIRTY)`; all other bits unchanged; print `d` when it returns 0. | same with `flags & ~VOLUME_IS_DIRTY` | same as `clear` |
| 5 | `ntfs_umount`; then `fcntl(3, F_FULLFSYNC)`. If the device rejects `F_FULLFSYNC`, `fsync` and a warning on stderr (R-30); any other error: exit 12. | same | same |
| 6 | Mount again with `NTFS_MNT_RDONLY`, read the flag back, compare, unmount. Mismatch: exit 14. | same | same |

**What step 4 writes, and in which order** [V:S25]. `ntfs_volume_write_flags()` (`volume.c`) changes the flag in the `$VOLUME_INFORMATION` attribute and calls `ntfs_inode_sync()` on `$Volume` (record 3). That ends in `ntfs_mft_records_write()` (`libntfs-3g/mft.c`), which writes the record to `$MFT` first (W1) and then, because record 3 is inside the mirrored range, the same record to `$MFTMirr` (W2). Each is one multi-sector-protected write of one MFT record (normally 1 KiB). Nothing else is written. Every later mount, read-only or not, compares records 0 to 3 of `$MFT` and `$MFTMirr` byte for byte and fails on a torn record ("Incomplete multi sector transfer") or a mismatch ("$MFTMirr does not match $MFT") (`volume.c` `ntfs_device_mount`, the loop after "Comparing $MFTMirr to $MFT"). No write order avoids this: whichever copy is written first, a cut between the two leaves them different.

Crash outcome of each point (same for `set`, `clear` and `clear-repair`, with "flag" meaning the old or the new value):

| Cut point | On disk | Journal | Next attach |
|---|---|---|---|
| Before step 4 | Unchanged (step 1 writes nothing) | `pending` (set) or `set` (clear) | Probe gives the old state; the entry makes it `ro`. Correct. |
| Inside W1 (torn sectors of the `$MFT` copy) | `$MFT` record 3 fails its fix-up check | as above | Probe `corrupt`, policy `refuse` (residual, below) |
| Between W1 and W2 | Copies differ | as above | Probe `corrupt`, policy `refuse` (residual) |
| Inside W2 | `$MFTMirr` record 3 fails its fix-up check | as above | Probe `corrupt`, policy `refuse` (residual) |
| After W2, before the device flush returns | Any of the rows above or the new value, depending on the drive cache | as above | As the matching row |
| After step 5 | New value on both copies | `pending`, then `set` (set); entry removed after exit 0 (clear) | `set`: `dirty_flag`, `ro`. `clear`: `clean`, `rw` once the entry is gone. |

**Residual risk (R-33).** A cut inside W1 to W2 makes the volume unmountable by NTFS-3G, read-only included, until Windows `chkdsk` repairs `$MFTMirr` [A: U15]. The window is two 1 KiB writes and one flush, once at the start and once at the end of each read/write session; it cannot be closed without changing NTFS-3G's mirror check, which is out of scope (ADR-002). The data region is never touched. When the probe says `corrupt` and the journal holds an entry with `marker=pending`, or `marker=set` with `state=open` after a `clear` attempt, the notification says "this Mac was interrupted while marking the volume; data was not changed; run `chkdsk` on Windows" instead of the generic text. Because the default policy is `rw`, every attach of a clean volume pays this exposure even if the user only reads; the configuration option `automount_mode=ro` (T-M4-14) automounts read-only and skips volmark, and the user then unmounts and runs `ntfsmac mount` for read/write. `U-VMK-07` and the fixture `txf-nonresident` test the table above.

**Timeout without a torn write.** The daemon gives volmark 20 s until it prints `w`; until then a `SIGKILL` cannot cut a write. After `w` the daemon does not kill it until it prints `d` or exits, up to a further 120 s (a dead device makes the write fail quickly; this limit covers a hung device only). Only after that does it send `SIGKILL`, record `marker=failed` and use the residual text above.

Ordering in the daemon:

1. `set`: journal entry `marker=pending` and synced; device opened `O_RDWR` and bound to the probed volume (section 4.1 step 6, ADR-020); probe descriptor then closed; `volmark set`; on exit 0 journal `marker=set` and synced; then the worker is spawned with the same descriptor.
2. If `set` fails for any reason (including exit 11 or 15: the engine, the flag or the boot sector disagrees with the probe), the daemon closes the writable descriptor, records `marker=failed`, opens `O_RDONLY` (and binds it again), and mounts `ro` with reason "could not mark the volume; mounted read-only". The volume is never written by the worker in that attach. If the failure came after `w`, a partly written `$Volume` record is possible (table above); the journal entry stays so the next attach is also `ro` or, if the copies differ, `refuse` with the residual text.
3. `clear`: only on the clean path of section 4.5 (positive clean-session proof of section 7, daemon `F_FULLFSYNC` succeeded, journal `state=open`). On exit 0 the journal entry is removed. On failure the entry stays with `marker=set`, so the flag stays on disk and the next attach is `ro`; the notification says to run `chkdsk`.
4. Crash windows outside step 4: after `set` and before the worker: flag set, entry `pending` or `set`, next attach `ro` (correct). Between `clear` and journal removal: flag clear, entry present, LSN unchanged, next attach `ro` until `journal-clear` (conservative). There is no window in which the flag is clear, the entry is gone and the session was unclean; inside step 4 the worst outcome is `refuse`, never `rw`.

Interaction with other systems, all intended:

| System | Sees `VOLUME_IS_DIRTY` set after an unclean session on a Mac |
|---|---|
| Windows | Runs `autochk` at boot for a fixed disk, or offers "Scan and fix" for a removable disk; `chkdsk` clears the flag. The flag is also set during every read/write session, so a disk moved to Windows without a clean eject is always checked. |
| Linux `ntfs3` | Refuses a read/write mount without `force` [A]. |
| NTFS-3G elsewhere | Does not check the flag at mount [V:S25]; ntfsprogs tools warn. |
| This product on any Mac | Probe `dirty_flag`, mount `ro`. |

Until T-M3-12 passes, the marker step is disabled (`marker=none` in the journal) and guarantee D3 is host-only (section 9).

## 4. Data flows

### 4.1 Disk appears and is mounted

1. `diskarbitrationd` reports a new disk. `ntfsmacd` filters on `DAMediaContent` (`Windows_NTFS`, GPT basic-data GUID `EBD0A0A2-...`, or whole-disk media) and `DAMediaLeaf`.
2. Conflict check: if another third-party NTFS bundle is installed (for example `/Library/Filesystems/ufsd_NTFS.fs`), the daemon is in **passive mode** and stops here (section 8.4).
3. In active mode the daemon answers the Disk Arbitration mount-approval callback with a dissenter for this disk, so Apple's read-only NTFS driver does not auto-mount it first [A: T-M4-02 verifies the dissenter is honoured on macOS 27].
4. The daemon opens the device `O_RDONLY` (descriptor A), reads the first 4096 bytes through A and keeps their SHA-256 (H0; a byte compare, not an NTFS parse), spawns `ntfsmac-probe` with A, and waits at most 20 s. The probe verdict carries `serial` and `total_sectors` (section 3.1).
5. Policy (section 3.2) gives `rw`, `ro` or `refuse`. On `refuse` the dissent is withdrawn and the system handles the disk.
6. Binding (ADR-020). Before any write descriptor exists, the daemon checks that no file system has the disk mounted: the Disk Arbitration description of the disk has no `DAVolumePath`, and the disk is still the object the probe ran on (same BSD name and `DAMediaPath`) [A: U18, T-M4-02]. Otherwise the volume goes to `ro` (if only Apple's read-only driver has it) or `Failed`, and no `O_RDWR` open happens.
   For `ro`: the worker gets descriptor A itself, the one the probe read; after the probe exits, the daemon resets A's status flags with `F_SETFL` and re-reads the first 4096 bytes through A; a hash other than H0 gives `Failed`.
   For `rw`: write the journal entry; open the device `O_RDWR` (descriptor B) while A is still open; require `fstat(A).st_rdev == fstat(B).st_rdev` and the first 4096 bytes read through B to hash to H0; only then close A. Run `ntfsmac-volmark set` (section 3.6) with `--expect-serial` and `--expect-sectors` taken from the parsed verdict; volmark compares them with the boot sector it reads through B. After volmark exits 0, the daemon re-reads the first 4096 bytes through B and requires H0 again (volmark changes only MFT record 3, never sector 0). Any mismatch, or a volmark failure, closes B and continues as `ro` with a freshly bound read-only descriptor. The worker gets B.
   What the binding covers: a disk swapped or re-enumerated between probe and mount, a BSD name reused by another device, and another driver mounting the disk in between. What it does not cover: a device whose firmware answers differently to the same reads (a hostile USB device can do that at any time, including during the session); that is the same exposure as D2 (section 9).
7. Create the mount point. The label from the probe goes through the sanitiser (ADR-016) to give `<leaf>`. The daemon opens `/Volumes` with `O_DIRECTORY | O_NOFOLLOW`, checks it is a root-owned directory, and calls `mkdirat(dirfd, leaf, 0700)`. On `EEXIST` it tries `<leaf> 2` to `<leaf> 99` in order; it never mounts on, adopts or changes a directory it did not create. If all are taken, the volume goes to `Failed` and is left unmounted. It then opens the created directory with `openat(dirfd, leaf, O_RDONLY | O_DIRECTORY | O_NOFOLLOW)` and works only on that descriptor: `fstat` (a directory, root-owned, mode 0700) records its device and inode, and `fchown` hands it to the console user. No step names the path again before the mount; removing an unused directory is `unlinkat` after an `fstat` of the name equals the record.
8. Build the worker vector (section 3.3) and run `argv_check()`. Spawn the worker as the console user inside the sandbox profile, in its own process group, with `POSIX_SPAWN_CLOEXEC_DEFAULT`, so that only descriptors 0 to 2, the device descriptor and the write end of the session channel (section 7, environment `NTFSMAC_ERRFD`) are inherited. Wait for the mount to appear by polling `getfsstat(MNT_NOWAIT)` for an entry whose `f_mntonname` is the mount point (the call returns the kernel's cached table and does not call the file system) or for the worker to exit; the wait has its own deadline of 30 s, shorter than `daemon_timeout`. After the mount, root never calls `stat`, `statfs`, `open` or `readdir` on a path inside the worker-served file system, because a compromised worker can make such a call block uninterruptibly and stall the volume's queue (section 6) [A: U20, T-M4-01].
9. Record state `Mounted(rw|ro)`. Post a user notification that states the mode and, for `ro`, the reason.

### 4.2 Read

`read(2)` -> VFS -> FSKit (unified buffer cache; a cached page returns without reaching the module) -> macFUSE FSModule -> `FUSE_READ` -> `ntfs-3g` `ntfs_fuse_read` -> `libntfs-3g` runlist lookup -> `pread` on the device fd.
macFUSE 5.5.0 rejects an empty `FUSE_READ` reply with `EIO` [V:S1]; a module crash on empty replies, fixed in 5.4.0, is described in [V:S7]. Neither source says anything about zero-filling short reads, so no such behaviour is assumed. NTFS-3G must never answer a non-EOF read with an empty reply; T-M3-07 tests reads at and across EOF, including short reads, and records what the backend returns.

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

Worker side [V:S25]: the device sync operation `ntfs_device_unix_io_sync()` and the close path `ntfs_device_unix_io_close()` (when the device is dirty) both call `ntfs_fsync()` in `libntfs-3g/unix_io.c`. On `__APPLE__` that function already issues `fcntl(fd, F_FULLFSYNC)` and, if that fails for any reason, silently falls back to plain `fsync(fd)`, returning the fallback's result. No patch is needed for `F_FULLFSYNC` (the earlier patch 0002 is withdrawn). The problem is the silent fallback: on a drive or bridge that rejects `F_FULLFSYNC`, the sync point returns success although the drive cache was not flushed. Patch 0006 therefore reports every fallback as a `W fullfsync <errno>` record (section 7); it does not change the return value. A session with a `W` record can still end clean, but its report never says the drive cache was flushed (D1).

### 4.5 Unmount and eject

1. Trigger: Finder eject, `diskutil unmount`, `umount`, or `ntfsmac unmount`. macFUSE 5.5.0 supports Finder unmount for `local` volumes [V:S1].
2. FSKit calls `synchronize`, then `unmount`, on the FSModule; the worker receives `FUSE_DESTROY`, whose handler `ntfs_fuse_destroy2()` calls `ntfs_close()` and so `ntfs_umount()`, which syncs and closes the device (`src/ntfs-3g.c`, `libntfs-3g/volume.c` `__ntfs_volume_release`) [V:S25]. With patch 0006 the worker then writes `U <n>` (section 7). It exits 0 in every case after `fuse_loop()` returns, whether or not the unmount succeeded (`main()` sets `err = 0` after `fuse_loop`; `ntfs_close()` only logs a failed `ntfs_umount`), so the exit status is not evidence of a clean end.
3. The daemon sees the exit. For a `rw` session it treats the session as clean only if the positive clean-session proof of section 7 holds (records `S rw` first and `U 0` last, no `E`, channel within bounds and at EOF, unmount seen before the exit, session not re-adopted, worker exit 0 as a necessary condition only) and its own `F_FULLFSYNC` on its copy of the fd succeeded. Then it runs `ntfsmac-volmark clear` (when the marker step is enabled) and, on exit 0, removes the journal entry (section 3.5 rule 1). It closes the fd and removes the mount point directory only if its device and inode match the ones recorded in section 4.1 step 7 and it is empty (`unlinkat(..., AT_REMOVEDIR)`).
   The user is told "ejected, all data written" only when every step of the clean path succeeded and no `W` record arrived; with a `W` record the text is "ejected, all data written; this drive did not confirm that its cache was flushed". Otherwise the notification says the session did not end cleanly and the volume will be read-only next time.
4. For "eject" of the physical disk the daemon then calls `DADiskEject` on the whole disk. Whether Finder's eject button on a macFUSE `local` volume also ejects the underlying media without daemon help is [U]; T-M4-03 resolves it.

Anything short of the positive proof (a missing `S` or `U` record, an `E` record, a broken channel, a non-zero exit, an exit without a preceding unmount, a re-adopted session) leaves the journal entry and the dirty marker in place. Silence is never success.

## 5. Threading and locking model

| Layer | Model | Consequence |
|---|---|---|
| FSKit to FSModule | Requests arrive over NSXPC on a serial queue unless the module redispatches [V:S15]. macFUSE internals are closed; 5.5.0 notes "fast paths for uncontended locks" [V:S1]. | Out of our control. |
| Worker (`ntfs-3g`) | Single-threaded `fuse_loop`, not `fuse_loop_mt` [V:S11]. `libntfs-3g` is not thread-safe [A: consistent with the single-threaded loop]. | All file system operations are strictly serialized. No locking is added. The multi-threaded loop must not be enabled. |
| Daemon | One main dispatch queue for Disk Arbitration and socket events. One serial queue per volume, keyed by BSD name, running the state machine `Appeared -> Probing -> Deciding -> Mounting -> Mounted -> Unmounting -> Gone`, plus `Failed`. | No shared mutable state between volumes except the journal directory (one file per volume, atomic rename). |
| Probe | Single thread, hard timeout enforced by the daemon (SIGKILL at 20 s). | |
| Device exclusivity | The daemon holds the only open descriptors. For `ro` the worker reuses the probe's descriptor; for `rw` the writable descriptor is opened while the probe's is still open (for the binding check of section 4.1 step 6) and the probe's is closed before volmark starts. Every daemon descriptor is `O_CLOEXEC`, and every spawn uses `POSIX_SPAWN_CLOEXEC_DEFAULT`, so no process inherits another volume's descriptors. A second `mount` request for a volume not in state `Appeared` or `Failed` is rejected. | Two writers on one volume cannot happen through this product. Another driver (Paragon) is excluded by passive mode. |

Advisory locks: the FSKit backend does not forward `fcntl`/`flock` locks to the file system; they are local to the Mac [V:S15][V:S1, remote locking is kernel-backend only]. This is acceptable for a locally attached disk.

## 6. Security model

Threat: a malicious or corrupt NTFS volume (the user plugs in an untrusted USB drive). NTFS-3G had two releases labelled Security Release in 2026 (2026.7.7, 2026.9.18) and two further releases with a CVE or memory-safety fixes (2026.2.25, 2026.9.28), fixing heap overflows, use-after-free and out-of-bounds bugs triggered by crafted volumes [V:S10]. The metadata parser must be assumed exploitable.

| Control | Detail |
|---|---|
| Privilege separation | Only `ntfsmacd` is root. It opens devices and creates `/Volumes` entries. It contains no NTFS parsing. |
| Unprivileged parser | Probe and volmark run as `nobody`, never at the same time; worker runs as the console user. None can open another device. Volmark is the only one of the three that parses NTFS while holding a writable descriptor before the worker; it is short-lived and gets that descriptor only for `rw` decisions. |
| Disk-derived strings | The volume label is attacker-controlled. It is used only after the sanitiser of ADR-016 (allow-listed characters, length cap, path-component rules) and only as the mount-point leaf and, if needed, `volname`. The probe verdict is parsed by a strict, fuzzed parser with a grammar for every field (section 3.1); `serial` and `logfile_lsn` are used only as parsed integers (journal file names are re-formatted from them), and reasons travel as enum codes that the daemon and CLI turn into their own text. |
| Sandbox | Both run under a `sandbox_init` profile (`integration/sandbox/*.sb`): no network, no process exec, file access limited to the inherited fd, the mount point, for the worker a read of `/usr/local/etc/ntfs-macos/UserMapping` only when the opt-in `usermapping=` option is set (ADR-012), and for the worker the Mach service `io.macfuse.mount` (required for sandboxed FUSE servers [V:S6]). `sandbox_init` is deprecated but functional [A: T-M4-06 verifies on macOS 27; if it fails, drop to uid separation only and record the residual risk]. |
| Least access | Read-only decisions get an `O_RDONLY` descriptor, so once the probe has classified a volume as unsafe, a later bug in the worker or volmark cannot write to it. This does not protect against a probe that is fooled into reporting `clean` (section 9, D2). |
| Option allow-list | The daemon builds the worker command line itself, and `argv_check()` re-parses it against the allow-list before every spawn (section 3.3). Client-supplied strings are limited to `bsd`, `mode` and the confirmation fields, which are compared, never used to build a command. Disk-supplied strings are limited to the sanitised label. |
| Client authentication | `LOCAL_PEERCRED` on the control socket; console user or `admin` for `mount` and `unmount`. `journal_clear` and `repair` additionally need a peer that is not a spawned process or its descendant, the signed `ntfsmac` CLI, and a fresh administrator password prompt validated by the daemon (section 3.4, ADR-019). |
| Mount flags | FSKit mounts are `nodev,nosuid` by construction [A]. `uid`/`gid`/`fmask`/`dmask` map every file to the console user; the execute bit is cleared on files (`fmask=0133`, limitation N9). |
| Supply chain | Dependencies pinned by version and SHA-256 in `third_party/deps.lock`; the macFUSE installer package signature is checked before use (IMPLEMENTATION_PLAN.md section 3). |
| No system weakening | No kext, no SIP change, no Reduced Security, no replaced Apple binary. The installer writes only to `/Library/LaunchDaemons`, `/usr/local` (including `/usr/local/etc/ntfs-macos`), `/Library/Application Support/ntfs-macos`, `/var/db/ntfs-macos`. |
| Hardening | Release build: hardened runtime, `-fstack-protector-strong`, `-D_FORTIFY_SOURCE=2`, `-ftrivial-auto-var-init=zero`. Local CI builds with ASan and UBSan (TEST_STRATEGY.md section 9). |

Residual risk: a worker compromise gives the attacker the console user's identity inside the sandbox plus write access to that one volume.

## 7. Failure behaviour

| Event | Behaviour | Guarantee after the event |
|---|---|---|
| Device removed without eject | Device I/O returns `ENXIO`/`EIO`; the worker returns errors to FUSE; the daemon receives the Disk Arbitration disappearance, force-unmounts the mount point, kills the worker after 5 s. Journal entry and dirty marker remain. | Data not yet written by the worker is lost. Metadata may be mid-update. Next mount on this Mac is `ro` (journal). Once the on-disk marker is enabled (T-M3-12 passed, section 3.6), `VOLUME_IS_DIRTY` stays set, so Windows runs or offers `chkdsk` and any Mac with this product mounts `ro`. Before that, only this Mac knows (D3). |
| Worker crash or `SIGKILL` | macFUSE disconnects the volume (`daemon_timeout`, default 60 s [V:S4], supported on the FSKit backend since 5.5.0 [V:S1]); the daemon detects the exit immediately and force-unmounts. Journal and dirty marker remain. | Same as above. |
| `ntfsmacd` crash | launchd restarts it. On start it reconciles: for each journal entry, if a live worker and mount exist it re-adopts them [A: T-M4-05; if re-adoption is impractical, the daemon force-unmounts and the volume becomes `ro` on next mount]. The read end of the session channel died with the old daemon, so a re-adopted session can never be proven clean: it ends with the journal entry and marker in place. | No data path goes through the daemon, so open files are unaffected. Next mount `ro`. |
| macFUSE FSModule crash | Volume disappears or returns `EIO` [V:S7 describes this failure mode]. Worker exits non-zero. | Same as worker crash. |
| I/O error from the device | `libntfs-3g` returns the error; the application gets `EIO` on `read`/`write`. Errors on `close` are lost to the application (FSKit limit). The daemon learns of device errors through the session channel below, independently of FSKit: on the first `E` record it sets journal `state=write_error`, posts a notification, and requests unmount; the session can no longer end cleanly, so the marker stays set and the next mount is `ro`. | Earlier acknowledged writes are not rolled back. |
| `ntfsmac-volmark set` fails or times out | Writable descriptor closed; volume mounted `ro`; journal `marker=failed` (section 3.6). | No worker write happened. A failure after volmark printed `w` can leave the two `$Volume` copies different; the next attach is then `refuse` (probe `corrupt`) with the residual text of section 3.6, otherwise `ro`. |
| Host panic or power loss | No clean unmount. Journal and dirty marker remain. | Same as device removal. |
| Volume full | `ENOSPC` from `libntfs-3g` on `write`. | No corruption expected; tested in T-M5-06. |
| Probe timeout or crash | State `unknown`, action `ro`. | No write happened. |
| macFUSE missing, too old, or extension not approved | Daemon reports it in `doctor` and in the notification; the disk is left to the system (Apple read-only driver). | No change to the volume. |

**Session channel (how the daemon learns of a write error, and how a session proves it ended cleanly).** The worker is stock `ntfs-3g`, whose errors otherwise reach only FUSE replies and its log, and whose exit status is 0 after every return from `fuse_loop()` (`src/ntfs-3g.c` `main()`: `err = 0` right after `fuse_loop(fh)`; `ntfs_close()` only logs "Failed to close volume" when `ntfs_umount()` fails) [V:S25]. Silence on a channel is therefore never evidence. Patch 0006 (ADR-002, ADR-018) writes fixed-format records to the descriptor named by the environment variable `NTFSMAC_ERRFD`, and the daemon requires positive records for a clean session.

Records (ASCII, one per line, at most 32 bytes including `\n`):

| Record | Written by patch 0006 at | Meaning |
|---|---|---|
| `S rw` or `S ro` | `src/ntfs-3g.c` `main()`, immediately after `mount_fuse()` succeeded and `ctx->mounted = TRUE`; `ro` when `NVolReadOnly(ctx->vol)` (the engine's own `MAY_RDONLY` fallback included) | Channel is live; the volume is mounted, in this mode. |
| `E <op> <errno>` | `libntfs-3g/unix_io.c`: `ntfs_device_unix_io_pwrite()` and `ntfs_device_unix_io_write()` when the write fails or is short (`op` `pwrite`, `write`); `ntfs_device_unix_io_sync()` when `ntfs_fsync()` fails (`sync`); `ntfs_device_unix_io_close()` when its `ntfs_fsync()` or `close()` fails (`close`); `src/ntfs-3g.c` `ntfs_close()` when `ntfs_umount()` returns non-zero (`umount`) | A device-level failure. |
| `W fullfsync <errno>` | `libntfs-3g/unix_io.c` `ntfs_fsync()`, when `fcntl(F_FULLFSYNC)` fails and the fallback `fsync()` is used | The drive cache was not confirmed flushed (section 4.4). |
| `U <n>` | `src/ntfs-3g.c` `ntfs_close()`, only when `ntfs_umount()` returned 0; `n` is the number of `E` records this process tried to write (written or not) | The volume was released and the device synced and closed without an error that the library reported. |

`ntfs_close()` runs from the FUSE destroy handler `ntfs_fuse_destroy2()` (if libfuse calls it) and in any case at the end of `main()`; a second call returns at once because the first sets `ctx->vol = NULL`, so `U` is written at most once [V:S25]. `ntfs_umount()` returning 0 is not enough on its own: `__ntfs_volume_release()` (`volume.c`) ignores the return value of `ntfs_inode_sync()` for `$Bitmap`, `$MFT` and `$MFTMirr`, which is why a failed device write underneath still has to arrive as an `E` record and why `U` carries the count.

**Positive clean-session proof.** A read/write session is clean only if all of these hold; anything else is unclean:

1. The first record is `S rw` and the last is `U 0`; there is no `E` record and nothing after `U`.
2. The channel stayed within its bounds (below) and reached end-of-file after the worker exited.
3. The daemon saw the unmount (its own unmount request, or the Disk Arbitration unmount of the mount point) before the worker exited.
4. The session was not re-adopted after a daemon restart (section 7, `ntfsmacd` crash).
5. Worker exit status 0 (necessary, not sufficient).

Missing `NTFSMAC_ERRFD`, an unpatched worker, a lost read end, `SIGPIPE` in the worker, a descriptor leak that keeps the pipe open, and a worker that stays quiet all fail rule 1 or 2, so they give "unclean", never "clean". A `W` record does not make the session unclean; it only changes the report (section 4.5).

**Bounds and descriptor hygiene.**

| Item | Rule |
|---|---|
| Pipe | Created by the daemon with both ends `O_CLOEXEC`; the read end is non-blocking and read on the volume's queue. The write end reaches only the worker, through `posix_spawn` file actions with `POSIX_SPAWN_CLOEXEC_DEFAULT`. |
| In the worker | Patch 0006 sets `FD_CLOEXEC` on `NTFSMAC_ERRFD` when the device layer first reads the variable (at device open, before `mount_fuse()`), so no helper process that libfuse or macFUSE may start inherits it [A: U16], and writes with `O_NONBLOCK`; a write that would block is dropped and still counted in `U <n>`. |
| Line length | At most 32 bytes including `\n`. A longer line, a line without `\n` at end-of-file, a byte outside `0x20..0x7E` before the `\n`, or a record outside the grammar above is a protocol error. |
| Record count | At most 64 records per session. Record 65 is an overflow. |
| On protocol error or overflow | Journal `state=write_error` (synced); the session can no longer be clean; the daemon keeps reading and discarding into a fixed 4 KiB buffer until end-of-file, so the worker never blocks on a full pipe and memory stays bounded. It never closes the read end while the worker runs (no `SIGPIPE` on the worker). |
| After worker exit | The daemon drains the pipe without blocking. If end-of-file does not come within 2 s (some other process still holds the write end), the session is unclean. |

| Signal | Daemon reaction |
|---|---|
| First `E` record | Journal `state=write_error` (synced), notification "write error on <label>: unmounting; data written after <time> may be lost", unmount requested. |
| `S ro` in a `rw` session | Session will not be clean (the engine and the probe disagree); notification says the engine mounted read-only. |
| Protocol error or overflow | As the first `E` record. |
| Worker exit without `U 0` | Session not clean; journal and marker stay. |

The channel reports device-level failures only. Engine-level errors that are not device failures (`ENOSPC`, `EINVAL` on a bad name) are returned to the application and are not session failures. Log parsing is not used: NTFS-3G's log text is not a stable interface.

A compromised worker can write a false `S rw` and `U 0`. That makes only its own volume look clean, which is no more than it can already do by writing that volume's device directly (section 6, residual risk; D2). It cannot affect another volume's session: each pipe is per volume, and descriptors do not leak between spawns.

## 8. macOS integration

### 8.1 Disk Arbitration

The daemon is the integration point, not an FSKit module of our own (ADR-003). It uses the public DiskArbitration C API: `DARegisterDiskAppearedCallback`, `DARegisterDiskDisappearedCallback`, `DARegisterDiskMountApprovalCallback`, `DARegisterDiskUnmountApprovalCallback`, `DADiskUnmount`, `DADiskEject`.

Facts from this host (macOS 27.2.1) that shape the design [V:S17]:

| Driver | Type | Claims `Windows_NTFS` with `FSProbeOrder` |
|---|---|---|
| Paragon `ufsd_NTFS.fs` | kext-backed bundle in `/Library/Filesystems` | 1 |
| Apple `ntfs.fs` | system bundle, read-only | 1000 |
| Apple `com.apple.fskit.exfat` | FSKit module | 2000 (probes NTFS-typed partitions for exFAT) |

Kext-backed bundles are probed before FSKit modules, and Apple's read-only NTFS driver masks a third-party NTFS FSKit module in automount; an Apple engineer called the kext priority "difficult to address" [V:S14]. A probe result of "usable but limited" suppresses automount [V:S13]. These are the reasons a native FSKit module would also need a Disk Arbitration helper.

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

Each guarantee names what it depends on. A guarantee marked **conditional** must not be stated to users, in the README or in the release report, until the named tasks have passed; until then the "Until then" column is what may be said.

| # | Guarantee | Mechanism | Status | Until then |
|---|---|---|---|---|
| D1 | After an eject or unmount that the product reports as clean and flushed (section 4.5 step 3, no `W` record), every byte the applications wrote before the unmount started is on the device and the drive cache has been flushed. | FSKit `synchronize` + `unmount` deliver every dirty buffer to the worker [A: U4]; `ntfs_umount`; upstream `ntfs_fsync()` issues `F_FULLFSYNC` in the worker (`unix_io.c`), with its silent `fsync` fallback made visible as a `W` record by patch 0006; daemon `F_FULLFSYNC`; the clean report requires the positive clean-session proof of section 7 (`S rw`, `U 0`, no `E`) and a successful daemon `F_FULLFSYNC`, and says "flushed" only without a `W` record and when the daemon's own `F_FULLFSYNC` did not fall back. | **Conditional** on T-M2-06 (opcodes at unmount), T-M3-10 (`F_FULLFSYNC` at each sync point) and `I-SYNC-*`; real drive caches only after Environment C (T-M5-10). | "Unmount writes all data the file system received and asks the drive to flush; whether FSKit hands over every cached write before unmount is being verified." |
| D2 | If the probe correctly classifies a volume as unsafe, no process of this product receives a writable descriptor for it, so no later parser bug (worker or volmark) can write to it. | Policy maps unsafe states to `ro`; `O_RDONLY` descriptor; `argv_check()` rule 7. | By design; tested by `U-POL-*`, `I-DMN-02..08`. | |
| D3 | A volume left without clean unmount by this product is mounted read-only on next attach to this Mac. Once the on-disk marker is enabled, it is also flagged `VOLUME_IS_DIRTY` for Windows, Linux and other Macs. | Journal (section 3.5); marker (section 3.6). | This-Mac part: by design, tested by `U-JRN-*`, `I-DMN-14..16`. Other-machine part: **host-only until T-M3-12 passes** (and `I-DMN-22`). | "Read-only next time on this Mac." |
| D4 | Windows hibernation state and `$LogFile` content are never discarded automatically. | `norecover` always; `remove_hiberfile` and `recover` banned by the option allow-list and `argv_check()`. | By design; `U-POL-*`, `U-ARGV-*`. | |
| D5 | Data read back equals data written (hash equality across unmount and remount, and against Linux NTFS-3G). | Integrity suite. | Tested (`I-INT-*`, `C-REF-*`). | |

**What D2 does not cover.** The verdict comes from `ntfsmac-probe`, which parses untrusted on-disk metadata with `libntfs-3g` and must be assumed exploitable (section 6). A probe bug, or a crafted volume that makes the probe report `clean` for a dirty, unclean-log or hibernated volume, leads to an `O_RDWR` descriptor. The product then has two further checks; they reduce the risk but are not guarantees, because they use the same parser family as the probe:

| Second line | Covers | Does not cover |
|---|---|---|
| `ntfsmac-volmark set` runs the library's hibernation and `$LogFile` checks itself through the public API (`ntfs_volume_check_hiberfile`, `ntfs_check_logfile`, `ntfs_is_logfile_clean`, restart page 2.0), so it refuses hibernated (`hibr`/`HIBR` header or short `hiberfil.sys`), unclean-log and version-2 `$LogFile` volumes, and it refuses an already set `VOLUME_IS_DIRTY`; any refusal makes the mount `ro` (section 3.6). | `hibernated`, `fast_startup`, `unclean_log`, `dirty_flag` misreported as `clean` | Hibernation states the engine does not recognise either (it checks only the `hibr`/`HIBR` signature and the file size; a `wake`/`WAKE` header is caught by the probe only, fixture `hiber-wake`); a differently cased name (`HiberFil.sys`) or a copy outside the root, which neither volmark nor the engine looks at (fixtures `hiber-case`, `hiber-subdir`; the probe catches the case variant only); a crafted volume that fools both. Windows itself writes and resumes only the root `hiberfil.sys` (U19), so a copy elsewhere is not a hibernation image the product must protect; it is listed here so that nobody assumes it is checked. Absent while the marker step is disabled. |
| The worker always gets `norecover` and never `remove_hiberfile`, so `ntfs-3g` sets `NTFS_MNT_MAY_RDONLY` and falls back to read-only on the same hibernation and `$LogFile` findings [V:S25, `ntfs-3g.c`, `volume.c`]. | `hibernated`, `fast_startup`, `unclean_log` | `dirty_flag` (the engine does not check it); a crafted volume that fools the engine. |

`I-DMN-23` tests both lines with a probe stub that reports `clean` for every unsafe fixture and asserts that the image hash is unchanged.

**What D1 depends on that the product cannot see.** If FSKit drops a flush error (N2) for data that never reached the worker, neither the worker nor the session channel can know. The clean report in section 4.5 only proves that everything the worker received was written without a reported error, and, without a `W` record, flushed with `F_FULLFSYNC`.

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
| N9 | Executable files on the volume. `fmask=0133` clears the execute bit on every file, so scripts and binaries cannot be run from an NTFS volume, and `chmod +x` has no lasting effect (error or silent no-op: recorded by `I-PERM-03`). | Deliberate conservative mapping (ADR-012); the backend's `chmod` behaviour is [A] until `I-PERM-*`. |

User-facing documentation must carry N1 to N5 and N9 verbatim in a "Limitations" section.

## 10. Build and deployment view

| Artifact | Built by | Installed at |
|---|---|---|
| `ntfsmac-worker`, `mkntfs`, `ntfsfix`, `ntfsinfo`, `ntfslabel` | NTFS-3G autotools build driven by `scripts/build-ntfs3g.sh`, `--with-fuse=external`, static `libntfs-3g` | `/usr/local/libexec/ntfs-macos/` |
| `ntfsmac-probe`, `ntfsmacd`, `ntfsmac` | CMake + Ninja (ADR-007) | `/usr/local/libexec/ntfs-macos/`, `/usr/local/bin/ntfsmac` |
| `org.ntfs-macos.ntfsmacd.plist` | static file | `/Library/LaunchDaemons/` |
| sandbox profiles, default config | static files | `/Library/Application Support/ntfs-macos/` |
| `ntfs-macos-<ver>.pkg`, `uninstall.sh` | `pkgbuild`/`productbuild` (present in base macOS) | local `dist/` only (ADR-015) |

Target: arm64, deployment target macOS 27.0. External runtime prerequisite: macFUSE >= 5.5.0 installed by the user.

## 11. Open unknowns

| ID | Unknown | How to resolve | Blocks |
|---|---|---|---|
| U1 | Does macFUSE's libfuse2 accept `backend=fskit`? | T-M2-03 | Everything |
| U2 | Does a macOS 27 guest under Virtualization.framework load and run the macFUSE FSKit module? (kexts do not load in VMs [V:S3]; FSKit is user space, so expected yes.) | T-M2-02 | VM testing |
| U3 | Can the FSKit extension be enabled without the GUI (`pluginkit -e use`)? | T-M2-02 | VM automation |
| U4 | Which FUSE opcodes result from `fsync`, `F_FULLFSYNC`, `sync`, unmount? | T-M2-06 | Section 9 wording |
| U5 | Does `ntfs-3g` accept `/dev/fd/N` (device size and sector-size ioctls on a re-opened descriptor)? | T-M2-05; fallback patch `0003` | Privilege separation |
| U6 | Can the FUSE server run as the console user when started by a root daemon (forum posters report that FSKit modules are enabled per user and that root-run `mount -F` fails [V:S14]; the Apple engineer in that thread is unsure about root mounts, so this is [A] until T-M4-01)? | T-M4-01 | Daemon design |
| U7 | Is a Disk Arbitration mount-approval dissenter still honoured for external NTFS disks on macOS 27? | T-M4-02; fallback: let Apple's driver mount read-only, then unmount and remount | Automount |
| U8 | Does Finder eject of the volume also eject the media? | T-M4-03 | UX |
| U9 | Does NTFS-3G already set `VOLUME_IS_DIRTY` during a read/write session? Source answer: no, only ntfsprogs tools write it [V:S25]. Test confirmation pending. | T-M3-01 (test) | Nothing: ADR-006 phase 2 is the default plan either way |
| U10 | Raw (`/dev/rdisk`) versus buffered (`/dev/disk`) device node: alignment handling in `unix_io.c` on Darwin, and performance. | T-M3-10 | Performance, D1 |
| U11 | tart licence terms and macOS 27 guest support in the current release (only a secondary source found [V:S20]). | T-M1-04 | VM tooling |
| U12 | Does a Windows read/write mount, and `chkdsk`, always advance `current_lsn` in the `$LogFile` restart area; does `chkdsk` clear `VOLUME_IS_DIRTY`, and which other flags (for example `VOLUME_MODIFIED_BY_CHKDSK`) does it leave set; and does a plain Windows read/write mount without `chkdsk` leave our `VOLUME_IS_DIRTY` set (the negative case)? | Windows steps in T-M5-10 (`H-USB-08`, `H-USB-09`); H6 | Enabling journal-clear rule 2 (section 3.5); flag rule of section 3.1 check 3 |
| U13 | Does an NTFS-3G read/write session with `norecover` leave the `$LogFile` restart area byte-identical? Source: `ntfs_logfile_reset` is only called with `recover` [V:S25]. | T-M3-01 (test) | Enabling journal-clear rule 2 |
| U14 | Does the macFUSE FSKit backend name the volume from `volname` or from the mount-point leaf? | T-M2-04 | Whether `volname` is passed at all (section 3.3) |
| U15 | Does Windows `chkdsk /f` repair a torn or mismatched `$MFT`/`$MFTMirr` copy of record 3 (the outcome of a volmark write cut between its two writes), and does Apple's read-only `ntfs.fs` mount such a volume? NTFS-3G refuses it at every mount (`volume.c` `ntfs_device_mount` mirror compare) [V:S25]. | Fixture `vmk-split` (T-M3-12) taken to Windows in T-M5-10 (`H-USB-10`); Apple driver in the VM (T-M4-11) | Residual-risk text of section 3.6 and R-33 |
| U16 | Does macFUSE's libfuse (v2) or its mount path start helper processes from the worker that would inherit descriptors, and is `SIGPIPE` ignored in the worker? (macFUSE's libfuse is not in the NTFS-3G tarball.) | T-M4-13 (process tree and descriptor table of a live worker in the VM) | Nothing: `FD_CLOEXEC`, `POSIX_SPAWN_CLOEXEC_DEFAULT` and the non-blocking write are applied either way (section 7) |
| U17 | Can the root daemon validate an Authorization Services external form sent by the CLI for a custom right with rule `authenticate-admin-nonshared` on macOS 27, and how does the prompt behave in an SSH session without a GUI? | T-M4-04 | ADR-019 check 4; how `I-DMN-19`/`I-DMN-21` answer the prompt |
| U18 | Does the Disk Arbitration description show `DAVolumePath` for a disk mounted by Apple's `ntfs.fs` or another driver, and does an `O_RDWR` open of such a disk fail? | T-M4-02 | Binding step of section 4.1 step 6 (ADR-020) |
| U19 | Does Windows resume only from `\hiberfil.sys` in the root (never another directory), and does the probe's root listing with `$UpCase` comparison find every case variant that NTFS-3G's lookup misses? | T-M3-02 (probe) with fixtures from T-M3-04, run against the probe and the engine; Windows part in T-M5-10 | Check 5 of section 3.1; second-line table of section 9 |
| U20 | Does `getfsstat(MNT_NOWAIT)` on macOS 27 list a macFUSE FSKit mount promptly and without a call into the worker (Apple's `getfsstat(2)` describes `MNT_NOWAIT` as returning kernel-retained data), including while the worker is stopped with `SIGSTOP`? (Not testable from the NTFS-3G tarball.) | T-M4-01 (VM: stop the worker, poll for 30 s, check the daemon queue stays responsive) | Mount-wait step of section 4.1 step 8; fallback is a helper process that does the `statfs` under a timeout |

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
| S25 | NTFS-3G 2026.9.28 release tarball (digest in IMPLEMENTATION_PLAN.md section 3), read without building: `src/ntfs-3g_common.c` (`optionlist`, `parse_mount_options`, `ntfs_strappend_escaped`), `src/ntfs-3g.c` (mount flags from `recover`/`remove_hiberfile`), `libntfs-3g/volume.c` (read/write mount checks, `ntfs_logfile_reset`, `ntfs_volume_write_flags`), `ntfsprogs/*` (the only writers of `VOLUME_IS_DIRTY`); for the M1 re-check fixes also `src/ntfs-3g.c` (`main`, `ntfs_close`, `ntfs_fuse_destroy2`, `ntfs_open`), `libntfs-3g/unix_io.c` (`ntfs_fsync`, `ntfs_device_unix_io_pwrite`, `_write`, `_sync`, `_close`), `libntfs-3g/volume.c` (`ntfs_device_mount` mirror compare and check block, `fix_txf_data`, `__ntfs_volume_release`, `ntfs_volume_write_flags`), `libntfs-3g/mft.c` (`ntfs_mft_records_write`), `include/ntfs-3g/layout.h` (`VOLUME_FLAGS`, `NTFS_BOOT_SECTOR`), `include/ntfs-3g/volume.h` and `logfile.h` (public check functions) |
