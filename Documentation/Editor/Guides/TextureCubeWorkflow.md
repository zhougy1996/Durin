# Texture Cube Workflow

Summary: Import, inspect, and use six-face or equirectangular TextureCube assets in the editor.

## Import A Texture Cube

The Content Browser's `Import > From File...` action currently creates only
Texture2D assets. The TextureCube first-import dialog has been removed.
Existing TextureCube assets remain usable and reimportable. The underlying
TextureCube factory still supports these source layouts:

- `Six Faces` accepts one ordinary source file for each face in
  `+X/-X/+Y/-Y/+Z/-Z` order.
- `Equirectangular Panorama` accepts one PNG, JPEG, BMP, TGA, or Radiance HDR
  source.

Sources may be project-relative or external absolute files; reimport reads them
in place and does not copy them into Content.

For a panorama, face dimension zero selects the `Width / 4` default. Explicit
dimensions must be in `[1, 4096]`. Exposure is available only for HDR input and
remains an offline HDR-to-LDR build setting.

The factory validates decode, projection, color conversion, mip generation, and
platform format before publication.

## Inspect And Reimport

Content Browser cards label the asset as `Texture Cube`. Selection details show
the active layout, authoritative source, original panorama dimensions when
applicable, face override, exposure, output dimensions, mip count, and format.
Reimport reads the persisted sources without rewriting them. Moving,
duplicating, or deleting the asset never mutates those source files.

## Use A Sky Box

For a custom sky, use an ordinary Static Mesh Component. In the Material Editor,
choose the Surface domain, Opaque blend and Unlit shading, enable **Is Sky**,
and connect the sky color to Emissive. Assign the material or an instance to
the mesh slot. For a dome viewed from inside, use inward-facing geometry or
enable **Two Sided**.

For a cubemap, drop the TextureCube into the level. The editor creates a Sky
Box Actor with the built-in sphere and cube material. Adjust its mesh, scale,
tint and intensity in Details. Dropping another cube updates the existing Sky
Box. Placement requires at most one visible Sky Box Actor.

To supply your own sky material on that component, assign it to **Material
Override**. It supplies its own parameters. Clearing it restores the stored
cubemap, tint and intensity when slot zero has no other material override.
A Sky Box needs an assigned mesh to appear in the level.
Sky Light capture follows the Sky Light source setting.

Face orientation, panorama projection, build, and rendering contracts are
defined by [Cube Textures](../../Runtime/Assets/Families/CubeTextures.md).
