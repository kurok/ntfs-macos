# Implementation plan

Milestones M1 to M7 follow section 8 of [PROJECT_BRIEF.md](PROJECT_BRIEF.md). Tasks are in [AGENT_TASKS.md](AGENT_TASKS.md); tests in [TEST_STRATEGY.md](TEST_STRATEGY.md).
Source tags `[V:Sn]`, `[A]`, `[U]` are defined in [ARCHITECTURE.md](ARCHITECTURE.md).

## 1. Rules that apply to every milestone

| Rule | Detail |
|---|---|
| Exit gate | A milestone is closed only when every acceptance criterion has a recorded result file under `tests/results/` (TEST_STRATEGY.md section 10) and Agent E has signed the audit note `docs/audits/Mn.md`. |
| Where mounts happen | Only inside the macOS VM (Environment B) or, from M5, on approved hardware (Environment C). Never on the developer host: it has Paragon NTFS installed and claims every NTFS volume first (ARCHITECTURE.md section 8.1). |
| Stop conditions | A reproducible data mismatch in any integrity test stops feature work until root cause is found. Three failed Sonnet attempts on one task escalate once to Opus; a failed Opus attempt stops the task and writes `CHECKPOINT.md`. |
| Claims | README status table uses four words only: `Verified (VM)`, `Verified (hardware)`, `Linux only`, `Untested`. |
| Checkpoint | After each milestone: update `CHECKPOINT.md`, `NEXT_ACTIONS.md`, tag `mN-done`. |

## 2. Milestones

### M1 Architecture and groundwork

| # | Acceptance criterion | Measured by |
|---|---|---|
| 1.1 | Six architecture documents exist and are internally consistent (task ids, ADR numbers, test ids resolve). | T-M1-06 audit note lists zero dangling references. |
| 1.2 | Licence files and SPDX headers policy in place (ADR-010). | `LICENSES/` contains GPL-2.0, LGPL-2.1, macFUSE notice; README states GPL-2.0-or-later. |
| 1.3 | `third_party/deps.lock` pins every dependency; `scripts/fetch-deps.sh` refuses a checksum mismatch. | Unit test with a corrupted file fails closed. |
| 1.4 | CMake skeleton configures and builds an empty target on macOS (CLT) and Linux. | `cmake --preset release && cmake --build` exit 0 on both. |
| 1.5 | Environment A image builds NTFS-3G 2026.9.28 and `mkntfs` runs on a plain file. | `vm/linux/run.sh selftest` exit 0. |
| 1.6 | VM tool chosen and a macOS 27 guest boots headless with SSH. | `vm/macos/tart.sh selftest` prints guest `sw_vers` 27.x. |

**Exit gate M1:** 1.1 to 1.6 pass. Human actions H1 and H2 done.

### M2 Working prototype

| # | Acceptance criterion | Measured by |
|---|---|---|
| 2.1 | NTFS-3G 2026.9.28 builds on macOS 27 arm64 against macFUSE 5.5.0 `libfuse`, zero errors; warnings listed. | Build log in `tests/results/`. |
| 2.2 | In the VM, a 256 MiB image made by `mkntfs` on Linux mounts read/write through `-o backend=fskit` as a non-root user. | `mount` output shows the volume; test `I-SMOKE-01`. |
| 2.3 | Write 100 files (1 KiB to 64 MiB, random data), unmount, remount, SHA-256 of every file equal. Then the same image is read on Linux with NTFS-3G and hashes are equal. | `I-INT-01`, `C-REF-01`. |
| 2.4 | Accepted and rejected mount options recorded. | `docs/spikes/M2-options.md`; ARCHITECTURE.md section 3.3 corrected. |
| 2.5 | FUSE opcodes observed for `write`, `close`, `fsync`, `F_FULLFSYNC`, `sync`, unmount recorded. | `docs/spikes/M2-sync.md`; ARCHITECTURE.md sections 4.4 and 9 corrected. |
| 2.6 | Device fd passing works, or patch 0003 exists and works. | `I-SMOKE-02`. |

**Exit gate M2 (backend gate, ADR-001):** 2.2 and 2.3 pass on three consecutive runs. If they cannot pass, follow the ADR-001 fallback order and stop for a human decision before any native FSKit work.

### M3 File system correctness

| # | Acceptance criterion | Measured by |
|---|---|---|
| 3.1 | `ntfsmac-probe` classifies all fixture classes correctly: clean, dirty flag, unclean log, version-2 log, hibernated (`hibr`, `HIBR`, short file), BitLocker signature, non-NTFS, truncated image. Zero device writes (image hash unchanged). | Unit tests `U-PROBE-*`, 100 percent pass; hash check in each. |
| 3.2 | Policy unit tests cover every row of the table in ARCHITECTURE.md section 3.2, and a test proves the option builder cannot emit `recover` or `remove_hiberfile`. | `U-POL-*`. |
| 3.3 | Functional suite passes in the VM: create, read, write, append, truncate (shrink, extend), rename (same dir, cross dir, overwrite), delete, mkdir/rmdir, nested depth 32, 10,000 files in one directory. | `I-FUNC-*`, 100 percent pass. |
| 3.4 | Names: Unicode (NFC, NFD, emoji, CJK, RTL), 255-unit names, case-only differences, Windows-forbidden names refused with a defined errno. | `I-NAME-*`. |
| 3.5 | Large file > 4 GiB and sparse file round-trip with equal hashes; allocated size of the sparse file reported. | `I-BIG-*`. |
| 3.6 | Timestamps (mtime, ctime, creation time) preserved across remount within NTFS resolution (100 ns) and equal to the Linux reading of the same image. | `I-META-*`. |
| 3.7 | xattr set/get/list/remove and Finder-style copy (`cp -c`, `ditto`) pass, or xattrs are disabled and documented (ADR-012). | `I-XATTR-*`. |
| 3.8 | Reads at EOF, across EOF and of a file truncated to zero never return `EIO`. | `I-READ-*`. |
| 3.9 | Crash-consistency sweep on Linux: for at least 500 cut points over 5 workloads, the image mounts read-only afterwards, `ntfsfix -n` reports no fatal error, and every file that was synced before the cut has the right hash. | `S-CRASH-*`. Any failure is triaged with Opus and reported upstream. |
| 3.10 | Reference comparison: the same 12 operation scripts run through NTFS-3G on Linux and through this product on macOS give identical tree dumps (paths, sizes, hashes, stream names), except the listed expected differences. | `C-REF-*`. |
| 3.11 | Fuzz harness runs 1 CPU-hour on the probe and 1 CPU-hour on mount-and-walk with ASan and UBSan; no crash in project code; engine crashes filed upstream and listed. | `F-*` logs. |
| 3.12 | Session journal unit tests pass, including crash between steps (simulated by killing the process at each step). | `U-JRN-*`. |

**Exit gate M3:** 3.1 to 3.12 pass; Opus audit (T-M3-17) of probe, policy, patches 0002/0004 and the dirty-marker ordering finds no open data-loss issue.

### M4 macOS integration

| # | Acceptance criterion | Measured by |
|---|---|---|
| 4.1 | Attaching an NTFS disk image in the VM results in an automatic mount within 10 s, in the mode the policy table requires, for each fixture class. | `I-DMN-01..08`. |
| 4.2 | The worker runs as the console user, not root; the probe as `nobody`; verified with `ps`. | `I-DMN-09`. |
| 4.3 | Unmount through `ntfsmac unmount`, `umount`, `diskutil unmount` and Finder eject (AppleScript) each end with: worker exit 0, journal entry gone, mount point removed, hashes intact on remount. | `I-DMN-10..13`. |
| 4.4 | Killing the worker, killing the daemon, and force-detaching the image each leave the journal entry in place, and the next attach mounts read-only with the documented reason. | `I-DMN-14..16`. |
| 4.5 | Passive mode: with a dummy third-party NTFS bundle present, no dissent is issued and no automount happens. | `I-DMN-17`. |
| 4.6 | Developer installer installs, `ntfsmac doctor` is green, uninstaller removes every installed path (list compared to a manifest). | `I-PKG-01..02`. |
| 4.7 | Finder: volume visible on the desktop and sidebar, copy of a 1,000-file tree in and out with equal hashes. | `I-FND-01..02` (scripted with `osascript`; screenshots stored). |

**Exit gate M4:** 4.1 to 4.7 pass in the VM; audit T-M4-12 signed. Items that the VM cannot show (real USB insertion) are listed for M5 hardware validation.

### M5 Automated validation

| # | Acceptance criterion | Measured by |
|---|---|---|
| 5.1 | Whole suite (unit, integration, compatibility, stress) runs from one command per environment and writes one result file. | `scripts/test-all.sh --env A|B`. |
| 5.2 | Soak: 6 hours of mixed workload in the VM, zero hash mismatches, worker RSS growth below 10 percent after the first hour. | `S-SOAK-01`. |
| 5.3 | Parallel: 16 writer processes for 30 minutes, zero mismatches, no hang longer than 60 s. | `S-PAR-01`. |
| 5.4 | `leaks` on the worker after the functional suite reports zero leaks from project patches; engine leaks listed. | `S-LEAK-01`. |
| 5.5 | Low space: filling the volume gives `ENOSPC`, no mismatch in pre-existing files, volume clean after unmount. | `S-NOSPC-01`. |
| 5.6 | Performance report with sequential, random, small-file, directory-listing numbers for this product (VM), NTFS-3G on Linux (same image, same VM host), and CPU and memory use. Regressions above 2x against the Linux reference are explained. | `P-*`, `docs/PERFORMANCE.md`. |
| 5.7 | Full local CI run (`scripts/ci-local.sh`, TEST_STRATEGY.md section 9) green for Environments A and B and the host build. | Result file committed under `tests/results/` for the gate commit. |
| 5.8 | Environment C protocol executed on disposable media: USB insert, read/write, eject, unplug during write, Windows `chkdsk` after each scenario. | `docs/HARDWARE_TEST_PROTOCOL.md` results, signed by the human. |

**Exit gate M5:** 5.1 to 5.7 pass. 5.8 passes, or is recorded as "not done" and the release is labelled "VM-verified only, not production-ready" (brief section 9).

### M6 Packaging

| # | Acceptance criterion | Measured by |
|---|---|---|
| 6.1 | `packaging/build-pkg.sh` produces `ntfs-macos-<version>.pkg` from a clean checkout with pinned dependencies. | Script exit 0 in a fresh VM clone. |
| 6.2 | Two builds from the same commit give byte-identical binaries, or the differences are listed and explained. | `scripts/repro-check.sh`. |
| 6.3 | Install and uninstall tested on a fresh VM clone; `pkgutil --files` matches the manifest. | `I-PKG-03..04`. |
| 6.4 | Signed and notarized package passes `spctl -a -t install` (only if credentials exist; otherwise recorded as unsigned per ADR-011). | Command output. |
| 6.5 | Source release archive contains this repository, the NTFS-3G tarball and the patch series (GPL obligation). | `scripts/make-source-release.sh` + checksum file. |
| 6.6 | `docs/INSTALL.md` and `docs/LIMITATIONS.md` exist; limitations include N1 to N8 from ARCHITECTURE.md section 9. | Audit check. |

### M7 Release readiness

| # | Acceptance criterion | Measured by |
|---|---|---|
| 7.1 | Independent Opus review of every change to the data path, probe, policy and daemon privilege code since M3. | `docs/audits/M7.md`. |
| 7.2 | Zero open findings with severity "data corruption" or "privilege escalation". | Risk register and issue list. |
| 7.3 | `docs/RELEASE_READINESS.md` has a row for every Definition-of-Done item with status and evidence link. | Document. |
| 7.4 | Full suite re-run on the release commit. | Result file with the release SHA. |

## 3. Pinned dependencies

`third_party/deps.lock` is the single source of truth. Format: one line per artifact, `name version url sha256`.

| Dependency | Version | Source | Integrity |
|---|---|---|---|
| NTFS-3G + ntfsprogs | 2026.9.28 | Tarball `ntfs-3g_ntfsprogs-2026.9.28.tgz` linked from https://github.com/tuxera/ntfs-3g/releases/tag/2026.9.28 (hosted on tuxera.com) | SHA-256 published on the release page: `350d9415c59f7c3fa74e23985ad2d56423c6a168337368f672e2e362d8f295c8` [V:S10]. T-M1-02 re-reads the page and confirms the digest before pinning. |
| macFUSE | 5.5.0 (minimum and tested version) | https://github.com/macfuse/macfuse/releases/tag/macfuse-5.5.0 | Not bundled. For the VM: download once, record the SHA-256 in `deps.lock` (trust on first use), and require `pkgutil --check-signature` to show a valid Developer ID Installer signature and notarization before install. The expected signer identity is recorded in `deps.lock` at first verification [U until then]. |
| libfuse (v2) / libfuse3 3.18.3 | as shipped inside macFUSE 5.5.0 [V:S1] | macFUSE package | Covered by the macFUSE check. |
| CMake | >= 3.28 | Homebrew | Version check in `scripts/bootstrap.sh`. |
| Ninja, pkgconf | any current | Homebrew | Version recorded in build log. |
| tart | version chosen in T-M1-04 | Decided in T-M1-04 (the Homebrew tap name is [U11]) | Version pinned in `vm/macos/versions.env`. |
| macOS guest image | macOS 27.x, exact tag and digest chosen in T-M1-04 | OCI image | Digest pinned in `vm/macos/versions.env`. |
| Linux base image | `debian:13-slim` by digest | Docker Hub | Digest pinned in `vm/linux/Dockerfile`. |
| Fuzzing | clang libFuzzer (in the Linux image) | distribution package | Version in image. |

Rules: no dependency is fetched at build time without a checksum; updating a pin is its own commit with a full Environment A run; a NTFS-3G security release triggers a pin update task within one working session.

## 4. Sequencing

```
M1 groundwork ──> M2 prototype (backend gate) ──> M3 correctness ──┬─> M5 validation ──> M6 packaging ──> M7
                                                  └─> M4 integration ┘
```

M3 and M4 run in parallel after the M2 gate: M3 work is in `src/probe`, `src/policy`, `src/journal`, `tests/`, `third_party/patches`; M4 work is in `src/daemon`, `src/cli`, `integration/`, `packaging/`. The shared contract is `include/ntfsmac/*.h`, frozen by task T-M3-00 before either starts.

## 5. Human actions

| ID | Action | Needed for | Why an agent cannot do it |
|---|---|---|---|
| H1 | Approve installing on the host: `cmake`, `ninja`, `pkgconf` (Homebrew), and the VM tool chosen in T-M1-04. | M1 | Host change; brief requires permission. |
| H2 | Start a container runtime (Docker Desktop daemon) or approve installing Podman/colima. | M1 (Environment A) | Host change. |
| H3 | In the golden macOS VM: enter the admin password for the macFUSE installer and enable the macFUSE file system extension in System Settings (General > Login Items & Extensions > File System Extensions [A: exact pane name to confirm on 27.2]). | M2 | GUI consent; scripted enabling is unknown (U3). One time per golden image. |
| H4 | Withdrawn 2026-11-13: the repository is public and the owner decided on no GitHub-hosted CI and no published binaries for now (ADR-015). Revisit only on the owner's request. | n/a | Owner decision. |
| H5 | Provide disposable NTFS media (one USB flash drive, one USB HDD or SSD) and approve writes to them by serial number. | M5 (Environment C) | Brief: never write to physical media without approval. |
| H6 | Provide a Windows 10/11 machine or VM to run `chkdsk` and to create real hibernated and Fast Startup volumes. | M5 | Licensing and hardware. |
| H7 | Decide how to run hardware tests given Paragon NTFS on the host: a second Mac, an external macOS 27 boot volume, or disabling Paragon in its own settings for the test window. Agents will not disable it. | M5 | Third-party software on the user's machine. |
| H8 | Install full Xcode; join the Apple Developer Program; create Developer ID Application and Installer certificates; store a `notarytool` keychain profile. | M6 (signed release only) | Paid membership and personal credentials. |
| H9 | If the M2 gate fails: choose between the native FSKit port (needs H8 immediately) and the kernel backend as an opt-in mode. | Conditional | Changes project scope and cost. |

## 6. Model usage plan

| Work | Model |
|---|---|
| All tasks by default | Sonnet 5.5 |
| Reviews marked "Opus review" in AGENT_TASKS.md (probe checks, patches 0002/0004/0005, dirty-marker ordering, sandbox profile, crash-sweep findings, M3/M5/M7 audits) | Opus 5.5, one bounded review each, input limited to the diff and the named design section |
| Fable | Not used after this document set |

Expected Opus invocations for the whole project: 10 to 14. Each is listed in AGENT_TASKS.md so the coordinator can budget them.
