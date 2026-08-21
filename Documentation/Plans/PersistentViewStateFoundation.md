# Persistent View State Foundation Plan

Summary: Add renderer-owned persistent view identity and transactional previous-frame state without implementing a temporal rendering effect.

Last reviewed: 2026-08-21

Status: Completed
Completed: 2026-08-21

## Current Status

Implementation and validation are complete. RenderCore now exposes opaque,
process-unique `FSceneViewStateId` and move-only `FSceneViewStateOwner` types;
Renderer owns the render-thread registry, immutable prepared temporal context,
transactional previous metadata, discontinuity policy, typed history probe,
device/manual invalidation, stale-ID diagnostics, and shutdown audit.

Main and auxiliary `FSceneViewport`s own isolated state tokens, attach IDs to
Engine-built views, retain viewport lifetime through queued submissions, and
propagate initialization, world/level, camera-source, preview-source, focus,
teleport, and caller-requested cuts. Direct render callers remain stateless.

Validation passed on 2026-08-21: `RenderContractTests` (40 tests),
`RendererSceneContractTests` (24), `ViewportTests` (104),
`SkyBoxVulkanIntegrationTests` (1), `EditorGridVulkanTests` (7),
`RendererResourceReloadVulkanTests` (1), the complete `fast-all` selection of
54 native targets, and a full `all` build. Stateful/no-consumer output matched
stateless output byte-for-byte in forward, deferred, and constrained offscreen
paths; window-backed persistent rendering passed resize and present coverage.
The lasting contract is published in
[Persistent view state](../Runtime/Rendering/PersistentViewState.md).

## Goal

Ship an optional persistent-view-state contract that lets one logical viewport
carry previous successful render metadata and future feature-owned GPU history
across render submissions, with explicit identity, render-thread mutation,
transactional commit, discontinuity reporting, deterministic teardown, and a
fully stateless fallback.

## Scope

- A stable opaque view-state identity and RAII ownership token exposed through
  RenderCore/`IRendererModule` without exposing Renderer implementation data.
- Renderer-private state lookup and storage keyed by non-reusable identity.
- Per-state previous/current view metadata, successful-submission sequence,
  explicit camera-cut and invalidation causes, and prepared temporal context.
- Begin/commit/abort semantics so only a complete successful view advances
  previous-frame state.
- Render-thread creation, mutation, invalidation, device-generation handling,
  removal, shutdown, and queued-command ordering.
- `FSceneViewport` ownership and `DEngine` submission integration for persistent
  main and auxiliary viewports; direct callers remain opt-in.
- Diagnostics and focused tests sufficient for later feature renderers to own
  strongly typed history resources inside the private concrete state.

## Non-Goals

- TAA, temporal upsampling, volumetric-cloud reprojection, GTAO accumulation,
  motion blur, exposure adaptation, denoising, or any other temporal effect.
- Motion vectors, velocity buffers, jittered projection, Halton sequences,
  optical flow, disocclusion tests, neighborhood clipping, or history blending.
- A generic string-keyed or type-erased GPU resource cache in RenderCore.
- Sharing one history between stereo eyes, cube faces, thumbnails, picking,
  reflection captures, or unrelated viewports.
- Inferring persistent identity from output pointers, scene pointers, matrices,
  camera object addresses, viewport dimensions, or frame numbers.
- A render graph, transient allocator, cross-queue synchronization, or changes
  to scene ownership.

## Design Decisions and Invariants

### Identity and public boundary

- Persistent identity is explicit. RenderCore defines a small opaque
  `FSceneViewStateId`, an invalid sentinel, and move-only
  `FSceneViewStateOwner`; Renderer creates owners through
  `IRendererModule::CreateViewState()`.
- `FSceneView` carries only the copyable state ID and explicit discontinuity
  input. It does not expose previous matrices, history textures, or the concrete
  Renderer state.
- IDs are process-unique and are never reused during one Renderer-module
  lifetime. A missing, invalid, released, or foreign ID renders stateless and
  records a bounded diagnostic; it never aliases another view.
- The owner token's release callback enqueues Renderer-state removal. Render
  submissions retain the owning `FSceneViewport`, so FIFO render-command order
  keeps its ID alive through queued use. Direct callers must retain the owner
  until their submissions have executed.
- Renderer shutdown requires all Engine-held owners to be released before the
  private renderer is destroyed, flushes queued removal, and diagnoses leaked
  live states. The public token contains no Renderer-private fields.

### State ownership and thread model

- `FSceneViewState` is Renderer-private and all lookup, mutation, feature-history
  access, reset, and destruction occur on the rendering thread.
- `FSceneRenderer` owns the state registry. `FScene`, `DWorld`, cameras, and
  output textures do not own temporal state.
- The concrete state is an extensible owner, not a universal cache. Later
  temporal features add strongly typed Renderer-private substate and explicit
  reset/release behavior; RenderCore remains unaware of feature resources.
- One owner represents one logical view stream. Separate viewport panels,
  stereo eyes, reflection faces, and preview streams require separate owners.
  Reusing one ID for multiple submissions in an interleaved stream is invalid
  and diagnosed in checked/test builds.

### Previous-view and submission contract

- At the start of `RenderView_RenderThread`, the renderer resolves the optional
  state and builds one immutable `FSceneViewTemporalContext` for that
  submission. Feature renderers consume that context rather than reading or
  mutating the registry directly.
- The context contains current and previous final fitted view/projection/view-
  projection matrices, camera position, viewport rectangle, output extent,
  depth convention, scene identity, successful-state sequence, and a bitmask of
  discontinuity causes. Matrix convention and precision match `FSceneView`.
- The renderer maintains its own monotonic render-submission serial. Temporal
  ordering does not depend on `GFrameCounter`, wall time, swapchain image index,
  output pointer identity, or the number of game ticks.
- Previous metadata is the last successfully completed submission for the same
  state, after `FitViewToOutput`. Shadow views and feature-internal views never
  advance the main view state.
- `Begin` does not mutate committed previous state. `Commit` runs only after
  the complete view reaches `ERenderViewResult::Success`; invalid output,
  unavailable renderer/environment resources, failed passes, or aborted
  recording leave the previous state and successful sequence unchanged.
- A state may have feature-local pending resources during one submission.
  Commit publishes them atomically; abort discards pending candidates and
  preserves the last-known-good committed history.

### Discontinuity and invalidation

- `FSceneView` gains an explicit camera-cut/discard-history signal. Teleports,
  camera possession changes, cinematic cuts, and callers that cannot preserve
  continuity set it; the renderer does not guess cuts from movement magnitude.
- The foundation reports at least: first use, explicit camera cut, scene change,
  output/viewport extent change, projection change, depth-convention change,
  inactive-gap expiry, device invalidation, manual invalidation, and missing
  state.
- First use, explicit cut, scene change, depth-convention change, device
  invalidation, manual invalidation, and expired inactivity are hard resets.
  Extent and projection changes are reported distinctly and default to invalid
  history; later features may support resampling only through a documented
  feature-specific policy.
- A failed render does not itself discard committed history. The next attempt
  receives the elapsed submission gap and the original previous state, unless a
  hard invalidation cause also occurred.
- Device invalidation releases all feature GPU history before retry while
  preserving the public ID. Manual invalidation targets one owner or all states
  through Renderer-owned APIs and never requires callers to access private data.
- Stage 0 freezes a finite inactive-submission threshold and whether disabled
  temporal consumers retain or shed feature history before implementation.

### Stateless behavior and compatibility

- An invalid state ID preserves current rendering output and existing call
  sites. Temporal context reports no valid history, and no persistent allocation
  is created implicitly.
- Picking, one-shot thumbnails, qualification helpers, and synthetic direct
  views remain stateless unless they explicitly create and retain an owner.
- Adding state must not change current matrices, raster output, pass ordering,
  presentation, or resource fallback when no temporal consumer exists.

## Current Foundations and Gaps

| Area | Existing foundation | Gap closed by this plan |
| --- | --- | --- |
| View submission | `FSceneView` is copied safely into queued render commands and fitted to the output on the render thread. | No persistent identity, discontinuity input, previous metadata, or successful sequence exists. |
| Renderer boundary | `IRendererModule` owns scene creation/rendering while concrete `FSceneRenderer` remains private. | No analogous public owner/deleter contract exists for view state. |
| Thread/lifetime | Renderer scenes use deferred destruction and render commands retain submitted values/resources. | No view-state create/use/remove ordering or shutdown audit exists. |
| Resource recovery | Renderer resource generation, invalidation, retry, and last-known-good patterns exist. | Per-view feature histories have no device/manual invalidation hook or transactional publication owner. |
| Viewport | `FSceneViewport` already persists for one logical main or auxiliary output and is retained by render submissions. | It owns no renderer view-state token and `BuildSceneView` supplies no ID. |
| Validation | Current view fitting, constrained viewports, renderer failure, backlog, and shutdown fixtures exist. | No continuity, isolation, cut, failed-commit, stale-ID, or state-removal fixtures exist. |

## Implementation Stages

### Stage 0: Freeze identity, lifetime, and temporal semantics

- [x] Inventory every `FSceneView` producer and direct `RenderView` caller,
  including main/auxiliary viewports, thumbnails, picking, preview scenes, and
  qualification fixtures; classify each as persistent or stateless by default.
- [x] Freeze `FSceneViewStateId`, owner creation/release, queued lifetime,
  renderer-registry ownership, module shutdown order, and stale/foreign-ID
  behavior.
- [x] Freeze the temporal context fields, final-fitted matrix convention,
  submission serial, begin/commit/abort sequence, and the exact success point.
- [x] Freeze discontinuity flags, hard-reset table, inactive threshold,
  projection/extent policy, manual/device invalidation, and diagnostics.
- [x] Define the private strongly typed feature-substate extension pattern and
  explicitly reject a public generic history cache.
- [x] Record unchanged stateless output references and focused native-test
  targets before modifying production code.

#### Acceptance Gate

- Identity, owner lifetime, thread access, previous-state selection, commit and
  abort, reset causes, expiry, shutdown, and stateless behavior are unambiguous.
  No temporal algorithm, jitter, velocity, cloud, or generic-cache decision can
  enter implementation.

### Stage 1: Add opaque ownership and renderer-private state storage

- [x] Add RenderCore ID/owner types and `IRendererModule` create/release support
  with move-only ownership and an invalid sentinel.
- [x] Add the Renderer-private registry and state object with process-unique ID
  allocation, render-thread assertions, lookup, queued removal, live-count
  diagnostics, and module-shutdown auditing.
- [x] Add optional state ID and explicit discard-history input to `FSceneView`
  while preserving aggregate/default construction and safe command capture.
- [x] Reject duplicate, stale, foreign, released, and invalid use
  deterministically without creating implicit state or changing current output.
- [x] Add focused identity, move, release-order, queued-use, isolation,
  stale-ID, and shutdown tests.

#### Acceptance Gate

- Multiple owners remain isolated; queued renders cannot observe premature
  removal; stale IDs cannot alias; all private mutations occur on the render
  thread; and stateless callers retain byte-identical output.

### Stage 2: Implement transactional previous-view state

- [x] Add the monotonic render-submission serial and immutable temporal-context
  preparation after final view fitting.
- [x] Store the frozen previous-view metadata and successful-state sequence in
  the private state, with explicit begin, commit, abort, and pending-candidate
  cleanup.
- [x] Detect and report the frozen discontinuity causes and apply hard-reset,
  expiry, manual-invalidation, and device-generation policies.
- [x] Ensure only the outer successful `RenderView` commits; shadow/internal
  views, failed early exits, failed feature passes, and aborted commands do not.
- [x] Add tests for first/continuous frames, cuts, scene/depth/projection/extent
  changes, inactive gaps, failure then recovery, manual/device invalidation,
  and serial wrap/overflow policy.

#### Acceptance Gate

- Each submission sees exactly the last committed successful fitted view or an
  explicit invalid history reason. Failed renders never advance or erase valid
  state accidentally, and resets never expose stale feature history.

### Stage 3: Integrate persistent scene viewports

- [x] Give each main and auxiliary `FSceneViewport` one owner created through
  the active Renderer module and release it before renderer shutdown.
- [x] Attach the viewport's ID to Engine-built views before enqueue; keep the
  viewport retained until rendering completes and preserve stateless fallback
  when owner creation is unavailable.
- [x] Propagate explicit camera cuts from viewport/camera lifecycle events such
  as initialization, level/scene replacement, teleport/focus operations that
  discard continuity, and caller-requested reset.
- [x] Keep thumbnails, picking, preview scenes, and direct qualification calls
  stateless by default; add explicit opt-in only where the Stage 0 inventory
  justifies persistence.
- [x] Add main/auxiliary isolation, resize, scene replacement, viewport
  destruction with backlog, renderer restart, and multi-viewport tests.

#### Acceptance Gate

- A logical scene viewport preserves one state across normal redraws, distinct
  viewports never share history, cuts and scene changes invalidate explicitly,
  destruction is ordered after queued use, and non-persistent views allocate no
  state.

### Stage 4: Qualify extension and lifecycle behavior

- [x] Add a Renderer-private test-only strongly typed history probe that owns
  CPU metadata and an RHI texture candidate, then prove begin/commit/abort,
  last-known-good retention, reset, device release, and destruction ordering.
- [x] Run focused RenderCore, Renderer, Engine viewport, rendering-thread,
  resource-recovery, and runtime tests using repository workflows.
- [x] Compare stateless and stateful-with-no-consumer output across forward,
  deferred, constrained, offscreen, and window-backed paths.
- [x] Exercise render backlog, repeated failed frames, inactivity expiry,
  manual invalidation, device invalidation/retry, module shutdown, and leaked
  owner diagnostics.
- [x] Publish lasting view-state ownership and temporal-context contracts, then
  close this plan only when every gate has evidence.

#### Acceptance Gate

- The complete matrix passes; the typed probe proves safe future GPU-history
  ownership without a public cache; existing non-temporal pixels and pass order
  are unchanged; and shutdown has zero live private states or queued removals.

## Validation Matrix

| Layer | Required evidence |
| --- | --- |
| Pure contract | Unique/non-reused IDs, invalid sentinel, move-only owner, discontinuity table, sequence and expiry arithmetic, begin/commit/abort transitions. |
| Render thread | Create/use/remove FIFO ordering, thread assertions, backlog retention, stale/foreign rejection, registry isolation, and shutdown audit. |
| Temporal metadata | Final fitted current/previous matrices, scene/output/depth identity, success-only advancement, failed-frame recovery, cuts, resize, projection change, and inactivity. |
| Feature extension | Test-only strongly typed CPU/GPU history publishes transactionally, retains last-known-good data, resets, releases on device invalidation, and never enters RenderCore. |
| Engine integration | Main and auxiliary viewport persistence/isolation, camera/level cuts, destruction ordering, renderer unavailable fallback, and direct stateless callers. |
| Rendering parity | No-consumer stateful and stateless outputs, command counts, pass order, presentation, and diagnostics remain equivalent. |
| End to end | Repeated runtime redraws advance one successful sequence per logical viewport and release every state cleanly at shutdown. |

## Definition of Done

- Every Stage 0 decision is recorded and every stage acceptance gate passes.
- Persistent identity and previous-view metadata are optional, explicit,
  renderer-owned, render-thread mutated, and transactionally committed.
- `FSceneViewport` supplies stable state without making direct render callers
  persistent accidentally.
- A typed private probe demonstrates future feature GPU-history lifecycle while
  RenderCore remains feature-agnostic.
- Existing rendering is unchanged when no temporal consumer is enabled.
- Lasting contracts are published and plan/document validators pass.

## Deferred Follow-ups

- Motion-vector production and previous local-to-world transforms.
- Projection jitter and temporal sample sequences.
- TAA/temporal upscaling and history rejection/filtering.
- Volumetric-cloud, GTAO, exposure, denoising, or reflection histories.
- Stereo/multiview grouping and history-resampling policies.
- Per-feature memory budgets, eviction telemetry, and pooled history resources.

## Related Documentation

- [Runtime lifecycle](../Runtime/Core/RuntimeLifecycle.md)
- [Viewport rendering](../Runtime/Rendering/ViewportRendering.md)
- [Render resource lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [Renderer resource recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Volume texture foundation plan](VolumeTextureFoundation.md)
- [Agent build and run workflow](../Agents/BuildAndRun.md)
- [Agent testing workflow](../Agents/Testing.md)

## Related Code

- [`SceneView.h`](../../Engine/Source/Runtime/RenderCore/Public/SceneView.h)
- [`SceneOwnership.h`](../../Engine/Source/Runtime/RenderCore/Public/SceneOwnership.h)
- [`IRendererModule.h`](../../Engine/Source/Runtime/RenderCore/Public/IRendererModule.h)
- [`RendererModule.cpp`](../../Engine/Source/Runtime/Renderer/Private/RendererModule.cpp)
- [`SceneRenderer.h`](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.h)
- [`SceneRenderer.cpp`](../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp)
- [`SceneViewport.h`](../../Engine/Source/Runtime/Engine/Public/Client/SceneViewport.h)
- [`SceneViewport.cpp`](../../Engine/Source/Runtime/Engine/Private/Client/SceneViewport.cpp)
- [`Engine.cpp`](../../Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp)
