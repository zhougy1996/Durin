# Terrain Runtime Tile Reference Plan

Summary: Publish one finite Terrain World tile through Engine rendering, collision, picking, and query ownership

Last reviewed: 2026-08-26

Status: Active
Completed:

## Current Status

T1 now provides checked Terrain World values, five immutable products,
independent DDC builds, atomic generations, region Cook, and source/DDC-free
loading. This plan owns the first Engine consumer of that boundary. Existing
`DTerrainHeightmap`, `DTerrainComponent`, `ATerrainActor`, and their renderer
path remain unsupported oracles and are not inputs to the new tile.

## Goal

Load one complete cooked Terrain World generation into new runtime ownership
and prove render, collision, picking, and query coordinate parity through a
bounded deterministic reference path.

## Scope

- New immutable Engine handles and one finite tile owner for a complete
  Metadata/Height/Coverage/Collision/Query generation.
- Reference 32-cell patches, camera-relative positions, conservative bounds,
  deterministic LOD/topology, shared renderer passes, and exact seams.
- Collision/query publication independent of renderer resources.
- Cooked load, replacement, failure retention, device recovery, unload, and
  shutdown diagnostics.

## Non-Goals

- Multi-tile interest mapping, traversal, eviction, teleport, or World
  Partition integration; T4 owns them.
- Programmable Terrain surface evaluation; T3 owns the material domain.
- GPU-driven visibility, virtual textures, editor sculpting, or compatibility
  with old Terrain assets and payloads.

## Design Decisions and Invariants

- Runtime consumes only a checked `FTerrainTileGeneration` or cooked product
  handles; it never reads source, DDC, or authoring objects.
- One consumer handle observes one generation. Replacement publishes all
  required runtime values atomically and retains the prior safe generation on
  failure.
- Global sample identity, 0.25 m height quantum, +X/+Y convention, winding,
  repeated edges, and stable logical layers remain unchanged through render,
  collision, picking, and query.
- Collision/query lifetime is independent of Renderer initialization and
  device recovery. Render residency never authorizes physics traversal.
- The reference topology remains available after later scalable paths arrive.

## Current Foundations and Gaps

`TerrainBuild` and `Engine` now provide the complete source-free tile.
Renderer already provides indexed Terrain topology, shared material passes,
camera-relative anchors, and resource recovery for the old finite family;
Physics provides immutable heightfield geometry. Missing work is new Engine
ownership that consumes Terrain World values without retaining any legacy
object, component, scene proxy, key, or package path.

## Implementation Stages

### Stage 0: Add runtime values and checked generation admission

- [ ] Add world/tile/product/generation handles and one finite tile owner with
  exactly-once terminal diagnostics.
- [ ] Decode and admit a complete generation without source, DDC, reflected
  authoring objects, or old Terrain schema dependencies.
- [ ] Publish replacement atomically and retain the prior generation on
  missing, corrupt, incompatible, cancelled, or stale input.
- [ ] Add lifetime, unload blocking, and shutdown-conservation tests.

#### Acceptance Gate

- A complete cooked generation becomes an immutable Engine tile; invalid input
  cannot become visible or release the prior complete set.

### Stage 1: Implement deterministic reference geometry and rendering

- [ ] Build 8×8 32-cell patches from global cell rectangles without owning or
  renumbering border samples.
- [ ] Implement conservative bounds, geometric-error LOD, crack-free topology,
  and camera-relative positions from canonical double coordinates.
- [ ] Bind Height/Coverage through the shared forward, GBuffer, depth, and
  shadow material passes with an explicit default-layer fallback.
- [ ] Qualify asymmetric corners, negative coordinates/heights, exact winding,
  pass coverage, device recovery, and resource release.

#### Acceptance Gate

- The reference tile renders the canonical extent without seams or coordinate
  drift and recovers deterministically after resource invalidation.

### Stage 2: Publish collision, picking, and query parity

- [ ] Construct immutable collision from the checked Collision product without
  Renderer ownership or source fallback.
- [ ] Publish Height/normal/layer/min-max query values from the checked Query
  product and expose stable revision/generation identity.
- [ ] Route exact picking through the same canonical coordinates and prove
  render/collision/query cell and triangle parity.
- [ ] Exercise missing/corrupt product, failed replacement, unload, and shutdown
  while collision retains the prior safe generation or blocks entry.

#### Acceptance Gate

- Render, collision, picking, and query return the same global sample, height,
  normal, layer, winding, and generation across borders and failure recovery.

### Stage 3: Integrate lifecycle and qualify the finite reference

- [ ] Connect runtime ownership to Engine/Renderer/module startup, device
  recovery, World retirement, and shutdown ordering.
- [ ] Add counters for product/resident bytes, patches, draws, triangles,
  requests, generations, fallbacks, failures, and terminal conservation.
- [ ] Run the smallest registered contract, renderer, collision, Cook, reload,
  device-recovery, and application launch targets selected through DevTool.
- [ ] Publish lasting runtime/render/collision/query contracts and activate T3
  or T4 only when their entry evidence is satisfied.

#### Acceptance Gate

- One complete finite tile builds, cooks, loads without source/DDC, renders,
  collides, picks, queries, replaces, recovers, unloads, and shuts down within
  T0 memory and latency budgets without any old Terrain object.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Admission | Complete generation, incompatible/legacy/missing/corrupt/stale rejection, prior retention |
| Coordinates | Asymmetric signs, boundaries, winding, positions, bounds, seams, origin changes |
| Rendering | Reference topology, LOD, shared passes, fallback, device loss/recovery, release |
| Collision/query | Height/normal/layer/cell/triangle/generation parity without Renderer |
| Lifecycle | Cooked load, replacement, unload blocking, world retirement, shutdown conservation |
| Budgets | Resident/product bytes, patches/draws/triangles, latency, fallback and terminal counters |

## Definition of Done

- A source/DDC-free T1 generation is consumed only through new Engine runtime
  values and passes every stage gate.
- Render, collision, picking, and query agree on canonical identity and never
  mix generations or depend on legacy Terrain objects.
- Lasting Engine, rendering, physics, and query contracts plus the Terrain
  roadmap match the implemented lifecycle and measured budgets.

## Deferred Follow-ups

- T3 owns programmable Terrain surfaces.
- T4 owns multi-tile interest, residency, streaming, traversal, and teleport.
- T7 owns the general regional query service after T4 vocabulary stabilizes.

## Related Documentation

- [Terrain World Data](../Runtime/Terrain/TerrainWorldData.md)
- [Terrain World System Roadmap](../Roadmaps/TerrainWorldSystem.md)
- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [Runtime Collision](../Runtime/Physics/Collision.md)
- [Render Resource Lifecycle](../Runtime/Rendering/RenderResourceLifecycle.md)
- [Renderer Resource Recovery](../Runtime/Rendering/RendererResourceRecovery.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Terrain`
- `Engine/Source/Runtime/Engine/Public/Collision`
- `Engine/Source/Runtime/Renderer`
- `Engine/Source/Runtime/RenderCore`
- `Engine/Source/Runtime/Engine`
- `Engine/Source/Developer/TerrainBuild`
- `Engine/Tests/Native/EngineTests/Private/Terrain`
