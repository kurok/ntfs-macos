# M3 metadata and timestamps (T-M3-06)

Cases `I-META-01..07`, `tests/integration/metadata/i_meta.py` (helpers in `metalib.py`). Date 2026-11-14.
Measured on a clone of `ntfsmac-golden`: macOS 27.2 arm64 guest, macFUSE 5.5.0 FSKit backend, NTFS-3G 2026.9.28
built against libfuse2, image file volume, mounted by the console user (uid 501). Reference: Environment A, Linux
container, root, kernel FUSE.

Results: `tests/results/2026-11-14-054309b-B.json` (7 pass), `tests/results/2026-11-14-054309b-A.json` (7 pass).
Evidence logs and per-case `observations.json`: `tests/results/evidence/2026-11-14-054309b-{A,B}/`.

Method. The on-disk values are not taken from a tool's text. `metalib.RawVolume` reads the MFT of the unmounted
image and decodes `$STANDARD_INFORMATION` and `$FILE_NAME` (four FILETIMEs and the file flags). I-META-01
cross-checks it against `ntfsinfo -F` (whole seconds, same values, also for the 1601 creation times). A view is
`os.lstat` (ns), `getattrlist` and `st_flags` in B. "After remount" is a read-only session on the same image. The
cases pin what the backend does, so a new macFUSE or NTFS-3G that changes it fails the case.

## Summary

mtime, ctime and atime are shown, stored and round-trip exactly (to 100 ns) when they come from a write, `touch -t`
or `utimensat` with both times. Creation time is neither shown nor stored correctly in B: the OS shows 1970-01-01
and every file or directory made through the mount gets a wrong `$STANDARD_INFORMATION` creation time on disk
(1601-01-01 plus the create time in seconds, see below). Read-only and hidden flags are not reachable from macOS:
`chflags uchg` and `chflags hidden` return success and change nothing, and the NTFS attributes enforce nothing and
show nothing. Criterion 3.6 holds for mtime, ctime and atime; it does not hold for creation time in B.

## What is shown, stored, lost

| Metadata | Shown by macOS (B) | Stored on disk (B) | Lost or wrong |
|---|---|---|---|
| mtime | Yes, exact after remount (`I-META-01`, `-02`) | Yes, 100 ns, floor of the request | Pre-1970 value with a fraction shows rounded up to the next second (A too) |
| ctime | Yes, exact after remount | Yes, time of the change | Cannot be set; `touch -t` moves it to now |
| atime | Only what was set or the create time; **updates by reads never show** until a remount (`I-META-03`) | Yes, per mount option (see below) | `touch -a` and `utimensat` with mtime omitted: dropped, nothing reaches the disk |
| creation time | **No**: `st_birthtime` and `getattrlist` crtime are 1970-01-01 | **Wrong**: `$STANDARD_INFORMATION` creation = create-time seconds written as 100 ns ticks (1601-01-01 00:0x). `$FILE_NAME` creation is right | Real creation time lost on every create; `setattrlist` and `cp -p` from APFS cannot set it |
| backup time | Not shown | Not in NTFS | n/a |
| READONLY attribute | No (`st_flags` 0, `ls -lO` empty, mode unchanged) | Yes, only if set with `system.ntfs_attrib` or by `chmod` on a `permissions` mount | `chflags uchg` is lost; READONLY is not enforced |
| HIDDEN attribute | No (`UF_HIDDEN` never set) | Yes, only by `system.ntfs_attrib` or `hide_dot_files` on create | `chflags hidden` is lost |
| dot-name | Not special in NTFS | No attribute unless `hide_dot_files` | n/a |
| `$FILE_NAME` times | Not shown | Written at create; **not updated** by writes (f1: SI mtime 1.2 s ahead of FN mtime, A and B alike) | Windows tools that read them see the create time |

## Findings

### Times of new entries (I-META-01)

- File, directory, write and append: mtime, ctime and atime shown after a remount equal the on-disk
  `$STANDARD_INFORMATION` values to the nanosecond, for files and directories. The same in A.
- Precision: NTFS keeps 100 ns. In B the times NTFS-3G stamps by itself (create, write) are whole microseconds
  (0 of 12 values had digits below a microsecond); in A they carry 100 ns digits (10 of 12). The difference is the
  clock the driver uses, not the volume. Values set explicitly keep 100 ns in both environments (`I-META-02`).
- Creation time, B: the backend calls `setcrtime` right after every create, and the macOS code of NTFS-3G
  (`ntfs_macfuse_setcrtime` in `src/ntfs-3g.c`) stores `tv->tv_sec` as the FILETIME without conversion. A file made at
  Unix second S has creation FILETIME S, shown by tools as 1601-01-01 00:02:59 (S = 1794692339 gives 179 s). The
  nanoseconds are dropped. A Windows reader sees the year 1601 as "Date created" for every file and directory made on
  macOS, and for `.fseventsd` that macOS makes in each root. A: creation time is the real create time.
  `ntfs_macfuse_getxtimes` has the same raw-seconds error, but the backend does not call it, so the view stays
  1970-01-01.
- Writes move `$STANDARD_INFORMATION` mtime and ctime; `$FILE_NAME` times stay at the create time.
- `ntfsfix -n` clean after all of this.

### Explicit times (I-META-02)

- `touch -t`, `touch -m -t`, `utimensat` with ns, `cp -p` from APFS: stored as asked, floor to 100 ns
  (123456789 ns gives 123456700). `cp -p` copies mtime and atime with the same truncation. `cp` without `-p` gives the
  copy time.
- Range: 1601-01-01 00:00:00 (FILETIME 0), 1950, 2106 (past 32 bits) and 2200 are stored exactly.
- Pre-1970 value with a fraction (1950-06-15 12:00:00.25, 1969-12-31 23:59:59.5): the disk is exact, the view after a
  remount is rounded up to the next whole second (A too, so it is NTFS-3G's timespec conversion). A copy made from
  the view loses up to one second.
- ctime cannot be set: every set moves it to now.
- B only, atime handling in `setattr`: a request that sets mtime alone (`touch -m`, atime `UTIME_OMIT`) also sets
  atime to now; a request that sets atime alone (`touch -a`, mtime omitted) is dropped without an error and nothing
  reaches the disk (ctime does not move either). A honours both correctly. Only a call with both times stores an atime.
- Python and `touch` on macOS use `UTIME_OMIT` = -2 (Linux (1<<30)-2). The helper selects it per platform.

### atime and mount options (I-META-03)

- Fixture atime 2020-01-01, then a read, `readdir` and `stat` in a new session, atime read from disk:
  - `noatime`: unchanged. `atime`, `relatime` and the default (no option): moved to the read time, file and
    directory. B and A alike.
  - Product options (they contain `noatime`): unchanged.
  - A read never changes mtime.
- B: `stat` through the mount keeps showing the old atime after the update, until a remount (T-M2-04). A shows it at
  once. The relatime one-day rule was not exercised (fixture atime is far in the past).

### Creation time APIs (I-META-04)

- `setattrlist(ATTR_CMN_CRTIME)` returns success, `getattrlist` still reads 0. On disk the value is the raw-seconds
  FILETIME (`1234567890` for 2009-02-13 23:31:30.123456789, nanoseconds dropped).
- `cp -p` from an APFS file with an old birth time does not carry it: the copy has the create-time garbage.
- `system.ntfs_times` (xattr, 32 bytes: creation, data change, MFT change, access, little endian FILETIME) reads and
  writes the real values in B and A, with 100 ns exactness, and also updates the `$FILE_NAME` creation time. It is the
  only way found to read or set the true creation time on this backend (`xattr -px` and `-wx`). The live view does not
  change after the set (birth stays 1970). Tested on a default mount; not tested together with
  `streams_interface=openxattr`.
- Fix path: `ntfs_macfuse_setcrtime` and `getxtimes` must convert with `timespec2ntfs` and `ntfs2timespec`. That
  would store a right creation time; showing it needs the backend to use `getxtimes` (it does not).

### Effects of operations on times (I-META-05)

Moves of mtime/ctime/atime as `stat` shows them live, then the same values after a remount:

| Operation | mtime | ctime | atime shown | Note |
|---|---|---|---|---|
| write, truncate | moves | moves | no | |
| rename, move (file) | no | moves | no | |
| directory after rename, unlink, link, create, mkdir | moves | moves | no | |
| `utimensat` with the same value | no | moves | no | |
| `chmod` | no | no | no | I-PERM-03 |
| read | no | no | no in B, yes in A | atime update only on disk in B |
| `link()` | no | A: moves. B: moves on disk, live view stale | no | B returns EIO and still creates the name (below) |

- `link()`: B returns EIO, but the second name is created on disk (same MFT record) and shows after a remount; ctime
  of the file moves on disk, but the live view of the file and of the directory stays at the old values until a
  remount (the EIO path leaves the cache stale). A: success, ctime moves, view current. Hard links share the record
  and so the times.
- Live views equal the disk values, except atime in B and after `link()` in B.

### Read-only attribute (I-META-06)

- `chflags uchg` (file and directory): returns success, `st_flags` stays 0, nothing on disk. `nouchg` also succeeds.
- `chmod 0444` on a default or product mount: success, mode unchanged, disk unchanged (I-PERM-03).
- `system.ntfs_attrib` = 0x21 (READONLY + ARCHIVE) on a file or directory: stored on disk and persists across
  remounts. macOS shows nothing: mode 0644 (0755), `st_flags` 0, `ls -lO` empty.
- Nothing is enforced: append, truncate, rename, unlink and chmod succeed on a READONLY file, in the live session and
  in the next. Same in A.
- `permissions` mount (opt-in): `chmod 0444` sets READONLY on disk and the mode shows 0444; `chmod 0644` clears it.
  Writes still succeed (modes are display values, I-PERM). Same in A.

### Hidden flag (I-META-07)

- `chflags hidden` (file, directory): success, `st_flags` 0, nothing on disk.
- HIDDEN set with `system.ntfs_attrib` (0x22) is stored and shown by nothing: a default mount lists the entry,
  `UF_HIDDEN` is never set. SYSTEM (0x24) is the same.
- Dot-names: default mount stores no HIDDEN for `.name` files and directories. With `hide_dot_files` new dot-names
  get HIDDEN on disk (`0x22`, files and directories); existing ones are not changed, other names stay 0x20.
- `hide_hid_files`: names with HIDDEN are not listed (`readdir`, `ls -a`), but `stat` and open by name still work.
- `show_sys_files`: the NTFS system files are listed. B does not list `$MFT` (record 0: a directory entry with inode
  0 reads as deleted); A does.
- `noatime`, `relatime` and `atime` are the only options with an effect on times; `hide_*` and `show_sys_files` act on
  listing and on the attribute written at create.

## Not covered

- An image written by B read by Linux (the harness has no image transfer between environments). The on-disk
  decoding is independent of both drivers (and checked against `ntfsinfo`), so "equal to the Linux reading" is
  shown for the disk, not for a Linux mount of the same file.
- Windows-made fixtures with real creation times; reparse points; timestamps on named streams.
- The relatime one-day rule; `UTIME_NOW` per field; time zones (all runs UTC).
- Creation time with `streams_interface=openxattr` through `system.ntfs_times`.

## Proposed edits to shared files

1. `docs/RISK_REGISTER.md` R-09: creation time is not only "shown as 1970"; on disk every file made on macOS gets a
   wrong `$STANDARD_INFORMATION` creation time (raw seconds as FILETIME, 1601). Raise the impact and link this
   document. Add the effects to the limitations list (creation time, chflags uchg and hidden, READONLY, atime view,
   `link()` EIO).
2. New patch `third_party/patches/ntfs-3g/0005-macfuse-xtimes-conversion.patch`: in `ntfs_macfuse_getxtimes`,
   `ntfs_macfuse_setcrtime` and `ntfs_macfuse_setchgtime` use `ntfs2timespec()` and `timespec2ntfs()` instead of the
   raw `tv_sec`; then re-run `I-META-01` and `-04` (the pins `SI_CRTIME_IS_RAW_SECONDS` flip to false). Alternative:
   make the backend not call `setcrtime` on create (not controllable from our side).
3. `docs/IMPLEMENTATION_PLAN.md` 3.6: split the criterion. mtime, ctime, atime: met (I-META-01/-02/-03). Creation
   time: "stored correctly after patch 0005; not shown to macOS (R-09)".
4. `docs/TEST_STRATEGY.md` I-META row: list `I-META-01..07` and the files; add that creation time is read with
   `system.ntfs_times`.
5. `docs/ARCHITECTURE.md` section 3.3 and `integration/mount-options.json`: keep `noatime` in the product line (the only
   way atime stays unchanged on disk); `hide_dot_files`, `hide_hid_files` and `show_sys_files` stay out (they change
   listing or write HIDDEN; decision for the owner of the interop rules). The text "atime changes are not visible" now
   also holds for reads in general in B.
6. `tests/integration/permissions/i_perm.py`: its helpers (`mount_opts`, `ntfsinfo`, `walk_entries`) cannot be
   imported by another case module, because importing a case module registers its cases again and the harness
   reports a duplicate id. Move them to a helper module without `@case`, as `functional/fsops.py` is.
