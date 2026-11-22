<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Directory listing on the FSKit backend (I-RDIR-01..05)

A VM test (Environment B) for patch 0014 and ADR-027, and a regression check to rerun after every
macFUSE or NTFS-3G update. The macFUSE 5.5.0 FSKit module serves several enumerations from one FUSE
directory handle and rejects a READDIR reply that ends inside an entry; NTFS-3G's upstream readdir
triggers both (`EINVAL` from `readdir`, `ls` and `getattrlistbulk`).

| Case | What must hold |
|---|---|
| I-RDIR-01 | `readdir` + `unlink` of every entry, then `rmdir`: the directory ends empty and `rmdir` succeeds. Also run while another process lists the same directory in a loop with `ls -f` and with `getattrlistbulk`: with macFUSE 5.5.0 that leaves files (about 5 percent of 1000), because the FSKit module continues every enumeration of a directory from one shared FUSE handle, not from the caller's cookie (ADR-027). It prints `KNOWN` and does not fail the run unless `--strict`; `NOW-PASSES` means a macFUSE update fixed it. A run is `KNOWN` only when all of these hold: `readdir` reported no error; every `unlink` failure was `ENOENT` (a name listed twice through the shared position); the final listing worked; `rmdir` failed with `ENOTEMPTY`; and 0 < files left < n/4. Anything else fails the run, as do a failed listing of the lister (`EINVAL` would be a regression) and files left by the alone case. Each run prints an `INFO` line with the unlink failures grouped by errno |
| I-RDIR-02 | `seekdir` to a `telldir` cookie after reading to the end lists the same tail again |
| I-RDIR-03 | two descriptors on one directory, one handing off to the other (sequential and interleaved, 2 KiB and 16 KiB buffers): both list every name once |
| I-RDIR-04 | a directory that grows from INDEX_ROOT only to an index allocation during a listing: the earlier names are listed once, no error |
| I-RDIR-05 | `ls -f` and `getattrlistbulk` of 400 names of 58 bytes (more than 10240 bytes of listing) |

Not part of `scripts/test-all.sh --env B`: run it by hand from the host, after every macFUSE or NTFS-3G update (one disposable clone of `ntfsmac-golden`, deleted at the end; about 30 minutes with the defaults, most of it the 10000-entry cases):

    tests/integration/readdir/readdir_vm.sh [--sizes "1000 10000"] [--repeats 3] [--strict] [--timeout 5400] [--deps-tar FILE] [--out FILE]

It builds NTFS-3G with the patch series in the guest (or unpacks `--deps-tar`, checked like
`dmn_vm.sh` does), makes a fresh image file with `mkntfs` in the guest and runs
`readdir_cases.sh` there as the guest user, under a host-side deadline. Exit 0: all checks passed (KNOWN runs included); 1: a check failed; 2: a setup error or the deadline. To run inside a guest that already has NTFS-3G:

    cc -O2 -o dirtests dirtests.c
    NTFS3G=~/ntfsmac/deps/bin/ntfs-3g sh readdir_cases.sh -m ~/mnt IMAGE

`dirtests.c` also builds on Linux (the same cases without `bulk`). The mount uses the product's
options with `norecover`. After a session with tens of thousands of files, macFUSE can take minutes to
unmount (the module spins in `Volume.synchronize`); the script waits at most 300 s and notes it.
