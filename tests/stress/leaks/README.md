# Leak detection on the worker (S-LEAK-01, T-M5-03, criterion 5.4)

Environment B. `sh tests/stress/leaks/run.sh --out DIR [--fixtures DIR] [--iterations N]
[--deps-tar FILE]` (registered through `ENVS`, so `scripts/test-all.sh --env B` and
`vm/macos/run-suite.sh` run it as the stage `stress-leaks`). Default 3 iterations, about 1 minute with
a prepared `--deps-tar` (a stale tar only seeds the build, which always re-runs and is a no-op when current); a full run is `--iterations 50` (about 6 minutes of guest time).

| File | Purpose |
|---|---|
| `run.sh` | host: selftest, patched-line map, clone a guest, NTFS-3G build or `--deps-tar`, run, fetch, classify, delete the guest |
| `leaks_guest.sh` | guest: control scan, mount, three `leaks` scans around the workload, unmount |
| `workload.py` | the functional workload, every write read back against a model |
| `leakctl.c` | control: 8 known leaks from `ctl_leak_site` |
| `classify.py` | patch map, `leaks` parser, project/engine/platform classification, `result.json`, `selftest` |

A standalone run takes no VM slot (the callers `test-all.sh` and `run-suite.sh` hold it): with two guests
already running, take a `slot-N` directory yourself.

## What works in the guest

* `leaks PID` (as root, `sudo -n`) works on the worker: it is not hardened and carries no entitlement
  that blocks `task_for_pid` for root. Exit status 0 no leaks, 1 leaks found.
* Allocation stacks need `MallocStackLogging=1` **in the worker's environment at start**; it cannot be
  switched on later. The daemon starts the worker with a fixed environment (`PATH` only), so the run
  starts the worker itself: the same guest-built `ntfs-3g` (the daemon's `ntfsmac-worker`), the same
  argument vector as `src/options` (device as inherited `/dev/fd/9`, mount point `/Volumes/...`, the
  fixed option string with `norecover`), `NTFSMAC_ERRFD` set, as the console user. Missing: the sandbox
  profile and the launchd job. A daemon-started worker can be scanned too (`sudo leaks PID`), but
  prints no stacks, so it cannot be classified.
* Source lines in the stacks (`ntfs-3g.c:4604`) come from the debug map of the binary, which points at the
  object files of the guest build (`~/ntfsmac/build`). Without them (a `--deps-tar` from `dmn_vm.sh`
  has only `deps/`) the run FAILS ("no source line information"): the patched-line rule would be blind.

## Classification

The worker is NTFS-3G with libntfs-3g linked in, so project and engine code share one image and are
separated by source line. `classify.py patched-map` applies `third_party/patches/ntfs-3g` to the pinned
tarball and diffs it against the pristine tree. A leaked root is

* **project** if a frame of the worker is at a line a patch added or changed (R1), or the allocation site
  is in the worker image and in a function a patch changed (R2), or a frame is in an image given by
  `--project-image` (use it for the daemon and CLI, `src/`);
* **engine** if it has a frame in the worker or libfuse and is not project;
* **platform** if only Apple libraries are in the stack.

The run fails on a project leak, on a leak without a stack, on a failed workload, and on a failed control.
Engine and platform leaks are listed in `summary.txt` and in the note of `S-LEAK-01`.

Known engine leaks (macOS 27.2, macFUSE 5.5.0, NTFS-3G 2026.9.28): 1 block of 160 bytes from
`fuse_new_common` (libfuse, the `-oiconpath=` option string, allocated once at mount, not growing). Early
scans of the idle worker can also show libfuse request buffers from `fuse_session_loop` (1.4 KiB to
22 KB, engine).

## Can it fail?

* `classify.py selftest`: synthetic stacks for each class and for the no-stack case.
* Control: `leakctl` leaks 8 blocks; `leaks` must find them with `ctl_leak_site` in the stack, and
  the same output must classify as project under a rule naming that function.
* `--project-image libfuse.2.dylib` turns the real engine leak into a project leak and the run exits 1.

## Criterion run and its result (M5-F04, M5-F05)

The criterion run is `sh tests/stress/leaks/run.sh --out DIR --iterations 50`. Case ids: the plain ids (`S-LEAK-01`, `S-LEAK-01-CONTROL`) are written only by a run at the criterion parameters (at least 50 workload iterations; the control always runs and must pass).
Every other run, among them the registered default of `test-all.sh --env B`, writes the same ids with the suffix
`-SHORT` and the parameters in the case note, and cannot satisfy the milestone list (`tests/gate/M5.cases`).
To put the criterion run into the record (clean tree; replace DIR):

```sh
sh tests/stress/leaks/run.sh --out DIR --iterations 50
python3 tests/harness/report.py adopt --env B --dir DIR    # writes tests/results/<date>-<sha7>-B-leaks.json + evidence/
# commit tests/results/ (the file and its evidence directory), then the gate:
sh scripts/ci-local.sh --verify-result tests/results/<file>.json ... --require-env host,A,B --milestone M5
```

`adopt` refuses a dirty, sha-less, invalid, failed or `-SHORT` result, and evidence that is a symlink or lies outside DIR. `run.sh` records the commit and tree state at the START in `provenance.txt`; a commit made during the run, or a tree dirty at the start or the end, marks the result dirty. One result per commit, day and suite can be adopted: a second one is refused. Unit test of the id rule: `python3 -B tests/stress/leaks/classify.py selftest`;
of `adopt`: `python3 -B tests/unit/harness/test_adopt.py`.
