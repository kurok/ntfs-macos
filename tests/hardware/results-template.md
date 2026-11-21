# Hardware run <UTC date> at <short commit>

Copy this file to `tests/results/hardware/<UTC date>-<sha7>.md` and fill it in while you work. Protocol:
`docs/HARDWARE_TEST_PROTOCOL.md` (every case id below is a section there). Do NOT type a line that starts with
`Hardware-verified:` anywhere in this file until you sign it in section 4: `tests/harness/report.py` counts every such
line at the start of a line, in a file named `<UTC date>-<sha7>.md` directly in `tests/results/hardware/`, as a verified feature. Files with another name, and files whose commit is not an ancestor of `HEAD`, are ignored.

## 1. Run

| Item | Value |
|---|---|
| Operator (GitHub handle or initials, not the full name) | |
| Date (UTC) and place | |
| Commit tested (`git rev-parse HEAD`) and tree clean? | |
| Install used (`packaging/dev-install.sh`; `--job-arg --automount-mode=ro` for every case except H-USB-01; `ntfsmac doctor` output kept as) | |
| Mac model, macOS version and build | |
| macFUSE version | |
| NTFS-3G version (`ntfsmac-worker --version`) | |
| Other NTFS drivers on this Mac (Paragon etc.), and how they were switched off or kept out | |
| Windows version and build, edition, machine | |
| Fast Startup state on the Windows machine (`powercfg /a`, registry `HiberbootEnabled`) | |

## 2. Devices (each serial was read from the device itself and compared on both machines)

| Id | Kind | Vendor and model | Serial (Mac form / Windows form) | Size | USB port / hub / cable | Windows `DriveType` | Disposable (operator initials) |
|---|---|---|---|---|---|---|---|
| F | flash drive | | | | | | |
| S | USB SSD or HDD | | | | | | |

## 3. Cases

Result values: PASS, FAIL, NOT DONE (with the reason), INCONCLUSIVE (with the reason). A FAIL or INCONCLUSIVE needs a
finding paragraph under the table. Evidence files are under `tests/results/evidence/hardware-<date>/`.

| Case | Device | Repetitions | Result | Evidence files | Short statement (what was observed) |
|---|---|---|---|---|---|
| H-USB-01 | | | | | |
| H-USB-02 | | | | | |
| H-USB-03 | | | | | |
| H-USB-04 | | | | | |
| H-USB-05 | | | | | |
| H-USB-06 | | | | | |
| H-USB-07 | | | | | |
| H-USB-08 | | | | | |
| H-USB-09 | | | | | |
| H-USB-10 | | | | | |
| H-USB-11 | | | | | |
| H-FND-01 | | | | | |
| H-SYNC-01 | | | | | |
| H-NAME-01 | | | | | |
| H-PERF-01 | | | | | |

Per-case record tables (copy those of the protocol sections here, filled in), then the findings:

### Findings

(one paragraph per FAIL, INCONCLUSIVE and NOT DONE; for each, the question or risk it leaves open:
U12, U15, U19, U22, U23, R-19, R-26, R-30, R-32, R-33, R-36)

## 4. Sign-off

I carried out the steps above myself, with my own hands on the devices named in section 2, on disposable media whose
contents I was willing to destroy. The results are as I observed them. I changed nothing in the results afterwards.

| Item | Value |
|---|---|
| Operator (GitHub handle or initials) | |
| Date (UTC) | |
| Commit tested | |
| Mac / macOS | |
| Windows | |
| Devices (model, serial) | |
| Overall: criterion 5.8 (met = H-USB-01 to 04 PASS on both devices) and T-M5-10 (08 to 11 executed and recorded) | met / not met / partly (list the cases) |
| Signature (the operator types the handle or initials) | |
