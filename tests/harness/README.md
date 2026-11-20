# Test harness (T-M2-08)

Python 3.9, standard library only (it also runs under the macOS guest's `/usr/bin/python3`).

| File | Purpose |
|---|---|
| `core.py` | `@case("ID", envs=("A","B"))` decorator, discovery, `Skip`/`Fail`, per-case `Ctx` |
| `runner.py` | runs cases, maps outcomes to `pass`/`fail`/`skip`/`error`, collects evidence |
| `report.py` | result file build, name, write, hand-written validator; `aggregate` (merge the stages of one `scripts/test-all.sh` run into one file); `readme` (README status table); `git-info` |
| `features.json` | feature name to case-id patterns, the input of the README status table |
| `transport.py` | `LocalTransport`, `ContainerTransport` (Env A), `VmTransport` (Env B, `tart.sh exec` / `tart exec -i`); file copy as tar streams |
| `volume.py` | pluggable mount/unmount per environment, macFUSE detection |
| `run.py` | CLI |
| `run-env-b.sh` | clone a disposable VM, run in it, always delete it |

## Declaring a case

A module anywhere under `tests/integration`, `tests/compatibility` or `tests/stress`:

```python
from harness import case

@case("I-XYZ-01", envs=("A", "B"), title="what it checks")
def run(ctx):
    vol = ctx.volume(64 * 1024 * 1024)   # Skip with a note if the env cannot mount
    ...                                  # raise Fail("msg") to fail, ctx.skip("note") to skip
```

Return normally for `pass`. `Fail` or `AssertionError` is `fail`, any other exception is `error`,
`Skip` is `skip` and must carry a note (an empty note becomes `error`; the validator rejects it too).
`ctx.log()` writes `tests/results/evidence/<run>/<ID>.log`; `ctx.evidence_file("x.txt")` gives a path
for more small evidence files.

## Running

```sh
python3 tests/harness/run.py --env A --cases 'I-INT-*'          # Env A: through vm/linux/run.sh
tests/harness/run-env-b.sh --cases I-INT-01                     # Env B: clone, run in the guest, delete
python3 tests/harness/run.py --env A --list                     # list cases for an env
python3 tests/harness/run.py --env host --local --out /tmp/res  # run on this machine
```

`--local` runs in the current process (what executes inside the container or guest). Without it,
env A calls `vm/linux/run.sh run` and env B copies the harness into the running VM named by `--vm`,
runs it there and copies the result and evidence back. Exit code: 0 no fail/error (skips allowed),
1 a case failed or errored, 2 usage or setup error.

Result: `<out>/<UTC date>-<short sha>-<env>.json` (default `tests/results/`), schema in
`docs/TEST_STRATEGY.md` section 10 with `"schema": "ntfsmac-test-result/1"`. `dirty_tree` ignores changes
under `tests/results/` (outputs, not inputs). Inside the container the repo is read-only and has no
way to pick up the host's git state reliably, so the orchestrator passes `--git-sha` and `--dirty`.

Dev overrides for I-INT-01 (local runs only, not forwarded into the container): `NTFSMAC_SEED`,
`NTFSMAC_INT01_FILES`, `NTFSMAC_INT01_MAX`.

## One command per environment (T-M5-01)

```sh
scripts/test-all.sh --env host --quick            # build, unit tests (no ASan), product build
scripts/test-all.sh --env A [--cases GLOB]        # container: selftest, probe, volmark, harness A, stress
scripts/test-all.sh --env B [--quick] [--slot-dir DIR]   # VM: harness B, C-REF-*, daemon suites, stress
scripts/test-all.sh --env A --dry-run             # print the stage plan, run nothing
```

Every stage runs, a failing stage does not stop the run, and one result file is written
(`tests/results/<date>-<sha>-<env>.json` or `--out FILE`, evidence in `evidence/<name>/`). Each stage
is a case `STAGE-<ENV>-<NAME>` (`pass`, `fail`, or `skip` with the reason); the cases of the harness
runs are merged in unchanged, so the schema does not change. Exit 0 only if no stage and no case
failed and something passed (a run in which every stage skipped is not a success). `--quick` leaves out the
long stages and runs only `I-INT-*`; `--cases` runs only the harness stages. The whole suite is three
commands: `--env host` (macOS build and unit tests), `--env A` and `--env B`. A failed preflight (docker,
tart, golden VM, image, pinned tarball) fails the stage `preflight` and skips the rest; nothing is built or
downloaded for it. Host paths are replaced by `<repo>`, `<home>` and `<tmp>` in everything copied under
`tests/results`.

Env B goes through `vm/macos/run-suite.sh`, which calls `run-env-b.sh` (twice: guest cases, then
`--orchestrate` for the host-side cases), `dmn_all.sh` and the B stress entries; each of those
clones the golden VM and deletes its clone. B always takes a `slot-N` lock with `mkdir` in `${NTFSMAC_VM_SLOT_DIR:-${TMPDIR:-/tmp}/ntfsmac-vm-slots}`
(`--slot-dir DIR` overrides).

Stress entry points: `tests/stress/NAME/run.sh` is run as `run.sh --out DIR` for the environments in
`tests/stress/NAME/ENVS` (crashsweep is built in as A); with none, the stage is skipped as "none
registered". `scripts/test-all.sh --list-stress ENV` prints them.

By hand:

```sh
python3 tests/harness/report.py aggregate --env A --work DIR [--work DIR2] [--out FILE]
python3 tests/harness/report.py readme            # rewrite the table between the status-table markers
python3 tests/harness/report.py readme --check    # exit 1 if README.md is stale (manual and gate step)
```

The table uses, per case, the newest clean-tree result that is not a skip; `features.json` maps
features to case ids. `Verified (VM)` needs every matching case to pass in B or host (`Partial (VM)`: the rest
skipped), `Linux only` the same in A, `Failing (VM)` / `Failing (Linux)` is any fail or error there (shown
before any pass), `Verified (hardware)` comes from a `Hardware-verified: ID[, ID]` line in
`tests/results/hardware/*.md`.

Unit tests: `python3 -m unittest discover -s tests/unit/harness` (also `ctest -R harness`).
