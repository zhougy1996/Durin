# Render Graph Scene Authoring Refactor Plan

Summary: Decompose scene-frame graph authoring into a thin executor, typed feature contributors, and an isolated retained-backing provider without changing graph behavior.

Last reviewed: 2026-08-24

Status: Active
Completed:

## Current Status

The Render Graph foundation and production scene migration are complete. The
renderer now builds one parent graph, resolves only retained logical resources,
and delegates dependency, culling, transition, and execution ordering to
RenderCore. Production and qualification evidence freezes the current scene
graph at 11 scheduled passes, 22--25 dependencies, and either 1 or 17 texture
transitions for the representative volumetric-cloud frames.

The remaining problem is code ownership rather than graph correctness.
`FRenderGraphSceneFrameExecutor::Execute_RenderThread` currently contains frame
transaction setup, route selection, logical resource declarations, retained
backing publication, all scene pass declarations, feature result plumbing,
graph compilation, execution, capture, and finalization. The executor header
also exposes one private execution adapter per feature and directly retains the
complete renderer service fan-out. This makes the sole graph authority easy to
inspect in one place, but makes feature-local changes expensive and encourages
new declarations to accumulate in the same function.

No implementation stage has started. This plan performs a behavior-preserving
decomposition: the parent graph, pass/resource identities, stable order,
dependencies, transitions, output, failure outcomes, telemetry, and current
single-queue execution model remain frozen while authoring ownership moves to
typed feature contributors.

## Goal

Make scene Render Graph code concise, locally understandable, and easy to
extend without weakening the single-parent-graph architecture:

- keep `FRenderGraphSceneFrameExecutor` as a thin owner of frame transaction,
  graph lifetime, compile/execute, and final commit or abort;
- make a scene composer show the frame pipeline as direct composition of
  feature contributions rather than resource declarations and command
  callbacks;
- colocate each feature's pass declarations, resource uses, immutable prepared
  inputs, typed outputs, and record callback beside that feature;
- isolate retained-target acquisition and logical-to-physical publication in
  one typed backing provider;
- replace implicit mutable result capture and boolean route combinations with
  explicit typed topology and result channels; and
- preserve exact graph behavior and qualification evidence throughout the
  refactor.

## Scope

- `Renderer` ownership and organization of production scene graph authoring.
- A typed `FSceneFrameTopology` selected before graph construction from the
  immutable scene plan, options, qualification policy, and prepared feature
  readiness.
- A narrow `FSceneFrameGraphComposer` that calls feature contributors in stable
  declaration order and returns typed final outputs.
- Feature-owned graph contribution functions for directional shadow, GBuffer,
  ambient occlusion, Contact Visibility, volumetric-cloud shadow, deferred
  lighting, opaque scene color, volumetric-cloud spatial/composite, sorted
  translucency, post process, and editor assistance.
- Feature-specific input/output structs containing graph handles and the
  smallest immutable prepared-data slices required to author and record their
  passes.
- A renderer-local retained-backing provider that translates typed allocation
  classes and retained graph requests into one complete-or-null transient
  target publication.
- Execution-lifetime typed result storage with declared single-writer and
  reader relationships for non-resource status or control values.
- Focused authoring, capture, failure-injection, integration, and qualification
  tests that freeze the existing graph and renderer behavior.
- Updates to lasting Runtime Rendering contracts after the new ownership is
  implemented.

## Non-Goals

- Changing Render Graph compilation, hazard, culling, transition, or pass-view
  semantics in `RenderCore` unless a separately demonstrated correctness defect
  blocks the refactor.
- Changing a feature algorithm, shader, permutation, PSO, material policy,
  render output, telemetry meaning, temporal policy, or fallback behavior.
- Introducing physical transient aliasing, async compute, multiple queues,
  pass merging, scheduling reordering, parallel recording, or persistent graph
  reuse.
- Introducing nested graphs, feature-local schedulers, manually restored
  inter-pass access, or a second production scene path.
- Building a public plugin API, runtime-polymorphic contributor registry,
  generic service locator, mutable blackboard, or feature dependency injection
  framework.
- Moving feature semantics into RenderCore or RHI.
- Renaming every historical `*Renderer` type. Types with real persistent
  resource, pipeline, preparation, or command-recording ownership may retain
  that name; only obsolete scheduling responsibility is removed.
- Reducing source line count as an acceptance criterion. Structural ownership,
  explicit dependencies, capture parity, and local testability are the gates.

## Design Decisions and Invariants

1. **One parent graph remains the only scene scheduler.** Contributors add
   passes to a caller-owned builder; they never compile, execute, or create a
   child graph. The composer is direct compile-time composition, not a dynamic
   registry.
2. **Executor owns lifecycle, not feature authoring.** It may prepare the frame,
   select topology, create the builder, invoke the composer, compile/execute,
   publish capture and telemetry, and commit or abort temporal/output state. It
   does not call `AddPass`, create feature transients, declare feature uses, or
   implement feature command callbacks.
3. **Feature contributors own coherent pass chains.** An `AddXxxPasses`
   function owns the logical resources created exclusively for that feature,
   every use declaration for its passes, and the bounded callback that invokes
   feature recording. Multi-pass features such as volumetric clouds remain one
   contributor with typed intermediate outputs; they do not become nested
   schedulers.
4. **Feature recorders retain rendering semantics.** Shader and route-specific
   parameter construction, PSO, viewport/scissor, draw/dispatch, and
   feature-local fallback binding remain in the feature renderer or recorder.
   Cross-feature order, transient lifetime, and inter-pass access do not.
5. **Inputs and outputs are typed and narrow.** Contributors receive explicit
   graph handles, dimensions, and immutable feature-prepared data. A callback
   does not capture the entire `FSceneRenderPlan`, composer context, executor,
   or an unbounded authoring scope when a smaller typed slice suffices.
6. **Topology is a valid state, not a boolean bag.** Mutually exclusive routes
   use enums or variants such as Disabled/Fragment/Compute. Route and fallback
   selection finishes before pass declaration; graph execution never selects a
   different rendering policy after compilation.
7. **Physical backing is an isolated atomic boundary.** Human-readable backing
   names remain diagnostics, but allocation policy uses renderer-owned typed
   classes. The provider publishes every retained handle or none, preserves
   existing pool ownership/recovery, and cannot expose undeclared targets to a
   callback.
8. **Non-resource results use declared typed channels.** Status and control
   values have graph-execution-lifetime storage, a single writer, and declared
   readers. Tokens remain only for dependencies not already represented by a
   resource or explicit effect; external mutable variables are not the hidden
   data path behind nominal token dependencies.
9. **The composer is a wiring diagram.** It may import frame-wide persistent
   resources and connect typed feature outputs to inputs. It does not contain
   shader logic, physical target-family field mapping, large feature-specific
   declaration blocks, or record callbacks. Repetition shared by two features
   stays local until a third use proves a stable helper contract.
10. **Behavior and diagnostics are frozen.** Existing pass/resource names,
    pass domains, explicit effects, stable declaration order, output access,
    failure mapping, telemetry, capture schema, and structural/CPU budgets do
    not change merely to simplify source organization. Any unavoidable change
    is recorded with before/after capture evidence before implementation
    continues.
11. **Renderer remains the ownership layer.** This plan may add private
    Renderer files and tests. It does not make scene feature types public or
    extend the generic RenderCore graph API for renderer-only convenience.
12. **Each extraction is independently shippable.** There is never a period in
    which both the executor and a contributor author the same pass or resource
    edge. Each stage removes the old declaration in the same change that adds
    its new owner.

## Current Foundations and Gaps

| Area | Foundation | Gap selected by this plan |
| --- | --- | --- |
| Graph authority | One `FRenderGraphBuilder`, one compile, and one execute own the production scene | The complete authoring body is concentrated in `Execute_RenderThread` |
| Prepared frame | `FSceneRenderPlan` provides immutable prepared feature data | Callbacks and adapters can still receive the complete plan instead of a bounded feature slice |
| Logical resources | Every inter-pass texture has one graph identity and declared uses | Creation and use declarations for all features live in the central executor |
| Feature execution | Existing renderer types own shaders, pipelines, preparation, and draw/dispatch code | Their graph-facing contracts are expressed as large executor adapters with long parameter lists |
| Route selection | Compute/fragment/disabled paths are selected before compile | Route state is distributed across requirements booleans, options, qualification flags, and local conditions |
| Retained backing | Compiler requests only retained logical resources; publication is atomic | One large resolver maps diagnostic strings, target families, handles, and physical textures inline |
| Typed outcomes | Status-only pass results and graph tokens no longer forward cross-pass physical pointers | Tokens often order access to callback-captured mutable result objects rather than owning the typed data path |
| Inspection | Capture freezes names, domains, dependencies, uses, transitions, and CPU observations | There are no small contributor-level authoring tests that make feature ownership easy to change locally |
| Validation | Full build, routine tests, Vulkan integration, and four renderer qualifications pass | Refactoring needs explicit parity gates after every extraction rather than one final big-bang comparison |

## Implementation Stages

### Stage 0: Freeze topology and select the ownership seams

- [ ] Capture representative disabled, invalid-input, compute, fragment,
  offscreen, present, resize, debug, and qualification frames before source
  movement; record pass/resource identity, domains, dependencies, uses,
  transitions, effects, retained requests, failure outcomes, and CPU budgets.
- [ ] Inventory every responsibility in `Execute_RenderThread`, every private
  `RenderXxx_RenderThread` adapter, every direct renderer dependency, and every
  cross-pass mutable result; assign each to lifecycle, topology, composer,
  backing provider, feature contributor, feature recorder, or finalization.
- [ ] Define `FSceneFrameTopology` with valid route enums/variants and prove
  each current option/qualification/readiness combination maps to the same
  authored passes and fallback behavior.
- [ ] Define naming and placement for composer, backing provider, shared graph
  inputs, execution-lifetime result channels, and each feature contributor.
- [ ] Add structural tests that reject nested compile/execute ownership and
  freeze the single parent graph's current declaration/capture contract.

#### Acceptance Gate

- Every current executor responsibility and mutable cross-pass value has one
  selected future owner; topology contains no invalid compute/fragment
  combinations; baseline captures and failure outcomes are checked into tests;
  no production behavior has changed.

### Stage 1: Extract topology and retained-backing infrastructure

- [ ] Introduce typed topology selection before graph construction and replace
  feature-route requirement booleans without changing authored graph variants.
- [ ] Move logical target descriptions and allocation classes into narrow scene
  graph resource types, keeping human-readable names only for diagnostics.
- [ ] Extract `FSceneFrameGraphBackingProvider` from the inline resolver; give
  it only the transient pool/target services and graph resource table required
  for complete-or-null retained publication.
- [ ] Preserve request-bounded acquisition: culled resources do not allocate,
  optional families are requested only when retained, and a failed requested
  family publishes no partial `FResolvedSceneFrameTargets`.
- [ ] Add focused tests for every allocation class, duplicate/unknown mapping,
  optional family omission, injected acquisition failure, resize/recovery, and
  atomic publication.
- [ ] Remove the old string-policy switch and target-field mapping from the
  executor after provider parity passes.

#### Acceptance Gate

- Executor code contains no transient target-family mapping or backing-class
  policy; all baseline retained requests, physical bindings, failure results,
  pool recovery, graph captures, and output fixtures match Stage 0.

### Stage 2: Move feature pass authoring to typed contributors

- [ ] Establish the contributor pattern with directional shadow and GBuffer:
  explicit immutable inputs, logical resources, pass declarations, bounded
  record callbacks, typed outputs, and contributor-level capture tests.
- [ ] Extract ambient occlusion, Contact Visibility, volumetric-cloud shadow,
  and deferred lighting while preserving compute/fragment/disabled topology,
  factor-one fallbacks, debug outputs, and production/qualification behavior.
- [ ] Extract opaque scene rendering, volumetric-cloud spatial/composite,
  sorted translucency, post process, and editor assistance while preserving
  attachment exits, present/offscreen behavior, and final-output effects.
- [ ] Move feature-local parameter construction and command recording behind
  each contributor's narrow recorder contract; remove corresponding
  `RenderXxx_RenderThread` adapters from the executor as their callers move.
- [ ] Ensure contributor callbacks capture only their immutable prepared slice,
  typed result writer, required service/recorder, and declared handles resolved
  through `FRenderGraphPassResources`.
- [ ] Run focused capture and feature fixtures after each contributor group so
  one extraction cannot hide a topology or failure change in a later batch.

#### Acceptance Gate

- Every scene pass is authored by exactly one feature contributor; contributors
  neither compile nor execute graphs; the executor contains no feature
  `AddPass`/`Use*` block or feature command callback; all Stage 0 graph and
  rendering evidence remains equivalent.

### Stage 3: Introduce the composer and explicit typed result flow

- [ ] Implement `FSceneFrameGraphComposer` as direct stable-order calls that
  connect typed contributor outputs to downstream typed inputs and return the
  final scene/output values required by frame finalization.
- [ ] Replace callback-captured mutable pass results with execution-lifetime
  typed result channels whose single writer and readers are paired with graph
  token declarations.
- [ ] Remove redundant control tokens where declared resource/effect edges
  already express the complete dependency; retain and name only genuine
  non-resource status/control dependencies.
- [ ] Keep shared persistent/default/environment imports unique per physical
  identity and pass them through explicit shared graph inputs without exposing
  an untyped resource blackboard.
- [ ] Add composer tests for feature omission, route variants, stable ordering,
  dependency minimality, culling, result propagation, and failure short-circuit
  behavior.
- [ ] Confirm the composer contains no feature draw/dispatch logic, physical
  target-field mapping, graph compile/execute, or frame commit/abort behavior.

#### Acceptance Gate

- The composer reads as a bounded frame pipeline of typed contributor calls;
  no pass communicates through an undeclared external mutable result; capture
  dependencies remain minimal and behaviorally equivalent to Stage 0.

### Stage 4: Reduce the executor to frame lifecycle ownership

- [ ] Move frame transaction initialization, temporal begin/commit/abort,
  telemetry publication, graph capture publication, and error translation into
  small lifecycle helpers with explicit ownership and ordering.
- [ ] Reduce `Execute_RenderThread` to output validation, preparation, topology
  selection, graph/composer invocation, compile/execute, and finalization.
- [ ] Replace the executor's direct feature renderer fan-out with the smallest
  explicit dependency aggregates owned by composer/contributors; do not add a
  service locator or runtime registry.
- [ ] Remove obsolete requirements, resolved-target containers, adapters,
  helpers, includes, and compatibility paths once no production or test caller
  uses them.
- [ ] Add structural ownership tests or targeted source assertions that keep
  `AddPass`, feature transient creation/use declarations, backing resolution,
  and feature command recording out of the executor.
- [ ] Review names and file boundaries so a new rendering feature has one
  obvious contributor location, typed integration point, and focused test
  target.

#### Acceptance Gate

- `FRenderGraphSceneFrameExecutor` owns only frame and graph lifecycle; adding a
  feature does not require adding a private render adapter or a feature-specific
  pass/resource block to it; there is one production authoring path and no
  compatibility scheduler.

### Stage 5: Qualify architecture, behavior, and cost

- [ ] Pass focused topology, backing-provider, contributor, composer,
  RenderCore graph, RHI transition, transient-pool, and Renderer failure tests.
- [ ] Pass scene image/readback, contact compute/fragment/factor-one, GBuffer,
  deferred lighting, volumetric cloud, editor assistance, present/offscreen,
  resize, multi-view, duplicate-submission, recovery, and shutdown fixtures.
- [ ] Compare Stage 0 and final graph captures for pass/resource identity,
  domain, scheduled order, dependencies, uses, effects, retained requests,
  transition plans, compile/execute CPU observations, and diagnostics.
- [ ] Run the repository's focused Renderer Vulkan integration and Directional
  Shadow, GBuffer, HDR display, and Volumetric Cloud qualification workflows;
  preserve truthful named-device timing gates.
- [ ] Pass the complete repository build, routine native-test set, and required
  changed/all documentation, plan, and roadmap validators.
- [ ] Move lasting contributor, topology, backing, result-flow, and extension
  rules into Runtime Rendering documentation and record final evidence here.

#### Acceptance Gate

- All behavior, failure, diagnostic, structural, and cost gates pass; the
  executor/composer/contributor/backing-provider ownership rules are documented
  as the required path for future scene features.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Ownership | Source/structural tests prove one parent builder/compile/execute, feature-owned declarations, isolated backing, and no executor feature adapters |
| Topology | Disabled, production, debug, qualification, compute, fragment, factor-one, missing-input, present, and offscreen variants author the expected pass set and domains |
| Graph parity | Before/after captures compare names, resources, uses, effects, stable order, dependencies, retained requests, transitions, and budgets |
| Typed isolation | Contributor unit tests compile from narrow inputs; callbacks cannot discover undeclared resources or the complete frame plan; result channels enforce declared single-writer/readers |
| Allocation and failure | Retained-only acquisition, atomic publication, injected family failure, pool exhaustion, retry, resize, recovery, and complete abort preserve current outcomes |
| Rendering behavior | Existing images/readbacks, draw/dispatch identity, telemetry, temporal commit/abort, output access, viewport/scissor, and feature fallback fixtures remain equivalent |
| Backend integration | Focused RHI/Vulkan transition and renderer integration sets pass with validation enabled |
| Qualification | Directional Shadow, GBuffer, HDR display, and Volumetric Cloud qualification targets pass; named hardware gates remain adapter-truthful |
| Cost | Structural budgets remain at or below 12 declared passes, 28 dependencies, and 20 texture transitions; debug compile and complete recording remain below 5 ms and 250 ms observation ceilings |
| Repository | Complete configured build, routine native tests, and required documentation lifecycle validators pass |

## Definition of Done

- `FRenderGraphSceneFrameExecutor::Execute_RenderThread` contains no
  feature-specific pass/resource authoring or physical target-family binding.
- `FRenderGraphSceneFrameExecutor` has no private `RenderXxx_RenderThread`
  adapter per scene feature and no direct ownership fan-out whose only purpose
  is feature graph authoring.
- One `FSceneFrameGraphComposer` expresses the frame pipeline through direct,
  typed contributor calls without compiling/executing a graph or recording GPU
  commands.
- Every production scene pass and exclusive transient resource has exactly one
  feature contributor owner with explicit inputs and outputs.
- Route topology cannot represent mutually contradictory feature paths and is
  fixed before graph construction.
- Retained backing acquisition/publication is isolated, typed,
  request-bounded, complete-or-null, and covered by failure/recovery tests.
- No pass depends on an undeclared external mutable result; non-resource
  control/status flow uses explicit execution-lifetime typed channels.
- No nested graph, manual migrated transition, mutable blackboard, dynamic
  feature registry, compatibility scheduler, or second production path exists.
- Stage 0 and final graph captures and rendering fixtures are equivalent except
  for an explicitly reviewed diagnostic-only difference.
- Full build, routine tests, focused integration, renderer qualifications, and
  documentation validators pass.
- Lasting architecture and feature-extension rules are authoritative in
  Runtime Rendering documentation; this plan records final evidence and is
  marked completed.

## Deferred Follow-ups

- Physical transient aliasing and placed-resource reuse remain evidence-gated
  by measured memory benefit and RHI/Vulkan lifetime support.
- Async compute and multi-queue scheduling remain evidence-gated by independent
  queue capability and overlap measurements.
- Pass merging, scheduling reordering, parallel recording, and persistent graph
  reuse require separate bottleneck evidence and plans.
- A public extension/plugin model is deferred until an actual external feature
  owner cannot use the private typed contributor pattern.
- A generic RenderCore typed-value facility is deferred unless at least one
  non-Renderer graph demonstrates the same requirement; this plan may use a
  private Renderer execution-result abstraction.
- Broad renaming or decomposition of feature renderer classes unrelated to
  removing scheduling responsibility belongs to feature-local maintenance.

## Related Documentation

- [Render Graph](../Runtime/Rendering/RenderGraph.md)
- [Renderer Frame Preparation and Render Graph Execution](../Runtime/Rendering/RendererFramePreparation.md)
- [Render Graph Architecture Roadmap](../Roadmaps/RenderGraphArchitecture.md)
- [Render Graph Foundation Consolidation Plan](RenderGraphFoundationConsolidation.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderGraphSceneFrameExecutor.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RenderGraphSceneFrameExecutor.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPlan.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/RendererTransientTargetPool.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderTelemetry.h`
- `Engine/Source/Runtime/RenderCore/Public/RenderGraph.h`
- `Engine/Source/Runtime/RenderCore/Private/RenderGraph.cpp`
- `Engine/Tests/Native/RenderCoreTests/Private/RenderGraphTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/HDRDisplayMappingQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudQualificationTests.cpp`
