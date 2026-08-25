# Terrain Build Module Extraction Plan

Summary: Extract Terrain heightmap and Terrain World build, DDC, and cook ownership from GeometryBuild into a dedicated TerrainBuild developer module.

Last reviewed: 2026-08-26

Status: Completed
Completed: 2026-08-26

## Current Status

All stages are complete. `TerrainBuild` now owns the unchanged Heightmap and
Terrain World public contracts, implementations, Cook production, and one
module-owned six-function registration transaction. `GeometryBuild` independently
owns StaticMesh and Skeletal/Animation registration, while Runtime Terrain assets
and Engine integration remain in `Engine`.

Configuration and the full Debug Editor `all` build pass. Focused validation
passes TerrainHeightmapTests 11/11, TerrainHeightmapCookTests 1/1,
TerrainWorldBuildTests 15/15, StaticMeshTests 74/74, SkeletalAssetTests 34/34,
and TextureTests 87/87 including independent GeometryBuild/TerrainBuild
unload-reload coverage. Changed-document and all-plan validation pass.

## Goal

Give Terrain an independently selectable Developer build module while preserving
Heightmap and Terrain World keys, payloads, DDC behavior, cook formats, function
identities, module-retirement safety, and existing Runtime publication behavior.

## Scope

- Add a `TerrainBuild` Developer module with `Engine` as its public dependency
  and `Core` plus `DerivedDataCache` as private dependencies.
- Move the existing GeometryBuild public and private Terrain sources without
  changing their namespaces or serialized contracts.
- Give TerrainBuild an independent module-owned transaction for the
  TerrainHeightmap function and five Terrain World product functions.
- Reduce GeometryBuild registration and module ownership to StaticMesh,
  SkeletalMesh, and AnimationClip.
- Update project selection, editor/tool startup, consumer descriptors, tests,
  dependency-closure assertions, and lasting documentation.

## Non-Goals

- Create a Runtime `Terrain` module or move `DTerrainHeightmap`,
  `DTerrainComponent`, `ATerrainActor`, or rendering code out of existing Runtime
  modules.
- Split StaticMesh from SkeletalMesh/AnimationClip.
- Change Build Framework, DDC backend, Terrain payload schemas, cache buckets,
  build keys, cook manifests, or runtime loading behavior.
- Rename the existing registered build-function identities and intentionally
  invalidate disposable cache entries.

## Design Decisions and Invariants

- `DerivedDataCache` retains only the family-neutral Build Framework; all typed
  Terrain inputs, functions, codecs, policy, and reconstruction remain in
  `TerrainBuild`.
- Heightmap and Terrain World remain together because they share one Terrain
  authoring/cook domain and target selection, despite having distinct payloads.
- TerrainBuild owns exactly six function registrations and releases them in
  reverse acquisition order before its module callback gate retires.
- GeometryBuild and TerrainBuild have no dependency on each other; consumers
  declare only the modules whose public headers or startup registration they use.
- Existing `Durin.GeometryBuild.Terrain...` function identities remain stable in
  this extraction because they are persisted production identity, not module UI.

## Current Foundations and Gaps

- GeometryBuild currently registers two StaticMesh, two Skeletal/Animation, one
  TerrainHeightmap, and five Terrain World functions as one transaction.
- All Terrain implementation and public headers already occupy isolated
  `Private/Terrain` and `Public/Terrain` directories.
- AssetForgeBuiltins and LevelEditor consume Terrain public APIs, while MainFrame
  and DurinAssetTool load the provider module for startup registration.
- Test targets currently link GeometryBuild for both geometry and Terrain cases,
  and root target-closure assertions do not know TerrainBuild.

## Implementation Stages

### Stage 0: Fix the extraction boundary

- [x] Inventory Terrain sources, registrations, consumers, tests, and build metadata.
- [x] Select a Developer-only extraction that preserves Runtime ownership and wire formats.

#### Acceptance Gate

- The module boundary, identity compatibility, dependencies, and non-goals are explicit.

### Stage 1: Create TerrainBuild and split provider registration

- [x] Add the module descriptor, API surface, PCH, module entry point, and function registry.
- [x] Move Heightmap and Terrain World sources to TerrainBuild and update export macros.
- [x] Remove all Terrain registration and source ownership from GeometryBuild.

#### Acceptance Gate

- GeometryBuild and TerrainBuild compile independently and neither depends on the other.

### Stage 2: Rewire consumers and target metadata

- [x] Register TerrainBuild in the project and selected editor target.
- [x] Update AssetForgeBuiltins, LevelEditor, MainFrame, DurinAssetTool, test targets,
  and dependency-closure assertions to declare the correct module.
- [x] Remove stale GeometryBuild linkage from Terrain-only test targets.

#### Acceptance Gate

- Configure succeeds, module closure is correct, and every Terrain public-header
  consumer links or loads TerrainBuild explicitly.

### Stage 3: Validate contracts and complete the extraction

- [x] Build TerrainBuild, GeometryBuild, dependent editor/import modules, and full `all`.
- [x] Run focused Terrain Heightmap, Terrain World, StaticMesh, and Skeletal coverage
  selected through the native-test registry.
- [x] Update and validate lasting architecture, workspace, build, and Terrain documents.

#### Acceptance Gate

- Focused tests, full build, documentation validation, and static ownership audits pass.

## Validation Matrix

| Change | Validation |
| --- | --- |
| Provider split and exports | Build `TerrainBuild` and `GeometryBuild` independently |
| Heightmap recipes and publication | Focused Terrain Heightmap build/cook tests |
| Terrain World functions and cook | `TerrainWorldBuildTests` |
| Remaining GeometryBuild behavior | Focused StaticMesh and Skeletal tests |
| Project graph and startup selection | Configure, dependent module builds, and full `all` build |
| Documentation and plan lifecycle | Changed-document validation and all-plan validation |

## Definition of Done

- `TerrainBuild` is the sole owner of Developer Terrain build and cook sources.
- GeometryBuild owns no Terrain headers, sources, registrations, or documentation contract.
- Typed Terrain identities, payloads, cache behavior, cook formats, and Runtime behavior remain compatible.
- Consumers, test targets, project selection, and startup loads declare the new module explicitly.
- Required focused tests, full build, documentation validators, and static audits pass.
- The completed plan and implementation are committed together with exact plan and stage provenance.

## Deferred Follow-ups

- A dedicated Runtime `Terrain` module requires first removing Engine's direct
  Terrain component special cases and is intentionally a separate plan.
- StaticMesh and Skeletal/Animation were separated into `StaticMeshBuild` and
  `SkeletalBuild` by the follow-up Geometry Build Module Removal plan.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Terrain World Data](../Runtime/Terrain/TerrainWorldData.md)
- [Terrain Heightmap Asset](../Runtime/Terrain/TerrainHeightmapAsset.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build System](../Development/Build/BuildSystem.md)

## Related Code

- [`StaticMeshBuild`](../../Engine/Source/Developer/StaticMeshBuild)
- [`SkeletalBuild`](../../Engine/Source/Developer/SkeletalBuild)
- [`Engine project descriptor`](../../Engine/Engine.dproject)
- [`AssetForgeBuiltins`](../../Engine/Source/Editor/AssetForgeBuiltins)
- [`Terrain tests`](../../Engine/Tests/Native/EngineTests/Private/Terrain)
