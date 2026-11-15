# M3 mapped I/O: which `mmap` writes reach the worker and when (T-M3-11, N7, D1, R-05)

Date: 2026-11-15. Answers the T-M3-11 row of `docs/AGENT_TASKS.md` and the `I-MMAP` row of `docs/TEST_STRATEGY.md`.
Cases: `tests/integration/mmap/i_mmap.py` (`I-MMAP-01` to `I-MMAP-07`), helpers `mmaplib.py` and `mmaptool.c`
(same directory). Result file: `tests/results/2026-11-15-0d859be-B.json` (all seven cases pass); evidence: `tests/results/evidence/2026-11-15-0d859be-B/`
(`<ID>.log`, `<ID>.observations.json`, `<ID>.worker-debug*.log`). Builds on `docs/spikes/M2-sync.md` and
`docs/spikes/M3-sync.md`.

## Answer

1. **A write through a mapping sends nothing to the worker.** The dirty page stays in the kernel's page cache.
   The `FUSE_WRITE` requests (1 MiB each; 4 KiB and 13 B tails for the odd sizes) are produced later, by one of
   the events below. Until then neither the worker nor the session channel knows the data exists.
2. **What pushes dirty mapped pages to the worker, and when** (1 MiB file, all measured on the debug log):

   | Event | `FUSE_WRITE` arrives |
   |---|---|
   | `msync(MS_SYNC)` | before `msync` returns (0 to 1 ms after the call starts), then no `FSYNC` (`I-MMAP-02`, `I-MMAP-05`; in `I-MMAP-06` the file of this row had been pushed by a syncer tick 2 s before the call, see the idle row) |
   | `msync(MS_ASYNC)` | about 1 ms after the call, after it returned |
   | `fsync(fd)`, `fcntl(F_FULLFSYNC)` | during the call, followed by `FUSE_FSYNC` |
   | `sync(2)` | during the call, with `FUSE_FSYNC` for every node |
   | `read(2)` of the same file | during the read (the read path flushes dirty pages of the file first) |
   | `close(fd)` | during the close, **also when the mapping is still live**; `FUSE_RELEASE` follows |
   | `munmap` with the descriptor still open | **nothing**; the pages stay dirty |
   | `munmap` then `close(fd)` | at `close`, as above |
   | nothing at all (idle, mapping live, descriptor open) | when the macOS 30 s syncer tick comes: a burst of `FUSE_FSYNC` for every node, with the `WRITE` just before it. Measured 1.6 s and 16 s after the write, depending on the tick phase; ticks were 30.0 s apart (`I-MMAP-03`, `I-MMAP-06`) |
   | `umount` while the mapping is live | `umount` is refused (below), but the attempt writes the dirty pages first |
   | `umount -f` | the dirty pages are written before `FUSE_DESTROY` |

   So every path in the table ends at the worker, and in every run the data was on the image after a clean
   unmount (`ntfsfix -n` exit 0, hashes equal). The two cases in which a dirty mapped page can be lost are the
   ones where the worker is gone before any of these events (answer 6).
3. **`munmap` plus `close` plus idle plus clean unmount keeps the data** (`I-MMAP-03`: 4 KiB, 1 MiB and 8 MiB files,
   mapped write, no `msync`, `munmap`, `close`, 40 s idle, `umount`; all three equal after remount).
4. **Unmount with a live mapping.** `umount` fails with `EBUSY` ("Resource busy -- try 'diskutil unmount'"), also when
   the descriptor is closed and only the mapping holds the file. `diskutil unmount force` on the mount point also fails
   (rc 1, "failed to unmount"). `umount -f` works, writes the dirty pages, sends `DESTROY`, and the worker exits 0.
   After the forced unmount every access through the mapping is `SIGBUS` (read and write); `msync` and `munmap`
   return 0; `close` returns 0. Data after remount equal in all four variants, including pages dirtied again after the
   refused `umount` (`I-MMAP-04`). For the product: the worker must expect a forced unmount while applications still
   map files, and that path is safe for data that was dirty before it.
5. **Extension through a mapping works.** `ftruncate` up and write through the map, then `msync`, `munmap` without
   `msync` followed by `close`, both equal after remount, with the sizes 4096, 16484 and 1 MiB+13 (`I-MMAP-05`). Past
   EOF: reading and writing inside the last page works but the bytes are not stored and the size does not change;
   a whole page past EOF is `SIGBUS` for read and write. After a shrink under a mapping the cut-off pages are `SIGBUS`; growing again
   gives zero bytes by `read(2)` and by the map.
6. **Worker `kill -9` with a live mapping** (`I-MMAP-07`, 8 pages of 16 KiB, three variants):
   - The mount stays. Every system call on it fails with `ENXIO` (`Device not configured`), `ls` too. `umount` fails with
     `ENXIO`, `diskutil unmount force` fails (rc 1), `umount -f` works.
   - **No error reaches the application through the mapping.** Pages already in memory can still be read **and written**
     with no signal. Pages not in memory give `SIGBUS` (read and write). `msync(MS_SYNC)` returns **0**, `munmap` returns 0.
     `fsync`, `pread`, `pwrite` and `close` return `ENXIO`. A program that uses only `mmap` and `msync` is told that
     the data is safe when it is not.
   - Data on the image afterwards: dirty pages that had not been pushed are lost (variant `dirty`: all pages old); a page
     `msync`ed before the kill is on the image and the page dirtied after it is not (variant `synced-then-dirty`: page 0 new,
     page 1 old). That is a half-updated file with no error anywhere. Nothing was pushed in the 0.3 s between write and kill; with
     a longer gap the syncer tick (answer 2) can push it first.
   - `ntfsfix -n` exits 0 on all three images and a read-only mount works. Writing by the mapper after the kill never reaches the disk.
7. **Read coherence.** A mapped read sees `write(2)` data (sizes 1 byte to 3 MiB+7) at once. A live read-only mapping sees a
   later `write(2)` from another descriptor, from its own descriptor and after `O_TRUNC` rewrite within 5 ms
   (`I-MMAP-01`). `read(2)` sees a mapped write on a live mapping at once (page cache is unified).

## Consequences

- **N7:** write-back ordering is the kernel's: a dirty mapped page reaches the worker only through the events in answer 2.
  The worker sees an ordinary `FUSE_WRITE` and answers it before the flush call returns, so a worker error becomes an error of
  `msync(MS_SYNC)`, `fsync`, `close` or `read` of the file; for `MS_ASYNC`, the syncer and `umount` the error has
  no caller. Error reporting for a dead worker is the weak point: answer 6. The product cannot add a signal; it can only
  report that the worker died (patch 0006 `U`/`E` records, daemon) and keep the marker so the next mount is read-only.
- **D1:** the claim "FSKit holds no dirty write buffers" holds for `write(2)` only. **For mapped writes the kernel holds dirty
  pages the worker has not seen**, up to about 30 s (the syncer tick) or until `msync`, `fsync`, `close(fd)`, `read(2)`,
  `sync(2)` or unmount. This does not weaken D1 for a clean unmount, because `umount` and `umount -f` both flush those pages
  to the worker before `DESTROY` in every run. It does make the premise of D1 wider: "every byte the applications wrote before
  the unmount started" includes bytes written through mappings, and they reach the worker only because of that flush. D1 must
  say so, and it must say what happens when the worker dies first (loss, and `msync` still returns 0).
- **Eject while an application maps a file:** a plain `umount` is refused. A product that ejects must either report "busy"
  or use the forced path (`umount -f`), after which the application gets `SIGBUS`. `diskutil unmount force` does not work on this
  mount, so a forced eject cannot go through `diskutil`.
- **R-05:** `msync(MS_SYNC)` returning 0 is not proof of durability when the worker has died. Add to the residual.

## Method

| Item | Value |
|---|---|
| Guest | disposable clone of `ntfsmac-golden` (`tests/harness/run-env-b.sh`), macOS 27.2, arm64, macFUSE 5.5.0, kernel page size 16 KiB |
| Worker | NTFS-3G 2026.9.28 built in the guest; `ntfs-3g -o backend=fskit,local,norecover,uid,gid,no_detach,debug IMAGE MOUNTPOINT`, absolute mount point, 30 s mount deadline, overall watchdog with SIGKILL, `/sbin/umount` with a 60 s deadline |
| Volumes | 256 MiB images made in the guest with `mkntfs -F -Q`; image files, not device nodes (the FUSE side of mapped I/O does not depend on the device; the device flush is `I-SYNC-01..02`). A fresh image for every variant that unmounts by force or kills the worker |
| Mapped access | `mmaptool.c`: a C helper process holding one descriptor and one `MAP_SHARED` mapping, commands on stdin; every mapped access is under a `SIGBUS` guard, so a fault is reported, not fatal. It stays alive across umount and worker kill |
| FUSE side | the worker's `-o debug` output, each line stamped on arrival (`CLOCK_REALTIME`); the `WRITE`/`READ` size and offset are read from the detail line. Each step records the requests in [call start - 20 ms, call end + settle], with `<` for during the call and `>` for after it |
| Device side | `syncspy.c` of I-SYNC-01 (a `DYLD_INSERT_LIBRARIES` library loaded only into the test worker, built the same way): device writes and syncs per step |
| Check | after each case: clean unmount, `ntfsfix -n` (exit 0), read-only mount, size and SHA-256 of every file |

## Cases

| Case | What it does | Result |
|---|---|---|
| `I-MMAP-01` | mapped read of 9 files written with `write(2)` (1 B to 3 MiB+7); live read-only mapping, then `write(2)` from another descriptor, from the mapper's own descriptor, and `O_TRUNC` rewrite; `read(2)` after a mapped write | pass; new data seen in 2 to 4 ms |
| `I-MMAP-02` | mapped write over existing content, `msync` `MS_SYNC` and `MS_ASYNC`, 4 KiB, 1 MiB+13 and 8 MiB; read(2); remount; asserts that `MS_SYNC` returned with all bytes at the worker | pass |
| `I-MMAP-03` | no `msync`; `munmap`, `close`, 40 s idle, clean unmount; remount | pass, data present |
| `I-MMAP-04` | `umount`, `diskutil unmount force`, `umount -f` with live mapping (descriptor open; descriptor closed; forced first; dirtied again after the refusal) | pass, data present in all four |
| `I-MMAP-05` | extension, past-EOF and shrink under a mapping | pass |
| `I-MMAP-06` | one file per trigger: idle 40 s, `MS_ASYNC`, `MS_SYNC`, `fsync`, `F_FULLFSYNC`, `sync(2)`, `munmap`, `munmap`+`close`+idle, `close` with mapping live, `read(2)`; then `umount` with live mappings | pass |
| `I-MMAP-07` | `kill -9` of the worker: dirty, synced then dirty, clean | pass (the case fails only when a mapper command hangs or the volume cannot be unmounted) |

The cases fail on a broken invariant only (data differs after a clean unmount, `ntfsfix -n` fails, `msync(MS_SYNC)` returns
before the data is at the worker, a command hangs). What the backend does at a worker kill is recorded, not judged.

## Limits

- Image files, not device nodes; the drive cache is out of scope (Environment C).
- The syncer interval (30 s) is a macOS property seen on 27.2; the cases do not assert it.
- Only `kill -9` of the worker was tested; a worker that hangs without dying was not.
- The product's session channel and daemon were not involved; the cases run the worker directly.
- One page size (16 KiB) and one mapping at a time per file.
