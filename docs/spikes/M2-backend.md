# M2 backend spike: NTFS-3G on macFUSE libfuse2 with the FSKit backend (T-M2-03)

Date: 2026-11-14. Answers U1 / R-01. Raw logs: `tests/results/evidence/m203/`.

## Answer

**U1: yes.** NTFS-3G 2026.9.28 built against macFUSE 5.5.0's libfuse2 (FUSE API 29) mounts an NTFS
image file read/write with `-o backend=fskit` as the normal user `admin`, without root and without a
patch. Evidence (`smoke-run1.log`):

```
/dev/disk6s1 on /private/var/folders/.../ntfsmac-smoke.6eHgCN (macfuse, local, nodev, nosuid, noowners, noatime, fskit, mounted by admin)
```

libfuse2 did not reject `backend=fskit`, so there is no error text to quote and T-M2-03b (libfuse3
port) is not triggered by this result. Patch 0001 is not needed: the series stays empty.

Two caveats the answer depends on (details below): the mount only works for **image files**
(an attached block device is refused for an unprivileged user by NTFS-3G itself), and **without
`backend=fskit` the mount silently never appears**.

## Environment

| Item | Value |
|---|---|
| Host | macOS 27.2.1 (26B131) arm64, tart 2.41.1 |
| Guest | clone of `ntfsmac-golden`: macOS 27.2 (26B123) arm64, kernel 27.2.0 VMAPPLE, 4 CPUs |
| Compiler | Apple clang 21.0.0 (clang-2100.4.1.1), Command Line Tools, configure picked `gcc -std=gnu23` (the CLT `gcc` is clang) |
| macFUSE | 5.5.0, `/usr/local/lib/libfuse.2.dylib` (current version 12.9.0), `fuse.pc` says Version 2.9.9, headers in `/usr/local/include/fuse` |
| NTFS-3G | 2026.9.28 (`third_party/deps.lock`), no patches. `ntfs-3g --version`: `ntfs-3g 2026.9.28 external FUSE 29` |
| Fixture | `clean-256m-4k` exported from Environment A (`tests/fixtures/gen/clean.sh --only clean-256m-4k --export build/fx-m203`), treedump hash `b681baf0...3333f7` as in the manifest |

## Build (criterion 2.1: zero errors)

`scripts/build-ntfs3g.sh --fuse external --jobs 4 --build-dir ~/build/ntfs-3g --prefix ~/build/deps`, run in the
guest from a tar stream of the repo. No change to the script was needed: `pkg-config` is
`/opt/homebrew/bin/pkg-config` (on the `tart exec` PATH) and macFUSE's `fuse.pc` is in
`/usr/local/lib/pkgconfig`, which the script already puts in `PKG_CONFIG_PATH`. Nothing was installed in
the guest. The whole build takes about 12 s.

Configure line (full log `build-ntfs3g.log`):

```
PKG_CONFIG_PATH=/usr/local/lib/pkgconfig .../configure --prefix=$HOME/build/deps --exec-prefix=$HOME/build/deps \
  --with-fuse=external --enable-static --disable-shared --enable-ntfsprogs --disable-ldconfig \
  --disable-mount-helper --disable-plugins
```

Compile flags seen: `-DFUSE_USE_VERSION=26 -I/usr/local/include/fuse -D_FILE_OFFSET_BITS=64 -g -O2 -Wall`.
`ntfs-3g` links `/usr/local/lib/libfuse.2.dylib`, libicucore and CoreFoundation only.

Result: 0 errors, 17 warning lines (file: count and kind):

| Warning | Where | Count |
|---|---|---|
| `-Waddress-of-packed-member` | collate.c, dir.c, unistr.c | 5 |
| `-Wuninitialized-const-pointer` (`dummy`) | ea.c, object_id.c, reparse.c | 3 |
| `-Wunused-but-set-variable` | attrib.c, utils.c (x2 each) | 4 |
| `-Wdeprecated-declarations`: `daemon()` deprecated since macOS 10.5 | ntfs-3g.c, lowntfs-3g.c | 2 |
| `-Wsometimes-uninitialized` (`ni`) | ntfs-3g.c | 1 |
| `#warning Trimming not supported : FITRIM or BLKDISCARD not defined` | ioctl.c | 1 |
| `-single_module is obsolete` (linker flag probe, configure) | configure | 1 |

All are upstream code warnings. The `daemon()` use is what makes `ntfs-3g` return before the mount is visible
(see "Mount behaviour"). `mkntfs`, `ntfsclone`, `ntfslabel`, `ntfsresize`, `ntfscp`, `ntfsundelete` install in
`$prefix/sbin`, the other ntfsprogs (including `ntfsfix`) in `$prefix/bin`. The same build works through
`run-env-b.sh` (`env-b-guest-build.log`).

## The mount as a normal user

Working command (uid 501, gid 20, no sudo):

```
ntfs-3g -o backend=fskit,local,norecover,uid=501,gid=20 IMAGE MOUNTPOINT
```

### Option variations and results

Every variation below exited 0 from `ntfs-3g`; the question is whether a mount appeared (`option-variants.log`,
`option-variants-foreground.log`). The image was a fresh copy each time.

| `-o` (after `uid=501,gid=20`, `norecover` always present) | Result |
|---|---|
| `backend=fskit,local` | mounts, `/dev/disk6s1 on ... (macfuse, local, nodev, nosuid, noowners, noatime, fskit, mounted by admin)`, files are `-rwxrwxrwx admin:staff` |
| `backend=fskit` (no `local`) | mounts, source is `macfuse://41243B74-...` instead of `/dev/disk6s1`, no `local` flag. Use `local` |
| `backend=fskit,local,noatime,windows_names,big_writes,streams_interface=xattr,fmask=0133,dmask=0022` (the ARCHITECTURE.md 3.3 set) | mounts, all accepted, files `-rw-r--r--`, dirs `drwxr-xr-x`: `fmask`/`dmask` take effect |
| `backend=fskit,local,noowners` / `allow_other` / `volname=NTFSMAC` / `streams_interface=windows` / `sync` / `noappledouble` | each mounts, no error. Whether each has an effect is T-M2-04's job (`volname` did not change the mount's name in `mount` output) |
| `local` only (no `backend`) | **no mount, no error.** `ntfs-3g` exits 0, the daemon stays alive, `mount` shows nothing after 10 s and the daemon ignores `SIGTERM` (needed `kill -9`) |
| `backend=bogus,local` | same as above: the value is not validated, behaves like no `backend` |

Foreground run (`-o no_detach`, which this NTFS-3G has; `-f` is "Unknown option"): without `backend=fskit`,
NTFS-3G itself reports success and then waits forever:

```
Mounted /Users/admin/fx/var.img (Read-Write, label "clean256m4k", NTFS 3.1)
Mount options: local,allow_other,nonempty,relatime,default_permissions,fsname=/Users/admin/fx/var.img
```

So NTFS-3G adds `allow_other`, `nonempty`, `default_permissions` and `fsname=<device>` to the options it hands
to libfuse. These come from NTFS-3G, not from our command line, and do not conflict with the banned `fsname` rule
(which is about what the daemon passes), but T-M2-04 should check that `argv_check()` is not confused by them if it
ever reads them back. Because the default backend hangs without any message, **the worker must treat "no mount
within N seconds" as failure and must always pass `backend=fskit`.** Mount waiting needs polling anyway:
`ntfs-3g` calls `daemon()` and returns 0 before the mount shows up (a few hundred ms later).

### I-SMOKE-01

`tests/integration/smoke/mount_smoke.sh` (POSIX sh, shellcheck clean) runs in the guest as `admin`: rw mount, checks
`mount` output for `fskit` and no `read-only`, mkdir, small write/read, 1 MiB random file with SHA-256, rename,
append, Unicode name (`café-日本.txt`), create+delete, stat size, `sync`, `/sbin/umount`, wait for the daemon to
exit, read-only remount (checks `read-only` in `mount`, data intact, deleted file stays deleted, a write is
refused), unmount, `ntfsfix -n`. 28 checks per run. A **fresh copy of the image for each run** was used
(streamed from the host with `tart exec -i`).

| Run | Result | Log |
|---|---|---|
| 1 | PASS (28/28, `ntfsfix -n` exit 0) | `smoke-run1.log` |
| 2 | PASS (28/28) | `smoke-run2.log` |
| 3 | PASS (28/28) | `smoke-run3.log` |

All three in the same clone (booted once). `ntfsfix -n` output: `Mounting volume... OK`, `$MFT and $MFTMirr completed
successfully`, `Checking the alternate boot sector... OK`, `NTFS volume version is 3.1`.

## Attached disk (input for T-M2-05)

`tart.sh run m203 --disk IMAGE` with the same 256 MiB image (`block-device.log`). Findings:

- The guest sees it as `/dev/disk1` (whole disk, no partition table, found by the size 268435456 bytes, not by
  number; the number would have been 1 here and differs between boots).
- **macOS mounts it by itself, read-only, with its own driver:**
  `/dev/disk1 on /Volumes/clean256m4k (ntfs, local, nodev, nosuid, read-only, noowners, noatime, fskit)`.
  `/System/Library/Filesystems/ntfs.fs` exists and is an FSKit module. This holds the device
  (`ntfsfix` as root: `Error opening '/dev/disk1': Resource busy`). It has to be unmounted first;
  `diskutil unmount /dev/disk1` worked as `admin` without sudo. T-M2-05 and the daemon must handle this
  automount (DiskArbitration claim) in every attach.
- Device nodes are `brw-r----- root:operator` (`/dev/disk1`) and `crw-r----- root:operator` (`/dev/rdisk1`).
  `admin` is not in `operator`: `dd if=/dev/disk1` and `/dev/rdisk1` give `Permission denied`; `ntfsfix -n /dev/disk1`
  gives `Error opening '/dev/disk1': Permission denied`.
- **NTFS-3G refuses before it even tries to open it**, for any block device as non-root with the external FUSE
  (`src/ntfs-3g.c`, `if (getuid() && ctx->blkdev)`, under `#ifndef FUSE_INTERNAL`, line 4449 of the pinned source):

  ```
  Unprivileged user can not mount NTFS block devices using the external FUSE
  library. Either mount the volume as root, or rebuild NTFS-3G with integrated
  FUSE support and make it setuid root. Please see more information at
  https://github.com/tuxera/ntfs-3g/wiki/NTFS-3G-FAQ
  ```
  exit status 19. `ctx->blkdev` is set when `stat()` of the path says `S_ISBLK`; a `/dev/fd/N` path to a block
  device is likely to hit this too, because `stat` follows it. When `blkdev` is set NTFS-3G also adds
  `,blkdev,blksize=N` to the FUSE options (`set_fuseblk_options`), which macFUSE may or may not accept. Both
  facts are for T-M2-05: patch 0003 may have to cover this check, not only the open. The only root command run was one
  `sudo -n ntfsfix -n /dev/disk1` (check only), which showed the device busy because of the automount; a root
  `ntfs-3g` mount of the block device was not tried (T-M2-05).

So the answer for the attached-disk path is: a normal user cannot open `/dev/diskN` and NTFS-3G would refuse
a block device even if it could.

## Harness: I-INT-01 and I-INT-03 in Environment B

`tests/harness/volume.py` has a new `MacosNtfs3g` adapter (mirrors the Linux one: image file, `mkntfs -F -Q`,
`ntfs-3g -o backend=fskit,local,norecover,uid,gid[,ro]`, polls `mount` for the fskit mount line, `sync` +
`/sbin/umount`, waits for the driver to exit). `run-env-b.sh` now clones from `golden`, builds NTFS-3G in the
guest (about 15 s) under `~/ntfsmac/deps`, and puts it on the guest PATH for `run.py`. It deletes the clone on exit.

Result `tests/results/2026-11-14-2ee449b-B.json` (`dirty_tree: true`, macFUSE 5.5.0, NTFS-3G 2026.9.28):

| Case | Status | Time |
|---|---|---|
| I-INT-01 (100 seeded files, 1 KiB..64 MiB, about 600 MB, remount, SHA-256) | pass | 11.2 s |
| I-INT-03 (negative control) | pass | 10.8 s |

The first run failed for one reason only: macOS creates `.fseventsd/fseventsd-uuid` in the root of the volume
on every mount, and the comparison reported it as an unexpected extra file (all 100 test files matched byte for
byte). `hash_tree()` in `tests/integration/integrity/remount_hash.py` now ignores a root-level `.fseventsd`
(`MACOS_ROOT_METADATA`). The failed result and its evidence are kept as `failed-first-run-*.json` and
`first-run-evidence/` under `tests/results/evidence/m203/`. This touches a case file, not only `tests/harness/`.

Throughput: the 600 MB were written, unmounted, remounted and hashed in about 10 s, i.e. writes land in the
page cache (see U4 below).

## Surprises and notes for later tasks

- **macOS adds `.fseventsd` to the volume root** at every mount (a directory with `fseventsd-uuid`), also on a
  read-only mount. No `.DS_Store`, `._*`, `.Spotlight-V100` or `.Trashes` appeared during the smoke runs or in 8 s
  of idle after a write session; `mdutil -s` on the mount point answered "unknown indexing state". Finder was not
  running in the headless guest, so Finder-driven `.DS_Store` writes are not covered.
- **Ownership and permissions.** Mounts show `noowners`. Without `fmask`/`dmask`, `uid`/`gid` give `admin:staff`
  and mode `rwxrwxrwx` for everything; `fmask=0133,dmask=0022` produce `rw-r--r--` / `rwxr-xr-x`.
- **Timestamps.** mtime and atime of new files equal the guest's wall clock (UTC, +0000); `stat -f %SB` (birth
  time) shows `1970-01-01 00:00:00`, so NTFS creation time is not exposed through this backend.
- **Extended attributes.** `xattr -w com.test.x 1 file` fails with `[Errno 102] Operation not supported on socket`
  (ENOTSUP) with the default `streams_interface`; the build log says "XATTRS are on". Relevant to T-M2-04 for
  `streams_interface=xattr` and to macOS metadata writes.
- **Performance and durability (U4 input).** `dd bs=1m count=128` to the mount took 0.107 s (1.25 GB/s) and a
  following `sync` took 19 ms; reading the same file back took 0.006 s. These are page-cache numbers, not disk speed:
  `sync` returning in 19 ms right after 128 MiB suggests it does not wait for the NTFS-3G write-back. 200 tiny
  files (one `echo` each) took 0.44 s. Durability needs T-M2-06, not these numbers.
- **Unmount** with `/sbin/umount` as the mounting user works and takes about 20 ms; the daemon is gone within
  one poll. `umount` rc 0 even right after writes.
- **`tart exec` PATH** lacks `/sbin`: the script uses `/sbin/mount` and `/sbin/umount`.
- **Stuck daemons.** A `ntfs-3g` started without `backend=fskit` never exits on `SIGTERM`; anything that
  launches workers needs `SIGKILL` as the fallback.

## Proposed doc changes (not applied)

1. `docs/ARCHITECTURE.md` 3.3: state that `backend=fskit` is not optional in a stronger way: without it the
   mount hangs silently (exit 0, no message), so the daemon's mount wait must fail on timeout and kill the worker
   with `SIGKILL`; and that `ntfs-3g` returns before the mount is visible (poll `getfsstat`, as step 8 already says).
2. `docs/ARCHITECTURE.md` and the unknowns table: U1 answered yes for image files; add a new row for "NTFS-3G
   external FUSE refuses block devices for non-root (`unpriv_fuseblk_msg`)" next to U5, and record that macOS
   automounts attached NTFS read-only (`ntfs.fs`), so every attach must first `diskutil unmount`/DiskArbitration-
   claim the device.
3. `docs/TEST_STRATEGY.md`: tree comparisons on macOS must ignore the root `.fseventsd`.
4. `docs/AGENT_TASKS.md`: T-M2-03 done, T-M2-03b not needed, T-M2-05 should start from the block-device findings
   above (patch 0003 scope includes the `getuid() && ctx->blkdev` check) and the `blkdev,blksize=` FUSE options.
5. `docs/VM_SETUP.md`: guest build prefix `~/ntfsmac/deps`, the `.fseventsd` note, and that the golden VM
   needs no extra installs for building NTFS-3G (pkg-config and the CLT are already there).
