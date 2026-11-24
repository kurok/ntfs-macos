<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Upstream reports to NTFS-3G

Reports to NTFS-3G (https://github.com/tuxera/ntfs-3g) that come from this project's tests.
Criteria 3.9 and 3.11 of `IMPLEMENTATION_PLAN.md` are met when a report is drafted and listed
here; filing is an owner action (owner decision 2026-11-16, M3 audit F-03). Draft texts and
reproducer images are kept outside the repository until filed. Security findings are filed
privately (GitHub security advisories) and are not described further here before Tuxera
publishes them.

| Item | Source | Kind | Channel | Status | Local fix |
|---|---|---|---|---|---|
| F3 `ntfs_valid_descr()` header read before the length check | fuzzing, `tests/fuzz/TRIAGE.md` | security | private advisory | Filed 2026-11-16, GHSA-g3h3-7rhv-rrmq | patch 0010 (ADR-026) |
| F4 `ntfs_possible_symlink()` reparse tag read without a size check | fuzzing | security | private advisory | Filed 2026-11-16, GHSA-vp2x-xjmj-x4x2 | patch 0011 |
| F1 `retrievesecurityattr()` unchecked allocation size | fuzzing | security | private advisory | Filed 2026-11-16, GHSA-r4cv-86xq-37qj | patch 0012 |
| F6 `ntfs_inode_sync_file_name()` reparse tag read without a length check | review of 0011 | security | private advisory | Drafted 2026-11-16, not filed | patch 0013 |
| F2 `ntfs_mft_load()` leaks the run list on "not self-contained" | fuzzing | bug | public issue | Drafted 2026-11-16, not filed | none (4 KiB per failed mount) |
| F5 `ntfs_make_symlink()` leaks the attribute on invalid data | fuzzing | bug | public issue | Drafted 2026-11-16, not filed | none (56 bytes per access) |
| F7 `ntfs_device_mount()` leaks the `$AttrDef` inode when loading `$AttrDef` fails | fuzzing (re-run of 2026-11-16) | bug | public issue | Drafted 2026-11-16, not filed; reproduced with valgrind on stock 2026.9.28 | none (about 4 KiB per failed mount) |
| F8 `ntfs_boot_sector_parse()` accepts MFT and INDX record sizes up to 2 GiB (`ntfs_mft_load()` allocates 1 GiB on mount; `1 << 31` overflows `int`) | fuzzing (campaign of 2026-11-23, N-37cd62), `tests/fuzz/TRIAGE.md` | security (denial of service) | private advisory | Drafted 2026-11-23, not filed | patch 0015 |
| Read-only mount refuses a volume whose `$MFTMirr` is one write behind `$MFT` | crash sweep, `docs/spikes/M3-crashsweep-triage.md` | bug / enhancement | public issue | Drafted 2026-11-16, not filed; related upstream issue #209 | ADR-021 (`mirror_split`, repair) |
| `ntfs_fsync()` never flushes the drive cache of a block device on macOS | T-M3-10, patch 0007 | bug | public issue | Text in the 0007 header, not filed | patch 0007 (ADR-022) |
| Creation time stored as a raw `tv_sec` FILETIME on macOS | T-M3-06, patch 0008 | bug | public issue | Text in the 0008 header, not filed | patch 0008 (ADR-024) |
| `ntfs_readdir()` resumed inside the index allocation tests bit 0 of the bitmap byte instead of bit `bmp_pos & 7` (`libntfs-3g/dir.c`), for every block until the next 4 KiB chunk of the bitmap: it reads blocks that are not in use ("Failed to read index block", `EIO`) or skips blocks in use (lost entries). Latent: ntfs-3g and lowntfs-3g always start at position 0 | investigation of S-PAR-01 (2026-11-22); reproducer `resume.c` (stops `ntfs_readdir()` after K entries and resumes at the last position + 1), kept outside the repository | bug | public issue | Not drafted | none needed: patch 0014 reads from position 0 only |
| fuse-lite `readdir_fill` leaves a handle "filled" after an empty READDIR, so a later READDIR at a nonzero offset (seekdir after the end) gets 0 bytes without asking the file system (`libfuse-lite/fuse.c`; libfuse 2.9 has the same code) | patch 0014 review (2026-11-22): `seekdir` after the end lists nothing on Linux without the fix | bug | public issue | Not drafted | patch 0014 (fuse-lite hunk, Linux only) |

When a fixed NTFS-3G release is pinned, drop each carried fix it covers (ADR-026) and update
this table.

## Reports to macFUSE

Filing is an owner action, as above.

| Item | Source | Kind | Channel | Status | Local fix |
|---|---|---|---|---|---|
| `diskutil unmount` of an FSKit-backend mount answers "failed to unmount" (FSKit access check error 45), macFUSE 5.5.0 on macOS 27.2 | T-M4-11, `tests/results/evidence/m4-11/diskutil_unmount-without-daemon.txt` | bug | public issue | Drafted 2026-11-19, not filed | none: `ntfsmac unmount`, `umount` and Finder eject work (R-37) |
| FSKit backend: readdir fails with `EINVAL` (and the module can crash) for a libfuse high-level file system whose readdir ignores the offset; enumerations of one directory share one handle position, so a reader that deletes while another process lists misses entries | S-PAR-01 `EINVAL` (2026-11-21), investigation 2026-11-22 | bug | public issue | Draft, not posted (text below) | patch 0014 (ADR-027); the shared position (item 4) has no local fix, known limit |

### Draft: FSKit backend directory enumeration (draft, not posted)

Title: FSKit backend: directory enumeration fails with EINVAL, can crash the fsmodule, and mixes
concurrent enumerations of one directory

macFUSE 5.5.0, FSKit backend (`-o backend=fskit`), macOS 27.2 (26B123) arm64, libfuse2 high-level
API (FUSE_USE_VERSION 29). A file system whose readdir ignores the offset and fills every entry with
offset 0 (libfuse's filled-buffer mode, used for example by NTFS-3G) shows these failures:

1. Deterministic, no concurrency. On an unchanged directory whose listing is longer than 10240
   bytes, getdirentries64 with a buffer of 16 KiB or more, and so `ls` (`fts_read: Invalid
   argument`), fails with EINVAL whenever the module's 10240-byte FUSE_READDIR reply ends in a
   fuse_dirent cut after its 24-byte header: 116 files with 58-byte names fail and 115 pass; 142
   files with 41-byte names pass (the cut falls exactly after a header); 80 files with 104-byte
   names fail. libfuse's high-level `fuse_lib_readdir` answers with `contents + off` clipped only to
   the buffer length, so a cut trailing record is legitimate; the Linux kernel ignores it (`if
   (reclen > nbytes) break`). The module should ignore the cut tail and continue from the `off` of
   the last complete entry.
2. Concurrent. With two or more readers enumerating one directory while another process creates,
   renames or deletes entries in it, readers get intermittent EINVAL (sometimes EIO) on a later
   getdirentries call; one reader with the same writers never fails. The module appears to reuse one
   FUSE directory handle for several enumerations (in one trace 59 OPENDIRs served 305 fills, one
   handle was refilled from offset 0 21 times): an offset-0 READDIR from one reader makes libfuse
   refill the shared buffer, so another reader's cookie (a byte offset into the old buffer) points
   into a record.
3. Crash. Under (2) the module process crashed twice (EXC_BREAKPOINT, Swift assertion in
   `Request.BodySegment.managed(copy:)` called from
   `Volume.enumerateDirectory(_:startingAt:verifier:attributes:packer:resultHandler:)`), which
   unmounts the volume. Shortly before one crash the file system received a LOOKUP for a garbage
   name.

4. Enumerations share one position. With an offset-honouring readdir, a process that reads a
   directory and unlinks the entries it got (readdir; unlink; ...; rmdir) while another process lists
   the same directory in a loop (`ls -f`, or getattrlistbulk) misses entries in every run, and rmdir
   fails with ENOTEMPTY: 1000 files, 36 of 36 runs, 26 to 52 left; 10000 files, 17 of 17 runs, 112 to
   397 left. Some names reach it twice (its second unlink gets ENOENT). A FUSE trace shows one OPENDIR
   for the directory and READDIR offsets that continue the other process's listing: the first
   enumeration's cookies never reach the file system after the second starts, so a file system cannot
   keep per-enumeration state. Expected: each FSKit enumeration continues from its own cookie (pass it
   as the READDIR offset), or gets its own FUSE handle.

The same file system with an offset-honouring readdir (stable per-entry offsets passed to the
filler) shows none of 1 to 3. Reproducer: an in-memory libfuse2 file system of about 250 lines and a
getdirentries64 loop; available on request.
