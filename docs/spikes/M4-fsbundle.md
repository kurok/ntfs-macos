# M4 spike: legacy `/Library/Filesystems/<name>.fs` bundle for `diskutil mount` (T-M4-09)

Date: 2026-11-17. Question: can a legacy file system bundle give `diskutil mount`, Disk Utility and
Finder "Mount" support for NTFS volumes with our worker, and what does it do to Apple's `ntfs.fs`, to the
daemon's automount, to the policy "a dirty or hibernated volume is never mounted rw", and to signing?
ADR-003 chose the LaunchDaemon and left the bundle to this spike.

Environment: one disposable clone of `ntfsmac-golden` (macOS 27.2 26B123, arm64, macFUSE 5.5.0), deleted
at the end; `ntfs-3g 2026.9.28` built in the guest with `scripts/build-ntfs3g.sh` (the patch series, so
patch 0003 is in). Test disks are image files attached with `hdiutil attach` (with and without
`-nomount`): bare NTFS images (64 MiB, `mkntfs`) and one GPT image with a "Microsoft Basic Data"
partition holding the same file system. Every ntfs-3g mount used `norecover`; none used `recover`,
`remove_hiberfile` or `fsname`. Nothing was attached or installed on the host. Scripts:
`tests/spikes/fsbundle/mkbundle.sh` (builds the experimental bundle in the guest) and
`tests/spikes/fsbundle/scenario.sh` (one attach, mount, write, unmount round). Guest paths below are
`<home>` for the guest user's home.

## Recommendation

**Do not adopt a legacy bundle for v1; keep ADR-003 as it is (daemon with Disk Arbitration callbacks).**
A bundle can be made to work on macOS 27.2: `diskutil mount diskN` and automount both reached our helper
and produced a read/write FSKit mount of the pinned worker. But the helper runs as the console user in a
context where the engine refuses to mount a block device and where the mount cannot be waited for; it
bypasses the probe, the policy, the journal and the binding (the engine alone mounted a volume with
`VOLUME_IS_DIRTY` set read/write); with the daemon's dissenter active the bundle is never reached at all;
and the mechanism is the legacy one Apple moved away from. A usable version would need a root IPC from the
helper to the daemon, which makes the daemon the mount path again and the bundle only a trigger. The cost
and the new attack surface (a root-run probe on every attach, a helper reachable by any local user) buy
one thing: `diskutil mount diskNsM` works while the daemon would otherwise refuse it. That is better
served by a clearer dissent message and the `ntfsmac mount` command (see "If the owner wants `diskutil
mount` anyway").

## Findings

### F1. What the guest has

`/System/Library/Filesystems` is `restricted` (SIP); `/Library/Filesystems` is `root:wheel 755`, flag `sunlnk`
only. The guest holds `macfuse.fs` there (Team `3T5GSNBU6W`, hardened runtime on its helper, 13 personalities,
one `NTFS-3g` with `FSSubType 8`; it has no `FSMediaTypes`, so it takes no part in probing). Apple's
`ntfs.fs` (3.14.3, `FSImplementation` `UserFS`) declares three `FSMediaTypes`, all with
`FSProbeExecutable ntfs.util` and `FSProbeArguments -p`:

| Media type | `FSProbeOrder` |
|---|---|
| `Windows_NTFS` (hint of an MBR or GPT partition) | 1000 |
| `EBD0A0A2-B9E5-4433-87C0-68B6B72699C7` (GPT "Microsoft Basic Data") | 2000 |
| `Partitionless` (whole disk, no map) | 4000 |

and one personality `NTFS` with `FSMountExecutable mount_ntfs`. There is no `/sbin/mount_ntfs`; the mount
is done through FSKit/userfsd (`ntfs ... fskit` in the mount table, read-only). `diskutil info` on an
unmounted bare image shows `Type (Bundle): ntfs`. The probe protocol, from running `ntfs.util` by hand:
`ntfs.util -p disk6 removable readonly`, volume label on stdout, exit status 255 (-1, recognised) or 254
(-2, not recognised); DA also calls `-k disk6` (UUID key) after a positive probe.

### F2. A minimal bundle is picked up at once, and it wins the probe

`mkbundle.sh` makes `/Library/Filesystems/ntfs3g.fs` with `Info.plist` (media types above, one personality
`NTFS-3G`, `FSMountExecutable mount_ntfs3g`), a probe script and a mount script. No reload and no reboot:
the next attach probed with it (`PROBE uid=0 argv: -p disk6 removable readonly`; DA runs the probe **as
root**). Findings that matter:

- The probe order did **not** decide the contest with Apple's driver. Orders 1, 3999 and 5000 all won
  against Apple's 1000, 2000 and 4000 (`diskutil info`: `Type (Bundle): ntfs3g`, helper run once each).
  A bundle without `FSImplementation` and one with `kext` both won. This agrees with ARCHITECTURE.md 8.1:
  non-FSKit bundles are probed before UserFS/FSKit ones. Order matters only between two non-Apple legacy
  bundles; that contest (our bundle against Paragon's order 1) was **not** tested.
- For a GPT partition the bundle must also declare the GUID media type; with only `Windows_NTFS` and
  `Partitionless` it was never probed and Apple mounted the partition read-only. With the GUID key added
  our helper mounted the partition (`ntfs3g`, `macfuse` mount).
- A probe that exits 254 hands the disk to the next claimant: with a "decline" flag set, Apple's `ntfs.fs`
  mounted the volume read-only (`ntfs, local, ... read-only ... fskit`). So a bundle can step aside per
  volume; with the bundle removed, Apple's read-only automount is back (baseline).
- `DAVolumeKind` of a claimed disk becomes `ntfs3g` instead of `ntfs` (visible in `da_watch`:
  `kind=ntfs3g`). The daemon's filter and conflict check would have to know our own bundle's kind and
  name, and `doctor`'s check for "another third-party NTFS bundle" (ARCHITECTURE.md 8.4) must exclude it.

### F3. The `FSImplementation` key decides whether DA calls the mount helper at all

On macOS 27.2, `diskarbitrationd` has a second path for file systems that can run as user-space file
systems: `DAUserFSMount`, which talks to `userfsd` and the `UVFSService` (strings in the daemon:
`DAEnableUserFSMount{External,Internal,Removable}`, `FSImplementation`, `UserFS`, `kext`). The mount of a
claimed disk was tried through it first:

| Bundle | Result of `diskutil mount` or automount |
|---|---|
| `FSImplementation` = `["UserFS"]` or `["UserFS","kext"]` | Fails: `UVFSService ... fsname:ntfs3g:not:supported`, DA `mount call returned 0x1001`, `Volume on disk6 failed to mount`. The helper is never run. `hdiutil attach` fails with "no mountable file systems" and the image is detached. There is no fallback to Apple's driver after a failed UserFS mount |
| no key, or `["kext"]` | DA runs `mount`, which runs our `mount_ntfs3g`. |

A claim with a UserFS bundle that cannot actually mount is therefore a black hole for the volume. (The
early runs of this spike showed the same failure for a bundle that had had its key removed in place with
`plutil`; DA had cached the plist, and recreating the bundle directory gave the table above. An installer
must replace the bundle directory, not edit the plist.) I also tried setting the three
`DAEnableUserFSMount*` preferences to `false` and restarting DA; it was not needed once the key was
right, and the final runs used the default preferences. Changing those preferences is a system setting
and out of the question for the product.

### F4. What the mount helper receives and where it runs

`diskutil mount disk6` (and automount) ran the helper as:

```
MOUNT uid=501 argv: -o nodev -o noowners -o nosuid /dev/disk6 /Volumes/NTFSCLEAN
PATH=/usr/bin:/bin:/usr/sbin:/sbin     (environment otherwise empty)
```

- It runs as the **console user** (uid 501), not as root, in both the `diskutil mount` and the automount
  case. (The first runs "mounted" a volume that did not exist because the helper could not append to a
  root-owned log; see F5.) With no console user the uid is unknown: not tested.
- It receives no `rw`/`ro` choice and no policy; the mount point `/Volumes/<label>` has been made by DA.
- `ntfs-3g` refused to run there: `Unprivileged user can not mount NTFS block devices using the external
  FUSE library`. Run as root, the FSKit backend daemonised and never completed the mount (the open
  question U6 of ADR-004, root-run FSKit mounts). Only the product's own shape worked: open the device,
  pass `/dev/fd/N` (patch 0003), run the worker as the console user. In this guest the opener could be the
  helper itself only because an image's `/dev/diskN` node is owned by the attaching user
  (`brw-r----- admin staff`). A USB disk's node is root-owned (ADR-004), so on real hardware the helper cannot
  open it and would need a root helper or an IPC to the daemon.

### F5. The helper cannot wait for the mount, and DA's accounting is wrong when it fails

The mount of the volume is a second mount: the macFUSE FSKit backend creates its own disk (`disk7`, later
`disk8`, a GPT image with one partition; our probe saw it: `PROBE disk8 not ntfs`) and DA mounts that
partition (`/dev/disk8s1 on /Volumes/NTFSCLEAN (macfuse, ... fskit, mounted by admin)`). While the helper
waited for the mount to appear, the worker reported `Mounted /dev/fd/9 (Read-Write ...)` but
`MFMount: Failed to initialize volume`. The likely cause, not proven, is that DA is busy running our helper and cannot serve the nested mount.
The helper timed out after 10 s and `diskutil mount` failed. The same worker started the same way from a
shell, outside a DA call, mounted at once. When the helper started the worker in the background and
returned 0 immediately, everything worked: the volume appeared two to three seconds after
`Volume NTFSCLEAN on disk6 mounted`, so for those seconds `diskutil` says "mounted" and the directory is
empty. Two consequences:

- Success is reported before it exists. With a stand-in helper that exited 0 and started nothing,
  `diskutil mount` printed `Volume NTFSCLEAN on disk6 mounted` and `diskutil info disk6` said
  `Mounted: Yes, Mount Point: /Volumes/NTFSCLEAN`, while `mount` listed nothing. A real worker that fails
  after the helper returned would leave the same false state, and the daemon has no event to react to.
- DA sees the disk `diskN` as mounted at `/Volumes/<label>`, while the real mount is of `diskMsK` (macFUSE).
  `diskutil unmount disk6`, `diskutil unmount /Volumes/NTFSCLEAN`, `diskutil eject disk6` all worked in the
  test (the worker exited, the macFUSE disk went away, `diskutil info disk6` said `Mounted: No`), and a
  second `diskutil mount disk6` worked. Not tested: Finder eject itself (no GUI session), and a write
  followed by a hard stop.

### F6. Safety: the bundle bypasses the policy, and the engine alone does not enforce it

The helper had a refusal rule built only from the engine's own probe (`ntfs-3g.probe --readwrite`, then
`--readonly`), then `norecover`, `rw` or `ro`:

| Image | Helper decision | Result |
|---|---|---|
| clean | `rw` | mounted `Read-Write`; write ok |
| `hiberfil.sys` with `hibr` (made through the worker, 8 KiB) | `ro` (`Windows is hibernated, refused to mount.`) | mounted `Read-Only`; `touch` gives `Read-only file system` |
| `$Volume` flags `0x0001` (`DIRTY`, from `ntfsfix`; `ntfsinfo` confirms) | `rw` | **mounted `Read-Write`**, write ok |

The dirty-flag volume was mounted read/write with `norecover`, because the engine only refuses an unclean
`$LogFile`, not the flag (the T-M3-01 finding: NTFS-3G neither sets nor honours it as a mount gate; the
policy in ADR-005 item 1 gets "dirty_flag gives ro" from `ntfsmac-probe`, not from the engine). So the
helper must not decide anything itself; it can only ask the daemon, which means the probe, the policy
table, the journal (ADR-006), the device binding (ADR-020), the worker spawn through `spawn.c` and its
sandbox (ADR-004, T-M4-06) and the mount-flag comparison of ARCHITECTURE.md 4.1 step 8 all stay in the
daemon. A worker started from a DA helper is outside all of that: the daemon would see an `ntfs-3g` mount
it did not start (a case for `reconcile.c`, whose worker re-adoption is still open and which force-unmounts today).

### F7. Interaction with the daemon

With `da_watch --dissent` (the daemon's DA module, T-M4-02) running and the bundle installed:

- Automount attach: dissent at once, `hdiutil attach: failed - no mountable file systems`, image gone.
- `hdiutil attach -nomount` then `diskutil mount disk6`: `Volume on disk6 failed to mount: "claimed by
  ntfsmacd"`. **The bundle's mount helper was not run**; its probe was (as root, three times: at attach, at
  the mount call, and the UUID key).

So while the daemon is active and dissenting, the bundle does nothing for `diskutil mount`. For it to work
the daemon would have to withdraw its dissent for exactly the mounts a user asked for (it cannot tell
`diskutil mount` from the automount at the approval callback) or stop dissenting altogether, giving up
the claim that keeps Apple's mount and the binding exclusive (ADR-003 consequences, ADR-020). The two
mechanisms are alternatives, not layers.

Each attach also runs the bundle's probe as root before the daemon decides. In the experiment it read one
sector. Any production probe would be a second root-run parser of attacker-controlled bytes next to
`ntfsmac-probe`, which ADR-004 keeps unprivileged and sandboxed; to stay harmless it would have to be a
boot-sector magic check only.

### F8. Security and signing on macOS 27

What the VM can and cannot show: **SIP is disabled and assessments are disabled in the golden VM**
(`csrutil status`, `spctl --status`), so it cannot prove what is enforced on a normal Mac. Observed:

- DA ran the probe script as root from a bundle whose directory and scripts were owned by `admin:staff` and
  mode 777 (`ls -ld` shown; probe `uid=0`). There is no ownership or signature check on the bundle's
  executables. A bundle must be `root:wheel`, not group- or world-writable, or it is a local root escalation;
  `/Library/Filesystems` itself is root-owned, so only root can add one.
- Unsigned scripts ran as helpers. On a SIP-enabled Mac the same is expected for `/Library/Filesystems`
  (not SIP-protected, unlike `/System/Library/Filesystems`) but that was not shown. A real bundle should
  ship a signed Mach-O helper under the hardened runtime, as `macfuse.fs` does (Team ID, `flags=runtime`),
  and be notarised with the installer.
- Non-FSKit (`kext`) legacy bundles are the form that Paragon uses on this host, and macOS 27 keeps both the
  key and the DA path. Apple documents the direction (FSKit; `mount -F` for developers moving off a Filesystems
  bundle, S17). Whether a later release drops the path is unknown [U]; the code in DA 27.2 treats `UserFS` first.
- DA runs the helper through `mount` as the logged-in user, so any local user can start it by attaching a disk or
  running `diskutil mount`; a root IPC from it would need a new authorisation class (ADR-017 and ADR-019 authorise
  by signed CLI and administrator password, which the DA helper path has neither of).

## What was not tested

- A real USB disk and a root-owned `/dev/diskN` (Environment C); a disk with no console user (uid of the helper);
  a second legacy bundle at the same probe order (Paragon); Disk Utility and Finder themselves (no GUI session;
  both call `DADiskMount`, as `diskutil mount` does, so the same route is expected [A]); data durability across a
  helper-started session; SIP-enabled behaviour (see F8).

## If the owner wants `diskutil mount` anyway

Smallest honest design, only to price it: a bundle with no `FSImplementation` (or `kext`), media types for
`Windows_NTFS`, the GUID partition type and `Partitionless`, a signed probe that recognises the boot-sector
magic and exits 254 whenever the daemon is not running or has declined, and a signed mount helper that is
only a client of a new daemon request ("mount BSD name"): the daemon does probe, policy, binding, spawn and
journal, and the helper returns 0 only after `getfsstat` shows the macFUSE mount (it cannot, F5, unless the
daemon answers first and the helper returns before the mount appears, which brings back false success).
Required changes: daemon filter and conflict check for kind `ntfs3g`, withdrawal of the dissent for a
helper-initiated mount, a new peer class for the DA helper, installer ownership checks, tests with a real USB
disk. That is a new milestone, not a spike.

Cheaper and already available: the daemon's dissent text appears verbatim in `diskutil` (`failed to mount:
"claimed by ntfsmacd"`). Making it say what to do (for example `claimed by ntfsmacd; use: ntfsmac mount
<disk>`) costs a string and tells the user why `diskutil mount` refuses.

## Proposed amendment to ADR-003 (not applied)

Append to **Decision**: "The legacy bundle was evaluated in T-M4-09 (`docs/spikes/M4-fsbundle.md`) and is
rejected for v1. On macOS 27.2 a bundle without a UserFS implementation can intercept `diskutil mount`
and automount and runs a mount helper as the console user, but the helper cannot open a root-owned
device, cannot wait for the FSKit mount (probably because the nested mount needs Disk Arbitration, which is busy running the
helper), makes DA report a mount that may not exist, bypasses the probe and policy (the engine mounted a
`VOLUME_IS_DIRTY` volume read/write), and is never reached while the daemon's dissenter is active."

Append to **Consequences**: "`diskutil mount diskNsM` for an NTFS volume fails with the dissenter's text while
the daemon is active; the dissent text names `ntfsmac mount` (ARCHITECTURE.md 8.3). No bundle is installed in
`/Library/Filesystems`; the conflict check of section 8.4 keeps treating any non-Apple NTFS bundle there as
a reason for passive mode."
