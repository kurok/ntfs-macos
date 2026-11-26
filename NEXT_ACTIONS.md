# Next actions

State on 2026-11-26: M5 gate passed; criterion 5.6 pending the owner's decision. The results are at fd6a0de
(full local CI run at ac5cf2b, host, A and B, plus the criterion runs S-SOAK-01, S-PAR-01, S-LEAK-01,
S-KILL-01..05, S-DISC-01..05 with `--full`, and the fuzz campaign F-CAMPAIGN-01, 8.00 CPU-hours, all signatures
known). `docs/PERFORMANCE.md` still says "NO RESULTS YET": the criterion 5.6 performance run has not been made.
Milestone 6 (packaging) is in progress.

## Human actions needed (details: docs/IMPLEMENTATION_PLAN.md section 5)

H1, H2 and H3 are done (tools installed, Docker running, tart installed, macFUSE approved in the
current golden VM `ntfsmac-golden`; a rebuilt golden image needs H3 again, `vm/macos/provision-golden.sh` waits for it).

- **H8 (Apple Developer account):** needed only for a signed and notarized release (criterion 6.4, T-M6-02).
  Without it the release is the unsigned package of ADR-011 ("complete, unsigned"). Nothing else in M6 waits for it.
- H5 to H7 (disposable USB media, a Windows machine, how to test with Paragon on the host) are needed for
  criterion 5.8 and T-M5-10 (`docs/HARDWARE_TEST_PROTOCOL.md`); the release stays labelled "VM-verified only"
  until 5.8 passes (brief section 9).

## Blocked on the owner

Decisions that are open. None of them stops the M6 work in the list below.

1. **Perf `dir100000` (criterion 5.6).** In Environment B the 100,000-entry directory case cannot pass with
   macFUSE 5.5.0 as measured (`docs/spikes/M5-perf.md`, section "Criterion 5.6 at ac5cf2b"; 0f3357f changed the
   test only). The options listed there: pre-populate the directories in the image in Environment A, record the
   case as not runnable on this macFUSE version, or extend the daemon's unmount wait to a module that is still
   unmounting. No choice is made, and `docs/LIMITATIONS.md` states the behaviour only.
2. **ADR-028 (Spotlight and Quick Look wait at unmount): Proposed.** Code is on main (647c18f); the owner has not
   accepted it as a product choice. `docs/LIMITATIONS.md` describes it as proposed.
3. **ADR-002 and ADR-026 amendment for patch 0016 (F9, `INDEX_ROOT.index_block_size`): Proposed.** The patch is on
   main (14241bd) and edits index code, which the scope sentence of ADR-002 excludes unless amended.
4. **Upstream report drafts are not posted, by owner decision:** NTFS-3G F2, F5, F6, F7, F8, F9, the stale
   `$MFTMirr` report, and the reports for patches 0007 and 0008 (`docs/UPSTREAM_REPORTS.md`); and the macFUSE
   report on synchronize after every request with directory handles held (draft in the same file). Sending is a
   human act.

## Agent tasks ready now

1. Milestone 6 (tasks and criteria: docs/AGENT_TASKS.md, docs/IMPLEMENTATION_PLAN.md section M6):
   - **T-M6-01** release package (`packaging/build-pkg.sh`, `packaging/scripts/*`, `packaging/distribution.xml`):
     in progress in a parallel change. Criteria 6.1 and 6.3 (`I-PKG-03..04`).
   - **T-M6-03:** `docs/INSTALL.md` and `docs/LIMITATIONS.md` written (criterion 6.6); the installed-layout table in
     INSTALL.md must be checked against the package when T-M6-01 lands.
   - **T-M6-02:** `packaging/sign-and-notarize.sh` written, with a dry run and the stub test U-PKG-05; never run for
     real (needs H8). Criterion 6.4 is recorded as unsigned per ADR-011 unless H8 is done.
   - T-M6-04 (`scripts/repro-check.sh`, criterion 6.2) and T-M6-05 (`scripts/make-source-release.sh`, criterion 6.5; the GPL
     source archive is needed before any binary is distributed, ADR-015).
   - T-M6-06 M6 audit (independent; root-run package scripts, package contents, uninstall manifest), after T-M6-01 to 05.
2. Open M5 follow-up: `docs/PERFORMANCE.md` needs the criterion 5.6 run and the owner's decision on item 1 above.
3. Run `scripts/ci-local.sh --quick` before every push (ADR-015). Packages are built locally and published, if
   at all, only by the owner as a GitHub pre-release.
4. Milestone 7 (release readiness, `docs/RELEASE_READINESS.md`, audit M7) follows M6.
