# Crash-consistency sweep (T-M3-14, criterion 3.9, `S-CRASH-*`)

Linux only, Environment A (internal FUSE, container). Nothing is mounted on the host.

```
tests/stress/crashsweep/run.sh --mode short   # 50 cut points x 5 workloads (CI stage linux-crash-sweep-short)
tests/stress/crashsweep/run.sh --mode full    # 110 x 5 = 550 cut points (criterion 3.9 needs >= 500)
tests/stress/crashsweep/run.sh --plan short   # torn write short:N:512 instead of cut:N (supplementary, not gated)
tests/stress/crashsweep/run.sh --plan skip    # drop write N only, keep later writes: reordering (supplementary, not gated)
```

Files: `build.sh` (builds release and `--enable-faultio` NTFS-3G, proves the release build is clean),
`crashsweep.py` (workloads, sweep, checks), `inner.sh` (container entry), `run.sh` (host entry).
Results go to `tests/results/evidence/m314/<run>/`: `results.tsv`, `results.jsonl`, `summary.json`,
`build-proof.txt`, `sweep.log`. The image of a failing point (gzip) and its event log go to
`build/crashsweep-failing/<stamp>/` (git-ignored), never into the evidence directory.

## What is run

Patch 0004 (`third_party/patches/ntfs-3g/0004-faultio-device-layer.patch`) adds the layer only when
configured with `--enable-faultio`. Plans (env `NTFSMAC_FAULT_PLAN`): `count`, `cut:N`, `eio:N`,
`dead:N`, `short:N:K`, `delay:MS`. The sweep also sets `NTFSMAC_FAULT_STATE` (running write counter in
a file, read by the workload), `NTFSMAC_FAULT_TRACE` (offset, length and magic of every write, including
the one about to be dropped) and `NTFSMAC_FAULT_KILL=1` (see below). Plan `skip:N` drops write N only.

Per workload (`smallfiles`, `seqwrite`, `renamechurn`, `overwrite_truncate`, `dirtree`):

1. Plan `count`: the workload runs to the end, then a clean unmount. Gives W (device writes) and is the
   no-fault control: every claim must hold, the walk must be clean.
2. For K cut points spread evenly over 1..W: fresh `mkntfs` image (64 MiB, 4096-byte clusters), mount
   with the fault build (`-o rw,norecover,big_writes`), run the workload, unmount, check.
3. Control `cut:0`: the image must be byte-identical to the fresh image.

Cut semantics in the sweep: writes 1..N reach the image, write N+1 is never issued: the layer kills
`ntfs-3g` with SIGKILL at that point. Without `NTFSMAC_FAULT_KILL` the layer follows the documented
plan table (later writes are dropped and report success); in the first trial that made `ntfs-3g` read
back stale metadata and spin at 100 % CPU in one dirtree point, which no real power loss can do.
`fsync` of a file or directory in NTFS-3G syncs the whole device (`ntfs_fuse_fsync`), so the workload
reads the write counter right after `fsync` returns: that counter value `w` is the claim time.

## What is checked, and how each outcome is treated

The checks run with the release build (no fault layer), always with `norecover`.

| Check | Pass | Fail |
|---|---|---|
| mount | `ntfs-3g -o ro,norecover` mounts the image | any refusal (message recorded) |
| fsck | `ntfsfix -n` exits 0 | any other exit status (last lines recorded) |
| claims | every file whose `fsync` (file and parent directory) returned at write count `w <= N` has the claimed size and SHA-256 of the claimed prefix, unless an operation on that path started before write N (recorded as a "touch" with the counter before the operation, so a later rename, unlink or overwrite that may be partly on disk cancels the claim) | missing, short, unreadable or different |
| walk (advisory) | every directory listed, every file read to the end | counted as `walk_dirty`, not a failure of 3.9 |
| mirror_split | a failing cut point is classified when all of: the refusal and the `ntfsfix -n` message are exactly `$MFTMirr does not match $MFT (record k).`, k in 0..3, and nothing else failed; the write trace shows write N is `$MFT` record k and write N+1 (never issued) is `$MFTMirr` record k; after copying `$MFT` record k over the mirror record on a scratch copy, mount, `ntfsfix -n`, all claims and the walk pass | any other failure is unclassified |
| rw probe (informational) | `ntfs-3g -o rw,norecover` on the finished image: mounted rw, mounted ro, or refused | none |

"Mounts read-only and `ntfsfix -n` clean" means exactly the first two rows. Facts about NTFS-3G
2026.9.28 that decide how they read:

- NTFS-3G has no journal it replays. `norecover` stops it from resetting an unclean `$LogFile`
  (`recover` would write to the image), so a read-only mount never repairs anything. Whatever is on
  disk at the cut is what the checks see.
- NTFS-3G does not set `VOLUME_IS_DIRTY` when it mounts read/write (that is what `ntfsmac-volmark`,
  T-M3-12, adds). A cut image therefore looks clean to the rw probe: 532 of 550 rw probes in the full
  run mounted read/write. The rw probe tells nothing about consistency; the daemon must not rely on
  NTFS-3G refusing a rw mount after a crash.
- `ntfsfix -n` is a shallow check (boot sector, `$MFT` against `$MFTMirr`, `$LogFile`; it never walks
  indexes or runlists). Exit 0 does not mean the tree is intact. That is why the claims and the walk
  exist, and why walk failures are reported separately: a half-created file (directory entry whose MFT
  record is not yet written, or file size updated before its clusters) is an inherent non-atomic window
  of a filesystem without a journal, class (a) of TEST_STRATEGY section 6, not a failure of 3.9.
- Mount refused or `ntfsfix -n` nonzero is a failing cut point, recorded with the exact message. The one
  known cause is `mirror_split` (docs/spikes/M3-crashsweep-triage.md): `ntfs_mft_records_write()` writes
  `$MFT` record 0 and `$MFTMirr` record 0 as two device writes with no flush between them, so a cut
  between them leaves the copies different, and NTFS-3G refuses even a read-only mount. It is upstream
  class (a), reported with its count and not failing. `ntfsfix -n` prints "OK" for corrections it did not
  write and tries a read/write `ntfs_mount` (`ntfsfix.c:1649`); its text is never taken as evidence of a repair.

## Gate (modes short and full, plan cut)

Exit 0 iff all controls pass (10 of 10), zero unclassified failures, zero broken claims, zero workload
hangs. `mirror_split` and `walk_dirty` counts are reported in `summary.json` (`gate` field) and never fail;
no count is pinned. The sweep's fault model is writes persisting in issue order (the image is a file and
the kernel keeps every issued write after SIGKILL); `--plan skip` is a supplementary reordering check (a
skipped mirror write alone also gives the mirror window, and is not auto-classified).

Torn writes (`--plan short`) are supplementary: they tear multi-sector structures (1024-byte MFT
records, 4096-byte index blocks) which NTFS protects only by the update-sequence check, so many more
points fail (mount refused with "Incomplete multi-sector transfer", claims in the damaged record's
neighbourhood broken). They are not part of 3.9.
