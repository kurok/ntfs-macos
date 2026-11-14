# macOS test VM (Environment B, tart)

Task T-M1-04. Wrapper: `tart.sh` (`pull`, `clone`, `run`, `ssh`, `stop`, `delete`, `selftest`).
Pins: `versions.env`. State (logs, pid files, scratch disks): `~/.cache/ntfsmac-vm`.
All VMs are named `ntfsmac-*`; the script never clones, stops or deletes anything else.

## Status: PARTIAL. Guest SSH is blocked on this host (see "Blocked")

| Item | Result |
|---|---|
| tart licence | Verified, free for this use (below) |
| macOS 27 image, pinned by digest | Verified, pulled |
| Clone as disposable snapshot | Verified (0.8 s, no measurable space) |
| `--disk` options | Documented in `tart run --help` (below); guest-side attach not yet seen |
| Headless boot, VM gets a DHCP lease | Verified (`tart ip` returns 192.168.64.x) |
| SSH into guest, `sw_vers`, FSKit probe, `diskutil list` | NOT verified: host cannot reach guest IP |
| `tart.sh selftest` exit 0 | NOT reached; fails at "no SSH" |

## Licence and cost (U11)

- tart 2.41.1, `brew info`: `FSL-1.1-ALv2`. Repo: https://github.com/openai/tart (moved from Cirrus Labs).
  LICENSE (https://raw.githubusercontent.com/openai/tart/main/LICENSE): Functional Source
  License 1.1, Apache-2.0 future licence, (c) OpenAI 2022-2026. Any purpose except a "Competing Use"
  (offering the software in a commercial product that substitutes for it) is permitted, including
  internal use. No usage caps, seat or core limits are stated; the tart README and docs
  (https://tart.run/quick-start/) state no price or limit. Running it on one Mac for open-source
  testing is a permitted purpose. Each release becomes Apache-2.0 after two years.
  Not legal advice; the text of LICENSE governs.
- Guest image: `ghcr.io/cirruslabs/macos-golden-gate-vanilla` (macOS 27 = "Golden Gate"; published by
  https://github.com/cirruslabs/macos-image-templates, "vanilla macOS installation with auto-login, no
  additional software"). The registry tags are `latest`, `27.2`, `27.0`; `latest` and `27.2` are the
  pinned digest. The macOS guest itself is covered by the Apple macOS Software License Agreement
  (https://www.apple.com/legal/sla/), which allows running macOS in virtual machines on Apple-branded
  hardware (limit: two additional virtual copies per Mac for development/testing, per the SLA
  virtualisation clause). One host, one concurrent guest here. Not independently re-read in this task;
  confirm the clause wording before any distribution of images (none is distributed).
- Cost: zero. The image is a ~30 GB download, 50 GB on disk (sparse, ASIF). Host free space was 1.4 TB.

## Image

- Repo `ghcr.io/cirruslabs/macos-golden-gate-vanilla`, digest in `versions.env`, tag at pin 27.2.
- Login is the image's public test login (https://tart.run/quick-start/), stored in `versions.env`.
- No tart guest agent in vanilla, so `tart exec` does not work; SSH only. (The `-base` image has the agent
  and brew, but is not used.)

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

## Blocked: host cannot reach the guest

Observed: guest boots headless, DHCP lease appears in `/var/db/dhcpd_leases` (192.168.64.2, MAC matches
the VM), `tart ip` returns it, `bridge100` (192.168.64.1) answers ping, but `ping` and `nc -z 192.168.64.2 22`
from the Bash tool and from the app's Terminal panel both fail at once with "No route to host", for 240 s.
Guest CPU is idle. No sudo, no packet capture was available (`/dev/bpf*` is root-only).

Most likely cause (not proven): macOS Local Network privacy blocks the desktop agent app (the responsible
process of both shells) from non-loopback LAN addresses including the vmnet bridge. Needs a human to check
System Settings > Privacy & Security > Local Network for the app, or run the selftest from Terminal.app /
iTerm once to see whether the prompt appears. A launch of Terminal.app by the agent was denied by the
permission classifier and not retried. Other possible causes: OpenVPN full tunnel on the host (utun routes
present), guest first-boot state. Tart's own install caveat (shorter DHCP lease via `sudo defaults write`)
was skipped as instructed; only needed for many VMs a day.

## Unknowns left

- Everything guest-side: `sw_vers`/arch, FSKit presence (`systemextensionsctl list`,
  `pluginkit -m -A -p com.apple.fskit.fsmodule`), extra-disk device path, `ro` flag effect.
  `tart.sh selftest` prints all of them once SSH works.
- T-M2-02: macFUSE install and extension approval in the guest, whether it works without the GUI
  (vanilla has auto-login; GUI access is via `tart run` without `--no-graphics`, or `--vnc`).
- Whether a raw image attached with `--disk` shows up without a "disk not readable" dialog blocking headless runs.
- tart `--net-softnet` (needs the softnet helper with root) is an alternative network path; untested.
