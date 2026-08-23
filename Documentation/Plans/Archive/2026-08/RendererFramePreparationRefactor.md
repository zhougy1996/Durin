# Renderer Frame Preparation Refactor Plan

Summary: Separate immutable per-view preparation, typed pass outcomes, telemetry, and transient-resource ownership so the fixed renderer is ready for a bounded Render Graph follow-up.

Last reviewed: 2026-08-23

Status: Archived
Completed: 2026-08-24

## Current Status

The Renderer still executes one view through a fixed, explicit render-thread
sequence. That ordering is correct and remains the production authority, but
its frame-local data boundary has accumulated unrelated responsibilities.
`FPreparedSceneView` now contains the fitted view, temporal state, visibility
and geometry results, directional-shadow preparation, lighting uploads,
environment and volumetric-cloud inputs, execution-mutated resource state, and
the complete private counter aggregate. Most `FSceneRenderer` pass helpers
receive that value by mutable reference.

The same logical draw values are mutated from `Prepared` through
`ResourcesPrepared` to `Executed`; resource availability and directional-
shadow bindings are written back into draw records; pass completeness can be
derived from counters; and profiling qualification can mutate the whole
prepared view through a global sink. `SceneRenderer.cpp` consequently owns
preparation, feature policy, target acquisition, pass ordering, transitions,
fallback selection, temporal commit, and statistics reduction in one 2,605-line
implementation.

The lower layers already provide the prerequisites this refactor must preserve:
recorded RHI command lists, explicit buffer/texture access transitions,
resource views, complete-or-null resource publication, GPU-completion-aware
retirement, render-thread scene snapshots, and transactional persistent view
state. The selected path therefore restructures Renderer without changing the
RHI executor or introducing a Render Graph in this plan.

All stages are complete. The renderer now owns one command-local
`FSceneRenderPlan` with view, optional environment, lighting, receiver,
optional directional-shadow, and optional cloud partitions. Logical geometry
is immutable after preparation; material, pipeline, geometry, palette,
terrain, and shadow resources are recorded in family-specific resolved values.
GBuffer completeness and rendered-geometry state come from typed pass results
rather than telemetry, and compile-time tests exclude readiness, phase, target,
binding, and execution fields from logical draw/view types. Feature sources are
reduced once by `SceneRenderTelemetry`; registration lives in the profiling
owner, rendering policy does not read telemetry, and aborted views publish no
counter snapshot or public statistics. A Renderer-private transient target
pool now owns Scene, GBuffer, GTAO, contact/cloud visibility, isolated
deferred/debug, and cloud spatial/composite textures with full-description
keys, bounded retention, generation retry, and deterministic release. Feature
pass execution consumes resolved targets without owning physical extent
caches. Frame preparation, the readable fixed pass sequence, telemetry, and
final output selection now have explicit private owners; `SceneRenderer.cpp`
contains renderer composition, lifetime/invalidation, and the narrow frame
entry rather than feature orchestration. Focused and aggregate Renderer/RHI/
Vulkan validation, inline and threaded execution, qualification images and
measurements, the full build and test suite, Editor smoke, resource recovery,
and documentation validation pass. The GBuffer performance qualification was
rerun in a quiet lane after two noisy samples and passed without changing its
frozen thresholds.

## Goal

Replace the mutable all-purpose prepared-view handoff with an immutable,
feature-bounded frame preparation model; separate logical draw selection from
GPU resource resolution and command recording; make pass success and outputs
typed rather than counter-derived; isolate telemetry from correctness state;
and expose every pass's exact resource inputs and outputs while preserving the
current fixed execution order.

The completed Renderer must provide a narrow seam for a later Render Graph
plan to register the same passes and resources without redesigning scene
preparation, feature ownership, temporal transactions, failure policy, or
public rendering behavior again.

## Scope

- Renderer-private frame and feature preparation types currently aggregated by
  `FPreparedSceneView`.
- Receiver-view StaticMesh, SplineMesh, SkeletalMesh, and Terrain logical draw
  preparation, shared Skeletal palette preparation, combined translucency
  ordering, and directional-shadow cascade draw preparation.
- Separation of immutable logical preparation from fallible shader, pipeline,
  material-binding, geometry, upload, and target resolution.
- Typed pass input, output, status, fallback, and completeness values for
  GBuffer, deferred lighting, GTAO, contact shadows, volumetric-cloud shadows,
  volumetric clouds, Scene Color, post process, and editor assistance.
- Feature-local preparation and execution telemetry plus one final reduction
  into the stable `FSceneViewStatistics` publication contract.
- Classification and ownership of imported, persistent/history, and
  frame-transient textures and buffers used by the scene renderer.
- A Renderer-owned transient-target provider that preserves current physical
  caching and recovery behavior while removing target ownership from feature
  pass executors.
- Decomposition of `FSceneRenderer` into explicit preparation, fixed-pass
  execution, telemetry, and finalization owners without a public renderer or
  pass registry.
- Focused, aggregate, image, transition, failure/recovery, multi-view,
  performance, full-build, runtime-smoke, and documentation qualification.
- Publication of the implemented frame-preparation and resource-ownership
  contract under `Documentation/Runtime/Rendering/`.

## Non-Goals

- Implementing `FRDGBuilder`, graph dependency compilation, pass culling,
  automatic barrier generation, transient aliasing, render-pass merging, or a
  graph visualization/debugger.
- Adding asynchronous compute, multiple GPU queues, parallel command recording,
  task-parallel scene preparation, or another command executor.
- Changing visible rendering algorithms, pass order, GBuffer layout, shading,
  shadow fitting/filtering, GTAO, cloud sampling, temporal reconstruction,
  post process, editor assistance, or final presentation policy.
- Adding GPU-driven visibility, indirect draws, meshlets, a persistent mesh
  draw-command cache, bindless resources, local-light shadows, or clustered
  lighting.
- Introducing a public/runtime-polymorphic feature renderer registry or moving
  Renderer feature policy into RenderCore or RHI.
- Centralizing PSOs merely because pass boundaries become narrower. The open
  PSO-cache investigation remains evidence-gated at the later RDG boundary.
- Changing `FScene`, SceneInfo/SceneProxy publication, component render-state
  lifetime, material representation, asset residency, or viewport submission
  contracts.
- Aliasing physical target memory in the new transient-target provider. The
  initial provider preserves non-overlapping semantic ownership and existing
  GPU-safety assumptions; aliasing requires its own measured RDG work.

## Design Decisions and Invariants

### Frame plan ownership is broad; pass access is narrow

- One command-local `FSceneRenderPlan`-equivalent value may own the complete
  prepared frame, but only `FSceneRenderer` orchestration may observe it as a
  whole. Feature preparation and execution APIs receive exact typed subvalues
  as `const` inputs plus explicit output/result values.
- The frame plan contains no reflected/game-thread object and never outlives
  the render command's existing SceneInfo/proxy lifetime boundary.
- The selected semantic partitions are fitted view/temporal metadata,
  environment, lighting, receiver geometry, directional shadow, and optional
  volumetric cloud. Exact type names are frozen in Stage 0 before Stage 1 edits.
- Optional features use one optional/variant value with a complete validity
  contract rather than independent pointer and Boolean combinations.
- Shared Skeletal palette data remains submission-local and is shared by the
  receiver and shadow cascades without creating duplicate uploads.

### Logical preparation is immutable after publication

- Visibility, primitive facts, LOD selection, sections/patches, material
  identity, pipeline keys, sort keys, cascade membership, and translucent order
  form immutable logical preparation.
- Logical draw records do not contain `bResourcesReady`, execution phases,
  pass counters, target pointers, or per-pass directional-shadow bindings.
- Resource preparation returns a separate resolved execution value containing
  accepted logical draw indices/references and the exact fallible GPU bindings
  needed by execution. It never writes readiness back into logical records.
- Execution consumes logical and resolved values as `const`; exactly-once
  ordering is owned by the fixed frame executor and typed results rather than
  three duplicated geometry-family phase enums.
- Directional-shadow textures/samplers, lighting uniforms, GBuffer inputs, and
  other pass products are explicit pass parameters, not fields copied into
  every prepared draw.

### Pass results own correctness; telemetry only observes it

- Every fallible pass returns a feature-specific result containing status,
  reason/fallback, output resources, completeness, and execution measurements.
- GBuffer completeness is a typed result established by draw execution. It is
  never inferred from attempted/successful/rejected telemetry counts.
- Requested policy such as cloud quality, debug mode, and route preference
  remains in immutable view/options or feature preparation. No later decision
  reads a value back from counters.
- Feature-local telemetry is reduced once after command recording. Public
  `FSceneViewStatistics` meanings, latest-only publication, and the rule that a
  failed view publishes no partial statistics remain unchanged.
- Timing/capture/qualification observers receive immutable typed pass data or
  pass results. No development callback can mutate a complete frame plan.

### Resource lifetime classes are explicit

- Imported resources are externally owned output, asset textures, default
  textures, environment resources, and other resources whose lifetime is
  established outside the frame. Their required initial and final access stay
  explicit at the pass boundary.
- Persistent resources are shader maps, PSOs, samplers, geometry/topology
  caches, renderer-generation state, and committed temporal history. Feature
  renderers and view state retain their existing ownership unless a specific
  stage records a narrower owner.
- Frame-transient resources are Scene Color/depth, GBuffer attachments, GTAO
  intermediates, contact/cloud visibility, isolated deferred/debug outputs,
  and cloud spatial/composite intermediates that have no cross-frame semantic
  identity.
- `FRendererTransientTargetPool`-equivalent ownership is feature-neutral and
  Renderer-private. Frame setup acquires typed leases from complete texture
  descriptions; pass execution receives those leases and does not call
  `EnsureTargets_RenderThread`.
- The provider preserves current extent keys, byte budgets, complete-or-null
  publication, last-known-good/retry behavior where applicable, invalidation,
  release, RHI reference retention, and shutdown audit. It does not infer pass
  scheduling or perform physical aliasing.

### Fixed orchestration remains the production scheduler

- `FSceneRenderer` remains the owner of renderer composition, feature
  renderers, view-state registry, resource invalidation fan-out, and exact
  per-view ordering.
- `RenderView_RenderThread` is reduced to validation, temporal transaction,
  frame preparation, transient acquisition, fixed pass execution, telemetry
  publication, and commit/abort. Feature-specific preparation and pass bodies
  live with their semantic owners.
- Pass resource input/output structures use backend-neutral RHI resources and
  access intent. They do not expose Vulkan state or introduce a second resource
  state tracker.
- Manual transitions remain authoritative in this plan and must continue to
  match the public RHI access contract. The later graph may synthesize the same
  transitions; it may not replace their semantics.

### Temporal, failure, and output transactions do not weaken

- `FSceneViewState::Begin()` still precedes temporal consumers, and only a
  completely successful view commits metadata and feature history. Every early
  return aborts pending state through one owning scope.
- Duplicate, missing, released, or foreign view state retains its current
  discontinuity and diagnostic behavior.
- Required environment failure remains view-fatal. Optional feature target,
  pipeline, compute-route, or history failure retains its documented fallback
  and does not publish a partially valid output.
- Window-backed output finishes in `Present`; offscreen output finishes in
  `ShaderReadOnly`. Fixed-aspect bars, viewport/scissor restoration, editor
  assistance ordering, and main/auxiliary view isolation remain exact.

### RDG readiness has a bounded definition

- The follow-up graph must be able to wrap each fixed pass using its immutable
  preparation, typed resource inputs/outputs, and execution callback without
  reopening the scene-preparation or feature-result design.
- RDG readiness does not require graph-shaped placeholder abstractions in this
  plan. No generic node, blackboard, pass registry, or dependency solver is
  introduced merely to resemble a future graph.
- Stage 6 records the final pass/resource/lifetime inventory and measured
  transition/target evidence. A separate `RenderGraphAndTransientResources`
  plan is selected only after that evidence defines its first consumers and
  acceptance gates.

## Current Foundations and Gaps

| Boundary | Current foundation | Gap selected by this plan |
| --- | --- | --- |
| Scene submission | Immutable `FSceneView` policy and render-thread-only `FScene`/SceneInfo snapshots | Frame preparation mixes unrelated features and mutable execution state in `FPreparedSceneView`. |
| Visibility and geometry | Per-view classification, family-specific logical draws, stable sort keys, cascade-local LOD, shared Skeletal palettes | Logical draws also own readiness, pass bindings, phase, and execution counters. |
| Pass composition | One explicit `FSceneRenderer` preserves complete view order and fallback behavior | Most helpers receive broad mutable state; preparation, execution, results, and telemetry are not distinct. |
| Resource access | RHI exposes validated buffer/texture ranges, views, access states, transitions, and recorded ownership | Resource requirements are encoded inside pass bodies and target owners rather than typed at orchestration boundaries. |
| Intermediate targets | Feature owners use bounded extent-keyed complete-or-null target caches | Lifetime overlap and aggregate retention are not observable from one frame owner; execution methods acquire their own outputs. |
| Persistent resources | Feature-owned shaders, PSOs, samplers, caches, retry slots, and explicit coordinator invalidation | Persistent and transient ownership are presented through the same renderer state objects. |
| Temporal history | Transactional Begin/Commit/Abort and feature-local cloud history | Raw view-state access and temporal inputs travel through the all-purpose prepared view. |
| Statistics and diagnostics | Bounded private counters, public statistics reduction, GPU queries, capture seams | Counters can drive correctness decisions and one qualification sink can mutate prepared state. |
| Testing | Renderer scene, GBuffer, shadow, cloud, viewport, RHI, Vulkan, resource-recovery, and image qualification already exist | Structural ownership, constness, typed-result conservation, and target-provider failure need focused coverage. |

## Stage 0 Frozen Baseline

### Prepared-view ownership map

| Existing fields | Frozen destination | Writer and lifetime | Failure contract |
| --- | --- | --- | --- |
| `View`, `TemporalContext`, `ViewState` | `FPreparedViewContext` | Fixed frame preparation; command-local, with borrowed registry state | Duplicate/missing state records a discontinuity; only a successful view commits. |
| `SkyBox`, `bHasSkyBox`, `ViewEnvironmentTexture`, `bHasViewEnvironment` | Optional `FPreparedEnvironment` | Environment override or render-thread scene snapshot | An invalid required override is view-fatal; an absent scene sky is valid. |
| `Lights`, `LightingUniformBuffer` | `FPreparedLighting` | Visibility/light preparation and one dynamic uniform upload | A missing upload is view-fatal; asset IBL may use the complete default set. |
| `StaticMeshes`, `SkeletalMeshes`, `Terrains`, `TranslucentGeometry` | `FPreparedReceiverGeometry` | Receiver logical preparation followed by separate resolution | Empty geometry is valid; combined translucency is distance-descending, then complete stable key/family order. |
| `SkeletalPalettes` | `FPreparedSkeletalPaletteTable` owned by receiver geometry | One submission-local table shared by receiver and every shadow cascade | Individual rejected palettes reject their dependent draws without duplicating uploads. |
| `DirectionalShadow`, caster table, and three cascade geometry arrays | Optional `FPreparedDirectionalShadow` | Selected directional light, caster classification, and cascade preparation | Invalid receiver or target disables shadow contribution and retains the documented fallback. |
| Cloud requested/force flag, parameters, textures, history key, shadow visibility | Optional `FPreparedVolumetricCloud`, immutable qualification input, and typed shadow result | Scene snapshot or development fixture; command-local except committed view-state history | Missing optional inputs disable the feature; compute failure may select fragment; no partial output is published. |
| `Counters` | `FSceneRenderTelemetry` with feature-owned sources and one final reducer | Preparation/resolution/execution observations during one command | Rendering policy and success may not read telemetry; failed views publish no partial public statistics. |

The outer frozen type name is `FSceneRenderPlan`. Only fixed orchestration may
observe it. The selected private files are `SceneRenderPlan.h` for immutable
partitions, `SceneRenderResults.h` for pass status/output values,
`SceneRenderTelemetry.{h,cpp}` for observation and reduction,
`RendererTransientTargetPool.{h,cpp}` for frame leases, and
`FixedSceneFrameExecutor.{h,cpp}` for the explicit scheduler. Feature-specific
resolved draw types remain beside their owning mesh renderer.

### Fixed pass and access order

The production schedule is frozen as: validate output/resources; acquire Scene
Color/depth; begin temporal state; prepare view/environment/visibility/lights,
receiver and shadow logical draws; resolve geometry and shadow resources;
render directional-shadow cascades; acquire and render GBuffer when requested;
prepare deferred inputs; run GTAO, contact visibility, and cloud-shadow
visibility; run optional isolated deferred/debug branches; bootstrap Scene
Color; production deferred lighting; retained unlit opaque/masked geometry;
transition depth to shader read; render/reconstruct/composite clouds; transition
Scene Color to attachment write; render combined sorted translucency; select
debug or Scene Color output; post process; editor assistance; restore output
viewport/scissor; transition the external output to `Present` or
`ShaderReadOnly`; then publish telemetry and commit temporal state.

Qualification paths reuse the same GBuffer/deferred/GTAO inputs and occur
before production Scene Color. The manual transition authority remains the RHI
command list. Imported output starts in its caller-provided state and finishes
in `Present` for window-backed views or `ShaderReadOnly` for offscreen views.
Any early return leaves the temporal scope to abort and does not publish a
partial successful statistic.

### Resource classes and budgets

| Class | Frozen resources and owner | Key, retry, and release |
| --- | --- | --- |
| Imported | External output; asset/default/environment textures; material geometry and buffers | External or persistent owner defines lifetime; pass inputs declare required access and fallback. |
| Persistent/history | Shader maps, PSOs, samplers, geometry/topology caches, renderer-generation state, dynamic allocator backing, and committed cloud history | Existing feature/coordinator generation and GPU-safe retirement remain authoritative; view-state removal/invalidation releases history. |
| Frame-transient | Scene Color/depth; GBuffer material/normals/surface/emissive; GTAO raw/filter/resolve; contact and cloud-shadow visibility; isolated deferred/debug color; cloud spatial/composite targets | Full texture description plus extent is the key. Current complete-or-null caches and last-known-good retry behavior define the replacement budget; coordinator invalidation and renderer shutdown release every retained reference. |

At 1920x1080, the frozen qualification reports 33,177,600 GBuffer bytes,
16,588,800 isolated deferred bytes, 4,147,200 GTAO active bytes, 69,984,000
hybrid active bytes, 120,315,648 active bytes with shadow, and a 268,435,456
retained ceiling. The centralized provider may not retain more than the summed
replaced-owner ceiling and performs no aliasing in this plan.

### Correctness and performance references

The configured baseline is Windows MSVC x64, Debug `DurinEditor`, Vulkan on an
NVIDIA GeForce RTX 3090 (driver 591.86, Vulkan 1.4.325), validation enabled.
Focused results on 2026-08-23 were `EditorRenderingTests` 77/77,
`RendererSceneContractTests` 27/27, `VolumetricCloudSceneContractTests` 6/6,
and `RHIResourceTransitionValidationTests` 7/7. `GBufferQualificationTests`
passed at 1920x1080 with 30 warm-up frames and 120 measured frames.

Frozen GPU medians/p95 values in nanoseconds are: GBuffer 71,904/72,864,
Scene Color 255,104/257,504, production deferred 223,872/224,896, retained
opaque 7,120/7,744, sorted translucency 7,344/8,096, FXAA 61,328/62,176,
directional shadow 10,816/11,968, contact compute 320,528/323,040, and total
399,024/402,784. Full-resolution GTAO feature is 690,016/1,128,448 and
half-resolution GTAO is 402,160/865,088. The reference run was performed
without a competing build/test process; browser/desktop GPU exclusivity was
not externally enforceable, so the values are acceptance references only when
rerun in an equally quiet lane.

The comparison gates remain median render-thread regression <=5%, p95 <=10%,
unchanged GPU-pass median regression <=5%, identical image/draw/dispatch and
temporal outcomes, and retained bytes no greater than the frozen replacement
budget.

## Implementation Stages

### Stage 0: Freeze the frame, ownership, and measurement baseline

- [x] Inventory every `FPreparedSceneView` field, writer, reader, lifetime,
  failure behavior, and selected destination partition. Include the development
  cloud preparation seam and every whole-view API.
- [x] Record the exact production and qualification pass sequence, each pass's
  resource reads/writes, manual access transitions, target acquisition, output
  fallback, viewport/scissor contract, and final external-resource state.
- [x] Classify every renderer target and upload as imported, persistent/history,
  or frame-transient. Record description, active bytes, retained budget, extent
  key, creation/retry owner, invalidation generation, and release path.
- [x] Freeze the semantic type/file layout for fitted view/temporal context,
  environment, lighting, receiver geometry, directional shadow, volumetric
  cloud, resolved execution resources, pass results, and telemetry. Resolve any
  remaining naming or shared-palette ownership question here.
- [x] Add or select focused structural tests for const logical preparation,
  combined translucency, shadow cascade membership, resource-result
  conservation, GBuffer completeness, temporal abort/commit, and immutable
  diagnostics observation before deleting old seams.
- [x] Capture the existing focused/aggregate test baseline, representative
  inline/threaded rendered images, draw/dispatch identities, pass GPU timings,
  render-thread median/p95, active/retained target bytes, resize churn, and
  resource-failure/retry behavior following the repository workflows.
- [x] Freeze the quiet-lane measurement host, scene fixtures, resolutions,
  warm-up/sample policy, and comparison gates. Unless Stage 0 evidence selects
  a stricter bound, median render-thread time may regress by at most 5%, p95 by
  at most 10%, individual unchanged GPU pass medians by at most 5%, and retained
  target bytes may not exceed the summed replaced owners' baseline budget.

#### Acceptance Gate

- Every mutable field and resource has one selected owner, every pass has an
  explicit current resource contract, all unresolved design questions are
  closed, and correctness/performance/memory baselines are reproducible before
  structural implementation begins.

### Stage 1: Publish immutable feature-bounded frame preparation

- [x] Introduce the selected command-local frame-plan partitions and builders
  without changing visibility, LOD, sort, shadow, light, environment, cloud, or
  temporal policy.
- [x] Move receiver geometry, directional-shadow cascade preparation,
  environment, lighting, and volumetric-cloud inputs into their owning values;
  retain one submission-local Skeletal palette owner shared by all required
  views.
- [x] Change preparation and policy consumers to accept only the exact typed
  inputs they require. Only orchestration may receive the outer frame plan.
- [x] Represent optional environment and cloud inputs as complete optional
  values and remove independent requested/valid pointer-Boolean combinations.
- [x] Replace the mutable volumetric-cloud preparation sink with a typed scene
  fixture or immutable feature-input seam that cannot modify unrelated frame
  state.
- [x] Retire `FPreparedSceneView` and its include fan-out after all consumers
  use the new boundaries; update focused preparation tests to construct only
  the smallest owning value.

#### Acceptance Gate

- No feature API accepts the retired whole prepared view; the frame partitions
  contain only their selected semantic data; existing preparation, visibility,
  shadow, cloud-scene, and view tests pass with identical logical outcomes.

### Stage 2: Separate logical draws from resolved execution resources

- [x] Remove readiness, execution phase, directional-shadow target/sampler,
  and execution counters from StaticMesh, SkeletalMesh, and Terrain logical
  draw/view values.
- [x] Introduce family/pass-specific resolved execution values that reference
  immutable logical draws and own fallible material, pipeline, geometry,
  dynamic-upload, palette, height/topology, and batching results.
- [x] Make resource preparation return complete resolved values or typed
  failure/fallback results rather than partially mutating a logical view.
- [x] Pass lighting, directional-shadow, GBuffer, depth, and other pass products
  explicitly into draw execution/material binding instead of copying them into
  each draw.
- [x] Make all geometry execution paths consume logical and resolved data as
  `const`; move exactly-once sequencing to the frame executor and remove the
  three prepared/resources-prepared/executed phase enums.
- [x] Introduce typed results for GBuffer and geometry execution. Establish
  completeness directly from execution outcomes and test conservation against
  attempted, successful, rejected, skipped, batched, section/patch, triangle,
  and hardware-draw measurements.

#### Acceptance Gate

- Logical preparation cannot be mutated by resource resolution or execution;
  no correctness branch reads geometry counters; resource failures preserve
  current fallbacks; geometry, shadow, GBuffer, image, draw, and triangle
  references remain exact.

### Stage 3: Isolate feature telemetry and diagnostics

- [x] Split the flat private counter aggregate into feature-owned preparation,
  resource, execution, memory, and timing telemetry values with explicit
  conservation equations.
- [x] Return or record telemetry beside typed pass results without allowing a
  pass to read it as policy or success state.
- [x] Reduce feature telemetry exactly once after command recording into the
  existing public `FSceneViewStatistics`; remove repeated counter-copy passes
  and preserve saturated arithmetic and unavailable-view semantics.
- [x] Replace pass-specific global mutation/capture seams with immutable
  observers over named pass results/resources. Retain nonblocking GPU timing
  query ownership and the current development-only registration lifetime.
- [x] Move statistics reduction, profiling scope, sink registration, and
  capture adaptation out of `SceneRenderer.cpp` into explicit diagnostics
  owners.
- [x] Add tests proving failed views publish no partial statistics, observers
  cannot mutate frame data, route/quality policy does not originate in
  telemetry, and public statistics remain identical for successful views.

#### Acceptance Gate

- Telemetry is write-only from the perspective of rendering decisions, every
  public statistic has one feature-owned source and one final reduction, and
  timing/capture qualification passes without a mutable frame-wide callback.

### Stage 4: Establish frame-transient target ownership

- [x] Introduce the selected Renderer-private transient-target provider with
  complete texture descriptions, typed leases, extent/budget accounting,
  generation-aware retry, and deterministic release.
- [x] Move Scene Color/depth, GBuffer, GTAO, contact/cloud visibility, isolated
  deferred/debug, and cloud spatial/composite target acquisition from feature
  pass executors to frame setup. Preserve committed cloud history under view
  state rather than treating it as transient.
- [x] Keep shaders, PSOs, samplers, material/geometry caches, default textures,
  environment resources, and other persistent payloads with their existing
  feature/shared owners; update coordinator invalidation fan-out explicitly.
- [x] Give every pass a typed input/output resource structure. Execution must
  not create or look up its output target, and it must declare the expected
  imported/persistent resources needed for fallback.
- [x] Preserve current manual access transitions and exact render-pass
  attachment load/store/initial/final contracts while centralizing target
  acquisition; add range/access validation for every typed resource boundary.
- [x] Cover extent reuse/eviction, multi-view interleaving, resize, allocation
  failure, retry, shader/device invalidation, RHI reference retention, release,
  and shutdown with inline and threaded executors.

#### Acceptance Gate

- Feature pass execution owns no frame-transient target cache or acquisition;
  active and retained bytes reconcile through the provider; output, fallback,
  transitions, recovery, multi-view isolation, and shutdown match baseline;
  no physical aliasing or new synchronization is introduced.

### Stage 5: Decompose and narrow fixed frame orchestration

- [x] Separate frame preparation, fixed pass execution, telemetry/finalization,
  and `FSceneRenderer` composition into matching private types/files. Keep
  feature shader/pipeline/render bodies in their existing feature renderers.
- [x] Reduce `RenderView_RenderThread` to output validation, temporal scope,
  preparation, transient acquisition, fixed execution, telemetry publication,
  and commit/abort; remove feature-specific counter copying and target creation
  from that function.
- [x] Express the fixed sequence through typed pass calls whose signatures show
  exact preparation and resource dependencies. Do not add a generic graph node,
  blackboard, runtime registry, or callback scheduler.
- [x] Preserve qualification-only paths as explicit optional branches over the
  same typed GBuffer/deferred/GTAO resources rather than a second preparation or
  execution architecture.
- [x] Centralize final Scene Color selection, debug-output selection,
  post-process input, editor-assistance demand, and Present/ShaderReadOnly
  finalization so every successful route reaches one audited output boundary.
- [x] Add structural tests or compile-time assertions preventing whole-plan
  feature access, mutable logical execution inputs, telemetry-driven policy,
  and pass-local transient acquisition from returning.

#### Acceptance Gate

- One readable fixed scheduler exposes complete pass order and typed
  dependencies; feature owners remain discoverable; `FSceneRenderer` retains
  composition and transaction authority without containing feature
  implementation; all qualification and production routes share the same
  preparation/resource/result model.

### Stage 6: Integrate, qualify, document, and hand off RDG readiness

- [x] Run focused Renderer preparation, visibility, material, geometry,
  shadow, GBuffer, deferred, GTAO, cloud, temporal, viewport, statistics,
  resource-recovery, RHI-transition, and Vulkan tests following the agent
  testing workflow.
- [x] Run the affected aggregate, required Renderer/RHI/Vulkan builds,
  inline/threaded runtime matrices, validation layers, resize/multi-view cases,
  full `all` build, and verified Editor smoke following the build/run workflow.
- [x] Compare fixed/moving image references, pass/draw/dispatch identities,
  temporal history outcomes, target active/retained bytes, creation/eviction,
  render-thread median/p95, and GPU pass timings against the frozen Stage 0
  gates. Remove rejected compatibility/scaffolding paths rather than retaining
  two architectures.
- [x] Publish the implemented immutable preparation, resolved execution,
  transient target, fixed scheduling, telemetry, failure, temporal, and output
  contracts under Runtime Rendering and update direct owners without
  duplicating RHI or viewport documentation.
- [x] Record the final pass/resource/lifetime/transition inventory and the
  measured entry evidence for the bounded Render Graph follow-up. Reevaluate
  the existing PSO-cache investigation without selecting a cache absent its
  required evidence.
- [x] Complete this plan only after no retired prepared-view, phase/readiness,
  pass-local transient cache, mutable diagnostic seam, or duplicate production
  path remains and all required validation passes.

#### Acceptance Gate

- Focused and aggregate tests, builds, Vulkan validation, runtime matrices,
  images, performance/memory gates, recovery/shutdown, Editor smoke, and
  documentation validation pass; lasting contracts are published; a later RDG
  plan has bounded first consumers and needs no second frame-data redesign.

## Validation Matrix

| Boundary | Required evidence |
| --- | --- |
| Structural ownership | No `FPreparedSceneView` consumer remains; only orchestration sees the outer frame plan; logical draw values contain no readiness, target binding, phase, or execution telemetry. |
| Scene and visibility | Null/non-null Scene, hidden/visible/culled/invalid bounds, typed family selection, main/auxiliary views, and scene mutation retain exact outcomes and lifetimes. |
| Geometry preparation | Static, Spline, Skeletal, and Terrain LOD, material, section/patch, deformation, batching, sorting, triangles, and fallback match frozen references. |
| Directional shadow | Receiver selection, caster membership, cascades, bias, Opaque/Masked filtering, shared palettes, target fallback, counters, images, and motion remain exact. |
| Deferred chain | GBuffer completeness, hybrid retained geometry, deferred lighting, GTAO, contact visibility, isolated qualification, debug outputs, and fallback are typed and match baseline. |
| Volumetric cloud | Authored/absent/invalid input, compute/fragment route, shadow visibility, spatial/composite targets, temporal acceptance/rejection, history commit/abort, resize, and images pass. |
| Resource lifetime | Imported/persistent/transient classification, description keys, active/retained bytes, reuse/eviction, allocation failure, retry, invalidation, release, and shutdown reconcile. |
| RHI access | Render-pass attachment states and every buffer/texture transition pass in inline/threaded execution and Vulkan validation without a second state authority or new wait. |
| Temporal/output | Duplicate/missing state, cut/discard, failure abort, successful commit, fixed-aspect bars, viewport/scissor restoration, editor assistance, Present, and offscreen ShaderReadOnly pass. |
| Telemetry | Feature counters conserve against typed results; no counter controls rendering; successful public statistics are unchanged; failed views publish no partial value; observers are immutable. |
| Multi-view/recovery | Main, camera-preview, auxiliary, window/offscreen, interleaved extents, shader reload, retry, device invalidation, and last-known-good paths remain isolated and complete. |
| Performance and memory | Quiet-lane median/p95, GPU pass time, draw/dispatch counts, target active/retained bytes, creations/evictions, and resize churn meet the frozen Stage 0 gates. |
| Handoff | Focused and aggregate native tests, required builds, full build, validation-enabled runtime, Editor smoke, lasting documentation, and changed/all-plan validation pass. |

## Definition of Done

- `FPreparedSceneView` is retired; one command-local frame plan owns immutable
  feature partitions, and no pass receives the whole plan.
- Visibility and logical geometry preparation are immutable after publication.
  Resource resolution and command recording use separate typed values and
  cannot write readiness, phase, target bindings, or counters into logical
  draws.
- Every fallible pass exposes typed success/completeness/fallback/output state.
  No renderer decision reads telemetry or derives correctness from counts.
- Imported, persistent/history, and frame-transient resources have explicit
  owners. Feature pass executors acquire no transient targets; the centralized
  provider preserves budgets, recovery, invalidation, GPU safety, and shutdown.
- The fixed frame scheduler shows exact pass order and typed resource
  dependencies while preserving all production and qualification routes.
- Temporal Begin/Commit/Abort, feature history, environment requirements,
  failure isolation, final output transitions, multi-view statistics, and
  rendered output match the existing contracts.
- Profiling, capture, and qualification observe immutable pass inputs/results;
  no global seam can mutate unrelated frame preparation.
- Stage 0 performance, GPU, draw/dispatch, and memory gates pass; obsolete
  compatibility and duplicate ownership paths are removed.
- Lasting Runtime documentation and the final RDG migration inventory are
  complete; all required tests, builds, runtime validation, smoke, and
  documentation checks pass.

## Deferred Follow-ups

- A bounded `RenderGraphAndTransientResources` plan for dependency compilation,
  pass culling, automatic transition generation, graph diagnostics, and the
  first selected Renderer consumers.
- Physical transient allocation reuse and aliasing after logical lifetimes,
  Vulkan placement requirements, GPU completion, memory savings, and failure
  policy are measured.
- Asynchronous compute, multiple queues, queue-family ownership, timeline
  synchronization, and overlap only after one workload demonstrates benefit.
- Task-parallel visibility/draw preparation or parallel command recording after
  immutable snapshot, allocator, cancellation, and measured CPU-cost contracts
  are independently selected.
- Persistent mesh draw-command caching, PSO precaching/central caching,
  GPU-driven visibility, indirect draws, clustered lighting, bindless, and
  multi-view batching remain separate evidence-driven work.
- A public or runtime-polymorphic pass/feature registry remains deferred until
  another module requires registration rather than private composition.

## Related Documentation

- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Renderer Scene Representation](../../../Runtime/Rendering/SceneRepresentation.md)
- [Persistent View State](../../../Runtime/Rendering/PersistentViewState.md)
- [Renderer Resource Recovery](../../../Runtime/Rendering/RendererResourceRecovery.md)
- [RHI Command Execution](../../../Runtime/Rendering/RHICommandExecution.md)
- [RHI Resource Transitions](../../../Runtime/Rendering/RHIResourceTransitions.md)
- [Minimal GBuffer Contract](../../../Runtime/Rendering/GBuffer.md)
- [Deferred Directional Lighting](../../../Runtime/Rendering/DeferredDirectionalLighting.md)
- [PSO Cache for Render-Graph Expansion](../../../Investigations/PSOCacheForRenderGraphExpansion.md)
- [Renderer Modularization Plan](../2026-07/RendererModularization.md)
- [RHI and Vulkan Evolution Roadmap](../../../Roadmaps/Archive/2026-08/RHIAndVulkanEvolution.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderPlan.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRendererProfiling.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/StaticMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SkeletalMeshRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderPreparation.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DirectionalShadowRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GBufferRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/GroundTruthAmbientOcclusionRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/ContactShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/DeferredDirectionalLightingRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/VolumetricCloudShadowRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/PostProcessRenderer.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/SceneViewState.h`
- `Engine/Source/Runtime/RHI/Public/RHICommandList.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneContractTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/GBufferQualificationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/DirectionalShadowBaselineVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudVulkanTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/VolumetricCloudSceneVulkanTests.cpp`
