# Spike M3: file names and normalisation (T-M3-05)

Date: 2026-11-14. Answers U22; input for R-27 and criteria 3.3 and 3.4.
Cases: `tests/integration/functional/` (`func_ops.py`, `func_dirs.py`, `names.py`, `u22_names.py`, helpers in `fsops.py`).
Results: `tests/results/2026-11-14-26314ba-B-run2.json` (19 cases, Environment B, all pass),
`tests/results/2026-11-14-9cba2e6-host.json` (I-NAME-U22, A then B then A, pass),
`tests/results/2026-11-14-9cba2e6-A.json` (17 cases, Environment A reference, all pass).
Evidence: `tests/results/evidence/2026-11-14-26314ba-B-run2/` (`I-NAME-08.matrix.txt`, `I-NAME-08.worker-*.log`) and
`tests/results/evidence/2026-11-14-9cba2e6-host/` (`I-NAME-U22.phase-{a1,b,a2}.json`, `I-NAME-U22.treedump-linux-final.txt`).
Setup: macOS 27.2 guest, macFUSE 5.5.0 FSKit, NTFS-3G 2026.9.28 (`backend=fskit,local,norecover`); Linux side NTFS-3G 2026.9.28 in the container.

## U22 answers

1. **Which layer converts?** NTFS-3G, through its Darwin `nfconv` default (`ENABLE_NFCONV` is on in the guest build). Names from the kernel go through `ntfs_utf8_to_utf16` (to NFC) and names to the kernel through the UTF-16 to UTF-8 step (to NFD). With `-o nonfconv` the listing shows the bytes on disk and lookups are exact. The worker's debug log (`-o debug`) shows that macFUSE and FSKit pass the program's spelling through untouched: a lookup with NFC bytes reaches the worker as `LOOKUP /caf\xc3\xa9`, one with NFD bytes as `LOOKUP /cafe\xcc\x81`, in the default and the `nonfconv` worker alike. Evidence: `I-NAME-08.worker-default.log`, `I-NAME-08.worker-nonfconv.log`, `I-NAME-U22.phase-b.json` (`worker_lookups`).
2. **Two names on disk that differ only in normalisation, listed on the Mac:** both are listed, each as its NFD form, so the listing shows two entries with identical text (café NFC and NFD gave two NFD entries; the Angstrom sign U+212B, U+00C5 and A+U+030A gave three).
3. **Lookup by either form:** under the default mount a lookup of any spelling is converted to NFC, so it finds only the file whose on-disk name is that NFC form (the NFC file in every pair). The file whose on-disk name is not NFC cannot be opened, stat'ed or found by any spelling (ENOENT). This holds for files and for directories (g6 pair), and for a name that exists only in NFD on disk (no partner). Under `nonfconv` each spelling finds exactly its own file. Evidence: `I-NAME-U22.phase-b.json`, `I-NAME-08.matrix.txt`.
4. **Form of a name created on the Mac:** NFC on disk, whichever form the program typed (20 of 20 files and directories typed in NFC and NFD, checked on Linux with raw bytes and treedump). Typing both forms of one name on the Mac creates one file, not two (the second create opens the first).
5. **Can Windows open it?** Not tested here (T-M5-10). On disk the Mac-created names are NFC, which is what Windows tools normally write; names written by Linux or Windows in NFD are unreachable on the Mac by name (point 3).

## Hazards found (for ARCHITECTURE.md and the user docs)

- **Unreachable files.** Any name stored in a non-NFC form is listed on the Mac but cannot be opened, read, renamed or deleted by that name. `unlink` of the listed name of the café pair succeeded and deleted the other file (the NFC one); the NFD-named file stayed and is still unreachable (`hazard` in `phase-b.json`; Linux then showed only the NFD name). A recursive copy or delete on the Mac therefore acts on the wrong file or fails.
- **Two spellings, two caches.** Within one mount session macOS keeps one vnode per spelling of an NFC/NFD name, but NTFS-3G maps both to one file. After a write through the NFD spelling the NFC spelling still returned the old data and size for the rest of the session (also after `fsync`); after a rename of the NFD name the old NFC name still answered `stat`. A remount makes both consistent (I-NAME-01 records this and checks the on-disk result). The two spellings can also be seen as separate files by a tool that writes through both.
- **Names over 255 UTF-8 bytes** (CJK, emoji: 255 UTF-16 units allowed) are created and opened by exact name, but the listing returns them cut to 255 bytes (NTFS-3G `src/ntfs-3g.c`, "Returning file names larger than MAXNAMLEN"; the cut can fall inside a character). Nothing listed opens (ENOENT or EILSEQ), several long names can list as the same name, and `rmdir` of a directory holding only such names fails with ENOTEMPTY while the directory looks empty. R-27 says "hidden"; the observed behaviour is "listed cut". I-NAME-03.
- **Case-sensitive volume.** `Foo`, `foo` and `FOO` are distinct files and directories, lookups are exact (`fOo` is ENOENT), a case-only rename works (I-NAME-06). macOS tools that assume a case-insensitive volume may mishandle such pairs; Windows will see them but cannot open both.

## Other name results (I-NAME-*)

- 255 UTF-16 units work for ASCII, 85 CJK characters (255 bytes) and 126 units of emoji; 256 units give ENAMETOOLONG for create, mkdir and rename (I-NAME-03). The limit is in UTF-16 units, not bytes.
- 35 names (emoji with ZWJ, skin tone, flags; CJK; Hangul; Hebrew and Arabic, also with an RLM; Cyrillic; Greek; combining marks; spaces, dots and punctuation) as files and as directories survive a remount; the set is compared in NFC because the Mac lists NFD (I-NAME-02).
- `windows_names`: 21 file names and 5 directory names (colon, `"<>|*?\`, control character, trailing dot or space, `CON`/`PRN`/`AUX`/`NUL`/`COM1`/`LPT9` with and without extension) are refused with EINVAL for create, mkdir and rename; look-alikes such as `console`, `com10`, `CON1`, a leading space are accepted (I-NAME-04). Without the option all of them are created and survive a remount (I-NAME-05).
- Environment A (Linux) stores both forms as two files, as expected for the reference (I-NAME-01 on A).

## Functional suite (criteria 3.3)

I-FUNC-01..08, I-FUNC-DIR-01..03 and I-INT-CYCLE-01 pass in B and A. Each case ends with unmount, `ntfsfix -n`, read-only remount and SHA-256 of every file against an in-memory model. Behaviour recorded: delete of an open file keeps the descriptor usable and the name free; free space returns exactly after unlink; `rmdir` of a non-empty directory gives ENOTEMPTY; `unlink` of a directory gives EPERM; 10,000 entries create in about 100 s on the FSKit mount (the slowest case); 200 cycles over 50 files (about 950 MiB written) take about 45 s.

## Method notes

- `fsops.mount` adds extra options (`windows_names`, `nonfconv`, `debug`) on top of the adapter's mandatory ones, with a 40 s deadline and SIGKILL of the driver; it refuses `recover`, `remove_hiberfile` and `fsname`.
- With `debug` NTFS-3G still forks after start-up (`daemon(0, debug)`), so the process started exits 0 while the mount stays; the helper waits for the mount, not the process.
- I-NAME-U22 is an env-host case like C-REF-01: `tests/harness/run-env-b.sh --orchestrate --cases I-NAME-U22`.
