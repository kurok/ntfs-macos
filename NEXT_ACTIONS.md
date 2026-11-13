# Next actions

## Human actions needed (details: docs/IMPLEMENTATION_PLAN.md section 5)

1. **H1** — approve Homebrew installs on the host: `cmake`, `ninja`, `pkgconf`, and the VM tool (tart, pending T-M1-04 licence check).
2. **H2** — start the Docker Desktop daemon (or approve Podman/colima) for the Linux environment.

## Agent tasks ready now (no human dependency)

| Task | What | Role |
|------|------|------|
| T-M1-01 | Licence files, SPDX policy, README licence section | C |
| T-M1-02 | `deps.lock` + checksum-verifying fetch script (re-confirm NTFS-3G SHA-256) | C |
| T-M1-06 | Independent audit of the six architecture docs; spot-check 15 sourced claims | E |

## Unblocked by H1/H2

- T-M1-03 CMake skeleton and presets (H1)
- T-M1-04 VM tool verification and wrapper (H1)
- T-M1-05 Linux image building pinned NTFS-3G (T-M1-02, H2)

## Milestone 2 gate

T-M2-03 decides whether NTFS-3G runs on the macFUSE FSKit backend (risk R-01). It needs the
golden macOS 27 VM (T-M2-02) and the one-time GUI approval of the macFUSE extension (H3).
