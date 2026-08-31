# Core Object Package Linker Roadmap

Summary: Replace the split AssetRegistry/Engine object-stream stack with a CoreDObject-owned linker, package-level registry metadata, and one canonical DAST v8 package route.

Last reviewed: 2026-08-31

Status: Completed
Completed: 2026-08-31

- 2026-08-31: P0 through P6 completed the CoreDObject linker, canonical v8
  writer/reader, package-level Registry, offline corpus migration, live Engine
  cutover, higher-level workflow qualification, and deletion of the Engine v7
  runtime stack and dormant compatibility seams.
- 2026-08-31: P7 published the lasting package/linker contracts and passed the
  offline migration, 81-target ordinary native aggregate, full build,
  documentation lifecycle, maintained-corpus, and retired-route gates. The
  Core Object Package Linker program is complete.

## Current Status

P0 through P7 are complete. The
[Core Object Package Linker Foundation plan](../Plans/CoreObjectPackageLinkerFoundation.md)
landed CoreDObject-owned linker tables and canonical Map-key mechanics, and the
[Canonical DAST v8 Writer plan](../Plans/CanonicalDastV8Writer.md) landed the
canonical nine-section v8 writer with explicit BulkData placement and exact byte
fixtures. The [Construct-Free DAST v8 Reader plan](../Plans/ConstructFreeDastV8Reader.md)
landed bounded Registry projection, complete detached decode, and exact
write-read-write qualification. The
[Package-Level Asset Registry plan](../Plans/PackageLevelAssetRegistry.md)
landed bounded v8 front-matter scans, one package-metadata cache, package-level
dependency publication, and Engine-owned transient occurrence inspection. The
maintained corpus is canonical v8. The
[CoreDObject Engine Cutover plan](../Plans/CoreDObjectEngineCutover.md) has
switched ordinary Engine save/load and codec policy to exact v8 main/bulk
closures and deleted the Engine v7 codec, live object-stream byte entrypoints,
and payload-directory wire implementation. The
[higher-level operations plan](../Plans/CoreObjectHigherLevelAssetOperations.md)
qualified retained relocation, redirector fix-up, deletion, inspection, Cook,
and maintenance workflows on exact v8 closures and removed dormant seams. The
[final qualification plan](../Plans/CoreObjectPackageLinkerFinalQualification.md)
published lasting contracts and passed the program-wide gates.

## Outcome

CoreDObject owns package tables, tagged reflected values, link-time reference
identity, canonical value semantics, and synchronous construct-free linker
models. AssetRegistry reads explicit package metadata and maintains only
package-level dependency and asset-data state. Engine owns asset policy and
eventual publication, but does not parse package tables, interpret serialized
types, or implement a second value walker. DAST v8 is the sole production write
format and, after corpus migration, the sole production read format.

## Scope

- CoreDObject package summary, name/import/export tables, package indices,
  recursive property type names, tagged property records, linker tables, and
  canonical map-key token encoding.
- Canonical DAST v8 layout with explicit Asset Registry data, hard/soft package
  references, export data, and bulk-data boundaries.
- CoreDObject-owned construct-free save/load linkers and format-neutral object
  graph application contracts.
- AssetRegistry package-level hard, soft, and searchable-name dependency state
  that does not parse export payloads during ordinary scans.
- A bounded offline v7-to-v8 conversion route and repository-corpus migration.
- Retirement or deliberate removal of v7 runtime paths and occurrence-level
  registry features that no longer justify their complexity.

## Non-Goals

- Reproducing Unreal Engine binary layouts, API spellings, async loader stages,
  IoStore, or cooking architecture before the synchronous package foundation is
  complete.
- Preserving every current AssetRegistry, compatibility-audit, mutation, or
  diagnostic API merely because it exists.
- Keeping DAST v7 as a permanent production compatibility route.
- Launching DurinEditor, a game runtime, Cook, or an application-hosted smoke
  test while the replacement stack is under construction.
- Maintaining source or binary compatibility for internal package-reader APIs.

## Program Decisions and Invariants

- **Adopt UE ownership, not UE bytes.** CoreDObject is the sole authority for
  package link identity and reflected serialization semantics. AssetRegistry
  and Engine consume that authority through narrow capabilities.
- **DAST v8 is a clean production cut.** New saves emit only v8. The v7 decoder
  survives temporarily in offline migration tooling and focused fixtures; the
  final runtime does not carry a permanent dual-reader branch.
- **Registry dependencies are package-level.** Persistent registry state keeps
  hard package, soft package, and searchable-name edges. Object id, declaring
  field, nested array position, display route, and canonical map-key tokens are
  transient occurrence data produced only by tooling that needs an exact edit.
- **Exact mutation is on demand.** Relocation, deletion, and redirector fix-up
  may open candidate packages and collect exact occurrences. They do not force
  ordinary discovery to parse export payloads.
- **Canonical output remains required.** The v8 writer freezes tables before
  emission, applies deterministic ordering, rejects late discovery, and emits
  byte-identical output for identical logical inputs.
- **Migration fails closed.** A v7 package with an unsupported custom payload,
  retained unknown value, or ambiguous conversion is reported and left
  untouched. The program does not promise lossless preservation of low-value
  compatibility evidence, but it never silently discards authored data.
- **No live Engine validation is required during construction.** Child plans
  use CoreDObject- and AssetRegistry-focused native tests, byte fixtures, and
  compile validation. Engine may be compiled to keep the checkout buildable,
  but editor/game launch, live package loading, PostLoad behavior, and Cook are
  deferred until the explicit cutover milestone.
- **Each child plan leaves the repository buildable.** Temporary adapters and
  compile-only shims are allowed; parallel production implementations are
  removed at the first milestone whose acceptance gate no longer needs them.

## Preservation and Retirement Policy

| Capability | Program disposition |
| --- | --- |
| Asset/package identity, object Outer graph, reflected scalar/container/struct values, hard and soft references | Preserve and qualify in v8. |
| Deterministic package bytes, bounded parsing, bulk payload integrity, atomic file publication | Preserve. |
| Global occurrence-level reference routes and `DisplayRoute` state | Remove from persistent AssetRegistry state. |
| Canonical Map tokens for authored intent and exact mutation | Preserve in CoreDObject; compute only where needed. |
| DAST v7 writing, in-place v7 rewrite, and permanent v7 runtime loading | Retire. |
| Construct-free compatibility reports, deprecated-route evidence, and retained-unknown resave | Remove unless a later milestone proves a concrete migration-critical need. |
| Redirector fix-up, relocation, deletion, and Cook integrations | Reintroduce only through v8/package-level contracts; remove unsupported legacy variants rather than carrying parallel paths. |

## Milestones

- [x] **P0: Core Object Package Linker Foundation.** Execute the
  [foundation plan](../Plans/CoreObjectPackageLinkerFoundation.md); depends only
  on the current v7 fixtures and completes when CoreDObject owns the linker
  vocabulary and canonical token primitives, AssetRegistry can adapt decoded
  v7 packages into the format-neutral model, duplicate helpers are retired,
  and no Engine runtime has been launched.
- [x] **P1: Canonical DAST v8 Writer.** Execute the
  [writer plan](../Plans/CanonicalDastV8Writer.md) to freeze the v8 section contract and build
  a CoreDObject linker-save path for name/import/export tables, tagged values,
  Registry metadata, and bulk references; depends on P0 and completes when
  deterministic fixtures re-emit byte-identically without AssetRegistry or
  Engine serialization code.
- [x] **P2: Construct-Free DAST v8 Reader and Linker Model.** Implement bounded
  summary/table/export decoding and logical round-trip validation through the
  [reader plan](../Plans/ConstructFreeDastV8Reader.md); depends on
  P1 and completes when malformed-input, topology, reference, type, and limit
  coverage passes entirely below Engine.
- [x] **P3: Package-Level Asset Registry.** Replace occurrence persistence and
  export-payload scanning through the
  [registry plan](../Plans/PackageLevelAssetRegistry.md) with v8 Registry data plus import/soft-reference
  tables; depends on P2 and completes when ordinary scans read no export data,
  registry/cache state contains only selected dependency categories, and
  obsolete occurrence APIs and caches are removed.
- [x] **P4: Offline v7-to-v8 Migration.** Move v7 support behind a bounded
  conversion capability through the
  [migration plan](../Plans/OfflineDastV7ToV8Migration.md), produce deterministic migration reports, and convert
  the maintained asset corpus; depends on P1 and P2 and completes when all
  supported repository assets are v8, failures are explicit and non-mutating,
  and production writers expose no v7 route.
- [x] **P5: CoreDObject Load Application and Engine Cutover.** Apply linker
  exports to DPackage/DObject graphs through the
  [cutover plan](../Plans/CoreDObjectEngineCutover.md), then replace Engine package readers and
  writers with the CoreDObject capability; depends on P2 through P4 and
  completes when focused object-lifecycle tests pass, Engine contains no
  package-wire interpreter, and retired v7/live compatibility code is deleted.
- [x] **P6: Rebuild or Retire Higher-Level Asset Operations.** Reintroduce only
  justified v8 relocation, deletion, redirector, exact occurrence, Cook, and
  maintenance workflows; depends on P3 and P5 and completes when every former
  workflow is either qualified on the new contracts or deliberately removed
  with callers and documentation cleaned up. Execute the
  [higher-level operations plan](../Plans/CoreObjectHigherLevelAssetOperations.md).
- [x] **P7: Final Qualification and Contract Publication.** Move lasting linker,
  v8 format, Registry, and migration rules into their owning Runtime and
  Workspace documents; depends on all required prior milestones and completes
  when routine native aggregates, the full build gate, documentation
  validation, and repository-wide searches prove that no retired production
  route remains. Execute the
  [final qualification plan](../Plans/CoreObjectPackageLinkerFinalQualification.md).

## Program Validation

Each child plan follows the repository build and test workflows. Across the
program, acceptance evidence must cover canonical byte identity, bounded and
atomic decode failure, table/reference topology, package-level dependency
parity, scan byte ranges, offline conversion atomicity, reflected value parity,
bulk-data integrity, and absence of retired symbols. Application-hosted
execution is intentionally absent until P5 and is not substituted with an
editor launch during earlier milestones.

## Completion Criteria

- CoreDObject is the only production owner of package tables, tagged property
  semantics, and load/save linkers.
- AssetRegistry ordinary scans consume only explicit metadata and package
  reference tables, never export payloads.
- Engine contains no DAST table decoder, wire-type mapping, or duplicate nested
  value walker.
- The maintained asset corpus is DAST v8 and production exposes neither a v7
  writer nor a v7 runtime reader.
- Every higher-level asset workflow is either qualified on v8 or explicitly
  removed, with no dormant compatibility branch.
- Lasting contracts are published in the owning documentation and every active
  child plan is completed or explicitly dispositioned.

## Related Documentation

- [Asset Packages](../Runtime/Assets/AssetPackages.md)
- [Asset Catalog and Mutation](../Runtime/Assets/AssetCatalogAndMutation.md)
- [Serialization](../Runtime/Core/Serialization.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Build and Run Workflow](../Agents/BuildAndRun.md)
- [Testing Workflow](../Agents/Testing.md)
