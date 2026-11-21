# Disconnect simulation (S-DISC-*, T-M5-04, criterion 5.x)

Environment B. `sh tests/stress/disconnect/run.sh --out DIR [--reps N | --full] [--fixtures DIR]
[--deps-tar FILE]` (registered through `ENVS`, so `scripts/test-all.sh --env B` and `vm/macos/run-suite.sh`
run it as the stage `stress-disconnect`). Default 3 yanks per workload plus 2 controls: a yank cycle took 24 s in the guest, so about 6 minutes of
guest time and 2 minutes of setup with a prepared `--deps-tar`, 25 to 35 minutes without it (NTFS-3G is built in
the guest). `--full` is 20 per workload (80 yanks, about 35 minutes of guest time plus the setup).
`run.sh` writes `plan.txt` first; a run with fewer yanks than planned, without `S-DISC-05` when controls were
requested, with a guest exit other than 0 or 1 or with a failed setup step FAILs (`S-DISC-RUN`).

| File | Purpose |
|---|---|
| `run.sh` | host: guest from `dmn_vm.sh --keep`, copy the suite in, run it, fetch, `result.json`, delete the guest |
| `disc_guest.sh` | guest: the yank cycles (daemon, probe, volmark, sandbox and worker as installed by `dmn_lib.sh`) |
| `wl.pl` | guest: the four write workloads, the baseline writer and the hash verifier |
| `disc_result.py` | host: `reps.tsv` and `meta.txt` to `result.json` (`ntfsmac-test-result/1`), `selftest` |

| Case | Yank during |
|---|---|
| `S-DISC-01` | a large sequential write (64 KiB chunks, no fsync) |
| `S-DISC-02` | many small file creates (200 files per directory, many directories) |
| `S-DISC-03` | rename and metadata churn (create, rename, mkdir, truncate, chmod, utime, unlink) |
| `S-DISC-04` | a loop of write + fsync(file) + fsync(directory); the writer sits idle (10 ms polls) once the fsyncs of file K (1, 3, 6, ...) have returned and the yank lands then |
| `S-DISC-05` | negative controls: a real yank, then a fault injected into the image |

The yank is the one of I-DMN-16 (`yank_image` of `dmn_lib.sh`): the process that serves the virtual disk is
killed, because `hdiutil detach -force` answers EBUSY while the worker holds the device. The point of the yank
varies per repetition: 0.3, 1, 2.2, 0.6, 3.5 or 1.6 s after the workload starts, taken in that cycle starting at
the second value (the default 3 reps use 1, 2.2 and 0.6 s; `S-DISC-04`: file 3, 6 and 2); only `--reps 6` or more
reaches all of them. A workload must have written something (progress above 0) at the yank, or the rep FAILs.
`ntfsfix -n` exit 1 may be a `mirror_split` point (a yank between the two halves of a mirrored metadata write,
TEST_STRATEGY section 6): the rep FAILs with that hint and needs manual classification.

Checks after each yank: the volume row leaves Mounted; no worker, no macfuse mount, mount point gone; the
journal entry stays; `ntfsfix -n` exits 0; `VOLUME_IS_DIRTY` is set in the image; the next attach is
`Mounted ro` with reason `unclean_session` or `dirty_flag_journal`; every file recorded as synced before the
yank (baseline files, and for `S-DISC-04` every file whose two fsyncs had returned) has its SHA-256; the
read-only session leaves the image hash unchanged.

Controls: `hash` flips one byte of a synced file in the image after the yank, `dirty` clears the dirty flag;
the hash check (`BAD base/sentinel.bin: sha256`) and the dirty-flag check (`flags_after=0x0000`) must report FAIL
and be the only FAIL of that repetition, or the control fails. The expected
FAIL lines of a control do not count as failures of the run.

## Criterion run and its result (M5-F04, M5-F05)

The criterion run is `sh tests/stress/disconnect/run.sh --out DIR --full` (80 yanks, about 35 min). Case ids: the plain ids (`S-DISC-01` to `S-DISC-05`) are written only by a run at the criterion parameters (at least 20 yanks per workload (`--full`), all four workloads, both controls `hash dirty`, no `--fault`; `plan.txt` records `fault=` and a `--fault` run fails the workload cases).
Every other run, among them the registered default of `test-all.sh --env B`, writes the same ids with the suffix
`-SHORT` and the parameters in the case note, and cannot satisfy the milestone list (`tests/gate/M5.cases`).
To put the criterion run into the record (clean tree; replace DIR):

```sh
sh tests/stress/disconnect/run.sh --out DIR --full
python3 tests/harness/report.py adopt --env B --dir DIR    # writes tests/results/<date>-<sha7>-B-disconnect.json + evidence/
# commit tests/results/ (the file and its evidence directory), then the gate:
sh scripts/ci-local.sh --verify-result tests/results/<file>.json ... --require-env host,A,B --milestone M5
```

`adopt` refuses a dirty, sha-less, invalid, failed or `-SHORT` result, and evidence that is a symlink or lies outside DIR. `run.sh` records the commit and tree state at the START in `provenance.txt`; a commit made during the run, or a tree dirty at the start or the end, marks the result dirty. One result per commit, day and suite can be adopted: a second one is refused. Unit test of the id rule: `python3 -B tests/stress/disconnect/disc_result.py selftest`;
of `adopt`: `python3 -B tests/unit/harness/test_adopt.py`.

## Pre-built inputs (M5-F07)

`--deps-tar FILE` is checked before anything starts (`scripts/run-inputs.sh deps-check`): the tar must hold
`deps/.ntfsmac-deps-key`, written when `dmn_vm.sh` saved it, with the `scripts/build-ntfs3g.sh --print-key` value of the
checkout (pinned tarball digest, patch series, configure flags; computed for unset CFLAGS and LDFLAGS, which the guest
builds run without; the install prefix is not part of it, so the seal also names the layout, `dmn`) and every member must
be under `deps/`. A tar with another key or layout, no key, an unreadable one or a stray member is refused (exit 2), with
no fallback to building; delete it, or name a new file, to build and save a current one. NOT covered: the golden image
(macOS SDK, macFUSE headers, clang): a tar made on another golden image still passes. `--fixtures DIR`: each image the
suite uses must be listed in `DIR/.fixtures-key` (one line per image: name, sha256, key, written by
`tests/fixtures/gen/*.sh --export` for the images it extracted) with the fixtures key of this checkout and with the sha256
of the file as it is now. The fixtures key is the git tree hash of HEAD for the paths the generators read or run (`tests/fixtures`, `tests/tools`, `tests/stress/crashsweep`, `src`, `include`, `third_party/patches`, `third_party/deps.lock`, `vm/linux`, `scripts/build-ntfs3g.sh`, `CMakeLists.txt`, `cmake`; full list in `scripts/run-inputs.sh`). On an unclean tree it is a hash of the contents of those files (`git ls-files -c -o --exclude-standard`) and `fixtures_dirty=1` is recorded; the run goes on and its result is marked dirty by the provenance of the suite, which the gate rejects. A dirty key never equals a clean one. It is unavailable outside a git checkout. The keys are read once at the start of the run and appended to the start record `provenance.txt`. The run writes `inputs.txt` into `--out` (deps tar sha256 and
key or `deps_tar=none`, fixtures key, a manifest of the image used), copies those lines into `meta.txt` and adds a sentence to every case note.
