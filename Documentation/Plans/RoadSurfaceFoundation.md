# Road Surface Foundation Plan

Summary: Validate RoadWeaver graph semantics and preview authoritative final road curves created on planes or spheres.

Last reviewed: 2026-09-09

Status: Active
Completed:

## Current Status

The previous raw-curve plus instance-projection design has been superseded by
persisted final Cartesian curves. This architecture correction includes explicit
plane/sphere creation and fitting, fixed planet ownership, asset station
validation, legacy asset/instance conversion, and transient preview regression
coverage. Checkpoint validation completed on Win64-Debug-DurinEditor: `DevTool.bat test
affected` passed the road, spline, asset and world targets. Its sole failure was
`FVolumetricCloudSceneVulkanTests.EnabledCloudTraversesOffscreenAndPresentRoutes`
at the compile-time budget assertion; the exact case passed an isolated rerun
without source changes. The final `DevTool.bat test RoadGraphContractTests`
passed after adding multi-section fitting coverage. `DevTool.bat build` completed
target `all`; changed-document validation passed.

The broader P0 foundation is still active: interactive picking, complete editor
transaction replay and visual material qualification are not implied by this
architecture checkpoint. No traffic, terrain or radius-change tooling is started.

## Goal

Place a `DRoadNet` asset in a scene and derive preview geometry and queries from
the same persisted final curve. Preserve topology and stable identity through
editing, save/load and legacy conversion. Create roads explicitly on analytic
planes or spheres without imposing a permanent surface constraint.

## Selected Decisions

The implemented authority, coordinate system, numerical tolerances, normalized
station remapping and versioned compatibility are owned by
[Road Geometry](../Runtime/World/RoadGeometry.md). That contract replaces the
previous instance Surface descriptor decisions in this plan. The shared spline
foundation remains [Spline System](../Runtime/World/SplineSystem.md).

## Implementation Stages

### Stage 0: Freeze spatial and compatibility contracts

- [x] Select final Cartesian curves as the single persisted authority and retain
  the existing graph and spline types.
- [x] Determine fixed planet identity/radius/center and rigid network coordinates;
  support both plane and sphere creation without permanent projection.
- [x] Specify whole-cubic radial tolerance, bounded fitting, station remapping,
  endpoint validation and atomic failure behavior.
- [x] Inspect legacy content and select per-instance conversion that preserves
  shared source assets and generated-component persistence fixes.

Completion: implemented contracts are documented outside this plan.

### Stage 1: Enforce graph and mutation invariants

- [x] Validate IDs, topology, terminal flow, endpoints, section continuity and
  coverage of authoritative curve length before publication.
- [x] Route accepted asset mutations and reflected replay notifications through
  the existing mutation listener boundary; reject invalid candidates atomically.
- [x] Add schema 3 legacy station conversion and retain invalid data with diagnostics.
- [ ] Qualify full editor Undo/Redo/Cancel of road structural edits.

Completion: graph publication and complete editor replay agree.

### Stage 2: Build final-curve alignment snapshots

- [x] Move plane/sphere projection into explicit detached candidate fitting and
  asset publication; synchronize nodes, connectors and lane stations.
- [x] Certify whole-cubic spherical radial bounds and preserve existing point IDs.
- [x] Derive preview intervals exactly from the final curve and share its distance
  table with position, frame and lane queries.
- [x] Correct Engine distance-table sampling of nonuniform and inflected cubics.
- [x] Cover plane/sphere, seam/polar/long arcs, elevation, rigid coordinates,
  rejected fitting and legacy station compatibility numerically.

Completion: final curve, preview and distance queries have one geometry meaning.

### Stage 3: Connect assets to scene previews

- [x] Offer explicit Plane/Sphere selection at creation with fixed planet binding.
- [x] Remove editable Actor Surface controls and runtime projection; keep hidden
  legacy fields solely for version-gated conversion.
- [x] Convert legacy constrained instances into private persistent road subobjects
  without mutating shared assets, and preserve failures atomically.
- [x] Qualify transient export exclusion, converted-curve round trips, checked-in
  L_RoadNet opening and GC without Dirty changes.
- [x] Preserve Ready/Stale/Error diagnostics, component reuse, preflight and detach.
- [ ] Qualify interactive picking and visual material display for both surfaces.

Completion: scene generation and user-visible editor interaction are qualified.

### Stage 4: Qualify and hand off the foundation

- [x] Publish the final geometry and migration contracts in the runtime domain.
- [x] Record checkpoint affected-test and full editor build evidence.
- [ ] Complete the outstanding editor replay/picking/material gates before marking
  the broader roadmap P0 complete.

Completion: all stage gates pass; no broader completion is inferred from this checkpoint.

## Related Code

- [Road types](../../RoadWeaver/Source/Runtime/RoadWeaver/Public/RoadNet/RoadNetTypes.h)
- [Road validation](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadNet.cpp)
- [Explicit fitting and snapshots](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadSurface.cpp)
- [Scene adapter and migration](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadNetActor.cpp)
- [Creation dialog](../../RoadWeaver/Source/Editor/RoadWeaverEditor/Private/Dialogs/RoadNetCreateDialog.cpp)
- [Road System Evolution](../Roadmaps/RoadSystemEvolution.md)
