# Viewport Statistics Panel Plan

Summary: Turn the scene viewport FPS badge into the entry point for a compact, per-view rendering-statistics panel backed by exact render-thread snapshots.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

All stages are complete. RHI retains a monotonic count of non-empty indexed and
non-indexed graphics draws across immediate submissions and finite command-list
moves. RenderCore exposes a bounded `FSceneViewStatistics` value, Renderer
reduces its private counters to the stable metric contract, and
`FRendererModule::RenderView` returns an exact draw-count delta only for a
successful submission. Failure clears the caller's output.

`FSceneViewport` owns a mutex-protected latest-only snapshot with revision and
availability. Engine render commands capture the exact shared viewport,
publish success or explicit unavailability after `RenderView`, and keep main,
window-backed, and auxiliary publication identities independent. The FPS badge
is an ImGui activation target, its expanded panel is right-aligned directly
below it, unreadable narrow layouts are suppressed, and the complete overlay is
excluded before drag/drop, selection, navigation, wheel handling, and PIE
capture. `ShowStatistics` round-trips through the existing `SceneViewport`
session map with a default of `false`.

Validation completed on the `windows-msvc-x64` Debug DurinEditor profile:

- `RHICommandListTests`: 64 passed.
- `RendererSceneContractTests`: 17 passed, including concurrent coherent reads,
  failure clearing, lifetime retention, and main/auxiliary isolation.
- `ViewportTests`: 95 passed; `EditorShellTests`: 37 passed.
- `SkyBoxVulkanIntegrationTests`: passed with a zero-geometry SkyBox view whose
  total draw count is non-zero, plus invalid-output statistics clearing.
- `ViewportQualificationTests --mode qualification`: passed.
- `fast-all`: every selected contract, feature, and infrastructure target
  passed.
- Full `all` build passed after the final source/test changes.
- The built DurinEditor remained running through an eight-second startup smoke;
  its runtime log records successful LevelEditor load, first present, default
  workspace readiness, and no error, fatal, assertion, or validation-error
  entries.

The lasting ownership, metric, auxiliary-view, and interaction contracts now
live in [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md).

Stage 4 replaced the originally listed ad-hoc pointer-driven scenario pass with
repeatable `ViewportTests`, viewport qualification, Vulkan render integration,
and an error-free editor startup smoke. The available agent runtime can launch
and observe the native editor process but does not provide a repository-owned
native UI driver for deterministic FPS-badge clicking. The automated coverage
directly exercises layout, isolation, input geometry, metric semantics, real
graphics command recording, failure, resize/lifecycle qualification, and module
startup, while leaving no unrecorded manual-only acceptance claim.

## Goal

Let an editor user click the existing FPS badge and immediately inspect the
current viewport's visible primitives, visible main-pass triangles, total
graphics draw calls, and useful mesh/light/shadow breakdowns without opening a
separate tool. Values must describe one complete render submission for that
viewport, remain internally consistent across the game/render-thread boundary,
and never include a camera-preview or unrelated auxiliary viewport.

## Scope

- Make the top-right FPS badge visibly hoverable and clickable.
- Toggle a compact panel right-aligned beneath the FPS badge and clipped or
  constrained to the scene viewport.
- Publish one bounded, public per-view statistics value from Renderer through
  Engine to the owning `FSceneViewport`.
- Count all non-empty graphics draw commands recorded during the selected
  `RenderView` call, including scene geometry, shadows, SkyBox, post-process,
  and editor-assistance passes.
- Report visible main-pass triangles without multiplying them by shadow passes.
- Show a concise summary followed by static mesh, spline mesh, skeletal mesh,
  terrain, light, and shadow details supported by the snapshot.
- Persist the expanded/collapsed preference in Level Editor session settings;
  missing or invalid settings default to collapsed.
- Preserve editing, camera navigation, PIE, resize, camera preview, and
  render-resource failure behavior.

## Non-Goals

- GPU timings, CPU frame breakdowns, memory usage, overdraw, shader complexity,
  pipeline cache diagnostics, or a graph/history view.
- A general profiler window, customizable metric layout, CSV export, capture
  sessions, thresholds, warnings, or telemetry upload.
- Counting ImGui's rendering of the editor shell; draw calls cover the scene
  viewport's Renderer submission only.
- Reporting source-asset triangle totals, hidden-level totals, or triangles
  rejected before the current view's final LOD and visibility selection.
- Replacing Tracy or changing Renderer pass ordering, batching, culling, LOD,
  materials, or output policy.
- Showing camera-preview statistics in the main panel or merging multiple
  viewport submissions into one value.

## Design Decisions and Invariants

### Interaction and layout

- The existing FPS badge remains continuously visible and is the sole toggle.
  Clicking it alternates the panel state; clicking elsewhere does not close the
  panel. The collapsed state is the default for new or missing settings.
- Hover uses the normal interactive-control affordance and a short tooltip such
  as `Show rendering statistics` or `Hide rendering statistics`. Keyboard focus
  and activation follow the same ImGui item as pointer activation.
- The panel's right edge aligns with the FPS badge and its top edge begins at a
  scaled gap below the badge. Width, padding, rounding, border, opacity, and text
  colors use MonaImGui scaling and theme values rather than fixed screen pixels
  or private colors.
- Layout is calculated from viewport bounds. On narrow viewports the panel
  clamps horizontally and uses the available width; when the minimum readable
  size cannot fit, the FPS badge remains available and the detail panel is
  suppressed rather than drawing outside the viewport.
- The FPS badge and expanded panel form one input-exclusion region. Their
  clicks, wheel activity, and hover cannot select actors, begin gizmo edits,
  capture PIE input, or start viewport camera navigation. The image remains the
  semantic viewport item for drag/drop and focus outside that region.
- The panel is an overlay, not a dockable window, popup, or layout participant.
  It does not resize the render target or camera content rectangle.

### Metric semantics

- `FPS` continues to use ImGui's smoothed UI-frame rate and is not claimed to
  be GPU time or the reciprocal of the sampled render submission.
- `Primitives` is the number of visible Renderer primitives after visibility
  selection for the sampled view. A secondary submitted count may be shown as
  detail, but the summary value is visible count.
- `Triangles` is the sum of selected static/spline, skeletal, and terrain
  triangles in the main scene pass at the selected LOD. Shadow-cascade triangle
  submissions are excluded from this headline so enabling shadows does not
  redefine scene geometric complexity. `Shadow triangles` is shown separately.
- `Draw calls` is the exact number of non-empty `Draw` and `DrawIndexed`
  commands recorded between entry to and return from that view's
  `IRendererModule::RenderView` call. It includes shadow, scene, SkyBox,
  post-process, and editor-assistance graphics passes, but excludes commands
  outside the view and excludes compute dispatches.
- Geometry draw breakdowns use successful draw counters. The panel must not
  derive total draw calls by summing feature counters because that would omit
  full-screen and assistance passes and would become fragile as passes evolve.
- Every displayed field has a stable definition in the public summary type.
  Renderer-private histograms, resource retry counters, and diagnostic-only
  vectors are not copied into the editor-facing contract.
- Large integer values use compact, locale-independent display formatting with
  enough precision to distinguish meaningful changes; tooltips may expose the
  exact integer. Formatting never changes the stored value.

### Ownership and thread boundary

- RenderCore owns a small value type for the stable per-view summary. Renderer
  maps its private `FViewRenderCounters` into that value only after preparation
  and command recording have reached the final result for the submission.
- RHI owns a monotonic recorded-graphics-draw counter on each command list.
  Empty draws rejected by existing `Draw`/`DrawIndexed` guards do not increment
  it. Move, reset, admission, and immediate-list reuse preserve the same
  lifetime semantics as the existing recorded-command count.
- Engine creates a render-thread-local output value for each enqueued viewport
  render, samples the draw-counter delta around `RenderView`, then publishes the
  completed value to the exact `FSceneViewport` captured by that command.
- `FSceneViewport` owns latest-only synchronized publication. The UI obtains one
  coherent copy; it cannot observe fields from different submissions. Publishing
  must not wait on the game thread, and reading must not wait for the render
  thread or flush rendering commands.
- Captured ownership keeps the publication target alive until its render command
  completes. Destroyed or replaced viewports cannot receive data through a raw
  dangling pointer, and late completion cannot publish into a new viewport.
- Main and auxiliary viewports have separate storage. The Level Editor panel
  reads only the `FSceneViewport` attached to its main `MViewport` display
  source; camera-preview renders never overwrite that snapshot.
- A failed or incomplete render result publishes an unavailable state rather
  than combining zero/default fields with the last valid snapshot. The UI keeps
  layout stable and displays `Statistics unavailable` until a complete snapshot
  arrives.

### Preference and lifecycle

- `FSceneViewportPanel` owns the expanded/collapsed UI state. Renderer and
  `FSceneViewport` collect and retain only the latest bounded snapshot
  regardless of whether the panel is open; toggling therefore has no resource
  rebuild and shows data immediately.
- `FLevelEditorSessionSettings` loads, applies, and saves the preference under
  the existing `SceneViewport` map. The setting is editor preference state, not
  level/package data, and toggling it never dirties an asset.
- No-level, hidden-panel, viewport-initializing, resize, embedded PIE, paused
  PIE, and new-window PIE transitions retain existing reset/capture behavior.
  The overlay draws only when the main viewport texture was drawn and a readable
  layout fits.

## Current Foundations and Gaps

| Area | Existing foundation | Gap |
| --- | --- | --- |
| FPS presentation | `DrawViewportFPSOverlay` already calculates and paints a themed top-right badge. | It is draw-list-only, creates no interactive ImGui item, owns no expanded state, and exposes no bounds to input routing. |
| Viewport input | `FSceneViewportPanel` already excludes toolbar regions and coordinates navigation, drag/drop, focus, and embedded PIE capture. | Statistics badge/panel hit regions do not exist and therefore cannot be excluded before viewport input is processed. |
| Renderer counters | `FViewRenderCounters` already contains visibility, triangle, light, terrain, shadow, and feature draw counters per `RenderView`. | The type is Renderer-private profiling detail, its global sink is not viewport-addressed, and it contains more unstable detail than editor UI should consume. |
| Exact draws | `FRHICommandListBase` rejects empty draws and exposes total recorded command count. | There is no dedicated graphics-draw counter from which a per-view delta can be taken. |
| Viewport ownership | `DEngine::RedrawViewports` enqueues main and auxiliary renders separately, and every `FSceneViewport` already owns its output identity. | Render commands do not return a coherent result to their originating viewport. |
| Preference storage | `FLevelEditorSessionSettings` already persists grid and camera speed under `SceneViewport`. | Statistics expansion is not loaded, applied, or saved. |

## Implementation Stages

### Stage 0: Freeze the public summary and overlay contract

- [x] Define the exact public fields and units for availability, submission
  identity, visible/submitted primitives, headline and per-family triangles,
  shadow triangles, total draw calls, per-family successful draws, selected
  lights, and shadow state.
- [x] Inventory every Renderer graphics draw site and prove that a command-list
  draw-counter delta spans SkyBox, shadows, geometry, post-process, and editor
  assistance without including the surrounding viewport begin/end or ImGui.
- [x] Extract pure formatting and layout inputs from the FPS overlay so badge,
  panel, hit region, minimum-readable size, and theme-scaled gap are testable
  without renderer state.
- [x] Record the precise input-routing order that makes the overlay item active
  before actor picking, gizmo, navigation, and PIE-capture decisions.

#### Acceptance Gate

- Metric definitions, public fields, draw interval, failure state, panel bounds,
  input exclusion, default state, and session key are unambiguous; no UI field
  depends directly on a Renderer-private type.

### Stage 1: Add exact per-view statistics production

- [x] Add the RHI recorded-graphics-draw counter and accessor, incrementing only
  for non-empty recorded `Draw` and `DrawIndexed` commands.
- [x] Cover counter behavior for indexed/non-indexed draws, instances, empty
  no-ops, command-list movement, reset/reuse, and both regular and immediate
  recording paths.
- [x] Add the bounded RenderCore public statistics value and an explicit
  synchronous `RenderView` output seam; keep Renderer-private diagnostics out
  of the module boundary.
- [x] Map final Renderer counters to the public summary and calculate headline
  triangle totals with checked or saturating arithmetic appropriate to the
  public field widths.
- [x] Sample the graphics-draw counter before and after each `RenderView` call
  and attach only the resulting delta to that view's completed summary.
- [x] Add focused Renderer tests for static, skeletal, spline, terrain, shadow,
  full-screen, assistance, empty-scene, and recoverable-failure summaries.

#### Acceptance Gate

- A synchronous Renderer call returns one self-consistent bounded summary, and
  its total draw count exactly matches all non-empty graphics draw commands
  recorded inside that call without relying on feature-counter summation.

### Stage 2: Publish latest snapshots to the owning scene viewport

- [x] Add latest-only synchronized statistics storage and coherent snapshot
  access to `FSceneViewport` without exposing Renderer-private headers.
- [x] Capture the exact viewport publication state in main, window-backed, and
  auxiliary render commands and publish only after `RenderView` returns.
- [x] Represent invalid output and renderer-resource failure as unavailable;
  prevent partial snapshots and stale success from masquerading as the current
  submission.
- [x] Test interleaved main/auxiliary publications, late command completion,
  viewport replacement/destruction, unavailable-to-valid recovery, and
  concurrent publish/read behavior without render-thread flushes.

#### Acceptance Gate

- Each viewport exposes its own latest complete snapshot, auxiliary camera
  previews cannot overwrite the main viewport, and lifecycle transitions have
  no raw-pointer race, mixed-generation read, or synchronous thread wait.

### Stage 3: Make the FPS badge an interactive statistics panel

- [x] Replace the passive FPS drawing path with one interactive ImGui item whose
  custom paint preserves the current badge appearance and exposes stable badge
  and panel bounds.
- [x] Add expanded state to `FSceneViewportPanel`, toggle it on FPS activation,
  and exclude the complete overlay bounds from selection, gizmo, navigation,
  drag/drop delivery, wheel, and embedded PIE capture.
- [x] Draw the summary rows for FPS, primitives, triangles, and draw calls, then
  compact mesh-family, terrain, light, and shadow details directly below the
  badge using theme/scaling APIs.
- [x] Add unavailable, initializing, narrow-viewport, and large-value formatting
  states; exact-value tooltips must not alter click or input-exclusion behavior.
- [x] Load, apply, and save `ShowStatistics` in the existing Level Editor
  `SceneViewport` session-settings map with a default of `false`.
- [x] Add focused layout/formatting/settings tests and verify DPI scaling,
  resize, panel reopening, no-level state, embedded PIE, paused PIE, and camera
  preview by inspection.

#### Acceptance Gate

- Clicking the right-top FPS badge toggles a readable panel immediately below
  it; reported data belongs to the main viewport, persists as a preference, and
  every overlay interaction is isolated from scene and gameplay input.

### Stage 4: Integrate, document, and validate end to end

- [x] Update the viewport-rendering contract with statistics ownership, metric
  semantics, thread publication, auxiliary isolation, and UI interaction.
- [x] Run the focused RHI, Renderer/Engine, and Level Editor test targets selected
  through the repository testing workflow.
- [x] Build the affected RHI, RenderCore, Renderer, Engine, and LevelEditor
  dependency chain through the repository build workflow without overlapping
  another native build.
- [x] Launch the editor and record an error-free startup/first-present smoke;
  cover mixed geometry, shadows, grid/gizmo assistance, resize, camera preview,
  PIE input, and preference behavior through focused and qualification tests.
- [x] Confirm through pure counter-contract and Vulkan integration evidence that
  full-screen/SkyBox work contributes to total draws while headline triangles
  remain independent from shadow submissions and cascade count.
- [x] Move lasting behavior into the viewport-rendering contract, mark this plan
  completed only after every acceptance gate passes, and run the all-plan and
  changed-document validators.

#### Acceptance Gate

- The feature builds and passes focused automated and editor runtime validation;
  documented metric definitions match observed behavior, and no viewport input,
  rendering, resize, auxiliary-view, or lifecycle regression remains.

## Validation Matrix

| Area | Automated evidence | Runtime/visual evidence |
| --- | --- | --- |
| RHI draw counter | Indexed/non-indexed, empty, instance, move, reset, regular/immediate command-list tests. | Values remain stable while toggling non-draw command activity. |
| Renderer summary | Fixture assertions for triangle families, visibility, shadows, successful feature draws, total draw delta, and failure availability. | Empty and mixed scenes show plausible totals; shadows affect shadow detail and total draws but not headline triangles. |
| Viewport isolation | Interleaved main/auxiliary and lifecycle publication tests. | Opening camera preview does not replace or oscillate main-panel values. |
| Thread safety | Concurrent publish/read stress or deterministic synchronization test without flush. | Rapid resize/open/close produces no hitch, race symptom, or stale-viewport publication. |
| Interaction | Pure bounds/format helpers and session round-trip tests. | Badge hover/click works at supported DPI; panel clicks never select, navigate, drag/drop, or capture PIE input. |
| Lifecycle | Unavailable/recovery and missing-setting tests. | No-level, initializing, paused/embedded/new-window PIE, close/reopen, and restart retain defined behavior. |

## Definition of Done

- The FPS badge toggles one theme-consistent panel directly beneath it and the
  preference round-trips through Level Editor session settings.
- Summary values have the metric semantics fixed in this plan and come from the
  latest complete main-viewport render submission.
- Total draw calls cover every non-empty graphics draw inside `RenderView`;
  headline triangles cover visible main-pass geometry exactly once.
- Main, window-backed, and auxiliary viewport snapshots are isolated and cross
  the thread boundary without a flush, raw lifetime hazard, or mixed snapshot.
- Overlay regions are excluded from every scene-editing and gameplay-input path.
- Focused tests, affected builds, runtime scenarios, documentation validation,
  and all stage acceptance gates pass with recorded evidence.
- Lasting contracts are documented outside this plan, lifecycle metadata is
  updated, and the implementation plus plan status are committed together with
  required plan/stage provenance.

## Deferred Follow-ups

- Per-pass GPU timings and CPU preparation/recording durations.
- Rolling graphs, min/max/percentile history, frame capture, export, and
  performance-budget warnings.
- User-selected metrics, multiple detail levels, or comparison between main and
  auxiliary viewports.
- Compute-dispatch counts and a general command-stream diagnostics panel.
- Source-scene or unloaded-asset complexity reports independent of visibility.

## Related Documentation

- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Agent Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../../../Agents/Testing.md)
- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)

## Related Code

- [`FSceneViewportPanel`](../../../../Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.h)
- [`SceneViewportPanel.cpp`](../../../../Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp)
- [`ViewportPresentation.h`](../../../../Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.h)
- [`ViewportPresentation.cpp`](../../../../Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.cpp)
- [`FLevelEditorSessionSettings`](../../../../Engine/Source/Editor/LevelEditor/Private/Settings/LevelEditorSessionSettings.h)
- [`LevelEditorSessionSettings.cpp`](../../../../Engine/Source/Editor/LevelEditor/Private/Settings/LevelEditorSessionSettings.cpp)
- [`FSceneViewport`](../../../../Engine/Source/Runtime/Engine/Public/Client/SceneViewport.h)
- [`SceneViewport.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Client/SceneViewport.cpp)
- [`DEngine::RedrawViewports`](../../../../Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp)
- [`SceneView.h`](../../../../Engine/Source/Runtime/RenderCore/Public/SceneView.h)
- [`IRendererModule`](../../../../Engine/Source/Runtime/RenderCore/Public/IRendererModule.h)
- [`FSceneRenderer`](../../../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneRenderer.cpp)
- [`FViewRenderCounters`](../../../../Engine/Source/Runtime/Renderer/Private/Renderers/SceneVisibility.h)
- [`FRHICommandListBase`](../../../../Engine/Source/Runtime/RHI/Public/RHICommandList.h)
- [`RHICommandList.cpp`](../../../../Engine/Source/Runtime/RHI/Private/RHICommandList.cpp)
