# Gameplay Mouse Capture Plan

Summary: Add deterministic click-to-capture and release behavior for embedded and new-window Play without allowing ImGui, focus changes, or session teardown to leave the cursor hidden or gameplay input misrouted.

Last reviewed: 2026-08-11

Status: Archived
Completed: 2026-08-11

## Current Status

All stages are complete. The focused pre-change baseline passed on 2026-08-11:
`WorldTests` passed 76/76 and `ViewportTests` passed 81/81. The mutation
inventory found no additional cursor or Play-session owner beyond the files
listed below.

The pre-change revision is
`7bfd5122a072772841c48a1232c77b4c312d4dbc`.

The frozen public names are `ECursorMode::{Free, Hidden, Captured}` plus
`FGenericWindow::SetCursorMode`, `GetCursorMode`, `SetCursorPosition`, and the
existing `SetCursor` shape selector. `DEngine` owns
`SetGameInputWindow`, `ClearGameInputWindow`, and `ResetGameInputMouse`.
`DEditorEngine` owns `EEditorMouseCaptureState::{Released, Captured,
Suspended}`, capture requests, focus notification, and synchronous release.

Inventory: MonaImGui is the only production caller of `SetCursor`; GLFW is the
only platform implementation. Gameplay enablement is mutated by `DEngine`,
standalone `DGameEngine`, `DEditorEngine`, and `FSceneViewportPanel`. Editor
Play start, rollback, pause, step, stop, new-window loss, and teardown all
converge on `DEditorEngine`; no second lifecycle owner or capture coordinator
exists. Test seams will use a derived `FGenericWindow` without GLFW and the
public coordinator requests without requiring an ImGui frame. Dependency
direction remains `Core -> ApplicationCore -> MonaCore/Engine -> DurinEd ->
LevelEditor`, while MonaImGui consumes only the generic window contract.

Stage 1 added the independent cursor-mode contract, GLFW disabled/raw-motion
capture with saved-position restoration, and per-window MonaImGui arbitration.
Stage 2 added weak authoritative-window routing and mouse-baseline reset.
Stage 3 added the editor capture coordinator, embedded/new-window click
integration, synchronous lifecycle cleanup, Escape consumption, and the
released-state viewport hint. `WorldTests` passed 79/79 and `ViewportTests`
passed 82/82 after the changes; native edge motion and interactive repetition
were qualified through the final visible runtime smoke.

Final evidence on the `windows-msvc-x64` Agent Build Profile:

- `WorldTests` passed 79/79 and `ViewportTests` passed 82/82.
- Changed-document validation and all-plan validation passed.
- A full `all` build succeeded and linked
  `Engine/Binaries/Win64/Debug/Runtime/DurinEditor/DurinEditor.exe`.
- The visible Sandbox PIE lifecycle smoke passed embedded and new-window Play
  from both Level Start and Editor Camera. Each of the four combinations ran
  ten real `FGlfwWindow` capture/release cycles (40 total), then exercised
  pause, single-step, stop, window retirement, and clean editor shutdown.
- Focused native transition coverage exercised Escape consumption, focus
  suspension/restoration, pause, close, repeated release, target replacement
  and expiry, unrelated target rejection, held-state cleanup, and first-delta
  suppression.

## Goal

Make mouse ownership during editor Play predictable: clicking the active game
surface captures and hides the cursor, `Escape` releases and restores it while
Play continues, and pause, focus loss, window closure, session stop, failure,
or teardown cannot leave the cursor captured. Captured movement must remain
continuous at window edges and events from other editor windows must not enter
the gameplay input snapshot.

## Scope

- A platform-neutral native-window cursor mode distinct from cursor shape.
- GLFW free, hidden, and captured mappings, including best-effort raw mouse
  motion when the platform supports it.
- One authoritative gameplay input window and explicit reset behavior when
  ownership changes.
- One editor-owned mouse-capture state machine shared by embedded and
  new-window Play.
- Click-to-capture, `Escape` release, focus-loss suspension, and deterministic
  pause, stop, failure, and teardown cleanup.
- Per-window arbitration that prevents the ImGui backend from overriding an
  active gameplay capture while preserving ordinary editor cursor shapes.
- Focused native coverage, editor runtime validation, and lasting documentation
  of the interaction contract.

## Non-Goals

- Input mapping assets, rebinding, controller, touch, multiple local players,
  networking, replay, or enhanced-input systems.
- Gameplay-authored policies for temporarily showing a cursor in menus,
  inventory screens, dialogue, or other in-game UI. The capture API must allow
  a later gameplay owner, but this plan implements editor Play ownership only.
- Confining a visible cursor to a rectangle, multi-monitor pointer policy, or
  changing fullscreen/window-mode behavior.
- Replacing GLFW, changing ImGui navigation, or redesigning Mona event
  propagation beyond the window identity required for gameplay routing.
- Automatically recapturing after `Escape`, pause, or focus restoration.
- Changing Play simulation, world duplication, camera, possession, or Sandbox
  movement behavior.

## Design Decisions and Invariants

### Platform window contract

- Cursor shape and cursor mode are separate concepts. `ApplicationCore` owns a
  per-native-window mode with the semantic values `Free`, `Hidden`, and
  `Captured`; cursor-shape selection cannot implicitly enter or leave capture.
- `Free` displays the selected cursor shape and permits ordinary absolute
  motion. `Hidden` suppresses cursor presentation without promising confinement
  or continuous edge motion. `Captured` hides and locks the pointer and emits
  continuous relative movement semantics.
- GLFW maps `Captured` to `GLFW_CURSOR_DISABLED`, not
  `GLFW_CURSOR_HIDDEN`. It enables `GLFW_RAW_MOUSE_MOTION` only when supported
  and disables raw motion before leaving capture.
- Entering capture records the current cursor position. Returning to `Free`
  restores that position through an explicit platform-window operation so the
  cursor reappears where the user initiated capture. Reapplying the current
  mode is idempotent.
- Window destruction and platform shutdown leave no capture restoration work
  pending. A dead native window is never retained solely by input ownership.

### Gameplay input authority

- `DEngine` owns at most one weak gameplay input window identity. The Engine
  event handler accepts keyboard, mouse-button, mouse-move, wheel, and focus
  events only from that window while gameplay input is enabled.
- Enabling input without a live authoritative window does not accept events.
  Clearing or replacing the window disables the old authority and resets held
  keys, held buttons, transitions, wheel delta, and mouse tracking.
- Capture entry resets only the mouse baseline and accumulated mouse delta;
  the first captured movement therefore cannot produce a jump from the last
  free cursor position.
- Focus events from unrelated editor or auxiliary windows cannot change
  `FGameInputState::IsFocused()`.
- The non-repeated `Escape` press that releases editor Play capture is consumed
  by the capture owner and is not exposed as gameplay input. Other keys retain
  existing gameplay semantics while capture is active.

### Editor Play state machine

- `DEditorEngine` owns one capture coordinator because it already owns Play
  destination, Play window, pause, failure rollback, and teardown. The Level
  Editor reports embedded viewport interaction; it does not directly mutate
  platform cursor state.
- The coordinator exposes three observable states:
  `Released`, `Captured`, and `Suspended`. `Suspended` records that focus was
  lost while captured, but focus restoration transitions to `Released`, never
  directly back to `Captured`.
- Embedded and new-window Play both begin `Released`. A primary-button click on
  the active game surface requests capture. This avoids hiding a cursor that is
  still over the Play toolbar and gives both destinations one learnable rule.
- In embedded Play, the game surface is the rendered viewport image excluding
  toolbar and popup interaction regions. In new-window Play, the game surface
  is the Play window client area.
- While `Captured`, gameplay input is enabled only if the authoritative window
  is focused and the session is actively playing. `Escape` releases capture
  without stopping or pausing Play.
- Pause, step completion, session stop, failed startup rollback, Play-window
  close, editor shutdown, and native-window focus loss release the platform
  cursor before invalidating the target window or world.
- Resume and focus restoration leave the coordinator `Released`; the next
  explicit click is required to recapture.
- Cleanup operations are idempotent and safe during partial startup. No code
  path may depend on a later UI frame to make the cursor visible again.

### ImGui ownership

- Cursor arbitration is per native window. A process-global
  `ImGuiConfigFlags_NoMouseCursorChange` is not used because it would disable
  cursor updates for unrelated editor platform windows.
- The MonaImGui backend may update cursor shape and free/hidden presentation
  only when the hovered native window is not in `Captured` mode.
- When gameplay releases capture, the platform cursor becomes visible
  synchronously; the next ImGui cursor update may then select the appropriate
  editor cursor shape.

### User feedback

- A released embedded Play viewport shows a concise `Click to capture mouse`
  affordance alongside the existing Play-state presentation. The hint is
  absent while captured and must not intercept the capture click.
- The Play-state border and toolbar remain usable after `Escape` releases the
  mouse. No modal dialog is introduced.

## Current Foundations and Gaps

- `FGenericWindow::SetCursor` currently combines shape and visibility, while
  `FGlfwWindow::SetCursor(EMouseCursor::None)` selects only
  `GLFW_CURSOR_HIDDEN`; there is no captured mode or cursor-position setter.
- `FGameInputState` already clears held and transition state when disabled or
  unfocused, but its mouse baseline can be reset only as part of the private
  full reset.
- `FEngineInputEventHandler` receives the source `FGenericWindow` for every
  event but currently ignores that identity and updates one global focus bit.
- embedded Play enables input from `Context.bReadOnly && bViewportFocused` in
  `FSceneViewportPanel`; it does not require a click or define release.
- new-window Play toggles input from `GetActiveTopLevelWindow() == PlayWindow`
  during `DEditorEngine::Tick`, but does not capture the cursor and still lets
  focus callbacks from other windows mutate input focus.
- MonaImGui updates the cursor of the hovered platform window every frame and
  has no per-window gameplay-capture exclusion.
- `DEditorEngine::TeardownPlaySession` disables gameplay input after releasing
  Play-window references, so it has no earlier cursor-restoration boundary.

## Implementation Stages

### Stage 0: Freeze the cursor and input-ownership contracts

- [x] Record the focused `WorldTests` and `ViewportTests` baseline at the
  pre-change revision.
- [x] Inventory every repository call to `SetCursor`, every GameInput enable or
  focus mutation, and every editor Play start, pause, stop, rollback, and
  window-close path; update this plan before Stage 1 if another owner exists.
- [x] Add test-facing platform-window and capture-coordinator seams that do not
  require a live GLFW window or ImGui frame.
- [x] Pin the exact public enum and API names in this plan and confirm that
  dependency direction remains `Core -> ApplicationCore -> MonaCore/Engine ->
  DurinEd -> LevelEditor`, with MonaImGui consuming only the window contract.

Dependencies: none.

#### Acceptance Gate

- Baselines and the complete mutation inventory are recorded in `Current
  Status`; no unresolved API, ownership, ordering, or test-seam decision can
  change Stage 1 public contracts.

### Stage 1: Separate cursor presentation from native capture

- [x] Add the platform-neutral cursor-mode and cursor-position contracts to
  `FGenericWindow`, retaining only the compatibility necessary for migrated
  callers.
- [x] Implement stateful, idempotent free, hidden, and captured behavior in
  `FGlfwWindow`, including supported raw motion and synchronous position
  restoration.
- [x] Migrate MonaImGui to set cursor shape/presentation through the separated
  API and skip captured windows without disabling other platform windows.
- [x] Add focused native tests using a test window for mode idempotence,
  capture precedence over cursor-shape requests, restoration ordering, and
  destruction-safe cleanup.

Dependencies: Stage 0 contract and test seam.

#### Acceptance Gate

- Platform-neutral tests pass; editor cursor shapes still work in ordinary UI;
  a captured GLFW window provides continuous motion at every client edge and
  returns a visible cursor to its saved position on release.

### Stage 2: Route gameplay input through one authoritative window

- [x] Add explicit gameplay input-window assignment and clearing to `DEngine`
  and filter all Engine input events by that identity.
- [x] Split mouse-baseline reset from full `FGameInputState` reset and invoke it
  on capture entry and input-window replacement.
- [x] Ensure unrelated-window focus, keyboard, mouse-button, move, and wheel
  events neither mutate nor consume gameplay input.
- [x] Add focused tests for target replacement, target expiry, focus loss,
  first-delta suppression, held-state cleanup, and unrelated-window isolation.

Dependencies: Stage 1 window contract.

#### Acceptance Gate

- The focused gameplay-input tests pass and demonstrate that exactly one live,
  focused target window can populate `FGameInputState`; changing authority
  cannot leave held input or a stale mouse delta behind.

### Stage 3: Integrate capture with both editor Play destinations

- [x] Implement the `DEditorEngine` capture coordinator and route every Play
  lifecycle transition through its idempotent release boundary.
- [x] Have `FSceneViewportPanel` report embedded game-surface clicks, focus,
  popup/toolbar exclusion, and the host native window without directly
  changing cursor mode.
- [x] Route new-window client clicks and focus through the same coordinator;
  closing that window must release before requesting native destruction.
- [x] Reserve non-repeated `Escape` for release while captured and leave Play
  active. Require another eligible click before gameplay input resumes.
- [x] Release on pause, step completion, focus loss, stop, startup failure, and
  editor teardown; resume or refocus in `Released` state.
- [x] Add the released-state viewport hint and ensure it does not consume the
  recapture click or cover existing Play controls.
- [x] Add coordinator and editor integration tests covering the transition
  table, partial startup, repeated release, and both destinations.

Dependencies: Stage 2 authoritative input routing.

#### Acceptance Gate

- `WorldTests` and `ViewportTests` pass. Automated transition coverage proves
  every exit from `Captured` restores the cursor before target invalidation,
  and manual embedded/new-window checks match the frozen interaction contract.

### Stage 4: Qualify user-visible behavior and publish the contract

- [x] Update the viewport-rendering and gameplay-control documentation with
  cursor ownership, click-to-capture, `Escape`, focus, pause, and teardown
  behavior.
- [x] Run the changed-document validator and the all-plan validator.
- [x] Run the smallest affected native targets after final edits, then complete
  the required full `all` build for the user-visible editor change.
- [x] Exercise the validation matrix in the editor executable produced by that
  same full build and record the executable path and results in this plan.
- [x] Set `Status: Completed`, fill `Completed`, and record final evidence only
  after every required gate succeeds.

Dependencies: Stage 3 acceptance gate.

#### Acceptance Gate

- All required tests, documentation checks, the full build, and editor runtime
  scenarios pass; lasting documentation owns the shipped behavior and no
  cursor remains captured after any tested exit path.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Window contract | Mock/test-window coverage for free, hidden, captured, idempotence, shape arbitration, raw-motion capability fallback, position restoration, and cleanup ordering |
| Input routing | Focused tests for the authoritative window, unrelated windows, target replacement/expiry, held-state reset, and zero first delta after capture |
| Capture state machine | Table-driven coverage for `Released`, `Captured`, and `Suspended` across click, `Escape`, focus loss/gain, pause/resume, step, stop, failure, close, and teardown |
| Embedded Play | Click captures only inside the game image; toolbar/popup clicks do not; edge motion remains continuous; `Escape` restores the cursor and toolbar access; recapture requires another click |
| New-window Play | Client click captures; editor-window events are isolated; `Escape`, Alt-Tab, pause, close, and stop restore the cursor before window destruction |
| Repetition | At least ten embedded and ten new-window capture/release cycles, followed by repeated Play start/stop, produce no stuck hidden cursor, stale held key, first-frame look jump, or dead-window access |
| Regression | `WorldTests` and `ViewportTests` pass at target granularity; expand only if implementation crosses another native-test target or shared test infrastructure |
| Documentation | Changed-document validation and `doc plan validate --scope all` pass |
| Final editor gate | Full `all` build succeeds, and the editor executable from that build passes every manual scenario above |

## Definition of Done

- Cursor shape, visibility, and native capture have explicit non-conflicting
  ownership.
- Embedded and new-window Play implement the same click-to-capture and
  `Escape`-to-release behavior.
- Mouse look remains continuous at native-window edges while captured.
- Only the authoritative Play window contributes gameplay input.
- Focus loss, pause, step completion, stop, failure, close, and shutdown restore
  the cursor synchronously and clear stale input.
- ImGui cannot override a captured window and retains normal behavior in every
  other editor window.
- Focused tests, the required full build, runtime scenarios, and documentation
  validation pass, with evidence recorded in this plan.

## Deferred Follow-ups

- A gameplay-facing cursor policy for runtime menus or UI-heavy games.
- Visible cursor confinement and configurable capture/release shortcuts.
- Standalone-game automatic capture policy and user settings; the shared
  platform and Engine contracts created here may support it later.
- Telemetry or diagnostics for capture transitions if runtime investigation
  shows a need after the deterministic contract lands.

## Related Documentation

- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Sandbox Gameplay](../../../Runtime/Gameplay/SandboxGameplay.md)
- [Scene Viewport Navigation](../../../Editor/Guides/SceneViewportNavigation.md)
- [Native Gameplay Core](NativeGameplayCore.md)
- [C++ Coding Standards](../../../Development/Standards/CodingStandards.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Input/InputCoreTypes.h`
- `Engine/Source/Runtime/ApplicationCore/Public/Window/GenericWindow.h`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GenericWindow.cpp`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.h`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp`
- `Engine/Source/Runtime/MonaCore/Public/Application/MonaEventHandler.h`
- `Engine/Source/Runtime/MonaCore/Private/Application/MonaApplication.cpp`
- `Engine/Source/Runtime/MonaImGui/Private/ImGuiMonaImpl.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/Engine.h`
- `Engine/Source/Runtime/Engine/Public/Input/GameInputState.h`
- `Engine/Source/Runtime/Engine/Private/Engine/Engine.cpp`
- `Engine/Source/Runtime/Engine/Private/Input/GameInputState.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/EditorEngine.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/EditorEngine.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Tests/Native/EngineTests/Private/World/NativeGameplayCoreTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/`
- `Engine/Tests/Native/EngineTests/CMakeLists.txt`
