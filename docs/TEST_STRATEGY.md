# Test strategy

Source tags `[V:Sn]`, `[A]`, `[U]` are defined in [ARCHITECTURE.md](ARCHITECTURE.md). Acceptance thresholds are in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) section 2.

## 1. Principles

| Principle | Consequence |
|---|---|
| Data corruption is release-blocking. | Every write test ends with a hash comparison after unmount and remount. A mismatch stops feature work. |
| Linux results say nothing about macOS. | Each test id is tagged with the environment that ran it; the status table never promotes a Linux result to "Verified (VM)". |
| No destructive test on the developer host. | The host builds, runs unit tests and drives VMs. It never mounts an NTFS image: Paragon NTFS on the host claims `Windows_NTFS` volumes with probe order 1 and would mount any attached NTFS image itself [V:S17]. |
| A test that cannot fail is a defect. | Each integrity test has a negative control (a deliberately corrupted byte must make it fail). Agent E checks this in the M5 audit. |
| Images are disposable and synthetic. | Fixtures are generated, never copied from real disks. Fuzz inputs come only from generated images. |

## 2. Environments

| | A: Linux container | B: macOS 27 VM | C: hardware |
|---|---|---|---|
| Purpose | Build pinned NTFS-3G; generate fixtures; reference behaviour; unit tests; crash-consistency sweep; fuzzing; sanitizers | The real product path: macFUSE FSKit backend, daemon, Finder | USB media, real unplug, drive caches, Windows interoperability |
| Tool | Docker or Podman, image `vm/linux/Dockerfile` (Debian 13, pinned digest). Needs `--device /dev/fuse --cap-add SYS_ADMIN` for reference mounts. Fallback: Linux guest under tart. | tart (ADR-008). Golden image = vanilla macOS 27 + macFUSE 5.5.0 with its extension approved once by the human (H3). Each run uses a fresh `tart clone` and deletes it afterwards. | The human's test Mac or boot volume (H7), disposable media (H5), Windows machine or VM (H6) |
| Disk under test | Plain image file (`mkntfs -F`), mounted with `ntfs-3g` on the file | Raw image attached to the guest as a second virtual disk (`tart run --disk`), or attached inside the guest with `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount`; both give a `/dev/diskN` [A: T-M1-04 confirms] | Physical USB flash drive and USB SSD/HDD, identified by serial number in the approval |
| Rollback | Container is ephemeral | Clone per run; golden image never modified by tests | Re-format with `mkntfs` between scenarios |
| Automation | Full | Full after H3, over SSH | Scripted steps with human hands for plug and unplug |
| Diagnostics collected | stdout/stderr, ASan reports, core files | `log show` for `fskitd`, `io.macfuse`, `ntfsmacd`; worker FUSE debug log; `~/Library/Logs/DiagnosticReports`; `sysdiagnose` on hang | Same as B plus Windows `chkdsk` output |

### What the VM cannot show

| Gap | Why | Covered by |
|---|---|---|
| Real USB insertion and surprise removal | Virtual disks do not hot-unplug like USB. macOS 27 adds USB accessory passthrough to guests [V:S23]; tool support is [U]. | Environment C; in B, a force-detach of the image approximates it |
| Drive write-cache behaviour, `F_FULLFSYNC` honesty | Virtual disk backed by host file cache | Environment C |
| Sleep/wake with a mounted volume | Guest sleep is not representative | Environment C |
| Coexistence with Paragon or other NTFS drivers | Not installed in the guest; a dummy bundle tests only the detection logic | Environment C, only if the human chooses (H7) |
| Kernel backend | Kexts do not load in VMs [V:S3] | Not in scope (ADR-001) |
| Whether FSKit extensions work in a guest at all | [U2] | T-M2-02; if they do not, Environment B is impossible and a dedicated test Mac is required (brief section 4) |

## 3. Fixtures

All fixtures are generated in Environment A by scripts under `tests/fixtures/gen/` from a seed; images are not committed. `tests/fixtures/manifest.json` lists name, generator, seed, size and the SHA-256 expected after generation.

### 3.1 Clean images

| Name | Size | Cluster | Sector | Content |
|---|---|---|---|---|
| `clean-64m-4k` | 64 MiB | 4 KiB | 512 | empty |
| `clean-256m-4k` | 256 MiB | 4 KiB | 512 | empty |
| `clean-256m-64k` | 256 MiB | 64 KiB | 512 | empty |
| `clean-256m-4kn` | 256 MiB | 4 KiB | 4096 | empty |
| `tree-1g` | 1 GiB | 4 KiB | 512 | Seeded tree: 2,000 files, depth 8, sizes 0 B to 32 MiB, Unicode names, 20 files with named streams, 5 sparse, 5 compressed, hard links |
| `big-8g` | 8 GiB (sparse host file) | 4 KiB | 512 | one 5 GiB file |
| `tiny-*` | 1 to 8 MiB | 512 B to 4 KiB | 512 | fuzz seeds |

Command pattern: `mkntfs -F -Q -L <label> -c <cluster> -s <sector> <file>` from the pinned ntfsprogs. Population is done through a Linux `ntfs-3g` mount inside the container.

### 3.2 Unsafe-state images (synthetic)

| Name | How it is made | Expected probe `state` | Expected NTFS-3G behaviour with `norecover` [V:S11] |
|---|---|---|---|
| `dirty-flag` | `tests/tools/setvolflags` sets `VOLUME_IS_DIRTY` with `ntfs_volume_write_flags` | `dirty_flag` | (not checked by NTFS-3G at mount; our probe is the only guard) [A: confirmed by T-M3-01] |
| `unclean-log` | `tests/tools/logfile_mark` clears the "volume is clean" bit in both `$LogFile` restart areas, keeping fix-ups valid | `unclean_log` | `EOPNOTSUPP`, read-only fallback or refusal |
| `log-v2` | `logfile_mark --v2` sets restart page major version 2 | `fast_startup` | `EPERM`, rejected even with `recover` |
| `hiber-lower` | create `/hiberfil.sys`, 8 KiB, first bytes `hibr` | `hibernated` | `EPERM` |
| `hiber-upper` | same with `HIBR` | `hibernated` | `EPERM` |
| `hiber-short` | `/hiberfil.sys` of 100 bytes | `hibernated` | `EPERM` |
| `hiber-wake` | `/hiberfil.sys`, 8 KiB, first bytes `wake` | `hibernated` | mounts read/write (the engine does not recognise `wake`; the probe is the only guard, ARCHITECTURE.md section 3.1 check 5) |
| `hiber-case` | `/HiberFil.sys`, 8 KiB, first bytes `hibr` (name differs in case; the engine's lookup is case sensitive) | `hibernated` | mounts read/write (the engine does not find the file; the probe is the only guard, ARCHITECTURE.md section 3.1 check 5) |
| `hiber-subdir` | `/sub/hiberfil.sys`, 8 KiB, first bytes `hibr`, no file in the root | `clean` | mounts (accepted: neither the probe nor the engine looks outside the root, ARCHITECTURE.md section 9) |
| `hiber-inactive` | `/hiberfil.sys`, 8 KiB of zeros | `clean` | mounts |
| `bitlocker-sig` | 64 MiB of zeros with `-FVE-FS-` at offset 3 | `unsupported` | not NTFS |
| `truncated` | `clean-64m-4k` cut to 60 percent | `corrupt` | mount fails |
| `not-ntfs` | exFAT image | `unsupported` | not NTFS |
| `chkdsk-flag` | `setvolflags` sets only `VOLUME_MODIFIED_BY_CHKDSK` (0x8000) | `clean` (informational flag, ARCHITECTURE.md section 3.1 check 3) | mounts (the engine does not check flags) |
| `dirty-chkdsk-flag` | `VOLUME_IS_DIRTY` and `VOLUME_MODIFIED_BY_CHKDSK` | `dirty_flag`; `repair --clear-dirty` allowed | mounts |
| `txf-nonresident` | `tests/tools/txf_make` makes the root directory's `$TXF_DATA` non-resident | `clean` | read/write mount runs `fix_txf_data()` and makes it resident (`volume.c`); `FORENSIC` and `RDONLY` mounts do not |
| `vmk-split` | Made by T-M3-12: `clean-64m-4k` after `volmark set` cut between the `$MFT` and `$MFTMirr` writes of record 3 | `corrupt` | every mount fails ("$MFTMirr does not match $MFT"); used for U15 |

Each generator finishes by running Linux `ntfs-3g -o norecover,ro` and `-o norecover` against the image and asserting the behaviour in the last column, so the fixtures are validated against the engine, not only against our probe.

### 3.3 Hostile-label images (synthetic)

Generated by `tests/fixtures/gen/labels.sh` (T-M3-19) from `clean-64m-4k` with the pinned `ntfslabel`. Each must mount at the expected `/Volumes/<leaf>` with the expected worker argv; the image itself is otherwise clean.

| Name | Label (shown escaped) | Expected leaf |
|---|---|---|
| `label-comma` | `X,remove_hiberfile` | `X_remove_hiberfile` |
| `label-fuseopt` | `X,allow_other,uid=0` | `X_allow_other_uid_0` |
| `label-dotdot` | `../../etc/x` | `_._.._etc_x` |
| `label-dot` | `.hidden` | `_hidden` |
| `label-backslash` | `a\,b` | `a__b` |
| `label-newline` | `a\nb` | `a_b` |
| `label-bidi` | `abc\u202Etxt.exe` | `abc_txt.exe` |
| `label-long` | 128 UTF-16 units of `é` | 32 code points (64 bytes) |
| `label-empty` | empty | `NTFS-<serial>` |
| `label-dup-a`, `label-dup-b` | `DATA` on both | `DATA`, then `DATA 2` |

### 3.4 Real Windows images (optional, human-provided)

Created on Windows (H6): volume after hibernate, after shutdown with Fast Startup enabled, after power-off during a copy, and a BitLocker volume. Stored outside the repository; referenced by SHA-256 in `tests/fixtures/external.json`. They upgrade tests `U-PROBE-*` from "synthetic" to "real" evidence. If not provided, the release notes say that unsafe-state detection was verified on synthetic images only.

## 4. Test matrix

Environment column: where the test must pass to count. `A*` means "also run on Linux as a reference".

| Brief area | Id prefix | Cases | Env | Task |
|---|---|---|---|---|
| **Functional** | | | | |
| Mount and unmount | `I-SMOKE-01`, `I-SMOKE-02`, `I-DMN-01..08`, `I-DMN-10..13` | manual worker mount (`I-SMOKE-01`); mount by inherited device fd (`I-SMOKE-02`); daemon automount per fixture class (`I-DMN-01..08`); four unmount paths (`I-DMN-10..13`) | B | T-M2-03, T-M2-05, T-M4-03, T-M4-11 |
| Create, read, write, rename, move, delete | `I-FUNC-*` | create/append/overwrite/truncate up and down; rename same and cross directory, over existing; delete open file; rmdir non-empty fails | B, A* | T-M3-05 |
| Directories and nested paths | `I-FUNC-DIR-*` | depth 32; 10,000 entries; rename directory with contents | B, A* | T-M3-05 |
| Unicode and special names | `I-NAME-*` | NFC/NFD pairs, emoji, CJK, RTL, 255 UTF-16 units, trailing dot/space and reserved names refused (`windows_names`), case-only pairs | B, A* | T-M3-05 |
| Small and large files | `I-BIG-*` | 0 B, 1 B, cluster size +/- 1, 4 GiB + 1, 5 GiB | B | T-M3-09 |
| Sparse files | `I-BIG-SPARSE-*` | hole at start, middle, end; hash and allocated size | B, A* | T-M3-09 |
| Metadata and timestamps | `I-META-*` | mtime/ctime/creation time round trip; `touch -t`; read-only attribute; hidden flag | B, A* | T-M3-06 |
| xattrs and named streams | `I-XATTR-*` | set/get/list/remove; 1 B to 64 KiB values; stream visible in Linux tree dump; `ditto`, `cp -c`, Finder copy | B | T-M3-08 |
| Concurrent access | `S-PAR-*` | 16 writers, readers during writes, rename storms | B | T-M5-02 |
| **Integrity** | | | | |
| Hash before and after remount | `I-INT-01..03` | 100 random files; `tree-1g` full copy in and out; negative control | B | T-M2-08 |
| Repeated write/read cycles | `I-INT-CYCLE-*` | 200 overwrite cycles on 50 files | B | T-M3-05 |
| Interrupted writes | `S-CRASH-*` (Linux sweep), `S-KILL-*` | cut after write N; kill worker mid-copy | A, B | T-M3-14, T-M5-05 |
| Forced process termination | `S-KILL-*`, `I-DMN-14..15` | `SIGKILL` worker, daemon, macFUSE module process | B | T-M4-05, T-M4-11, T-M5-05 |
| Unclean unmount | `S-DISC-*`, `I-DMN-16` | force-detach image during write; next mount is read-only | B, C | T-M4-11, T-M5-04 |
| Dirty and hibernated refusal | `U-PROBE-*`, `I-DMN-02..08` | every image in 3.2: probe verdict, daemon mode, image hash unchanged after a read-only session | A, B | T-M3-02, T-M4-11 |
| Cross-platform interoperability | `C-REF-*`, `H-WIN-*` | Linux reads macOS-written image and the reverse; Windows `chkdsk` clean | A+B, C | T-M3-15, T-M5-10 |
| **Reliability** | | | | |
| Long-running workload | `S-SOAK-01` | 6 h mixed workload | B | T-M5-02 |
| Parallel operations | `S-PAR-01` | 16 processes, 30 min | B | T-M5-02 |
| Memory leaks | `S-LEAK-01`, ASan runs | `leaks` on worker; ASan suite on Linux | B, A | T-M5-03 |
| Device disconnection | `S-DISC-*`, `H-USB-*` | image force-detach; real unplug | B, C | T-M5-04, T-M5-10 |
| Low disk space | `S-NOSPC-01` | fill to `ENOSPC`, verify earlier files | B, A* | T-M5-06 |
| Corruption detection | `U-PROBE-*`, `F-*` | truncated and mutated images never crash the probe or the daemon | A | T-M3-16 |
| Recoverability | `S-CRASH-*`, `I-DMN-14..16` | after each failure: read-only mount works, synced data intact | A, B | T-M3-14, T-M4-11 |
| **Performance** | | | | |
| Sequential read/write | `P-SEQ-*` | 1 GiB, 1 MiB blocks | B, A* | T-M5-07 |
| Random read/write | `P-RND-*` | 4 KiB blocks over a 1 GiB file | B, A* | T-M5-07 |
| Small files | `P-SMALL-*` | create, stat, read, delete 10,000 files of 4 KiB | B, A* | T-M5-07 |
| Directory enumeration | `P-DIR-*` | list 10,000 and 100,000 entries | B, A* | T-M5-07 |
| CPU and memory | `P-RES-*` | worker and FSModule CPU seconds, peak RSS per benchmark | B | T-M5-07 |
| **Safety and security** | | | | |
| Policy and options | `U-POL-*` | every policy row; banned options unreachable | H, A | T-M3-03 |
| Journal | `U-JRN-*` | kill at each step; entry fields; `marker`/`state` transitions; four removal paths; tombstones; file names only from the parsed serial (section 4.1) | H, A | T-M3-13 |
| Disk-derived strings | `U-LABEL-*`, `U-ARGV-*`, `U-VERDICT-*`, `F-SANITIZE-*`, `I-DMN-18` | label sanitiser, argv validator, probe-verdict parser; hostile-label fixtures end to end (section 4.1) | H, A, B | T-M3-19, T-M4-03 |
| Journal clearing and repair | `I-DMN-19..21`, `H-USB-08`, `H-USB-09` | `journal-clear`, evidence rule, `repair --clear-dirty`, peer checks and administrator prompt of ADR-019 (section 4.1) | B, C | T-M4-14, T-M5-10 |
| On-disk dirty marker | `U-VMK-01..10`, `I-DMN-22`, `H-USB-10` | volmark steps, refusals, binding, cut at each write against the crash table; marker visible after force-detach (section 4.1) | A, B, C | T-M3-12, T-M4-14, T-M5-10 |
| Mount-point handling | `I-DMN-26` | descriptor-only mount point; hung worker during the mount wait (section 4.1 steps 7 and 8) | B | T-M4-01 |
| Device binding | `I-DMN-25` | disk swapped between probe and mount; disk mounted by another driver (section 4.1, ADR-020) | B | T-M4-14 |
| Second line against a wrong probe verdict | `I-DMN-23` | probe stub reports `clean` for every unsafe fixture; image hash unchanged | B | T-M4-11 |
| Session channel | `I-ERR-01..07`, `I-DMN-24` | patch 0006 records `S`, `E`, `W`, `U`; positive clean-session proof; bounds and `FD_CLOEXEC`; daemon marks session unclean and does not report a clean unmount (section 4.1) | A, B | T-M3-18, T-M4-13 |
| Privilege | `I-DMN-09`, sandbox tests, ctl authentication tests | worker uid, sandbox denials (including `connect()` to the control socket), socket peer checks of ADR-019 | B | T-M4-01, T-M4-04, T-M4-06 |
| Coexistence | `I-DMN-17` | dummy third-party NTFS bundle present: no dissent, no automount; `doctor` names the bundle | B | T-M4-07, T-M4-11 |
| Permissions and ownership | `I-PERM-01..06` | uid/gid, modes, `chmod`/`chown` outcomes, execute bit (N9), opt-in mapping smoke (section 4.1) | B, A* | T-M3-20 |
| Races in daemon logic | `U-TSAN-01..02` | state machine, control handler, journal and session under ThreadSanitizer with concurrent drivers (section 4.1) | H, A | T-M4-15 |
| Read path edge (macFUSE mitigation) | `I-READ-*` | no `EIO` at EOF | B | T-M3-07 |
| mmap | `I-MMAP-*` | mapped read and write, `msync`, unmount with dirty pages | B | T-M3-11 |
| Sync semantics | `I-SYNC-01..02` | opcode trace; upstream `F_FULLFSYNC` reaches the device at each sync point; fallback recorded (section 4.1) | B | T-M2-06, T-M3-10 |
| Packaging | `I-PKG-*` | install, doctor, uninstall, manifest equality | B | T-M4-10, T-M6-01 |
| Finder | `I-FND-*` | visible, copy in and out, eject | B, C | T-M4-11 |

`I-DMN-*` ranges: 01..08 automount per fixture class (02..08 are the unsafe classes of section 3.2), 09 privilege, 10..13 unmount paths, 14..16 kill worker, kill daemon, force-detach, 17 passive mode, 18 hostile labels, 19..21 journal clear, evidence rule, repair, 22 marker, 23 second line, 24 session channel, 25 device binding. Each implementing task writes the tests of its own range under its own file prefix in `tests/integration/daemon/` (T-M4-01: 09; T-M4-03: 10..13, 18; T-M4-05: 15; T-M4-07: 17; T-M4-14: 19..22, 25; T-M4-13: 24); T-M4-11 writes the rest (01..08, 14, 16, 23) and runs the whole range; T-M5-05 repeats 14..16 twenty times.

### 4.1 Enumerated cases for disk strings, journal clearing, marker, error channel, permissions and races

These ids come from the M1 audit fixes (ADR-016, ADR-017, ADR-006, ADR-018, ADR-012) and the M1 re-check fixes (ADR-014, ADR-019, ADR-020).

| Id | Case | Pass condition |
|---|---|---|
| `U-LABEL-01` | Every ASCII byte `0x00`-`0x7F` as a one-byte label | Only the ADR-016 allow-list survives; all others become `_`; no output byte in `, = \ / :` or below `0x20` |
| `U-LABEL-02` | `X,remove_hiberfile`, `X,allow_other`, `X=1`, `a\,b` | No `,` `=` `\` in output |
| `U-LABEL-03` | `.`, `..`, `../../etc/x`, `.hidden`, ` . `, trailing dots and spaces | Never `.` or `..`; no leading `.`; no `/`; no leading or trailing space or dot |
| `U-LABEL-04` | Invalid UTF-8: overlong `/` (`C0 AF`), lone continuation bytes, surrogates, 5-byte forms | Whole label replaced by the fallback |
| `U-LABEL-05` | C1 controls, bidi and format characters, U+2028/2029, private use, noncharacters, slash and colon look-alikes | Each replaced by `_` |
| `U-LABEL-06` | 128 UTF-16 units of 1-, 2-, 3- and 4-byte characters | Output at most 64 bytes, valid UTF-8, cut on a code-point boundary |
| `U-LABEL-07` | Empty, all spaces, all dots, all disallowed | `NTFS-<last 8 hex digits of serial>` |
| `U-LABEL-08` | Collision sequence with existing `DATA`, `DATA 2`..`DATA 99` in a temporary directory standing in for `/Volumes` | Next free name chosen with `mkdirat`; after 99, refusal; a pre-existing symlink named `DATA` is never followed; an NFC name with an existing NFD directory of the same name (`e` U+0065 U+0301 against U+00E9) gets the suffix, never the existing directory |
| `U-ARGV-01` | Builder output for every policy row and option set, including the opt-in `permissions` and `usermapping=/usr/local/etc/ntfs-macos/UserMapping` rows | `argv_check()` accepts |
| `U-ARGV-02` | Every single-token mutation of a valid vector: add, drop, duplicate, reorder argv elements; insert each banned key; drop `norecover`; add or drop `ro`; change `uid`, `fmask`, `streams_interface`, `volname` | `argv_check()` rejects each |
| `U-ARGV-03` | `ro` decision with an `O_RDWR` descriptor, and the reverse | Rejected (rule 7) |
| `U-ARGV-04` | Option string with `\`, NUL, control byte, empty token, value containing `=`, length 1025; `usermapping=` with a path containing a space or any path other than the fixed one | Rejected |
| `U-ARGV-05` | Mount point outside `/Volumes`, with `/` in the leaf, or not equal to the created leaf | Rejected |
| `U-VERDICT-01` | Probe output over 64 KiB, truncated JSON, trailing garbage | State `unknown` |
| `U-VERDICT-02` | Duplicate keys, unknown keys, unknown `state` or `logfile` enum, wrong types | State `unknown` |
| `U-VERDICT-03` | `\u0000` or invalid UTF-8 inside `label` | State `unknown` |
| `U-VERDICT-04` | Valid output for every fixture in section 3.2 | Parsed fields equal the probe's |
| `U-VERDICT-05` | Hostile values for every field of the ARCHITECTURE.md section 3.1 grammar: `serial` as `../../../Library/LaunchDaemons/x`, `0x1a2b...` (lower case), 15 or 17 hex digits, with NUL, `/`, newline or ESC; `logfile_lsn` likewise; integers with sign, fraction, exponent, leading zero or out of range; a `reasons` key with free text; `reason_codes` with an unknown code, a duplicate, 17 entries, or text containing ESC `[`; `state` `clean` with a reason code | State `unknown` for each; for every accepted verdict the journal path the daemon would build is `<journal dir>/<16 upper-case hex>` and no output text contains a byte the probe sent other than through the label sanitiser |
| `U-JRN-NAME-01` | Journal library given serials 0, 1, 2^64 - 1 and a pre-planted symlink at the entry and temporary names | Names are exactly 16 upper-case hex digits; the symlink is never followed (`O_NOFOLLOW`, `O_EXCL`); tombstone names match `cleared/<16 hex>-<YYYYMMDDTHHMMSSZ>` |
| `F-SANITIZE-01` | libFuzzer on the probe-verdict parser, seeded with hostile `serial`, `logfile_lsn` and `reason_codes` values | No crash, no sanitizer report; every accepted input re-serialises to an equivalent object and satisfies the section 3.1 grammar of ARCHITECTURE.md |
| `F-SANITIZE-02` | libFuzzer on label sanitiser followed by `argv_check()` of the resulting builder output | No crash; every output satisfies ADR-016; `argv_check()` always accepts the builder's vector |
| `I-DMN-18` | Attach each section 3.3 image in the VM | Mount point and Finder name equal the expected leaf; `ps -o args` of the worker shows no option beyond the allow-list; nothing created outside `/Volumes` |
| `U-JRN-*` additions | Create, `marker` `pending`->`set`, `pending`->`failed`, `state`->`write_error`, each removal path, tombstone limit 32, kill between temporary write and rename | Entry is either the old or the new version, never partial; removal always leaves a tombstone |
| `I-DMN-19` | `journal-clear`: refused for non-`admin`, wrong label, wrong serial, probe not `clean`, volume mounted `rw`, an entry whose `session_id` belongs to a live `rw` session on another disk with the same serial (clone), a request sent from inside the worker's process group, a request from a re-signed copy of the CLI, a request without or with a reused authorization; accepted on a `clean` volume with an entry, and on `chkdsk-flag` with an entry (N-04: informational flag only) | Refusals leave the entry; acceptance moves it to tombstones; current mount mode unchanged; next `mount` is `rw`; the lost-flush case (`marker=set`, flag clear, LSN unchanged) shows the stronger warning |
| `I-DMN-20` | Evidence rule: unclean session (force-detach), then in Environment A `setvolflags` clears the flag and `logfile_mark --bump-lsn` advances the LSN; re-attach with rule enabled, then disabled | Enabled: attach is `ro`, entry removed, next `mount` is `rw`. Disabled: attach `ro`, entry kept. Flag cleared without LSN change: entry kept in both modes |
| `I-DMN-21` | `repair --clear-dirty`: refused unless probe exactly `dirty_flag` with clean log (tested with `unclean-log`, `hiber-lower`, a fixture with an extra non-informational flag), refused while mounted, refused for non-`admin`, refused without the ADR-019 checks (same peer cases as `I-DMN-19`); accepted on `dirty-flag` and on `dirty-chkdsk-flag` (which keeps 0x8000); extra warning text present when a journal entry exists | On success the flag is clear (`ntfsinfo -m`), no other byte of the `$Volume` record changed, journal entry removed; `$LogFile` unchanged |
| `I-DMN-22` | Marker in the daemon flow: during a `rw` session the image has `VOLUME_IS_DIRTY`; after clean eject it does not; after force-detach it does; volmark replaced by a test binary that exits 1 | Flag states as stated; failing volmark gives a `ro` mount and `marker=failed` |
| `U-VMK-01` | `set` on `clean-64m-4k` | Exit 0; flag set; every other byte of the `$Volume` record unchanged |
| `U-VMK-02` | `set` on `dirty-flag` | Exit 11; image hash unchanged |
| `U-VMK-03` | `set` on `unclean-log`, `log-v2`, `hiber-lower`, `hiber-upper`, `hiber-short` (and on `hiber-case`: exit 0 is the expected result, because volmark uses the engine's case-sensitive rule; the probe is what refuses it, `U-POL-*`) | Exit 10 (volmark's own checks through the public API); image hash unchanged |
| `U-VMK-04` | `clear` after `set` | Exit 0; record byte-identical to before `set` |
| `U-VMK-05` | `clear` on a volume without the flag | Exit 13; image hash unchanged |
| `U-VMK-06` | `clear-repair` on `dirty-flag` and `dirty-chkdsk-flag`, and on a volume with an extra non-informational flag | Exit 0, `VOLUME_IS_DIRTY` clear and 0x8000 unchanged; exit 13 and image unchanged |
| `U-VMK-07` | `SIGKILL` before step 4 and a fault-layer cut (`count`, then `cut:N` and `short:N:K` over every write) inside step 4, for `set` and `clear` | Each outcome equals its row in the crash table of ARCHITECTURE.md section 3.6: unchanged, or flag value new on both copies (probe `dirty_flag` or `clean`), or `$MFT`/`$MFTMirr` record 3 torn or different (probe `corrupt`); in every case no byte outside the two copies of record 3 changed; never a probe `clean` with the flag lost after `set` reported exit 0 |
| `U-VMK-08` | Read-back mismatch injected with `short:N:K` on the flag write | Exit 14 |
| `U-VMK-09` | `set` with `--expect-serial` or `--expect-sectors` different from the image | Exit 15; image hash unchanged |
| `U-VMK-10` | `set` and `clear` on `txf-nonresident`, and `set` on a volume with a bit outside `VOLUME_FLAGS_MASK` | First: exit 0, `$TXF_DATA` still non-resident, fault layer `count` shows exactly the two record-3 writes; second: exit 11, image unchanged |
| `I-ERR-01` | Linux reference mount with the faultio build, `eio:N` on a data write | `S rw` first; exactly one `E pwrite 5` line; last line `U 1` (or `E umount` if the unmount failed) |
| `I-ERR-02` | `eio` on the device sync, and on the close-path `fsync` | One `E sync <errno>`, one `E close <errno>` line; no `U 0` |
| `I-ERR-03` | No fault, full functional workload, clean unmount | Exactly `S rw` then `U 0`; `NTFSMAC_ERRFD` unset behaves exactly like the unpatched build |
| `I-ERR-04` | `ntfs_umount` made to fail (fault layer `dead:N` from the first unmount-time write) | `E` records then `E umount <errno>`; no `U` line; worker exit status 0 (shows exit status is not evidence) |
| `I-ERR-05` | Read-only mount (`-o ro`), and a `norecover` read/write request on `unclean-log` (engine fallback) | `S ro` first |
| `I-ERR-06` | Faultio build with `F_FULLFSYNC` forced to fail (macOS VM) | `W fullfsync <errno>` line at each sync point; sync still returns 0 |
| `I-ERR-07` | Inspect the worker's descriptor table and a child it starts (test hook `system("true")` in the faultio build) | `NTFSMAC_ERRFD` has `FD_CLOEXEC`; the child has no copy of it; the write end is non-blocking |
| `I-PERM-01` | In the VM, create files and directories with `touch`, `mkdir` and `cp`, and stat files made on Linux before the run | Every entry reports the console uid and gid |
| `I-PERM-02` | Same tree | Files mode 0644 and directories 0755, whatever the Windows ACLs of the fixture; the Linux tree dump of the result shows no ACL change |
| `I-PERM-03` | `chmod` 0600, 0755, `+x` on a file and a directory | Outcome recorded as error or no-op, identical on three runs; on re-stat the mode is the mapped one or the documented value; Linux tree dump shows no change in NTFS attributes or ACLs |
| `I-PERM-04` | `chown` to another uid and to root (as root inside the VM) | Same pass condition as `I-PERM-03`; ownership on re-stat is still the console user |
| `I-PERM-05` | A file with a `#!/bin/sh` header and a small native binary, `chmod +x`, execute | `EACCES` (limitation N9); directories remain traversable; a read-only mount behaves the same |
| `I-PERM-06` | Opt-in `permissions` with an on-volume `.NTFS-3G/UserMapping` file, and `usermapping=/usr/local/etc/ntfs-macos/UserMapping` read through the worker sandbox, in Environment A (reference) and B (mount accepted by the builder's test-only switch) | Mount succeeds, one file created and read back with equal hash; no data claim beyond that (ADR-012) |
| `U-TSAN-01` | Daemon state machine driven from four threads with random events, built with `-fsanitize=thread` (host and Environment A) | No data-race report; final state satisfies the transition invariants |
| `U-TSAN-02` | Control-protocol handler with concurrent clients plus concurrent journal and session updates (kill at random points) | No data-race report; journal entries never partial |
| `I-DMN-23` | Daemon with a probe stub that reports `clean` for `dirty-flag`, `unclean-log`, `log-v2`, `hiber-lower`; marker step enabled | No write reaches the image (hash unchanged); mode reported `ro` with reason; with the marker step disabled, the four outcomes are recorded (expected: `dirty-flag` is written, the others fall back to read-only) |
| `I-DMN-24` | (a) VM faultio worker with `eio:N` during a copy; (b) worker started without `NTFSMAC_ERRFD`; (c) unpatched worker binary; (d) worker that writes 100 KiB without newline, and one that writes 65 valid records; (e) daemon killed and restarted mid-session (re-adoption); (f) a child of the worker that keeps the write end open after the worker exits | (a) journal `state=write_error`, notification text, unmount requested; (a) to (f) no "all data written" report, journal entry and marker kept, next attach `ro`; (d) daemon memory stays bounded and the worker never blocks |
| `I-DMN-26` | Mount-point handling: (a) test hook swaps the created leaf for a symlink to another directory between `mkdirat` and `openat`; (b) worker stopped with `SIGSTOP` right after spawn | (a) attach fails, the symlink target is not chowned or removed; (b) the mount wait ends at its 30 s deadline, the volume goes to `Failed`, and the daemon answers `list` for other volumes throughout |
| `I-DMN-25` | (a) Probe runs on image X; before the `O_RDWR` open the test detaches X and attaches image Y under the same BSD name; (b) the disk is mounted read-only by Apple's driver before the `O_RDWR` open; (c) volmark stub that changes sector 0 | (a) and (c): hash or `st_rdev` mismatch, no `O_RDWR` descriptor kept, volume `ro` or `Failed`, image Y unchanged; (b) no `O_RDWR` open, state as ARCHITECTURE.md section 4.1 step 6 |
| `I-SYNC-01` | `sync_trace.sh` with `fs_usage`-style tracing of the worker during `sync`, unmount and FUSE fsync | `F_FULLFSYNC` issued on the device descriptor at each sync point; any fallback to `fsync` recorded |
| `I-SYNC-02` | Same trace on the raw and the buffered device node | Table of sync points and fallbacks per node, used by ADR-014 and D1 |
| `U-POL-FLAGS` | Probe verdicts with `volume_flags` `["MODIFIED_BY_CHKDSK"]`, `["MOUNTED_ON_NT4"]`, `["DIRTY","MODIFIED_BY_CHKDSK"]`, `["CHKDSK_UNDERWAY"]`, `["UNKNOWN_BITS"]` | First two: `clean` path, `journal-clear` gate open; third: `repair` gate open; last two: `dirty_flag`, `repair` refused |

## 5. Reference comparison against NTFS-3G on Linux

Purpose: show that running the same engine behind macFUSE FSKit does not change on-disk results.

1. Twelve deterministic operation scripts (`tests/compatibility/ref/ops-NN.json`: sequences of create, write at offset, truncate, rename, delete, set time, set xattr) are replayed by `tests/harness` on a mount point. They never depend on wall-clock time.
2. Run 1: Environment A, Linux `ntfs-3g` (internal FUSE) on a copy of the fixture.
3. Run 2: Environment B, this product on another copy.
4. Both resulting images are dumped in Environment A by `tests/tools/treedump` (links `libntfs-3g`, read-only): path, type, size, allocated size, SHA-256 of the unnamed stream, each named stream with its hash, NTFS times, file attribute flags.
5. The dumps must be equal after removing entries listed in `tests/compatibility/expected-differences.md`. Expected classes: macOS-created metadata (`.fseventsd`, `.Spotlight-V100`, `.DS_Store`, `com.apple.*` streams), access times, and MFT record numbers.
6. Second independent reader: the Linux kernel `ntfs3` driver mounts the macOS-written image read-only and file hashes are compared again [A: `ntfs3` is available in the container host kernel; if not, this step is skipped and reported as skipped].
7. Structural check on every resulting image: `ntfsfix -n` (no-action mode), `ntfsinfo -m`, and a full read of every file. NTFS-3G has no full `fsck`; Windows `chkdsk` in Environment C is the authoritative structural check.

Performance comparison uses the same scripts as `P-*` in both environments on the same physical host and reports ratios, not absolute claims.

## 6. Crash, kill and disconnect testing

| Technique | Environment | What it proves |
|---|---|---|
| Crash-consistency sweep (below) | A | State of the image if the device stops accepting writes after write N, for many N |
| `SIGKILL` of worker during copy | B | Daemon cleanup, journal, read-only on next mount, no hang of applications beyond `daemon_timeout` |
| `SIGKILL` of `ntfsmacd` | B | Open files keep working; restart reconciliation |
| Kill of the macFUSE FSModule process | B | Volume failure mode and cleanup [V:S7 describes the mode] |
| Force-detach of the disk image during write (`hdiutil detach -force` in guest, or removing the virtual disk) | B | Approximation of unplug |
| Real unplug during write | C | The real thing |
| Guest hard power-off during write (`tart stop` without shutdown) | B | Host-panic approximation; journal survives; next boot mounts read-only |

After every scenario the checks are the same: (1) the next attach mounts read-only with the documented reason; (2) every file that had been synced before the event (the test records these) has the right hash; (3) `ntfsfix -n` reports no fatal error; (4) in Environment C, `chkdsk /scan` output is stored; (5) once T-M3-12 has passed, the image carries `VOLUME_IS_DIRTY`.

### Crash-consistency sweep

Patch 0004 adds a device-operations wrapper inside `libntfs-3g`, compiled only with `--enable-faultio`, controlled by the environment variable `NTFSMAC_FAULT_PLAN`:

| Plan | Effect |
|---|---|
| `cut:N` | Writes 1..N succeed; every later write is silently dropped and reports success (power loss with volatile cache) |
| `eio:N` | Write N returns `EIO`; later writes succeed |
| `dead:N` | Write N and everything after returns `EIO` (unplug) |
| `short:N:K` | Write N writes only K bytes |
| `count` | No fault; prints the number of device writes at exit |

Sweep: for each of 5 workloads (large sequential write; many small files; rename-heavy; truncate-heavy; directory growth past one index block), get the write count W with `count`, then run with `cut:N` for at least 100 values of N spread over 1..W. After each run, without the fault layer: mount read-only, run the checks above. The release build is verified not to contain the layer (`nm` shows no `faultio` symbol).

A failing cut point is not automatically a project bug: NTFS-3G has no journal, so some windows of metadata inconsistency are inherent. Each failure is classified by Opus as (a) expected non-atomic window, documented in `LIMITATIONS.md`, (b) engine defect, reported upstream, or (c) defect in our patches, release-blocking.

## 7. Fault injection on macOS

| Fault | Method |
|---|---|
| Device `EIO` | The `--enable-faultio` worker build is also used in the VM for `eio:N` and `dead:N`, to observe how errors surface through FSKit to `write`, `close`, `fsync` |
| Slow device | `NTFSMAC_FAULT_PLAN=delay:ms` (same layer) to test `daemon_timeout` and Finder responsiveness |
| Probe hang | Test build of the probe with a sleep; daemon must time out and choose read-only |
| Full volume | Small fixture filled by the test |

## 8. Fuzzing

| Target | Harness | Input | Oracle |
|---|---|---|---|
| `ntfsmac-probe` logic | libFuzzer, in-process, in-memory device operations | Mutated `tiny-*` images | No crash, no sanitizer report, no device write call, verdict is valid JSON |
| `libntfs-3g` mount + directory walk + read first 64 KiB of each file | libFuzzer, in-memory device, `NTFS_MNT_RDONLY` | Mutated `tiny-*` images | No crash or sanitizer report |
| Control socket parser | libFuzzer | Byte strings | No crash |
| Probe-verdict parser and label sanitiser (`src/sanitize/`) | libFuzzer (`F-SANITIZE-*`, `tests/fuzz/sanitize/`) | Byte strings, seeded with real probe output | Section 4.1 oracles |
| Read/write operation sequences on a valid image | Custom generator replaying random operation scripts through `libntfs-3g` under ASan, comparing with an in-memory model | Seeds | Model equality, no sanitizer report |

Build: clang, `-fsanitize=fuzzer,address,undefined`, Environment A only. Mutation: generic byte mutation plus a custom mutator that re-computes multi-sector fix-ups so that mutated MFT records pass the first validity check and reach deeper code. Corpus and crashes live under `tests/fuzz/`; inputs are synthetic only.

Budgets: 5 minutes per target in the local full CI run; 1 CPU-hour per target for the M3 gate; 8 CPU-hours for M5 (local machine, 18 cores available).

Handling of findings: crashes in engine code are expected (two NTFS-3G releases labelled Security Release in 2026 and two further releases with CVE or memory-safety fixes [V:S10]). They are recorded in `docs/SECURITY_FINDINGS.md`, a report for the NTFS-3G maintainers is drafted, and sending it is a human decision. A finding reachable through a read-only mount of a crafted volume raises risk R-07 and is reviewed by Opus.

## 9. Continuous integration (local only)

There is no GitHub-hosted CI: no GitHub Actions workflows, no hosted runners and no binaries uploaded to GitHub (ADR-015). All checks run on the developer Mac through `scripts/ci-local.sh`, which wraps `scripts/test-all.sh`.

| Stage | Where | Content | Blocking |
|---|---|---|---|
| `linux-build-test` | Environment A (local container) | Fetch pinned deps, build NTFS-3G and project (release, ASan, UBSan), unit tests (daemon-logic unit tests also under ThreadSanitizer, `U-TSAN-*`), fixture generation, Linux reference suite with FUSE, probe tests on all fixtures | Yes |
| `linux-crash-sweep-short` | Environment A | 50 cut points per workload | Yes |
| `linux-fuzz-short` | Environment A | 5 minutes per target, corpus kept locally | Yes on crash in project code |
| `static-analysis` | Environment A | `clang-tidy`, `cppcheck`, `shellcheck`, `-Wall -Wextra -Werror` on project code | Yes |
| `macos-build` | Host (build only, never a mount) | Build project code and NTFS-3G against macFUSE headers and libraries extracted from the verified package without installing it [A: extraction with `pkgutil --expand-full` gives usable headers and dylibs]; unit tests; unsigned package build | Yes |
| `macos-vm` | Environment B | Mount, integrity, stress, soak, performance and Finder tests | Yes for the stages a milestone gate names |

When to run: `ci-local.sh --quick` before every push, from the day T-M1-07 merges (it starts as bootstrap check, configure, build and `ctest`, and gains static analysis and the Linux stages as their tasks land; T-M5-08 completes it); the full run before each milestone gate and before a release. The result file for each run is committed under `tests/results/` (section 10), so every gate and release points at a recorded local run of the tested commit (section 10 defines the exact rule).

No test in the `macos-build` stage may attach or mount a disk image; that stage only builds and runs unit tests on the host.

Build outputs, packages and release candidates stay on the local machine under `build/` and `dist/` (both git-ignored). Publishing binaries anywhere is a later, explicit human decision.

## 10. Recording results

| Item | Rule |
|---|---|
| Result file | `tests/results/<UTC date>-<short sha>-<env>.json`, produced by `tests/harness/report.py` |
| Schema | `{ schema, git_sha, dirty_tree, env, host_os, guest_os, macfuse_version, ntfs3g_version, started, finished, cases: [ { id, status: pass/fail/skip/error, duration_s, evidence: [paths], note } ] }` |
| Dirty tree | A result from an uncommitted tree is marked `dirty_tree: true` and cannot satisfy a milestone gate |
| "Result for the exact commit" | A result file cannot contain the SHA of the commit that adds it. `git_sha` is the commit that was tested (HEAD at the start of the run, clean tree). A gate or release commit G is covered by a result file when `git_sha` is G or an ancestor of G and `git diff --name-only <git_sha> G` lists only paths under `tests/results/`, `CHECKPOINT.md`, `NEXT_ACTIONS.md` and the generated README status table. `scripts/ci-local.sh --verify-result <file>` performs this check (T-M5-08); the milestone audit repeats it. |
| Skips | A skipped case needs a `note`; skips are listed in the milestone audit |
| Evidence | Logs and small artifacts under `tests/results/evidence/<run>/`; images are not stored, only their hashes |
| Status table | `README.md` section generated from the newest result per environment: each feature is `Verified (VM)`, `Verified (hardware)`, `Linux only`, or `Untested` |
| Hardware runs | `tests/results/hardware/<date>.md`, written from the protocol, with device serial numbers and the human's confirmation |
| Flaky tests | A test that fails once and passes on retry is recorded as `fail` with a note, and gets a task; no silent retries |

## 11. Environment C protocol (summary)

Full steps are written in T-M5-10. Minimum scenarios, each on one flash drive and one SSD or HDD, each followed by Windows `chkdsk <drive>: /scan` with stored output:

| # | Scenario | Pass condition |
|---|---|---|
| H-USB-01 | Format on Windows, plug into the Mac, automount read/write, copy `tree-1g` in, eject, verify on Windows | Hashes equal on Windows; `chkdsk` clean |
| H-USB-02 | Files written on Windows, read on the Mac | Hashes equal |
| H-USB-03 | Unplug while idle (mounted read/write) | Next mount read-only on the Mac; Windows offers a scan; `chkdsk` finds no lost data |
| H-USB-04 | Unplug during a large copy | Next mount read-only; files completed and synced before the copy are intact; `chkdsk` repairs at most the file in flight |
| H-USB-05 | Volume from a hibernated Windows and from a Fast Startup shutdown | Read-only on the Mac, reason shown; Windows resumes normally afterwards |
| H-USB-06 | Sleep and wake the Mac with the volume mounted | Volume usable; hashes equal |
| H-USB-07 | BitLocker volume | Not mounted; clear message |
| H-FND-01 | Finder: drag-copy a folder, rename, move to Trash, empty Trash, eject button | All succeed; Windows sees the same tree |
| H-USB-08 | After H-USB-03: record all `$Volume` flags and `$LogFile` `current_lsn` (Linux `ntfsinfo` on the Mac side, read-only), run Windows `chkdsk /f`, record both again, re-attach to the Mac | Flag cleared and LSN changed by Windows (resolves the positive half of U12); every flag left set is recorded, and a lone `MODIFIED_BY_CHKDSK` probes `clean` (N-04); with the evidence rule enabled the attach is `ro` and the next `mount` is `rw`; with it disabled, `journal-clear` succeeds |
| H-USB-09 | After a second H-USB-03: plug the drive into Windows and decline "Scan and fix", write one file, eject safely; record flags and `current_lsn` before and after (negative case of U12) | `VOLUME_IS_DIRTY` still set although the LSN may have changed; on the Mac the attach is `ro` (`dirty_flag`) with the evidence rule enabled and disabled. If the flag is cleared, rule 2 stays disabled permanently and the finding goes to the coordinator |
| H-USB-10 | The `vmk-split` image written to a flash drive: run Windows `chkdsk /f`, then re-attach to the Mac (U15) | Recorded: whether `chkdsk` repairs `$MFTMirr` and what it reports; afterwards the probe gives `clean` or `dirty_flag`, never `corrupt` |

No scenario may run on a drive that the human has not named by serial number.
