# Asset Import Simplification Roadmap

Summary: Replace AssetForge's generic import platform with a thin shared module, direct asset-family importers, and a UE-style source-file model.

Last reviewed: 2026-08-27

Status: Active
Completed:

## Current Status

The graph-based AssetForge framework is implemented and qualified, but its
extension, replay, asynchronous-operation, and mounted-source abstractions are
substantially broader than Durin's selected product requirements. Built-in
families are the only supported import implementations, Scene import is
creation-only, and there is no committed requirement for runtime import,
third-party importer registration, hot-unloadable providers, or user-composed
planning pipelines.

The former `AssetImportDataRefactor` plan has been removed rather than
continued because its central outcome, one generic persisted AssetForge replay
model, conflicts with this roadmap. Its completed implementation remains in
Git history and in the current codebase only until the milestones below remove
or replace it.

Milestone A is active through the [AssetForge Simplification Foundation
Plan](../Plans/AssetForgeSimplificationFoundation.md). Later plans are created
only after the preceding exit gate has fixed the boundary they consume.

## Outcome

Durin imports authored assets through explicit asset-family code. AssetForge
contains only shared values and helpers that demonstrably remove duplication;
it owns no generic translator/planning/builder protocol, component registry,
lease system, import graph, replay schema, or asynchronous import state
machine. Each family owns its accepted formats, settings, source dependencies,
build invocation, publication, reimport behavior, and editor-only import data.

Imported assets retain lightweight source-file metadata using project-relative
paths when possible and normalized absolute paths otherwise. Import and
reimport do not require sources to reside in package mounts. Missing derived
data is rebuilt through family build/compilation facilities rather than a
generic `Recover` import mode.

## Scope

- Narrow `AssetForge` and `AssetForgeBuiltins` to shared import utilities and
  direct built-in family importers.
- Remove `FSourceGraph`, `FBuildGraph`, planning passes, the three component
  registries, component registration/lease ownership, generic import requests,
  jobs, operations, and framework replay persistence.
- Remove `EImportMode::Recover`; route disposable-data reconstruction through
  each asset family's existing build or compilation domain.
- Replace mounted-source ingest, relocation, shared replacement, dependency,
  and repair workflows with lightweight source-file selection and reimport.
- Migrate Texture2D, TerrainHeightmap, standalone StaticMesh, TextureCube, and
  VolumeTexture to direct family importers.
- Keep Scene import as one built-in creation-only importer with private local
  dependency ordering for its outputs.
- Update authored repository content, tests, editor workflows, module
  boundaries, and lasting documentation to the simplified contract.

## Non-Goals

- Runtime asset import.
- Third-party or dynamically registered import providers.
- Hot unload of importer implementations while accepted import work is active.
- User-configurable translator or planning-pass stacks.
- A general intermediate scene/node graph or persisted replay language.
- Whole-scene reimport, stable multi-output reconciliation, or generated-output
  ownership records.
- Automatic copying, moving, deleting, or version-control management of source
  files.
- Preserving current AssetForge replay or mounted-source schemas as a public
  compatibility baseline; repository-owned authored assets are regenerated or
  resaved with the milestone that removes each schema.

## Program Decisions and Invariants

- Runtime `Engine` retains the editor-only `DAssetImportData` base and a
  lightweight `FAssetImportInfo`/`FSourceFile` value contract. Asset-family
  concrete import-data objects live with their editor importer implementation.
- `FSourceFile` stores a normalized filename, content hash, optional timestamp,
  and display/role metadata. A file under the project directory is persisted
  project-relative; another file is persisted as an absolute path. Resolution
  never requires an Asset mount or mutates the source file.
- One import attempt captures every required source into immutable bytes once.
  Recognition, hashing, decoding, and building consume that same captured
  closure; later phases do not reopen a physical source.
- A family importer performs all failable decode/build validation before its
  narrow editor-thread publication. Failure before publication leaves an
  existing asset unchanged.
- Successful live publication and package persistence remain separate facts.
  Save failure leaves valid newly published state Dirty for an explicit retry.
- Family build/compilation systems may remain asynchronous. AssetForge does not
  own a second generic operation lifecycle, cancellation vocabulary, mailbox,
  progress history, or worker admission system.
- Missing or corrupt disposable data invokes a family rebuild path. It is not
  represented as import, reimport, repair, or source replacement.
- Scene may use private transient nodes or a private topological order when its
  Skeleton, SkeletalMesh, and AnimationClip outputs require it. Those values
  are not public AssetForge protocols and are never persisted for replay.
- DurinEd dispatches to the finite built-in importer set explicitly. A small
  compile-time descriptor table is permitted for UI labels and extensions; it
  must not recreate registration, leasing, or hot-unload semantics.
- Compatibility is repository-bounded and read-old/write-new only where a
  milestone demonstrates that temporary reading is cheaper and safer than
  regenerating the affected corpus. No generic compatibility framework is
  introduced.

## Current Foundations and Gaps

| Area | Foundation to retain | Gap to close |
| --- | --- | --- |
| Source values | Engine already owns `FSourceFile`, `FAssetImportInfo`, and `DAssetImportData` | Their current path and concrete replay model are coupled to mounted sources and AssetForge graphs |
| Family builds | Texture, mesh, skeletal, animation, and Terrain domains already own normalized build products and DDC behavior | Generic AssetForge adapters duplicate their orchestration and recovery entrypoints |
| Publication | Candidate preparation and one-way editor-thread publication are qualified | The useful failure boundary is embedded in generic builder and job protocols |
| Importers | Built-in code already exposes family-specific import/reimport functions | Provider implementations still translate those calls into generic requests, graphs, and registry lookups |
| Scene | Creation-only peer outputs and dependency-ordered publication are implemented | The implementation is expressed through public generic graph/planning contracts |
| Editor | Import dialogs and activity surfaces exist | They assume mounted-source destinations and one generic import operation handle |
| Authored data | Repository content is the only required authored baseline | Recently written `DAssetForgeImportData` and mounted-source fields must be audited and replaced without indefinite dual writes |

## Milestone Map

| Milestone | Dependencies | Deliverable | Entry gate | Exit gate |
| --- | --- | --- | --- | --- |
| A. Foundation and Texture2D proof | None | Thin target boundary, lightweight filename semantics, and one direct Texture2D importer | Roadmap decisions accepted | Texture2D import/reimport/build/publication no longer uses graphs, registries, generic jobs, mounted-source mutation, or `Recover` |
| B. Direct single-asset families | A | TerrainHeightmap, StaticMesh, TextureCube, and VolumeTexture migrated to family-owned importers and import data | Foundation APIs and Texture2D pattern qualified | All standalone import/reimport and derived-data rebuild paths bypass the generic framework |
| C. Scene and editor workflow cutover | B | Private Scene orchestration and direct DurinEd importer dispatch using UE-style file selection | No standalone family depends on generic orchestration | Scene, dialogs, Content Browser actions, and source inspection require neither registries nor mounted-source workflows |
| D. Framework removal and qualification | C | Obsolete AssetForge framework, mounted-source management, schemas, tests, and documentation removed | Every production caller has a direct replacement | Repository searches, content migration, focused/aggregate tests, builds, Cook/runtime closure, and documentation validation pass |

## Child Plan Boundaries

| Plan | Status | Owned result |
| --- | --- | --- |
| [AssetForge Simplification Foundation](../Plans/AssetForgeSimplificationFoundation.md) | Active | Final thin boundary, source filename contract, Texture2D vertical slice, and migration evidence for later families |
| `DirectSingleAssetImporters` | Proposed after Milestone A | TerrainHeightmap, StaticMesh, TextureCube, and VolumeTexture cutover |
| `SceneAndImportWorkflowSimplification` | Proposed after Milestone B | Scene-private ordering, direct UI dispatch, source inspection, and removal of mounted-source user workflows |
| `AssetForgeFrameworkRemoval` | Proposed after Milestone C | Physical deletion, authored corpus cleanup, dependency closure, full qualification, and lasting documentation |

## Program Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Source policy | Project-relative and external absolute sources import and reimport; missing, changed, and unsupported files produce family diagnostics without mount mutation |
| Determinism | Decode, hashes, build keys, and publication consume one immutable captured source closure |
| Existing asset safety | Decode/build/cancellation failure before publication preserves prior live and persisted state |
| Persistence | Successful publication plus save success/failure retain the selected Dirty and on-disk semantics |
| Derived data | Warm, missing, and corrupt DDC paths rebuild through family build systems without `EImportMode::Recover` |
| Family behavior | Texture2D, TextureCube, VolumeTexture, StaticMesh, TerrainHeightmap, Scene, skeletal, animation, and material import coverage passes |
| Cooking and runtime | Editor-only filenames/settings are stripped as selected; Game/cooked loading has no AssetForge or offline-codec dependency |
| Removal | Searches find no production graph, planning-pass, component lease/registry, generic import-job, replay-data, or mounted-source workflow path |
| Documentation | Changed/all, all-plan, and all-roadmap validators pass; lasting contracts describe only implemented behavior |

## Risks and Control Gates

- External source paths reduce repository portability. Milestone A must prove
  deterministic relative-path storage beneath the project and explicit missing
  source diagnostics before other families migrate.
- Removing the generic async operation loses shared cancellation and Activity
  History semantics. Each child plan must identify which UI behaviors remain
  product requirements and map them to existing family compilation or editor
  task facilities without recreating the deleted state machine.
- Current recovery tests may conflate import replay with DDC rebuilding. No
  `Recover` caller is removed until its family has a tested build-domain
  replacement or an explicit decision that source-free recovery is unsupported.
- Scene is the largest multi-output exception. It migrates only after all
  single-output families establish that no public graph abstraction remains
  necessary.
- Physical deletion is delayed until production call sites are cut over, but
  no milestone may add new users to APIs selected for removal.

## Completion Criteria

- Every supported import entrypoint is owned by a concrete built-in family and
  uses no generic translator, planning, builder, registry, lease, graph, replay,
  or import-job protocol.
- AssetForge exposes only the documented thin shared types/helpers; its module
  size and dependency closure are materially reduced and enforced.
- Imported assets use the selected lightweight source-file model without
  mounted-source ingestion or relocation management.
- Derived-data reconstruction is family Build behavior and no `Recover` import
  mode remains.
- Scene uses only private transient orchestration and publishes ordinary peer
  assets once.
- Repository-owned authored assets contain no retired replay or mounted-source
  schema, and supported cooked/runtime targets load without editor import code.
- Focused and aggregate validation, Editor build/smoke, Cook/runtime closure,
  boundary searches, and documentation validation pass.
- Lasting architecture and workflow documents are updated, every required child
  plan is completed, and conditional work is explicitly dispositioned.

## Related Documentation

- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Mounted Source Workflows](../Editor/Guides/MountedSourceWorkflows.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing](../Agents/Testing.md)

## Related Code

- [`AssetForge`](../../Engine/Source/Editor/AssetForge)
- [`AssetForgeBuiltins`](../../Engine/Source/Editor/AssetForgeBuiltins)
- [`AssetImportData.h`](../../Engine/Source/Runtime/Engine/Public/Asset/AssetImportData.h)
- [`Texture2DImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp)
- [`SceneImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/SceneImport.cpp)
- [`ImportDialogSupport.cpp`](../../Engine/Source/Editor/DurinEd/Private/Import/ImportDialogSupport.cpp)
- [`MountedSource.h`](../../Engine/Source/Runtime/AssetCore/Public/Asset/MountedSource.h)
