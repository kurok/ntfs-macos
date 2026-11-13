# ntfs-macos

Native NTFS read/write support for macOS 27 and newer, built on mature open-source
components (NTFS-3G on the macFUSE FSKit userspace backend) rather than a from-scratch
filesystem.

> **Status: not started.** Nothing here is implemented or tested yet. Do not use this
> on a drive that holds data you care about.

## Goals

- Read/write NTFS on macOS 27+, Apple Silicon first, Intel where feasible
- Finder integration and standard mount/unmount, removable drives
- Conservative safety: read-only fallback for dirty, hibernated (Fast Startup) or unknown volumes
- No SIP changes, no replaced system binaries, minimal kernel involvement

See [docs/PROJECT_BRIEF.md](docs/PROJECT_BRIEF.md) for the full brief, milestones and
definition of done. Progress is tracked in [CHECKPOINT.md](CHECKPOINT.md) and
[NEXT_ACTIONS.md](NEXT_ACTIONS.md).

## Layout

| Path | Purpose |
|------|---------|
| `src/`, `include/` | Glue code, adapters, helpers |
| `integration/` | macOS integration (FSKit, Disk Arbitration, mount helpers) |
| `packaging/` | Installer, signing and notarization prep |
| `tests/` | `unit/`, `integration/`, `compatibility/`, `stress/`, `fixtures/` |
| `vm/` | Linux container and macOS VM test environments |
| `scripts/` | Build and dev tooling |
| `docs/` | Architecture, decisions, plan, test strategy, risks |
| `LICENSES/` | Third-party licenses (NTFS-3G is GPL-2.0; macFUSE terms to be reviewed) |

## License

To be decided after the licensing review in Milestone 1 (NTFS-3G's GPL-2.0 constrains
the options).
