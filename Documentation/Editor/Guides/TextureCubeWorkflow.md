# Texture Cube Workflow

Summary: Import, inspect, and use six-face or equirectangular TextureCube assets in the editor.

## Import A Texture Cube

Open the Content Browser `Import` menu, choose `Texture...`, and select
`Texture Cube` as the asset type. Then select one source layout:

- `Six Faces` accepts one ordinary source file for each face in
  `+X/-X/+Y/-Y/+Z/-Z` order.
- `Equirectangular Panorama` accepts one PNG, JPEG, BMP, TGA, or Radiance HDR
  source.

Switching layouts retains the current inputs for both modes. Sources may be
project-relative or external absolute files; the editor reads them in place
and does not copy them into Content.

For a panorama, face dimension zero selects the `Width / 4` default. Explicit
dimensions must be in `[1, 4096]`. Exposure is available only for HDR input and
remains an offline HDR-to-LDR build setting.

The dialog validates decode, projection, color conversion, mip generation, and
platform format before changing files and repeats validation immediately before
publication. The preview reports source dimensions and range, output face size,
mip count, projection convention, and the resulting LDR format.

## Inspect And Reimport

Content Browser cards label the asset as `Texture Cube`. Selection details show
the active layout, authoritative source, original panorama dimensions when
applicable, face override, exposure, output dimensions, mip count, and format.
Reimport reads the persisted sources without rewriting them. Moving,
duplicating, or deleting the asset never mutates those source files.

## Use A Sky Box

Create a `Sky Box Actor` and assign a compatible TextureCube to its component.
Tint, nonnegative intensity, and rotation use ordinary reflected property
editing. Preview edits update the scene; committed edits dirty the level
package. Dropping another TextureCube replaces the existing Sky Box texture.
A level with multiple visible Sky Box actors is invalid and must be reduced to
one before placement can continue.

Face orientation, panorama projection, build, and rendering contracts are
defined by [Cube Textures](../../Runtime/Rendering/CubeTextures.md).
