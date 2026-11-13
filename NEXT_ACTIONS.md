# Next actions

## Human actions needed (details: docs/IMPLEMENTATION_PLAN.md section 5)

1. **H1** — approve Homebrew installs on the host: `cmake`, `ninja`, `pkgconf`, and the VM tool (tart, pending T-M1-04 licence check).
2. **H2** — start the Docker Desktop daemon (or approve Podman/colima) for the Linux environment.

## Agent tasks ready now (no human dependency)

Done: T-M1-01 (licences), T-M1-02 (deps lock, NTFS-3G digest confirmed), T-M1-06 (audit, `docs/audits/M1.md`).

1. **Apply audit fixes 1-4 before M2** (docs/audits/M1.md section 6): volume-label sanitising in the
   daemon, a defined way to clear the host journal, dirty marker (NTFS-3G never sets
   `VOLUME_IS_DIRTY`; make ADR-006 phase 2 the plan), and durability wording. Design change with
   data-loss impact: Opus 5.5.
2. **Apply audit fixes 5-7** (missing tests, claim wording, structure): Sonnet 5.5.
3. Independent re-check of the fixes by the audit role (Sonnet 5.5).

## Unblocked by H1/H2

- T-M1-03 CMake skeleton and presets (H1)
- T-M1-04 VM tool verification and wrapper (H1)
- T-M1-05 Linux image building pinned NTFS-3G (T-M1-02, H2)

## Milestone 2 gate

T-M2-03 decides whether NTFS-3G runs on the macFUSE FSKit backend (risk R-01). It needs the
golden macOS 27 VM (T-M2-02) and the one-time GUI approval of the macFUSE extension (H3).
