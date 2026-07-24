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
- Texture2D uploads use array slice 0. Cube uploads name their face explicitly
  and translate it to the corresponding array slice.
- Vulkan cube images use the cube-compatible image flag and one cube image view
  spanning base layer 0 through layer 5.

## Related Code

- `Engine/Source/Runtime/Core/Public/Math/Vector.h`
- `Engine/Source/Runtime/RHI/Public/RHIDefinitions.h`
- `Engine/Source/Runtime/RHI/Public/RHIResources.h`
- `Engine/Source/Runtime/VulkanRHI/Private/VulkanTexture.cpp`
