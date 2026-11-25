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

## Criterion 5.6 at ac5cf2b: `dir100000: [Errno 2]` and deletes at about 10 per second (2026-11-25)

Question: the criterion run `perf B` at ac5cf2b stopped with `perf: rep 1 dir100000: [Errno 2] No such file or
directory` right after `dir_10000_stat`, and deletes in that run went at about 10 per second. Is it the test, the
readdir offsets of patch 0014, the FUSE layer or the engine?

Answer: as far as the evidence goes, the FUSE layer, in the macFUSE 5.5.0 FSKit module
(`io.macfuse.app.fsmodule.macfuse-local`), not our code. Once clients other than the bench hold directories of the
volume open, every request is followed by a volume-wide synchronize that sends one FSYNCDIR per held handle and runs
as a barrier over the module's cached items (`Volume.synchronize`, `Item.withLocks` in the samples). In 2 of 4 B
reruns that synchronize began to fail with `ENOENT` (error 2), about five times a second, right after the 10,000
files of `dir10000` were removed; the bench's next call in the directory setup got that error. The test did not say
which call failed: `dirs()` makes and removes its files outside `timed()`, so the error reached `run()`
(`tests/perf/bench.py:640` at ac5cf2b) as a bare errno.

Evidence: one guest at a time (clones of `ntfsmac-golden`), the bench with `--reps 1 --seq-mib 16 --rnd-ops 100
--small-files 10000` and the directory sizes below. Every number here is in `tests/results/evidence/perf-macfuse/2026-11-25-686bffe/summary.txt` (excerpt: bench rate
lines, FSKit error counts, FUSE request counts, the module sample).

| Run | What it shows |
|---|---|
| A, criterion sizes (Docker, 1 repetition) | every phase passes: small delete 8597 files/s, `dir_100000_stat` 10389 entries/s. The engine, the bench and the image size are fine |
| R1: B, the product as at ac5cf2b (rerun at 686bffe, which changes only CI scripts), 10,000 and 100,000 entries | small delete 148.9 files/s with 210.5 s of system CPU in 67.2 s (the original run: 150.2 files/s, 210.2 s); `dir_10000_stat` 1882 entries/s. From the end of the `dir10000` teardown, during the `dir100000` setup, the FSKit log shows `-[FSVolumeConnector synchronize:replyHandler:]_block_invoke:error:2` 3696 times in 12 minutes; the bench made no progress |
| R2: R1 with an experimental engine change (`flag_nullpath_ok`, so that libfuse would pass FSYNC/FSYNCDIR of a removed node to `ntfs_fuse_fsync` instead of answering `ENOENT`) | the same message from the end of the `dir10000` teardown (count not saved); teardown 123.9 per second |
| R3: a worker that writes the FUSE trace, 10,000 and 20,000 entries | no error storm. In the session after the `dir10000` remount, `/`, `/perfbench` and `/perfbench/dir20000` were opened by other clients and never released; from then on each request is followed by one FSYNCDIR per held handle (115,252 FSYNCDIR in that session, none in the small-file sessions). The trace has 98,597 `LOOKUP` answered `ENOENT` (lookups of names that do not exist yet), but no FSYNC or FSYNCDIR answered with an error. The remount after the 20,000 entries did not complete: the volume stayed Mounted for the hook's 90 s, so the bench's cold-cache step failed |
| R4: R3 with the R2 change, 10,000 and 100,000 entries | no error storm; the `dir100000` setup slowed down until it was stopped; the module sample shows `Volume.synchronize` and `Item.withLocks` at the top |
| ntfs-3g run directly in the guest with the FUSE trace (no daemon, mount below `/private/tmp`), 10,000 files | no directory handle held, 5 FSYNC/FSYNCDIR in all, none answered with an error; unlink 162/s |

Not the cause:

- The engine: A passes with the same bench and sizes, and no FSYNC/FSYNCDIR was answered with an error in the traced
  runs.
- Patch 0014 (readdir offsets): no listing runs when the error comes (it is the setup of the next directory);
  every listing of the runs above was complete and verified by name.
- A path or a race in the test: the directory exists and the volume stays mounted (the bench's error message now says
  so).
- libfuse answering `ENOENT` for FSYNC/FSYNCDIR of a removed node: not excluded, only made unlikely. It rests on one
  run of the experimental change (R2), which failed the same way, and no trace shows that an FSYNC of a removed node
  reached `ntfs_fuse_fsync` in that run; the traced runs (R3, R4) are the ones without the error storm.
- The daemon's unmount bound: a consequence, not the cause. In the original run the remount before `dir_10000_stat`
  succeeded and the `ENOENT` came afterwards, in the `dir100000` setup, which has no remount. The failed remount of R3
  comes from the same slow module, after the error-free part.

What changed (test only; no product change):

- `tests/perf/bench.py` `dirs()`: every step around the timed phases (mkdir, each create, the fsync of the directory,
  each unlink, the rmdir) goes through `untimed()`, so an error names the step, the file and whether the volume is
  still mounted (`dir100000 setup: create e0012345 (12346 of 100000): [Errno 2] ...; /Volumes/perf is still mounted`);
  the rate of the setup and of the teardown is printed (`dir10000 teardown: 10000 files removed in 66.114 s, 151.3 per
  second`), so a slow delete is visible.
- `--fault dir-setup` and the `perf.result` self-test (`tests/perf/result.py`): a failing create in the setup must
  produce that message, fail `P-DIR-01` and `P-RUN-01`, and let the next directory run.

Delete rate, measured: Environment A 8597 files/s (small files). Environment B, before and after this change (the
change is diagnostic, so the rate is the same): small files 149 to 164 per second; the 10,000-entry teardown 124 to 151
per second. The original run's about 10 per second was not reproduced with a number; it matches the state in which the
synchronize fails or the module holds tens of thousands of items.

What remains:

- The `dir_100000` phases cannot pass in Environment B with macFUSE 5.5.0 as measured: making 100,000 entries through
  the module slows down until it stalls, and the cold-cache remount after many entries did not complete in R3.
  On main (647c18f) a plain unmount refused with `EBUSY` asks again for up to 60 s while every holder is Spotlight or
  Quick Look (proposed ADR-028); `fseventsd` is not on that list, the R3 trace does not name the holders of the directory
  handles, and a module still busy in its synchronize is not covered by that wait. Options for the owner: pre-populate
  the directories in the image in Environment A (both environments then list the same on-disk directory and only the
  listing goes through the module), record the 100,000-entry case as not runnable on this macFUSE version, or extend
  the daemon's unmount wait to a module that is still unmounting (a product decision beyond ADR-028). None is done here.
- Report to macFUSE (draft in `docs/UPSTREAM_REPORTS.md`): synchronize as a barrier with one FSYNCDIR per held
  directory handle after every request, and synchronize failing with `ENOENT` after a mass delete.
