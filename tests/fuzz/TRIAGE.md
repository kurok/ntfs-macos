<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Fuzz triage: F-PROBE and F-MOUNT-WALK (T-M3-16, criterion 3.11)

Harnesses: `tests/fuzz/image/` (see the header of `fuzz_probe.c`, `fuzz_mountwalk.c`, `image.h`).
Runs: `tests/fuzz/image/run.sh`, Environment A (Debian container, clang 19.1.7, libFuzzer,
ASan + UBSan, aarch64), libntfs-3g 2026.9.28 with the release patch series 0003, 0006, 0007,
0008, 0009 (0004 is applied by the build script but its code is compiled only with
`--enable-faultio`, which is not used), built with the same instrumentation. The runs of
2026-11-15 below ran on that series; the runs of 2026-11-16 ran on the series with 0010 to 0013.
Results of the first runs: `build/fuzz/image/hour1/`, `build/fuzz/image/hour2/` (git-ignored, no
longer present). The final statistics, summaries, leak lists and the UBSan list of the runs of
2026-11-16 are in `tests/results/evidence/m316/` (F-PROBE, F-MOUNT-WALK) and
`tests/results/evidence/m319/` (F-SANITIZE-01..03).

## Runs

| Target | Start | End | CPU time | Executions | Result |
|---|---|---|---|---|---|
| F-PROBE | 2026-11-15T14:33:42Z | 15:34:04Z | 3601 s, one process | 4,029,820 (1119 exec/s) | no crash, no oracle failure, no timeout; corpus 4951 inputs, 3367 edges |
| F-MOUNT-WALK, run A | 14:33:42Z | 15:04:35Z | 1826 s in 60 processes | 1,264,689 | stopped by the restart limit (60 restarts) after F1 and F3 ended every few seconds of fuzzing; shows F1, F3 |
| F-MOUNT-WALK, run B (`--skip-security`) | 15:34:22Z | 16:34:44Z | 3603 s in 7 processes | 3,585,398 (1319 exec/s) | 6 crashes, all F4; corpus 3931 inputs, 4049 edges |
| F-PROBE, run 2026-11-16 | 2026-11-16T09:43:00Z | 10:43:15Z | 3601 s, one process | 3,722,680 (1033 exec/s) | no crash, no oracle failure, no timeout; corpus 5409 inputs, 3630 edges; leak replay: one engine leak, F7 |
| F-MOUNT-WALK, run 2026-11-16 (security reads on, engine with 0010 to 0013) | 09:43:00Z | 10:43:15Z | 3601 s, one process | 2,014,880 (559 exec/s) | no crash, no timeout, no restart; corpus 5708 inputs, 4756 edges; leak replay: F5 only |
| F-SANITIZE-01 verdict | 09:43:00Z | 10:43:02Z | 3601 s, one process | 286,328,494 (79,513 exec/s) | no finding; corpus 2427 inputs, 581 edges |
| F-SANITIZE-02 label | 09:43:00Z | 10:43:02Z | 3601 s | 38,189,056 (10,605 exec/s) | no finding; corpus 986 inputs, 363 edges |
| F-SANITIZE-03 display | 09:43:00Z | 10:43:02Z | 3601 s | 18,696,629 (5,192 exec/s) | no finding; corpus 876 inputs, 172 edges |

Criterion 3.11: one CPU-hour on the probe is met with no finding in project code. The
mount-and-walk hour is met in CPU time, with F1 and F3 (security descriptor reads) left out
of run B and F4 ending each of its processes (6 restarts, 3603 s in all). Because F4 recurs,
the part of the engine behind it was explored less in run B than the time suggests; see
"Open".

The run of 2026-11-16 is the second F-MOUNT-WALK hour on the engine with fixes 0010 to 0013 and without
`--skip-security`: F1, F3 and F4 no longer stop it (one process for the whole hour, no restart), and the
security descriptor code is reached (gcov of the replayed corpus: `security.c` 19.0 % of 1703 lines against
6.8 % in run B, `acls.c` 46.2 %; `reparse.c` 26.8 %, `dir.c` 33.1 %, `index.c` 26.2 %, `xattrs.c` 33.9 %). The
probe target ran with `probe/names.c` carrying the zero-header rule of audit fix F-05 (uncommitted at run time,
committed with this evidence); `policy.c` and `volmark/args.c` are not in any fuzz target. Nothing in project code crashed, leaked or
broke an oracle in any of the five targets.

Project code: nothing in `src/probe/*`, `src/common/mftmirr.c` or the verdict parser crashed,
leaked or broke an oracle in 4.0 million executions. The probe's oracles: exit status OK for
any readable image, verdict passes `ntfsmac_probe_verdict_validate()`, serialises, the daemon's
strict parser accepts the text and re-serialises it to the same text, `clean` only for a whole
NTFS volume, no write request to the device layer. No change to project code was needed.

Coverage (gcov, replay of the grown corpus, lines executed): probe 77.8 %, names 87.6 %, json
94.1 %, nfc 95.6 %, text 88.0 %, mftmirr 79.8 %, state 74.2 %, devio 49.3 % (the rest of devio
is the raw-device path and the overlay edge cases that need a block device). Under
F-MOUNT-WALK run B: bootsect 96.3 %, volume 44.2 %, dir 33.4 %, reparse 34.5 %, runlist
29.7 %, unistr 48.7 %, xattrs 32.2 %, index 23.0 %, compress 20.2 %, attrib 17.6 %, inode 18.1 %,
security 6.8 % (skipped by design in run B), mft 9.1 % (mostly write paths).

## Findings

All findings are in NTFS-3G (libntfs-3g). Filed upstream 2026-11-16 as private GitHub security
advisories, state "triage": F3 GHSA-g3h3-7rhv-rrmq, F4 GHSA-vp2x-xjmj-x4x2, F1 GHSA-r4cv-86xq-37qj,
each with a standalone image and a reproduction on the unmodified 2026.9.28 release (valgrind;
F3 and F1 through a small libntfs-3g program, F4 through `ls` on an `ntfs-3g` mount).
Fixed locally (2026-11-16, T-M3-25, ADR-026): F3 by patch 0010, F4 by patch 0011, F1 by patch 0012,
carried until a fixed NTFS-3G release is pinned. Regression: ctests `fuzz.findings.F1`, `fuzz.findings.F3`,
`fuzz.findings.F4` (one replay each of the reproducer through the harness, security reads on; F2 is not
included), registered only for an ASan build. Verified in Environment A on the library without the patches
(`--no-patches`) and with them: F1 and F4 fail unpatched in every ASan build tried (clang and gcc, -O0 and
-O1, with and without UBSan); F3 fails unpatched in clang -O0/-O1, gcc -O0 and gcc -O1 with
`address,undefined`, but PASSES unpatched in gcc -O1 with `-fsanitize=address` alone (the asan stage of
`tests/unit/probe/inner.sh`: the compiler removes the loads at acls.c:682-685). There F3 is checked by
valgrind on the standalone image (invalid read of size 4 at acls.c:682 unpatched, none patched). With the
patches all three pass in every build. The tables and texts below describe the findings as made against the
unmodified release. F6 (below) was found in the review of the patches and has no replay. F2 and F5
(leaks) are drafted as public issues and not yet filed. The draft texts below are the originals. Reproducers are inputs of the harness (a recipe for an image, see
`tests/fuzz/image/image.h`); replay: build `fuzz_mountwalk` (`run.sh` does, in the container)
and run `fuzz_mountwalk tests/fuzz/image/findings/FILE`. To get a standalone image for an
upstream report, write the image that `fz_image_build()` makes for the input to a file.

| Id | Where | Kind | Severity | Reproducer | Reachable from the probe |
|---|---|---|---|---|---|
| F1 | `security.c` `retrievesecurityattr()` (line 1832) | allocation of an on-disk size, up to 4 GiB | low (resource use) | `tests/fuzz/image/findings/F1-getsecurityattr-alloc.bin` | no (probe never reads descriptors) |
| F2 | `volume.c` `ntfs_mft_load()` (line 365 and the error exit after it) | memory leak, 4096 bytes | low | `findings/F2-mft-load-runlist-leak.bin` | yes (any failed mount of a bad `$MFT`), probe is a short-lived process |
| F3 | `acls.c` `ntfs_valid_descr()` (line 682), from `getsecurityattr()` (security.c:1894) | heap-buffer-overflow, read of 4 bytes | medium | `findings/F3-valid-descr-heap-overflow.bin` | no |
| F4 | `reparse.c` `ntfs_possible_symlink()` (line 928), from `ntfs_filldir()` (dir.c:923) during `ntfs_readdir()` | heap-buffer-overflow, read of 4 bytes | medium | `findings/F4-possible-symlink-heap-overflow.bin` | not in 4.0 million probe executions (the probe's walk calls `ntfs_readdir()`; not proven unreachable) |
| F5 | `reparse.c` `ntfs_make_symlink()` (line 814 and following) | memory leak, 56 bytes | low | found only by the leak stage of run B (`hour2/mountwalk/leaks.log.sym`), corpus input in `hour2/mountwalk/corpus/` | no |
| F6 | `inode.c` `ntfs_inode_sync_file_name()` (lines 815 to 822) | heap out-of-bounds read of 4 bytes (reparse tag of a non-resident or short attribute, no resident or length check), written into the index entries of the file's names | low to medium (read-write mounts only) | none (found by code reading in review; no image built) | no |
| F7 | `volume.c` `ntfs_device_mount()` (line 1240, `ni = ntfs_inode_open(vol, FILE_AttrDef)`) | memory leak, 136 + 4096 bytes: the `$AttrDef` inode is not closed when `ntfs_attr_open(ni, AT_DATA, ...)` (or a later step of the `$AttrDef` load) fails, because `error_exit` calls only `__ntfs_volume_release()` | low | `findings/F7-attrdef-inode-leak.bin` (9 bytes; `fuzz_probe` and `fuzz_mountwalk` with leak detection on) | yes (any failed mount with a bad `$AttrDef`), probe is a short-lived process |
| F8 | `volume.c` `ntfs_mft_load()` (line 265, `ntfs_malloc(vol->mft_record_size)`), from `ntfs_volume_startup()`; the size is set in `bootsect.c` `ntfs_boot_sector_parse()` | allocation of an on-disk size, up to 2 GiB (libFuzzer: out-of-memory, `malloc(1073741824)`); `1 << 31` overflows `int` | low (resource use; denial of service on mount) | `findings/F8-mft-record-size-alloc.bin` (9 bytes) | yes (every mount), probe is a short-lived process |
| F9 | `dir.c` `ntfs_inode_lookup_by_name()` (line 380) and `ntfs_readdir()` (line 1283), `ntfs_malloc(index_block_size)`; `index.c` `ntfs_index_lookup()` (`icx->block_size`) and `ntfs_ir_to_ib()`; the size is read from `INDEX_ROOT.index_block_size` | allocation of an on-disk size, up to 2 GiB per directory open (ASan: allocation-size-too-big at 1 GiB under a 512 MB limit) | low (resource use; denial of service) | `image/findings/F9-index-root-block-size-alloc.bin` (12 bytes) | no (a campaign needs a mutation of `index_block_size` of a directory with an index allocation, reached by the walk) |
| U | `bootsect.c` 229, 231, 237, 275 (236, 238, 244 with patch 0015, which moves them by seven lines); `volume.c` 273, 624, 1094; `runlist.c` 862, 882, 897, 961; `attrib.c` 385, 386, 1040, 1073, 1172; `unistr.c` 183, 188, 345, 376; `reparse.c` (misaligned loads) | undefined behaviour: left shifts of negative or large values, signed overflow, negative shift exponent, misaligned `ntfschar` loads | low (benign on arm64 and x86-64 with the usual compilers) | UBSan reports in `hour*/*/libfuzzer-*.log.sym`, list in `ubsan-engine.txt` | yes (bootsect, volume, runlist, unistr hit by the probe too) |

Why they do not touch the probe: the probe opens the device O_RDONLY, mounts read-only,
reads at most the first 4 KiB of `hiberfil.sys` and lists names. It never calls the security
descriptor, extended attribute, reparse-data or stream code. The FUSE side (ntfs-3g
`getxattr`, `readlink`, `readdir` with `permissions`) does, which is why F1, F3, F4 and F5
matter for the mounted volume, behind the probe verdict.

### F1: unbounded allocation from the security descriptor length

`retrievesecurityattr()` takes `datasize` from the `$SII` index entry (a 32-bit value on
disk), subtracts the header size and passes the result to `ntfs_malloc()` before it knows
that the `$SDS` stream holds that many bytes. A corrupt `$Secure` makes it ask for up to
4 GiB; the read then fails and the buffer is freed. ASan stops on the allocation size
("allocation-size-too-big"), libFuzzer's limit on "out-of-memory (malloc(4294967147))".

Draft upstream report: *libntfs-3g: retrievesecurityattr() allocates an attacker-chosen
size.* In `libntfs-3g/security.c`, `retrievesecurityattr()` computes
`size = le32_to_cpu(psii->datasize) - sizeof(SECURITY_DESCRIPTOR_HEADER)` from the `$SII`
entry and calls `ntfs_malloc(size)` before comparing `offs + size` with the size of the `$SDS`
data attribute. With a crafted volume the allocation is up to 4 GiB (found by libFuzzer with
ASan, through `ntfs_get_ntfs_acl()`, i.e. `getxattr system.ntfs_acl`; the same path is used
by every permission check). Suggested fix: reject `datasize` larger than the `$SDS` data size
minus `dataoffs` (and smaller than the header) before allocating. Version 2026.9.28.

### F2: run list of `$MFT` leaked when the MFT is not self-contained

In `ntfs_mft_load()` the run list `nrl` returned by `ntfs_mapping_pairs_decompress()` is
stored in `vol->mft_na->rl` only after the check `nrl->lcn != vol->mft_lcn`; when that check
fails the code goes to `error_exit` without freeing `nrl` (4096-byte block allocated at
runlist.c:820). Draft upstream report: *ntfs_mft_load() leaks the decompressed run list on
"The MFT is not self-contained".* Free `nrl` before `goto error_exit` on that path (and on the
`ntfs_mapping_pairs_decompress()` failure path no allocation remains). Version 2026.9.28.
(Not a security issue; the library is called once per mount.)

### F3: `ntfs_valid_descr()` reads the descriptor header before checking the length

`ntfs_valid_descr(securattr, selflen)` reads `phead->dacl`, `sacl`, `owner` and `group`
(acls.c:677 to 682) before it compares `selflen` with the header size. A `$SECURITY_DESCRIPTOR`
attribute of one byte, returned by `ntfs_attr_readall()` (security.c:1892), makes the read run
past the allocation. Draft upstream report: *ntfs_valid_descr() reads past the end of a short
security descriptor.* In `libntfs-3g/acls.c`, `ntfs_valid_descr()` dereferences the header of
`securattr` before checking `selflen >= sizeof(SECURITY_DESCRIPTOR_RELATIVE)`. A v1 volume
with a one-byte `$SECURITY_DESCRIPTOR` attribute gives a heap-buffer-overflow read of 4 bytes
via `getsecurityattr()` (security.c:1894) from `ntfs_get_ntfs_acl()` and every permission
check. Suggested fix: return FALSE first when `selflen < sizeof(SECURITY_DESCRIPTOR_RELATIVE)`.
Version 2026.9.28.

### F4: `ntfs_possible_symlink()` reads the reparse tag without checking the size

`ntfs_possible_symlink()` tests `reparse_attr && attr_size` and then reads
`reparse_attr->reparse_tag` (4 bytes at offset 0). With a one-byte `$REPARSE_POINT` attribute
the read is 3 bytes past the allocation (reparse.c:925 allocation, :928 read). It runs in
`ntfs_filldir()` for every directory entry whose file has the reparse flag, so a plain
`ls` of a crafted volume reaches it. Draft upstream report: *ntfs_possible_symlink() reads
beyond a short $REPARSE_POINT attribute.* Replace `attr_size` with
`attr_size >= (s64)sizeof(le32)` (or `offsetof(REPARSE_POINT, reparse_data)`), as
`ntfs_make_symlink()` does through `valid_reparse_data()`. Version 2026.9.28.

### F5: `ntfs_make_symlink()` leaks the attribute buffer on invalid reparse data

`free(reparse_attr)` sits inside the `if (reparse_attr && attr_size && valid_reparse_data(...))`
block (reparse.c, about line 903), so an attribute that fails `valid_reparse_data()` is leaked
(56 bytes in the run). Draft upstream report: *ntfs_make_symlink() leaks the reparse
attribute when valid_reparse_data() fails.* Move the `free()` out of the `if`.
Version 2026.9.28.

### F6: `ntfs_inode_sync_file_name()` reads the reparse tag without a resident or length check

Found in the review of patches 0010 to 0012; the same pattern as F4. `ntfs_inode_sync_file_name()` looks up
`$REPARSE_POINT` and reads `rpp->reparse_tag` at `attr + value_offset` with no test of `non_resident` or
`value_length`; `ntfs_attr_inconsistent()` has no case for this attribute and skips its `value_offset` test for
a zero length. For a non-resident attribute the `value_offset` field is part of `lowest_vcn`, so the read can
be up to 64 KiB from the attribute. Reached from `ntfs_inode_sync()` for an inode with the file-name-dirty
flag, so on read-write mounts only. The tag read is stored in the index entries of the file's names. Fixed
locally by patch 0013 (not yet reported upstream: draft `build/upstream-reports/05-security-inode-sync-reparse.md`,
git-ignored). No reproducer was built and no replay reaches it (the harness mounts read-only).

### F7: `$AttrDef` inode leaked when its attribute cannot be opened

Found by the leak replay of the F-PROBE run of 2026-11-16 (and reproduced with `fuzz_mountwalk`), not by
libFuzzer, which runs with leak detection off. In `ntfs_device_mount()` the inode `ni` of `$AttrDef`
(`volume.c:1240`) stays open when `ntfs_attr_open(ni, AT_DATA, AT_UNNAMED, 0)` returns NULL (a `$AttrDef`
without an unnamed `$DATA`); the later failure exits of the `$AttrDef` block (size above 24 bits, allocation,
short read) have the same shape, with `na` open as well. `error_exit` at the end of the function frees `m`,
`m2` and calls `__ntfs_volume_release()`, none of which closes `ni` or `na`. In the first case LeakSanitizer
reports a direct leak of 136 bytes (the inode, `inode.c:95`) and an indirect leak of 4096 bytes (its record,
`mft.c:361`); in the later exits the direct leak is the attribute (`ntfs_attr_open`, `attrib.c:426`, opened at
`volume.c:1246`) and the inode is indirect. `known.tsv` matches both forms as F7, each pinned to its
`volume.c` line, so a leak from another open in `ntfs_device_mount()` is reported as new. Library
code only: `volume.c` is not touched by the patch series. Reproducer: `tests/fuzz/image/findings/F7-attrdef-inode-leak.bin`
(replay `ASAN_OPTIONS=detect_leaks=1 fuzz_probe FILE`; the leak is also reported by `fuzz_mountwalk`). Not
fixed here: a leak in an error path of a short-lived process, like F2 and F5. Drafted 2026-11-16 as a public
issue (`docs/UPSTREAM_REPORTS.md`), not filed; reproduced on stock 2026.9.28 with valgrind on the image that
`fz_image_build()` makes from the reproducer (`ntfs-3g -o ro`: "Failed to open ntfs attribute", 136 bytes
definitely and 4096 bytes indirectly lost from `volume.c:1240`). Suggested fix: close `na` and `ni` before
`goto error_exit` in the `$AttrDef` block, as the `bad_upcase` label does for `$UpCase`.

### F8: MFT record size from the boot sector is allocated without a bound

Found by the extended campaign of T-M5-09 (8 CPU-hours, 2026-11-23, signature `N-37cd62`, class R2, denial of
service): `oom ntfs_mft_load < ntfs_volume_startup < ntfs_device_mount @volume.c:265`, libFuzzer
"out-of-memory (malloc(1073741824))", in `fuzz_mountwalk` units. `ntfs_boot_sector_is_ntfs()` accepts a
`clusters_per_mft_record` byte of 1 to 0x40 (clusters) and 0xe1 to 0xf7 (negative: the record size is 2^-c bytes,
so 2^9 to 2^31); `ntfs_boot_sector_parse()` sets `vol->mft_record_size` to `1 << -c` or `c << cluster_size_bits`
and checks only that it is a power of two. `ntfs_mft_load()` allocates that many bytes at the first step of every
mount. The same holds for `clusters_per_index_record` and `vol->indx_record_size` (no check at all). c = -31 is
`1 << 31`, not representable in `int` (undefined behaviour). Real volumes have 1024 or 4096 byte MFT records and
4096 byte INDX records; the Linux ntfs3 driver uses the same limit (`MAXIMUM_BYTES_PER_MFT` and `MAXIMUM_BYTES_PER_INDEX` are 4096 in
`fs/ntfs3/ntfs_fs.h`; `ntfs_init_from_boot()` in `super.c` refuses a larger record). Fixed locally by patch 0015 (2026-11-23, ADR-026): `ntfs_boot_sector_parse()`
computes both sizes in 64 bits and returns -1 with an error message when either is above 4096 bytes, before
anything is stored or allocated. Reproducer: `tests/fuzz/image/findings/F8-mft-record-size-alloc.bin` (the
minimised libFuzzer unit, 9 bytes); regression: ctest `fuzz.findings.F8` (ASan build of the patched library, allocation limit 512 MB; written 2026-11-23, not yet run in Environment A on the unpatched and the patched library). In
`known.tsv` row F8 is `fixed` (0015), so the signature on an engine that carries the patch is a REGRESSION; on
pristine 2026.9.28 it is expected. Not yet reported upstream: drafted, see `docs/UPSTREAM_REPORTS.md`. Draft
text: *libntfs-3g: ntfs_boot_sector_parse() accepts MFT and INDX record sizes up to 2 GiB.* A boot sector with
`clusters_per_mft_record` = 0xe2 makes `ntfs_mft_load()` call `ntfs_malloc(1073741824)` on mount, and 0xe1 shifts
`1` by 31 into the sign bit of an `int`. Suggested fix: refuse record sizes above 4096 bytes in
`ntfs_boot_sector_parse()`. Version 2026.9.28.

Follow-up of F8: the same family for `INDEX_ROOT.index_block_size`, read from disk, is F9 below (fixed by patch 0016).

### F9: INDEX_ROOT index_block_size is allocated without an upper bound

Found as the follow-up of F8 (code review of the users of on-disk sizes, 2026-11-25; not found by a campaign, but reproduced by a
crafted image). The field `INDEX_ROOT.index_block_size` (offset 8 of the `$INDEX_ROOT` value) was checked only for "at least
512" and "a power of two" in `ntfs_attr_inconsistent()` (`attrib.c:3547`) and in the two directory readers of `dir.c` (289,
1196), only for "at least 512" in `ntfs_index_lookup()` (`index.c:829`), and not at all in `ntfs_ir_to_ib()` (`index.c:1138`).
`ntfs_inode_lookup_by_name()` (`dir.c:380`) and `ntfs_readdir()` (`dir.c:1283`) call `ntfs_malloc(index_block_size)` and
read blocks of that size; `ntfs_index_lookup()` sizes every index block of the index context with it. A value of 2^30 asks
for 1 GiB per directory open (up to 2^31). Real volumes: Windows and mkntfs write 4096. Fixed locally by patch 0016 (2026-11-25, ADR-026, proposed, owner decision
pending): `ntfs_index_block_size_valid()` accepts a power of two from 512 to 4096; it does not compare the size with the boot
sector INDX record size (upstream never needs them equal, and a mismatch on the root directory would fail the mount). The
readers reject anything else with `EIO` and an `ntfs_log_error()` before they allocate, and `ntfs_attr_inconsistent()` refuses a
root above 4096 at the MFT record read. On the patched engine F9 exercises that `attrib.c` MFT-record rejection ("Corrupt
index root in MFT record 5"; the mount fails): the checks in `dir.c` and `index.c` are defence in depth that a normal read does
not reach, because a record with a bad root never loads. Reproducer: `tests/fuzz/image/findings/F9-index-root-block-size-alloc.bin` (12 bytes: base `tree`, one edit
that sets `index_block_size` of the root directory, `$INDEX_ROOT` at byte 0x5550 of the image, to 0x40000000); regression:
ctest `fuzz.findings.F9` (ASan build of the patched library, allocation limit 512 MB), which fails on the pristine library
(`ntfs_malloc` called from `ntfs_inode_lookup_by_name` at `dir.c:380`, `ntfs_pathname_to_inode`, `ntfs_open_secure`, during
the mount: requested allocation size 0x40000000 exceeds the maximum) and passes on the patched one (checked in Environment A,
2026-11-25). In `known.tsv` row F9 is `fixed` (0016), so the signature on an engine that carries the patch is a REGRESSION; on
pristine 2026.9.28 it is expected. Not yet reported upstream: drafted, see `docs/UPSTREAM_REPORTS.md`. Draft text:
*libntfs-3g: INDEX_ROOT.index_block_size is allocated without an upper bound.* A directory whose `$INDEX_ROOT` has
`index_block_size` = 0x40000000 makes `ntfs_inode_lookup_by_name()` and `ntfs_readdir()` call `ntfs_malloc(1073741824)`
per open; the checks at `attrib.c:3547` and `dir.c:290` only test for a power of two of at least 512. Suggested fix: refuse
values above 4096. Version 2026.9.28.

### U: undefined behaviour in the library

UBSan in the library is set to report and continue (each location once per process); in the
project code any report aborts. The reports are signed shifts (`<<` of negative or large
values: `bootsect.c:229`, `volume.c:273`, `runlist.c:862`, `897`, `attrib.c:385`, `386`, `1172`), signed
overflow (`bootsect.c:229, 231`, `volume.c:624`, `1094`, `runlist.c:882`, `961`, `attrib.c:1040`,
`1073`), a negative shift exponent (`bootsect.c:237`), a shift into the sign bit
(`bootsect.c:275`), and misaligned 16-bit loads from on-disk names and reparse data
(`unistr.c:183`, `188`, `345`, `376`, `reparse.c`). `attrib.c:1172` is new in the campaign of 2026-11-23: `rl->lcn << vol->cluster_size_bits`, the byte offset passed to
`ntfs_pread()` in `ntfs_attr_pread()`, from a crafted run list; same family as `385` and `386`. Patch 0015 moves the
lines of `bootsect.c` from 183 on (by six up to line 197, by seven after it) and removes the shifts at `275` and `290` (64-bit arithmetic); `known.tsv`
lists the old and the new numbers of `bootsect.c`. All follow from corrupt on-disk numbers; none
was linked to a wrong result in a harness oracle. They are listed, not proposed as one
report: upstream already carries the shifts of `runlist.c` (T-M3-02 recorded them). If
reported, one issue per file with the unsigned-arithmetic fix is the usual form.

## Short stage of ci-local.sh

`scripts/ci-local.sh` stage `fuzz-short` runs `image/run.sh` and `sanitize/run.sh` with `--known-ok` and 300 s per
target. `--known-ok` forgives only the leaks of the corpus replay that match an `open` or `benign` row of
`tests/fuzz/campaign/known.tsv` (F2, F5, F7); the ids are printed per target and in the stage note. The image targets leak-check every corpus input (chunks of 64 per process, a failing chunk replayed one input per process, 1500 s bound), because one process over the corpus stops at the first leak and a known one would hide a new one; `summary.txt` has `leak replay: inputs N, leaking M` and `replay complete`, and the stage fails without them. The stage builds
the patched engine, so a `fixed` row is a regression. Any artifact, any non-zero libFuzzer run exit, an unknown leak
or a leak count mismatch still fails; for the sanitize targets the flag is a no-op. The classifier is
`tests/fuzz/campaign/shortcheck.py` (the matcher of the campaign); without `--known-ok` the scripts fail on every
finding, as the acceptance runs of one CPU-hour need.

## Open

- F4 ended every libFuzzer process of run B of F-MOUNT-WALK within minutes, so run B covered the paths
  behind it less than 3603 s suggested. Closed by the run of 2026-11-16: the engine carries 0010 to
  0013, the run was made without `--skip-security`, it used one process for the whole 3601 s with no
  crash (2,014,880 executions, 4756 edges against 4049, `security.c` 19.0 % against 6.8 %). The
  8 CPU-hour run of T-M5-09 remains the place for depth.
- F7 (engine leak, new in the run of 2026-11-16) is drafted as a public issue, like F2 and F5, and listed in
  `docs/UPSTREAM_REPORTS.md`; reproduced with valgrind on stock 2026.9.28 (136 + 4096 bytes lost).
- Leaks stop libFuzzer at the first one, so `inner.sh` runs with leak detection off and
  lists every distinct leak of the final corpus in a replay (`leaks.log.sym`). A leak that
  needs an input that added no coverage is not found this way.
- `ctest` replays (`fuzz.probe_seeds`, `fuzz.mountwalk_seeds`) run with `detect_leaks=0`,
  because the seeds include F2.
- The probe seeds reach the walk of a mounted volume; hostile names of the kind of
  `U-NAME-*` come from the base images and the edit seeds only (no dictionary).
- T-M5-09: the extended campaign (driver `tests/fuzz/campaign/run.sh`, classification and the finding M5-01 in
  `docs/SECURITY_FINDINGS.md`) covers all targets, including the new F-CTL-01 (`tests/fuzz/ctl/`), and
  replaces the by-hand recipe of this file for new runs. The 8 CPU-hour run itself is still to be made.
