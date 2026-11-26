<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# ntfs-macos <label> (<kind: developer preview, release>)

<!-- Template for the release notes in docs/releases/<label>.md (ADR-015 amendment 2026-11-16).
     Fill every <...>; delete this comment. -->

Tag `v<label>` on commit `<12-hex>`, the commit the package was built from (`<40-hex>`, <date>).
GitHub <pre-release | release>.

## Files

| File | Content |
|---|---|
| `ntfs-macos-<label>.pkg` | Installer package, <unsigned (no Developer ID, not notarized) | signed and notarized>, arm64, macOS 27 or newer |
| `ntfs-macos-<label>-source.tar.gz` | The GPL source of exactly the tree that was built, with the pinned NTFS-3G tarball (`scripts/make-source-release.sh`) |
| `SHA256SUMS` | SHA-256 of the package, `uninstall.sh`, `manifest.txt` and the source archive, computed on the build host |
| `SHA256SUMS.sig` | Signature of `SHA256SUMS` with the release key (`scripts/sign-release.sh`) |
| `BUILD-INFO` | Source commit, source date and versions |

Package sha256: `<64-hex>`.

## Signing key fingerprint

`SHA256SUMS.sig` is signed with the key `<SHA256:...>` (ED25519), principal `ntfs-macos-release`,
the key in `packaging/release-signers` at this tag. Check it before installing:
[docs/INSTALL.md](../INSTALL.md), "Download and check". <If the key changed since the last release,
say so here and why.>

## Contents

<What is in the package.>

## Tested

<release-local.sh run (build in an isolated golden-VM clone, verify in a fresh clone), suites and their
result files.>

## Known limits

- <Unsigned: Gatekeeper blocks a double-click; install with `sudo installer` (see INSTALL.md).>
- Apple silicon and macOS 27 or newer only.
