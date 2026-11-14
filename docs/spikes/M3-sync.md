# M3 sync path: `ntfs_fsync()` at each sync point, raw versus buffered node, cost of `sync` (T-M3-10, U10, ADR-014)

Date: 2026-11-14. Answers the T-M3-10 row of `docs/AGENT_TASKS.md` and U10. Cases: `tests/integration/sync/i_sync.py`
(`I-SYNC-01`, `I-SYNC-02`), interpose library `tests/integration/sync/syncspy.c`, dtrace check
`tests/integration/sync/dtrace_check.sh`. Result file: `tests/results/2026-11-14-26314ba-B.json` (both cases pass);
evidence: `tests/results/evidence/2026-11-14-26314ba-B/`, `tests/results/evidence/m310/` (dtrace and virtio runs).
Builds on `docs/spikes/M2-sync.md`.

## Answer

1. **On an image file, `ntfs_fsync()` works as ADR-014 says.** Each sync point that reaches the worker while the device is
   dirty ends in exactly one `fcntl(F_FULLFSYNC)` on the device, it returns 0, and the `fsync` fallback does not run
   (5 device syncs in the whole session: one at mount, one each for `fsync`, `F_FULLFSYNC` and `sync(2)`, one at unmount).
2. **On a device node it never works.** `fcntl(F_FULLFSYNC)` on `/dev/diskN` and on `/dev/rdiskN` fails with `ENOTTY` (25) at
   every sync point, on a disk image attached with `hdiutil` and on a VirtIO block device. The plain `fsync` fallback then runs
   every time, returns 0, and nothing tells the application or the user. **The product mounts device nodes, not image files, so
   the `F_FULLFSYNC` branch of upstream `ntfs_fsync()` is dead code in production, and the fallback is the normal path.** The
   premise of ADR-014 and of D1 (`upstream ntfs_fsync() issues F_FULLFSYNC in the worker`) is true only for image files.
3. **`ioctl(DKIOCSYNCHRONIZE)` works on both nodes** (return 0 on the `hdiutil` disk and on the VirtIO disk, buffered and raw). It
   is the call that does what `F_FULLFSYNC` does for a file. Whether the drive cache is really flushed cannot be seen from a VM
   (Environment C).
4. **The raw node cannot be used.** `libntfs-3g` does no alignment handling on Darwin. The kernel refuses reads and writes on
   `/dev/rdiskN` whose offset or length is not a multiple of 512 (`EINVAL`), and NTFS-3G issues such I/O from the first
   moments of a mount (an 8-byte read at offset 8192). A raw mount fails before it is complete. The buffered node works and a
   write workload on it leaves a volume that `ntfsfix -n` accepts.
5. **The `sync` mount option is far too expensive: 88 to 94 percent of sequential write throughput on the image file (658 to 37 MB/s in the recorded run).** It makes `ntfs_pwrite()` call the
   device sync after every write (30 device syncs for a 4 MiB write instead of 0). The ADR-014 threshold is 30 percent.
   On a node the option costs nothing in this VM, but only because there `F_FULLFSYNC` fails at once and `fsync` is cheap: it
   adds no durability either. `sync` stays out of the allow-list.

## Method

| Item | Value |
|---|---|
| Guest | disposable clone of `ntfsmac-golden` (`tests/harness/run-env-b.sh`), macOS 27.2 (26B123), arm64, macFUSE 5.5.0, SIP disabled in the golden VM |
| Worker | NTFS-3G 2026.9.28 built in the guest by `scripts/build-ntfs3g.sh --fuse external` (series with 0003; 0004 is in the series but not compiled in) |
| Mount | `ntfs-3g -o backend=fskit,local,norecover,uid,gid,no_detach[,debug][,sync]`, absolute mount point, a 30 s mount deadline and SIGKILL on every wait; `/sbin/umount` |
| Volumes | 256 MiB images made in the guest with `mkntfs -F -Q`; the nodes are the ones `hdiutil attach -nomount -imagekey diskimage-class=CRawDiskImage` returns for such an image (checked by size, whole disk, "Virtual"); owned by the guest user, so no sudo |
| Node mount | the case opens the node and hands it to the worker as `/dev/fd/N` (patch 0003), as the daemon does |
| FUSE side | the worker's own `-o debug` log, each line stamped on arrival (`CLOCK_REALTIME`); node ids from the request line |
| Device side | `syncspy.c`: a `DYLD_INSERT_LIBRARIES` library, loaded only into the test worker, that interposes `fcntl`, `fsync`, `open`, `pread`, `pwrite`, `write` and logs every `F_FULLFSYNC` and `fsync` with result, duration and the number of device writes since the last sync, and every failed device read or write. It is built for arm64 and arm64e, because the macFUSE mount helper that libfuse starts inherits the variable and is arm64e. No patch, no change to the worker, no new patch number |
| Corroboration | `dtrace_check.sh`: `dtrace` on the worker pid (works in the golden guest, where SIP is off; needs `sudo -n`), no library loaded. Output in `tests/results/evidence/m310/dtrace/` agrees with `syncspy` line for line |
| Fault injection | `NTFSMAC_SYNCSPY_FAIL=fullfsync` makes the interposed `F_FULLFSYNC` return -1 with `ENOTTY` without calling the kernel, to show the fallback on an image file |
| Filtering | macOS fsyncs `.fseventsd` four times at mount and the FUSE `pid` is 0. Each sync point has its own time window ([call start - 20 ms, call end + 100 ms], calls 1.2 s apart, 3 s after mount before the first); counts are per window. The mount-time requests are outside every window and are listed separately below |

## What upstream does (pristine 2026.9.28; patch 0003 shifts `unix_io.c` by about 30 lines)

| Place | Behaviour |
|---|---|
| `libntfs-3g/unix_io.c:82-105` `ntfs_fsync()` | on `__APPLE__`: `fcntl(fd, F_FULLFSYNC, NULL)` (line 94); if it returns non-zero for any reason, `fsync(fd)` (line 100) and return that result. No message, no errno kept |
| `unix_io.c:334-346` `ntfs_device_unix_io_sync()` | returns 0 at once for a read-only device (line 338); else `ntfs_fsync()`; clears the dirty flag only on success |
| `unix_io.c:212-216` `ntfs_device_unix_io_close()` | `ntfs_fsync()` only if the device is dirty |
| `libntfs-3g/device.c:173-184` `ntfs_device_sync()` | **skips the device sync when the device is not dirty.** Dirty is set by any `ntfs_pwrite()` (`device.c:276`) or device `write`/`pwrite` (`unix_io.c:283, 322`) and cleared by a successful sync |
| `src/ntfs-3g.c:2905-2915` `ntfs_fuse_fsync()` | `FUSE_FSYNC` and `FUSE_FSYNCDIR` both call `ntfs_device_sync()` (line 2912), so subject to the dirty gate; the file, the flags and `datasync` are ignored |
| `libntfs-3g/volume.c:207` `__ntfs_volume_release()` | calls `dev->d_ops->sync()` **directly**, not through the dirty gate: one device sync at every read/write unmount, then `close` (line 209), whose own sync is skipped because the first cleared the flag |
| `device.c:292` `ntfs_pwrite()` | `if (NDevSync(dev) && total && dops->sync(dev))`: with the `sync` mount option (`ntfs-3g.c:4044`) **every `ntfs_pwrite()` is followed by a device sync** |
| `unix_io.c:131-132, 141-142` | `S_ISBLK` sets `NDevBlock`; `S_ISCHR` (the raw node) does not, and then `O_EXCL` is added at open. Nothing else in the file mentions block size or alignment. `ntfs_pread()` and `ntfs_pwrite()` (`device.c:205-296`) pass the caller's offset and length straight to `pread` and `pwrite` |
| `libntfs-3g/volume.c:588`, `include/ntfs-3g/device_io.h:59-66` | on Darwin `BLKSSZGET`, `BLKGETSIZE64` and `BLKBSZSET` are defined as made-up numbers (`0x1268` and so on) and fail with `ENOTTY`; the failure of the block size call is only logged at debug level. So the sector size is never read from the device and nothing aligns anything |

## I-SYNC-01: the trace

Each cell is from the result of this run (`trace-*.txt`, `trace.json`). "Device F_FULLFSYNC" is the number of
`fcntl(F_FULLFSYNC)` calls the worker made on the device in the window, "rc" its result, "fallback" whether a plain `fsync` followed.
`FSYNC` counts are the `FUSE_FSYNC` requests of the worker's log. Durations of the device `F_FULLFSYNC` on the image
file were 4 to 18 ms in the three runs made.

### Image file (device = the `.img` file on APFS)

| Sync point | `FUSE_FSYNC` | Device `F_FULLFSYNC` | rc | Fallback ran |
|---|---|---|---|---|
| `fsync(fd)` after a write | 1 | 1 | 0 | no |
| `fsync(fd)`, nothing written since | 1 | **0** (device clean) | | |
| `fcntl(F_FULLFSYNC)` after a write | 1 | 1 | 0 | no |
| `fcntl(F_FULLFSYNC)`, nothing written since | 1 | **0** | | |
| `sync(2)`, the dirty file still open | 1 | 1 | 0 | no |
| `sync(2)`, the file open, nothing new | 1 | **0** | | |
| `sync(2)`, data written, file closed, nothing open | **0** | **0** (the device stays dirty) | | |
| unmount (`FUSE_DESTROY`) | 0 (only `DESTROY`) | 1 (`volume.c:207`, unconditional for rw) | 0 | no |
| unmount of a read-only mount | 0 | 0 | | |
| at mount (macOS's `.fseventsd` file) | 4 | 1 (the first request syncs, the others find the device clean) | 0 | no |

### Buffered node (`/dev/diskN`) and, identical in every row, the raw node's calls where it mounts

| Sync point | `FUSE_FSYNC` | Device `F_FULLFSYNC` | rc, errno | Fallback `fsync` |
|---|---|---|---|---|
| `fsync(fd)` after a write | 1 | 1 | -1, 25 (`ENOTTY`) | **ran**, rc 0 |
| `fsync(fd)`, nothing written since | 1 | 0 | | |
| `fcntl(F_FULLFSYNC)` after a write | 1 | 1 | -1, 25 | **ran**, rc 0 |
| `fcntl(F_FULLFSYNC)`, nothing written since | 1 | 0 | | |
| `sync(2)`, the dirty file still open | 1 | 1 | -1, 25 | **ran**, rc 0 |
| `sync(2)`, the file open, nothing new | 1 | 0 | | |
| `sync(2)`, data written, file closed | 0 | 0 | | |
| unmount (`FUSE_DESTROY`) | 0 | 1 | -1, 25 | **ran**, rc 0 |
| at mount | 5 | 1 | -1, 25 | **ran**, rc 0 |

The application sees no error at any point: `fsync` and `F_FULLFSYNC` return 0. The `F_FULLFSYNC` call itself costs 3 microseconds (the kernel
refuses it at once); the fallback `fsync` took 0.4 to 25 ms.

### Raw node (`/dev/rdiskN`)

The worker does not mount (exit 12, "Failed to read NTFS $Bitmap"); the first failed device call is `pread(off=8192, len=8)` with
`EINVAL` (`I-SYNC-01.raw.mountfail.log`). There are no sync points to trace. `F_FULLFSYNC` on the raw node, tested on a
scratch image without NTFS (`I-SYNC-02.alignment.txt`), fails with `ENOTTY` like the buffered one.

### Injected `F_FULLFSYNC` failure on the image file

With the interposed `F_FULLFSYNC` failing, every device sync (mount, the three dirty sync points, unmount) is an `F_FULLFSYNC` that fails
with `ENOTTY` followed by exactly one `fsync` that returns 0. The application calls return 0, the volume is intact after remount by path
(`cmp` of the data and `ntfsfix -n` rc 0). This is the same picture as on a node, and it confirms that the fallback is silent
(ADR-014 facts). It is what patch 0006's `W fullfsync` record is for: **on a device node it would be sent at every sync point of every
session.**

### Other checks in the cases

- `sync(2)` and unmount: as in `M2-sync.md`, and unchanged on the nodes: `sync(2)` gives `FUSE_FSYNC` only for files that are open; with no
  file open it gives nothing, so data written and closed stays unsynced in the device layer until unmount.
- A volume that was written, synced and unmounted by each kind passes `ntfsfix -n` and every file compares equal when remounted read-only by path.
- `dtrace` (`tests/results/evidence/m310/dtrace/timeline.txt`): on the image file `fcntl(fd 3, F_FULLFSYNC) = 0` at each step and at unmount, no `fsync`;
  on the buffered node `fcntl(...) = -1 errno 25` immediately followed by `fsync(fd 3) = 0`, at every step and at unmount. Same as `syncspy`.
- VirtIO block device (`tart.sh run --disk`, `sudo -n` for the node, scratch image only): `F_FULLFSYNC` on `/dev/disk0` and `/dev/rdisk0`, also after a
  write, is `ENOTTY`; `fsync` returns 0; `ioctl(DKIOCSYNCHRONIZE)` returns 0 on both (`tests/results/evidence/m310/virtio/`). A manual check, not part of the cases:
  the harness VM has no extra disk.

## I-SYNC-02: raw versus buffered node (U10)

### The kernel at the node (scratch image, no NTFS; `I-SYNC-02.alignment.txt`)

| Call | `/dev/diskN` (buffered) | `/dev/rdiskN` (raw) |
|---|---|---|
| `pread`/`pwrite`, offset and length both multiples of 512 | ok | ok |
| any offset or length that is not a multiple of 512 (all 24 such combinations of the 30 tried: 1 to 4140 bytes at offsets 0, 1, 512, 513, 4096) | ok | **`EINVAL`** |
| `fcntl(F_FULLFSYNC)` | `ENOTTY` | `ENOTTY` |
| `fsync` | ok | ok |
| `ioctl(DKIOCSYNCHRONIZE)` | ok | ok |

The device block size reported is 512 bytes. The raw node accepts exactly the aligned requests.

### NTFS-3G through each node (`correctness()`, files of 1 byte to 5 MiB, aligned and not, an append, a truncate)

| Device | Mount | Write/verify | After remount by path |
|---|---|---|---|
| image file | ok | 12 of 12 files correct, no error | `ntfsfix -n` rc 0 |
| buffered node | ok | 12 of 12 files correct, no error, no failed device call | `ntfsfix -n` rc 0 |
| raw node | **fails**: `pread(off=8192, len=8)` -> `EINVAL`, exit 12 | not reached | not reached |

The raw node was formatted through the image file (`mkntfs` on the node fails too: in a development run a `write` of 4140 bytes at an aligned offset gave `EINVAL`).
Even a read-only mount would fail: the failing read is in the mount code (the bitmap of `$MFT`). NTFS-3G writes 1-byte and 1024-byte
pieces (`$Bitmap`, `$MFT` records) at arbitrary byte offsets all the time (seen with `syncspy` and `NTFSMAC_SYNCSPY_WRITES` in a development run: `off=8200 len=1`), so the
failing case is not a corner. No case of silent corruption was found: the case asserts that any file a write call reported as written compares equal,
and passes.

### Node throughput (bare node, aligned I/O only, scratch image; `I-SYNC-02.node-io.txt`)

| Node | 32 x 1 MiB write | + `DKIOCSYNCHRONIZE` | 2048 x 4 KiB write | read 32 MiB |
|---|---|---|---|---|
| buffered `/dev/diskN` | 192 MB/s | 183 MB/s (flush 9 ms) | 114 MB/s | 23900 MB/s (buffer cache) |
| raw `/dev/rdiskN` | 2574 MB/s | 1551 MB/s (flush 8 ms) | 19 MB/s | 2245 MB/s |

These are numbers of the `hdiutil` virtual disk on the host's APFS file, not of a drive. The only usable conclusion is the shape: raw is fast for large
aligned requests and 6 times slower than buffered for 4 KiB ones (no coalescing), buffered reads from the cache. With the raw node unusable for NTFS-3G,
there is no NTFS-level comparison.

## Benchmark: default, `sync`, node (small fixed workloads, one run of the case)

Workloads (sizes multiples of 4096): write 32 MiB in 1 MiB writes then `fsync` and close, 3 times; create 100 files of 4 KiB (no `fsync`), 3 times;
30 times 4 KiB append + `fsync` (one run, median and maximum of the per-`fsync` time); unmount time. Cells: median (the three runs). `debug` is off.

| Variant | Mount | Sequential write 32 MiB (MB/s) | 100 x 4 KiB files (files/s) | `fsync` 4 KiB (ms, median/max) | Unmount (ms) |
|---|---|---|---|---|---|
| **default**, image file | ok | 658 (691, 361, 658) | 624 (624, 635, 394) | 4.1 / 20.3 | 140 |
| **`sync` option**, image file | ok | **37** (37, 37, 40) | **17** (17, 17, 18) | 0.7 / 41.6 | 163 |
| default, buffered node | ok | 132 (111, 146, 132) | 622 (625, 622, 400) | 0.8 / 3.4 | 143 |
| `sync` option, buffered node | ok | 148 (113, 187, 148) | 323 (366, 312, 323) | 0.5 / 0.7 | 136 |
| **raw node** | **no mount** (`EINVAL`) | | | | |

Device syncs the worker makes for 4 x 1 MiB writes, then `fsync` and close, on the image file (`syncspy`, `I-SYNC-02.bench.txt`):

| Variant | During the writes | At `fsync` and close | `FUSE_FSYNC` | Mean device `F_FULLFSYNC` |
|---|---|---|---|---|
| default | 0 | 1 | 1 | 6.2 ms |
| `sync` | **30** | 0 (nothing dirty) | 1 | 3.3 ms |

Reading the table:

- `sync` on the image file costs 94 percent of sequential write throughput in the recorded run (88 to 94 in three runs) and 97 percent of small-file creation (658 to 37 MB/s, 624 to 17 files/s). The mechanism is
  `device.c:292`: one `F_FULLFSYNC` of about 3 ms after every `ntfs_pwrite()`, about 7 per MiB. A real drive that takes 10 to 100 ms for a cache flush would be slower still.
- On the buffered node `sync` shows little cost (132 to 148 MB/s, within the spread; small files 622 to 323 files/s) because each of its syncs is
  `F_FULLFSYNC` failing in 3 microseconds followed by an `fsync` that returns in under a millisecond. There the option adds no protection that
  `fsync` does not already give, and no measurement here says what `fsync` on the node does to a real drive's cache.
- The node is slower than the image file for big sequential writes (132 against 658 MB/s). This is the virtual disk driver below, not NTFS-3G; do not read it as a fact about
  USB drives. Runs of the same variant differ by up to 2 times (the 361 and 394 values), so differences under that are noise.
- Variation between runs of the case: default against `sync` in sequential write was 658/37 MB/s in the recorded run and 583/40 and 354/42 MB/s in two earlier runs, so the cost is 88 to 94 percent.

## What this means

**D1 (clean unmount flushes the drive cache).** The mechanism text in ARCHITECTURE.md section 9 says upstream `ntfs_fsync()` issues `F_FULLFSYNC` in the worker. On a device node that is
false: the worker issues `F_FULLFSYNC`, the kernel refuses it, and `fsync` runs. So the worker's own sync never gives the "drive cache flushed" evidence, with or without patch 0006.
What can still be claimed: all data the file system received was written to the device node (`ntfs_umount` -> `volume.c:207`, then `close`), and the buffered node's `fsync` was called.
A flushed claim needs a call that works on a node. `ioctl(fd, DKIOCSYNCHRONIZE, &{0,0,0})` returned 0 on every node tried. D1 stays **Conditional**; the condition changes from "no `W` record" to
"the daemon's post-exit `DKIOCSYNCHRONIZE` succeeded", and Environment C must still show that a real drive honours it.

**N1 (`fsync` returning 0 means stable storage).** Keep as a non-guarantee, with a stronger reason: on a node every worker `fsync` is the fallback `fsync` of a buffered device, 0 means "handed to the
driver", and nothing says more. A repeated `fsync` with no write in between does not touch the device at all (`device.c:173`), which is correct for a clean device. A failed sync leaves the
dirty flag set, so the next one retries.

**N5 (`F_FULLFSYNC` semantics).** Keep "not supported for workloads that need fsync durability". `F_FULLFSYNC` from the application is the same `FUSE_FSYNC` as `fsync`, the worker syncs the whole
device, and on a node it is `fsync` of the buffered node. The word "until T-M3-10" can go: this task found that the position is final unless the worker is changed to use `DKIOCSYNCHRONIZE`.

**R-30.** The risk is not "some USB bridges lack `F_FULLFSYNC`": on macOS 27 **no block device node accepted it** (disk image, VirtIO). The consequence for patch 0006 and ADR-014 item 2: `W fullfsync` would
fire at every sync point, so a daemon that treats it as "the drive cache was not flushed" reports every session as not flushed. Either the worker's sync is changed for device nodes
(a change to `ntfs_fsync()` in `unix_io.c`, which is already carried by 0003 and 0006: on `__APPLE__`, when `fstat` says block or character device, call `DKIOCSYNCHRONIZE` and fall back
to `fsync`), or the daemon does the flush after the worker exits and `W` is dropped or only counted. ARCHITECTURE.md lines 319 and 384 (daemon step 5, volmark step 4) have the same
problem: `fcntl(F_FULLFSYNC)` on the device descriptor fails with `ENOTTY` there too, so they should use `DKIOCSYNCHRONIZE`, with `F_FULLFSYNC` only for image files.
The cost of `sync` is settled: not enabled (item 5 of ADR-014; 88 to 94 percent against a 30 percent limit).

**U10 (raw versus buffered).** Resolved: use the **buffered** node (`/dev/diskN`). `unix_io.c` has no alignment handling on Darwin; the raw node's 512-byte alignment rule breaks NTFS-3G at mount;
fixing it would be a bounce-buffer layer in `unix_io.c` (a patch outside ADR-002's scope: it touches the device layer for a speed gain that is not needed). The buffered node worked for all workloads here.
`DKIOCSYNCHRONIZE` works on both. Performance of the two with NTFS-3G cannot be compared because the raw one does not mount.

## Proposed edits to shared files (not applied)

- `docs/TECHNICAL_DECISIONS.md` ADR-014: facts section, add "on a device node `F_FULLFSYNC` fails with `ENOTTY` (measured, T-M3-10), so the fallback is the normal path"; item 2 and 3 change from
  `fcntl(F_FULLFSYNC)` to `ioctl(DKIOCSYNCHRONIZE)` for device descriptors; item 5: `sync` not enabled, 88 to 94 percent measured.
- `docs/ARCHITECTURE.md` D1 mechanism and condition as in "What this means"; N1, N5 reasons; U10 row "Resolved 2026-11-14: buffered node; raw node refuses unaligned I/O and NTFS-3G fails at mount
  (`docs/spikes/M3-sync.md`)"; lines 319 and 384 use `DKIOCSYNCHRONIZE`.
- `docs/RISK_REGISTER.md` R-30: restate as "no device node accepts `F_FULLFSYNC` on macOS 27; flush by `DKIOCSYNCHRONIZE`; hardware effect [U] until Environment C".
- Patch 0006 (T-M3-18): the `W fullfsync` record needs a decision (above) before the patch is written; `I-ERR-*` should not assume that a clean node session has no `W`.
- `docs/spikes/M2-options.md` and the `sync` entry: add the measured cost.

## Limits

One guest, one macOS and macFUSE build, virtual disks only (`hdiutil` image, VirtIO). No spinning disk, USB bridge or flash drive: the drive-cache part of D1 is untested, and `DKIOCSYNCHRONIZE` returning 0 on a virtual disk
proves nothing about a drive (Environment C, T-M5-10). The benchmark is one run of the case with 3 repetitions per workload and a spread of up to 2 times; the buffered-node rows measure the virtual disk driver as much as NTFS-3G.
`syncspy` and `dtrace` see the calls the worker makes, not what the kernel does below `fsync` on a device node. The `fsync` fallback on a node was not shown to flush or not flush the device cache. `mmap`
writes and the directory `fsync` were not repeated (M2-sync). SIP is off in the golden VM, so `dtrace_check.sh` does not show that `dtrace` works with SIP on.
