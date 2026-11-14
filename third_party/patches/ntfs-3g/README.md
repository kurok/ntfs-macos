# NTFS-3G patch series

Patches applied by `scripts/build-ntfs3g.sh` on top of the pinned NTFS-3G tarball
(`third_party/deps.lock`). Rationale and policy: ADR-002 in `docs/TECHNICAL_DECISIONS.md`.
The series is empty until a patch is needed.

## Rules

- File name: `NNNN-short-description.patch`, four digits. Applied in lexical order with
  `patch -p1` from the unpacked source root. A patch that does not apply cleanly fails the build.
- Numbers come from the ADR-002 table. A number is never reused.
- Each patch starts with a header giving: rationale, upstream status, and the test that covers it.
- Scope: build fixes, the test-only fault-injection layer and the session channel only.
  Anything touching allocation, runlists, MFT or index code is out of scope; report it upstream.
- The build stamp hashes the file names and contents of the series. Editing, adding or removing
  a patch triggers a rebuild; nothing else is needed.

## Numbering

| Patch | Purpose | Status |
|---|---|---|
| 0001 | Build fixes for macOS 27 + macFUSE 5.5.0 | Conditional: only if T-M2-03 shows a need |
| 0002 | Withdrawn (upstream `ntfs_fsync()` already uses `F_FULLFSYNC`). The number is not reused; the build script refuses a `0002-*` file. | Withdrawn |
| 0003 | Accept an inherited descriptor as the device | Conditional: only if `/dev/fd/N` fails (T-M2-05) |
| 0004 | Test-only fault-injection layer (`--enable-faultio`) | Later task (T-M3-14) |
| 0005 | libfuse3 port of `ntfs-3g.c` | Conditional: only per ADR-001 fallback 1 |
| 0006 | Session channel on `NTFSMAC_ERRFD` (ADR-018) | Later task |
