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
| `report.py` | `docs/PERFORMANCE.md` from the two results, with the slowdown and a flag above 2x, and the check of the explanations (below); `report.py selftest` |
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

## Explanations are keyed to the run (M5-F06)

`report.py` prints a run key for the pair of results: the first 7 characters of the `git_sha` of A, of B and 8 hex characters
of the SHA-256 of the canonical JSON (sorted keys, compact) of both result files (`abc1234/abc1234/9f8e7d6c`; two runs of one commit
have two keys, a re-serialised copy keeps its key).
Each entry under "Explanations" in `docs/PERFORMANCE.md` carries the key of the run it was written for:
`- **seq_write** (P-SEQ-01) [run abc1234/abc1234/9f8e7d6c]: text`. When the report is generated again (`--out
docs/PERFORMANCE.md` reads the entries already in the file):

- an entry whose key is the current run key counts as explained;
- an entry with another key, or with none (`[run unkeyed]`, the entries written about the smoke run), is printed as
  "(from run KEY, not re-confirmed)" and does not count;
- a flagged phase (above 2x) with no entry gets `PLACEHOLDER: explanation needed` and does not count.

The report is written in every case, so that it can be edited. It exits 1 while a flagged phase has a placeholder or an
unconfirmed entry, while the results use smoke parameters, and when the comparison is invalid: A and B differ in `git_sha`,
parameters or image, a tree was dirty or a case failed (the open items are listed on stderr). A result file that cannot be read
or is not a valid result also exits 1, with a message. To confirm an entry for the current run, re-read it against the new
numbers, change the key in its brackets to the printed run key, and run `report.py` again. Nothing else confirms an entry. The
edit is a change in a commit that a reviewer sees: it is the review that makes the confirmation count, the tool cannot tell
a re-read from a changed key. With criterion parameters, no flagged phase and a valid comparison it exits 0.

## Pre-built inputs (M5-F07)

`--deps-tar FILE` (B) is checked with `scripts/run-inputs.sh deps-check` before the guest is cloned: the tar must hold
`deps/.ntfsmac-deps-key`, written when `dmn_vm.sh` saved it, with the key that `scripts/build-ntfs3g.sh --print-key` gives for
this checkout (pinned tarball digest, patch series, configure flags; for unset CFLAGS and LDFLAGS, as the guest build runs)
and the layout `dmn`; every member must be under `deps/`. A tar with another key, no key, no readable key or a stray member is
refused and the run stops (exit 2); delete it or point `--deps-tar` at a new file to build and save a current one. The golden
image (SDK, macFUSE headers, clang) is not part of the key. The fixture directory of this entry is the one image the run made
or was given (`--image`), which is no generator output: `dmn_vm.sh` gets `--fixtures-unkeyed`, a flag that only the suite
`perf` may use (it is refused with any other suite); the image sha256 is in the run metadata and the report requires A and B
to use the same one. The result directory gets `inputs.txt` (deps tar sha256 and key, `deps_tar=none`, or `n/a` in A, which has
no guest), and the same sentence is in the note of every case that has one.
