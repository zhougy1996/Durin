# Window Frame Contract

Summary: Define platform-window decoration modes and the native Windows and macOS custom-title-bar boundaries.

Modules: ApplicationCore, MonaCore, MonaImGui, MainFrame

## Decoration Modes

`EWindowDecorationMode` is selected before native creation:

- `System` lets the platform draw the frame and caption.
- `CustomTitleBar` retains native window-management capabilities while Durin draws the caption pixels.
- `None` creates an undecorated window.

Requested and effective modes are distinct. Unsupported custom mode becomes
effective `System`; callers branch layout from the effective mode so fallback
never produces two title bars or no title bar. Runtime transitions are supported
only between `System` and `None`. The editor root requests custom mode on
Windows and macOS; detached ImGui viewports and other windows retain their prior
policy.

## Windows Message Bridge

`FGlfwWindow` owns the ApplicationCore WndProc installed after GLFW creates the
`HWND` and before the hidden window is shown. It stores and chains the GLFW
procedure, restores it once before destruction, and exposes one composition point
for native window features. A later MonaImGui viewport hook chains in this order:

```text
MonaImGui viewport procedure -> FGlfwWindow procedure -> GLFW procedure
```

Features such as modal-loop continuation extend this bridge instead of installing
a competing ApplicationCore hook.

## macOS Cocoa Bridge

The macOS editor root keeps a normal titled, closable, miniaturizable, resizable
`NSWindow`. An Apple-only Objective-C++ bridge adds the full-size-content-view
style, hides the system title text, and makes the title-bar background
transparent. AppKit retains and draws the standard close, minimize, and zoom
traffic-light controls, including their fullscreen and accessibility behavior.
Durin draws only the surrounding background, branding, project title, and menus.

The bridge does not replace GLFW's `NSWindowDelegate`. It installs one local
AppKit event monitor scoped to the exact native window and consumes only a
primary-button down inside a valid published drag region. Eligible events are
handed to `performWindowDragWithEvent:`; every native-control, menu, docking,
viewport, stale-layout, other-window, and non-primary event continues through
the existing GLFW/AppKit path.

All bridge setup, layout publication, metrics queries, appearance changes, and
teardown occur on the AppKit main thread. Setup is transactional. Failure
restores the original window properties and reports effective `System` before
first show. Teardown makes the monitor inert, removes it, restores bridge-owned
properties, and completes before GLFW destroys the Cocoa window.

## Windows Custom Frame

Custom mode starts from a decorated, resizable GLFW window. ApplicationCore keeps
`WS_CAPTION`, `WS_THICKFRAME`, `WS_MINIMIZEBOX`, `WS_MAXIMIZEBOX`, and
`WS_SYSMENU`, suppresses standard caption layout and painting, and applies
`SWP_FRAMECHANGED` before first show. Retaining the caption capability bit is
required for Windows to propose a maximized outer frame that contains the full
work-area client; removing it produces a positive top offset and clips the
rendered title bar even when the reported client extent is correct.
`WM_NCCALCSIZE` extends client rendering through the caption. Maximized bounds and
`WM_GETMINMAXINFO` use the nearest monitor work area. When an app bar on that
monitor is auto-hidden, the client expands through its otherwise reserved band
while retaining a one-physical-pixel shell activation edge. DWM shadow and corner
behavior remain available where Windows provides them. During a native
position-only modal loop, `WM_GETMINMAXINFO` chains through the original
procedure because maximized bounds do not affect movement; this keeps repeated
move validation free of synchronous shell app-bar queries.
Pure movement leaves the modal-loop continuation timer disabled so timer messages
and engine frames cannot delay native pointer-driven positioning. Each
custom-frame `WM_MOVING` update calls `DwmFlush` after native processing to pace
visible position changes against desktop composition.

If hook or frame setup fails, ApplicationCore restores the complete system frame,
reports effective `System`, and logs one error before the window becomes visible.
Custom activation forwards minimized windows through the original procedure.
For visible windows it invokes the original procedure with non-client repaint
suppressed, preserving default activation bookkeeping without restoring native
caption pixels over the custom frame.

## Windows Movement Pacing

The resolved custom-frame failure had a distinctive split symptom: editor and
viewport animation remained smooth while the window was stationary, but the
entire `HWND` advanced in uneven steps during a native title-bar drag. That split
distinguishes desktop-composition pacing from engine render throughput. Hit
testing already returned `HTCAPTION`, and Windows—not Durin—remained responsible
for calculating and applying the window position.

The primary cause on the affected Windows graphics-driver path was that native
`WM_MOVING` position updates and the Vulkan-backed client surface reached DWM
without a composition barrier owned by the custom-frame path. Windows could
therefore display position changes with uneven delivery cadence even though the
surface itself rendered smoothly. Calling `DwmFlush` after chained native
processing fixed the cadence because it waits for the calling application's
queued desktop-composition changes to become visible before the next move update
continues.

Two synchronous hot-path costs amplified the symptom but were not the primary
cause. A full render/RHI drain from every modal-loop continuation could hold the
WndProc while pointer updates waited, and custom `WM_GETMINMAXINFO` handling
could query four shell app-bar edges during repeated thick-frame move validation.
Removing either cost alone improved individual transitions but did not make
continuous movement composition-paced.

A later experiment restored editor refresh from a low-priority `WM_TIMER` during
movement and omitted the sizing-only render/RHI drain. It still caused smaller
but visible hitches. Timer priority controls when a frame callback begins; it
cannot preempt that frame after it starts, so newly arriving `WM_MOVING` messages
must wait for the game, UI, render-submission, and maintenance body to return.
The final policy therefore freezes client rendering at the last presented frame
for the duration of a pure move.

The maintained solution has these invariants:

1. Windows remains the only owner of drag position; Durin never follows the
   cursor with repeated `SetWindowPos` calls.
2. A custom-frame `WM_MOVING` chains through GLFW/native processing first and
   then calls `DwmFlush` exactly once.
3. Pure movement keeps the continuation timer disabled and DWM moves the last
   presented client surface without running engine frames in the WndProc.
4. `WM_SIZING` alone starts the 16 ms continuation timer; sizing and final
   continuations drain render/RHI work required for surface extent changes.
5. Only `WM_MOVING` calls `DwmFlush`; sizing timer messages do not create a
   second composition barrier, particularly for Vulkan presentation.
6. Position-only `WM_GETMINMAXINFO` uses the original procedure and does not
   query shell app bars.

If this symptom regresses, first compare stationary client animation with whole
window motion. Smooth client animation plus uneven `HWND` movement points back
to the native/DWM path; it is not evidence that Vulkan present mode or editor
frame rate should be changed. Automated tests cover callback admission, timer
lifetime, native-procedure chaining, and the min/max fast path, but perceived movement
cadence still requires an interactive custom-frame check. If the invariants hold
and movement remains uneven, capture the UI thread, DWM, and Vulkan present
timeline before adding another synchronization mechanism.

## Title-Bar Layout and Hit Testing

The renderer publishes one complete `FWindowTitleBarLayout` generation per frame.
Its rectangles are integer renderer-aligned logical client units and contain no
Win32, Cocoa, GLFW, DWM, or ImGui types. On Windows these units are client pixels.
The native bridge converts a `WM_NCHITTEST` screen point to client space once and
evaluates:

1. DPI-derived resize edges and corners;
2. close, maximize/restore, and minimize rectangles;
3. draggable rectangles;
4. ordinary client content.

The Windows mapping returns the corresponding resize result, `HTCLOSE`,
`HTMAXBUTTON`, `HTMINBUTTON`, `HTCAPTION`, or `HTCLIENT`. `HTMAXBUTTON` and normal
non-client processing preserve Windows 11 Snap Layout. Before the first rendered
generation, the hidden window uses a conservative layout matching the configured
title-bar metrics.

On macOS, Cocoa points convert once from bottom-left window coordinates to the
same top-left logical client coordinates published by MainFrame. Retina backing
scale is not applied to these logical hit regions; framebuffer pixels remain a
rendering concern. A reverse `FWindowTitleBarPlatformMetrics` snapshot reports
the live native traffic-light exclusion rectangle. MainFrame reserves that
leading region, publishes drag regions around it, and does not publish synthetic
caption-button regions on macOS.

## Interaction Ownership

ApplicationCore leaves window management with the native platform. On Windows
it owns window drag, resize, minimize, maximize/restore, close, system menus, and
caption double-click through non-client processing. Because custom mode
suppresses the standard caption interaction, the Windows bridge converts one
matched native caption-button press/release pair into exactly one corresponding
`WM_SYSCOMMAND`; `HTMAXBUTTON` remains the hit-test result used for Snap Layout.

On macOS, AppKit owns traffic-light hover/press/action, window drag, zoom,
fullscreen, and accessibility. MainFrame never imitates or overlays the native
controls. It draws Windows caption pixels from the interaction-state snapshot
and draws only the macOS title/menu content outside the platform exclusion.

The native title remains populated for task switching, system window menus,
accessibility, and platform metadata. GLFW callbacks remain authoritative for
close, focus, position, window size, and framebuffer size.

## DPI and Responsive Geometry

On Windows, resize thickness comes from `GetSystemMetricsForDpi` at hit-test
time. Published render rectangles already use active client-pixel coordinates
and receive no second DPI multiplication. MainFrame scales its 36-pixel base
geometry through global editor UI scale and publishes the resulting actual
rectangles.

macOS uses AppKit logical points for title-bar composition and queries the native
control frames every rendered title-bar frame, so resize, zoom, fullscreen, and
display changes do not depend on fixed traffic-light offsets.

The Windows custom editor frame enforces a 640 by 480 base minimum track size.
The platform applies window DPI, while the published layout can raise the minimum
width for editor UI scale. MainFrame removes the project title before menus or
caption regions and preserves explicit drag space at supported widths.
