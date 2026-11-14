# Checkpoint

- **Phase:** M2 started: T-M2-01 (build-ntfs3g.sh) and T-M2-07 (fixtures, treedump) done. M1 tasks done. Done: T-M1-01, -02, -03, -05, -06, -07 (`scripts/ci-local.sh --quick`); audit fixes applied (b99d414, 6a3e5e0); re-audit `docs/audits/M1-recheck.md`: accept with fixes; all 15 findings applied (N-01..N-08 in 9d20cea, N-09..N-15 next commit; N-03 split-record crash accepted as residual R-33; new ADR-019/020, U15-U20). T-M1-04 done: `vm/macos/tart.sh selftest` OK on the `macos-golden-gate-base` image via `tart exec` (SSH from the desktop app is blocked by Local Network privacy).
- **Completed:**
  - Host and quota preflight (2026-11-13): macOS 27.2.1 arm64, Command Line Tools only (no Xcode),
    Docker CLI with no running daemon, no VM tool. Paragon NTFS and fuse-t 1.2.9 are installed;
    macFUSE is not.
  - Architecture pass (Fable 5.2, single run): `docs/ARCHITECTURE.md`, `TECHNICAL_DECISIONS.md`,
    `IMPLEMENTATION_PLAN.md`, `AGENT_TASKS.md`, `TEST_STRATEGY.md`, `RISK_REGISTER.md`.
- **Decisions:** NTFS-3G 2026.9.28 as an unprivileged worker on macFUSE 5.5.0 FSKit backend;
  root broker daemon + read-only safety probe; `norecover` always, `recover`/`remove_hiberfile`
  banned; arm64 only; GPL-2.0-or-later; macFUSE is a user-installed prerequisite, never bundled.
- **Open:** see [NEXT_ACTIONS.md](NEXT_ACTIONS.md) and `docs/AGENT_TASKS.md`.
- **Known issues / unverified:**
  - R-01: whether libfuse2-based `ntfs-3g` can use `backend=fskit` at all is unproven (T-M2-03).
  - Durability limited to volume sync points and clean unmount (ARCHITECTURE.md section 9).
  - Open unknowns in ARCHITECTURE.md section 11 (now up to U20), each mapped to a task.
  - No mount tests on this host: Paragon NTFS claims NTFS volumes first. All mounts run in the VM.
- **Last test results (2026-11-13/14):** fetch test ALL PASSED (host); CMake release/asan/ubsan + linux ctest 100% (host, debian:13-slim); `vm/linux/run.sh selftest` OK release and ASan; `vm/macos/tart.sh selftest` OK (2026-11-14, base image, tart exec). `scripts/ci-local.sh --quick` PASSED and `tests/unit/ci/test_ci_local.sh` 9/9 (host, 2026-11-14).
- **Policy:** repo public; no GitHub-hosted CI or binaries (ADR-015); commits as `kurok` noreply.
- **Git revision:** see `git log -1`.

## Resume

```bash
git clone https://github.com/kurok/ntfs-macos.git && cd ntfs-macos
```

Then start with NEXT_ACTIONS.md. Tasks needing no human action can begin immediately.
