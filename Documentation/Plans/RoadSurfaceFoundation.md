# Road Surface Foundation Plan

Summary: Validate RoadWeaver graph semantics and preview authoritative final road curves created on planes or spheres.

Last reviewed: 2026-10-18

Status: Completed
Completed: 2026-10-18

## Current Status

P0 is complete. Persisted final Cartesian curves own road geometry, explicit
plane/sphere fitting and fixed planet identity. Current-schema persistence,
validation, derived preview lifecycle, editor transaction replay, viewport picking
and material display have acceptance evidence. This closes the foundation only;
the later road-authoring, junction and terrain milestones remain separate work.

Closeout on 2026-10-18 used source base
`1c0c3718c0e02f5a0b27fc8d3266e7eb87a9171a` plus this closeout's test-only
changes, `MacOS-arm64-Debug-DurinEditor`, macOS 27.0.1, Xcode 26.6 and Apple M4
(10 GPU cores, 16 GB unified memory). Production source is unchanged. New test
targets were compiled; the earlier workspace `all`/editor build evidence remains
applicable rather than requiring another broad build for test/document changes.

| Acceptance selection | Result |
| --- | --- |
| `./DevTool test affected --report Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/RoadSurfaceAffected.xml` | All three RoadWeaver targets passed: RoadGraphContractTests 14/14, RoadSceneIntegrationTests 5/5 and RoadEditorIntegrationTests 8/8. |
| `./DevTool test RoadEditorIntegrationTests --isolate --test-jobs 4 --report Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/RoadEditorIsolated.xml` | All eight plane/sphere cases also passed individually. |
| `RoadViewportQualificationTests --mode qualification`, `DURIN_RHI_BACKEND=metal` | Both plane/sphere cases passed on native Metal with threaded RHI. |
| Same qualification, `DURIN_RHI_BACKEND=vulkan` | Both cases passed on MoltenVK, Apple M4 Vulkan 1.3.334, driver `0x28a1`, validation enabled and threaded RHI; no VUID/validation errors. |
| `./DevTool run --project RoadWeaver/RoadWeaver.dproject -- --exit-after-ticks=3` | Real Metal editor window loaded the default road level, presented, completed three ticks and shut down normally. |

The editor integration fixture uses the real `FPropertyEditSession` and
`DTransBuffer`, not manually delivered Undo/Redo notifications. Complete road-array
addition/removal replays preserve IDs and exact serialized definitions, rebuild
valid generated previews and restore the saved package revision. Cancel rolls
back the interactive candidate without adding history. Duplicate lane identities
are rejected without changing data, preview snapshots or Dirty state. The
whole-object `FScopedTransaction::Modify` path is qualified separately.

The viewport fixture drives the production LevelEditor selection mode with
normalized mouse input and the real GPU hit-proxy backend, without a fake picking
backend. Ordinary clicks select the generated road, Ctrl-click toggles it and
blank-space clicks clear selection on both surfaces. A 40 m, two-lane preview
(sphere radius 100 m) renders at 256x192 with authored red/green unlit materials;
material overrides visibly replace the road color. Checked pixels number 4188
for the plane and 3874 for the sphere for each color on both backends. All four
paired Metal/MoltenVK RGB images match byte-for-byte and were visually inspected.
These are correctness/image receipts, not performance qualification.

GPU execution used the authorized host environment outside the Codex sandbox.
Reproduce with `DURIN_RHI_BACKEND=metal` or `vulkan`, `DURIN_TEST_KEEP_WORK=1` and
`./DevTool test RoadViewportQualificationTests --mode qualification --report`.
The final per-case reports are `RoadViewportMetalCases.xml` and
`RoadViewportVulkanCases.xml`, with corresponding CTest reports
`RoadViewportMetal.xml` and `RoadViewportVulkan.xml`, all under
`Build/NativeTestResults/MacOS-arm64-Debug-DurinEditor/`. Set `GTEST_OUTPUT=xml:`
to an absolute report path to retain per-case properties. Raw native logs,
PPM captures, hashes and exact-image comparison results are retained under
`Build/Qualification/RoadSurfaceFoundation/` (`images.json`). Final GPU launcher
logs are `Build/.agent-state/logs/20261018-062452-666968-37039-ctest.log` and
`20261018-062423-777297-36739-ctest.log` in the same directory. Affected and isolated
logs are `20261018-062514-089838-37176-ctest.log` and
`20261018-062530-249910-37221-ctest.log` there.
The successful window smoke log is
`Build/.agent-state/logs/20261018-062546-852223-37260-DurinEditor.log`.

Earlier failed attempts remain in the launcher logs: unrelated application-test
discovery timed out while an old cache override enabled those optional tests;
restoring the preset's `DURIN_ENABLE_APPLICATION_TESTS=OFF` resolved discovery.
Fixture development also corrected asset-package initialization, unique package
identities, scene Release ordering and the RHI frame boundary around input-driven
GPU picking. No production workaround was needed. The final successful receipts
cover these corrections; later documentation-only changes reuse those results.

The earlier Win64 checkpoint passed the workspace `all` build and road/spline/
asset/world validation. Its unrelated cloud compile-time-budget failure passed
an isolated rerun without source changes. It is historical coverage, not a new
Windows receipt for this closeout. No remaining foundation acceptance gate is
deferred to that platform.

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
- [x] Qualify full editor Undo/Redo/Cancel of road structural edits.

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
- [x] Qualify interactive picking and visual material display for both surfaces.

Completion: scene generation and user-visible editor interaction are qualified.

### Stage 4: Qualify and hand off the foundation

- [x] Publish the final geometry and migration contracts in the runtime domain.
- [x] Record checkpoint affected-test and full editor build evidence.
- [x] Complete the outstanding editor replay/picking/material gates before marking
  the broader roadmap P0 complete.

Completion: all stage gates pass; no broader completion is inferred from this checkpoint.

## Related Code

- [Road types](../../RoadWeaver/Source/Runtime/RoadWeaver/Public/RoadNet/RoadNetTypes.h)
- [Road validation](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadNet.cpp)
- [Explicit fitting and snapshots](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadSurface.cpp)
- [Scene adapter and migration](../../RoadWeaver/Source/Runtime/RoadWeaver/Private/RoadNet/RoadNetActor.cpp)
- [Creation dialog](../../RoadWeaver/Source/Editor/RoadWeaverEditor/Private/Dialogs/RoadNetCreateDialog.cpp)
- [Road System Evolution](../Roadmaps/RoadSystemEvolution.md)
