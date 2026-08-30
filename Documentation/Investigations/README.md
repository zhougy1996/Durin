# Engineering Investigations

- [macOS MoltenVK argument-buffer instability](MacOSMoltenVKArgumentBufferInstability.md) —
  scene geometry is stable when MoltenVK uses discrete resource indexes; keep
  that qualified workaround until a LunarG SDK containing MoltenVK 1.4.2 or
  newer can be retested with Metal argument buffers enabled.
- [Editor icon atlas activation](EditorIconAtlas.md) —
  the current two-icon procedural viewport atlas is deterministic and adequate;
  an offline source-art and packing pipeline needs a larger scheduled icon set
  or an explicit visual-design requirement before implementation is justified.
- [Native graybox scene authoring expansion](NativeGrayboxSceneAuthoring.md) —
  the transaction-backed StaticMesh authoring service and create-only arena
  command are qualified, but a broader solution needs concrete workflows and a
  replacement model for object scope, persistent identity, edited-source
  ownership, and occupied-Level publication before implementation is planned.
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
This index lists verified unresolved investigations only. Authoring and
lifecycle rules are in `AGENTS.md`; resolved history belongs in Git, an archived
implementation plan, and the resulting runtime, editor, development, or
workspace documentation.
