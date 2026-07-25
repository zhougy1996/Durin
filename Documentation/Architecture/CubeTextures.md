# Cube Textures

This document defines the coordinate, face-order, and source-image orientation
contract shared by cube-texture import, the RHI, VulkanRHI, and sky rendering.

## Coordinate System

Durin uses the following world-space basis:

- forward is `+X`
- right is `+Y`
- up is `+Z`

A skybox shader reconstructs a world-space direction from the current view. It
removes camera translation and applies the inverse skybox-component rotation
before sampling the cube texture.

## Face and Array-Layer Order

One `TextureCube` is one square two-dimensional image with six physical array
layers. Layers always use this order:

| Array layer | Face | Principal world direction |
| ---: | --- | --- |
| 0 | `+X` | forward |
| 1 | `-X` | backward |
| 2 | `+Y` | right |
| 3 | `-Y` | left |
| 4 | `+Z` | up |
| 5 | `-Z` | down |

The source images are stored top-to-bottom in normal image-file row order.
Import does not apply an implicit horizontal or vertical flip.

## Source-Image Orientation

The following table defines what world direction lies at each edge when a face
image is viewed normally, with its first row at the top. `U` increases from the
left edge to the right edge and `V` increases from the top edge to the bottom
edge.

| Face | Image center | Top edge | Right edge | Bottom edge | Left edge |
| --- | --- | --- | --- | --- | --- |
| `+X` | `+X` | `+Y` | `-Z` | `-Y` | `+Z` |
| `-X` | `-X` | `+Y` | `+Z` | `-Y` | `-Z` |
| `+Y` | `+Y` | `-Z` | `+X` | `+Z` | `-X` |
| `-Y` | `-Y` | `+Z` | `+X` | `-Z` | `-X` |
| `+Z` | `+Z` | `+Y` | `+X` | `-Y` | `-X` |
| `-Z` | `-Z` | `+Y` | `-X` | `-Y` | `+X` |

This orientation follows cube sampling after adapting the texture-coordinate
row direction to Durin's top-to-bottom source images. Import UI and validation
errors must display the face names and must not require users to know layer
numbers.

## Direction-to-Face Ground Truth

For a nonzero sampling direction `D = (X, Y, Z)`, select the component with the
largest absolute magnitude. Ties use the first matching axis in `X`, `Y`, `Z`
order. Let `Ma` be that magnitude. The selected face and normalized image
coordinates are:

| Selected face | Condition | `U` | `V` |
| --- | --- | ---: | ---: |
| `+X` | `X > 0` and `abs(X)` is largest | `(-Z / Ma + 1) / 2` | `(-Y / Ma + 1) / 2` |
| `-X` | `X < 0` and `abs(X)` is largest | `( Z / Ma + 1) / 2` | `(-Y / Ma + 1) / 2` |
| `+Y` | `Y > 0` and `abs(Y)` is largest | `( X / Ma + 1) / 2` | `( Z / Ma + 1) / 2` |
| `-Y` | `Y < 0` and `abs(Y)` is largest | `( X / Ma + 1) / 2` | `(-Z / Ma + 1) / 2` |
| `+Z` | `Z > 0` and `abs(Z)` is largest | `( X / Ma + 1) / 2` | `(-Y / Ma + 1) / 2` |
| `-Z` | `Z < 0` and `abs(Z)` is largest | `(-X / Ma + 1) / 2` | `(-Y / Ma + 1) / 2` |

The six principal-axis cases all produce `(U, V) = (0.5, 0.5)` on the
corresponding face. Directional test images use a distinct center color per
face and labeled edge markers matching the source-orientation table.

## Validation Rules

- Width and height must be equal and nonzero.
- A cube must contain exactly six layers in the order above.
- All faces use the same dimensions, pixel format, mip count, and source row
  convention.
- Decoded faces remain RGBA8 source data. Desktop platform data uses one BC
  format for the complete cube: opaque color uses BC1 and any transparency in
  any face promotes all six faces to BC3.
- Texture2D uploads use array slice 0. Cube uploads name their face explicitly
  and translate it to the corresponding array slice.
- Vulkan cube images use the cube-compatible image flag and one cube image view
  spanning base layer 0 through layer 5.
- Cube render resources query backend format support before image creation and
  preserve unsupported-format diagnostics separately from general upload
  failures.

## Validation Readback and Layout Tracking

- Textures intended for CPU verification opt into `CPUReadback`, which adds
  transfer-source usage. `RHIReadTexture2D` synchronously copies exactly one
  named mip and array slice into tightly packed bytes; compressed subresources
  retain their block-row representation. This is a validation and diagnostic
  path, not a per-frame renderer path.
- Vulkan readback transitions only the requested subresource from its tracked
  layout to transfer source and back. It finalizes the command list, waits for
  completion, invalidates noncoherent staging memory, and copies the result
  before destroying the staging allocation.
- Render-pass attachment final layouts are committed to each Vulkan texture's
  per-mip/per-slice layout tracker when the pass ends. Later upload, readback,
  and sampling barriers therefore start from the layout actually established
  by the render pass rather than a stale creation or upload layout.
- Cube render resources opt into readback so the Stage 6 Vulkan integration
  test can compare every face and every mip with the asset platform data before
  checking rendered pixels.

## Sky Component and Scene Snapshot

- `DSkyBoxComponent` owns a reflected `TObjectPtr<DTextureCube>`, linear Tint,
  nonnegative Intensity, a serialized stable scene GUID, and a runtime instance
  ID. The reflected
  pointer participates in package dependency tracking, serialization, and GC.
- Translation and scale remain ordinary authored transform data but do not
  enter the sky snapshot. Only the component's world rotation is published.
- Registration, visibility, rotation, texture, Tint, and Intensity changes
  enqueue revisioned snapshot replacement or removal through `IScene`.
- `FScene` owns snapshots only on the rendering thread. A snapshot contains no
  reflected object pointer; it retains the cube render-resource proxy instead.
- Multiple visible sky components are retained so editor diagnostics can report
  the conflict. The active entry is selected by serialized scene GUID, then
  stable object path for duplicated GUIDs, independent of registration order.
- Per-instance revision tombstones prevent an older queued replacement or
  removal from overriding newer state. Runtime instance IDs also keep duplicated
  components with the same serialized GUID as distinct scene entries.

## Sky Rendering

- The renderer uses a dedicated fullscreen triangle generated from
  Vulkan `VertexIndex`; its pipeline has an empty vertex declaration and binds
  no vertex buffer.
- The sky pipeline targets the Scene Color pass with blending, culling, depth
  testing, and depth writes disabled. It draws before static meshes; editor
  grid, gizmos, lines, and icons are composed later and therefore remain above
  the sky.
- The fragment shader transforms a far clip-space position through inverse
  view-projection, subtracts the camera world position, and normalizes the
  result. It then applies the inverse normalized component rotation before cube
  sampling. Component translation and scale never enter this transform.
- The draw uses the view's fitted viewport and scissor, preserving black
  letterbox regions. The same path is independent of Lit/Unlit and
  Solid/Wireframe mesh settings.
- Ready asset resources are sampled with a renderer-owned linear clamp sampler.
  Missing, rebuilding, failed, or deleted resources bind the renderer-owned
  black cube. A scene with no active sky issues no sky draw.
- sRGB cube formats perform hardware conversion to linear on sampling. The
  shader multiplies that linear value by `Tint * Intensity`, writes Scene
  Color, and leaves the existing post-process/output conversion path unchanged.

## Editor Workflow

- The Content Browser's Import menu provides `Texture Cube...`. Its modal owns
  one source slot for each named face in `+X/-X/+Y/-Y/+Z/-Z` order and displays
  both the corresponding Durin world direction and the source-image top/right
  orientation from this document.
- The modal calls the same decode, shape, cross-face consistency, mip-build, and
  platform-format validation used by final import. Its confirmation action
  remains disabled until all six sources and the mounted destination are valid,
  and it revalidates immediately before creating any files.
- Content Browser tiles use a stable cube icon and identify the asset as
  `Texture Cube`. Selection details load the asset summary and report its
  dimensions, six-face count, and mip count.
- `Sky Box Actor` is available through the ordinary reflected actor-creation
  menu and owns its `DSkyBoxComponent` by default. The component's reflected
  object property names `DTextureCube` as its required class, so the shared
  Details asset picker excludes incompatible assets.
- Tint, nonnegative Intensity, and actor/component rotation use ordinary
  reflected edits. Preview edits publish scene updates and committed edits
  dirty the level package through the standard property transaction path.
- If more than one visible skybox is registered, the component Details view
  shows a nonblocking warning naming the active actor and every ignored actor.
  The editor model mirrors the renderer's GUID, object-path, then instance-ID
  ordering; it does not query render-thread scene state.

## Related Code

- `Engine/Source/Runtime/Core/Public/Math/Vector.h`
- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/SkyBoxComponent.h`
- `Engine/Source/Runtime/Engine/Public/IScene.h`
- `Engine/Source/Runtime/Renderer/Private/Scene.cpp`
- `Engine/Source/Runtime/Renderer/Private/SkyBoxRendering.cpp`
- `Engine/Shaders/Slang/SkyBox.slang`
- `Engine/Source/Editor/LevelEditor/Private/Assets/TextureCubeImportDialog.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/SkyBoxDetails.cpp`
