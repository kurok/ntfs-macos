# M2 spike: device by inherited descriptor, `ntfs-3g /dev/fd/N` (T-M2-05)

Date: 2026-11-14. Answers U5. Raw logs: `tests/results/evidence/m205/`.
Environment: one disposable clone of `ntfsmac-golden` (macOS 27.2 26B123, macFUSE 5.5.0, as in
`M2-backend.md`), deleted at the end. Disk: fixture `clean-256m-4k` exported from Environment A
(`tests/fixtures/gen/clean.sh --only clean-256m-4k --export build/fx-m205`), a copy attached with
`tart.sh run m205 --disk build/fx-m205/disk.img`. The guest saw it as `/dev/disk1`
("Apple Inc. VirtIO Block Media", 268435456 bytes, Fixed), found by size, never by number.

## Answer

**U5: yes, with patch 0003.** A root parent opens the device (`O_RDWR` or `O_RDONLY`), drops a child
to `admin` (uid 501, no `operator` membership, cannot open `/dev/disk1`: `Permission denied`), and the
child runs `ntfs-3g /dev/fd/3 MOUNTPOINT -o backend=fskit,local,norecover,uid=501,gid=20[,ro]`. With
patch 0003 this mounts, writes, reads back, unmounts and passes `ntfsfix -n`, for the attached block
device (3 runs, 16/16 checks each) and for an image file (1 run, 15/15). **Unpatched NTFS-3G fails in
all cases**, for three separate reasons (below), so the patch is required.

## `/dev/fd/N` semantics on macOS 27 (ADR-020 assumption)

Measured with `fd_pass semantics` as root and in an unprivileged child that inherited the
descriptor (`unpatched-blockdev.log`, `unpatched-image.log`, `edge-cases.log` E8, `realpath-devfd.log`).
Same results for `/dev/disk1` and a regular file.

| Question | Result |
|---|---|
| Does `open("/dev/fd/N")` share the open file description with N? | **Yes.** Offset 4096 set on N is the new descriptor's offset at open; seeking the new one to 8192 moves N's offset to 8192. `O_NONBLOCK` set on the new one shows on N. Same `st_dev`/`st_ino`/`st_rdev`. The new descriptor does not get `FD_CLOEXEC`. |
| Access mode of the new descriptor | **The original's, not the requested one.** N `O_RDWR`, `open("/dev/fd/N", O_RDONLY)` returns a descriptor whose `F_GETFL` is `O_RDWR`. |
| Wider mode than the original | Refused: N `O_RDONLY`, `open(..., O_RDWR)` (with or without `O_EXCL`) gives `EACCES`, also for root. |
| Unprivileged process, inherited N, no permission on the file/device | **`open("/dev/fd/N")` fails with `EACCES`**, even `O_RDONLY` and although the process holds N. With permission on the underlying file (mode 0644, owned by the user) it succeeds (E8). So the open re-checks the caller's permission on the underlying vnode; the descriptor alone is not enough. |
| `stat("/dev/fd/N")` | Works unprivileged; reports the underlying type and `st_rdev` (block device `0x1000004`), but a synthesised mode (`060440` for the block device, `100444` for a mode-000 file). |
| `realpath("/dev/fd/N")` | Block device: `/dev/fd/3` (unchanged). **Regular file: `/dev/fd/<file name>`**, e.g. `/dev/fd/img0.img`, which does not exist. `/dev/fd/03` becomes `/dev/fd/3`. `F_GETPATH` gives the real path. |
| `fcntl(F_SETLK)` on N | Read lock on `O_RDONLY`, write lock on `O_RDWR`: both ok on the block device and on a file. |

Consequence for ADR-020: the [A] "opening `/dev/fd/N` shares the open file description" is **verified**,
and it is worse than assumed: a re-open does not even narrow the access mode, so the rejected option
("open once `O_RDWR`, give the probe a read-only view") is correctly rejected. For an unprivileged
consumer a re-open is not possible at all, so any program that gets a device by inheritance must use
the descriptor itself (`dup`), not `open("/dev/fd/N")`.

## Unpatched NTFS-3G 2026.9.28: exact errors

Block device, `O_RDWR` and `O_RDONLY` descriptor (`unpatched-blockdev.log`, exit 19 both):

```
Unprivileged user can not mount NTFS block devices using the external FUSE
library. Either mount the volume as root, or rebuild NTFS-3G with integrated
FUSE support and make it setuid root. Please see more information at
https://github.com/tuxera/ntfs-3g/wiki/NTFS-3G-FAQ
```

`stat("/dev/fd/3")` follows to the block device, so `ctx->blkdev` is set and `main()` refuses before
opening (`src/ntfs-3g.c` line 4449, `getuid() && ctx->blkdev`, external FUSE only).

Image file (mode 000, so the child cannot open it by path; `unpatched-image.log`, exit 19 both):

```
Failed to access '/dev/fd/img0.img': No such file or directory
Please check '/dev/fd/img0.img' and the ntfs-3g binary permissions,
```

`ntfs_parse_options()` canonicalises the device name with `realpath()`, which rewrites `/dev/fd/3`
(see table).

`ntfsfix -n /dev/fd/3` as the child (block device and file, exit 1):

```
Mounting volume... Error opening '/dev/fd/3': Permission denied
```

`libntfs-3g/unix_io.c` re-opens the name with `open()`, which fails as shown in the semantics table.
This third failure would also hit `ntfs-3g` once the first two are removed.

## Patch 0003 (`third_party/patches/ntfs-3g/0003-inherited-device-descriptor.patch`)

4 files, +124/-1 (after review, see "Review changes"), every hunk under `#ifdef __APPLE__` (and the `ntfs-3g.c` ones also under the
existing `#ifndef FUSE_INTERNAL`). The Linux build is unchanged.

| Hunk | What | Why |
|---|---|---|
| `include/ntfs-3g/device.h`, `libntfs-3g/unix_io.c`: `ntfs_device_inherited_fd()` | Returns N only for the exact name `/dev/fd/N` (decimal, first digit 1-9, 3 <= N, parsing stops above about 10^6) when `fcntl(N, F_GETFD)` says N is open in this process; else -1. One parser, exported from the library, used by the other hunks. | Recognise only a descriptor the process already holds. Anything else is an ordinary path. |
| `unix_io.c` `ntfs_device_unix_io_open()` | For such a name: `fstat(N)` instead of `stat(name)`; `fcntl(N, F_DUPFD_CLOEXEC, 3)` instead of `open(name)`, then `FD_CLOEXEC` on N (N stays open); no `O_EXCL`. A write-only N is refused; a read/write request on an N that is not `O_RDWR` fails with `EBADF` and the message `Descriptor '/dev/fd/N' is read-only, read-write was requested`. The advisory lock is still taken, but gives no lasting exclusivity (`daemon()` forks after `ntfs_open()`, and POSIX locks do not survive the fork); exclusivity comes from the daemon (ADR-020 binding, Disk Arbitration dissenter). | macOS refuses the re-open for an unprivileged process. The duplicate shares N's open file and access mode, so it can never be more than N. `EBADF`, not `EROFS`: `ntfs_device_mount()` (`volume.c` ~549) retries read-only on any `EROFS`, which would turn a read/write request into a silent read-only mount. Size and sector size come from `DKIOCGETBLOCKCOUNT`/`DKIOCGETBLOCKSIZE` on the duplicate (unchanged code; works, 268435456 bytes). |
| `src/ntfs-3g_common.c` `ntfs_parse_options()` | Keep `/dev/fd/N` as given instead of `realpath()`. | `realpath()` rewrites it for regular files. |
| `src/ntfs-3g.c` `main()` + `inherited_blkdev()` | The non-root block device refusal is skipped only when the device name is an inherited descriptor and `fstat` of it is a block device. (The first version also compared `st_rdev` with `stat(name)`; through fdesc that compares N with itself, so it was dropped in review.) | The refusal exists because Linux `fuseblk` needs root. Here the device was opened by the privileged parent; the child only uses it. A block device given by path is still refused (E3: exit 19 with the patched binary). |

Why it is safe: the only new capability is "use a descriptor you already hold", which a process can
always do with `read`/`write` on N directly. No path is opened with new privileges, no access mode is
widened, and the refusal for non-root block devices by path is kept. `ctx->blkdev` stays set for an
inherited block device, so NTFS-3G still adds `blkdev,blksize=4096` to the FUSE options; macFUSE 5.5.0
with `backend=fskit` accepts them (E6 foreground: `Mount options: backend=fskit,local,ro,allow_other,
nonempty,relatime,default_permissions,fsname=/dev/fd/3,blkdev,blksize=4096`, mount appears).
Scope: the `unix_io.c` change is library-wide on macOS. Every libntfs-3g user (`ntfs-3g`, `ntfsfix`, `mkntfs` and the other ntfsprogs, `ntfs-3g.probe`, and our probe and volmark) accepts `/dev/fd/N` this way, and the library exports one new symbol. `lowntfs-3g` gets the `realpath` change through `ntfs-3g_common.c` but keeps its own block-device refusal (`lowntfs-3g.c` ~4721), so it still refuses inherited block devices; it is not patched and must not be shipped. Upstream status: not submitted.

Build checks: `scripts/build-ntfs3g.sh` applies it in the guest (`build-patched.log`: "applying
0003-inherited-device-descriptor.patch", 17 `warning:` lines, same as unpatched `build-unpatched.log`,
none in patched code) and in Environment A (`build-linux-envA.log`, `vm/linux/run.sh run -- sh -c
'sh /work/scripts/build-ntfs3g.sh --build-dir /tmp/b --prefix /tmp/deps'`, exit 0). No change to
`scripts/build-ntfs3g.sh` was needed.

## I-SMOKE-02

`tests/integration/smoke/fd_pass.sh TARGET EXPECT_BYTES` in the guest as `admin`; `fd_pass.c` (built
with `-Wall -Wextra -Werror` in the guest) is the only thing started with `sudo -n`. The parent checks
size and NTFS boot sector before opening read/write; the child drops to `admin` with `setgroups`/
`setgid`/`setuid`, proves it cannot open the target by path, and execs with `POSIX_SPAWN_SETEXEC |
POSIX_SPAWN_CLOEXEC_DEFAULT`, inheriting only 0-2 and the device descriptor.

| Target | Descriptor | Result | Log |
|---|---|---|---|
| `/dev/disk1` (attached image) | `O_RDWR`, rw mount | mount `(macfuse, local, nodev, nosuid, noowners, noatime, fskit, mounted by admin)`, not read-only; write, read back, 1 MiB random file; umount; driver exits | `patched-blockdev-run{1,2,3}.log` |
| `/dev/disk1` | `O_RDONLY`, `ro` | mount shows `read-only`; file and SHA-256 from the rw case intact; write refused; umount; driver exits | same |
| `/dev/disk1` | `O_RDONLY`, `ntfsfix -n /dev/fd/3` as `admin` | exit 0, `Mounting volume... OK` | same |
| image file, mode 000 | `O_RDWR` / `O_RDONLY` / `ntfsfix -n` | same checks, all pass | `patched-image-run1.log` |

Runs: block device 3x16/16 PASS, image 1x15/15 PASS. Unpatched: 3 FAIL each (`unpatched-*.log`).
The mount source is a macFUSE synthetic disk (`/dev/disk7s1`), not `/dev/fd/3`.

Edge cases (`edge-cases.log` E1-E10, `edge-cases-2.log` F1-F4 with `no_detach` so that messages after
`daemon()` are visible). In `edge-cases.log`, E5 unmounted before its mount appeared, so its driver and
then E6's were stopped with `SIGKILL`; F3 is the clean run of that case.

- `O_RDONLY` descriptor without `ro` (first patch version): `Error opening '/dev/fd/3' read-write`,
  `Could not mount read-write, trying read-only`, exit 0, **mounts read-only**. Cause: the patch
  returned `EROFS`, and `ntfs_device_mount()` in `volume.c` (~549) retries `O_RDONLY` on any `EROFS`,
  not gated on `NTFS_MNT_MAY_RDONLY`. Fixed in review (now fails, exit 18; see "Review changes").
- `O_RDWR` descriptor with `ro`: mounts read-only (allowed by NTFS-3G; forbidden by argv rule 7).
- Patched binary, non-root, `/dev/disk1` by path: still refused, exit 19.
- `/dev/fd/7` not open: `Failed to access volume '/dev/fd/7': Bad file descriptor`, exit 11.
- `/dev/fd/03`: not matched by the parser, but `realpath()` canonicalises it to `/dev/fd/3`, which
  then is the inherited descriptor and mounts. Harmless (still the process's own descriptor); the
  daemon's argv rule 1 already forbids a leading zero.

## Automount handling

The attached disk is automounted by macOS read-only with `ntfs.fs` (`/dev/disk1 on /Volumes/clean256m4k
(ntfs, local, nodev, nosuid, read-only, noowners, noatime, fskit)`). While it is mounted **even a root
`open(O_RDONLY)` of `/dev/disk1` fails with `EBUSY`** (first attempt, `fd_pass: open /dev/disk1:
Resource busy`). `fd_pass.sh` therefore runs `diskutil unmount` (as `admin`, no sudo; "Volume
clean256m4k on disk1 unmounted") before any open, checking the size through `diskutil info -plist`,
which does not open the device. After our own mounts and unmounts macOS did not re-mount the disk
(E9). This confirms U21's premise: in the daemon, the DA dissenter (or an unmount) must come before
step 4's probe open, not only before the `O_RDWR` open.

## Review changes (independent review: accept with changes, no bypass found)

Applied to patch 0003 and re-verified in a fresh clone of the golden VM (deleted afterwards):

1. A read/write request on a non-`O_RDWR` inherited descriptor returns `EBADF` instead of `EROFS`, so the
   ungated `EROFS` retry in `volume.c` cannot turn it into a read-only mount.
2. After the `F_DUPFD_CLOEXEC` duplicate, N is marked `FD_CLOEXEC`. N is not closed: a retry duplicates
   it again, and closing any descriptor of the vnode drops the process's POSIX lock.
3. A write-only inherited descriptor is refused for any request with a clear message.
4. The `st_rdev` comparison in `inherited_blkdev()` is dropped: through fdesc, `stat("/dev/fd/N")` and
   `fstat(N)` describe the same vnode, so it compared N with itself. No concrete reason to keep it.
5. Patch header: the lock gives no lasting exclusivity; the change is library-wide on macOS with one new
   exported symbol; `lowntfs-3g` still refuses inherited block devices and must not be shipped.

`fd_pass.c` got a `wo` mode (O_WRONLY descriptor, negative tests only), and `run` now checks the target
through its own read-only descriptor before opening the requested mode.

Results (`*-rev.log`): guest build with the patch applied, 17 `warning:` lines as before, none in patched
code (`build-patched-rev.log`); Environment A build exit 0 (`build-linux-envA-rev.log`); I-SMOKE-02 block
device 3 runs x 16/16 PASS, image 1 run 15/15 PASS (`patched-blockdev-run{1,2,3}-rev.log`,
`patched-image-run1-rev.log`). Edge cases (`edge-cases-rev.log`):

| Case | Result |
|---|---|
| R1 `O_RDONLY` descriptor, no `ro` | `Descriptor '/dev/fd/3' is read-only, read-write was requested`, `Failed to mount '/dev/fd/3': Bad file descriptor`, exit 18 (`NTFS_VOLUME_UNKNOWN_REASON`), no mount |
| R2 `O_WRONLY` descriptor, `ro` | `... is write-only, read-only was requested`, exit 18, no mount |
| R3 `O_WRONLY` descriptor, rw | `... is write-only, read-write was requested`, exit 18, no mount |
| R4 `O_RDWR` descriptor with `ro` | mounts read-only (allowed by NTFS-3G; forbidden by argv rule 7) |
| R5 non-root, `/dev/disk1` by path | still refused, exit 19 |
| R6 `ntfsfix -n` on `O_WRONLY` | refused (`Bad file descriptor`), exit 1 |
| R7 running driver | `lsof` shows two read descriptors on `/dev/disk1` (3 = inherited N, 4 = duplicate) |

## Open issues

- The driver keeps the inherited descriptor N open next to its `F_DUPFD_CLOEXEC` duplicate for its whole
  life (N is `FD_CLOEXEC` after review; R7 shows both). No child process of the driver was seen in these runs, but the sandbox/`posix_spawn` design should not assume the worker holds a single reference.
- A stuck no-mount `ntfs-3g` still needs `SIGKILL` (as in T-M2-03); `fd_pass.sh` does that on timeout.
- Whether the FSKit backend gets `blkdev`/`blksize` semantics or just ignores them is not known; the
  mount works either way.
- Removable (USB) media and partitioned disks (`/dev/diskNsM`) were not tested; only a whole virtio disk
  and an image file. Environment C.

## Proposed doc changes (not applied)

1. ARCHITECTURE.md section 11, U5: answered yes with patch 0003; record the three unpatched failures and
   that the fix is `dup` of the inherited descriptor, not a re-open.
2. ADR-020 table: replace "[A]" with "[V: T-M2-05]" and add "the re-opened descriptor keeps the original
   access mode (`O_RDONLY` request on an `O_RDWR` descriptor gives `O_RDWR`), and an unprivileged process
   cannot re-open `/dev/fd/N` without permission on the device (`EACCES`)".
3. ARCHITECTURE.md 4.1 step 8 / 3.3: the worker's exit status does not prove the mode. After every mount
   the daemon must compare `MNT_RDONLY` from `getfsstat` with its decision and treat a mismatch as
   `Failed` (or `ro` with reason), in addition to argv rule 7. Patch 0003 now fails hard for a read-only
   descriptor with a read/write request, but NTFS-3G can still fall back to read-only on its own (a
   hibernated volume or an unclean `$LogFile` with `norecover`: `ntfs-3g` sets `NTFS_MNT_MAY_RDONLY` whenever `remove_hiberfile` is absent).
4. ARCHITECTURE.md 4.1 step 3/4 and U21: the macOS automount makes even a root `O_RDONLY` open fail with
   `EBUSY`, so the dissenter must be in place (or the volume unmounted) before the probe open; record that
   `diskutil unmount` as the console user works and the disk is not re-mounted afterwards.
5. ARCHITECTURE.md 3 (components table) / 5: the worker holds two references to the device (inherited N
   and NTFS-3G's `F_DUPFD_CLOEXEC` duplicate). Device exclusivity text stays true (same open file).
6. ARCHITECTURE.md 3.3: note that NTFS-3G adds `blkdev,blksize=<cluster size, max page size>` for an
   inherited block device, accepted by macFUSE 5.5.0 FSKit, next to the existing note about
   `allow_other,nonempty,fsname=`. `fsname=/dev/fd/N` is what libfuse gets.
7. third_party/patches/ntfs-3g/README.md numbering table: 0003 status "Needed (T-M2-05), applied"; Opus
   review pending per AGENT_TASKS.md.
8. TEST_STRATEGY.md: register I-SMOKE-02 as `tests/integration/smoke/fd_pass.sh` (Environment B, needs
   passwordless sudo in the guest for `fd_pass` only).
