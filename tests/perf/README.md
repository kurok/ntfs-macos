# Performance benchmarks (T-M5-07, criterion 5.6, `P-*`)

One tool, `bench.py` (Python 3.6+, standard library only), used identically in Environment A (NTFS-3G on Linux, the
reference) and Environment B (this product in a clone of `ntfsmac-golden`). Entry point (not a stress entry, no `ENVS`
file, so `scripts/test-all.sh` does not run it):

```
sh tests/perf/run.sh --env A --out DIR --save-image IMG        # first call: makes the image, keeps it
sh tests/perf/run.sh --env B --out DIR --image IMG [--deps-tar FILE]   # same image bytes, needs a VM slot
python3 tests/perf/report.py --a A-DIR/result.json --b B-DIR/result.json --out docs/PERFORMANCE.md
```

`--smoke` selects reduced parameters (32 MiB, 1,000 random ops, 200 small files, 1,000 entries, 2 repetitions). The
defaults are the criterion run: 5 repetitions, 1 GiB sequential in 1 MiB blocks (fsync at the end), 16,384 random 4 KiB
reads and writes over a 1 GiB file, 10,000 small files of 4 KiB, directories of 10,000 and 100,000 entries.

| File | Role |
|---|---|
| `bench.py` | the workload: phases, verification of every read, sampling of worker CPU and peak RSS (worker, and in the guest the FSKit module processes), JSON out. `--dir DIR` runs on a plain directory (self-test only) |
| `env_a.sh` | runs in the container: unpacks the image, mount hook (`ntfs-3g -o rw,norecover,...`, the product's option list), runs `bench.py` |
| `perf_guest.sh` | runs in the guest (suite `perf` of `tests/integration/daemon/dmn_vm.sh`): daemon, image attached as a virtual disk, mount hook through `ntfsmac mount` / `unmount` |
| `result.py` | `bench.json` to a harness result (`ntfsmac-test-result/1`): cases `P-RUN-01`, `P-SEQ-01..02`, `P-RND-01..02`, `P-SMALL-01..04`, `P-DIR-01..`, `P-RES-01`; the numbers are in the note after `data=`. `result.py selftest` shows that a corrupted read, a short read and a missing directory entry make the matching case FAIL |
| `report.py` | `docs/PERFORMANCE.md` from the two results, with the slowdown and a flag above 2x; `report.py selftest` |
| `run.sh` | the entry point above; the guest is named `ntfsmac-perf-PID` and deleted on every exit path |

The baseline (`P-RAW-*`): the same block sizes on what the file system sits on, on scratch images (never the image of
the file system). A: a file in the container (and a loop device with direct I/O when the container may make one; it
usually may not, the case is then a skip with the reason). B: the buffered node `/dev/diskN` and the raw node
`/dev/rdiskN` of an attached scratch image, and a plain image file. The report gives each sequential and random phase also
as a ratio against its own environment's baseline. Per phase the result also holds the time of the final `fsync`,
CPU of the worker, of the FSKit module and `diskimages-helper` (B) and whole-system CPU. The JSON is rewritten after every
repetition and printed as `PERF-JSON` lines, so a run killed at its deadline leaves a partial result, which fails
`P-RUN-01`; `--max-wall` (set by `run.sh` from the deadline) stops after a repetition when the next would not fit.

Cold cache means: unmount and mount before every read phase (the file system caches and the worker's are empty); the
cache of the image file one level down is not dropped in either environment. The cases pass when the phase finished in
every repetition with verified data; they set no speed threshold, the 2x rule is applied by the report and explained
by a person. `P-RES-01` is recorded in A as well (the report compares worker CPU and memory with the reference);
TEST_STRATEGY.md lists it for B only.
