# Cube Textures

Summary: Define cube-texture assets, source ingestion, platform payloads, upload, and rendering use.

Modules: Engine, AssetForgeBuiltins, TextureBuild, Renderer, RHI

This document defines the coordinate, face-order, and source-image orientation
contract shared by cube-texture import, the RHI, VulkanRHI, and sky rendering.

Runtime Engine owns reflected source provenance, TextureCube runtime/platform
values, serialization, Cooked loading, detached publication, and render
resources. `AssetForge/Builtins/TextureCubeImport.h` owns validation,
format admission, typed source translation, import/reimport, mounted-source
mutation, package save, and rollback. One immutable source capture supplies the
bytes, hash, size, path, and fingerprint used by each operation. TextureBuild owns source-independent face/panorama recipes and
DDC policy. Runtime Engine has no authoring callback bundle; the only uncooked
load seam is the independently reversible AssetForgeBuiltins
`TextureCubePostLoadPolicy`, which reuses the same translator.

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

## Editor Thumbnail Sampling

The TextureCube provider presents the authored cube as an opaque 100-degree
environment and samples through the face, row, and direction contract above.
It supplies a counted stable texture reference as one submission-local view
environment; accepted work retains no asset, concrete resource, Actor,
Component, or Scene membership. Provider identity, scheduling, revision checks,
persistence, failure, and reset behavior are owned by
[Asset Thumbnails](../../Editor/Architecture/AssetThumbnails.md).

## Equirectangular Panorama Import

An equirectangular panorama is an offline source layout for the existing LDR
cube build path. It does not change runtime sampling or introduce a
floating-point texture format.

### Source Layout and Compatibility

`DTextureCube` serializes an `ETextureCubeSourceLayout` value with these stable
numeric values:

| Serialized value | Enumerator | Authoritative source |
| ---: | --- | --- |
| 0 | `SixFaces` | Six ordered normalized source-provenance values |
| 1 | `EquirectangularPanorama` | One normalized panorama provenance value |

The property initializer is `SixFaces`. Packages written before the property
existed therefore retain value 0 when deserialization leaves the missing
property at its initialized value. Unknown serialized values are invalid and
must not be inferred from nonempty source strings. Only the active layout owns
source files; inactive-layout strings never participate in rebuild, move, or
delete.

Six-face imports retain the `<AssetName>_px`, `_nx`, `_py`, `_ny`, `_pz`, and
`_nz` suffixes and their current extension behavior in complete mounted
`FSourcePath` values. A panorama ingested into managed storage places its
authoritative source at the explicitly selected writable destination, normally
ending in `<AssetName>_panorama<extension>`, where `extension` is the accepted source
extension normalized to lowercase, including its leading period. Provenance
stores exact XXH3-128 source hashes plus decoder version 1 and projection
version 1. Moving or deleting a package does not move or delete potentially
shared source art. Legacy face and panorama filename fields are rejected.

### Derived Data and Cooking

Builder identity covers the active source layout and hashes, face dimension,
finite exposure, sRGB policy, schema versions, target platform, and profile.
Cube TXPL schema 1 uses the shared texture envelope with exactly six matching
slices in the frozen `+X/-X/+Y/-Y/+Z/-Z` order. Cook strips source provenance
and publishes the payload under stable ID
`d52878ce-8f50-48c7-a3c7-ff846e2c4c5a`. Generic DDC, build-session, Cook, DBLK,
and runtime fallback rules are defined by
[Asset Data Lifecycle and Storage](../Assets/AssetDataLifecycle.md) and
[Texture System](TextureSystem.md).

### Projection Coordinates

The decoded panorama has nonzero dimensions and must satisfy `Width ==
2 * Height` using checked arithmetic. It uses normal top-left image origin.
The horizontal center looks toward `+X`, moving right rotates toward `+Y`, and
the top edge approaches `+Z`.

For a normalized direction `D`:

```text
U = 0.5 + atan2(D.y, D.x) / (2 * pi)
V = 0.5 - asin(clamp(D.z, -1, 1)) / pi
```

`U` is reduced to `[0, 1)` so the longitude seam wraps. `V` is clamped to
`[0, 1]`. For a face of dimension `N`, pixel `(x, y)` uses
`u = (x + 0.5) / N` and `v = (y + 0.5) / N`. With
`a = 2 * u - 1` and `b = 2 * v - 1`, normalize the following vector to obtain
the panorama sampling direction:

| Face | Unnormalized direction |
| --- | --- |
| `+X` | `( 1, -b, -a)` |
| `-X` | `(-1, -b,  a)` |
| `+Y` | `( a,  1,  b)` |
| `-Y` | `( a, -1, -b)` |
| `+Z` | `( a, -b,  1)` |
| `-Z` | `(-a, -b, -1)` |

This table is the algebraic inverse of the direction-to-face ground truth
above. Projectors must use the shared implementation of this inverse.

For bilinear sampling, continuous texel coordinates are
`X = U * Width - 0.5` and `Y = V * Height - 0.5`. Both horizontal tap indices
wrap modulo `Width`; both vertical tap indices clamp to
`[0, Height - 1]`. Fractional weights are computed before index wrap or clamp.
This makes the seam continuous and extends the first and last source rows to
the poles without an out-of-range read.

The default face dimension is `max(1, floor(Width / 4))` using integer
division. An explicit override is in `[1, 4096]`. It replaces only the output
dimension and never changes coordinate or filtering behavior.

### LDR Color

PNG, JPEG, BMP, and TGA panorama RGB channels are sRGB encoded. Each bilinear
tap is decoded to linear before interpolation:

```text
linear(c) = c / 12.92                              when c <= 0.04045
linear(c) = ((c + 0.055) / 1.055) ^ 2.4           otherwise
```

Here `c` is the normalized encoded channel. Interpolated linear RGB is encoded
with the inverse transfer:

```text
srgb(c) = 12.92 * c                                when c <= 0.0031308
srgb(c) = 1.055 * c ^ (1 / 2.4) - 0.055           otherwise
```

Clamp encoded RGB to `[0, 1]` and quantize with
`floor(value * 255 + 0.5)`. Alpha is linear, receives the same bilinear
weights, is clamped and quantized by the same rule, and promotes the complete
cube to BC3 when any projected byte is below 255.

### Radiance HDR Color

Radiance `.hdr` pixels decode to finite, nonnegative linear RGB float values
and opaque alpha. Malformed input, a negative or nonfinite decoded channel, or
a nonfinite exposure result fails the build before publication. Exposure is a
finite EV value in `[-16, 16]` and is applied first:

```text
x = decodedLinear * exp2(EV)
```

Each exposed channel then uses the fixed ACES fitted filmic curve:

```text
filmic(x) = clamp(
    (x * (2.51 * x + 0.03)) /
    (x * (2.43 * x + 0.59) + 0.14),
    0, 1)
```

The curve is evaluated independently per channel. Zero maps to zero. Input
validation rejects negative and nonfinite values rather than silently
clamping them. Values whose finite curve result exceeds one are clamped to
one; there is no adjustable white point. The result is converted to sRGB and
RGBA8 with the LDR encoding and quantization rule above.

The following scalar cases are golden ground truth. Decimal intermediates are
shown for review; the final byte is authoritative for tests and rebuilds.

| Linear input | EV | Exposed | Filmic | sRGB | Byte |
| ---: | ---: | ---: | ---: | ---: | ---: |
| 0 | 0 | 0 | 0 | 0 | 0 |
| 0.18 | 0 | 0.18 | 0.2668989204 | 0.5534575720 | 141 |
| 0.18 | 2 | 0.72 | 0.7250070156 | 0.8677025342 | 221 |
| 1 | 0 | 1 | 0.8037974684 | 0.9082304957 | 232 |
| 4 | 0 | 4 | 0.9734171097 | 0.9882227102 | 252 |
| 16 | 0 | 16 | 1 after clamp | 1 | 255 |

### Import Allocation Limits

All products below are checked in `uint64` before conversion to `size_t` or
allocation. Failure clears the destination result.

| Quantity | Checked formula | Limit |
| --- | --- | ---: |
| Encoded source | file byte count | 512 MiB |
| Panorama pixels | `Width * Height` | 33,554,432 pixels |
| LDR decode | `Width * Height * 4` | 128 MiB at the pixel limit |
| HDR decode | `Width * Height * 3 * sizeof(float)` | 384 MiB at the pixel limit |
| Projected RGBA8 cube | `6 * FaceDimension * FaceDimension * 4` | 384 MiB at dimension 4096 |

Each source dimension must also be at most 16384. The exact 2:1 check, pixel
limit, and byte checks occur before decoded or projected storage is allocated.
Tests cover zero dimensions, ratio mismatch, values just above each limit,
`uint32` dimension products that would overflow without promotion, and a face
override of 4097.

The analytical fixtures in
`Engine/Tests/Native/EngineTests/Data/EquirectangularPanorama` are the executable
color and orientation ground truth. `AnalyticalLDR.tga` uses paired longitude
bands so all four equatorial principal axes are exact under bilinear sampling,
constant pole rows, a wrapped seam pair, and deterministic 45-degree edge
mixtures. `AnalyticalHDR.hdr` uses the same layout with exactly representable
RGBE values below and above display range. Its accompanying README lists every
source pixel.

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
- Cube render resources opt into readback so the hardware-backed Vulkan
  integration test can compare every face and every mip with panorama-derived
  LDR and tone-mapped HDR platform data before checking rendered pixels. The
  same test samples both sides of the panorama longitude seam and a cube-face
  boundary, then covers component replacement and resource retirement while
  Vulkan Validation is active.

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
  reflected object pointer or concrete render-resource owner; it retains a
  counted stable `FRHITextureReferenceRef`.
- The abstract `DTexture` base owns the stable `FTextureReference`, shared
  revision/completion state, and current generic texture resource for every
  texture leaf. `DTextureCube` owns cube source, platform data, and the hook
  that snapshots validated data into an `FTextureCubeResource`. Common rebuild
  publication retargets the stable RHI reference, so active sky snapshots and
  preview proxies observe a new cube without reacquiring the asset or concrete
  resource.
- Scene removal, proxy closure, thumbnail cancellation, and accepted queued
  draws drop their counted stable references independently. A copied stable
  reference may keep the targeted GPU texture alive until RHI deferred
  deletion, but never keeps the concrete C++ resource alive.
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

The user-facing import, inspection, reimport, and Sky Box workflow is documented
in [Texture Cube Workflow](../../Editor/Guides/TextureCubeWorkflow.md).

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
