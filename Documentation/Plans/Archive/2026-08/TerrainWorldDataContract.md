# Terrain World Data Contract Plan

Summary: Freeze the product targets, canonical coordinates, tile products, ownership, budgets, and legacy Terrain removal boundary for the long-term Terrain World system.

Last reviewed: 2026-08-25

Status: Archived
Completed: 2026-08-25

## Current Status

T0 is complete. The authoritative [Terrain World Data
contract](../Runtime/Terrain/TerrainWorldData.md) freezes four numeric profiles,
signed global coordinates, 0.25 m height quanta, 256-cell tiles, exact repeated
borders, five typed products, 8×8-tile region packages, compatibility rejection,
budgets, outcomes, and the World Partition-neutral interest boundary.

The selected profiles cover 4.096 km finite qualification, 32.768 km continuous
traversal, 131.072 km teleport recovery, and a 524.288 km large-open-world
storage case. They range from 256 to 262,144 tiles and from 5 to 4,097 installed
packages. Peak build memory is capped at 4 GiB and 768 MiB per task; the
contract records independent resident and latency budgets for every profile.

Legacy cleanup used a one-time `AssetReferenceStoreTests` qualification against
the real Sandbox. It registered `/Engine` and `/Game`, registered the authored
`DImportRecord` class needed by the corpus, ran catalog/reference
`FullValidation`, registered the project-default-Level external store, and
prepared the three-asset deletion closure. Preparation returned zero blockers,
zero warnings, three exact package entries, no aliases, no external persistent
or loaded references, and no conflicting companions. The committed transaction
removed:

- `Sandbox/Content/Levels/LEV_TestTerrain.dasset`;
- `Sandbox/Content/Terrains/TestTerrain.dasset`;
- `Sandbox/Content/Terrains/TestTerrain_1025.dasset`;
- `Sandbox/Content/Sources/TerrainHeightmaps/Erosion_Out.raw`;
- `Sandbox/Content/Sources/TerrainHeightmaps/Erosion1_Out.raw`.

`Sandbox/Configs/Project.yaml` still selects `/Game/Levels/GrayboxStage15`, and
the post-transaction catalog lookup confirmed that Level remained present while
all three deleted packages were absent. A post-cleanup `DurinAssetTool`
canonical-resave dry plan selected that Level as `1 ready, 0 blocked`, confirming
package compatibility/load preparation. The one-time destructive test code was
then removed; only the content deletion remains.

Current Terrain disposition is explicit: exact height/extrema, LOD/stitching,
collision separation, camera-relative rendering, build/Cook, and lifecycle
tests remain T2 characterization oracles. `AssetCore`, `AssetBuildCore`, task,
package/Cook, collision, render-resource, and asset-mutation capabilities are
independent reusable infrastructure. `DTerrainHeightmap`, component/actor/scene
proxy, Terrain renderer/topology/vertex factory, import/editor workflow, legacy
build keys/codecs/shaders, and their user documents are T2 replacement targets.
None is a compatibility dependency of the new family.

## Goal

Produce an implementation-ready contract for the first new Terrain World
assets and tile build products. T1 must be able to implement source
composition, derived data, Cook, and source-free tile loading without reopening
coordinate, seam, ownership, packaging, compatibility, or budget decisions.

Preserve an integration seam for a future World Partition system: Terrain
streaming consumes spatial interest and priority requests through a capability
boundary, while Terrain tile dimensions, packages, and typed residency remain
independent from future Actor/Level cell dimensions.

## Scope

- Inventory and safely remove the isolated legacy Sandbox Terrain content set.
- Freeze representative finite, traversal, teleport, and large-open-world
  profiles with numeric storage, memory, latency, and precision goals.
- Define Terrain World identity, signed global sample coordinates, world-space
  conversion, height datum and quantization, valid extents, and overflow rules.
- Select uniform tile cell dimensions and exact shared-edge/shared-corner
  ownership, encoding, validation, and neighbor dependency rules.
- Define authored intent, normalized build inputs, immutable tile products,
  manifests, derived-data identity, cooked bulk, and runtime metadata.
- Define logical layer identity and coverage ownership without implementing the
  programmable Terrain surface.
- Separate metadata, render-height, surface, collision, and query product
  identities so T4 can stream them independently.
- Define the spatial-interest adapter through which future World Partition,
  current camera/player policy, editor focus, physics, or explicit preload can
  request Terrain regions without owning Terrain tile layout.
- Freeze versioning, compatibility rejection, failure vocabulary,
  observability, and resource ceilings required by T1 and T2.
- Publish lasting selected contracts to the appropriate Runtime domains.

## Non-Goals

- Implementing new Terrain Engine objects, renderer scene ownership, collision
  resources, editor placement, or runtime streaming.
- Removing current Terrain source code, tests, contracts, or Engine fixtures
  before the T2 replacement qualifies required behavior.
- Loading, converting, or preserving existing `DTerrainHeightmap`,
  `DTerrainComponent`, `ATerrainActor`, Terrain Level state, or cooked payloads.
- Designing generic World Partition Actor cells, Data Layers, HLOD, cross-cell
  Actor references, editor collaboration, or Level package streaming.
- Requiring Terrain tiles to equal future World Partition cells.
- Implementing material compilation, Terrain layer evaluation, virtual
  textures, GPU-driven rendering, ecosystem consumers, or sculpting.
- Treating imported image coordinates, local tile ranges, render resources,
  collision resources, or virtual-texture pages as canonical identity.

## Design Decisions and Invariants

### Compatibility and cleanup

- The new system uses new class, schema, payload, and builder identities. Old
  packages fail explicit compatibility checks; there is no implicit converter,
  dual serializer, redirector, or hidden legacy load path.
- Stage 0 removes only the isolated Sandbox content proven by the complete
  reference preparation result. Current Terrain code and native fixtures stay
  as characterization oracles until T2 selects their removal.
- Deletion uses the asset mutation contract: alias closure, unified incoming
  references, external stores, residency, and exact companion files must all
  be resolved before removal.
- `GrayboxStage15` remains the default Sandbox Level and is outside cleanup.

### Canonical coordinates and seams

- A Terrain World owns a signed integer global sample lattice. Tile and patch
  coordinates are derived views and never replace global sample identity.
- Stage 1 evaluates at least signed 64-bit global X/Y coordinates and a signed
  integer canonical height domain. Narrower persisted encodings must round-trip
  the selected envelope and decode shared borders identically.
- World-level spacing, height datum, and height quantization convert samples to
  double-precision world positions. Tiles cannot derive scale or offset from
  local observed min/max.
- Conversion uses checked integer arithmetic. Invalid extent, overflow,
  non-finite spacing, non-positive quantization, and non-finite origins fail
  before publication.
- Camera-relative rendering and future origin rebasing alter presentation only;
  they never mutate sample, tile, layer, or product identity.
- The world selects one uniform power-of-two cell dimension. Stage 0 compares
  128-, 256-, and 512-cell candidates before selecting it.
- A tile key is a signed grid coordinate plus Terrain World identity; paths,
  hashes, addresses, and traversal order are not tile identity.
- Neighbors expose complete shared edges/corners from one canonical global
  value. Border disagreement is rejected rather than hidden by skirts,
  averaging, resampling, or overlapping geometry.
- Builders explicitly declare whether they consume interiors, shared edges, or
  a neighbor halo for bounds, normals, LOD error, collision, and coverage.

### Products, packages, and layers

- A compact authored Terrain World definition owns intent, stable layer IDs,
  source/composition references, coordinates, tile scheme, and build policy;
  it does not embed all runtime samples or platform resources.
- Normalized tile inputs are immutable, value-owned, source-format independent,
  and complete. Workers do not inspect reflected objects.
- Metadata, canonical height, surface coverage/cache inputs, collision, and
  query values have versioned identities. Stage 2 freezes atomic generation
  and independent build/residency relations.
- Authored state remains in `.dasset`, rebuildable outputs in DDC, and
  deployable bytes in cooked bulk. Physical cache and workstation paths never
  enter identity.
- Package granularity is selected from measured tile count, dependency count,
  header/open overhead, partial installation, and mutation behavior; it is not
  assumed to be one package per tile or one package per world.
- Layer IDs and coverage remain independent from Terrain surface programs and
  visual resources. T3 may change evaluation without invalidating authored
  logical coverage identity.

### World Partition integration seam

- Terrain accepts spatial interest expressed as world bounds or a stable
  Terrain region plus consumer class, desired product class, priority,
  safety/deadline policy, and request lifetime.
- A standalone distance/viewport producer may drive T4 before generic World
  Partition exists. Future World Partition becomes another producer; it does
  not call package internals or dictate Terrain tile size.
- Terrain owns interest-to-tile mapping, coalescing, typed residency, fallback,
  and eviction. The upstream producer owns only interest lifetime and priority.
- World cells may cover multiple Terrain tiles and Terrain tiles may overlap
  multiple interest sources; a one-to-one mapping is forbidden.
- Physics safety, render visibility, query, editor focus, and explicit preload
  remain distinct interest classes.

### Failure, limits, and observability

- Every collection has a numeric count, byte, extent, or depth ceiling selected
  from the frozen profiles.
- Outcomes distinguish invalid definition, unsupported/legacy schema, missing
  dependency, border mismatch, overflow, budget rejection, cancellation,
  supersession, corruption, and publication failure.
- Failure retains the previous complete immutable generation or reports a
  bounded unavailable state; partial tile products never publish.
- Identity inputs, dimensions/extents, bytes by product, dependency/neighbor
  counts, build timings/origin/status, and retained/peak bytes are observable
  without inspecting physical cache layout.

## Current Foundations and Gaps

| Area | Foundation | Gap closed here |
| --- | --- | --- |
| Asset storage | Versioned packages, soft references, cooked bulk, compatibility inspection | No Terrain World schema, tile product graph, package decision, or legacy rejection boundary |
| Build | Immutable build functions, cache validation, family-owned cancellation | No normalized tile input, neighbor identity, output values, or ceilings |
| Existing Terrain | Exact height, extrema, LOD/stitching, collision separation, camera-relative evidence | Disposable finite ownership; no global lattice or multi-tile contract |
| Materials | Stable identities and active compiler roadmap | No Terrain layer/coverage identity; execution belongs to T3 |
| World/Level | Camera/player state can produce initial interest | No World Partition; Terrain needs a neutral seam, not an internal substitute |
| Asset mutation | Catalog/reference projections and transactional deletion | Candidate content still requires authoritative removal proof |

## Implementation Stages

### Stage 0: Freeze product profiles and legacy removal closure

- [x] Record finite qualification, continuous traversal, teleport/recovery, and
  large-open-world profiles with numeric dimensions, horizontal resolution,
  vertical range/precision, layer counts, expected authored/cooked bytes, and
  target platform.
- [x] Freeze peak build memory, resident bytes by product class, initial
  activation latency, traversal latency, teleport recovery, and expected
  storage/package counts for each profile.
- [x] Compare 128-, 256-, and 512-cell tiles using border overhead,
  package/request count, rebuild amplification, render patches, and collision
  working set; select one default and versioning policy.
- [x] Prepare deletion through the authoritative reference projection for
  `LEV_TestTerrain`, `TestTerrain_1025`, and `TestTerrain`, including aliases,
  hard/soft references, external stores, residency, sources, and exact files.
- [x] Confirm `GrayboxStage15` remains loadable/default, then remove the five
  isolated files or record a proven incoming reference that narrows the set.
- [x] Inventory current Terrain code/tests/docs and classify each as T2 oracle,
  future replacement target, or independent reusable infrastructure.
- [x] Record selected profiles, tile comparison, deleted set, reference
  evidence, and retained exceptions in `Current Status`.

#### Acceptance Gate

- Four numeric profiles and budgets reject incompatible coordinate, tile,
  package, or payload proposals.
- One tile dimension is selected from recorded measurements.
- Cleanup leaves `GrayboxStage15` default/loadable with no dangling persistent
  reference or unowned companion.
- Existing implementation has an explicit disposition without compatibility.

### Stage 1: Freeze global coordinates, heights, tiles, and seams

- [x] Specify types and checked conversions for world ID, global sample, tile
  key, in-tile coordinate, origin, spacing, datum, integer height, and
  quantization.
- [x] Freeze valid ranges, invalid states, comparison/hash, byte order,
  versions, and overflow/non-finite rejection.
- [x] Specify signed tile division/modulo for negative coordinates, borders,
  and the maximum world boundary.
- [x] Define interiors, repeated edges/corners, optional halo, row/axis order,
  winding, and half-open/inclusive region conventions.
- [x] Define neighbor keys and edge/halo consumption for bounds, normals, LOD,
  collision, coverage, and incremental rebuild.
- [x] Create asymmetric golden fixtures covering negative tiles/heights,
  extrema, all edge/corner orientations, non-square extents, and overflow.
- [x] Publish the canonical coordinate/seam contract in Runtime Terrain docs.

#### Acceptance Gate

- All profiles fit chosen integer/double domains with precision and margin.
- Independent neighbors decode bit-identical shared samples and agree on world
  positions, normals, bounds, and cell identities.
- Negative and extreme mappings are unambiguous; malformed input fails before
  publication.
- The Runtime contract is authoritative for T1 and T2.

### Stage 2: Freeze authored intent and tile product graph

- [x] Define the authored World schema: coordinates, extent, tile policy,
  layer library, ordered composition sources, build settings, and references.
- [x] Define normalized tile inputs with source content, composition policy,
  rectangle/halo, layers, neighbor evidence, builder identity, and cancellation.
- [x] Define metadata, height, coverage, collision, and query value schemas with
  independent magic, version, checksum, ceilings, and compatibility fields.
- [x] Decide atomic publication and independent build/load/residency relations.
- [x] Define deterministic build keys, cached/local validation, corruption,
  Cook selection, and runtime compatibility inspection.
- [x] Select package granularity and specify manifest lookup, partial
  installation, reachability, unload blocking, and product location.
- [x] Specify exact rejection of all legacy Terrain classes and schemas.

#### Acceptance Gate

- T1 can implement codecs, build definitions, keys, manifests, Cook, and
  source-free load without reopening a format decision.
- Every product has one owner, identity, ceiling, dependencies, checksum,
  compatibility rule, and failure terminal.
- Package costs meet Stage 0 profiles.
- Legacy payloads reject without partial decode or hidden dependency.

### Stage 3: Freeze runtime ownership and World Partition seam

- [x] Define value-only runtime handles for world, tile, product class,
  immutable generation, and regional interest without old Terrain objects.
- [x] Define interest fields for bounds/region, consumer, product/quality,
  priority, safety/deadline, generation, and cancellation lifetime.
- [x] Define a standalone camera/player/editor producer and the identical
  adapter contract for a future World Partition producer.
- [x] Specify interest-to-product mapping ownership, coalescing, ordering,
  cancellation, supersession, terminals, and telemetry.
- [x] Specify independent metadata/render/surface/collision/query residency
  vocabularies and minimum complete fallbacks.
- [x] Walk through unequal World/Terrain grids, overlapping sources, physics
  radius beyond render visibility, and teleport replacement.
- [x] Publish the integration boundary without creating generic World
  Partition APIs or a new roadmap.

#### Acceptance Gate

- T4 can use standalone or future World Partition interest without changing
  tile, product, package, or residency identity.
- Overlap, priority, cancellation, collision safety, and teleport are
  deterministic.
- No public type leaks old objects or assumes one-to-one cell/tile mapping.

### Stage 4: Lock budgets, failures, and T1 handoff

- [x] Consolidate extent, byte, dependency, neighbor, task, queue, resident,
  and peak-build ceilings for all schemas and profiles.
- [x] Freeze status/error and prior-generation/fallback behavior for validation,
  build, cache, Cook, load, publication, and interest requests.
- [x] Define diagnostics and conservation for bytes, products, dependencies,
  cache origins, timings, generations, requests, rejection, and cancellation.
- [x] Define structural, codec, malformed, determinism, integration, Cook,
  runtime, performance, memory, and shutdown fixtures required from T1.
- [x] Update the roadmap T0 state and T1 gate with selected contracts/budgets.
- [x] Create `Terrain Tile Build and Cook` only after prior gates pass.

#### Acceptance Gate

- Every collection/operation has a ceiling and one counted terminal outcome.
- T1 validation distinguishes fidelity, corruption, compatibility,
  cancellation, budget, and lifecycle failures.
- Lasting contracts, roadmap, and plan agree on ownership, formats, World
  Partition seam, budgets, and deferrals.
- T1 begins without reopening T0 decisions.

## Validation Matrix

| Area | Stage | Validation |
| --- | --- | --- |
| Cleanup | 0 | Incoming-reference/alias closure is empty outside deletion set; exact files removed; `GrayboxStage15` loads/defaults without missing reference |
| Profiles | 0 | Numeric dimensions, samples/tiles, bytes, working sets, latency, and platform limits reconcile |
| Tile choice | 0 | 128/256/512 comparison reports border, package/request, rebuild, patch, and collision costs |
| Coordinates | 1 | Golden/property checks cover signs, boundaries, extrema, overflow, quantization, and precision |
| Seams | 1 | Independent neighbors agree on shared values, position, bounds, normals, cells, coverage coordinates, and keys |
| Schemas | 2 | Round trip, deterministic bytes, version/magic/checksum, truncation, trailing/oversized/malformed data, cross-value mismatch, and legacy rejection |
| Build identity | 2 | Content/composition, tile/halo, neighbor, layer, builder, schema, and platform changes invalidate intended identities only |
| Package/Cook | 2 | Manifest, reachability, partial install, open counts, bulk, source/DDC-free load, corruption, unload, and dependencies meet profiles |
| Streaming seam | 3 | Standalone/simulated partition interest produces equivalent requests; overlap, unequal grids, radii, cancellation, and teleport conserve terminals |
| Budgets | 4 | One-past-limit rejection, bounded diagnostics, previous-state retention, and request/product/byte conservation through failure/shutdown |
| Documentation | 0-4 | Changed-document, plan, roadmap, link, and whitespace validation passes |

Native validation selection follows [Agent testing workflow](../../../Agents/Testing.md);
build/run/recovery follow [Agent build and run
workflow](../Agents/BuildAndRun.md). Later child plans own exact targets and
commands.

## Definition of Done

- All stage gates pass with evidence and completed checklists.
- The isolated five-file content set is removed without dangling asset, source,
  alias, external, or default-Level references.
- Coordinate, height, tile, seam, layer, product, package, compatibility,
  failure, diagnostic, and budget decisions are numeric and published.
- Future World Partition can supply interest without owning Terrain tiles or
  requiring equal grids; T4 can qualify first with a standalone producer.
- Old runtime code remains only as classified T2 evidence/replacement scope;
  no new contract requires its API.
- The roadmap records T0 completion and selected T1 entry evidence.
- `Terrain Tile Build and Cook` is created only after T0 passes.
- Documentation validates and the plan/status changes are committed.

## Deferred Follow-ups

- T1 implements source composition, codecs, DDC, Cook, manifests, and load.
- T2 replaces the finite runtime and selects old source/test/doc removal.
- Material System M5 unlocks T3 programmable Terrain surfaces.
- T4 implements typed multi-tile residency through the interest seam.
- A World Partition roadmap begins only when Actor/Level cells, Data Layers,
  HLOD, cross-cell references, or editor collaboration are selected.
- Surface caches, VT, GPU-driven work, consumers, and authoring stay conditional.

## Related Documentation

- [Terrain World Data](../../../Runtime/Terrain/TerrainWorldData.md)
- [Terrain Tile Build and Cook](TerrainTileBuildAndCook.md)
- [Terrain World System Roadmap](../../../Roadmaps/TerrainWorldSystem.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Material System Roadmap](../../../Roadmaps/MaterialSystem.md)
- [Current Terrain Heightmap Asset](../../../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Current Finite Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [Runtime Collision](../../../Runtime/Physics/Collision.md)
- [Level System](../../../Runtime/World/LevelSystem.md)
- [Task System](../../../Runtime/Core/TaskSystem.md)
- [Async Asset Operations](../../../Editor/Architecture/AsyncAssetOperations.md)

## Related Code

- `Sandbox/Configs/Project.yaml`
- `Sandbox/Content/Levels/LEV_TestTerrain.dasset`
- `Sandbox/Content/Terrains/TestTerrain.dasset`
- `Sandbox/Content/Terrains/TestTerrain_1025.dasset`
- `Sandbox/Content/Sources/TerrainHeightmaps/Erosion_Out.raw`
- `Sandbox/Content/Sources/TerrainHeightmaps/Erosion1_Out.raw`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Runtime/Engine/Public/Components/TerrainComponent.h`
- `Engine/Source/Runtime/Engine/Public/Actors/TerrainActor.h`
- `Engine/Source/Runtime/Engine/Public/Engine/TerrainSceneProxy.h`
- `Engine/Source/Runtime/Renderer/Private/Renderers/TerrainRenderer.cpp`
- `Engine/Source/Runtime/Renderer/Public/Terrain/TerrainLOD.h`
- `Engine/Source/Runtime/Renderer/Public/Terrain/TerrainTopology.h`
- `Engine/Source/Runtime/RenderCore/Public/Terrain/TerrainVertexFactory.h`
- `Engine/Source/Developer/GeometryBuild/Public/Terrain/TerrainHeightmapBuildOperations.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/Mutation.h`
- `Engine/Source/Runtime/AssetCore/Public/Asset/References.h`
