# Equirectangular Texture Cube Import Plan

Summary: Offline LDR and Radiance HDR panorama projection into rebuildable LDR TextureCube assets.

Last reviewed: 2026-07-26

## Current Status

All stages and acceptance gates are complete as of 2026-07-26.
`DTextureCube` now serializes an explicit
source layout, package-relative panorama provenance, face-dimension override,
exposure, and source dimensions. LDR and Radiance HDR panorama imports validate
and build before filesystem mutation, copy one authoritative source beside the
package, rebuild deterministically on `PostLoad`, and participate in
layout-aware move, rename, and delete transactions. Reimport validates and
builds detached replacement data before changing the copied source, serialized
settings, platform data, or render-resource revision, and rolls back publication
if package save fails. The `Texture Cube...` editor modal now preserves separate
six-face and panorama inputs while switching modes, exposes bounded panorama
dimension and HDR exposure settings, revalidates changes and final import, and
previews the projection and LDR output. Content Browser selection details expose
the authoritative panorama, source dimensions, import settings, and LDR format.

Validation evidence on `Win64-Debug-DurinEditor-Tests`: 32 AssetCoreTests,
51 RenderCoreTests, one VulkanRHITest, and all 271 EngineTests pass. The
hardware-backed sky test runs on an NVIDIA GeForce RTX 3090 with
`VK_LAYER_KHRONOS_validation`; it reads every face and mip of LDR and
tone-mapped HDR panorama cubes, checks compressed/sRGB rendered colors,
longitude-seam and face-boundary continuity, camera translation, component
rotation and replacement, letterboxing, foreground occlusion, and resource
retirement. The complete `all` target builds, and an eight-second
`DurinEditor --hidden-window` smoke reaches successful editor initialization
without Error, Fatal, VUID, or Vulkan Validation diagnostics.

User-confirmed visible checks with representative panoramas at multiple
viewport aspect ratios passed on 2026-07-26. No axis swap, implicit flip,
panorama-seam break, pole artifact, camera parallax, letterboxing issue, or
foreground-occlusion regression was observed. Lasting contracts are recorded
in the owning runtime documentation, and this completed plan is archived. The
first slice deliberately does not introduce floating-point texture assets,
BC6H build output, image-based lighting, or runtime panorama sampling.

## Goal

Users can select one ordinary 2:1 panorama or Radiance HDR environment image in
the Content Browser, import it as a correctly oriented `DTextureCube`, assign it
to a `Sky Box Actor`, and retain a rebuildable reference to the original
panorama after save, reload, move, and reimport.

## Scope

- Add an equirectangular source mode to `DTextureCube` without regressing the
  existing six-face source mode.
- Accept 2:1 PNG, JPEG, BMP, and TGA panoramas through the existing RGBA8 image
  decoder.
- Add bounded Radiance `.hdr` float decoding for this import path.
- Project the panorama into Durin's documented
  `+X/-X/+Y/-Y/+Z/-Z` face convention on the CPU during asset import or rebuild.
- Decode and resample LDR color in linear space, then encode the projected cube
  as sRGB RGBA8 source data.
- Apply import-time exposure and one deterministic filmic tone-mapping curve to
  HDR inputs before encoding the projected cube as sRGB RGBA8 source data.
- Derive a default face dimension from panorama angular density and permit a
  bounded explicit override.
- Preserve the original panorama beside the `.dasset` package as the
  authoritative rebuild and reimport source.
- Extend the Content Browser import dialog, validation, diagnostics, asset
  summary, automated tests, and user/runtime documentation.

## Non-Goals

- OpenEXR decoding.
- Floating-point `DTexture2D` or `DTextureCube` source/platform data.
- `RGBA16_FLOAT`, `R11G11B10_FLOAT`, or BC6H texture building and compression.
- Runtime equirectangular texture sampling or GPU conversion.
- Diffuse irradiance, specular prefiltering, BRDF lookup textures, sky lights,
  reflection probes, or any other image-based-lighting feature.
- Changing Scene Color, post process, output transfer, or display-HDR behavior.
- Importing cube crosses, vertical/horizontal strips, DDS, KTX, or cube arrays.
- Editing or painting panoramas inside Durin.
- A dedicated interactive three-dimensional cube preview.
- Solving general cross-face seamless mip filtering for manually imported
  six-face assets.

## Design Decisions and Invariants

### Asset and Source Ownership

- `DTextureCube` owns an explicit serialized source-layout value rather than
  inferring layout from which source strings happen to be nonempty.
- Existing assets default to the six-face layout so old serialized packages
  remain loadable without migration edits.
- A panorama-backed cube stores one package-relative copied source filename,
  projection settings, and HDR transform settings. Generated faces are derived
  data and are not written as six additional authoritative image files.
- `FTextureCubeSourceData` remains the common six-face RGBA8 build boundary.
  Both source layouts must produce this structure before the existing mip,
  compression, render-resource, and RHI paths run.
- Import validates and decodes the external file before creating a package or
  copying a source. A failed import, replacement, move, or delete must not leave
  a partial package, orphaned source, or mixed-layout asset.
- Asset move/delete contributors operate only on the active layout's source
  files and preserve rollback behavior.

### Projection and Coordinate Contract

- Input must have an exact 2:1 width-to-height ratio, nonzero dimensions, and
  remain within checked decode and projection memory limits.
- A panorama viewed normally uses top-left image origin. Its horizontal center
  points toward Durin `+X` (forward), moving right rotates toward `+Y` (right),
  and its top edge approaches `+Z` (up).
- For normalized direction `D`, panorama coordinates are defined as:
  `U = 0.5 + atan2(D.y, D.x) / (2*pi)` and
  `V = 0.5 - asin(clamp(D.z, -1, 1)) / pi`.
- Horizontal sampling wraps at `U=0/1`; vertical sampling clamps at the poles.
  Bilinear taps follow the same rules and never read outside the decoded image.
- Cube-face pixel centers convert to directions through one shared inverse of
  the authoritative direction-to-face convention in
  `Documentation/Runtime/Rendering/CubeTextures.md`. Import code must not carry
  a second undocumented axis or flip table.
- The derived default face dimension is `panorama width / 4`, matching the
  panorama's equatorial angular texel density. An explicit override is a
  positive, validated face dimension and does not change projection semantics.
- The projector writes all six faces in the existing top-to-bottom source-row
  convention and preserves their fixed array-layer order.

### Color and HDR Contract

- LDR input is interpreted as sRGB color. Samples are decoded to linear before
  bilinear interpolation and encoded back to sRGB RGBA8 afterward.
- Radiance HDR input is decoded to finite linear RGB float values. NaN,
  infinity, negative channels, malformed scanlines, dimension overflow, and
  truncated payloads fail with actionable diagnostics.
- Exposure is stored in EV and applies the linear multiplier `exp2(EV)` before
  tone mapping. Import and `PostLoad` rebuild must produce byte-identical output
  for the same source and settings.
- Stage 0 selects and documents one fixed filmic tone-mapping curve, its white
  behavior, clamp behavior, and golden numeric cases. The editor does not expose
  competing tone-mapper choices in this slice.
- Alpha is opaque for Radiance HDR input. LDR alpha is projected consistently;
  any projected transparency promotes the complete cube to the existing BC3
  path.
- The resulting asset remains an sRGB LDR color cube. `Tint` and `Intensity`
  continue to execute in the existing sky shader and are not silently folded
  into imported pixels.

### Threading, Failure, and Runtime Boundaries

- Decode, projection, tone mapping, mip construction, and compression are
  editor/asset-build work and never run on the render thread.
- The renderer and `DSkyBoxComponent` continue to consume only the existing
  `FTextureCubeRenderResource`; neither learns about panorama provenance.
- Rebuild publication retains the existing revisioned render-resource contract.
  A failed reimport leaves the last valid asset and render resource intact.
- Projection loops use checked size arithmetic and bounded allocation. CPU task
  parallelism may be added only if output ordering and byte determinism remain
  unchanged.
- Source diagnostics identify whether failure occurred during file validation,
  LDR/HDR decode, projection, color transform, platform build, source copy, or
  package publication.

## Current Foundations and Gaps

| Layer | Reusable foundation | Required gap |
| --- | --- | --- |
| Image decode | Bounded RGBA8 `Asset::DecodeImageFromFile` using `stb_image` | Bounded Radiance HDR float result and format-specific diagnostics |
| Cube convention | Documented face order, edge orientation, `ResolveTextureCubeFaceUv`, and labeled test cube | Shared face-UV-to-direction inverse and panorama-UV projection tests |
| Texture build | `FTextureCubeSourceData`, full mip chains, BC1/BC3 compression, and validation | Projected RGBA8 source construction from one panorama |
| Asset lifecycle | Transactional six-face import plus reload, move, and delete contributors | Explicit source layout, one-file panorama provenance, settings, and rollback |
| Render resource | Revisioned six-layer upload and stable sky rendering | No runtime change required beyond regression validation |
| Editor | `Texture Cube...` modal and source validation | Source-mode selection, panorama file/settings UI, and derived summary |
| Tests | Cube asset, orientation, Vulkan upload/readback, sky rendering, and editor workflow coverage | Analytical panorama fixtures, HDR/color golden cases, and panorama lifecycle coverage |

## Implementation Stages

### Stage 0: Lock Projection, Color, and Compatibility Contracts

This stage resolves the only intentionally open design detail—the exact fixed
filmic curve—and turns the coordinate and source-layout decisions into
executable ground truth.

- [x] Add the panorama coordinate convention, exact 2:1 requirement, default
  face-size rule, and LDR/HDR color behavior to the cube-texture runtime
  documentation.
- [x] Select one fixed filmic tone-mapping curve and document its formula,
  exposure order, negative/nonfinite handling, output clamp, and golden numeric
  examples.
- [x] Define the serialized source-layout enum and compatibility rule for
  packages that predate the enum.
- [x] Define authoritative source filenames for panorama imports and
  rename/move behavior without changing existing six-face suffixes.
- [x] Add a small analytical equirectangular LDR fixture whose principal axes,
  seam, poles, and face edges are unambiguous.
- [x] Add a small Radiance HDR fixture with known linear values above and below
  display range.
- [x] Define memory-limit cases and checked formulas for panorama pixels, six
  projected faces, and RGBA8/float byte counts.

#### Acceptance Gate

- Documentation and fixtures uniquely determine every principal direction,
  face edge, panorama seam, pole, exposure result, and output byte value needed
  by later tests.
- An old six-face asset has one unambiguous deserialized layout and unchanged
  source ownership.
- No color, projection, source naming, or compatibility decision remains open
  before implementation starts.

### Stage 1: Implement Bounded Decode and Projection Foundations

Depends on Stage 0. This stage produces validated CPU face data without creating
or mutating an asset.

- [x] Add a bounded float-image result and Radiance HDR decode entry point under
  AssetCore, keeping the existing RGBA8 API and supported-extension behavior
  unchanged for other importers.
- [x] Validate Radiance headers, dimensions, scanline encoding, payload length,
  checked allocation, finite channels, and deterministic error messages.
- [x] Add a shared cube-face pixel-center-to-direction helper consistent with
  `ResolveTextureCubeFaceUv` and the documented top-left row convention.
- [x] Implement horizontal-wrap/vertical-clamp bilinear equirectangular sampling.
- [x] Implement sRGB decode/resample/encode for LDR sources.
- [x] Implement linear HDR sampling, EV exposure, the Stage 0 filmic curve, and
  sRGB RGBA8 encoding.
- [x] Project all faces directly into `FTextureCubeSourceData`, including
  transparency aggregation and explicit face-dimension validation.
- [x] Add focused unit tests for principal axes, all documented face edges,
  seam wrapping, poles, LDR linear-space interpolation, HDR golden values,
  invalid inputs, and allocation limits.

#### Acceptance Gate

- Both fixtures project into six valid, identically sized RGBA8 faces whose
  sampled pixels match Stage 0 ground truth.
- LDR interpolation does not occur in encoded sRGB space, and HDR output matches
  the documented exposure and tone-map values.
- Malformed or oversized data fails before unsafe allocation and leaves output
  structures empty.

### Stage 2: Integrate Panorama Provenance into DTextureCube

Depends on Stage 1. This stage makes panorama-derived cubes persistent,
rebuildable assets while preserving six-face behavior.

- [x] Add the reflected source-layout enum, panorama source filename, face
  dimension, and exposure fields to `DTextureCube`.
- [x] Keep existing six-face getters and packages compatible; expose explicit
  provenance queries rather than returning invented generated-face filenames.
- [x] Add panorama validation and import APIs that reuse Stage 1 projection and
  the existing `BuildCubePlatformData` path.
- [x] Copy the original panorama beside the package and serialize only the
  package-relative authoritative filename.
- [x] Make `PostLoad` resolve and project the active source layout before the
  common platform build.
- [x] Extend move/delete contributors and transactional rollback for the
  panorama source without touching inactive layout fields.
- [x] Make reimport validate and build a replacement completely before swapping
  source/settings/platform data or publishing a new render-resource revision.
- [x] Report source layout, original panorama dimensions, derived face
  dimension, mip count, output pixel format, and last build failure through
  stable asset diagnostics.
- [x] Add asset tests for import, save/reload, old-package compatibility,
  rebuild determinism, reimport success/failure, missing/corrupt source, move,
  rename, delete, and rapid revision replacement.

#### Acceptance Gate

- One LDR panorama and one Radiance HDR panorama can each be imported, saved,
  unloaded, reloaded, and rebuilt into the expected existing LDR cube resource.
- Existing six-face asset tests and serialized packages behave unchanged.
- Failed import or reimport leaves no partial files and does not replace the
  last valid asset or GPU resource.

### Stage 3: Complete the Editor Import Workflow

Depends on Stage 2. This stage makes the feature usable without external
conversion tools or test code.

- [x] Add `Six Faces` and `Equirectangular Panorama` source modes to the
  `Texture Cube...` modal.
- [x] In panorama mode, provide one source picker with appropriate LDR and
  Radiance HDR filters, a bounded face-dimension override, and HDR-only exposure.
- [x] Show the source panorama dimensions, derived/output face dimension,
  projection convention summary, mip count, and output format before import.
- [x] Revalidate immediately when source mode, file, dimension, or exposure
  changes and again before filesystem mutation.
- [x] Disable irrelevant settings without losing the user's current values when
  switching modes in one dialog session.
- [x] Make validation messages distinguish incorrect aspect ratio, unsupported
  extension, decode failure, excessive size, invalid override, and HDR color
  failure.
- [x] Extend Content Browser selection details to identify panorama provenance
  and HDR-to-LDR import settings.
- [x] Add an editor workflow test covering panorama selection, import, actor
  creation, assignment, level save/reload, and persisted source/settings.

#### Acceptance Gate

- A new user can complete
  `2:1 panorama -> Texture Cube -> Sky Box Actor -> viewport sky` entirely in
  DurinEditor.
- Invalid settings cannot create files, while a valid import produces an asset
  whose summary clearly states that the runtime result is LDR.
- Switching back to six-face mode retains the existing workflow and validation.

### Stage 4: Validate Rendering, Lifecycle, and Documentation

Depends on Stages 1-3. This stage closes production and handoff evidence without
expanding into native HDR or IBL.

- [x] Reuse the hardware-backed Vulkan cube test to read every generated face
  and mip from panorama-derived platform data.
- [x] Render and verify all six principal directions, face boundaries,
  longitude seam placement, camera translation invariance, component rotation,
  letterboxing, and foreground occlusion.
- [x] Compare LDR and tone-mapped HDR imports against deterministic pixel
  tolerances after BC compression and sRGB sampling.
- [x] Exercise rapid reimport, missing-source recovery, asset move/delete,
  component replacement, editor shutdown, and render-resource retirement with
  Vulkan Validation enabled.
- [x] Run the affected AssetCore, Engine, RenderCore, Vulkan, and editor tests
  through the repository BuildTool workflow referenced by
  `Documentation/Development/Build/BuildAndRun.md`.
- [x] Complete a full build and hidden-window DurinEditor smoke run from one
  Agent Build Profile.
- [x] Perform visible checks with representative outdoor day, sunset, and
  indoor panoramas at multiple viewport aspect ratios.
- [x] Move lasting source-layout, projection, color, asset-lifecycle, and editor
  workflow contracts into the owning runtime/editor documentation.
- [x] Update this plan's status, checklist, and evidence, then archive it only
  after every required gate passes.

#### Acceptance Gate

- Automated, hardware-backed, and visible checks show no axis swap, implicit
  flip, panorama-seam break, pole artifact, camera parallax, or lifecycle error.
- Existing six-face import and skybox rendering regressions remain green.
- Vulkan Validation, the full build, and the editor smoke run complete without
  task-related diagnostics.
- Long-lived behavior is documented outside the plan and every Definition of
  Done item has recorded evidence.

## Validation Matrix

| Dimension | Required cases | Primary evidence |
| --- | --- | --- |
| LDR input | PNG/JPEG/BMP/TGA, corrupt file, wrong aspect, oversized image, alpha | AssetCore/projector and asset tests |
| HDR input | Valid old/new Radiance scanlines, exposure range, corrupt/truncated data, nonfinite/negative samples | HDR decoder and golden color tests |
| Projection | Six principal axes, every documented face edge, longitude seam, north/south poles | Analytical CPU fixture tests |
| Color | sRGB-linear interpolation, HDR exposure, fixed filmic curve, sRGB encode | Numeric golden tests |
| Resolution | Derived default, explicit override, NPOT input/output, minimum/maximum and overflow | Validation and mip-build tests |
| Compatibility | Existing six-face packages, import, reload, move, delete, rendering | Existing plus new asset regression tests |
| Transactionality | Import failure, reimport failure, copy/package failure, rollback | Filesystem and asset lifecycle tests |
| Resource lifetime | Initial build, rapid rebuild, missing source, replacement, deletion, shutdown | Revision and render-thread integration tests |
| GPU result | Six faces and mips, BC1/BC3, sRGB decode, rendered direction pixels | Vulkan readback and rendering tests |
| Editor workflow | Mode switch, validation, import, assignment, save/reload | Editor workflow test and visible check |

## Definition of Done

- [x] The Content Browser imports a valid 2:1 LDR or Radiance HDR panorama as a
  correctly oriented existing-format `DTextureCube`.
- [x] The copied original panorama and serialized settings rebuild
  deterministically after save/reload and survive asset move/rename.
- [x] HDR input is clearly identified as an offline HDR-to-LDR transform and
  never implies floating-point runtime storage or IBL.
- [x] Existing six-face assets and their editor workflow remain compatible.
- [x] Invalid or failed import/reimport operations leave no partial artifacts
  and preserve the last valid asset and render resource.
- [x] CPU projection, color, asset lifecycle, editor workflow, Vulkan readback,
  and rendered-result tests pass.
- [x] The full build, Vulkan Validation run, hidden-window editor smoke, and
  representative visible checks pass.
- [x] Lasting contracts are recorded in the owning documentation and this plan
  is archived according to `Documentation/Plans/AGENTS.md`.

## Deferred Follow-ups

- OpenEXR decoding and shared float-image source infrastructure.
- Native floating-point Texture2D/TextureCube source and platform data.
- BC6H compression, float cube preview/readback, and HDR display output.
- Seam-aware cross-face mip filtering for all cube source layouts.
- Asynchronous projection/compression with progress and cancellation.
- Diffuse irradiance convolution, specular prefiltering, BRDF integration, sky
  lights, reflection probes, and material IBL.
- Additional panorama layouts such as cube cross, strips, fisheye, mirror ball,
  and equiangular cubemap.

## Related Documentation

- [Documentation Guide](../../../README.md)
- [Texture Support Plan](../../TextureSupport.md)
- [Cube Textures](../../../Runtime/Rendering/CubeTextures.md)
- [Viewport Rendering](../../../Runtime/Rendering/ViewportRendering.md)
- [Build and Run](../../../Development/Build/BuildAndRun.md)
- [Native Tests](../../../Development/Build/NativeTests.md)
- [Archived SkyBoxComponent Plan](SkyBoxComponent.md)

## Related Code

- `Engine/Source/Runtime/AssetCore/Public/ImageDecoder.h`
- `Engine/Source/Runtime/AssetCore/Private/ImageDecoder.cpp`
- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/Engine/Public/Texture/TextureCube.h`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCube.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureBuild.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/TextureCubeRenderResource.cpp`
- `Engine/Source/Runtime/Renderer/Private/SkyBoxRendering.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureCubeImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Tests/Native/EngineTests/Private/TextureCubeTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/SkyBoxTests.cpp`
