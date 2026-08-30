# Field-Level Bulk Data Foundation Plan

Summary: Establish field-level BulkData semantics, package-resource access, and the headerless raw `.dbulk` segment contract.

Last reviewed: 2026-08-30

Status: Completed
Completed: 2026-08-30

## Current Status

All five stages are complete. `FBulkData`, `FEditorBulkData`, Archive
serialization, package resources, DAST v7/raw `.dbulk`, transactional v6/DABK
resave, construct-free tooling, and all authored bulk-owning families now use
the selected contracts. Warm derived-data hits avoid source-range reads; cold
paths own one immutable snapshot. Resource retirement is conserved across
temporary canonical-resave loads, package unload, and runtime shutdown.

The bounded 4 MiB qualification fixture measured 9.8 ms metadata load with
zero field residency and zero range reads, 18.5 ms first access through one
4 MiB read, 171.0 ms canonical v7 save, and 230.9 ms v6-to-v7 resave at
17.3 MiB/s on macOS arm64 Debug. Enforced ceilings are 500 ms for metadata and
first access, 2 seconds for save, and 4 seconds for resave, with a 0.25 MiB/s
resave floor. The complete registered targets pass: `CoreUtilityTests` 91/91,
`CoreObjectTests` 85/85, `AssetPackageTests` 133/133, `TextureTests` 78/78,
`StaticMeshTests` 74/74, `SkeletalAssetTests` 35/35,
`TerrainHeightmapTests` 11/11, and the LevelEditor target build. The user-run
macOS application smoke also passed. Asset package failure injection passed;
no sanitizer mode is registered by the owning targets.

The checked-in compatibility inventory is finite: 25 `.dasset` packages are
still DAST v6 and nine tracked `.dabulk` companions remain. M4 may remove v6,
DABK, and their fixtures only after all 25 packages are canonically resaved to
v7 and the tracked `.dabulk` count reaches zero; structured cooked DBLK v2 is
removed only after M2 migration. M2 can now activate using the qualified field
lifetime/resource binding, raw segment publication/recovery, lazy metadata and
range-I/O behavior, authored-family migration, and warm-DDC no-read evidence.

This plan is the active M1 child of the
[Package Bulk Data System roadmap](../Roadmaps/PackageBulkDataSystem.md). It
selects one breaking DAST v7 write path and one compatibility read/resave path
rather than maintaining two authored writers.

## Goal

Land the field-level runtime and editor BulkData contracts, a package-resource
range-I/O boundary, and DAST v7 authored package support whose external payloads
occupy one deterministic headerless `.dbulk` segment. Migrate every current
authored `FEditorBulkData` asset field and provide a transactional DAST
v6/DABK-to-v7 resave path, while leaving cooked asset-field conversion to the
next roadmap milestone.

## Scope

- Replace resident identity-oriented `FBulkData` with a field-level value that
  owns bounded metadata, optional allocation, lock state, and a package-resource
  source without a content hash, payload GUID, DDC key, schema, target, or path.
- Replace the current `FEditorBulkData` wrapper with a separate field-level
  authored value that owns instance identity, content-derived payload identity,
  size, source location, optional immutable memory, asynchronous `GetPayload`,
  and atomic `UpdatePayload`.
- Replace `FArchiveBulkDataTransfer` with an Archive BulkData operation and
  explicit serialization parameters while retaining reflection's `BulkData`
  field kind and deterministic identical/default behavior.
- Add the first loose `FPackageResourceManager` backend for bounded `.dasset`
  and `.dbulk` segment reads without exposing physical paths through a field.
- Define and implement DAST v7 inline/external field metadata, package-level
  segment extent/digest binding, deterministic raw segment layout, inspection,
  and atomic publication/recovery.
- Migrate Texture2D, TextureCube, VolumeTexture, StaticMesh, SkeletalMesh,
  AnimationClip, and TerrainHeightmap authored canonical fields and their
  importer/builder accessors to the new editor payload API.
- Preserve bounded DAST v6/DABK v2 reading only for load and canonical resave;
  stop all new `.dabulk` writes and remove migrated stable `.dabulk` files only
  after the replacement package/segment/catalog transaction commits.
- Update focused native tests and lasting package/data-lifecycle contracts for
  the implemented authored behavior.

## Non-Goals

- Converting family-specific cooked descriptors or structured DBLK v2 cooked
  companions to runtime `FBulkData`; the roadmap's M2 plan owns that migration.
- Creating a project-level Cook scheduler, incremental Cook database, archive,
  install chunks, remote Cook store, or Cook On The Fly service.
- Treating DDC as a BulkData source, storing a DDC key in either BulkData type,
  or changing family-owned Build recipes and value codecs beyond their input
  access adaptation.
- Implementing editor payload virtualization or a remote content-addressed
  store; this plan lands the content/source seam and measures it.
- Adding optional or memory-mapped segments, per-field compression, shared
  physical-range deduplication, or generation-named companions.
- Retaining a standalone header, directory, target, schema, payload hash table,
  or magic inside the new raw `.dbulk`.
- Changing imported canonical schemas, source provenance, or asset-reference
  semantics.

## Design Decisions and Invariants

- DAST v7 is the sole authored writer after this plan. DAST v6 remains readable
  only to support existing assets and explicit canonical resave.
- The new stable companion suffix is `.dbulk`. Its bytes are a package segment,
  not a self-describing file format. The segment contains only field payloads
  and zero alignment padding.
- The owning DAST package records exact raw segment extent and one XXH3-128
  whole-segment digest. This digest binds the stable sibling generation but is
  not exposed through `FBulkData` or used as editor payload identity.
- External field metadata records storage flags, logical size, stored size,
  segment-relative offset, and alignment. Authored M1 payloads are
  uncompressed, so logical and stored sizes are equal; the wire contract
  reserves versioned storage flags rather than inventing compression behavior.
- The existing 256 KiB inline/external threshold and 16-byte external alignment
  remain unchanged in M1. A later policy change requires measured package-count,
  load, and source-control evidence.
- Writer order is the canonical frozen object order followed by canonical
  reflected field order. Offset arithmetic is checked, each payload range is
  unique and non-overlapping, padding is zero, and exact segment extent is the
  end of the final aligned payload without undeclared trailing bytes.
- `FBulkData` is non-semantic package data. Its serialized/storage metadata is a
  save/load product and must not enter asset dirty-state comparison as authored
  content. Resident byte equality is not a general reflected-property identity.
- `FBulkData` read-only locks may load a detached range; write locks require a
  resident detached allocation and explicit resize rules. Unlock, unload,
  copies, moves, async reads, package unload, and shutdown have one checked state
  machine selected in Stage 0 before public implementation.
- A field stores a logical package-resource handle plus segment and range, never
  a filesystem path. The loose backend resolves the physical sibling from the
  current package runtime domain.
- `FEditorBulkData` does not inherit from or wrap `FBulkData`. It has no
  `Lock`/`Unlock`; `GetPayload` returns an asynchronously completed immutable
  owned/shared buffer and `UpdatePayload` replaces the entire content
  transactionally.
- Editor instance identity and content identity are distinct. The content ID is
  computed from canonical uncompressed payload bytes with a versioned hash
  algorithm selected and frozen in Stage 0; it excludes package/object path,
  field route, offset, compression, source metadata, settings, and target.
- Identical editor payload values compare by payload ID and byte size without
  forcing a load. Copies retain an immutable snapshot of identity/source or
  memory and are unaffected by later updates to the original.
- Build recipes may use editor payload ID as one canonical build-key input and
  request bytes only after a DDC miss. No builder receives a physical segment
  path or holds a package-resource callback beyond its admitted operation.
- Package load validates the DAST summary and every declared external range
  against the raw segment before object publication, but does not eagerly
  allocate payload bytes. Whole-segment digest validation policy and its I/O
  budget are frozen in Stage 0 and must still reject mixed generations before a
  field read can succeed.
- Package/segment save remains a recoverable transaction: stage both, preserve
  the prior stable segment when present, publish segment then package, publish
  catalog last, verify the committed pair, then retire backup and old
  `.dabulk`. Failure restores the prior complete generation.
- Construct-free inspection, relocation, deletion, duplicate, source-control
  closure, and orphan detection derive the companion from DAST v7 segment
  metadata. Internal staging/backup files are never reported as authored
  payloads.
- Save and resave assign offsets in captured values and writer-owned metadata;
  they never write storage offsets, handles, flags, or residency back into live
  asset fields.

## Initial Foundations and Gaps

Reflection already registers custom BulkData property serialization and
identical functions. Core archives already expose a BulkData policy, DAST v6
already has a logical BulkData opcode, and the package writer already performs
discovery/capture passes that can assign a deterministic segment layout without
mutating the object graph. Core file I/O and package mutation already provide
atomic publication, backup recovery, bundle rollback, catalog admission, and
failure injection.

At plan activation, the missing foundation was a resource-backed field state
machine. Archive transfer required verified resident bytes, package load read
the complete DABK before graph publication, `FEditorBulkData` accessors returned
immediate spans, and inspection, mutation, inventory, tests, and source-control
rules assumed a DABK header and payload directory. The completed stages moved
those consumers together.

## Implementation Stages

### Stage 0: Freeze field, wire, resource, and migration contracts

- [x] Specify the `FBulkData` state machine for empty, unloaded attached,
  loading, resident unlocked, read-locked, write-locked, detached, failed, and
  retired states, including legal copy/move/unload/reload transitions and
  exactly-once async completion.
- [x] Specify `FEditorBulkData` instance identity, versioned content-ID
  algorithm and encoding, empty-payload identity, copy snapshot, memory-only,
  package-backed, update, async retrieval, cancellation, and failure semantics.
- [x] Freeze DAST v7 BulkData field bytes, storage flags, relative-offset base,
  size/alignment limits, package segment summary, canonical field ordering,
  zero-padding rule, and maximum package/segment/range counts.
- [x] Select and document when whole-segment digest validation occurs during
  live load, construct-free inspection, range access, and canonical resave, with
  explicit maximum I/O and allocation budgets.
- [x] Freeze the package-resource handle lifetime, async request ownership,
  package unload blocking/cancellation, module shutdown, and loose-backend
  failure vocabulary.
- [x] Define the DAST v6/DABK v2 compatibility and transaction sequence,
  including old/new sibling conflicts, backup recovery, Git/LFS partial state,
  canonical resave, stale `.dabulk` removal, and rollback.
- [x] Add golden metadata fixtures and table-driven invalid cases before the
  production writer is enabled.

#### Acceptance Gate

- One reviewed contract gives every field state and wire word exactly one
  meaning; mixed-generation detection, lifetime, compatibility, and migration
  have no unresolved alternatives; golden fixtures reject header bytes,
  overflow, overlap, nonzero padding, invalid flags, extent/digest mismatch,
  and unsupported versions deterministically.

### Stage 1: Implement field values, Archive boundary, and package resources

- [x] Replace `FBulkDataDescriptor`/verified-resident construction with bounded
  field metadata, optional allocation, checked lock/unlock/resize/unload, and an
  attached package-resource range that contains no hash, GUID, DDC key, schema,
  target, or path.
- [x] Replace `FArchiveBulkDataTransfer` with Archive serialization of a
  `FBulkData` value plus explicit owner, element size, alignment, storage-policy,
  and Cook-index parameters; update default/delta/reflection adapters without
  changing unrelated property kinds.
- [x] Add the package-resource interface and loose backend for bounded sync and
  async segment range reads, request cancellation, package retirement, and
  shutdown conservation.
- [x] Rebuild `FEditorBulkData` as the separate content-addressed asynchronous
  field value selected in Stage 0, including atomic update, copy snapshot,
  memory-only payloads, package registration, and owned result buffers.
- [x] Add focused native tests for state transitions, illegal locks, copy/move,
  detached mutation, async success/failure/cancel, unloaded reload, retired
  resources, instance/content identity separation, and DDC-independent types.

#### Acceptance Gate

- Field values and package-resource requests pass their complete state/lifetime
  matrix without a physical path or DDC dependency; `FEditorBulkData` can expose
  a content ID without loading bytes and every admitted async request reaches
  exactly one terminal result before owner retirement.

### Stage 2: Emit and load DAST v7 with a raw authored bulk segment

- [x] Add DAST v7 logical BulkData encoding and construct-free inspection for
  inline and external fields plus package-level segment extent/digest metadata.
- [x] Implement deterministic capture, alignment, checked offset assignment,
  raw byte emission, zero padding, and exact extent/digest calculation without
  a segment header or directory and without mutating live field state.
- [x] Publish `.dbulk` and `.dasset` as one failure-injected transaction with
  companion-first ordering, prior-generation backup, rollback, verification,
  catalog-last publication, and no empty segment file.
- [x] Validate package summary, stable sibling, whole-segment binding, all
  declared ranges, overlap, padding, limits, and storage flags before live graph
  publication while leaving external payload allocations unloaded.
- [x] Convert inspection, inventory, orphan discovery, relocation, deletion,
  duplicate, and bundle-save closure to the DAST v7 raw segment while retaining
  explicitly routed v6/DABK behavior.
- [x] Add golden round trips and failure-injection tests for inline-only,
  single/multiple external fields, zero-length payload, boundary threshold,
  deterministic repeated save, corruption, truncation, mixed generations,
  backup recovery, move/delete, and package unload.

#### Acceptance Gate

- A test package with multiple field-level values saves to deterministic DAST
  v7 plus a headerless raw `.dbulk`, metadata-loads without payload allocation,
  serves exact ranges through the package-resource API, rejects every malformed
  or mixed pair before access, and preserves or restores the prior complete
  generation at every injected publication failure.

### Stage 3: Migrate authored asset families and retire `.dabulk` writes

- [x] Adapt Texture2D, TextureCube, VolumeTexture, StaticMesh, SkeletalMesh,
  AnimationClip, and TerrainHeightmap authored fields, accessors, importers,
  reimporters, builders, compilation domains, and save-readiness checks to
  asynchronous immutable editor payload access and atomic update.
- [x] Make family Build definitions derive keys from the editor content ID and
  prove a validated DDC hit does not read the package bulk range; a miss reads
  and owns exactly one immutable payload snapshot before worker execution.
- [x] Retain v6/DABK reading for old assets and implement canonical resave that
  writes only DAST v7/raw `.dbulk`, verifies the new package closure, publishes
  catalog state, and then removes the stable `.dabulk` without exposing an
  intermediate missing-authority state.
- [x] Update source-control closure, storage inventory, compatibility reports,
  asset mutation, and fixtures to distinguish legacy readable `.dabulk` from
  canonical `.dbulk` and to diagnose conflicting or orphaned siblings.
- [x] Add representative warm-DDC, cold-DDC, import, reimport, save/reload,
  duplicate, move, delete, cancellation, failed save, unload, and shutdown tests
  for every migrated family.

#### Acceptance Gate

- Every current authored canonical payload uses field-level
  `FEditorBulkData`; new saves emit no `.dabulk`; warm DDC paths avoid source
  range reads; old packages resave without payload or identity drift; and each
  family survives its authoring, mutation, failure, unload, and shutdown matrix.

### Stage 4: Qualify the foundation and publish lasting contracts

- [x] Measure metadata-only package load, first payload access, warm/cold DDC
  source reads, resident bytes, open resource handles, segment bytes, save
  latency, and canonical resave throughput against explicit bounded fixtures.
- [x] Run the smallest registered Core, CoreDObject, AssetCore, Engine asset-
  family, package compatibility, and application smoke targets selected through
  repository guidance; include sanitizer/failure-injection modes where the
  owning target supports them.
- [x] Update Asset Packages, Asset Data Lifecycle, Serialization, catalog/
  mutation, file I/O, source-control, and relevant family contracts to own the
  implemented DAST v7, field lifetime, raw segment, and migration behavior.
- [x] Record remaining old-package/fixture inventory, compatibility removal
  gates, measured budgets, and the exact M2 cooked-migration entry evidence in
  this plan and the roadmap.

#### Acceptance Gate

- The authored foundation is the documented and measured production path;
  every required target passes; no new `.dabulk` writer or eager family
  accessor remains; compatibility inventory is finite; and the roadmap has
  sufficient evidence to activate the cooked field migration without reopening
  M1 field, wire, or lifetime decisions.

## Validation Matrix

| Area | Evidence |
| --- | --- |
| Runtime field | Empty/resident/unloaded/detached/retired states, legal and illegal locks, resize, copy/move, range reload, cancellation, and terminal conservation |
| Editor field | Instance/content identity, empty/update/copy snapshot, async memory/package retrieval, failed source, no lock API, and content ID without payload load |
| Archive/reflection | BulkData property registration, serialize parameters, default/delta behavior, object freeze, immutable save capture, and no unrelated property-wire changes |
| DAST v7/raw segment | Golden bytes, inline/external threshold, canonical order, alignment, zero padding, extent/digest, bounds/overlap/overflow/truncation/mixed-generation rejection, and no header/directory |
| Package resource | Sync/async ranges, missing/truncated file, resource retirement, package unload, module shutdown, bounded handles/bytes, and no asset-level paths |
| Publication/mutation | Single/bundle save, every failure phase, rollback/recovery, inspection, inventory, duplicate, move, delete, orphan/conflict reporting, catalog-last commit, and source-control closure |
| Compatibility | DAST v6/DABK read, canonical resave, identity/content parity, old/new sibling conflict, stale `.dabulk` cleanup, no dual write, and finite inventory |
| Asset families | Import/reimport, warm/cold DDC, compile/build cancellation, save/reload, mutation, failed persistence, unload, and shutdown for every current authored bulk owner |
| Budgets | Metadata load and first-access latency, DDC-hit source reads, resident/segment bytes, handles, save/resave throughput, and bounded failure diagnostics |

## Definition of Done

- `FBulkData` and `FEditorBulkData` implement the selected field-level semantics
  and no longer share the current descriptor/resident-wrapper model.
- DAST v7 plus a headerless raw `.dbulk` is the only authored write path, with
  deterministic ranges and package-level extent/digest binding.
- Every current authored bulk asset family uses asynchronous immutable editor
  payload access and can obtain a build-key identity without loading payload
  bytes.
- Existing DAST v6/DABK assets have a tested transactional canonical resave;
  compatibility is read-only, explicitly inventoried, and writes no
  `.dabulk`.
- Package-resource, publication, mutation, failure, unload, and shutdown
  behavior passes the validation matrix within recorded budgets.
- Lasting implemented contracts reside in their owning Runtime and Development
  documents, and the Package Bulk Data System roadmap records M1 completion and
  M2 entry evidence.

## Deferred Follow-ups

- The roadmap's M2 plan migrates cooked runtime fields, asset-family loaders,
  and structured DBLK v2 companions to raw package segments.
- M3 integrates generic Cook capture/publication and incremental output-store
  behavior after all cooked families use `FBulkData`.
- M4 removes legacy readers, fixtures, and APIs after the complete repository
  corpus is migrated.
- Optional/memory-mapped segments, archive/install-chunk stores, remote editor
  payload virtualization, and physical deduplication remain evidence-gated M5
  work.

## Related Documentation

- [Package Bulk Data System Roadmap](../Roadmaps/PackageBulkDataSystem.md)
- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Content Version Control](../Development/VersionControl/ContentVersionControl.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization/Archive.h`
- `Engine/Source/Runtime/Core/Private/Serialization/Archive.cpp`
- `Engine/Source/Runtime/CoreDObject/Public/DObject`
- `Engine/Source/Runtime/CoreDObject/Private/DObject`
- `Engine/Source/Runtime/Engine/Public/Asset/BulkData.h`
- `Engine/Source/Runtime/Engine/Public/Asset/EditorBulkData.h`
- `Engine/Source/Runtime/Engine/Public/Asset/EditorBulkDataStorage.h`
- `Engine/Source/Runtime/Engine/Public/Asset/PackageSerialization.h`
- `Engine/Source/Runtime/Engine/Private/Asset`
- `Engine/Source/Developer/TextureBuild`
- `Engine/Source/Developer/StaticMeshBuild`
- `Engine/Source/Developer/SkeletalBuild`
- `Engine/Source/Developer/TerrainBuild`
- `Engine/Source/Editor/AssetForgeBuiltins`
- `Engine/Source/Programs/DurinAssetTool`
- `Engine/Tests/Native/AssetCoreTests/Private/BulkDataTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageTests.cpp`
- `Engine/Tests/Native/AssetCoreTests/Private/PackageV6Tests.cpp`
- `Engine/Tests/Native/EngineTests`
