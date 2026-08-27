# AssetForge Framework Removal Plan

Summary: Delete obsolete generic import infrastructure and qualify the final thin import boundary

Last reviewed: 2026-08-27

Status: Completed
Completed: 2026-08-27

## Current Status

Milestones A through C have removed every production import caller of the
generic AssetForge graph, component registry, request/job/operation, replay,
and mounted-source workflow. Direct built-in importers own all supported
families, Scene owns private ordering, and DurinEd uses a finite built-in
dispatch table. `DurinAssetTool` and several tests still use `FImportService`
only as an obsolete derived-data recovery pump; they are removal callers, not
evidence for retaining the service.

The remaining work is physical deletion and dependency closure. Obsolete code
is concentrated in AssetForge's service/graph/operation implementation,
AssetCore mounted-source management, unused provider adapters and replay data,
their characterization tests, and authored/documentation compatibility debt.
This plan begins with a repository-bounded inventory so shared capture and
diagnostic helpers are moved before their former module is narrowed or removed.

Stage 1 is complete. Generic orchestration, persistence, provider adapters, and
their contract tests are physically deleted. Direct-family import support now
lives in AssetForgeBuiltins, Scene cancellation follows its explicit callback,
and canonical resave waits through `FAssetCompilingManager` before checking
save readiness. Focused validation passed AssetImportDataTests 2/2,
AssetImportTests 17/17, SceneImportTests 4/4, SkeletalSceneLifecycleTests 1/1,
TerrainHeightmapTests 11/11, TextureTests 75/75, StaticMeshTests 74/74,
MaterialTests 100/100, StaticMeshThumbnailTests 9/9, and ThumbnailTests 58/58;
AssetForgeBuiltins and DurinAssetTool builds also pass.

Stage 2 is complete. AssetCore mounted-source staging, replacement, relocation,
repair, and transaction code and tests are deleted, as are StaticMesh and
Terrain source-mutation adapters and compatibility entrypoints. Texture2D
publication accepts only the normalized filename contract, and Scene snapshot
capture no longer exposes mounted-path entrypoints. A byte scan of all 25
repository-owned authored packages outside source/tests found no retired replay
or mounted-source field, and no import-specific source-mount configuration
required migration. AssetForgeBuiltins builds, and AssetImportDataTests 2/2,
TerrainHeightmapTests 11/11, StaticMeshTests 73/73, and
EditorAssetWorkflowTests 32/32 pass. After deleting the final Texture2D mounted
publication fallback, TextureTests 75/75, AssetPackageTests 125/125,
AssetCookTests 13/13, and ContentBrowserWorkflowTests 60/60 also pass.

Stage 3 is complete. The empty `AssetForge` module and orphaned
`AssetImportCore` API shell are deleted, 45 explicit module/build/test
dependencies are removed, and editor initialization loads only
`AssetForgeBuiltins` before catalog scanning. Texture2D and VolumeTexture no
longer expose legacy import-recovery features; the former Texture2D recovery
pump's recursive no-work wait was found by the aggregate gate and removed.
The default Editor `all` build passes, `fast-all` passes all 63 selected native
targets, SceneImportTests 4/4, SkeletalSceneLifecycleTests 1/1,
TerrainHeightmapCookTests 1/1, and the hidden 8-tick Sandbox startup exits
normally in 2.53 seconds. The Vulkan-only TextureCook qualification binary
builds, but its run is unavailable in the Codex background session because
Metal is not exposed to MoltenVK; ordinary AssetCook and Terrain Cook coverage
pass. From the pre-removal `da776e8d` baseline, the completed program changes
204 files with 2,089 additions and 13,102 deletions, a net reduction of 11,013
lines.

## Frozen Removal Inventory

The Stage 0 inventory found 6,299 lines in the 28 AssetForge headers and source
files, plus 779 lines in `MountedSource.h`/`.cpp`. The three framework-specific
AssetCore test files add 1,651 lines. A byte-oriented scan of repository-owned
authored content outside source and test fixtures found no retired replay,
graph, provider, or mounted-source field, so no authored compatibility reader
or corpus regeneration is required.

| Classification | Exact boundary |
| --- | --- |
| Delete | Public source/build graphs, schema payloads, translators, planning passes, builders, component registration/leases, requests/results, jobs, operations, async execution, service lifecycle, replay/provenance objects, provider registration no-ops, and their characterization tests |
| Move and narrow | Diagnostics, progress labels, immutable source snapshot capture, output summaries, payload hashing, and publication locking move into AssetForgeBuiltins as direct-family values; Scene cancellation uses its explicit callback instead of worker thread-local state |
| Retain | Direct family APIs and concrete import-data objects in AssetForgeBuiltins; family post-load/DDC features with concrete runtime callers; Engine's `DAssetImportData`, `FAssetImportInfo`, and `FSourceFile` filename contract |
| Delete in source-policy stage | AssetCore mounted-source staging/replacement/relocation and tests, the `AssetTools.h` umbrella include, StaticMesh/Terrain source-mutation feature adapters, and the unused ingest/change compatibility entrypoints |
| Replace caller | `DurinAssetTool` canonical resave and recovery-waiting tests use asset compiling/readiness or concrete family waits, then drop `FImportService`; stale module dependencies are removed after include closure |

Deletion order is fixed as follows: first move the thin direct-family values and
replace service/cancellation callers; second delete generic orchestration and
provider compatibility code; third delete mounted-source and source-mutation
surfaces; fourth remove stale module dependencies and either delete AssetForge
or prove a remaining multi-family helper. Validation is fixed to AssetForge,
AssetCore package/import-data, Texture, Material, Scene/skeletal, StaticMesh,
Terrain, Content Browser/editor workflow, thumbnail, and Cook tests, followed
by the default Editor `all` build, hidden 8-tick startup/normal-exit smoke,
content/symbol/dependency searches, and all documentation validators.

## Goal

Delete the obsolete generic import framework and mounted-source management,
leave only demonstrably shared thin helpers at their correct owners, migrate
the repository corpus, and qualify Editor, Cook, and runtime dependency closure.

## Scope

- Remove generic source/build graphs, planning passes, component registration
  and leases, import requests, jobs, operations, replay schemas, and service
  lifecycle code.
- Remove mounted-source staging, replacement, relocation, dependency, repair,
  and transaction APIs plus tests and compatibility fields.
- Move retained capture/diagnostic helpers out of obsolete framework ownership
  and eliminate the AssetForge module after dependency evidence permits.
- Remove unused built-in provider adapters and update module/project closure.
- Audit or regenerate repository-owned authored content into concrete family
  import-data schemas.
- Update lasting documentation and validate Editor, Cook, and runtime loading.

## Non-Goals

- Adding third-party importer registration, runtime import, whole-scene
  reimport, source copying, or a replacement generic orchestration layer.
- Changing supported family formats or normalized runtime payload formats.
- Preserving retired graph/replay/mounted-source schemas as an indefinite
  public compatibility contract.

## Design Decisions and Invariants

- Production behavior established by Milestones A-C is the semantic baseline;
  removal may relocate helpers but must not route a family back through a
  generic request or operation.
- One import attempt captures each required file once and consumes immutable
  bytes through decode and build preparation.
- Concrete family import data stores normalized filenames and family settings;
  cooked assets contain no editor source provenance.
- Missing disposable data remains family Build/PostLoad behavior, never import
  `Recover` behavior.
- Repository compatibility is read-old/write-new only when an inventory proves
  it is safer than regenerating the bounded corpus. No compatibility registry
  or dual-write framework is introduced.
- Delete callers before definitions and remove module dependencies only after
  searches and link closure prove they are unnecessary.

## Current Foundations and Gaps

| Area | Foundation | Removal gap |
| --- | --- | --- |
| Importers | All supported families call concrete direct APIs | Legacy provider adapters and generic definitions remain compiled |
| Scene | Private capture, ordering, collision preflight, and atomic peer save | Some shared value names/includes still originate in AssetForge framework headers |
| Editor | Finite built-in dispatch and regular file selection | Dead operation/mounted-source contracts and tests remain outside production |
| Runtime | Concrete import data and Cook stripping are implemented | AssetCore still exports mounted-source mutation APIs |
| Content | Repository corpus passed prior canonical migration | Final scan must find and disposition retired replay/source fields |

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory remaining framework and mounted-source definitions, tests,
  module dependencies, authored fields, and lasting documentation references.
- [x] Classify every remaining AssetForge value as delete, move, or retain with
  a concrete multi-family caller.
- [x] Freeze deletion order, compatibility disposition, and exact validation
  selections before changing module closure.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Remove generic orchestration

Dependencies: Stage 0 complete.

- [x] Delete import graphs, planning passes, component registry/leases,
  requests, jobs, operation snapshots, service scheduling, and replay data.
- [x] Delete unused provider adapters and move retained capture/diagnostic
  helpers to their narrow owning modules.
- [x] Remove obsolete tests and replace only behavior still required by direct
  family contracts.

#### Acceptance Gate

- No production or test target compiles a generic importer protocol, request,
  job, operation, component registry, graph, or replay schema.

### Stage 2: Remove mounted-source management and migrate content

Dependencies: Stage 1 complete.

- [x] Delete AssetCore mounted-source staging, mutation, dependency, repair,
  replacement, and relocation APIs and their tests.
- [x] Remove retired serialization fields/readers and audit or regenerate the
  repository-owned authored corpus.
- [x] Remove stale source-mount configuration and module dependencies.

#### Acceptance Gate

- Source workflows use only normalized filenames and immutable reads; content
  and searches contain no retired mounted-source or replay schema.

### Stage 3: Narrow modules and qualify removal

Dependencies: Stage 2 complete.

- [x] Narrow or remove AssetForge, update project/module closure, and prove
  Cooked/runtime targets require no editor importer or offline decoder.
- [x] Run focused family, Scene, failure, package, Cook, editor workflow,
  aggregate, full Editor build, and hidden-window smoke validation.
- [x] Update lasting documentation, complete the roadmap, and record final
  line/module/dependency reduction evidence.

#### Acceptance Gate

- Removal searches, content audit, documentation validation, Editor build and
  smoke, Cook, and runtime closure pass with no compatibility exception left.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Direct families | Texture, cube, volume, static mesh, Terrain, Scene, skeletal, animation, and material tests pass |
| Failure safety | Decode/build/cancellation failures preserve prior live and persisted state |
| Source policy | Project-relative and external absolute source tests pass without source mutation |
| Content | Repository scan and compatibility audit find no retired replay or mounted-source fields |
| Cooking/runtime | Cook tests and registered runtime closure load without editor import modules or offline codecs |
| Removal | Symbol and dependency searches find no generic graph/registry/job/operation/replay/mounted-source APIs |
| Product | Default Editor `all` build and bounded hidden-window startup/exit pass |
| Documentation | Changed/all, all-plan, and all-roadmap validation pass |

## Definition of Done

- AssetForge contains only proven shared thin helpers or is removed entirely.
- Every generic import protocol, replay schema, and mounted-source mutation API
  is physically deleted with no production, test, content, or documentation
  dependency.
- Direct import behavior, package safety, Cook/runtime closure, Editor build
  and smoke, and all documentation validators pass.
- The Asset Import Simplification roadmap is completed with quantified final
  module, dependency, and line-count evidence.

## Deferred Follow-ups

- New import families, if selected later, follow the concrete family pattern;
  extensibility is reconsidered only from demonstrated independent providers.

## Related Documentation

- [Asset Import Simplification Roadmap](../Roadmaps/AssetImportSimplification.md)
- [Asset Import Architecture](../Editor/Architecture/AssetImportFramework.md)
- [Source File Workflows](../Editor/Guides/SourceFileWorkflows.md)
- [Asset Data Lifecycle And Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing](../Agents/Testing.md)

## Related Code

- [`AssetForgeBuiltins`](../../Engine/Source/Editor/AssetForgeBuiltins)
- [`AssetImportData.h`](../../Engine/Source/Runtime/Engine/Public/Asset/AssetImportData.h)
- [`SourceFilename.h`](../../Engine/Source/Runtime/Engine/Public/Asset/SourceFilename.h)
- [`SceneDirectImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneDirectImport.cpp)
