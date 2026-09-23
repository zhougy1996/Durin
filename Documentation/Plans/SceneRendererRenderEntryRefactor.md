# Scene Renderer Render Entry Refactor Plan

Summary: Make FSceneRenderer::Render(FRDGBuilder&) the scene graph authoring entry, absorb Composer, and separate per-submission orchestration from persistent renderer services.

Last reviewed: 2026-09-23

Status: Completed
Completed: 2026-09-23

## Current Status

All stages are complete. `FSceneRenderer::Render(FRDGBuilder&)` authors the
production graph and exposes its output handle for appended consumers;
`FSceneRenderingService` owns persistent state. Composer and static-only authoring
wrappers are removed, with named resource preparation and typed feature inputs.

Final workspace `all` build passed. The affected selection passed all 23 targets
(0 failed/skipped); final RendererSceneContractTests passed 68/68, including the
new entry test, which also passed alone. Changed-document and plan validation
passed. Runtime documentation describes the implemented ownership and lifetime.
Additional image/performance qualification was not run; affected Vulkan integration
coverage passed and no visual-equivalence or performance claim is made.

## Goal

Expose one readable scene-authoring flow through
`auto FSceneRenderer::Render(FRDGBuilder& GraphBuilder) -> void`. The scene renderer
owns one prepared submission and explicitly connects typed feature outputs.
The caller owns graph execution and success/failure finalization.

Align the entry and ownership boundaries with UE's scene-renderer/RDG separation;
retain Durin's existing rendering behavior and failure contracts. Do not introduce
`FDeferredShadingSceneRenderer` or a renderer inheritance hierarchy in this plan.

## Scope and Selected Decisions

- Make `FSceneRenderer` submission-local. Move the current long-lived ownership
  into a Renderer-private `FSceneRenderingService`: resource coordinator, allocator,
  persistent feature renderers, caches, view-state registry, startup/shutdown,
  pending scene updates, and invalidation. Keep the external renderer-module
  interface stable where possible. This service owns real persistent duties;
  it must not become another graph-authoring forwarding layer.
- Prepare the submission before `Render`. Bind its view, options, resolved
  resources, feature decisions, transaction, and observation data explicitly to
  that instance. Eliminate a mutable global/current-frame pointer or temporary
  pointer installed on a persistent renderer to simulate the single-argument API.
- `Render` declares resources and passes in producer-before-consumer order. It
  neither calls `GraphBuilder.Execute` nor commits history or output transactions.
  It is single-use for one prepared submission; copying or moving an instance
  after callback capture must not invalidate borrowed data.
- Keep the instance and its submission data alive through graph execution and
  publication/abort. The caller must not destroy or repurpose them after authoring
  but before deferred callbacks execute. Persistent service references must remain
  valid for that interval as well.
- Absorb Composer into the scene renderer. Keep the main sequence short by using
  named scene-texture/resource preparation and feature-stage functions, with
  implementation distributed across feature source files as appropriate.
- Retain outer orchestration for preparation failures, bounded resource retry,
  execution/capture, telemetry, temporal state, and output finalization. Retain
  `FSceneRenderPipeline` only for those concrete duties; remove duplicated frame
  ownership and graph-authoring wrappers after migration.
- Feature functions receive `FRDGBuilder&` explicitly and narrow named input
  groups, then return typed outputs. Group related scene textures, environment,
  and shadow inputs rather than repeating handle/raw-pointer lists. Stateful
  feature renderers keep their resource/cache responsibilities. Remove static-only
  `XXXRendering` wrapper classes where a named `AddXXXPasses` function suffices.
- Preserve immediate and owned recording authoring paths (`AddPass` and
  `AddRecordingPass`). RDG's private `RecordPasses` is execution machinery and is
  outside this scene-interface migration; renaming it is not an acceptance gate.
- Preserve pass dependencies, culling roots, attachment handoffs, fallback/debug
  routes, allocation retirement, capture names, and publication conditions.
  A recorded graph is not proof of GPU completion or successful scene output.
- Keep external shared-graph composition possible, but do not add multiview
  batching, cross-view resource sharing, new shading paths, scheduling policy,
  or a generic feature registration framework in this refactor.

## Required References and Coordination

- [Renderer frame preparation](../Runtime/Rendering/RendererFramePreparation.md):
  ownership, frame schedule, typed results, transaction publication, and capture.
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)
  and [Render Graph](../Runtime/Rendering/RenderGraph.md): failure/retry,
  execution lifetime, extraction, and recording boundaries.
- [Build workflow](../Agents/BuildAndRun.md),
  [test workflow](../Agents/Testing.md), and
  [documentation workflow](../Agents/Documentation.md).
- [Geometry submission refactor](GeometrySubmissionRefactor.md) and
  [RDG/RHI multi-queue execution](RdgRhiMultiQueueExecution.md) share renderer
  consumers. Reconcile their current stages before editing shared files; do not
  roll back geometry contracts, queue ownership, or retirement behavior.
- Start code inspection in
  `Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h`,
  `SceneRenderPipeline.cpp`, `SceneRenderer.cpp`, and the feature
  `*Rendering.h/.cpp` files. Search all project source/test roots declared in
  `Durin.dworkspace` when migrating symbols.

## Implementation Stages

### Stage 0: Freeze Ownership and Migration Baselines

Dependency: none. Outcome: an explicit ownership and call-site inventory.

- [x] Inventory `FSceneRenderer` construction, module ownership, friends, service
  access, hit-proxy rendering, pending scene updates, and test fixtures across all
  workspace projects. Separate persistent state from submission-local state.
- [x] Define construction/preparation, single-use authoring, lifetime, and
  finalization contracts for the new renderer. Identify every callback capture
  and publication destination whose owner moves.
- [x] Specify the exact outer retry boundary, including fresh graph and fresh
  submission state for each supported retry; preserve existing retry limits.
- [x] Record representative graph/capture baselines for production, fallback,
  deferred/debug, clouds/history, offscreen/present, and editor assistance routes.
  Discover applicable tests from the configured registry and record selections.
- [x] Record conflicts with active geometry/queue work and resolve remaining
  ownership details in this plan before implementing Stage 1.

Completion: the inventory assigns each state and operation one owner; the
validation matrix identifies observable behavior and existing or missing coverage.

### Stage 1: Separate Persistent Services and Submission State

Dependency: Stage 0. Outcome: service lifetime and scene-attempt lifetime are explicit.

- [x] Introduce `FSceneRenderingService` and migrate persistent state and its
  module/lifecycle consumers, preserving startup, reload, shutdown, and view states.
- [x] Establish submission-local `FSceneRenderer` ownership of the prepared frame
  context with safe callback lifetime and narrow references to persistent services.
- [x] Migrate outer preparation/retry/finalization to these owners. Keep one frame
  context per attempt and preserve abort behavior on every early return.
- [x] Migrate source and test consumers atomically; compile affected project targets
  and run the lifecycle/resource-recovery tests selected in Stage 0.

Completion: no cached resources are recreated merely because a new view is rendered;
no long-lived service holds a dangling current-submission pointer.

### Stage 2: Establish Render and Remove Composer

Dependency: Stage 1. Outcome: `FSceneRenderer::Render(FRDGBuilder&)` owns scene authoring.

- [x] Move Composer's authoring into `Render`, extracting resource setup and feature
  stages so the main body directly shows Durin's existing schedule.
- [x] Move graph budgets and root/output declarations with their authoring owner;
  preserve baseline pass names, ordering, typed edges, barriers, and capture shape.
- [x] Let the outer owner call `Render`, execute the caller-owned graph once, inspect
  both graph and scene results, and then commit or abort transactions.
- [x] Delete Composer, its friendship, includes, build entries, and obsolete tests
  of its name. Retain tests of the production authoring boundary itself.
- [x] Verify authoring performs no pass callbacks; appended caller work can consume
  the scene output under its declared access contract. Exercise failure paths to
  prove no history/output publication occurs before successful finalization.

Completion: production uses the new entry with no Composer compatibility wrapper
or nested graph execution, and Stage 0 behavior baselines remain satisfied.

### Stage 3: Normalize Feature Authoring Interfaces

Dependency: Stage 2. Outcome: explicit feature inputs/outputs replace repetitive adapters.

- [x] Begin with GBuffer and Deferred/BaseScene: introduce narrow texture and
  environment input groups, distinguish CPU setup data from graph dependencies,
  and retain graph resolver authority for physical resource access.
- [x] Apply the same conventions to shadows, AO/contact visibility, clouds,
  SceneColor, post process, and editor assistance. Keep branch-specific inputs
  explicit rather than introducing an all-feature context bag.
- [x] Replace static-only authoring wrappers with named feature functions where
  useful; preserve persistent renderer objects and purposeful recording helpers.
- [x] Migrate every consumer and relevant fixture, preserving graph dependency
  declarations independently of the renamed C++ call structure.

Completion: the main render body communicates feature order and typed data flow;
authoring functions have consistent graph/input/output conventions and no obsolete
forwarding layer remains.

### Stage 4: Validate and Publish the New Contract

Dependency: Stages 1–3. Outcome: completed integration with durable documentation.

- [x] Complete an `all` build and the affected native-test selection using the linked
  workflows, including affected Engine, Sandbox, and RoadWeaver consumers.
- [x] Run the Stage 0 renderer contract/integration matrix, covering resource retry,
  aborted recording, typed scene failures, temporal history, output final access,
  and execution-lifetime ownership. Reuse existing meaningful tests; add coverage
  only for missing behavioral guarantees.
- [x] Select GPU validation under the test workflow if transitions, submission,
  resource lifetime, or rendered behavior changed; record exact executed/omitted
  coverage and evidence. Do not claim visual equivalence from compilation alone.
- [x] Update owning runtime documentation and examples to the final entry, ownership,
  and execution/finalization contract. Remove obsolete Composer references outside
  historical documents; validate documentation and all plans.
- [x] Record validation evidence, close only satisfied checks, and mark this plan
  completed according to the plan lifecycle rules.

Completion: all required gates pass, the migration is complete across consumers,
and documentation describes implemented behavior rather than an intended facade.

## Stage 0 Ownership and Validation Baseline

RendererModule is the sole persistent owner; Scene borrows the service for sky
updates, and hit proxies use its resources. Engine view-fitting tests are the
only direct external class consumers. Sandbox/RoadWeaver have no private callers.
The submission owns an in-place context before preparation; Pipeline prepares it.
Neither moves. Telemetry and view-state guards finalize before context destruction,
and the graph dies first. Callbacks borrow prepared/resolved records, telemetry,
publications and persistent services through Execute. Each of 64 attempts creates
fresh state; batch waits occur after attempt destruction. A graph exists only
after preparation succeeds and is never retried.

Baseline schedule: shadow layers/result, GBuffer, AO, contact, cloud shadow,
deferred, base, cloud spatial/composite, SceneColor, post process, editor.
Preserve disabled producers, diagnostic roots, complete environment fallbacks,
Present/GraphicsShaderRead final access and budgets 15/36/34 and 256/4096.
Registry matrix: RendererSceneContractTests (typed results, view state, policy),
RendererResourceReloadVulkanTests (lifecycle/retry),
StaticMeshRenderPreparationVulkanTests (production/fallback/deferred/debug/capture),
VolumetricCloudSceneContractTests and cloud integration (history). Entry-specific
deferred authoring/caller consumption needs coverage.

Geometry stages 1–3 and queue stages 1–2 are implemented. Their outstanding
qualification is independent. Preserve prepared generic geometry, sync points,
allocator retirement and disabled-by-default production async policy.

## Implementation and Validation Evidence

- Stages 1–3 were migrated atomically to keep private callers consistent. The
  context stays in place from preparation until telemetry/view-state guards have
  finalized. Constructors and feature resource ownership otherwise keep their
  existing behavior. Module and Scene use the persistent service; hit proxies
  and Engine view-fitting fixtures were migrated. All workspace roots were searched.
- Render shows typed producer-to-consumer calls first; resource setup is a named
  helper. All twelve authoring functions use explicit Graph and feature inputs.
  GBuffer retains its typed four-texture set; deferred/base share selected
  environment inputs. Pass parameter metadata and callback dependency declarations
  remain unchanged, as do recording functions, queue policy and pool retirement.
- Initial `all` build passed. Affected selection passed 23/23 targets, including
  renderer/resource reload, geometry preparation, editor, cloud and Vulkan
  integrations. Report: `Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.
- The new `SceneRenderAuthorsIntoCallerGraphAndAllocationFailureDoesNotPublish`
  contract case passed independently after fixing its test namespace and moving
  the rejection allocator out of a lambda for MSVC parsing. Existing static
  interface assertions now check free authoring functions and nonmovable submission.
- No shader, draw, transition, queue submission or GPU retirement algorithm changed.
  Executed affected Vulkan integrations cover resource reload, geometry, editor
  and cloud routes. Additional image/performance qualification was not run; no
  new visual-equivalence or performance claim is made.

Final validation on 2026-09-23: workspace `all` build passed after the output-handle
API and final source layout. RendererSceneContractTests passed 68/68 with its
own XML report beside the affected report. Diff review confirmed persistent
service function bodies and scene graph identity/profile labels against the
pre-migration revision. Changed-document validation passed for eight documents;
all-plan validation completed before commit. Existing affected results are reused
for unchanged rendering/recording algorithms; the final focused run covers the
added entry API and fixture.
