# RDG Resource Allocation and Extraction Plan

Summary: Replace scene-name-based Render Graph backing publication with descriptor-driven allocation, strong resource ownership, and handle-bound external registration and extraction.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

The implementation path is selected and implementation has not started. The
current graph already compiles retained logical resource lifetimes before
execution, but delegates physical publication to a scene-specific backing
resolver. That resolver reconstructs feature topology from string backing
classes, allocates feature target bundles, and maps individual physical
textures back to logical handles by matching diagnostic resource names.

The selected replacement follows the Unreal Engine RDG ownership model at the
semantic boundary: graph-created resources carry descriptions, retained
resources are allocated by a graph execution allocator, existing resources are
registered by physical identity, and graph outputs are extracted by handle to
an explicit strong-reference destination. Diagnostic names never select a
physical resource or pool entry.

New infrastructure types use concise `FRDG*` names, including
`FRDGAllocator`, `FRDGAllocationRequest`, `FRDGAllocatedResources`, and
`FRDGExecutionContext`. Existing public `FRenderGraph*` types remain in place
during this plan unless a compatibility-preserving local adjustment is needed;
a broad naming conversion such as `FRenderGraphBuilder` to `FRDGBuilder` is a
separate follow-up after the ownership migration is complete.

## Goal

Make the logical graph resource description the single source of truth for
physical allocation and lifetime:

- allocate every retained graph-created texture or buffer from its exact
  description without feature-name or resource-name routing;
- retain imported and allocated RHI resources through strong references for
  the complete compiled-graph execution lifetime;
- register external resources by physical identity and extract graph outputs
  by graph handle into explicit destinations;
- preserve compile-before-execute, retained-only allocation, all-or-none
  preparation failure, culling, transition, recovery, and capture behavior;
- remove duplicated scene-target descriptions, feature-owned transient-target
  acquisition, `FResolvedSceneFrameTargets`, and the scene backing provider;
- make an ordinary new transient pass require only a logical resource
  description, typed pass parameters, and a callback; and
- leave assets, asset formats, authored sources, DDC keys, Cook output, and
  serialized content unchanged.

## Scope

- RenderCore allocation requests, execution context, strong backing ownership,
  external registration, extraction, validation, capture, and tests.
- A Renderer-owned first implementation of `FRDGAllocator` backed by a
  descriptor-keyed retained resource pool and existing renderer resource
  recovery generations.
- Scene-frame texture migration for Scene Color/depth, GBuffer, ambient
  occlusion, contact visibility, cloud shadow/spatial/composite, deferred
  diagnostics, and GBuffer diagnostics.
- Imported window/offscreen targets, default textures, environment textures,
  persistent feature textures, and view-state history boundaries.
- Graph-level active, retained, peak, hit, miss, eviction, and allocation
  failure observations with pointer-free capture identities.
- Compatibility adapters needed to migrate current `FRenderGraph*` call sites
  in bounded stages.

## Non-Goals

- Modifying `.dasset` packages, source art, shader source assets, authored
  levels, DDC or Cook formats, asset references, or asset migration behavior.
- Renaming all existing `FRenderGraph*` APIs to `FRDG*` in the same change.
- Physical transient-memory aliasing, placed resources, alias barriers, or
  same-frame physical reuse based on non-overlapping logical lifetimes.
- Asynchronous compute, multiple queues, pass merging, scheduling reordering,
  parallel recording, persistent graph reuse, or PSO-cache redesign.
- A public plugin pass registry, mutable resource blackboard, or string-based
  resource lookup replacement under another name.
- Extracting ordinary frame-local scratch resources merely to keep the current
  resolved-target container alive.

## Selected Design

### Ownership and module boundary

RenderCore owns the allocator contract and graph semantics. Renderer initially
owns the concrete allocator and retained pool because device-generation
recovery, renderer diagnostics, and current memory ceilings already live there.
RenderCore never switches on scene feature, target family, resource name, or
Renderer enum.

`FCompiledRenderGraph::Execute` receives an `FRDGExecutionContext` containing
the allocator. The allocator receives one immutable batch containing only
retained graph-created resources after compilation and culling. It returns a
complete strong-reference table or one failure; no pass records commands after
partial allocation.

The first concrete API shape is:

```cpp
struct FRDGAllocationRequest final
{
	uint32 ResourceId = 0;
	ERenderGraphResourceKind Kind = ERenderGraphResourceKind::Texture;
	FRHITextureDesc TextureDesc;
	FRHIBufferDesc BufferDesc;
	uint32 FirstPass = 0;
	uint32 LastPass = 0;
};

class FRDGAllocator
{
public:
	virtual ~FRDGAllocator() = default;
	virtual auto Allocate(
		std::span<const FRDGAllocationRequest> Requests,
		FRDGAllocatedResources& OutResources,
		std::string& OutError) -> bool = 0;
};

struct FRDGExecutionContext final
{
	FRDGAllocator& Allocator;
};
```

Exact signatures may be refined during Stage 0, but the allocator remains a
batch execution dependency rather than a builder callback that understands
scene semantics.

### Logical creation, external registration, and extraction

Graph-created resources retain description plus diagnostic name. A
description-first overload may be added alongside the existing creation API
without beginning the broad naming migration:

```cpp
const auto SceneColor = Graph.CreateTexture(SceneColorDesc, "Scene.Color");
```

Existing external resources are registered with a counted RHI or pooled
resource reference. Repeated registration deduplicates by physical identity
when description and boundary access agree:

```cpp
const auto Output = Graph.RegisterExternalTexture(
	OutputTexture, "Scene.Output", InitialAccess, FinalAccess);
```

Only a resource whose lifetime intentionally crosses the graph boundary is
queued for extraction:

```cpp
Graph.QueueTextureExtraction(
	NextHistory, &PendingHistory, ERHIAccess::GraphicsShaderRead);
```

Extraction destinations receive no value until allocation and command
recording complete successfully. A failed compile, allocation, preparation, or
execution leaves every destination unchanged. Duplicate or conflicting
extractions fail deterministically before recording. Ordinary Scene Color,
GBuffer, visibility, cloud intermediate, and post-process scratch textures are
not extracted.

An immediate `ConvertToExternal*` compatibility API is not required for the
production migration. Add it only if a concrete legacy caller cannot wait for
end-of-graph extraction, and mark it as a bounded migration mechanism because
it extends lifetime and reduces transient reuse freedom.

### Strong physical ownership

Compiled resources retain `FTextureRHIRef` or `FBufferRHIRef` backing instead
of relying on raw pointers owned indirectly by a renderer pool. Pass-scoped
resolvers may continue returning raw pointers as non-owning callback views.
Imported references, allocated references, final transitions, extraction
publication, and recorded command retention therefore share one explicit
ownership chain.

### Pool identity and transaction rules

The retained pool key is the complete allocation-compatible RHI description:
dimension, usage flags, format, extent, depth, array size, mip count, sample
count, and clear binding/value. Debug name, graph resource ID, feature name,
and pass name are excluded from allocation identity.

Removing the name from the key must not cause overlapping logical resources
with equal descriptions to receive the same physical texture. One allocation
batch reserves a distinct pool entry for every retained logical resource for
the whole graph execution. Entries become reusable only after the transaction
retires. The pool may reuse compatible inactive entries across frames, but the
first implementation does not reuse one physical entry between two logical
resources in the same graph even when `FirstPass` and `LastPass` do not
overlap.

Allocation is transactional. Newly created entries remain candidates until
the complete batch succeeds. On failure, no graph backing or extraction is
published, no pass runs, and newly created candidates are reconciled according
to the existing complete-or-null resource recovery contract.

### Memory policy and observation

Replace feature bundle groups as allocation identity with graph-wide pool
policy. Structural memory ceilings protect supported scale independently from
observational regression budgets. Feature attribution may use a typed
observation tag, but that tag cannot affect physical compatibility, resource
selection, transitions, culling, or execution success.

Captures report logical resource name, description, lifetime, allocation
result, stable physical allocation ID, logical bytes, reuse hit/miss, and
retained/active/peak totals without exposing pointers. Renaming a logical
resource may change diagnostic text but cannot change allocation count,
physical compatibility, output, or failure outcome.

## Implementation Stages

### Stage 0: Freeze behavior and finalize the execution contract

- [ ] Capture representative disabled, compute, fragment, debug, present,
  offscreen, resize, multi-view, allocation-failure, device-recovery, and
  temporal-history frames, including retained requests, descriptions,
  transitions, outputs, and current memory observations.
- [ ] Inventory every current graph-created texture/buffer, external import,
  physical target bundle, pool group, retained-byte ceiling, and post-graph
  consumer; classify each resource as transient, imported, or extracted.
- [ ] Confirm that `FResolvedSceneFrameTargets` has no required post-graph
  consumer and record any exception before implementation.
- [ ] Finalize `FRDGAllocator`, `FRDGAllocationRequest`,
  `FRDGAllocatedResources`, `FRDGExecutionContext`, and extraction transaction
  signatures with RenderCore-to-Renderer dependency direction preserved.
- [ ] Freeze the rule that new infrastructure uses concise `FRDG*` names while
  existing `FRenderGraph*` names remain compatible during this plan.
- [ ] Add focused tests proving that resource names currently influence pool
  identity and backing publication so the migration demonstrates their
  removal rather than silently changing behavior.

#### Acceptance Gate

- Every physical target and cross-graph lifetime has one selected future path;
  baseline behavior and memory evidence are reproducible; no asset or format
  work is required; and no allocator signature depends on Renderer feature
  types or strings.

### Stage 1: Add allocator execution and strong backing ownership

- [ ] Introduce the RenderCore `FRDG*` allocator types and pass an
  `FRDGExecutionContext` to compiled execution.
- [ ] Change imported and allocated compiled resource backing to counted
  texture/buffer references while keeping pass-scoped raw pointer resolution.
- [ ] Adapt the current backing resolver behind `FRDGAllocator` so production
  behavior remains available during migration without two allocation
  authorities for one resource.
- [ ] Validate complete batch publication, missing/incompatible backing,
  imported-reference retention, builder/compiled-graph destruction, early
  failure, and command-recording lifetime.
- [ ] Extend capture and failure diagnostics with allocation disposition and
  stable physical allocation identity.

#### Acceptance Gate

- All retained resources are owned strongly by the compiled execution;
  allocator failure is atomic and occurs before the first pass; current scene
  output, transitions, culling, and recovery remain equivalent through the
  compatibility adapter.

### Stage 2: Replace feature bundles with a descriptor-driven RDG pool

- [ ] Implement a batch allocator over a retained pool whose compatibility key
  excludes diagnostic names and includes every allocation-relevant descriptor
  field.
- [ ] Reserve unique entries for equal-description resources within one graph
  transaction and reuse only inactive compatible entries across transactions.
- [ ] Preserve device/manual generation invalidation, bounded eviction,
  complete-or-null creation, retry, shutdown, and diagnostic publication.
- [ ] Replace feature-group allocation identity with graph-wide structural and
  regression memory budgets; retain feature memory reporting only as
  observation.
- [ ] Add tests for rename invariance, equal-description concurrent resources,
  cold/warm reuse, resize churn, budget eviction, injected partial creation
  failure, device loss, and deterministic allocation capture.

#### Acceptance Gate

- Resource naming has no effect on allocation or reuse; equal descriptions
  never alias unsafely; cold/warm, resize, failure, and recovery behavior pass;
  and current supported memory ceilings are preserved or changed only with
  measured evidence.

### Stage 3: Add UE-like registration and extraction APIs

- [ ] Add description-first logical creation overloads while preserving current
  creation call sites until migrated.
- [ ] Add `RegisterExternalTexture`/`RegisterExternalBuffer` using strong
  physical identity, explicit initial/final access, and deterministic
  deduplication/conflict diagnostics.
- [ ] Add `QueueTextureExtraction`/`QueueBufferExtraction` with explicit final
  access and destinations published only after successful execution.
- [ ] Integrate extraction roots with culling and final transition planning;
  an extracted resource and its complete producer closure must be retained.
- [ ] Cover duplicate/conflicting extraction, destination lifetime,
  compile/allocation/preparation failure, imported-to-extracted round trip,
  resize, and view-state abort/commit.
- [ ] Keep `Import*` and old creation overloads as compatibility entry points;
  record their later naming/migration disposition without renaming the full
  public graph surface here.

#### Acceptance Gate

- Graph-local, imported, and cross-graph resources have distinct enforced
  lifetime paths; extraction is handle-bound and transactional; names remain
  diagnostic-only; and no production scene resource requires a semantic
  backing resolver.

### Stage 4: Migrate the production scene frame

- [ ] Migrate Scene Color/depth and GBuffer first, using graph descriptions as
  the sole creation descriptions and constructing feature target views inside
  pass callbacks from the parameter resolver.
- [ ] Migrate ambient occlusion, contact visibility, cloud shadow,
  deferred/debug targets, cloud spatial/composite, and post-process scratch in
  bounded feature groups with capture and image parity after each group.
- [ ] Register output, default, environment, persistent shadow, weather, and
  other externally owned textures directly by strong physical identity.
- [ ] Route actual temporal/history resources through registration and queued
  extraction with existing Begin/Commit/Abort publication semantics.
- [ ] Remove transient-target acquisition and `Ensure*Targets_RenderThread`
  responsibility from feature renderers after their graph path migrates.
- [ ] Remove `ResolveFrameTargets_RenderThread`, `FResolvedSceneFrameTargets`,
  scene backing-class parsing, name-based physical publication, and
  `FSceneFrameGraphBackingProvider` after the final consumer moves.

#### Acceptance Gate

- Production has one descriptor authority and one allocation path; scene
  callbacks resolve only declared graph resources; no scene resource name or
  feature bundle selects physical backing; all baseline outputs, routes,
  transactions, failures, transitions, captures, and memory gates pass.

### Stage 5: Remove compatibility allocation and qualify the contract

- [ ] Remove `FRenderGraphBackingResolver`, preparation fields used only by
  semantic backing publication, obsolete Renderer target-pool dependencies,
  and duplicate resource descriptions once repository consumers are migrated.
- [ ] Keep low-level compatibility APIs only where an independent RenderCore or
  Vulkan oracle still requires them; document each bounded consumer.
- [ ] Update the lasting Render Graph, renderer preparation, resource recovery,
  and feature-rendering contracts with allocator, registration, extraction,
  ownership, failure, memory, and extension rules.
- [ ] Pass focused RenderCore graph/allocation, RHI transition, Renderer scene
  contract, target layout/memory, recovery, view-state, and Vulkan integration
  coverage according to repository testing guidance.
- [ ] Pass representative Directional Shadow, GBuffer, HDR output, volumetric
  cloud, present/offscreen, resize, multi-view, and Editor smoke qualification.
- [ ] Pass the required build, routine native-test aggregates, and changed/all
  documentation lifecycle validation.
- [ ] Record whether the evidence gate for a separate physical transient
  aliasing plan is met; do not implement aliasing as cleanup in this stage.

#### Acceptance Gate

- No production allocation or publication depends on a resource name;
  graph-created descriptions are the only transient creation authority;
  imported and extracted resources have explicit strong ownership; obsolete
  scene backing infrastructure is gone; validation passes; and lasting
  contracts own the implemented behavior.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Asset isolation | No asset, source-art, DDC, Cook, package-format, or serialized-content diff; representative runtime content loads unchanged |
| Allocation identity | Renaming logical resources leaves allocation count, reuse, output, and failure behavior unchanged |
| Ownership | Imported, allocated, extracted, compiled, culled, failed, and destroyed paths retain and release counted RHI references exactly once |
| Transactionality | Partial allocation and every pre-recording failure run no pass and publish no backing or extraction destination |
| Resource uniqueness | Equal-description resources with overlapping or conservatively retained lifetimes receive distinct physical allocations |
| Culling and extraction | Unused transient branches cull; queued extraction is an explicit root and retains its complete producer closure |
| Access and transitions | Imported initial/final access, extracted final access, attachments, compute/graphics handoffs, and Vulkan state tracking remain exact |
| Scene parity | Pass/resource identity, routes, images/readbacks, typed results, telemetry, present/offscreen output, and temporal commit/abort match the baseline |
| Recovery | Allocation failure, retry, resize churn, multi-view, device/manual invalidation, shutdown, and restart expose no partial or dangling state |
| Memory | Active, retained, peak, hit/miss, eviction, and failure statistics are deterministic; supported ceilings remain bounded |
| Architecture | Renderer features no longer acquire frame-transient targets; RenderCore allocator contracts contain no scene feature types or semantic name switches |

## Deferred Follow-Ups

- Rename the mature public graph surface from `FRenderGraph*` to concise
  `FRDG*` names in one compatibility-planned change after this ownership model
  stabilizes.
- Move the concrete allocator/pool from Renderer to RenderCore if a second
  production graph consumer demonstrates the same need and device-generation
  ownership can be expressed without reversing module dependencies.
- Create a physical transient aliasing plan only when lifetime telemetry shows
  material memory benefit and RHI/Vulkan expose the required placement,
  retirement, and alias-transition contracts.
- Evaluate an immediate external-conversion API only for a demonstrated legacy
  caller that cannot use queued extraction.

## Related Documentation

- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Render Graph Architecture Roadmap](../Roadmaps/Archive/2026-08/RenderGraphArchitecture.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphBackingProvider.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphBackingProvider.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphComposer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFramePreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneFrameGraphTypes.h`
