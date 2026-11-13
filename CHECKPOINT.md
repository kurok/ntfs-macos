# Checkpoint

- **Phase:** 1 complete (architecture). Milestone 1 implementation tasks not started.
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
  - 11 open unknowns in ARCHITECTURE.md section 11, each mapped to a task.
  - No mount tests on this host: Paragon NTFS claims NTFS volumes first. All mounts run in the VM.
- **Last test results:** none (no code yet).
- **Git revision:** see `git log -1`.

## Resume

```bash
git clone https://github.com/kurok/ntfs-macos.git && cd ntfs-macos
```

Then start with NEXT_ACTIONS.md. Tasks needing no human action can begin immediately.
