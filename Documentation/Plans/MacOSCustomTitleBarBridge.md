# macOS Custom Title Bar Bridge Plan

Summary: Add an AppKit-owned bridge for the macOS editor main window so Durin can render an integrated title/menu bar while native traffic-light controls, window management, and the existing Windows custom frame remain authoritative.

Last reviewed: 2026-08-16

Status: Active
Completed:

## Current Status

The Windows editor main window already uses the shared platform-neutral
decoration mode, title-bar layout, and interaction snapshots, backed by an
ApplicationCore-owned WndProc bridge. macOS currently converts a requested
`CustomTitleBar` mode to effective `System`, so MainFrame retains the ordinary
client menu bar beneath the AppKit title bar.

The selected macOS direction is a platform peer, not a line-for-line port of
the Win32 message bridge. ApplicationCore will own a main-thread Cocoa
event/notification bridge without replacing GLFW's `NSWindowDelegate`. The
window will keep its decorated, resizable `NSWindow`, native traffic-light
buttons, fullscreen behavior, shadow, accessibility metadata, and system window
commands. AppKit will expose a transparent full-size title-bar content area;
Durin will draw the background, branding, project title, and menus around the
native controls.

This is an independent follow-up to the macOS platform-runtime milestone. It
depends on the qualified Cocoa window lifecycle but does not block completion
of that milestone's system-decorated Editor shell. No implementation work has
started under this plan.

## Goal

Give the macOS editor root window the same integrated Durin title/menu-bar
experience as Windows while preserving platform-native macOS behavior and
keeping one clear ApplicationCore owner for Cocoa integration.

The completed path must preserve native close, minimize, zoom, fullscreen,
window dragging, focus, keyboard shortcuts, accessibility metadata, Retina
geometry, Spaces behavior, and clean teardown. It must also leave the Windows
WndProc bridge, native hit testing, Snap Layout, modal-loop continuation, and
system-frame fallback behavior unchanged.

## Scope

- Enable `EWindowDecorationMode::CustomTitleBar` for the editor root window on
  macOS only after the Cocoa bridge and frame setup both succeed.
- Add a macOS-specific Objective-C++ bridge owned by ApplicationCore and scoped
  to one GLFW-backed `NSWindow`.
- Extend the AppKit title-bar content area while retaining native traffic-light
  controls and native window style capabilities.
- Expose platform title-bar metrics so MainFrame reserves the live native
  control region instead of duplicating traffic-light geometry.
- Reuse the existing generation-based Durin drag-layout publication without
  invoking ImGui or renderer code from an AppKit event callback.
- Route eligible title-bar drag gestures to AppKit and leave menus, docking,
  viewport content, and native controls untouched.
- Preserve native window title, focus, maximized/zoomed state, GLFW callbacks,
  Vulkan/Metal presentation, and existing editor startup ordering.
- Provide complete rollback to effective `System` before first show when setup
  fails.
- Add value, native Cocoa lifecycle, visible-editor, rendering-resize, and
  Windows non-regression validation.
- Publish the lasting cross-platform and macOS contracts after implementation.

## Non-Goals

- Replacing, subclassing, or competing with GLFW's `NSWindowDelegate`.
- Drawing imitations of the macOS traffic-light buttons or manually issuing
  close, minimize, zoom, or fullscreen commands for those buttons.
- Applying custom title bars to detached ImGui viewports, PIE/game windows,
  dialogs, popups, tool windows, or secondary Mona windows.
- Replacing GLFW, patching vendored GLFW, or moving Cocoa event pumping out of
  its current main-thread ownership.
- Introducing a borderless `NSWindow`, custom shadow, custom resize handles,
  private AppKit APIs, vibrancy, translucency, or a new material system.
- Making the custom title bar a prerequisite for the macOS platform-runtime or
  MoltenVK milestones.
- Refactoring the working Windows custom-frame or modal-loop bridges except for
  additive platform-neutral data needed by both platforms.
- Supporting runtime transitions into or out of custom mode; selection remains
  a pre-creation policy with system-frame fallback.

## Design Decisions and Invariants

### macOS gets a Cocoa peer, not a WndProc abstraction

The Win32 bridge remains Windows-only. macOS uses a dedicated Objective-C++
component under `ApplicationCore/Private/MacOS` that receives the GLFW Cocoa
window after creation. The component owns any AppKit event monitor and
notification registrations, retains no renderer or ImGui objects, and is
removed before `glfwDestroyWindow`.

The bridge must not replace `NSWindow.delegate`; GLFW remains the delegate and
continues to own its normal close, focus, size, framebuffer, and input callback
translation. AppKit notifications and a window-scoped local event monitor are
the initial selected composition mechanisms. Stage 0 must prove that
`performWindowDragWithEvent:` preserves native drag and title-bar double-click
preferences. If that native behavior cannot be demonstrated, Stage 0 must
record a revised decision before implementation, with a transparent native
hit-test view as the bounded alternative; silent manual window-position loops
are prohibited.

### AppKit keeps the frame and traffic-light controls

Custom mode starts from a normal titled, closable, miniaturizable, resizable
`NSWindow`. ApplicationCore adds the full-size-content-view style, hides the
standard title text, and makes the title-bar background transparent while
leaving `standardWindowButton:` controls installed and visible. The native
window title remains populated for Window menu, accessibility, Mission Control,
and other system metadata.

Durin draws title-bar content but does not draw or hit-test the traffic lights.
The platform reports their current client-space exclusion region to MainFrame;
MainFrame keeps branding, project text, menus, and drag regions outside it.
Fullscreen transitions and AppKit layout changes refresh this geometry rather
than relying on fixed offsets.

### Cross-platform data stays value-only and directionally clear

MainFrame continues to publish one complete, monotonically generated
`FWindowTitleBarLayout`. The macOS bridge reads only the latest copied value and
never calls ImGui during AppKit dispatch. A separate platform-metrics snapshot
reports whether window controls are native and the leading/trailing exclusion
rectangles MainFrame must reserve; Windows continues to report that Durin draws
its three caption buttons.

Coordinates at the C++ boundary use the same logical client coordinate space as
the MainFrame ImGui viewport. Cocoa points are converted once, including the
AppKit bottom-left to client top-left Y flip. Retina backing scale is not applied
to logical hit regions; framebuffer pixels remain owned by rendering. Stage 0
must lock this rule with 1x and Retina evidence before production event routing.

### Event routing is narrow and native

The local event monitor is scoped by exact `NSWindow` identity. It considers
only primary-button events in a valid published drag region. Events over native
controls, menus, docking UI, viewport content, invalid/stale layouts, or other
windows pass through unchanged. Eligible drag gestures are handed to AppKit;
the bridge never updates window position in a polling loop and never synthesizes
duplicate GLFW or ImGui clicks.

Native traffic lights own their hover, press, action, tooltip, accessibility,
and fullscreen semantics. MainFrame needs only focus/zoom state and platform
metrics on macOS; Windows retains its current rendered-button interaction
snapshot.

### Setup and teardown are transactional and main-thread-only

All Cocoa frame mutation, monitor/notification registration, geometry refresh,
and removal occur on the AppKit main thread. Setup captures every modified
`NSWindow` property. A failure at any step removes partial observers/monitors,
restores the prior window properties, reports effective `System`, and logs one
diagnostic before the hidden window is shown.

Destruction first makes the bridge inert, removes event/notification tokens,
restores only properties still owned by the bridge, and then permits GLFW to
destroy the native window. Repeated create/destroy and failed-initialization
paths must leave no callback capable of reaching a dead `FGlfwWindow`.

### Windows behavior is a release gate

macOS implementation lives in platform-isolated Objective-C++ sources and
`__APPLE__` declarations. Windows source selection, link libraries, WndProc
ordering, `WM_NCHITTEST`, `WM_SYSCOMMAND`, DWM behavior, Snap Layout, and modal
loop handling are not generalized into Cocoa concepts.

Additive platform-neutral APIs must have defaults that preserve the current
Windows behavior. This plan cannot complete on macOS-only evidence: a Win64
configure/build and the focused native custom-frame/modal-loop suites must pass
on a Windows host or CI worker after the final shared-interface change.

## Current Foundations and Gaps

- `EWindowDecorationMode`, requested/effective fallback, generated title-bar
  layouts, shared hit testing, and interaction snapshots already exist.
- MainFrame already draws an integrated brand/title/menu bar and selects it from
  the effective decoration mode.
- `FGlfwWindow` obtains the `NSWindow` through `glfwGetCocoaWindow` and performs
  Cocoa-dependent Metal-layer preparation on the main thread.
- macOS currently treats requested custom mode as unsupported and falls back to
  `System`.
- The existing Objective-C runtime calls are embedded in common C++; there is no
  owned Cocoa title-bar bridge or Objective-C++ lifecycle unit.
- The current layout assumes Durin-rendered caption controls on the right and
  has no reverse platform-metrics channel for native controls on the left.
- Cocoa point/client conversion, traffic-light layout changes, native drag
  dispatch, fullscreen transitions, failure rollback, and repeated teardown are
  not covered by focused tests.
- The Windows custom-frame behavior is implemented and documented; its focused
  native tests provide the required non-regression baseline.

## Implementation Stages

### Stage 0: Characterize AppKit composition and freeze the bridge contract

Outcome: native-window ownership, coordinate conversion, drag semantics, and
traffic-light geometry are proven before shared interfaces change.

- [ ] Capture system-decorated and full-size-content-view behavior on the
  supported Apple Silicon/macOS baseline, including Retina and fullscreen.
- [ ] Prototype a window-scoped local event monitor without replacing GLFW's
  delegate; prove normal GLFW close, focus, resize, cursor, and key callbacks
  remain singular.
- [ ] Prove AppKit-owned dragging and the user's title-bar double-click action;
  record and justify the transparent hit-test-view alternative if the event
  monitor cannot preserve both.
- [ ] Record exact Cocoa-to-MainFrame coordinate conversion at 1x and Retina
  scale, including content-layout origin and Y-axis conversion.
- [ ] Inventory native traffic-light frames in restored, zoomed, and fullscreen
  states and select the platform-metrics value shape.
- [ ] Record the pre-change Win64 custom-title-bar and modal-loop focused-test
  baseline used by the final non-regression gate.

#### Acceptance Gate

- The selected Cocoa composition path retains GLFW delegate ownership, native
  drag/double-click behavior, and live traffic-light geometry with a documented
  logical-coordinate model; Windows baseline evidence is recorded.

### Stage 1: Implement the transactional Cocoa bridge

Outcome: ApplicationCore can activate and safely tear down a native macOS custom
title-bar frame without involving MainFrame or rendering callbacks.

- [ ] Add a bounded Objective-C++ bridge in `ApplicationCore/Private/MacOS` and
  include/link it only for Apple targets with the required AppKit framework.
- [ ] Install full-size content, transparent title-bar background, hidden native
  title text, and retained standard traffic-light controls transactionally.
- [ ] Add platform-metrics publication for native-control exclusions and refresh
  it on resize, backing-scale, zoom, focus, screen, and fullscreen changes.
- [ ] Route only valid drag-region primary-button gestures to AppKit and pass
  all other events through unchanged.
- [ ] Keep title-bar layout exchange value-only and protect event callbacks from
  stale generations, failed setup, and teardown.
- [ ] Restore system decoration completely on setup failure and remove every
  monitor/observer before native-window destruction.
- [ ] Add native Cocoa tests for effective mode, style/property state, standard
  control presence, coordinate conversion, event filtering, rollback, and
  repeated create/destroy.

#### Acceptance Gate

- A hidden application-hosted native test repeatedly creates, activates, uses,
  and destroys the Cocoa bridge with native controls intact, no duplicate GLFW
  callbacks, correct 1x/Retina geometry, and verified system-frame rollback.

### Stage 2: Integrate the macOS MainFrame layout

Outcome: the editor renders one responsive integrated macOS title/menu bar
around native traffic lights while Windows rendering remains unchanged.

- [ ] Extend MainFrame's existing integrated layout to consume platform metrics
  and place macOS branding/title/menu/drag content after the native leading
  control exclusion.
- [ ] Keep Windows' right-side Durin-rendered minimize, maximize/restore, and
  close regions pixel- and behavior-compatible with the current implementation.
- [ ] Exclude traffic lights, menus, project text interactions, docking, and
  viewport content from macOS drag regions at every supported UI scale and
  window width.
- [ ] Preserve the ordinary menu-bar layout whenever effective mode is `System`
  so failed activation never produces two title bars or loses all title bars.
- [ ] Refresh layout correctly across focus, zoom, fullscreen, monitor, Retina,
  theme, UI-scale, project-browser, loading, and ready-state transitions.
- [ ] Add value/layout tests covering native-left-controls and
  Durin-right-controls policies without compiling Cocoa concepts into Windows.

#### Acceptance Gate

- Visible macOS Editor qualification shows one integrated title/menu bar with
  native traffic lights and correct drag/menu behavior across startup states,
  resize, zoom, fullscreen, focus, theme, UI scale, and Retina transitions;
  focused shared layout tests pass.

### Stage 3: Qualify rendering, lifecycle, and Windows non-regression

Outcome: the feature is releasable without weakening either platform's native
window or rendering contracts.

- [ ] Exercise drag, resize, minimize, zoom/restore, fullscreen, close,
  Command-key menus, accessibility inspection, Spaces, and multi-monitor moves
  on the supported macOS baseline.
- [ ] Run repeated editor startup/shutdown and Vulkan viewport resize/recreate
  qualification to prove the title-bar view policy does not disturb the
  `CAMetalLayer`, swapchain extent, or clean RHI teardown.
- [ ] Run macOS native test aggregates and the application-hosted window
  lifecycle suites under the repository testing policy.
- [ ] On Win64, configure and build the Editor, then run shared title-bar,
  native custom-frame, modal-loop, launch-boundary, and relevant aggregate
  suites after all shared-interface changes are final.
- [ ] Verify platform source selection excludes Objective-C++/AppKit code from
  Win64 and excludes Win32 bridge code from macOS targets.
- [ ] Update the Window Frame contract with the implemented cross-platform and
  Cocoa ownership rules; update macOS runtime documentation only where its
  implemented lifecycle contract changes.
- [ ] Record final evidence, close all required gates, and complete the plan
  only after both macOS and Windows qualification are available.

#### Acceptance Gate

- Supported macOS editor interaction, rendering, and teardown pass natively;
  Win64 build and focused native frame/modal-loop tests pass with unchanged
  Windows behavior; lasting contracts and plan evidence are current.

## Validation Matrix

| Area | Required evidence |
| --- | --- |
| Shared values | Layout generations, stale/invalid rejection, logical coordinate conversion, native-control exclusion, and system fallback unit tests. |
| Cocoa ownership | Main-thread setup/removal, unchanged GLFW delegate, scoped event routing, observer cleanup, failure rollback, and repeated lifetime native tests. |
| macOS interaction | Native traffic lights, drag, title-bar double-click preference, zoom, fullscreen, focus, keyboard menus, accessibility, Spaces, and multi-monitor/Retina qualification. |
| Editor layout | Project Browser, loading, and ready states; narrow widths; theme and UI scale; no menu/docking/viewport input theft. |
| Rendering | Stable Metal-layer ownership, framebuffer sizing, Vulkan resize/swapchain recreation, minimize/restore, and clean RHI shutdown. |
| Windows | Win64 Editor configure/build; existing shared title-bar, native custom-frame, modal-loop, and launch-boundary suites; no AppKit/Objective-C++ source or link dependency. |
| Documentation | Changed-doc validation plus all-plan and all-roadmap lifecycle validation. |

Validation follows the repository [Build and Run](../Agents/BuildAndRun.md),
[Testing](../Agents/Testing.md), and [Documentation](../Agents/Documentation.md)
agent workflows rather than embedding command lines that may become stale.

## Definition of Done

- macOS reports effective `CustomTitleBar` only after transactional Cocoa setup
  succeeds and otherwise restores a complete system frame.
- MainFrame renders one integrated macOS title/menu bar without duplicating or
  imitating native traffic-light controls.
- AppKit remains authoritative for native controls, drag/double-click, zoom,
  fullscreen, accessibility, Spaces, and window management.
- GLFW retains delegate ownership and emits singular existing callbacks.
- Retina, fullscreen, monitor, focus, startup-state, theme, and UI-scale changes
  refresh geometry without stealing client input.
- Cocoa bridge teardown cannot outlive its `FGlfwWindow` or disturb Metal/Vulkan
  presentation lifetime.
- Objective-C++ and AppKit dependencies remain Apple-only.
- A real Win64 build and focused Windows native suites demonstrate no regression
  after final shared changes.
- Implemented lasting behavior is documented outside the plan, all required
  validation passes, and plan lifecycle metadata is complete.

## Deferred Follow-ups

- Custom title bars for detached ImGui viewports or other secondary windows.
- Custom traffic-light artwork or alternate button placement.
- Vibrancy, material effects, translucent title bars, or unified toolbar APIs.
- Linux X11/Wayland custom-decoration implementations.
- Runtime switching between system and custom title bars.
- Distribution-specific safe-area adjustments not observed on the supported
  macOS qualification baseline.

## Related Documentation

- [Window Frame Contract](../Runtime/Core/WindowFrames.md)
- [Editor Main Window Custom Title Bar Plan](EditorMainWindowCustomTitleBar.md)
- [macOS Platform Runtime Plan](MacOSPlatformRuntime.md)
- [macOS Platform Enablement Roadmap](../Roadmaps/MacOSPlatformEnablement.md)
- [macOS Native Test Application Host Plan](MacOSNativeTestApplicationHost.md)

## Related Code

- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.cpp`
- `Engine/Source/Runtime/ApplicationCore/Private/Window/GlfwWindow.h`
- `Engine/Source/Runtime/ApplicationCore/Private/MacOS/`
- `Engine/Source/Runtime/ApplicationCore/Public/Window/GenericWindow.h`
- `Engine/Source/Runtime/ApplicationCore/Public/Window/GenericWindowDefinition.h`
- `Engine/Source/Runtime/ApplicationCore/CMakeLists.txt`
- `Engine/Source/Editor/MainFrame/Private/MainFrameModule.cpp`
- `Engine/Tests/Native/EngineTests/Private/Viewport/ViewportFoundationTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Application/NativeWindowModalLoopTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/Launch/MacOSWindowLifecycleTests.cpp`
