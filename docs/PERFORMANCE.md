# Performance (T-M5-07, criterion 5.6)

<!-- template: no results yet. Generate with `python3 tests/perf/report.py --a <A result.json> --b <B result.json> --out docs/PERFORMANCE.md` -->

> **NO RESULTS YET.** The criterion run has not been made. Run `sh tests/perf/run.sh --env A --out DIR --save-image IMG` and `sh tests/perf/run.sh --env B --out DIR --image IMG`, then the generator above. The tables will hold: sequential, random, small-file and directory-listing numbers for A (NTFS-3G on Linux) and B (this product), the slowdown with a flag above 2x, a raw-storage baseline for both environments and each phase's ratio against its own baseline, CPU and memory per phase, and the explanations of every flagged phase.

**VM numbers are not hardware numbers.** Both environments are virtual machines on one physical host; read the ratios, not the absolute values.

## Explanations

Each entry is keyed to the run it was written for: `- **phase** (case) [run KEY]: text`. KEY is the run key that `tests/perf/report.py` prints for a pair of results (the first 7 characters of the `git_sha` of A and of B and 8 hex characters of the SHA-256 of the canonical JSON of both result files, `abc1234/abc1234/9f8e7d6c`, so two runs of one commit have two keys). When the report is generated, an entry whose key is not the current run key is printed as "(from run KEY, not re-confirmed)" and does not count as an explanation; `report.py` exits 1 while a phase above 2x has a placeholder or such an entry. To confirm an entry for a run, re-read it against that run's numbers, change the key in its brackets to the printed run key, and generate the report again; that edit goes into a commit that a reviewer sees, and `report.py` also exits 1 for smoke parameters. The four entries below were written about the smoke run (32 MiB, 2 repetitions; `docs/spikes/M5-perf.md`), which has no result file and no commit, so their key is `unkeyed`: they are not confirmed for any criterion run.

- **seq_write** (P-SEQ-01) [run unkeyed]: Until 2026-11-20 the daemon's tier-0 disk I/O throttled `diskimages-helper` under the image node; fixed with passive disk I/O (docs/spikes/M5-perf.md). Smoke, not criterion: 1.81 MiB/s before, 89 MiB/s after (1.8x below the node). The rest is the FUSE round trip through macFUSE's FSKit module.
- **seq_read** (P-SEQ-02) [run unkeyed]: Same cause and fix (docs/spikes/M5-perf.md). Smoke, not criterion: 2.18 MiB/s before, 56.9 MiB/s after (1.8x below the node, which itself reads far slower than A's image file).
- **rnd_write** (P-RND-01) [run unkeyed]: Same cause and fix (docs/spikes/M5-perf.md). Smoke, not criterion: 95.7 ops/s before, 2009 ops/s after (1.3x below the node); the final `fsync` 4.5 s before, 0.07 s after.
- **rnd_read** (P-RND-02) [run unkeyed]: Same cause and fix (docs/spikes/M5-perf.md). Smoke, not criterion: 62.6 ops/s before, 1834 ops/s after (2.7x below the node); the rest is about 0.5 ms per request through macFUSE FSKit and the worker.
