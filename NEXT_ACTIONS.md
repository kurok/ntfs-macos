# Next actions

## Human actions needed (details: docs/IMPLEMENTATION_PLAN.md section 5)

H1 and H2 are done (tools installed, Docker running, tart installed).

## Blocked on the owner

Nothing. H3 is done for the current golden VM (`ntfsmac-golden`); a rebuilt golden image needs it again (`vm/macos/provision-golden.sh` waits for it).

## Agent tasks ready now

1. T-M2-04 option matrix, T-M2-05 device by fd (start from U5 notes: non-root block-device refusal, root:operator nodes, read-only automount U21), T-M2-06 sync trace, T-M2-09 cross-read; then T-M2-10 audit.
2. Run `scripts/ci-local.sh --quick` before every push (ADR-015).
3. Confirm the three assumptions recorded by the N-05/N-07/N-08 fixes (`/dev/fd/N` sharing, `POSIX_SPAWN_CLOEXEC_DEFAULT`, peer code-signing checks) when T-M4-02/-04/-13 run.

## Milestone 2 gate

T-M2-03 decides whether NTFS-3G runs on the macFUSE FSKit backend (risk R-01). Needs T-M1-04 finished,
the golden macOS 27 VM (T-M2-02) and the one-time GUI approval of the macFUSE extension (H3).
