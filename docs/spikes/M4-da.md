# M4 spike: Disk Arbitration watcher, dissenter and "no other mount" check (T-M4-02)

Date: 2026-11-16. Answers U7, U18, U21. Raw logs and scripts: `tests/results/evidence/m402/`.
Environment: one disposable clone of `ntfsmac-golden` (macOS 27.2 26B123, arm64), deleted at the end.
The tool is `tests/integration/daemon/da_watch.c`, built in the guest with `cc` against
`src/daemon/da.c`, run as root (`sudo -n`). It prints one line per event and never writes to a device:
its open tests open a descriptor and close it again. Disks, found by size and never by number:

| Disk | How attached | Size (bytes) |
|---|---|---|
| virtio disk with a bare NTFS file system (fixture `clean-256m-4k` plus 3 MiB of zeros) | `tart.sh run --disk`, present at boot | 271581184 |
| bare NTFS image (same fixture plus 1 MiB) | `hdiutil attach -imagekey diskimage-class=CRawDiskImage` | 269484032 |
| GPT image, one "Microsoft Basic Data" partition holding the fixture | same | 270532608 (partition 268435456) |

## Answers

**U7, is the mount-approval dissenter honoured on macOS 27.2: yes.** The module dissents from inside
the `disk_appeared` callback (the table lookup in the approval callback runs later on the same
serial queue). Five hot attaches with the dissent: four bare-NTFS images (`E3` three cycles, `E4`
once) and one GPT partition (`E6`). None was mounted by Apple's driver; `hdiutil attach` printed no device and the
image went away a few milliseconds later (`disappeared`); `hdiutil` evidently tears an image down
when its mount is refused. The control run without dissent (`E2`, `E5`) mounted every time: 18 ms to
390 ms after `appeared`, read-only, `ntfs`, `fskit`. On the persistent virtio disk (`E7`) the effect
is visible without the teardown: `diskutil mount disk0` while the dissent was active failed with
`Volume on disk0 failed to mount: "claimed by ntfsmacd"`, and no NTFS mount existed. Withdrawing the
dissent 14 s later, and calling `DADiskMount` in the withdrawal, gave the mount 70 ms later:
`/dev/disk0 on /Volumes/clean256m4k (ntfs, local, nodev, nosuid, read-only, noowners, noatime, fskit)`.
The fallback "let Apple mount, then unmount and remount" is not needed. Not tested: a real USB disk
(Environment C) and the race on a persistent disk at attach (a virtio disk cannot be hot-attached;
the image cases cover the race, the virtio case covers the persistent disk).

**U21, claim or unmount first: the claim works when the daemon is running at attach; an unmount
covers a disk that was mounted before.** The virtio disk was already mounted by `ntfs.fs` when the
tool started (`E0`: `DAVolumePath` present, `kind=ntfs`). `ntfsmac_da_unmount` (`DADiskUnmount`,
no force) succeeded 4 of 4 times, as root (`E1`, `E9a` to `E9c`). Nothing re-mounted the disk in the
25 s watched afterwards, and after the unmount both `open("/dev/disk0", O_RDONLY)` and `O_RDWR` worked.
The first unmount after boot took 10.7 s once (`E1`); the three later ones took 13 to 20 ms, and a plain
`diskutil unmount` took under a second. The cause of the first delay is not known; the module waits up
to 30 s. `ntfsmac_da_unmount` must not run on the Disk Arbitration queue (it waits for a callback
that queue delivers).

**U18, `DAVolumePath` and the `O_RDWR` open: `DAVolumePath` is present; the open fails only on the
buffered node.** With Apple's `ntfs.fs` mounted (`E0`, `E2`, `E5`):

| Open (as root) | `O_RDONLY` | `O_RDWR` |
|---|---|---|
| `/dev/diskN` (buffered) | `EBUSY` | `EBUSY` |
| `/dev/rdiskN` (raw) | ok | ok |

So the "no other mount" check must read the description. It must not rely on an open failing, and
the raw node gives no protection at all. `DAVolumeKind` (`ntfs`) is set from the first `appeared`,
before any mount (`volume_path=0 kind=ntfs`), so it does not show a mount. `DAVolumePath` appears
with the mount and goes away with the unmount (`mounted` and `unmounted` events, from the
description-changed callback). The description of a disk that was already mounted at the first
`appeared` already carries `DAVolumePath`; the module reports a `disk_mounted` for it right after
`disk_appeared`. Not tested: a disk mounted by another driver (none in the VM); the check looks only
at `DAVolumePath`, so it does not depend on which driver mounted.

## Filter facts (section 4.1 step 1)

- A whole disk with a GPT map is not a candidate (`DAMediaContent` `GUID_partition_scheme`); its
  partition `disk7s1` is (`E5`: `Microsoft Basic Data`, file system `NTFS`, `kind=ntfs`).
- A bare NTFS image or virtio disk (no partition map) is a whole disk and a candidate.
- A 6 MiB whole disk with no content and no volume kind (`disk5`, belongs to the guest) is also a
  candidate, because the rule "whole disk with empty content" is that broad. The probe refuses it. The
  dissent applies only after the daemon decides, so it costs nothing. If this turns out noisy, the
  filter can also require `volume_kind` `ntfs` for the whole-disk case, as `DAVolumeKind` is available
  at `appeared`; that is a one-line change in `da.c`.

## Implementation

- `src/daemon/da.c`: one serial dispatch queue runs all Disk Arbitration callbacks. Registered:
  mount approval (first, so there is no window), appeared, disappeared, and description-changed for
  `DAVolumePath` (gives `disk_mounted` and `disk_unmounted`). Dissent table of up to 128 BSD names, kept
  with a flag "a mount was refused". `ntfsmac_da_withdraw` removes the entry and, if a mount was
  really refused, calls `DADiskMount` so the system handles the disk (section 4.1 step 5). A disk the
  daemon mounted itself keeps its dissent until it disappears, so the daemon does not call
  `withdraw` for it. `unmount` and `eject` start the Disk Arbitration call and wait on a condition
  variable for the completion callback, 30 s at most; a late callback frees the waiter. Status mapping:
  busy to `E_BUSY`, not mounted to `E_STATE`, not permitted to `E_DENIED`, timeout to `E_TIMEOUT`.
- `src/daemon/da_filter.c`: the pure parts (filter, BSD name check, name table), unit tested on every host.
- Off macOS, `da.c` compiles to the "not implemented" stubs.
- `internal.h`: one field added, `ntfsmac_disk_info.volume_kind`. No signature changed.

## Tests

- `U-DMN-DA-01..04` (`tests/unit/da/`): filter, BSD names, table, argument checks. Run by `ctest`.
- The experiments above are manual runs of `da_watch` in a clone (not in `ctest`, they need
  `diskarbitrationd` and root). The end-to-end attach test (`I-DMN-01` and friends) needs the wired
  daemon (T-M4-08).

## Open points

- Real USB media: whether a USB disk is mounted before the daemon's callback can dissent (timing of
  `appeared` against the first mount attempt). The 18 ms best case of `E2` is the number to watch.
- A mount by another driver (Paragon), and the passive-mode interaction (T-M4-07).
- The 10.7 s first unmount.
