# Texture System

Durin's current texture path is an editor-oriented Texture2D pipeline with
explicit source, platform, render-resource, editor, and material boundaries.

## Asset and Build Ownership

- `DTexture2D` owns the copied source-file reference plus the reflected `Usage`
  and `bSRGB` build settings.
- `FTextureSourceData` is decoded RGBA8 edit data. It records source dimensions,
  original channel count, and whether transparency is present.
- `FTexturePlatformData` is rebuilt from source data. It currently contains a
  complete uncompressed `RGBA8_UNORM` or `SRGBA8_UNORM` mip chain.
- Source and platform data are intentionally separate. Future compression,
  target-platform selection, and derived-data caching replace platform data
  without mutating the decoded source representation.
- Normal usage generates linear-space mips by averaging and renormalizing the
  encoded normal vector. Color usage filters RGB in linear space when sRGB is
  enabled. Data/Mask usage averages channels independently.

## Transactional Build-Setting Edits

The Texture Editor changes `Usage` and `bSRGB` through reflected-property
transactions. `DTexture2D::PreEditChangeProperty` builds complete candidate
platform data from detached proposal storage before the live setting changes.
An invalid usage or failed build rejects the proposal without changing the
asset.

After a successful write, `PostEditChangeProperty` atomically installs the
validated candidate and queues a new render-resource revision. Cancel, Undo,
and Redo use the same hooks and therefore rebuild the matching platform data.
Changing usage resets sRGB to that preset's default; editing sRGB afterward is
an explicit override. Committed edits dirty the package through the shared
reflected transaction path. Direct `SetUsage` and `SetSRGB` calls follow the
same rebuild rule and dirty the package after success.

## Render-Thread Boundary

Each texture owns one shared `FTexture2DRenderResource` proxy. Game-thread code
may retain the proxy but never reads its RHI texture. Build and release requests
carry monotonically increasing revisions to the render thread. Stale commands
cannot replace newer data, and every mip is uploaded before the new RHI texture
becomes the applied revision. Diagnostic resource state is tagged with the
request revision, and upload failures are reported only for the matching build;
a later request clears the prior failure instead of inheriting it permanently.

Material render data and static-mesh scene proxies retain the shared proxy, not
a reflected texture object or a raw RHI texture. Rebuilding a texture updates
that same proxy, so already-bound materials and previews observe the replacement
without rebinding reflected dependencies. Missing or not-yet-ready resources
resolve through renderer-owned default textures.

Before creating a texture, the render resource asks the active RHI whether the
selected format supports the requested optimal-tiling usage. Vulkan derives this
answer from physical-device format features. An unsupported format is rejected
before image creation and remains distinguishable from a general creation or
upload failure in the asset's persistent editor diagnostics.

## Editor Contract

`TextureEditor` registers a per-resource workspace for `DTexture2D`. It exposes:

- source file, dimensions, source channel count, transparency, and decoded format;
- transactional Usage and sRGB controls;
- platform format, mip count and range, byte size, residency policy, build
  revision, and current platform-data status;
- normal workspace save, Dirty, close protection, Undo, and Redo behavior.

The editor previews the built platform representation and allows each mip level
to be selected. Every open texture document owns independent preview state and
one registered preview texture, so simultaneously visible documents cannot
reuse or overwrite one another's image. Missing or invalid platform data falls
back to decoded source data when available; otherwise the preview is released.
Persistent source, decode, build, upload, and format status is shown with retry
controls. Content Browser thumbnails remain persistent derivatives of the
copied source image rather than the built platform representation.

## Current Limitations

- Platform data is rebuilt by decoding the source image during every normal
  `PostLoad`; there is no texture derived-data key or cooked payload.
- Platform format selection is uncompressed.
- Build work is synchronous, every mip is fully resident, and there is no memory
  accounting or streaming.
- The shipped material shader consumes only the base-color texture parameter.

## Related Code

- `Engine/Source/Runtime/Engine/Public/Texture/Texture2D.h`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp`
- `Engine/Source/Runtime/Engine/Private/Texture/Texture2DRenderResource.cpp`
- `Engine/Source/Editor/TextureEditor/`
- `Engine/Source/Runtime/Engine/Private/Materials/MaterialTypes.cpp`
- `Engine/Shaders/Slang/StaticMesh.slang`
