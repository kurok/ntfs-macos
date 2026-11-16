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
| Read-only mount refuses a volume whose `$MFTMirr` is one write behind `$MFT` | crash sweep, `docs/spikes/M3-crashsweep-triage.md` | bug / enhancement | public issue | Drafted 2026-11-16, not filed; related upstream issue #209 | ADR-021 (`mirror_split`, repair) |
| `ntfs_fsync()` never flushes the drive cache of a block device on macOS | T-M3-10, patch 0007 | bug | public issue | Text in the 0007 header, not filed | patch 0007 (ADR-022) |
| Creation time stored as a raw `tv_sec` FILETIME on macOS | T-M3-06, patch 0008 | bug | public issue | Text in the 0008 header, not filed | patch 0008 (ADR-024) |

When a fixed NTFS-3G release is pinned, drop each carried fix it covers (ADR-026) and update
this table.
