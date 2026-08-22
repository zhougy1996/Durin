# PSO Cache for Render-Graph Expansion

**Status:** Open; reevaluate when render-graph/RDG work begins
**Last reviewed:** 2026-08-07

## Scope And Verdict

The current Renderer directly owns the graphics PSOs it creates. This is an
intentional fit for the present fixed rendering paths: renderer resource
payloads and creation slots retain `FGraphicsPipelineStateRHIRef` values, while
recorded RHI commands retain their own counted references until replay. There
is no verified need for a shared runtime PSO-object cache today.

Render-graph/RDG work is the next required reevaluation point, not an automatic
requirement to add a cache. A graph may increase the number of independently
described passes, render-target layouts, material permutations, and transient
owners that request equivalent PSOs. The unresolved question is whether that
expansion produces repeated expensive creation on the critical rendering path
or a working set that renderer-local ownership can no longer manage clearly.

## Verified Current Behavior

### P1 — PSO creation has no shared object cache

`RHICreateGraphicsPipelineState()` reaches the Vulkan pipeline manager, whose
creation entry point returns a newly allocated
`FVulkanGraphicsPipelineState`. Repeated equivalent initializers are not
deduplicated at this boundary.

**Impact today:** renderer code must retain every PSO it expects to reuse. This
is currently satisfied by renderer-owned resource payloads and keyed creation
slots; the reviewed paths do not create their stable PSOs once per frame.

### P1 — Final RHI reference release is irreversible

Commit `793f5a0f` removed generic zero-reference resurrection from
`FRHIResource`. The final `Release()` atomically enters deferred deletion, and
later `AddRef()` calls are rejected in every build configuration.

**Impact:** a future PSO cache must express retention through ordinary strong
references or a cache-specific lifetime mechanism. It must not depend on a raw
pointer recovering a generic RHI resource after its reference count reaches
zero.

### P2 — Renderer ownership is distributed by rendering feature

Static mesh, sky box, post-process, grid, gizmo, icon, and overlay renderers
retain their PSOs in feature-owned state. This makes ownership and shader/device
generation invalidation explicit, but it does not provide cross-feature
deduplication or one place to observe total PSO creation cost.

## Risks And Assumptions

- **Risk:** graph-generated passes may independently request equivalent PSOs,
  turning currently stable creation into repeated driver work.
- **Risk:** render-target format, sample count, material state, and pass-state
  combinations may grow faster than direct renderer ownership remains
  auditable.
- **Risk:** a capacity- or time-bounded cache can thrash when the live working
  set moves around its eviction boundary. Recreating a recently evicted PSO may
  be substantially more expensive than retaining it briefly.
- **Assumption:** RDG does not itself require a PSO cache. If graph compilation
  resolves each request to stable renderer-owned PSOs without duplicates or
  critical-path creation, the current ownership model can remain.
- **Assumption:** driver pipeline caches may reduce backend compilation cost but
  do not replace engine-level object ownership, creation diagnostics, or
  duplicate-request suppression.

## Evidence Required At The RDG Boundary

Before selecting a cache design, instrument the centralized graphics-pipeline
creation path and record:

- a stable hash or comparable identity for the complete PSO initializer;
- total requests, unique identities, and duplicate creations;
- creation duration, maximum duration, and creations per frame;
- whether creation occurred during initialization, background preparation, or
  a render-critical path;
- shader and device generations responsible for invalidation;
- approximate live PSO count, memory cost where available, and reuse distance
  for identities that were released and recreated.

The investigation should advance to a plan when measurements show one or more
of the following:

- equivalent initializers are created by multiple graph passes or renderer
  features;
- PSO creation appears on a render-critical path;
- creation produces observable frame-time spikes;
- renderer-local PSO collections become duplicated or difficult to invalidate;
- a bounded working set needs explicit eviction and reuse policy.

## Candidate Directions

These are candidates, not selected architecture:

1. Keep renderer-owned strong references when graph compilation can reuse
   stable feature resources.
2. Add a centralized strong-reference map keyed by the complete graphics or
   compute PSO identity when cross-feature deduplication is sufficient.
3. Add budget- or time-bounded LRU eviction with hysteresis only after live-set
   and reuse-distance measurements define a policy.
4. Add asynchronous creation or precaching when compilation latency, rather
   than retained-object count, is the measured problem.
5. If eviction-boundary thrashing is verified, retain recently evicted PSOs in
   a cache-owned strong-reference grace set. Do not restore generic
   zero-reference resurrection in `FRHIResource`.

## Open Questions

- Which layer owns the canonical PSO identity: graph compilation, Renderer,
  RenderCore, or RHI?
- Which initializer fields participate in graphics and compute PSO keys?
- How do shader reload and device recreation invalidate cached entries without
  mixing generations?
- Must lookup and creation remain RHI-thread-only, or can graph preparation
  request asynchronous creation safely?
- Which memory/count budget and eviction hysteresis are justified by measured
  content rather than copied from another engine?

## Related Documentation

- [Runtime lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)
- [Material system](../Runtime/Rendering/MaterialSystem.md)
- [Compute shader pipeline roadmap](../Roadmaps/Archive/2026-08/ComputeShaderPipeline.md)

## Relevant Implementation

- [`FRHIResource` lifetime state](../../Engine/Source/Runtime/RHI/Public/RHIResources.h)
- [RHI deferred deletion](../../Engine/Source/Runtime/RHI/Private/RHIResources.cpp)
- [Vulkan graphics-pipeline creation](../../Engine/Source/Runtime/VulkanRHI/Private/VulkanPipeline.cpp)
- [Static-mesh PSO ownership](../../Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp)
- [Post-process PSO ownership](../../Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.cpp)
- [Editor-grid keyed PSO creation](../../Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistance/EditorGridRenderer.cpp)
