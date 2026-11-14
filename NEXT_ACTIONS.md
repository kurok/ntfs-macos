# Next actions

## Human actions needed (details: docs/IMPLEMENTATION_PLAN.md section 5)

1. **H1** — approve Homebrew installs on the host: `cmake`, `ninja`, `pkgconf`, and the VM tool (tart, pending T-M1-04 licence check).
2. **H2** — start the Docker Desktop daemon (or approve Podman/colima) for the Linux environment.

## Blocked on the owner

- **T-M1-04 networking:** allow Local Network for the desktop app (System Settings > Privacy & Security > Local Network), or run `vm/macos/tart.sh selftest` from Terminal.app.

## Agent tasks ready now

1. **Re-audit fixes, high first** (`docs/audits/M1-recheck.md`, Opus 5.5 for N-01..N-08):
   - N-01 clean-session decision must need positive records, not silence/exit 0 (ntfs-3g sets `err = 0` after `fuse_loop` unconditionally — verified).
   - N-02 grammar for every probe field; journal file names from the parsed serial; `reasons` as enum codes.
   - N-03..N-08 (volmark `$Volume`/`$MFTMirr` non-atomic write, U12 negative case, admin-peer boundary, `ntfs_fsync` already uses F_FULLFSYNC with silent fallback — verified, error-channel bounds, binding probed volume to opened device).
   - N-09..N-15 low items: Sonnet 5.5.
2. T-M1-07 minimal `scripts/ci-local.sh --quick` (Sonnet).

## Milestone 2 gate

T-M2-03 decides whether NTFS-3G runs on the macFUSE FSKit backend (risk R-01). Needs T-M1-04 finished,
the golden macOS 27 VM (T-M2-02) and the one-time GUI approval of the macFUSE extension (H3).
