# Engineering Investigations

- [Native Vulkan presentation pacing on Windows](NativeVulkanPresentationPacing.md) —
  readiness, completion and UE-style synchronization probes retain native desktop
  stalls or reduce window presentation throughput; uncapped offscreen rendering
  alone does not establish an uncapped native presentation repair.
- [Windows uncapped presentation responsiveness](WindowsUncappedPresentationResponsiveness.md) —
  non-pumping owner-thread waits expose a Win32 WSI wait-cycle hazard;
  a bounded A/B reproduces desktop stutter with continued engine frame progress
  and isolates a presentation/composition correlation; scheduling attribution
  remains unresolved.
- [Slang reflection context lifetime](SlangReflectionContextLifetime.md) —
  a recycled-thread session-pool defect is repaired; the original SDK reflection
  crash's ownership and causal relationship to that defect remain unproven.
- [Vulkan integration driver worker crash](VulkanIntegrationDriverWorkerCrash.md) —
  normal-run captures locate an invalid indirect callback in an NVIDIA worker;
  the callback record's corruption or lifetime owner remains unresolved.
- [RHI creation qualification attribution](RHICreationQualificationAttribution.md) —
  validation DLL load/unload reproduces private-memory retention independently
  of the engine; allocator-retention stacks and following-frame host-wait
  attribution remain unresolved.
- [macOS MoltenVK argument-buffer instability](MacOSMoltenVKArgumentBufferInstability.md) —
  scene geometry is stable when MoltenVK uses discrete resource indexes; keep
  that qualified workaround until a LunarG SDK containing MoltenVK 1.4.2 or
  newer can be retested with Metal argument buffers enabled.
- [Editor icon atlas activation](EditorIconAtlas.md) —
  the current three-icon procedural viewport atlas is deterministic and adequate;
  an offline source-art and packing pipeline needs a larger scheduled icon set
  or an explicit visual-design requirement before implementation is justified.
- [DHT and Ninja parallelism coordination](DHTNinjaParallelismCoordination.md) —
  Ninja cannot account for parser processes hidden inside module-level DHT
  commands, so fixed worker and pool limits cannot both avoid oversubscription
  and lend idle compiler capacity to critical-path reflection generation.
- [Metal unsynchronized drawable pacing](MetalUnsynchronizedDrawablePacing.md) —
  native controls isolate a titled/opaque/three-drawable display-path throttle;
  title-bit and opacity transitions reverse it, and traces confirm direct-display
  versus compositor output. Production now fixes the pool at two; private
  system policy and cross-machine behavior remain unresolved.

This index lists verified unresolved investigations only. Authoring and
lifecycle rules are in `AGENTS.md`; resolved history belongs in Git, an archived
implementation plan, and the resulting runtime, editor, development, or
workspace documentation.
