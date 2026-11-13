# Agent task backlog

Read [ARCHITECTURE.md](ARCHITECTURE.md) sections 1 to 5 before any task. Each task names the further section it needs.

## Conventions

| Field | Meaning |
|---|---|
| Role | B file system engineer, C macOS integration engineer, D testing and reliability engineer, E independent audit (never reviews its own change). A is the coordinator and owns no task here. |
| Model | `S` = Sonnet 5.5 (default). `S+O` = Sonnet implements, then one bounded Opus review of the diff. `O` = Opus does the task. |
| Size | S: under 150 changed lines. M: 150 to 500. L: over 500; the coordinator should split it if the agent stalls. |
| Wave | Tasks with the same wave letter inside a milestone can run in parallel git worktrees. Their "Files" columns do not overlap. A task starts only when its dependencies are merged. |
| Env | A = Linux container, B = macOS VM, H = host (build and unit tests only, never a mount). |
| Done | The acceptance test passes, a result file is written (TEST_STRATEGY.md section 10), and the commit touches only the listed files. A task that needs another file stops and asks the coordinator. |

Hard rules for every task: no mount on the host; no write to a physical device; no `recover`, `remove_hiberfile`, `ntfsfix` without `-n` in automated code paths (only the fixture tools and the explicit repair command may differ, as stated in their tasks); no secrets in the repo.

## M1 Groundwork

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M1-01 | Licence files, SPDX policy, README licence section (ADR-010) | C | S | none | `LICENSES/*`, `COPYING`, `README.md` | Files present; README names GPL-2.0-or-later; macFUSE notice included | S | a | H |
| T-M1-02 | `deps.lock` and checksum-verifying fetch script (plan section 3) | C | S | none | `third_party/deps.lock`, `scripts/fetch-deps.sh`, `tests/unit/fetch/*` | Correct digest passes; altered byte fails with exit 1; NTFS-3G digest re-confirmed against the release page | S | a | H, A |
| T-M1-03 | CMake skeleton, presets (`release`, `asan`, `ubsan`, `linux`), `scripts/bootstrap.sh`, logging helper | C | S | H1 | `CMakeLists.txt`, `CMakePresets.json`, `cmake/*`, `scripts/bootstrap.sh`, `src/common/*`, `include/ntfsmac/log.h` | Configure and build exit 0 on macOS CLT and in the Linux image | M | a | H, A |
| T-M1-04 | Verify VM tool: licence, cost, macOS 27 guest boot, extra disk attach, clone as snapshot; write wrapper (ADR-008, U11) | D | S | H1 | `vm/macos/tart.sh`, `vm/macos/versions.env`, `vm/macos/README.md` | `tart.sh selftest` boots a 27.x guest, attaches a 64 MiB raw image, guest sees a new `/dev/diskN`; licence finding recorded in README | M | a | H |
| T-M1-05 | Linux image that builds pinned NTFS-3G and ntfsprogs with ASan option | D | S | T-M1-02, H2 | `vm/linux/Dockerfile`, `vm/linux/run.sh` | `run.sh selftest`: `mkntfs -F` on a file, mount with `ntfs-3g`, write, unmount, exit 0 | M | b | A |
| T-M1-06 | Audit the six architecture documents: dangling ids, spot-check 15 `[V]` claims against their sources | E | S | none | `docs/audits/M1.md` | Note lists each checked claim with result; zero dangling references or a fix list | S | a | H |

## M2 Prototype (backend gate)

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M2-01 | `build-ntfs3g.sh`: unpack, apply patch series, configure `--with-fuse=external`, static lib, install to `build/deps` | B | S | T-M1-02, T-M1-03 | `scripts/build-ntfs3g.sh`, `third_party/patches/ntfs-3g/README.md` | Builds on Linux (internal FUSE) and prints the exact macOS configure line; idempotent second run | M | a | A |
| T-M2-02 | Golden macOS 27 VM: install macFUSE 5.5.0 after signature check, record whether the extension can be enabled without GUI (U2, U3) | C | S | T-M1-04, H3 | `vm/macos/provision-golden.sh`, `docs/VM_SETUP.md` | In a clone of the golden image, a minimal libfuse3 "hello" file system (libfuse `example/hello.c`) mounts with `-o backend=fskit`; steps reproducible from the doc | M | a | B |
| T-M2-03 | Build NTFS-3G in the VM against macFUSE libfuse; first read/write mount of a fixture as a normal user (U1) | B | S | T-M2-01, T-M2-02, T-M2-07 | `tests/integration/smoke/mount_smoke.sh`, `docs/spikes/M2-backend.md`, `third_party/patches/ntfs-3g/0001-*.patch` (only if needed) | `I-SMOKE-01` passes 3 times; spike note states whether libfuse2 accepted `backend=fskit` with the exact error text if not | M | b | B |
| T-M2-03b | Conditional (only if T-M2-03 shows libfuse2 cannot use FSKit): port `src/ntfs-3g.c` FUSE glue to libfuse3 high-level API | B | S+O | T-M2-03 | `third_party/patches/ntfs-3g/0005-*.patch` | `I-SMOKE-01` passes; Linux reference suite `C-REF-*` still passes with the patched binary on libfuse3; Opus review of every changed callback signature | L | c | A, B |
| T-M2-04 | Mount option matrix on the FSKit backend: each option in ARCHITECTURE.md section 3.3 accepted, ignored or rejected | B | S | T-M2-03 | `docs/spikes/M2-options.md`, `integration/mount-options.json` | Table complete; JSON lists the allow-list actually usable | S | c | B |
| T-M2-05 | Device by inherited fd: does `ntfs-3g /dev/fd/N` work for image-backed and virtio disks (U5)? | B | S | T-M2-03 | `tests/integration/smoke/fd_pass.c`, `tests/integration/smoke/fd_pass.sh`, `docs/spikes/M2-fdpass.md`, `third_party/patches/ntfs-3g/0003-*.patch` (only if needed) | `I-SMOKE-02`: parent opens the device, child without device permission mounts and writes | M | c | B |
| T-M2-06 | Sync trace: run worker with FUSE debug logging; record opcodes for `write`, `close`, `fsync`, `F_FULLFSYNC`, `sync`, unmount; measure delay between `write` return and `FUSE_WRITE` (U4) | B | S | T-M2-03 | `tests/integration/smoke/sync_trace.sh`, `tests/integration/smoke/sync_probe.c`, `docs/spikes/M2-sync.md` | Note contains the opcode table and raw logs; proposes exact wording changes for ARCHITECTURE.md sections 4.4 and 9 | M | c | B |
| T-M2-07 | Fixture generator v1 (clean images: 64 MiB, 256 MiB, 4 KiB and 64 KiB clusters, 512 and 4096 sector) and tree-dump tool | D | S | T-M1-05 | `tests/fixtures/gen/clean.sh`, `tests/fixtures/manifest.json`, `tests/tools/treedump.c`, `tests/tools/CMakeLists.txt` | Images reproducible by seed; `treedump` output equal for two generations | M | a | A |
| T-M2-08 | Test harness (Python stdlib): case discovery, JSON result file, VM transport over SSH; remount-hash test | D | S | T-M1-04 | `tests/harness/*`, `tests/integration/integrity/remount_hash.py` | `I-INT-01` runs in the VM and writes a result file that validates against the schema | M | a | B |
| T-M2-09 | Cross-read check: image written on macOS read on Linux, hashes equal | D | S | T-M2-03, T-M2-08 | `tests/compatibility/cross_read.py` | `C-REF-01` passes | S | c | A, B |
| T-M2-10 | M2 audit: reproduce 2.2 and 2.3 independently from a fresh clone; check spike notes against raw logs | E | S | T-M2-03..09 | `docs/audits/M2.md` | Signed note or defect list | S | d | B |

## M3 Correctness

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M3-00 | Freeze shared contracts: probe JSON schema, policy enum, journal API, worker option builder API | B | S | M2 gate | `include/ntfsmac/probe.h`, `include/ntfsmac/policy.h`, `include/ntfsmac/journal.h`, `include/ntfsmac/options.h`, `docs/schemas/probe.schema.json` | Headers compile; schema validates the example in ARCHITECTURE.md section 3.1 | S | a | H |
| T-M3-01 | Establish NTFS-3G dirty-flag behaviour by source reading and test (U9) | E | S | T-M2-03 | `docs/spikes/M3-dirtyflag.md` | Note cites file and line for each finding; test shows `$Volume` flags before, during and after a read/write session and after `kill -9` | S | b | A |
| T-M3-02 | `ntfsmac-probe` (ARCHITECTURE.md section 3.1) | B | S+O | T-M3-00, T-M3-04 | `src/probe/*`, `tests/unit/probe/*` | `U-PROBE-*` all pass; image SHA-256 unchanged after every run; ASan clean. Opus review of check order and severity mapping | M | c | A, H |
| T-M3-03 | Policy library and option builder (sections 3.2, 3.3; ADR-005) | B | S | T-M3-00 | `src/policy/*`, `src/options/*`, `tests/unit/policy/*` | `U-POL-*` pass, including the test that no input yields `recover` or `remove_hiberfile` | M | b | H, A |
| T-M3-04 | Unsafe fixtures: dirty flag, unclean `$LogFile`, version-2 restart page, three hibernation variants, BitLocker signature, truncated image | D | S+O | T-M2-07 | `tests/fixtures/gen/unsafe.sh`, `tests/tools/setvolflags.c`, `tests/tools/logfile_mark.c`, `tests/tools/hiber_make.sh` | Linux `ntfs-3g -o norecover` refuses or falls back exactly as `volume.c` specifies for each fixture. Opus review of the `$LogFile` edit tool (multi-sector fix-ups) | M | b | A |
| T-M3-05 | Functional operation suite (`I-FUNC-*`, `I-NAME-*`) | D | S | T-M2-08 | `tests/integration/functional/*` | Criteria 3.3 and 3.4 | M | b | B |
| T-M3-06 | Metadata and timestamps (`I-META-*`) | D | S | T-M2-08 | `tests/integration/metadata/*` | Criterion 3.6 | S | b | B |
| T-M3-07 | Read path at and across EOF, truncated files (`I-READ-*`) | D | S | T-M2-08 | `tests/integration/readpath/*` | Criterion 3.8 | S | b | B |
| T-M3-08 | xattr, named streams, Finder-style copy (`I-XATTR-*`) | D | S | T-M2-08 | `tests/integration/xattr/*` | Criterion 3.7; result states which `streams_interface` value is kept | S | b | B |
| T-M3-09 | Large, sparse, compressed-file read/update (`I-BIG-*`) | D | S | T-M2-08 | `tests/integration/special/*` | Criterion 3.5; compressed fixture made on Linux reads back equal | S | b | B |
| T-M3-10 | Patch 0002 (`F_FULLFSYNC`), raw versus buffered node, `sync` option cost, write-path trace (ADR-014, U10) | B | S+O | T-M2-06 | `third_party/patches/ntfs-3g/0002-*.patch`, `docs/spikes/M3-sync.md`, `tests/integration/sync/*` | Trace shows `F_FULLFSYNC` at each sync point; benchmark table for the three variants; Opus review of the patch | M | b | B |
| T-M3-11 | `mmap` read and write tests, including `msync` and unmount with dirty mapped pages | D | S | T-M2-08 | `tests/integration/mmap/*` | Hashes equal after remount; behaviour on worker kill recorded | S | b | B |
| T-M3-12 | On-disk dirty marker as a separate `libntfs-3g` step (ADR-006), only if T-M3-01 shows NTFS-3G does not do it | B | O | T-M3-01, T-M3-13 | `src/volmark/*`, `tests/unit/volmark/*` | Kill at every step leaves the volume either flagged or journalled; Windows-visible flag confirmed with `ntfsinfo -m` | M | d | A |
| T-M3-13 | Session journal library (section 3.5) | B | S | T-M3-00 | `src/journal/*`, `tests/unit/journal/*` | `U-JRN-*` pass with process kill at each step | S | b | H, A |
| T-M3-14 | Patch 0004 fault-injection device layer and crash-consistency sweeper | D | S+O | T-M2-01, T-M2-07 | `third_party/patches/ntfs-3g/0004-*.patch`, `tests/stress/crashsweep/*` | Criterion 3.9; release build proven not to contain the layer (`nm` check). Opus triage of any failing cut point | L | b | A |
| T-M3-15 | Reference comparison: 12 operation scripts, Linux versus macOS tree dumps | D | S | T-M3-05 | `tests/compatibility/ref/*`, `tests/compatibility/expected-differences.md` | Criterion 3.10 | M | c | A, B |
| T-M3-16 | Fuzz harnesses: probe; mount, walk and read; in-memory device ops; seed corpus from tiny fixtures | D | S | T-M3-02 | `tests/fuzz/*` | Criterion 3.11; crash triage file lists each finding with reproducer path | M | d | A |
| T-M3-17 | M3 audit | E | O | all M3 | `docs/audits/M3.md` | Covers probe, policy, option builder, patches, journal and marker ordering; no open data-loss issue or a blocking list | M | e | A, B |

## M4 macOS integration

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M4-00 | Daemon skeleton: volume state machine, internal interfaces, stub modules that compile | C | S | T-M3-00 | `src/daemon/CMakeLists.txt`, `src/daemon/volume.c`, `src/daemon/internal.h`, `tests/unit/daemon/statemachine_*` | State machine unit tests cover every transition in ARCHITECTURE.md section 5 | M | a | H |
| T-M4-01 | Spawn probe as `nobody` and worker as console user with inherited fd; supervise exit (U6) | C | S | T-M4-00, T-M2-05 | `src/daemon/spawn.c`, `tests/integration/daemon/spawn_*` | `I-DMN-09`; worker started from a root process mounts successfully as the user | M | b | B |
| T-M4-02 | Disk Arbitration watcher, NTFS filter, mount-approval dissenter (U7) | C | S | T-M4-00 | `src/daemon/da.c`, `docs/spikes/M4-da.md` | Attaching an image triggers the callback; note states whether the dissenter stops Apple's read-only mount on 27.x and, if not, implements unmount-then-remount | M | b | B |
| T-M4-03 | Unmount and eject flow, mount point lifecycle (section 4.5, U8) | C | S | T-M4-00 | `src/daemon/unmount.c`, `src/daemon/mountpoint.c` | `I-DMN-10..13` | M | b | B |
| T-M4-04 | Control socket with peer authentication, and `ntfsmac` CLI (section 3.4) | C | S | T-M4-00 | `src/daemon/ctl.c`, `src/cli/*`, `tests/unit/ctl/*` | Requests from a non-console, non-admin uid are refused; malformed JSON never crashes the daemon (small fuzz loop) | M | b | H, B |
| T-M4-05 | Restart reconciliation from journal and live mounts | C | S | T-M4-00, T-M3-13 | `src/daemon/reconcile.c` | `I-DMN-15` | S | b | B |
| T-M4-06 | Sandbox profiles for probe and worker | C | S+O | T-M4-01 | `integration/sandbox/probe.sb`, `integration/sandbox/worker.sb`, `tests/integration/daemon/sandbox_*` | Worker cannot open `/etc/passwd`-class paths or sockets, can mount; Opus review of the profiles. If unworkable, record residual risk R-07 | S | c | B |
| T-M4-07 | Conflict detection, passive mode, `doctor` checks (section 8.4) | C | S | T-M4-00 | `src/daemon/conflict.c`, `src/daemon/doctor.c` | `I-DMN-17` | S | b | B |
| T-M4-08 | Wire modules in `main.c`; launchd plist; notifications | C | S | T-M4-01..05, T-M4-07, T-M3-02, T-M3-03 | `src/daemon/main.c`, `integration/launchd/org.ntfs-macos.ntfsmacd.plist`, `src/daemon/notify.c` | `I-DMN-01..08` | M | d | B |
| T-M4-09 | Spike: legacy `/Library/Filesystems` bundle for `diskutil mount` support (ADR-003). Time box: one agent session | C | S | T-M4-08 | `docs/spikes/M4-fsbundle.md` | Recommendation with evidence; no product code | S | e | B |
| T-M4-10 | Developer installer and uninstaller with manifest | C | S | T-M4-08 | `packaging/dev-install.sh`, `packaging/uninstall.sh`, `packaging/manifest.txt` | `I-PKG-01..02` | S | e | B |
| T-M4-11 | Daemon integration tests and Finder tests | D | S | T-M4-08 | `tests/integration/daemon/dmn_*`, `tests/integration/finder/*` | Criteria 4.1 to 4.5, 4.7 | M | e | B |
| T-M4-12 | M4 audit: privilege boundaries, option allow-list, socket authentication | E | S+O | all M4 | `docs/audits/M4.md` | No path from client input to a root-executed string or banned option | M | f | B |

## M5 Validation

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M5-01 | One-command suite per environment, result aggregation, README status table generator | D | S | M3, M4 | `scripts/test-all.sh`, `tests/harness/report.py`, `vm/macos/run-suite.sh` | Criterion 5.1 | M | a | A, B |
| T-M5-02 | Soak and parallel workloads | D | S | T-M5-01 | `tests/stress/soak/*`, `tests/stress/parallel/*` | Criteria 5.2, 5.3 | M | b | B |
| T-M5-03 | Leak detection run | D | S | T-M5-01 | `tests/stress/leaks/*` | Criterion 5.4 | S | b | B |
| T-M5-04 | Disconnect simulation (force-detach image during write) | D | S | T-M5-01 | `tests/stress/disconnect/*` | Next mount read-only; image passes read-only checks; pre-synced files intact | S | b | B |
| T-M5-05 | Kill tests: worker, daemon, macFUSE module process | D | S | T-M5-01 | `tests/stress/kill/*` | Criterion 4.4 repeated 20 times each without hang | S | b | B |
| T-M5-06 | Low-space tests | D | S | T-M5-01 | `tests/stress/enospc/*` | Criterion 5.5 | S | b | B |
| T-M5-07 | Benchmarks and Linux reference numbers | D | S | T-M5-01 | `tests/perf/*`, `docs/PERFORMANCE.md` | Criterion 5.6 | M | b | A, B |
| T-M5-08 | Local CI runner `scripts/ci-local.sh` with `--quick` and full modes (TEST_STRATEGY.md section 9; no GitHub workflows, ADR-015) | D | S | T-M5-01 | `scripts/ci-local.sh` | Criterion 5.7 | M | b | CI |
| T-M5-09 | Extended fuzz campaign (8 CPU-hours) and triage; upstream reports drafted, not sent without human approval | D | S+O | T-M3-16 | `tests/fuzz/findings/*`, `docs/SECURITY_FINDINGS.md` | Every crash classified: project code or engine, exploitable or not | M | b | A |
| T-M5-10 | Hardware protocol document and execution with the human | D | S | H5, H6, H7 | `docs/HARDWARE_TEST_PROTOCOL.md`, `tests/results/hardware/*` | Criterion 5.8 | M | c | hardware |
| T-M5-11 | M5 audit: look for tests that cannot fail, skipped cases, results not tied to a commit | E | O | all M5 | `docs/audits/M5.md` | Signed note or defect list | M | d | all |

## M6 Packaging

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M6-01 | Release package build with preflight for macFUSE version and conflicts | C | S | T-M4-10 | `packaging/build-pkg.sh`, `packaging/scripts/preinstall`, `packaging/scripts/postinstall`, `packaging/distribution.xml` | Criteria 6.1, 6.3 | M | a | B |
| T-M6-02 | Signing and notarization script driven by a keychain profile name | C | S | T-M6-01, H8 | `packaging/sign-and-notarize.sh` | Criterion 6.4, or documented unsigned path | S | b | H |
| T-M6-03 | `INSTALL.md`, `LIMITATIONS.md`, uninstall instructions | C | S | T-M6-01 | `docs/INSTALL.md`, `docs/LIMITATIONS.md` | Criterion 6.6 | S | a | none |
| T-M6-04 | Reproducibility check | D | S | T-M6-01 | `scripts/repro-check.sh` | Criterion 6.2 | S | b | B |
| T-M6-05 | GPL source release archive | C | S | T-M6-01 | `scripts/make-source-release.sh` | Criterion 6.5 | S | b | H |

## M7 Release readiness

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M7-01 | Release audit of data path, probe, policy, privilege code since M3 | E | O | M6 | `docs/audits/M7.md` | Criterion 7.1 | M | a | all |
| T-M7-02 | Fix blocking findings (one task per finding, created by the coordinator) | B or C | S, O if data-loss | T-M7-01 | per finding | Finding's reproducer passes; E re-verifies | varies | b | all |
| T-M7-03 | Full suite on the release commit and `RELEASE_READINESS.md` | D | S | T-M7-02 | `docs/RELEASE_READINESS.md`, `tests/results/release/*` | Criteria 7.3, 7.4 | S | c | A, B |

## Opus budget summary

| Use | Tasks |
|---|---|
| Review after Sonnet implementation | T-M2-03b (conditional), T-M3-02, T-M3-04, T-M3-10, T-M3-14, T-M4-06, T-M4-12, T-M5-09 |
| Opus does the task | T-M3-12 (conditional), T-M3-17, T-M5-11, T-M7-01 |
| Escalation only | Any task after three failed Sonnet attempts; any reproducible hash mismatch |

## File ownership map (to prevent overlapping edits)

| Area | Owner tasks |
|---|---|
| `include/ntfsmac/*.h` | T-M3-00 only; later changes are a coordinator-scheduled single task |
| `third_party/patches/ntfs-3g/` | One patch file per task (0001: T-M2-03, 0002: T-M3-10, 0003: T-M2-05, 0004: T-M3-14, 0005: T-M2-03b) |
| `src/daemon/` | One source file per task as listed; `main.c` only T-M4-08 |
| `tests/harness/` | T-M2-08, then T-M5-01 |
| `tests/integration/<suite>/` | One directory per task |
| `docs/ARCHITECTURE.md` | Corrections from spikes are applied by the coordinator in a single commit per milestone, using the wording proposed in the spike notes |
