# Forced process termination, repeated (S-KILL-*, T-M5-05, criterion 4.4)

Environment B. `sh tests/stress/kill/run.sh --out DIR [--reps N] [--targets LIST] [--step SECONDS]
[--fixtures DIR] [--deps-tar FILE]` (registered through `ENVS`, so `scripts/test-all.sh --env B` and
`vm/macos/run-suite.sh` run it as the stage `stress-kill`). Default 2 repetitions per target (smoke, about
6 minutes of guest time with `--deps-tar`); the full run is `--reps 20` (about 30 minutes).

One guest (`ntfsmac-kill-PID`, always deleted: by the driver, and by `run.sh` by name as a last resort) runs every
repetition of every target (`kill_guest.sh`, started through `tests/integration/daemon/dmn_vm.sh --name NAME stress-kill`,
which also installs the daemon, probe, volmark, sandbox and worker exactly as the I-DMN suites do). The helpers of
`tests/integration/daemon/dmn_lib.sh` are reused; `wait_for` is replaced by a wall-clock version (the lib's counts
iterations, so a slow check would stretch it), and the helpers that can block are wrapped with a deadline.

| Case | Target | What is killed |
|---|---|---|
| `S-KILL-01` | worker | `SIGKILL` of the `ntfsmac-worker` that holds the disk, during a rw session |
| `S-KILL-02` | daemon | `SIGKILL` of `ntfsmacd` with a rw volume mounted, launchd restarts it |
| `S-KILL-03` | macFUSE module process | `SIGKILL` of the FSKit module instance that serves the mount |
| `S-KILL-04` | image | the process that serves the virtual disk (`diskimages-helper`) is killed during a write (the yank of I-DMN-16) |
| `S-KILL-05` | controls | injected damage must fail the intended check and no other; never-ending and slow steps must fail after their deadline |
| `S-KILL-RUN` | the run | failures outside the repetitions (daemon start, setup), a guest that did not finish, a host deadline |

Every repetition checks: journal entry kept; the volume's dirty flag set on the image (`flags_after=0x0001`); the
session is settled for the daemon before the journal is sampled; no stray worker and no macFUSE mount; the daemon
answers (worker, module, image: the same process; daemon: a new pid within the deadline, no worker of the dead
daemon alive, the restarted daemon holds no rw mount); the worker kill is announced to the user; the detach target
checks that the writer is still writing at the yank; the next attach is `Mounted ro` with reason `unclean_session` or
`dirty_flag_journal`; the file written durably (fsync) before the kill is intact; the read-only session leaves the
image hash unchanged; after the repetition nothing is left behind and no module instance serves a volume.

Deadlines: every waiting step has `--step` seconds (default 90) on the wall clock, every command that can block
(CLI, `hdiutil`, `lsof`, a read on a dead mount) runs under `tmo`, and the host supervises the whole guest run with a
deadline derived from `--step` and the repetition count (SIGTERM, which deletes the guest, then SIGKILL).
An expired deadline is a FAIL.

## The macFUSE module process

macFUSE 5.5.0 runs a volume as an FSKit module: `io.macfuse.app.fsmodule.macfuse-local.appex`, a process of the
console user that the system starts. In the guest there are two kinds of instance (`ps`, `lsof`):

* one long-lived instance without a volume (it holds no device);
* one instance per mounted volume, started at mount time, holding the volume's raw device (`/dev/rdiskNs1`, a
  synthetic disk FSKit created; `mount` shows `/dev/diskNs1 ... (macfuse, ... fskit)`) and a unix socket.

`kill_guest.sh` finds the instance that serves the mount by the kernel mount's device (`module_of`: the module
process with `/dev/r<that device>` open) and FAILs when not exactly one instance matches.

Required for `S-KILL-03` (owner decision: a module death is not a clean unmount): journal entry kept, dirty flag
set, next attach `Mounted ro` with the documented reason. Observed so far (macOS 27.2 guest): FSKit takes the
module's death as the end of the mount, the worker gets an ordinary unmount and exits 0, and the daemon ends the
session through the clean-session path (journal entry removed, "ejected, all data written", volume clean, next
attach rw). The case reports that as `module death reported as a clean session` and FAILs until the daemon is
changed.

Measurement (a NOTE and a `MEASURE` line, stored in `kill-module-measure.json`, not a check): before the kill a page
of a file is dirtied through a shared mapping (`mmapdirty.c`, no `msync`) and a second file is written with `write(2)`
and no `fsync`; after the kill the next attach reads both back. The result says whether each survived and whether
the session was reported clean (ARCHITECTURE section 9, D1).

## Controls (`S-KILL-05`)

A check that cannot fail proves nothing. The run damages the state itself, with the checks in "control mode"
(failures are counted separately); each control must fail on exactly the intended check, any other failing check
makes the control FAIL:

* `inject-journal`: the journal entry is hidden for the check "the journal entry stays" only;
* `inject-image`: a byte is appended to the image after the read-only session: "the read-only session left the
  image unchanged" must fail;
* `inject-flags`: the dirty flag is cleared on the image before the check "the dirty flag is still set";
* `hang`: `tmo 3 sleep 60`, `wait_for 3 false` and `wait_for 6` of a check that takes 4 s and never succeeds
  must each fail, the slow one within 12 s (the iteration-counting wait would need 30 s).

`KILL_CONTROL=0` / `--no-control` skips them.

## Files

| File | Purpose |
|---|---|
| `run.sh` | host: fixture, `dmn_vm.sh --name NAME stress-kill` (one guest) under a supervisor, `result.py` |
| `kill_guest.sh` | guest: the repetitions, the checks, the controls |
| `mmapdirty.c` | guest: dirties a mapped page without `msync` (measurement) |
| `result.py` | parses the guest output into `result.json` (`ntfsmac-test-result/1`), with a self-test |
| `ENVS` | `B` |

The long run, on a prepared tar of the guest's NTFS-3G build:
`sh tests/stress/kill/run.sh --out DIR --reps 20 --deps-tar FILE` (about 30 minutes, one guest).

## Criterion run and its result (M5-F04, M5-F05)

The criterion run is `sh tests/stress/kill/run.sh --out DIR --reps 20` (about 30 min). Case ids: the plain ids (`S-KILL-01` to `S-KILL-05`, `S-KILL-RUN`) are written only by a run at the criterion parameters (at least 20 repetitions of all four targets, controls on (no `--no-control`), the driver ended with status 0 or 1; any other driver status also fails `S-KILL-RUN`).
Every other run, among them the registered default of `test-all.sh --env B`, writes the same ids with the suffix
`-SHORT` and the parameters in the case note, and cannot satisfy the milestone list (`tests/gate/M5.cases`).
To put the criterion run into the record (clean tree; replace DIR):

```sh
sh tests/stress/kill/run.sh --out DIR --reps 20
python3 tests/harness/report.py adopt --env B --dir DIR    # writes tests/results/<date>-<sha7>-B-kill.json + evidence/
# commit tests/results/ (the file and its evidence directory), then the gate:
sh scripts/ci-local.sh --verify-result tests/results/<file>.json ... --require-env host,A,B --milestone M5
```

`adopt` refuses a dirty, sha-less, invalid, failed or `-SHORT` result, and evidence that is a symlink or lies outside DIR. `run.sh` records the commit and tree state at the START in `provenance.txt`; a commit made during the run, or a tree dirty at the start or the end, marks the result dirty. One result per commit, day and suite can be adopted: a second one is refused. Unit test of the id rule: `python3 -B tests/stress/kill/result.py selftest`;
of `adopt`: `python3 -B tests/unit/harness/test_adopt.py`.
