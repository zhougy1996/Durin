# Aether Heightfield Collision Plan

Summary: Add immutable regular-grid heightfield collision to Aether and publish Terrain revisions through existing World query, Cook, lifecycle, and diagnostic contracts.

Last reviewed: 2026-08-12

Status: Active
Completed:

## Current Status

Planning is complete and Stage 0 is ready to execute. T0 supplies exact
top-left row-major `uint16` samples, immutable revision snapshots, exact 64x64
regional extrema, DDC restore, and source-free THPL Cook/load. T1 supplies the
reflected Terrain Actor/Component, the authoritative spacing/height conversion,
stable heightmap-revision notification, and a finite single-LOD render baseline.

AetherCore already owns immutable shared Primitive, Compound, ConvexHull, and
TriangleMesh geometry. Reference and Production Ray/Sweep/Overlap dispatch,
triangle feature tests, deterministic triangle-mesh BVH traversal, closest-hit
ordering, initial-penetration behavior, and bounded counters are qualified.
Aether owns scene broad phase and stable body handles. Engine already publishes
component geometry through `FBodyInstance` and the existing World APIs.

The missing work is a first-class `HeightField` geometry kind and builder,
regular-grid acceleration that does not retain an expanded triangle mesh,
HeightField dispatch for the complete query matrix, Terrain component
publication and revision replacement, cooked-runtime construction, and bounded
HeightField diagnostics. Stage 0 must freeze the supported collision extent,
resource layout, transform domain, query oracle, and persistent-payload choice
before production types or schemas are added.

## Goal

Make one finite `DTerrainHeightmap` revision collide through Durin's existing
World line trace, sphere/capsule/box sweep, and overlap APIs with the same sample
orientation, cell split, component-local height conversion, filtering, stable
hit ordering, and lifecycle behavior as the rendered Terrain surface.

The final collision resource is immutable, shared, independently bounded, and
regular-grid aware. Sparse Production queries visit conservative hierarchy
nodes and local cells rather than scanning every authored cell or retaining two
explicit triangles per cell. Cooked runtime operation uses the committed height
payload without source, DDC, Renderer, or editor dependencies.

## Scope

- Add `ECollisionGeometryKind::HeightField` and an AetherCore-owned immutable
  heightfield resource with dimensions, exact unsigned samples or an equivalent
  lossless owned sample representation, spacing, signed height scale, height
  offset, exact bounds, deterministic cell acceleration, identity, and retained
  byte facts.
- Preserve the T0/T1 coordinate rule: sample `(X,Y)` maps to local
  `(X * SpacingX, Y * SpacingY,
  HeightOffset + Sample / 65535 * HeightScale)` and source rows increase local Y.
- Use the same rendered cell diagonal and triangle membership:
  `(A,B,C)` and `(B,D,C)`, with exact right/bottom boundaries and stable
  Y-major cell ordinals.
- Support segment Ray, and Box/Sphere/Z-axis Capsule Sweep and Overlap through
  `CollisionGeometry`, `FPhysicsScene`, and `DWorld` without adding Terrain-only
  query APIs.
- Add a deterministic Reference path and a bounded triangle-mesh oracle for
  parity; add a Production path whose work is proportional to conservative
  hierarchy candidates and touched cells.
- Publish collision from `DTerrainComponent` through its inherited
  `FBodyInstance`, with reflected collision settings, stable revision tracking,
  transactional heightmap/property changes, shared resource identity, and
  ordinary World registration/filter/motion semantics.
- Qualify authored save/load, reimport, cooked-runtime load, two Worlds,
  component removal, level teardown, and Engine shutdown.
- Expose bounded resource/build/query/revision diagnostics and collision debug
  geometry using existing physics diagnostic and viewport-overlay ownership.

## Non-Goals

- Terrain render LOD, crack stitching, skirts, tessellation, GPU queries, or
  using render-selected LOD as collision geometry; T3 owns visual scalability.
- Sculpting, holes, painted layers, runtime deformation, partial dirty-region
  rebuild, or network replication.
- World partition, streaming collision tiles, residency, origin rebasing, or
  asynchronous world-scale activation.
- Simulation response, rigid-body terrain motion, contact manifolds, events,
  navmesh generation, character-movement policy changes, or a new physics
  backend. Terrain bodies remain query-compatible World bodies.
- A second height source decoder, color conversion, resampling, observed-range
  normalization, or source-file access from Aether/Engine consumers.
- Per-cell materials, face identifiers as a public gameplay ABI, caves,
  overhangs, thickness, or closed-volume terrain semantics.
- Replacing Aether scene broad phase, `FBodyInstance`, collision profiles,
  filters, closest-hit ordering, or existing primitive/hull/mesh algorithms.
- Keeping an expanded full-resolution triangle array or triangle-mesh BVH as
  the shipping HeightField representation. Such a mesh is test-oracle and
  bounded bring-up evidence only.

## Design Decisions and Invariants

### Ownership and module direction

- AetherCore owns the immutable `HeightField` geometry payload, validation,
  exact local bounds, cell hierarchy, Reference/Production algorithms, identity,
  retained-byte accounting, and serialization-neutral read-only accessors.
- Aether owns only scene-body publication, broad-phase candidate selection,
  stable handles, query-policy dispatch, and aggregate diagnostics. It never
  traverses height samples or HeightField nodes directly.
- Engine captures one complete `shared_ptr<const FTerrainHeightmapPayload>`
  revision and component interpretation, builds or retrieves one detached
  AetherCore resource, and publishes it through `DTerrainComponent`'s existing
  `FBodyInstance` path. AetherCore does not depend on Engine or `DObject`.
- Renderer buffers, render proxies, patch visibility, GPU height textures, and
  visual LOD are never collision inputs or lifetime owners.

### Coordinates, cells, transforms, and surface semantics

- Width and height count samples; collision cells are `(Width - 1) *
  (Height - 1)`. Cell `(X,Y)` has `A=(X,Y)`, `B=(X+1,Y)`, `C=(X,Y+1)`, and
  `D=(X+1,Y+1)`. Stable triangle ordinals are `2 * (Y * CellsX + X)` and the
  following ordinal.
- Spacing X/Y must be positive finite values. Height scale and offset may be
  finite and signed exactly as in T1. Negative height scale changes winding
  orientation but not sample identity; reported normals follow the qualified
  two-sided query convention frozen in Stage 0.
- Component-to-world transforms use the existing valid physics-transform
  domain. Stage 0 records the exact accepted scale/reflection contract and adds
  a named component collision status for rejected transforms; rendering may
  remain available when collision cannot publish.
- HeightField is a zero-thickness, double-sided query surface. It does not imply
  solid volume below the terrain. Boundary ownership and ties use stable cell,
  triangle, then existing body-handle order so traversal order cannot change a
  winner.
- A ray or moving query that begins in contact/penetration follows existing
  triangle-mesh initial-overlap, time-zero, normal, and penetration conventions.
  Stage 0 freezes golden boundary/tangency cases before new algorithms land.

### Immutable resource and acceleration

- Construction validates checked dimensions/counts, exact sample count,
  finite interpretation values, supported extent, allocation ceilings, and
  conservative finite bounds before publication. Failure returns no resource
  and a named build status; it cannot mutate a prior resource.
- The production resource retains one lossless regular sample plane plus a
  deterministic 2D hierarchy over cell rectangles. Nodes retain conservative
  local XYZ bounds or exact sample extrema plus checked cell ranges. They never
  retain expanded per-cell vertices or triangles.
- T0's 64x64 extrema hierarchy may seed the first node layer, but AetherCore
  owns its collision traversal layout and validates every copied fact. No
  Aether query holds a `FTerrainHeightmapPayload` pointer or calls Engine code.
- Reference visits stable Y-major cells and exact triangles without
  acceleration. Production traverses conservative nodes, visits stable
  candidate cells, and uses the same exact leaf kernels. Compare runs both,
  reports mismatch through existing physics diagnostics, and returns the
  established production/reference policy result.
- Traversal uses bounded preallocated or fixed scratch with explicit overflow
  handling. Qualified inputs have zero fallback, overflow, unsupported, or
  non-convergence; a structural Production failure uses the existing bounded
  Reference fallback rather than returning an unsafe miss.
- Geometry identity includes builder/schema version, dimensions, canonical
  sample identity, spacing, height scale, height offset, and acceleration
  policy. Components share only byte-equivalent collision interpretation.

### Query compatibility and ordering

- HeightField participates in the existing `CollisionGeometry::Raycast`,
  `Sweep`, and `Overlap` entry points and `ECollisionQueryAlgorithm` policies.
  No operation silently converts the complete HeightField to TriangleMesh.
- Segment bounds and swept/overlap bounds prune hierarchy nodes
  conservatively. Closest-hit pruning is strict-safe: an equal-time candidate
  remains eligible for stable ordinal tie resolution.
- Sphere, Capsule, and Box leaf work reuses or factors the qualified
  shape/triangle distance, SAT, contact, and conservative-advancement kernels.
  HeightField-specific code selects candidate cells; it does not redefine
  global hit fields or World ordering.
- Scene counters distinguish HeightField node, leaf-region, cell, and triangle
  feature work while preserving existing aggregate equations and saturating
  overflow behavior. Capturing/resetting scene diagnostics remains O(1).

### Engine publication and revision transaction

- Terrain collision is opt-in through inherited collision settings and defaults
  to `NoCollision`, matching other new primitive components. Enabling a profile
  does not publish until heightmap, properties, transform, and collision resource
  are all valid.
- `DTerrainComponent::BuildCollisionGeometry` returns one immutable resource and
  world transform through the existing `DPrimitiveComponent` path. Its collision
  revision changes only when geometry identity or collision-relevant
  interpretation changes; material and render-only state do not rebuild it.
- Heightmap reassignment, changed committed revision, spacing, height scale, or
  height offset constructs the complete candidate before replacing registered
  bodies in stable object-handle order. Identical reimport and semantic no-op
  edits retain geometry identity and body state.
- Failed asset reimport leaves the old committed height revision and body
  untouched. An explicit invalid component edit or missing assignment removes
  collision rather than retaining geometry that no longer matches authored
  properties. Recovery publishes exactly once after a complete valid state.
- Multiple components with identical payload revision and interpretation share
  one resource; different revisions or interpretations never alias. Resource
  lifetime is held by bodies and recorded queries independently of the asset,
  component, Renderer, or editor.

### Cook and persistent data decision

- Cooked runtime always begins from the exact validated THPL revision and never
  opens source or DDC. Collision publication must work before Renderer resource
  initialization and in a game target without editor modules.
- Stage 0 measures deterministic runtime HeightField build time, peak memory,
  retained bytes, package duplication, and startup cost at the representative
  and proposed maximum extents. It then records exactly one persistent policy:
  build from THPL at runtime, or add a separately versioned HeightField
  collision companion derived transactionally from the same canonical samples.
- If an independent companion is selected, its descriptor, ID, schema, target,
  bounds, offsets, checksums, ceilings, and corruption behavior are frozen
  before implementation. THPL plus required collision data publish atomically;
  a missing/incompatible required payload is a hard cooked load failure with no
  source/DDC fallback and no partial height/collision generation.
- If runtime build is selected, no duplicate cooked sample plane is added. The
  builder version and interpretation key are still explicit, deterministic,
  measured, and tested from source-free THPL load.

### Limits, failure, and diagnostics

- The 2..16384 T0 asset domain is not implicitly the T2 collision domain.
  Stage 0 freezes a representative fixture, proposed maximum, retained/peak
  bytes, build time, sparse/dense query work, and runtime startup budgets. Assets
  beyond the chosen collision ceiling remain valid assets and may still render.
- Build, extent, allocation, invalid-property, invalid-transform,
  missing-payload, persistent-decode, and publication failures are distinct
  bounded statuses. Diagnostics cap retained text and never expose cache paths,
  source paths, pointer identities, or container-order-dependent data.
- Debug capture is disabled by default and bounded when enabled. It samples
  hierarchy bounds and exact cells/triangles without materializing the complete
  surface or changing collision results.

## Current Foundations and Gaps

| Area | Existing foundation | T2 gap |
| --- | --- | --- |
| Height authority | Immutable exact `uint16` payload, revision, dimensions, top-left row-major samples, exact regional extrema, THPL Cook/load | No collision builder input/key, HeightField identity, or revision-to-body publication |
| Geometry | Immutable Primitive/Compound/Hull/TriangleMesh resources, bounds, identities, retained bytes, deterministic mesh BVH | No HeightField kind, regular-grid storage/accessors, build diagnostics, or cell hierarchy |
| Narrow phase | Qualified shape/triangle kernels, Reference/Production/Compare policy, stable hit records | No regular-grid Ray/Sweep/Overlap dispatch, cell tie policy, or heightfield parity matrix |
| Physics scene | Stable body handles, broad phase, filters, ignore, motion partitions, closest ordering, O(1) counters | No HeightField bounds publication or heightfield-specific work conservation |
| Engine component | Terrain properties/revision callback and inherited BodyInstance; StaticMesh geometry publication precedent | No Terrain collision status, resource cache, collision revision, or BuildCollisionGeometry override |
| Persistence | Exact THPL cooked companion and strict source-free load; independent DCOL precedent | Persistent HeightField policy, version, corruption gate, and runtime build/load evidence are not selected |
| Editor/debug | Reflected BodyInstance, collision overlay, bounded mesh debug snapshots | No HeightField status facts, sampled debug surface, or revision-coherence inspection |
| Qualification | Triangle-mesh queries and T1 1025x1025 render baseline | No frozen terrain query oracle, collision ceiling, sparse/dense work, build/startup, or sharing evidence |

## Implementation Stages

### Stage 0: Freeze semantics, limits, layout, and persistence

- [ ] Build asymmetric, flat, extreme, saddle, ridge, non-square, odd-edge,
  negative-height-scale, and transformed fixtures using the exact T0/T1 mapping.
  Generate a bounded explicit triangle-mesh oracle with the frozen cell split
  and stable ordinals; record Ray/Sweep/Overlap golden hits at interiors,
  diagonals, shared edges, corners, tangency, and initial penetration.
- [ ] Audit existing triangle feature kernels, query status/fallback behavior,
  closest-hit ties, physics-transform validation, BodyInstance publication,
  collision debug capture, THPL Cook/load, and heightmap revision recreation.
  Record every explicit HeightField dispatch and lifecycle case.
- [ ] Measure Reference full-cell scans and prototype hierarchy candidates at
  representative and proposed maximum dimensions. Freeze supported dimensions,
  node/leaf policy, traversal scratch/depth, build peak and retained bytes,
  sparse/dense node/cell/feature work, build time, and cooked startup budgets.
- [ ] Freeze resource identity bytes, build statuses, exact bounds rounding,
  signed-height and valid-transform domain, double-sided normal convention,
  equal-time/equal-penetration ordering, and counter equations.
- [ ] Compare source-free deterministic runtime construction from THPL against
  an independently versioned collision companion. Select one persistent policy
  using measured startup, peak-memory, duplication, corruption, and transaction
  evidence; if a companion wins, freeze its binary schema and descriptor rules.
- [ ] Add failing contract/characterization tests for geometry accessors,
  validation ceilings, golden queries, Reference/oracle parity, Production work,
  Engine publication, revision changes, Cook/runtime, sharing, and teardown.
- [ ] Record the Stage 0 handoff in this plan before production types, schemas,
  or component collision fields are implemented.

#### Acceptance Gate

- Every coordinate, surface, hit, transform, identity, failure, lifetime, and
  persistence decision has one golden fixture or measured budget.
- The selected maximum fits checked build/startup memory and time; sparse query
  targets require local node/cell work rather than total-cell work.
- Persistent policy and any binary schema are singular and explicit; no later
  stage must choose between incompatible storage or Cook models.

### Stage 1: Add immutable HeightField geometry and Reference queries

Dependencies: Stage 0 frozen resource, semantics, limits, and oracle.

- [ ] Add `ECollisionGeometryKind::HeightField`, construction/build APIs,
  read-only dimension/sample/interpretation/node accessors, exact local bounds,
  identity, retained-byte accounting, and named build diagnostics without
  changing `FCollisionGeometryRef` size.
- [ ] Build the deterministic regular-grid hierarchy and checked immutable
  payload. Reject invalid dimensions/counts, samples, settings, bounds, limits,
  depth, allocation, and cooked facts transactionally.
- [ ] Implement stable Y-major cell/triangle reconstruction on demand with the
  exact T1 diagonal, signed-height mapping, boundary handling, and no persistent
  expanded vertex/index/triangle arrays.
- [ ] Implement HeightField Reference Ray/Sweep/Overlap over stable cells using
  the qualified leaf kernels; integrate status, counters, hit normalization,
  initial penetration, and tie rules.
- [ ] Prove geometry bounds/identity/bytes, permutation-independent construction
  where applicable, flat/extreme/negative-scale validity, snapshot lifetime,
  and complete Reference parity against the triangle-mesh oracle.
- [ ] Run the smallest AetherCore/PhysicsScene targets and record the Stage 1
  handoff with frozen sizes, bytes, fixtures, and remaining Production gaps.

#### Acceptance Gate

- HeightField resources are immutable, deterministic, bounded, and retain no
  expanded mesh; failed builds publish nothing.
- Reference matches the oracle for every qualified Ray/Sweep/Overlap cell,
  boundary, transform, tangency, and penetration fixture with stable hits.
- Existing Primitive/Compound/Hull/TriangleMesh results, reference sizes, and
  retained accounting remain unchanged.

### Stage 2: Implement accelerated Production Ray, Sweep, and Overlap

Dependencies: Stage 1 exact resource/Reference behavior and Stage 0 work gates.

- [ ] Implement conservative segment, swept-shape, and overlap bounds against
  the regular-grid hierarchy with deterministic near-first traversal, bounded
  scratch, strict-safe closest pruning, and stable node/cell ordering.
- [ ] Implement Production line trace and exact local cell feature dispatch;
  cover vertical, grazing, coplanar, below/upward, boundary, mirrored-height,
  and zero-length segments without false negatives.
- [ ] Implement Sphere/Capsule/Box Sweep and Overlap candidate selection using
  the same qualified leaf kernels and conservative advancement/status policy as
  other feature geometry.
- [ ] Integrate Production/Reference/Compare, bounded fallback, mismatch facts,
  and HeightField node/leaf/cell/triangle counters through AetherCore and
  `FPhysicsScene` without changing World bool/closest-hit APIs.
- [ ] Qualify randomized/adversarial transforms, diagonal/shared-edge ties,
  initial penetration, tangency, filters, ignore sets, multiple bodies, and
  sequential unrelated queries with zero parity mismatch or false negatives.
- [ ] Measure sparse/dense work at representative and maximum extents; prove
  sparse Production visits bounded local candidates rather than all cells and
  that qualified cases have zero fallback, overflow, unsupported, or
  non-convergence.
- [ ] Record the Stage 2 handoff with counter equations, worst-case work,
  controlled timing, oracle parity, and any evidence-gated refinements.

#### Acceptance Gate

- Production and Reference/oracle return identical hit/miss, time,
  position, normal, penetration, and stable winner throughout the frozen matrix.
- Sparse queries satisfy Stage 0 node/cell/feature and timing budgets; dense
  work remains within explicit finite limits.
- Existing geometry kinds and World ordering regressions remain clean, with no
  new exceptional status in ordinary qualified queries.

### Stage 3: Publish Terrain collision and synchronize revisions

Dependencies: Stage 2 qualified HeightField geometry and T0/T1 revision hooks.

- [ ] Add reflected Terrain collision status/facts and the minimum component
  collision configuration needed beyond inherited BodyInstance policy. Keep
  render status independent from collision availability.
- [ ] Add a bounded Engine-owned HeightField resource cache keyed by exact
  payload revision identity and collision interpretation. Build detached
  candidates, share identical resources, never alias different generations,
  and prune/release without Renderer or editor ownership.
- [ ] Implement `DTerrainComponent::BuildCollisionGeometry`, world-transform
  validation, collision-state revision, property setters/edit hooks, and
  registration through existing PrimitiveComponent/BodyInstance/World paths.
- [ ] Extend the heightmap revision recreation transaction to remove affected
  bodies, publish one complete asset revision, then recreate render and physics
  state in stable object-handle order. Coalesce no-op reimport and related
  property edits without exposing mixed render/collision generations.
- [ ] Validate assignment, unassignment, spacing/height edits, changed/no-op/
  failed reimport, invalid/recovered properties, collision profile/filter/motion
  changes, two components sharing a resource, different revisions/settings,
  two Worlds, level unload, component destruction, and Engine shutdown.
- [ ] Prove DWorld line traces, shape sweeps, and overlaps hit Terrain through
  ordinary filters/ignore/closest ordering, and that rendered/collision sample
  positions agree on the asymmetric fixture.
- [ ] Record the Stage 3 handoff with component defaults, status transitions,
  revision equations, sharing/resource counts, and lifecycle evidence.

#### Acceptance Gate

- A registered Terrain component publishes exactly one correct World body only
  when collision and geometry are valid; filter/motion/handle behavior remains
  ordinary BodyInstance policy.
- Changed revisions replace render and collision coherently; no-op/failure
  retains identity; invalid authored state removes stale collision.
- Shared resources, bodies, cached generations, and teardown counts reconcile
  across multiple components and Worlds without stale object access.

### Stage 4: Complete persistent Cook/load and runtime independence

Dependencies: Stage 0 persistent selection and Stage 3 publication lifecycle.

- [ ] Implement the selected persistent policy. For runtime build, make the
  deterministic builder consume only validated THPL state. For a companion,
  implement deterministic encode/decode, strict descriptor/section/checksum/
  topology/bounds validation, and atomic contribution with THPL.
- [ ] Persist only required Terrain collision policy and version facts; strip
  source path, DDC identity, editor diagnostics, transient caches, body handles,
  and process-specific state from cooked packages.
- [ ] Validate identical recook bytes/identities, source/DDC removal, cooked-only
  asset/level load, game-target construction, collision before render init, and
  equal editor/PIE/standalone World query results.
- [ ] Cover missing, wrong-target, wrong-version, truncated, oversized,
  overlapping, checksum, dimension, node, bound, and manifest corruption
  appropriate to the selected policy. Required failure publishes no partial
  asset/resource/body and never falls back to source or DDC.
- [ ] Measure cooked payload duplication, runtime retained/peak bytes, build or
  decode time, first-body latency, and many-instance sharing at the frozen
  maximum; enforce Stage 0 budgets.
- [ ] Record the Stage 4 handoff with exact schemas/versions or runtime-builder
  contract, golden hashes, target facts, runtime evidence, and failure matrix.

#### Acceptance Gate

- Cooked runtime reproduces identical HeightField identity, bounds, query hits,
  and sharing without source, DDC, Renderer resources, or editor modules.
- Persistent corruption or required-data absence is detected before
  publication, with no partial height/collision state or fallback.
- Package, startup, peak-memory, retained-memory, and first-query costs meet the
  frozen finite budgets.

### Stage 5: Qualify diagnostics, debug presentation, and T2 handoff

Dependencies: Stages 1-4 complete end-to-end behavior.

- [ ] Add bounded read-only HeightField facts: status/diagnostic, asset and
  collision revision, resource identity, dimensions/cells/nodes/depth,
  retained/persistent/peak bytes, build/load result, sharing, and coherence.
- [ ] Extend existing collision debug snapshots and viewport overlay with
  bounded sampled HeightField node/cell/triangle detail. Preserve disabled cost,
  snapshot caps, and primitive/hull/mesh presentation.
- [ ] Reconcile unique HeightField resources/bytes and query
  node/leaf/cell/feature/fallback counters in O(1) scene capture/reset; qualify
  saturation and controlled structural fallback.
- [ ] Run the complete structural, randomized, Cook/runtime, editor/PIE/
  standalone, multiple-World, lifecycle, parity, sparse/dense performance,
  sharing, memory, and regression matrix. Use focused targets throughout and
  the root validation policy for any required aggregate run.
- [ ] Because Terrain collision/debug is user-visible, complete the required
  full `all` build and validation-enabled editor smoke from one Agent Build
  Profile; validate plan, roadmap, and repository documentation.
- [ ] Publish lasting HeightField geometry/query, Terrain collision component,
  revision, Cook/runtime, diagnostics, limits, and failure contracts under the
  owning Runtime documentation; update the Heightfield Terrain and Aether
  roadmaps with T2 evidence and precise T4 entry state.
- [ ] Record final revision, profile, test counts, fixtures, query/build/startup
  measurements, bytes, limits, executable, decisions, and deferred work before
  closing every passed checklist and completing this plan.

#### Acceptance Gate

- One clean handoff proves exact HeightField collision through World
  Ray/Sweep/Overlap, coherent revision replacement, bounded Production work,
  Cooked runtime independence, sharing, diagnostics, debug presentation,
  lifecycle safety, and no regressions to existing collision families.
- T2 lasting contracts are owned by Runtime documentation and T4 can consume
  stable collision status/query behavior without redefining physics semantics.

## Validation Matrix

| Scenario | Required behavior | Evidence owner |
| --- | --- | --- |
| Coordinate identity | Asymmetric corners/interiors, source Y direction, spacing, signed height and rendered/collision positions agree | HeightField builder and Terrain integration tests |
| Cell topology | Exact diagonal, Y-major ordinals, odd/non-square edges, shared edge/corner ties, and no out-of-extent triangles | AetherCore geometry tests |
| Resource validity | Dimensions/counts/settings/bounds/limits checked; immutable identity, hierarchy, retained/peak bytes, and failure transaction are deterministic | AetherCore builder tests |
| Ray | Interior, diagonal, boundary, vertical, grazing, coplanar, upward/below, zero-length, and closest ties match oracle | Reference/Production query tests |
| Sweeps | Sphere/Capsule/Box hits, misses, tangency, initial penetration, motion direction, and time/normal/penetration match oracle | AetherCore and PhysicsScene tests |
| Overlaps | Sphere/Capsule/Box surface contact and penetration match zero-thickness two-sided semantics without treating below-terrain space as solid | AetherCore and PhysicsScene tests |
| Production work | Sparse node/cell/feature work is local, dense work is bounded, scratch cannot escape, and ordinary cases have zero exceptional status | Characterization and controlled performance tests |
| World policy | Filters, ignore, closest ordering, stable handles, motion partition, Reference/Production/Compare, and multiple bodies remain compatible | PhysicsScene and World tests |
| Revision transaction | Assignment, property edits, changed/no-op/failed reimport, invalid/recovery, and removal publish complete matching generations exactly once | Engine Terrain lifecycle tests |
| Sharing and lifetime | Same revision/interpretation shares; different keys do not alias; two Worlds, removal, query retention, unload, and shutdown balance | Engine/Aether lifecycle tests |
| Cooked runtime | Selected persistent policy loads/builds exact collision without source/DDC/Renderer/editor and rejects corruption before publication | Terrain heightmap Cook/runtime process tests |
| Diagnostics/debug | Resource/work equations reconcile, O(1) capture/reset holds, strings and overlays are bounded, disabled debug has no retained surface expansion | Physics diagnostics and LevelEditor tests |
| Regression | Primitive/Compound/Hull/TriangleMesh sizes, queries, bytes, Cook, debug, Box/Sandbox behavior, and renderer Terrain remain unchanged | Focused and required aggregate targets |

## Definition of Done

- `FCollisionGeometryRef` represents one immutable regular-grid HeightField with
  exact dimensions, sample interpretation, conservative hierarchy, bounds,
  identity, bytes, and deterministic construction failure facts.
- HeightField Reference and Production implement the complete segment Ray and
  Box/Sphere/Capsule Sweep/Overlap matrix with zero qualified oracle mismatch or
  false negative and stable existing hit semantics.
- Production sparse work follows hierarchy candidates and touched cells rather
  than total terrain triangles, uses bounded scratch, and retains no expanded
  shipping triangle mesh.
- `DTerrainComponent` publishes through inherited BodyInstance/World policy,
  shares exact resources, tracks collision revisions, and replaces render and
  collision generations coherently across edits/reimport/failure/teardown.
- Cooked editor, PIE, standalone, and game runtime reproduce exact collision
  without source, DDC, Renderer, or editor dependencies and reject incomplete
  persistent state transactionally.
- Counters, inspection facts, debug snapshots, build/query/startup timings,
  retained/peak bytes, ceilings, sharing, and failure statuses are bounded and
  documented.
- Focused tests, required aggregate validation, full build/editor smoke, plan
  and roadmap validation, lasting Runtime docs, roadmap status, and committed
  handoff all pass.

## Deferred Follow-ups

- Terrain patch render LOD, adjacency resolution, crack control, and GPU/draw
  scalability (T3).
- Polished Terrain collision property UX, error presentation, picking workflow,
  undo/redo qualification, and final end-to-end terrain workflow (T4).
- Streaming collision tiles, world partition, origin rebasing, partial
  residency, and asynchronous build/publication behind measured finite limits.
- Writable height regions, dirty-cell acceleration rebuild, holes, per-cell
  materials, sculpting, procedural generation, runtime deformation, and network
  replication.
- Simulation contacts, rigid/dynamic Terrain, navmesh consumption, character
  policy changes, public face IDs, or backend-provider heightfield APIs until a
  concrete consumer freezes their compatibility requirements.

## Related Documentation

- [Heightfield Terrain Roadmap](../Roadmaps/HeightfieldTerrain.md)
- [Aether Physics Evolution Roadmap](../Roadmaps/AetherPhysicsEvolution.md)
- [Terrain Heightmap Asset](../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Terrain Rendering](../Runtime/Rendering/TerrainRendering.md)
- [Runtime Collision](../Runtime/Physics/Collision.md)
- [Native Tests](../Development/Build/NativeTests.md)
- [Build and Run](../Development/Build/BuildAndRun.md)

## Related Code

- `Engine/Source/Runtime/AetherCore/Public/Collision/CollisionGeometry.h`
- `Engine/Source/Runtime/AetherCore/Private/Collision/CollisionGeometry.cpp`
- `Engine/Source/Runtime/AetherCore/Public/Physics/PhysicsTypes.h`
- `Engine/Source/Runtime/Aether/Public/Physics/PhysicsScene.h`
- `Engine/Source/Runtime/Aether/Private/Physics/PhysicsScene.cpp`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmapDerivedData.h`
- `Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmap.cpp`
- `Engine/Source/Runtime/Engine/Private/Terrain/TerrainHeightmapDerivedData.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/TerrainComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/TerrainComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Components/PrimitiveComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/PrimitiveComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Physics/BodyInstance.h`
- `Engine/Source/Runtime/Engine/Public/Physics/BodySetup.h`
- `Engine/Source/Runtime/Engine/Private/Physics/BodySetup.cpp`
- `Engine/Source/Runtime/Engine/Public/Engine/World.h`
- `Engine/Source/Runtime/Engine/Private/Engine/World.cpp`
- `Engine/Tests/Native/EngineTests/Private/Physics/PhysicsSceneTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TerrainRenderPrimitiveTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TerrainHeightmapTests.cpp`
- `Engine/Tests/Native/EngineTests/Private/TerrainHeightmapCookTests.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
