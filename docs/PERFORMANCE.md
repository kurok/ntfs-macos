# Performance (T-M5-07, criterion 5.6)

<!-- template: no results yet. Generate with `python3 tests/perf/report.py --a <A result.json> --b <B result.json> --out docs/PERFORMANCE.md` -->

> **NO RESULTS YET.** The criterion run has not been made. Run `sh tests/perf/run.sh --env A --out DIR --save-image IMG` and `sh tests/perf/run.sh --env B --out DIR --image IMG`, then the generator above. The tables will hold: sequential, random, small-file and directory-listing numbers for A (NTFS-3G on Linux) and B (this product), the slowdown with a flag above 2x, a raw-storage baseline for both environments and each phase's ratio against its own baseline, CPU and memory per phase, and the explanations of every flagged phase.

**VM numbers are not hardware numbers.** Both environments are virtual machines on one physical host; read the ratios, not the absolute values.

## Explanations

- **seq_write** (P-SEQ-01): Until 2026-11-20 the daemon's tier-0 disk I/O throttled `diskimages-helper` under the image node; fixed with passive disk I/O (docs/spikes/M5-perf.md). Smoke, not criterion: 1.81 MiB/s before, 89 MiB/s after (1.8x below the node). The rest is the FUSE round trip through macFUSE's FSKit module.
- **seq_read** (P-SEQ-02): Same cause and fix (docs/spikes/M5-perf.md). Smoke, not criterion: 2.18 MiB/s before, 56.9 MiB/s after (1.8x below the node, which itself reads far slower than A's image file).
- **rnd_write** (P-RND-01): Same cause and fix (docs/spikes/M5-perf.md). Smoke, not criterion: 95.7 ops/s before, 2009 ops/s after (1.3x below the node); the final `fsync` 4.5 s before, 0.07 s after.
- **rnd_read** (P-RND-02): Same cause and fix (docs/spikes/M5-perf.md). Smoke, not criterion: 62.6 ops/s before, 1834 ops/s after (2.7x below the node); the rest is about 0.5 ms per request through macFUSE FSKit and the worker.
