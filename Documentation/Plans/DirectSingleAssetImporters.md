# Direct Single-Asset Importers Plan

Summary: Migrate TerrainHeightmap, StaticMesh, TextureCube, and VolumeTexture to family-owned import and rebuild paths

Last reviewed: 2026-08-27

Status: Completed
Completed: 2026-08-27

## Current Status

All five stages are complete. TerrainHeightmap, standalone StaticMesh,
TextureCube, and VolumeTexture now own direct filename-based import, reimport,
source selection, publication, and family build recovery. Scene is the only
remaining production caller of generic AssetForge orchestration.

Six StaticMesh, one TextureCube, and two VolumeTexture packages were migrated
to concrete family import data. Engine's 6 packages and Sandbox's 25 packages
all audit `Compatible`/`Current` with no findings or deprecated-route evidence;
project-scope canonical-resave CI reports 0 ready and 0 blocked for both.
Temporary deprecated read routes were removed before the final audit.

Relative to rebased Milestone A (`2dc84779`), source and tests changed across
72 files: 2,075 lines were added, 4,477 removed, for a net reduction of 2,402
lines. Eleven source/test files were deleted, including all four standalone providers,
the shared image-provider bridge, Terrain adapter, and Texture relocation
workflow. Boundary searches leave `EImportMode::Recover` only in the generic
framework retained for Scene and no generic request/graph/job use in the four
direct importer/recovery implementations.

Focused validation passed: TextureTests 76/76, StaticMeshTests 74/74,
TerrainHeightmapTests 11/11, AssetImportDataTests 4/4, AssetImportTests 17/17,
AssetPackageTests 125/125, AssetCookTests 13/13,
TerrainHeightmapCookTests 1/1, EditorAssetWorkflowTests 35/35, and
SceneImportTests 4/4. The default Debug Editor `all` build and the Sandbox
hidden-window 8-tick smoke passed; the final run completed in 5.90 seconds in an
authorized macOS application context. Cook tests strip concrete import data,
and the runtime Engine binary/module boundary has no AssetForge,
AssetForgeBuiltins, offline decoder, or developer build-module link.

## Goal

Make TerrainHeightmap, standalone StaticMesh, TextureCube, and VolumeTexture
own their source filenames, source interpretation settings, direct import and
reimport execution, publication, and disposable-data reconstruction without
AssetForge providers, graphs, jobs, operations, replay provenance, mounted
source mutation, or `Recover` mode.

## Scope

- Add one concrete editor-only `DAssetImportData` type per remaining standalone
  family, using common labeled filenames and only family source-interpretation
  fields that are not already authoritative on the asset.
- Convert physical-file capture, import, reimport, and explicit source
  replacement to direct family functions that preserve the Texture2D
  publication and save boundary.
- Replace provider-based missing/corrupt DDC recovery with each family's build
  or post-load domain.
- Remove the four provider registrations, provider schemas, build adapters,
  generic provenance conversion, mounted-source operations, and obsolete UI
  operation handles once callers move.
- Canonically migrate repository-owned StaticMesh, TextureCube, and
  VolumeTexture packages; TerrainHeightmap requires tests but no authored
  package migration.

## Non-Goals

- Migrating Scene, skeletal Scene outputs, animation, or generated materials.
- Removing generic AssetForge while Scene still requires its graph and job.
- Redesigning normalized build inputs, DDC keys, cooked payloads, or runtime
  resources.
- Introducing a family registry, generic direct-import interface, or new
  asynchronous operation abstraction.
- Preserving source copy, relocation, shared replacement, or repair workflows
  for migrated families.

## Design Decisions and Invariants

- Engine assets own only an editor-only `DAssetImportData` base reference;
  concrete classes and source decoding remain in AssetForgeBuiltins.
- Common `FSourceFile` labels are stable per family: `source` for StaticMesh,
  VolumeTexture, and TerrainHeightmap; `panorama` or the six canonical face
  roles for TextureCube. Every filename follows the Milestone A relative/
  absolute policy and every attempt captures each file once.
- StaticMesh axis conversion, Terrain raw/image interpretation, Volume atlas
  interpretation, and TextureCube decoder/layout information live in family
  import data only when required to decode the encoded source again. Runtime
  build settings remain on the asset and are not duplicated.
- Each direct importer prepares all decoded and built state before mutating a
  live asset. Publication is non-cancelable; save failure leaves the valid
  package Dirty. Source replacement never copies or moves a physical file.
- Disposable-data recovery may read retained normalized editor source payloads
  or recapture the persisted filename according to existing family capability,
  but it never submits an import request, rewrites source metadata, or saves.
- Temporary deprecated read routes are allowed only to migrate the known
  repository packages. They are removed before the final compatibility audit.
- Scene may continue to construct Texture2D and StaticMesh products through
  private generic orchestration, but standalone-family APIs and assets carry no
  generic request/provenance dependency.

## Current Foundations and Gaps

| Family | Foundation | Cutover gap |
| --- | --- | --- |
| TerrainHeightmap | Direct file decode/build helpers, Terrain build/DDC domain, post-load feature | Provider, adapter, provenance, mounted-source UI, and `Recover` requests |
| StaticMesh | Direct Assimp decode and StaticMeshBuild product/publication helpers | 628-line provider plus schema/adapter, generic jobs, provenance, and source mutation feature |
| TextureCube | Direct six-face/panorama validation, TextureBuild product, package save | Provider/adapter, mounted-source dialog modes, provenance, and `Recover` post-load policy |
| VolumeTexture | Direct atlas decode, build/publication, save/reimport helpers | Provider, shared image graph, generic operation handles, provenance, and recovery request |

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Inventory every provider/adapter/schema registration, public generic
  submission/provenance API, mounted-source caller, recovery feature, test, and
  authored package for the four families.
- [x] Freeze each family import-data field set, source labels, direct execution
  order, recovery mapping, and temporary compatibility disposition.
- [x] Record baseline files/lines/dependencies and classify shared helpers as
  retained for Scene, made direct, or deleted.

#### Acceptance Gate

- The four family boundaries, authored migration, caller map, deletion list,
  and validation selections have no unresolved ownership decision.

### Stage 1: Establish family import data and direct capture

Dependencies: Stage 0 complete.

- [x] Add and qualify the four concrete import-data classes, labeled common
  filename state, validation, clone-to-owner, authored persistence, and Cook
  stripping.
- [x] Convert shared physical capture and source diagnostics to filenames
  without package mounts or source mutation.
- [x] Attach family import data at each existing narrow publication seam and
  remove duplicate authorities from current execution; deprecated Engine fields
  remain read/write only for the bounded authored-package migration in Stage 4.

#### Acceptance Gate

- Every family can persist, inspect, clone, and Cook-strip current filename
  metadata without generic replay data or mounted source paths.

### Stage 2: Cut over TerrainHeightmap and VolumeTexture

Dependencies: Stage 1 complete.

- [x] Convert import, reimport, source selection, dialog submission, and package
  save to direct TerrainHeightmap and VolumeTexture calls.
- [x] Route missing/corrupt disposable data through TerrainBuild and
  TextureBuild family paths without `EImportMode::Recover`.
- [x] Remove both providers, Terrain adapter, registrations, mounted-source
  mutation paths, generic provenance APIs, and retired tests.

#### Acceptance Gate

- TerrainHeightmap and VolumeTexture standalone production paths have no
  generic graph/job/provider/replay/mounted-source dependency and preserve
  transactional publication behavior.

### Stage 3: Cut over StaticMesh and TextureCube

Dependencies: Stage 2 complete.

- [x] Convert direct import, reimport, source selection, dialogs, Content
  Browser actions, and package save for StaticMesh and TextureCube.
- [x] Replace their provider-based DDC recovery with family build/post-load
  behavior and remove `Recover` requests.
- [x] Remove providers, schemas, adapters, registrations, generic provenance,
  mounted-source operations, and obsolete tests.

#### Acceptance Gate

- All four standalone families bypass generic orchestration; only Scene may
  retain graph/provider/job code.

### Stage 4: Migrate content and qualify Milestone B

Dependencies: Stage 3 complete.

- [x] Canonically migrate the known repository packages, remove temporary read
  routes, prove a project-scope resave no-op, and pass the compatibility audit.
- [x] Run focused family/import/package/Cook/editor tests, default Editor build
  and hidden-window smoke, and runtime/Cook dependency closure checks.
- [x] Record reduction metrics and boundary searches, update lasting docs and
  the roadmap, create the Scene milestone plan, and run all documentation
  validators.

#### Acceptance Gate

- Milestone B tests, builds, smoke, content audit, dependency closure, removal
  searches, reduction evidence, and documentation validation pass.

## Validation Matrix

| Concern | Required evidence |
| --- | --- |
| Metadata | Single and multi-source filenames, family settings, ownership, clone, save/load, inspection, Cook stripping |
| Direct import | New import, collision, decode/build failure, publication, save success/failure for each family |
| Reimport/source replacement | Persisted filenames plus current settings; failed attempts preserve live and disk state |
| Recovery | Warm/missing/corrupt DDC uses family build behavior and never rewrites import metadata |
| Scene isolation | Scene creation still works while standalone providers and schemas are absent |
| Content | Authored packages use only current family import data and pass compatibility/no-op resave |
| Deployment | Cooked/runtime targets have no editor import data, AssetForge, decoder, or source fallback dependency |
| Removal | Searches leave generic orchestration only in Scene and framework code required by Scene |

## Definition of Done

- TerrainHeightmap, StaticMesh, TextureCube, and VolumeTexture use direct
  family-owned import, reimport, source selection, and build recovery.
- Their Engine assets contain only one editor source authority through common
  import data and no generic provenance or mounted `FSourcePath` schema.
- Standalone providers, schemas, adapters, registrations, jobs, operations,
  mounted-source workflows, and `Recover` calls are removed.
- Repository content, focused/aggregate tests, Editor build/smoke, Cook/runtime
  closure, reduction evidence, and documentation validators pass.
- The roadmap activates the Scene/editor workflow child plan.

## Deferred Follow-ups

- Replace Scene's public source/build graph and provider orchestration with
  private transient ordering in the next milestone.
- Remove AssetForge framework modules and remaining mounted-source management
  only after Scene and editor workflow cutover.

## Related Documentation

- [Asset Import Simplification Roadmap](../Roadmaps/AssetImportSimplification.md)
- [AssetForge Simplification Foundation](AssetForgeSimplificationFoundation.md)
- [Asset Import Framework](../Editor/Architecture/AssetImportFramework.md)
- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Agent Build And Run](../Agents/BuildAndRun.md)
- [Agent Testing](../Agents/Testing.md)

## Related Code

- [`StaticMeshImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/StaticMeshImport.cpp)
- [`TerrainHeightmapImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/TerrainHeightmapImport.cpp)
- [`TextureCubeImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/TextureCubeImport.cpp)
- [`VolumeTextureImport.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/VolumeTextureImport.cpp)
- [`AssetForgeBuiltinsModule.cpp`](../../Engine/Source/Editor/AssetForgeBuiltins/Private/AssetForgeBuiltinsModule.cpp)
