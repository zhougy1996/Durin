# Editor Icon Atlas Plan

Last reviewed: 2026-07-20

## Current Status

The editor viewport currently uses a small runtime-generated icon atlas for
component visualizers. Camera and directional-light glyphs are built as
deterministic supersampled masks in `RendererModule.cpp`, uploaded once during
overlay-icon resource initialization, and selected through `EViewOverlayIcon`.

This is an acceptable implementation while the icon set remains small: it has
no editor asset-loading dependency, introduces no separately licensed source
images, and keeps startup behavior deterministic. Move to an offline-generated
atlas when the set grows beyond roughly four to six icons, when visual design
requires frequent iteration, or when procedural glyph code becomes harder to
maintain than source artwork.

## Offline Atlas Pipeline

- [ ] Define a canonical source-art directory for editor-only SVG icons.
- [ ] Add an offline build step that rasterizes the sources at the required
  resolution and packs them into a single RGBA atlas.
- [ ] Generate icon metadata containing stable icon identifiers and normalized
  UV rectangles instead of hard-coding atlas coordinates in the renderer.
- [ ] Add transparent padding around every packed icon and inset sampled UVs to
  prevent linear-filter bleeding between neighboring cells.
- [ ] Keep `EViewOverlayIcon` as the renderer-facing identifier and validate
  that every enum value has exactly one generated atlas entry.
- [ ] Package the generated atlas and metadata as editor resources without
  making game/runtime builds depend on the SVG sources.
- [ ] Replace `BuildEditorIconAtlasPixels()` with loading of the generated
  output while retaining a visible fallback for missing or invalid entries.
- [ ] Document source-art licensing and attribution requirements.

## Migration Candidates

- [ ] Point light.
- [ ] Spot light.
- [ ] Audio source and listener.
- [ ] Reflection capture.
- [ ] Physics and collision helpers.
- [ ] Navigation and gameplay marker components.

Only add candidates backed by an actual component visualizer. The atlas should
not become a general UI icon library; normal editor UI icons remain owned by the
UI/font icon system.

## Validation Gaps

- [ ] Test that generated metadata covers all `EViewOverlayIcon` values and has
  valid, non-overlapping UV rectangles within the atlas bounds.
- [ ] Add a deterministic-generation check so unchanged SVG inputs produce
  byte-identical atlas and metadata outputs.
- [ ] Verify camera and directional-light icons at multiple UI scales and DPI
  settings, including hovered, selected, depth-tested, and x-ray passes.
- [ ] Verify that neighboring high-contrast icons do not bleed at atlas cell
  boundaries under linear sampling.
- [ ] Run the viewport visualization tests, a successful full `all` build, and
  a `DurinEditor` startup/rendering smoke test after migration.

## Recommended Implementation Order

1. Keep the current procedural atlas while only Camera and Directional Light
   use viewport icons.
2. Define SVG conventions and stable icon identifiers when the next group of
   component visualizers is scheduled.
3. Implement deterministic rasterization, packing, metadata generation, and
   padding validation.
4. Switch the renderer to generated resources and remove procedural glyph code.
5. Add visual regression coverage before expanding the atlas further.

## Related Code

- `Engine/Source/Runtime/RenderCore/Public/IRendererModule.h`
- `Engine/Source/Runtime/Renderer/Private/RendererModule.cpp`
- `Engine/Source/Editor/LevelEditor/Public/LevelEditorCustomizations.h`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/CameraEditorCustomizations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/DirectionalLightEditorCustomizations.cpp`
