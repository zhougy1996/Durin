# Scene Render Frame Orchestration Simplification Plan

Summary: Collapse the one-shot scene frame entry, establish one frame-owned context and canonical feature plan, and move graph authoring plus command recording behind feature-owned `AddPasses` boundaries without changing rendered behavior.

Last reviewed: 2026-09-01

Status: Active
Completed:

## Current Status

Stage 3 proves the feature-owned boundary with GBuffer and Contact Visibility.
Both private feature headers now publish only compact canonical inputs, typed
outputs, and `AddPasses`; their sources own parameter metadata, graph setup,
callbacks, and private recording functions. Composer calls those entries
directly, and their former contributor inputs and recorder methods are removed.
The recorder facade no longer borrows either feature renderer. Focused scene
contracts remain 40/40.

Stage 2 now publishes one canonical feature plan after resource preparation.
Purpose flags preserve production, debug, qualification, and dependency demand;
deferred/GBuffer/AO closure is computed once; and Contact Visibility,
cloud-shadow, and cloud-spatial decisions own their exact route and reason.
Their explicit preparation entries resolve persistent route resources and call
pure selectors without empty-target render calls or preparation-only policy
branches. Composer and feature callbacks consume const decision references, and
callbacks reject capability drift instead of reselecting a route. The focused
scene contract target continues to pass all 40 tests.

Stage 1 established one frame owner and stack-owned five-part context. The
pipeline now owns graph compile, allocation-backed execution, capture, result
mapping, and final transaction publication directly; the executor object and
one-shot callback are removed. The focused scene contract target continues to
pass all 40 tests with the pipeline/context type boundary asserted. Stage 0
previously froze the migration inventory and baseline contract. The active
[RDG Compatibility Retirement and Allocator Simplification Plan](RDGCompatibilityRetirementAndAllocatorSimplification.md)
has explicitly released ownership of the scene-render orchestration sources;
its only remaining work is an exclusive quiet-GPU qualification rerun and a
documentation-only status update. Its RDG parameter, allocation, capture,
setup/execute, and output-transaction semantics remain frozen dependencies.

The current frame path computes deferred, ambient-occlusion, GBuffer, and
route decisions more than once; carries both generic topology routes and
feature-specific prepared routes; expands those values through a wide compose
input into feature contributor inputs; and finally reaches command recording
through one recorder facade borrowing renderer services and mutable frame
state. The executor and pipeline are both one-shot objects, but graph execution
is returned to the executor through a callback supplied to the pipeline.

The frozen production footprint is 1,633 lines across the executor, pipeline,
composer, contributor declarations, and recorder facade. Eight transported
decision names appear 76 times across those files: `bNeedsGBuffer` (13),
`bRequiresDeferredOpaque` (12), `bWantsDeferredInputs` (10),
`bWantsGroundTruthAmbientOcclusion` (8), `bWantsIsolatedDeferred` (14),
`bWantsIsolatedGroundTruthAmbientOcclusion` (2),
`bWantsProductionDeferred` (15), and
`bWantsProductionGroundTruthAmbientOcclusion` (2). Contact visibility, cloud
shadow, and cloud spatial rendering each carry both a generic topology route
and a feature route. The recorder facade borrows 19 renderer services or frame
state references, and the pipeline returns graph execution through one
one-shot callback.

The destination inventory is frozen as follows: fitted view, caller options,
output intent, prepared scene work, and editor-assistance requests belong to
`Logical`; persistent resources and pre-graph availability belong to
`Resolved`; deferred/AO/GBuffer dependency closure plus contact/cloud route and
reason belong only to `Features`; pending temporal state and final output
publication belong to `Transaction`; telemetry, statistics, graph capture, and
failure diagnostics belong to `Observation`. RDG handles, parameter blocks,
and pass results remain graph-local compiler/execution values rather than a
second frame decision representation.

The pre-change `RendererSceneContractTests` baseline passes all 40 tests. Its
typed-pass, route-exclusivity, contributor-order, parameter-authority, temporal
commit/abort, graph-capture, telemetry-publication, resize, multi-view, and
repeated-view contracts are the structural oracle. Existing Renderer view,
editor-assistance, Vulkan cloud, and qualification coverage remains the oracle
for disabled/factor-one/compute/fragment routes, fallback/resource failures,
present/offscreen output, and rendering parity; Stage 5 reruns the bounded
affected set after migration.

## Goal

Make one scene-render submission readable as a single ownership flow:

`Logical -> Resolved -> Feature Plan -> Compose/AddPasses -> Execute -> Commit/Observation`

One stack-owned frame context carries the submission's logical plan, resolved
resources, final feature decisions, transaction state, and observations. One
frame pipeline owns validation, preparation, graph construction,
compile/execute/capture, and final commit or abort. The composer remains the
only cross-feature scheduling authority, while each feature owns its RDG
declarations, pass callback, and command-recording implementation.

## Scope

- Merge the one-shot `FSceneRenderGraphExecutor` responsibility into
  `FSceneRenderPipeline` and remove the graph-execution callback inversion.
- Replace pipeline-owned mutable frame members with one stack-owned
  `FSceneFrameContext` divided into `Logical`, `Resolved`, `Features`,
  `Transaction`, and `Observation` sections.
- Replace `FSceneRenderTopology`, prepared feature routes, and transported
  `bWants*` / `bNeeds*` decisions with one immutable
  `FSceneFrameFeaturePlan` built after resource resolution.
- Give routed features one feature-specific final decision containing the
  selected route and reason. Preserve meaningful fallback states such as
  contact/cloud-shadow factor-one rather than translating them through a
  generic disabled route.
- Keep `FSceneRenderGraphComposer` as the stable schedule and cross-feature
  wiring authority.
- Move contributor declarations, RDG parameter metadata, pass setup, callback
  execution, and recorder bodies into feature-owned rendering units, then
  remove the central contributor, service, and recorder facades.
- Preserve graph-owned typed values, typed pass parameters, narrow typed
  outputs, pass result status, explicit output roots, capture, telemetry, and
  temporal commit/abort semantics.

## Non-Goals

- Do not change shading algorithms, output formats, quality tiers, fallback
  policy, view visibility, draw ordering, material selection, or public render
  options/statistics.
- Do not change RDG compilation, culling, allocation, transition planning,
  execution scheduling, structural budgets, or capture schema.
- Do not introduce a public pass registry, mutable frame blackboard, generic
  dependency-injection container, runtime feature graph, or plugin scheduler.
- Do not pass the complete mutable frame context into feature callbacks.
- Do not remove the RDG setup/execute boundary: graph resources and uses remain
  setup-time declarations, and RHI commands remain execute-time callback work.
- Do not combine forward/deferred rendering paths or redesign feature renderer
  resource ownership in this plan.

## Selected Architecture

### Frame Owner and Lifetime

`FSceneRenderer::RenderView_RenderThread` constructs one temporary
`FSceneRenderPipeline` and calls its frame entry. The pipeline retains one
borrowed `FSceneRenderer&`; submission-specific state is constructed inside the
entry as `FSceneFrameContext` and does not survive the call. The pipeline
directly owns the stack `FRDGBuilder`, compilation, allocation-backed
execution, capture publication, and result translation.

`FSceneFrameContext` has five explicit ownership partitions:

- `Logical` owns the fitted view, output description, immutable
  `FSceneRenderPlan`, editor-assistance preparation, and caller policy snapshot.
- `Resolved` owns `FResolvedSceneResources`, selected persistent fallbacks, and
  fallible readiness facts established before graph authoring.
- `Features` owns the immutable final `FSceneFrameFeaturePlan`, including
  purposes, dependency closure, selected route, route reason, quality, and
  feature extent where applicable.
- `Transaction` owns temporal begin/commit/abort state and final Scene Color and
  post-process publications.
- `Observation` owns `FSceneRenderTelemetry` and submission-local diagnostic
  state. Correctness and route selection never read this partition.

Only the pipeline and composer may see the complete context. Logical,
resolved, and feature-plan access is const after publication. Feature
`AddPasses` functions receive feature-specific const views plus exact upstream
typed outputs; callbacks receive only immutable captured values, submitted pass
parameters, the parameter resolver, and the required feature services.

### Canonical Feature Decisions

`FSceneFrameFeaturePlan` is constructed once after logical preparation and
resource resolution. It distinguishes intent from final capability without
publishing two final representations:

- Purpose flags describe why work exists, including production, public debug,
  and qualification. Purposes may coexist.
- A feature-specific route decision describes what will execute and why.
- Dependency closure determines shared producers such as GBuffer once. Callers
  query the canonical plan rather than carrying `bNeedsGBuffer` or equivalent
  booleans through compose and pass inputs.
- Persistent readiness belongs to `Resolved`; graph target presence belongs to
  typed RDG handles; callback success belongs to typed pass results. No generic
  readiness boolean duplicates those three lifetime classes.

Contact visibility, cloud-shadow visibility, and cloud spatial rendering gain
explicit route-resolution entry points. Route resolution consumes settings,
qualification policy, platform capability, resolved input availability, and
expected graph target capability, and returns the exact feature route and
reason. It does not call a render function with null targets or a
`bPreparationOnly` policy. Once the feature plan is published, neither the
composer nor a callback reselects the route.

### Feature Graph Boundary

The composer declares the stable graph order and passes each producer's typed
output directly to its consumers. Each existing feature rendering unit owns:

- its compact `AddPasses` declaration and feature-specific input/output types;
- its RDG resource and parameter structures plus metadata;
- logical texture/value creation and exact dependency declarations;
- its deferred pass callback and private command-recording function;
- its feature result and observation updates.

Per-feature inputs may hold references to canonical context partitions,
feature decisions, exact renderer services, and upstream RDG outputs. They may
not copy final decisions into additional `bEnabled`, `bRequested`,
`bProductionDeferred`, `bWants*`, `bNeeds*`, `GraphRoute`, or `PreparedRoute`
fields. Pass parameter structures and typed graph outputs remain distinct
because they are compiler capabilities and dependency edges, not duplicate
policy expressions.

The migration removes `FSceneRenderGraphServices`,
`FSceneRenderGraphComposeInputs`, `FSceneRenderFeatureRecorders`, the generic
graph-contributor declarations, and the centralized graph-parameter
implementation after their last users move. The composer may include private
feature headers directly; no replacement umbrella header may reproduce the
former wide input and service surface.

### Failure, Transaction, and Observation

Every early failure aborts the pending view-state transaction. Graph compile,
allocation, or execution failure publishes neither successful final output nor
public statistics. Successful post process, including optional editor
assistance, commits temporal state and reduces observation into the existing
public statistics exactly once.

Pass callbacks remain safe for deferred or parallel execution: they do not
read mutable pipeline state, select routes, allocate graph targets, commit the
frame transaction, or publish capture. Feature-local GPU timing and telemetry
continue to observe actual command-recording results.

## Implementation Stages

### Stage 0: Freeze the frame contract and migration inventory

- [x] Wait for the RDG compatibility plan to complete or record an explicit
  handoff that releases the overlapping scene-render source files; do not run
  concurrent source-writing stages in the same checkout.
- [x] Inventory every current decision from logical request through topology,
  prepared route, compose input, feature input, callback capture, pass result,
  and telemetry, and assign exactly one destination in `Logical`, `Resolved`,
  `Features`, `Transaction`, or `Observation`.
- [x] Record the baseline production source footprint and the exact counts of
  transported `bWants*`, `bNeeds*`, generic/feature route pairs, borrowed
  recorder services, and one-shot execution callbacks.
- [x] Freeze representative graph captures and results for contact visibility,
  cloud shadow, and cloud spatial disabled/factor-one, compute, fragment, and
  fallback cases; include GBuffer/deferred/AO dependency combinations.
- [x] Freeze present, offscreen, editor-assistance, missing-output,
  resource-unavailable, compile-failure, execution-failure, temporal
  commit/abort, telemetry, resize, and repeated-view behavior.
- [x] Add or strengthen focused contract assertions where the existing tests do
  not distinguish policy selection, RDG setup, callback execution, and final
  transaction publication.

#### Acceptance Gate

- The predecessor write boundary is clear, every duplicated field has one
  selected destination, and behavior/capture baselines cover all routes and
  frame transaction outcomes affected by the refactor.

### Stage 1: Establish the single frame owner and context

- [x] Add the five-part `FSceneFrameContext` and construct it on the stack for
  each render submission.
- [x] Move telemetry, resolved resources, temporal context, view-state pointer,
  final publications, and submission diagnostics out of persistent pipeline
  members into their context partitions.
- [x] Make frame preparation, resolution, composition, execution, and
  finalization consume the appropriate context partition with const access
  after publication.
- [x] Move graph compile/execute/capture behavior into `FSceneRenderPipeline`
  and remove `FSceneRenderGraphExecute` and the pipeline-to-executor callback.
- [x] Change `FSceneRenderer::RenderView_RenderThread` to invoke the pipeline
  directly, then remove `FSceneRenderGraphExecutor` and its friend/declaration,
  source registration, and type-shape tests.
- [x] Preserve current compile/execute warning cardinality, structural budget
  observation, allocator ownership, capture publication, and result mapping.

#### Acceptance Gate

- One temporary pipeline owns the complete submission; all mutable frame data
  is stack-owned by one context; there is no execution callback inversion or
  second one-shot frame object; focused frame, failure, capture, and view-state
  tests pass without graph-shape changes.

### Stage 2: Publish one immutable feature plan

- [x] Introduce purpose flags and feature-specific decision records for
  deferred, GBuffer, ambient occlusion, contact visibility, cloud-shadow
  visibility, cloud spatial/composite, GBuffer debug, post process, and editor
  assistance.
- [x] Build shared dependency closure once, including the production,
  qualification, debug, and ambient-occlusion reasons that require GBuffer or
  deferred inputs.
- [x] Extract pure route-resolution APIs for contact visibility, cloud-shadow
  visibility, and cloud spatial rendering; preserve their exact route-reason
  telemetry and factor-one/disabled semantics.
- [x] Remove preparation-only render calls with null targets and remove the
  associated expected/ready target policy branches after their last caller.
- [x] Replace `FSceneRenderTopology`, prepared route locals, compose booleans,
  and contributor route pairs with const references to the canonical feature
  plan.
- [x] Assert that the feature plan is not mutated after graph authoring begins
  and that callbacks execute the authored route without capability reselection.

#### Acceptance Gate

- Every frame feature decision has one final representation; GBuffer and
  deferred dependency closure is computed once; route and reason parity passes
  for all production, debug, qualification, forced-fragment, compute,
  fallback, invalid-input, and unavailable-resource cases.

### Stage 3: Prove the feature-owned `AddPasses` boundary

- [x] Define the private feature-header convention for compact canonical views,
  typed outputs, and `AddPasses`; keep pass resource/parameter implementations
  in the owning feature source.
- [x] Migrate GBuffer as the representative multi-attachment graphics producer,
  moving its parameter metadata, pass setup, callback, and recording body
  behind one feature-owned entry.
- [x] Migrate contact visibility as the representative routed
  graphics/compute/factor-one feature, using the canonical route decision and
  exact upstream directional-shadow/GBuffer outputs.
- [x] Remove the migrated methods and services from the recorder facade and
  remove their contributor input structures without introducing equivalent
  forwarding adapters.
- [x] Verify that Composer remains the only cross-feature ordering authority
  and that neither migrated feature can inspect the full frame context.

#### Acceptance Gate

- GBuffer and contact visibility preserve their parameter declarations,
  graph captures, callbacks, telemetry, render-target layouts, and Vulkan
  results through direct feature-owned `AddPasses` entries, with no copied
  policy booleans or duplicate routes.

### Stage 4: Migrate the complete scene feature schedule

- [ ] Migrate directional shadow, ambient occlusion, cloud-shadow visibility,
  deferred directional lighting, base scene, cloud spatial/composite, Scene
  Color/translucency, post process, and editor assistance to the proven
  feature-owned boundary.
- [ ] Move each pass resource/parameter metadata definition from the central
  graph-parameter source into its owning feature source while preserving stable
  structure names and field paths used by captures and diagnostics.
- [ ] Replace the wide compose input with direct access to the canonical
  frame context and feature-specific const views; keep complete-context access
  confined to Pipeline and Composer.
- [ ] Remove `FSceneRenderGraphServices`, `FSceneRenderGraphComposeInputs`,
  `FSceneRenderFeatureRecorders`, generic contributor declarations/macros, and
  centralized graph-parameter files after their final users move.
- [ ] Remove obsolete friends, includes, forward declarations, build entries,
  and tests that assert deleted facade type shapes; replace them with ownership
  and single-authority contract checks.
- [ ] Confirm the final composer reads as the stable feature schedule and
  contains no route selection, resource preparation, command recording, or
  telemetry reduction.

#### Acceptance Gate

- All production passes use typed parameterized RDG authoring through their
  owning feature; the composer is the sole scheduler; no central recorder,
  service bag, compose boolean bundle, contributor facade, or centralized
  parameter implementation remains.

### Stage 5: Qualify, document, and complete the ownership change

- [ ] Pass the focused Renderer scene contract, view, editor-assistance,
  render-target-layout, GBuffer, contact-shadow, volumetric-cloud, resource
  recovery, RDG, and Vulkan transition coverage selected through the repository
  testing workflow.
- [ ] Pass representative present/offscreen, resize, repeated/multi-view,
  forced route, fallback, allocation failure, device recovery, and Editor smoke
  qualification without changing the frozen public results or graph contracts.
- [ ] Pass the required build and routine native-test aggregates according to
  the repository build and testing workflows.
- [ ] Compare final captures with Stage 0 for pass identities, parameter field
  paths, routes, dependencies, transitions, typed values, output roots,
  allocation observations, and transaction results; explain any intentional
  structural difference before accepting it.
- [ ] Record final production source footprint and decision/route/service/
  callback counts. Confirm that removed facades were not recreated under new
  names and that feature input fields express capabilities rather than copied
  policy facts.
- [ ] Update the lasting
  [Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md)
  and [Render Graph](../Runtime/Rendering/RenderGraph.md) contracts to describe
  the final frame owner, feature plan, Composer boundary, and feature-owned
  setup/execute split.
- [ ] Run changed/all documentation validation and all-plan lifecycle
  validation, then record evidence and complete the plan.

#### Acceptance Gate

- Rendering, failure, capture, telemetry, memory, transition, and transaction
  behavior remain qualified; one frame owner and one feature decision authority
  remain; all removed architecture symbols are absent; lasting documentation
  describes the implemented ownership model.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Frame ownership | One pipeline invocation owns preparation through finalization; no executor callback or persistent mutable submission state remains. |
| Decision authority | Repository searches and contract tests find one final feature plan, no transported `bWants*` / `bNeeds*` policy fields, and no generic plus prepared route pairs. |
| Route parity | Disabled/factor-one, compute, fragment, forced, debug, qualification, invalid-input, and unavailable-resource cases preserve selected route and reason. |
| RDG contract | Every production pass retains one immutable typed parameter object, exact resources/values, stable metadata field paths, and a deferred callback. |
| Scheduling | Composer alone orders features and wires narrow typed outputs; features cannot schedule siblings or inspect the mutable complete frame. |
| Execution safety | Callbacks do not allocate graph targets, select routes, mutate logical/resolved plans, commit transactions, publish capture, or read observations for correctness. |
| Rendering parity | Representative output, pass result, draw, fallback, debug, editor-assistance, present/offscreen, and resize behavior matches the Stage 0 baseline. |
| Transactionality | Invalid output, preparation/resource failure, graph failure, and incomplete final output abort state and suppress statistics; complete output commits once. |
| Diagnostics | Telemetry, GPU timing, regression budgets, capture, allocation observations, and warning cardinality remain observational and stable. |
| Maintainability | Central recorder/services/compose/contributor/parameter facades are absent and final accounting demonstrates that equivalent forwarding representations were not reintroduced. |

## Related Documentation

- [Renderer Frame Preparation](../Runtime/Rendering/RendererFramePreparation.md)
- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Persistent View State](../Runtime/Rendering/PersistentViewState.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Volumetric Cloud Spatial Rendering](../Runtime/Rendering/VolumetricCloudSpatialRendering.md)
- [RDG Compatibility Retirement and Allocator Simplification Plan](RDGCompatibilityRetirementAndAllocatorSimplification.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphExecutor.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphExecutor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPipeline.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPipeline.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPreparation.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphTypes.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphComposer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphComposer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphContributors.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderGraphParameters.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderFeatureRecorders.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderFeatureRecorders.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/AmbientOcclusionRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowVisibilityRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/BaseSceneRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneColorRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/EditorAssistanceRendering.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererEditorAssistanceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RDGTests.cpp`
- `Engine/Tests/Native/VulkanRHITests/Private/VulkanResourceTransitionTests.cpp`
