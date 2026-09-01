# Package-Level Asset Registry Plan

Summary: Replace persistent reference occurrences and export-payload scans with header-resident package dependency metadata.

Last reviewed: 2026-08-31

Status: Archived
Completed: 2026-08-31

## Current Status

All stages are complete. AssetRegistry dispatches v8 front matter through
CoreDObject's Registry projection, retains a bounded v7 header fallback, and
stores hard, soft, redirect, searchable, object-count, and bulk facts in one
catalog cache. Package edges are deterministically derived and atomically
validated against `FAssetData`; ordinary cold, warm, corrupt-cache recovery,
and full-validation scans never read export/value payloads. The occurrence
cache, payload-refresh loop, associated warning/stat APIs, and public
AssetRegistry occurrence vocabulary are removed. Exact route records now live
only behind Engine's explicit inspection capability. Focused Registry and
publication tests pass, and AssetRegistry, Engine, and ContentBrowser compile.
P4 owns bounded v7 conversion and maintained-corpus migration.

## Goal

Make ordinary AssetRegistry discovery, cache reuse, dependency queries, and
publication package-level operations. A scan reads only bounded front matter,
stores selected hard/soft/searchable categories, and never interprets an export
value. Exact object/property/container occurrences are collected only by
explicit Engine tooling that opens a candidate package on demand.

## Scope

- Package-level hard, soft, redirect, and searchable-name metadata in catalog,
  snapshot, publication, reference-query, and cache contracts.
- v8 front-header dispatch through CoreDObject's Registry projection, with a
  temporary bounded v7 header fallback until P4 converts the maintained corpus.
- One catalog cache containing all persistent package metadata; removal of the
  occurrence-route reference cache and full-payload refresh pass.
- Package dependency projections derived entirely from catalog entries, with
  deterministic deduplication, revision consistency, and atomic publication.
- Separation of exact occurrence records/extraction into the Engine tooling
  capability used only by relocation, deletion, redirector fix-up, and editing.
- Focused AssetRegistry tests plus compile-only Engine/editor consumers; no
  Engine runtime execution.

## Non-Goals

- Converting repository assets, deleting the bounded v7 header decoder, or
  changing the production writer/reader route; P4 and P5 own those transitions.
- Rewriting exact references, applying linker exports, constructing DObjects,
  running PostLoad, or executing mutation workflows.
- Preserving occurrence-cache bytes, `DisplayRoute`, object ids, declaring
  fields, nested indices, or Map-key tokens in persistent registry state.
- Launching Engine, an editor/game binary, Cook, or an application-hosted test.

## Program Decisions

- `FAssetData` is the sole persistent per-package metadata record. It owns
  canonical hard package dependencies, soft package dependencies, and
  searchable names as distinct sorted unique `FAssetPath` categories.
- Registry reference queries return package-level edges only: source package,
  target package, and hard/soft/redirect category. Duplicate occurrences of the
  same category collapse to one edge. Redirect stays explicit and agrees with
  the catalog redirect destination.
- The registry cache schema is replaced rather than migrated in place. A stale
  occurrence cache is ignored and removed from read/write/dirty-state policy.
- Incremental reuse is based on the catalog fingerprint and cached package
  metadata. A cache miss reads bounded front matter once. `PayloadReadAttempts`
  and `PayloadBytesRead` become zero for ordinary scans and are retired from the
  persistent-reference model.
- V8 metadata comes only from `ReadPackageV8Registry(...)`. The mounted virtual
  package identity and independently known `.dasset`/`.dbulk` extents are passed
  by the scanner. V7 fallback publishes only metadata available in its bounded
  header and is explicitly temporary until P4.
- Exact occurrence extraction is not a registry query. Engine keeps a separate
  transient record and explicit package-inspection operation; those records are
  never stored in `FAssetRegistryState`, snapshots, publications, or caches.

## Implementation Stages

### Stage 0: Freeze the persistent/transient cut line

- [x] Inventory every occurrence field, cache encoding, refresh read, public
  query, and Engine/editor consumer; classify package-level versus exact-tool use.
- [x] Add the package-level edge/category contract and move exact occurrence
  vocabulary behind the Engine inspection capability without behavior execution.
- [x] Freeze catalog metadata categories, canonical order, redirect invariants,
  limits, cache replacement policy, and temporary v7 fallback behavior.
- [x] Add focused construct-free fixtures proving v8 Registry projection maps to
  exact package-level catalog and edge records.

#### Acceptance Gate

Persistent AssetRegistry state has a complete package-level target shape and
exact tooling has a separate transient type before occurrence storage is removed.

### Stage 1: Read and cache header-resident metadata

- [x] Dispatch header reads by DURF format version and route v8 front matter
  through CoreDObject without duplicating DAST parsing in AssetRegistry.
- [x] Extend `FAssetPackageHeader` and `FAssetData` with distinct canonical
  hard, soft, and searchable categories plus exact main/bulk extents.
- [x] Replace the registry-cache schema with those package-level fields and
  reject stale, malformed, duplicate, unsorted, or category-invalid entries.
- [x] Preserve bounded v7 header fallback without reading its export/value bytes.
- [x] Prove incremental reuse and full validation publish identical metadata.

#### Acceptance Gate

One bounded header read or one cache hit supplies every ordinary persistent
metadata fact; no catalog scan reads an export section.

### Stage 2: Derive package edges and remove occurrence persistence

- [x] Build hard/soft/redirect package edges deterministically from catalog
  metadata during the same atomic publication candidate.
- [x] Simplify reference index, snapshots, validation, and queries to
  package-level edges and catalog fingerprints.
- [x] Delete the reference-cache schema, occurrence route/token/display bytes,
  payload extraction loop, cache dirty state, warnings, and related statistics.
- [x] Reject inconsistent redirects, invalid paths, duplicate category edges,
  or metadata/reference publication drift without advancing the revision.
- [x] Prove ordinary refresh payload-read counts remain zero.

#### Acceptance Gate

AssetRegistry stores and publishes no occurrence-level fact, uses no separate
reference cache, and derives complete dependency queries from catalog metadata.

### Stage 3: Rehome exact consumers and retire obsolete APIs

- [x] Update Content Browser dependency presentation to package-level categories.
- [x] Move exact occurrence types and extraction entry points to Engine's
  explicit inspection/tooling seam and update mutation preparation callers.
- [x] Remove occurrence structures and extraction declarations from public
  AssetRegistry headers and remove unused v7 extraction glue from refresh code.
- [x] Keep relocation, deletion, and redirector code compileable through
  on-demand exact inspection without executing those workflows.
- [x] Prove repository searches find no occurrence route/display/token field in
  AssetRegistry state, cache, publication, snapshot, or query code.

#### Acceptance Gate

Ordinary consumers use package edges; exact consumers open packages explicitly;
AssetRegistry exposes no persistent occurrence capability.

### Stage 4: Qualify scans and publish landed contracts

- [x] Run focused v8/v7-header, cache, publication, query, redirect, malformed,
  incremental, full-validation, and atomic-failure AssetRegistry tests.
- [x] Compile AssetRegistry, Engine, affected editor targets, and exact-tool
  consumers without executing an Engine binary.
- [x] Publish package-level registry, bounded scan, cache, and exact-on-demand
  ownership contracts in Runtime and Workspace documentation.
- [x] Complete the plan, update P3, and pass documentation validation.

#### Acceptance Gate

Ordinary scans read only front matter, persistent state is package-level, exact
occurrences are transient Engine tooling, all compile gates pass, and P4 can
convert the corpus without preserving an occurrence cache.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Scan boundary | Header byte counters stop at declared front matter; payload counters/routes are absent. |
| Metadata parity | V8 hard, soft, searchable, redirect, class, count, and bulk facts match CoreDObject projection. |
| Cache | Cold, warm, stale, removed, corrupt, reordered, and schema-mismatch cases are bounded and atomic. |
| Package edges | Hard/soft/redirect categories deduplicate and sort independently of package enumeration order. |
| Publication | Catalog, edge projection, fingerprints, and revision publish as one consistent snapshot. |
| Exact tools | Mutation preparation opens candidates explicitly and no occurrence record enters registry state. |
| Ownership | AssetRegistry contains no export-value walker, Map route token, or live-object dependency. |
| Compatibility | v7 fallback reads header only; Engine/editor exact consumers compile but are not executed. |

## Related Code and Documentation

- [AssetRegistry catalog](../../../../Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/Catalog.h)
- [AssetRegistry references](../../../../Engine/Source/Runtime/AssetRegistry/Public/AssetRegistry/References.h)
- [AssetRegistry refresh](../../../../Engine/Source/Runtime/AssetRegistry/Private/AssetRegistryRefresh.cpp)
- [AssetRegistry cache](../../../../Engine/Source/Runtime/AssetRegistry/Private/AssetRegistryCache.cpp)
- [CoreDObject package format](../../../../Engine/Source/Runtime/CoreDObject/Public/DObject/PackageFormat.h)
- [Construct-Free DAST v8 reader plan](ConstructFreeDastV8Reader.md)
- [Asset Catalog and Mutation](../../../Runtime/Assets/AssetCatalogAndMutation.md)
- [Asset Packages](../../../Runtime/Assets/AssetPackages.md)
- [Core Object Package Linker roadmap](../../../Roadmaps/Archive/2026-08/CoreObjectPackageLinker.md)
- [Build and Run Workflow](../../../Agents/BuildAndRun.md)
- [Testing Workflow](../../../Agents/Testing.md)
