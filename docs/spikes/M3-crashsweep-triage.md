# Spike M3: triage of the crash-sweep failures and review of patch 0004 (T-M3-14)

Date: 2026-11-14. Input: full run `tests/results/evidence/m314/full/` (550 cut points, 532 pass, 18 fail), short
run `.../short/` (250, 11 fail), torn-write run `.../torn512/` (550, 204 fail), failing images and event logs in
`build/crashsweep-failing/20261114T154121Z/` (git-ignored). Source: NTFS-3G 2026.9.28 from
`third_party/cache/ntfs-3g_ntfsprogs-2026.9.28.tgz`, sha256 `350d9415...f295c8`, equal to `third_party/deps.lock`.
All line numbers below are of that pristine tarball. Everything that ran, ran in Environment A
(`vm/linux/run.sh run`, repo read-only at `/work`, images in the container `/tmp`), always with `norecover`;
nothing was mounted on the host. No design document was edited.

## Summary

| # | Finding |
|---|---|
| 1 | All 18 failing cut points (and all 11 of the short run) are the same thing: the cut fell exactly between the write of `$MFT` record 0 (write N, 1024 bytes at byte 0x4000) and the write of `$MFTMirr` record 0 (write N+1, 1024 bytes at 0x1fff000). Proven by an independent write trace, not inferred. |
| 2 | The writer is `ntfs_mft_record_init()` (`mft.c:1349-1350`, "Sync MFT to minimize data loss") when the MFT grows by one record; `ntfs_mft_records_write()` writes `$MFT` (`mft.c:189`) and then `$MFTMirr` (`mft.c:204`) as two separate device writes with no flush between them. Upstream property; any interruption between the two `pwrite` calls (power loss, unplug, `SIGKILL` or crash of `ntfs-3g`) gives the same image. The fault layer does not cause it. |
| 3 | In every failing image only record 0 differs; `$MFT` is the newer copy (USN one higher, `$DATA` data and initialized size one record larger, sometimes allocation four clusters larger). The task statement "same USN" is not what the images show. |
| 4 | Apart from the mirror, the 18 volumes are intact: after copying either copy of record 0 over the other (scratch copy), every image mounts `ro,norecover`, the walk is clean, every live claim holds and `ntfsfix -n` exits 0. |
| 5 | Upstream `ntfsfix` (without `-n`) repairs it, `$MFT` wins, but it also sets `VOLUME_IS_DIRTY` and resets `$LogFile` unconditionally (`ntfsfix.c:330-340`, called at 1599). It is not acceptable as a product repair path. |
| 6 | Product impact: the window is not volmark-specific (R-33 assumes two writes per session edge). It opens once for every new MFT record beyond the MFT's initialized size, i.e. during ordinary file creation on a young volume: 8.0 % of all device writes in `smallfiles`, 7.6 % in `dirtree`. The probe then says `corrupt` and the policy refuses the volume, even read-only. D3 ("read-only on next attach") does not hold for this outcome. |
| 7 | Patch 0004: release build unaffected (proof re-run five times in this triage, PASS); counter verified against an independent libc-level trace in all 5 workloads; parser safe for its purpose. Minor items listed in answer 4; `build-ntfs3g.sh --configure-arg` cannot inject commands but globs, accepts any configure argument and can install a fault build into the product's default prefix. |
| 8 | Torn writes: "Incomplete multi-sector transfer" is the update-sequence protection doing its job. Without a journal NTFS-3G has no way back, so a torn MFT record or index block makes data that was already durable unreadable (115 broken claim sets in torn512). Expected, class (a), but it means fsync durability holds only for devices whose 4 KiB writes are atomic. |

## Method

1. Decoder (Python, read-only, on the host on the gzipped images): boot sector -> `$MFT` LCN 4, `$MFTMirr` LCN 8191,
   cluster 4096, record 1024; read records 0..3 of both copies; compare raw; apply the update-sequence fix-up per
   512-byte sector; decode header and attribute records (non-resident header fields and run list).
2. Write trace (Environment A): an `LD_PRELOAD` shim around libc `pwrite`, `pwrite64` and `write` that logs, for the
   image's descriptor only, the call number, offset, length and the first four bytes of the buffer. Each workload
   ran once with the fault build and plan `count`. The shim's call count equals the fault layer's counter in all
   five workloads (smallfiles 1841, renamechurn 1508, dirtree 4997, seqwrite 25664, overwrite_truncate 1000), so
   write N in the sweep is call N in the trace.
3. Replay: fresh sweeps of `cut:N` for N in 64..72 (smallfiles), 66..74 (renamechurn), 272..280 (dirtree).
4. Repair experiments on scratch copies: raw copy of `$MFT` record 0 onto `$MFTMirr` record 0, and the reverse;
   upstream `ntfsfix -n` and `ntfsfix`; sha256 and sector diff before and after.

## 1. Root cause

### Code path

- `ntfs_mft_record_alloc()` sets the record's bit in the MFT bitmap (`mft.c:1816`) and, when the record lies beyond
  the `$MFT` initialized size, calls `ntfs_mft_record_init(vol, ll)` (`mft.c:1823-1824`).
- `ntfs_mft_record_init()` (`mft.c:1260`): extends allocation if needed (`ntfs_mft_data_extend_allocation`,
  loop at 1285), raises `initialized_size`/`data_size` one record at a time and formats each new record
  (`ntfs_mft_record_format`, 1306-1312, a single write of a record >= 16, no mirror), writes the new sizes into
  the `$DATA` attribute record of record 0 (1330-1332), marks `$MFT` dirty (1335) and syncs it immediately
  (`ntfs_inode_sync(mft_na->ni)`, 1349-1350).
- `ntfs_inode_sync()` -> `ntfs_inode_sync_in_dir()` -> `ntfs_mft_record_write()` (`inode.c:1032`,
  `include/ntfs-3g/mft.h:85-91`) -> `ntfs_mft_records_write(vol, 0, 1, b)` (`mft.c:144`): the record is copied
  for the mirror (187), written to `$MFT` with `ntfs_attr_mst_pwrite` (189) and then, if that write succeeded
  (201), written to `$MFTMirr` (204). Two device writes, no sync between them, no ordering barrier.
- Every later mount, read-only included, reads records 0..3 of both copies and fails on any byte difference
  within the record's used size (`volume.c:1038-1046`, "$MFTMirr does not match $MFT (record %d)"). The compare
  is after fix-up, so it includes the USN and fails even if only the USN differs.

In these workloads every write of record 0 comes from this path: record-0 writes per workload equal the number
of new MFT records (smallfiles 148 = 8 directories + 140 files; overwrite_truncate 13 = 1 + 12; seqwrite 2), and
every one of them is immediately followed by the mirror write.

### The cut is between the two writes (trace, all 18)

| Workload | Cut N | Write N | Write N+1 (not issued) |
|---|---|---|---|
| smallfiles | 68, 558, 693, 760, 1283, 1385 | 0x4000, 1024 B (`$MFT` rec 0) | 0x1fff000, 1024 B (`$MFTMirr` rec 0) |
| renamechurn | 70, 263, 305, 415, 471, 1162 | same | same |
| dirtree | 276, 459, 2292, 2338, 3255, 3805 | same | same |

The 11 failures of the short run (smallfiles 76, 226, 864, 1089; renamechurn 1015, 1138, 1384; dirtree 510, 1020,
1326, 4079) are also record-0 writes in the trace, with the same message. Replay: `cut:68`, `cut:70` and `cut:276`
failed again with the same message; their eight neighbours each passed `ntfsfix -n`. The workload event logs
carry claims only (no device offsets), which is why the trace was needed.

Window size per workload (record-0/mirror pairs over all device writes): smallfiles 148/1841 (8.0 %),
renamechurn 60/1508 (4.0 %), dirtree 378/4997 (7.6 %), overwrite_truncate 13/1000 (1.3 %), seqwrite 2/25664.
Expected hits for 110 evenly spaced cuts: about 23; observed 18.

### What differs in record 0 (decoded)

Records 1..3 are identical in all 18 images. Record 0 differs in 5, 8 or 9 raw bytes. Raw offsets 0x30, 0x1fe and
0x3fe are the USN (update sequence array and the two sector trailers). The rest is the `$DATA` attribute at
record offset 0x100: 0x118 highest VCN, 0x128-0x12f allocated size, 0x130 data size, 0x138 initialized size,
0x141 the length byte of the single run. `$BITMAP` of `$MFT`, the header (LSN 0, sequence 1, flags in use,
408 bytes used) and every other attribute are equal.

| Image | USN `$MFT` / mirror | Data = initialized size `$MFT` / mirror | Allocation and run `$MFT` / mirror |
|---|---|---|---|
| smallfiles-c68 | 6 / 5 | 69632 / 68608 (68 / 67 records) | equal |
| renamechurn-c70 | 6 / 5 | 69632 / 68608 | equal |
| dirtree-c276 | 31 / 30 | 95232 / 94208 | 110592 / 94208; run (LCN 4, 27 clusters) / (4, 23); highest VCN 26 / 22 |
| smallfiles-c558 | 47 / 46 | 111616 / 110592 | 126976 / 110592; run 31 / 27 clusters |
| smallfiles-c760 | 63 / 62 | 128000 / 126976 | 143360 / 126976; run 35 / 31 clusters |
| other 13 | mirror + 1 | `$MFT` one record (1024) larger | equal |

In every image `$MFT` is exactly one `ntfs_mft_record_init()` step ahead: one more record initialized (and in three
images one more 16-record allocation chunk). The newly initialized record is formatted but not in use (highest
in-use record = new initialized count - 2 in all 18). Both `ntfs_mft_records_write()` copies are protected from the
same buffer, so a completed pair has equal USNs; mirror USN = `$MFT` USN - 1 means the mirror still holds the
previous pair.

### Upstream property, not a fault-layer artefact

- The two copies are two `pwrite` calls (`mft.c:189`, `mft.c:204`). Nothing in NTFS-3G orders or flushes between
  them. A power loss or unplug after the first reached the medium, a `SIGKILL` or crash of `ntfs-3g` between the
  two calls (no power loss needed: the second call was never made), or a drive cache that persists only one of
  them produces this image. The sweep's kill-before-write-N+1 is the most benign of these.
- Counter: write and pwrite share one counter (`faultio_write`, both macros); the libc-level trace counts the same
  number of writes in all five workloads, so no write bypasses the layer and none is double-counted.
- No partial sector: write N is a complete 1024-byte write in all 18 cases (plan `cut`, `keep == count`), and the
  fix-up of both copies verifies. The fault layer drops nothing before N and kills before N+1 is issued.
- Reproducible and deterministic: same cut, same outcome in a fresh run.

Classification (TEST_STRATEGY section 6): (a) expected non-atomic window of an engine without a journal. It is not
(c): no ntfs-macos patch is on the path. The refusal of a read-only mount over a stale mirror is worth an upstream
report (see the list at the end), but the write order itself is not fixable: whichever copy is written first, a
cut between the writes leaves them different (as ARCHITECTURE.md section 3.6 already says for record 3).

## 2. Consequence for the product

Today: probe check 2 (`ntfs_mount` read-only) fails -> `corrupt` -> policy `refuse`, for any attach after such a
crash, on any Mac and on Linux NTFS-3G. The user's data is intact (finding 4) but unreachable through this product
until the mirror is repaired. Because the window opens on every MFT growth, the exposure is per created file on a
young volume, not "two writes per session edge" as R-33 assumes for volmark; and the daemon's own `SIGKILL` of a
hung worker can produce it, not only power loss. D3 promises read-only on the next attach; for this outcome the
result is refuse. Refuse is safe for the disk; it is an availability failure, not a data-loss one.

**Windows `chkdsk`.** The mirror is a recovery copy that `chkdsk` rebuilds; upstream says so itself:
`ntfsfix.c:509-511` ("chkdsk.exe mirrors it fully, so we do similarly") and `volume.c:997` (Windows 10 no longer
updates the full `$MFTMirr`). The usual message is "Correcting errors in the master file table's (MFT) mirror".
Not verified here (no Windows in A): it belongs in U15 / `H-USB-10`, extended from record 3 to record 0. The
upstream comment also suggests Windows itself may leave the mirror different without any crash; whether NTFS-3G
then refuses a volume Windows considers healthy is open (worth a fixture in `H-USB-*`).

**`ntfsfix` without `-n`** (`fix_mftmirr`, `ntfsfix.c:449-596`): compares records 0..mftmirr_size-1 and rewrites
the differing record from `$MFT` unless `$MFT`'s copy is not in use and has no valid magic (`use_mirr`,
`ntfsfix.c:563-566`); a torn `$MFT` copy (fix-up failure) aborts with "Cannot handle this yet" (`ntfsfix.c:531-538`),
so it never repairs the "primary torn" case. On a scratch copy of smallfiles-c68 it changed exactly four 1 KiB
regions: `$MFT` record 0 (rewritten, USN + 1), `$MFT` record 3 (`VOLUME_IS_DIRTY` set, `set_dirty_flag`,
`ntfsfix.c:295`), and the mirror copies of both; afterwards `ro,norecover` mounts and `ntfsfix -n` exits 0. It also
ran `empty_journal()` (`ntfsfix.c:330-340`, not guarded by `-n` or by the log state), which here changed no byte
only because this `$LogFile` was already in the reset state. On a volume with a live Windows `$LogFile` it would
discard it, which is exactly what `recover` does and what D4 bans. Its own repair also writes record 0 and 3 as
`$MFT`-then-mirror pairs, so a cut during the repair recreates the condition (it is idempotent, a rerun fixes it).
Conclusion: safe for the data in this specific state, not safe as a general tool, and never to be run by the
product.

**`ntfsfix -n` is not a pure reader of this state.** In `-n` mode it prints "Correcting differences in $MFTMirr
record 0...OK" and "Going to empty the journal ($LogFile)... OK", yet the image sha256 is unchanged (checked). Its
exit 1 comes from the final `ntfs_mount(opt.volume, 0)` (`ntfsfix.c:1649`), a read/write mount attempted in `-n`
mode, which fails only because the mirror still differs. Harmless on the sweep's scratch copies; the sweeper must
not take `-n` output text as evidence of a repair.

**Is the newer copy always `$MFT`?** In the sweep, yes (18 of 18), because writes persist in issue order. Not in
general: (i) a drive cache without a flush between the two writes may persist the mirror and lose `$MFT`; (ii) a
torn `$MFT` write leaves the mirror as the only valid copy (18 such points in torn512); (iii) Windows may leave the
mirror older by design (above). Both copies are internally consistent in the observed case: copying the mirror
over `$MFT` (rolling record 0 back one step) also gave a clean `ro` mount, clean walk, all claims and
`ntfsfix -n` 0 for all 18 images, because the extra record is not yet in use. That will not hold in general: an
older record 0 can describe an MFT that ends before a record already in use.

**Options for the owner** (safety rules: no silent auto-repair; `recover` and `remove_hiberfile` stay banned;
`rw` only from a clean volume):

| Option | What | For | Against |
|---|---|---|---|
| A. Keep `refuse` | No change; document. Remedy: Windows `chkdsk`. | No new code; probe stays simple; nothing writes. | User loses access on the Mac after a common crash; D3 text must get an exception; frequency is not "rare" on young volumes. |
| B. Dedicated probe state, read-only access | New state (e.g. `mirror_split`, reason `MFTMIRR_MISMATCH`) only when: the mount fails solely at the mirror compare; records 0..3 of `$MFT` pass fix-up and `ntfs_mft_record_check`; the mirror records pass fix-up or differ only in records 0..3; and a read-only mount that skips the compare passes every other probe check. Policy `ro` (never `rw`), notification "volume needs repair; files are readable". Needs a patch that lets a `NTFS_MNT_RDONLY` mount tolerate the mismatch with a warning (or a read-only device-layer view that serves `$MFT` bytes for the mirror records); the worker gets the same. | Restores D3; no write to the disk; the engine still uses `$MFT`, as Windows does. | New engine patch (upstream review, R-06); a mirror-newer case (reordering) reads through a stale record 0 (files beyond the old MFT end missing in the view, read-only so no damage); more probe logic on untrusted data. |
| C. Explicit repair command | `ntfsmac repair --sync-mirror` under ADR-017 rules (typed confirmation, admin peer, ADR-019, volume unmounted, probe state exactly `mirror_split`, `logfile` clean, `hiberfile` absent or inactive). Copies only the differing records, as raw protected bytes, from the copy that is valid and newer (valid fix-up and record check, then USN order); refuses if both are valid and the order is unclear or the newer one is the mirror. One write per record to the mirror only, `F_FULLFSYNC`, read-back compare. Leaves `VOLUME_IS_DIRTY` set so Windows still checks. | Brings the volume back without Windows; much narrower than `ntfsfix` (no `$LogFile` reset, no rewrite of `$MFT`). | A writer on a volume the product has just classified as damaged; its own write can be cut (retry is safe); USN order is a heuristic. |
| D. Automatic repair on attach | Run C without asking. | Seamless. | Violates "never auto-repair silently". Listed only to reject it. |

B and C combine (B for access, C or `chkdsk` to return to `rw`). Whatever is chosen, the notification for this
state should say that the data region was not touched and name `chkdsk` as the reference remedy, as R-33 already
does for volmark.

## 3. Criterion 3.9

Intent (as I read 3.9 and TEST_STRATEGY section 6): after any cut the volume is still readable and not
structurally broken, and synced data survives. The 18 points meet the second and third parts (finding 4: nothing
but a stale recovery copy, every claim intact) and fail the first only because NTFS-3G treats a stale mirror as
fatal at every mount. `ntfsfix -n` exit 1 here is not a fatal error in the sense of damaged metadata: `ntfsfix`
classifies it as a correctable difference and corrects it from `$MFT`.

Recommendation: count them as **expected, classified window (class a)**, not as a pass and not as an unexplained
failure, with a mechanical rule and a required mitigation decision:

1. A cut point is `mirror_split` (classified) only if all hold: the `ro,norecover` refusal and the `ntfsfix -n`
   message are exactly "$MFTMirr does not match $MFT (record k)" with k in 0..3; write N of the trace is `$MFT`
   record k and write N+1 is `$MFTMirr` record k; and on a scratch copy with the mirror record replaced by the raw
   `$MFT` record the strict checks pass (`ro` mount, `ntfsfix -n` exit 0, all claims). Anything else stays a failure.
2. 3.9 passes when every cut point passes or is `mirror_split`, all controls pass and no claim is broken; the
   summary reports the `mirror_split` count next to pass and fail.
3. M3 gate: the owner records the decision on section 2 (option A with the D3 exception and R-33 widened, or B/C
   as tasks). Until then the classified window counts as an open product defect in the risk register, not as a
   closed test failure.

CI short stage (`linux-crash-sweep-short`): exit 0 iff controls 10/10, zero unclassified failures, zero broken
claims, zero workload hangs; `mirror_split` and `walk_dirty` reported, not failing. Do not pin the count (11 today):
it follows NTFS-3G's write sequence and would break on any engine or workload change; the rule above already
proves each one. Optionally fail if `mirror_split` exceeds the number of record-0..3 pairs among the sampled cuts,
which is impossible if the rule is applied and so catches a broken classifier.

## 4. Patch 0004 review

**Release build unaffected.** `tests/results/evidence/m314/full/build-proof.txt` shows nm and strings clean for
the release `libntfs-3g.a`, `ntfs-3g`, `ntfsfix`, `mkntfs`, the release `unix_io.o` equal to a 0003-only reference
(objdump of `.text .rodata .data` and nm, same configure line) and the positive control. `build.sh` ran five
times during this triage (each experiment rebuilds in a fresh container), PASS each time. The non-faultio branch of `faultio.h` is three macros expanding to the
upstream calls; the include replaces a blank line, so no line moves (`__LINE__` and debug output unchanged).

**Parser** (`faultio_init`). Bounded: fixed prefixes with `strncmp`, every number must start with a digit (no sign,
no space, no `0x`), trailing text rejected (`!*end`), `eio`/`dead`/`short` reject N = 0, an unknown plan becomes
`FAULTIO_BAD` (every write fails `EINVAL`: fails closed). No buffer is written from the plan. Gaps (test-only,
low): `strtoull` overflow is not checked, so `cut:99999999999999999999` saturates to `ULLONG_MAX` and silently
behaves like `count`; check `errno == ERANGE`. `short:N:0` is accepted (tears to zero bytes, i.e. `cut:N-1` with the
kill at N); fine but undocumented.

**Counters.** One `unsigned long long`, `__atomic_add_fetch`, shared by `write()` and `pwrite()`; `ntfs-3g` runs
single-threaded `fuse_loop()` (`src/ntfs-3g.c:4579`), so ordering is exact. The state file is written before the
device write, so the counter a workload reads after `fsync` is at least the true value (claims are conservative).
Verified against the independent trace (section "Method"). The state-file `pwrite` uses libc directly and is not
counted (trace confirms).

**KILL.** `kill(getpid(), SIGKILL)` before write N+1 (cut) or right after the partial write (short); `atexit`
report is skipped, which is intended. The `cut:0` control proves the first write is intercepted and that the mount
itself issues no write. Without `KILL` the documented "drop and report success" mode lets the process read stale
metadata; the sweep correctly does not use it.

**No effect without the variable.** `NTFSMAC_FAULT_PLAN` unset or empty: `kind = NONE`, straight to the real call,
no state file opened, no `atexit`. Cost is one `getenv` on the first write.

**Other findings.**

| Sev. | Item |
|---|---|
| Low | `NTFSMAC_FAULT_STATE` is opened `O_WRONLY|O_CREAT` without `O_NOFOLLOW`, from the environment, in whatever process links the library. A faultio `ntfs-3g` installed setuid root (some distributions do that for `ntfs-3g`) would let any user create or overwrite 21 bytes at offset 0 of any file. Test-only, but cheap to close: ignore all `NTFSMAC_FAULT_*` when `getuid() != geteuid()` (or use `secure_getenv`), add `O_NOFOLLOW`. |
| Low | The partial-write path ignores a short return of the real `pwrite` (`r < keep`) and reports `count`. Irrelevant for regular files; note in the header. |
| Low | The sweep's fault model is "writes persist in issue order" (the image is a file, the kernel keeps every issued write after `SIGKILL`). Real devices without a flush may persist any subset; the mirror-newer case of section 2 and other reorderings are never exercised. Suggest a plan `skip:N` (drop write N only, keep the rest up to a later cut) as a supplementary reordering sweep. |
| Info | `faultio.h` is not added to `include/ntfs-3g/Makefile.am`, so it is not installed with the headers. Correct (internal), worth a sentence in the patch header. |

**`scripts/build-ntfs3g.sh --configure-arg`.** No command injection: the value is never `eval`ed; it is appended
to `configure_args` and expanded unquoted (`./configure $configure_args`, line 201), so only field splitting and
pathname expansion apply, and whitespace (including newline and tab, `[[:space:]]`) is rejected at line 64.
Remaining issues:

1. Globbing: no `set -f`, so a value with `*`, `?` or `[` expands against the files in `$src` (the cwd of
   configure). Add `set -f` in that subshell, or reject `*?[` in the value.
2. Any argument is accepted, including `--prefix=...` (configure's last one wins, overriding the script's own),
   `CC=...` or `LIBS=...`. Not a privilege boundary (the caller runs the script), but it defeats the intent.
   Suggest an allow-list: `--enable-faultio` only (plus `--enable-debug` if wanted).
3. `--configure-arg --enable-faultio` with the default `--prefix` installs the fault library into `build/deps`, the
   product's default dependency prefix. The stamp includes the configure line, so the next plain run rebuilds,
   but anything that links `build/deps/lib/libntfs-3g.a` before that run gets the layer. Refuse
   `--enable-faultio` unless both `--prefix` and `--build-dir` are given and differ from the defaults, and make
   the packaging step run the same nm/strings check on the final probe, volmark and worker binaries (no CMake or
   packaging target consumes `build/deps` yet, so this is the moment to add it).

Verdict: patch 0004 is fit for its purpose; the three low items and the script fixes can follow in the same task.

## 5. Torn writes (`short:N:512`)

"Incomplete multi-sector transfer" is NTFS working as designed: every 1 KiB MFT record and 4 KiB index block
carries the update sequence number in the last two bytes of each 512-byte sector, and a record whose sectors do
not all carry it is rejected. In torn512 the 204 failures map exactly onto torn multi-sector structures (trace by
buffer magic): torn `FILE` in `$MFT` records 0..3 (18) or in the mirror (23) -> mount refused; torn `INDX` of a
system index (48, `$Secure`, so `ntfsfix` "Failed to open $Secure") -> mount refused; torn `INDX` of a user
directory (50) or torn `FILE` of a user file (65) -> claims broken (for example all of `big.bin` unreadable,
including the prefix that was synced before). Torn data clusters (140) never failed. Windows survives the same
tear by redoing the record from `$LogFile`; NTFS-3G with `norecover` has no log to redo from, so the tear is
permanent until `chkdsk`. For the product this means fsync durability (D1, claims) holds only for devices that
write a 4 KiB block atomically; a 512-byte tear inside an aligned 4 KiB write is stricter than most 512e and 4Kn
drives behave, but USB bridges and flash without power-loss protection give no such promise. Class (a); belongs in
LIMITATIONS next to N4, not in 3.9.

## Recommended changes (for the owner; no design document was edited)

1. IMPLEMENTATION_PLAN 3.9: add the `mirror_split` classification rule of answer 3 and the M3-gate decision.
2. TEST_STRATEGY section 6: name the `mirror_split` class and its mechanical rule; define check (3) "`ntfsfix -n`
   no fatal error" as exit 0, or exit 1 with a classified `mirror_split`; state the sweep's fault model (in-order
   persistence) and add an optional `skip:N` reordering sweep.
3. Sweeper: record offset, length and magic of writes N and N+1 (a trace from the `count` run, or a
   `NTFSMAC_FAULT_TRACE` file in the layer) and classify automatically; implement the scratch-copy mirror check;
   report `mirror_split`. README: note that `ntfsfix -n` attempts a read/write `ntfs_mount` (`ntfsfix.c:1649`) and
   prints "OK" for corrections it did not write.
4. ARCHITECTURE section 3.6 and R-33: the mirror window is a property of every read/write session that grows the
   MFT (`mft.c:1350`) and of any worker crash or `SIGKILL`, not only of volmark; widen R-33 or add a risk.
5. ARCHITECTURE section 9 D3: either add the exception "unless the `$MFT`/`$MFTMirr` copies differ (refused)" or
   adopt option B; section 3.1/3.2: new state and reason code if B is chosen.
6. ADR-017: `repair --sync-mirror` if option C is chosen; record explicitly that `ntfsfix` is not a product repair
   path (it resets `$LogFile` unconditionally).
7. U15 / `H-USB-10`: extend to record 0 and to a mirror left stale by Windows itself; take
   `smallfiles-c68.img.gz` (64 MiB image, only record 0 differs) as fixture `mftmirr-split-rec0`; ask the same of
   Apple's read-only `ntfs.fs`.
8. LIMITATIONS: torn multi-sector writes (answer 5) and the mirror window.
9. Upstream report (3.9 asks for it): "read-only mount refuses a volume whose `$MFTMirr` is one sync behind
   `$MFT`; `ntfs_mft_records_write()` cannot avoid that window; suggest a warning instead of a refusal for
   `NTFS_MNT_RDONLY` when the `$MFT` records are valid", with the decoded image as evidence.
10. Patch 0004 and `build-ntfs3g.sh`: the items in answer 4.
