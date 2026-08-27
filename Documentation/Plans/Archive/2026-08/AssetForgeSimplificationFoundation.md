# AssetForge Simplification Foundation Plan

Summary: Define the thin AssetForge boundary and prove direct per-family import with Texture2D before removing the generic framework.

Last reviewed: 2026-08-27

Status: Archived
Completed: 2026-08-27

## Current Status

All four stages and the Milestone A exit gate are complete. Engine common
metadata now persists normalized source filenames, and Texture2D imports,
reimports, replaces sources, and rebuilds disposable data through direct
family code. Its provider/schema/adapter, generic request and provenance API,
mounted-source mutation UI, and `Recover` path are removed.

The six repository Texture2D packages were canonically migrated to
`DTexture2DImportData`. The final audit reports 25 compatible packages and no
errors or resave recommendations; a project-scope canonical-resave plan reports
0 ready, 0 blocked, and 25 skipped. The default Editor target builds and its
hidden-window 8-tick startup/shutdown smoke exits successfully.

The remaining generic framework is a compatibility baseline only for
TerrainHeightmap, standalone StaticMesh, TextureCube, VolumeTexture, and Scene.
The next roadmap child plan owns their single-asset cutover.

## Goal

Select and qualify the exact thin AssetForge contract, change common import
metadata from mounted paths to lightweight source filenames, and migrate
Texture2D end to end to a direct family-owned importer. The result must provide
a repeatable pattern for later single-asset families without retaining or
recreating generic graphs, component registries, lifecycle leases, import jobs,
or `Recover` mode.

## Scope

- Inventory the complete production, test, module, authored-data, and editor
  surface used by the framework mechanisms selected for removal.
- Freeze the final public API and module ownership of thin `AssetForge`, direct
  `AssetForgeBuiltins` importers, Engine import metadata, and DurinEd dispatch.
- Replace the mounted `FSourcePath` member of common import metadata with a
  normalized source filename contract supporting project-relative and external
  absolute files.
- Add the Texture2D-specific editor-only import-data state required to reimport
  the texture without a generic replay descriptor.
- Make Texture2D import and reimport call its family capture, decode, build,
  publication, and save code directly.
- Route Texture2D missing/corrupt derived-data reconstruction through the
  texture compilation/build domain and remove its use of
  `EImportMode::Recover`.
- Remove Texture2D's provider registration, graph/planning/build adapters,
  generic request construction, and mounted-source ingest/change/repair/
  relocation entrypoints once all callers are migrated.
- Update representative authored fixtures and focused tests to the new source
  and import-data schema.
- Record the measured cutover pattern and exact residual generic dependencies
  that later child plans must remove.

## Non-Goals

- Migrating TerrainHeightmap, StaticMesh, TextureCube, VolumeTexture, or Scene.
- Physically deleting generic framework types still required by an unmigrated
  family.
- Redesigning TextureBuild normalized inputs, products, DDC key semantics,
  cooked payloads, or runtime render resources.
- Adding a new importer interface, registry, service locator, plugin protocol,
  graph, planning layer, or generic asynchronous task abstraction.
- Preserving mounted-source ingest, relocation, shared replacement, or mount
  dependency behavior for Texture2D.
- Automatic source-file copying or version-control integration.
- Automatic migration of arbitrary external projects or production content;
  only the repository-owned authored baseline is required.
- Generalizing the Texture2D implementation before at least one later family
  demonstrates identical concrete duplication.

## Design Decisions and Invariants

### Module and API boundary

- Runtime `Engine` owns `FSourceFile`, `FAssetImportInfo`, and abstract
  `DAssetImportData` because Engine asset classes retain the editor-only base
  reference. These types contain no AssetForge component, graph, registry,
  replay, or mounted-source vocabulary.
- Editor `AssetForge` may retain only family-neutral values/helpers proven by
  current callers: bounded import diagnostics/results, immutable physical-file
  capture, filename normalization/resolution, and narrowly scoped publication
  or save helpers if those helpers have at least two real consumers. Stage 0
  must reject speculative utilities.
- `AssetForgeBuiltins` owns `DTexture2DImportData`, Texture2D source settings,
  extension recognition, source capture/decode, direct import/reimport, and the
  adapter into TextureBuild/texture compilation.
- DurinEd calls the Texture2D importer explicitly. It does not discover the
  importer through `FImportService`, component registries, modular features, or
  leases.

### Source filename model

- `FSourceFile` persists a `Filename` string rather than `FSourcePath`. A
  selected regular file beneath the configured project directory is stored as
  a normalized project-relative generic path; a file outside it is stored as a
  normalized absolute path.
- Relative filenames resolve only against the current project directory.
  Absolute filenames resolve directly. Package mounts, mount dependencies,
  redirectors, and asset package location do not participate.
- Normalization rejects empty values, traversal that escapes the project base,
  malformed absolute paths, nonregular files at capture time, excessive
  strings, and platform-incompatible forms with a structured diagnostic.
- Persisted hash is authoritative for detecting changed bytes. Timestamp and
  byte count are diagnostics and cheap-change hints, never substitutes for a
  content hash.
- Reimport uses the persisted filename and current asset settings. Selecting a
  different file is an explicit family operation that replaces import metadata
  only after the new candidate is valid; it does not move either source file.

### Texture2D import data and execution

- `DTexture2DImportData` contains common source information plus only the
  decoder/import settings that are required to reinterpret encoded source
  bytes. Editable Texture build settings remain on `DTexture2D` and in its
  existing build-key inputs.
- Texture2D captures the encoded file exactly once per attempt. Hashing,
  decoding, build-key preparation, and publication consume that immutable byte
  snapshot. No later phase reopens the filename.
- Import validates the destination and source, builds a detached product, then
  creates/publishes the asset on the editor thread. Reimport prepares a complete
  replacement before modifying the existing live texture.
- Failure or cancellation before publication leaves the existing texture,
  import-data object, DDC identity, package bytes, and Dirty state unchanged.
  Final publication is non-cancelable and applies once.
- Package save follows successful publication. Save failure leaves the valid
  in-memory texture and its package Dirty while preserving prior on-disk and
  catalog state.
- Texture compilation may continue to use its established family-owned
  asynchronous queue, priority, completion, and cancellation behavior. No
  Texture2D path submits `FImportJob` or depends on generic import-operation
  snapshots/history after cutover.
- Missing or corrupt Texture2D derived data submits a texture build from the
  retained source and current settings. It does not construct an import
  request, publish authored import metadata, save a package, or use
  `EImportMode::Recover`.

### Compatibility and removal

- The supported authored baseline is the repository content present when this
  plan executes. Stage 0 inventories every Texture2D carrying legacy typed
  source fields, mounted paths, provenance, or `DAssetForgeImportData`.
- Prefer regenerating or canonical-resaving repository assets in the same
  change that changes the schema. Add a bounded read-old path only when the
  inventory proves regeneration would lose irreplaceable authored intent.
- New Texture2D publication writes only the selected family import-data schema.
  It never dual-writes generic provenance or mounted-source fields.
- Temporary compatibility code is private to Texture2D and has a named removal
  gate in this plan; it may not enter Engine's common import-data contract.

## Current Foundations and Gaps

| Area | Current foundation | Required foundation result |
| --- | --- | --- |
| Common metadata | Engine has reflected, editor-only `FSourceFile`, `FAssetImportInfo`, and `DAssetImportData` with validation, clone-to-owner, save/load, Cook stripping, and construct-free inspection tests | Replace mounted path semantics and remove AssetForge replay assumptions without weakening object ownership or Cook stripping |
| Texture source | `FEncodedSourceSnapshot` and direct image decoding already produce immutable encoded/normalized values | Generalize only the physical filename capture needed by direct import and eliminate later file reopening |
| Texture build | TextureBuild and Texture2D compilation already own settings, product construction, DDC, priority, and completion | Make them the only Texture2D build/recovery execution path |
| Publication | `FTexture2DImportedState`/product publication already applies coherent texture state | Attach family import data at the same narrow editor-thread boundary without generic builders |
| Generic path | Texture2D provider, schema, adapter, request, graphs, service, job, registry, and replay persistence are qualified | Remove every Texture2D dependency on them while leaving unmigrated families compiling |
| Editor workflow | Dialog and Content Browser actions can submit/observe Texture2D work | Replace mounted-source destination and generic-handle assumptions with direct family calls and existing compilation observation |
| Tests/content | Broad import, cache, source relocation, async, package, and Cook coverage exists | Reclassify tests by retained product behavior versus deliberately deleted mounted/framework behavior and migrate repository fixtures |

## Stage 0 Cutover Record

- Baseline on 2026-08-27: `AssetForge` contains 30 files, 6,124 lines, 17
  public files, 11 private files, and 461 declaration-like symbols;
  `AssetForgeBuiltins` contains 61 files, 13,689 lines, 11 public files, 48
  private files, and 588 declaration-like symbols. `AssetForge` publicly
  depends on `AssetCore` and `Engine`; `AssetForgeBuiltins` publicly depends on
  `AssetForge`, `AssetCore`, `Core`, `CoreDObject`, `Engine`, and `TextureBuild`,
  and privately on the mesh, skeletal, and terrain build modules.
- Texture2D's removable framework surface is
  `Texture2DImportProvider.cpp`, `Texture2DImportProviderSchema.h`, the
  Texture2D registrations in `AssetForgeBuiltinsProviders.cpp`, generic
  request/provenance APIs in `Texture2DImport.h`, and the `FImportHandle`
  recovery map in `Texture2DPostLoad.cpp`. Direct callers are the texture
  import dialog, texture editor module/widget/property extension, source
  relocation/replacement code, source-reference inspection, and the focused
  texture, package, Content Browser, and editor smoke tests.
- Six repository-authored Texture2D packages contain the legacy
  `FTexture2DSourceImportData`/`FSourcePath` schema: `TEX_StoneHead` and the five
  Vintage Lighter textures. Their encoded sources are present under
  `Sandbox/Content/Textures/Textures` and
  `Sandbox/Content/Sources/Models/vintage_lighter_1k`; they are regenerable and
  require no general compatibility reader. Other authored packages only
  reference Texture2D assets.
- `FSourceFile::Filename` is a bounded normalized generic string. Files below
  `FPaths::ProjectDir()` persist as project-relative paths; other files persist
  as normalized absolute paths. Relative values may not be empty, absolute,
  dot, or escape through `..`; resolution is only against the project
  directory. Hash is the change authority, while size and timestamp are hints.
- `DTexture2DImportData` stores only the common single `source` entry and the
  decoder id/version. Usage, color space, resolution, compression, alpha-mip
  mode, and coverage threshold remain solely on `DTexture2D`; destination and
  cancellation are transient call state.
- Direct import performs destination validation, one immutable file capture,
  decode, detached TextureBuild, new-asset creation/publication, family import
  data attachment, then package save. Direct replacement and reimport prepare
  a complete candidate before exchanging live state. Publication is the
  non-cancelable boundary; save failure leaves the published package Dirty.
- Settings changes and missing/corrupt DDC use
  `SubmitTexture2DCompilation`; Texture2D post-load no longer submits
  `EImportMode::Recover`. Source repair is replaced by explicit source-file
  selection and is not a derived-data operation.
- During Milestone A, `AssetForgeBuiltins` continues to depend on `AssetForge`
  for unmigrated families, but Texture2D public and private code may not include
  or call its requests, graphs, registries, jobs, operations, or replay types.
- Retained tests cover decode/build/DDC/cook, import/reimport, atomic failure,
  package persistence, family compilation, source inspection, and editor
  dispatch. Tests specifying mounted-source ingest, relocation, shared
  replacement, repair, provider selection, generic handles, or Texture2D
  replay are rewritten to the filename/direct-family contract or removed;
  unrelated family framework tests remain for later milestones.

## Completion Evidence

- The final implementation removes 2,239 lines while adding 766 across the
  complete change. `AssetForgeBuiltins` falls from 61 files and 13,689 lines to
  59 files and 12,931 lines. Its Texture2D provider, provider schema, and build
  adapter are deleted; the new family import-data class accounts for the one
  added public header. `AssetForge` remains 29 files and 6,155 lines because
  its generic implementation is still required by the five unmigrated
  families and Scene.
- Boundary searches find no generic request, service, graph, job, operation,
  provenance, provider, mounted-source mutation, or `EImportMode` dependency
  in the direct Texture2D importer and its Engine publication/post-load code.
  Residual occurrences in TextureEditor belong to TextureCube and
  VolumeTexture, and residual Texture2D mentions in generic provider code are
  Scene output construction rather than standalone Texture2D import.
- Focused validation passes: `AssetImportDataTests` 4/4,
  `TextureTests` 77/77, `AssetPackageTests` 125/125,
  `TextureThumbnailTests` 9/9, and `EditorAssetWorkflowTests` 35/35. The
  default `all` Editor build passes, as does a non-sandbox hidden-window
  startup/shutdown smoke using 8 ticks.
- Cook stripping and runtime closure are covered by the Texture2D deterministic
  Cook/load tests: cooked packages contain neither `AssetImportData` nor source
  filenames/hashes and load without source files, DDC, AssetForge, or offline
  image decoding.
- AssetForge and AssetForgeBuiltins both remain required by multiple
  unmigrated families; no shared helper was introduced solely to replace a
  deleted Texture2D abstraction. AssetForgeBuiltins is loaded before the first
  editor catalog scan so its concrete editor-only import-data class is present
  for construct-free reference indexing.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory every Texture2D production/test include, public entrypoint,
  provider registration, graph node/payload, planning pass, builder adapter,
  component identity, generic job/operation call, mounted-source operation,
  recovery feature, provenance field, import-data object, module dependency,
  and editor caller.
- [x] Inventory repository-authored Texture2D packages and fixtures; record
  which current source/provenance schemas they contain and whether each can be
  regenerated or canonical-resaved.
- [x] Measure the current AssetForge/AssetForgeBuiltins public and private file,
  line, symbol, and target-dependency baseline so later milestones can prove a
  real reduction rather than a vocabulary move.
- [x] Finalize the exact Engine `FSourceFile` filename representation,
  project-relative conversion, absolute-path handling, normalization,
  resolution, hash/timestamp semantics, validation bounds, serialization, and
  construct-free inspection behavior.
- [x] Finalize `DTexture2DImportData` fields and ownership; map every retained
  setting to either the asset, family import data, or transient request and
  reject duplicate authorities.
- [x] Specify the direct Texture2D synchronous import/reimport sequence,
  family-compilation asynchronous seam, cancellation boundary, publication,
  save result, and source-change diagnostics.
- [x] Map every Texture2D `Recover` caller and test to family Build/DDC behavior
  or explicitly remove unsupported behavior; distinguish source repair from
  disposable-data reconstruction.
- [x] Select the exact temporary build direction that allows Texture2D to
  bypass the framework while other families continue to compile, without
  introducing a replacement service or registry.
- [x] Classify existing tests as retained, rewritten, moved to a later child
  plan, or deleted because they exclusively specify retired mounted-source or
  generic-framework behavior.

#### Acceptance Gate

- The source filename format, family import-data schema, direct execution and
  publication sequence, async/recovery mapping, authored-content disposition,
  temporary dependency direction, deletion inventory, and validation set have
  no unresolved ownership or compatibility decision.

### Stage 1: Establish lightweight source metadata and capture

Dependencies: Stage 0 complete.

- [x] Change Engine common import metadata from mounted `FSourcePath` to the
  selected filename value while preserving reflected ownership, validation,
  cloning, authored round trip, editor-only stripping, and construct-free
  source inspection.
- [x] Implement deterministic project-relative persistence, normalized absolute
  fallback, resolution, missing-file classification, changed-hash diagnostics,
  and bounded capture without any mount lookup or source mutation.
- [x] Adapt source-reference inspection/indexing only to the minimum needed to
  read common filenames; do not retain relocation, reverse mutation, or shared
  source transaction semantics under a new name.
- [x] Add focused common-metadata tests for project relocation, external
  absolute sources, traversal rejection, missing files, changed content,
  multiple labeled sources, clone-to-owner, authored save/load, inspection,
  and Cook stripping.
- [x] Update or regenerate repository fixtures selected by Stage 0 and prove a
  second canonical load/save requires no mutation.

#### Acceptance Gate

- Common import metadata stores and inspects valid lightweight filenames,
  rejects unsafe/invalid values, survives authored round trips and project
  relocation as selected, and remains absent from Cooked packages without
  requiring Asset mounts or AssetForge replay types.

### Stage 2: Cut Texture2D over to its direct importer

Dependencies: Stage 1 complete and common filename behavior qualified.

- [x] Add `DTexture2DImportData` and its bounded value conversion, validation,
  clone-to-owner, legacy disposition, and editor-only persistence tests.
- [x] Implement one direct Texture2D import path that validates an input file
  and destination, captures immutable bytes, decodes once, invokes TextureBuild,
  publishes content plus import data on the editor thread, and reports package
  persistence independently.
- [x] Implement direct Texture2D reimport and explicit source-file replacement
  from the persisted filename and current asset settings, with no source copy,
  move, relocation, or mount dependency behavior.
- [x] Route settings-driven rebuild and missing/corrupt DDC reconstruction
  through Texture2D compilation; remove Texture2D `EImportMode::Recover`,
  generic request, operation, and framework progress ownership.
- [x] Convert DurinEd Texture2D dialog and Content Browser actions to explicit
  family entrypoints and observe only the retained family compilation state.
- [x] Remove Texture2D provider registration, provider schema, generic build
  adapter, graph/planning payload construction, generic provenance conversion,
  mounted-source mutation entrypoints, and now-dead recovery feature adapters.
- [x] Update or delete tests according to the Stage 0 classification and add
  failure-injection coverage for decode/build failure, cancellation before
  publication, save failure after publication, missing source, changed source,
  and corrupt/missing DDC.

#### Acceptance Gate

- Texture2D import, reimport, source replacement, settings rebuild, and DDC
  reconstruction use only the direct family path; no Texture2D production code
  references generic graphs, passes, registries, leases, jobs, replay state,
  mounted-source mutation, or `Recover`.
- Existing asset state is preserved on every pre-publication failure, save
  failure leaves valid published state Dirty, and source/cooked/runtime
  behavior matches the selected contracts.

### Stage 3: Qualify the pattern and hand off later families

Dependencies: Stage 2 complete.

- [x] Run the focused common import-data, Asset package/inspection/Cook,
  TextureBuild, Texture2D import/reimport, compilation, DDC corruption,
  Content Browser, dialog, and source-index selections using the repository
  testing workflow.
- [x] Build the affected standalone modules and default Editor target, then run
  the selected hidden-window startup/shutdown smoke after focused tests pass.
- [x] Qualify Cook and runtime-only target closure: source filenames, concrete
  editor import data, AssetForge/AssetForgeBuiltins, and offline image import
  code must not become runtime requirements.
- [x] Audit public headers, module descriptors, generated reflected code,
  startup/shutdown ownership, and target closure for the selected dependency
  direction and absence of a replacement generic framework.
- [x] Record before/after files, lines, symbols, dependencies, and Texture2D
  framework references; explain any retained AssetForge helper with at least
  two concrete consumers or remove it.
- [x] Update lasting source-data, asset-import, Texture2D, editor workflow, and
  module documentation only with implemented contracts. Leave generic
  architecture and mounted-source documents in place only for unmigrated
  families and mark their narrowed applicability accurately.
- [x] Update the roadmap status and create the next child plan only after this
  plan's exit gate is satisfied.
- [x] Run changed/all documentation, all-plan, and all-roadmap validation and
  record exact validation evidence before completion.

#### Acceptance Gate

- Focused tests, affected modules, Editor build/smoke, Cook/runtime closure,
  boundary searches, authored-content audit, and documentation validators pass.
- The direct Texture2D implementation is smaller and has fewer dependencies
  than its removed framework path, and the next child plan can copy its
  decisions without inventing a new common protocol.

## Validation Matrix

Follow [Agent Build And Run](../../../Agents/BuildAndRun.md) and [Agent
Testing](../Agents/Testing.md); run focused validation before aggregates and do
not overlap build process trees.

| Concern | Required evidence |
| --- | --- |
| Filename model | Project-relative and external absolute round trip, project relocation, normalization/traversal rejection, missing/changed diagnostics, and construct-free inspection |
| Object lifecycle | Family import data owns the correct Outer/strong reference, clones to the live asset, survives authored save/load, and is pruned from Cooked output |
| Immutable capture | Hash, decode, build key, and publication consume one captured byte buffer even if the physical file changes during the attempt |
| Direct import | New asset creation, collision rejection, decode/build failure, publication, save success, and save failure have explicit results |
| Reimport | Persisted filename plus current settings rebuilds the existing asset; a failed attempt preserves prior content and metadata |
| Source replacement | A newly selected file updates content and metadata atomically without copying, moving, deleting, or mount dependency mutation |
| Derived data | Warm, missing, and corrupt DDC behavior uses Texture2D compilation and does not save or rewrite import metadata during reconstruction |
| Async behavior | Family queue priority, cancellation, completion, and shutdown remain safe without generic import jobs/operation state |
| Editor workflow | Dialog and Content Browser actions invoke Texture2D directly and present retained diagnostics/progress without mounted-source destinations |
| Compatibility/content | Repository Texture2D assets are regenerated/resaved or use one bounded documented reader; new saves contain no generic replay or mounted-source schema |
| Deployment | Cooked/runtime targets contain no editor import metadata, AssetForge dependency, offline image decoder, or source fallback |
| Reduction | Before/after file, line, symbol, dependency, and reference evidence proves removal rather than relocation of framework complexity |

## Definition of Done

- The exact thin AssetForge boundary and direct family-import pattern are
  implemented and recorded in the roadmap.
- Engine common import metadata uses the selected lightweight source filename
  model and has no mounted-source or AssetForge replay dependency.
- `DTexture2DImportData` is the only Texture2D replay/source authority; editable
  build settings remain on `DTexture2D` without duplicate copies.
- All Texture2D import, reimport, source replacement, settings rebuild, and DDC
  reconstruction paths bypass generic translators, graphs, planning passes,
  builders, registries, leases, jobs, operations, and replay state.
- Texture2D no longer exposes mounted-source ingest, relocation, shared-source
  replacement/repair, or `EImportMode::Recover` behavior.
- Failure/cancellation, publication, save, Dirty-package, DDC, Cook stripping,
  and runtime closure match the selected invariants and tests.
- Repository-authored Texture2D content contains only supported current schema;
  no indefinite dual-write or generic compatibility subsystem remains.
- Focused and aggregate validation, affected builds, Editor smoke, boundary
  searches, reduction evidence, and documentation validators pass.
- Lasting documentation reflects the implemented Texture2D boundary and the
  roadmap identifies the next ready child plan.

## Deferred Follow-ups

- Migrate TerrainHeightmap, standalone StaticMesh, TextureCube, and
  VolumeTexture through the next roadmap child plan.
- Replace Scene's public graph/planning execution with private transient
  ordering after single-output families are complete.
- Remove generic framework files, service/module startup, remaining component
  registrations, and generic tests only after all production families migrate.
- Remove the remaining mounted-source management framework and editor workflow
  after no importer depends on it.
- Consider auto-reimport watch directories only from a separate measured editor
  requirement; do not retain mounted-source management as its prerequisite.
- Introduce third-party importer extensibility only with a concrete plugin
  requirement and a separate design; do not pre-pay registration or lease
  complexity in this roadmap.

## Related Documentation

- [Asset Import Simplification Roadmap](../../../Roadmaps/Archive/2026-08/AssetImportSimplification.md)
- [Asset Import Framework](../../../Editor/Architecture/AssetImportFramework.md)
- [Source File Workflows](../../../Editor/Guides/SourceFileWorkflows.md)
- [Asset Data Lifecycle and Storage](../../../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../../../Runtime/Assets/AssetCompilation.md)
- [Async Asset Operations](../../../Editor/Architecture/AsyncAssetOperations.md)
- [Code Modules](../../../Workspace/CodeModules.md)
- [Agent Build And Run](../../../Agents/BuildAndRun.md)
- [Agent Testing](../../../Agents/Testing.md)

## Related Code

- [`AssetImportData.h`](../../../../Engine/Source/Runtime/Engine/Public/Asset/AssetImportData.h)
- [`SourceHint.h`](../../../../Engine/Source/Runtime/Engine/Public/Asset/SourceHint.h)
- [`Texture2DImport.h`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Public/AssetForge/Builtins/Texture2DImport.h)
- [`Texture2DImportData.h`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Public/AssetForge/Builtins/Texture2DImportData.h)
- [`Texture2DImport.cpp`](../../../../Engine/Source/Editor/AssetForgeBuiltins/Private/Texture2DImport.cpp)
- [`Texture2D.cpp`](../../../../Engine/Source/Runtime/Engine/Private/Texture/Texture2D.cpp)
- [`ImportDialogSupport.cpp`](../../../../Engine/Source/Editor/DurinEd/Private/Import/ImportDialogSupport.cpp)
- [`SourceReferenceIndex.cpp`](../../../../Engine/Source/Editor/DurinEd/Private/Source/SourceReferenceIndex.cpp)
