# M5 performance: why the mount on a disk image was 28-89x slower than its device node (T-M5-07 follow-up)

Date: 2026-11-20. Question: the T-M5-07 smoke run (Environment B, 32 MiB, 2 repetitions) measured the product mount
28 to 89 times slower than the buffered node `/dev/diskN` it sits on (sequential and random read and write), while NTFS-3G on Linux is 1.8 to 13 times slower
than its image file. The worker used under 2 % CPU, so the time was spent waiting. Is the cause ours?

Answer: yes. It was an I/O priority inversion between the worker and `diskimages-helper`, caused by the I/O tier the
worker inherited from the daemon. The fix (`ntfsmac_spawn_io_passive()` in `src/daemon/spawn.c`, called once in the
daemon's `main()` before any thread, and again in every child the spawn module starts) makes the disk I/O of the daemon
and of everything it starts passive. After the fix the same smoke run is 21 to 49 times faster on those four phases and
1.3 to 2.7 times slower than the node. Durability is unchanged. Scope: measured on disk images attached with `hdiutil`
(the worker on the buffered node `/dev/diskN`); see "Scope and what remains".

## Cause

- The daemon is a launchd job with `ProcessType Interactive`, so its children inherit I/O tier 0. `fs_usage -f diskio`
  shows the worker's I/O to `/dev/diskN` as `WrMeta[A]` (no `T` flag, tier 0). A shell started by `tart exec` (where
  the baseline and `dd` run) issues tier 1 (`WrMeta[AT1]`).
- `diskimages-helper` serves the image with tier-1 passive I/O on the disk that holds the image file
  (`WrData[ANT1P]`). A disk image is in the same throttle domain as that disk, so a tier-0 I/O to the image node is
  activity that throttles the helper's tier-1 I/O.
- Result: the helper serves one request, then waits before it serves the next (the observed gap; we did not tie it to a
  documented kernel constant). In the trace of one
  1 MiB write (64 buffer I/Os of 16 KiB), the worker's I/Os complete 5.5 to 6 ms apart, and each backing write of the
  helper takes 0.1 to 0.4 ms. The worker's `pwrite` of 1 MiB took 0.38 s (2.6 MiB/s). The same 64 I/Os from `dd`
  complete 0.1 ms apart (1 MiB in 8 ms).
- Reproduced without the product: the same pattern (a 4 KiB read, four small writes and one 1 MiB write, 8 times) on
  a zero-filled scratch image node, by `python3` started as a launchd job: tier 1 (default from `tart exec`) 6.0 ms per
  round; tier 0 (`ProcessType Interactive`) did not finish 8 rounds in 3 s (396 of 516 I/Os done, about 6 ms apart);
  `ProcessType Interactive` with `taskpolicy -d passive` 5.9 ms per round (I/O shows `[AP]`).

## Hypotheses that were checked and are not the cause

| Hypothesis | Evidence | Result |
|---|---|---|
| Small device I/Os, each a round trip | The worker writes and reads 1 MiB per FUSE request (`pwrite`/`pread` `B=0x100000`) and 1 KiB / 4 KiB for metadata; the kernel splits each into 16 KiB buffer I/Os, exactly as for `dd` on the same kind of node | not the cause: same I/O shape as the fast baseline |
| A device flush per write (patch 0007, ADR-022) | During a plain 4 MiB write without `fsync`: 0 `fsync`, `fcntl` or `ioctl` calls by the worker. The flush runs only at `fsync` and unmount, as designed | not the cause. The 4.5 s `fsync` of the random-write phase was the same throttling (dirty buffers drained one per period): 0.07 s after the fix |
| Small FUSE requests (max_write, read-ahead) | Write and read requests arrive as 1 MiB; the option builder passes no size option and none is needed | not the cause; no option change |
| Process QoS / I/O throttling | See "Cause" | the cause |

## Before and after

Micro test in one kept guest (16 MiB file, 300 random 4 KiB operations, 50 small files; `ntfsmac unmount` and
`mount` before each read phase), old daemon then new daemon, same image:

| Phase | Before | After |
|---|---|---|
| sequential write (with `fsync`) | 2.50 MiB/s | 97.0 MiB/s |
| sequential read (cold) | 1.99 MiB/s | 70.9 MiB/s |
| random 4 KiB write (with `fsync`) | 161 ops/s | 1572 ops/s |
| random 4 KiB read (cold) | 150 ops/s | 1490 ops/s |
| small files: create + write | 46 files/s | 372 files/s |

`sh tests/perf/run.sh --env B --smoke` (same image bytes as the T-M5-07 smoke, SHA-256 `a96393f4...`); before is the
T-M5-07 smoke, after is this change. Smoke parameters, not the criterion run:

| Phase | Before | After | After vs buffered node (same run) |
|---|---|---|---|
| sequential write | 1.81 MiB/s | 89.0 MiB/s | 1.8x (node 161) |
| sequential read (cold) | 2.18 MiB/s | 56.9 MiB/s | 1.8x (node 99.8) |
| random 4 KiB write | 95.7 ops/s, `fsync` 4.54 s | 2009 ops/s, `fsync` 0.07 s | 1.3x (node 2650) |
| random 4 KiB read (cold) | 62.6 ops/s | 1834 ops/s | 2.7x (node 4963) |
| small files: create + write | 51.5 files/s | 340 files/s | |
| small files: read (cold) | 342 files/s | 581 files/s | |
| small files: delete | 311 files/s | 501 files/s | |
| directory of 1000: readdir + stat (cold) | 1109 entries/s | 1766 entries/s | |

The whole B smoke run (clone, setup, every phase, deletion) took 2.5 minutes.

## What the change does and does not do

- `setiopolicy_np(IOPOL_TYPE_DISK, IOPOL_SCOPE_PROCESS, IOPOL_PASSIVE)`: once in the daemon's `main()` before any
  thread starts (the daemon's own I/O and every child it starts, also those not started by the spawn module, inherit
  it), again in each child of the spawn module before the exec, and, defensively, in the `--exec-as` step after
  `launchctl asuser` (not observed to reset it; a failure there is logged). A passive I/O keeps the tier of the process
  but does not count as activity for the throttling of others.
- Unchanged: the order and completion of I/O, `fsync`, the `DKIOCSYNCHRONIZE` flush of patch 0007 at every sync point
  and at unmount (D1, ADR-022), the session channel and the clean-session proof, the worker's option vector and
  `argv_check` (no option changed).
- Side effect: the worker's I/O no longer delays lower-tier I/O of other processes on the same disk (for example
  Spotlight or Time Machine on another volume of that disk). Its own I/O is still tier 0.
- Kill suite (`tests/stress/kill/kill_guest.sh`, S-KILL-04): the writer during the yank was a fixed 37.5 MiB `dd`,
  which now ended before the yank. It is now one `perl` process that rewrites a pre-made 10 MiB file (opened without
  `O_CREAT`) until a write fails, and the repetition checks that it ended with a write error (not `ENOSPC`, not killed).
  A failed detach of the image is now reported with `hdiutil` and `lsof` output and fails the repetition as "still
  attached" (`_detach_raw` in `tests/integration/daemon/dmn_lib.sh`).
- Test: every spawn test (`daemon.spawn`, `daemon.sandbox`) now also checks in the stand-in child that the process
  disk I/O policy is `IOPOL_PASSIVE`; without the call `daemon.spawn` fails.

## Scope and what remains

- The inversion needs a lower-tier process between the node and the storage, as `diskimages-helper` is for a disk
  image. That is every Environment B measurement and any NTFS disk image a user attaches. Only disk images attached with
  `hdiutil` and the buffered node `/dev/diskN` were tested. A physical USB or internal disk has no such helper; whether
  it was affected was not measured (Environment C).
- After the fix the remaining cost is mostly per request: about 0.5 ms per 4 KiB operation through macFUSE's FSKit
  module and the worker (random 4 KiB read 2.7x slower than the node). That is macFUSE FSKit and FUSE round trips,
  not the device path; no change is proposed here.
- The criterion run (`tests/perf/run.sh --env A` and `--env B` with the default parameters) has not been made.
