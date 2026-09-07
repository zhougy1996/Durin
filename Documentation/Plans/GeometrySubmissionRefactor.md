# Geometry Submission Refactor Plan

Summary: Replace primitive-family rendering dispatch with a common geometry-batch contract, extensible vertex-factory bindings, and shared mesh-pass processing while preserving the existing render graph.

Last reviewed: 2026-09-08

Status: Active
Completed:

## Current Status

Stages 1–3 implementation and correctness checks are complete. Stage 2 supports multiple batches,
checked vertex/instance inputs, factory pass capabilities and named cooked
vertex requests. Stage 3 uses generic primitive membership, visibility and
cascade candidates; the obsolete StaticMesh proxy getter is removed. The
remaining Spline getter serves dynamic mutation and test diagnostics.

Stage 4 now compares final color, all four GBuffer attachments, scene depth and
actual masked-shadow depth for the independent geometry/custom factory at
1920×1080 (shadow target retains its production resolution). Covered pixels and
silhouettes agree. A forward-only factory exercises optional GBuffer exclusion,
translucency, shadow collection failure and recovery. Retired bindings expire
across 100 updates apart from the deliberately retained initial frame.

Cooked Local/Spline vertex stages compose with forward, GBuffer and masked-shadow
material stages after ShaderBuild unload. A separate fixed opaque-shadow
fragment request closes that runtime lookup gap. Shader-type compilation
metadata is the common authored/cooked source; the duplicate factory compile
callback and authored-only opaque vertex shortcut are removed. The affected selection passes 70/70 and the bounded GPU selection passes 7/7
after this follow-up. Final plan completion remains gated on the measurements below.

Stage 0 image-pair coverage, allocation/retained-memory measurements and exclusive
GPU performance baselines remain open. Frozen source baseline:
`d7d1749ba9832f7d31237eb432d8e75e81bb47c3`. The user requested plan execution on
2026-09-08; the previously deferred exclusive GPU/RTX 3090 qualification remains
a final gate. Apple M4 timings are diagnostic and do not satisfy named RTX 3090
gates. No image tolerance or performance threshold has been relaxed.
See the stage handoffs below for exact evidence and remaining obligations.

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

- [x] Inventory receiver, shadow, GBuffer, retained-forward, translucent,
  preview/thumbnail, geometry-cache, shader compilation/cook, and recovery
  consumers of StaticMesh-specific prepared data; record exact migration owners.
- [x] Finalize collection context/envelope header ownership, typed factory
  binding operations, direct draw/instance semantics, participation vocabulary,
  payload lifetime, cache identities, and failure mapping. Resolve backend
  capability and shader-cook integration questions before Stage 1.
- [x] Define independent geometry and custom-factory qualification fixtures;
  place them in existing registered test owners where possible. Define numeric
  image tolerances, representative scenes, CPU/memory/performance budgets, and
  sampling policy before changing production behavior.
- [ ] Run the smallest registry-selected baseline set covering scene contracts,
  StaticMesh/Spline preparation, directional shadows, GBuffer, and resource
  reload. Record preset/backend, commands, results, captures, and known failures.

Completion: interface/failure/lifetime decisions and reproducible baseline
evidence are recorded here; unresolved items blocking later stages are closed.


#### Stage 0 decisions (2026-09-07)

These are selected implementation contracts, not claims that the interfaces
already exist. Paths below are repository-relative source ownership locations.

| Consumer | Migration owner and obligation |
| --- | --- |
| Receiver and shadow preparation | Renderer `Private/Renderers/SceneRenderPreparation.cpp`, `StaticMeshRenderPreparation.{h,cpp}`, `SceneRenderPlan.h`: preserve one preparation path, per-view collection, post-sort indices and separate resolution. |
| LOD and material extraction | Engine `Private/Rendering/StaticMeshSceneProxy.cpp` and `SplineMeshSceneProxy.cpp`: take over selection from Renderer `ViewPreparationMath.{h,cpp}` and section/material association from `StaticMeshRenderPreparation.cpp`; use one Engine-private asset helper for both providers. |
| Geometry binding and sort/cache facts | Renderer `StaticMeshDrawExecution.{h,cpp}`, `MeshRendererShared.h`, `MeshRenderPreparationCommon.h`: replace LOD/section/local-factory dereferences and six-word section geometry sort facts with checked ranges and stable resource/layout identities. No separate persistent geometry cache was found in these consumers; existing shader/PSO caches are in `StaticMeshRenderer.cpp` and `GBufferRenderer.cpp`. |
| Forward, retained-forward, translucency, masked shadow | Renderer `StaticMeshRenderer.{h,cpp}`, `StaticMeshDrawExecution.{h,cpp}`, `MeshRendererExecution.h`, `SurfaceMaterial.h`: pass policy remains Renderer-owned; factory supplies vertex stage and typed vertex bindings. |
| GBuffer | Renderer `GBufferRenderer.{h,cpp}`, `GBufferRendering.{h,cpp}`, `BaseSceneRendering.cpp`: remove `EGBufferVertexDomain` admission/selection and use registered factory capabilities. |
| Membership and visibility | Renderer `Scene.{h,cpp}`, `SceneInfo.h`, `SceneVisibility.{h,cpp}`, `DirectionalShadowView.{h,cpp}`: one authoritative primitive set; generic receiver/caster candidates, independent cascade selection, diagnostic-only family counters. |
| Telemetry and recovery | Renderer `SceneRenderTelemetry.{h,cpp}`, `MeshRendererExecution.h`, `RendererResourceSlotCache.h`, `Resources/RendererResourceCoordinator.*`: preserve attempted/successful/rejected conservation, transactional replacement, retry and generation invalidation. |
| Preview and thumbnails | DurinEd `Private/Preview/PreviewScene.cpp`, `Private/Thumbnail/ThumbnailPreviewScene.cpp`; StaticMeshEditor and MaterialEditor `Private/Thumbnail/*ThumbnailRenderer.cpp`: enter through the production scene path. Asset authoring/loading may stay concrete; audit thumbnail rejection statistics when diagnostics become generic. |
| Shader compilation and cook | Renderer `MeshRendererShared.h`, `StaticMeshRenderer.cpp`, `GBufferRenderer.cpp`; RenderCore `Private/Shader/MaterialShader.cpp`, `ShaderCookedLibrary.cpp`; ShaderBuild `Private/ShaderLibraryProducer.cpp`: factory registration owns vertex shader descriptors/permutations and participates in deterministic inventory before freeze. |

Header and API ownership:

- RenderCore `Public/GeometrySubmission.h` will own checked resource views,
  direct draw/instance ranges, topology, pass/capability identifiers and typed
  submission outcomes. `Public/VertexFactory.h` will own factory identity,
  parameter-layout compatibility and immutable binding interface declarations.
- Engine `Public/Rendering/MeshBatch.h` will own `FMeshCollectionContext`,
  `FMeshBatch`, and `FMeshBatchCollector`, because the envelope contains
  `FMaterialRenderData`. `FPrimitiveSceneProxy` gains a const render-thread
  `CollectMeshBatches(Context, Collector)` operation with an empty default.
- Context contains primitive identity, local-to-world transform/world bounds,
  normalized projected size plus fallback reason, forced-LOD policy and render
  purpose. Renderer computes projection using its existing view math; no
  `FSceneView`, scene frame, command list, component or reflected asset enters
  the provider. Engine selects LOD/residency using these scalar facts.
- Batches carry stable batch/element IDs, transform/bounds, value-retained
  material data, factory type plus immutable binding reference, resource views,
  topology/ranges and participation intent. Provider LOD diagnostics are optional.
  Stage 1's single adapter may expose legacy selected resources to the old
  prepared representation; that adapter and its concrete borrows must disappear
  in Stage 2. Generic records never downcast a batch to recover an asset.

Range, failure and backend decisions:

- Preserve RHI `FRHIDrawArguments` / `FRHIDrawIndexedArguments` semantics:
  counts are vertices or indices, `FirstVertex`/`FirstIndex` are element offsets,
  `VertexOffset` is signed, and `FirstInstance`/`InstanceCount` are explicit.
  Vulkan `VulkanContext.cpp::RHIDraw` and `RHIDrawIndexed` already forward all
  these fields; this plan requires no new RHI operation or cook file format.
- Validate byte offsets, strides, index formats, index/vertex bounds, topology
  element divisibility and instance stream ranges before admission, using widened
  arithmetic and subtraction-based bounds checks. Indexed vertex bounds include
  the signed base-vertex offset. Empty ranges produce an explicit empty result;
  overflow, invalid bounds/transform or inconsistent binding layouts are invalid
  submissions. Conservative primitive culling for invalid bounds does not make
  invalid batch data drawable.
- Outcomes are `Submitted`, `Empty`, `Excluded`, `Unsupported`,
  `InvalidSubmission`, and `ResourceFailure`. Material policy exclusions,
  including translucent shadows, are not resource failures. An advertised
  capability missing a shader or binding fails resolution. Optional GBuffer
  exclusions retain existing forward routing; required feature failures feed
  existing scene output transactions. Public counters keep their existing meaning.
- Capabilities cover pass, topology, indexed/non-indexed and instance ranges.
  Unsupported backend/factory combinations return `Unsupported` explicitly;
  neither silently substitute a single instance nor treat an unsupported draw
  as successful. Qualification on other backends remains open until executed.

Factory binding, ownership and identity decisions:

- Use a RenderCore polymorphic immutable binding object with virtual destruction,
  a factory/layout identity and validated typed parameter operations; retain it
  with `std::shared_ptr<const ...>`. Concrete typed payloads use their natural
  alignment and destruction, without a byte arena, raw `void*`, or family variant.
  Spline updates publish a new binding object; already collected frames retain
  the previous object. Material data is captured by value at collection time.
- Renderer registers a factory-owned mesh shader implementation keyed by the
  public factory type. It provides pass compatibility, vertex shader compilation
  descriptors and binding resolution using narrow transform/view uniform inputs.
  Generic processors may invoke only these vertex operations; implementations
  cannot record a pass or choose material/pass state. Registration validates
  unique identity and compatible parameter layouts before use.
- Asset buffer/factory borrows retain existing scene-command and asset-retirement
  fence boundaries. Plans must not escape that borrow; independent providers
  retain their resource owner explicitly. Resolved records retain RHI references
  through command execution. Unregister/reload cannot destroy implementations
  while resolved frames still reference them; drain those frames first.
- Shader identity is material × factory stable type × shader/layout version ×
  permutation × pass × target. PSOs additionally include vertex declaration,
  raster/depth/blend/attachment state and shader generation. Resource binding
  identity includes resource generation and immutable content revision; content
  revision alone must not create a new PSO. Deterministic sort ties use primitive,
  batch and element identity, never object addresses.
- Factory-owned shader registration must complete before runtime inventory freeze.
  Existing library serialization is retained; descriptors enter the existing
  compilation/request lifecycle. Stage 2 extends `RenderShaderCookIntegrationTests`
  to check deterministic complete factory/pass requests and runtime lookup with
  ShaderBuild unloaded, rather than keeping its current fixed count of 15 as an
  extensibility assumption. Material shader-map compilation and global runtime
  inventory are distinct existing mechanisms and both require coverage.

#### Frozen qualification fixtures and gates

- Extend `RenderContractTests` with checked ranges/layout/capability tests and
  `RendererSceneContractTests` with empty/multi-batch providers, mixed pass intent,
  ordering, hidden/removal, invalid bounds, offscreen casters and snapshot lifetime.
- Add a qualification-only provider in the existing
  `StaticMeshRenderPreparationVulkanTests` owner. It owns independent vertex/index
  buffers, two independently materialized elements and stable resource revisions;
  it must never allocate or reference `FStaticMeshRenderData`, LOD resources or
  sections. Exercise indexed, non-indexed, nonzero first-instance, multiple views,
  material replacement, buffer recreation and detach through the production scene.
- Add a second test factory with an immutable `float4` displacement/scale uniform,
  a distinct layout/version and registered vertex shader. Verify color, GBuffer,
  depth/masked-shadow deformation parity plus unsupported-pass outcomes. Share
  only fixture helpers between the preparation, shadow and GBuffer test owners;
  no test-only branch is allowed in production executors.
- Representative comparisons: opaque/masked/translucent StaticMesh and bent
  Spline; lit/unlit, retained-forward and GBuffer; near/far LOD transitions;
  one/three cascades, offscreen casters; two views with different LOD policies;
  before/after material and geometry updates. Use identical camera, asset bytes,
  backend, driver, resolution and deterministic seed for each before/after pair.
- At 1920×1080, RGBA8 comparisons allow channel error at most 2/255, with at most
  0.1% of pixels exceeding that tolerance and none exceeding 8/255. Linear
  GBuffer/depth readbacks allow absolute error 1e-5 for depth and 1e-4 for other
  float channels; compare only valid covered pixels and require identical
  coverage/masked silhouettes. Existing stricter fixture assertions still apply.
- Performance sampling: exclusive quiet GPU lane, 30 warm-up frames then 120
  measured frames, three consecutive runs, report median and p95 per run.
  Require CPU preparation/resolution and GPU pass median/p95 each within 10% of
  the same-device baseline. For baseline values below 100 microseconds, use an
  absolute 10-microsecond allowance. Keep named RTX 3090 gates in the GBuffer
  test unchanged; Apple GPU observations cannot qualify those named gates.
- CPU allocations and peak retained preparation/binding bytes must remain within
  10% of the baseline at fixed primitive/draw counts; no monotonic retained-memory
  growth over 100 update/recreate/remove cycles. Attachment memory and resolved
  draw counts must remain unchanged. Capture image bytes, device/driver metadata,
  scene parameters, timings, allocation counts and retained bytes before Stage 1.
  Missing measurements remain open gates, not zero-valued baselines.

#### Stage 0 baseline evidence

Host profile `macos-xcode-arm64`, preset `MacOS-arm64-Debug-DurinEditor`, Debug,
Tracy disabled. Authorized GPU execution used Apple M4, Vulkan API 1.3.334,
driver `0x28a1`, MoltenVK from Vulkan SDK 1.4.357.0. All builds completed
successfully. No application-hosted test
was requested or run. Commands were invoked from the checkout root.

1. `./DevTool test '@domain=renderer+static-mesh+spline,kind=contract+feature+qualification' --mode qualification --timeout 600 --agent`
   initially ran in the sandbox: 1/7 targets passed; six GPU targets failed at
   RHI initialization (`VK_ERROR_INCOMPATIBLE_DRIVER`, Metal unavailable).
   Log: `Build/.agent-state/logs/20260907-045522-660031-49430-ctest.log`.
2. The same command was rerun with authorized sandbox elevation: 5/7 targets
   passed. Passing targets: `SplineQualificationTests`,
   `StaticMeshRenderPreparationVulkanTests`, `RendererResourceReloadVulkanTests`,
   `HDRDisplayMappingQualificationTests`, `VolumetricCloudQualificationTests`.
   `DirectionalShadowBaselineVulkanTests` and `GBufferQualificationTests` both
   terminated with SIGTRAP in `VulkanPendingState.cpp:579`, validating sampled
   `DirectionalShadowDepthArray`, set 0/binding 9. The diagnostic prints
   expected/tracked access 16/16, but `FVulkanTextureStateTracker::Validate`
   checks every subresource and reports only the first tracked value; equality
   in this message does not prove the whole view is transitioned correctly.
   Log: `Build/.agent-state/logs/20260907-045606-351748-50280-ctest.log`.
   Test time 33.52 seconds. GPU timings are diagnostic: lane exclusivity was
   not established. No passing shadow/GBuffer captures were obtained.
3. `./DevTool test '@domain=renderer+static-mesh+spline,kind=contract+feature' --timeout 600 --agent`
   passed 14/14 targets, including `RenderContractTests`,
   `RendererSceneContractTests`, `StaticMeshTests`, `SplineTests`,
   `EditorRenderingTests`, and `StaticMeshThumbnailTests`.
   Log: `Build/.agent-state/logs/20260907-045650-764630-50617-ctest.log`.
   DevTool qualification mode adds a qualification label filter even when the
   selector also names ordinary kinds; therefore the ordinary lane was run
   separately in one bounded invocation. Extra targets above are selected by
   existing registry domain metadata, not separately scheduled test processes.

#### Stage 0 baseline repair handoff (2026-09-07)

The shadow/GBuffer SIGTRAP was a production graph declaration bug, not a
MoltenVK capability failure. `DirectionalShadowRendering.cpp` declared all three
array layers as managed depth attachments even for single-map rendering.
Graph entry handoffs moved all three to depth-write access, but the actual render
pass restored shader-read access only for layer 0. The other layers then failed
whole-array descriptor validation. The declaration now uses the prepared view's
actual `CascadeCount`; unused layers retain their imported shader-read state.
No RHI validation was relaxed and no manual transition or extra pass was added.

`DirectionalShadowBaselineVulkanTests` now checks the captured attachment range
against actual cascade telemetry for single-map and three-cascade fixtures.
The stale final graph transition-count expectation was also corrected from 1
to 13: managed attachment entry handoffs are now counted by the graph compiler,
in addition to the final sampled-depth/attachment boundary. The focused rerun
had already matched every frozen image hash and failed only that old structural
expectation; no image hash, image tolerance or timing threshold was changed.

Validation on the same Debug/Apple M4/Vulkan environment:

- `./DevTool test DirectionalShadowBaselineVulkanTests FDirectionalShadowBaselineVulkanTests.CapturesFrozenLitArtifactsAndSubTexelMotion --mode qualification --timeout 600 --agent`:
  descriptor assertion gone; all frozen images matched; only stale transition
  count failed before its correction. Log:
  `Build/.agent-state/logs/20260907-050327-332098-52736-ctest.log`.
- `./DevTool test '@domain=renderer+static-mesh+spline,kind=qualification' --mode qualification --timeout 600 --agent`:
  passed 7/7 targets, including all three `DirectionalShadowBaselineVulkanTests`
  cases and `GBufferQualificationTests`. GPU execution used authorized elevation.
  Test time 47.30 seconds. Log:
  `Build/.agent-state/logs/20260907-050410-352745-53503-ctest.log`.
- `./DevTool test affected --timeout 600 --agent`: passed 6/6 targets:
  `EditorRenderingTests`, `MaterialTests`, `RenderShaderCookIntegrationTests`,
  `RendererSceneContractTests`, `SkyBoxTests`, `VolumetricCloudSceneContractTests`.
  Log: `Build/.agent-state/logs/20260907-050527-829860-53717-ctest.log`.

Remaining Stage 0 gates are the full frozen before/after image-pair coverage,
allocation/retained-memory evidence and quiet-lane timing baseline. The native
correctness failure is closed; this handoff does not claim performance
qualification or completion of the geometry submission refactor.

#### Stage 0 preparation sampling handoff (2026-09-07)

`StaticMeshRenderPreparationVulkanTests.RecordsMixedGeometryPreparationBaseline`
now constructs 64 primitives (32 StaticMesh, 32 bent Spline), each with four
sections: two opaque, one masked, one translucent. It checks all 256 prepared
draws on every sample and records three runs of 30 warm-up and 120 measured
preparations. The timer covers preparation only, excluding queue dispatch,
assertions, destruction, resolution and GPU execution. No timing acceptance
threshold is applied to this diagnostic lane.

On the existing Debug/Apple M4/Vulkan profile, median/p95 nanoseconds were
7,130,395.5/7,247,792; 7,167,479/7,347,125; 7,142,583/7,265,875. Top-level prepared
vector capacity was 274,960 bytes in every run. This is container storage only:
allocator overhead, transitive material ownership, bindings, temporary sorting
storage and process retention are excluded. It does not close the allocation
count, full retained-memory or update/recreate/remove-cycle gates.

Validation: `./DevTool test StaticMeshRenderPreparationVulkanTests --mode qualification --timeout 600 --agent`
passed the whole target, including the added case. Receipt:
`Build/.agent-state/logs/20260907-051115-088335-56781-ctest.log`.
Per-run values are emitted in GoogleTest properties and standard output
(CTest `Testing/Temporary/LastTest.log`); the values above preserve the baseline
when CTest overwrites that temporary log.

Decision change authorized by the user: exclusive GPU and named RTX 3090
performance qualification may remain open until final acceptance rather than
blocking Stage 1. This does not waive those gates or declare missing local
measurements passed.

### Stage 1: Introduce geometry submission and adapt existing providers

Dependencies: Stage 0. Outcome: both existing proxies emit the common contract.

- [x] Add the logical batch, checked range/resource views, collection context,
  collector, and render-thread primitive submission seam at the selected layers.
- [x] Move StaticMesh LOD/residency selection, section extraction, and material
  association into its provider; adapt Spline with the same geometry mechanism
  and its own immutable deformation binding.
- [x] Exercise the collector from production preparation through one temporary
  compatibility adapter; record that adapter's removal in Stage 2. Do not keep
  two independently maintained selection/material algorithms.
- [x] Add contract coverage for empty and multiple batches, indexed/non-indexed
  and instance ranges, overflow/out-of-range rejection, material slots, invalid
  bounds, unavailable LODs, immutable snapshots, and borrow/retirement rules.

Completion: existing families enter preparation through collection, preserve
baseline behavior, and no provider imports Renderer-private declarations.

#### Stage 1 contract groundwork (2026-09-07)

`RenderCore/Public/GeometrySubmission.h` introduces typed submission outcomes,
retained buffer views and overflow-safe stream/draw range checks. Direct and
indexed arguments retain RHI first-element, signed base-vertex and instance
semantics. `RenderContractTests` covers partial final strides, empty ranges,
uint64 overflow, index formats/alignment, negative base vertices, topology
arity and instance stream bounds. This is additive groundwork; providers and
production preparation do not consume it yet.

`Engine/Public/StaticMesh/StaticMeshLODSelection.h` now owns the existing LOD
selection/residency API and its implementation. Renderer retains projected-view
math and includes the Engine API. Selection and fallback algorithms are moved
unchanged, preserving the existing test boundary while removing their Renderer
ownership. Section/material extraction and the common collector remain open.

Validation: `RenderContractTests` passed 141 cases, including four new geometry
range cases. `./DevTool test affected --timeout 600 --agent` passed after the
module ownership move (36 selected targets). Receipt:
`Build/.agent-state/logs/20260907-051614-588613-59723-ctest.log`.
The full `./DevTool build --target all` also passed after the cross-module
export move; receipt:
`Build/.agent-state/logs/20260907-051719-377932-61354-cmake.log`.

#### Stage 1 collection seam handoff (2026-09-07)

`Engine/Public/Rendering/MeshBatch.h` now owns scalar collection context,
value-retained material elements, stable primitive/batch/element identities,
explicit receiver/shadow intent and transactional batch admission. It rejects
invalid/singular transforms, invalid bounds, duplicate identities, inconsistent
factory/layout keys, invalid draw/instance ranges, unavailable resources and
incorrect buffer usages. Empty and policy-excluded batches have distinct
outcomes. RHI resource references and immutable polymorphic factory bindings
are retained with the collected snapshot.

`FPrimitiveSceneProxy::CollectMeshBatches` provides the const render-thread
submission seam with an empty default. `FVertexFactoryBinding` establishes
immutable typed payload lifetime and factory/layout identity; pass-specific
typed parameter operations remain part of Stage 2 factory integration.

Two `RendererSceneContractTests` cases cover the empty default and an independent
(non-StaticMesh-resource) multi-element collector fixture. They exercise
transactional rejection, missing/wrong-usage buffers, layout mismatch, duplicate
IDs, singular transforms, invalid bounds, policy exclusion and exactly-once
virtual destruction of an aligned binding retained after provider references
are released. This fixture qualifies the collector boundary only: it does not
claim the Stage 4 independent provider can render through the production scene.

Validation: the whole `RendererSceneContractTests` target passed 45 cases;
`./DevTool test affected --timeout 600 --agent` passed 39/39 targets. Receipt:
`Build/.agent-state/logs/20260907-052301-903072-65034-ctest.log`.

Stage 1 task 1 is complete. Provider collection overrides, production adapter,
material/LOD snapshot qualification and all later stages remain open.

#### Stage 1 provider integration (2026-09-08)

StaticMesh and Spline now override `CollectMeshBatches`. The Engine-private
`StaticMeshBatchCollection.h` helper owns the sole LOD/residency, section range
and material association algorithm. Collection retains material values and an
immutable Local/Spline binding. Each Spline collection captures its dynamic
revision and accepted-update count; subsequent provider updates cannot mutate
that snapshot. The collector records typed admission/failure outcomes.

Production preparation calls the base proxy collection operation for each view.
Its single compatibility adapter reads the selected asset borrows from
`StaticMeshBatchBinding.h`; these fields, its concrete casts and the legacy
prepared representation must disappear in Stage 2. Prepared primitives retain
the collected binding under the existing scene-command asset retirement fence.
No Renderer-private declaration enters either Engine provider.

The existing preparation qualification covers per-view LOD selection, forced
LOD, residency fallback/retry, material slots, pass classification, deterministic
ordering and singular transforms. Added checks exercise real provider material
replacement and immutable Spline snapshots across receiver/shadow collections.

Validation on Debug/Apple M4/Vulkan:

- Initial `./DevTool test affected --timeout 600 --agent`: passed 28/28 targets;
  receipt `Build/.agent-state/logs/20260908-001045-477593-90204-ctest.log`.
- `./DevTool test '@domain=renderer+static-mesh+spline,kind=qualification' --mode qualification --timeout 600 --agent`:
  passed 7/7 targets with existing image assertions unchanged; receipt
  `Build/.agent-state/logs/20260908-001213-527177-93949-ctest.log`.
  GPU timings remain diagnostic; Stage 0 memory/image-pair and final exclusive
  performance gates remain open.
- Final `./DevTool test affected --timeout 600 --agent`: passed 71/71 targets;
  receipt `Build/.agent-state/logs/20260908-001426-761501-94363-ctest.log`.
- Final provider failure-outcome assertions: the complete
  `StaticMeshRenderPreparationVulkanTests` target passed; receipt
  `Build/.agent-state/logs/20260908-001512-692334-95874-ctest.log`.
- `./DevTool doc validate --scope changed` and `git diff --check` passed.

Stage 1 is complete. Stage 2 must remove the compatibility adapter and concrete
prepared data; no independent provider has yet qualified production rendering.

### Stage 2: Generalize prepared draws and mesh-pass execution

Dependencies: Stage 1. Outcome: all mesh passes consume generic prepared data.

- [x] Replace family-specific prepared primitives/sections and geometry-cache
  inputs with the public logical contract and Renderer-private generic records;
  migrate all identified preview and production callers and remove the adapter.
- [x] Implement Local/Spline factory descriptors, shader permutations, typed
  binding resolution, and factory-aware cache identities using the existing
  registration/resource lifecycle. Cover cooked shader lookup as applicable.
- [x] Route receiver, shadow depth, GBuffer, retained-forward, and translucent
  draws through common mesh-pass processing with pass-owned policy. Remove
  Local/Spline shader-selection branches from generic executors.
- [x] Preserve immutable plans, post-sort resolved indices, resource retry and
  fallback semantics, deterministic ordering, and telemetry conservation.
- [x] Validate masked clipping and Spline deformation parity across color,
  GBuffer, and shadow; exercise material replacement and resource invalidation.

Completion: no generic prepared/execution record requires StaticMesh or Spline
data; supported passes resolve through factory capabilities and match baselines.

#### Stage 2 generic geometry and factory execution handoff (2026-09-08)

`FVertexFactoryInputBinding` retains declaration/stream snapshots. Prepared
primitives no longer contain `FStaticMeshLODResources*`, `FLocalVertexFactory*`
or inline Spline data; prepared draws no longer contain `FStaticMeshSection*`.
The Stage 1 concrete resource adapter is gone. Provider-only LOD and material-slot
diagnostics preserve current histograms and deterministic ordering. Direct draw
arguments retain index/non-indexed, base-vertex and instance semantics through RHI.

`MeshVertexFactory.{h,cpp}` owns registered vertex operations, with Local/Spline
implementations resolving typed shader references and deformation parameters for
forward, shadow and GBuffer. Generic executors no longer select Local/Spline
vertex shaders or unpack Spline payloads. Registration rejects missing/duplicate
identities, is synchronized, and retains implementations for module lifetime.
Factory/layout, declaration and topology participate in pipeline identity.

This is a partial Stage 2 handoff, not stage completion. Still required:

- Remove the one-batch production restriction and preserve primitive-level
  accounting for independent multi-batch providers and 64-bit element identities.
- Complete factory capability validation and typed unsupported/failure routing,
  including generic stream and instance-range qualification.
- Integrate factory/pass requests before shader inventory freeze, and extend
  cooked coverage with ShaderBuild unloaded. The existing 15-request integration
  test exercises the old fixed inventory only; it does not prove this gate.
- Qualify independent providers/custom factories, resource generations and the
  predeclared image/memory/performance comparisons before final acceptance.

The existing GPU regression selection passed 7/7 targets after vertex-factory
migration with all frozen image expectations unchanged; receipt
`Build/.agent-state/logs/20260908-002524-584939-99639-ctest.log`.
The affected ordinary selection passed 71/71 targets; receipt
`Build/.agent-state/logs/20260908-002752-816708-99755-ctest.log`.
Final validation after topology identity, batch-transform consumption and the
new factory registration contract test:

- `./DevTool test affected --timeout 600 --agent`: 71/71 targets passed;
  receipt `Build/.agent-state/logs/20260908-003251-980854-1502-ctest.log`.
- The same bounded GPU qualification selection: 7/7 targets passed;
  receipt `Build/.agent-state/logs/20260908-003406-835663-2707-ctest.log`.
- Changed-document validation and whitespace validation passed.

These results establish existing-family correctness, not the remaining extension
or performance gates. The lasting implemented boundary is recorded in
[Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md).

### Stage 3: Unify primitive membership and view candidates

Dependencies: Stage 2. Outcome: primitive type no longer gates pass admission.

- [x] Replace StaticMesh/Spline membership and visibility output lists with
  generic primitive candidates; maintain atomic add/remove/release ownership.
- [x] Remove caster-family classification and per-family cascade candidate
  lists; retain independent caster volumes, cascade masks, and per-view LOD.
- [x] Apply conservative relevance and batch-level participation through the
  common policy. Preserve diagnostic family statistics without dispatch authority.
- [x] Remove obsolete typed render-consumer getters/enums. Retain any narrowly
  justified mutation/diagnostic identity only with its remaining use documented.
- [x] Update Scene Representation and Renderer Frame Preparation contracts to
  describe the implemented generic primitive route and unchanged non-mesh rules.
- [x] Test offscreen casters, mixed participation within one primitive, hidden
  primitives, invalid bounds fallback, cascade membership, update ordering, and
  detach/release with no stale candidates.

Completion: a new provider requires no membership, visibility, or caster switch
entry; scene lifecycle and candidate/outcome conservation checks pass.

#### Generic candidates and extension qualification handoff (2026-09-08)

Scene ownership, receiver visibility and directional cascade candidates now use
one primitive list. Collection accepts multiple batches, preserves 64-bit batch
and element identities, and applies independent shadow participation. Family
counts remain diagnostic. The user explicitly authorized repository cleanup;
`GetStaticMeshProxy` and its declaration are removed, with both test callers
using checked concrete casts from `GetProxy`. `GetSplineMeshProxy` remains only
for dynamic Spline mutation and test inspection.

Input validation checks actual RHI declarations, stream offsets/strides, vertex
and instance-rate bounds, and resource-view consistency. Sort declarations are
owned value vectors including input rate; trailing unused attributes are omitted.
Collection transfers material snapshots into prepared draws before publication.
No pointer-based declaration sort or shared-storage aliasing was introduced.

Factory registration contributes six built-in pass requests before inventory
freeze. Cook tests verify deterministic inventory bytes, load every request
with ShaderBuild unloaded, and construct a vertex-only material map for each
factory request. Material linking permits independently compiled stage source
paths only on its explicit linking path; ordinary source-map compilation keeps
its source-path restriction and stage/reflection validation. Complete cooked
vertex-plus-generated-fragment permutations remain an acceptance obligation.

`GeometrySubmissionTestSupport.h` owns independent vertex/index/instance buffers
without StaticMesh render data, LODs or sections. Its custom factory owns a
separate shader source and deformation uniform. The production fixture exercises
two batches, indexed/non-indexed draws, first instance 2 with two instances,
actual instance-rate shader fetch, masked/opaque materials, and one non-casting
batch. At 1920×1080, lit/unlit final-color comparisons match an equivalent
existing-factory transform under the frozen tolerance. One hundred material and
buffer replacement cycles retain the original snapshot correctly. This is
lifetime correctness evidence, not allocation or full retained-memory accounting.
Direct GBuffer/depth capture parity and optional-pass fallback qualification
remain open.

Validation:

- `./DevTool test affected --timeout 600 --agent` passed 71/71; receipt
  `Build/.agent-state/logs/20260908-064651-642201-35310-ctest.log`.
- `./DevTool test '@domain=renderer+static-mesh+spline,kind=qualification' --mode qualification --timeout 600 --agent`
  passed 7/7 after the final cleanup and owned sort/material changes; receipt
  `Build/.agent-state/logs/20260908-064738-636071-36475-ctest.log`.
  These are correctness results; GPU timings remain diagnostic.
- Changed-document validation and `git diff --check` passed.
- Earlier focused independent geometry GPU execution passed, including 1080p
  parity and instance-rate input; receipt
  `Build/.agent-state/logs/20260908-063310-518971-31307-ctest.log`.
- Focused cook integration passed; receipt
  `Build/.agent-state/logs/20260908-062831-720992-29343-RenderShaderCookIntegrationTests.log`.

Remaining audits include required shadow collection resource-failure propagation,
factory compile-option parity with cooked requests, direct attachment parity,
full allocations/retention and exclusive same-device performance comparisons.
The named RTX 3090 gate is unexecuted on this Apple M4 host. No thresholds were
changed, and the plan remains Active.

### Stage 4: Prove extension boundaries and retire migration scaffolding

Dependencies: Stage 3. Outcome: independent extension evidence and final handoff.

- [x] Add a qualification-only procedural provider owning its own geometry,
  with no StaticMesh render-data/section dependency, using an existing factory.
  Verify main view, GBuffer, retained-forward, translucency policy, and shadows
  through the production pipeline without core dispatch changes.
- [x] Add a minimal custom-factory fixture with a distinct deformation/binding
  layout. Verify factory registration, color/depth/GBuffer deformation parity,
  masked shadows, cache separation, and explicit unsupported-pass outcomes
  without modifying generic executors. This is not a skeletal-animation feature.
- [x] Exercise non-indexed and instance-range submission on supported backends,
  geometry/material updates, removal, resource recreation, and multiple views.
- [x] Remove temporary adapters, obsolete family-based draw structures, dead
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

#### Attachment, capability and cooked-stage closure (2026-09-08)

Decision refinement: shader-type `ModifyCompilationEnvironment` owns vertex
compile configuration for both authored material maps and the cook producer.
Factory implementations select those shader types and retain typed bindings;
they no longer provide a second compile-option callback. Opaque shadows use
the registered shadow vertex stage plus the separate `Surface.OpaqueShadow`
fixed fragment request. The former authored-only vertex-output shortcut is
removed so both data domains execute the same permutation. Shader library
serialization and material identity validation are unchanged.

Required input-binding failures are recorded as ResourceFailure during
preparation. A shadow cascade with a collection resource failure disables the
shadow transaction through the existing preparation-failure path; a subsequent
valid collection recovers. It cannot silently render a partial shadow set.

Qualification samples scene depth into R32_FLOAT using a test capture shader.
The RHI intentionally defers direct depth transfers, so the failed attempt was
replaced without relaxing transfer validation or extending RHI. The same capture
shader family samples layer zero of the single-map production shadow array
through a read-only capture sink. Tests require nonempty covered regions,
identical coverage and depth error at most 1e-5. Constant material GBuffer
encodings match exactly, a stricter comparison than the frozen float allowance.
The tests leave capture sinks unset after each render.

Focused receipts:

- Cooked stage combinations after ShaderBuild unload passed:
  `Build/.agent-state/logs/20260908-065855-181808-42362-RenderShaderCookIntegrationTests.log`.
- Independent geometry, scene/GBuffer depth, optional GBuffer exclusion,
  translucency and shadow failure/recovery passed:
  `Build/.agent-state/logs/20260908-070208-189447-45523-ctest.log`.
- Actual masked-shadow depth/coverage parity passed:
  `Build/.agent-state/logs/20260908-070349-209904-47364-ctest.log`.

Final regression receipts:

- `./DevTool test affected --timeout 600 --agent`: 70/70 passed;
  `Build/.agent-state/logs/20260908-070730-839227-50278-ctest.log`.
- `./DevTool test '@domain=renderer+static-mesh+spline,kind=qualification' --mode qualification --timeout 600 --agent`:
  7/7 passed, including the final missing-binding and independent existing-factory
  translucency cases; `Build/.agent-state/logs/20260908-071002-120071-51650-ctest.log`.

Full allocator counts, transitive peak retained bytes, complete frozen-source
image pairs and exclusive CPU/GPU performance comparison remain open. The
100-cycle weak-reference check proves binding release, not the full memory
budget. Existing top-level vector measurements do not include transitive storage.
An Allocations capture was attempted with `xcrun xctrace record --template
Allocations --time-limit 60s` against only the
`RecordsMixedGeometryPreparationBaseline` test process. Instruments reported
"Failed to attach to target process" and exited with code 2. No allocation
measurements were obtained; machine profiling authorization was not changed.
The failed diagnostic trace is `/tmp/geometry-submission-allocations-20260908.trace`.
Allocator qualification needs a working process-profiling environment before
it can be compared with the frozen source baseline.
RTX 3090 access has been requested; this Apple M4 machine cannot satisfy that
named gate. No qualification requirement has been waived. Remaining before/after captures,
allocator measurements and timing qualification should run on the final
qualification device against both the frozen source commit and the final
implementation; this preserves identical device/driver/scene conditions and
avoids substituting cross-device measurements.

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
