# Asset Import Service Consolidation Plan

Summary: Consolidate provider registration and import orchestration behind one service while preserving transactional publication and reimport behavior.

Last reviewed: 2026-08-15

Status: Completed
Completed: 2026-08-15

## Current Status

This is the completed M3 child plan of the
[Asset Architecture Simplification Roadmap](../Roadmaps/AssetArchitectureSimplification.md).
M1 owns resident authoring publication and M2 owns strict package compatibility.
Stage 0 freezes every importer registration, execution path, editor caller, and
failure/lifetime gate before public registries are consolidated.

## Outcome

- A production importer registers one immutable descriptor with one import
  service; callers do not coordinate provider, single-asset-handler, and
  record-handler registries.
- Initial import, single-asset reimport/repair, and record-backed multi-output
  reimport use one request, planning, execution, and publication vocabulary.
- Provider leases, asynchronous planning, cancellation, module retirement, and
  publication admission have one lifetime owner.
- Import candidates remain ordinary `NewlyCreated`, Dirty resident packages;
  successful publication changes their existing resident entries to
  `Published`, and rollback uses explicit `UnloadPackage(..., DiscardUnsaved)`.
- Scene multi-output reconciliation, import records, source repair, warnings,
  stale-plan rejection, and failure compensation retain their behavior.

## Scope

- AssetImportCore provider/handler registries, generic and specialized plan and
  execution APIs, async planning, import-record actions, and public headers.
- StandardAssetImport descriptors and module-owned registration lifetime.
- LevelEditor, TextureEditor, scene/direct import, tests, and other production
  callers that currently fetch or pass registries.
- Asset Import Framework, Asset Packages, Asset Data Lifecycle, and roadmap
  contracts affected by the consolidated service.

## Non-Goals

- Adding source formats, changing decoded asset data, or redesigning DAST/DDC.
- Replacing import records, mounted-source ownership, or AssetCore publication
  transactions.
- Introducing a general job system, remote importer execution, or plugin RPC.
- Combining runtime authoring-feature providers with editor importers.

## Stage 0: Freeze Registration And Execution Inventory

Dependencies: M1 and M2 complete.

- [x] Inventory every production and test registration in the provider,
  single-asset-handler, and import-record-handler registries.
- [x] Inventory initial import, single-asset reimport/repair, multi-output plan,
  record action, async plan, cancellation, and shutdown callers.
- [x] Characterize registration retirement, ambiguity, stale-plan, publication
  rollback, warning acceptance, output reconciliation, and restart behavior.
- [x] Assign each public type/function to the consolidated service, a retained
  value contract, or a deletion stage.

### Acceptance Gate

- Every production registration and execution entry has one named destination.
- Focused baseline tests cover all behaviors that cross a registry or lifetime
  boundary.

### Frozen inventory

StandardAssetImport currently performs eight coordinated registrations for
three logical importers:

| Logical importer | Provider registration | Additional registrations | Stage 1 descriptor |
| --- | --- | --- | --- |
| Scene | `Durin.Scene` | one `FSceneRecordHandler` | provider plus record capability |
| Geometry | identity-only `Assimp` | `DStaticMesh` single-asset handler | provider plus one asset-class capability |
| Image | identity-only `DurinImage` | `DTexture2D`, `DTextureCube`, and `DTerrainHeightmap` handlers | provider plus three asset-class capabilities |

`RegisterStandardAssetImportProviders` manually rolls back all three registries
and separately opens async admission for the same provider ids. Shutdown drains
by provider id, unregisters four class handlers and one record handler, then
unregisters three providers. The consolidated service owns that atomicity and
turns the table above into exactly three module-owned descriptor registrations.

Production execution callers divide into these destinations:

| Current surface | Production callers | Destination |
| --- | --- | --- |
| `CreateImportPlan` plus provider registry | AsyncImport and SceneImport | service `Plan` using source recognition or explicit importer id |
| single-asset capability/plan/execute/repair plus two registries | LevelEditor Content Browser, TextureEditor, direct texture/terrain tests | service single-asset operations using the descriptor selected by class and persisted provider id |
| multi-output plan/execute | SceneImport | service multi-output operations retaining import-record index and reconciliation values |
| record capability/action plus record-handler registry | LevelDocumentController and Content Browser | service record operations using the record's provider id |
| async provider admission/cancel/drain globals | AsyncImport and StandardAssetImport shutdown | descriptor registration and service-owned coordinator lifetime |

The generic plan, source snapshot, diagnostics, progress, preview, target
precondition, import-record state/index, reconciliation, prepared candidate,
and execution-result types remain value contracts. `FProviderRegistry`,
`FSingleAssetHandlerRegistry`, `FImportRecordHandlerRegistry`, their global
getters, caller-supplied registry parameters, and identity-only registration
orchestration are deletion targets across Stages 1-4.

Focused baseline passes: AssetImportCoreTests 27/27 covers provider ambiguity,
retirement, leases, async cancellation, planning limits, stale revisions, and
import-record transactions; AssetImportTests 17/17 covers publication rollback
and restart; SceneImportTests 15/15 covers initial/reimport reconciliation,
warnings, cancellation, and source closure; TextureTests passes 66 tests with
two existing platform skips and covers single-asset reimport and repair.

## Stage 1: Introduce One Import Service And Descriptor

Dependencies: Stage 0 complete.

- [x] Introduce one import-service owner for descriptor registration, storage,
  revision, and lease-backed lookup; Stage 4 moves async/publication admission.
- [x] Make one immutable importer descriptor carry source recognition/planning
  plus optional single-asset and import-record capabilities.
- [x] Register each StandardAssetImport importer once with its module-owned
  lifetime gate and fold the transitional identity providers into descriptors.
- [x] Preserve duplicate rejection, deterministic recognition, ambiguity,
  retirement, outstanding-lease, and module-shutdown behavior.

### Acceptance Gate

- Production modules perform one registration per logical importer.
- One service owns all registry storage and descriptor registration atomicity;
  the temporary registry getter bridge is removed as callers migrate in Stages
  2-4.

`FImportService` now owns provider, single-asset, and record capability storage.
`FImporterDescriptor` binds one provider to its optional class and record
capabilities and registers or rolls back that complete unit under one
module-owned gate. StandardAssetImport performs exactly three logical
registrations: Scene, Assimp geometry, and DurinImage. Failure and shutdown
unregister complete descriptors rather than sequencing eight registry entries.
The old global getters temporarily return the service's internal registries, so
there is no parallel registry state while later stages migrate callers.

AssetImportCoreTests passes 28/28 including a new descriptor atomicity and
duplicate/unregister case. SceneImportTests passes 15/15, and TextureTests
passes 66 tests with two existing platform skips, covering descriptor-backed
record and single-asset capabilities.

## Stage 2: Consolidate Planning And Single-Asset Execution

Dependencies: Stage 1 complete.

- [x] Route initial planning and single-asset reimport/repair through the
  service without caller-supplied registries.
- [x] Share source capture, provenance, capability, diagnostics, progress,
  stale-plan validation, and publication transaction vocabulary.
- [x] Remove superseded single-asset registry and duplicate facade APIs after
  all callers migrate.
- [x] Replace identity-only provider adapters with descriptor-owned source
  recognition/settings for the single-asset importers.
- [x] Preserve no-op reimport, new-source replacement, source repair, DDC/build
  behavior, and failure-atomic resident state.

### Acceptance Gate

- Initial and single-asset reimport paths resolve one descriptor through one
  service and publish through the same authoring boundary.
- No public single-asset registry remains.

Initial Scene planning and every production single-asset capability, reimport,
execute, and repair operation now enter through `FImportService`; callers no
longer fetch or pass provider/handler registries. Single-asset plans retain the
owning service and one service revision for stale-plan validation instead of
two mutable registry pointers and revisions. The single-asset registry is now a
private implementation type, its global getter and free orchestration facades
are gone, and StandardAssetImport's Assimp/image recognition and default
settings live declaratively in each descriptor rather than in identity-only
provider classes.

LevelEditor and TextureEditor build independently. AssetImportCoreTests passes
28/28, SceneImportTests 15/15, TerrainHeightmapTests 11/11, and TextureTests
passes 66 tests with two existing platform skips. These cover current/new
source reimport, no-op updates, source repair, descriptor retirement, stale
plans, DDC/build publication, save failure, and state restoration.

## Stage 3: Consolidate Multi-Output And Record Actions

Dependencies: Stage 2 complete.

- [x] Route multi-output plan/execution and import-record capabilities/actions
  through the same service and descriptor lease.
- [x] Share request/result, preview, diagnostics, progress, warning acceptance,
  stale-plan, and publication vocabulary where semantics are identical.
- [x] Remove the import-record-handler registry and duplicate provider lookup.
- [x] Preserve managed/detached output reconciliation, tombstones, record
  identity, relocation repair, orphan policy, and bundle compensation.

### Acceptance Gate

- Initial scene import and record reimport resolve the same registered
  descriptor and execute through one service-owned boundary.
- No public import-record-handler registry remains.

Scene multi-output planning/execution and editor import-record
capability/action calls now enter `FImportService`. The service resolves the
same descriptor's provider lease and record capability; callers no longer
perform a second handler lookup. The record-handler registry is a private
implementation type, and its getter plus the free multi-output/record
orchestration facades are removed. Common generic plan, preview, diagnostics,
progress, warning, stale-input, and atomic bundle publication values remain
shared; reconciliation and record-action results stay specialized because they
carry managed/detached output semantics absent from single-asset import.

LevelEditor builds with the new boundary. AssetImportCoreTests passes 28/28,
including import-record initial/reimport plans, warning acceptance, managed and
detached outputs, tombstones, stale revisions, every publication failure phase,
rollback, provider lease retirement, and restart. SceneImportTests passes 15/15
through the same service-backed record descriptor.

## Stage 4: Consolidate Async Lifetime And Migrate Callers

Dependencies: Stages 1-3 complete.

- [x] Make async planning, completion mailboxes, cancellation, and provider
  retirement use the service-owned descriptor admission/lease state.
- [x] Migrate LevelEditor, TextureEditor, StandardAssetImport, tests, and other
  callers away from registry getters and registry-parameter APIs.
- [x] Split oversized public headers by stable request/result/service concerns
  where that reduces dependency exposure without duplicating entry points.
- [x] Search for and remove retired registry, handler, identity-provider, and
  specialized orchestration symbols.

### Acceptance Gate

- Shutdown drains all admitted work before descriptor/provider destruction.
- Production callers use one service entry per import operation.

The provider registry joined the two handler registries in the private
implementation header. Production and tests register descriptors on an
`FImportService`; no registry getter or caller-supplied registry API remains.
`ImportService.h` now isolates the stable service/descriptor surface from the
larger request/result vocabulary in `AssetImportCore.h` and the reconciliation
values in `MultiOutputImport.h`.

Descriptor registration opens async admission. Service unregistration closes
that provider, cancels and drains admitted work, then retires handler/provider
storage, so a worker cannot race module teardown. Launch, handle cancellation,
owner/provider drain, global drain, and shutdown admission are service
operations; the mailbox/result observation functions remain handle-level value
operations. StandardAssetImport no longer sequences separate async admission
beside registration. Retired-symbol search finds no production registry getter,
public registry definition, identity-provider adapter, standalone
single/multi/record orchestration facade, or external admission function.

LevelEditor and TextureEditor build independently. AssetImportCoreTests passes
28/28 across local services, ambiguity, provider retirement, outstanding
leases, async equivalence/supersession/cancellation/rejection, and record
transactions. SceneImportTests passes 15/15 across async and synchronous scene
planning, record reimport, cancellation, and provider teardown.

## Stage 5: Qualify And Publish Import Ownership

Dependencies: Stages 0-4 complete.

- [x] Run focused AssetImportCore, import-record, single-asset, scene,
  mounted-source, editor workflow, restart, and failure-injection suites.
- [x] Run complete native qualification, default full build, and hidden-window
  editor smoke without concurrent build processes.
- [x] Update lasting import, package, lifecycle, workspace, and roadmap
  documentation with the implemented service boundary.
- [x] Run changed-document, all-plan, all-roadmap, and repository documentation
  validation and record evidence for the M3 exit gate.

### Acceptance Gate

- Every production importer registers once and all import modes use one
  service-owned plan/execution/publication boundary.
- Lasting documentation and validation evidence satisfy the M3 roadmap exit
  gate and leave M5 blocked only on M4.

Qualification passed on 2026-08-15. The default `all` build and complete native
target suite passed, and DurinEditor remained alive through an 8-second
hidden-window Sandbox smoke. The authored baseline reports 28/28 current DAST
v4 packages. Focused evidence includes AssetImportCoreTests 28/28,
AssetImportTests 17/17, SceneImportTests 15/15, TerrainHeightmapTests 11/11,
and TextureTests 66 with two existing platform skips. LevelEditor and
TextureEditor build independently. Retired-symbol search found no production
registry getter, public registry definition, identity-provider adapter,
standalone specialized orchestration facade, or external async admission API.
Changed-document validation passed for five lasting documents; all 172 plan
records, all 16 roadmaps, and all 113 active repository documents validated.

## Completion Criteria

- All stages and acceptance gates pass with evidence recorded here.
- Provider, single-asset-handler, and import-record-handler registries no longer
  coexist as public orchestration surfaces.
- Create, reimport, repair, record reconciliation, cancellation, retirement,
  and rollback behavior remains qualified.

## Related Documentation

- [Asset Architecture Simplification Roadmap](../Roadmaps/AssetArchitectureSimplification.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- [`AssetImportCore.h`](../../Engine/Source/Editor/AssetImportCore/Public/AssetImportCore.h)
- [`MultiOutputImport.h`](../../Engine/Source/Editor/AssetImportCore/Public/MultiOutputImport.h)
- [`AsyncImport.h`](../../Engine/Source/Editor/AssetImportCore/Public/AsyncImport.h)
- [`AssetImportCore.cpp`](../../Engine/Source/Editor/AssetImportCore/Private/AssetImportCore.cpp)
- [`MultiOutputImport.cpp`](../../Engine/Source/Editor/AssetImportCore/Private/MultiOutputImport.cpp)
- [`StandardAssetImportProviders.cpp`](../../Engine/Source/Editor/StandardAssetImport/Private/StandardAssetImportProviders.cpp)
- [`AssetImportCoreTests.cpp`](../../Engine/Tests/Native/AssetCoreTests/Private/AssetImportCoreTests.cpp)
- [`ImportRecordTests.cpp`](../../Engine/Tests/Native/AssetCoreTests/Private/ImportRecordTests.cpp)
