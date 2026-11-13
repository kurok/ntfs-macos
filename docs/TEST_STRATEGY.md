# Test strategy

Source tags `[V:Sn]`, `[A]`, `[U]` are defined in [ARCHITECTURE.md](ARCHITECTURE.md). Acceptance thresholds are in [IMPLEMENTATION_PLAN.md](IMPLEMENTATION_PLAN.md) section 2.

## 1. Principles

| Principle | Consequence |
|---|---|
| Data corruption is release-blocking. | Every write test ends with a hash comparison after unmount and remount. A mismatch stops feature work. |
| Linux results say nothing about macOS. | Each test id is tagged with the environment that ran it; the status table never promotes a Linux result to "Verified (VM)". |
| No destructive test on the developer host. | The host builds, runs unit tests and drives VMs. It never mounts an NTFS image: Paragon NTFS on the host claims `Windows_NTFS` volumes with probe order 1 and would mount any attached NTFS image itself [V:S17]. |
| A test that cannot fail is a defect. | Each integrity test has a negative control (a deliberately corrupted byte must make it fail). Agent E checks this in the M5 audit. |
| Images are disposable and synthetic. | Fixtures are generated, never copied from real disks. Fuzz inputs come only from generated images. |

## 2. Environments

| | A: Linux container | B: macOS 27 VM | C: hardware |
|---|---|---|---|
| Purpose | Build pinned NTFS-3G; generate fixtures; reference behaviour; unit tests; crash-consistency sweep; fuzzing; sanitizers | The real product path: macFUSE FSKit backend, daemon, Finder | USB media, real unplug, drive caches, Windows interoperability |
| Tool | Docker or Podman, image `vm/linux/Dockerfile` (Debian 13, pinned digest). Needs `--device /dev/fuse --cap-add SYS_ADMIN` for reference mounts. Fallback: Linux guest under tart. | tart (ADR-008). Golden image = vanilla macOS 27 + macFUSE 5.5.0 with its extension approved once by the human (H3). Each run uses a fresh `tart clone` and deletes it afterwards. | The human's test Mac or boot volume (H7), disposable media (H5), Windows machine or VM (H6) |
| Disk under test | Plain image file (`mkntfs -F`), mounted with `ntfs-3g` on the file | Raw image attached to the guest as a second virtual disk (`tart run --disk`), or attached inside the guest with `hdiutil attach -imagekey diskimage-class=CRawDiskImage -nomount`; both give a `/dev/diskN` [A: T-M1-04 confirms] | Physical USB flash drive and USB SSD/HDD, identified by serial number in the approval |
| Rollback | Container is ephemeral | Clone per run; golden image never modified by tests | Re-format with `mkntfs` between scenarios |
| Automation | Full | Full after H3, over SSH | Scripted steps with human hands for plug and unplug |
| Diagnostics collected | stdout/stderr, ASan reports, core files | `log show` for `fskitd`, `io.macfuse`, `ntfsmacd`; worker FUSE debug log; `~/Library/Logs/DiagnosticReports`; `sysdiagnose` on hang | Same as B plus Windows `chkdsk` output |

### What the VM cannot show

| Gap | Why | Covered by |
|---|---|---|
| Real USB insertion and surprise removal | Virtual disks do not hot-unplug like USB. macOS 27 adds USB accessory passthrough to guests [V:S23]; tool support is [U]. | Environment C; in B, a force-detach of the image approximates it |
| Drive write-cache behaviour, `F_FULLFSYNC` honesty | Virtual disk backed by host file cache | Environment C |
| Sleep/wake with a mounted volume | Guest sleep is not representative | Environment C |
| Coexistence with Paragon or other NTFS drivers | Not installed in the guest; a dummy bundle tests only the detection logic | Environment C, only if the human chooses (H7) |
| Kernel backend | Kexts do not load in VMs [V:S3] | Not in scope (ADR-001) |
| Whether FSKit extensions work in a guest at all | [U2] | T-M2-02; if they do not, Environment B is impossible and a dedicated test Mac is required (brief section 4) |

## 3. Fixtures

All fixtures are generated in Environment A by scripts under `tests/fixtures/gen/` from a seed; images are not committed. `tests/fixtures/manifest.json` lists name, generator, seed, size and the SHA-256 expected after generation.

### 3.1 Clean images

| Name | Size | Cluster | Sector | Content |
|---|---|---|---|---|
| `clean-64m-4k` | 64 MiB | 4 KiB | 512 | empty |
| `clean-256m-4k` | 256 MiB | 4 KiB | 512 | empty |
| `clean-256m-64k` | 256 MiB | 64 KiB | 512 | empty |
| `clean-256m-4kn` | 256 MiB | 4 KiB | 4096 | empty |
| `tree-1g` | 1 GiB | 4 KiB | 512 | Seeded tree: 2,000 files, depth 8, sizes 0 B to 32 MiB, Unicode names, 20 files with named streams, 5 sparse, 5 compressed, hard links |
| `big-8g` | 8 GiB (sparse host file) | 4 KiB | 512 | one 5 GiB file |
| `tiny-*` | 1 to 8 MiB | 512 B to 4 KiB | 512 | fuzz seeds |

Command pattern: `mkntfs -F -Q -L <label> -c <cluster> -s <sector> <file>` from the pinned ntfsprogs. Population is done through a Linux `ntfs-3g` mount inside the container.

### 3.2 Unsafe-state images (synthetic)

| Name | How it is made | Expected probe `state` | Expected NTFS-3G behaviour with `norecover` [V:S11] |
|---|---|---|---|
| `dirty-flag` | `tests/tools/setvolflags` sets `VOLUME_IS_DIRTY` with `ntfs_volume_write_flags` | `dirty_flag` | (not checked by NTFS-3G at mount; our probe is the only guard) [A: confirmed by T-M3-01] |
| `unclean-log` | `tests/tools/logfile_mark` clears the "volume is clean" bit in both `$LogFile` restart areas, keeping fix-ups valid | `unclean_log` | `EOPNOTSUPP`, read-only fallback or refusal |
| `log-v2` | `logfile_mark --v2` sets restart page major version 2 | `fast_startup` | `EPERM`, rejected even with `recover` |
| `hiber-lower` | create `/hiberfil.sys`, 8 KiB, first bytes `hibr` | `hibernated` | `EPERM` |
| `hiber-upper` | same with `HIBR` | `hibernated` | `EPERM` |
| `hiber-short` | `/hiberfil.sys` of 100 bytes | `hibernated` | `EPERM` |
| `hiber-inactive` | `/hiberfil.sys`, 8 KiB of zeros | `clean` | mounts |
| `bitlocker-sig` | 64 MiB of zeros with `-FVE-FS-` at offset 3 | `unsupported` | not NTFS |
| `truncated` | `clean-64m-4k` cut to 60 percent | `corrupt` | mount fails |
| `not-ntfs` | exFAT image | `unsupported` | not NTFS |

Each generator finishes by running Linux `ntfs-3g -o norecover,ro` and `-o norecover` against the image and asserting the behaviour in the last column, so the fixtures are validated against the engine, not only against our probe.

### 3.3 Real Windows images (optional, human-provided)

Created on Windows (H6): volume after hibernate, after shutdown with Fast Startup enabled, after power-off during a copy, and a BitLocker volume. Stored outside the repository; referenced by SHA-256 in `tests/fixtures/external.json`. They upgrade tests `U-PROBE-*` from "synthetic" to "real" evidence. If not provided, the release notes say that unsafe-state detection was verified on synthetic images only.

## 4. Test matrix

Environment column: where the test must pass to count. `A*` means "also run on Linux as a reference".

| Brief area | Id prefix | Cases | Env | Task |
|---|---|---|---|---|
| **Functional** | | | | |
| Mount and unmount | `I-SMOKE-01`, `I-DMN-01..13` | manual worker mount; daemon automount per fixture class; four unmount paths | B | T-M2-03, T-M4-11 |
| Create, read, write, rename, move, delete | `I-FUNC-*` | create/append/overwrite/truncate up and down; rename same and cross directory, over existing; delete open file; rmdir non-empty fails | B, A* | T-M3-05 |
| Directories and nested paths | `I-FUNC-DIR-*` | depth 32; 10,000 entries; rename directory with contents | B, A* | T-M3-05 |
| Unicode and special names | `I-NAME-*` | NFC/NFD pairs, emoji, CJK, RTL, 255 UTF-16 units, trailing dot/space and reserved names refused (`windows_names`), case-only pairs | B, A* | T-M3-05 |
| Small and large files | `I-BIG-*` | 0 B, 1 B, cluster size +/- 1, 4 GiB + 1, 5 GiB | B | T-M3-09 |
| Sparse files | `I-BIG-SPARSE-*` | hole at start, middle, end; hash and allocated size | B, A* | T-M3-09 |
| Metadata and timestamps | `I-META-*` | mtime/ctime/creation time round trip; `touch -t`; read-only attribute; hidden flag | B, A* | T-M3-06 |
| xattrs and named streams | `I-XATTR-*` | set/get/list/remove; 1 B to 64 KiB values; stream visible in Linux tree dump; `ditto`, `cp -c`, Finder copy | B | T-M3-08 |
| Concurrent access | `S-PAR-*` | 16 writers, readers during writes, rename storms | B | T-M5-02 |
| **Integrity** | | | | |
| Hash before and after remount | `I-INT-01..03` | 100 random files; `tree-1g` full copy in and out; negative control | B | T-M2-08 |
| Repeated write/read cycles | `I-INT-CYCLE-*` | 200 overwrite cycles on 50 files | B | T-M3-05 |
| Interrupted writes | `S-CRASH-*` (Linux sweep), `S-KILL-*` | cut after write N; kill worker mid-copy | A, B | T-M3-14, T-M5-05 |
| Forced process termination | `S-KILL-*`, `I-DMN-14..15` | `SIGKILL` worker, daemon, macFUSE module process | B | T-M5-05 |
| Unclean unmount | `S-DISC-*`, `I-DMN-16` | force-detach image during write; next mount is read-only | B, C | T-M5-04 |
| Dirty and hibernated refusal | `U-PROBE-*`, `I-DMN-02..08` | every image in 3.2: probe verdict, daemon mode, image hash unchanged after a read-only session | A, B | T-M3-02, T-M4-11 |
| Cross-platform interoperability | `C-REF-*`, `H-WIN-*` | Linux reads macOS-written image and the reverse; Windows `chkdsk` clean | A+B, C | T-M3-15, T-M5-10 |
| **Reliability** | | | | |
| Long-running workload | `S-SOAK-01` | 6 h mixed workload | B | T-M5-02 |
| Parallel operations | `S-PAR-01` | 16 processes, 30 min | B | T-M5-02 |
| Memory leaks | `S-LEAK-01`, ASan runs | `leaks` on worker; ASan suite on Linux | B, A | T-M5-03 |
| Device disconnection | `S-DISC-*`, `H-USB-*` | image force-detach; real unplug | B, C | T-M5-04, T-M5-10 |
| Low disk space | `S-NOSPC-01` | fill to `ENOSPC`, verify earlier files | B, A* | T-M5-06 |
| Corruption detection | `U-PROBE-*`, `F-*` | truncated and mutated images never crash the probe or the daemon | A | T-M3-16 |
| Recoverability | `S-CRASH-*`, `I-DMN-14..16` | after each failure: read-only mount works, synced data intact | A, B | T-M3-14 |
| **Performance** | | | | |
| Sequential read/write | `P-SEQ-*` | 1 GiB, 1 MiB blocks | B, A* | T-M5-07 |
| Random read/write | `P-RND-*` | 4 KiB blocks over a 1 GiB file | B, A* | T-M5-07 |
| Small files | `P-SMALL-*` | create, stat, read, delete 10,000 files of 4 KiB | B, A* | T-M5-07 |
| Directory enumeration | `P-DIR-*` | list 10,000 and 100,000 entries | B, A* | T-M5-07 |
| CPU and memory | `P-RES-*` | worker and FSModule CPU seconds, peak RSS per benchmark | B | T-M5-07 |
| **Safety and security** | | | | |
| Policy and options | `U-POL-*` | every policy row; banned options unreachable | H, A | T-M3-03 |
| Journal | `U-JRN-*` | kill at each step | H, A | T-M3-13 |
| Privilege | `I-DMN-09`, sandbox tests, ctl authentication tests | worker uid, sandbox denials, socket peer checks | B | T-M4-01, T-M4-04, T-M4-06 |
| Read path edge (macFUSE mitigation) | `I-READ-*` | no `EIO` at EOF | B | T-M3-07 |
| mmap | `I-MMAP-*` | mapped read and write, `msync`, unmount with dirty pages | B | T-M3-11 |
| Sync semantics | `I-SYNC-*` | opcode trace; `F_FULLFSYNC` reaches the device at each sync point | B | T-M2-06, T-M3-10 |
| Packaging | `I-PKG-*` | install, doctor, uninstall, manifest equality | B | T-M4-10, T-M6-01 |
| Finder | `I-FND-*` | visible, copy in and out, eject | B, C | T-M4-11 |

## 5. Reference comparison against NTFS-3G on Linux

Purpose: show that running the same engine behind macFUSE FSKit does not change on-disk results.

1. Twelve deterministic operation scripts (`tests/compatibility/ref/ops-NN.json`: sequences of create, write at offset, truncate, rename, delete, set time, set xattr) are replayed by `tests/harness` on a mount point. They never depend on wall-clock time.
2. Run 1: Environment A, Linux `ntfs-3g` (internal FUSE) on a copy of the fixture.
3. Run 2: Environment B, this product on another copy.
4. Both resulting images are dumped in Environment A by `tests/tools/treedump` (links `libntfs-3g`, read-only): path, type, size, allocated size, SHA-256 of the unnamed stream, each named stream with its hash, NTFS times, file attribute flags.
5. The dumps must be equal after removing entries listed in `tests/compatibility/expected-differences.md`. Expected classes: macOS-created metadata (`.fseventsd`, `.Spotlight-V100`, `.DS_Store`, `com.apple.*` streams), access times, and MFT record numbers.
6. Second independent reader: the Linux kernel `ntfs3` driver mounts the macOS-written image read-only and file hashes are compared again [A: `ntfs3` is available in the container host kernel; if not, this step is skipped and reported as skipped].
7. Structural check on every resulting image: `ntfsfix -n` (no-action mode), `ntfsinfo -m`, and a full read of every file. NTFS-3G has no full `fsck`; Windows `chkdsk` in Environment C is the authoritative structural check.

Performance comparison uses the same scripts as `P-*` in both environments on the same physical host and reports ratios, not absolute claims.

## 6. Crash, kill and disconnect testing

| Technique | Environment | What it proves |
|---|---|---|
| Crash-consistency sweep (below) | A | State of the image if the device stops accepting writes after write N, for many N |
| `SIGKILL` of worker during copy | B | Daemon cleanup, journal, read-only on next mount, no hang of applications beyond `daemon_timeout` |
| `SIGKILL` of `ntfsmacd` | B | Open files keep working; restart reconciliation |
| Kill of the macFUSE FSModule process | B | Volume failure mode and cleanup [V:S7 describes the mode] |
| Force-detach of the disk image during write (`hdiutil detach -force` in guest, or removing the virtual disk) | B | Approximation of unplug |
| Real unplug during write | C | The real thing |
| Guest hard power-off during write (`tart stop` without shutdown) | B | Host-panic approximation; journal survives; next boot mounts read-only |

After every scenario the checks are the same: (1) the next attach mounts read-only with the documented reason; (2) every file that had been synced before the event (the test records these) has the right hash; (3) `ntfsfix -n` reports no fatal error; (4) in Environment C, `chkdsk /scan` output is stored.

### Crash-consistency sweep

Patch 0004 adds a device-operations wrapper inside `libntfs-3g`, compiled only with `--enable-faultio`, controlled by the environment variable `NTFSMAC_FAULT_PLAN`:

| Plan | Effect |
|---|---|
| `cut:N` | Writes 1..N succeed; every later write is silently dropped and reports success (power loss with volatile cache) |
| `eio:N` | Write N returns `EIO`; later writes succeed |
| `dead:N` | Write N and everything after returns `EIO` (unplug) |
| `short:N:K` | Write N writes only K bytes |
| `count` | No fault; prints the number of device writes at exit |

Sweep: for each of 5 workloads (large sequential write; many small files; rename-heavy; truncate-heavy; directory growth past one index block), get the write count W with `count`, then run with `cut:N` for at least 100 values of N spread over 1..W. After each run, without the fault layer: mount read-only, run the checks above. The release build is verified not to contain the layer (`nm` shows no `faultio` symbol).

A failing cut point is not automatically a project bug: NTFS-3G has no journal, so some windows of metadata inconsistency are inherent. Each failure is classified by Opus as (a) expected non-atomic window, documented in `LIMITATIONS.md`, (b) engine defect, reported upstream, or (c) defect in our patches, release-blocking.

## 7. Fault injection on macOS

| Fault | Method |
|---|---|
| Device `EIO` | The `--enable-faultio` worker build is also used in the VM for `eio:N` and `dead:N`, to observe how errors surface through FSKit to `write`, `close`, `fsync` |
| Slow device | `NTFSMAC_FAULT_PLAN=delay:ms` (same layer) to test `daemon_timeout` and Finder responsiveness |
| Probe hang | Test build of the probe with a sleep; daemon must time out and choose read-only |
| Full volume | Small fixture filled by the test |

## 8. Fuzzing

| Target | Harness | Input | Oracle |
|---|---|---|---|
| `ntfsmac-probe` logic | libFuzzer, in-process, in-memory device operations | Mutated `tiny-*` images | No crash, no sanitizer report, no device write call, verdict is valid JSON |
| `libntfs-3g` mount + directory walk + read first 64 KiB of each file | libFuzzer, in-memory device, `NTFS_MNT_RDONLY` | Mutated `tiny-*` images | No crash or sanitizer report |
| Control socket parser | libFuzzer | Byte strings | No crash |
| Read/write operation sequences on a valid image | Custom generator replaying random operation scripts through `libntfs-3g` under ASan, comparing with an in-memory model | Seeds | Model equality, no sanitizer report |

Build: clang, `-fsanitize=fuzzer,address,undefined`, Environment A only. Mutation: generic byte mutation plus a custom mutator that re-computes multi-sector fix-ups so that mutated MFT records pass the first validity check and reach deeper code. Corpus and crashes live under `tests/fuzz/`; inputs are synthetic only.

Budgets: 5 minutes per target in the local full CI run; 1 CPU-hour per target for the M3 gate; 8 CPU-hours for M5 (local machine, 18 cores available).

Handling of findings: crashes in engine code are expected (four NTFS-3G security releases in 2026 [V:S10]). They are recorded in `docs/SECURITY_FINDINGS.md`, a report for the NTFS-3G maintainers is drafted, and sending it is a human decision. A finding reachable through a read-only mount of a crafted volume raises risk R-07 and is reviewed by Opus.

## 9. Continuous integration (local only)

There is no GitHub-hosted CI: no GitHub Actions workflows, no hosted runners and no binaries uploaded to GitHub (ADR-015). All checks run on the developer Mac through `scripts/ci-local.sh`, which wraps `scripts/test-all.sh`.

| Stage | Where | Content | Blocking |
|---|---|---|---|
| `linux-build-test` | Environment A (local container) | Fetch pinned deps, build NTFS-3G and project (release, ASan, UBSan), unit tests, fixture generation, Linux reference suite with FUSE, probe tests on all fixtures | Yes |
| `linux-crash-sweep-short` | Environment A | 50 cut points per workload | Yes |
| `linux-fuzz-short` | Environment A | 5 minutes per target, corpus kept locally | Yes on crash in project code |
| `static-analysis` | Environment A | `clang-tidy`, `cppcheck`, `shellcheck`, `-Wall -Wextra -Werror` on project code | Yes |
| `macos-build` | Host (build only, never a mount) | Build project code and NTFS-3G against macFUSE headers and libraries extracted from the verified package without installing it [A: extraction with `pkgutil --expand-full` gives usable headers and dylibs]; unit tests; unsigned package build | Yes |
| `macos-vm` | Environment B | Mount, integrity, stress, soak, performance and Finder tests | Yes for the stages a milestone gate names |

When to run: `ci-local.sh --quick` (static analysis, unit tests, Linux build) before every push; the full run before each milestone gate and before a release. The result file for each run is committed under `tests/results/` (section 10), so every gate and release points at a recorded local run for the exact commit SHA.

Build outputs, packages and release candidates stay on the local machine under `build/` and `dist/` (both git-ignored). Publishing binaries anywhere is a later, explicit human decision.

## 10. Recording results

| Item | Rule |
|---|---|
| Result file | `tests/results/<UTC date>-<short sha>-<env>.json`, produced by `tests/harness/report.py` |
| Schema | `{ schema, git_sha, dirty_tree, env, host_os, guest_os, macfuse_version, ntfs3g_version, started, finished, cases: [ { id, status: pass/fail/skip/error, duration_s, evidence: [paths], note } ] }` |
| Dirty tree | A result from an uncommitted tree is marked `dirty_tree: true` and cannot satisfy a milestone gate |
| Skips | A skipped case needs a `note`; skips are listed in the milestone audit |
| Evidence | Logs and small artifacts under `tests/results/evidence/<run>/`; images are not stored, only their hashes |
| Status table | `README.md` section generated from the newest result per environment: each feature is `Verified (VM)`, `Verified (hardware)`, `Linux only`, or `Untested` |
| Hardware runs | `tests/results/hardware/<date>.md`, written from the protocol, with device serial numbers and the human's confirmation |
| Flaky tests | A test that fails once and passes on retry is recorded as `fail` with a note, and gets a task; no silent retries |

## 11. Environment C protocol (summary)

Full steps are written in T-M5-10. Minimum scenarios, each on one flash drive and one SSD or HDD, each followed by Windows `chkdsk <drive>: /scan` with stored output:

| # | Scenario | Pass condition |
|---|---|---|
| H-USB-01 | Format on Windows, plug into the Mac, automount read/write, copy `tree-1g` in, eject, verify on Windows | Hashes equal on Windows; `chkdsk` clean |
| H-USB-02 | Files written on Windows, read on the Mac | Hashes equal |
| H-USB-03 | Unplug while idle (mounted read/write) | Next mount read-only on the Mac; Windows offers a scan; `chkdsk` finds no lost data |
| H-USB-04 | Unplug during a large copy | Next mount read-only; files completed and synced before the copy are intact; `chkdsk` repairs at most the file in flight |
| H-USB-05 | Volume from a hibernated Windows and from a Fast Startup shutdown | Read-only on the Mac, reason shown; Windows resumes normally afterwards |
| H-USB-06 | Sleep and wake the Mac with the volume mounted | Volume usable; hashes equal |
| H-USB-07 | BitLocker volume | Not mounted; clear message |
| H-FND-01 | Finder: drag-copy a folder, rename, move to Trash, empty Trash, eject button | All succeed; Windows sees the same tree |

No scenario may run on a drive that the human has not named by serial number.
