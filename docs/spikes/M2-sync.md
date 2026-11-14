# M2 sync trace: which FUSE opcodes reach the worker (T-M2-06, U4)

Date: 2026-11-14. Answers U4. Raw logs: `tests/results/evidence/m206/`. Driver: `tests/integration/smoke/sync_trace.sh`
(host side clones a disposable guest from `golden`, builds NTFS-3G, runs the guest half, copies the logs, deletes the
clone), probe: `tests/integration/smoke/sync_probe.c`.

## Answer

**U4, in one paragraph.** On the macFUSE 5.5.0 FSKit backend the worker sees `write(2)` data **before `write()` returns**
(FSKit holds no write-back buffer for it), `close` as `FUSE_RELEASE` only (no `FUSE_FLUSH` for the probe's files),
`fsync(fd)` as a per-file `FUSE_FSYNC` (so "FSKit has no per-file fsync" is wrong), `fcntl(F_FULLFSYNC)` as the **same**
`FUSE_FSYNC` request (not distinguishable), `sync(2)` as `FUSE_FSYNC` for each file that is open at that moment and as
nothing at all when no file is open, `fsync` on a directory as **no request**, and unmount as `STATFS`, `FORGET`s and
`DESTROY` (no `FSYNC`, `FSYNCDIR` or `FLUSH`). An `ENOSPC` from the worker is seen by the application at `write()`, not at
`fsync` or `close`. The consequence for D1: the premise "FSKit `synchronize` + `unmount` deliver every dirty buffer" is
not needed, because there are no dirty FSKit buffers for `write(2)` data; what can still be unflushed sits below the
worker, in the device writes `libntfs-3g` made, and the unmount path that flushes it is `DESTROY` -> `ntfs_umount()`.

## Method

| Item | Value |
|---|---|
| Guest | clone of `ntfsmac-golden`, macOS 27.2 (26B123), kernel 27.2.0 VMAPPLE, macFUSE 5.5.0 (`libfuse.2.dylib`, FUSE API 29) |
| Worker | NTFS-3G 2026.9.28 built by `scripts/build-ntfs3g.sh --fuse external` (patch series as of this date, incl. 0003) |
| Mount | `ntfs-3g -o backend=fskit,local,norecover,uid=501,gid=20,debug,no_detach IMAGE MOUNTPOINT` as user `admin`, no sudo anywhere |
| Volumes | images made in the guest with `mkntfs -F -Q` (256 MiB; 24 MiB for ENOSPC); never a host disk |
| Log | libfuse2 `-o debug` (NTFS-3G passes `-odebug` when given `debug`): one `unique: N, opcode: NAME` line per request plus a detail line and the reply. Each line is stamped with `CLOCK_REALTIME` when read; `sync_probe` prints `CLOCK_MONOTONIC` and `CLOCK_REALTIME` around every call, so the logs merge into `timeline-*.txt` and `opcodes-*.txt` |

Findings about the tooling itself:

- `-o debug` works as the per-request log on macFUSE (the log starts with `INIT: 7.19`); `no_detach` keeps the worker in the
  foreground. The FUSE `pid` field is always 0 on this backend, so requests cannot be tied to the calling process.
- **The mount point must be an absolute path** (logged by the M2 audit, `tests/results/evidence/2026-11-14-00abf04-claims.log` cases C2 and C3). With a relative one `ntfs-3g` prints "Mounted ...", stays alive and no
  mount ever appears (the same silent failure as without `backend=fskit`; a note for ARCHITECTURE.md 3.3 / 4.1).
- On every mount macOS itself opens files on the volume (the `.fseventsd/fseventsd-uuid` file is created, written (36
  bytes), fsynced four times (five times in the steps session) and closed; `.VolumeIcon.icns`, `getxattr`s). Those
  are the `FSYNC x4`, `WRITE x1` and `FLUSH x1` entries in the `mount_begin` rows (`FSYNC x5` and `FLUSH x2` in `opcodes-steps.txt`); they are not the probe.

## Opcode table (application call -> FUSE opcodes)

From `opcodes-steps.txt` / `timeline-steps.txt` (session `steps`, one pass at 4 KiB, 1 MiB and 64 MiB; the same pattern
repeats at all three sizes). The probe waits 1.5 s between steps. "Latency" is the time from the call to the opcode, or the
worker's reply time where stated. `STATFS`, `GETATTR`, `GETXATTR` and `LOOKUP` requests that macOS adds around most calls are
left out.

| Application call | FUSE opcodes seen | Timing |
|---|---|---|
| `open(O_CREAT)` | `LOOKUP`, `CREATE`, `SETATTR`, `GETATTR`, `RELEASE` (of the create handle), `OPEN` (a second handle) | all within 3 ms |
| `write(fd, buf, N)` | 4 KiB: 1 `WRITE`. 1 MiB: 1 or 8 `WRITE` (8 in 2 of 5 delay runs). 64 MiB: 64 `WRITE` of 1 MiB (78 in one run) | **all `WRITE`s arrive before `write()` returns**; the last one 0.3 to 0.5 ms before it |
| `close(fd)` | `RELEASE` | 0.5 to 1.5 ms after `close()` starts, before it returns. **No `FLUSH`** for any file the probe opened |
| `fsync(fd)` after a write | `FSYNC` (`fsync[0] datasync: 1`), on the file's node id | request at +0 ms; reply (device sync in `ntfs_fuse_fsync`) 0 to 50 ms, mean 10.5 ms over the 19 `FSYNC` of the probe in the steps log (8.7 ms if the 5 mount-time `FSYNC`s of macOS are included) |
| `fcntl(fd, F_FULLFSYNC)` after a write | `FSYNC`, byte-for-byte the same line (`datasync: 1`, `insize: 56`) | `fcntl` returned 0, errno 0 in every call |
| `sync(2)` with the written file still open | `FSYNC` for that file | at +0 ms |
| `sync(2)` with a clean file open | `FSYNC` for that file | at +0 ms |
| `sync(2)` after write and close, nothing open | **only `STATFS`**, no `FSYNC` | |
| `fsync` and `F_FULLFSYNC` on a clean file descriptor | `FSYNC` each (FSKit does not skip clean files) | at +0 to 1 ms |
| `fsync` and `F_FULLFSYNC` on a directory | **no request** (only `STATFS`); both return 0 | |
| `rename` | `LOOKUP` x3, `GETATTR`, `RENAME` | |
| `unlink` | `LOOKUP`, `GETATTR`, `UNLINK`, later `FORGET` | |
| unmount (`/sbin/umount`) | `STATFS` x7-8, `FORGET` per node, `DESTROY`. **No `FSYNC`, `FSYNCDIR` or `FLUSH`** | `umount` took 44 ms (rc 0), `DESTROY` reply 8 ms (empty volume) to 56 ms (after the delay runs); the worker exited within about 30 ms of `umount_end` |

Notes:

- `FSYNC` repeats: the request line is always `datasync: 1`, whatever the application asked for (NTFS-3G's handler ignores the flag
  and syncs the whole device: `ntfs_fuse_fsync()` calls `ntfs_device_sync()`). A second, unprompted `FSYNC` for the same
  node arrived 0.5 s after `fsync()` returned once in the final run (64 MiB, `fsync_end` row of `opcodes-steps.txt`) and once in
  an earlier development run (4 KiB). It was not caused by the probe; the trigger is unknown (it may be macOS itself).
- `FLUSH` appears only for files that macOS opened at mount time (the fseventsd file, the volume icon), 2 to 3 times per
  session, and always fails with `-78 (Function not implemented)` because `ntfs_3g_ops` has no `.flush`. Why FSKit sent it
  for those and not for the probe's files is not known. An application that relies on `close()` returning a flush error
  gets nothing from this worker either way.
- `FSYNCDIR` never arrived: NTFS-3G maps `.fsyncdir` to the same function, but FSKit did not call it for a directory `fsync`.

## F_FULLFSYNC

From the application, `fcntl(F_FULLFSYNC)` on this volume succeeds (rc 0, errno 0 in all 9 calls of the steps session, also
on a clean descriptor and on a directory) and produces the same `FUSE_FSYNC` request as `fsync` (identical request line,
identical size, same `datasync: 1`; the reply latency of both is the same range, 7 to 50 ms). **At the FUSE level it cannot be
told from `fsync`.** Whether the worker then issues `F_FULLFSYNC` on the device is `ntfs_fsync()` in `unix_io.c`, which
T-M3-10 traces; U4 says only that the worker gets one `FSYNC` per application sync call, so the drive-cache flush is the
worker's own doing and not something the application's `F_FULLFSYNC` asks for.

## Write delay (U4's second question)

`delay.tsv`, session `delay`: write N bytes in one `write()`, keep the descriptor open for 6 s, close. 5 runs per size.
`first_write_after_write_ms` is the time of the first `FUSE_WRITE` request minus the time `write()` returned; negative
means the request came before `write()` returned.

| Size | Runs | `write()` call duration (ms) | First `FUSE_WRITE` relative to `write()` return (ms) | Last `FUSE_WRITE` (ms) | `WRITE` count |
|---|---|---|---|---|---|
| 4 KiB | 5 | 0.8 to 41.9 (median 1.7) | -41.5 to -0.5 | same as first | 1 |
| 1 MiB | 5 | 1.4 to 6.2 | -5.7 to -0.7 | -0.8 to -0.3 | 1 or 8 |
| 64 MiB | 5 | 32.3 to 77.0 | -76.4 to -32.0 | -0.5 to -0.3 | 64 (78 once) |

**There is no delay: FUSE_WRITE arrives during `write()`, and `write()` returns after the worker has answered the last one.**
In all 15 runs the first `FUSE_WRITE` came before `close()` (and before `write()` returned); `RELEASE` followed `close` by 0.6 to
1.5 ms. A 64 MiB `dd` followed by an immediate unmount (session `dirty`) delivered all 72 `WRITE`s before `dd` exited.
So the 0.1 s for 128 MiB in T-M2-03 is not an FSKit buffer cache: it is the speed of `pwrite` into the image file's page cache
below `libntfs-3g`. The data is "written" for the NTFS-3G level and not yet on the disk.

## Unmount with data just written (D1 evidence)

Session `dirty`: `dd` 64 MiB random data into the volume, then `/sbin/umount` at once (variant `plain`) or after `sync` and 3 s
(variant `synced`), then a read-only remount and `cmp` with the source.

| Variant | Opcodes after `umount` starts | `DESTROY` reply | After remount |
|---|---|---|---|
| plain | `STATFS` x8, `FORGET` x4, `DESTROY` | 37 ms | size 67108864, `CMP: identical`, `ntfsfix -n` OK |
| synced | `STATFS` x8, `FORGET` x4, `DESTROY` | 28 ms | size 67108864, `CMP: identical`, `ntfsfix -n` OK |

No `FSYNC` arrives at unmount in either variant, and `sync(2)` before it also produced no request (nothing was open). The
unmount path is `DESTROY` -> `ntfs_fuse_destroy2()` -> `ntfs_umount()`, which is where `ntfs_fsync()` runs (ARCHITECTURE.md 4.5). This
confirms that nothing is waiting in FSKit at unmount. It does not show that the data reached the physical disk: that is the
page cache below the worker (T-M3-10, and real caches in Environment C).

## Error visibility (ENOSPC)

Session `enospc`: 24 MiB volume, one file written in 1 MiB chunks without `fsync` until the volume is full
(`probe-enospc.log`, `timeline-enospc.txt`).

| Call | Result |
|---|---|
| `write()` #22 (after 21 MiB accepted) | **fails at once: -1, errno 28 (ENOSPC)**. Worker log: `Failed to allocate clusters: No space left on device`, `unique: 254, error: -28` |
| a retry `write()` of 1 byte | also -1, errno 28. FSKit first sent the 1 byte (at offset 22 MiB: the file offset had advanced by the failed 1 MiB) and then re-sent the failed 1 MiB `WRITE` at 21 MiB, which failed again |
| `fsync()` afterwards | **returns 0** (the worker's `FSYNC` succeeded) |
| `fcntl(F_FULLFSYNC)` afterwards | returns 0 |
| `close()` afterwards | returns 0 |
| size after close | 23068673 bytes (22 MiB + 1): a hole where the failed chunk was, a stray byte after it |
| a new 64 KiB file on the full volume | create, `write`, `fsync` and `close` all succeed (a small file fits in the free MFT record and clusters left) |
| after `unlink` of the big file | writes work again |

So the application sees the error **at `write()` only**. `fsync()` and `close()` report nothing about it (and the earlier
failure is not "sticky"), and no later call returns it. Data from a failed `write()` is lost unless the application checks its return
value. Because the error comes back synchronously, N2 (a `close` that cannot return errors) is not a loss path for `write(2)` data on
this backend: there are no deferred write errors to report at `close`. Not tested: `mmap` writes, `pwrite` into holes with
`O_DIRECT`, and a worker that dies (SIGKILL) mid-write.

## Limits of this result

One guest, one macFUSE build, image files only, one writer process. Memory pressure and long idle times were not tried:
FSKit might keep a cache for large files or under other conditions, but nothing like it showed up in 15 delay runs, 3 step
sizes and 2 unmount runs. Read caching was not examined (reads were not traced for caching). `mmap` is T-M3-11.

## Proposed wording changes (not applied)

### ARCHITECTURE.md section 4.3, first paragraph

Replace

> `write(2)` -> VFS -> FSKit (data may sit in the buffer cache; `write` returning does not mean the worker has seen it [A: T-M2-06 measures]) -> FSModule -> `FUSE_WRITE` -> ...

with

> `write(2)` -> VFS -> FSKit -> FSModule -> `FUSE_WRITE` (one request of up to 1 MiB; a 64 MiB write is 64 requests) -> `ntfs_fuse_write` -> ... FSKit holds no write-back buffer for `write(2)` data on this backend: every `FUSE_WRITE` has been answered before `write()` returns, and an error from the worker (for example `ENOSPC`) is returned by that `write()` [V: T-M2-06, `docs/spikes/M2-sync.md`]. Data written by `write(2)` that has not reached the disk is in the device writes below `libntfs-3g` (the page cache of the device or image), not in FSKit.

### ARCHITECTURE.md section 4.4, the table

Replace the whole table with:

| Application call | What reaches the worker | Evidence |
|---|---|---|
| `write(fd)` | `FUSE_WRITE` (up to 1 MiB each), all answered before `write` returns. A worker error is returned by the `write`. | [V: T-M2-06] |
| `fsync(fd)` | `FUSE_FSYNC` for the file's node, always with `datasync: 1` whatever the application asked for. Also sent for a descriptor with nothing dirty. `ntfs_fuse_fsync()` ignores the argument and syncs the whole device. | [V: T-M2-06] |
| `fcntl(F_FULLFSYNC)` | The same `FUSE_FSYNC` as `fsync`, not distinguishable at the FUSE level. `fcntl` returns 0. | [V: T-M2-06] |
| `fsync` or `F_FULLFSYNC` on a directory | No request. Returns 0. | [V: T-M2-06] |
| `close(fd)` | `FUSE_RELEASE`, before `close` returns. No `FUSE_FLUSH` for the files tested (macOS's own mount-time opens do get one, answered `ENOSYS` because `ntfs_3g_ops` has no `.flush`). A flush error cannot occur, since there is no flush. | [V: T-M2-06] |
| `sync(2)` | `FUSE_FSYNC` for each file that is open at that moment (dirty or clean); when no file is open, no request at all (one `sync(2)` call per size in the steps session). | [V: T-M2-06] |
| unmount | `FUSE_DESTROY` (after `STATFS` and `FORGET`s); no `FUSE_FSYNC`, `FUSE_FSYNCDIR` or `FUSE_FLUSH`. | [V: T-M2-06] |

and replace the sentence after the table's first paragraph "Worker side [V:S25]: ..." by prefixing:

> Because `sync(2)` and unmount produce no request when no file is open, the only sync points the worker is certain to get are an application's own `fsync`/`F_FULLFSYNC` and the `DESTROY` at unmount. Unmount is therefore the point at which everything the worker wrote is flushed (`ntfs_umount()` -> `ntfs_fsync()`).

### ARCHITECTURE.md section 4.5 step 2

Replace "FSKit calls `synchronize`, then `unmount`, on the FSModule; the worker receives `FUSE_DESTROY`" with "the worker receives `FUSE_DESTROY` (preceded by `STATFS` and `FORGET` requests only; no `FSYNC`)".

### ARCHITECTURE.md section 9

D1, mechanism column. Replace "FSKit `synchronize` + `unmount` deliver every dirty buffer to the worker [A: U4];" with
"FSKit holds no dirty write buffers: every `FUSE_WRITE` is answered before `write()` returns, so nothing is lost between the
application and the worker at unmount [V: T-M2-06];". Status column: replace "**Conditional** on T-M2-06 (opcodes at unmount), T-M3-10" with "**Conditional** on T-M3-10". "Until then" column: replace with "Unmount writes all data the file system received and asks the drive to flush; whether the drive confirmed it is shown by the session report."

N1: replace the reason with "`fsync` reaches the worker as `FUSE_FSYNC`, which `ntfs_fuse_fsync()` turns into a whole-device sync; whether the drive cache is flushed depends on `ntfs_fsync()` and the `W fullfsync` record (T-M3-10)." Keep N1 as a non-guarantee until T-M3-10.

N2: replace with "| N2 | `close(fd)` returning 0 means the data is on stable storage. | `close` gives `FUSE_RELEASE` only; there is no flush step and so no flush error. A write error is returned by `write()` itself; `fsync` and `close` do not report an earlier failed `write` (T-M2-06). |"

N3: remove from the list of non-guarantees as stated ("`write(2)` returning means the worker has received the data"); replace by "| N3 | `write(2)` returning means the data is on the disk. | The worker has received and answered the write, but `libntfs-3g` writes into the device's page cache; only a sync point flushes it. |"

N5: replace "[U] until T-M2-06. Until then documented as ..." with "`F_FULLFSYNC` arrives as the same `FUSE_FSYNC` as `fsync` (T-M2-06); the drive-cache flush is done by the worker's `ntfs_fsync()` and is not a guarantee until T-M3-10. Documented as 'not supported for workloads that need fsync durability' until then."

U4 row of section 11: "Resolved 2026-11-14: `fsync` and `F_FULLFSYNC` give `FUSE_FSYNC` (same request); `sync(2)` gives `FUSE_FSYNC` only for open files; directory `fsync` gives nothing; close gives `FUSE_RELEASE`; unmount gives `FUSE_DESTROY` only (`docs/spikes/M2-sync.md`)."

Section 3.3 / 4.1 (mount): "The mount point passed to the worker must be an absolute path; with a relative one the worker reports 'Mounted' and the mount never appears."

## Other things T-M3-10 and T-M2-04 should know

- `FUSE_FSYNC` latency is 0 to 50 ms for a small file, 28 to 56 ms for `DESTROY` after 64 MiB: the device sync is not instant even on the APFS image.
- macOS's own mount-time traffic (four `FSYNC`s on the fseventsd file, `FLUSH` of it) means that any "worker saw FSYNC" assertion must filter by node id or by time window.
- The `pid` field of every FUSE request is 0; the worker cannot attribute a request to an application.
