# Next actions

1. Environment and quota preflight: confirm model availability (Fable 5.2, Sonnet 5.5,
   Opus 5.5) and free allowance; check Docker/Podman and macOS VM tooling on the host.
2. Phase 1 (Fable 5.2): research macFUSE 5.5+ FSKit backend, NTFS-3G 2026.9.28+, FSKit,
   Disk Arbitration, licensing. Produce `docs/ARCHITECTURE.md`, `TECHNICAL_DECISIONS.md`,
   `IMPLEMENTATION_PLAN.md`, `AGENT_TASKS.md`, `TEST_STRATEGY.md`, `RISK_REGISTER.md`.
3. Choose the project license based on the licensing review.
4. Milestone 2: build NTFS-3G against macFUSE (FSKit) and mount a disposable image in a
   macOS 27 VM.
