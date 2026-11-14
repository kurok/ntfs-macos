# Test harness (T-M2-08)

Python 3.9, standard library only (it also runs under the macOS guest's `/usr/bin/python3`).

| File | Purpose |
|---|---|
| `core.py` | `@case("ID", envs=("A","B"))` decorator, discovery, `Skip`/`Fail`, per-case `Ctx` |
| `runner.py` | runs cases, maps outcomes to `pass`/`fail`/`skip`/`error`, collects evidence |
| `report.py` | result file build, name, write, hand-written validator (`python3 -m harness.report FILE...` from `tests/`) |
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

Unit tests: `python3 -m unittest discover -s tests/unit/harness` (also `ctest -R harness`).
