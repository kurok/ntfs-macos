# Agent task backlog

Read [ARCHITECTURE.md](ARCHITECTURE.md) sections 1 to 5 before any task. Each task names the further section it needs.

## Conventions

| Field | Meaning |
|---|---|
| Role | B file system engineer, C macOS integration engineer, D testing and reliability engineer, E independent audit (never reviews its own change). A is the coordinator and owns no task here. |
| Model | `S` = Sonnet 5.5 (default). `S+O` = Sonnet implements, then one bounded Opus review of the diff. `O` = Opus does the task. |
| Size | S: under 150 changed lines. M: 150 to 500. L: over 500; the coordinator should split it if the agent stalls. |
| Wave | Tasks with the same wave letter inside a milestone can run in parallel git worktrees. Their "Files" columns do not overlap. A task starts only when its dependencies are merged. |
| Env | A = Linux container, B = macOS VM, H = host (build and unit tests only, never a mount). `hardware` = Environment C with the human present (TEST_STRATEGY.md section 2). `all` = every environment that the task's own tests name (A, B, and H for builds). `none` = nothing is executed (documents only). There is no hosted CI, so no value for it exists (ADR-015). |
| Done | The acceptance test passes, a result file is written (TEST_STRATEGY.md section 10), and the commit touches only the listed files. A task that needs another file stops and asks the coordinator. |

Hard rules for every task: no mount on the host; no write to a physical device; no `recover`, `remove_hiberfile`, `ntfsfix` without `-n` in automated code paths (only the fixture tools may differ, as stated in their tasks; the explicit repair command does not run `ntfsfix`, ADR-017); no disk-derived string used before the sanitiser (ADR-016); no secrets in the repo.

## M1 Groundwork

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M1-01 | Licence files, SPDX policy, README licence section (ADR-010) | C | S | none | `LICENSES/*`, `COPYING`, `README.md` | Files present; README names GPL-2.0-or-later; macFUSE notice included | S | a | H |
| T-M1-02 | `deps.lock` and checksum-verifying fetch script (plan section 3) | C | S | none | `third_party/deps.lock`, `scripts/fetch-deps.sh`, `tests/unit/fetch/*` | Correct digest passes; altered byte fails with exit 1; NTFS-3G digest re-confirmed against the release page | S | a | H, A |
| T-M1-03 | CMake skeleton, presets (`release`, `asan`, `ubsan`, `linux`), `scripts/bootstrap.sh`, logging helper | C | S | H1 | `CMakeLists.txt`, `CMakePresets.json`, `cmake/*`, `scripts/bootstrap.sh`, `src/common/*`, `include/ntfsmac/log.h` | Configure and build exit 0 on macOS CLT and in the Linux image | M | a | H, A |
| T-M1-04 | Verify VM tool: licence, cost, macOS 27 guest boot, extra disk attach, clone as snapshot; write wrapper (ADR-008, U11) | D | S | H1 | `vm/macos/tart.sh`, `vm/macos/versions.env`, `vm/macos/README.md` | `tart.sh selftest` boots a 27.x guest, attaches a 64 MiB raw image, guest sees a new `/dev/diskN`; licence finding recorded in README | M | a | H |
| T-M1-05 | Linux image that builds pinned NTFS-3G and ntfsprogs with ASan option | D | S | T-M1-02, H2 | `vm/linux/Dockerfile`, `vm/linux/run.sh` | `run.sh selftest`: `mkntfs -F` on a file, mount with `ntfs-3g`, write, unmount, exit 0 | M | b | A |
| T-M1-06 | Audit the six architecture documents: dangling ids, spot-check 15 `[V]` claims against their sources | E | S | none | `docs/audits/M1.md` | Note lists each checked claim with result; zero dangling references or a fix list | S | a | H |
| T-M1-07 | Minimal local CI runner `scripts/ci-local.sh --quick`: bootstrap check, configure, build, `ctest`; non-zero exit on any failure; stages are added only by T-M5-08 (ADR-015, TEST_STRATEGY.md section 9) | D | S | T-M1-03 | `scripts/ci-local.sh`, `tests/unit/ci/*` | Criterion 1.7 | S | b | H, A |

## M2 Prototype (backend gate)

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M2-01 | `build-ntfs3g.sh`: unpack, apply patch series, configure `--with-fuse=external`, static lib, install to `build/deps` | B | S | T-M1-02, T-M1-03 | `scripts/build-ntfs3g.sh`, `third_party/patches/ntfs-3g/README.md` | Builds on Linux (internal FUSE) and prints the exact macOS configure line; idempotent second run | M | a | A |
| T-M2-02 | Golden macOS 27 VM: install macFUSE 5.5.0 after signature check, record whether the extension can be enabled without GUI (U2, U3) | C | S | T-M1-04, H3 | `vm/macos/provision-golden.sh`, `docs/VM_SETUP.md` | In a clone of the golden image, a minimal libfuse3 "hello" file system (libfuse `example/hello.c`) mounts with `-o backend=fskit`; steps reproducible from the doc | M | a | B |
| T-M2-03 | Build NTFS-3G in the VM against macFUSE libfuse; first read/write mount of a fixture as a normal user (U1) | B | S | T-M2-01, T-M2-02, T-M2-07 | `tests/integration/smoke/mount_smoke.sh`, `docs/spikes/M2-backend.md`, `third_party/patches/ntfs-3g/0001-*.patch` (only if needed) | `I-SMOKE-01` passes 3 times; spike note states whether libfuse2 accepted `backend=fskit` with the exact error text if not | M | b | B |
| T-M2-03b | Conditional (only if T-M2-03 shows libfuse2 cannot use FSKit): port `src/ntfs-3g.c` FUSE glue to libfuse3 high-level API | B | S+O | T-M2-03 | `third_party/patches/ntfs-3g/0005-*.patch` | `I-SMOKE-01` passes; Linux reference suite `C-REF-*` still passes with the patched binary on libfuse3; Opus review of every changed callback signature | L | c | A, B |
| T-M2-04 | Mount option matrix on the FSKit backend: each option in ARCHITECTURE.md section 3.3 accepted, ignored or rejected | B | S | T-M2-03 | `docs/spikes/M2-options.md`, `integration/mount-options.json` | Table complete; JSON lists the allow-list actually usable | S | c | B |
| T-M2-05 | Device by inherited fd: does `ntfs-3g /dev/fd/N` work for image-backed and virtio disks (U5)? | B | S+O | T-M2-03 | `tests/integration/smoke/fd_pass.c`, `tests/integration/smoke/fd_pass.sh`, `docs/spikes/M2-fdpass.md`, `third_party/patches/ntfs-3g/0003-*.patch` (only if needed) | `I-SMOKE-02`: parent opens the device, child without device permission mounts and writes, with both an `O_RDONLY` and an `O_RDWR` descriptor; Opus review of patch 0003 if it exists (it touches device access) | M | c | B |
| T-M2-06 | Sync trace: run worker with FUSE debug logging; record opcodes for `write`, `close`, `fsync`, `F_FULLFSYNC`, `sync`, unmount; measure delay between `write` return and `FUSE_WRITE` (U4) | B | S | T-M2-03 | `tests/integration/smoke/sync_trace.sh`, `tests/integration/smoke/sync_probe.c`, `docs/spikes/M2-sync.md` | Note contains the opcode table and raw logs; proposes exact wording changes for ARCHITECTURE.md sections 4.4 and 9 | M | c | B |
| T-M2-07 | Fixture generator v1 (clean images: 64 MiB, 256 MiB, 4 KiB and 64 KiB clusters, 512 and 4096 sector) and tree-dump tool | D | S | T-M1-05 | `tests/fixtures/gen/clean.sh`, `tests/fixtures/manifest.json`, `tests/tools/treedump.c`, `tests/tools/CMakeLists.txt` | Images reproducible by seed; `treedump` output equal for two generations | M | a | A |
| T-M2-08 | Test harness (Python stdlib): case discovery, JSON result file, VM transport over SSH; remount-hash test | D | S | T-M1-04 | `tests/harness/*`, `tests/integration/integrity/remount_hash.py` | `I-INT-01` runs in the VM and writes a result file that validates against the schema | M | a | B |
| T-M2-09 | Cross-read check: image written on macOS read on Linux, hashes equal | D | S | T-M2-03, T-M2-08 | `tests/compatibility/cross_read.py` | `C-REF-01` passes | S | c | A, B |
| T-M2-10 | M2 audit: reproduce 2.2 and 2.3 independently from a fresh clone; check spike notes against raw logs | E | S | T-M2-03..09 | `docs/audits/M2.md` | Signed note or defect list | S | d | B |

## M3 Correctness

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M3-00 | Freeze shared contracts: probe JSON schema, policy enum, journal API, worker option builder API | B | S | M2 gate | `include/ntfsmac/probe.h`, `include/ntfsmac/policy.h`, `include/ntfsmac/journal.h`, `include/ntfsmac/options.h`, `docs/schemas/probe.schema.json` | Headers compile; schema validates the example in ARCHITECTURE.md section 3.1 and encodes the per-field grammar of that section (every key required, patterns for `serial` and `logfile_lsn`, integer ranges, `volume_flags` and `reason_codes` enums); `probe.h` holds the reason-code list | S | a | H |
| T-M3-01 | Confirm by test the source finding that NTFS-3G never sets `VOLUME_IS_DIRTY` (U9), and that a `norecover` read/write session leaves the `$LogFile` restart area unchanged (U13) | E | S | T-M2-03 | `docs/spikes/M3-dirtyflag.md` | Note cites file and line for each finding; test shows `$Volume` flags and the restart-area bytes (incl. `current_lsn`) before, during and after a read/write session and after `kill -9` | S | b | A |
| T-M3-02 | `ntfsmac-probe` (ARCHITECTURE.md section 3.1) | B | S+O | T-M3-00, T-M3-04 | `src/probe/*`, `tests/unit/probe/*` | `U-PROBE-*` all pass, including `hiber-case` reported `hibernated` and `hiber-subdir` reported `clean` (U19); image SHA-256 unchanged after every run; ASan clean. Opus review of check order and severity mapping | M | c | A, H |
| T-M3-03 | Policy library and option builder (sections 3.2, 3.3; ADR-005, ADR-017) | B | S | T-M3-00 | `src/policy/*`, `src/options/*`, `tests/unit/policy/*` | `U-POL-*` pass, including the test that no input yields `recover` or `remove_hiberfile`, every row with each journal state, the evidence rule (on and off), and the `journal-clear` and `repair` gates | M | b | H, A |
| T-M3-04 | Unsafe fixtures: dirty flag, unclean `$LogFile`, version-2 restart page, four hibernation variants (`hibr`, `HIBR`, short file, `wake`), two root-name variants (`hiber-case`, `hiber-subdir`), BitLocker signature, truncated image, informational-flag fixtures (`chkdsk-flag`, `dirty-chkdsk-flag`), non-resident root `$TXF_DATA` (`txf-nonresident`); `logfile_mark --bump-lsn` (advances `current_lsn` in both restart areas, keeps the log clean) to simulate a Windows check | D | S+O | T-M2-07 | `tests/fixtures/gen/unsafe.sh`, `tests/tools/CMakeLists.txt` (appends its three tools only), `tests/tools/setvolflags.c`, `tests/tools/logfile_mark.c`, `tests/tools/txf_make.c`, `tests/tools/hiber_make.sh` | Linux `ntfs-3g -o norecover` refuses or falls back exactly as `volume.c` specifies for each fixture; after `--bump-lsn` the image still mounts read/write with `norecover` and the probe reports the new LSN. Opus review of the `$LogFile` edit tool (multi-sector fix-ups) | M | b | A |
| T-M3-05 | Functional operation suite (`I-FUNC-*`, `I-NAME-*`) | D | S | T-M2-08 | `tests/integration/functional/*` | Criteria 3.3 and 3.4 | M | b | B |
| T-M3-06 | Metadata and timestamps (`I-META-*`) | D | S | T-M2-08 | `tests/integration/metadata/*` | Criterion 3.6 | S | b | B |
| T-M3-07 | Read path at and across EOF, truncated files (`I-READ-*`) | D | S | T-M2-08 | `tests/integration/readpath/*` | Criterion 3.8 | S | b | B |
| T-M3-08 | xattr, named streams, Finder-style copy (`I-XATTR-*`) | D | S | T-M2-08 | `tests/integration/xattr/*` | Criterion 3.7; result states which `streams_interface` value is kept | S | b | B |
| T-M3-09 | Large, sparse, compressed-file read/update (`I-BIG-*`) | D | S | T-M2-08 | `tests/integration/special/*` | Criterion 3.5; compressed fixture made on Linux reads back equal | S | b | B |
| T-M3-10 | Sync path without a patch (ADR-014; patch 0002 withdrawn): trace upstream `ntfs_fsync()` at each sync point, record whether its `fsync` fallback is taken on the VM disk, raw versus buffered node, `sync` option cost (U10) | B | S | T-M2-06 | `docs/spikes/M3-sync.md`, `tests/integration/sync/*` | `I-SYNC-01..02`: trace shows `F_FULLFSYNC` at each sync point and states for each whether the fallback ran; benchmark table for the three variants | M | b | B |
| T-M3-11 | `mmap` read and write tests, including `msync` and unmount with dirty mapped pages | D | S | T-M2-08 | `tests/integration/mmap/*` | Hashes equal after remount; behaviour on worker kill recorded | S | b | B |
| T-M3-12 | `ntfsmac-volmark`: on-disk dirty marker `set`, `clear`, `clear-repair` (ADR-006 default plan, ARCHITECTURE.md section 3.6). When it passes, the coordinator enables the marker step and changes D3 from host-only | B | O | T-M3-01, T-M3-04, T-M3-10, T-M3-13, T-M3-14 | `src/volmark/*`, `tests/unit/volmark/*` | `U-VMK-01..10` pass; the `FORENSIC` mount writes nothing before step 4 (`count` plan shows zero writes; `txf-nonresident` unchanged by `set`); cut at every write gives exactly the outcomes of the crash table in ARCHITECTURE.md section 3.6; flag confirmed with `ntfsinfo -m`; no bit other than `VOLUME_IS_DIRTY` changes and no byte outside the two copies of record 3 changes; generates the `vmk-split` image for U15 | M | d | A |
| T-M3-13 | Session journal library (section 3.5): entry fields, `marker`/`state` transitions, four removal paths, tombstones | B | S | T-M3-00 | `src/journal/*`, `tests/unit/journal/*` | `U-JRN-*` pass with process kill at each step, including the naming rule: file names only from the 64-bit serial, created with `openat` under an `O_NOFOLLOW` directory descriptor (`U-JRN-NAME-01`) | S | b | H, A |
| T-M3-14 | Patch 0004 fault-injection device layer and crash-consistency sweeper | D | S+O | T-M2-01, T-M2-07 | `third_party/patches/ntfs-3g/0004-*.patch`, `tests/stress/crashsweep/*` | Criterion 3.9; release build proven not to contain the layer (`nm` check). Opus triage of any failing cut point | L | b | A |
| T-M3-15 | Reference comparison: 12 operation scripts, Linux versus macOS tree dumps | D | S | T-M3-05 | `tests/compatibility/ref/*`, `tests/compatibility/expected-differences.md` | Criterion 3.10 | M | c | A, B |
| T-M3-16 | Fuzz harnesses: probe; mount, walk and read; in-memory device ops; seed corpus from tiny fixtures | D | S | T-M3-02 | `tests/fuzz/*` | Criterion 3.11; crash triage file lists each finding with reproducer path | M | d | A |
| T-M3-18 | Patch 0006: session channel on `NTFSMAC_ERRFD` (ADR-018): `S`, `E`, `W`, `U` records at the hooks named in ARCHITECTURE.md section 7, `FD_CLOEXEC` and non-blocking writes | B | S+O | T-M3-10, T-M3-14 | `third_party/patches/ntfs-3g/0006-*.patch`, `tests/integration/errfd/*` | `I-ERR-01..07` pass; Linux reference suite still passes with the patched binary; Opus review of the patch against `src/ntfs-3g.c` `main`/`ntfs_close` and `unix_io.c` | S | c | A, B |
| T-M3-19 | Disk-string hardening: strict probe-verdict parser, label sanitiser, `argv_check()` (ADR-016); hostile-label fixtures | B | S+O | T-M3-00, T-M3-03 | `src/sanitize/*`, `tests/unit/sanitize/*`, `tests/fuzz/sanitize/*`, `tests/fixtures/gen/labels.sh` | `U-LABEL-*`, `U-ARGV-*`, `U-VERDICT-01..05` pass (per-field grammar, hostile `serial`, reason codes only); `F-SANITIZE-*` 1 CPU-hour each without finding; Opus review of the allow-list, the verdict grammar and of `argv_check()` against `ntfs-3g`'s option parser | M | c | H, A |
| T-M3-20 | Ownership and permission tests (`I-PERM-*`, ADR-012): uid/gid, modes, `chmod`/`chown` outcomes, execute bit (N9), opt-in mapping smoke | D | S | T-M2-08 | `tests/integration/permissions/*` | Criterion 3.16 | S | b | B, A |
| T-M3-17 | M3 audit | E | O | all M3 | `docs/audits/M3.md` | Covers probe, policy, option builder, sanitiser and argv validator, patches (incl. 0006), journal and volmark ordering; no open data-loss issue or a blocking list | M | e | A, B |

## M4 macOS integration

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M4-00 | Daemon skeleton: volume state machine, internal interfaces, stub modules that compile; `src/daemon/CMakeLists.txt` collects `*.c` with `file(GLOB ... CONFIGURE_DEPENDS)` so later tasks add a source file without editing it | C | S | T-M3-00 | `src/daemon/CMakeLists.txt`, `src/daemon/volume.c`, `src/daemon/internal.h`, `tests/unit/daemon/statemachine_*` | State machine unit tests cover every transition in ARCHITECTURE.md section 5 | M | a | H |
| T-M4-01 | Spawn probe as `nobody` and worker as console user with inherited fd; supervise exit (U6) | C | S | T-M4-00, T-M2-05 | `src/daemon/spawn.c`, `tests/integration/daemon/spawn_*` | `I-DMN-09`, `I-DMN-26`; worker started from a root process mounts successfully as the user; U20 answered | M | b | B |
| T-M4-02 | Disk Arbitration watcher, NTFS filter, mount-approval dissenter (U7); "no other mount" check for the binding of ARCHITECTURE.md section 4.1 step 6 (U18, ADR-020) | C | S | T-M4-00 | `src/daemon/da.c`, `docs/spikes/M4-da.md` | Attaching an image triggers the callback; note states whether the dissenter stops Apple's read-only mount on 27.x and, if not, implements unmount-then-remount; note records whether `DAVolumePath` appears for an Apple-driver mount and whether `O_RDWR` then fails (U18) | M | b | B |
| T-M4-03 | Unmount and eject flow, mount point lifecycle with sanitised leaf, `mkdirat` and collision suffixes (sections 4.1 step 7, 4.5; ADR-016; U8) | C | S | T-M4-00, T-M3-19 | `src/daemon/unmount.c`, `src/daemon/mountpoint.c` | `I-DMN-10..13`; `I-DMN-18` | M | b | B |
| T-M4-04 | Control socket with peer authentication, and `ntfsmac` CLI (section 3.4); `journal_clear` and `repair` registered as stubs for T-M4-14 behind the five checks of ADR-019; CLI text tables for `reason_codes` | C | S+O | T-M4-00 | `src/daemon/ctl.c`, `src/cli/*`, `tests/unit/ctl/*` | Requests from a non-console, non-admin uid are refused; `journal_clear`/`repair` are refused from a non-`admin` user, from a process in a spawned process group or its descendant, from an unsigned or differently signed client, and without a fresh authorization; U17 answered in the result note; malformed JSON never crashes the daemon (small fuzz loop); Opus review of the peer checks | M | b | H, B |
| T-M4-05 | Restart reconciliation from journal and live mounts | C | S | T-M4-00, T-M3-13 | `src/daemon/reconcile.c` | `I-DMN-15` | S | b | B |
| T-M4-06 | Sandbox profiles for probe, volmark and worker | C | S+O | T-M4-01 | `integration/sandbox/probe.sb`, `integration/sandbox/volmark.sb`, `integration/sandbox/worker.sb`, `tests/integration/daemon/sandbox_*` | Worker cannot open `/etc/passwd`-class paths or sockets, can read only the fixed `UserMapping` path when `usermapping=` is set, cannot `connect()` to `/var/run/ntfsmacd.sock`, can mount; volmark and probe cannot open any path or socket; Opus review of the profiles. If unworkable, record residual risk R-07 | S | c | B |
| T-M4-07 | Conflict detection, passive mode, `doctor` checks (section 8.4) | C | S | T-M4-00 | `src/daemon/conflict.c`, `src/daemon/doctor.c` | `I-DMN-17` | S | b | B |
| T-M4-08 | Wire modules in `main.c`; launchd plist; notifications (including the volmark-interrupted text of ARCHITECTURE.md section 3.6 and the `W`-record wording of section 4.5) | C | S | T-M4-01..05, T-M4-07, T-M4-13, T-M4-14, T-M3-02, T-M3-03 | `src/daemon/main.c`, `integration/launchd/org.ntfs-macos.ntfsmacd.plist`, `src/daemon/notify.c` | Manual attach in the VM of `clean-64m-4k` (mounts `rw`) and `dirty-flag` (mounts `ro`), recorded in the result file; the full `I-DMN-01..08` are written and run by T-M4-11 | M | e | B |
| T-M4-09 | Spike: legacy `/Library/Filesystems` bundle for `diskutil mount` support (ADR-003). Time box: one agent session | C | S | T-M4-08 | `docs/spikes/M4-fsbundle.md` | Recommendation with evidence; no product code | S | f | B |
| T-M4-10 | Developer installer and uninstaller with manifest | C | S | T-M4-08 | `packaging/dev-install.sh`, `packaging/uninstall.sh`, `packaging/manifest.txt` | `I-PKG-01..02`; installs and removes the authorization right `org.ntfs-macos.journal-modify` and records the CLI code requirement (ADR-019) | S | f | B |
| T-M4-11 | Daemon integration tests and Finder tests: `I-DMN-01..08`, `I-DMN-14`, `I-DMN-16`, the second-line test with a probe stub (`I-DMN-23`), and a run of the whole `I-DMN-*` range (TEST_STRATEGY.md section 4) | D | S | T-M4-08 | `tests/integration/daemon/dmn_*`, `tests/integration/finder/*` | Criteria 4.1 to 4.5, 4.7, 4.10; records whether Apple's `ntfs.fs` mounts the `vmk-split` image (U15) | M | f | B |
| T-M4-13 | Session-channel watcher: pipe with `O_CLOEXEC`, spawn with `POSIX_SPAWN_CLOEXEC_DEFAULT`, record grammar and bounds, positive clean-session proof (`S rw` ... `U 0`, no `E`, EOF, no re-adoption), `W` handling (ARCHITECTURE.md section 7, ADR-018) | C | S | T-M4-01, T-M3-13, T-M3-18 | `src/daemon/errwatch.c`, `tests/integration/daemon/errwatch_*` | `I-DMN-24`; unit tests of the parser with over-long lines, endless stream, 65 records, bytes outside `0x20..0x7E`; U16 answered from a live worker's process tree and descriptor table | S | c | B |
| T-M4-14 | Session lifecycle: journal entry, device binding (ADR-020), `volmark set`/`clear` ordering with the `w`/`d` timeout rule, `ro` on volmark failure, `automount_mode=ro`, journal evidence rule, `journal_clear` and `repair` requests and CLI commands (sections 3.4 to 3.6, 4.1 step 6; ADR-017, ADR-019) | C | S+O | T-M4-01, T-M4-04, T-M4-13, T-M3-03, T-M3-12, T-M3-13 | `src/daemon/session.c`, `src/daemon/ctl_journal.c`, `src/cli/cmd_journal.c`, `src/cli/cmd_repair.c`, `tests/integration/daemon/journal_*` | `I-DMN-19..22`, `I-DMN-25`; Opus review of the ordering against ARCHITECTURE.md section 3.6 and of the binding | M | d | B |
| T-M4-12 | M4 audit: privilege boundaries, option allow-list, socket authentication, disk-derived strings | E | S+O | all M4 | `docs/audits/M4.md` | No path from client or disk input to a root-executed string, a path outside `/Volumes` or the journal directory, or a banned option; `journal_clear`/`repair` unreachable without the ADR-019 checks; probe strings never reach a file name or a terminal | M | g | B |
| T-M4-15 | ThreadSanitizer run of the daemon logic: concurrent drivers for the state machine, control handler, journal and session (TEST_STRATEGY.md section 4.1); needs a `tsan` preset from the coordinator's wiring commit | D | S | T-M4-00, T-M4-04, T-M3-13 | `tests/unit/tsan/*` | Criterion 4.11 | S | c | H, A |

## M5 Validation

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M5-01 | One-command suite per environment, result aggregation, README status table generator | D | S | M3, M4 | `scripts/test-all.sh`, `tests/harness/report.py`, `vm/macos/run-suite.sh` | Criterion 5.1 | M | a | A, B |
| T-M5-02 | Soak and parallel workloads | D | S | T-M5-01 | `tests/stress/soak/*`, `tests/stress/parallel/*` | Criteria 5.2, 5.3 | M | b | B |
| T-M5-03 | Leak detection run | D | S | T-M5-01 | `tests/stress/leaks/*` | Criterion 5.4 | S | b | B |
| T-M5-04 | Disconnect simulation (force-detach image during write) | D | S | T-M5-01 | `tests/stress/disconnect/*` | Next mount read-only; image carries `VOLUME_IS_DIRTY` (once T-M3-12 has passed); image passes read-only checks; pre-synced files intact | S | b | B |
| T-M5-05 | Kill tests: worker, daemon, macFUSE module process | D | S | T-M5-01 | `tests/stress/kill/*` | Criterion 4.4 repeated 20 times each without hang | S | b | B |
| T-M5-06 | Low-space tests | D | S | T-M5-01 | `tests/stress/enospc/*` | Criterion 5.5 | S | b | B |
| T-M5-07 | Benchmarks and Linux reference numbers | D | S | T-M5-01 | `tests/perf/*`, `docs/PERFORMANCE.md` | Criterion 5.6 | M | b | A, B |
| T-M5-08 | Complete the local CI runner `scripts/ci-local.sh` started in T-M1-07: full mode with every stage of TEST_STRATEGY.md section 9, and `--verify-result <file>` implementing the "result for the exact commit" rule of section 10 (no GitHub workflows, ADR-015) | D | S | T-M5-01, T-M1-07 | `scripts/ci-local.sh` | Criterion 5.7 | M | b | H, A, B |
| T-M5-09 | Extended fuzz campaign (8 CPU-hours) and triage; upstream reports drafted, not sent without human approval | D | S+O | T-M3-16 | `tests/fuzz/findings/*`, `docs/SECURITY_FINDINGS.md` | Every crash classified: project code or engine, exploitable or not | M | b | A |
| T-M5-10 | Hardware protocol document and execution with the human, including `H-USB-08` (Windows `chkdsk` after an unclean session: records `current_lsn` and all flags before and after), `H-USB-09` (plain Windows mount without `chkdsk` must leave our flag set), together resolving U12, and `H-USB-10` (`chkdsk` on the `vmk-split` image, U15) | D | S | H5, H6, H7 | `docs/HARDWARE_TEST_PROTOCOL.md`, `tests/results/hardware/*` | Criterion 5.8 | M | c | hardware |
| T-M5-11 | M5 audit: look for tests that cannot fail, skipped cases, results not tied to a commit | E | O | all M5 | `docs/audits/M5.md` | Signed note or defect list | M | d | all |

## M6 Packaging

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M6-01 | Release package build with preflight for macFUSE version and conflicts | C | S | T-M4-10 | `packaging/build-pkg.sh`, `packaging/scripts/preinstall`, `packaging/scripts/postinstall`, `packaging/distribution.xml` | Criteria 6.1, 6.3 | M | a | B |
| T-M6-02 | Signing and notarization script driven by a keychain profile name | C | S | T-M6-01, H8 | `packaging/sign-and-notarize.sh` | Criterion 6.4, or documented unsigned path | S | b | H |
| T-M6-03 | `INSTALL.md`, `LIMITATIONS.md`, uninstall instructions | C | S | T-M6-01 | `docs/INSTALL.md`, `docs/LIMITATIONS.md` | Criterion 6.6 | S | b | none |
| T-M6-04 | Reproducibility check | D | S | T-M6-01 | `scripts/repro-check.sh` | Criterion 6.2 | S | b | B |
| T-M6-05 | GPL source release archive | C | S | T-M6-01 | `scripts/make-source-release.sh` | Criterion 6.5 | S | b | H |
| T-M6-06 | M6 audit: root-run `preinstall`/`postinstall` scripts, package contents, uninstall manifest; no path from outside input to a root-executed string (Opus only if a privilege finding needs it) | E | S | T-M6-01..05 | `docs/audits/M6.md` | Criterion 6.7; signed note or defect list | S | c | B |

## M7 Release readiness

| ID | Title | Role | Model | Depends | Files | Acceptance test | Size | Wave | Env |
|---|---|---|---|---|---|---|---|---|---|
| T-M7-01 | Release audit of data path, probe, policy, privilege code since M3 | E | O | M6 | `docs/audits/M7.md` | Criterion 7.1 | M | a | all |
| T-M7-02 | Fix blocking findings (one task per finding, created by the coordinator) | B or C | S, O if data-loss | T-M7-01 | per finding | Finding's reproducer passes; E re-verifies | varies | b | all |
| T-M7-03 | Full suite on the release commit and `RELEASE_READINESS.md` | D | S | T-M7-02 | `docs/RELEASE_READINESS.md`, `tests/results/release/*` | Criteria 7.3, 7.4 | S | c | A, B |

## Opus budget summary

| Use | Tasks |
|---|---|
| Review after Sonnet implementation | T-M2-03b (conditional), T-M2-05 (conditional, patch 0003), T-M3-02, T-M3-04, T-M3-14, T-M3-18, T-M3-19, T-M4-04, T-M4-06, T-M4-12, T-M4-14, T-M5-09 |
| Opus does the task | T-M3-12, T-M3-17, T-M5-11, T-M7-01 |
| Escalation only | Any task after three failed Sonnet attempts; any reproducible hash mismatch |

## File ownership map (to prevent overlapping edits)

| Area | Owner tasks |
|---|---|
| `include/ntfsmac/*.h` | T-M3-00 only; later changes are a coordinator-scheduled single task |
| `third_party/patches/ntfs-3g/` | One patch file per task (0001: T-M2-03, 0003: T-M2-05, 0004: T-M3-14, 0005: T-M2-03b, 0006: T-M3-18; 0002 withdrawn, ADR-014). Opus review at merge: 0003, 0004, 0005, 0006; 0001 is build-only |
| `CMakeLists.txt` (top level), `CMakePresets.json`, `cmake/*` | Created by T-M1-03 (role C). Later changes (an `add_subdirectory` line for a new directory, a new preset such as `tsan`, a new `NTFSMAC_SANITIZE` value) are made only by the coordinator in one "build wiring" commit before the wave that needs them; tasks never edit these files |
| `<dir>/CMakeLists.txt` below the top level | Owned by the task that owns `<dir>`. Shared ones: `tests/tools/CMakeLists.txt` is created by T-M2-07 and appended to by T-M3-04 (a later wave, listed in its Files column); `src/daemon/CMakeLists.txt` is created by T-M4-00 with a source glob, so T-M4-01..07 add `.c` files without touching it |
| `src/volmark/`, `src/sanitize/` | T-M3-12; T-M3-19 |
| `scripts/ci-local.sh`, `tests/unit/ci/*` | T-M1-07 creates them; T-M5-08 completes them (full mode, `--verify-result`); no other task edits them |
| `tests/fuzz/` | `tests/fuzz/sanitize/` T-M3-19 (wave c); everything else T-M3-16 (wave d) |
| `src/cli/` | T-M4-04; then `cmd_journal.c` and `cmd_repair.c` T-M4-14 |
| `src/daemon/` | One source file per task as listed; `main.c` only T-M4-08 |
| `tests/harness/` | T-M2-08, then T-M5-01 |
| `tests/integration/<suite>/` | One directory per task |
| `docs/ARCHITECTURE.md` | Corrections from spikes are applied by the coordinator in a single commit per milestone, using the wording proposed in the spike notes |
