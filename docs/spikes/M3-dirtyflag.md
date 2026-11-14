# Spike M3: NTFS-3G and the dirty flag / `$LogFile` (T-M3-01)

Questions: U9 (does NTFS-3G ever set `VOLUME_IS_DIRTY`?) and U13 (does a `norecover` read/write session
leave the `$LogFile` restart area unchanged?). Both confirmed by source and by test, in Environment A
(Linux container) and Environment B (macOS 27.2 guest, macFUSE 5.5.0 FSKit, `ntfs-3g 2026.9.28 external
FUSE 29`). Script: `tests/integration/smoke/dirtyflag_check.sh` (52 PASS, exit 0 in both). Logs with full
hex dumps of both restart pages per snapshot: `tests/results/evidence/m301/2026-11-14-{linux,macos}.log`.

## U9: NTFS-3G never sets `VOLUME_IS_DIRTY`

Source (NTFS-3G 2026.9.28, tarball sha256 matches `third_party/deps.lock`):

- `libntfs-3g/volume.c:1181` only reads the flags (`vol->flags = vinf->flags`).
- The only write is `ntfs_volume_write_flags()`, `volume.c:1738-1790` (line 1781 masks with
  `VOLUME_FLAGS_MASK`, then `ntfs_inode_sync`). It has no caller in `libntfs-3g/`, `src/ntfs-3g.c`,
  `src/lowntfs-3g.c` or `src/ntfs-3g_common.c`.
- Callers outside the driver: `ntfsprogs/ntfsfix.c:1676`, `ntfsresize.c:3009`, `ntfsmove.c:907`,
  `ntfsck.c:802` (clears). `ntfsfix.c:309` also writes through its own `OLD_ntfs_volume_set_flags`
  (`set_dirty_flag()`, `ntfsfix.c:297-324`). `mkntfs.c:4752` sets it on a new volume only when writing the
  backup boot sector fails. `ntfsfix.c:1667-1670` states that libntfs-3g never sets or clears the bit on
  mount or unmount.

Test: `$Volume` flags were `0x0000` in all six snapshots (before; during session 1; after clean unmount;
during session 2; after `kill -9`; after remount and unmount), read by both `ntfsinfo -F '$Volume'` and
`ntfsinfo -m`. The sha256 of MFT record 3 was identical across all six.

## U13: a `norecover` rw session leaves `$LogFile` unchanged

Source:

- `volume.c:1286` runs the log check only for mounts that are neither `RDONLY` nor `FORENSIC`. When
  `ntfs_volume_check_logfile` fails without `NTFS_MNT_RECOVER`, the driver falls back to read-only
  (`need_fallback_ro`, 1298) or fails (1300).
- `ntfs_logfile_reset` (`volume.c:1689`) runs only in the `else` branch at `volume.c:1304`; it calls
  `ntfs_empty_logfile` (`volume.c:1713`), whose `ntfs_attr_pwrite` at `logfile.c:754` is the only write to
  `$LogFile` in `logfile.c`.
- `NTFS_MNT_RECOVER` comes only from `ctx->recover` (`src/ntfs-3g.c:4034-4035`); `norecover` clears it
  (`src/ntfs-3g_common.c:445`), `recover` sets it (`:442`).
- Readers only: `ntfs_volume_check_logfile` (`volume.c:678-720`), `ntfs_check_logfile`,
  `ntfs_is_logfile_clean`. Outside the driver, `ntfsfix.c:335` and `ntfsresize.c:3020` call
  `ntfs_logfile_reset`, so a `$LogFile` change after those tools is expected.

Test, two log fixtures:

- `empty`: fresh `mkntfs` image; `$LogFile` is all `0xff` (no restart area).
- `synth`: the same image with two hand-built version 1.1 restart pages that `ntfs_check_logfile` and
  `ntfs_is_logfile_clean` accept (so the driver takes the clean path).

In `synth`, both copies stayed identical in every snapshot in both environments: copy 0 at offset 0,
`current_lsn` `0x00000001a2b3c4c5`; copy 1 at 4096, `0x00000001a2b3c4d5`; `client_in_use_list` 0, flags
`0x0002` (`RESTART_VOLUME_IS_CLEAN`), `chkdsk_lsn` 0, `open_count` 1. The sha256 of the first 8192 bytes and
of the whole 2 MiB `$LogFile` was the same in all six snapshots.

Unclean log: with the clean flag cleared in both restart pages, a `norecover` rw mount printed
"The disk contains an unclean file system (0, 0)" and came up read-only (macOS `mount` shows
`read-only`, Linux `ro`); `$LogFile` stayed byte-identical. This is the `norecover` + unclean-log case
that docs/spikes/M2-options.md had left for a fixture.

## `kill -9`

After `kill -9` of a rw driver, in both environments: the stale mount was removed, a rw remount with
`norecover` worked and accepted writes, data written before the kill was readable, `ntfsfix -n` on a
copy exited 0 (matches M2-options surprise 8). This is why the product sets its own marker (ADR-006,
`ntfsmac-volmark`): the engine leaves no trace of an interrupted session.

## Controls and limits

- Detector controls on copies (never on the test image, never mounted): `ntfsfix` without `-n` set the
  flag to `0x0001` and the detector reported it; on the unclean-log copy it reset `$LogFile` to `0xff`
  and the hash changed; on clean `synth` it set the flag but left `$LogFile` alone (`fix_mount` path only,
  `ntfsfix.c:1599`).
- Snapshots copy the image file while the driver runs. The DURING copy held all 42 directory entries of
  session 1 after `sync`; data still in the driver's own caches before `sync` is not covered.
- The `synth` log is hand-built, not written by Windows. A Windows-written `$LogFile` (T-M5-10, U12)
  would add confidence for real volumes; the `empty` fixture needs no such assumption.
- `dirtyflag_check.sh` (`build_page`, `inject_logfile`, `unclean_case`: zero the 2 bytes at offset 62 of
  each restart page) is a working generator for clean and unclean restart areas; T-M3-04 and T-M3-12 can
  reuse it.
