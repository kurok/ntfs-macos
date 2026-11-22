# NTFS-3G patch series

Patches applied by `scripts/build-ntfs3g.sh` on top of the pinned NTFS-3G tarball
(`third_party/deps.lock`). Rationale and policy: ADR-002 in `docs/TECHNICAL_DECISIONS.md`.

## Rules

- File name: `NNNN-short-description.patch`, four digits. Applied in lexical order with
  `patch -p1` from the unpacked source root. A patch that does not apply cleanly fails the build.
- Numbers come from the ADR-002 table. A number is never reused.
- Each patch starts with a header giving: rationale, upstream status, and the test that covers it.
- Scope: build fixes, the inherited device descriptor (0003), the test-only fault-injection layer,
  the session channel, the one device-sync change of 0007 (ADR-022), the two macOS FUSE-glue
  fixes in `src/ntfs-3g.c` of 0008 (ADR-024) and 0009 (ADR-025), the directory-listing change of
  0014 in `src/ntfs-3g.c` with its fuse-lite hunk (ADR-027), and the four carried security fixes
  of 0010 to 0013 (ADR-026: rejections of malformed on-disk data in `acls.c`, `reparse.c`,
  `security.c` and `inode.c`, removed when a pinned release fixes them) only.
  Anything touching allocation, runlists, MFT or index code is out of scope; report it upstream.
- The build stamp hashes the file names and contents of the series. Editing, adding or removing
  a patch triggers a rebuild; nothing else is needed.

## Numbering

| Patch | Purpose | Status |
|---|---|---|
| 0001 | Build fixes for macOS 27 + macFUSE 5.5.0 | Conditional: only if T-M2-03 shows a need |
| 0002 | Withdrawn (upstream `ntfs_fsync()` already uses `F_FULLFSYNC`). The number is not reused; the build script refuses a `0002-*` file. | Withdrawn |
| 0003 | Accept an inherited descriptor as the device | **Applied** (T-M2-05): unpatched `/dev/fd/N` fails three ways; independently reviewed, changes applied. macOS only, library-wide; `lowntfs-3g` stays unpatched and must not ship (`docs/spikes/M2-fdpass.md`) |
| 0004 | Test-only fault-injection layer (`--enable-faultio`) | **Applied** (T-M3-14), built only with `--enable-faultio` |
| 0005 | libfuse3 port of `ntfs-3g.c` | Conditional: only per ADR-001 fallback 1 |
| 0006 | Session channel on `NTFSMAC_ERRFD` (ADR-018) | **Applied** (T-M3-18; independently reviewed, changes applied). Cross-platform; 0007 to 0009 re-based onto it, 0007 gained the `W flush` call. Series order: 0003, 0004, 0006, 0007, 0008, 0009 |
| 0007 | Drive flush on device nodes: `ntfs_fsync()` issues `fsync` then `ioctl(DKIOCSYNCHRONIZE)` for a block or character device on macOS (ADR-022) | **Applied** (T-M3-21; independently reviewed, accepted). macOS only, `unix_io.c` only; applies with and without 0004. To do: report upstream (report text in the patch header) (`F_FULLFSYNC` fails with `ENOTTY` on device nodes, so the silent `fsync` fallback is the normal path); sending is a human decision |
| 0008 | Creation-time conversion: `ntfs_macfuse_getxtimes`, `ntfs_macfuse_setcrtime` and `ntfs_macfuse_setchgtime` convert with `timespec2ntfs()`/`ntfs2timespec()` instead of storing `tv_sec` as a FILETIME (ADR-024) | **Applied** (T-M3-22; independently reviewed, accepted). To do: report upstream (report text in the patch header) (every file made through macFUSE gets a 1601 creation time); sending is a human decision |
| 0009 | Xattr names under `windows_names`: `ntfs_fuse_setxattr` skips `ntfs_forbidden_chars` for the xattr (named-stream) name on macOS; file and directory names keep the check (ADR-025) | **Applied** (T-M3-23; reviewed, accepted). Keeps an empty-name guard in `setxattr` and adds the same guard to `ntfs_fuse_removexattr` (M3 audit F-07). Reverted if Windows `chkdsk` drops or flags streams whose name contains `:` (`H-USB-11`, U23) |
| 0010 | `ntfs_valid_descr()` (`acls.c`) returns FALSE for a descriptor shorter than its header before it reads the header (F3, GHSA-g3h3-7rhv-rrmq; ADR-026) | **Applied** (T-M3-25). Cross-platform, library-wide. Carried until a fixed NTFS-3G release is pinned, then removed; covered by `fuzz.findings.F3` (detects the unpatched defect only in builds that keep the loads; see the patch header) |
| 0011 | `ntfs_possible_symlink()` (`reparse.c`) reads the reparse tag only from an attribute of at least the header size (F4, GHSA-vp2x-xjmj-x4x2; ADR-026) | **Applied** (T-M3-25). Cross-platform, library-wide. The other reparse readers are already guarded (listed in the header). Carried until a fixed release is pinned, then removed; covered by `fuzz.findings.F4` |
| 0012 | `retrievesecurityattr()` and `setsecurityattr()` (`security.c`) reject a `$SII` or `$SDH` length smaller than the header or beyond the end of `$SDS` before they allocate (F1, GHSA-r4cv-86xq-37qj; ADR-026) | **Applied** (T-M3-25). Cross-platform, library-wide. Carried until a fixed release is pinned, then removed; covered by `fuzz.findings.F1`. Rejects also a record across a 256 KiB `$SDS` block and a size differing from the caller's in the write path |
| 0013 | `ntfs_inode_sync_file_name()` (`inode.c`) reads the reparse tag only from a resident value of at least the header size (F6, found in review; ADR-026) | **Applied** (T-M3-25). Cross-platform, library-wide, read-write mounts only. **Not yet reported upstream** (draft report in `build/upstream-reports/05-security-inode-sync-reparse.md`); carried per ADR-026; no replay reaches it |
| 0014 | Directory listing (ADR-027): each open directory handle keeps a snapshot of its listing, read once from position 0, and READDIR is served from it with offsets (position + 1), so no reply ends inside an entry and no listing buffer is shared. Avoids `EINVAL` from `readdir`, `ls` and `getattrlistbulk` (and a crash of the module) on the macFUSE FSKit backend, and keeps `readdir` + `unlink` + `rmdir` working. New `opendir`/`releasedir` in `src/ntfs-3g.c`; one hunk in `libfuse-lite/fuse.c` (Linux only) for `seekdir` after the end; reparse plugins keep the upstream mode; no change to `libntfs-3g` | **Applied** (2026-11-22; ADR-027 accepted by the owner, with a known limit: on the FSKit backend a `readdir` + `unlink` loop misses entries while another process lists the same directory, a macFUSE defect that NTFS-3G cannot work around; `tests/integration/readdir` reports it as KNOWN). Cross-platform, FUSE glue only. Validated in Environment B and A (ADR-027). To do: suggest upstream (offset-based readdir, the fuse-lite fix); the FSKit defects are a separate macFUSE report |
