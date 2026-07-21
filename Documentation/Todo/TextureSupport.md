# Texture Support TODO

Last reviewed: 2026-07-21

## Current Status

Texture support has a working asset, material, and render-resource vertical
slice, but it is not yet exposed as a complete editor workflow. The current
implementation can import a `DTexture2D`, preserve it through material and
material-instance parameters, rebuild its CPU platform data, upload it through
the render thread, and sample it as a static-mesh base-color texture. Missing
or unavailable resources resolve to the renderer-owned white texture.

The focused `FTexture2DTests.*` and `FEditorTextureSmokeTests.*` suites passed
on 2026-07-21 using the `Win64-Debug-DurinEditor-Tests` preset. The editor
workflow smoke test imports a texture and mesh, assigns the texture through a
material, and verifies the static-mesh scene proxy carries geometry, UVs, and
the imported texture render resource. Texture imports expose Color, Normal, and
Data/Mask presets, build complete usage-aware mip chains, and retain an explicit
color-space override. The complete 117-test `EngineTests` target, a full `all`
build, and an eight-second `DurinEditor` Vulkan/shader smoke test also passed on
2026-07-21.

## Implemented

- [x] `DTexture2D` asset type and source-file tracking.
- [x] PNG, JPEG, BMP, and TGA decoding to RGBA8 source data.
- [x] Source transparency detection and image-size safety limits.
- [x] Separate source data and platform data representations.
- [x] Asset import, package save/load, rename/move, and deletion handling.
- [x] Render-thread-owned RHI texture creation, upload, replacement, and release.
- [x] Revision checks that prevent stale render commands from replacing newer resources.
- [x] Renderer-owned white, black, and flat-normal fallback textures.
- [x] Base-color material texture parameters, instance inheritance, and local overrides.
- [x] Static-mesh base-color sampling with a shared linear-wrap sampler and white fallback.
- [x] RHI and Vulkan format definitions for uncompressed and BC texture formats.
- [x] Vulkan support for uploading individual mip levels.
- [x] Focused import, reload, move, delete, and invalid-input tests.

## Required for the First Usable End-to-End Workflow

- [x] Add texture parameters to `DMaterialInterface`, `DMaterial`, and
  `DMaterialInstance`, including inheritance and local overrides.
- [x] Preserve texture asset references through reflection, serialization, and
  garbage collection.
- [x] Snapshot render-resource references into `FMaterialRenderData` without
  allowing renderer code to read reflected material objects.
- [x] Bind a base-color texture and sampler in the static-mesh fragment shader.
- [x] Resolve missing, unloaded, and not-yet-ready material textures to the
  appropriate renderer default texture.
- [x] Define the initial sampler policy. A shared linear-wrap sampler is
  sufficient for the first vertical slice.
- [x] Add a texture import entry and dialog to the editor and Content Browser.
- [x] Show imported texture assets with an image thumbnail or preview.
- [x] Add an editor smoke test that imports a texture, assigns it to a material,
  and verifies that it is visible on a static mesh.

## Texture Build Pipeline

The current platform build produces a complete uncompressed `RGBA8_UNORM` or
`SRGBA8_UNORM` mip chain according to the asset usage and explicit color-space
override. Extend it
with explicit build settings rather than inferring permanent behavior from the
source filename.

- [x] Add sRGB versus linear color-space selection.
- [x] Generate a complete mip chain with usage-appropriate image filters.
- [x] Add texture usage presets, initially Color, Normal, and Data/Mask.
- [ ] Select platform formats from usage and alpha requirements.
- [ ] Add BC1/BC3/BC5/BC7 compression for supported desktop targets.
- [ ] Add maximum-resolution and quality settings.
- [ ] Decide how alpha coverage should be preserved while generating mips.
- [ ] Validate platform-format support before creating the RHI resource.

## Derived Data and Residency

- [ ] Serialize or cache built platform data so normal asset loading does not
  decode the source image on every `PostLoad`.
- [ ] Define a derived-data key that includes the source content, build
  settings, target platform, and texture builder version.
- [ ] Ensure cooked/runtime builds do not require the original PNG, JPEG, BMP,
  or TGA file.
- [ ] Move source decoding and platform-data construction off the main thread.
- [ ] Define load, unload, and failure states visible to material resolution.
- [ ] Add residency accounting and memory statistics.
- [ ] Defer texture streaming until profiling demonstrates that full residency
  is no longer acceptable.

## Validation Gaps

- [ ] Test render-resource build, replacement, stale-revision rejection, and
  release on the render thread.
- [ ] Test default-texture fallback while an asset is missing or not ready.
- [ ] Test real Vulkan upload and shader sampling, including multiple mip levels.
- [ ] Test sRGB and linear textures against known sample values.
- [ ] Test compressed formats and non-block-aligned dimensions.
- [ ] Test failed imports and rebuilds for transactional cleanup of packages and
  copied source files.
- [x] Run a successful full `all` build and `DurinEditor` smoke test after the
  material and editor integration lands.

## Later Scope

These features are intentionally outside the first Texture2D/material slice:

- [ ] HDR source formats and floating-point texture assets.
- [ ] DDS or KTX ingestion with prebuilt mip and compression data.
- [ ] Texture arrays, cube maps, cube-map arrays, and volume textures.
- [ ] Virtual textures or sparse residency.
- [ ] Runtime-generated and writable texture assets.
- [ ] Per-material sampler assets if shared sampler policies become insufficient.

## Recommended Implementation Order

1. Complete the base-color material texture and shared-sampler rendering path.
2. Add editor import, assignment, thumbnail, and preview support.
3. Add color-space settings, mip generation, and texture usage presets.
4. Add desktop block compression and derived-data caching.
5. Add render-thread, Vulkan, and editor end-to-end validation.
6. Add residency management and advanced texture types only when required.
