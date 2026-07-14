# Editor UI Style

Durin editor UI uses three layers of styling and persistence:

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

## Validation

Check editor UI changes in both color themes at 75%, 100%, 125%, 150%, and 200% UI scale.
Resize the Content Browser and Scene Viewport through all responsive modes and verify that
controls remain readable, clickable, and non-overlapping. Editor UI changes require a
`DurinEditor` build and runtime smoke test.
