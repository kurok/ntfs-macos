# macOS test VM (Environment B, tart)

Task T-M1-04. Wrapper: `tart.sh` (`pull`, `clone [--from golden]`, `run [--graphics]`, `exec`/`ssh`, `stop`, `delete`, `selftest`).
Golden VM with macFUSE (T-M2-02): `provision-golden.sh`, documented in `docs/VM_SETUP.md`.
Pins: `versions.env`. State (logs, pid files, scratch disks): `~/.cache/ntfsmac-vm`.
All VMs are named `ntfsmac-*`; the script never clones, stops or deletes anything else.

## Status: DONE (2026-11-14, `tart.sh selftest` exit 0)

| Item | Result |
|---|---|
| tart licence | Verified, free for this use (below) |
| macOS 27 image, pinned by digest | Verified, pulled (`-base`, see "Image") |
| Clone as disposable snapshot | Verified (1.4 s, 0 MiB host space) |
| Headless boot, guest reachable | Verified via `tart exec` (guest agent) in about 19 s |
| `sw_vers`, arch | macOS 27.2 (26B123), arm64 |
| FSKit | `FSKit.framework` present, `fskitd` running, built-in modules msdos, exfat, ftp; 0 system extensions |
| `--disk` attach | 256 MiB rw and 64 MiB `:ro` images appear as whole-disk block devices; the `ro` one reports "Media Read-Only: Yes" |
| Clone delete, base image kept | Verified |

Guest disk numbers are not stable: the 256 MiB scratch disk was `disk2` on one boot and `disk0` on the
next. Tools must find test disks by size or IORegistry identity, never by `diskN` number.

## Licence and cost (U11)

- tart 2.41.1, `brew info`: `FSL-1.1-ALv2`. Repo: https://github.com/openai/tart (moved from Cirrus Labs).
  LICENSE (https://raw.githubusercontent.com/openai/tart/main/LICENSE): Functional Source
  License 1.1, Apache-2.0 future licence, (c) OpenAI 2022-2026. Any purpose except a "Competing Use"
  (offering the software in a commercial product that substitutes for it) is permitted, including
  internal use. No usage caps, seat or core limits are stated; the tart README and docs
  (https://tart.run/quick-start/) state no price or limit. Running it on one Mac for open-source
  testing is a permitted purpose. Each release becomes Apache-2.0 after two years.
  Not legal advice; the text of LICENSE governs.
- Guest image: `ghcr.io/cirruslabs/macos-golden-gate-base` (macOS 27 = "Golden Gate"; published by
  https://github.com/cirruslabs/macos-image-templates: the vanilla image plus Homebrew and the tart guest
  agent). Only tag `latest`, which was the pinned digest on 2026-11-14 (uploaded 2026-11-07). The macOS guest itself is covered by the Apple macOS Software License Agreement
  (https://www.apple.com/legal/sla/), which allows running macOS in virtual machines on Apple-branded
  hardware (limit: two additional virtual copies per Mac for development/testing, per the SLA
  virtualisation clause). One host, one concurrent guest here. Not independently re-read in this task;
  confirm the clause wording before any distribution of images (none is distributed).
- Cost: zero. The base image is a 32 GB download (38 GB uncompressed disk). Host free space was 1.3 TB.

## Image and transport

- Repo `ghcr.io/cirruslabs/macos-golden-gate-base`, digest in `versions.env`.
- Default transport is `tart exec` through the tart guest agent over virtio; it needs no host-to-guest
  network. `NTFSMAC_TRANSPORT=ssh` uses SSH with the image's public test login (`versions.env`).
- The earlier pin, `macos-golden-gate-vanilla@sha256:b35695ce...a873`, has no guest agent, so only SSH
  works with it, and SSH is blocked from this app (next section).

## Clone as snapshot

`tart clone <digest ref> ntfsmac-x` took 0.80 s and changed host free space by 0 MiB (APFS clone).
Deleting the clone left the base OCI image in `tart list`. Selftest repeats this check.
Note: a clone is a snapshot of the pristine image only; there is no mid-run snapshot (tart has
`suspend` but T-M2-02 should instead build a golden VM by cloning from a stopped one).

## `--disk` options (from `tart run --help`, 2.41.1)

`--disk <path[:options]>`, repeatable. Path may be an image file, a block device, a remote VM name or an
NBD URL. Options, comma-separated: `ro` (read-only attach) and `sync=none` (skip syncing to permanent
storage: faster, data-loss risk). The default is read-write with synchronisation on. There is no other
cache mode. For durability tests use the default (sync on) and treat `sync=none` as the "lost write cache"
approximation. `tart.sh run` accepts only image files (refuses `/dev/*`) and passes the option string through.

## Why SSH does not work from the desktop app

Diagnosed 2026-11-14: the guest has a DHCP lease, ARP resolves its MAC on `bridge100`, the host route to
192.168.64.2 is via `bridge100`, the application firewall is off, yet ping and `nc` get no answer (earlier:
"No route to host"). The packets are dropped on the host before the bridge, which matches macOS Local
Network privacy for the app that runs the shell. SSH should work from a terminal app that has Local
Network permission; `tart exec` avoids the question entirely, so it is the default.

## Unknowns left

- Whether an attached image that holds a file system triggers a "disk not readable" dialog that blocks
  headless runs (the scratch images here are blank).
- tart `--net-softnet` (needs the softnet helper with root) is an alternative network path; untested.
