# AGENTS.md

Working rules for AI agents on this repo. The full brief is in
[docs/PROJECT_BRIEF.md](docs/PROJECT_BRIEF.md); this file is the short version.

## Roles and models

| Phase / role | Model |
|--------------|-------|
| Phase 1 architecture (one focused pass, then released) | Fable 5.2 |
| A Coordinator | Sonnet 5.5 |
| B Filesystem engineer | Sonnet; Opus 5.5 only for hard correctness / data-loss issues |
| C macOS integration engineer | Sonnet |
| D Testing and reliability engineer | Sonnet |
| E Independent audit / blame | Sonnet; Opus for complex findings. Never approves its own changes. |

No Haiku, no Fable after Phase 1.

## Hard rules

- Data corruption is release-blocking. Unsafe/unknown volume state → read-only.
- Never discard Windows hibernation state or run destructive NTFS repairs automatically.
- Never write to physical media or a user's data drive without explicit human approval.
- Never disable SIP, lower security settings, or replace Apple binaries.
- No paid overage. If free allowance can't be verified or is exhausted: stop, write
  `CHECKPOINT.md` and `NEXT_ACTIONS.md`, and do not retry.
- Don't claim macOS behavior from Linux-only tests.
- Small, reviewable commits; tests with significant changes; no secrets.
- Use git worktrees for parallel work; no two agents edit the same files.
