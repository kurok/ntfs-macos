# Next actions

## Human actions needed (details: docs/IMPLEMENTATION_PLAN.md section 5)

1. **H1** — approve Homebrew installs on the host: `cmake`, `ninja`, `pkgconf`, and the VM tool (tart, pending T-M1-04 licence check).
2. **H2** — start the Docker Desktop daemon (or approve Podman/colima) for the Linux environment.

## Blocked on the owner

- **T-M1-04 networking (diagnosed):** packets to the guest are dropped by Local Network privacy. Choose one: (a) allow Local Network for the desktop app (System Settings > Privacy & Security > Local Network); (b) switch to the `macos-golden-gate-base` image (32 GB pull) and use `tart exec` over the guest agent, no network needed; (c) run `vm/macos/tart.sh selftest` from Terminal.app.
- Then: T-M2-02 (golden VM + macFUSE, needs H3), T-M2-08 (harness, VM transport), T-M2-03 (backend gate).

## Agent tasks ready now

1. Run `scripts/ci-local.sh --quick` before every push (ADR-015).
2. Confirm the three assumptions recorded by the N-05/N-07/N-08 fixes (`/dev/fd/N` sharing, `POSIX_SPAWN_CLOEXEC_DEFAULT`, peer code-signing checks) when T-M4-02/-04/-13 run.

## Milestone 2 gate

T-M2-03 decides whether NTFS-3G runs on the macFUSE FSKit backend (risk R-01). Needs T-M1-04 finished,
the golden macOS 27 VM (T-M2-02) and the one-time GUI approval of the macFUSE extension (H3).
