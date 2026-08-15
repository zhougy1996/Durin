# PIE Rendering Diagnostics Plan

Summary: Add viewport-owned rendering diagnostics to embedded and new-window PIE without weakening runtime world isolation or editor mutation locks.

Last reviewed: 2026-08-16

Status: Completed
Completed: 2026-08-16

## Current Status

PIE correctly duplicates the editor Level into a transient runtime World and
switches rendering to the runtime camera. The Level Editor becomes read-only,
and `FLevelEditorViewportClient::CalcSceneView()` deliberately declines to
produce the editor-camera view while Play is active. `DEngine::BuildSceneView()`
then uses the active gameplay camera, but it copies `FSceneViewSettings` only
when a viewport client produced the matrices. The camera-fallback path therefore
renders with default settings.

The viewport toolbar also wraps camera, editing, and rendering controls in the
same read-only disable scope. As a result, embedded PIE cannot change Lit/Unlit,
Solid/Wireframe, shadow diagnostics, contact shadows, or FXAA. New-window PIE is
created with a null viewport client and has no persistent per-window render
policy at all.

All stages are complete. A deterministic regression first proved that
camera fallback discarded all non-default view settings. Engine now preserves
the viewport client's exact settings whether matrices come from that client or
the active gameplay camera. A pure toolbar-capability policy also records the
Editing, read-only, embedded-PIE, and new-window-PIE permission matrix.

The Level Editor separates editor mutation from render inspection, embedded
PIE retains the existing viewport policy, and new-window PIE owns an isolated
session client labeled `Play Window` in the render popup. `ViewportTests` pass
98 of 98 and `WorldTests` pass 104 of 104; the broader `fast-all` selection and
the full `all` build also pass. The visible editor PIE lifecycle smoke passed
embedded/new-window and Level Start/Editor Camera combinations, including the
new-window render-policy seed, isolation, and release assertions. Lasting
Viewport Rendering, PIE architecture, and user-guide contracts are updated.

## Goal

Allow a developer to inspect the live PIE World through supported rendering
diagnostics while the gameplay camera, simulation, pause/single-step behavior,
input capture, and editor read-only guarantees remain intact.

The completed workflow must support:

- changing supported view settings during embedded PIE;
- changing the active Play window's settings during new-window PIE;
- using the same controls while PIE is paused or single-stepping;
- retaining gameplay-camera matrices while only render policy changes;
- leaving actor/component/property mutation and editor-camera navigation locked;
- deterministic restoration and lifetime behavior across start failure, Stop,
  repeated sessions, panel closure, and editor shutdown.

## Scope

- Preserve viewport-client `FSceneViewSettings` when Engine camera fallback
  supplies gameplay matrices.
- Split Level Editor toolbar permissions into editor-interaction controls and
  render-diagnostic controls.
- Keep shading, rasterization, directional-shadow diagnostics, contact shadows,
  and FXAA adjustable during PIE.
- Keep runtime collision debug drawing adjustable for the transient Play World.
- Add a lightweight session-owned viewport client for new-window PIE.
- Route the toolbar's render controls to the viewport that currently displays
  gameplay and make that target visible in the UI.
- Cover embedded/new-window, playing/paused, startup failure, Stop, and repeated
  session behavior with native and visible validation.
- Update the lasting viewport-rendering, PIE architecture, and user workflow
  documents when implementation is qualified.

## Non-Goals

- Reproducing Unreal Engine's complete `UGameViewportClient`, LocalPlayer,
  split-screen, networked PIE, Show Flag registry, or console-variable system.
- Adding arbitrary buffer visualization, shader-complexity modes, GPU resource
  inspection, RenderDoc integration, or a general render-debug console in this
  plan.
- Allowing editor-camera flight, transform gizmos, selection mutation,
  drag/drop, asset editing, Details editing, Save, Undo, or Redo during PIE.
- Changing gameplay camera selection, possession, Play From Camera bootstrap,
  World duplication, runtime-change application, or input-capture policy.
- Making standalone Shipping builds expose editor-only diagnostics.
- Persisting new-window Play diagnostics as project settings.

## Design Decisions and Invariants

### Render policy remains viewport-owned

`FViewportClient` remains the owner of persistent `FSceneViewSettings`.
`FSceneViewport` continues to own output policy and target lifetime, while
`FSceneView` remains the immutable per-render submission snapshot. The World and
camera components do not acquire debug-view state.

`DEngine::BuildSceneView()` must initialize `OutView.Settings` from the supplied
viewport client before asking that client for matrices. If the client declines
and Engine resolves the active gameplay camera, the same settings remain in the
completed view. A null viewport client retains default settings. Client-produced
views keep the same effective settings as today.

All persistent setting mutation occurs on the game/UI thread. Render commands
capture the completed `FSceneView` by value; the render thread never reads a
mutable viewport client or PIE controller.

### Embedded PIE uses the existing viewport client in this plan

The embedded scene viewport keeps its existing
`FLevelEditorViewportClient`. During PIE that client still declines to supply
editor-camera matrices, so Engine camera fallback remains authoritative. Its
`FSceneViewSettings`, however, continue to define how that gameplay view is
rendered.

This is a deliberate current-stage bridge: it fixes the missing policy without
introducing runtime client swapping and raw-pointer lifetime risk into the
embedded panel. A later general game-viewport design may replace it with a
dedicated Play client without changing the Engine fallback invariant.

Changes made during embedded PIE remain on that editor viewport after Stop,
because it is the same viewport policy before, during, and after Play. This
matches the existing per-viewport persistence model and lets a developer compare
the PIE and restored editor views under the same diagnostic mode.

### New-window PIE owns a lightweight Play client

`DEditorEngine` owns one lightweight `FViewportClient` for the new-window Play
session. It is created before the window-backed `FSceneViewport`, seeded from
the previous main scene viewport client's settings when available, and passed
to `FSceneViewport::CreateWindowBacked()` instead of `nullptr`.

The lightweight client need not produce camera matrices in this plan; Engine
camera fallback remains responsible for the gameplay view. The client exists to
own the independent window render policy. It is destroyed only after the Play
viewport no longer references it and after the editor main viewport has been
restored. Start failure, window close, Stop, and shutdown use the same teardown
ordering.

New-window settings are session-local. They start as a copy of the editor
viewport settings and are discarded when the window closes. They do not mutate
the inactive editor viewport.

### UI capability is separated from document read-only state

`Context.bReadOnly` continues to disable every operation that can mutate editor
content or the editor camera. It no longer disables the complete viewport
toolbar as one block.

The toolbar receives separate targets:

- an editor-interaction client, used for camera, edit modes, transforms,
  snapping, grid, and other editor-only controls;
- a render-settings client, used only for `FSceneViewSettings` diagnostics.

Outside PIE both targets resolve to `FLevelEditorViewportClient`. During
embedded PIE the render target remains that client. During new-window PIE it
resolves to the session-owned Play client. The view-mode button or popup must
label the latter target as `Play Window` so a control in the editor window does
not silently affect another surface.

The allowed PIE set is explicit:

- Lit/Unlit and Solid/Wireframe;
- directional-shadow quality and existing shadow diagnostic modes;
- contact-shadow enable/debug state;
- FXAA;
- collision debug drawing on the transient Play World.

World Grid, editor camera position/speed/distance, edit modes, transforms,
snapping, and editor assistance remain disabled or hidden during PIE. Changing
render diagnostics must not capture gameplay input. A developer first releases
mouse capture through the existing Escape behavior, then interacts with the
toolbar.

### State, failure, and restoration are deterministic

- Paused PIE is still Play for permissions and render-setting target resolution.
- Single-step consumes the settings snapshot current for that rendered frame.
- A failed session start cannot publish or retain a Play render target.
- A missing source viewport client seeds the Play client with default settings.
- Stop restores the previous main viewport before releasing the new-window
  viewport and its client.
- Repeated sessions receive fresh new-window clients and no settings retained
  from a prior Play window.
- Closing or hiding the Level Editor panel must not leave a viewport pointing at
  a destroyed client; the implementation must preserve the existing ownership
  gate and Engine-held viewport lifetime contract.
- Unsupported diagnostic combinations retain the renderer's existing fallback
  behavior; this plan does not introduce a second validation or coercion layer.

## Current Foundations and Gaps

Existing foundations:

- `FViewportClient` already owns `FSceneViewSettings` and exposes get/set APIs.
- `FSceneView` already carries one immutable settings snapshot to rendering.
- `FLevelEditorViewportClient` already owns the editor viewport's settings.
- The Level Editor toolbar already exposes the initial supported modes and
  diagnostics.
- PIE already distinguishes embedded and new-window destinations, restores the
  previous main viewport, and has bounded mouse-capture teardown.
- The runtime World already owns transient collision debug-draw state.
- Viewport, renderer-scene, PIE lifecycle, mouse-capture, and visible smoke-test
  foundations already exist.

Gaps to close:

- Engine camera fallback drops viewport-client settings.
- The toolbar conflates read-only content with read-only render policy.
- Toolbar rendering controls are typed around the Level Editor client instead
  of a render-policy capability.
- The new Play window is created with a null viewport client.
- There is no UI indication or routing for a different active render-settings
  target.
- Tests do not qualify fallback settings, PIE-time control permissions, or
  per-destination setting isolation and restoration.

## Implementation Stages

### Stage 0: Contract and regression baseline

- [x] Add focused tests demonstrating that camera fallback currently loses a
  non-default `FSceneViewSettings` value.
- [x] Add presentation-policy tests that enumerate which toolbar capabilities
  remain enabled for Editing, embedded PIE, paused embedded PIE, and new-window
  PIE.
- [x] Record the selected embedded persistence and new-window session-isolation
  policies in test names and fixtures rather than relying only on UI snapshots.
- [x] Confirm the smallest existing native suites for Engine view construction,
  viewport interaction, Level Editor presentation, and PIE lifecycle using
  [Agent Testing Workflow](../Agents/Testing.md).

#### Acceptance Gate

- The regression tests fail for the specific missing fallback-settings and
  monolithic-toolbar-disable behavior, while existing targeted suites remain
  green.
- Test fixtures can exercise settings routing without creating an OS window or
  requiring a live GPU for policy-only assertions.

### Stage 1: Preserve settings through camera fallback

- [x] Change `DEngine::BuildSceneView()` so a non-null viewport client's
  settings are applied whether matrices come from the client or active-camera
  fallback.
- [x] Preserve default settings for null-client standalone and legacy paths.
- [x] Cover client view success, client decline plus valid gameplay camera,
  client decline without a camera, null client, and independent main/auxiliary
  viewport cases.
- [x] Verify render-command capture still owns a value snapshot and introduces
  no viewport-client access on the render thread.

#### Acceptance Gate

- Native Engine viewport tests prove exact non-default settings survive the
  fallback path and do not leak between viewports.
- Existing camera aspect-ratio, projection, auxiliary viewport, and renderer
  scene-view tests remain unchanged in behavior.

### Stage 2: Enable bounded embedded-PIE diagnostics

- [x] Extract a testable toolbar capability policy from `Context.bReadOnly` and
  PIE destination/state.
- [x] Split render-settings controls from editor camera, transform, edit-mode,
  snapping, grid, drag/drop, and other mutation controls.
- [x] Keep the explicit allowed PIE diagnostic set enabled in Playing and
  Paused states.
- [x] Route collision overlay changes to the transient Play World and prove the
  editor World is not mutated.
- [x] Ensure opening or using a render popup cannot start viewport navigation,
  selection, drag/drop, or gameplay mouse capture.
- [x] Verify Stop keeps embedded diagnostic settings on the restored editor
  viewport and restores ordinary edit permissions.

#### Acceptance Gate

- In embedded PIE, each allowed diagnostic visibly changes the gameplay-camera
  render while actor/property editing and editor-camera movement remain locked.
- Pause and single-step retain the chosen diagnostic mode.
- Escape releases gameplay capture, toolbar interaction succeeds, and clicking
  back on the game surface follows the existing recapture contract.
- Native presentation and interaction tests cover the permission matrix and
  input exclusion boundaries.

### Stage 3: Add independent new-window render policy

- [x] Add the session-owned lightweight Play viewport client to
  `DEditorEngine` with explicit construction and teardown ordering.
- [x] Seed it from the previous main viewport client's settings, or defaults
  when no client is available.
- [x] Create the window-backed Play viewport with that client and expose only a
  bounded render-settings target accessor needed by Level Editor presentation.
- [x] Route render controls to the Play client while new-window PIE is active and
  label the target `Play Window`.
- [x] Keep the editor viewport settings unchanged while the Play window is
  active, and discard the Play client's later changes on Stop.
- [x] Cover successful start, bootstrap failure, native-window close, explicit
  Stop, repeated sessions, and editor shutdown.

#### Acceptance Gate

- New-window PIE begins with the editor viewport's diagnostic settings, accepts
  later independent changes, and leaves the editor viewport unchanged.
- Stop and window close restore the exact previous main viewport with no stale
  client pointer, retained window policy, lost input release, or lifecycle
  regression.
- Repeated-session and failure-injection tests pass without carrying settings
  from one Play window to the next.

### Stage 4: Rendering qualification and lasting documentation

- [x] Add or extend rendering integration coverage for at least one shading or
  raster mode and one shadow diagnostic through embedded PIE camera fallback.
- [x] Qualify new-window output through the smallest practical visible or RHI
  integration path without depending on screenshot color alone for all policy
  assertions.
- [x] Run the targeted native suites, then the required broader validation from
  [Agent Build and Run Workflow](../Agents/BuildAndRun.md) and
  [Agent Testing Workflow](../Agents/Testing.md).
- [x] Run the editor visible smoke path for embedded/new-window, Play/Pause/Step,
  mouse release/recapture, Stop, and repeated sessions.
- [x] Update the lasting contracts in Viewport Rendering and Play In Editor
  Architecture, and update the user-facing Play In Editor guide.
- [x] Record final evidence, close every acceptance gate, set lifecycle metadata
  to Completed, and leave archival to the standard monthly workflow.

#### Acceptance Gate

- All validation-matrix rows pass in the supported configuration with no new
  validation diagnostics.
- Lasting documents describe ownership, setting lifetime, user controls, and
  embedded/new-window differences without depending on this plan.
- No TODO, test-only bypass, default-off implementation path, or undocumented
  diagnostic target remains.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Engine policy | Exact `FSceneViewSettings` propagation for client-built, camera-fallback, and null-client views |
| Viewport isolation | Two clients keep independent settings; auxiliary views do not inherit main/PIE policy |
| Embedded PIE | Gameplay camera retained; allowed modes render; settings persist after Stop |
| New-window PIE | Initial copy, independent mutation, discard on Stop, previous viewport restoration |
| Permissions | Actor/property/editor-camera/edit-mode mutations remain unavailable in Playing and Paused states |
| Runtime debug | Collision debug changes only the transient Play World |
| Input | Escape release, toolbar interaction, recapture, focus loss, Pause, Stop, and window close remain coherent |
| Lifecycle | Start failure, repeated sessions, panel closure, and shutdown leave no stale client or window references |
| Rendering | At least one geometry view mode and one shadow diagnostic visibly qualify through gameplay-camera fallback |
| Regression | Existing projection, renderer, viewport interaction, PIE lifecycle, and visible editor smoke coverage remains green |
| Documentation | Changed scope and all-plan validators pass; lasting contracts and guide are updated |

## Definition of Done

- Embedded and new-window PIE both have an explicit non-null render-policy owner
  whenever the active surface is expected to accept diagnostics.
- Gameplay camera fallback preserves the active viewport client's complete
  `FSceneViewSettings` snapshot.
- The Level Editor distinguishes render inspection from editor mutation, with
  the exact supported controls available during Playing and Paused PIE.
- New-window policy is independently owned, visibly targeted, and released in a
  safe order.
- Embedded persistence, new-window isolation, null fallback, input boundaries,
  and lifecycle failure paths are covered by deterministic native tests.
- Rendering integration and visible smoke evidence demonstrate that policy
  changes reach the actual PIE frame.
- Viewport Rendering, Play In Editor Architecture, and Play In Editor guide are
  authoritative for the finished behavior.
- Required documentation, plan, build, and test validation passes, and the
  successful implementation is committed with exact plan/stage provenance.

## Deferred Follow-ups

- A general runtime/game viewport client shared by standalone, embedded PIE,
  new-window PIE, split-screen, and future multiplayer instances.
- A data-driven View Mode and Show Flag registry comparable to Unreal Engine's
  higher-level mode plus lower-level flag separation.
- Runtime console commands and presets for render diagnostics and scalability.
- Buffer, GBuffer, material, light-complexity, overdraw, LOD, Nanite-equivalent,
  and GPU-memory visualization tools.
- Eject/Possess or Simulate-In-Editor camera workflows for inspecting the live
  runtime World from a free editor camera.
- RenderDoc capture integration and in-editor GPU event/resource inspection.
- Project or user-session persistence policies for diagnostic presets.

## Related Documentation

- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Play In Editor Architecture](../Editor/Architecture/PlayInEditorArchitecture.md)
- [Play In Editor](../Editor/Guides/PlayInEditor.md)
- [Viewport Editing Architecture](../Editor/Architecture/ViewportEditing.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Client/ViewportClient.h`
- `Engine/Source/Runtime/Engine/Public/Client/Viewport.h`
- `Engine/Source/Runtime/Engine/Public/Client/SceneViewport.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/LevelEditorViewportClient.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.h`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportInteractionTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportCustomizationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/NativeGameplayCoreTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/RendererSceneViewTests.cpp`
