# Spike M3: xattrs, named streams and Finder-style copy (T-M3-08)

Date: 2026-11-14. Input for criterion 3.7, R-09 and R-26, ADR-012 and ARCHITECTURE.md section 3.3.
Cases: `tests/integration/xattr/` (`i_xattr.py` I-XATTR-01..08 and A1, `a_after_b.py` I-XATTR-09, helpers in `xattr_ops.py`; mount helper imported from `tests/integration/functional/fsops.py`).
Results (all pass): `tests/results/2026-11-14-ff7c35b-B.json` (I-XATTR-01..08, Environment B),
`tests/results/2026-11-14-ff7c35b-host.json` (I-XATTR-09, Linux then macOS then Linux),
`tests/results/2026-11-14-ff7c35b-A.json` (I-XATTR-A1, Environment A reference).
Evidence: `tests/results/evidence/2026-11-14-ff7c35b-B/` (logs, `I-XATTR-02.max-size.txt`, `I-XATTR-03.worker-debug.log`, `I-XATTR-04.worker-debug.log`) and `tests/results/evidence/2026-11-14-ff7c35b-host/` (`I-XATTR-09.phase-{a1,b,a2}.json`).
Setup: macOS 27.2 guest, macFUSE 5.5.0 FSKit, NTFS-3G 2026.9.28 (`backend=fskit,local,norecover,uid,gid`), Linux side NTFS-3G 2026.9.28 in the container. Mount options of the cases: `windows_names,noatime,streams_interface=openxattr` (the set proposed in M2-options) unless the case says otherwise.
Not run: Finder itself (no GUI session in the guest). "Finder-style" means `ditto`, `cp -p`, `cp -c` and `copyfile(3)` with `COPYFILE_ALL`, which is what Finder calls.

## Decision

**Keep `streams_interface=openxattr`.** `streams_interface=xattr` is not usable: it stores `user.*` only and refuses every other name with ENOTSUP, and then every copy of an ordinary macOS file (one with `com.apple.quarantine`, `com.apple.FinderInfo` or a resource fork) fails: `ditto` exits 1 and leaves no file, `cp -p`, `cp -c` and `copyfile(3)` exit non-zero and leave a 0-byte target (I-XATTR-08). With `openxattr` the same copies succeed with every xattr byte-identical (I-XATTR-06).

**One conflict with the option set: `windows_names` refuses xattr names that contain `:`** (see finding 8). That includes `com.apple.metadata:_kMDItemUserTags`, which holds Finder tag names. The product needs a decision on it (proposals at the end).

## Findings

1. **Set, get, list, remove work** on files and directories with `openxattr`: names of any namespace (`user.*`, `com.test.x`, `com.apple.*`, non-ASCII), values of 0 bytes and up, `XATTR_CREATE` over an existing name gives EEXIST, `XATTR_REPLACE` of a missing one gives ENOATTR (93), get and remove of a missing name give ENOATTR, replacing by a shorter value leaves no stale tail, `xattr(1)` `-w -p -l -d -c` work (I-XATTR-01). macOS limits names to 127 bytes: 128 gives ENAMETOOLONG. NTFS stores a name in NFC and macOS lists it in NFD (same nfconv layer as file names, U22); `xattr_ops.xmap` compares in NFC.
2. **Value size: 1 B to 64 KiB all work; the maximum is 262143 bytes (256 KiB minus 1)**, found by bisection, the next size gives EFBIG. A failed replace leaves the old value intact. 200 xattrs on one file, four values of 64 KiB on one file, and 64 KiB on a directory work and persist (I-XATTR-02, `I-XATTR-02.max-size.txt`). Linux reference: the kernel stops at 64 KiB (65537 gives E2BIG, I-XATTR-A1).
3. **Persistence:** every xattr checked in cases 01 to 04, 06 and 07 is present after unmount, `ntfsfix -n` (exit 0) and a read-only remount, with identical bytes.
4. **FinderInfo (the T-M2-04 mystery, explained).**
   - A `com.apple.FinderInfo` value must be exactly 32 bytes: any other size, empty included, gives ERANGE. Colour labels work: byte 9 holds the label in bits 1-3; a value with label 6 (red) and a nonzero last byte is stored and read back byte for byte, on files and directories, through the API and `xattr -wx/-px`, and survives a remount (I-XATTR-03).
   - **An all-zero FinderInfo is written successfully and listed, but reading it gives ENOATTR.** T-M2-04 wrote 32 zero bytes, so its "write ok, read back fails" was this case, not a general failure. A value with a single bit set reads back. Overwriting a colour with zeros also makes the read fail (the same as clearing it on HFS+ and APFS); `removexattr` then works.
   - **Which layer:** not NTFS-3G. The worker's own `-o debug` log shows `getxattr ... com.apple.FinderInfo 32 0` answered `success, outsize: 48` (16 header bytes and 32 data bytes) for the all-zero file, exactly as for a nonzero one, and Linux reads the same stream as 32 zero bytes (I-XATTR-03 and I-XATTR-09). The ENOATTR is produced on the macOS side of the FSKit backend after the worker answers. I did not separate the macFUSE extension from the kernel further.
   - Effect: harmless for users. A real colour or an "extended flags" value is never all-zero, and an all-zero FinderInfo means "none" on Apple file systems.
5. **Quarantine and resource fork work.** `com.apple.quarantine` (57-byte value) sets, reads, removes and persists. `com.apple.ResourceFork` works through the xattr API (70 KiB tested) and through the path `file/..namedfork/rsrc` in both directions (write by path, read by xattr and the reverse, `stat` size right); after the xattr is removed the path gives ENOENT (I-XATTR-04). `com.apple.lastuseddate#PS` and `com.apple.TextEncoding` work.
6. **Finder-style copy, APFS to NTFS: all tools preserve everything.** `ditto`, `cp -p`, `cp -c` (falls back to a copy across volumes), `cp -pc` and `copyfile(COPYFILE_ALL)` copy a file with FinderInfo, quarantine, resource fork (70 KiB), `lastuseddate`, `TextEncoding` and `user.*`, and a directory with FinderInfo, with every xattr equal to the source. **No `._*` file appears** in any case (checked in every case, and in the Linux listing of I-XATTR-09): the volume reports xattr support, so macOS does not use AppleDouble companions. Plain `cp` copies xattrs too; `cp -X` still carries `com.apple.quarantine` (macOS behaviour, not the volume) (I-XATTR-06).
7. **Copy from NTFS: a resource fork stops `cp -p` and `copyfile(3)`.** `cp -p` (and `copyfile(COPYFILE_ALL)`) from NTFS to APFS fails with "could not copy extended attributes: Invalid argument" when the source has a `com.apple.ResourceFork`, at any fork size; `ditto` works, and a file without a resource fork copies with every tool. copyfile reads the fork at a non-zero position; macOS refuses that with EINVAL for this volume before the request reaches the worker (the worker log shows only position 0), so NTFS-3G's own position handling is never reached (I-XATTR-04 trace, I-XATTR-06). Resource forks are rare in current files, but Finder uses copyfile, so a Finder copy of such a file off the volume is expected to fail the same way (not tested).
8. **`windows_names` also checks xattr names** (NTFS-3G calls `ntfs_forbidden_chars` on the stream name in `setxattr`, `src/ntfs-3g.c`). Names with `: / \ * ? " < > |`, a trailing dot or space, or a control character give EINVAL. The two names macOS uses most are affected: `com.apple.metadata:_kMDItemUserTags` (Finder **tags**), `com.apple.metadata:kMDItemFinderComment`, `com.apple.metadata:com_apple_backup_excludeItem`. Without `windows_names` they are stored. Effects (I-XATTR-05, I-XATTR-06):
   - Setting tags on a file or folder fails with EINVAL. The colour (FinderInfo) is stored, the tag name is not.
   - Copying a file that carries a `:` xattr onto the volume fails: `ditto` exits 1 and creates no file; `cp -p`, `cp -c`, `cp -pc` exit 1 and leave the file with data and every xattr except the `:` ones; `copyfile(COPYFILE_ALL)` fails with EINVAL and leaves an empty file.
   - A `:` stream already on the volume (written without `windows_names`, or by another tool) is listed and readable under `windows_names`, can be removed, but cannot be replaced (EINVAL).
   - With `streams_interface=openxattr` alone (no `windows_names`) all of this works.
9. **NTFS named streams and Linux (I-XATTR-09, A then B then A).** The stream name on disk is the xattr name macOS used: `com.apple.quarantine` is a stream with that name, read by `ntfscat -a DATA -n` byte for byte (a 200000-byte value, a 64 KiB value, FinderInfo, resource fork, `lastuseddate#PS`, a directory's FinderInfo all identical). Linux with `streams_interface=openxattr` lists the same names; with the Linux default (`xattr`) it lists them as `user.<name>` (`user.com.apple.quarantine`, and a macOS `user.k` as `user.user.k`). Streams made on Linux/Windows style (`Zone.Identifier`, `note`) are xattrs `Zone.Identifier` and `note` on macOS. A value above 64 KiB is listed on Linux but `getxattr` fails (E2BIG, kernel limit); `ntfscat` reads it. A non-ASCII xattr name is stored in NFC on disk, listed in NFD on macOS. `ntfsfix -n` is clean after the round trip. `tests/tools/treedump.c` does not print streams; the case uses its own dump (see proposals).
10. **Data operations keep xattrs** (I-XATTR-07): append, overwrite, truncate to 0 and to 70000, `O_TRUNC` rewrite, `chmod`, `utime`, rename in a directory, into a subdirectory and over an existing file (the source's xattrs replace the target's, no mix). A file unlinked and created again has none. A read-only mount returns EROFS for set and remove and still reads. `link()` fails with EIO on this backend (recorded; not an xattr matter, hard links share the xattrs on Linux).
11. **`streams_interface=xattr` for comparison** (I-XATTR-08): `user.k` works and persists; `com.apple.quarantine`, `com.apple.ResourceFork`, `com.apple.FinderInfo`, `com.apple.lastuseddate#PS` and `com.test.x` give ENOTSUP (102 EOPNOTSUPP); no `._*` companions either. Copy results as in the decision above. Spike M2 had this, now with the copy tools.

## Not covered

- Finder itself (tags set by hand, colour dots, copy by drag), Spotlight indexing and `mds` writes. A guest with a GUI session is needed (T-M5-10 area).
- Whether Windows and `chkdsk` accept a stream whose name contains `:` (finding 8, option b below). [U]
- `.DS_Store` and `.metadata_never_index` (R-26): no `._*`, `.DS_Store`, `.Spotlight-V100` or `.Trashes` appeared in any headless run; `.fseventsd` appears in the root on every mount (M2).

## Proposed changes (not applied)

1. **ARCHITECTURE.md 3.3 and "Finder writes .DS_Store" (line 560):** replace "a `com.apple.FinderInfo` write returns success but cannot be read back" by finding 4 (only an all-zero value, answered above the worker; colours work). Add findings 7 and 8 to the known limits. R-09 and R-26: same correction; R-09 mitigation "FinderInfo in T-M3-08" is done.
2. **Decide the `windows_names` conflict (finding 8).** Options: (a) keep as is and document that Finder tags, comments and `com_apple_backup_excludeItem` cannot be stored, and that copying such files fails; (b) a small patch in `third_party/patches/ntfs-3g/` that skips `ntfs_forbidden_chars` in `ntfs_fuse_setxattr` for the open namespace, keeping `windows_names` for file names (Windows cannot address a stream with `:` by path, so check with chkdsk first); (c) drop `windows_names` (the file-name protection of I-NAME-* goes). I recommend (b). Then I-XATTR-05 and the f2 branch of I-XATTR-06 flip from "refused" to "stored", and the cases say so.
3. **`integration/mount-options.json`:** keep `streams_interface=openxattr` as effective; add to its note that `windows_names` also applies to xattr names and that names are limited to 127 bytes and values to 262143 bytes.
4. **`tests/tools/treedump.c`:** add `-x` (list each file's named streams as name, size, SHA-256) so a Linux tree dump shows the streams; I-XATTR-09 uses its own Python dump meanwhile. It should skip `system.*`.
5. **docs/TEST_STRATEGY.md I-XATTR row:** list I-XATTR-01..09 and A1, and state that I-XATTR-09 runs with `run-env-b.sh --orchestrate`, the others with the plain call. TEST_STRATEGY and IMPLEMENTATION_PLAN 3.7 wording "FinderInfo write returns success, read back fails" becomes "all-zero value reads as ENOATTR".
6. **User docs and `ntfsmac doctor`:** `cp -p` and Finder copies of files with a resource fork fail from NTFS (finding 7); recommend `ditto`.

## Reproduce

```sh
tests/harness/run-env-b.sh --cases 'I-XATTR-*'                       # 01..08 in a disposable guest
tests/harness/run-env-b.sh --orchestrate --cases I-XATTR-09           # Linux, macOS, Linux
python3 tests/harness/run.py --env A --cases 'I-XATTR-*'              # I-XATTR-A1
```

`--roots integration/xattr` limits discovery to these cases.
