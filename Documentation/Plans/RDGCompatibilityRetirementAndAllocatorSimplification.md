# RDG Compatibility Retirement and Allocator Simplification Plan

Summary: Retire the remaining legacy Render Graph backing paths and feature-bundle target allocation, then simplify descriptor-driven RDG allocation and diagnostics without changing rendering behavior.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

Stage 4 functional qualification is complete, but two repository gates remain
open. The complete runtime build and hidden 10-tick Sandbox Editor smoke pass.
Focused RenderCore/Renderer/Vulkan coverage, volumetric-cloud qualification,
and HDR display qualification pass. Three GBuffer qualification attempts
completed all functional and memory work but failed frozen GPU timing gates;
two runs explicitly reported unstable samples, so repository guidance requires
an exclusive quiet GPU rerun rather than a threshold or code change.

Both `test fast-all` and `test all` stop while linking the unrelated
`AssetPackageTests` target because `BulkDataTests.cpp` references the existing
private `EncodePackageBulkDataDirectory` and `DecodePackageBulkDataDirectory`
implementations without link-visible definitions. Renderer-focused targets do
not depend on that target. The plan remains Active until the pre-existing asset
test link failure is resolved and the GBuffer timing gate passes in a quiet
lane.

A Stage 4 follow-on retired the remaining raw-pointer `ImportTexture` /
`ImportBuffer` and prebound `CreateTexture` / `CreateBuffer` overloads after a
repository inventory confirmed that only `RenderContractTests` used them.
Tests now retain external resources through counted references and publish
graph-created backing through `FTestRDGAllocator`. RenderCore stores one counted
RHI reference per physical resource, distinguishes only external from
graph-created resources, and no longer carries prebound capture state or raw-
pointer extraction fallbacks. The Win64 Debug `all` build passes, and
`RenderContractTests` passes all 104 tests.

Final implementation accounting against the frozen pre-plan source is 357
production lines added and 949 removed (net -592). Compatibility-symbol count
is zero. The final representative scene batch requested 13 active resources
(615,488 bytes); the pool retained 30 resources (929,600 bytes), with 42 reuse
hits, 30 misses, zero evictions, and zero allocation failures. Its 1.053 s
scene-test time compares with the 1.124 s baseline; GPU qualification timing is
diagnostic only until an exclusive pass is available.

Stage 3 simplified the RDG pool transaction. Texture and buffer reservations
now share candidate selection, generation suppression, rollback, publication,
and error reporting; typed creation remains at the RHI boundary. Compatibility
keys contain only exact descriptors, active IDs use a set, retained bytes are
incremental, and inactive eviction preserves stable entry order. The unchanged
640 MiB structural ceiling is a named policy with boundary coverage. Graph
creation uses only the description-first descriptor overloads. Allocated entries
retain stable nonzero IDs across graphs and diagnostic renames, while imported,
prebound, culled, and failed resources report ID zero with truthful dispositions.

Stage 3 passed `RenderContractTests` (104 tests, 386 ms),
`RendererSceneContractTests` (39 tests, 79 ms),
`VulkanRHIIntegrationTests` (65 tests, 8.656 s),
`EditorGridVulkanTests` (8 tests, 2.732 s), and
`VolumetricCloudSceneVulkanTests` (1 test, 1.070 s). The focused Vulkan reuse
oracle proves that equal descriptors with different diagnostic names reuse the
same nonzero pool allocation ID. These representative timings remain within
ordinary shared-machine variance of the frozen diagnostic baseline.

Stage 1 retired the RenderCore backing resolver. RenderCore now compiles only
name-free `FRDGAllocationRequest` records and execution accepts retained
physical resources only through counted-reference `FRDGAllocator` publication.
RenderCore and Vulkan transition fixtures use that same contract; missing and
incompatible allocator results remain atomic and diagnostic. Backing-class
metadata, raw-pointer publication, compatibility errors, dump fields, and the
diagnostics-panel column are removed.

Stage 2 retired feature-owned transient allocation. Feature renderers expose
exact descriptor helpers and record only into caller-provided targets. Focused
EditorGrid and volumetric-cloud tests create counted targets through one shared
test fixture. The Renderer pool now contains only RDG descriptor entries;
feature bundles, leases, group storage, aggregate legacy queries, and
allocation-only constructor dependencies are removed. The former group enum is
now `ERDGAllocationObservation`, and scene telemetry queries RDG-retained bytes
directly by observation without affecting compatibility or eviction.

Stage 0 froze the compatibility inventory and baseline on Win64 Debug.
Feature-owned allocation is used only by focused EditorGrid and
volumetric-cloud Vulkan/qualification tests; production scene contributors
already receive graph-resolved targets. Every feature constructor still
receives `FRendererTransientTargetPool` only for those test allocation methods
and retained-byte queries.

The frozen capture identity contract is: allocator pool entries publish a
stable nonzero pool allocation ID; imported, prebound, culled, and failed
resources publish ID zero and use an explicit disposition. Observation tags
remain attribution only. Names, tags, graph-local indices, and feature routes
cannot affect compatibility or fabricate physical identity.

The functional baseline passed `RenderContractTests` (103 tests, 359 ms),
`RendererSceneContractTests` (38 tests, 80 ms), `VulkanRHIIntegrationTests`
(65 tests, 8.762 s), `EditorGridVulkanTests` (7 tests, 2.386 s), and
`VolumetricCloudSceneVulkanTests` (1 test, 1.124 s). These fixtures cover cold
allocation, warm reuse, equal-description uniqueness, imported registration,
extraction, injected allocation failure, resize, recovery invalidation,
GBuffer, ambient occlusion, contact shadow, and volumetric cloud. Scene-cloud
captures retain six 11-pass graphs with nonzero active resources, retained
resources no lower than active resources, retained bytes no lower than active
bytes, zero failures, and both reuse hits and misses on the final frame. The
allocator baseline uses one linear descriptor scan per request, rescans earlier
candidates for active identity, sums the complete retained pool for statistics
and each eviction condition, and linearly scans both pools for the oldest
evictable entry. Timings were collected in a shared desktop/GPU session and are
diagnostic comparison data, not an acceptance threshold.

## Goal

Leave one allocation and ownership model for graph-created physical resources:

- retained logical resources are allocated only through `FRDGAllocator`;
- production and tests do not use semantic backing classes or raw-pointer
  backing publication;
- feature renderers record into caller-provided targets and do not own a
  second frame-transient allocation path;
- allocation captures distinguish stable pool allocation identity from
  external resource identity without presenting graph-local IDs as
  physical identities;
- texture and buffer allocation share one transaction policy with bounded,
  measurable lookup and eviction work; and
- rendering output, failure atomicity, recovery generations, memory ceilings,
  transition behavior, and feature qualification remain unchanged.

## Scope

- RenderCore preparation, execution, capture, and compatibility APIs associated
  with `FRenderGraphBackingResolver`.
- Renderer legacy feature-bundle allocation and the focused tests that depend
  on feature-owned target acquisition.
- Descriptor-keyed RDG pool candidate selection, rollback, statistics,
  allocation identity, retained-byte accounting, and eviction.
- Lasting Render Graph, renderer preparation, and resource-recovery contracts
  affected by removing compatibility behavior.

## Non-Goals

- Physical transient-memory aliasing or same-frame backing reuse based on
  non-overlapping logical lifetimes.
- Async compute, multiple queues, pass merging, scheduling reordering, or
  parallel command recording.
- Moving the concrete allocator from Renderer to RenderCore.
- Renaming the complete `FRenderGraph*` public surface to `FRDG*`.
- Changing feature algorithms, shaders, render-target formats, image output,
  serialized assets, Cook output, or memory ceilings without separate measured
  evidence.
- Replacing focused renderer tests with scene-only end-to-end coverage; tests
  retain direct feature recording coverage through explicit test-owned targets.

## Selected Decisions

- **Remove compatibility before optimizing internals.** Tests migrate to a
  counted-reference `FRDGAllocator` or explicit test target fixture before the
  legacy resolver and feature bundle pool are deleted. Production does not
  retain dormant fallback allocation paths.
- **Test target ownership belongs to tests.** A focused feature test may create
  exact RHI targets through a shared test fixture, but production feature
  renderers no longer expose `Ensure*Targets_RenderThread` solely to support
  those tests.
- **Observation tags remain diagnostic-only.** A typed Renderer observation
  tag may attribute retained bytes, but it cannot participate in descriptor
  compatibility, allocation selection, failure, or eviction priority.
- **Physical identity is honest and scoped.** RDG pool entries retain stable,
  pointer-free allocation IDs. External resources use an explicit external
  disposition with no fabricated pool allocation ID unless a real cross-capture
  identity registry is introduced and tested.
- **Transactionality precedes publication.** Candidate validation, rollback,
  failure-generation suppression, and complete output publication remain
  all-or-none. Refactoring may not mutate extraction destinations or record a
  pass after partial allocation failure.
- **Complexity changes require evidence.** Shared helpers, active-ID sets, and
  incremental byte accounting are preferred when they make the code smaller
  and preserve deterministic order. A hash-indexed pool is introduced only if
  compile/execute measurements show the current linear descriptor lookup is
  material.

## Implementation Stages

### Stage 0: Freeze compatibility inventory and diagnostic semantics

- [x] Confirm with repository-wide call-site searches that production Runtime
  code does not call `SetBackingResolver` and record every remaining test
  consumer that must migrate.
- [x] Inventory every `Ensure*Targets_RenderThread`,
  `AcquireBundle_RenderThread`, retained-byte query, and feature renderer
  constructor dependency; distinguish production recording needs from
  test-owned allocation needs.
- [x] Freeze representative cold allocation, warm reuse, equal-description
  uniqueness, imported registration, extraction, allocation failure, resize,
  device/manual recovery, and retained-byte capture evidence.
- [x] Define capture semantics for pool-allocated, imported, prebound, culled,
  and failed resources, including whether non-pool resources report allocation
  ID zero or a separately named identity.
- [x] Record baseline allocator resource counts and lookup/eviction timing for
  representative scene, GBuffer debug, ambient occlusion, contact shadow, and
  volumetric-cloud graphs.

#### Acceptance Gate

- Every compatibility consumer has one selected replacement; capture identity
  cannot be confused with a graph-local resource index; and allocator cleanup
  has reproducible functional, memory, failure, and timing baselines.

### Stage 1: Retire the RenderCore backing resolver path

- [x] Replace RenderCore and Vulkan resolver-based fixtures with a minimal
  counted-reference test `FRDGAllocator` that validates the same descriptor and
  transition behavior.
- [x] Remove `FRenderGraphPreparationRequest`,
  `FRenderGraphResourceBackings`, `FRenderGraphBackingResolver`,
  `SetBackingResolver`, and the resolver execution branch.
- [x] Stop storing `BackingClass` in graph resource descriptions and captures
  once no independent diagnostic consumer remains.
- [x] Build only `FRDGAllocationRequest` records for retained graph-created
  physical resources; preserve culling, lifetime, observation tag, descriptor,
  and deterministic request ordering.
- [x] Remove compatibility-only friends, raw-pointer publication state, error
  messages, dumps, and tests without weakening missing-allocator or
  incompatible-backing diagnostics.

#### Acceptance Gate

- RenderCore exposes one retained-resource allocation contract; all graph,
  transition, ownership, extraction, and Vulkan oracle tests use
  `FRDGAllocator`; and no production or test code references the legacy backing
  resolver types.

### Stage 2: Retire feature-owned transient target allocation

- [x] Add or extend a shared Renderer/Vulkan test fixture that creates exact
  counted texture targets from feature-owned descriptor helpers and releases
  them through normal RHI reference lifetime.
- [x] Migrate GBuffer, ambient occlusion, contact shadow, deferred diagnostics,
  GBuffer debug, volumetric cloud, and cloud-shadow tests away from
  `Ensure*Targets_RenderThread`.
- [x] Remove feature renderer `Ensure*Targets_RenderThread` methods and
  allocation-only `FRendererTransientTargetPool` constructor dependencies;
  retain target description/byte calculation helpers where they remain lasting
  feature contracts.
- [x] Route production feature retained-byte telemetry through RDG allocation
  observations rather than legacy bundle-pool queries, preserving per-feature
  attribution and graph-wide totals.
- [x] Remove `FLease`, `AcquireBundle_RenderThread`, the `Groups`/`FEntry`
  storage, legacy group eviction, and unused aggregate queries from
  `FRendererTransientTargetPool`.
- [x] Reduce `ERendererTransientTargetGroup` to a clearly named typed
  observation vocabulary, or replace it with a narrower type that cannot be
  mistaken for allocation identity.

#### Acceptance Gate

- Feature renderers only record into caller-provided targets; focused tests own
  their physical fixtures; production and tests use no feature-bundle pool;
  and feature plus graph-wide memory telemetry remains truthful across cold,
  warm, resize, disabled-route, and recovery frames.

### Stage 3: Simplify RDG pool transactions and allocation diagnostics

- [x] Replace imported/prebound `PhysicalAllocationId = ResourceIndex + 1`
  capture publication with the Stage 0 identity contract and cover identity
  behavior across multiple graphs and frames.
- [x] Separate the name-bearing legacy texture key from the allocation-compatible
  descriptor key so diagnostic names cannot enter RDG compatibility by
  construction.
- [x] Extract shared candidate reservation, failure-generation, rollback,
  publication, and error-reporting operations used by texture and buffer
  allocation while keeping typed RHI creation at the boundary.
- [x] Track active allocation IDs without rescanning all earlier candidates and
  maintain retained bytes incrementally so eviction does not repeatedly sum the
  complete pool.
- [x] Centralize the RDG retained-byte structural ceiling as a named Renderer
  policy with boundary tests and lasting documentation; do not change its value
  in this plan without measured evidence.
- [x] Remove redundant descriptor-first/name-first creation overloads after
  production and tests converge on the selected description-first API shape.
- [x] Preserve stable pool entry order and deterministic allocation IDs unless
  Stage 0 measurements justify an indexed lookup; if indexing is justified,
  prove output, reuse, eviction, and capture order remain deterministic.

#### Acceptance Gate

- Texture and buffer allocation implement one observable transaction policy;
  names cannot affect compatibility; physical allocation identities are
  truthful; allocation failure remains atomic; retained-byte accounting and
  eviction remain bounded; and representative allocator timing does not
  regress beyond the frozen baseline tolerance.

### Stage 4: Qualify the single allocation contract

- [x] Pass focused RenderCore graph, ownership, registration, extraction,
  culling, allocation, failure, and RHI transition coverage according to the
  repository testing workflow.
- [x] Pass focused Renderer target-layout, memory, scene contract, recovery,
  view-state, and Vulkan feature coverage affected by fixture migration.
- [ ] Pass representative GBuffer, ambient occlusion, contact shadow,
  volumetric cloud, present/offscreen, resize, multi-view, allocation-failure,
  recovery, and Editor smoke qualification.
- [ ] Pass the required build and routine native-test aggregates according to
  repository build and testing guidance.
- [x] Update lasting Render Graph, renderer frame-preparation, and
  renderer-resource-recovery documentation, then pass changed/all document and
  plan lifecycle validation.
- [x] Record final source size, compatibility symbol count, allocation request
  count, retained memory, reuse/eviction observations, and representative
  timing against the Stage 0 baseline.
- [x] Retire raw-pointer import and prebound creation APIs after migrating their
  remaining tests to counted external registration and test allocator backing.

#### Acceptance Gate

- The repository contains one graph-created resource allocation path, no
  production feature-owned transient target allocator, no legacy backing
  resolver, truthful allocation diagnostics, passing functional and recovery
  qualification, and lasting contracts that describe the final behavior.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Compatibility removal | Repository searches find no resolver, preparation-backing, feature-bundle acquisition, or feature `Ensure*Targets` symbols after their owning stage. |
| Allocation transaction | Missing, duplicate, incompatible, over-budget, and injected partial creation failures record no pass and publish no extraction destination. |
| Ownership | Allocated, external, extracted, culled, failed, and destroyed graphs retain and release counted RHI references through their documented boundary. |
| Identity | Pool allocations remain stable and unique where required; external captures never fabricate pool identity; names do not affect identity or reuse. |
| Memory | Active, retained, peak, hit, miss, failure, and eviction observations remain deterministic and within the frozen structural ceiling. |
| Rendering parity | Representative routes preserve pass topology, transitions, output/readbacks, telemetry, present/offscreen behavior, and temporal commit/abort. |
| Recovery | Resize churn, allocation failure, retry suppression, device/manual generation changes, shutdown, and restart leave no partial or dangling state. |
| Maintainability | One request representation and one pool transaction path remain; texture/buffer differences are confined to typed descriptor, creation, and publication operations. |

## Related Documentation

- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [RDG Resource Allocation and Extraction Plan](RDGResourceAllocationAndExtraction.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/RDG.h`
- `Engine/Source/Runtime/RenderCore/Private/RDG.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphExecutor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphComposer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/AmbientOcclusionRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowVisibilityRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRendering.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RDGTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
