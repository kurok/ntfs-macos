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
| `LICENSES/` | Licence texts (REUSE layout), SPDX policy, macFUSE notice |

## Licence

All code in this repository is licensed under **GPL-2.0-or-later** (SPDX:
`GPL-2.0-or-later`); see [COPYING](COPYING). The choice follows NTFS-3G, which is
GPL-2.0-or-later and is linked into the probe and used as the worker (ADR-010 in
[docs/TECHNICAL_DECISIONS.md](docs/TECHNICAL_DECISIONS.md)). Binary releases must
offer the corresponding source, including the exact NTFS-3G tarball and patch
series. Mac App Store distribution is out of scope.

Every new source file starts with `SPDX-License-Identifier: GPL-2.0-or-later`.
Code under `third_party/` keeps its own licence. Details are in
[LICENSES/README.md](LICENSES/README.md).

**macFUSE notice.** macFUSE is a prerequisite that you install yourself. It is not
bundled with or downloaded by ntfs-macos, and it is not covered by this project's
licence. Its terms restrict bundling and automated installation of its binaries
with commercial software, and parts of it are closed source. Get it from the
official site, <https://macfuse.github.io/>. See
[LICENSES/LicenseRef-macFUSE-notice.txt](LICENSES/LicenseRef-macFUSE-notice.txt)
and ADR-013.
