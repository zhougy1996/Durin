# Editor UI Style

Summary: Define editor visual tokens, spacing, typography, colors, icons, and interaction styling.

Modules: MonaImGui, DurinEd

This document defines the implemented visual-style, design-token, layout, and
theme conventions for the Durin editor UI.

The editor uses three layers of styling and persistence:

- `MonaImGui` owns shared design metrics, global UI scaling, semantic colors, and reusable controls.
- `Engine/Configs/DurinEditorTheme.*.yaml` owns ImGui and semantic theme colors only.
- `LevelEditorSession.yaml` and `imgui.ini` own user preferences and user-adjusted layout state.

## Choosing Where A Value Lives

Use `ImGuiStyle` for standard padding, spacing, borders, and rounding. Use
`MonaImGui::GetUIStyleMetrics()` for shared editor measurements and
`MonaImGui::ScaleUI()` for component-specific measurements expressed in base design units.

Keep a value local and give it a descriptive name when it defines one component's geometry,
such as a viewport icon path or content tile layout. Do not share unrelated values merely
because they currently have the same number.

Persist values only when users can change them. Product defaults and validation limits stay
in type-safe C++ policy constants; colors that must vary between light and dark themes use
`EUIThemeColor` and the YAML `SemanticColors` map.

Zeros, normalized ratios used by algorithms, collection counts, and mathematical tolerances
are not UI design tokens.

## Dynamic Font Sizes

ImGui 1.92 bakes a separate font entry for each font and requested pixel size. Any editor
control that derives an explicit font size from a continuous value, such as a slider, zoom,
animation, or resizable tile, must pass that size through
`MonaImGui::QuantizeDynamicFontSize()` before calling size-aware APIs such as
`ImFont::CalcTextSizeA()`, `ImDrawList::AddText()`, or `ImFont::GetFontBaked()`.

The helper rounds to a shared 4-pixel grid. Use the same quantized value for measurement,
layout, and drawing. Fixed semantic font sizes and calls that use the current ImGui font size
do not need quantization. Do not introduce a component-local rounding step; keeping one grid
across the editor bounds font-atlas growth and maximizes reuse of baked glyphs.

`ImGui::PushFont()` is different: its size argument is a base size before global scaling.
Choose it from a bounded, discrete set of semantic sizes; do not pass a continuously changing
value or a value returned by `QuantizeDynamicFontSize()` directly.

## Responsive Layout

Level Editor toolbars classify their available width as full, compact, or narrow with
`ResolveEditorUILayout`. Full layouts expose all controls, compact layouts prioritize search
and primary actions, and narrow layouts move secondary actions to another row or an overflow
menu. New panels should degrade in the same order instead of clipping fixed-width controls.

## Bottom Drawers

`MonaImGui` owns the reusable bottom-drawer presentation primitive. A drawer is
an undecorated transient ImGui overlay anchored to a caller-provided workspace
rectangle; it does not create or split a dock node and does not change the
underlying viewport's render allocation. Feature modules own tool selection,
shortcuts, badges, and the content drawn inside it.

The default drawer height is 36% of its anchor, clamped between scaled 180- and
720-unit limits. Its top edge is vertically resizable for the current session.
Open/close motion uses a short time-based transition, recomputes its screen
geometry every frame, and leaves no interactive rectangle after it becomes
invisible. Ordinary focus loss does not dismiss a drawer because editor tools
may begin a drag operation inside it and complete the drop on the viewport.

A selected status-bar action toggles its drawer; selecting another drawer tool
switches content in place. Escape dismisses only when the drawer owns focus and
no text input, popup, active item, or drag-and-drop operation has precedence.
Drawer headers provide close and open-in-window actions. At compact widths the
open-in-window label degrades to an icon with a tooltip.

## Editor Main Window Title Bar

On Windows, the editor root window uses one integrated title/menu bar drawn by
`MainFrame`. Secondary windows keep their system or undecorated frame policy.
If custom-title-bar activation fails, the root keeps the system caption and the
workspace host draws its ordinary client menu bar.

The base geometry before global UI scaling is a 36-pixel bar, a 20-pixel Durin
mark centered in a 42-pixel safe slot, 8-pixel control padding, and three 46-pixel
caption regions. The order is brand, `Durin - <project>` title, a centered
18-pixel separator with 12-pixel side gaps, File/Edit/Tools/Window/Help menus,
explicit drag space, minimize, maximize/restore, and close. At narrow widths the
title and its separator truncate and then disappear together; menus and caption
regions remain intact.

Use `ImGuiCol_MenuBarBg` for the bar, normal/disabled text for active/inactive
windows, and normal button hover/active colors for minimize and maximize. Close
uses `EUIThemeColor::Error` only while hovered or pressed. The established Durin
brand-mark geometry is shared with the project browser and avoids another image
lifetime in MainFrame.

Caption regions are drawn but are not ImGui buttons. ApplicationCore publishes
native hover, pressed, focus, and maximized state; Windows owns caption commands
and Snap Layout. Menu rectangles remain client input, while only the title and
explicit spacer are draggable.

## Validation

Check editor UI changes in both color themes at 75%, 100%, 125%, 150%, and 200% UI scale.
Resize the Content Browser and Scene Viewport through all responsive modes and verify that
controls remain readable, clickable, and non-overlapping. Editor UI changes require a
`DurinEditor` build and runtime smoke test.
