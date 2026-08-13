# Editor Icon Atlas Investigation

Summary: Determine when editor viewport visualization icons should move from deterministic procedural masks to an offline-generated atlas.

**Status:** Deferred pending a larger icon set or an explicit visual-design requirement.

**Last reviewed:** 2026-08-10

## Verified Current Behavior

The editor viewport uses a small runtime-generated icon atlas for component
visualizers. Camera and directional-light glyphs are built as deterministic
supersampled masks in `RendererModule.cpp`, uploaded once during overlay-icon
resource initialization, and selected through `EViewOverlayIcon`.

The current implementation has no editor asset-loading dependency, introduces
no separately licensed source images, and keeps startup deterministic. Only two
component visualizers consume the atlas today, so replacing it now would add a
source-art format, rasterizer, packer, generated metadata, packaging rules, and
licensing workflow without resolving a demonstrated maintenance or rendering
problem.

## Observable Impact

There is no known correctness defect in the present two-icon path. The cost is
future-facing: procedural glyph code becomes harder to review and visually
iterate as the icon set grows, while hard-coded atlas placement would become
fragile if many independently authored icons were added.

This investigation should become an implementation plan only when at least one
of these triggers is observed:

- more than four to six viewport component icons are scheduled with real
  visualizer consumers;
- source artwork needs repeated designer iteration that procedural masks cannot
  represent economically;
- maintenance evidence shows the procedural glyph builder or fixed placement is
  causing regressions;
- a product requirement selects a consistent licensed icon family for viewport
  visualizers.

Normal editor UI icons remain owned by the UI/font icon system and do not count
toward this trigger.

## Candidate Direction

If a trigger is met, investigate a canonical editor-only SVG source directory,
deterministic offline rasterization and packing, generated stable identifiers
and normalized UV rectangles, transparent cell padding and UV inset, and
editor-resource packaging that excludes SVG inputs from game/runtime builds.
`EViewOverlayIcon` should remain the renderer-facing identity unless a concrete
consumer proves that contract insufficient. Missing or invalid generated data
would need a visible deterministic fallback.

This is a candidate direction, not an adopted architecture. Toolchain choice,
source-art ownership, generated-file policy, resolution/DPI strategy, and
licensing rules remain unresolved until a trigger supplies actual inputs and
constraints.

## Evidence and Validation Gaps

- No inventory links prospective point-light, spot-light, audio, reflection,
  physics, navigation, or gameplay icons to scheduled component visualizers.
- No comparison measures procedural versus authored glyph maintenance cost or
  startup/package impact.
- No rendering evidence establishes required atlas resolution, DPI behavior,
  padding, filtering, hovered/selected appearance, depth-tested behavior, or
  x-ray behavior for a larger set.
- No deterministic-generation proof demonstrates byte-identical atlas and
  metadata output from unchanged source artwork.
- No licensing and attribution policy has been selected for source art.

When a trigger is met, capture the consumer inventory, representative artwork,
visual acceptance images at multiple UI scales, generation determinism, cell
bleed tests, and package/startup measurements before selecting an implementation
plan.

## Related Documentation

- [Viewport Rendering](../Runtime/Rendering/ViewportRendering.md)
- [Rendering Capability Expansion Roadmap](../Roadmaps/Archive/2026-08/RenderingCapabilityExpansion.md)

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/IRendererModule.h`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Editor/LevelEditor/Public/LevelEditorCustomizations.h`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/CameraEditorCustomizations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/DirectionalLightEditorCustomizations.cpp`
