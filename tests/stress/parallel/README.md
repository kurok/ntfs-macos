# Parallel writers (S-PAR-01, T-M5-02, criterion 5.3)

Environment B. `sh tests/stress/parallel/run.sh --out DIR [--full | --smoke | --duration S] [--workers N]
[--hang-limit S] [--verify-every S] [--controls LIST] [--fixtures DIR] [--deps-tar FILE]`; registered through
`ENVS` as the stage `stress-parallel`. The code is shared with the soak (`tests/stress/soak`: `mix_host.sh`,
`mix_guest.sh`, `mixlib.py`, `mix_result.py`; kind `par`).

Criterion 5.3: 16 writer processes for 30 minutes, zero mismatches, no hang longer than 60 s. The registered
default is 5 minutes with 16 writers. **The criterion run needs `run.sh --out DIR --full`** (30 min, verification
every 2 min). `--smoke` is 2 minutes.

Workload: 16 forked processes on one daemon-mounted rw volume. Each has a private tree `par/wNN` with a model (the
operations of the soak: small files, sequential and random writes, truncate, rename, mmap, directory churn, deletes,
readdir) and shares four directories `par/shared/sK` with the others: it creates files whose name carries the content
key and size (content = SHAKE-256 of the key; published by rename of a `.tmp-` name, so a reader never sees a partial
file), renames them (only the generation suffix changes), deletes them, reads and verifies three files of any writer
by their name (ENOENT is tolerated only for files of other writers), and checks that the listing holds exactly its own
names. Working set 1.5 MiB per writer. All content is verified every `--verify-every` s, at the end, and after a
clean unmount, `ntfsfix -n` and remount against the manifests (every file, no extra or missing entry).

Hang rule: every operation is timed in a slot in shared memory; the parent kills the workers and fails the run when a
worker made no progress for `--hang-limit` s (60) or when the longest operation was that long; any single operation or
gap of 60 s or more is a hang. The result carries operation counts, latency p50/p99/p99.9 (log2 millisecond buckets),
the maximum and the five slowest operations.

Controls (`S-PAR-01-CONTROL`, default `corrupt stall`): a byte flipped behind the model's back (driver exit 1, mismatch
the only failure kind, failing remount verification) and a simulated stall of worker 1 inside one operation with the
hang limit lowered to 4 s for that run (driver exit 1, hang the only failure kind, worker 1 named). `--fault corrupt|stall`
injects into the real run (the suite must then FAIL).

What stays outside the 60 s rule and what the controls do not cover: see the last section of
`tests/stress/soak/README.md` (same driver). In short: `sync`, unmount, remount and the remount tree walk are bounded
by deadlines only, and the stall control is a sleep inside the timed window, not a blocked filesystem call.

## Criterion run and its result (M5-F04, M5-F05)

The criterion run is `sh tests/stress/parallel/run.sh --out DIR --full` (30 min, 16 writers). Case ids: the plain ids (`S-PAR-01`, `S-PAR-01-CONTROL`) are written only by a run at the criterion parameters (at least 30 min, exactly 16 writers, hang limit at most 60 s, both controls `corrupt stall` on, no `--fault`).
Every other run, among them the registered default of `test-all.sh --env B`, writes the same ids with the suffix
`-SHORT` and the parameters in the case note, and cannot satisfy the milestone list (`tests/gate/M5.cases`).
To put the criterion run into the record (clean tree; replace DIR):

```sh
sh tests/stress/parallel/run.sh --out DIR --full
python3 tests/harness/report.py adopt --env B --dir DIR    # writes tests/results/<date>-<sha7>-B-parallel.json + evidence/
# commit tests/results/ (the file and its evidence directory), then the gate:
sh scripts/ci-local.sh --verify-result tests/results/<file>.json ... --require-env host,A,B --milestone M5
```

`adopt` refuses a dirty, sha-less, invalid, failed or `-SHORT` result, and evidence that is a symlink or lies outside DIR. `run.sh` records the commit and tree state at the START in `provenance.txt`; a commit made during the run, or a tree dirty at the start or the end, marks the result dirty. One result per commit, day and suite can be adopted: a second one is refused. Unit test of the id rule: `python3 -B tests/stress/soak/mix_result.py selftest`;
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
