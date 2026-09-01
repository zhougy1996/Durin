# CoreDObject Engine Cutover Plan

Summary: Apply detached v8 linker tables to live object graphs and atomically replace Engine's v7 package reader/writer with the CoreDObject format.

Last reviewed: 2026-08-31

Status: Archived
Completed: 2026-08-31

- 2026-08-31: Stages 0 through 3 landed direct Archive-to-linker capture,
  detached linker application through the lifecycle transaction, an exact
  main/bulk codec context, canonical v8 save/load, and v8-only ordinary policy.
- 2026-08-31: Stage 4 removed the Engine v7 codec, live object-stream byte
  reader/writer entrypoints, v7 payload-directory wire codec, and the temporary
  capture dependency on the offline converter. Migration fixtures now build
  v7 envelopes only inside AssetRegistry native tests.
- 2026-08-31: Stage 5 qualified CoreDObject, Registry, offline migration,
  malformed-header, v8 codec mutation, ordinary save/load, and external
  BulkData behavior; Engine and the direct MaterialTests consumer compiled
  without launching an application. P5 is complete.

## Current Status

P0 through P4 are complete. CoreDObject owns canonical v8 read/write, Registry
projection, package indices, tagged values, and validation. The maintained
corpus is v8. Engine now selects only its v8 codec. Live Archive events become
linker tables with detached BulkData facts and no object-stream byte image, and
validated linker tables enter the unpublished-skeleton lifecycle transaction
without a package-wire parse. All P5 stages are complete; P6 owns the bounded
higher-level operation decisions below.

## Goal

Make CoreDObject linker tables the only production package serialization model.
Engine translates live reflected object graphs to detached linker tables for
save and applies validated linker tables to unpublished object skeletons for
load. The codec policy selects only v8; v7 live read/write, object-stream wire
interpretation, and duplicated recursive value walkers are removed after
parity and rollback tests pass.

## Scope

- Format-neutral Engine capture from `DPackage` and reflected Archive events to
  `ObjectPackage::FLinkerTables`, including delta/provenance, custom versions,
  imports/exports, references, containers, structs, and BulkData.
- Live application from fully validated linker tables to unpublished
  `DPackage`/`DObject` skeletons with dependency resolution, authored ledgers,
  custom-version context, PostLoad, publication, and complete rollback.
- A v8 Engine codec for header, validation, inspection/schema/reference
  projection, load, write, relocation/redirect needs retained through P6, and
  exact external bulk companion handling.
- Atomic policy switch to v8 for ordinary readers and writers, followed by
  deletion of Engine v7 codec/live wire code and obsolete tests/APIs.
- Focused native lifecycle, malformed-input, value-parity, dependency-cycle,
  BulkData, save/load identity, and failure-injection validation.

## Non-Goals

- Requalifying every higher-level relocation, deletion, Cook, fix-up, or editor
  workflow; P6 owns those operations after the core live path is stable.
- Keeping v7 live compatibility, a dual writer, a runtime migration-on-load
  branch, or Engine-owned DAST section/table parsing.
- Changing the v8 wire contract, Registry projection, maintained corpus, or
  package identity model established by P1 through P4.
- Launching an editor/game application or performing GPU work.

## Program Decisions

- CoreDObject validates complete main/bulk bytes before Engine sees a linker
  table. Engine never parses v8 offsets, ids, tags, hashes, or section bytes.
- Save capture and load application share reflected logical-type and Archive
  value adapters, but neither owns a second package wire model. All temporary
  compatibility adapters are private and deleted with the v7 codec.
- Application creates the complete Outer topology before resolving references;
  imports resolve through ordinary package loading only after the skeleton-ready
  callback. No object becomes ordinarily resident until values, ledgers,
  PostLoad, and package invariants succeed.
- BulkData enters/leaves linker values as detached bytes with explicit element
  size, alignment, and storage. Engine package-resource registration binds only
  the exact v8 external segment described by CoreDObject.
- Ordinary writer and supported-reader policy switch to v8 in the same commit
  that removes the v7 codec from the table. No mixed production policy lands.

## Implementation Stages

### Stage 0: Freeze live linker parity and ownership

- [x] Map every existing live v7 capture/application fact to linker tables and
  identify the smallest reusable reflection/Archive adapters.
- [x] Freeze skeleton, dependency, value, ledger, PostLoad, publish, rollback,
  BulkData, and diagnostic contracts with synthetic fixtures.
- [x] Freeze v8 codec capability ownership and explicitly disposition P6-only
  mutation/inspection operations.

#### Acceptance Gate

No live v7 fact is silently lost, no v8 wire fact enters Engine, and the atomic
cutover/delete boundary is reviewable before policy changes.

### Stage 1: Capture live packages into linker tables

- [x] Replace object-stream package capture with a detached linker-table builder.
- [x] Cover all reflected scalar/container/struct/reference kinds, canonical
  imports/exports, custom versions, provenance, redirects, and BulkData policy.
- [x] Emit ordinary saves only through `ObjectPackage::WritePackageV8` and prove
  deterministic repeated encoding plus unchanged output on failure.

#### Acceptance Gate

Live packages serialize to canonical v8 without producing a v7 object stream or
calling any Engine package-wire writer.

### Stage 2: Apply linker tables to live object graphs

- [x] Build unpublished package/object skeletons from exports and validate live
  class, Outer, main-export, and schema identities.
- [x] Resolve imports and recursively apply tagged values through reflection,
  preserving custom-version and authored-ledger semantics.
- [x] Run PostLoad then publish atomically; inject failure at every phase and
  prove complete graph/dependency/resource rollback.

#### Acceptance Gate

Canonical v8 fixtures load with full value parity and no publication leak;
malformed or incompatible linker tables fail before durable state changes.

### Stage 3: Install the v8 codec and switch production policy

- [x] Implement v8 codec header, validation, inspect/schema/reference, load,
  write, and external bulk binding without parsing wire data in Engine.
- [x] Switch ordinary writer and supported-reader policy to v8 atomically.
- [x] Update save/load, admission, package resource, and focused consumers for
  exact main/bulk closure behavior.

#### Acceptance Gate

Ordinary save and load use v8 end to end, maintained assets load in native tests,
and the codec table contains no production v7 reader or writer.

### Stage 4: Delete the v7 live implementation

- [x] Remove Engine v7 codec, object-stream live reader/writer, v7 bulk wire
  interpreter, duplicate recursive walkers, and obsolete public/test seams.
- [x] Retain only the bounded temporary offline converter below Engine until its
  P6/P7 disposition, with no runtime registration.
- [x] Prove repository searches find no Engine production v7 route or selected
  writer/reader policy.

#### Acceptance Gate

Engine owns reflection capture/application only; CoreDObject is the sole package
table/value/wire implementation and no v7 live compatibility branch remains.

### Stage 5: Publish P5 and hand off higher-level operations

- [x] Run focused CoreDObject, codec, lifecycle, save/load, malformed-input,
  dependency, BulkData, Registry, and maintained-corpus tests.
- [x] Compile Engine and direct asset/editor consumers without launching an
  application.
- [x] Publish lasting runtime/module contracts, complete the plan and P5, and
  define exact P6 operation dispositions.

#### Acceptance Gate

The v8 live core is the only production route, evidence is reproducible, and P6
can qualify or remove higher-level operations without reopening package format.

## P6 Handoff

| Operation | Required disposition |
| --- | --- |
| Relocation | Keep only if main/bulk source identity, destination identity, staging, rollback, and Registry publication pass on v8 closures. |
| Reference rewrite / redirector fix-up | Use the v8 linker mutation capability and exact package-level candidates; remove object-stream occurrence and payload walkers. |
| Deletion | Rebuild analysis on package-level Registry dependencies plus exact on-demand v8 inspection; delete legacy companion inference. |
| Inspection | Complete v8 recursive value/BulkData projection only for concrete consumers; retire compatibility and deprecated-route APIs. |
| Cook | Emit/read canonical v8 closures or remove unsupported legacy canonicalization and payload-directory variants. |
| Offline converter | Keep only in AssetRegistry/AssetMaintenance for maintained migration; remove at P7 if no supported corpus/input remains. |

Validation evidence on 2026-08-31: `CoreObjectTests` 85/85,
`PackageRegistryContractTests` 3/3, `PackageLinkerAdapterTests` 8/8,
`PackageMigrationTests` 3/3, and the focused AssetPackageTests malformed-header,
ordinary save/load, v8 codec/mutation, and external BulkData cases passed.
`Engine` and `MaterialTests` compiled successfully.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Capture | Every supported reflected kind maps to one deterministic linker value. |
| Application | Skeleton, dependency, value, ledger, PostLoad, and publication ordering are explicit. |
| Failure | Injected failures leave no package, object, resource, or output publication. |
| BulkData | Inline/external bytes, element size, alignment, and companion binding remain exact. |
| Policy | Reader/writer policy selects v8 only; no runtime v7 codec remains. |
| Ownership | Engine contains no package table/tag/section parser or writer. |
| Regression | Maintained v8 corpus and focused ordinary save/load tests pass. |

## Related Code and Documentation

- [CoreDObject package format](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageFormat.h)
- [Engine package operations](../../../../Engine/Source/Runtime/Engine/Private/Asset/AssetPackageOperations.cpp)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Core Object Package Linker roadmap](../../../Roadmaps/Archive/2026-08/CoreObjectPackageLinker.md)
- [Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Testing Workflow](../../../Agents/Testing.md)
