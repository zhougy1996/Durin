# Window Frame Contract

Summary: Define platform-window decoration modes and the Windows custom-title-bar boundary.

Modules: ApplicationCore, MonaCore, MonaImGui, MainFrame

## Decoration Modes

`EWindowDecorationMode` is selected before native creation:

- `System` lets the platform draw the frame and caption.
- `CustomTitleBar` retains native window-management capabilities while Durin draws the caption pixels.
- `None` creates an undecorated window.

Requested and effective modes are distinct. Unsupported custom mode becomes
effective `System`; callers branch layout from the effective mode so fallback
never produces two title bars or no title bar. Runtime transitions are supported
only between `System` and `None`. The Windows editor root requests custom mode;
detached ImGui viewports and other windows retain their prior policy.

## Shared Windows Message Bridge

`FGlfwWindow` owns the ApplicationCore WndProc installed after GLFW creates the
`HWND` and before the hidden window is shown. It stores and chains the GLFW
procedure, restores it once before destruction, and exposes one composition point
for native window features. A later MonaImGui viewport hook chains in this order:

```text
MonaImGui viewport procedure -> FGlfwWindow procedure -> GLFW procedure
```

Features such as modal-loop continuation extend this bridge instead of installing
a competing ApplicationCore hook.

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
behavior remain available where Windows provides them.

If hook or frame setup fails, ApplicationCore restores the complete system frame,
reports effective `System`, and logs one error before the window becomes visible.

## Title-Bar Layout and Hit Testing

The renderer publishes one complete `FWindowTitleBarLayout` generation per frame.
Its rectangles are integer window-client pixels and contain no Win32, GLFW, DWM,
or ImGui types. Native code converts a `WM_NCHITTEST` screen point to client space
once and evaluates:

1. DPI-derived resize edges and corners;
2. close, maximize/restore, and minimize rectangles;
3. draggable rectangles;
4. ordinary client content.

The Windows mapping returns the corresponding resize result, `HTCLOSE`,
`HTMAXBUTTON`, `HTMINBUTTON`, `HTCAPTION`, or `HTCLIENT`. `HTMAXBUTTON` and normal
non-client processing preserve Windows 11 Snap Layout. Before the first rendered
generation, the hidden window uses a conservative layout matching the configured
title-bar metrics.

## Interaction Ownership

ApplicationCore owns window drag, resize, minimize, maximize/restore, close,
system menus, and caption double-click through native non-client processing.
Because custom mode suppresses the standard caption interaction, the shared
Windows bridge converts one matched native caption-button press/release pair into
exactly one corresponding `WM_SYSCOMMAND`; `HTMAXBUTTON` remains the hit-test
result used for Snap Layout.
MainFrame only draws pixels. It reads a value snapshot containing hovered part,
pressed part, focus, and maximized state, and never places ImGui buttons over
caption regions.

The native title remains populated for the taskbar, Alt+Tab, accessibility, and
system-menu metadata. GLFW callbacks remain authoritative for close, focus,
position, window size, and framebuffer size.

## DPI and Responsive Geometry

Resize thickness comes from `GetSystemMetricsForDpi` at hit-test time. Published
render rectangles already use active client-pixel coordinates and receive no
second DPI multiplication. MainFrame scales its 36-pixel base geometry through
global editor UI scale and publishes the resulting actual rectangles.

The Windows custom editor frame enforces a 640 by 480 base minimum track size.
The platform applies window DPI, while the published layout can raise the minimum
width for editor UI scale. MainFrame removes the project title before menus or
caption regions and preserves explicit drag space at supported widths.
