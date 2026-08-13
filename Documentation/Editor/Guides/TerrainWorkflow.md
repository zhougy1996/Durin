# Terrain Workflow

Summary: Import, place, configure, inspect, reimport, and validate one finite Terrain in the Level Editor.

Last reviewed: 2026-08-14

## Import And Place

1. In Content Browser, choose Terrain Heightmap import and select either a
   non-interlaced 16-bit grayscale PNG or a headerless square `.raw` containing
   unsigned 16-bit little-endian samples. RAW dimensions are inferred from the
   exact byte count; rectangular files and manual dimension entry are not
   supported.
2. Confirm the destination. The grayscale preview's white L identifies the
   canonical top-left corner.
3. Drag the card into Scene Viewport. The editor creates one Terrain Actor at
   the drop ray, selects it, and records one undoable action.
4. Undo removes the complete placement; Redo recreates it. A read-only Level,
   stale heightmap, or name conflict fails without modifying the Level.

World Outliner can construct an empty Terrain Actor. Assign its heightmap in
the generated Terrain Component before expecting a rendered or pickable surface.

## Configure And Inspect

Edit Heightmap, Spacing X/Y, Height Scale/Offset, Material, Visibility,
collision settings, and transform through ordinary reflected controls. Spacing
must be positive and finite; other numeric values must be finite. Invalid
values do not alter the object or transaction history.

Read-only rows below the controls show component render and collision health.
Inspect the referenced Terrain Heightmap asset for its dimensions, revision,
retained bytes, and asset diagnostic. `Ready` means that consumer published a
complete generation; collision set to `No Collision` is reported as disabled.
Missing heightmap, invalid payload/properties, extent rejection, and collision-
build failures include a bounded diagnostic. Ready collision facts include
heightmap and component revisions, resource identity, cells, hierarchy nodes/
depth, and retained/peak bytes.

Clicking the surface selects the exact Terrain Actor/component at full
resolution. Picking does not require collision and does not follow visual LOD.

## Reimport, Save, And Runtime

Use the ordinary Reimport action. Changed content publishes one new revision to
loaded consumers and invalidates the thumbnail. Identical content changes
nothing. Missing/corrupt source or save failure retains the complete revision
and selection.

Save Level and heightmap packages before Cook. Cooked Game loading uses the
packaged canonical payload and requires neither the PNG/RAW source nor local
DDC. Renderer and collision consumers see the same canonical sample plane for
either authoring format.

## Supported Limits

- Qualified Terrain: at most 1025x1025 samples.
- Render patches: 64x64 cells.
- Heightmap thumbnails: fixed 256x256.
- Displayed Terrain diagnostic: at most 2,048 bytes.
- Streaming, sculpting, holes, layers, foliage, navmesh, water, and procedural
  terrain are outside this workflow.

## Related Architecture

- [Terrain Editing Architecture](../Architecture/TerrainEditing.md)
- [Terrain Heightmap Asset](../../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Runtime Collision](../../Runtime/Physics/Collision.md)
