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
