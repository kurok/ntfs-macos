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
| Disk under test | Plain image file (`mkntfs -F`), mounted with `ntfs-3g` on the file | Image file mounted directly by the worker, or a raw image attached to the guest as a second virtual disk (`tart run --disk`), which the guest sees as a whole `/dev/diskN` [V: T-M2-03, T-M2-05]. Find it by size, never by number (numbers change between boots). macOS automounts it read-only with `ntfs.fs` first; tests run `diskutil unmount` before any open (U21). `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount` inside the guest is an alternative [A] | Physical USB flash drive and USB SSD/HDD, identified by serial number in the approval |
| Rollback | Container is ephemeral | Clone per run; golden image never modified by tests | Re-format with `mkntfs` between scenarios |
| Automation | Full | Full after H3, through the tart guest agent (`tart exec`) | Scripted steps with human hands for plug and unplug |
| Diagnostics collected | stdout/stderr, ASan reports, core files | `log show` for `fskitd`, `io.macfuse`, `ntfsmacd`; worker FUSE debug log; `~/Library/Logs/DiagnosticReports`; `sysdiagnose` on hang | Same as B plus Windows `chkdsk` output |

### What the VM cannot show

| Gap | Why | Covered by |
|---|---|---|
| Real USB insertion and surprise removal | Virtual disks do not hot-unplug like USB. macOS 27 adds USB accessory passthrough to guests [V:S23]; tool support is [U]. | Environment C; in B, a force-detach of the image approximates it |
| Drive write-cache behaviour, `DKIOCSYNCHRONIZE` and `F_FULLFSYNC` honesty | Virtual disk backed by host file cache; `DKIOCSYNCHRONIZE` returning 0 on a virtual disk proves nothing about a drive (T-M3-10) | Environment C |
| Sleep/wake with a mounted volume | Guest sleep is not representative | Environment C |
| Coexistence with Paragon or other NTFS drivers | Not installed in the guest; a dummy bundle tests only the detection logic | Environment C, only if the human chooses (H7) |
| Kernel backend | Kexts do not load in VMs [V:S3] | Not in scope (ADR-001) |
| Whether FSKit extensions work in a guest at all | Resolved: they do (U2, T-M2-02) | Environment B |

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

Generated by `tests/fixtures/gen/unsafe.sh` (T-M3-04) unless the row names another task; `tests/fixtures/manifest.json` holds each fixture's expected probe fields and the exact Linux messages. `ntfs-3g` passes `NTFS_MNT_MAY_RDONLY` whenever `remove_hiberfile` is absent, so a read/write request that the library refuses becomes a read-only mount, never an error exit [V: T-M3-04]; the last column gives that observed outcome for `-o norecover` (read/write request) and `-o ro,norecover`.

The synthetic `$LogFile` restart pages (`logfile_mark --init`, used by `unclean-log`, `log-v2`, `log-init`, `log-bumped`) are fixtures for `libntfs-3g` and the probe only. Their client LSNs point at an all-`0xff` log page, so they are not a Windows-clean baseline: `H-USB-08`, `H-USB-09` and T-M5-10 must use logs written by Windows.

| Name | How it is made | Expected probe `state` | Observed Linux NTFS-3G 2026.9.28 with `norecover` [V: T-M3-04] |
|---|---|---|---|
| `dirty-flag` | `tests/tools/setvolflags` sets `VOLUME_IS_DIRTY` with `ntfs_volume_write_flags` | `dirty_flag` | `ro` and `rw` mount (the engine does not check the flag; the probe is the only guard; U9, T-M3-01) |
| `unclean-log` | `tests/tools/logfile_mark --init --unclean`: clears the "volume is clean" bit in both `$LogFile` restart areas, keeping fix-ups valid | `unclean_log` (`logfile_lsn` `0x0000000000100400`) | `ro` mounts; `rw` request falls back to read-only ("The disk contains an unclean file system", then "Falling back to read-only mount because the NTFS partition is in an unsafe state.") |
| `log-v2` | `logfile_mark --init --v2` sets restart page version 2.0 | `fast_startup` | `ro` mounts; `rw` falls back to read-only ("Metadata kept in Windows cache, refused to mount.", then the fallback line) |
| `log-init` | `logfile_mark --init`: a clean synthetic restart page pair | `clean` (`logfile_lsn` `0x0000000000100400`) | `ro` and `rw` mount |
| `log-bumped` | `log-init`, then `logfile_mark --bump-lsn` (one sequence-number step; still clean); with `log-init` the pair for the evidence rule (ARCHITECTURE.md section 3.5) | `clean` (`logfile_lsn` `0x0000000000180400`) | `ro` and `rw` mount |
| `hiber-lower` | create `/hiberfil.sys`, 8 KiB, first bytes `hibr` | `hibernated` | `ro` mounts; `rw` falls back to read-only ("Windows is hibernated, refused to mount.", then the fallback line) |
| `hiber-upper` | same with `HIBR` | `hibernated` | as `hiber-lower` |
| `hiber-short` | `/hiberfil.sys` of 100 bytes | `hibernated` (`HIBER_SHORT`) | `ro` mounts; `rw` falls back to read-only ("Hibernated non-system partition, refused to mount.") |
| `hiber-wake` | `/hiberfil.sys`, 8 KiB, first bytes `wake` | `hibernated` (`HIBER_WAKE`) | mounts read/write (the engine does not recognise `wake`; the probe is the only guard, ARCHITECTURE.md section 3.1 check 5) |
| `hiber-unknown` | `/hiberfil.sys`, 8 KiB, first bytes `link` (a signature that is neither `hibr`/`HIBR`/`wake`/`WAKE` nor zeros; M3 audit F-05) | `hibernated` (`HIBER_SIGNATURE`) | mounts read/write (the engine tests only `hibr`/`HIBR`; the probe is the only guard, ARCHITECTURE.md section 3.1 check 5) |
| `hiber-case` | `/HiberFil.sys`, 8 KiB, first bytes `hibr` (name differs in case; the engine's lookup is case sensitive) | `hibernated` | mounts read/write (the engine does not find the file; the probe is the only guard, ARCHITECTURE.md section 3.1 check 5) |
| `hiber-subdir` | `/sub/hiberfil.sys`, 8 KiB, first bytes `hibr`, no file in the root | `clean` | mounts (accepted: neither the probe nor the engine looks outside the root, ARCHITECTURE.md section 9) |
| `hiber-inactive` | `/hiberfil.sys`, 8 KiB of zeros (first 4 bytes all zero) | `clean` (`hiberfile` `inactive`) | `ro` and `rw` mount |
| `bitlocker-sig` | 64 MiB of zeros with `-FVE-FS-` at offset 3 | `unsupported` | not NTFS ("NTFS signature is missing.") |
| `truncated` | `clean-64m-4k` cut to 40 percent, below `$MFTMirr` (which `mkntfs` puts at 50 percent) | `corrupt` (`MOUNT_FAILED`) | every mount fails ("Failed to read $MFTMirr: Input/output error") |
| `truncated-60` | `clean-64m-4k` cut to 60 percent (the earlier recipe of this row); kept to document that a truncated volume can still mount | `corrupt` (`MOUNT_FAILED`), from the device-size part of check 2 (ARCHITECTURE.md section 3.1); the manifest marks it expected to fail until T-M3-02 lands that check | `ro` and `rw` mount: all metadata lies below 60 percent |
| `not-ntfs` | exFAT image | `unsupported` | not NTFS |
| `chkdsk-flag` | `setvolflags` sets only `VOLUME_MODIFIED_BY_CHKDSK` (0x8000) | `clean` (informational flag, ARCHITECTURE.md section 3.1 check 3) | mounts (the engine does not check flags) |
| `dirty-chkdsk-flag` | `VOLUME_IS_DIRTY` and `VOLUME_MODIFIED_BY_CHKDSK` | `dirty_flag`; `repair --clear-dirty` allowed | mounts |
| `txf-nonresident` | `tests/tools/txf_make` makes the root directory's `$TXF_DATA` non-resident | `clean` | read/write mount runs `fix_txf_data()` and makes it resident (`volume.c`); `FORENSIC` and `RDONLY` mounts do not |
| `vmk-split` | Made by `tests/fixtures/gen/vmk.sh` (T-M3-12): `clean-64m-4k` after `volmark set` cut between the `$MFT` and `$MFTMirr` writes of record 3 (fault plan `cut:1`); difference: the USN and bit 0 of the `$VOLUME_INFORMATION` flags, `$MFT` one USN ahead (`tests/results/evidence/m312/final/vmk-split-record3.txt`). Made by the probe's `inner.sh` (stage `fixtures-vmk`); manifest entry present | `mirror_split` (`MFTMIRR_STALE`, with `FLAG_DIRTY` because the overlay view reads the `$MFT` copy, which has the bit set; rule M4 k = 3 of ARCHITECTURE.md section 3.7) | every mount fails ("$MFTMirr does not match $MFT (record 3)"); used for U15 |
| `vmk-split-clear` | Made by `tests/fixtures/gen/vmk.sh`: `clean-64m-4k` after a complete `volmark set`, then `volmark clear` cut between the two record-3 writes (fault plan `cut:1`); `$MFT` one USN ahead with `VOLUME_IS_DIRTY` clear, `$MFTMirr` still set (the reverse direction) | `mirror_split` (`MFTMIRR_STALE`) | every mount fails ("$MFTMirr does not match $MFT (record 3)") |
| `mirror-split` | Made by T-M3-02 with the crash-sweep tools (`tests/fixtures/gen/mirror.sh`): workload `smallfiles` on a fresh `clean-64m-4k` with the fault build and plan `cut:N`, N chosen from the write trace so that write N is `$MFT` record 0 and write N+1 `$MFTMirr` record 0 (N = 68 for NTFS-3G 2026.9.28); accepted only if the sweep's `mirror_split` rule classifies it. Same content as the triage image `smallfiles-c68.img.gz` | `mirror_split` (`MFTMIRR_STALE`) | every mount fails ("$MFTMirr does not match $MFT (record 0)"); used for U15, `H-USB-10` and the repair tests |
| `mirror-newer` | As `mirror-split`, but plan `skip:N` with the cut after write N+1: the mirror write lands, the `$MFT` write does not | `corrupt` (rule M5: mirror newer) | every mount fails (mirror compare) |
| `mirror-torn` | As `mirror-split`, plan `short:N:512` on the `$MFT` record 0 write | `corrupt` (rule M2: torn `$MFT` record) | every mount fails ("Incomplete multi sector transfer") |
| `mirror-extra` | `mirror-split` with one more byte changed in the mirror's record 0 outside the fields rule M4 allows, fix-up recomputed (test helper of T-M3-02) | `corrupt` (rule M4) | every mount fails (mirror compare) |
| `names-nfd` | Made by T-M3-02 (`tests/fixtures/gen/names.sh`) in Environment A, which stores names as given: on `clean-64m-4k`, `café` in NFC and in NFD (different suffixes) in the root, one NFD-only name in a subdirectory, an NFC/NFD pair of directory names, and 20 more NFD names (22 in all, so the 16-path cap applies) | `names_not_nfc` (`NAMES_NOT_NFC`, `nonnfc_count` 22, 16 paths) | `ro` and `rw` mount; each file is reachable by its own bytes (Linux does not convert) |
| `names-nfc` | As `names-nfd`, with every name in NFC, plus ASCII, emoji, CJK and Hangul names in NFC | `clean` (`nonnfc_count` 0) | `ro` and `rw` mount |

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
| Unicode and special names | `I-NAME-01..06`, `I-NAME-08`, `I-NAME-U22` | NFC/NFD pairs (01), emoji, CJK, RTL and accented names across a remount (02), 255 UTF-16 units and names over 255 UTF-8 bytes (03), names refused by `windows_names` (04) and the control without it (05), case-only pairs (06), default against `nonfconv` worker and debug log (08); U22: pairs that differ only in normalisation written from Environment A, then listed and looked up by both forms in B (`I-NAME-U22`, env-host case run A, B, A by `run-env-b.sh --orchestrate`) | B, A* | T-M3-05 |
| Names that are not NFC | `U-PROBE-NAMES-01..03`, `U-POL-NAMES` | probe check 5a finds them, bounded path list, read-only policy (ARCHITECTURE.md section 3.1, ADR-023) | A, H | T-M3-02 |
| Small and large files | `I-BIG-01..03` (`tests/integration/special/i_big.py`, helpers `biglib.py`) | 0 B, 1 B, cluster size +/- 1, 1 MiB +/- 1, 64 MiB + 1, 128 MiB (01); 4 GiB + 1 (02) and 5 GiB (03): write, `pread` around 2^31 and 2^32, updates, truncate, hash after remount. `I-BIG-02` and `-03` need about 7 GiB free in the guest; `run-env-b.sh` grows the clone's disk to 64 GB (`NTFSMAC_B_DISK_GB`), and on a smaller disk they skip with a note | B, A* (01) | T-M3-09 |
| Compressed files | `I-BIG-COMP-01` (`tests/integration/special/comp_a_after_b.py`) | compressed directory and files made on Linux, read and updated in B (with and without `nocompression`), read back on Linux; host case run A, B, A by `run-env-b.sh --orchestrate --cases I-BIG-COMP-01` | A+B | T-M3-09 |
| Sparse files | `I-BIG-SPARSE-01..02` | hole at start, middle, end, all hole; updates into holes; write past EOF without `ftruncate` (dense in B, N22); `SEEK_DATA`/`SEEK_HOLE` and `F_PUNCHHOLE` recorded, not asserted (01); 6 GiB sparse file with data across 4 GiB (02); hash and allocated size (`st_blocks`, on-disk value by `ntfsinfo -F`) | B, A* | T-M3-09 |
| Metadata and timestamps | `I-META-01..07` (`tests/integration/metadata/i_meta.py`, helpers `metalib.py`) | times of new entries against the on-disk `$STANDARD_INFORMATION` and `$FILE_NAME` (01); explicit times, range and precision (02); atime and mount options (03); creation-time APIs, read and set with `system.ntfs_times` (04); effects of operations, incl. `link()` (05); READONLY attribute and `chflags uchg` (06); HIDDEN, `chflags hidden`, `hide_*` and `show_sys_files` (07). On-disk values are decoded from the MFT of the unmounted image. `I-META-01` and `-04` pin the raw-seconds creation time (`SI_CRTIME_IS_RAW_SECONDS`) until patch 0008 (T-M3-22) flips them to the real time | B, A* | T-M3-06, T-M3-22 |
| xattrs and named streams | `I-XATTR-01..09`, `I-XATTR-A1` (`tests/integration/xattr/`) | set/get/list/remove with `streams_interface=openxattr` (01); value sizes up to 262143 bytes (02); `com.apple.FinderInfo` colours and the all-zero value that reads as `ENOATTR` (03); quarantine and resource fork (04); names with `:` under `windows_names` (05); `ditto`, `cp -p`, `cp -c`, `copyfile(3)` both ways (06); data operations keep xattrs (07); `streams_interface=xattr` for comparison (08); streams read by Linux, A then B then A (09); Linux reference (A1). `I-XATTR-09` runs with `run-env-b.sh --orchestrate`, the others with the plain call. `I-XATTR-05` and the `f2` branch of `-06` expect refusal until patch 0009 (T-M3-23) flips them to stored | B, A | T-M3-08, T-M3-23 |
| Concurrent access | `S-PAR-*` | 16 writers, readers during writes, rename storms | B | T-M5-02 |
| **Integrity** | | | | |
| Hash before and after remount | `I-INT-01..03` | 100 random files; `tree-1g` full copy in and out; negative control; the root `.fseventsd` that macOS creates on every mount is not test data and is skipped | B | T-M2-08 |
| Repeated write/read cycles | `I-INT-CYCLE-*` | 200 overwrite cycles on 50 files | B | T-M3-05 |
| Interrupted writes | `S-CRASH-*` (Linux sweep), `S-KILL-*` | cut after write N; kill worker mid-copy | A, B | T-M3-14, T-M5-05 |
| Forced process termination | `S-KILL-*`, `I-DMN-14..15` | `SIGKILL` worker, daemon, macFUSE module process | B | T-M4-05, T-M4-11, T-M5-05 |
| Unclean unmount | `S-DISC-*`, `I-DMN-16` | force-detach image during write; next mount is read-only | B, C | T-M4-11, T-M5-04 |
| Dirty and hibernated refusal | `U-PROBE-*`, `I-DMN-02..08` | every image in 3.2: probe verdict, daemon mode, image hash unchanged after a read-only session | A, B | T-M3-02, T-M4-11 |
| Stale mirror | `U-PROBE-MIRROR-01..05`, `U-VMK-11..14`, `I-DMN-27`, `H-USB-10` | `mirror_split` only under the strict rule; refused by policy; `repair --sync-mirror` gates, writes and refusals (section 4.1; ARCHITECTURE.md section 3.7, ADR-021) | A, B, C | T-M3-02, T-M4-16, T-M5-10 |
| Cross-platform interoperability | `C-REF-*`, `H-WIN-*` | Linux reads macOS-written image and the reverse (`C-REF-01`: section 4.2); Windows `chkdsk` clean | A+B, C | T-M2-09, T-M3-15, T-M5-10 |
| **Reliability** | | | | |
| Long-running workload | `S-SOAK-01` | 6 h mixed workload | B | T-M5-02 |
| Parallel operations | `S-PAR-01` | 16 processes, 30 min | B | T-M5-02 |
| Memory leaks | `S-LEAK-01`, ASan runs | `leaks` on worker; ASan suite on Linux | B, A | T-M5-03 |
| Device disconnection | `S-DISC-*`, `H-USB-*` | image force-detach; real unplug | B, C | T-M5-04, T-M5-10 |
| Low disk space | `S-NOSPC-01` | fill to `ENOSPC`, verify earlier files | B, A* | T-M5-06 |
| Corruption detection | `U-PROBE-*`, `F-*` | truncated and mutated images never crash the probe or the daemon | A | T-M3-16 |
| Recoverability | `S-CRASH-*`, `I-DMN-14..16` | after each failure: read-only mount works (or the point is a classified `mirror_split`, section 6), synced data intact | A, B | T-M3-14, T-M4-11 |
| **Performance** | | | | |
| Sequential read/write | `P-SEQ-*` | 1 GiB, 1 MiB blocks | B, A* | T-M5-07 |
| Random read/write | `P-RND-*` | 4 KiB blocks over a 1 GiB file | B, A* | T-M5-07 |
| Small files | `P-SMALL-*` | create, stat, read, delete 10,000 files of 4 KiB | B, A* | T-M5-07 |
| Directory enumeration | `P-DIR-*` | list 10,000 and 100,000 entries | B, A* | T-M5-07 |
| CPU and memory | `P-RES-*` | worker and FSModule CPU seconds, peak RSS per benchmark | B | T-M5-07 |
| **Safety and security** | | | | |
| Policy and options | `U-POL-*`, `U-ARGV-06` | every policy row; banned options unreachable; argv validator against the measured option classes | H, A | T-M3-03, T-M3-19 |
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
| Permissions and ownership | `I-PERM-01..07` | uid/gid, modes, `chmod`/`chown` outcomes, execute bit (N9, refuted), opt-in mapping smoke, another local account (N10, N16) (section 4.1) | B, A* | T-M3-20 |
| Races in daemon logic | `U-TSAN-01..02` | state machine, control handler, journal and session under ThreadSanitizer with concurrent drivers (section 4.1) | H, A | T-M4-15 |
| Read path edge (macFUSE mitigation) | `I-READ-01..07` (`tests/integration/readpath/i_read.py`, helpers `readlib.py`) | no `EIO` at, across or past EOF: `pread` grid of sizes and offsets up to 2^40, cached and `F_NOCACHE` (01); truncate while open (02) and by another descriptor (03); resident and non-resident transitions (04); FUSE debug log: no request at or past EOF, no empty reply (05, B only); `readv`, `mmap`, `cat`, `dd`, `cp`, `head`, `tail` (06); 512 B and 64 KiB clusters (07) | B, A* | T-M3-07 |
| mmap | `I-MMAP-01..07` (`tests/integration/mmap/i_mmap.py`, helpers `mmaplib.py`, `mmaptool.c`) | mapped read coherence (01); mapped write with `msync` `MS_SYNC` and `MS_ASYNC` (02); no `msync`, `munmap`, `close`, idle, unmount (03); `umount`, `diskutil unmount force`, `umount -f` with a live mapping (04); extension, past-EOF and shrink under a mapping (05); one file per flush trigger, from the FUSE debug log (06); worker `kill -9` with dirty mapped pages, outcome recorded (07) | B | T-M3-11 |
| Sync semantics | `I-SYNC-01..02` | opcode trace; device flush at each sync point: upstream `F_FULLFSYNC` and its fallback (T-M3-10), then `DKIOCSYNCHRONIZE` on a device node with patch 0007 (T-M3-21); raw against buffered node; `sync` option cost (section 4.1) | B | T-M2-06, T-M3-10, T-M3-21 |
| Packaging | `I-PKG-*` | install, doctor, uninstall, manifest equality | B | T-M4-10, T-M6-01 |
| Finder | `I-FND-*` | visible, copy in and out, eject | B, C | T-M4-11 |

`I-DMN-*` ranges: 01..08 automount per fixture class (02..08 are the unsafe classes of section 3.2), 09 privilege, 10..13 unmount paths, 14..16 kill worker, kill daemon, force-detach, 17 passive mode, 18 hostile labels, 19..21 journal clear, evidence rule, repair, 22 marker, 23 second line, 24 session channel, 25 device binding, 26 mount-point handling, 27 stale-mirror repair. Each implementing task writes the tests of its own range under its own file prefix in `tests/integration/daemon/` (T-M4-01: 09, 26; T-M4-03: 10..13, 18; T-M4-05: 15; T-M4-07: 17; T-M4-14: 19..22, 25; T-M4-13: 24; T-M4-16: 27); T-M4-11 writes the rest (01..08, 14, 16, 23) and runs the whole range; T-M5-05 repeats 14..16 twenty times.

### 4.1 Enumerated cases for disk strings, journal clearing, marker, error channel, permissions and races

These ids come from the M1 audit fixes (ADR-016, ADR-017, ADR-006, ADR-018, ADR-012) and the M1 re-check fixes (ADR-014, ADR-019, ADR-020).

| Id | Case | Pass condition |
|---|---|---|
| `U-LABEL-01` | Every ASCII byte `0x00`-`0x7F` as a one-byte label | Only the ADR-016 allow-list survives; all others become `_`; no output byte in `, = \ / :` or below `0x20` |
| `U-LABEL-02` | `X,remove_hiberfile`, `X,allow_other`, `X=1`, `a\,b` | No `,` `=` `\` in output |
| `U-LABEL-03` | `.`, `..`, `../../etc/x`, `.hidden`, ` . `, trailing dots and spaces | Never `.` or `..`; no leading `.`; no `/`; no leading or trailing space or dot |
| `U-LABEL-04` | Invalid UTF-8: overlong `/` (`C0 AF`), lone continuation bytes, surrogates, 5-byte forms | Whole label replaced by the fallback |
| `U-LABEL-05` | C1 controls, bidi and format characters, default ignorables, spaces other than U+0020, Hangul fillers, U+2028/2029, private use, noncharacters, code points unassigned in Unicode 15.1.0, slash and colon look-alikes; every code point of Unicode against an independent oracle | Each replaced by `_`; a combining mark at the start of a part becomes `_` too |
| `U-LABEL-06` | 128 UTF-16 units of 1-, 2-, 3- and 4-byte characters | Output at most 64 bytes, valid UTF-8, cut on a code-point boundary |
| `U-LABEL-07` | Empty, all spaces, invalid UTF-8, all dots, all disallowed (nothing allowed survives) | `NTFS-<last 8 hex digits of serial>` |
| `U-LABEL-08` | Collision sequence with existing `DATA`, `DATA 2`..`DATA 99` in a temporary directory standing in for `/Volumes` | Next free name chosen with `mkdirat`; after 99, refusal; a pre-existing symlink named `DATA` is never followed; an NFC name with an existing NFD directory of the same name (`e` U+0065 U+0301 against U+00E9) gets the suffix, never the existing directory |
| `U-ARGV-01` | Builder output for every policy row and option set, including the opt-in `permissions` and `usermapping=/usr/local/etc/ntfs-macos/UserMapping` rows | `argv_check()` accepts |
| `U-ARGV-02` | Every single-token mutation of a valid vector: add, drop, duplicate, reorder argv elements; insert each banned key; drop `norecover`; add or drop `ro`; change `uid`, `fmask`, `streams_interface`; add `volname`, `big_writes` or `rdonly`; add `permissions` next to `uid` | `argv_check()` rejects each |
| `U-ARGV-03` | `ro` decision with an `O_RDWR` descriptor, and the reverse | Rejected (rule 7) |
| `U-ARGV-04` | Option string with `\`, NUL, control byte, empty token, value containing `=`, length 1025; `usermapping=` with a path containing a space or any path other than the fixed one | Rejected |
| `U-ARGV-05` | Mount point outside `/Volumes`, with `/` in the leaf, or not equal to the created leaf | Rejected |
| `U-ARGV-06` | Every option of `integration/mount-options.json` (T-M2-04) as a single-token addition to a valid vector | Keys of `always` and `opt_in` accepted only with their grammar; every key under `ignored`, `rejected`, `banned` and `not_rejected_by_backend` rejected; `streams_interface` accepted only as `openxattr` or `none` |
| `U-VERDICT-01` | Probe output over 64 KiB, truncated JSON, trailing garbage | State `unknown` |
| `U-VERDICT-02` | Duplicate keys, unknown keys, unknown `state` or `logfile` enum, wrong types | State `unknown` |
| `U-VERDICT-03` | `\u0000` or invalid UTF-8 inside `label` | State `unknown` |
| `U-VERDICT-04` | Valid output for every fixture in section 3.2 | Parsed fields equal the probe's |
| `U-VERDICT-05` | Hostile values for every field of the ARCHITECTURE.md section 3.1 grammar: `serial` as `../../../Library/LaunchDaemons/x`, `0x1a2b...` (lower case), 15 or 17 hex digits, with NUL, `/`, newline or ESC; `logfile_lsn` likewise; integers with sign, fraction, exponent, leading zero or out of range; a `reasons` key with free text; `reason_codes` with an unknown code, a duplicate, 19 entries, or text containing ESC `[`; `state` `clean` with a reason code; `nonnfc_paths` with 17 entries, a 513-byte path, or a count that does not match the paths or the `NAMES_NOT_NFC` code | State `unknown` for each; for every accepted verdict the journal path the daemon would build is `<journal dir>/<16 upper-case hex>` and no output text contains a byte the probe sent other than through the label sanitiser or the path display sanitiser of ADR-023 (a path with ESC or a bidirectional control is shown escaped) |
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
| `U-VMK-03` | `set` on `unclean-log`, `log-v2`, `hiber-lower`, `hiber-upper`, `hiber-short` (and on `hiber-wake`, `hiber-unknown` and `hiber-case`: exit 0 is the expected result, because volmark applies the engine's rules, which do not recognise a `wake` header and look the name up case-sensitively; T-M3-04 saw both mount read/write on Linux; the probe is what refuses them, `U-PROBE-*`) | Exit 10 (volmark's own checks through the public API; for a read/write request the driver would fall back to read-only on the same findings, T-M3-04); image hash unchanged |
| `U-VMK-04` | `clear` after `set` | Exit 0; record identical to before `set` except the update sequence number and the sector tails that carry it |
| `U-VMK-05` | `clear` on a volume without the flag | Exit 13; image hash unchanged |
| `U-VMK-06` | `clear-repair` on `dirty-flag` and `dirty-chkdsk-flag`, and on a volume with an extra non-informational flag | Exit 0, `VOLUME_IS_DIRTY` clear and 0x8000 unchanged; exit 13 and image unchanged |
| `U-VMK-07` | `SIGKILL` before step 4 and a fault-layer cut (`count`, then `cut:N` and `short:N:K` over every write) inside step 4, for `set` and `clear` | Each outcome equals its row in the crash table of ARCHITECTURE.md section 3.6: unchanged, or flag value new on both copies (probe `dirty_flag` or `clean`), or `$MFT`/`$MFTMirr` record 3 torn or different (probe `corrupt`); in every case no byte outside the two copies of record 3 changed; never a probe `clean` with the flag lost after `set` reported exit 0 |
| `U-VMK-08` | Read-back mismatch injected with `short:N:K` on the flag write | Exit 14 |
| `U-VMK-09` | `set` with `--expect-serial` or `--expect-sectors` different from the image | Exit 15; image hash unchanged |
| `U-VMK-10` | `set` and `clear` on `txf-nonresident`, and `set` on a volume with a bit outside `VOLUME_FLAGS_MASK` or with any flag but the informational `MODIFIED_BY_CHKDSK` and `MOUNTED_ON_NT4` (`CHKDSK_UNDERWAY`, `RESIZE_LOG_FILE`, `UPGRADE_ON_MOUNT`, `DELETE_USN_UNDERWAY`, `REPAIR_OBJECT_ID`) | First: exit 0, `$TXF_DATA` still non-resident, fault layer `count` shows exactly the two record-3 writes; second: exit 11, image unchanged |
| `I-ERR-01` | Linux reference mount with the faultio build, `eio:N` on a data write | `S rw` first; exactly one `E pwrite 5` line; last line `U 1` (or `E umount` if the unmount failed) |
| `I-ERR-02` | `eio` on the device sync, and on the close-path `fsync` | One `E sync <errno>`, one `E close <errno>` line; no `U 0` |
| `I-ERR-03` | No fault, full functional workload, clean unmount | Exactly `S rw` then `U 0`; `NTFSMAC_ERRFD` unset behaves exactly like the unpatched build |
| `I-ERR-04` | `ntfs_umount` made to fail (fault layer `dead:N` from the first unmount-time write) | `E` records then `E umount <errno>`; no `U` line; worker exit status 0 (shows exit status is not evidence) |
| `I-ERR-05` | Read-only mount (`-o ro`), and a `norecover` read/write request on `unclean-log`, which the engine turns into a read-only mount ("Falling back to read-only mount ...", T-M3-01 in A and B, T-M3-04 in A); both in Environment A and with the macOS worker in B (`backend=fskit,local,norecover`) | `S ro` first; in B the mount has `MNT_RDONLY` and the image hash is unchanged |
| `I-ERR-06` | macOS VM: the drive flush forced to fail at each sync point, on a buffered device node (`DKIOCSYNCHRONIZE`, patch 0007) and on an image file (`F_FULLFSYNC`); and the same session on a device node without a forced failure | Forced: `W flush <errno>` line at each sync point that reached the device; sync still returns 0. Not forced, device node: no `W` line |
| `I-ERR-07` | Inspect the worker's descriptor table and a child it starts (test hook `system("true")` in the faultio build) | `NTFSMAC_ERRFD` has `FD_CLOEXEC`; the child has no copy of it; the write end is non-blocking |
| `I-PERM-01` | In the VM, create files and directories with `touch`, `mkdir` and `cp`, and stat files made on Linux before the run | Every entry reports the console uid and gid |
| `I-PERM-02` | Same tree | Files mode 0644 and directories 0755, whatever the Windows ACLs of the fixture; the Linux tree dump of the result shows no ACL change |
| `I-PERM-03` | `chmod` 0600, 0755, `+x`, 0777, 0000, 4755 on a file and a directory | Recorded outcome: success with no effect, identical on three runs; on re-stat and after a remount the mode is 0644 or 0755; `ntfsinfo -v` of the entry unchanged (NTFS attributes, times, security descriptor) |
| `I-PERM-04` | `chown` to another uid and to root (as root inside the VM) | Recorded outcome: success with no effect, identical on three runs; ownership on re-stat and after a remount is still the console user; `ntfsinfo -v` unchanged |
| `I-PERM-05` | A file with a `#!/bin/sh` header, a copy of `/usr/bin/true` and a binary compiled in the guest, mode 0644, `chmod +x`, execute | B: runs (the bit is not checked; limitation N9), also after a remount and on a read-only mount; `chmod +x` succeeds and changes nothing. A: `EACCES`. Directories remain traversable |
| `I-PERM-06` | Opt-in `permissions` (without `uid`, `gid`, `fmask`, `dmask`, which make it a silent no-op, T-M2-04) with an on-volume `.NTFS-3G/UserMapping` file, and `usermapping=/usr/local/etc/ntfs-macos/UserMapping` read through the worker sandbox, in Environment A (reference) and B (mount accepted by the builder's test-only switch) | Mount succeeds, one file created and read back with equal hash; no data claim beyond that (ADR-012). The implicit line of a mapping file (`::SID`) needs a last component of 1000 or more; a lower one is dropped with `Bad implicit SID pattern` |
| `I-PERM-07` | B only. A second, non-admin local account reads, writes, lists, renames and deletes files on a mount owned by the console user, including a mode-000 file (N10); a second pass on a `permissions` mount with real modes 0600, 000 and a 0700 directory; a create on a read-only mount | Outcome recorded per operation; the limitation text of N10 and N16 matches it (every operation succeeds on both mounts; `EROFS` on the read-only mount) |
| `U-DMN-SM-01` | Volume state machine (`src/daemon/volume.c`): one test per transition of ARCHITECTURE.md section 5 with the events of sections 4.1 and 4.5, and an exhaustive check over every state, event and disk-present pair that the table holds these transitions and no others | Each transition gives the stated next state and record fields; every other pair is refused |
| `U-DMN-SM-02` | Same: invalid transitions (events out of order, any event in `Gone`, `mount` while not `Appeared` or `Failed`), bad arguments, a `ro` decision with a writable mount | Result `INVALID_TRANSITION`, `BAD_ARG` or `MODE_MISMATCH`; the record is unchanged |
| `U-DMN-SM-03` | The stub modules of `src/daemon/` link and return "not implemented"; the stub session watcher never proves a session clean | `NTFSMAC_DMN_E_NOTIMPL`; no clean proof |
| `U-DMN-MP-01..08` | Mount point module (`src/daemon/mountpoint.c`) in a temporary directory: plain and hostile labels (`,`, `/`, `..`, `:`, `\`, NUL, invalid UTF-8, empty, 300 bytes) give the sanitised leaf; mode 0700, owner, device and inode recorded; the base name and ` 2` to ` 99` (99 names) then `Busy`; an existing file, directory or dangling link is never reused or changed; remove refuses a non-empty directory, a swapped directory and a link, and accepts a vanished one; bad arguments, missing, linked or wrongly owned parent | Results as stated; nothing outside the temporary directory is made; no directory is left behind |
| `U-DMN-UM-01..05` | Unmount retry logic (`src/daemon/unmount.c`) with test hooks: first call works; transient `EBUSY` then success after delays; permanent `EBUSY` gives `Busy` with the holder's pid after 1 + `retries` calls (capped at 20); `MNT_FORCE` only when `force` is set; `EINVAL`, `ENOENT`, `EPERM`, `EIO` mapped and not retried; bad paths refused before any call | Call, sleep and lookup counts and results as stated |
| `U-DMN-CFL-01..05` | Conflict detection (`src/daemon/conflict.c`): the pure name rule (Paragon, Tuxera, other `*ntfs*.fs`; not `ntfs.fs`, macFUSE, fuse-t, `ntfsmac*`); the directory scan on temporary directories (stray file, dangling link, link to a bundle, several drivers, missing and unreadable directory); the passive-mode gates (no dissent, no automount, `mount` refused when another driver holds the volume) | Passive only when a bundle directory of another NTFS driver exists; unreadable directory gives passive and `E_IO`; gates as ARCHITECTURE.md section 8.4 |
| `U-DMN-DOC-01..07` | `doctor` checks (`src/daemon/doctor.c`) against temporary directories: version and plist readers (pure); macFUSE missing, too old, newer, unreadable version; extension state from a test hook; conflicting drivers named by fixed code; journal entries, unreadable entries, tombstones, missing and unreadable journal directory (not created); callback stop | One finding per check with the fixed code, `ok` flag and value; no disk-derived string |
| `U-DMN-RC-01` | Restart reconciliation (`src/daemon/reconcile.c`): which live mount belongs to a journal entry (leaf equals the label, or the label plus ` 2` to ` 99`; the fallback leaf for an empty label; nothing outside `/Volumes/<leaf>`) | Match or no match as stated; no overflow on long paths |
| `U-DMN-RC-02` | Same: decision for every marker (`pending`, `set`, `failed`, `none`), every state (`open`, `write_error`) and every kind of live mount (none, other name, read-only, writable with a worker, writable without a worker) | `READOPT` only for a writable mount with a live worker and marker `set` or `none`; `FORCE_UNMOUNT` for every other writable mount; `KEEP_ENTRY` otherwise |
| `U-DMN-RC-03` | Same: ambiguity (two entries with one label, two mounts for one entry, a read-only and a writable mount of one label, a mount claimed by two entries) | A writable mount that cannot be told apart is force-unmounted once and never re-adopted; every entry appears in the plan |
| `U-DMN-RC-04` | Same: bad arguments and too small a plan capacity | Fail closed: no items, `E_INVAL` |
| `U-DMN-RC-05` | Same: run over a real journal in a temporary directory, early stop by the callback, the live lister on macOS | Callbacks as planned; no journal entry is changed, moved or removed |
| `U-DMN-DA-01` | Disk Arbitration filter (`src/daemon/da_filter.c`): `Windows_NTFS` and the GPT basic-data GUID in any case, a whole disk with empty content, partition maps and other types refused | Candidate or not as stated in ARCHITECTURE.md section 4.1 step 1 |
| `U-DMN-DA-02` | BSD name check: `diskN` and `diskNsM` accepted; paths, `rdisk`, signs, empty digits, long digit runs, trailing bytes refused | Only the two forms pass |
| `U-DMN-DA-03` | Name table of the dissent and known-disk sets: add, remove, flag, duplicate add, full table | No entry lost or duplicated; a full table refuses the new name only |
| `U-DMN-DA-04` | Bad arguments to the `ntfsmac_da_*` calls (NULL session, no `disk_appeared`, bad names) | Never `OK`; `E_INVAL` on macOS |
| `U-DMN-EW-01` | Session channel parser (`src/daemon/errwatch.c`): every record kind (`S rw`, `S ro`, each `E` operation, `W flush`, `U 0`, `U n`), the flags each sets, and the clean proof of a good session | Flags as stated; `U n` with n above 0 also sets the `E` flag; the proof holds only for `S rw` ... `U 0` at end-of-file |
| `U-DMN-EW-02` | Same: grammar. 66 malformed or mis-ordered streams (extra, missing or doubled spaces, case, signs, leading zeros, errno 0 and 100000, unknown operations, `S` twice, a first record that is not `S`, `U` first, anything after `U`, empty lines, `\r`) | Each one is a protocol error and never a clean proof; after an error the later bytes are discarded with `E_STATE` |
| `U-DMN-EW-03` | Same: bounds. A 31-byte line is the longest accepted; 32 bytes without a newline, 100 KiB without a newline, every byte outside `0x20..0x7E` (all 256 values, `NUL`, `0x7F`, `0x80`, UTF-8), 64 records good and record 65 an overflow, 100000 valid records, 16 MiB of garbage | Protocol error at the first violation; state stays the fixed struct (line length 0 after an error); no unbounded buffer |
| `U-DMN-EW-04` | Same: the result does not depend on how the bytes are split over reads (every split point and one byte at a time), empty reads, `NULL` with length 0 | Same flags and record count as the whole stream |
| `U-DMN-EW-05` | Same: the proof needs every part: unmount not seen first, re-adopted, exit status 1, -1 and 256, `NULL` watcher, silence, missing `S`, missing `U`, line without a newline, no end-of-file, byte after end-of-file, empty watcher, hand-made flags and record counts the parser never makes | Not clean in each case |
| `U-DMN-EW-06` | Pipe (`src/daemon/errwatch_io.c`): both ends `FD_CLOEXEC`, descriptors 3 or higher also with 0 to 2 closed, read end non-blocking and read-only, write end blocking and write-only | As stated |
| `U-DMN-EW-07` | Reading from a real pipe: a clean stream then EOF; a held write end gives `E_TIMEOUT` at the deadline and no EOF flag, a later EOF is seen by `pump`; a writer that closes late (200 ms) inside the wait; an endless writer (4 KiB writes) ends `drain` at its deadline and the stream is discarded; a closed read descriptor gives `E_IO`; bad arguments | EOF timing as stated in ARCHITECTURE.md section 7 (2 s); never blocks; memory bounded |
| `U-SPN-01..07` | Spawn module (`src/daemon/spawn.c`), probe: a stand-in program checks inside the child that only the device descriptor and descriptors 0 to 2 are open, the child leads its own process group, signals are at their defaults (also when the parent ignores `SIGPIPE`), and reads the device through the descriptor; hung, garbage, non-zero, crashing and oversize (over 64 KiB) probes; missing program; bad arguments; a user the process cannot become; no descriptor left open in the parent | Verdict parsed; every failure gives state `unknown` with `TIMEOUT`, `IO`, `INVAL` or `DENIED`; a hang is killed at its deadline |
| `U-SPN-10..15` | Same, volmark: arguments formatted from integers (16 upper-case hex digits, decimal sectors); exit code passed on; silence until the deadline gives -1 (no write begun); after `w` the first limit no longer applies, the further limit gives -2 (a write may be cut); failed start gives -3; bad arguments | As stated; no kill between `w` and the exit within the further limit |
| `U-SPN-20..29` | Same, worker and supervision: argument vector and environment (`PATH`, `NTFSMAC_ERRFD` only) as in section 3.3, channel record arrives, mount mode reported from the table, a worker that exits before the mount fails at once, a worker that ignores `SIGTERM`, or is stopped with `SIGSTOP`, is killed with its whole process group at the deadline (no worker process left, the grandchild included), the test hook that drops `backend=fskit` after `argv_check()` reaches the child, refusals before any start (`ro` decision with an `O_RDWR` descriptor, mount point outside `/Volumes`, a slash in the leaf, equal channel and device descriptors, missing `usermapping` file), handle checks never signal pid 0 or 1 | As stated |
| `I-DMN-09` (module part), `I-DMN-26` b and c (module part), `U6`, `U20` | `tests/integration/daemon/spawn_vm.sh` in a disposable guest: a root process starts the real worker as the console user with an inherited descriptor (rw and ro), probe and volmark stand-ins start as `nobody`; worker stopped after the mount (30 s of `getfsstat(MNT_NOWAIT)`); worker stopped right after the spawn; `backend=fskit` dropped after `argv_check()` | Worker uid and group, mounts, channel records, deadline kill and no leftover process as stated in the driver; the daemon-level cases (`list` answered during the wait, state `Failed`) stay with T-M4-08 and T-M4-11 |
| `I-DMN-24` a and b (module part), `U16` | `tests/integration/daemon/errwatch_vm.sh` in a disposable guest: the real worker through the pipe, parser and spawn module of the daemon: a rw session with a file write and unmount; the same without `NTFSMAC_ERRFD`; a faultio worker (`eio:N`, N the first device write after the mount); `lsof` on the live worker for the holders of the write end | Clean session: proof holds, only the worker holds the write end, daemon's copy closed; no channel: EOF, no record, not clean; fault: `E` record, not clean; the daemon-level cases (c) to (f), the journal state and the notification stay with T-M4-08, T-M4-11 and T-M4-14 |
| `U-CTL-PARSE-01..05` (05: control code points in string fields) | Control request parser (`src/daemon/ctl_parse.c`, T-M4-04): every command of ARCHITECTURE.md section 3.4 with its fields; malformed JSON (truncated, trailing bytes, bad escapes, lone surrogates, `\u0000`, raw control bytes, invalid UTF-8, embedded NUL); lines over 4096 bytes; unknown command; unknown, duplicate, missing and misplaced fields; wrong value types; bad `bsd`, `mode`, `action`, serial and `auth` values | Valid lines fill the request; every other input gives `BAD_JSON`, `BAD_FIELD`, `UNKNOWN_COMMAND` or `TOO_LONG`, never `OK` |
| `U-CTL-PEER-01..06` | Each peer check of ADR-019 alone with fake system facts (`ctl_peer.c`): gate (console user or `admin`), check 1, check 2 (peer, parent, grandparent and process group spawned; gone and failing lookups; loop; chain limit), check 3 (yes, no, cannot tell, no audit token), check 4 (none, accepted, denied, cannot tell, replay, table of 64), missing hooks, order of the four checks | Each check refuses for its own reason (`not_admin`, `spawned_process`, `client_not_signed`, `no_authorization`, `authorization_replayed`, `authorization_denied`); a fact that cannot be found out refuses (`check_unavailable`); a refusal stops the later checks |
| `U-CTL-HANDLE-01..06` | `ctl_handle_line` with fake operations: every command reaches its operation with the right arguments; a peer that is neither console user nor `admin` reaches nothing; `journal_clear` and `repair` with each of checks 1 to 4 failing alone (operation not called), with all passing (called once, with the typed fields), and with the same form twice; replies are one NUL-terminated JSON line, never echo request bytes; failing, missing and newline-writing operations | As stated; `not_implemented` for a missing operation, after the four checks for the two privileged commands |
| `U-CTL-FUZZ-01` | Small deterministic fuzz loop over `ctl_handle_line` (200,000 inputs in `ctest`, 1,000,000 under `asan`): mutated requests and random bytes, varying every fact the checks read; the input is a heap block of exact length | No crash or sanitizer report; reply shape holds; a privileged operation is reached only when every check passed; accepted requests satisfy the field grammar |
| `U-CTL-SOCK-01..08` (06 refused and silent peers cost almost nothing, 07 a live socket is not taken over, 08 the right definition at start and per request) | Control socket server on a temporary socket (fake peer lookup): start and stop, mode 0660, stale socket replaced, regular file kept; one reply per line, pipelining, split writes, request without newline; oversize line, 8 requests per connection, idle client; unknown and refused peers; the real `LOCAL_PEERCRED`, `LOCAL_PEERPID`, audit token and `proc_pidinfo` lookups on a socket pair | As stated; an idle client holds the server at most 5 s |
| `U-CTL-CLI-01..05` | `ntfsmac` CLI library: text tables cover all 18 reason codes, every refusal and error code; request builder escaping and size limit; safe printing (control bytes, C1, bidirectional controls, invalid UTF-8); rendering of `list`, dumps, refusals and exit codes; the CLI against the real server with fake operations | As stated; no raw control byte in any output |
| `I-CTL-SIG-01` | VM only (`tests/unit/ctl/vm/u17.sh`, part A): the real check 3 against an ad-hoc signed client with the hardened runtime, one without it, and a re-signed copy | `YES` only for the client that satisfies the requirement and has the hardened runtime |
| `I-CTL-AUTHZ-01` | VM only (`u17.sh`, parts B1 and B2): the external form crossing from a user process to root; references that lack the right; the prompt without a GUI session (U17) | See the T-M4-04 result and ARCHITECTURE.md U17. The password step itself (the full `I-DMN-19` path) needs a person at the VM console and belongs to T-M4-14 |
| `U-TSAN-01` | Daemon state machine driven from four threads with random events, built with `-fsanitize=thread` (host and Environment A). `tests/unit/tsan/tsan_statemachine.c` (T-M4-15) models the per-volume serial queue of ARCHITECTURE.md section 5: 8 volumes, one queue and one consumer thread each, 4 producer threads posting random events (and some bad arguments); no lock in `volume.c`, so a shared record or hidden global would be reported. Run: `cmake --preset tsan` then build and `ctest`, or `scripts/ci-local.sh --quick --preset tsan`; it also runs in the normal build as `tsan.statemachine` | No data-race report; after every event the record invariants hold (a refusal changes nothing, Gone accepts nothing, a lost disk stays lost, `Failed` has a reason); a single-thread replay of each volume's events gives the same final record |
| `U-TSAN-02` | Control-protocol handler with concurrent clients plus concurrent journal and session updates (kill at random points). `tests/unit/tsan/tsan_ctl_journal.c` (T-M4-15): the real socket server with 4 clients (pipelined and malformed requests, fresh authorization forms), 3 threads calling `ntfsmac_ctl_handle_line_env()` with one shared replay table, operations that read and clear a real journal while 4 volume threads create, update and remove entries in it; a child process doing the same on one entry is killed with `SIGKILL` at random points while reader threads read. Runs as `tsan.ctl_journal`. The session part is deferred until T-M4-14 (`session.c` is a stub) | No data-race report; journal entries never partial (`get` is whole or absent, `list` skips nothing); one authorization form sent by all threads at once reaches the privileged operation exactly once |
| `I-DMN-23` | Daemon with a probe stub that reports `clean` for `dirty-flag`, `unclean-log`, `log-v2`, `hiber-lower`; marker step enabled | No write reaches the image (hash unchanged); mode reported `ro` with reason; with the marker step disabled, the four outcomes are recorded (expected: `dirty-flag` is written, the others fall back to read-only, and the daemon reports them `ro` with reason from its `MNT_RDONLY` check of section 4.1 step 8 of ARCHITECTURE.md) |
| `I-DMN-24` | (a) VM faultio worker with `eio:N` during a copy; (b) worker started without `NTFSMAC_ERRFD`; (c) unpatched worker binary; (d) worker that writes 100 KiB without newline, and one that writes 65 valid records; (e) daemon killed and restarted mid-session (re-adoption); (f) a child of the worker that keeps the write end open after the worker exits | (a) journal `state=write_error`, notification text, unmount requested; (a) to (f) no "all data written" report, journal entry and marker kept, next attach `ro`; (d) daemon memory stays bounded and the worker never blocks |
| `I-DMN-26` | Mount-point handling: (a) test hook swaps the created leaf for a symlink to another directory between `mkdirat` and `openat`; (b) worker stopped with `SIGSTOP` right after spawn; (c) test hook drops `backend=fskit` from the vector after `argv_check()` (the worker then exits 0, mounts nothing and ignores `SIGTERM`, T-M2-03) | (a) attach fails, the symlink target is not chowned or removed; (b) and (c) the mount wait ends at its 30 s deadline, the worker's process group gets `SIGKILL` and no worker process is left, the volume goes to `Failed`, and the daemon answers `list` for other volumes throughout |
| `I-DMN-25` | (a) Probe runs on image X; before the `O_RDWR` open the test detaches X and attaches image Y under the same BSD name; (b) the disk is mounted read-only by Apple's driver before the `O_RDWR` open; (c) volmark stub that changes sector 0 | (a) and (c): hash or `st_rdev` mismatch, no `O_RDWR` descriptor kept, volume `ro` or `Failed`, image Y unchanged; (b) no `O_RDWR` open, state as ARCHITECTURE.md section 4.1 step 6 |
| `I-SYNC-01` | `tests/integration/sync/i_sync.py` with the `syncspy.c` interpose library (and `dtrace_check.sh`) on the worker during `fsync`, `F_FULLFSYNC`, `sync(2)` and unmount, on an image file and on a buffered device node | T-M3-10 (unpatched): per sync point, the device `F_FULLFSYNC` and whether the `fsync` fallback ran (on a node: always, `ENOTTY`). Extended by T-M3-21 (patch 0007): on the device node each sync point that reaches a dirty device, and the unmount, shows one `fsync` followed by one `ioctl(DKIOCSYNCHRONIZE)` returning 0 and no `F_FULLFSYNC`; the image file still shows one `F_FULLFSYNC`; a clean device shows none |
| `I-SYNC-02` | Same on the raw and the buffered node; alignment table at the bare node; `sync` option benchmark | T-M3-10: raw node fails at mount (`EINVAL`, U10 resolved: buffered); `sync` cost recorded (88 to 94 percent, ADR-014 item 5). Extended by T-M3-21: the daemon-style flush (`fsync`, `DKIOCSYNCHRONIZE`) on the buffered node after the worker exits returns 0; `ntfsfix -n` and a read-only remount after a patched session pass |
| `U-PROBE-NAMES-01` | Probe on `names-nfd` | `names_not_nfc`, `NAMES_NOT_NFC`; `nonnfc_count` 22; 16 paths, each a path of the fixture; image hash unchanged |
| `U-PROBE-NAMES-02` | Probe on `names-nfc`, on every fixture of section 3.2 except `names-nfd`, and on `dirty-flag` with one NFD name added | `nonnfc_count` 0 and no `NAMES_NOT_NFC` for the first two; the third: state `dirty_flag` with `FLAG_DIRTY` and `NAMES_NOT_NFC` |
| `U-PROBE-NAMES-03` | Probe on `names-nfd` with one directory index made unreadable, and with a test-only time budget of 0 | `unknown` (scan not finished), never `clean` |
| `U-POL-NAMES` | Verdicts `names_not_nfc` with and without a journal entry; `mirror_split` with `MFTMIRR_STALE` and `NAMES_NOT_NFC` for the `repair --sync-mirror` gate; `dirty_flag` with `NAMES_NOT_NFC` for the `repair --clear-dirty` gate | `ro` in both columns; both gates open (the code is not a damage finding) |
| `U-PROBE-MIRROR-01` | Probe on `mirror-split`, on `vmk-split` (record 3, flag set in `$MFT`) and on `vmk-split-clear` (an interrupted `clear`: flag set in `$MFTMirr`); negative cases of record 3 on copies of both (extra flag bit, another byte, USN not +1 or equal, `$VOLUME_INFORMATION` missing or truncated, USN-only difference) give `corrupt`; in-memory cases in `test_mftmirr.py` | `mirror_split`, `MFTMIRR_STALE` (plus `FLAG_DIRTY` where the `$MFT` copy has the flag); checks 3 to 5 reported from the overlay view; negatives `corrupt`; image hash unchanged |
| `U-PROBE-MIRROR-02` | Probe on `mirror-newer`, `mirror-torn`, `mirror-extra`, and on `mirror-split` with a record at index 4 or above also changed | `corrupt` (`MOUNT_FAILED`), never `mirror_split`; image hash unchanged |
| `U-PROBE-MIRROR-03` | Probe on `mirror-split` with `VOLUME_IS_DIRTY` set (session with the marker), and with a `hibr` `hiberfil.sys` | `mirror_split` with `FLAG_DIRTY`, respectively `HIBER_SIGNATURE`, in `reason_codes` |
| `U-PROBE-MIRROR-04` | Each rule M1 to M6 of ARCHITECTURE.md section 3.7 negated alone on an in-memory copy (USN equal, USN reversed, size two records ahead, run list changed before the mirror's end, attribute length changed, header field changed) | `corrupt` for each |
| `U-PROBE-MIRROR-05` | The 18 failing images of the full sweep and the 11 of the short sweep (`build/crashsweep-failing/`, regenerated if missing) | `mirror_split` for each |
| `U-VMK-11` | `sync-mirror` on `mirror-split` (and on `mirror-split` with `VOLUME_IS_DIRTY` set) | Exit 0; only the `$MFTMirr` copy of record 0 changed (sector diff), now byte-equal to `$MFT` record 0; `$MFT`, `$LogFile` and the flags unchanged; read-only mount and `ntfsfix -n` succeed; probe `clean` (respectively `dirty_flag`) |
| `U-VMK-12` | `sync-mirror` on `mirror-newer`, `mirror-torn`, `mirror-extra`, `clean-64m-4k`, `hiber-lower` with a split mirror, and with wrong `--expect-serial` | Exit 10, 10, 10, 10, 10, 15; image hash unchanged |
| `U-VMK-13` | Fault-layer cut (`count`, then `cut:N` and `short:N:K` over every write) of `sync-mirror` | Each outcome equals its row in the crash table of ARCHITECTURE.md section 3.7; a rerun after a non-torn cut exits 0 |
| `U-VMK-14` | `sync-mirror` run twice | Second run exits 10 (nothing differs), image unchanged |
| `I-DMN-27` | `repair --sync-mirror` end to end in the VM on `mirror-split`: attach (refused, notification names the repair), request refused for non-`admin`, wrong label or serial, a spawned-process peer and a missing authorization (as `I-DMN-21`), refused on `mirror-newer` and on a `mirror_split` volume with an unclean log; accepted otherwise | Refusals change nothing (image hash); after acceptance the reply carries the fresh probe state, no automatic mount, the journal entry (if any) is kept, and the next attach follows the policy table; `ntfsfix` is never executed (process audit) |
| `U-POL-FLAGS` | Probe verdicts with `volume_flags` `["MODIFIED_BY_CHKDSK"]`, `["MOUNTED_ON_NT4"]`, `["DIRTY","MODIFIED_BY_CHKDSK"]`, `["CHKDSK_UNDERWAY"]`, `["UNKNOWN_BITS"]` | First two: `clean` path, `journal-clear` gate open; third: `repair` gate open; last two: `dirty_flag`, `repair` refused |
| `U-POL-SYNCMIRROR` | `repair --sync-mirror` gate: verdict `mirror_split` with `MFTMIRR_STALE` alone and with `FLAG_DIRTY`; then each condition negated alone (another state, an extra reason code such as `LOG_UNCLEAN` or `HIBER_SIGNATURE`, `logfile` not `clean`, `hiberfile` `hibernated`, mounted, peer, confirmation) | First two allowed; every other case refused with its own gate code |

### 4.2 Cases registered in M2

| Id | Script | Environment and how it runs | Pass condition |
|---|---|---|---|
| `I-SMOKE-01` | `tests/integration/smoke/mount_smoke.sh` | B, as the guest user, on a fresh copy of `clean-256m-4k` | rw mount with `fskit` and not read-only; file operations; read-only remount refuses a write; `ntfsfix -n` exit 0 (28 checks) |
| `I-SMOKE-02` | `tests/integration/smoke/fd_pass.sh` (with `fd_pass.c`) | B; `fd_pass` is the only command run with `sudo -n` (passwordless sudo in the guest); target is an attached virtual disk or an image file | A root parent opens the device `O_RDWR` or `O_RDONLY`, a child without device permission mounts it from `/dev/fd/N` read/write and read-only, writes, reads back, unmounts, `ntfsfix -n` exit 0. Needs patch 0003 |
| `I-INT-01`, `I-INT-03` | `tests/integration/integrity/remount_hash.py` | A and B (`tests/harness/run-env-b.sh` clones the golden VM and builds NTFS-3G in it) | Section 4 matrix |
| `C-REF-01` | `tests/compatibility/cross_read.py` | Host case driving both environments: `tests/harness/run-env-b.sh --orchestrate --cases C-REF-01` (clones and deletes the VM). Without a running VM it skips with a note | 106 files written on macOS (100 seeded plus Unicode names, nesting, an empty file and directories) read on Linux with equal size and SHA-256; image hash equal at every hop; treedumps equal; `ntfsfix -n` exit 0. macOS lists non-ASCII names in NFD while the disk holds NFC (U22), so macOS names are compared in NFC; the root `.fseventsd` is skipped |
| `U-ARGV-06` | unit test of `argv_check()` (T-M3-19) | H, A | Section 4.1 |

Evidence-only scripts (not regression cases): `tests/integration/smoke/option_matrix.sh` (T-M2-04; re-run on each macFUSE update, R-09) and `tests/integration/smoke/sync_trace.sh` with `sync_probe.c` (T-M2-06, FUSE opcode trace). A trace that asserts on `FUSE_FSYNC` must filter by node id or time window: macOS itself fsyncs `.fseventsd/fseventsd-uuid` four times at every mount, and the FUSE `pid` field is always 0 on this backend.

A read/write mount always changes the image, even when the test writes nothing, because macOS creates `.fseventsd` in the root (T-M2-04 `rw-noop`). A read-only mount leaves the image byte-identical (`ro-noop`). "Image hash unchanged" checks therefore apply to read-only sessions and to refusals only.

## 5. Reference comparison against NTFS-3G on Linux

Purpose: show that running the same engine behind macFUSE FSKit does not change on-disk results.

1. Twelve deterministic operation scripts (`tests/compatibility/ref/ops-NN.json`: sequences of create, write at offset, truncate, rename, delete, set time, set xattr) are replayed by `tests/harness` on a mount point. They never depend on wall-clock time.
2. Run 1: Environment A, Linux `ntfs-3g` (internal FUSE) on a copy of the fixture.
3. Run 2: Environment B, this product on another copy.
4. Both resulting images are dumped in Environment A by `tests/tools/treedump` (links `libntfs-3g`, read-only): path, type, size, allocated size, SHA-256 of the unnamed stream, each named stream with its hash, NTFS times, file attribute flags.
5. The dumps must be equal after removing entries listed in `tests/compatibility/expected-differences.md`. Expected classes: macOS-created metadata (`.fseventsd`, `.Spotlight-V100`, `.DS_Store`, `com.apple.*` streams), access times, and MFT record numbers. Names are compared on disk bytes from Environment A dumps; names listed by macOS are NFD (U22) and are not compared byte for byte.
6. Second independent reader: the Linux kernel `ntfs3` driver mounts the macOS-written image read-only and file hashes are compared again [A: `ntfs3` is available in the container host kernel; if not, this step is skipped and reported as skipped].
7. Structural check on every resulting image: `ntfsfix -n` (no-action mode), `ntfsinfo -m`, and a full read of every file. NTFS-3G has no full `fsck`; Windows `chkdsk` in Environment C is the authoritative structural check.

Performance comparison uses the same scripts as `P-*` in both environments on the same physical host and reports ratios, not absolute claims.

## 6. Crash, kill and disconnect testing

| Technique | Environment | What it proves |
|---|---|---|
| Crash-consistency sweep (below) | A | State of the image if the device stops accepting writes after write N, for many N |
| `SIGKILL` of worker during copy | B | Daemon cleanup, journal, read-only on next mount, no hang of applications (`daemon_timeout` is not relied on: T-M2-04 found an explicit value ignored) |
| `SIGKILL` of `ntfsmacd` | B | Open files keep working; restart reconciliation |
| Kill of the macFUSE FSModule process | B | Volume failure mode and cleanup [V:S7 describes the mode] |
| Force-detach of the disk image during write (`hdiutil detach -force` in guest, or removing the virtual disk) | B | Approximation of unplug |
| Real unplug during write | C | The real thing |
| Guest hard power-off during write (`tart stop` without shutdown) | B | Power-cut approximation; journal survives; next boot mounts read-only. A plain `tart stop` is not a clean shutdown: files written seconds before it were missing after the next boot (T-M2-02), so `vm/macos/tart.sh stop` syncs and shuts the guest down first, and no durability test may treat a plain `tart stop` as clean. Its use as a power cut is an approximation only: which layer loses the data (guest page cache, virtual disk, host cache) is not measured, so it is not shown to be equivalent to a power cut |

After every scenario the checks are the same: (1) the next attach mounts read-only with the documented reason, or is refused as `mirror_split` (classified window below; D3 exception); (2) every file that had been synced before the event (the test records these) has the right hash; (3) `ntfsfix -n` reports no fatal error, which means exit 0, or exit 1 on a point classified `mirror_split`; (4) in Environment C, `chkdsk /scan` output is stored; (5) once T-M3-12 has passed, the image carries `VOLUME_IS_DIRTY`.

`ntfsfix -n` is a shallow check (boot sector, `$MFT` against `$MFTMirr`, `$LogFile`; it never walks indexes or run lists), and it is not a pure reader: it prints "Correcting differences in $MFTMirr ... OK" and "Going to empty the journal ... OK" without writing (image hash unchanged), and its final step is a read/write `ntfs_mount` (`ntfsfix.c:1649`), whose failure gives its exit 1. Its text is never evidence of a repair [V: T-M3-14 triage].

### Crash-consistency sweep

Patch 0004 adds a device-operations wrapper inside `libntfs-3g`, compiled only with `--enable-faultio` (release objects proven identical to a build without it by `nm` and `objdump`, `tests/results/evidence/m314/full/build-proof.txt`). `write()` and `pwrite()` share one write counter. Environment variables (all ignored when `NTFSMAC_FAULT_PLAN` is unset or empty):

| Variable | Meaning |
|---|---|
| `NTFSMAC_FAULT_PLAN` | The plan (table below). An unknown plan makes every write fail with `EINVAL` (fails closed). |
| `NTFSMAC_FAULT_KILL` | `1`: at the fault point the layer kills `ntfs-3g` with `SIGKILL` instead of dropping later writes. The sweep always sets it: dropping writes and reporting success let the engine read back stale metadata and spin in one trial, which no real power loss can do. |
| `NTFSMAC_FAULT_STATE` | File holding the running write counter, written before each device write; the workload reads it right after `fsync` returns, so a claim's write count is conservative. |
| `NTFSMAC_FAULT_TRACE` | File receiving offset, length and the first bytes (magic) of every write, including the one about to be dropped; the sweeper classifies `mirror_split` from it. |

| Plan | Effect (with `NTFSMAC_FAULT_KILL=1`) |
|---|---|
| `count` | No fault; reports the number of device writes at exit |
| `cut:N` | Writes 1..N reach the image; the process is killed before write N+1 is issued. (Without `KILL`: later writes are dropped and report success.) |
| `skip:N` | Write N alone is dropped; later writes go through: a reordering of persistence (supplementary) |
| `short:N:K` | Write N writes only its first K bytes, then the kill (torn write; supplementary) |
| `eio:N` | Write N returns `EIO`; later writes succeed |
| `dead:N` | Write N and everything after returns `EIO` (unplug) |
| `delay:MS` | Every write is delayed (section 7) |

Fault model: the sweep's images are files, and the kernel keeps every issued write after `SIGKILL`, so writes persist in issue order. Real devices without a flush may persist any subset; `--plan skip` exercises one reordering, and neither it nor `--plan short` is part of 3.9.

Sweep (`tests/stress/crashsweep/run.sh --mode full`, Environment A): for each of 5 workloads (`seqwrite`, `smallfiles`, `renamechurn`, `overwrite_truncate`, `dirtree`), get the write count W with `count`, then run with `cut:N` for 110 values of N spread over 1..W (550 points; criterion 3.9 needs at least 500). Controls: the `count` run of each workload and `cut:0`, which must leave the image byte-identical to the fresh one (10 controls). After each run, with the release build and `norecover`: read-only mount, `ntfsfix -n`, the durability claims (every file whose `fsync` of file and parent returned at a write count up to N, unless a later operation on that path had started) and an advisory walk (`walk_dirty`, not a failure). `tests/stress/crashsweep/README.md` has the details.

A failing cut point is not automatically a project bug: NTFS-3G has no journal, so some windows of metadata inconsistency are inherent. Each failure is classified by the senior reviewer as (a) expected non-atomic window, documented in the limitations (ARCHITECTURE.md section 9), (b) engine defect, reported upstream, or (c) defect in our patches, release-blocking.

**Classified window `mirror_split` (class a).** A failing cut point is `mirror_split` only if all three hold; anything else stays an unclassified failure:

1. The read-only refusal and the `ntfsfix -n` message are exactly "$MFTMirr does not match $MFT (record k)." with k in 0..3, and nothing else failed.
2. The write trace shows that write N is `$MFT` record k and write N+1 (never issued) is `$MFTMirr` record k.
3. On a scratch copy with the mirror record replaced by the raw `$MFT` record, the read-only mount, `ntfsfix -n` (exit 0), every claim and the walk pass.

The product side of this window (probe state, refusal, `repair --sync-mirror`) is ADR-021 and ARCHITECTURE.md section 3.7.

**Gate (modes `short` and `full`, plan `cut`).** Exit 0 if and only if: controls 10 of 10; zero unclassified failures; zero broken claims; zero workload hangs. The `mirror_split` and `walk_dirty` counts are reported in `summary.json` and never fail the gate. No count is pinned: they follow NTFS-3G's write sequence and change with any engine or workload change, and the rule above already proves each point. Full run at 7174b4b: 550 points, 532 pass, 18 `mirror_split`, 0 of 15,626 claims broken, 0 hangs; short run: 250 points, 11 `mirror_split` (`tests/results/evidence/m314/`).

**Torn writes (`--plan short`, supplementary).** "Incomplete multi-sector transfer" on a torn 1 KiB MFT record or 4 KiB index block is the update-sequence protection working, not a defect. Without a journal the record cannot be redone, so a tear can make already synced data unreadable (204 failing points of 550 with `short:N:512`, 115 broken claim sets). Class (a); limitation N11 (ARCHITECTURE.md section 9).

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

Handling of findings: crashes in engine code are expected (two NTFS-3G releases labelled Security Release in 2026 and two further releases with CVE or memory-safety fixes [V:S10]). They are recorded in `docs/SECURITY_FINDINGS.md`, a report for the NTFS-3G maintainers is drafted, and sending it is a human decision. A finding reachable through a read-only mount of a crafted volume raises risk R-07 and is independently reviewed.

## 9. Continuous integration (local only)

There is no GitHub-hosted CI: no GitHub Actions workflows, no hosted runners and no binaries uploaded to GitHub (ADR-015). All checks run on the developer Mac through `scripts/ci-local.sh`, which wraps `scripts/test-all.sh`.

| Stage | Where | Content | Blocking |
|---|---|---|---|
| `linux-build-test` | Environment A (local container) | Fetch pinned deps, build NTFS-3G and project (release, ASan, UBSan), unit tests (daemon-logic unit tests also under ThreadSanitizer, `U-TSAN-*`), fixture generation, Linux reference suite with FUSE, probe tests on all fixtures | Yes |
| `linux-crash-sweep-short` | Environment A | `tests/stress/crashsweep/run.sh --mode short`: 50 cut points per workload, the gate rule of section 6 (controls, unclassified failures, broken claims, hangs; `mirror_split` and `walk_dirty` reported, not pinned) | Yes |
| `linux-fuzz-short` | Environment A | 5 minutes per target, corpus kept locally | Yes on crash in project code |
| `static-analysis` | Environment A | `clang-tidy`, `cppcheck`, `shellcheck`, `-Wall -Wextra -Werror` on project code | Yes |
| `macos-build` | Host (build only, never a mount) | Build project code and NTFS-3G against macFUSE headers and libraries extracted from the verified package without installing it [A: extraction with `pkgutil --expand-full` gives usable headers and dylibs]; unit tests; unsigned package build | Yes |
| `macos-vm` | Environment B | Mount, integrity, stress, soak, performance and Finder tests | Yes for the stages a milestone gate names |

When to run: `ci-local.sh --quick` before every push, from the day T-M1-07 merges (it starts as bootstrap check, configure, build and `ctest`, and gains static analysis and the Linux stages as their tasks land; T-M5-08 completes it); the full run before each milestone gate and before a release. The result file for each run is committed under `tests/results/` (section 10), so every gate and release points at a recorded local run of the tested commit (section 10 defines the exact rule).

No test in the `macos-build` stage may attach or mount a disk image; that stage only builds and runs unit tests on the host.

Build outputs, packages and release candidates stay on the local machine under `build/` and `dist/` (both git-ignored). Publishing binaries anywhere is a later, explicit human decision.

`scripts/install-git-hooks.sh` installs a local `pre-push` hook that runs `ci-local.sh --quick`, so the "before every push" rule is enforced on this machine (`git push --no-verify` skips it once).

Local release pipeline: `scripts/release-local.sh` runs `ci-local.sh --quick`, then `packaging/build-pkg.sh` in a disposable clone of the golden VM (the engine links macFUSE's libfuse, which the host does not have), copies the unsigned package to `dist/<label>/`, and installs it in a second fresh clone: every installed binary must load and run, and `uninstall.sh` must leave no file and no receipt. Both VMs are deleted afterwards. Until T-M6-01 the package holds only the parts that exist (worker, ntfsprogs, probe, volmark under `/usr/local/libexec/ntfs-macos/`) and has no install scripts; see `packaging/README.md`.

## 10. Recording results

| Item | Rule |
|---|---|
| Result file | `tests/results/<UTC date>-<short sha>-<env>.json`, produced by `tests/harness/report.py` |
| Schema | `{ schema, git_sha, dirty_tree, env, host_os, guest_os, macfuse_version, ntfs3g_version, started, finished, cases: [ { id, status: pass/fail/skip/error, duration_s, evidence: [paths], note } ] }` |
| Dirty tree | A result from an uncommitted tree is marked `dirty_tree: true` and cannot satisfy a milestone gate |
| "Result for the exact commit" | A result file cannot contain the SHA of the commit that adds it. `git_sha` is the commit that was tested (HEAD at the start of the run, clean tree). A gate or release commit G is covered by a result file when `git_sha` is G or an ancestor of G and `git diff --name-only <git_sha> G` lists only paths under `tests/results/`, `docs/audits/`, `CHECKPOINT.md`, `NEXT_ACTIONS.md` and the generated README status table. `scripts/ci-local.sh --verify-result <file>` performs this check (T-M5-08); the milestone audit repeats it. |
| Skips | A skipped case needs a `note`; skips are listed in the milestone audit |
| Evidence | Logs and small artifacts under `tests/results/evidence/<run>/`; images are not stored, only their hashes |
| Status table | `README.md` section generated from the newest result per environment that has at least one case not `skip` (a skip-only result, such as `2026-11-14-3593b7c-B.json`, is ignored): each feature is `Verified (VM)`, `Verified (hardware)`, `Linux only`, or `Untested` |
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
| H-USB-10 | The `mirror-split` image (record 0; the sweep's `smallfiles-c68.img.gz`) and the `vmk-split` image (record 3), each written to a flash drive: run Windows `chkdsk /f`, then re-attach to the Mac (U15). Then a second copy of `mirror-split` repaired on the Mac with `ntfsmac repair --sync-mirror`, followed by Windows `chkdsk /scan`. Also recorded: whether a volume used only by Windows 10 or later ever probes `mirror_split` (Windows no longer updates the full `$MFTMirr`, `volume.c:997`) | Recorded: whether `chkdsk` repairs `$MFTMirr` and what it reports; afterwards the probe gives `clean` or `dirty_flag`, never `corrupt` or `mirror_split`; after the Mac repair `chkdsk /scan` reports no mirror error |
| H-USB-11 | With patch 0009: on the Mac, tag a file and a folder in Finder and add a Finder comment (streams `com.apple.metadata:_kMDItemUserTags`, `com.apple.metadata:kMDItemFinderComment`), eject, then on Windows run `chkdsk /scan` and `chkdsk /f`, re-attach to the Mac (U23, ADR-025) | Recorded: whether `chkdsk` reports, drops or keeps the streams; on the Mac the tags and comment are still there. If `chkdsk` drops or flags them, patch 0009 is reverted and tags are documented as not stored |

No scenario may run on a drive that the human has not named by serial number.
