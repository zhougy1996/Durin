# Geometry Submission Refactor Plan

Summary: Replace primitive-family rendering dispatch with a common geometry-batch contract, extensible vertex-factory bindings, and shared mesh-pass processing while preserving the existing render graph.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Planning only; no implementation stages have started. Source inspection confirms
StaticMesh/SplineMesh dispatch in scene membership, visibility output, shadow
candidate classification, preparation, and shader selection. Receiver and shadow
preparation already share `PrepareStaticMeshView_RenderThread`; retain that
reuse while replacing its family-specific input and output contracts.

Next: Stage 0. Test target names below were discovered from the configured native
test registry; no native baseline or performance qualification has been run for
this plan. Close stages only with recorded implementation and validation evidence.

## Goal

Add a mesh primitive using an existing vertex factory by implementing its
detached geometry provider, without changing scene membership, visibility,
shadow candidate classification, or existing mesh-pass executors. Add a vertex
factory through its owned shader/binding implementation and registration, without
adding primitive-family or deformation-family branches to those core paths.

StaticMesh and SplineMesh must retain current receiver, GBuffer, retained-forward,
translucency, and directional-shadow behavior. A third, independent geometry
fixture must prove this extension boundary before the old path is removed.

## Scope and Non-Goals

- Include Engine proxy submission, RenderCore geometry/vertex-factory contracts,
  Renderer mesh preparation, resource resolution, mesh-pass processing, scene
  membership, and all existing mesh consumers including preview entry points.
- Preserve graph scheduling, immutable plans, resolved-index alignment, output
  transactions, resource recovery, material policy, and public telemetry meaning.
- Do not implement production terrain, skeletal animation, a procedural-mesh
  authoring component, GPU-driven culling, indirect drawing, mesh shaders, or
  new rendering passes. Minimal qualification providers are part of this plan.
- Do not unify lights, sky, or volumetric clouds into mesh batches. Do not
  redesign the component-facing scene publication API or all dynamic-update
  messages merely to remove render-consumer type dispatch.
- Indexed/non-indexed direct draw and instance ranges belong in the contract;
  unsupported backend combinations must report explicit capability outcomes.
  A new RHI feature or asset/cook format requires a separately recorded scope
  decision, not an implicit expansion of this refactor.

## Selected Design

### Ownership and Dependency Direction

Engine providers own asset-specific LOD/section interpretation and detached
dynamic state. A const render-thread collection operation, provisionally
`CollectMeshBatches(Context, Collector)`, submits logical data through the base
primitive interface. Collection performs no draw recording or fallible GPU
resource creation and receives no complete scene-frame context.

RenderCore owns reusable geometry resource views, draw ranges, vertex-factory
type/binding descriptions, and capability vocabulary. Engine owns the
proxy-facing collection seam and any envelope requiring Engine material types.
Renderer-private code owns view adaptation, pass policy, sort/preparation,
shader/PSO resolution, and draw execution. Neither Engine nor RenderCore may
depend on Renderer-private types; RenderCore must not acquire an Engine
dependency to host the batch envelope. Stage 0 fixes exact header placement.

The collection context supplies narrow view/LOD inputs and render purpose. Shared
projection math may move to an appropriate lower layer; StaticMesh LOD selection
and residency fallback remain provider-owned. The renderer must not recover an
asset type from the resulting batch to finish geometry preparation.

### Logical Batch and Vertex Factory

Each logical batch describes primitive identity, stable batch/element identity,
transform and bounds, material identity or retained snapshot, vertex-factory
type and binding reference, geometry resources, topology, validated draw and
instance ranges, and pass participation intent. Provider-local LOD identifiers
may support diagnostics but cannot be required for generic draw execution.

Prepared primitives/draws contain no mandatory `FStaticMeshLODResources*`,
`FStaticMeshSection*`, `FLocalVertexFactory*`, or inline Spline payload. StaticMesh
and SplineMesh may continue borrowing the same asset resources inside their
providers. Use typed binding layouts and owned payload references; do not replace
the family switch with an unvalidated `void*` payload or variant of all families.

Extend the existing `FVertexFactoryType`/`FVertexFactory` foundation with explicit
shader compatibility, compilation identity, parameter layout, and binding
resolution. Mesh-pass processors select material × vertex-factory × pass
combinations. Vertex-factory implementations own vertex input/deformation
binding; pass processors own pass state and material policy. Neither may invoke
arbitrary whole-pass callbacks from a primitive proxy.

Shader/PSO and binding cache identities must distinguish factory type, compatible
layout/permutation, and relevant resource generations. Dynamic content revision
invalidates resource bindings without needlessly multiplying pipeline keys.
Local and Spline factory implementations must work in every currently supported
mesh pass, including masked depth/shadow and GBuffer.

### Visibility and Pass Participation

Use one authoritative primitive membership collection and generic candidate
records. Coarse primitive relevance is conservative; per-batch participation is
resolved from primitive/batch intent, material policy, factory capabilities, and
view policy. Statistics or diagnostic family labels never decide eligibility.

Main-view and shadow/cascade culling remain separate: offscreen primitives can
cast visible shadows. Each view invokes the same collection contract with its
own LOD policy; never build caster geometry solely from receiver-visible batches.
View-independent geometry and dynamic snapshots may be reused across views,
but view-dependent selection cannot be cached without its full selection key.

Keep current material rules, including translucent shadow exclusion. Distinguish
intentional exclusion, unsupported capability, invalid submission, and resource
failure with typed outcomes. A claimed-supported combination that lacks its
shader or binding is a failure, not an empty successful pass. Preserve each
feature's documented fallback and view-failure policy.

This deliberately replaces the primitive typed-view requirement in
[Scene Representation](../Runtime/Rendering/SceneRepresentation.md). Light and
other non-mesh family rules remain authoritative. Update the implemented contract
alongside the stage that switches production primitive consumers.

### Thread, Lifetime, and Ordering

Collection, resolution, and consumption remain on the render thread under the
existing scene command ordering. No game-thread component or reflected asset is
read by a provider during collection. Each published frame references immutable
dynamic snapshots; a later material/geometry update cannot alter an earlier plan.

Retain existing asset-retirement fences and command-bounded borrows where valid.
Any reference that outlives that borrow must explicitly retain its owner; resolved
RHI resources remain alive through command execution. Define payload alignment,
destruction, resource-generation invalidation, and retirement in Stage 0.

Preserve logical preparation → resource resolution → graph execution. Assign
contiguous resolved indices after sorting and retain current deterministic tie
breakers, translucent distance order, and per-draw readiness. No pointer-keyed
submission lookup, telemetry-driven readiness, or second production scheduler.

## Implementation Stages

### Stage 0: Freeze contracts and establish baselines

Dependencies: none. Outcome: reviewable interfaces and an executable baseline.

- [ ] Inventory receiver, shadow, GBuffer, retained-forward, translucent,
  preview/thumbnail, geometry-cache, shader compilation/cook, and recovery
  consumers of StaticMesh-specific prepared data; record exact migration owners.
- [ ] Finalize collection context/envelope header ownership, typed factory
  binding operations, direct draw/instance semantics, participation vocabulary,
  payload lifetime, cache identities, and failure mapping. Resolve backend
  capability and shader-cook integration questions before Stage 1.
- [ ] Define independent geometry and custom-factory qualification fixtures;
  place them in existing registered test owners where possible. Define numeric
  image tolerances, representative scenes, CPU/memory/performance budgets, and
  sampling policy before changing production behavior.
- [ ] Run the smallest registry-selected baseline set covering scene contracts,
  StaticMesh/Spline preparation, directional shadows, GBuffer, and resource
  reload. Record preset/backend, commands, results, captures, and known failures.

Completion: interface/failure/lifetime decisions and reproducible baseline
evidence are recorded here; unresolved items blocking later stages are closed.

### Stage 1: Introduce geometry submission and adapt existing providers

Dependencies: Stage 0. Outcome: both existing proxies emit the common contract.

- [ ] Add the logical batch, checked range/resource views, collection context,
  collector, and render-thread primitive submission seam at the selected layers.
- [ ] Move StaticMesh LOD/residency selection, section extraction, and material
  association into its provider; adapt Spline with the same geometry mechanism
  and its own immutable deformation binding.
- [ ] Exercise the collector from production preparation through one temporary
  compatibility adapter; record that adapter's removal in Stage 2. Do not keep
  two independently maintained selection/material algorithms.
- [ ] Add contract coverage for empty and multiple batches, indexed/non-indexed
  and instance ranges, overflow/out-of-range rejection, material slots, invalid
  bounds, unavailable LODs, immutable snapshots, and borrow/retirement rules.

Completion: existing families enter preparation through collection, preserve
baseline behavior, and no provider imports Renderer-private declarations.

### Stage 2: Generalize prepared draws and mesh-pass execution

Dependencies: Stage 1. Outcome: all mesh passes consume generic prepared data.

- [ ] Replace family-specific prepared primitives/sections and geometry-cache
  inputs with the public logical contract and Renderer-private generic records;
  migrate all identified preview and production callers and remove the adapter.
- [ ] Implement Local/Spline factory descriptors, shader permutations, typed
  binding resolution, and factory-aware cache identities using the existing
  registration/resource lifecycle. Cover cooked shader lookup as applicable.
- [ ] Route receiver, shadow depth, GBuffer, retained-forward, and translucent
  draws through common mesh-pass processing with pass-owned policy. Remove
  Local/Spline shader-selection branches from generic executors.
- [ ] Preserve immutable plans, post-sort resolved indices, resource retry and
  fallback semantics, deterministic ordering, and telemetry conservation.
- [ ] Validate masked clipping and Spline deformation parity across color,
  GBuffer, and shadow; exercise material replacement and resource invalidation.

Completion: no generic prepared/execution record requires StaticMesh or Spline
data; supported passes resolve through factory capabilities and match baselines.

### Stage 3: Unify primitive membership and view candidates

Dependencies: Stage 2. Outcome: primitive type no longer gates pass admission.

- [ ] Replace StaticMesh/Spline membership and visibility output lists with
  generic primitive candidates; maintain atomic add/remove/release ownership.
- [ ] Remove caster-family classification and per-family cascade candidate
  lists; retain independent caster volumes, cascade masks, and per-view LOD.
- [ ] Apply conservative relevance and batch-level participation through the
  common policy. Preserve diagnostic family statistics without dispatch authority.
- [ ] Remove obsolete typed render-consumer getters/enums. Retain any narrowly
  justified mutation/diagnostic identity only with its remaining use documented.
- [ ] Update Scene Representation and Renderer Frame Preparation contracts to
  describe the implemented generic primitive route and unchanged non-mesh rules.
- [ ] Test offscreen casters, mixed participation within one primitive, hidden
  primitives, invalid bounds fallback, cascade membership, update ordering, and
  detach/release with no stale candidates.

Completion: a new provider requires no membership, visibility, or caster switch
entry; scene lifecycle and candidate/outcome conservation checks pass.

### Stage 4: Prove extension boundaries and retire migration scaffolding

Dependencies: Stage 3. Outcome: independent extension evidence and final handoff.

- [ ] Add a qualification-only procedural provider owning its own geometry,
  with no StaticMesh render-data/section dependency, using an existing factory.
  Verify main view, GBuffer, retained-forward, translucency policy, and shadows
  through the production pipeline without core dispatch changes.
- [ ] Add a minimal custom-factory fixture with a distinct deformation/binding
  layout. Verify factory registration, color/depth/GBuffer deformation parity,
  masked shadows, cache separation, and explicit unsupported-pass outcomes
  without modifying generic executors. This is not a skeletal-animation feature.
- [ ] Exercise non-indexed and instance-range submission on supported backends,
  geometry/material updates, removal, resource recreation, and multiple views.
- [ ] Remove temporary adapters, obsolete family-based draw structures, dead
  code, and superseded tests; audit core paths for concrete proxy/factory casts
  and type switches. Registration and owned factory implementations are allowed.
- [ ] Run final affected tests and the bounded qualification lanes below;
  compare Stage 0 captures, timings, allocation/retained-memory data, and counters
  against the predeclared gates. Record deviations rather than rebaseline them.
- [ ] Publish lasting batch/factory contracts in their owning Runtime documents,
  update affected rendering contracts and direct links, validate documentation,
  and complete this plan only when every required gate has evidence.

Completion: both extension fixtures pass, baseline behavior/performance gates
pass, old production routes are gone, and final evidence is recorded.

## Validation and Handoff

Follow [Build and Run](../Agents/BuildAndRun.md) before repository build/run work
and [Testing](../Agents/Testing.md) before test selection/execution. Use the
registry and affected selection rather than inferring targets from directories.
Current candidate owners, to narrow by changed behavior during Stage 0:

| Coverage | Registered targets / evidence |
| --- | --- |
| Batch/factory and scene contracts | `RenderContractTests`, `RendererSceneContractTests` |
| Existing geometry and updates | `StaticMeshTests`, `SplineTests`, `StaticMeshRenderPreparationVulkanTests` |
| Pass parity and offscreen casters | `DirectionalShadowBaselineVulkanTests`, `GBufferQualificationTests` |
| Material/shader and resource lifetime | `MaterialTests`, `RendererResourceReloadVulkanTests`; `RenderShaderCookIntegrationTests` when shader registration/cook behavior changes |
| Preview integration | `EditorRenderingTests`, `StaticMeshThumbnailTests` for affected entry points |
| Extensibility | Independent provider and custom-factory cases registered with the matching contract/qualification owners |

Use one bounded test invocation for the selected set. GPU performance evidence
requires an exclusive quiet lane and the Stage 0 sampling policy; correctness
results under contention do not satisfy timing gates. Backend skips or missing
execution environments remain explicitly open gates, not completion evidence.
Application-hosted tests are not required by this plan.

At each stage handoff, record changed interfaces, exact tests and results,
remaining adapter/deletion obligations, and any supported-behavior exceptions.
Keep each stage buildable and commit its status/checklist updates with exact
`Plan` and `Stage` trailers according to repository rules. Do not close a later
stage merely because the existing two families render successfully.

## Related Code

- `Engine/Source/Runtime/Engine/Public/Rendering/PrimitiveSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/StaticMeshSceneProxy.h`
- `Engine/Source/Runtime/Engine/Public/Rendering/SplineMeshSceneProxy.h`
- `Engine/Source/Runtime/RenderCore/Public/VertexFactory.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/SceneInfo.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowView.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.h`

## Related Contracts

- [Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Scene Representation](../Runtime/Rendering/SceneRepresentation.md)
- [Static Mesh Rendering](../Runtime/Rendering/StaticMeshRendering.md)
- [Directional Shadows](../Runtime/Rendering/DirectionalShadows.md)
- [Render Resource Lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Render Graph](../Runtime/Rendering/RenderGraph.md)
