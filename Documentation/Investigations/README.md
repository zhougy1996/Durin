# Engineering Investigations

- [Completed CPU task system audit](CompletedTaskSystemAudit.md) — the completed
  graph and ownership design is sound, but Worker admission, terminal tracking,
  diagnostic cost, and terminal snapshot coherence need remediation before
  broad adoption.
- [DurinDevTool Windows lock recovery boundary](DurinDevToolWindowsLockRecovery.md) —
  Windows ACL recovery currently leaks Windows-only guidance through the
  cross-platform lock-opening path and lacks an automated live-lock integration
  test.
- [DHT and Ninja parallelism coordination](DHTNinjaParallelismCoordination.md) —
  Ninja cannot account for parser processes hidden inside module-level DHT
  commands, so fixed worker and pool limits cannot both avoid oversubscription
  and lend idle compiler capacity to critical-path reflection generation.
- [PBR pipeline production gaps](PBRPipelineProductionGaps.md) —
  HDR output, render passes, scene-lighting ownership, and stale editor controls
  retain ranked end-to-end gaps.
- [PSO cache for render-graph expansion](PSOCacheForRenderGraphExpansion.md) —
  Renderer-owned PSOs are sufficient today, but RDG work must measure duplicate
  creation, critical-path cost, and working-set growth before selecting cache,
  eviction, or precaching policy.

This index lists verified unresolved investigations only. Authoring and
lifecycle rules are in `AGENTS.md`; resolved history belongs in Git, an archived
implementation plan, and the resulting runtime, editor, development, or
workspace documentation.
