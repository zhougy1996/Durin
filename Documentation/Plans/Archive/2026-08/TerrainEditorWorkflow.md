# Terrain Editor Workflow Plan

Summary: Complete the finite Terrain editor workflow with transactional placement and properties, exact surface picking, bounded presentation and diagnostics, and end-to-end qualification.

Last reviewed: 2026-08-14

Status: Archived
Completed: 2026-08-14

## Current Status

T0-T3 have stabilized the height authority, Terrain primitive, collision, and
patch LOD contracts. The Level Editor already exposes explicit grayscale16
heightmap import, generic reimport, reflected Terrain properties, World Outliner
construction, viewport drag/drop placement, conservative picking bounds, and
render/collision revision propagation.

The remaining T4 gaps are product-workflow gaps rather than new runtime terrain
features. Terrain drag/drop mutates the Level directly instead of using a
planned undoable authoring transaction. Terrain enters the viewport picking
scene index but the geometry backend has no Terrain provider, so a surface
request cannot produce a Terrain hit. Generic Details exposes raw fields but
does not present a coherent render/collision status, bounded diagnostics, or
revision/resource facts. Heightmap assets retain the generic icon, and no one
fixture proves import through cooked Game use as a single workflow.

T4 is complete. Terrain placement now uses a revision-checked authoring
transaction; reflected property edits retain their validated generic
transaction path. Exact full-resolution Terrain surface picking participates in
reference, accelerated, and compare policies. Canonical thumbnails and bounded
asset/render/collision Details facts complete the finite editor presentation.

Qualification on the 1025x1025 fixture visited 4,096 of 1,048,576 cells
(0.390625 percent), tested 8,192 triangles, measured a 607,293,900 ns reference
median and 8,068,300 ns accelerated median, and reported zero Terrain parity
mismatches. Focused authoring, viewport, thumbnail, heightmap, Cook, render,
collision, aggregate native, Editor/Game build and smoke gates completed under
the repository validation workflow.

## Goal

A user can import one finite heightmap, recognize it in Content Browser, place
and select a Terrain, edit and undo its supported properties, inspect render
and collision health, reimport without mixed generations, save/reload, Cook,
launch Game, and obtain exact World collision behavior. Every action preserves
the authoritative asset revision and the existing Renderer and Physics
contracts.

## Scope

- Content Browser presentation and existing import/reimport actions for
  `DTerrainHeightmap`.
- Transactional creation and configuration of `ATerrainActor` from viewport
  drag/drop and ordinary World Outliner construction.
- Terrain Component Details rows for authored properties, immutable heightmap
  facts, render status, collision status, revisions, resource facts, and
  bounded actionable diagnostics.
- Exact full-resolution Terrain surface picking through the existing semantic
  viewport picking service and scene index.
- Undo/redo, dirty-state, save/reload, asset replacement, successful and failed
  reimport, Cook, Editor launch, Game launch, and shutdown qualification.
- Lasting Editor architecture/guide documentation plus final terrain roadmap
  evidence.

## Non-Goals

- Sculpt, smooth, flatten, erosion, holes, painted layers, or runtime
  deformation.
- Streaming, world partition, foliage, navmesh, water, roads, splines, or
  procedural terrain generation.
- A dedicated Terrain asset editor, live source watcher, or source-control UI.
- A Terrain-only viewport, selection system, transaction stack, diagnostic
  console, import framework, or picking scene.
- Renderer-selected LOD picking, render-buffer readback, GPU picking, or use of
  visual LOD as collision geometry.
- Changes to T0 sample orientation, T1/T3 render topology or LOD policy, or T2
  World collision semantics.

## Design Decisions and Invariants

### Authoring and transactions

- Level Editor owns one `FTerrainLevelAuthoringService` with capture, plan, and
  execute phases. Viewport drop and any Terrain-specific creation action use
  this service; UI code does not directly spawn and mutate a Terrain Actor.
- A placement plan captures the target Level generation, read-only state,
  unique Actor name, heightmap identity/revision, placement transform, and
  initial component values. Execution rejects stale or invalid inputs before
  mutation and commits create/configure/select-dirty effects as one undoable
  editor transaction.
- Undo destroys the created Actor and restores the prior package saved state;
  redo recreates the same authored values and stable requested name subject to
  the transaction manager's established identity rules. Failed and net-zero
  edits add no history entry.
- Generic reflected property edits remain the mutation mechanism. Terrain
  validation rejects invalid object classes, non-finite values, and non-positive
  spacing before publication. One accepted edit causes one coherent render and
  physics revision transition and one dirty-state change.

### Picking

- Terrain remains an `ActorComponentSurface` candidate in the shared picking
  scene index. A Terrain geometry provider is added to the existing reference,
  accelerated, and compare backend policy; it returns the owning Actor,
  component, primitive identity, and non-negative world distance through the
  existing semantic result.
- Picking uses the committed full-resolution height payload and its immutable
  regular-grid acceleration through a narrow snapshot/query boundary. It does
  not read Renderer buffers or selected LOD, require collision to be enabled,
  publish a BodyInstance, or materialize a full triangle array.
- The exact cell diagonal, top-left row-major orientation, signed height
  reconstruction, double-sided surface rule, and stable closest-hit ordering
  match rendering and Physics. Rotation, translation, positive non-uniform
  scale, mirroring, grazing rays, border cells, and asymmetric non-square
  fixtures are covered.
- Bounds only reject candidates. Missing, stale, malformed, over-budget, or
  singular snapshots make that component invalid or a miss according to the
  existing complete-request policy; conservative bounds never become a
  successful hit.
- Compare mode runs the accelerated heightfield traversal against a bounded
  exact cell oracle and returns the reference result on parity mismatch while
  incrementing named Terrain diagnostics.

### Presentation and diagnostics

- `DTerrainHeightmap` receives a deterministic editor-derived thumbnail from
  its canonical payload, not from the encoded source or Terrain Renderer. It
  uses a fixed grayscale min/max mapping and orientation marker, includes the
  package/payload and generator schema in identity, obeys the shared thumbnail
  scheduler/cache budgets, and safely falls back to the class icon.
- Terrain Details groups authored values separately from read-only asset,
  render, and collision facts. Status labels use stable enum values; diagnostics
  are capped at 2,048 bytes and provide no per-frame log stream.
- Details never makes cached renderer or collision resources authoritative.
  Facts are snapshots and may report pending/unavailable state; refresh is
  driven by existing object, selection, asset, and scene invalidation paths.
- Reimport continues through the provider-neutral single-asset action. A
  successful changed revision updates every loaded Terrain consumer atomically;
  identical reimport is a semantic no-op; failure retains the prior package,
  proxy, collision geometry, selection, thumbnail, and Details facts.

### Qualification boundary

- The golden workflow uses one asymmetric 257x129 grayscale16 heightmap with
  distinct corners, an interior peak, and non-default spacing, signed height
  scale/offset, material, transform, visibility, and collision settings.
- Maximum-scale checks retain the existing 1025x1025 sample ceiling, 64x64-cell
  patches, 64 MiB viewport scene-index budget, terrain render qualification
  gates, and qualified HeightField collision budgets. T4 does not silently
  widen any earlier limit.
- Thumbnail output is fixed at 256x256 and remains inside the shared thumbnail
  CPU/GPU/disk budgets. Terrain-specific Details state retains no unbounded
  history and no duplicate sample payload.
- Qualification records Terrain picking visited nodes/cells/triangles and
  median time. A sparse ray against a 1025x1025 Terrain must visit no more than
  1 percent of its cells, accelerated median must be no slower than one quarter
  of the exact full-cell oracle median, and compare parity mismatches must be
  zero.

## Current Foundations and Gaps

| Area | Existing foundation | T4 gap |
| --- | --- | --- |
| Import/reimport | Explicit heightmap import, provider-neutral reimport, transactional asset publication, rollback | No workflow-level UI and consumer-generation fixture |
| Placement | Terrain drag/drop creates, positions, dirties, and selects an Actor | Direct Level mutation; no plan, undo/redo, stale/read-only rejection, or isolated service tests |
| Properties | Reflected component fields validate and recreate render/physics state | No Terrain-specific grouping, facts, diagnostic presentation, or editor transaction matrix |
| Picking | Terrain publishes conservative bounds and enters the shared scene index | No Terrain geometry provider; surface selection cannot complete with a hit |
| Presentation | Content Browser knows the asset class and actions | Generic icon only; no canonical height preview or invalidation proof |
| Runtime proof | Asset, render, collision, Cook, and Game tests pass independently | No single persisted fixture proves the complete editor-to-runtime workflow |
| Documentation | Runtime asset/render/collision contracts are authoritative | No lasting Terrain editor workflow architecture or user guide |

## Implementation Stages

### Stage 0: Freeze the workflow contract and fixtures

- [x] Add the asymmetric 257x129 golden source and expected sample, bounds,
  pick, render-revision, collision-revision, and thumbnail facts under the
  existing native test-data ownership.
- [x] Capture baseline behavior for viewport drop, generic reflected edits,
  reimport refresh, Terrain scene-index admission, and missing geometry hits.
- [x] Freeze stable placement, property-validation, render, collision,
  thumbnail, and picking diagnostic categories and their 2,048-byte cap.
- [x] Freeze the 1025x1025 picking qualification rays, exact oracle, work
  counters, retained-memory observations, and timing methodology.
- [x] Confirm the owning modules and dependency direction for the authoring
  service, Details customization, thumbnail provider, and immutable picking
  snapshot before adding public APIs.

#### Acceptance Gate

- The committed fixtures make orientation and expected values unambiguous,
  baseline tests demonstrate each named gap, all budgets are numeric, and no
  open ownership or format decision remains.

### Stage 1: Make placement and property editing transactional

- [x] Implement `FTerrainLevelAuthoringService` capture/plan/execute values with
  stale Level, read-only, asset-class/revision, transform, name, and component
  property validation.
- [x] Route Terrain viewport drop through the service and remove the direct
  spawn/configure branch while preserving ray placement, unique naming,
  selection, and user-facing failure reporting.
- [x] Make create/configure one transaction with exact undo/redo and package
  saved-state restoration; cover cancellation, failed execution, and repeated
  undo/redo without leaked Actor, proxy, or BodyInstance state.
- [x] Qualify reflected edits for heightmap, spacing, signed height range,
  material, visibility, collision policy, and transform, including no-op and
  invalid edit behavior.

#### Acceptance Gate

- Viewport placement and supported Details edits are deterministic, read-only
  safe, dirty the package exactly once, undo/redo exactly, and leave render and
  collision consumers on one accepted asset revision.

### Stage 2: Add exact Terrain surface picking

- [x] Expose the minimum immutable Terrain picking snapshot/query boundary from
  Engine/PhysicsCore without exposing Renderer state or mutable samples to
  LevelEditor.
- [x] Add Terrain geometry providers to reference, accelerated, and compare
  picking policies with bounds rejection, stable hit identity/distance, named
  counters, and complete-request failure behavior.
- [x] Cover asymmetric coordinates, both cell triangles, borders, grazing and
  double-sided rays, transforms, positive non-uniform scale, mirroring,
  singular transforms, hidden/retired components, asset replacement, and
  revision invalidation.
- [x] Extend scene-index and selection tests so Terrain competes correctly with
  meshes, visualizers, and other Terrains under the established distance,
  priority, and stable-key rules.
- [x] Run the 1025x1025 qualification fixture and enforce the frozen cell-work,
  parity, retained-memory, and relative-time gates.

#### Acceptance Gate

- Clicking visible Terrain selects its exact Actor/component surface with
  renderer/collision coordinate parity; empty space remains empty, stale work
  cannot win, compare mismatches are zero, and maximum-scale work stays within
  the frozen bounds.

### Stage 3: Complete presentation, diagnostics, and reimport feedback

- [x] Register a scoped exact-class Terrain heightmap thumbnail provider with
  deterministic canonical-payload generation, fixed visual identity, safe
  cancellation/unload, persistent-cache invalidation, and class-icon fallback.
- [x] Add a `DTerrainComponent` Details customization that groups authored
  controls and bounded read-only asset/render/collision facts without exposing
  mutable derived state.
- [x] Present actionable missing-heightmap, invalid-property, invalid-payload,
  extent, render-resource, and collision-build failures; ensure search and
  selection refresh use the existing Details contracts.
- [x] Qualify identical, changed, missing-source, corrupt-source, save-failure,
  asset replacement, and shared multi-world reimport paths while preserving
  selection and the prior complete generation on failure.
- [x] Cover provider registration/unregistration, cold/warm thumbnail requests,
  orientation, revision invalidation, cache corruption, cancellation, and
  shutdown.

#### Acceptance Gate

- Heightmap cards are recognizable and revision-correct, Terrain Details gives
  bounded coherent health information, and every reimport outcome is visible
  without stale thumbnails, mixed consumer revisions, or lost selection.

### Stage 4: Qualify the end-to-end workflow and complete T4

- [x] Automate the golden sequence: import, reveal, drag/drop place, configure,
  undo/redo, exact pick, save, reload, changed and failed reimport, World
  collision query, Cook, source/DDC removal, Game load, and shutdown.
- [x] Re-run maximum Terrain asset, render/LOD, collision, viewport picking,
  thumbnail, transaction, lifecycle, and resource qualification with the T4
  integrations enabled.
- [x] Run focused Terrain/editor suites, aggregate native tests, full Editor and
  Game builds, hidden-window Editor/Game smokes, and documentation validators
  according to the repository build, test, and documentation guides.
- [x] Publish lasting Terrain editor behavior under Editor Architecture and a
  concise user workflow under Editor Guides; update existing asset, viewport,
  thumbnail, rendering, and collision documents only where their lasting
  contracts changed.
- [x] Record measured counters/timings/bytes and final workflow evidence here,
  mark T4 complete in the Heightfield Terrain roadmap, and leave T5/T6
  conditional unless their activation evidence exists.

#### Acceptance Gate

- A source-free cooked Game reproduces the saved Terrain while Editor import,
  placement, properties, exact selection, diagnostics, reimport, and collision
  pass the golden workflow. All frozen validation rows and repository gates
  pass with no stale object, proxy, body, thumbnail job, pick ticket, or module
  callback after teardown.

## Validation Matrix

| Contract | Evidence | Required result |
| --- | --- | --- |
| Asset presentation | Thumbnail provider unit/integration fixtures | Canonical orientation and revision-correct 256x256 output; safe icon fallback and bounded cache use |
| Placement | Terrain authoring service and viewport interaction tests | One atomic create/configure transaction; stale/read-only/failure paths do not mutate |
| Properties | Reflected property transaction and Terrain component tests | Valid edits undo/redo and synchronize one generation; invalid/no-op edits add no history |
| Exact picking | Terrain provider golden/random/compare tests | Full-resolution closest hit matches the exact oracle across cells, transforms, and ties |
| Picking scale | 1025x1025 qualification | At most 1 percent cells visited, accelerated median at most one quarter oracle median, zero parity mismatches |
| Reimport | Single-asset UI plus shared-consumer lifecycle tests | Changed revision propagates atomically; identical is no-op; every failure retains the prior complete state |
| Diagnostics | Details customization tests | Stable statuses/facts, actionable capped messages, no mutable derived-state editing or unbounded history |
| Persistence | Level save/reload and asset replacement tests | Authored values and references round-trip; derived consumers rebuild from the same revision |
| Cook/runtime | Cook test and DurinGame smoke without source/DDC | Exact height, render, and collision behavior require only cooked packages/companions |
| Lifecycle | undo/redo, level switch, PIE/read-only, provider unload, shutdown | No stale Actor/component, scene entry, body, thumbnail work, pick ticket, or callback |
| Aggregate quality | Focused suites, `@terrain`, all native tests, Editor/Game builds and smokes, docs validators | Every repository gate passes with recorded resource/timing facts |

## Definition of Done

- Every Stage 0-4 task and acceptance gate has evidence and is checked.
- The asymmetric golden workflow passes from import through source-free cooked
  Game load and exact collision query.
- Terrain placement/property edits are transactional; surface picking is exact
  and bounded; presentation and diagnostics are revision-correct and bounded.
- Existing asset, Renderer, Physics, viewport, thumbnail, transaction, package,
  Cook, and module-lifetime contracts remain authoritative and pass their
  regression suites.
- Lasting behavior is documented under Editor Architecture/Guides, this plan is
  marked completed, and the Heightfield Terrain roadmap records T4 completion.
- T5 streaming and T6 writable authoring remain conditional unless concrete
  activation evidence and budgets are accepted separately.

## Deferred Follow-ups

- T5 partitioned height/render/collision residency after finite-terrain scale
  evidence exceeds the frozen component budgets.
- T6 sculpting and material-layer authoring after a concrete writable workflow,
  brush/layer limits, latency budget, and persistence model are accepted.
- Optional Terrain asset editor, source watching, specialized inspection plots,
  GPU picking, or thumbnail rendering variants after measured user need.

## Related Documentation

- [Heightfield Terrain Roadmap](../../../Roadmaps/Archive/2026-08/HeightfieldTerrain.md)
- [Terrain Heightmap Asset](../../../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Terrain Rendering](../../../Runtime/Rendering/TerrainRendering.md)
- [Runtime Collision](../../../Runtime/Physics/Collision.md)
- [Viewport Editing Architecture](../../../Editor/Architecture/ViewportEditing.md)
- [Asset Thumbnails](../../../Editor/Architecture/AssetThumbnails.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Content Browser](../../../Editor/Architecture/ContentBrowser.md)

## Related Code

- `Engine/Source/Runtime/Engine/Public/Actors/TerrainActor.h`
- `Engine/Source/Runtime/Engine/Public/Components/TerrainComponent.h`
- `Engine/Source/Runtime/Engine/Private/Components/TerrainComponent.cpp`
- `Engine/Source/Runtime/Engine/Public/Terrain/TerrainHeightmap.h`
- `Engine/Source/Editor/LevelEditor/Private/Panels/SceneViewportPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Panels/ContentBrowserPanel.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingService.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Viewport/ViewportPickingSceneIndex.cpp`
- `Engine/Source/Editor/LevelEditor/Private/Customizations/LevelEditorCustomizations.cpp`
- `Engine/Source/Editor/LevelEditor/Private/LevelEditorModule.cpp`
- `Engine/Source/Editor/DurinEd/Public/Thumbnail/AssetThumbnailProvider.h`
- `Engine/Tests/Native/EngineTests/Private/Terrain`
- `Engine/Tests/Native/EngineTests/Private/Viewport`
- `Engine/Tests/Native/EngineTests/Private/Editor`
