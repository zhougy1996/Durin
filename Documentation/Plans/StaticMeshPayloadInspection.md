# StaticMesh Payload Inspection Plan

Summary: Move StaticMesh inspection aggregation to the editor and expose read-only source, derived, cooked, CPU, GPU, and collision diagnostics.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Planning only. Stage 0 can begin independently; the Stage 1 prerequisite
[StaticMesh source residency](StaticMeshSourceResidency.md) API is complete.
Its supplementary GPU validation does not block this plan. Stage 2 still
requires the bounded operation diagnostics from
[StaticMesh authored compilation](StaticMeshAuthoredCompilation.md).

DStaticMesh::InspectCollision currently assembles an Inspector-oriented value in
Engine. MStaticMeshInspector displays LOD/material/collision and GPU statistics,
but does not assemble domain-qualified authored source, DDC, cooked payload,
and CPU residency information. Texture payload inspection has already moved
its aggregation into TextureEditor and is a reference for ownership, not a
requirement to introduce a shared cross-family inspection framework.

## Goal

Let editor users distinguish missing source storage, unavailable build output,
pending CPU work, and GPU failure without a status query changing asset state.
Retain the Engine facts needed by runtime consumers and present repair ownership
in StaticMeshEditor.

## Selected Design

- StaticMeshEditor owns the inspection snapshot, package-field interpretation,
  diagnostic aggregation, and UI. Engine exposes bounded const runtime facts.
  Audit all InspectCollision consumers before moving or removing its API; keep
  genuinely runtime-required queries in Engine.
- Separate authored source storage and residency, render/collision derived
  products, cooked field placement, decoded CPU state, and GPU state. A known
  identity or bulk descriptor does not prove the backing data is readable.
- Default live inspection and construct-free package inspection read metadata
  only. They do not acquire source handles, decode/load bulk, probe DDC, invoke
  providers, initialize resources, dirty packages, save, or remove files.
- Show DDC origin/key/timing/persistence diagnostics only from an available
  operation snapshot. No diagnostic history is copied onto DStaticMesh or
  DBodySetup. Missing or evicted history is explicitly unavailable, never an
  inferred cache hit or miss. Avoid exposing backend filesystem paths.
- Preserve separate authored-compilation and cooked-load states. Collision
  coherence must be based on existing meaningful facts; do not label a nonzero
  revision alone as proven coherence if it cannot establish correspondence.
- Repair guidance names explicit workflows: restore/reimport authored data,
  rebuild disposable derived output, recook cooked data, or retry resource
  initialization. This plan adds guidance, not automatic repair or deletion.
- No new cooked format, source schema, repair framework, or renderer behavior.

## Implementation Stages

### Stage 0: Audit consumers and freeze inspection semantics

- [ ] Inventory InspectCollision callers and determine the minimal Engine facts
  used by runtime versus editor consumers; record a migration map.
- [ ] Specify live and construct-free snapshot fields, placement/readability
  distinctions, unavailable states, collision coherence meaning, and bounded
  diagnostic strings. Verify metadata inspection APIs have no hidden I/O.
- [ ] Define the UI grouping and messages for pending, absent, failed, cancelled,
  and ready CPU/GPU states, and the explicit workflow for each repair category.

Completion: one field/owner/state table with no ambiguity about query effects.

### Stage 1: Implement editor-owned metadata inspection

Depends on Stage 0 and completion of the source residency plan.

- [ ] Add StaticMeshEditor inspection APIs and move editor-only aggregation out
  of DStaticMesh; migrate callers and remove obsolete Engine exports after
  runtime consumers have narrow replacements.
- [ ] Implement construct-free package field inspection for ImportedData,
  RenderData, and CollisionData plus metadata-only live source/residency facts.
- [ ] Preserve package/field-qualified diagnostics and explicit unavailable
  states for unsupported, absent, or malformed descriptors.
- [ ] Test authored and cooked fixtures, absent companions, malformed metadata,
  missing live data, and collision statistics. Instrument/assert zero bulk
  reads, object construction, cache calls, provider calls, and mutation.

Completion: snapshots describe metadata truthfully and queries are observational.

### Stage 2: Integrate operation diagnostics and Inspector UI

Depends on Stage 1 and completion of the authored compilation plan.

- [ ] Join bounded manager observations with live source, cooked-load, CPU/GPU,
  and collision facts; qualify request identity and prevent old diagnostics
  from appearing as the current asset state.
- [ ] Update MStaticMeshInspector with source placement/identity, current or
  unavailable build diagnostics, CPU/GPU phases, and separate render/collision
  payload facts while preserving LOD and material editing behavior.
- [ ] Add actionable workflow guidance with no implicit repair. Distinguish
  metadata presence from validated/read data and keep evicted history explicit.
- [ ] Test pending/current/superseded/failed/cancelled operations, DDC hit versus
  rebuilt observations, cache persistence failure, cooked CPU failure, and GPU
  failure/retry states. Verify polling causes no retries or source reads.

Completion: the UI reflects domain-specific readiness and current observations
without changing the state it reports.

### Stage 3: Qualify and publish editor contracts

Depends on Stage 2.

- [ ] Run inspection and affected StaticMesh editor/native coverage under the
  repository workflows; exercise narrow/wide Inspector layouts and long error
  messages using available UI verification, recording any unavailable lane.
- [ ] Verify Engine/Game has no new StaticMeshEditor dependency and that the
  editor-only aggregation is absent from runtime-only targets.
- [ ] Update StaticMesh Inspector guidance and asset lifecycle inspection
  ownership; record actual validation evidence and complete only after gates
  pass. Do not describe metadata inspection as physical payload validation.

Completion: read-only behavior, module boundaries, UI, and documentation agree.

## Validation And Contract Owners

Use [build workflow](../Agents/BuildAndRun.md) and
[testing workflow](../Agents/Testing.md) for implementation validation.
Contracts: [StaticMesh Inspector](../Editor/Guides/StaticMeshInspector.md),
[asset lifecycle](../Runtime/Assets/AssetDataLifecycle.md), and
[package bulk data](../Runtime/Assets/BulkData.md).

## Related Code

- `Engine/Source/Runtime/Engine/Public/StaticMesh/StaticMesh.h`
- `Engine/Source/Runtime/Engine/Private/StaticMesh/StaticMesh.cpp`
- `Engine/Source/Runtime/Engine/Public/Asset/CookedMeshLoading.h`
- `Engine/Source/Editor/StaticMeshEditor/Private/Widgets/MStaticMeshInspector.cpp`
- `Engine/Source/Editor/TextureEditor/Public/Diagnostics/TexturePayloadInspection.h`
- `Engine/Source/Editor/TextureEditor/Private/Diagnostics/TexturePayloadInspection.cpp`
