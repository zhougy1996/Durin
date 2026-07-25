# Engineering Investigations

- [BuildTool Windows lock recovery boundary](BuildToolWindowsLockRecovery.md) —
  Windows ACL recovery currently leaks Windows-only guidance through the
  cross-platform lock-opening path and lacks an automated live-lock integration
  test.
- [DHT and Ninja parallelism coordination](DHTNinjaParallelismCoordination.md) —
  Ninja cannot account for parser processes hidden inside module-level DHT
  commands, so fixed worker and pool limits cannot both avoid oversubscription
  and lend idle compiler capacity to critical-path reflection generation.

This index lists verified unresolved investigations only. Authoring and
lifecycle rules are in `AGENTS.md`; resolved history belongs in Git, an archived
implementation plan, and the resulting runtime, editor, development, or
workspace documentation.
