# Risk register

Created 2026-11-13 (Phase 1). The coordinator updates the Status column at each milestone gate.
Source tags `[V:Sn]`, `[A]`, `[U]` are defined in [ARCHITECTURE.md](ARCHITECTURE.md). Task ids are in [AGENT_TASKS.md](AGENT_TASKS.md).

Scales: likelihood and impact are Low (L), Medium (M), High (H). "Opus" = the mitigation or any finding under this risk requires Opus-level review.
Owner roles: A coordinator, B file system, C macOS integration, D testing, E audit, Human.

## Register

| ID | Category | Risk | L | I | Mitigation | Owner | Opus | Status |
|---|---|---|---|---|---|---|---|---|
| R-01 | Backend limitation | libfuse2-based `ntfs-3g` cannot use `-o backend=fskit` (documented for libfuse3 only in 5.1.0; a third party saw libfuse2 reject it) [V:S5][V:S9] | M | H | T-M2-03 on day one of M2. Fallback: libfuse3 port (T-M2-03b). Second fallback: human decision H9. | B | Yes (port review) | Open |
| R-02 | Data corruption | FSKit or the macFUSE FSModule corrupts or drops data. A corruption mitigation shipped as recently as 5.5.0 (Oct 2026); earlier releases fixed zero-filled small writes and a crash on empty reads [V:S1][V:S7] | M | H | Hash-after-remount on every write test; cross-read on Linux; soak and parallel suites; `I-READ-*` for the empty-reply path; pin macFUSE minimum 5.5.0 and re-run the suite on each macFUSE update before raising the tested version. | D | Yes (any mismatch) | Open |
| R-03 | Data corruption | Unsafe volume mounted read/write: hibernated or Fast Startup Windows, unclean `$LogFile`. NTFS-3G's default `recover` resets the journal [V:S11] | L | H | Own probe before mount; `norecover` always; banned options unreachable (unit test); `O_RDONLY` descriptor for read-only decisions; fixtures validated against the engine; real Windows images when available (H6). | B | Yes (T-M3-02, T-M3-17) | Open |
| R-04 | Data corruption | Metadata inconsistency after unplug, crash or power loss. NTFS-3G does not journal | H | H | Cannot be eliminated. Journal + dirty marker force read-only next time and a Windows `chkdsk`; crash-consistency sweep classifies windows; `F_FULLFSYNC` at sync points; documented limitation N4. | B, D | Yes (sweep triage, T-M3-12) | Open, accepted residual |
| R-05 | Backend limitation | Applications believe `fsync`/`close` succeeded when data is not durable: no per-file fsync in FSKit, close errors dropped on macOS 27 [V:S1] | H | M | Document N1, N2, N5; T-M2-06 measures actual behaviour; after first device write error the session is failed and the next mount is read-only; user notification on write error. | B, C | No | Open, accepted residual |
| R-06 | Data corruption | Our own patches (0002 sync, 0003 fd, 0005 libfuse3 port) or the dirty-marker step introduce a write bug | M | H | Minimal patch policy (ADR-002); each patch has a test; Linux reference suite must still pass with patched binary; Opus review of each. | B | Yes | Open |
| R-07 | Security | Crafted NTFS volume exploits a parser bug in `libntfs-3g` (four security releases in 2026) [V:S10] | M | H | Parser never runs as root; sandbox; read-only descriptor when unsafe; fuzzing; track upstream releases and re-pin within one session. Residual: user-level code execution in a sandbox. | B, C | Yes (T-M4-06, findings) | Open |
| R-08 | Security | Root daemon flaw: client input reaches a root-executed command or a banned option; socket spoofing | L | H | Option allow-list built by the daemon; peer credential check; no shell invocation; socket fuzz; audit T-M4-12. | C | Yes (T-M4-12) | Open |
| R-09 | Backend limitation | macFUSE FSKit backend lacks options or behaviour the design assumes (`fsname`, `volname`, creation time, xattr semantics, `daemon_timeout`); "most kernel-handled mount options not implemented" [V:S2] | M | M | T-M2-04 option matrix; design corrected from evidence; features that cannot work are documented, not emulated. | B | No | Open |
| R-10 | Backend limitation | Performance much lower than the kext backend or Linux (single-threaded engine + FSKit XPC path) [V:S2][V:S15] | H | M | Measure (T-M5-07); `big_writes`; report honestly; no correctness trade for speed. Not a release blocker unless a workload hangs. | D | No | Open |
| R-11 | Apple policy | Apple changes FSKit or Disk Arbitration behaviour in a 27.x update (the API gained V2, V2.4 and V3 levels within about 18 months [V:S16]); macFUSE needs time to follow | M | H | Suite is re-run on each macOS point release in the VM before the version is listed as supported; `doctor` warns on untested OS versions. | C, D | No | Open |
| R-12 | Apple policy | Mount-approval dissenter no longer honoured, so Apple's read-only NTFS driver always mounts first; Disk Arbitration bugs with third-party file systems were reported [V:S14] | M | M | T-M4-02 measures; fallback is unmount-then-remount; second fallback is manual mount only, with automount documented as unsupported. | C | No | Open |
| R-13 | Apple policy | If the native FSKit fallback (ADR-001 D) is needed: restricted entitlement, provisioning profile, possible lack of Developer ID support, kext precedence over FSKit modules for NTFS, physical-disk permission failures [V:S14] | M | H | Not started without human decision H9; evaluated only if the M2 gate fails. | Human, C | Yes (design) | Dormant |
| R-14 | Licensing | macFUSE licence forbids bundling or automated installation with commercial software without permission; kext and other parts are closed [V:S8] | L | M | Prerequisite model, never bundled or downloaded by the installer (ADR-013); licence notice in `LICENSES/`. | C | No | Mitigated by design |
| R-15 | Licensing | GPL obligations not met in a binary release (missing corresponding source for NTFS-3G + patches) | L | M | `make-source-release.sh` and criterion 6.5; SPDX headers; audit at M7. | C, E | No | Open |
| R-16 | Licensing | Dependency on a single closed-source component (macFUSE FSModule) maintained mainly by one developer; a licence or availability change strands the product | L | H | Backend-independent probe, policy, journal and tests keep the native FSKit port viable; fuse-t noted as a possible emergency backend but its binary licence is non-commercial only [V:S17]. | A | No | Open, accepted |
| R-17 | Paragon conflict | Paragon NTFS on the developer host claims NTFS volumes first (`FSProbeOrder` 1) [V:S17]; two drivers could write one volume; test results on the host would be about Paragon, not this product | H (on host) | H | No mount tests on the host, ever; VM only. Passive mode when another NTFS bundle is detected; `mount` refuses if the volume is already mounted; hardware tests only after decision H7. Agents never disable Paragon. | D, Human | No | Open |
| R-18 | VM limitation | FSKit extensions may not load, or cannot be approved, inside a macOS guest [U2]; enabling needs a GUI click [U3] | M | H | T-M2-02 first. If it fails: Environment B is not possible, a dedicated test Mac is required (brief section 4), project checkpoints for the human. Golden image approved once, clones reuse it. | C, Human | No | Open |
| R-19 | VM limitation | VM cannot reproduce USB unplug, drive caches, sleep/wake, real Finder behaviour | H | M | Environment C protocol (H5 to H7). Without it the release is labelled "VM-verified only". | D, Human | No | Open |
| R-20 | VM limitation | VM tool licence or macOS 27 guest support differs from what a secondary source reports [V:S20][U11] | M | M | T-M1-04 verifies before anything depends on it; lume as fallback. | D | No | Open |
| R-21 | Cost | Opus or Sonnet allowance exhausted mid-milestone; trial-and-error loops on VM issues burn tokens | M | M | Small tasks; spikes are time-boxed; three-attempt rule; Opus use enumerated (10 to 14 planned); deterministic tools before AI; checkpoint files after each milestone and on exhaustion. | A | No | Open |
| R-22 | Cost | Paid services assumed without approval (Apple Developer Program fee, hosted CI minutes) | L | L | No GitHub-hosted CI and no published binaries (ADR-015); signing is optional and needs H8 | A | Mitigated |
| R-23 | Cost | Host changes made without approval (Homebrew installs, container runtime, VM tool) | L | M | H1 and H2 are explicit gates; `scripts/bootstrap.sh` only checks and prints what is missing, it does not install. | A | No | Open |
| R-24 | Scope | Brief requires Intel support; macOS 27 does not run on Intel [V:S22] | Certain | L | ADR-009: arm64 only; reported as not applicable. | A | No | Closed by decision |
| R-25 | Integration | Worker cannot run as the console user when started from a root daemon, or cannot use an inherited descriptor [U5][U6] | M | M | T-M2-05, T-M4-01. Fallbacks: patch 0003; a per-user LaunchAgent that receives the descriptor over a Unix socket from the daemon. | C | No | Open |
| R-26 | Data corruption | macOS writes its own metadata (`.Spotlight-V100`, `.fseventsd`, `.DS_Store`, `com.apple.*` xattrs) and changes user-visible content or breaks Windows tools | M | L | `I-XATTR-*`; expected-differences list; consider `mdutil -i off` marker and `.metadata_never_index` at mount, decided from T-M3-08 results. | D | No | Open |
| R-27 | Data corruption | Name handling differences: Unicode normalisation (`nfconv`), case sensitivity, Windows-forbidden names, names over 255 UTF-8 bytes hidden on macOS [V:S12] | M | M | `I-NAME-*` on both platforms; `windows_names`; documented behaviour for case-only pairs and over-long names. | D | No | Open |
| R-28 | Process | A test passes without testing anything, or a claim is made from Linux results only | M | H | Negative controls; environment tags; four-word status vocabulary; E audits at M2, M3, M5, M7. | E | Yes (T-M5-11) | Open |
| R-29 | Supply chain | Tampered dependency download | L | H | SHA-256 pins; package signature check for macFUSE; no unpinned fetch at build time. | C | No | Open |
| R-30 | Durability | Device `F_FULLFSYNC` unsupported or slow on some USB bridges; `sync` option cost | M | M | Fallback to `fsync` with a logged warning; benchmark in T-M3-10; hardware check in Environment C. | B | No | Open |

## Risks requiring Opus-level review (summary)

| Risk | Review point |
|---|---|
| R-01 | T-M2-03b, only if the libfuse3 port is needed |
| R-02 | Any reproducible hash mismatch, immediately |
| R-03 | T-M3-02 (probe), T-M3-04 (fixture tool), T-M3-17 (audit) |
| R-04 | T-M3-14 triage, T-M3-12 ordering |
| R-06 | Each patch at merge time |
| R-07 | T-M4-06 sandbox, fuzz findings reachable from a read-only mount |
| R-08 | T-M4-12 |
| R-13 | Design review if the fallback is activated |
| R-28 | T-M5-11, T-M7-01 |

## Top five

1. **R-01** libfuse2 and the FSKit backend: decides whether the chosen architecture works at all.
2. **R-04** non-journaled metadata updates under unplug or power loss: inherent, can only be contained.
3. **R-02** data-integrity defects in a young, closed FSKit path.
4. **R-18** FSKit extension inside a macOS guest: decides whether automated macOS testing is possible.
5. **R-07** hostile volumes against an engine with a recent history of memory-safety fixes.
