# Next actions

## Human actions needed (details: docs/IMPLEMENTATION_PLAN.md section 5)

H1 and H2 are done (tools installed, Docker running, tart installed).

## Blocked on the owner

Nothing. H3 is done for the current golden VM (`ntfsmac-golden`); a rebuilt golden image needs it again (`vm/macos/provision-golden.sh` waits for it).

## Agent tasks ready now

1. Milestone 3, in this order (tasks and criteria: docs/AGENT_TASKS.md, docs/IMPLEMENTATION_PLAN.md section M3):
   - **Step 1, no VM: DONE (f8b3815).** T-M3-00 contracts first (probe JSON schema, policy enum, journal API, option builder API). Then in parallel: T-M3-01 (U9 by test, Environment A), T-M3-03 (policy library and option builder, emits the corrected 3.3 set), T-M3-04 (unsafe fixtures, S+O), T-M3-13 (session journal library), T-M3-14 (patch 0004 fault injection and crash sweeper, S+O).
   - **Step 2, macOS VM suites, two at a time (two-VM limit):** T-M3-10 (sync path, first: T-M3-12 and T-M3-18 need it), T-M3-05 (names, U22), T-M3-08 (xattr, openxattr), T-M3-20 (ownership, N10), T-M3-06 (metadata), T-M3-07 (EOF reads), T-M3-09 (large, sparse, compressed), T-M3-11 (mmap).
   - **Step 3:** T-M3-02 (probe, S+O; needs 00 and 04), T-M3-19 (disk-string hardening, S+O; needs 00 and 03), T-M3-18 (patch 0006 session channel, S+O; needs 10 and 14), T-M3-15 (Linux vs macOS reference comparison; needs 05).
   - **Step 4:** T-M3-12 (`ntfsmac-volmark`, Opus; needs 01, 04, 10, 13, 14), T-M3-16 (fuzzing, needs 02).
   - **Step 5:** T-M3-17 M3 audit (Opus, independent).
   - Opus runs planned: T-M3-12, T-M3-17, and reviews of T-M3-02, -04, -14, -18, -19 (7). Check plan usage before each step; stop and checkpoint before any paid overage.
3. Run `scripts/ci-local.sh --quick` before every push (ADR-015).
4. Confirm the three assumptions recorded by the N-05/N-07/N-08 fixes (`/dev/fd/N` sharing, `POSIX_SPAWN_CLOEXEC_DEFAULT`, peer code-signing checks) when T-M4-02/-04/-13 run.

## Milestone 2 gate

T-M2-03 decides whether NTFS-3G runs on the macFUSE FSKit backend (risk R-01). Needs T-M1-04 finished,
the golden macOS 27 VM (T-M2-02) and the one-time GUI approval of the macFUSE extension (H3).
