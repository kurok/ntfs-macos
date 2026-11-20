# Soak (S-SOAK-01, T-M5-02, criterion 5.2)

Environment B. `sh tests/stress/soak/run.sh --out DIR [--full | --smoke | --duration S] [--baseline S]
[--verify-every S] [--rss-every S] [--controls LIST] [--fixtures DIR] [--deps-tar FILE]`; registered through
`ENVS` as the stage `stress-soak`. The parallel suite (`tests/stress/parallel`) shares every file here.

Criterion 5.2: 6 hours of mixed workload in the VM, zero hash mismatches, worker RSS growth below 10 percent
after the first hour. The registered default is 10 minutes (baseline at a sixth of the run), so a full B run stays
short. **The criterion run needs `run.sh --out DIR --full`** (6 h, baseline at 1 h, verification every 10 min, RSS
every 30 s; the host deadline is the run plus about 40 min of setup and mount cycles). `--smoke` is 3 minutes;
`--baseline S` moves the baseline point (the smoke run in the task used 60 s).

| File | Purpose |
|---|---|
| `run.sh` | entry point (kind `soak`) |
| `mix_host.sh` | host: guest from `dmn_vm.sh --keep`, copy in, deadline, fetch, `mix_result.py`, delete the guest (shared with parallel) |
| `mix_guest.sh` | guest: daemon-mounted rw volume, driver run, clean unmount, `ntfsfix -n`, remount, tree verification, controls |
| `mixlib.py` | guest: the workload with a model, the verification, the hang watchdog, `rss_growth`, the manifest walk, `selftest` |
| `mix_result.py` | host: `runs.tsv` + summaries + `plan.txt` to `result.json`; judges everything again, `selftest` |

Workload (one worker process, private tree `soak/w00`, model = every file's bytes and every directory): create small
files (0 to 8 KiB around block and sector sizes), sequential appends, random `pwrite` (also past the end: a hole
reads as zeros), truncate (shrink and extend), rename (also over an existing file), mmap read and write with
`msync`, mkdir, rmdir (and the ENOTEMPTY error path), directory rename, delete, readdir and read-back with `stat`.
Working set at most 16 MiB of the 64 MiB volume. A mismatch or unexpected error stops the worker and fails the run.

RSS rule (`mixlib.rss_growth`): samples of `ps -o rss=` of the worker that holds the device, taken every
`--rss-every` s from the start. Baseline = the first sample at or after the baseline point (rejected when later than
60 s after it), growth = (peak of every sample after the baseline - baseline) / baseline, FAIL at 10 percent or
more (exactly 10 fails), `error` (also a FAIL) when the rule cannot be applied (no baseline sample, fewer than 3
samples after it). The guest computes it; the host computes it again from `rss.tsv`.

Cases: `S-SOAK-01` passes only when the driver exited 0, ran the planned duration, made at least duration/verify
full verifications, had zero mismatches/errors, no operation or gap of 60 s or more, unmounted cleanly,
`ntfsfix -n` exit 0 before and after, the remount came up `Mounted rw` and the walk of the tree matched the
manifests, and the RSS rule passed. `S-SOAK-01-CONTROL` (default control `corrupt`): a byte flipped on the volume behind
the model's back must give driver exit 1 with mismatch as the only failure kind and a failing remount verification,
and the RSS rule on synthetic samples must give pass/pass/fail/fail/error. `--fault corrupt` injects the flip into
the real run (the suite must then FAIL). `S-SOAK-RUN` fails on an abnormal guest exit or a failed setup step.

Outside the hang rule and the controls (read this before relying on a pass):
- The 60 s rule covers every timed operation of the workers (also each file read of a verification) and the gaps
  between them. It does not cover `sync`, the daemon unmount, the remount and the tree walk of `mixlib.py verify`;
  those run under deadlines only (`tmo` 180 s for the unmount, 120 s for the volume to leave Mounted, 900 s for the
  verification) and the host deadline of the whole run. A hang there fails the run by deadline, not by the 60 s rule.
- The RSS rule needs the baseline point inside the run with at least 3 samples after it; a run shorter than the
  criterion says "shortened run, not criterion 5.2" in the case note and is not evidence for it.
- The controls show that the model comparison, the remount walk, the hang watchdog (parallel) and the RSS arithmetic
  can fail. They do not show that a real RSS growth of the live worker is seen (the rule runs on synthetic samples
  there), that a hang inside the kernel or the filesystem (an operation that really blocks) is caught (the stall
  is a sleep inside the timed window), or that a corrupted file the model never touches again is found by anything
  but the periodic and final verification.
- Slots in shared memory carry one aligned 8-byte store per field; the watchdog reads the `last progress` field and
  needs two consecutive polls (0.5 s apart) to agree on a stale value, so a torn read cannot make a false hang.
- The shared directories of the parallel suite hold at most 40 files per writer (a create at the limit becomes a
  delete) and their bytes count in the writer's working-set cap, so the volume cannot fill up.
- A driver that is killed (deadline, TERM) kills its process group; workers also exit when their parent is gone.
