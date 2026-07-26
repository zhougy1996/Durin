# Texture Support Plan

Summary: Texture2D assets, platform data, material sampling, and validation.

Last reviewed: 2026-07-26

## Current Status

The first usable Texture2D-to-static-mesh workflow is complete. The Content
Browser can import PNG, JPEG, BMP, and TGA images through a texture-specific
dialog, show persistent source-image thumbnails, and expose the resulting asset
through the schema-driven Material Editor texture picker. Materials and material
instances preserve texture references, inheritance, and local overrides, while
resolved static-mesh material slots snapshot the render resource without letting
the renderer read reflected material objects. The render thread uploads every mip
and the static-mesh shader samples the base-color texture through a shared
linear-wrap sampler. Missing or not-yet-ready resources resolve to the
renderer-owned white texture.

The current build remains an editor-oriented implementation rather than a
production texture pipeline. A warm `DTexture2D::PostLoad` restores platform
data from DDC without decoding the copied source image, but there is not yet a
cooked, source-independent runtime package and all built mips are fully
resident. A dedicated Texture Editor shows source and platform diagnostics and
changes usage, the explicit sRGB override, maximum resolution, compression
quality, and alpha mip policy through validated reflected transactions with
Dirty, save, cancel, Undo, and Redo behavior. Normal and Data/Mask currently
affect build defaults and mip filtering only; the shipped material shader
consumes only the base-color texture parameter.

The original focused `FTexture2DTests.*` and `FEditorTextureSmokeTests.*` suites
passed on 2026-07-21 using the `Win64-Debug-DurinEditor-Tests` preset. On
2026-07-24, the new reflected build-setting coverage passed all six
`FTexture2DTests.*`, including rejection, Dirty state, and Undo/Redo behavior.
The built-preview review on 2026-07-24 isolated preview state per document,
made render-resource diagnostics and failures revision-aware and recoverable,
invalidated stale derived data when a copied source disappears, and replaced
handwritten status-name switches with reflected enum metadata. All ten focused
`FTexture2DTests.*` and all 200 `EngineTests` passed, followed by a successful
full `all` build and an eight-second `DurinEditor --hidden-window` smoke run.
Stage 2 was formally closed on 2026-07-25 after all ten focused
`FTexture2DTests.*` passed again, confirming the built mip preview and persistent,
recoverable source/build/upload failure-state foundation.
The same day's first compression prerequisite added backend texture-format
capability queries and rejects unsupported optimal-tiling usages before Vulkan
image creation while preserving a distinct Unsupported Format diagnostic. The
ten focused texture tests, a full `all` build, and a hidden-window editor smoke
run passed after the change.
The Vulkan upload path now derives staging row pitch and payload size from shared
pixel-format block metadata, including NPOT extents and sub-4x4 tail mips, rather
than treating compressed blocks as individual texels. All eleven focused
`FTexture2DTests.*`, a full `all` build, and an eight-second
`DurinEditor --hidden-window` Vulkan smoke run passed after the change.
The desktop builder now selects BC1 for opaque Color, BC3 for transparent Color,
BC5 for Normal, and BC7 for Data/Mask, encodes every mip through a commit-pinned
`bc7enc_rdo` dependency, and extends NPOT block edges without changing logical
mip dimensions. All eleven focused texture tests and all 204 `EngineTests`
passed, followed by a successful full `all` build and a ten-second
`DurinEditor --hidden-window` Vulkan smoke run.
Maximum resolution now selects a mip-aligned base level without introducing a
second resize path, while Low, Normal, and High compression quality settings
control the search effort used by every desktop BC encoder. Both settings are
serialized and rebuild through the same validated Texture Editor transactions
as Usage and sRGB, including Dirty, Undo, and Redo behavior.
All twelve focused `FTexture2DTests.*` and all 205 `EngineTests` passed after
the change, followed by a successful full `all` build and a ten-second
`DurinEditor --hidden-window` Vulkan smoke run.
Stage 3 was closed on 2026-07-26 with a dedicated hardware-backed Vulkan test.
The test uploads and explicitly samples all three mip levels of known linear and
sRGB RGBA8 textures plus builder-produced BC1, BC3, BC5, and BC7 payloads. GPU
readback verifies mip selection, sRGB decode, compressed color channels, and
compressed alpha independently of a swapchain or visible window.
Stage 4 is now in progress. Texture2D packages retain the imported source-content
hash and lightweight source fingerprint, while versioned, checksummed platform
mip payloads are stored beneath the project Derived Data Cache. A warm
`PostLoad` validates the source fingerprint and restores platform data without
opening or decoding the source image; changed sources, changed build settings,
missing entries, and corrupt payloads become safe rebuilds. Cooked payload
packaging remains open. The shared asset-data lifecycle now reserves `.bin` for
unreferenced, rebuildable DDC objects and `.dbulk` for manifest-owned cooked
payloads. The initial cooked layout uses one package-relative companion per
`.dasset` while keeping payload references relocatable into a future archive.
All four focused `FDerivedDataCacheTests.*`, all fifteen focused
`FTexture2DTests.*`, and all 214 `EngineTests` passed, followed by a successful
full `all` build and a ten-second `DurinEditor --hidden-window` smoke run. The
full `CoreTests` executable was also attempted; its DDC tests passed, but eleven
unrelated Logger tests failed because their work-directory log files could not
be opened for writing in the current environment.

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
- [x] Schema-driven Material Editor texture selection, inherited-source display,
  override reset, save/reload, and live preview invalidation.
- [x] Static-mesh base-color sampling with a shared linear-wrap sampler and white fallback.
- [x] Static-mesh material-slot resolution and live dependency updates preserve
  the selected texture render-resource snapshot.
- [x] RHI and Vulkan format definitions for uncompressed and BC texture formats.
- [x] Usage- and alpha-driven BC1, BC3, BC5, and BC7 desktop compression.
- [x] Vulkan support for uploading individual mip levels.
- [x] Persistent project-local Content Browser thumbnails derived from texture
  source images, with invalidation and regeneration.
- [x] Dedicated per-resource Texture Editor with source/platform diagnostics,
  save state, and transactional build-setting controls.
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
- [x] Select textures from the Material Editor and preserve inherited or local
  instance values through undo/redo and save/reload.
- [x] Add an editor smoke test that imports a texture, assigns it to a material,
  and verifies that it is visible on a static mesh.

## Implementation Stages

### Stage 1: Post-Import Build Settings

- [x] Add a dedicated Texture2D asset editor or Details workflow.
- [x] Expose source dimensions, transparency, usage, sRGB override, platform
  format, mip count, and build diagnostics after import.
- [x] Allow usage and sRGB changes to rebuild transactionally, participate in
  undo/redo, dirty the package, and refresh dependent materials and previews.

#### Acceptance Gate

- The Content Browser opens Texture2D assets in a per-resource editor; valid
  setting changes rebuild platform data and the shared render resource, invalid
  proposals leave the asset unchanged, save/Dirty/close behavior is consistent,
  and focused transaction plus full integration validation passes.

### Stage 2: Built Texture Preview and Failure States

- [x] Preview the built platform texture and selectable mip levels rather than
  only the copied source image.
- [x] Surface missing-source, decode, build, upload, and unsupported-format
  states to the user.

#### Acceptance Gate

- The editor can inspect the actual built mip chain and presents actionable,
  persistent state for every source/build/upload failure boundary.

### Stage 3: Desktop Platform Formats and Compression

The current platform build produces a complete compressed desktop BC mip chain
according to the asset usage, transparency, explicit color-space override,
maximum resolution, compression quality, and opt-in alpha-coverage policy.
These remain explicit serialized build settings rather than permanent behavior
inferred from the source filename.

- [x] Add sRGB versus linear color-space selection.
- [x] Generate a complete mip chain with usage-appropriate image filters.
- [x] Add texture usage presets, initially Color, Normal, and Data/Mask.
- [x] Select platform formats from usage and alpha requirements.
- [x] Add BC1/BC3/BC5/BC7 compression for supported desktop targets.
- [x] Add maximum-resolution and quality settings.
- [x] Decide how alpha coverage should be preserved while generating mips.
- [x] Validate platform-format support before creating the RHI resource.

#### Acceptance Gate

- Color, Normal, and Data/Mask usages select their intended desktop formats;
  supported BC payloads remain valid for non-block-aligned dimensions and every
  mip uploads and samples through Vulkan, while unsupported device formats fail
  before resource creation with an actionable Texture Editor diagnostic.

### Stage 4: Versioned Derived Data and Cooked Payloads

- [x] Define the shared authored, source, DDC, cooked package, and cooked bulk
  lifecycle rules, including `.bin` versus `.dbulk` semantics.
- [x] Serialize or cache built platform data so normal asset loading does not
  decode the source image on every `PostLoad`.
- [x] Define a derived-data key that includes the source content, build
  settings, target platform, and texture builder version.
- [ ] Ensure cooked/runtime builds do not require the original PNG, JPEG, BMP,
  or TGA file.

#### Acceptance Gate

- Warm editor loads restore validated platform mip data without source decoding;
  source or setting changes and incompatible or corrupt cache objects rebuild
  safely, and cooked runtime packages can load without source-image files.

### Stage 5: Asynchronous Build and Material Readiness

- [ ] Move source decoding and platform-data construction off the main thread.
- [ ] Define load, unload, and failure states visible to material resolution.

#### Acceptance Gate

- Decode and build work cannot stall the main thread, and materials consistently
  resolve the correct fallback across load, build, upload, unload, and failure.

### Stage 6: Residency Accounting

- [ ] Add residency accounting and memory statistics.
- [ ] Defer texture streaming until profiling demonstrates that full residency
  is no longer acceptable.

#### Acceptance Gate

- CPU platform bytes and resident GPU texture bytes are attributable and visible;
  streaming remains deferred unless measured workloads exceed the accepted
  full-residency budget.

## Validation Gaps

- [ ] Test render-resource build, replacement, stale-revision rejection, and
  release on the render thread.
- [ ] Test default-texture fallback while an asset is missing or not ready.
- [x] Test real Vulkan upload and shader sampling, including multiple mip levels.
- [x] Test sRGB and linear textures against known sample values.
- [x] Test compressed formats and non-block-aligned dimensions.
- [ ] Test failed imports and rebuilds for transactional cleanup of packages and
  copied source files.
- [x] Run a successful full `all` build and `DurinEditor` smoke test after the
  material and editor integration lands.

## Later Scope

These features are intentionally outside the first Texture2D/material slice:

- [ ] HDR source formats and floating-point texture assets.
- [ ] DDS or KTX ingestion with prebuilt mip and compression data.
- [x] Cube-map assets and static skybox sampling. See
  [Cube Textures](../Runtime/Rendering/CubeTextures.md) and
  [SkyBoxComponent](Archive/2026-07/SkyBoxComponent.md).
- [ ] Equirectangular LDR and Radiance HDR panorama import into an LDR cube
  asset. See
  [Equirectangular Texture Cube Import](EquirectangularTextureCubeImport.md).
- [ ] Texture arrays, cube-map arrays, and volume textures.
- [ ] Virtual textures or sparse residency.
- [ ] Runtime-generated and writable texture assets.
- [ ] Per-material sampler assets if shared sampler policies become insufficient.

## Recommended Implementation Order

1. Preview the built platform texture and selectable mip levels in the Texture
   Editor, and expose actionable load/build/upload failures.
2. Finish desktop compression controls with maximum resolution, quality, alpha
   coverage, and focused Vulkan sampling coverage.
3. Cache built platform data behind a versioned derived-data key so ordinary
   loads and cooked/runtime builds do not depend on source decoding.
4. Move decode and build work off the main thread and make load/build/upload
   states visible to material fallback and the editor.
5. Add residency accounting; introduce streaming only when profiling justifies it.
6. Add advanced texture types and additional material texture roles only when
   their consuming renderer paths are defined.

## Related Documentation

- `Documentation/Runtime/Assets/AssetPackages.md`
- `Documentation/Runtime/Assets/AssetDataLifecycle.md`
- `Documentation/Runtime/Rendering/MaterialSystem.md`
- `Documentation/Runtime/Core/RuntimeLifecycle.md`
- `Documentation/Runtime/Rendering/TextureSystem.md`
- `Documentation/Plans/MaterialSystem.md`
- `Documentation/Plans/Archive/2026-07/AssetRegistryAndThumbnailCache.md`
- `Documentation/Plans/Archive/2026-07/MaterialParameterDomainRefactor.md`
- `Documentation/Plans/Archive/2026-07/StaticMeshMaterialSlots.md`

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialTypes.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureImportDialog.cpp`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Editor/MaterialEditor/Private/Widgets/MMaterialEditor.cpp`
- `Engine/Shaders/Slang/StaticMesh.slang`
