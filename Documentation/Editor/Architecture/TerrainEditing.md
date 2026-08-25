# Terrain Editing Architecture

Summary: Define finite Terrain placement, exact picking, Details health, thumbnails, and revision-safe editor integration.

Modules: Engine, LevelEditor, DurinEd

Last reviewed: 2026-08-14

## Authority And Ownership

`DTerrainHeightmap` remains the only height authority. LevelEditor consumes an
immutable payload and revision for placement, thumbnails, and picking; it does
not decode source images or inspect Renderer and Physics storage. A Terrain
component owns spacing, signed height scale/offset, transform, material,
visibility, and collision policy. Renderer, collision, thumbnail, and picking
resources are derived snapshots and may report unavailable state independently.

LevelEditor owns `FTerrainLevelAuthoringService`, the Terrain Details
customization, and the canonical heightmap thumbnail provider. Engine owns the
minimal `FTerrainPickingSnapshot` boundary. DurinEd owns provider registration,
scheduling, cache identity, persistence, upload budgets, and cancellation.

## Transactional Placement

Viewport drag/drop captures a request before mutation. Planning validates the
Level package path/edit revision, read-only state, unique Actor name, heightmap
readiness/revision, finite transform, positive finite spacing, and finite
height scale/offset. The plan also freezes the Actor-hierarchy revision.

Execution rejects a replaced or edited Level, changed heightmap generation,
read-only transition, or new name conflict. Success creates, configures, and
positions the `ATerrainActor` through one transaction. Undo destroys it and
restores the prior package saved state; redo recreates the same name and values.
Failure leaves no Actor and adds no history entry.

Reflected component edits retain the generic property transaction path.
`DTerrainComponent::PreEditChangeProperty` rejects wrong object classes,
non-finite numeric values, and non-positive spacing before publication.

## Exact Surface Picking

Terrain remains an `ActorComponentSurface` candidate in the shared scene index.
Each provider query captures the current immutable payload, asset revision,
component interpretation, and local-to-world matrix. Hidden, unregistered,
malformed, over-extent, or singular components are misses.

The reference provider tests both full-resolution triangles of every cell using
the `(A,B,C)` and `(B,D,C)` diagonal shared by rendering and Physics. The
accelerated provider first intersects immutable 64-sample regional min/max
bounds, then tests full-resolution cells only in surviving regions. Neither
provider reads render LOD, collision enablement, BodyInstance, or Renderer
buffers, and neither materializes a triangle array.

Hits are double-sided and converted to world distance before the existing
stable resolver compares them with other surfaces. Rotation, positive
non-uniform scale, and mirroring are supported. Compare returns the reference
result on mismatch and increments shared and Terrain-specific parity counters.

Private diagnostics count applicable/invalid targets, bounds rejects, regional
node visits, visited cells, tested triangles, and parity mismatches. The
1025x1025 qualification ray must visit at most one percent of cells and its
accelerated median must be at most one quarter of the full-cell oracle median.

## Details And Diagnostics

Terrain Details leaves authored controls editable while hiding raw transient
status fields. Heightmap asset dimensions, revision, retained bytes, and asset
diagnostics belong to the heightmap asset rather than the component panel. The
component panel summarizes render status and collision status/revision/resource/
geometry facts; disabled collision is reported as disabled rather than as an
unavailable derived resource. Status text maps stable runtime enums. Every
displayed diagnostic is capped at 2,048 bytes. Details never makes a derived
resource authoritative or retains a status history.

## Canonical Heightmap Thumbnail

The exact-class provider generates deterministic 256x256 opaque RGBA from the
canonical row-major samples. It maps payload minimum to black and maximum to
white, preserves aspect ratio, uses a fixed dark letterbox, and draws a small
white L at canonical top-left. Flat payloads map to middle gray.

The provider reads neither encoded source nor Renderer. Cache identity contains
the package fingerprint, provider/schema, fixed output, and asset revision.
Generated pixels use the shared bounded DurinEd persistence/upload/cancellation
pipeline. Invalid or unavailable payloads safely retain the class icon.

## Reimport And Lifetime

Reimport remains the provider-neutral single-asset action. Existing heightmap
publication retires render/physics state before exchanging a changed revision
and rebuilds loaded consumers afterward. Identical content is a no-op. Failure
retains the prior package, payload, thumbnail identity, facts, selection,
proxy, and collision generation.

LevelEditor unregisters its thumbnail provider before releasing the workspace.
Picking tickets remain viewport-owned and are invalidated by Level replacement,
registration change, mode exit, or shutdown.

## Related Code

```text
Engine/Source/Editor/LevelEditor/Public/TerrainLevelAuthoring.h
Engine/Source/Editor/LevelEditor/Public/TerrainDetails.h
Engine/Source/Editor/LevelEditor/Public/TerrainHeightmapAssetThumbnail.h
Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingService.cpp
Engine/Source/Runtime/Engine/Public/Components/TerrainComponent.h
```
