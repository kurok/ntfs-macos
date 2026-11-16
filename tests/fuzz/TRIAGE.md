<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Fuzz triage: F-PROBE and F-MOUNT-WALK (T-M3-16, criterion 3.11)

Harnesses: `tests/fuzz/image/` (see the header of `fuzz_probe.c`, `fuzz_mountwalk.c`, `image.h`).
Runs: `tests/fuzz/image/run.sh`, Environment A (Debian container, clang 19.1.7, libFuzzer,
ASan + UBSan, aarch64), libntfs-3g 2026.9.28 with the release patch series 0003, 0006, 0007,
0008, 0009 (0004 is applied by the build script but its code is compiled only with
`--enable-faultio`, which is not used), built with the same instrumentation. Results of the
runs: `build/fuzz/image/hour1/`, `build/fuzz/image/hour2/` (git-ignored).

## Runs

| Target | Start | End | CPU time | Executions | Result |
|---|---|---|---|---|---|
| F-PROBE | 2026-11-15T14:33:42Z | 15:34:04Z | 3601 s, one process | 4,029,820 (1119 exec/s) | no crash, no oracle failure, no timeout; corpus 4951 inputs, 3367 edges |
| F-MOUNT-WALK, run A | 14:33:42Z | 15:04:35Z | 1826 s in 60 processes | 1,264,689 | stopped by the restart limit (60 restarts) after F1 and F3 ended every few seconds of fuzzing; shows F1, F3 |
| F-MOUNT-WALK, run B (`--skip-security`) | 15:34:22Z | 16:34:44Z | 3603 s in 7 processes | 3,585,398 (1319 exec/s) | 6 crashes, all F4; corpus 3931 inputs, 4049 edges |

Criterion 3.11: one CPU-hour on the probe is met with no finding in project code. The
mount-and-walk hour is met in CPU time, with F1 and F3 (security descriptor reads) left out
of run B and F4 ending each of its processes (6 restarts, 3603 s in all). Because F4 recurs,
the part of the engine behind it was explored less in run B than the time suggests; see
"Open".

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
unmodified release. F6 (below) was found in the review of the patches and has no replay. and texts below describe the findings as made against the unmodified release. F2 and F5
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
| U | `bootsect.c` 229, 231, 237, 275; `volume.c` 273, 624, 1094; `runlist.c` 862, 882, 897, 961; `attrib.c` 385, 386, 1040, 1073; `unistr.c` 183, 188, 345, 376; `reparse.c` (misaligned loads) | undefined behaviour: left shifts of negative or large values, signed overflow, negative shift exponent, misaligned `ntfschar` loads | low (benign on arm64 and x86-64 with the usual compilers) | UBSan reports in `hour*/*/libfuzzer-*.log.sym`, list in `ubsan-engine.txt` | yes (bootsect, volume, runlist, unistr hit by the probe too) |

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

### U: undefined behaviour in the library

UBSan in the library is set to report and continue (each location once per process); in the
project code any report aborts. The reports are signed shifts (`<<` of negative or large
values: `bootsect.c:229`, `volume.c:273`, `runlist.c:862`, `897`, `attrib.c:385`, `386`), signed
overflow (`bootsect.c:229, 231`, `volume.c:624`, `1094`, `runlist.c:882`, `961`, `attrib.c:1040`,
`1073`), a negative shift exponent (`bootsect.c:237`), a shift into the sign bit
(`bootsect.c:275`), and misaligned 16-bit loads from on-disk names and reparse data
(`unistr.c:183`, `188`, `345`, `376`, `reparse.c`). All follow from corrupt on-disk numbers; none
was linked to a wrong result in a harness oracle. They are listed, not proposed as one
report: upstream already carries the shifts of `runlist.c` (T-M3-02 recorded them). If
reported, one issue per file with the unsigned-arithmetic fix is the usual form.

## Open

- F4 ends every libFuzzer process of F-MOUNT-WALK within minutes, so run B covers the paths
  behind it less than 3603 s suggest. F4 is now fixed locally (patch 0011) and so are F1 and F3
  (0012, 0010): a further run on the patched engine, without `--skip-security`, is advised;
  T-M5-09 (8 CPU-hours) is the place.
- Leaks stop libFuzzer at the first one, so `inner.sh` runs with leak detection off and
  lists every distinct leak of the final corpus in a replay (`leaks.log.sym`). A leak that
  needs an input that added no coverage is not found this way.
- `ctest` replays (`fuzz.probe_seeds`, `fuzz.mountwalk_seeds`) run with `detect_leaks=0`,
  because the seeds include F2.
- The probe seeds reach the walk of a mounted volume; hostile names of the kind of
  `U-NAME-*` come from the base images and the edit seeds only (no dictionary).
