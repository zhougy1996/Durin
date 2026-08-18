# Level Editor Viewport-First Layout Plan

Summary: Replace the Level Editor's panel-heavy default layout with a viewport-first workspace and a reusable MonaImGui bottom-drawer presentation primitive.

Last reviewed: 2026-08-18

Status: Completed
Completed: 2026-08-18

## Current Status

Implementation and required validation are complete. The version-6 default
layout gives Scene Viewport 76% of workspace width and stacks World Outliner
above Details in the 24% right column at a 35%/65% height split. Content Browser
and Console now share a transient MonaImGui bottom drawer; Activity History
starts closed and opens as a non-docked floating window.

MonaImGui owns the drawer's caller-owned state, bounded geometry, animation,
resize handle, focus-safe Escape behavior, and overlay window. LevelEditor owns
tool selection, status actions, panel embedding, focus requests, reset, reveal,
and unread Console policy. No vendored ImGui file or `imgui_internal.h` was
changed.

Validation evidence:

- `MonaImGui` and `LevelEditor` focused builds passed.
- The final `all` build passed for `Win64-Debug-DurinEditor`.
- `EditorShellTests` passed 42 of 42, covering drawer timing, geometry, overlay
  begin/end, scale bounds, panel defaults, and unread-count policy.
- `EditorAssetWorkflowTests` passed 77 tests with one registered skip across 78
  cases, preserving Content Browser model and workflow coverage.
- Hidden Sandbox startup and clean `--exit-after-ticks=180` shutdown returned
  exit code 0.
- A 2560x1380 offscreen capture verified the viewport-first default, right-column
  split, closed transient panels, and status-bar actions at the active host UI
  scale. Both semantic theme palettes and 75%-200% drawer scale bounds are also
  exercised by `UIStyleTests`.

## Goal

- Make the Scene Viewport the dominant default Level Editor surface without
  removing user-controlled docking.
- Keep World Outliner and Details continuously available in a predictable
  right-hand hierarchy.
- Make Content Browser and Console quickly accessible without permanently
  consuming viewport height.
- Make Activity History available on demand without opening it by default.
- Introduce a reusable, editor-agnostic bottom-drawer primitive in `MonaImGui`
  using upstream ImGui's supported window, child, focus, input, and drawing APIs.

## Scope

- A new Level Editor default dock tree and layout-version migration.
- Explicit initial-open policy for Level Editor panels.
- A `MonaImGui` bottom-drawer primitive with deterministic geometry, animation,
  focus, toggle, resize, and dismissal behavior.
- Level Editor ownership of drawer tool selection and lifecycle.
- Embeddable Content Browser and Console bodies that can render inside the
  drawer while retaining ordinary floating/dockable-window presentation where
  required.
- Status-bar entry points, tooltips, selected state, notification badges, and
  keyboard access for Content Browser, Console, and Activity History.
- Automated state/policy tests plus scaled, themed runtime validation.

## Non-Goals

- Modifying, forking, or patching ImGui docking or viewport internals.
- Replacing the existing workspace framework or `imgui.ini` persistence.
- Forcing a single immutable layout after the default has been created.
- Making the drawer a native operating-system window.
- Reworking Content Browser information architecture, asset operations, Console
  command semantics, notification storage, or viewport rendering.
- Adding drawers to Material, Texture, StaticMesh, or SkeletalMesh workspaces in
  this plan; the MonaImGui primitive must nevertheless remain reusable by them.
- Automatically opening Console when a warning or error is received.

## Design Decisions and Invariants

### Default workspace geometry

- The new default has one horizontal split: Scene Viewport on the left and one
  right tool column using approximately 24% of available width.
- The right column has one vertical split: World Outliner above Details.
  Outliner receives approximately 35% of the column height and Details receives
  the remainder.
- There is no default bottom dock node. Content Browser, Console, and Activity
  History are not assigned to the default dock tree.
- Reset Layout reconstructs this exact default and closes transient drawer
  state. User docking remains governed by the existing ImGui workspace class
  and saved-layout behavior after reconstruction.
- `Workspace::LayoutVersion` is incremented so existing installations receive
  the new default instead of silently retaining the version-4 dock tree.

### Drawer ownership boundary

- `MonaImGui` owns only presentation and interaction mechanics. It must not
  depend on LevelEditor types or know the meanings of Content Browser and
  Console.
- The primitive accepts a stable caller-owned ID, an anchor/content rectangle,
  bounded height policy, open/close requests, and presentation flags. It
  returns frame results such as visible extent, appearing/closing state,
  resize result, and dismissal request.
- LevelEditor owns which tool is selected, whether the drawer is open, tool
  switching, shortcuts, badges, and calls into the selected panel body.
- Drawer state is instance-owned and must not be stored in function-local
  statics or a process-global registry. Multiple future workspaces may use the
  primitive without sharing state.
- The implementation uses supported ImGui calls such as `SetNextWindowPos`,
  `SetNextWindowSize`, `Begin`, child regions, draw lists, and ordinary input
  queries. It must not include `imgui_internal.h` from the new public MonaImGui
  API and must not edit vendored ImGui sources.

### Drawer behavior

- The drawer overlays the bottom of the workspace content area above the status
  bar. Opening it does not split the dock tree or resize the Scene Viewport's
  render allocation.
- Only one drawer tool is visible at a time. Selecting the active status-bar
  action toggles the drawer closed; selecting the other drawer tool switches
  content without an intermediate close.
- The initial height is 36% of available workspace height, clamped to scaled
  minimum and maximum values. A top-edge resize affordance lets the user change
  the height for the current session.
- Open and close animation is time-based, uses a bounded duration, and derives
  geometry from the current anchor rectangle every frame. Reduced or skipped
  animation under an invalid/zero delta time must settle in a valid state.
- `Escape` closes the drawer when its contents own keyboard focus and no modal,
  popup, active text edit completion, or drag operation needs the key. Clicking
  the same status action also closes it. Ordinary loss of focus does not close
  it, so Content Browser drag-and-drop into the Scene Viewport remains viable.
- The drawer captures pointer input over its visible rectangle. Hidden or
  animating-out content must not leave an interactive invisible region.
- Opening Content Browser requests focus for its search/navigation surface;
  opening Console requests focus for the command input. Switching tools
  transfers focus once rather than every frame.
- Drawer open state is transient and is not restored across editor launches.
  The user-adjusted height may be added to Level Editor session settings only
  after Stage 0 confirms that persistence is desirable.

### Panel presentation and background work

- Content Browser and Console separate their window wrapper from reusable body
  drawing. A body is drawn exactly once per frame, either in the drawer or in an
  ordinary panel window, never both.
- Existing hidden/background contracts remain intact. Console continues polling
  bounded log history while hidden so its badge and next open are current.
  Content Browser retains its mounted-content reconciliation behavior without
  scheduling thumbnails for invisible items.
- Existing Content Browser reveal/open requests open the Content Browser drawer
  and reveal the requested item unless a user-visible Content Browser window is
  already active.
- A drawer header provides an explicit `Open in Window` action. It closes the
  drawer and opens the selected tool as a normal workspace-class window; user
  docking thereafter uses existing ImGui behavior.

### Status bar and Activity History

- The status bar keeps a stretch spacer between its two groups. `Content Drawer`
  and Console are left-side tool actions; workspace status, notification actions,
  and Activity History remain compact right-side items.
- Console displays a bounded unread warning/error badge. Receiving records does
  not steal focus or open the drawer; opening Console clears the relevant unread
  presentation count without clearing Console history.
- Activity History starts closed. Its status action opens and focuses a floating
  ImGui workspace window with a close button. It is not part of the drawer and
  is not docked by the default layout.
- “Floating window” means an ImGui window independent of the internal Level
  Editor dock tree. Native multi-viewport detachment remains whatever the host's
  existing ImGui configuration supports and is not required by this plan.
- The Window > Panels menu remains a recovery path for every tool surface.

## Current Foundations and Gaps

- `MLevelEditor::BuildDefaultLayout` already owns the complete default dock
  construction, but currently creates 20% left, 25% right, and 25% bottom
  regions and docks all panels open.
- `ILevelEditorPanel` already supports open/closed state and hidden ticking, but
  defaults every panel to open and has no explicit construction-time policy.
- `FEditorNotificationOverlay` already draws the bottom status bar and has an
  Activity History button that opens and focuses the history panel.
- Content Browser, Console, and Activity History already use stable workspace
  window identities through `WorkspaceUI::BeginDockablePanel`.
- Console already polls records through `TickWhenHidden`.
- Content Browser and Console currently combine their ImGui window wrapper and
  body rendering, so they cannot yet be hosted safely inside a shared drawer.
- MonaImGui currently has no bottom-drawer primitive or session-owned drawer
  state contract.

## Implementation Stages

### Stage 0: Freeze interaction and state policy

- [x] Create a small code-level state model for `Closed`, `Opening`, `Open`, and
  `Closing`, including tool-switch and resize transitions independent of ImGui
  rendering.
- [x] Confirm drawer height persistence policy; default to session-transient
  unless a concrete usability requirement justifies settings migration.
- [x] Confirm shortcut ownership and collision policy, with `Ctrl+Space` as the
  proposed Content Browser toggle and no default Console shortcut unless an
  existing editor command map provides one.
- [x] Define focus and Escape precedence for active text input, popups, modals,
  and drag-and-drop.
- [x] Record final base-unit geometry and animation duration as named policy
  constants rather than theme values.

#### Acceptance Gate

- The transition table has one deterministic result for toggle, switch,
  dismiss, resize, workspace deactivation, and Reset Layout events.
- No unresolved choice changes public MonaImGui API shape or session-settings
  format.

### Stage 1: Add the MonaImGui bottom-drawer primitive

- [x] Add public, editor-agnostic drawer state/config/result types and
  `BeginBottomDrawer`/`EndBottomDrawer`-style scoped presentation APIs in
  MonaImGui.
- [x] Anchor the drawer to a caller-provided rectangle and implement scaled
  height clamps, top-edge resize, clipping, animation, focus-safe dismissal,
  and input capture.
- [x] Keep all implementation outside `Private/ThirdParty/ImGui` and depend only
  on supported ImGui/MonaImGui public facilities.
- [x] Add debug assertions for mismatched begin/end calls, invalid bounds, and
  duplicate drawing of one drawer state in a frame.
- [x] Add focused tests for the pure transition and geometry policy where those
  decisions do not require a live ImGui renderer.

#### Acceptance Gate

- A minimal test host can open, resize, switch content in, and close a drawer at
  multiple host sizes without modifying a dock tree.
- Drawer geometry remains finite and within its anchor at 75%, 100%, 125%,
  150%, and 200% UI scale.
- The diff contains no changes under MonaImGui's vendored ImGui directory.

### Stage 2: Make Content Browser and Console embeddable

- [x] Split each panel into a stable outer window path and a reusable body path
  without duplicating model, selection, log, or command-input state.
- [x] Add one-shot focus requests appropriate to each tool.
- [x] Preserve Content Browser reveal, mounted-content reconciliation,
  thumbnail visibility, drag source, and play-mode disabling behavior in both
  presentations.
- [x] Preserve Console hidden polling, filters, completion, scrolling, clear,
  and command execution in both presentations.
- [x] Add `Open in Window` transitions that never draw a body twice in one
  frame and retain the tool's logical state.

#### Acceptance Gate

- Both tools can alternate between hidden, drawer, floating, and docked
  presentation without losing selection/filter/history state or asserting.
- Content Browser drag-and-drop from the drawer into Scene Viewport succeeds.
- Console receives records while hidden and shows them on its next presentation.

### Stage 3: Integrate the viewport-first Level Editor layout

- [x] Replace the left/right/bottom default dock construction with a large left
  Scene Viewport and a right column split between World Outliner and Details.
- [x] Increment `Workspace::LayoutVersion` and make Reset Layout clear transient
  drawer state before reconstructing the default.
- [x] Give Activity History, Content Browser, and Console explicit closed initial
  state while keeping Scene Viewport, World Outliner, and Details open.
- [x] Make `MLevelEditor` own selected drawer tool, activation requests, reset,
  workspace-deactivation behavior, and selected panel body drawing.
- [x] Route Content Browser reveal operations to the drawer when no separate
  Content Browser panel is visible.
- [x] Remove obsolete “select default bottom tab” behavior.

#### Acceptance Gate

- A fresh or reset layout contains only Scene Viewport on the left and the
  Outliner/Details stack on the right, with no bottom dock node.
- The first frame after workspace activation does not focus or open a hidden
  tool surface.
- Previously saved version-4 dock data cannot suppress creation of the new
  versioned default.

### Stage 4: Complete status-bar interaction and Activity History

- [x] Refactor the status bar layout to expose Content Drawer, Console, and
  Activity History actions with responsive labels/icons and selected states.
- [x] Add bounded Console unread severity accounting and accessible tooltip
  text without automatic opening.
- [x] Wire drawer toggles, tool switching, keyboard shortcuts, Escape, and
  `Open in Window` actions.
- [x] Open Activity History as a focused floating workspace window and keep its
  notification update/toast behavior independent of history-window visibility.
- [x] Preserve Window > Panels recovery commands and clarify whether selecting
  Content Browser or Console there opens a window rather than the drawer.

#### Acceptance Gate

- Every tool is reachable using both the status bar and Window menu after its
  close button has been used.
- Narrow status bars degrade to icons/overflow without overlap or clipping.
- Notifications and Console records never steal viewport focus.

### Stage 5: Validate and document the implemented contract

- [x] Add or extend focused native tests for drawer state transitions, Level
  Editor initial-open policy, Console unread accounting, and reset behavior.
- [x] Build the affected editor targets and run selected tests using the
  repository build and testing workflows.
- [x] Smoke-test fresh layout, migrated layout, Reset Layout, workspace
  switching, play mode, Content Browser reveal, asset drag/drop, Console input,
  Activity History, and shutdown.
- [x] Validate dark/light themes and 75%, 100%, 125%, 150%, and 200% UI scale at
  wide, compact, and narrow window sizes.
- [x] Move lasting drawer/style behavior into Editor UI Style and workspace
  ownership/persistence behavior into Editor Workspace Framework.
- [x] Record validation evidence, complete the plan, and leave archival to the
  normal monthly plan workflow.

#### Acceptance Gate

- Required builds, focused tests, and runtime smoke cases pass with recorded
  evidence.
- Lasting contracts exist in their owning documentation and this plan contains
  only implementation history and evidence.

## Validation Matrix

| Area | Automated validation | Runtime validation |
| --- | --- | --- |
| Drawer state | Toggle, switch, dismiss, reset, resize, zero-delta transitions | Repeated rapid toggles and tool switches |
| Geometry | Clamp and finite-extent policy across sizes/scales | Wide, compact, narrow, maximized, and resized host |
| Default layout | Layout policy and panel initial-open tests where separable | Fresh settings, version migration, Reset Layout |
| Content Browser | Existing model/refresh tests plus new presentation-state tests | Reveal, navigation, import entry, drag asset into viewport |
| Console | Hidden polling and unread severity tests | Command input, completion, filters, scroll, clear, new errors |
| Activity History | Open/focus state test where separable | Status action, close/reopen, toast/status independence |
| Focus/input | Transition policy tests | Search, command entry, Escape, popup, modal, drag/drop |
| Presentation | Documentation/style validation | Both themes at 75%, 100%, 125%, 150%, and 200% scale |
| Lifecycle | Reset/deactivation state tests | Workspace switch, play mode, close/reopen, shutdown |

## Definition of Done

- Scene Viewport is the dominant fresh and reset Level Editor surface.
- World Outliner is above Details in the right-hand default column.
- Content Browser and Console start hidden and are usable through the bottom
  drawer without changing the dock tree.
- Activity History starts hidden and opens as a focused floating window from the
  status bar.
- Content Browser reveal and drawer drag/drop workflows remain functional.
- Console remains current while hidden and reports unread severity without
  unsolicited focus changes.
- The reusable drawer lives in MonaImGui, contains no LevelEditor dependencies,
  and requires no vendored ImGui modification.
- Automated validation, affected builds, runtime smoke tests, theme/scale checks,
  and documentation validation pass.
- Lasting behavior is documented in the owning Editor design and architecture
  documents.

## Deferred Follow-ups

- Reusing the drawer in other editor workspaces after the Level Editor contract
  is proven.
- Multiple simultaneously pinned drawers or arbitrary user-defined drawer tabs.
- Persisting drawer height or last-selected drawer tool across launches.
- Native operating-system window guarantees for Activity History.
- A general application command/shortcut registry if Level Editor-local
  shortcuts expose broader ownership needs.

## Related Documentation

- [Editor UI Style](../Editor/Design/UIStyle.md)
- [Editor Workspace Framework](../Editor/Architecture/WorkspaceFramework.md)
- [Content Browser](../Editor/Architecture/ContentBrowser.md)
- [Editor Console](../Editor/Guides/Console.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/MonaImGui/Public/MonaImGui.h`
- `Engine/Source/Runtime/MonaImGui/Public/MonaImGuiBottomDrawer.h`
- `Engine/Source/Runtime/MonaImGui/Private/MonaImGui.cpp`
- `Engine/Source/Runtime/MonaImGui/Private/MonaImGuiBottomDrawer.cpp`
- `Engine/Source/Editor/DurinEd/Public/Editor/WorkspaceUI.h`
- `Engine/Source/Editor/DurinEd/Private/Editor/WorkspaceUI.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorWorkspace.h`
- `Engine/Source/Editor/LevelEditor/Private/Workspace/LevelEditorPresentationPolicy.h`
- `Engine/Source/Editor/LevelEditor/Public/Widgets/MLevelEditor.h`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/MLevelEditor.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/LevelEditorPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanelView.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ConsolePanel.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ConsolePanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/EditorNotificationOverlay.h`
- `Engine/Source/Editor/LevelEditor/Private/Widgets/EditorNotificationOverlay.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorWorkspaceTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/EditorNotificationTests.cpp`
