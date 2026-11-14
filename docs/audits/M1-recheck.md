# Audit M1 re-check: fixes to the architecture documents

Auditor: role E (independent; wrote none of the changes). Date: 2026-11-14.
Scope: commits `b99d414` (fixes 1 to 4) and `6a3e5e0` (remaining items and the resolution table), read against `docs/audits/M1.md` sections 4, 6 and 8, `AGENTS.md`, and the NTFS-3G 2026.9.28 source (tarball extracted read-only, paths below relative to it).
Method: full read of the changed sections; every engine-behaviour claim the new design relies on checked in source; a Python id check (kept in the session scratchpad, not in the repo) over all six documents; adversarial review of the label sanitiser, `argv_check()`, the four journal-clearing paths, `ntfsmac-volmark` ordering and the error channel.

## 0. Verdict

**ACCEPT WITH FIXES.**

All 22 items of the M1 fix list are present in the text. 19 are confirmed as written; 3 are only partly resolved (items 1, 5, 6). The id graph is clean (section 4). The new design is mostly sound and its source claims are largely correct (section 3). Two new high findings must be fixed before T-M3-12, T-M3-18, T-M3-19 and T-M4-13 start, because they are design errors that those tasks would otherwise implement faithfully:

1. The "clean session" decision, which clears the on-disk dirty flag and the journal entry, rests on the **absence** of error lines and on **worker exit status 0**. The source shows exit 0 is unconditional after the FUSE loop, and the channel fails open (N-01).
2. The probe's `serial` string (and other free-text fields) is not covered by the strict parser rules, yet it names a file written by root in `/var/db/ntfs-macos/journal/` (N-02).

## 1. Resolution claims (M1.md section 8)

| Item | Claim | Result | Where / note |
|---|---|---|---|
| 1 | Resolved (b99d414) | **Partial** | Label sanitiser, `argv_check()`, `mkdirat` under `O_NOFOLLOW`, tests `U-LABEL-*`, `U-ARGV-*`, `U-VERDICT-*`, `F-SANITIZE-*`, `I-DMN-18`: present (ARCH 3.3, 4.1 step 7; ADR-016; TEST 4.1). Fix list said "label **and any disk-derived string**". Only the label is covered; `serial`, `logfile_lsn`, `total_sectors`, `reasons` have no grammar (N-02). `usermapping` path contains a space that rule 3 rejects (N-09). |
| 2 | Resolved (b99d414) | Confirmed | ADR-017; ARCH 3.2 gates, 3.4 `journal_clear` and `repair`, 3.5 four removal paths; `I-DMN-19..21`; T-M4-14; R-32. Design weaknesses are new findings N-04, N-05, N-11. |
| 3 | Resolved (b99d414) | Confirmed | ADR-006 default plan; ARCH 3.6; `volmark` in component table with uid, sandbox, fd, timeout; D3 "host-only until T-M3-12 passes"; section 7 wording fixed. The statement "writes only the flag" is inexact (N-03). |
| 4 | Resolved (b99d414) | Confirmed | D2 reworded; "What D2 does not cover" with a second-line table; `I-DMN-23`. I checked the second-line claims in `volume.c` (section 3). |
| 5 | Resolved (b99d414) | **Partial** | D1 is conditional and has an "Until then" text; clean report needs zero error records. But the clean condition also names "worker exit 0", which is not evidence (N-01), and D1 cites patch 0002 for `F_FULLFSYNC`, which upstream already does (N-06). `I-SYNC-*` has no owning task or criterion (N-14). |
| 6 | Resolved (b99d414) | **Partial** | Mechanism exists (patch 0006, ADR-018, T-M3-18, T-M4-13, `I-ERR-*`, `I-DMN-24`). It fails open and is unbounded (N-01, N-07). |
| 7 | Resolved (this change) | Confirmed | ARCH 3.1 check 4 "2.0 (major 2 and minor 0)", check 5 `wake`/`WAKE`, fixture `hiber-wake`, plan 3.1, T-M3-04. Matches `volume.c` (major 2 and minor 0; `hibr`/`HIBR`; 4096 bytes). |
| 8 | Resolved (b99d414, this change) | Confirmed | `I-PERM-01..06`, T-M3-20, criterion 3.16; `U-TSAN-01..02`, T-M4-15, criterion 4.11; N9 in ARCH 9 and ADR-012, carried to criterion 6.6; `I-DMN-18`, `I-DMN-21`. |
| 9 | Resolved | Confirmed | ARCH 4.2 no longer claims zero-fill. |
| 10 | Resolved | Confirmed | ARCH 6, R-07, TEST 8 now say two Security Release labels plus two further releases. No "four" left (grep). |
| 11 | Resolved | Confirmed | ADR-004, ADR-001 D, ARCH 8.1, U6, R-13 attribute the claims to forum posters or mark `[A]`. |
| 12 | Resolved | Confirmed | ADR-011: unsigned binaries `[A]` until T-M2-03; Intel-only fix noted. |
| 13 | Resolved | Confirmed | One date form in ARCH 0 and ADR-002. |
| 14 | Resolved | Confirmed | `RDONLY \| FORENSIC` nuance in ARCH 3.1; matches `volume.c` line 1286. |
| 15 | Resolved | Confirmed | ADR index has 18 rows, equal to the 18 headings. |
| 16 | Resolved | Confirmed | T-M6-03 is wave b; T-M6-06 and criterion 6.7 exist; R-28 lists it. |
| 17 | Resolved | Confirmed | Ownership map names owners; `src/daemon/CMakeLists.txt` uses a glob; `tests/tools/CMakeLists.txt` append is in T-M3-04's Files column. `scripts/ci-local.sh` is listed by T-M1-07 and T-M5-08 but not in the map (minor). |
| 18 | Resolved | Confirmed | Env column defines `hardware`, `all`, `none`; `CI` gone; T-M5-08 is `H, A, B`. |
| 19 | Resolved | Confirmed | T-M1-07, criterion 1.7, ADR-015 consequence text, TEST 9. |
| 20 | Resolved | Confirmed | `I-SMOKE-02`, `I-DMN-17` in the matrix; `I-DMN` 01 to 24 each have an owner (section 4). |
| 21 | Resolved | Confirmed | R-06, plan 6, budget table and ownership map agree: Opus review of 0002 to 0006; counts 16 in the table, "13 to 17" stated. |
| 22 | Resolved | Confirmed | TEST 10 defines "result for the exact commit"; criteria 5.7, 7.4; no disk attach in `macos-build`; plan 6.5. |

Tally: confirmed 19, partial 3, not resolved 0.

## 2. New findings

Severity: High = data-corruption or privilege hole in the design as written; Medium = a guarantee or safeguard that does not hold under a stated condition; Low = wording, consistency or hardening.

### High

**N-01. The clean-session decision fails open; worker exit 0 is not evidence (ARCH 3.5 path 1, 3.6 step 3, 4.5 step 3, 7; ADR-006, ADR-018).**
Scenario. After a session the daemon calls it clean when the worker exited 0, the pipe delivered no record, and its own `F_FULLFSYNC` succeeded. Then volmark clears `VOLUME_IS_DIRTY` and the journal entry is removed. Windows will then not check the volume, and this product mounts it `rw` again.
Source facts:
- `src/ntfs-3g.c` lines 4579 to 4601: after `fuse_loop(fh)` the code sets `err = 0`; `ntfs_close()` (line 3934) only logs `Failed to close volume` when `ntfs_umount()` fails. The process exits 0 even when the final inode sync, `$MFT` sync or device close failed. Exit 0 also does not distinguish a FUSE `DESTROY` from other loop exits.
- `libntfs-3g/unix_io.c` line 203: the device close path calls `ntfs_fsync()` directly (not the `sync` operation) and returns -1 on failure; line 82: `ntfs_fsync()` already tries `F_FULLFSYNC` and silently falls back to `fsync` (N-06). ADR-018 describes patch 0006 as covering failed or short `pwrite` and "a device sync"; the close-path `fsync` and a failed `ntfs_umount` are not named.
- The channel is silent in every failure of the channel itself: `NTFSMAC_ERRFD` unset or misspelled, patch not applied, pipe read end lost when the daemon restarts (T-M4-05 re-adoption), `SIGPIPE`, a descriptor leaked into a macFUSE helper process so that EOF never comes, or a compromised worker that simply stays quiet. All of these look like "zero error records".
Fix: make success a positive proof. Patch 0006 (or a two-line change in `ntfs_close()`) writes `S` at start (channel is live) and `U 0` only after `ntfs_umount()` returned 0 and the device close returned 0; the daemon needs both, in order, and treats their absence as unclean. Drop "worker exit 0" as evidence; keep it only as a necessary condition. Mark the session unclean when the daemon re-adopts a worker (error history is unknown). Extend the patch scope text and `I-ERR-*` with: close-path `fsync` failure, `ntfs_umount` failure, channel not set.

**N-02. Probe `serial` (and other free text) reaches a root-written file name unchecked (ARCH 3.1, 3.5; ADR-016 decision 1).**
Scenario. The probe is assumed exploitable (section 6). The strict parser checks UTF-8, duplicates, known keys and enums, and the label is sanitised, but `serial` is only "a string". The daemon uses the serial as the journal file name (`/var/db/ntfs-macos/journal/<serial>`) and the tombstone name `<serial>-<UTC>`, as root. A verdict with `"serial": "../../../Library/LaunchDaemons/x"` makes root create and rename a file outside the journal directory with partly controlled content. `reasons` is free text that is shown in `status`, notifications and the CLI; control characters or escape sequences would reach the administrator's terminal. No test (`U-VERDICT-*`) covers either.
Fix: define a grammar for every field: `serial` and `logfile_lsn` `^0x[0-9A-F]{16}$`, integers bounded, `reasons` replaced by enum codes the daemon maps to its own text; derive the file name from the parsed integer re-formatted by the daemon, never from the input string; add `U-VERDICT-05` for hostile serials and `reasons`, and put serial into `F-SANITIZE-01`.

### Medium

**N-03. Volmark's read/write library mount writes more than the flag, and the flag write is not atomic (ARCH 3.6, R-33, `U-VMK-07`).**
Source (`libntfs-3g/volume.c`, `mft.c`, `inode.c`):
- A read/write mount with `NTFS_MNT_NONE` runs `fix_txf_data()` after the hibernation and log checks (line 1309): it can truncate and rewrite the root `$TXF_DATA` attribute. In `set` this happens at step 1, **before** the dirty flag is set at step 3, so a cut at that point leaves a damaged volume with no on-disk marker (only the host journal). R-33 mentions the fix-up; ARCH 3.6 and the "Must never" cell do not, and the test fixtures (`clean-64m-4k`) never have a non-resident `$TXF_DATA`.
- `ntfs_volume_write_flags()` ends in `ntfs_inode_sync()`; `ntfs_mft_records_write()` writes the `$Volume` record to `$MFT` and then to `$MFTMirr` in two separate writes. Each is a 1 KiB multi-sector write with fix-ups. A cut between them, or a torn sector pair, makes `ntfs_mount()` fail with `$MFTMirr does not match $MFT` or an incomplete multi sector transfer (`volume.c` lines 1020 to 1050), for the probe too. The next attach is then `corrupt` (policy: `refuse`, left to the system), not `ro` as ARCH 3.6 step 2 says. `U-VMK-07` demands that "afterwards the image mounts read-only", which can fail by design.
- The 20 s `SIGKILL` of volmark can land between the two writes.
- Because the policy default is `rw`, every attach now writes `$Volume` twice even if the user only reads. A yank in the first seconds can corrupt a volume the user never wrote to.
Fix: state the `$TXF_DATA` fix-up and the two-step write in ARCH 3.6; either set the flag before any other write (open the volume with a minimal custom mount that skips `fix_txf_data`) or run the fix-up only in the worker; replace the `U-VMK-07` pass condition by "mounts read-only, or probe says `corrupt`, and in both cases no data region changed"; add a fixture with non-resident `$TXF_DATA`; do not `SIGKILL` volmark while it holds the writable fd (signal, then wait for the write to finish, or time out only before `set` step 3); record the extra write exposure in R-33 and offer an `ro`-first mount option.

**N-04. Journal evidence rule and repair gate: untested cases that recreate the M1 deadlock (ARCH 3.5 path 2, 3.2 `repair` gate; U12).**
- U12 asks whether a Windows rw mount or `chkdsk` advances the LSN and clears the flag. It does not ask the negative question the rule needs: a plain Windows rw mount of a volume that already has our flag set must leave `VOLUME_IS_DIRTY` set (or the rule can fire after a mount that replayed the log but did not check our un-journalled NTFS-3G writes). Add it to `H-USB-08`.
- The `repair` gate needs `volume_flags` exactly `["DIRTY"]`; probe check 3 maps `VOLUME_MODIFIED_BY_CHKDSK` to `dirty_flag`. If chkdsk leaves 0x8000 set, the volume is `ro`, `journal-clear` needs `clean`, and `repair` refuses: no way back. `layout.h` defines the bit; no ntfsprogs tool clears it. Record which flags remain after `chkdsk /f` in `H-USB-08` and decide the policy for a lone `MODIFIED_BY_CHKDSK` before M4.

**N-05. The typed confirmation is not a security boundary (ARCH 3.4, ADR-017).**
`confirm_label` and `confirm_serial` are shown by `list` and `status` to any peer on the socket (mode 0660, group `staff`). The daemon only compares them with values it already published. Authorisation is "member of `admin`" by `LOCAL_PEERCRED`, which is true of every process of a typical macOS user. A compromised worker (console user, usually admin) can call `journal_clear` or `repair` for another attached volume. The sandbox is `[A]` (T-M4-06, C9) and the stated fallback is uid separation only, in which case nothing stops this. Result: an unsafe volume reaches `rw` without informed human action.
Fix: require a daemon-side user-presence step (Authorization Services right with password, or a daemon-run prompt) for `journal_clear`, `repair` and `rw` mounts of previously unsafe volumes; refuse peers whose process group is the worker's; make the sandbox mandatory for release, or deny `connect()` to the socket explicitly in the worker profile; list this in T-M4-12.

**N-06. Patch 0002 premise is wrong, and the existing fallback masks sync errors (ARCH 4.4, ADR-014, ADR-002, D1, T-M3-10).**
`libntfs-3g/unix_io.c` `ntfs_fsync()` (lines 82 to 107) already issues `fcntl(F_FULLFSYNC)` on `__APPLE__` and falls back to `fsync()` when that fails. The documents state that the device sync is "plain `fsync`" [V:S11] and carry a patch for it. Consequences: patch 0002 is likely unnecessary (less patch surface; adjust R-06 and T-M3-10); and the fallback means a drive or bridge that rejects `F_FULLFSYNC` still returns success, so D1's "drive cache has been flushed" can be false without any error. Patch 0006 must report the fallback (as a warning record), and the clean report must say "flushed" only when `F_FULLFSYNC` itself succeeded in the daemon and in volmark (ADR-006 step 4 already tolerates the fallback with only a warning).

**N-07. Error channel: flooding, bounds and descriptor hygiene (ARCH 7, ADR-018).**
The worker handles hostile data and may be compromised. The text says unparseable data counts as an error record but does not bound the reader: a worker that writes without newline or in an endless stream grows the root daemon's buffer, or blocks on a full pipe if the daemon stops reading after the first record. Fix: fixed 64-byte line limit, at most N lines read, then close the read end and treat the overflow as an error; state that all other descriptors in the worker are `FD_CLOEXEC` so the pipe write end is not inherited by macFUSE helper processes (EOF would never arrive); note in section 7 that a lying worker can only make its own volume look clean, which is the same exposure as a compromised worker writing its device directly (D2 residual).

**N-08. Nothing binds the probed volume to the volume that volmark and the worker open (ARCH 4.1 steps 4 to 8, 3.6).**
The probe verdict (serial, total sectors, LSN) is not re-checked by volmark or the worker; volmark reads only flags. A device that answers differently the second time (hostile USB device) gets a journal entry under the probe's serial while another volume is flagged and written. Also step 6 opens `O_RDWR` without checking that Disk Arbitration has no mount for the disk (the dissenter is `[A]`, U7; Apple's read-only driver may mount at the same moment). Fix: pass expected `serial` and `total_sectors` to volmark (exit if different); re-read them in the worker start check; before `O_RDWR`, read the DA description and require no mount path, otherwise `ro` or `Failed`.

### Low

**N-09.** `usermapping=<path under /Library/Application Support/ntfs-macos/>` contains a space; `argv_check()` rule 3 allows bytes `0x21..0x7E` outside `volname`, so the builder's own opt-in output is rejected and `U-ARGV-01` cannot pass. Either install the mapping file at a path without spaces, or add a per-key rule for `usermapping`. Do not relax rule 3.

**N-10.** D2 "does not cover" omits two engine blind spots: `ntfs_hiberfile_open()` looks up `hiberfil.sys` in the root with case-sensitive matching (`NVolSetCaseSensitive` is the default at mount), so a differently cased name is "absent" to both the engine and the probe (which reuses the same rule); and the file is only searched in the root. Add both to the table and a fixture.

**N-11.** `journal_clear` checks that the target volume is not mounted `rw`, not that no live session owns the entry. Two clones with one serial (accepted in 3.5) let a clear on the `ro` clone remove the entry of the live `rw` one. Gate on `session_id` having no live session. Also warn more strongly when `marker=set`, flag clear and LSN unchanged (the lost-flush case, where nothing checked the volume), not only for `state=write_error`.

**N-12.** Root-side handling of the mount point: `fchownat` and the device/inode record are by name, not by the descriptor of the created directory (open it with `O_NOFOLLOW` after `mkdirat` and use `fchown`/`fstat`). The wait loop does `statfs` as root on a path that a compromised worker serves; that call can hang uninterruptibly and stalls the volume's queue (60 s `daemon_timeout`). Run it from a helper process or with a timeout. `/Volumes` is root-only, so these are hardening items.

**N-13.** Label look-alikes beyond slash and colon (for example Cyrillic or full-width letters imitating `Macintosh HD`, bidi-neutral spoofs, full-width comma) remain allowed. No option or path injection results (all such bytes are 0x80 or above and the argv rules hold), but typed confirmations and notifications show an attacker-chosen name. State it as an accepted display risk in ADR-016. NFC and NFD labels collide only through APFS name comparison, which the `mkdirat`/`EEXIST` suffix handles; add that case to `U-LABEL-08`.

**N-14.** Dependency edges and ownership: T-M3-12 uses the fault layer in `U-VMK-07` but does not depend on T-M3-14 (wave order makes it work); T-M4-14 relies on the error records (clean condition) but does not depend on T-M4-13 and shares wave c with it; `I-SYNC-*` (the gate of D1) is cited by no task acceptance or plan criterion; `scripts/ci-local.sh` is not in the ownership map although T-M1-07 and T-M5-08 both list it.

**N-15.** ARCH 3.2 header says "With explicit `--read-write` request"; the protocol (3.4) has `mode` `auto`/`ro` only. Align the wording.

## 3. Adversarial review: what holds

Verified in source (NTFS-3G 2026.9.28):

| Claim | Result |
|---|---|
| `ntfs-3g` splits `-o` with `strsep(&s, ",")`, no escape; token split at first `=`; unknown tokens pass to libfuse (`src/ntfs-3g_common.c` 296 to 600) | Confirmed. `fsname` is rejected (`FLGOPT_NOSUPPORT`); the engine appends `allow_other,nonempty,` and an escaped `fsname=` (lines 74, 621 to 626). |
| libfuse also splits on `,` and treats `\` as escape | Not verifiable here (macFUSE `libfuse` is not in the tarball). Rule 3 forbids `\`, so `argv_check()` is correct whatever libfuse does with it; engine-side re-parse matches `parse_mount_options` (`uid`/`gid` parsed with `%i`, masks with `%o`; rule 5 demands exact decimal and octal). |
| Label cannot inject an option or escape the mount point | Holds for the label: the sanitiser output alphabet excludes `, = \ /` and control bytes; non-ASCII UTF-8 bytes are all at least 0x80; the leaf is a separate argv element under a fixed `/Volumes/` prefix; `mkdirat` on an `O_NOFOLLOW` descriptor, `EEXIST` never adopts or follows (a symlink such as `Macintosh HD` gives `EEXIST`). `/Volumes` is root-owned 0755, so a user cannot race it. Remaining items: N-02, N-09, N-12, N-13. |
| `NTFS_MNT_NONE` read/write mount refuses on `hibr`/`HIBR`, short `hiberfil.sys`, unclean `$LogFile` and version 2.0 restart page (`volume.c` 1286 to 1307, 678 to 726, 796 to 850) | Confirmed. The checks run before `fix_txf_data`. |
| Mount and unmount never write `$LogFile` or any `$Volume` flag; `ntfs_logfile_reset` is called only with `NTFS_MNT_RECOVER` (line 1304) | Confirmed. No caller of `ntfs_volume_write_flags` or `VOLUME_IS_DIRTY` in `libntfs-3g` or `src/`; only ntfsprogs. `ntfs_umount` writes only inodes that are dirty. Exceptions: `fix_txf_data` and the two-step record write (N-03). |
| Worker with `norecover` and no `remove_hiberfile` gets `NTFS_MNT_MAY_RDONLY` (`src/ntfs-3g.c` 4030 to 4037) | Confirmed. |
| Journal clearing: four paths, none by time; evidence rule disabled until U12 and U13; never `rw` on the attach where a clear fires | Confirmed in the text. Gaps: N-04, N-05, N-11. Rule 2 can still lead to an automatic `rw` on the next attach, which is why U12 needs its negative case. |
| Probe to clear races | `journal_clear` and `repair` run a fresh probe under the volume's serial queue; typed label and serial are compared with that fresh probe; acceptable. The unbound serial between probe and mount is N-08. |
| Crash points of the ordering (journal entry, device open, `set`, `marker=set`, worker, `clear`, journal removal) | Enumerated: before `set` (entry present, flag unset, no write: `ro` next time, conservative); inside `set` (N-03: possible `corrupt`/refuse, not `ro`); after `set` before `marker=set` or worker (flag set, `ro`); during session (flag set, `ro`); after worker exit before `clear` (flag set, `ro`, user must repair); inside `clear` (N-03); between `clear` and removal (flag clear, entry present, `ro` until `journal-clear`). No window leaves flag clear, entry gone and session unclean, except through N-01 (false clean) and the cache-lying drive covered by D3's stated conditions. |

Guarantees claimed beyond conditions after the fixes: D1 (N-01, N-06; correctly labelled conditional), D2 (correctly reworded; blind spots N-10), D3 (host-only until T-M3-12; the on-disk part also depends on N-03). "No error lines" is used as proof of success in ARCH 4.5 step 3, section 7 table, ADR-006 step 3, ADR-018 and T-M4-13 acceptance; all must change with N-01.

## 4. Mechanical id check

Script scope: ARCHITECTURE, TECHNICAL_DECISIONS, IMPLEMENTATION_PLAN, AGENT_TASKS, TEST_STRATEGY, RISK_REGISTER.

| Kind | Defined | Distinct referenced | Dangling | Defined, never referenced |
|---|---|---|---|---|
| Tasks `T-Mn-nn[b]` | 75 | 75 | 0 | 0 |
| Risks `R-nn` | 33 | 33 | 0 | 0 |
| ADRs (index rows / headings) | 18 / 18, equal | 18 | 0 | 0 |
| Unknowns `Un` | 14 | 14 | 0 | 0 |
| Human actions `Hn` | 9 | 9 | 0 | 0 |
| Sources `Sn` | 25 | 25 | 0 | 0 |
| Limitations `Nn` | 9 | 9 | 0 | 0 |
| Guarantees `Dn` | 5 | 5 | 0 | 0 |
| Criteria (plan section 2) | 59 | all cited ones exist | 0 | see below |
| Test id families | all 41 families used outside TEST_STRATEGY appear in its matrix | | 0 | `I-SYNC-*` has no owning task or criterion (N-14) |

- Task dependency ranges (`T-M2-03..09`, `T-M4-01..05`, `T-M4-01..07`, `T-M6-01..05`), "all Mn" and "Mn gate": all resolve. Dependency cycles: 0. Same-wave dependency violations: 0.
- Same-wave Files overlaps: 0. Files listed by two tasks: `scripts/ci-local.sh` (T-M1-07, T-M5-08; different milestones, not in the ownership map) and `tests/tools/CMakeLists.txt` (T-M2-07, T-M3-04; declared in the map).
- `I-DMN-01..24`: every number has exactly one owner or writing task; ranges in TEST_STRATEGY, AGENT_TASKS and plan criteria 4.1 to 4.10 agree.
- Criteria never cited by number in AGENT_TASKS (1.1 to 1.6, 2.x, 3.1, 3.2, 3.12 to 3.15, 4.2, 4.3, 4.6, 4.8, 4.9, 7.2): each is covered through its test ids in the task acceptance column; not a defect.
- Missing formal edges (not cycles): T-M3-12 to T-M3-14, T-M4-14 to T-M4-13 (N-14). Stale wording: `--read-write` (N-15).
- Opus count: 12 reviews plus 4 tasks listed in the AGENT_TASKS budget table = 16; plan section 6 and R-21 say 13 to 17 (consistent).

Dangling: 0. Inconsistent or weak: 4 (N-09, N-14, N-15 and the ownership-map omission).

## 5. Counts

- Resolution claims: 22 checked; confirmed 19; partial 3 (1, 5, 6); not resolved 0.
- New findings: 15; high 2 (N-01, N-02); medium 6 (N-03 to N-08); low 7 (N-09 to N-15).
- Engine-behaviour claims checked in source: all the option-parsing, mount-check and no-flag-write claims are correct; the documents are wrong about the device sync (N-06) and about worker exit status (N-01); libfuse's own option parser could not be checked here (not in the tarball).
