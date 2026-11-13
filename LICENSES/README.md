# Licences and SPDX policy

This directory follows the [REUSE](https://reuse.software/) layout: one file per
licence, named by its SPDX identifier.

| File | Purpose |
|------|---------|
| `GPL-2.0-or-later.txt` | Licence of all code in this repository (ADR-010). Same text as `/COPYING`. |
| `LGPL-2.1.txt` | Reference text for macFUSE's libfuse, which is not shipped here. |
| `LicenseRef-macFUSE-notice.txt` | Notice that macFUSE is a user-installed prerequisite, not bundled (ADR-013), plus its licence text. |

## SPDX policy

- Every new source file starts with the identifier on its first comment line (after a shebang, if any):

  ```
  SPDX-License-Identifier: GPL-2.0-or-later
  ```

  Use the comment syntax of the language (`//`, `#`, `/* ... */`).
- Third-party code under `third_party/` keeps its own licence. Each vendored
  component carries its original licence file and headers unmodified, and its
  licence text is added here under its SPDX identifier (or `LicenseRef-<name>`
  if it has none). Do not add the project identifier to third-party files, and
  do not strip their notices. Patches we apply to them (for example NTFS-3G)
  are kept as patch files under their own upstream licence.
- Only components whose licence is compatible with GPL-2.0-or-later may be
  linked into a binary. macFUSE is never bundled or linked at build time from
  a vendored copy; it is detected at run time.
