<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Limitations and known behaviour

What ntfs-macos does not do, or does differently from what you may expect on a Mac or on Windows.
Read it before you trust a volume with data you cannot lose. Installation is in
[INSTALL.md](INSTALL.md).

The product is verified in virtual machines on macOS 27 (Apple silicon) and on disk images. It has
not been run against real USB drives, Windows `chkdsk` or Time Machine yet (Environment C,
`docs/HARDWARE_TEST_PROTOCOL.md`, criterion 5.8 of `docs/IMPLEMENTATION_PLAN.md`). Statements about
a drive cache therefore rest on the macOS flush call being made, not on a measured drive.

## Limitations

The table below is copied word for word from `docs/ARCHITECTURE.md` section 9 (the rows N1 to N5
and N9 to N22; the rule that requires this is stated under the table there). The numbers are the
ones used in the rest of the project documents. Terms such as "worker", "patch 0007" and "session
channel" are explained in `docs/ARCHITECTURE.md`; the short version is: the worker is the NTFS-3G
process that serves one mounted volume, and the numbered patches are this project's changes to
NTFS-3G (`third_party/patches/ntfs-3g/`). A `W flush` record is a line in the session report that
the daemon keeps for each mounted volume.

| # | Not guaranteed | Reason |
|---|---|---|
| N1 | `fsync(fd)` returning 0 means that file's data is on stable storage. | `fsync` reaches the worker as `FUSE_FSYNC`, which `ntfs_fuse_fsync()` turns into a whole-device sync. Without patch 0007 that is a plain `fsync` of the buffered node and 0 means "handed to the driver" (T-M3-10). With it the worker also sends `DKIOCSYNCHRONIZE`, but a failed flush still returns 0 (only a `W flush` record shows it) and whether drives honour it is [U] until Environment C. A repeated `fsync` with no write in between does not touch the device (`device.c:173`). |
| N2 | `close(fd)` returning 0 means the data is on stable storage. | `close` gives `FUSE_RELEASE` only; there is no flush step and so no flush error. A write error is returned by `write()` itself; `fsync` and `close` do not report an earlier failed `write` (T-M2-06). |
| N3 | `write(2)` returning means the data is on the disk. | The worker has received and answered the write, but `libntfs-3g` writes into the device's page cache; only a sync point flushes it (T-M2-06). |
| N4 | Atomicity or rollback of a multi-step metadata update after power loss or disconnect. | NTFS-3G does not replay or write the NTFS journal. |
| N5 | `F_FULLFSYNC` semantics for applications (databases, Time Machine-style tools). | `F_FULLFSYNC` arrives as the same `FUSE_FSYNC` as `fsync` (T-M2-06), and the worker syncs the whole device. On a node upstream's own `F_FULLFSYNC` fails (`ENOTTY`, T-M3-10); the drive flush is patch 0007's `DKIOCSYNCHRONIZE`, which fails silently towards the application and is unverified on real drives. Documented as "not supported for workloads that need fsync durability". |
| N9 | That files without an execute bit cannot be run. | Executable files on the volume run whatever their mode: the backend does not check the execute bit (`I-PERM-05`: a 0644 script, a copy of `/usr/bin/true` and a compiled binary run, also on a read-only mount). `fmask=0133` and `chmod +x` have no effect on this; `chmod` returns success and changes nothing (`I-PERM-03`) [V: T-M3-20, `docs/spikes/M3-permissions.md`]. |
| N10 | File modes and ownership as access control. Modes are metadata on this backend, not permissions. | The mount is `noowners`: `ls -ln` shows the mounting user to that user and to root, and shows the caller's own ids to any other account. `chmod` and `chown` return success and change nothing (`I-PERM-03`, `-04`). A mode-000 file and a 0700 directory are readable and writable by another local account, also with `permissions` (`I-PERM-07`) [V: T-M3-20]. |
| N11 | Durability of synced data on a device that can tear a 4 KiB write. | A torn MFT record (1 KiB) or index block (4 KiB) fails its update-sequence check ("Incomplete multi-sector transfer"): that is NTFS's protection working, not a bug. Without a journal NTFS-3G cannot redo the record, so data that was already synced becomes unreadable until `chkdsk` (115 broken claim sets in the torn-write sweep `short:N:512`). Synced data survives an interruption only on devices whose 4 KiB writes are atomic; USB bridges and flash without power-loss protection give no such promise [V: T-M3-14 triage, section 5]. |
| N12 | That every interrupted session leaves a volume that mounts read-only. | An interruption between NTFS-3G's `$MFT` and `$MFTMirr` writes leaves a volume no NTFS-3G mount accepts; the product refuses it until `ntfsmac repair --sync-mirror` or Windows `chkdsk` (section 3.7, D3 exception). The window opens for every new MFT record (8.0 % of device writes in the `smallfiles` sweep workload). |
| N13 | Case-insensitive names as on APFS or Windows. The volume is case-sensitive: `Foo`, `foo` and `FOO` are three files, a lookup must match the case exactly, and a case-only rename works. | NTFS-3G's POSIX namespace; no `ignore_case` in `ntfs-3g` (ADR-012) [V: T-M3-05, `I-NAME-06`]. Mac tools that assume a case-insensitive volume may mishandle such pairs, and Windows lists both but opens only one. |
| N14 | Names longer than 255 UTF-8 bytes (possible with CJK or emoji: NTFS allows 255 UTF-16 units). | They can be created and opened by their exact name, but a listing returns them cut to 255 bytes (sometimes inside a character), the listed name does not open, several such names can list as one, and `rmdir` of a directory that holds only such names fails with `ENOTEMPTY` although it looks empty [V: T-M3-05, `I-NAME-03`]. |
| N15 | That a name keeps the Unicode form a program or another system gave it; access to files whose name on disk is not NFC. | Names created on the Mac are stored NFC whatever form was typed, and listed in NFD. A volume that holds a name in another form (written by Linux or Windows tools) is mounted read-only, because such a file is listed but cannot be opened and deleting its listed name can delete another file (ADR-023, section 3.1 check 5a); rename such files elsewhere. Within one session, a file opened by both its NFC and its NFD spelling can return stale data through the second spelling until remount [V: T-M3-05, `docs/spikes/M3-names.md`]. |
| N16 | Keeping files private from other accounts on the same Mac. | Every other local account can read, change and delete every file on a mounted NTFS volume, whatever the shown modes and owners, mode-000 files and 0700 directories included, also with `permissions` (`I-PERM-07`). Only a read-only mount limits that, to reading. User text: "Other accounts on this Mac have full access to a mounted NTFS volume." [V: T-M3-20] |
| N17 | Creation time and file flags as on APFS. | macOS shows no creation time (`st_birthtime` is 1970-01-01); the right value is stored on disk with patch 0008 (ADR-024) and before it every file made on the Mac got a 1601 creation time. `chflags uchg` and `chflags hidden` return success and are lost. The NTFS READONLY and HIDDEN attributes are not shown, and READONLY is not enforced (a file with it can be written, renamed and deleted) [V: T-M3-06, `I-META-04`, `-06`, `-07`]. |
| N18 | atime and other times exactly as set or as used. | A call that sets only atime (`touch -a`, mtime omitted) is dropped without an error; one that sets only mtime also sets atime to now. Reads that move atime on disk are not shown until a remount (`noatime` keeps it unchanged on disk). ctime cannot be set. A time before 1970 with a fraction of a second is stored exactly but shown rounded up to the next second. `$FILE_NAME` times are not updated by writes, so Windows tools that read them see the create time [V: T-M3-06, `I-META-02`, `-03`, `-05`]. |
| N19 | Hard links. | `link()` returns `EIO` but the second name is created on disk and appears after a remount; until then the views of the file and the directory are stale [V: T-M3-06, `I-META-05`; T-M3-08, `I-XATTR-07`]. |
| N20 | Extended attributes without limits. | Names at most 127 bytes; values at most 262143 bytes (Linux reads at most 64 KiB through `getxattr`). `com.apple.FinderInfo` must be exactly 32 bytes, and an all-zero FinderInfo reads as `ENOATTR` (macOS side of the backend). `cp -p` and Finder-style `copyfile(3)` from the volume to APFS fail with `EINVAL` on a file with a resource fork; use `ditto`. Until patch 0009, xattr names with `:` (Finder tags, comments) are refused (ADR-025) [V: T-M3-08, `docs/spikes/M3-xattr.md`]. |
| N21 | That data written through `mmap` is safe once `msync` returns 0. | Mapped writes stay in the kernel's page cache until a flush event or the syncer tick (about 30 s, N7). If the worker dies first, those writes are lost with no error: `msync(MS_SYNC)` and `munmap` return 0, pages in memory can still be read and written, and a file can be left half-updated (pages synced before the death new, later pages old). Other calls on the volume fail with `ENXIO`. User text: "Programs that write files through memory mapping can lose up to about 30 seconds of changes, without an error, if the NTFS process stops unexpectedly." [V: T-M3-11, `I-MMAP-07`] |
| N22 | Sparse and compressed files kept as compact as on Linux or Windows. | A write past EOF without a preceding `ftruncate` fills the gap with zeros on disk (Linux NTFS-3G makes a hole); `ftruncate` up makes a sparse file, allocated in units of 64 KiB. `SEEK_DATA` and `SEEK_HOLE` do not see holes (data at 0, hole at EOF), and `fcntl(F_PUNCHHOLE)` returns `ENOTTY`. An update to a compressed file stores each touched 64 KiB unit uncompressed (a 4 byte overwrite in a 64 KiB text file grew it from 8 KiB to 64 KiB allocated). Space only; no data is lost [V: T-M3-09, `I-BIG-SPARSE-01`, `I-BIG-COMP-01`, `docs/spikes/M3-readpath-big.md`]. |

## Further limitations found in milestone 5

These are recorded in the repository and are not part of the table above.

### Very large directories are slow with macFUSE 5.5.0 (the `dir100000` case)

- **Behaviour.** With macFUSE 5.5.0 the product slows down when a volume holds directories with tens
  of thousands of entries and other programs keep directories of the volume open. In the test
  environment that runs through the macFUSE module, creating 100,000 entries in one directory slows
  down until it stalls, deleting small files ran at about 150 files per second (the same bench on
  NTFS-3G in the Linux container environment, without the module: 8597 files per second), and in one run the remount after 20,000
  entries did not complete within 90 s. The performance case `dir100000` could not pass in that
  environment, and the first failing run stopped with `[Errno 2] No such file or directory`. A
  directory of 10,000 entries listed at about 1,900 entries per second.
- **Cause, as far as the evidence goes.** The macFUSE FSKit module (`io.macfuse.app.fsmodule.macfuse-local`),
  not this project's code: once other clients hold directory handles open, each request is followed
  by a volume-wide synchronize that sends one `FSYNCDIR` per held handle. The engine and the test
  passed with the same sizes without the module. One part is not excluded: libfuse answering
  `ENOENT` for a `FSYNC` of a removed node.
- **Status.** The project has not decided how the performance criterion 5.6 treats this case; the
  owner's options are listed in the source. A report to macFUSE is drafted and not sent.
- **Source.** `docs/spikes/M5-perf.md`, section "Criterion 5.6 at ac5cf2b", and
  `docs/UPSTREAM_REPORTS.md`. Measured only in virtual machines on disk images; a physical drive was
  not measured.

### `ntfsmac unmount` can wait for Spotlight and Quick Look (ADR-028, still proposed)

- **Behaviour.** After heavy writes, `mds`, `mdworker` or Quick Look can still hold files of the
  volume, and a plain unmount is then refused as busy. The proposed behaviour: when every holder is
  one of those programs, `ntfsmac unmount` asks again every 2 s for at most 60 s, so it can take up
  to about 72 s to settle instead of being refused. `--force` ends the wait and the forced unmount
  runs instead. A Finder eject and an unmount through Disk Arbitration do not wait: they are
  refused at once and name the holder when the system reports one. Any other holder is reported at
  once. Spotlight stays on and the product hides nothing. It writes nothing to the volume itself,
  but Spotlight may still write its own `.Spotlight-V100` index there (to be revisited if a desktop
  session shows it, risk R-26).
- **Status.** ADR-028 is Proposed (owner decision pending); the text above describes the design in
  the repository, which may change.
- **Source.** `docs/TECHNICAL_DECISIONS.md` ADR-028. The hold was seen once in a 30 minute soak run;
  the wait is tested with stand-ins, not reproduced in a guest with a running indexer.

### Two disks with the same serial number: the clone is mounted read-only

- **Behaviour.** The journal of unsafe volumes is keyed by the NTFS volume serial number, which a
  clone or a copy of a disk image shares with the original. If the original has an open journal
  entry (it was not unmounted cleanly), a clone of it is also mounted read-only until the entry is
  cleared with `ntfsmac journal-clear` or `ntfsmac repair`. A clone cannot clear the entry of an
  original that is mounted read/write at that moment. The product errs towards read-only here and
  accepts it.
- **Two disks with the same serial attached at the same time** (a clone next to its original, or two
  copies of one image): the second one is mounted read-only with the reason `same_serial`, because a
  live session owns that serial. Remedy: detach one of the disks, or give one of them a new serial
  number on Windows. Source: `src/daemon/session.c` (`ntfsmac_session_serial_token`) and the reason
  text in `src/daemon/report.c`.
- **Source.** `docs/ARCHITECTURE.md` section 3.5 (key collisions, `journal-clear` gate).

### The package is not signed (ADR-011)

- **Behaviour.** The release package is not signed with an Apple Developer ID and not notarized
  unless the release notes say otherwise. Gatekeeper blocks a double-click install of it. Install it
  with `sudo installer` from Terminal; see [INSTALL.md](INSTALL.md). The installed programs carry
  ad hoc signatures only. A signed release needs an Apple Developer Program membership and is
  scripted in `packaging/sign-and-notarize.sh`; it has not been run.
- **Source.** `docs/TECHNICAL_DECISIONS.md` ADR-011, `docs/IMPLEMENTATION_PLAN.md` criterion 6.4 and
  item H8.

## Other behaviour that follows from the design

- macFUSE is not part of the package. You install it and approve its file system extension in
  System Settings yourself (`ntfsmac doctor` shows whether it is approved).
- Volumes that are dirty, hibernated (Windows Fast Startup), have an unclean log, or hold names in
  a form that cannot be handled are mounted read-only. `ntfsmac status <bsd>` shows the last check of a volume. A Windows `chkdsk`, or `ntfsmac repair` where it applies, is the way back to read/write.
- `diskutil mount diskNsM` is not routed to this product; use `ntfsmac mount` (ARCHITECTURE.md
  section 8.3).
- Intel Macs and macOS before 27 are not supported.
