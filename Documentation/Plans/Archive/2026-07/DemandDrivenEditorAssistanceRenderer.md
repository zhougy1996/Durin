# Demand-Driven Editor Assistance Renderer Plan

Summary: Refactor editor assistance into a demand-driven Renderer phase with lazy per-feature resources, output-specific pipelines, and isolated failure behavior.

Last reviewed: 2026-07-30

Status: Archived
Completed: 2026-07-30

## Current Status

Planning baseline: `f3a2e3ab` (`refactor(renderer): simplify scene creation lifecycle`).

Stage 0 implementation baseline: `241b6ea4`
(`docs(renderer): plan demand-driven editor assistance`). The planning baseline
is tree-equivalent for the Renderer working set; the only relevant baseline
diff is this active plan.

Stage 0 selected a Renderer-private, render-thread-only assistance state object.
It remains independent of `FRendererModule` object lifetime and is reset by the
existing queued `ReleaseRendererResources` command, so no queued render command
can observe a destroyed module member. Grid, Gizmo, Line, and Icon feature state,
lazy pipeline entries, dynamic capacities, and one-time diagnostic state will be
reset together by that owner.

The request contract is `FEditorAssistanceRequest`-style pure data derived from
`FSceneView` and the current `EViewportOutput`. Pipeline keys are feature,
output, depth mode, and Gizmo topology. A local prepared result will retain
per-view Line/Icon index counts plus the available pipeline references for each
draw operation; global feature state retains only reusable capacity and RHI
resources.

Post Process and Grid will share one explicit Renderer-private fullscreen
geometry owner. Post Process remains its ordinary eager caller, while Grid only
uses the already-created shared buffers after its own demand is established.
The owner is reset in the same renderer release command after all drawing has
drained.

The narrow test seam is pure request analysis, required pipeline-key mapping,
and drawable-operation selection from an available-key set. It covers failure
isolation without exposing or injecting an RHI resource factory. RHI creation
tracing remains Stage 4 integration evidence.

Stage 0 validation: the focused `EditorRenderingTests` build reached the link
step and failed only on the three intentionally undeclared implementations for
that pure seam (`AnalyzeRequest`, `GetRequiredPipelineKeys`, and
`BuildDrawableOperations`). This is the expected red contract against the
current eager and globally gated implementation.

Stage 1 handoff: baseline `284d742a`; working set was the assistance request
unit, `RendererModule.cpp`, render-target layouts, the private module header,
and focused Editor Rendering tests. Request analysis now precedes all assistance
initialization. Line/Icon preparation returns per-view index counts, Grid
uniform preparation occurs before the draw pass, and the local prepared result
decides whether any assistance operation survives. When none survives, Post
Process uses an output-specific final layout and the assistance render pass is
elided. Validation passed 13 focused request, draw-order, and render-target
layout tests through `EditorRenderingTests`.

Stage 2 handoff: baseline `abb09e3a`; working set expanded only to the
Renderer-private assistance implementation and fullscreen-geometry owner.
`RendererModule.cpp` now performs request, scene, post-process, and assistance
phase orchestration without Grid/Gizmo/Line/Icon shader types, feature states,
pipeline construction, preparation, or drawing. The assistance unit owns its
complete render-thread state and prepared-frame boundary, and the existing
queued renderer release command calls its single reset path. Post Process and
Grid now consume one explicit fullscreen-geometry owner, whose state is reset
in that same command. Renderer source glob discovery required no build metadata
change. The complete `EditorRenderingTests` target passed all 17 tests.

Stage 3 handoff: baseline `df3289a7`; working set was the Renderer-private
assistance header and implementation plus focused Editor Rendering tests. The
common recoverable-resource slot has not landed, so assistance uses a narrow
private `FGenerationScopedAttempt` that keeps payload availability separate
from latest-attempt state and is ready to migrate without changing demand or
pipeline keys. Base resources and keyed pipelines suppress repeated attempts
for the same relevant shader/device/manual generation, retry after a relevant
generation change, retain last-known-good payloads after shader/manual refresh
failure, and clear device-dependent payloads before device-generation retry.
Manual generation changes select only failed attempts rather than refreshing
ready resources. Each feature and pipeline key retains its own failure detail
and emits one failure diagnostic per attempted generation plus one recovery
diagnostic. The Stage 0 pure seam covers Icon-base, Line X-Ray, and wire-Gizmo
failure isolation and sequential Offscreen/Present key creation without an RHI
factory seam. The complete `EditorRenderingTests` target passed all 22 tests.

Stage 4 completion handoff: baseline `224583c8`; the final working set was this
plan, with no further code or long-lived documentation changes required. The
complete 22-test
`EditorRenderingTests` target passed, source inspection confirmed that empty
requests bypass assistance preparation and that feature/pipeline creation
occurs only after demand and valid geometry, and the complete Debug Editor
`all` target built successfully. A 30-tick hidden-window Vulkan editor smoke
exited normally with no Shader, Pipeline, Vulkan Validation, Error, or Fatal
diagnostics. A maximized editor capture verified the Grid, axis, light Icon,
scene composition, and viewport boundary without visible corruption. The user
then completed the interactive validation and confirmed that sequential main
and auxiliary views with different contents and sizes remain isolated, that
Offscreen followed by Present creates the later output path correctly, and
that the full Grid, solid/wire Gizmo, Line, Icon, depth occlusion,
X-Ray/Visible, FXAA on/off, constrained-scissor, and camera-preview matrix
passes. Stage 4 and the plan are complete.

Post-completion coordination evidence: the Recoverable Renderer Resource
Creation plan has landed its public RenderCore
`TRenderResourceCreationSlot`. Grid, Gizmo, Overlay Line, and Overlay Icon base
resources and their keyed pipelines now use that common state directly; the
temporary module-private `FGenerationScopedAttempt` and its three-state
availability enum have been removed. Focused failure injection proves
independent feature availability and generation-scoped recovery, while the
Renderer-owned changed/all/manual commands now provide the ordered invalidation
path anticipated by this plan.

The existing renderer already composes editor assistance after scene post-processing
and preserves scene depth for grid and overlay occlusion. The remaining resource
and failure boundaries are too broad:

- Every valid `RenderView()` calls the Gizmo, Overlay Line, Overlay Icon, and
  Editor Grid resource initializers before checking whether the view contains
  those features.
- The initializers create both Offscreen and Present pipeline variants. A first
  render therefore attempts all eighteen assistance pipelines even for a game
  view with no assistance data.
- `AreEditorAssistanceOutputPipelinesReady()` requires every pipeline from every
  feature and output mode. One missing pipeline suppresses preparation and
  drawing for the complete assistance phase.
- The assistance render pass is opened even when the view contains no
  assistance work or every requested operation is unavailable.
- Grid rendering reaches into post-process state for fullscreen geometry, and
  dynamic line/icon draw counts live in global feature state rather than a
  per-view prepared result.

The initial working set is:

- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.h`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.cpp`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.h`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.cpp`
- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- focused Renderer and Engine test files selected in Stage 0
- `Documentation/Runtime/Rendering/ViewportRendering.md`

Expand this working set only when Stage 0 identifies a direct renderer-resource
lifecycle dependency, an RHI test seam, or build metadata that does not discover
new Renderer source files automatically.

## Goal

Make editor assistance cost and availability follow the immutable contents of
each `FSceneView`:

- A view with no grid, gizmos, overlay lines, or overlay icons creates no
  assistance shaders, textures, buffers, or pipelines and opens no assistance
  render pass.
- A non-empty view initializes only the feature resources and current output
  pipeline variants required by its contents.
- Failure disables only the smallest affected feature or draw operation while
  independent assistance remains drawable.
- Editor assistance has one private renderer-stage owner whose preparation,
  resource lifetime, composition order, and release behavior are explicit.

## Scope

- Derive an editor-assistance workload from `FSceneView::EditorGrid`,
  `OverlayPrimitives`, `OverlayLines`, and `OverlayIcons`.
- Distinguish solid and wire Gizmo requirements so unused topology pipelines
  are not created.
- Lazily create pipeline variants by feature, depth mode, topology, and current
  Present or Offscreen output.
- Keep base resources independently owned by Grid, Gizmo, Overlay Line, and
  Overlay Icon feature states.
- Replace global readiness with per-operation availability and diagnostics.
- Keep dynamic, view-dependent preparation in a local prepared-frame object so
  sequential main and auxiliary views cannot reuse stale draw counts.
- Skip the editor-assistance render pass when no requested operation survives
  preparation.
- Extract editor-assistance orchestration and resources from
  `RendererModule.cpp` into the existing Renderer-private assistance unit.
- Preserve render-thread ownership, shutdown release, depth attachment,
  viewport/scissor, draw ordering, and final output layout contracts.
- Add focused tests for request analysis, required pipeline keys, ordering,
  output-specific laziness, empty-pass elision, and failure isolation.
- Update long-lived rendering documentation after the refactor lands.

## Non-Goals

- Changing grid appearance, fade behavior, overlay geometry, X-Ray opacity,
  Gizmo shapes, icon appearance, or hover/selection semantics.
- Moving editor assistance into Mona, ImGui, LevelEditor, or another module.
- Adding an editor-view boolean to `FSceneView`; assistance demand remains
  determined by the submitted immutable data.
- Replacing the procedural icon atlas with the offline atlas described by the
  separate Editor Icon Atlas plan.
- Changing scene post-processing, FXAA, future temporal-history ownership, or
  motion-vector contracts.
- Combining post-processing and assistance into one render pass.
- Introducing a general render graph, pass scheduler, PSO cache framework, or
  public RHI abstraction.
- Retrying shader, static-resource, or pipeline failures every frame.
- Refactoring unrelated eager renderer resources such as Sky Box resources.

## Design Decisions and Invariants

### Phase ownership and ordering

- Editor assistance remains a Renderer phase after scene post-processing and
  before the Present or ShaderReadOnly final transition.
- The phase continues to load preserved scene depth without clearing or
  writing it.
- The draw order remains Grid, X-Ray Gizmos, X-Ray Lines, X-Ray Icons, Visible
  Gizmos, Visible Lines, and Visible Icons.
- Fixed-aspect viewport and scissor restoration remains owned by the
  assistance phase; black bars remain untouched.
- `FRendererModule::RenderView()` orchestrates phases but delegates assistance
  workload analysis, preparation, render-pass elision, and drawing to one
  Renderer-private owner.

### Workload representation

- A pure `FEditorAssistanceRequest` is built once per rendered view and records
  the current output type plus Grid, solid Gizmo, wire Gizmo, Line, and Icon
  demand.
- Grid demand is `View.EditorGrid.bVisible`.
- Solid and wire Gizmo demand is derived from the actual
  `OverlayPrimitives` shapes rather than only testing whether the vector is
  empty.
- Line and Icon demand requires a non-empty corresponding vector. Geometry
  validation may still reduce a requested feature to no prepared draws.
- An empty request returns before any assistance resource initialization.

### Resource ownership and lazy state

- Grid, Gizmo, Line, and Icon each own an independent base-resource slot whose
  availability, attempt, failure, generation, and retry semantics follow the
  Recoverable Renderer Resource Creation plan.
- Base resources are constructed into temporaries and committed to the feature
  state only after the complete base set succeeds. This prevents accidental use
  of partially initialized shaders, declarations, static buffers, atlases, or
  samplers.
- A base-resource failure is sticky and logs once for the same relevant
  generation. Shader or explicit-reload invalidation may retry shader-backed
  state without a module restart; device invalidation clears dependent RHI
  payloads before retry.
- A failed shader refresh retains a valid last-known-good payload. Device
  invalidation never treats an old RHI payload as a fallback.
- Dynamic line and icon buffer allocation or geometry-generation failure is a
  per-view preparation failure. It does not permanently mark the feature
  failed.
- Renderer shutdown and any supported RHI reinitialization reset base states,
  pipeline entries, dynamic capacities, and failure-log state together.

### Pipeline keys and failure domain

- Pipeline creation is keyed by the dimensions that affect compatibility:
  feature, current `EViewportOutput`, depth mode, and Gizmo topology where
  applicable.
- Only the current output variant is requested. Rendering Offscreen assistance
  does not create a Present pipeline and vice versa.
- Grid requests one depth-tested pipeline for the current output.
- Lines and Icons request X-Ray and Visible pipelines for the current output
  only when their feature has prepared geometry.
- Gizmos request X-Ray and Visible pipelines only for the solid and/or wire
  topology present in the view.
- Pipeline entries have independent lazy state. One failed entry must not
  invalidate a different output, depth mode, topology, or feature.
- A failed X-Ray entry suppresses only that X-Ray operation; its Visible
  operation remains eligible. A failed wire entry does not suppress solid
  Gizmos.
- Diagnostics identify feature, output, depth mode, topology when applicable,
  and pipeline name. The plan removes the global incomplete-pipeline log.

### Preparation and render-pass elision

- Preparation occurs before `BeginRenderPass()` because line/icon buffer writes
  and lazy pipeline creation are not draw-pass work.
- Preparation returns a local `FPreparedEditorAssistance` containing the
  operation availability and view-dependent draw data for one view.
- Draw functions consume the prepared result rather than global line/icon index
  counts.
- If no operation is drawable after independent preparation and pipeline
  lookup, the assistance render pass is not opened.
- Resources remain cached after a later empty view; demand-driven means lazy
  creation and pass elision, not per-view destruction.

### Fullscreen geometry

- Grid must no longer reach into `FPostProcessRendererState` internals for its
  fullscreen triangle.
- Stage 0 will confirm the narrow lifetime boundary, then implementation will
  select one explicit shared Renderer-private fullscreen-geometry owner used by
  Post Process and Grid. Duplicating the geometry is the fallback only if a
  shared owner would introduce a less clear shutdown or command-list lifetime.
- This decision does not add a public Renderer or RHI interface.

### Relationship to the completed phase-boundary plan

- The archived Scene Post-Processing and Editor Assistance Boundary plan remains
  authoritative for phase order, depth lifetime, viewport/scissor behavior,
  output layouts, and exclusion from FXAA and future temporal history.
- That plan retained an all-assistance no-draw response to pipeline failure
  while moving work across the FXAA boundary. This refactor intentionally
  replaces that temporary failure behavior.
- Partial assistance remains safe now because every surviving operation is
  still composed after post-processing; no fallback moves any assistance
  feature into scene color or FXAA input.

### Relationship to recoverable resource creation

- `Documentation/Plans/Archive/2026-07/RecoverableRendererResourceCreation.md` owns the common
  transactional candidate, retained failure, generation dependency,
  last-known-good, retry, diagnostic, and invalidation semantics.
- This plan owns view-driven demand, feature states, pipeline-key
  decomposition, prepared operations, and failure isolation after resource
  lookup.
- The landed assistance implementation consumes the common RenderCore
  resource-state contract without changing this plan's demand or pipeline
  keys. Recoverable-resource Stage 4 owns its ordered changed/all/manual
  invalidation commands.
- Neither plan authorizes per-frame retry or an unsupported device-recovery
  lifecycle.

### Unreal Engine comparison boundary

- Durin's existing `FSceneView`, render-thread command submission, RHI pipeline
  compatibility, and completed phase contract determine the implementation.
- If Stage 0 finds unresolved ownership or device-reset behavior, Unreal
  Engine's view-driven optional renderer phases and lazy render-resource/PSO
  patterns may be consulted as comparison material.
- UE terminology or class hierarchy is not imported unless it clarifies an
  already required Durin responsibility; this plan does not introduce a render
  graph or extension framework merely for structural similarity.

## Current Foundations and Gaps

### Existing foundations

- `FSceneView` already contains all assistance demand as immutable view data.
- `RendererEditorAssistance::GetDrawOrder()` provides one explicit ordering
  contract.
- Individual draw helpers already check their selected pipeline and required
  feature resources before issuing draws.
- Overlay Line and Icon preparation already avoids geometry generation when
  their input vectors are empty.
- `FEditorAssistanceOutputPipelines` distinguishes Present and Offscreen
  compatibility.
- The completed phase-boundary work already supplies the correct final color
  plus preserved-depth render-target layouts.
- Renderer shutdown already has one render-thread command that releases the
  current global feature states.

### Gaps to close

- Demand is checked after all feature initializers have been called.
- Present and Offscreen pipelines are created as inseparable pairs.
- Gizmo initialization creates solid and wire topology variants together.
- Global readiness tests variants unrelated to the current view and output.
- Preparation and drawing are globally suppressed after any pipeline failure.
- The final assistance render pass is unconditional.
- Base-resource state cannot distinguish never requested, ready, and failed
  without inferring readiness from individual references.
- Grid has an implicit dependency on post-process fullscreen buffers.
- Global dynamic draw counts encode data belonging to one prepared view.
- Focused coverage does not assert the resource-demand or failure-isolation
  contract.

## Implementation Stages

### Stage 0: Lock ownership, request, and test contracts

- [x] Record the implementation baseline, relevant diff, working set, and
  existing render-thread release path before editing.
- [x] Confirm whether the assistance renderer is safest as a module-private
  render-thread state object or a private `FRendererModule` member whose
  destruction is ordered after queued release commands. Record the selected
  ownership and shutdown reasoning before implementation.
- [x] Define `FEditorAssistanceRequest`, the exact request-to-pipeline-key
  mapping, and the local prepared-result boundary.
- [x] Confirm the shared fullscreen-geometry owner and its initialization and
  release ordering.
- [x] Select the narrowest non-public test seam for observing requested
  pipeline keys and simulating one unavailable operation. Prefer pure request
  and draw-plan helpers; use an injectable resource factory only if RHI-backed
  behavior cannot otherwise be covered.
- [x] Add focused failing tests for an empty request, Grid-only Offscreen,
  solid-only Gizmos, wire-only Gizmos, Line/Icon demand, and independent
  operation availability.
- [x] Record the intentional failure-policy change from the archived
  phase-boundary plan in this plan if implementation discoveries refine it.

#### Acceptance Gate

- Ownership and release ordering have one selected design; pure tests describe
  the complete request and pipeline-key matrix; the focused suite fails against
  the current eager or globally gated behavior for the expected reasons.

### Stage 1: Introduce view-driven demand and empty-phase elision

- [x] Implement pure assistance request analysis from `FSceneView` and the
  current output type.
- [x] Move the empty-request decision ahead of all Gizmo, Line, Icon, and Grid
  initialization.
- [x] Make Overlay Line and Icon geometry preparation report whether any valid
  geometry survived clipping and input validation.
- [x] Represent per-view dynamic counts and operation demand in a local
  prepared result rather than relying on stale global counts.
- [x] Skip the final assistance render pass when the request is empty or
  preparation produces no drawable operation.
- [x] Preserve current phase order, depth attachment, viewport/scissor, and
  final layout behavior for every non-empty request.
- [x] Pass focused request, preparation, and draw-order tests.

#### Acceptance Gate

- A game-style empty view calls no assistance initializer and emits no
  assistance render pass; non-empty views retain current appearance, ordering,
  depth behavior, and final output transitions.

### Stage 2: Extract the independent assistance renderer

- [x] Turn the existing Renderer-private assistance unit into the selected
  `FEditorAssistanceRenderer`-style owner without exposing it through
  `IRendererModule`.
- [x] Move Grid, Gizmo, Line, and Icon feature states plus their initialization,
  preparation, and draw helpers out of `RendererModule.cpp`.
- [x] Keep `RendererModule.cpp` responsible only for scene, post-process, and
  assistance-stage orchestration.
- [x] Introduce the explicit shared fullscreen-geometry owner selected in
  Stage 0 and remove Grid access to post-process internals.
- [x] Route renderer shutdown and any RHI reset through one assistance resource
  reset path.
- [x] Preserve the existing public `FSceneView` representation unless a direct
  implementation conflict proves that a private derived request cannot express
  the required workload.
- [x] Keep the Renderer source and build metadata changes limited to files
  required by the extracted private owner.

#### Acceptance Gate

- `RendererModule.cpp` contains no Grid/Gizmo/Line/Icon resource state or
  feature-specific pipeline construction; the private assistance owner has a
  complete render-thread lifetime and the focused tests remain behaviorally
  unchanged.

### Stage 3: Add lazy output-specific pipelines and isolated failure

- [x] Add explicit tri-state base-resource initialization independently for
  Grid, Gizmo, Line, and Icon.
- [x] Replace paired Present/Offscreen pipeline construction with independent
  lazy entries keyed by current output, depth mode, and topology.
- [x] Create base resources and pipeline entries only after the request and
  prepared geometry prove they are needed.
- [x] Commit base-resource aggregates only after full base initialization;
  retain independent pipeline-entry success or failure.
- [x] Remove `AreEditorAssistanceOutputPipelinesReady()` and the global
  assistance-pipeline failure flag and message.
- [x] Make the prepared operation set include every successfully available
  operation even when a sibling feature or variant failed.
- [x] Add feature- and key-specific one-time diagnostics without per-frame
  retries or log spam.
- [x] Reset all lazy and diagnostic states through the assistance owner's
  release path.
- [x] Extend focused tests to cover one failed Icon base resource, one failed
  Line X-Ray pipeline, one failed wire Gizmo pipeline, and independent
  Offscreen/Present creation.

#### Acceptance Gate

- Empty views create zero assistance resources; Grid-only Offscreen creates
  only its current-output pipeline; a failed feature or pipeline entry does not
  suppress unrelated operations; rendering a later output type creates only
  the missing variants.

### Stage 4: Integration, rendering validation, and lasting documentation

- [x] Run focused request, ordering, render-target layout, grid, Renderer, and
  viewport tests through the repository-native test workflow.
- [x] Instrument or inspect the RHI creation path in a development build to
  verify zero assistance pipeline creation for a pure game view and the
  expected demand counts for representative editor views.
- [x] Validate sequential main and auxiliary views with different assistance
  contents and target sizes; confirm prepared data does not leak across views.
- [x] Validate Offscreen and Present paths, including creating one output mode
  first and the other later.
- [x] Run a successful full `all` build through the root DurinDevTool workflow.
- [x] Run the verified `DurinEditor` with the same Agent Build Profile and
  confirm Shader, Pipeline, Vulkan Validation, Error, and Fatal logs remain
  clean.
- [x] Perform the focused visual matrix for Grid, solid/wire Gizmos, Lines, and
  Icons with depth occlusion, X-Ray/Visible ordering, FXAA on/off, constrained
  viewport scissor, and an auxiliary camera preview.
- [x] Update `Documentation/Runtime/Rendering/ViewportRendering.md` with the
  demand, ownership, pass-elision, and failure-isolation contracts.
- [x] Update this plan's status and evidence, and keep the Editor Icon Atlas
  plan independent.

#### Acceptance Gate

- Focused and integration tests, the full build, real Vulkan editor smoke, and
  the visual matrix pass; measured resource creation follows view demand; the
  long-lived rendering contract describes the landed ownership and failure
  behavior.

## Validation Matrix

| Scenario | Required behavior | Evidence |
| --- | --- | --- |
| Pure game view | Zero assistance base resources and pipelines; no assistance render pass | Focused test plus RHI creation trace |
| Grid-only Offscreen view | Grid base resources and one Offscreen depth-tested pipeline only | Request/pipeline test plus trace |
| Solid Gizmo-only view | No wire, Line, Icon, or Grid pipeline creation | Focused test plus trace |
| Wire Gizmo-only view | Only wire X-Ray/Visible variants for the current output | Focused test plus trace |
| Lines-only view | Line geometry and current-output X-Ray/Visible variants only | Focused test plus render smoke |
| Icons-only view | Icon atlas/sampler and current-output X-Ray/Visible variants only | Focused test plus render smoke |
| Invalid or fully clipped Line/Icon input | No prepared operation and no empty assistance pass | Focused preparation test |
| Icon base initialization failure | Icons are skipped; Grid, Gizmos, and Lines remain eligible | Failure-injection test |
| Line X-Ray pipeline failure | Visible Lines and unrelated operations still draw | Failure-injection test |
| Wire Gizmo pipeline failure | Solid Gizmos and unrelated operations still draw | Failure-injection test |
| Offscreen followed by Present | Present variants are added lazily without recreating Offscreen variants | Pipeline-cache test plus trace |
| Main and auxiliary views | Per-view geometry counts, viewport/scissor, and resources do not cross-contaminate | Integration test and editor smoke |
| FXAA disabled/enabled | Scene edge treatment changes; assistance remains after post-process and equally crisp | Matched visual capture |
| Mesh occlusion | Visible variants and Grid use preserved depth; X-Ray variants retain existing emphasis | Visual capture |
| Fixed-aspect output | Assistance stays within the content scissor and black bars remain untouched | Camera-preview capture |
| Shutdown or supported RHI reset | All feature, pipeline, dynamic, and diagnostic state resets safely | Lifecycle test or smoke log |

## Definition of Done

- Assistance demand is derived only from the submitted immutable scene view and
  current output mode.
- Empty views create no assistance resources and submit no assistance render
  pass.
- Base resources are lazy and independently owned by feature.
- Pipeline variants are lazy and independently keyed by output, depth mode,
  and topology.
- No global readiness condition can disable unrelated assistance.
- Resource failures are sticky and diagnosed once only for the same relevant
  generation; shader, device, or manual invalidation permits the retry selected
  by the retained dependency mask. Transient per-view preparation failures do
  not change reusable feature state.
- Dynamic prepared data belongs to one rendered view and cannot leak into a
  sequential viewport.
- Grid no longer reaches into post-process private state.
- The post-process, preserved-depth, ordering, viewport/scissor, Present, and
  ShaderReadOnly contracts remain unchanged.
- Focused tests, full build, Vulkan editor smoke, and the visual validation
  matrix pass.
- Long-lived rendering documentation owns the final contract and this plan
  records complete implementation evidence before completion.

## Deferred Follow-ups

- Offline editor icon atlas generation and metadata, owned by the Editor Icon
  Atlas plan.
- General engine-wide PSO caching or prewarming policy.
- Async shader or pipeline compilation.
- A render graph or generalized optional-pass framework.
- Demand-driven initialization of unrelated scene-renderer resources.
- Automated image capture and pixel/semantic visual regression infrastructure.

## Related Documentation

- `Documentation/Runtime/Rendering/ViewportRendering.md`
- `Documentation/Investigations/EditorIconAtlas.md`
- `Documentation/Plans/Archive/2026-07/RecoverableRendererResourceCreation.md`
- `Documentation/Plans/Archive/2026-07/ScenePostProcessEditorAssistanceBoundary.md`
- `Documentation/Plans/Archive/2026-07/EditorWorldGridV2.md`
- `Documentation/Development/Build/BuildAndRun.md`
- `Documentation/Development/Build/NativeTests.md`

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/SceneView.h`
- `Engine/Source/Runtime/Renderer/Public/RendererModule.h`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.h`
- `Engine/Source/Runtime/Renderer/Private/RendererEditorAssistance.cpp`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.h`
- `Engine/Source/Runtime/Renderer/Private/EditorGridRendering.cpp`
- `Engine/Source/Runtime/Renderer/Private/RendererRenderTargetLayouts.h`
- `Engine/Source/Programs/Tests/RenderCoreTests/Private/RenderTargetLayoutTests.cpp`
- `Engine/Source/Programs/Tests/EngineTests/Private/EditorGridRenderingTests.cpp`
