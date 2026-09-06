# StaticMesh Payload Inspection Plan

Summary: Move StaticMesh inspection aggregation to the editor and expose read-only source, derived, cooked, CPU, GPU, and collision diagnostics.

Last reviewed: 2026-09-07

Status: Active
Completed:

## Current Status

Stages 0–1 are complete. Stage 2 implementation and routine CPU coverage are
complete; its GPU failure/retry gate and Stage 3 interactive layout qualification
remain open because the current session has no Metal access and the Mac is locked. Both prerequisite APIs are complete. No runtime
consumer needs the old Inspector aggregation: its only production caller was
MStaticMeshInspector, plus the StaticMesh collision native fixture.

### Inspection field, owner, and state contract

| Field / consumer | Owner and query | State / effect |
| --- | --- | --- |
| Former InspectCollision UI and native fixture | StaticMeshEditor `InspectStaticMeshCollision` | Migrate both; remove Engine aggregation/type. Runtime keeps BodySetup getters. |
| ImportedData.Geometry package descriptor | StaticMeshEditor, supplied tagged field tree | Absent, malformed, unsupported, or metadata present; inline/companion placement does not establish readability. |
| Live source identity / canonical bytes / decoded residency | Engine const source getters, editor snapshot | Metadata validity and residency separate; no AcquireGeometry or GetPayload. |
| RenderData / CollisionData package fields | StaticMeshEditor, supplied descriptors | Separate fields and byte counts; no companion existence/integrity probe. |
| Authored operation | Engine bounded manager snapshot, editor presentation | Request 0 means never observed/evicted. Queued, building, mailbox, succeeded, failed, cancelled, superseded are operation history, not live readiness. Source match alone does not establish settings coherence. |
| Render/collision DDC | Optional operation observations | Origin/key/read/write costs only when observed. Successful product does not imply persistence. No cache calls or backend paths. |
| CPU / cooked load | Engine const load snapshot | Resident render data is independent of authored-source residency. Unloaded, I/O queued, reading, decoding, ready, failed, cancelled; generation qualifies observation. |
| GPU | Engine resource snapshot | Unavailable, queued, ready, failed, with revision. Failure guidance names explicit resource retry. |
| Collision statistics | Editor aggregates installed immutable BodySetup geometry | No BuildSimpleGeometry: it can allocate/cache primitives. Revision is displayed but coherence stays unavailable because no source correspondence is stored. |
| Repair guidance | StaticMeshEditor | Restore/reimport source, rebuild disposable derived output, recook cooked data, explicitly retry resources. No automatic repair. |

All live queries run on the owner thread. Diagnostic messages are capped at
4096 bytes. Package queries consume already supplied metadata; the caller's
package acquisition is a separate operation. The shared descriptor reader's
new metadata mode skips inline payload hashing. Tagged-struct parsing copies
in-memory field records but does not resolve objects or open bulk storage.

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

- [x] Inventory InspectCollision callers and determine the minimal Engine facts
  used by runtime versus editor consumers; record a migration map.
- [x] Specify live and construct-free snapshot fields, placement/readability
  distinctions, unavailable states, collision coherence meaning, and bounded
  diagnostic strings. Verify metadata inspection APIs have no hidden I/O.
- [x] Define the UI grouping and messages for pending, absent, failed, cancelled,
  and ready CPU/GPU states, and the explicit workflow for each repair category.

Completion: one field/owner/state table with no ambiguity about query effects.

### Stage 1: Implement editor-owned metadata inspection

Depends on Stage 0 and completion of the source residency plan.

- [x] Add StaticMeshEditor inspection APIs and move editor-only aggregation out
  of DStaticMesh; migrate callers and remove obsolete Engine exports after
  runtime consumers have narrow replacements.
- [x] Implement construct-free package field inspection for ImportedData,
  RenderData, and CollisionData plus metadata-only live source/residency facts.
- [x] Preserve package/field-qualified diagnostics and explicit unavailable
  states for unsupported, absent, or malformed descriptors.
- [x] Test authored and cooked fixtures, absent companions, malformed metadata,
  missing live data, and collision statistics. Instrument/assert zero bulk
  reads, object construction, cache calls, provider calls, and mutation.

Completion: snapshots describe metadata truthfully and queries are observational.

### Stage 2: Integrate operation diagnostics and Inspector UI

Depends on Stage 1 and completion of the authored compilation plan.

- [x] Join bounded manager observations with live source, cooked-load, CPU/GPU,
  and collision facts; qualify request identity and prevent old diagnostics
  from appearing as the current asset state.
- [x] Update MStaticMeshInspector with source placement/identity, current or
  unavailable build diagnostics, CPU/GPU phases, and separate render/collision
  payload facts while preserving LOD and material editing behavior.
- [x] Add actionable workflow guidance with no implicit repair. Distinguish
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
- [x] Verify Engine/Game has no new StaticMeshEditor dependency and that the
  editor-only aggregation is absent from runtime-only targets.
- [ ] Update StaticMesh Inspector guidance and asset lifecycle inspection
  ownership; record actual validation evidence and complete only after gates
  pass. Do not describe metadata inspection as physical payload validation.

Completion: read-only behavior, module boundaries, UI, and documentation agree.

## Validation Evidence And Remaining Qualification

- `test StaticMeshTests`: 111/111 tests passed, including four new inspection
  cases. Existing authored manager scenarios now read through the editor
  snapshot: queued/current/superseded/failed/cancelled, retained-history eviction,
  cold/warm DDC and persistence failure. Cooked cancellation polling also uses
  the snapshot and preserves generation and absence of CPU data.
- The unreadable source probe received zero requests during 100 inspections;
  object count, CPU pointer, source identity, GPU revision, package dirty state
  and manager accounting were unchanged. The build provider was unloaded during
  polling and stayed unloaded. The inspection call graph contains no DDC calls.
- Authored/cooked fixtures preserve separate field presence. A nonexistent
  physical path does not change supplied metadata; malformed and unsupported
  fields remain distinct. Deliberately incorrect inline payload hashes are
  accepted only in metadata mode, demonstrating that this mode is not payload
  validation. The default existing descriptor API still rejects bad hashes.
- `test affected` passed all 28 selected routine targets on macOS arm64 Debug.
  The full `all` build passed on MacOS-arm64-Debug-DurinEditor. Final reruns
  after value-initializing empty snapshot enums also passed: affected build
  3.48 s / tests 24.95 s (28/28 targets), full `all` build 0.20 s.
  Final logs: `Build/.agent-state/logs/20260907-051849-402897-61906-ctest.log`
  and `Build/.agent-state/logs/20260907-051935-549937-62161-cmake.log`.
- Source/module audit found no runtime consumer of removed `InspectCollision`
  and no Runtime `.dmodule` dependency on StaticMeshEditor. Aggregation now
  exists only under the Editor module. No Game configuration was added.
- GPU qualification target compiled, but
  `FStaticMeshRenderPreparationVulkanTests.BlockingMeshCpuResidencyDoesNotInitializeGpuResources`
  failed before assertions at RHI initialization: `VK_ERROR_INCOMPATIBLE_DRIVER`,
  Metal unavailable. Log: `Build/.agent-state/logs/20260907-051830-929284-61887-ctest.log`.
  This is an outstanding GPU gate, not a passing failure/retry test. Do not
  retry in the same environment without evidence of restored device access.
- Computer-use inventory reported the Mac locked. Narrow/wide layouts and long
  messages therefore have code review/compile coverage only; wrapped value and
  diagnostic text is implemented but interactive verification is outstanding.
  No application smoke was launched.
- Inspector guidance and lifecycle ownership were updated; changed-document
  validation and all-plan validation passed. The plan stays Active until the
  GPU and interactive layout lanes pass. Resume those lanes in a capable,
  unlocked session, then complete Stage 2/3 checklists and lifecycle metadata.

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
