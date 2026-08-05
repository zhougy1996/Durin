# Engineering Investigations

- [DurinDevTool Windows lock recovery boundary](DurinDevToolWindowsLockRecovery.md) —
  Windows ACL recovery currently leaks Windows-only guidance through the
  cross-platform lock-opening path and lacks an automated live-lock integration
  test.
- [DHT and Ninja parallelism coordination](DHTNinjaParallelismCoordination.md) —
  Ninja cannot account for parser processes hidden inside module-level DHT
  commands, so fixed worker and pool limits cannot both avoid oversubscription
  and lend idle compiler capacity to critical-path reflection generation.
- [StaticMesh render-data lifetime](StaticMeshRenderDataLifetime.md) —
  StaticMesh replacement and destruction can retire uniquely owned render data
  before asynchronous proxy and resource teardown completes; the linked
  implementation plan adopts UE-style unique ownership with fenced retirement.
- [PBR pipeline production gaps](PBRPipelineProductionGaps.md) —
  HDR output, low-roughness BRDF behavior, render passes, scene-lighting
  ownership, and stale editor controls retain ranked end-to-end gaps.

This index lists verified unresolved investigations only. Authoring and
lifecycle rules are in `AGENTS.md`; resolved history belongs in Git, an archived
implementation plan, and the resulting runtime, editor, development, or
workspace documentation.
