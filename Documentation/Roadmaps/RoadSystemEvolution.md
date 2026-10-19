# Road System Evolution Roadmap

Summary: Evolve RoadWeaver's authored road graph into editable roads on planar and spherical surfaces, with junctions, terrain integration, and bounded incremental construction.

Last reviewed: 2026-10-18

Status: Active
Completed:

## Current Status

P0 is complete through
[Road Surface Foundation](../Plans/Archive/2026-10/RoadSurfaceFoundation.md), including final-curve
plane/sphere previews, validated graph publication, persistence, full editor
transaction replay, GPU viewport picking and material display. The plan records
the 2026-10-18 Apple M4 Metal/MoltenVK closeout and earlier build evidence.
The geometry contract is owned by [Road Geometry](../Runtime/World/RoadGeometry.md).

P1 is the next milestone; no viewport road-authoring or cross-section work is
claimed complete by P0. Later child plans remain proposed names and become
active only after their entry gates pass.

## Outcome

An authored road network can be placed and edited on ordinary or spherical
surfaces, generate continuous road and junction geometry, and publish matching
query/collision data. Terrain integration preserves the source terrain. Local
edits invalidate bounded derived work and obsolete builds cannot overwrite
newer authoring.

## Program Decisions

- Keep road semantics, alignment policy, surface adaptation, and builders in
  `RoadWeaver`; road authoring UI belongs to `RoadWeaverEditor`. Engine retains
  generic spline, mesh, scene, and collision mechanisms. Do not add lane,
  terrain, or road-width fields to `FSplinePoint`.
- `DRoadNet` is the authored authority. Scene instances reference it; generated
  curves, meshes, lookup indexes, and collision are derived. Persistent edits
  must pass one validated mutation boundary, including reflection and Undo/Redo.
- Preserve stable IDs and the node/road/lane/junction distinction. Geometric
  intersections do not imply graph connectivity. Curve control points are not
  network nodes.
- Network coordinates use meters and existing double-precision `FVector3`.
  Initial road instances allow rigid placement only. Define reference-surface
  distance separately from final three-dimensional road arc length; lane
  stationing uses the latter. Geometry edits must explicitly reconcile stations.
- Surface position, reference up, terrain normal, and road normal are distinct.
  Surface providers supply local spatial context; road policy owns grade,
  banking, and continuous frames. A Cartesian spline does not inherently stay
  on a sphere. Long spherical paths require bounded fitting/subdivision.
- Use analytic plane and sphere providers before depending on a production
  terrain implementation. Terrain sampling and terrain modification are separate
  capabilities. Missing providers produce diagnostics, not silent flat fallback.
- Reuse SplineMesh for the first visual closure and suitable repeated assets.
  General road cross sections and junctions may use dedicated mesh generation;
  their output must use supported engine resource lifetimes.
- Keep schema migration explicit and preserve existing IDs. Never silently
  repair ambiguous topology or overwrite incompatible assets.

## Milestones

- [x] P0: **Road Surface Foundation** — harden authored contracts and produce a
  scene preview from `DRoadNet` using analytic plane and sphere providers.
  Entry: existing RoadWeaver and Spline source available. Exit: all gates in the
  linked active plan pass, including persistence, spherical deviation/frame
  checks, invalid-input rejection, and preview lifecycle validation.
- [ ] P1: **Road Authoring and Cross Sections** — viewport node/curve editing,
  continuous width profiles, lane section transitions, shoulders, banking, and
  road-owned cross-section geometry. Depends on P0. Exit: a user creates,
  reshapes, splits, and saves a road; Undo/Redo restores topology and geometry;
  widening and narrowing have continuous boundaries and correct lane identity.
- [ ] P2: **Road Junction Construction** — explicit approach ports, trimming,
  shared boundary sections, T/cross junction surfaces, and directed lane
  connectors. Depends on P1. Exit: supported junction fixtures have matching
  approach seams, valid lane movements, and consistent surface queries; an
  overpass crossing remains disconnected. Unsupported layouts are diagnosed.
- [ ] P3: **Road Terrain Integration** — adapt to an available production
  terrain query/modifier API, adding reversible grading and transition bands.
  Depends on P2 and a verified terrain owner/API; this is an explicit entry
  gate, not an assumption that such an API exists today. Exit: creating,
  moving, and removing a road restores the terrain baseline correctly;
  overlapping junction modifiers are deterministic and terrain revision
  changes invalidate affected roads. The child plan owns the backend-specific
  sampling and publication contract.
- [ ] P4: **Road Incremental Runtime** — spatial/longitudinal chunks, stable
  seams, asynchronous construction, visual/collision publication, and LOD.
  Depends on P2; terrain-aware qualification also depends on P3. Entry: record
  representative network sizes and target frame, edit-latency, and memory
  budgets. Exit: measured workloads meet those recorded budgets; local edits
  rebuild only their dependency region; cancellation, unloading, and stale
  results preserve the latest valid state. Include a large-coordinate spherical
  case using chunk-local render geometry.
- [ ] P5 (conditional): **Road Structures and Advanced Junctions** — bridges,
  tunnels, roundabouts, or multi-level interchanges, selected by product need
  after P2. Bridge spans require independent elevation and abutment policy;
  tunnel portals require an available terrain-hole/volume capability. Exit:
  each selected feature has a bounded child plan and validated surface/topology
  behavior, or is explicitly deferred with rationale.

## Scope and Completion

P0-P4 are required. P3 remains pending while its terrain entry gate is unmet;
analytic previews do not count as production terrain integration. P4 can begin
with P2 fixtures, but cannot claim terrain qualification before P3 passes.
P5 is conditional and must be completed or explicitly dispositioned before
closing this roadmap. Traffic simulation, route planning, civil-design format
import/export, and automatic city generation are outside this program.

Each child plan records its own checks and evidence. Before completion, move
implemented contracts to the owning Runtime/Editor documentation and retain
links to completed child plans. Follow the repository
[build workflow](../Agents/BuildAndRun.md) and
[test workflow](../Agents/Testing.md) when implementation begins.

## Foundations

- [Spline system](../Runtime/World/SplineSystem.md)
- [Road network types](../../RoadWeaver/Source/Runtime/RoadWeaver/Public/RoadNet/RoadNetTypes.h)
- [Road network asset](../../RoadWeaver/Source/Runtime/RoadWeaver/Public/RoadNet/RoadNet.h)
- [SplineMesh actor](../../Engine/Source/Runtime/Engine/Private/Actors/SplineMeshActor.cpp)
