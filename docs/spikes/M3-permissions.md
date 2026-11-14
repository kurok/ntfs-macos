# M3 permissions and ownership (T-M3-20)

Cases `I-PERM-01..07`, `tests/integration/permissions/i_perm.py`. Date 2026-11-14.
Measured on a clone of `ntfsmac-golden`: macOS 27.2 (26B123) arm64, macFUSE 5.5.0 FSKit backend,
NTFS-3G 2026.9.28 built against libfuse2, image file volume, mounted by the console user `admin`
(uid 501, gid 20). Reference: Environment A, Linux container, root (uid 0), kernel FUSE.

Results: `tests/results/2026-11-14-a420e43-B.json` (7 pass), `tests/results/2026-11-14-a420e43-A.json`
(`I-PERM-01..06` pass; `I-PERM-07` is B only). Evidence logs: `tests/results/evidence/2026-11-14-a420e43-{A,B}/`.
Hand probes before the cases: `tests/results/evidence/m320-probes/`.

Product mount line used by the cases (B): `backend=fskit,local,norecover,uid=501,gid=20,noatime,windows_names,streams_interface=openxattr,fmask=0133,dmask=0022`.
In A: `noatime,uid=0,gid=0,fmask=0133,dmask=0022`.

## Summary

Modes and owners on this backend are display values. Nothing about them controls access, for the
console user, for root, or for another local account. Nothing a user does with `chmod` or `chown` reaches
the disk unless the opt-in `permissions` or `usermapping` is on, and even then it controls nothing.

## What is and is not access control

| Mechanism | Access control? | Evidence |
|---|---|---|
| `uid`, `gid`, `fmask`, `dmask` | No. They set what `ls -l` shows | `I-PERM-01`, `-02` |
| Mode bits shown (0644, 0755) | No. Not checked for read, write, list or execute | `I-PERM-05`, `-07` |
| `chmod`, `chown` on the default mount | No. Return success, change nothing | `I-PERM-03`, `-04` |
| `permissions` (stored modes) | No. Modes 0600, 0700, 000 are stored and shown, not enforced | `I-PERM-06`, `-07` pass 2 |
| `ro` (read-only mount) | Yes. Every write fails with `EROFS`, also for other accounts | `I-PERM-07` |
| Mount point location | Yes, but it is the parent directory, not the volume: a mount point under the console user's private `TMPDIR` cannot be reached by another account | `I-PERM-07` setup |

## Findings

### Owner and group (I-PERM-01)

- Every entry shows the console uid and gid (501:20) for entries made with `touch`, `mkdir`, `cp`, `cp -p`,
  `cp -R`, and again after a remount (14 entries in each of two sessions). The volume root, and the
  `.fseventsd` directory macOS creates, show the same.
- The mount is `noowners`. What is shown depends on who asks: the console user and root see 501:20, another
  local account sees its own ids (561:561 for an admin-made file). The text "shows the mounting user whatever
  `uid`/`gid` say" is true for the mounting user and root only.
- A: the same with 0:0, as the container user is root.
- Not covered: fixtures made on Linux before the run. The cases use entries made by an earlier mount session
  of the same image; the harness has no Linux-made image in the guest.

### Modes (I-PERM-02)

- Files 0644, directories 0755 in three read-write and read-only sessions, for files made by `touch`, `cp`,
  `open(O_CREAT, 0777)` and `mkdir(0700)`: the requested mode is not honoured.
- Without `fmask` and `dmask` (only `uid`, `gid`) B shows 0777 for everything (reference, log only).
- `ntfsfix -n` clean. Attributes and security descriptor of four entries unchanged (`ntfsinfo -v`, see below).
- Windows ACLs of a fixture were not tested (no such image in the guest).

### chmod (I-PERM-03)

- On the default mount `chmod` returns success for 0600, 0755, `+x`, 0777, 0000 and 4755 on a file and a
  directory, and the mode afterwards is still 0644 or 0755, in the same session and after a remount.
  Same outcome on three runs. A is identical (`ok`, no change). Silent success, not an error.
- `ntfsinfo -v` of the entry on the unmounted image is identical before and after, including all times
  (file attributes, `$STANDARD_INFORMATION`, security descriptor). A Linux tree dump of the image was not made
  (no Linux in the guest); `ntfsinfo` is the check.
- With `permissions` alone `chmod 0600` takes effect, is shown at once, and survives a remount (`I-PERM-06`,
  probe). With `permissions` plus `uid`/`gid`/masks it returns success and changes nothing (T-M2-04).

### chown (I-PERM-04)

- `chown` returns success and changes nothing, as the console user (also to uid 0 and to another uid, where
  macOS would normally return `EPERM`) and as root through `sudo -n` inside the guest. Ownership on re-stat
  and after a remount is 501:20. Three runs identical. `ntfsinfo -v` unchanged. A is identical (root, no-op).
- With `permissions`, root `chown 1234:4321` returns success and `ls -ln` still shows 501:20 (probe). Whether
  the mapped owner is stored cannot be seen through this backend.

### Execute bit (I-PERM-05, N9)

- N9's premise is refuted. A file with mode 0644 runs: a `#!/bin/sh` script, a copy of `/usr/bin/true`, and a
  binary compiled in the guest (`cc`, ad-hoc signed). They run directly by path, in a read-write session, after
  a remount, and on a read-only mount. `chmod +x` returns success and changes nothing (the bit cannot be set).
- Directories stay traversable and writable by the owner.
- A: `EACCES` for the same files, as the kernel checks the bit and the mapping cleared it.
- Consequence: `fmask=0133` gives no protection against running code from the volume, and "scripts and
  binaries cannot be run" is false on B. The cleared bit is cosmetic. Gatekeeper and quarantine behaviour for
  downloaded binaries were not examined.
- Probe note: `sudo -u USER /path/file` refuses a file without the x bit ("command not found"); that is sudo's
  own check. `sh -c /path/file` as the same account runs it.

### opt-in mapping smoke (I-PERM-06)

Three variants, each: mount succeeds, 1 MiB + 4097 bytes, a small file and a directory written, hashes equal live
and after remount, `ntfsfix -n` clean. No claim beyond that (ADR-012).

| Variant | Engine message at mount | `security id` of a new file (`ntfsinfo -v`) |
|---|---|---|
| `permissions` | `Using default user mapping` | 262 in B, 260 in A |
| `permissions` + on-volume `.NTFS-3G/UserMapping` | none | 261 in B, 260 in A |
| `usermapping=<absolute path>` (file in the work directory) | none | 262 in B, 260 in A |

- Without the opt-in, new files keep an inline descriptor with owner `S-1-5-32-544` (Administrators) in the
  MFT record. With any of the three variants they get a `$Secure` id. The descriptors were not decoded.
- A mapping file whose implicit line (`::SID`) ends in a number below 1000 (for example `-513`) makes the engine
  print `Bad implicit SID pattern ...` and mount anyway, with the line dropped. The implicit pattern needs a
  last component of 1000 or more (`ntfs_valid_pattern`, `libntfs-3g/acls.c`). The cases use `::<domain>-10000`.
- The path `/usr/local/etc/ntfs-macos/UserMapping` and the worker sandbox were not used (no worker yet); the file
  is read from the case's work directory.
- Files made before `permissions` was switched on show 0777 afterwards (default mapping, no stored descriptor);
  files made under it keep their modes (probe). Relevant to a user who turns the opt-in on for an existing volume.

### Another local account (I-PERM-07, N10)

A throwaway account (`ntfsmacperm`, uid 561, no password, created with `dscl`, used through `sudo -n -u`, deleted
at the end of the case and checked gone) acts on a mount made by `admin`. Every operation succeeds:

| Operation by the other account | Product mount | `permissions` mount |
|---|---|---|
| stat, list | ok, shows its own uid:gid | ok |
| read a file | ok | ok (mode 0600 file) |
| read a mode-000 file | ok (the file is 0644 there, `chmod 0` is a no-op) | ok (real mode 000) |
| append to a file | ok | ok (0600 and 000 files) |
| create file, mkdir, mkdir in a directory | ok | ok (create and list in a 0700 directory) |
| rename, delete admin's files | ok | ok (delete in a 0700 directory) |
| run a binary | ok | not run |
| chmod admin's file | ok, no-op | not run |

- The writes are on disk after a remount (appended text present, deleted file gone, new file present).
- On a read-only mount a create by the other account fails with `EROFS` (read-only file system).
- Mode-000 files and 0700 directories are readable and writable by an account that is not the owner, on a
  mount where the modes really are stored. The N10 text ("assume they have the same access") is correct and
  can be stated as measured.
- The engine adds `allow_other`; the guest has no other account type tested (no standard user created
  through the GUI, no group membership beyond the primary group).

## Harness notes

- `fsops.mount` always adds `uid` and `gid` in B, and the `permissions` opt-in must not have them. `i_perm.py`
  has its own `mount_opts` (same deadline and SIGKILL, same `_wait_b_mount`) that adds no ids.
- `ntfsinfo` and `ntfsfix` come from the guest-built prefix; `mkntfs` is in `sbin`, so `guest_tool` is needed.
- The harness work directory is under the console user's `TMPDIR` (mode 0700). `I-PERM-07` mounts at
  `/private/tmp/ntfsmac-perm07-<pid>/mnt` so the other account can reach it.
- A mount of the same image by a clone started from `ntfsmac-golden` needs about 3 s per mount; a case with
  three runs takes 25 s.

## Proposed edits to shared files (not made)

1. `docs/ARCHITECTURE.md` N9: replace with "Executable files on the volume run whatever their mode: the backend
   does not check the execute bit (`I-PERM-05`). `fmask=0133` and `chmod +x` have no effect on this." Section 9
   mount-flags row (about line 474): "open until `I-PERM-05`" becomes "refuted: files without an execute bit run".
2. N10: "The mount is `noowners`: `ls -ln` shows the mounting user to that user and to root, and shows the caller's
   own ids to any other account. A mode-000 file and a 0700 directory are readable and writable by another local
   account, also with `permissions` (`I-PERM-07`)." Drop "[A: I-PERM-07]".
3. `docs/TECHNICAL_DECISIONS.md` ADR-012: the sentence "the execute bit is cleared and `chmod +x` does not
   persist, but whether that stops execution is open" becomes "it does not stop execution (`I-PERM-05`)"; add that
   `chmod` and `chown` return success without effect on the default mount. Consider dropping `fmask=0133` for
   plain 0644: it protects nothing here, and it differs from what Windows would show only cosmetically. Decide
   before relaxing it; the Linux tree dump is unaffected either way.
4. `docs/TEST_STRATEGY.md` `I-PERM-05` row: expected result "runs on B (the bit is not checked), `EACCES` on A";
   `I-PERM-03`/`-04`: "success with no effect" is the recorded outcome; `I-PERM-07`: add "B only" and the
   `permissions` second pass; `I-PERM-06`: add that the implicit pattern line needs `-1000` or above.
5. `docs/IMPLEMENTATION_PLAN.md` 3.16: "the execute-bit limitation (N9) is demonstrated or corrected" is met by
   "refuted"; the N9 text must be corrected in the user docs (limitations N9 verbatim rule, line 617).
6. `integration/mount-options.json`, `usermapping` entry: add that a mapping file with an implicit pattern below
   1000 is accepted with a message and the line dropped; the daemon's file check (root-owned, mode 0644) should
   also parse the file and refuse a bad implicit line. Add `observed` note for `permissions`: files made
   before it show 0777.
7. `tests/integration/functional/fsops.py`: an optional `ids=False` argument of `mount()` (omit `uid` and `gid`)
   would let `i_perm.py` drop its own `mount_opts`.
8. N10 user text: tell users that another account on the Mac can read, change and delete the files of a mounted NTFS
   volume, whatever the shown modes; only a read-only mount, or not mounting for other users, limits that.
