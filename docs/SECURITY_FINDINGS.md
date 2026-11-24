<!-- SPDX-License-Identifier: GPL-2.0-or-later -->
# Security findings from fuzzing (T-M5-09)

This file classifies every crash, oracle failure, leak and sanitizer report that the fuzz campaigns
produced, and says where each one lives (project code or NTFS-3G), how bad it is, and what was done.
Per-run statistics and the first findings (F1 to F8) are in `tests/fuzz/TRIAGE.md`; upstream status is
in `UPSTREAM_REPORTS.md`; the carried fixes are patches 0010 to 0013 and 0015 (ADR-026).

## 1. How findings are found and recorded

| Item | Where |
|---|---|
| Campaign driver (any budget, Environment A) | `tests/fuzz/campaign/run.sh` (units in `inner.sh`, signatures in `fuzzsig.py`, merge and table in `triage.py`) |
| Targets | F-PROBE, F-MOUNT-WALK (`tests/fuzz/image/`), F-SANITIZE-01..03 (`tests/fuzz/sanitize/`), F-CTL-01 (`tests/fuzz/ctl/`, new in T-M5-09) |
| Known findings and how to recognise them | `tests/fuzz/campaign/known.tsv` |
| Regression inputs (one file per finding) | `tests/fuzz/findings/` for findings of the campaigns of M5 (`M5-NN-<target>.bin`; `run.sh --adopt` writes `N-<hash>-<target>.bin`, named by the signature, and never overwrites); `tests/fuzz/image/findings/` for F1 to F8 |
| Triage table of a run | `triage.md` and `triage.tsv` in the run's result directory (`build/fuzz/campaign/<stamp>/`, git-ignored; the table of a recorded campaign is copied into section 4 of this file) |

The oracle of every target is the one in `docs/TEST_STRATEGY.md` section 8. A finding is: a sanitizer report
(ASan, UBSan in project code), a harness oracle failure, a libFuzzer timeout or out-of-memory, a deadly
signal, or a leak found by the replay of the grown corpus with LeakSanitizer.

Signature. The kind (`asan:heap-buffer-overflow`, `leak`, `ubsan`, `oracle`, `timeout`, `oom`, `crash`), the
access (READ or WRITE), the first three frames that are neither sanitizer runtime, libFuzzer nor libc, and the
file and line of the top frame. The signature of a replay is its first fatal event: the engine is built with
recoverable UBSan, so a benign UBSan line that precedes a crash never decides the signature; UBSan counts only when
nothing else happened. A known finding is matched by kind and by the top frame (`known.tsv` anchors the patterns
at the first frame, and UBSan lines at `file:line` exactly). Two crashes with the same
signature are one finding; the smallest input is kept and minimised (`libFuzzer -minimize_crash`, kept only if
the signature does not change).

The short fuzz stage of `ci-local.sh` (`fuzz-short`, `tests/fuzz/{image,sanitize}/run.sh --known-ok --seconds 300`)
forgives only the leaks of the corpus replay that match a row of `known.tsv` with status `open` or `benign`
(`tests/fuzz/campaign/shortcheck.py`, which uses the signatures of `fuzzsig.py` and `triage.classify`, the function
of the campaign triage); the stage note lists the ids, for example `known findings seen: F2, F5`. The stage always
builds the patched engine, so a `fixed` row seen there is a `REGRESSION`, and an unmatched leak is `NEW`; both fail.
The image targets replay every corpus input one process per input (in chunks of 64 that are split on a leak; a
single libFuzzer process over the corpus stops at the first leak, so a known leak would hide a new one) and write
`replay complete` to the summary only when all inputs were replayed within the time bound. Every artifact (crash,
timeout, out of memory, slow unit), every libFuzzer run with a non-zero exit, a missing `replay complete`, an
AddressSanitizer or libFuzzer error in the leak log and any mismatch between the leaks counted and the leaks
parsed fail the stage whatever their signature. The flag does
nothing for the sanitize targets (project code, no leak replay).

Not covered: the "read/write operation sequences on a valid image" target of `TEST_STRATEGY.md` section 8
does not exist (T-M3-16 built the other four rows). The write paths of libntfs-3g (`mft.c`, `inode.c`
creation, `attrib.c` resizing) are therefore fuzzed only through the read-only mount and walk; this is a gap
for M7 criterion 7.2, not closed here.

## 2. Classification scheme

Every finding gets three labels.

**Component** (where the faulting code is, taken from the first frame outside runtime and libc):

| Label | Meaning |
|---|---|
| project | `src/`, or the oracle of a harness in `tests/fuzz/`: our code is wrong |
| engine | libntfs-3g (the pinned release plus our carried patches): the fault is in upstream code. The caller is recorded; if our call is what makes the engine misbehave (a bad argument), the finding is project |

**Class and exploitability** (the automatic class is a first guess from the sanitizer report; a human
confirms it in the table of section 4):

| Class | What | Exploitable? | Typical reaction |
|---|---|---|---|
| W1 | out-of-bounds write, use after free, double or invalid free | possible: treat as code execution until shown otherwise | block the release; fix or carry a patch; private report |
| R1 | out-of-bounds read of heap, stack or global memory | information leak or crash; not code execution by itself | fix or carry a patch; private report if the data is attacker-reachable |
| R2 | uncontrolled allocation, memory limit, timeout, hang | denial of service only | fix or carry a patch if reachable from the mounted volume; public issue otherwise |
| C | crash without memory corruption (null dereference, abort) | denial of service | as R2 |
| O | oracle failure in project code | decided by hand (a wrong verdict can be a security defect, for example `clean` for a dirty volume) | fix; regression input |
| L | memory leak | no; bounded per process (the probe and the worker are short-lived) | public issue; no local fix |
| U | undefined behaviour without a detected memory error | no, as built (arm64 and x86-64, clang and gcc) | list; one report per file when upstream takes it |
| S | slow unit that does not fail on replay | no | note |
| X | anything else | decided by hand | decide, then reclassify |

"Reachable" is judged against the product: the probe opens the device read-only and never reads security
descriptors, extended attributes or reparse data (`tests/fuzz/TRIAGE.md`); the mounted volume (FUSE
side) does, so findings in those paths matter behind the probe verdict. All inputs are crafted volumes: the
attacker needs a disk or image the user attaches.

**Status**: `known` (matches `known.tsv`), `NEW` (no match: classify by hand, add a regression input and a
row to `known.tsv`), `REGRESSION` (a finding that a carried patch fixes appeared on an engine that has the
patch, or a finding fixed in project code appeared at all), `REVIEW` (an artifact that does not reproduce, or a
libFuzzer exit without an artifact, for example 137 from the container being killed), `info`. A UBSan location of the
engine that is not in the known list also fails the run.

## 3. Findings before the campaign

| Id | Component | Class | Exploitable | Status | Fix | Upstream |
|---|---|---|---|---|---|---|
| F1 `retrievesecurityattr()` allocation from an on-disk size | engine | R2 | denial of service | fixed locally | patch 0012 | GHSA-r4cv-86xq-37qj, filed |
| F3 `ntfs_valid_descr()` reads header before length check | engine | R1 | information leak or crash | fixed locally | patch 0010 | GHSA-g3h3-7rhv-rrmq, filed |
| F4 `ntfs_possible_symlink()` reads tag without a size check | engine | R1 | information leak or crash (a plain `ls` reaches it) | fixed locally | patch 0011 | GHSA-vp2x-xjmj-x4x2, filed |
| F6 `ntfs_inode_sync_file_name()` reads tag without a length check | engine | R1 | information leak into the index entries; read-write mounts only | fixed locally | patch 0013 | drafted, not filed; no reproducer |
| F8 `ntfs_boot_sector_parse()` accepts MFT and INDX record sizes up to 2 GiB; `ntfs_mft_load()` allocates the MFT record size (N-37cd62, campaign of 2026-11-23) | engine | R2 | denial of service (allocation of up to 2 GiB on mount; `1 << 31` overflow) | fixed locally | patch 0015 (limit 4096 bytes) | drafted, not filed |
| F2 `ntfs_mft_load()` leaks the run list | engine | L | no | open | none | public issue drafted |
| F5 `ntfs_make_symlink()` leaks the attribute | engine | L | no | open | none | public issue drafted |
| F7 `ntfs_device_mount()` leaks the `$AttrDef` inode | engine | L | no | open | none | public issue drafted |
| U undefined behaviour (shifts, signed overflow, misaligned loads) in `bootsect.c`, `volume.c`, `runlist.c`, `attrib.c` (385, 386, 1040, 1073, 1172), `unistr.c`, `reparse.c` | engine | U | no, as built | open | none | not reported |

Project code: no finding of any class in the targets F-PROBE, F-MOUNT-WALK, F-SANITIZE-01..03 up to the
runs of 2026-11-16 (`tests/fuzz/TRIAGE.md`).

## 4. Campaign of M5 (T-M5-09)

### 4.1 Status

The full 8 CPU-hour campaign has **not been run yet**; this task built it and checked it with smoke runs
(section 4.3). The acceptance criterion ("every crash classified: project code or engine, exploitable or
not") is met for what the smoke runs produced and is met for the full run by the same procedure: run the
command below, copy `triage.md` into this section, classify every `NEW` row by hand, add a row to
`tests/fuzz/campaign/known.tsv`, and keep its input in `tests/fuzz/findings/` (`run.sh --adopt` copies it). `run.sh` also fails when a planned unit has no summary, when a unit exits with a
status other than 0, or 1 with a finding, and when a new UBSan location appears.

Full campaign (Environment A, ASan + UBSan, clang 19.1.7, aarch64 container; no network, no sudo):

```
sh tests/fuzz/campaign/run.sh --budget-hours 8 --jobs 5 --out build/fuzz/campaign/m5-full
```

Run it on a clean tree (a dirty tree is refused unless `--allow-dirty`). It writes `build/fuzz/campaign/m5-full/result.json`
(`F-CAMPAIGN-01`, with the commit that was fuzzed and the tree state at the start and the end; any budget below 8 CPU-hours,
a missing target or `--no-patches` gives `F-CAMPAIGN-01-SHORT`). After classifying every `NEW` row (add the row to `known.tsv`), commit the classification, then re-run only the triage on the same directory:
`python3 -I -B tests/fuzz/campaign/triage.py --results build/fuzz/campaign/m5-full --known tests/fuzz/campaign/known.tsv --plan "<units from run.sh>" --budget 28800 --result-out build/fuzz/campaign/m5-full/result.json`.
The result is written for the CURRENT commit only when the tree is clean now and every path changed since the fuzzed commit
(`provenance.txt`) is exactly `tests/fuzz/campaign/known.tsv`, `docs/SECURITY_FINDINGS.md`, `docs/UPSTREAM_REPORTS.md`, `CHECKPOINT.md` or `NEXT_ACTIONS.md`, or under `docs/audits/`, `tests/results/` or `tests/fuzz/findings/` (the minimised inputs),
so that the fuzzed code is unchanged: `git_sha` is then HEAD and the case note names the fuzzed commit. Any other change since
(code), or an uncommitted `known.tsv`, marks the result dirty and fails it with the reason. Then `python3 tests/harness/report.py adopt --env A --dir build/fuzz/campaign/m5-full`
puts it into `tests/results/` (docs/TEST_STRATEGY.md section 10).

Plan: 10 units, one process each, 2880 s of fuzzing each = 8.0 CPU-hours: F-MOUNT-WALK 4 shards, F-PROBE 2
shards, F-SANITIZE-01, -02, -03 and F-CTL-01 one each; shards use different seeds and their own corpus copy.
Five containers at a time on the 18 CPUs of the Docker VM, so two waves: expected wall time 1 h 45 min (2 x (48 min
fuzzing + about 1 to 3 min for the engine build, minimisation and the leak replay)). The jobs and the RSS limit of
each process are sized from the Docker VM (`docker info`: CPU count and memory), not the host: 85 percent of the
VM memory divided by the jobs, at most 2048 MB (1348 MB here with 7.9 GB and 5 jobs; peak RSS was 463 MB for
F-MOUNT-WALK in the smoke runs). The time is CPU time of the libFuzzer processes (user + system), not wall time,
and `triage.py` fails the run when the total is below 95 percent of the budget (a unit that stopped early, for
example after the restart limit of 80). A lower budget
is `--budget-seconds N`; `--dry-run` prints the plan.

### 4.2 What the driver does with a crash

1. The unit replays every artifact (`crash-*`, `oom-*`, `timeout-*`, `leak-*`, `slow-unit-*`) and takes its
   signature (section 1). An artifact that does not fail on replay becomes `REVIEW` (or `info` for a slow unit).
2. Equal signatures are one finding. The smallest input is minimised with `-minimize_crash`; it is kept only if
   the signature stays the same.
3. F-PROBE and F-MOUNT-WALK fuzz with leak detection off (a leak ends a libFuzzer process); the grown corpus is
   replayed once with LeakSanitizer and every distinct leak gets a signature. An unexplained leak is traced back
   to the corpus input that shows it (one run per file, capped at 10 minutes).
4. `triage.py` merges the units, matches each signature against `known.tsv` and writes `triage.md` and
   `triage.tsv`. A finding that a carried patch fixes and that shows up on an engine with that patch is a
   `REGRESSION`; on a pristine engine (`--no-patches`) it is the expected result and a check that the matching
   works. Exit status 1 means a new finding, a regression or a `REVIEW` row.
5. UBSan reports of the engine are recoverable and collected per location; locations that are not in the
   known list are reported as new.

### 4.3 Smoke campaign (this task)

Runs of about 2 to 6 minutes each, all in Environment A, results kept outside the repository. They check the
driver; they are not the 8 CPU-hours.

| Run | Command (abridged) | Units | CPU time | Result |
|---|---|---|---|---|
| 1 | `run.sh --budget-seconds 720 --shards mountwalk=1,probe=1 --jobs 6` | probe, mountwalk, verdict, label, display, ctl; 120 s each | 649 s | one project-code finding (M5-01, below) and the leaks F2 and F7 recognised as known (F7 first showed as new until the match rule was fixed); no other crash |
| 2 | the same with `--budget-seconds 300 --targets label,probe,mountwalk` with the label harness skipping M5-01 (an interim measure, removed later) | label, probe, mountwalk; 100 s each | 305 s | no finding in label; F2 and F7 known; 0 new |
| 3 | `run.sh --budget-seconds 200 --targets probe,mountwalk --no-patches` (default shards, 33 s each) | mountwalk x4, probe x2, pristine NTFS-3G 2026.9.28 | 204 s | self-test of the triage: F1 (allocation, and the out-of-memory form), F3, F4, F2 and F7 all found from the reproducer seeds and by mutation and matched to their ids; 0 new, 0 regression |
| 4 | triage of run 3 with the engine marked as patched (edited summaries) | | | F1, F3 and F4 become `REGRESSION` and the exit status is 1 |
| 5 | after the review fixes (CPU-time accounting, M5-01 fixed, no skip): `run.sh --budget-seconds 480 --shards mountwalk=1,probe=1 --jobs 6` | all six targets, 80 s each | 484 s | exit 0: no finding in verdict, label (clean with M5-01 fixed), display, ctl; F2 and F7 known; UBSan locations all known |
| 6 | the same on pristine NTFS-3G: `run.sh --budget-seconds 160 --targets probe,mountwalk --shards mountwalk=1,probe=1 --no-patches` | mountwalk, probe; 80 s each | 160 s | F1, F3, F4, F2 and F7 recognised as known; exit 0 |

The first run also shows that F-CTL-01 works: 16.0 million executions in 120 s (about 130,000 per second), 402
edges, no oracle failure (the same oracles as U-CTL-FUZZ-01, which runs 200,000 inputs in ctest).

Smoke-run coverage numbers (edges): F-MOUNT-WALK 4097 in 120 s (4756 after one hour in T-M3-16), F-PROBE 3048,
F-SANITIZE-01 582, F-CTL-01 402. These are low-confidence figures for a two-minute run.

### 4.4 Findings of this task

| Id | Component | Class | Exploitable | Status | Input | Reference |
|---|---|---|---|---|---|---|
| M5-01 | project: `ntfsmac_label_part_ok()` (`src/common/label_cp.c`), used by rule 2 of `argv_check` | O | no | **fixed** in this task | `tests/fuzz/findings/M5-01-label.bin` (10 bytes), ctest `fuzz.findings.M5-01` | none (project code) |

No new finding in NTFS-3G: every engine signature of the smoke runs is one of F1 to F4, F7 or the known UBSan
locations. No upstream report was drafted, because there is no new engine finding, and nothing was sent anywhere.

**M5-01: `argv_check` accepted a leaf that starts with a hyphen.** F-SANITIZE-02 fed the raw label `-` (8 bytes of
serial `ffffffffabcd012f`, choice byte `0x61`, label `-`) to `ntfsmac_argv_check_text()` as the mount-point leaf.
The check accepted it; the reference rule of the label sanitiser (ADR-016 as amended by audit M4 F-01, a leading
`-` becomes `_`) refuses it. T-M4-12 changed the sanitiser and the test oracle (`tests/unit/sanitize/oracle.h`)
but not `ntfsmac_label_part_ok()`, so the one-hour run of T-M3-19, made before that change, could not see it. Why it
was not exploitable: the builder passes the leaf through the sanitiser first (it never starts with `-`);
`argv_check` also requires the leaf to equal the one the daemon expects; and the leaf is the tail of the absolute
path `/Volumes/-`, which a program cannot read as an option. It was a guard weaker than the rule it backs up.

Fix (T-M5-09 review): `ntfsmac_label_part_ok()` refuses a leading `-` (the same test as `oracle_label_violation()`);
the rule-2 comment in `argv_check.c` says so. Tests: `U-ARGV` in `tests/unit/policy/test_options.c` now has the
leaves `-` and `-x` among the leaves the check must refuse (fails without the fix); `fuzz.findings.M5-01` replays
the 10-byte input through F-SANITIZE-02 (aborts on the oracle without the fix); `known.tsv` marks M5-01 as fixed,
so it reappearing in a campaign is a REGRESSION. The temporary skip in `fuzz_label.c` was removed: F-SANITIZE-02
checks every leaf again, in both directions.

### 4.5 Open points for the full run

- The "read/write operation sequences" target (section 1) is still missing.
- F-CTL-01 builds the whole tree with the sanitizer flags and `-D_GNU_SOURCE` because the daemon sources are
  written for macOS and do not compile on Linux with strict C17; the fake system and fake operations of
  `tests/unit/ctl/fakes.h` stand in for the operating system, so the XPC and socket I/O code is not fuzzed,
  only the parser, the handler and the five peer checks.
- Engine leaks (F2, F5, F7) end a libFuzzer process, so the targets that mount fuzz with leak detection off; a
  leak that needs an input that added no coverage is not found.
