# Volumetric Cloud Authoring

Summary: Define editor ownership for volume inspection, cloud Details presentation, viewport policy, and nonblocking render diagnostics.

Modules: TextureEditor, LevelEditor, Engine, RenderCore, Renderer

Last reviewed: 2026-09-03

## Ownership

`DVolumeTexture` and `DVolumetricCloudComponent` remain the authored runtime
authorities. Texture Editor and Level Editor add presentation only: they do not
create an editor-only cloud representation, retain Renderer textures, or publish
scene state outside the normal reflected-property and view-submission paths.

Texture Editor registers an exact `DVolumeTexture` workspace independently of
the exact `DTexture2D` route. The document retains its asset while open and
releases its bounded two-dimensional preview when replaced or closed. Level
Editor registers the component Details customization and consumes the existing
World Outliner **Add Actors** path for `AVolumetricCloudActor` creation.

## Volume inspection

The volume workspace reports dimensions, format, mip and byte counts, build
state, and source provenance. It reads built platform data first and falls back
to resident source data when necessary. Preview extraction supports R8 and
RGBA8, all available mips and channels, and three exact orientations:

- `XY`: X increases right, Y increases down, selected Z slice.
- `XZ`: X increases right, Z increases down, selected Y slice.
- `YZ`: Y increases right, Z increases down, selected X slice.

The selected slice is clamped to the axis extent. One slice is expanded to
RGBA8 for the existing texture preview and is limited to 2048 x 2048 x 4 bytes
(16 MiB). Preview values are stored voxels; the generic editor does not infer
coverage, erosion, extinction, lighting, or weather semantics.

## Cloud Details and viewport policy

The component customization groups the existing reflected properties into
Activation, Density Inputs, Layer, Mapping and Motion, and Optical Response.
These categories use the standard expandable Details groups and matching search
automatically opens a containing group. One compact read-only status row
refreshes the shared Engine eligibility result and reports whether the component
is active, ignored by stable scene selection, or ineligible. Base, detail, and
weather use the standard asset-picker reveal action instead of cloud-specific
duplicate buttons. All editable rows keep normal reflection, transaction,
undo/redo, validation, dirtying, and serialization behavior.

Cloud quality and debug selection belong to `FSceneViewSettings`, not the
component. Each viewport submits one quality (`Performance`, `High`, `Epic`, or
`Reference`) and one debug mode. `High` and `Lit` are defaults. Changing either
affects the next submitted view, remains isolated from other view settings, and
does not dirty the world.

Debug presentation reuses the production composite and intermediates:

- `Lit` uses normal radiance/transmittance composition.
- `Radiance` shows premultiplied cloud radiance.
- `Transmittance` shows scalar transmittance.
- `TemporalStatus` is green for accepted history, amber for rejected history,
  and gray when unavailable.
- `ShadowVisibility` shows scalar receiver visibility and gray when unavailable.

Debug selection adds no retained cloud target, GPU query, or readback and does
not change temporal acceptance or commit.

## Diagnostics and lifecycle

Renderer reduces private counters into the value-owned
`FSceneViewVolumetricCloudStatistics` member of `FSceneViewStatistics`.
`FSceneViewport` publishes the completed value through its existing immutable
statistics snapshot and revision contract. Level Editor reads only that copy;
it never flushes rendering or waits for the GPU.

The snapshot is capped at 160 bytes and reports quality/debug policy,
compute/fragment route and reason, shadow route, target/output extents,
primary/light/shadow work, active/retained/history/shadow bytes, and temporal
availability/acceptance. P5 intentionally creates no live timing query, so the
panel labels GPU time unavailable and qualification tests remain the timing
authority. Disabled rendering adds no pass, query, readback, or editor-owned
cache. Module unload releases workspace, customization, and preview ownership
before editor services are retired.

## Related documentation

- [Volumetric cloud authoring guide](../Guides/VolumetricCloudAuthoring.md)
- [Volume textures](../../Runtime/Assets/VolumeTextures.md)
- [Volumetric cloud scene contract](../../Runtime/Rendering/VolumetricCloudSceneContract.md)
- [Volumetric cloud temporal reconstruction](../../Runtime/Rendering/VolumetricCloudTemporalReconstruction.md)

## Related code

- `Engine/Source/Editor/TextureEditor/Private/Widgets/MVolumeTextureEditor.cpp`
- `Engine/Source/Editor/TextureEditor/Public/VolumeTexturePreview.h`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/VolumetricCloudDetails.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPresentation.cpp`
- `Engine/Source/Runtime/RenderCore/Public/VolumetricCloudView.h`
- `Engine/Source/Runtime/RenderCore/Public/ViewRenderStatistics.h`
