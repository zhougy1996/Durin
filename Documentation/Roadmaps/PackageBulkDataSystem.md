# Package Bulk Data System Roadmap

Summary: Replace purpose-specific bulk companions with field-level BulkData and one raw package bulk segment across authored and cooked assets.

Last reviewed: 2026-08-30

Status: Active
Completed:

## Current Status

Milestone 1 is complete. The
[Field-Level Bulk Data Foundation plan](../Plans/FieldLevelBulkDataFoundation.md)
landed field-level runtime/editor values, package-resource range access, DAST
v7 metadata, raw authored `.dbulk`, transactional v6/DABK resave, and authored
family migration. Its bounded qualification proves lazy metadata load,
exact-range access, resource retirement, publication recovery, and warm-DDC
source-read avoidance across the registered test matrix and application smoke.

Milestone 2 is complete through the
[Cooked Bulk Data Field Migration plan](../Plans/CookedBulkDataFieldMigration.md).
Every supported family now shares one PlatformData schema across DDC and Cook,
projects cooked bytes into lazy `FBulkData` fields, and loads through common
package-resource ranges without source/DDC fallback. New Cook output uses DAST
v7 plus an optional headerless raw segment; family descriptors, loaders, and
production DBLK publication are retired. The M3 entry gate is satisfied; Cook
scheduling, generic publication, incremental state, and store abstraction
remain outside M2.

The M4 compatibility inventory now contains 25 checked-in DAST v7 packages and
eight raw `.dbulk` companions, with no tracked `.dabulk` companions or unknown
legacy `CookedPayload` fields. The one 259-byte logical DBLK v2 regression
fixture generated from `AssetCoreTests/Data/CookedBulk/README.md` remains until
the compatibility decoder is retired in M4.

## Outcome

Every bulk payload is represented by a field-level `FBulkData` or
`FEditorBulkData` value whose serialized metadata belongs to its owning
`.dasset`. A loose `.dbulk` is a headerless package segment containing only
deterministically aligned payload ranges; the owning package is the sole
authority for layout, version, and interpretation. Authored and cooked packages
use the same segment representation, while a package-resource boundary permits
future archive or remote stores without exposing physical paths to assets.

## Scope

- Field-level runtime `FBulkData` metadata, residency, locking, unload/reload,
  range I/O, serialization, and copy/move ownership.
- Field-level `FEditorBulkData` with separate instance identity, content-derived
  payload identity, asynchronous immutable access, atomic update, and optional
  future virtualization.
- DAST package metadata for inline and external bulk fields plus one raw,
  headerless, stable-sibling `.dbulk` segment.
- Package-level segment size/hash binding, deterministic layout, atomic
  publication, recovery, inspection, relocation, deletion, and source-control
  closure.
- Transitional reads and canonical resave from DAST v6 DABK v2 `.dabulk` and
  cooked DBLK v2 `.dbulk` representations.
- Migration of authored canonical inputs, cooked platform payloads, asset-family
  loaders, Cook save projections, manifests, and tests.
- A package-resource interface that can later route the same logical segment to
  loose files, archives, install chunks, or a selected remote store.

## Non-Goals

- Making DDC a BulkData backend or storing DDC keys in `FBulkData`.
- Preserving `.dabulk` as a second long-term authored format after corpus
  migration.
- Giving a raw `.dbulk` an independent magic, header, entry table, schema,
  target, or self-description.
- Encoding texture, mesh, animation, Terrain, material, or other payload schema
  semantics in the package bulk layer.
- Building a general Pak/IoStore equivalent before loose package segments and
  field residency are measured and stable.
- Reworking import formats, source provenance, or asset-reference reachability
  except where their existing operations must consume the new field API.

## Program Decisions and Invariants

- `.dbulk` denotes the loose representation of a package bulk segment, not an
  authored or cooked authority class. Authored and cooked packages are
  distinguished by their package domain and root, not by different suffixes.
- A new `.dbulk` has no file header or directory. It contains only zero padding
  and payload ranges. Its owning DAST version and field metadata are required to
  interpret every byte.
- `FBulkData` is a field-level runtime/package-I/O value. It owns size, stored
  size, offset or package-resource location, storage flags, residency state,
  and optional allocation; it owns no content hash, payload GUID, asset schema,
  target platform, DDC key, or physical path.
- `FEditorBulkData` is a separate field-level authored value rather than a
  subclass or resident wrapper around `FBulkData`. It exposes asynchronous
  immutable payload access and atomic whole-payload replacement, not
  `Lock`/`Unlock` mutation.
- `FEditorBulkData` retains two distinct identities when required: an instance
  identifier for registration/ownership and a content-derived payload ID for
  equality, virtualization, and build-key input. The payload ID hashes canonical
  uncompressed bytes and excludes path, offset, compression, source timestamps,
  build settings, and Cook target.
- DDC remains an independent rebuildable service. A build key may include an
  editor payload ID, settings, producer version, and target, but no BulkData
  object resolves or owns a DDC entry.
- Because a stable raw segment cannot identify itself, every DAST package with
  external bulk stores the exact segment byte extent and a whole-segment digest.
  Load, inspection, recovery, and publication reject a mismatched package/segment
  pair before payload publication.
- External field metadata records bounded offset, logical size, stored size,
  alignment, and storage flags. Offsets are relative to the start of the
  selected package segment and never encode an operating-system path.
- Writers assign ranges in one canonical object/field order, use checked
  arithmetic, write zero padding, and produce identical bytes for identical
  captured values and policy. Initial authored output remains uncompressed and
  retains the current externalization threshold unless the active plan records
  measured evidence for a change.
- A package-resource manager, not an asset type, resolves package segments and
  serves synchronous or asynchronous bounded range reads. Loose files are its
  first backend, not a permanent runtime assumption.
- Save and Cook capture immutable owned payload inputs before worker encoding.
  Offset assignment and package serialization never mutate live reflected
  objects or package dirty state.
- Loose publication treats `.dasset` and required `.dbulk` as one recoverable
  consistency unit: publish the segment first, publish the referencing package
  second, publish catalog/manifest state last, and retain enough prior state to
  restore either generation on failure.
- DAST v7 is the first writer for the new field metadata. Legacy readers are a
  bounded migration facility; no workflow dual-writes DABK v2 and the raw
  segment for the same package generation.
- No production asset family migrates until construct-free range validation,
  lazy source lifetime, package unload, copy/move, cancellation, and failed
  publication retain deterministic terminal behavior.

## Foundations and Remaining Gaps

### Landed foundations

- A reflected `BulkData` property kind with custom serialize and identical
  operations.
- Field-level `FBulkData` and independent `FEditorBulkData` values with explicit
  residency, immutable asynchronous access, and content identity.
- DAST v7 logical object streams, bounded field inspection, stable package
  identity, canonical save ordering, and raw authored segments.
- Shared immutable byte buffers, checked archives, atomic file publication, and
  package bundle rollback.
- Package-resource range reads and authored raw-segment validation with strict
  range, alignment, extent, and digest binding.
- Companion-aware package move, delete, inspection, backup recovery, catalog
  publication, Cook manifest cleanup, and source-control policy.
- Family-owned canonical authored data and detached Build/DDC products for
  texture, static mesh, skeletal, animation, and Terrain assets.
- Read-only DAST v6/DABK compatibility and transactional canonical resave.

### Program gaps

- Cook scheduling and publication remain family/project coordinated rather
  than one generic captured save-plan and output-store transaction.
- The checked-in v6/DABK corpus has not yet been resaved and retired.
- No archive/install-chunk backend consumes the same package-segment contract.

## Milestone Map

| Milestone | State | Dependencies | Deliverable | Entry gate | Exit gate |
| --- | --- | --- | --- | --- | --- |
| 1. Field-level authored foundation | Complete | Existing DAST v6, package transaction, and BulkData property foundations | Field-level `FBulkData`/`FEditorBulkData`, package-resource API, DAST v7 field metadata, raw authored `.dbulk`, v6 compatibility, and authored-family migration | Current DABK/DBLK contracts and affected asset families are documented and covered by native tests | Passed: authored assets save and metadata-load through DAST v7/raw `.dbulk`; DDC hits use editor payload identity without reading bytes; v6 packages resave transactionally; no new `.dabulk` is written |
| 2. Cooked field and loader migration | Complete | M1 | Coherent editor payload snapshots, one family PlatformData schema across DDC/Cook, and runtime `FBulkData` fields replace cooked descriptors and DBLK v2 containers | Passed: M1 package-resource lifetime, raw segment binding, failure policy, and warm-DDC no-read behavior are qualified | Passed: every supported cooked asset loads lazily from raw `.dbulk` without source/DDC or family-owned physical-path resolution; old DBLK remains only in the selected decoder/fixture window |
| 3. Cook bulk publication integration | Proposed | M2; project-level Cook orchestration selected separately | Generic Cook save plans contribute field payloads to one package-segment writer and manifest/store boundary | All cooked families express runtime bytes through `FBulkData` and immutable save overrides | Cook produces deterministic package/segment pairs, distinguishes DDC and Cook hits, publishes atomically, cleans stale outputs from manifests, and never mutates authored objects |
| 4. Legacy retirement and corpus qualification | Proposed | M1-M3 | Repository corpus migration, old writer/reader removal, lasting contract updates, and storage inventory cleanup | All checked-in and fixture packages have a supported canonical resave path | `.dabulk`, DABK v2, structured DBLK v2, legacy storage APIs, and temporary adapters are removed; repository and runtime qualification use only the new model |
| 5. Scalable package stores | Evidence-gated | M3-M4; measured loose-file and streaming workloads | Optional/memory-mapped segments, archive/install-chunk routing, or remote editor payload virtualization selected from evidence | Profiles identify file-count, latency, memory, patch, or collaboration limits not met by loose segments | The selected backend preserves field semantics, package identity, failure policy, determinism, and bounded resource lifetime without asset-level path knowledge |

## Child Plan Boundaries

| Proposed or active plan | Milestone | Boundary | Activation |
| --- | --- | --- | --- |
| [Field-Level Bulk Data Foundation](../Plans/FieldLevelBulkDataFoundation.md) | M1 | Core field semantics, Archive/package-resource boundary, DAST v7 raw authored segment, compatibility, and authored asset migration | Complete |
| [Cooked Bulk Data Field Migration](../Plans/CookedBulkDataFieldMigration.md) | M2 | Publish coherent `FEditorBulkData` snapshots; unify DDC/Cook PlatformData schemas; replace cooked descriptors, family loaders, and DBLK v2 with runtime `FBulkData`; excludes Cook scheduler, generic publication, and archive storage | Complete |
| Cook Package Segment Publication | M3 | Generic Cook capture, layout, output-store, manifest, and incremental publication; excludes authored migration | M2 entry evidence passed; create after project-level Cook orchestration selection |
| Bulk Data Legacy Retirement | M4 | Corpus resave, compatibility deletion, fixture cleanup, and lasting documentation | Create only when all production families have migrated |
| Scalable Package Bulk Stores | M5 | One evidence-selected archive, optional, memory-mapped, or remote backend | Create only from measured post-M4 evidence |

## Program Validation Matrix

| Area | Required evidence |
| --- | --- |
| Field semantics | Default, copy, move, lock state, resize, resident/unloaded transition, async range completion, cancellation, owner retirement, and no hash/path/DDC state in runtime `FBulkData` |
| Editor semantics | Content-derived payload identity, instance identity separation, asynchronous immutable access, atomic update, copy snapshot behavior, DDC-key use without payload read, and failed retrieval diagnostics |
| Wire format | DAST v7 golden bytes, inline/external fields, canonical range order, zero padding, bounds/overflow/alignment rejection, exact segment extent/hash binding, and no raw-segment header or directory |
| Package lifecycle | Save, bundle save, crash-boundary failure injection, rollback, backup recovery, inspection, move, delete, duplicate, unload, source-control closure, and catalog publication |
| Compatibility | DAST v6/DABK v2 and old cooked DBLK v2 reads, canonical resave, no dual write, stale companion cleanup, and explicit unsupported-version behavior after retirement |
| Cook/runtime | Deterministic output, source/DDC-free cooked load, lazy range I/O, family schema validation after byte retrieval, missing/corrupt/mismatched segment failure, and prior-generation retention |
| Scale | Peak resident bytes, range-read count/bytes, open resource handles, package load latency, DDC-hit source-read avoidance, Cook throughput, and shutdown conservation |

Validation target selection and execution follow the repository
[build and run](../Agents/BuildAndRun.md) and
[testing](../Agents/Testing.md) workflows. Each child plan owns its exact
targets, fixtures, failure injection, budgets, and evidence.

## Risks and Control Gates

- **Cross-file ambiguity:** a headerless stable sibling cannot reject a mixed
  generation by itself. M1 cannot publish external fields until the package
  summary binds exact segment extent and digest and recovery tests cover every
  publication boundary.
- **Hidden eager loads:** existing asset accessors return resident spans.
  Migration must make every payload read explicit and prove that package load
  and DDC-hit Cook do not pull authored ranges accidentally.
- **Lifetime escape:** lazy fields may outlive their package resource or module.
  Resource handles, copies, async requests, unload, and shutdown require one
  counted ownership protocol before family migration.
- **Live-object mutation:** offset and storage flags are save products rather
  than authored state. Archive capture and save overrides must own them without
  writing back into loaded assets.
- **Identity conflation:** runtime field metadata, editor content identity,
  package segment integrity, and DDC keys solve different problems. No child
  plan may reuse one identity as another without an explicit versioned
  derivation.
- **Compatibility permanence:** a permissive v6 reader can become a permanent
  second architecture. M4 is required and its entry inventory must name every
  remaining old package and fixture.
- **Premature storage generalization:** archive, memory mapping, optional
  payloads, and remote virtualization remain evidence-gated until the loose
  segment establishes measured access patterns.

## Completion Criteria

- All required milestones M1-M4 have passed their exit gates; M5 is either
  complete or explicitly dispositioned from measured evidence.
- Authored and cooked packages use field-level BulkData and one headerless raw
  package segment representation without asset-level physical paths.
- `.dabulk`, DABK v2, structured DBLK v2, family-specific cooked companion
  resolution, and migration-only adapters are absent from active production
  paths.
- DDC remains an independent build cache, while editor payload IDs permit
  build-key lookup without unnecessary payload I/O.
- Package save, Cook, load, streaming, mutation, failure recovery, unload, and
  shutdown have bounded diagnostics and acceptance evidence.
- Lasting serialization, package, asset-lifecycle, and Cook contracts have
  moved to their owning Runtime documentation and all child plans retain
  completion provenance.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [File I/O](../Runtime/Core/FileIO.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Workspace and Projects](../Workspace/WorkspaceProjects.md)
- [Agent Build and Run Workflow](../Agents/BuildAndRun.md)
- [Agent Testing Workflow](../Agents/Testing.md)

## Related Code

- `Engine/Source/Runtime/Core/Public/Serialization`
- `Engine/Source/Runtime/CoreDObject/Public/DObject`
- `Engine/Source/Runtime/AssetRegistry`
- `Engine/Source/Runtime/Engine/Public/Asset`
- `Engine/Source/Runtime/Engine/Private/Asset`
- `Engine/Source/Developer/DerivedDataCache`
- `Engine/Source/Developer/TextureBuild`
- `Engine/Source/Developer/StaticMeshBuild`
- `Engine/Source/Developer/SkeletalBuild`
- `Engine/Source/Developer/TerrainBuild`
- `Engine/Tests/Native/AssetCoreTests`
- `Engine/Tests/Native/EngineTests`
