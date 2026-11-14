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
