# Expected differences in the reference comparison

Used by `C-REF-OP-01..12` (`tests/compatibility/ref/`, T-M3-15, criterion 3.10). The twelve operation scripts
(`ops-NN.json`) run on a fresh image in Environment A (Linux, pinned NTFS-3G, unpatched, mount options of the
product without `backend=fskit` and `local`) and in Environment B (macOS guest, NTFS-3G with the full patch
series, mount options exactly as `src/options/options.c` builds them). Both images are dumped on Linux:
`tests/tools/treedump` plus a full dump (type, size, SHA-256, link target, link count and group, allocated bytes,
NTFS attribute flags, NTFS times, every named stream with length and SHA-256), and the result code of every
operation is compared.

A difference passes only if it is covered by a row below. The judge (`tests/compatibility/ref/reflib.py`) names
the row id in code, checks the shape of the difference (not only the path), and fails if the id is missing from
this file. Scripts declare which operations and paths a row may cover (`ed_ops`, `ed_paths` in `ops-NN.json`);
ED-01 and ED-02 apply everywhere. Anything else fails the case.

| ID | Difference | Cause | Seen in |
|---|---|---|---|
| ED-01 | B has `.fseventsd` and `.fseventsd/fseventsd-uuid` in the root. | macOS creates them in the root of every mount, whatever the driver does (`docs/spikes/M2-backend.md`, C-REF-01). Not test data. | all |
| ED-02 | A name typed in a non-NFC form (NFD, or U+212B ANGSTROM SIGN) is stored as typed on A and in NFC on B. Names are matched by their NFC form; B must store NFC. | NTFS-3G's Darwin `nfconv` default converts names to NFC on the way in; both spellings reach the same file on B (`docs/spikes/M3-names.md`, U22 answers 1 and 4). Names typed in NFC are equal on both sides. | 05 |
| ED-04 | `link(2)` returns `EIO` on B and succeeds on A. The second name, the link count and the link group are on the disk of B and equal to A (no dump difference). | The backend returns `EIO` after the link is made and leaves the cache stale (`docs/spikes/M3-metadata.md`, `link()`). A remount shows the link. Open limitation. | 10 |
| ED-05 | A write past EOF without a preceding `ftruncate` is dense on B (allocation of the whole range, no sparse flag) and sparse on A. Size and SHA-256 are equal. | `docs/spikes/M3-readpath-big.md` finding 16. A sparse file has to be made with `ftruncate` first. Open limit. | 08 |
| ED-06 | A stream value above 64 KiB (the script uses 100000 bytes) is stored on B; A refuses it (`E2BIG`). The value is read from the image with `ntfscat`. | The Linux kernel limits an xattr value to 64 KiB, macOS and NTFS-3G allow 256 KiB minus 1 (`docs/spikes/M3-xattr.md` findings 2 and 9). A limit of the Linux reference, not of the product. | 06 |
| ED-07 | The allocated size of a sparse file differs by at most one compression unit (64 KiB), B larger (8 to 16 KiB seen). Size, SHA-256 and the sparse flag are equal. | Allocation is in units of 16 clusters and B rounds differently (`docs/spikes/M3-readpath-big.md` finding 13: 1056768 on Linux against 1064960 on B). | 08 |
| ED-08 | A stream name with `:` (here `com.apple.metadata:kMDItemFinderComment`) is created on B and refused on A (`EINVAL`). | A is unpatched NTFS-3G, whose `windows_names` also checks xattr names; patch 0009 exempts them (ADR-025, `docs/spikes/M3-xattr.md` finding 8). | 06 |
| ED-09 | `unlink` of a directory returns `EPERM` on B and `EISDIR` on A. Nothing changes on disk. | The macOS VFS answers for a directory (`docs/spikes/M3-names.md`, functional suite: `unlink` of a directory gives `EPERM`). | 03 |

## Not compared, and why (not differences)

| What | Why |
|---|---|
| mtime and atime of entries the script does not set | They carry the wall clock of the run. The scripts never depend on it, so they are not compared. Entries set with `utime` (script 07, on files and directories) are compared exactly (100 ns FILETIME). |
| ctime | It cannot be set; every change moves it to now (`docs/spikes/M3-metadata.md`). |
| Creation time | Compared only for plausibility: a real time inside the run on both sides. The raw-seconds value that macOS stored before patch 0008 fails the case, so a build without patch 0008 is caught. |
| Owner, group, mode | Display values from `uid`, `gid`, `fmask` and `dmask`; nothing is stored without `permissions` (`docs/spikes/M3-permissions.md`). The dump mount is read-only with its own options. |
| MFT record numbers, volume serial, image bytes | Different on every `mkntfs` run. |
| Image hash | Not comparable between runs. Each image is checked with `ntfsfix -n` (exit 0 required) and hashed at every hop. |

## Same on both sides, recorded as evidence

Refused names (`windows_names`: `a:b`, `q?`, `trail.`, `trail `, `CON`, `nul.txt`, `a<b`, a backslash, a tab, `pipe|`) are refused with the same errno on A and B (script 05). Errors of rename, unlink, rmdir, link and symlink (`ENOENT`, `ENOTEMPTY`, `EEXIST`, `EPERM`, `EISDIR`) match, except where a row above lists them. Compressed directory (script 12) with `nocompression`: same flags, sizes and allocation. Symbolic links (script 11) are stored with the same target and flags.
