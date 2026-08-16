# Editor Main Window Custom Title Bar Plan

Summary: Replace the Windows editor main-frame caption with a Durin-rendered title bar while preserving native window interaction and keeping secondary windows system-decorated.

Last reviewed: 2026-08-16

Status: Completed
Completed: 2026-08-16

- 2026-08-16: The Windows editor main frame, shared native window bridge,
  integrated title/menu bar, authored branding, fallback path, and lasting
  contracts are complete. Focused and broader native tests, full builds, hidden
  lifecycle runs, and visible editor qualification pass. The user accepted
  closure after the final branding and icon-glyph corrections; the exhaustive
  cross-monitor, theme, Snap Layout, and failure-injection matrix is explicitly
  retained as deferred qualification rather than claimed as completed evidence.

## Current Status

Implementation through the integrated MainFrame bar is complete. ApplicationCore
now exposes explicit decoration modes and owns one Windows WndProc bridge, custom
non-client calculation, DPI-aware native hit testing, work-area maximization,
interaction state, and complete system-frame fallback. MainFrame requests custom
mode before creation and draws one scaled title/menu bar across project-browser,
loading, and ready states while sharing all workspace menu commands with fallback.

The implemented visual baseline remains 36 pixels with a 20-pixel square brand
mark in a compact 24-pixel slot, a 20-pixel whitespace break before the menus,
and three 46-pixel caption regions before UI scaling. The `Durin` wordmark uses
the existing Roboto Medium asset at an optically matched size and shares a
baseline with the regular project suffix; menus retain the normal UI weight.
MainFrame loads the authored Durin logo through one shared asynchronous GPU
texture used by both the title bar and Project Browser. A 256-pixel UI derivative
keeps startup decode and texture memory bounded; alpha-aware runtime mips preserve
clean edges at the title-bar size and across UI scales. The supported minimum
track size is 640 by 480 base pixels, scaled by window DPI; the project title
truncates and hides before menus or native caption regions.

Automated evidence on 2026-08-16: `ApplicationCore` and `MainFrame` target builds,
a full `all` build, all 72 default native-test targets through `test all`, and
three focused `FGenericWindowTitleBarTests` passed. The focused set includes a
real hidden Win32 window proving retained style capabilities, caption suppression,
and native `WM_NCHITTEST` results. A visible editor startup reached ready state
and created the main swapchain without fallback or crash.

A follow-up native regression on 2026-08-16 proves that the persisted hidden-window
maximize request survives first show, its outer frame contains the active
monitor's calculated client maximum, and minimize, maximize/restore, and close
caption messages produce their corresponding system behavior. The reproduced top
crop was independent of saved ImGui layout: the maximized outer frame began 33
physical pixels below the monitor while its client falsely reported a zero top.
Custom mode now retains the native `WS_CAPTION` capability bit while suppressing
its layout, painting, and standard interaction, restoring Windows' overscanned
maximized frame. The boundary calculation expands through any detected
auto-hidden taskbar band except for its activation edge. The custom bridge owns
one `WM_SYSCOMMAND` transition per matched button press/release, while
`HTMAXBUTTON` remains available to Snap Layout.

A branding follow-up on 2026-08-16 replaced the separately approximated title-bar
and Project Browser polygons with the shared authored image. The `MainFrame` target
and full `all` build passed, and a visible Project Browser capture verified the
complete gradient mark, transparent edges, and small title-bar rendering.

A title-bar polish follow-up on 2026-08-16 tightened the brand's leading space,
replaced the hard divider with whitespace, and gave only the `Durin` wordmark a
medium weight with an optically matched size and baseline. The `MainFrame` target
and full `all` build passed, a 120-tick hidden editor run exited cleanly, and a
visible title-bar capture verified the compact spacing and vertical alignment.
Font Awesome remains merged into the default UI font before the independent
medium face is registered, preserving editor icon glyphs outside the title bar.

The plan is complete at the accepted implementation scope. The single visible
capture does not cover every configuration, so paired dark/light screenshots,
the full manual interaction matrix (Snap Layout, system menu, cross-monitor DPI,
taskbar edges, and narrow/theme/UI-scale comparisons), layout snapshot
automation, and injected hook-failure coverage remain explicitly deferred and
unverified. These gaps are recorded below rather than presented as passing
qualification.

Only the Windows editor main window adopts custom title-bar mode. Detached ImGui
viewports, PIE/game windows, dialogs, and other Mona windows retain their current
system-decorated or undecorated policy. Non-Windows platforms retain the current
system caption until a separately qualified implementation is selected.

This plan shares a Windows native-message foundation with
[Windows Native Window Modal-Loop Ticking](WindowsNativeWindowModalLoopTick.md).
Neither plan may install a competing `FGlfwWindow` WndProc layer. Whichever plan
implements that foundation first must leave one composable ApplicationCore-owned
message bridge for the other.

## Goal

Give the Durin editor main window a visually integrated title bar containing the
application identity, project title, existing editor menus, a draggable area,
and minimize/maximize/close controls rendered in the editor theme.

The finished window must still behave like a native resizable Windows desktop
window: edge and corner resize, drag, double-click maximize/restore, Alt+Space
and right-click system menus, minimize, maximize/restore, close, Windows 11 Snap
Layout, taskbar work-area constraints, DPI changes, and native keyboard behavior
remain authoritative.

## Scope

- Add explicit system, custom-title-bar, and undecorated window modes to the
  platform-neutral window contract.
- Request custom-title-bar mode only for the editor root `MWindow` before its
  native window is created.
- Implement a Windows custom non-client frame in `FGlfwWindow` while retaining
  the native resize frame, system command styles, shadow, and rounded-corner
  eligibility.
- Add a platform-neutral, frame-stable title-bar hit-region snapshot and native
  caption-button interaction state.
- Draw one integrated ImGui top bar in `MainFrame` with branding, project title,
  File/Edit/Tools/Window menus, a drag region, and caption controls.
- Route caption-button behavior through native hit-test/system-command semantics
  so Windows remains responsible for minimize, maximize/restore, close, and Snap
  Layout.
- Preserve existing editor bootstrap, project-browser, loading, workspace,
  docking, viewport, saved geometry, and theme-switch behavior.
- Fall back to the current system-decorated main window and ordinary client menu
  bar when custom-frame setup is unsupported or fails.
- Add value-level hit-test/layout tests, Windows native integration tests, and
  visible editor qualification.
- Publish the implemented native-frame and editor visual contracts outside this
  plan.

## Non-Goals

- Applying a custom title bar to detached ImGui viewports, PIE/game windows,
  standalone windows, dialogs, tool windows, or popups.
- Implementing custom title bars on macOS, X11, or Wayland.
- Creating a borderless GLFW window with `GLFW_DECORATED` set to false for the
  editor main frame.
- Moving the window by repeatedly calling `glfwSetWindowPos` from mouse input.
- Replacing GLFW, patching the vendored GLFW implementation, or bypassing its
  existing window/input callbacks.
- Reimplementing a window manager, custom drop shadow, transparency, Mica,
  Acrylic, or backdrop materials.
- Changing the contents or command behavior of existing editor menus.
- Redesigning docking tabs, document tabs, the workspace layout, or detached
  viewport decoration rules.
- Solving engine-frame starvation inside the Windows move/resize modal loop;
  that behavior remains owned by the related modal-loop plan.
- Guaranteeing Windows 11 Snap Layout on operating-system versions that do not
  provide it.

## Design Decisions and Invariants

### Decoration mode is explicit and requested before creation

Replace the decorated Boolean as the authoritative policy with a value
equivalent to:

```text
System          platform draws the frame and caption
CustomTitleBar  platform keeps native window capabilities; Durin draws caption content
None            platform creates an undecorated window
```

The requested mode is stored by `MWindow`, copied into
`FGenericWindowDefinition`, and reported as an effective mode by
`FGenericWindow`. Existing `SetWindowDecorated` callers migrate to the explicit
mode API: decorated ImGui viewports select `System`, while no-decoration
viewports select `None`.

`MainFrame` requests `CustomTitleBar` before `FMonaApplication::AddWindow`.
Runtime transitions into or out of custom-title-bar mode are not supported by
this plan. Existing runtime transitions between `System` and `None` for ImGui
platform windows remain supported.

If a platform cannot activate the requested custom frame, it reports `System`
as the effective mode. `MainFrame` must choose its integrated or legacy menu-bar
layout from the effective mode so a failure never creates two title bars or no
title bar.

### The custom frame retains native window capabilities

The Windows path initially creates a decorated, resizable GLFW window. After
GLFW supplies the `HWND`, ApplicationCore suppresses the standard caption and
extends the client area through the top frame while retaining the native style
capabilities required for resize, minimize, maximize, system menu, taskbar,
shadow, and Snap behavior.

The selected Win32 frame policy keeps `WS_THICKFRAME`, `WS_MINIMIZEBOX`,
`WS_MAXIMIZEBOX`, and `WS_SYSMENU`; it removes/suppresses standard caption
layout and painting only after the ApplicationCore native hook is active. A
`SWP_FRAMECHANGED` update completes the transition before the hidden window is
shown.

`WM_NCCALCSIZE` extends the client area. `WM_GETMINMAXINFO` and maximized-frame
calculation constrain the window to the active monitor work area and preserve
auto-hidden-taskbar access. `WM_NCACTIVATE`/`WM_NCPAINT` must not reintroduce a
standard caption. DWM frame extension/corner preference may be used only to
preserve the native shadow and Windows 11 rounding; they must not introduce a
transparent rendering dependency.

Client, window, and framebuffer sizes retain their current meanings to Mona,
ImGui, and the RHI. The added title bar consumes client layout space but does
not change viewport extent reporting or invent a second geometry coordinate
system.

### ApplicationCore owns one shared Windows message bridge

`FGlfwWindow` owns one Windows-only WndProc lifecycle installed after GLFW
creates the `HWND` and before the window is visible. It chains every unhandled
message to the previously installed GLFW procedure and restores that procedure
exactly once before `glfwDestroyWindow`.

The expected order when a later ImGui viewport hook exists is:

```text
MonaImGui viewport WndProc -> FGlfwWindow shared WndProc -> GLFW WndProc
```

The editor main viewport currently needs no additional MonaImGui WndProc for
custom-title-bar behavior. Title-bar logic lives in the shared ApplicationCore
hook, not in `MainFrame` or the renderer. If the modal-loop plan has already
created the shared hook, this plan extends it; if not, this plan creates the
shared scaffold without implementing modal continuation.

Hook failure is logged once and changes the effective decoration mode to
`System` before first show. No partial custom-frame state may remain active
after failure or destruction.

### Hit testing consumes an immutable layout snapshot

ImGui never runs inside the native WndProc. Once per editor frame, `MainFrame`
publishes a plain platform-neutral snapshot containing:

- title-bar height;
- one or more draggable client rectangles;
- minimize, maximize/restore, and close rectangles;
- a monotonically increasing layout generation and validity flag.

Rectangles use window-client pixel coordinates matching ImGui viewport
coordinates. The Windows hook converts the `WM_NCHITTEST` screen point exactly
once and evaluates this order:

1. DPI-aware resize corners and edges;
2. close, maximize/restore, and minimize regions;
3. registered draggable regions;
4. ordinary client content.

The resize thickness comes from `GetSystemMetricsForDpi` frame and padded-border
metrics, not a duplicated ImGui visual constant. `DwmDefWindowProc` receives
non-client messages first where Windows requires it; unhandled work continues
through the stored GLFW procedure.

The corresponding results are `HTCLOSE`, `HTMAXBUTTON`, `HTMINBUTTON`,
`HTCAPTION`, resize-edge/corner values, or `HTCLIENT`. Returning
`HTMAXBUTTON` and preserving native non-client processing is mandatory for the
Windows 11 Snap Layout qualification path.

Before the first valid rendered snapshot, the custom frame uses a conservative
pre-show layout derived from the configured title-bar metrics and current client
width. After publication, native code reads only the latest complete snapshot;
it never follows ImGui pointers or partially updated rectangles.

### Native code owns caption interaction; ImGui owns pixels

Caption buttons are drawn by `MainFrame`, but native hit-test and system-command
processing own their actions. The shared WndProc tracks only value state needed
for rendering: focused/inactive window, hovered caption part, pressed caption
part, and maximized/restored state. `MainFrame` reads that state on the next
frame and draws the matching visual.

Non-client mouse messages are not converted into duplicate ImGui clicks. Native
processing produces the existing GLFW close, size, position, focus, and
framebuffer callbacks, so downstream application and renderer ownership does
not change. Add a platform-neutral minimize operation for completeness and
tests, but the normal caption path remains a native system command.

The maximize glyph reflects the current restored/maximized state. The close
button uses the semantic error color only while hovered/pressed; other caption
buttons use the ordinary themed hover/active colors. The window title continues
to be set natively for the taskbar, Alt+Tab, accessibility metadata, and system
menu even though Durin also renders it.

### The top bar is one responsive MainFrame layout

In custom mode, the existing workspace host menu bar becomes the entire title
bar rather than appearing beneath a separate caption. The baseline visual order
is:

```text
[Durin mark] [project/editor title] [File Edit Tools Window] [drag space] [min] [max/restore] [close]
```

Selected base metrics before global UI scaling are a 36-pixel title-bar height,
a 20-pixel square branding slot, 8-pixel internal gaps, and three 46-pixel-wide
caption regions. Stage 0 may adjust these values only after a checked-in design
comparison records the replacement values and rationale.

The bar uses `ImGuiCol_MenuBarBg`, normal/disabled text, normal button
hover/active colors, and the semantic error color for close hover/press. It
derives `Engine/Content/Editor/Branding/DurinEditorLogoUI.png` from
`Engine/Content/Editor/Branding/DurinEditorLogo.png`, then shares one mipmapped
GPU texture between the title bar and Project Browser. The smaller derivative is
the runtime UI asset; the original remains the authored source. The project title
comes from the same state used to set the native window title.

At narrow widths the project title truncates first, then hides. Menu commands
and all three caption buttons remain fully hittable, and at least one 48-pixel
draggable region remains when the supported minimum main-window width is met.
Menu item rectangles remain `HTCLIENT`; the drag snapshot contains only the
explicit title/spacer regions and therefore cannot steal menu, docking, or
workspace input.

In system fallback mode, `MainFrame` retains the current ordinary client menu
bar and does not draw branding or caption controls.

### DPI, monitor, and startup transitions are explicit

Native resize borders are recomputed from the window DPI for every hit test or
from a cache invalidated by `WM_DPICHANGED`. Title-bar visual rectangles are
published in the actual client coordinates used to render them and receive no
second DPI multiplication in ApplicationCore.

Moving between monitors, maximizing on a secondary monitor, taskbar placement
on any edge, and auto-hidden taskbar behavior must use the target monitor's work
area. Restored geometry remains owned by GLFW/Mona and the existing host
settings.

The window stays hidden while the native frame mode is selected and applied.
The first visible presentation must contain either the complete custom bar or
the complete system fallback; a standard-caption flash followed by a custom
caption is not acceptable.

## Current Foundations and Gaps

- `FGenericWindowDefinition` currently carries only a Boolean OS-border policy.
- `MWindow` can retain window presentation state before
  `FMonaApplication::MakeWindow` creates the platform window.
- `FGlfwWindow` already obtains the Win32 `HWND`, applies the executable icon,
  and exposes maximize/restore/focus state.
- the editor root window is created hidden, its persisted maximize state is
  applied, and its renderer viewport is created before first show.
- `MainFrame` already owns the project title and all File/Edit/Tools/Window menu
  drawing in one workspace-host function.
- MonaImGui already has a later WndProc hook for detached viewport
  mouse-passthrough behavior, so deterministic hook chaining is an existing
  requirement.
- the active modal-loop plan has selected an ApplicationCore WndProc hook for
  every GLFW window but has not implemented it yet.
- the editor branding logo and theme colors needed by the proposed bar already
  exist.
- there is no custom-decoration mode, native non-client layout, title-bar region
  snapshot, caption-button state, minimize API, integrated title/menu bar, or
  custom-frame test coverage.

## Implementation Stages

### Stage 0: Freeze frame ownership, geometry, and visual baseline

Outcome: implementation begins from one native hook contract, one coordinate
model, and an approved top-bar geometry rather than discovering these inside UI
code.

- Deferred at closure: capture the previous Windows 11 caption/menu seam in
  both themes as historical baseline evidence.
- [x] Record whether the modal-loop plan's shared `FGlfwWindow` hook exists at
  implementation time and select reuse or scaffold-first ownership without
  installing a second hook.
- [x] Define the decoration-mode, effective-mode, hit-region snapshot, and
  caption-interaction value contracts with no Win32 or ImGui public types.
- [x] Freeze client versus screen coordinate conversion, DPI border metrics,
  first-frame fallback regions, and maximized work-area calculation.
- Deferred at closure: produce a paired 36-pixel top-bar comparison in both
  themes; the accepted dark-theme captures record the implemented metrics.
- [x] Identify the supported minimum main-window width and prove the proposed
  responsive order leaves menus, caption controls, and a drag region available.
- [x] Add pure value tests for hit-test priority, edge/corner classification,
  caption regions, client fallthrough, and invalid snapshot fallback.

Dependencies: none. Coordination with the active modal-loop plan is required
before editing the shared native hook lifecycle.

#### Acceptance Gate

- One component owns WndProc installation, chaining, and teardown across both
  active plans.
- The public contracts contain no `HWND`, `RECT`, DWM, GLFW, or ImGui types.
- Baseline evidence and selected visual metrics are recorded.
- Pure tests distinguish every resize edge/corner, caption button, drag region,
  and ordinary client result, including overlap priority.

### Stage 1: Establish decoration modes and the Windows custom frame

Outcome: a hidden/test main window can activate a captionless native frame with
correct client bounds, resize semantics, and deterministic fallback before any
custom pixels are drawn.

- [x] Replace the Boolean decoration definition with the explicit mode throughout
  `FGenericWindow`, `FGenericWindowDefinition`, `MWindow`, Mona window creation,
  and ImGui platform viewport styling.
- [x] Add effective-mode reporting, title-bar layout publication, interaction
  state retrieval, and minimize support to the neutral window API.
- [x] Implement or extend the single `FGlfwWindow` Windows WndProc lifecycle with
  exact-once install, chain, property/state cleanup, and restore.
- [x] Activate custom non-client calculation only for `CustomTitleBar`; leave
  `System` and `None` behavior byte-for-byte equivalent where practical.
- [x] Preserve the required native style flags, apply frame change while hidden,
  and retain DWM shadow/corner behavior.
- [x] Implement DPI-aware resize hit testing, caption/drag hit testing,
  maximized work-area correction, and system fallback on partial failure.
- Deferred at closure: extend Windows integration coverage from the passing
  hidden-window, hit-test, maximize, and teardown cases to injected hook/setup
  failure and complete effective-mode fallback.

Dependencies: Stage 0.

#### Acceptance Gate

- A custom-mode test window has no standard caption, retains shadow/rounding as
  supported, and resizes from all eight edges/corners.
- Normal and maximized client rectangles stay within the correct monitor work
  area without covering a visible or auto-hidden taskbar activation edge.
- System and undecorated windows retain their existing behavior.
- Hook setup failure produces a fully system-decorated window, not a partially
  modified frame.
- Main and detached ImGui WndProc chains tear down without stale properties,
  duplicate callbacks, or restoring the wrong predecessor.

### Stage 2: Render and publish the integrated MainFrame title bar

Outcome: the editor main window visibly uses one themed top bar and publishes
native hit regions that exactly match the rendered geometry.

- [x] Request `CustomTitleBar` on the editor root before binding/creation and
  branch layout from the effective mode after native attachment.
- [x] Refactor existing menu commands into a shared drawing helper so custom and
  system-fallback layouts execute identical File/Edit/Tools/Window behavior.
- [x] Draw branding, current project/editor title, menus, explicit drag space,
  and three caption controls using the selected theme tokens and metrics.
- [x] Reuse the authored brand image through one shared mipmapped texture
  lifetime for MainFrame and Project Browser.
- [x] Publish draggable and caption rectangles only after their final ImGui
  layout is known; publish one complete generation per frame.
- [x] Render hover, pressed, focus, and maximize/restore states from the native
  interaction snapshot without creating ImGui buttons over native caption
  regions.
- [x] Implement the selected narrow-width truncation/hide behavior and preserve
  the minimum drag region without overlapping menus or caption controls.
- [x] Draw the same bar during project-browser, loading, and ready-workspace
  states so bootstrap never swaps between incompatible top layouts.
- Deferred at closure: add MainFrame layout snapshots for minimum width, long
  project names, both themes, and every supported editor UI scale.

Dependencies: Stage 1.

#### Acceptance Gate

- Exactly one title/menu bar is visible in custom mode and the existing system
  caption plus menu bar is visible in fallback mode.
- Every rendered caption/drag rectangle matches the native hit-test snapshot at
  full and minimum supported widths.
- File/Edit/Tools/Window menus retain their current commands, shortcuts, and
  popup behavior.
- Long project names never overlap menus, drag space, or caption controls.
- Project browser, loading shell, and ready workspace share identical chrome
  geometry with no first-present standard-caption flash.

### Stage 3: Qualify native interaction, DPI, and hook composition

Outcome: the rendered main frame behaves indistinguishably from a native window
for window management and remains composable with all current window consumers.

- Deferred at closure: manually qualify minimize, maximize/restore, close,
  double-click caption, caption drag, system-menu, and Alt+Space behavior.
- Deferred at closure: qualify Windows 11 Snap Layout hover and selections.
- Deferred at closure: exercise menus, docking, tabs, viewport capture, resize
  edges, persisted states, mixed-DPI monitors, and taskbar placements.
- [x] Compose and test the shared WndProc with MonaImGui's later viewport hook
  and any modal-loop messages implemented by the related plan.
- [x] Confirm native move/resize modal-loop behavior is no worse than baseline;
  record continuous ticking evidence only if the related plan is complete.
- [x] Add diagnostics for effective decoration mode and invalid/stale layout
  generations without logging per-frame or per-hit-test noise.

Dependencies: Stage 2. The modal-loop plan is not required to render custom
chrome, but its native-hook stage must be integrated rather than duplicated if
either plan has begun implementation.

#### Acceptance Gate

- Native window-management interactions, including Windows 11 Snap Layout,
  behave correctly without duplicate commands or stuck hover/pressed states.
- Main-window client input and ImGui menus remain fully usable next to drag and
  caption regions.
- Cross-monitor DPI changes preserve visual/hit-region alignment and correct
  maximized work areas.
- Closing or exiting during hover, press, drag, resize, minimized state, or
  shutdown leaves no WndProc/property/state lifetime error.
- Hook-chain qualification passes whether or not modal continuation is enabled.

### Stage 4: Complete validation and publish lasting contracts

Outcome: automated, visible, and behavioral evidence is complete, and future
window/UI work routes to maintained contracts instead of this plan.

- [x] Build affected native tests and `DurinEditor` through the repository build
  workflow.
- [x] Run focused window/UI tests and the required broader native suite through
  the repository test-selection workflow.
- Deferred at closure: complete the full Windows 11 visual/interaction matrix
  and system-fallback qualification on another platform or injected path.
- Deferred at closure: capture paired dark/light comparisons at representative
  normal, maximized, snapped, and narrow sizes.
- [x] Update Editor UI Style with title-bar tokens, responsive order, visual
  states, and custom-versus-fallback ownership.
- [x] Create a focused implemented Runtime windowing contract under
  `Documentation/Runtime/Core/` covering decoration modes, the shared native
  hook, hit-region coordinates, system-command ownership, DPI, and fallback.
- [x] Cross-link the modal-loop plan/contract to the shared native hook if that
  work has been implemented.
- [x] Record exact validation evidence in `Current Status`, close passed
  checklists, and complete the plan lifecycle metadata.

Dependencies: Stage 3.

#### Acceptance Gate

- Required builds, focused tests, broader native tests, visible checks, and
  documentation validators pass.
- Before/after evidence shows a single visually integrated bar in both themes
  without sacrificing native window behavior.
- System fallback and non-Windows behavior have no double-title-bar regression.
- Lasting editor visual and runtime window contracts own the implemented rules.

## Validation Matrix

| Area | Automated evidence | Runtime/visual evidence |
| --- | --- | --- |
| Decoration modes | Requested/effective mode and migration tests | Main custom; detached/PIE system; popup none |
| Hit-test geometry | Edge/corner/button/drag/client priority table tests | Pointer cursor and actions match rendered regions |
| Native hook lifetime | Install/failure/chain/duplicate teardown tests | Main and detached windows close without stale hooks |
| Frame geometry | Normal/maximized/work-area value and hidden-window tests | Primary/secondary monitor, taskbar on each edge, auto-hide |
| Caption actions | Native message/system-command integration tests | Minimize, max/restore, close, double-click, right-click, Alt+Space |
| Snap Layout | `HTMAXBUTTON` path and state transition coverage | Windows 11 hover flyout and snap selections |
| MainFrame layout | Full/narrow/long-title/theme/scale layout tests | No overlap; exact draw/hit alignment |
| Bootstrap | Effective-mode and fallback selection tests | Project browser, loading, ready, persisted maximized startup |
| Client input | Region exclusion tests | Menus, docking, tabs, viewport capture, text input |
| DPI | Coordinate conversion and border-metric tests | 100/150/200% Windows DPI across monitors |
| UI scale | Layout snapshots at 75/100/125/150/200% | Readable glyphs and aligned hit targets |
| Themes/focus | Token/state mapping tests | Dark/light, active/inactive, hover/press, close-danger state |
| Fallback/platforms | Injected hook/capability failure tests | Complete system caption and legacy menu; non-Windows unchanged |
| Modal composition | Shared-hook message routing tests | Move/resize behavior matches baseline or related-plan evidence |

Build, run, and test commands must come from the repository workflows in
Related Documentation rather than being copied into this plan.

Closure disposition: implementation, automated native coverage, builds,
lifecycle runs, lasting contracts, and the accepted visible editor path pass.
The exhaustive manual and injected-failure cells that were not executed are
reclassified as deferred follow-ups below; completion does not claim those
matrix cells as passing evidence.

## Definition of Done

- The Windows editor main window renders one integrated Durin title/menu bar.
- Detached ImGui, PIE/game, dialog, and other windows retain their existing
  decoration modes.
- Native drag, eight-direction resize, double-click, system menus, caption
  commands, taskbar work area, and Windows 11 Snap Layout pass qualification.
- Rendered hit regions and native hit-test regions remain aligned across themes,
  focus states, supported UI scales, Windows DPI transitions, normal/maximized,
  snapped, and minimum-width layouts.
- Custom-frame failure produces a complete system-decorated fallback before the
  window becomes visible.
- ApplicationCore owns one shared native hook compatible with MonaImGui and the
  modal-loop plan; teardown is exact-once.
- Automated tests cover decoration policy, geometry priority, native hook
  lifetime, MainFrame layout, state mapping, and fallback.
- Required builds, broader tests, visible interaction checks, screenshots, and
  documentation validation have recorded evidence.
- Lasting windowing and UI-style contracts are published, and this plan is
  marked completed with evidence.

## Deferred Follow-ups

- Complete the Windows 11 caption interaction and Snap Layout manual matrix,
  including Alt+Space, right-click system menu, snap selections, and all window
  states.
- Qualify mixed-DPI monitors, primary/secondary taskbar placements, supported UI
  scales, focus states, and both editor themes.
- Add deterministic MainFrame layout snapshots for narrow/long-title cases and
  injected custom-frame setup/fallback failure coverage.
- Capture paired before/after dark and light evidence at normal, maximized,
  snapped, and narrow sizes.
- Qualify the complete system-caption fallback on a non-Windows platform or an
  injected unsupported custom-frame path.
- Custom title bars for detached ImGui, PIE/game, or standalone windows.
- macOS traffic-light/titlebar integration and Linux compositor-specific client
  decorations.
- Mica, Acrylic, transparency, custom shadows, or other backdrop treatments.
- User-configurable title-bar density or alternate menu placement.
- Title-bar document tabs or workspace tabs; this plan keeps docking/document
  tabs in the client workspace.
- Continuous engine rendering during a Windows-owned move/resize modal loop
  when it is not already delivered by the related modal-loop plan.

## Related Documentation

- [Editor UI Style](../Editor/Design/UIStyle.md)
- [Window Frame Contract](../Runtime/Core/WindowFrames.md)
- [Windows Native Window Modal-Loop Ticking](WindowsNativeWindowModalLoopTick.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)
- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)

## Related Code

- [`FGenericWindowDefinition`](../../Engine/Source/Runtime/ApplicationCore/Public/Window/GenericWindowDefinition.h)
- [`FGenericWindow`](../../Engine/Source/Runtime/ApplicationCore/Public/Window/GenericWindow.h)
- [`FGenericWindow` defaults](../../Engine/Source/Runtime/ApplicationCore/Private/Window/GenericWindow.cpp)
- [`FGlfwWindow`](../../Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.h)
- [`FGlfwWindow` implementation](../../Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp)
- [`MWindow`](../../Engine/Source/Runtime/MonaCore/Public/Widgets/MWindow.h)
- [`MWindow` implementation](../../Engine/Source/Runtime/MonaCore/Private/Widgets/MWindow.cpp)
- [`FMonaApplication` window creation](../../Engine/Source/Runtime/MonaCore/Private/Application/MonaApplication.cpp)
- [`MonaImGui` platform viewport integration](../../Engine/Source/Runtime/MonaImGui/Private/ImGuiMonaImpl.cpp)
- [`MonaImGui` theme and metrics](../../Engine/Source/Runtime/MonaImGui/Private/MonaImGui.cpp)
- [`MainFrame` editor shell](../../Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp)
- [`MainFrame` editor branding texture](../../Engine/Source/Editor/MainFrame/Private/EditorBranding.cpp)
- [Dark editor theme](../../Engine/Configs/DurinEditorTheme.Dark.yaml)
- [Light editor theme](../../Engine/Configs/DurinEditorTheme.Light.yaml)
- [Editor branding logo](../../Engine/Content/Editor/Branding/DurinEditorLogo.png)
- [Editor UI branding logo](../../Engine/Content/Editor/Branding/DurinEditorLogoUI.png)
- [Viewport foundation tests](../../Engine/Tests/Native/EngineTests/Private/Viewport/ViewportFoundationTests.cpp)
- [UI style tests](../../Engine/Tests/Native/EngineTests/Private/UIStyleTests.cpp)
