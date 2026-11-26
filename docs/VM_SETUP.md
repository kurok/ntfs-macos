# Golden macOS VM (Environment B)

Task T-M2-02. The golden VM `ntfsmac-golden` is the pinned macOS 27.2 base image
(`vm/macos/versions.env`) plus macFUSE 5.5.0, with its FSKit file system extension approved.
Test VMs are disposable clones of it. Tooling: `vm/macos/tart.sh` and `vm/macos/README.md`.

## Build it

```sh
vm/macos/provision-golden.sh            # new golden VM
vm/macos/provision-golden.sh --resume   # re-run on an existing one (idempotent)
```

The script, all of it inside the guest through `tart exec`:

1. Clones the pinned base image to `ntfsmac-golden` and boots it with a window (`tart.sh run --graphics`).
2. Downloads `macfuse-5.5.0.dmg` and checks its SHA-256 against `third_party/deps.lock`.
3. Requires the installer package to be signed by `Developer ID Installer: Benjamin Fleischer (3T5GSNBU6W)`
   and notarized (`pkgutil --check-signature`). The image has Gatekeeper disabled, so `spctl` alone
   would prove nothing.
4. Installs it with `sudo -n installer` (the base image has passwordless sudo for `admin`).
5. Builds `vm/macos/fskit-hello.c` in `~/ntfsmac` and mounts it with `-o backend=fskit`.
6. If the mount fails, opens System Settings at Login Items & Extensions and waits up to 15 minutes
   for a person to turn macFUSE on (human action H3).
7. Syncs and shuts the guest down cleanly.

Then: `vm/macos/tart.sh clone NAME --from golden`, `tart.sh run NAME`, `tart.sh exec NAME -- CMD`.

## Results (2026-11-14, macOS 27.2 26B123 guest, tart 2.41.1)

| Question | Result |
|---|---|
| macFUSE 5.5.0 download | SHA-256 `92c21ef2…3830` equals the release's `macfuse-5.5.0.sha256` and GitHub's asset digest |
| Signature | Developer ID Installer: Benjamin Fleischer (3T5GSNBU6W), notarized, ticket stapled |
| Installed parts | `/Library/Filesystems/macfuse.fs`; FSKit modules `io.macfuse.app.fsmodule.macfuse` and `…macfuse-local`; libfuse2 and libfuse3 in `/usr/local/lib`, headers in `/usr/local/include/fuse` and `fuse3` |
| U2: FSKit module runs in a VM | Yes. `fskit-hello` mounts as `macfuse://… on /private/tmp/hm (macfuse, nodev, nosuid, noowners, noatime, fskit, mounted by admin)`, reads back, unmounts with `umount` |
| Before approval | Mount fails: `MFMount(_:_:_:_:): File system extension not enabled`, `fuse: mount failed with error: 4` |
| U3: enable without GUI | No. `pluginkit -e use -i io.macfuse.app.fsmodule.macfuse[-local]` exits 0 and marks both `+`, but the mount still fails as above. The System Settings switch is required |
| Approval persistence | Survives reboot and `tart clone`: a clone booted headless mounts without any prompt |

## Network logins and the release build guest (T-M7-17)

The base image's login is the public `admin`/`admin` with passwordless `sudo`. Seen on 2026-11-26 in a
clone of the current `ntfsmac-golden`: Remote Login (`com.openssh.sshd`, TCP 22) and Screen Sharing
(`com.apple.screensharing`, TCP 5900) are on and listen on every address (`netstat -an`), and
`systemsetup -getremotelogin` says `On`. On the default shared (NAT) network anyone on this Mac, or
another VM on the vmnet bridge, who reaches the guest's address can log in and become root in it.

| Measure | Where | Applies to |
|---|---|---|
| No network: `tart.sh run --no-network` wires the guest's interface to a socket that a host process drains (`tart run --net-fd`). The guest gets no address (`tart ip`: none; only a self-assigned 169.254 address, no default route); `tart exec` still works (virtio). tart 2.41.1 has no "no network" option; `--net-softnet` restricts the guest's outgoing traffic but still lets it receive any incoming traffic, so it was not used | `vm/macos/tart.sh` | the release build clone (`scripts/release-local.sh`) |
| Remote Login, Screen Sharing, Remote Management and the Kerberos KDC (TCP 88, also listening on every address) off (`launchctl disable` and `bootout`), in the running clone only | `vm/macos/guest-login-check.sh --close` | the release build clone |
| Check: no TCP socket in the guest listens beyond loopback (any port), the host cannot connect to TCP 22, 5900 or 3283, a guest command that fails or prints nothing is an error, and with `--isolated` no address and no default route over a real interface (macOS keeps IPv6 default routes over its own `utun` tunnels; they do not count); the release build stops otherwise | `vm/macos/guest-login-check.sh` | the release build clone, before the build and before its result is copied |
| The same services off in the golden image | `vm/macos/provision-golden.sh` | the NEXT golden rebuild only; the current `ntfsmac-golden` is not changed |

Why not change the password instead: `dscl . -passwd` needs the old password in the guest, and a new
random one would have to be kept somewhere; with no listening service and no network there is
nothing that takes a password. The release build needs no network: the tree and the verified NTFS-3G
tarball are sent over `tart exec`. The verify clone of `release-local.sh` and the test clones keep the
shared network (some suites use it, for example the ssh transport); they produce nothing that is
published. Test: `tests/integration/packaging/release_guest_login_vm.sh` (I-REL-01) shows that the check fails on a
clone with the golden VM's defaults, fails with `--isolated` on a networked clone, passes after
`--close`, and fails again when Remote Login is turned on again on purpose (2026-11-26: PASSED). In
that run the host's `nc` could not reach port 22 although sshd listened in the guest (macOS Local
Network privacy for the calling app), so the in-guest listener check is the one that decides; the
host connection test is a second check for a host where it is allowed.

## Findings that affect later tasks

- **libfuse3 API on macOS.** macFUSE 5's `fuse3/fuse_common.h` sets `FUSE_DARWIN_ENABLE_EXTENSIONS=1`
  by default, which changes callback types (`struct fuse_darwin_attr` instead of `struct stat`,
  `fuse_darwin_fill_dir_t`). Code written for upstream libfuse3 must build with
  `-DFUSE_DARWIN_ENABLE_EXTENSIONS=0`. Relevant to T-M2-03b (libfuse3 port) and T-M2-03 (libfuse2 headers
  not checked yet).
- **Unclean stop loses guest writes.** Files written a few seconds before a plain `tart stop` were missing
  after the next boot. `tart.sh stop` now runs `sync; sudo /sbin/shutdown -h now` in the guest first.
  Durability tests must not treat `tart stop` as a clean shutdown; it is closer to a power cut.
- **Guest PATH under `tart exec`** lacks `/sbin` and `/usr/sbin`: use `/sbin/mount`, `/sbin/umount`,
  `/sbin/shutdown`, `/usr/sbin/installer`.
- **Disk numbers** in the guest change between boots (`vm/macos/README.md`); find test disks by size or
  identity.
- **Building NTFS-3G in a clone** needs no extra installs: `scripts/build-ntfs3g.sh --fuse external` finds
  macFUSE's libfuse2 through pkg-config in about 15 s (`tests/harness/run-env-b.sh` builds into
  `~/ntfsmac/deps`). macOS creates `.fseventsd` in the root of every mounted volume.
