# NTFS-3G patch series

Patches applied by `scripts/build-ntfs3g.sh` on top of the pinned NTFS-3G tarball
(`third_party/deps.lock`). Rationale and policy: ADR-002 in `docs/TECHNICAL_DECISIONS.md`.

## Rules

- File name: `NNNN-short-description.patch`, four digits. Applied in lexical order with
  `patch -p1` from the unpacked source root. A patch that does not apply cleanly fails the build.
- Numbers come from the ADR-002 table. A number is never reused.
- Each patch starts with a header giving: rationale, upstream status, and the test that covers it.
- Scope: build fixes, the inherited device descriptor (0003), the test-only fault-injection layer,
  the session channel, the one device-sync change of 0007 (ADR-022), and the two macOS FUSE-glue
  fixes in `src/ntfs-3g.c` of 0008 (ADR-024) and 0009 (ADR-025) only.
  Anything touching allocation, runlists, MFT or index code is out of scope; report it upstream.
- The build stamp hashes the file names and contents of the series. Editing, adding or removing
  a patch triggers a rebuild; nothing else is needed.

## Numbering

| Patch | Purpose | Status |
|---|---|---|
| 0001 | Build fixes for macOS 27 + macFUSE 5.5.0 | Conditional: only if T-M2-03 shows a need |
| 0002 | Withdrawn (upstream `ntfs_fsync()` already uses `F_FULLFSYNC`). The number is not reused; the build script refuses a `0002-*` file. | Withdrawn |
| 0003 | Accept an inherited descriptor as the device | **Applied** (T-M2-05): unpatched `/dev/fd/N` fails three ways; Opus-reviewed, changes applied. macOS only, library-wide; `lowntfs-3g` stays unpatched and must not ship (`docs/spikes/M2-fdpass.md`) |
| 0004 | Test-only fault-injection layer (`--enable-faultio`) | **Applied** (T-M3-14), built only with `--enable-faultio` |
| 0005 | libfuse3 port of `ntfs-3g.c` | Conditional: only per ADR-001 fallback 1 |
| 0006 | Session channel on `NTFSMAC_ERRFD` (ADR-018) | Later task (T-M3-18; re-bases 0007 and adds its `W flush` call) |
| 0007 | Drive flush on device nodes: `ntfs_fsync()` issues `fsync` then `ioctl(DKIOCSYNCHRONIZE)` for a block or character device on macOS (ADR-022) | Later task (T-M3-21). To do: report upstream (`F_FULLFSYNC` fails with `ENOTTY` on device nodes, so the silent `fsync` fallback is the normal path); sending is a human decision |
| 0008 | Creation-time conversion: `ntfs_macfuse_getxtimes`, `ntfs_macfuse_setcrtime` and `ntfs_macfuse_setchgtime` convert with `timespec2ntfs()`/`ntfs2timespec()` instead of storing `tv_sec` as a FILETIME (ADR-024) | Later task (T-M3-22). To do: report upstream (every file made through macFUSE gets a 1601 creation time); sending is a human decision |
| 0009 | Xattr names under `windows_names`: `ntfs_fuse_setxattr` skips `ntfs_forbidden_chars` for the xattr (named-stream) name on macOS; file and directory names keep the check (ADR-025) | Later task (T-M3-23). Reverted if Windows `chkdsk` drops or flags streams whose name contains `:` (`H-USB-11`, U23) |
