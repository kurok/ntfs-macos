# M2 option matrix: NTFS-3G mount options on macFUSE's FSKit backend (T-M2-04)

Date: 2026-11-14. Answers U14; input for R-09 and for the correction of ARCHITECTURE.md section 3.3 (criterion 2.4).
Raw log: `tests/results/evidence/m204/option-matrix.log` (run 4 of 4; guest build log and exit statuses beside it; `earlier-runs/` has runs 1 to 3, which differ only in probes fixed since: a page-cache flaw in the atime probe, FinderInfo size, big_writes measurement).
Reproduce: `tests/integration/smoke/option_matrix.sh` in a guest (79 mount cases plus echo and no-op sections; the driver does the same clone, build and run steps as `run-env-b.sh`, with a trap that deletes the clone).
Machine-readable result: `integration/mount-options.json`.

## Method

- One disposable clone of `ntfsmac-golden`, deleted by a trap at the end. macOS 27.2 arm64 guest, macFUSE 5.5.0, NTFS-3G 2026.9.28 built by `scripts/build-ntfs3g.sh --fuse external` (same flow as `run-env-b.sh`), no patches, no sudo in the guest, nothing on the host.
- Image: `clean-256m-4k` from Environment A (copy of the M2-03 export, identical SHA-256), a fresh APFS clone of it for every case. The big_writes part uses a sparse 5 GiB image made with `mkntfs -F -Q`, because NTFS-3G applies `big_writes` only to volumes of 4 GiB or more (`SAFE_CAPACITY_FOR_BIG_WRITES`, `include/ntfs-3g/param.h`).
- Every mount is `backend=fskit,local,norecover,<option under test>`; `recover`, `remove_hiberfile` and `fsname` were never passed. The script polls `mount` for the mount, and if it does not appear (or the driver exits) it kills the driver with SIGKILL. It ends with a check that no `ntfs-3g` process and no mount is left (result: none, exit 0).
- Classes: **E** accepted and effective; **I** accepted but ignored (exit 0, no observable effect); **R** rejected (exit status and text); **S** accepted but the promised behaviour is not delivered (silent failure); **N** not run or not testable here.
- "Echo" evidence is the engine's own `Mount options:` line from a foreground (`no_detach`) mount: it shows which tokens the engine consumed and which it handed to libfuse. A daemonised mount prints nothing.

## Answer in four lines

1. The backend and libfuse reject almost nothing: of the option variants that the engine does not parse itself, only `allow_root` was refused. Unknown tokens (`bogusopt`, `bogusopt=1`, an empty token) and malformed values for macFUSE options (`iosize=abc`, `daemon_timeout=abc`) mount fine. `argv_check()` is therefore the only guard; nothing downstream will catch a bad option.
2. `volname` has no effect (U14: the backend does not use it). `big_writes`, `iosize`, `daemon_timeout`, `noappledouble`, `noapplexattr`, `nobrowse`, `sync`, `async`, `dev`, `suid` are accepted and ignored.
3. `streams_interface=xattr` is the wrong value on macOS: it only stores `user.*` names and fails every `com.apple.*` name with `ENOTSUP`. `streams_interface=openxattr` stores all names and keeps them across a remount.
4. `uid`/`gid` are accepted but invisible (the mount is `noowners` and always shows the mounting user), and they switch the engine into a mode in which `permissions` does nothing. `permissions` only works without `uid`, `gid`, `fmask`, `dmask`.

## The matrix

Case names are those of the log (`=== CASE <name>`, or `=== ECHO echo-<option>`, `=== NOOP`).

### Section 3.3 "Always" row

| Option | Class | Evidence | Error text | Recommendation for 3.3 |
|---|---|---|---|---|
| `backend=fskit` | E | `baseline`: mounts as `/dev/disk6s1 on ... (macfuse, local, nodev, nosuid, noowners, noatime, fskit, mounted by admin)`. `backend-missing`, `backend-bogus`: exit 0, no mount for 15 s, `Mounted ... (Read-Write ...)` printed in the foreground and then it waits; SIGKILL needed | none (silent) | Keep, mandatory. Any other value behaves as absent. `argv_check()` rule 5 already fixes `backend=fskit` |
| `local` | E | `baseline` source `/dev/disk6s1` and flag `local`; `local-absent`: source `macfuse://F18DF045-...`, no `local` flag | none | Keep |
| `norecover` | E (by source) | Consumed by the engine (`ctx->recover = FALSE`, echo shows it is not passed on). It acts only when `$LogFile` is unclean: `volume.c` then falls back to read-only (`unclean_journal_msg`). The clean fixture cannot show it; needs the `unclean-log` fixture. `norecover-dirty-volume`: SIGKILL of a read/write driver leaves a volume that remounts read/write with `norecover` and passes `ntfsfix -n`, so NTFS-3G itself sets no dirty flag at mount | none | Keep, mandatory. Add a macOS case to the `unclean-log` fixture test (T-M3-12 / fixtures) |
| `noatime` | E | `atime-reference`, `noatime`: on-disk atime of `/boundary/c.bin` (`ntfsinfo -F`) stays `11:38:33`; `atime` and `relatime`: becomes the read time. `stat` through the mount shows `11:38` in all cases (the backend does not expose atime changes, `touch -a -t` has no effect) | none | Keep; effect is only on disk and is not visible in macOS |
| `windows_names` | E | `windows_names-off`: `a:b`, `CON`, `aux.txt`, `trail.`, `sp `, `q?x`, `a"b`, `lt<x`, `a*b`, `pipe\|x` all created. `windows_names-on`: all ten fail with `Invalid argument` (EINVAL) | `Invalid argument` | Keep. State in the user docs that such names cannot be created on the volume |
| `big_writes` | I | Echo: consumed by the engine, not passed to libfuse. 5 GiB image with `debug`: with and without it the driver sees 16 writes of 1048576 bytes (plus 2 of 36 bytes) and `max_write=0x02000000`. Timing 256 MiB dd + sync, on: 0.27/0.33/0.26 s, off: 0.38/0.19/0.28 s | none | Drop from "Always" (no effect; below 4 GiB the engine would not apply it either). R-10 must not rely on it |
| `streams_interface=xattr` | S | `streams-xattr`: `xattr -w user.test` ok and persists after remount; `com.test.x`, `com.apple.metadata:test`, `com.apple.quarantine`, `com.apple.ResourceFork`, FinderInfo: `[Errno 102] Operation not supported on socket` (ENOTSUP); `system.test` returns ok but is never listed | ENOTSUP per call | Replace by `openxattr` |
| `streams_interface=openxattr` | E | `streams-openxattr`: all of `user.test`, `com.test.x`, `com.apple.metadata:test`, `com.apple.quarantine`, `com.apple.ResourceFork`, `system.test` write and are listed, and are still there after a remount. `noappledouble-on-openxattr`: `cp -p` of a file with an xattr copies it, rc 0, no `._` file. Caveat: `xattr -wx com.apple.FinderInfo <32 bytes>` returns ok but `xattr -l` then reports `No such xattr: com.apple.FinderInfo` (write not readable back) | none | Use this value. T-M3-08 must test FinderInfo (tags, colours) explicitly |
| `streams_interface=none` | I | `streams-none`, `streams-default`: all xattr calls fail with ENOTSUP; the default when the option is absent | ENOTSUP per call | Allowed as the documented fallback of TECHNICAL_DECISIONS (xattrs unsupported) |
| `streams_interface=windows` | I | `streams-windows`: mounts, all xattr calls ENOTSUP | none | Keep out (as TECHNICAL_DECISIONS already says) |
| `streams_interface=<other>` | R | `streams-bogus` | exit 11, `Invalid named data streams access interface.` | `argv_check()` rule 5 grammar: `openxattr` or `none` |
| `streams_interface` (no value) | R | `streams-noval` | exit 11, `'streams_interface' option should have a value.` | covered by rule 4 |
| `user_xattr` | E | `user_xattr`: identical to `openxattr` on macOS (source: sets `NF_STREAMS_INTERFACE_OPENXATTR` under `__APPLE__`) | none | Not needed; use the explicit `streams_interface=openxattr` |
| `uid=<n>` | E (invisible) | `uid-gid-other` (`uid=1234,gid=4321`): `ls -ln` still `501 20`. Mount is `noowners`. `defperm-effect-with` (`uid=0`, mode 000): a mode-000 file is readable and creatable-in by uid 501, so no access control follows from it | `uid=abc`: exit 11, `'uid' option needs a decimal value` | Keep for the engine's file-creation defaults, but do not document it as ownership. `gid` uses the same check (`ntfs-3g_common.c`; not run) |
| `gid=<n>` | E (invisible) | as `uid` | see `uid` | as `uid` |
| `fmask=0133` | E | `fmask-dmask`: files `-rw-r--r--`; `fmask-dmask-strict` (0177/0077): `-rw-------`, `drwx------`; `fmask-dmask-without-uid` (no uid/gid): same as with | none | Keep |
| `dmask=0022` | E | as `fmask`: `drwxr-xr-x` | none | Keep |
| `umask=<octal>` | E | `umask` (0027): `-rwxr-x---` files and dirs | none | Not needed |

### Opt-in rows

| Option | Class | Evidence | Error text | Recommendation |
|---|---|---|---|---|
| `volname=<leaf>` | I | `volname` vs `no-volname`: identical `mount` line, `diskutil info` (`Volume Name: Not applicable (no file system)`), `df`, `getattrlist ATTR_VOL_NAME` (empty string), `/Volumes` listing. Echo: token reaches libfuse (`Mount options: ...,volname=NTFSMAC,...`) and has no effect. Finder's display name could not be checked: the guest is headless and the test mounts are not under `/Volumes` | none | Answer to U14: do not pass `volname`. Remove the row and the `volname` clauses in `argv_check()` rules 3 and 5, and from ADR-016's "optional `volname`". The name comes from the mount-point leaf [A, to confirm with a `/Volumes/<leaf>` mount in T-M4 Finder tests] |
| `ro` | E | `ro`: mount line `read-only`, `touch` gives `Read-only file system`. `ro-noop`: engine reports `Mounted ... (Read-Only ...)`, image SHA-256 unchanged | `Read-only file system` | Keep |
| `rdonly` | E (kernel only) | `rdonly`: mount is `read-only`, writes refused. But `rdonly-noop`: engine reports `Mounted ... (Read-Write ...)`; the image happens to stay unchanged because the kernel blocks macOS's own writes | none | Banned: it is not a substitute for `ro` (the engine still opens the volume read/write and may write) |
| `rw` | I | `rw-explicit`: no change | none | Not needed |
| `hide_hid_files` | E | `hide_hid_files-off/on`: a file given the DOS hidden attribute (`xattr -wx system.ntfs_attrib_be 00000002`, works as a normal user) is listed without the option and absent from the listing with it; `stat` by name still works | none | Keep as opt-in |
| `permissions` | S | `permissions-on` (with uid/gid): `chmod 600` returns success, mode stays `rwxrwxrwx`, also after remount. `permissions-only` (no uid/gid/masks): `chmod 600` shows `-rw-------` and persists across remount | none | Opt-in only without `uid`, `gid`, `fmask`, `dmask`. The option builder must drop those four when `permissions` is chosen, and `argv_check()` must require the same. Otherwise the opt-in does nothing silently |
| `usermapping=<abs path>` | E / S | `usermapping-file`: `chmod 600` takes effect and persists, with and without uid/gid (mapping overrides them). `usermapping-missing`: exit 0, mounted, behaves as without the option (no message, no error) | none even for a missing file | Keep, but the daemon must check that the file exists, is a regular root-owned file and mode 0644 before the spawn; the engine will not |

### Banned row and the other engine and FUSE options

| Option | Class | Evidence | Error text | Recommendation |
|---|---|---|---|---|
| `recover` | N | Not run by rule. In the engine option table (`OPT_RECOVER`); it is the engine default | n/a | Keep banned |
| `remove_hiberfile` | N | Not run by rule. In the engine option table | n/a | Keep banned |
| `fsname` | N | Not used by rule. Source: `FLGOPT_NOSUPPORT`. The engine builds `fsname=<device>` itself: every echo shows `...,fsname=WORK/var.img` | n/a | Keep banned |
| `dev` | I | `dev`: mounts; mount line stays `nodev`. Echo: passed to libfuse | none | Keep banned (defence in depth) |
| `suid` | I | `suid`: mounts; mount line stays `nosuid` | none | Keep banned |
| `allow_root` | R | `allow_root` | exit 21, `fuse: allow_other and allow_root are mutually exclusive` (the engine always adds `allow_other`) | Keep banned |
| `efs_raw` | N | `efs_raw`: accepted, no observable change; the effect needs an EFS-encrypted file | none | Keep banned |
| `allow_other` | I | `allow_other`: mounts. Echo: `allow_other,allow_other,nonempty,...` (the engine adds its own, the duplicate is harmless) | none | Do not pass; the engine adds it. Note `argv_check()` should not read the echo back (the text in M2-backend.md) |
| `nonempty` | I | `nonempty-on-empty-dir`, `nonempty-on-busy-dir`: with no option at all a mount on a directory holding a file works and hides the file | none | Do not pass |
| `default_permissions` | I | `default_permissions`: mounts. Echo shows a duplicate. `defperm-effect-with`: no kernel enforcement (`noowners`): a mode-000 file is readable by the mounting user | none | Do not pass. Do not describe mode bits as access control in the user docs: on this backend they are metadata |
| `no_def_opts` | S | `no_def_opts` (uid 0, masks 0777): mounts, then `mkdir` fails `Operation not supported on socket` (ENOTSUP) | ENOTSUP on mkdir | Ban (not in the current table; add it) |
| `blkdev` | I | `blkdev`: mounts an image file, no change. Echo: passed to libfuse. A real block device is T-M2-05 | none | Not passed by us; the engine adds it itself for a block device |
| `blksize=<n>` | I | `blksize`: mounts; echo prints `WARNING: blksize option is ignored because ntfs-3g must calculate it.` | warning only | Not passed |
| `bogusopt`, `bogusopt=1`, empty token `,,` | not rejected | `bogus-unknown-token`, `bogus-unknown-kv`, `empty-token`: all mount normally; echo: `bogusopt` reaches libfuse | none | `argv_check()` rule 4 (no empty token, allow-list keys) is the only protection: keep it as written |

### macFUSE-specific options

| Option | Class | Evidence | Recommendation |
|---|---|---|---|
| `noowners` | I | Always in the mount flags; `noowners` adds nothing | Do not pass |
| `noappledouble` | I | `noappledouble-off/on` with `streams_interface=none`, `xattr` and `openxattr`: no `._*` file is ever created. With `none`/`xattr`, `cp -p` of a file with an xattr fails with `could not copy extended attributes ...: Operation not supported on socket` instead of making a `._` companion; with `openxattr` the copy works | Do not pass |
| `noapplexattr` | I | `noapplexattr` (xattr), `noapplexattr-openxattr`: same results as without it | Do not pass |
| `nobrowse` | I | `nobrowse`: mount line gains no flag; echo passes it to libfuse. Finder effect not testable headless | Do not pass; revisit in Finder tests |
| `daemon_timeout=<n>` | I | `daemon_timeout=10` and `=abc` both mount. `daemon_timeout-effect`: driver stopped with SIGSTOP, `daemon_timeout=3`: still mounted after 8 s | Do not pass. Hung-worker handling stays the daemon's job (SIGKILL) |
| `iosize=<n>` | I | `iosize-default/65536/1048576`: `stat -f %k` is 1048576 and `df` block size 524280 in all; `iosize=abc` mounts | Do not pass |
| `ro` / `rdonly` | see opt-in | | |
| `sync` | I | `sync`: no `synchronous` flag in the mount line. The plain `/sbin/umount` returned `Resource busy` once in run 1 (case `sync`) and once in run 2 (`echo-sync`), not in runs 3 and 4 (`earlier-runs/` holds runs 1 to 3, the main log is run 4); the follow-up `umount -f` worked | Do not pass. The unmount path needs a retry anyway |
| `async` | I | `async`, `async-repeat-1`: no change | Do not pass |

### Mount side effects seen on the way

- `rw-noop` (mount read/write, change nothing, unmount): image hash **changes**. `ntfsls` of the root shows a new `.fseventsd` after the mount (macOS creates it, as M2-03 noted); `ro-noop` and `rdonly-noop` leave the image byte-identical. A read/write mount is never a read-only operation on disk.
- `ntfs-3g` exits 0 and prints nothing in the daemonised mode when it fails after `daemon()`; only `no_detach` shows the engine's lines. Reject reasons that happen before `daemon()` (bad `uid`, `streams_interface`, `allow_root`) come back as a non-zero status with text, as listed.
- Exit statuses seen: 0 (all accepted), 11 (bad value for an engine option), 21 (`allow_root`).

## Proposed changes to ARCHITECTURE.md section 3.3 (not applied)

1. "Always" row becomes
   `backend=fskit,local,norecover,noatime,windows_names,streams_interface=openxattr,uid=<console uid>,gid=<console gid>,fmask=0133,dmask=0022`: drop `big_writes` (no effect on this backend), replace `streams_interface=xattr` by `openxattr`.
2. Delete the "Volume name" row and every `volname` clause: row 3 and row 5 of `argv_check()`, the sentences "(U14)" and "Whether `volname`, `big_writes` and `noatime` are accepted ... is [A]". Close U14: the backend ignores `volname`. Also ADR-016 and TECHNICAL_DECISIONS decision 3 ("optional `volname`").
3. `permissions` opt-in: state that it replaces `uid`, `gid`, `fmask`, `dmask` (the option builder omits them; `argv_check()` rejects the mix), because with them it silently does nothing.
4. `usermapping=...`: add that the daemon verifies the file before spawn (the engine accepts a missing file without any message).
5. Banned list: add `rdonly` (kernel read-only only, engine stays read/write), `no_def_opts`, `allow_other`, `nonempty`, `default_permissions`, `blkdev`, `blksize`, `volname`, `noowners`, `noappledouble`, `noapplexattr`, `nobrowse`, `daemon_timeout`, `iosize`, `sync`, `async`, `user_xattr`, `umask`, `rw` (not rejected by the backend, so they must be rejected by `argv_check()` rule 4's allow-list; they are not harmful, they are simply not needed).
6. Rule 5 grammar: `streams_interface` is `openxattr` or `none`; `xattr` and `windows` are not generated.
7. Add the statement: "The backend and libfuse reject almost no option (unknown tokens and malformed macFUSE values mount); the allow-list in `argv_check()` is the only validation."
8. Sentence on mode bits: `uid`, `gid`, `fmask`, `dmask` set what `ls -l` shows and what NTFS-3G records; they are not access control on this backend (`noowners`; a mode-000 file is readable by the mounting user).
9. Keep the existing text on `backend=fskit` being mandatory; add exit status 0 with no output for the missing or wrong backend and the SIGKILL fallback (already in the M2-03 proposals).
10. Section 5/TEST plan: T-M3-08 must test `com.apple.FinderInfo` explicitly (write returns success, read back fails with `openxattr`), `com.apple.quarantine` and `com.apple.ResourceFork` (work); `norecover` needs a macOS pass over the `unclean-log` fixture.
11. Risk R-09: `volname`, `daemon_timeout`, `big_writes`, `iosize` are confirmed unavailable on the FSKit backend; creation time is the M2-03 finding.

## What this spike did not show

- `norecover` on an unclean `$LogFile` (no such fixture exists yet; only the source path is cited).
- The Finder-visible volume name and `nobrowse`, which need a `/Volumes/<leaf>` mount and a Finder session.
- `blkdev`/`blksize` on a real block device (T-M2-05) and `efs_raw` on an encrypted file.
- Any mount of a volume larger than the 5 GiB sparse image, or with real contents there.
