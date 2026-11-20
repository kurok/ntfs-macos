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
