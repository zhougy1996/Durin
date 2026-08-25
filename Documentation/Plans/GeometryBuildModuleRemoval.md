# Geometry Build Module Removal Plan

Summary: Split StaticMesh and skeletal-animation build ownership into StaticMeshBuild and SkeletalBuild, then remove GeometryBuild.

Last reviewed: 2026-08-26

Status: Completed
Completed: 2026-08-26

## Current Status

All stages completed on 2026-08-26. `StaticMeshBuild` now owns StaticMesh
render/collision recipes and collision-provider registration; `SkeletalBuild`
owns SkeletalMesh/AnimationClip recipes and uncooked-payload provider
registration. Each module has an independent two-function transaction and no
dependency on the other. The `GeometryBuild` source root, module target,
project mapping, startup load, and active consumer dependencies are removed,
while all persisted `Durin.GeometryBuild.*` identities remain unchanged.

The Debug Editor configuration and full `all` build pass. Independent module
builds pass, as do StaticMeshTests 74/74, SkeletalAssetTests 34/34, and the
focused split-module unload/reload lifecycle case 1/1. Changed-document,
all-plan, and all-document validation pass.

## Goal

Remove `GeometryBuild` after establishing independently loadable
`StaticMeshBuild` and `SkeletalBuild` developer modules with explicit consumer,
test, project, and runtime-variant ownership.

## Scope

- Move StaticMesh render/collision recipes, keys, operations, registration, and
  collision modular-feature ownership into `StaticMeshBuild`.
- Move SkeletalMesh/AnimationClip recipes, keys, operations, registration, and
  uncooked-payload modular-feature ownership into `SkeletalBuild`.
- Replace every active project, target, startup, test, and documentation
  contract that selects `GeometryBuild`.
- Preserve derived-data key, payload, and function-identity compatibility.

## Non-Goals

- Splitting SkeletalMesh and AnimationClip into separate developer modules.
- Renaming the persisted `Durin.GeometryBuild.*` Build function identities.
- Changing Runtime asset payload formats, import normalization, or cook policy.
- Introducing a compatibility `GeometryBuild` facade or forwarding module.

## Design Decisions and Invariants

- `StaticMeshBuild` and `SkeletalBuild` depend publicly on `Engine` and
  privately on `Core` and `DerivedDataCache`; neither depends on the other.
- Each module owns one atomic two-function registration transaction and its own
  module-owned callback gate.
- `StaticMeshBuild` is the sole provider of `IStaticMeshCollisionBuildFeature`.
- `SkeletalBuild` is the sole provider of `ISkeletalDerivedDataFeature`; the
  existing combined SkeletalMesh/AnimationClip Runtime interface remains
  intact because both payload families share skeleton compatibility and
  serialization context.
- Existing `Durin.GeometryBuild.StaticMesh`,
  `Durin.GeometryBuild.StaticMeshCollision`,
  `Durin.GeometryBuild.SkeletalMesh`, and
  `Durin.GeometryBuild.AnimationClip` identities remain stable.
- Consumers select only the new modules they directly compile against or load;
  mixed scene import and editor startup select both explicitly.

## Current Foundations and Gaps

- Terrain build ownership has already moved to independent `TerrainBuild`.
- StaticMesh and Skeletal implementation files already occupy separate source
  subtrees and register separate function pairs.
- The remaining gap is the shared `GeometryBuildFunctionRegistry`, module
  object, API macro, project mapping, startup loads, and broad test linkage.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Confirm scope, dependencies, and selected design.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Extract StaticMeshBuild

- [x] Create the module descriptor, API, PCH, registration transaction, and
  module lifecycle owner.
- [x] Move StaticMesh public/private sources and collision feature ownership.
- [x] Redirect StaticMesh consumers and focused test targets.

#### Acceptance Gate

- `StaticMeshBuild` compiles independently, owns exactly two registered
  functions, and has no dependency on `SkeletalBuild`.

### Stage 2: Establish SkeletalBuild and remove GeometryBuild

- [x] Create the Skeletal module descriptor, API, registration transaction,
  and module lifecycle owner.
- [x] Move SkeletalMesh/AnimationClip public/private sources and uncooked
  payload feature ownership.
- [x] Replace mixed consumers and startup loads with explicit new modules.
- [x] Remove every source, project, and target declaration for `GeometryBuild`.

#### Acceptance Gate

- `SkeletalBuild` compiles independently, owns exactly two registered
  functions, and no configured module or target named `GeometryBuild` remains.

### Stage 3: Validate contracts and lifecycle behavior

- [x] Configure the selected editor preset and build both new modules.
- [x] Build affected import, editor, program, and native-test targets.
- [x] Run focused StaticMesh, skeletal asset, and module unload tests.
- [x] Run the full `all` build and required documentation validators.

#### Acceptance Gate

- Selected focused tests pass, the full build passes, documentation validates,
  and static audits find only intentionally retained persisted identities or
  historical references.

## Validation Matrix

| Concern | Validation |
| --- | --- |
| Independent provider boundaries | Build `StaticMeshBuild` and `SkeletalBuild` separately; audit dependency closures |
| StaticMesh render/collision behavior | Focused StaticMesh, physics, viewport, and material tests selected from the configured registry |
| SkeletalMesh/AnimationClip behavior | Focused skeletal asset tests |
| Mixed import and startup selection | Build `AssetForgeBuiltins`, `MainFrame`, and `DurinAssetTool`; run relevant import/lifecycle tests |
| Project and Runtime variants | Configure plus root dependency-closure assertions |
| Repository integration | Full `all` build |
| Documentation | Changed-document, all-plan, and all-document validation |

## Definition of Done

- `StaticMeshBuild` solely owns StaticMesh render/collision authoring builds.
- `SkeletalBuild` solely owns SkeletalMesh/AnimationClip authoring builds.
- The new modules have independent registration, startup, shutdown, and unload
  lifecycles with no dependency on each other.
- Persisted Build identities and Runtime payload behavior remain compatible.
- `GeometryBuild` has no source root, descriptor, CMake target, project mapping,
  startup load, active consumer dependency, or active documentation ownership.
- Required builds, tests, documentation validation, and static audits pass.
- The completed plan and implementation are committed together with exact plan
  and stage provenance.

## Deferred Follow-ups

- Split `ISkeletalDerivedDataFeature` and extract a separate `AnimationBuild`
  only if independent animation deployment or ownership becomes necessary.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Skeletal Animation Playback](../Runtime/Animation/SkeletalAnimationPlayback.md)
- [Skeletal Mesh Rendering](../Runtime/Rendering/SkeletalMeshRendering.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Terrain Build Module Extraction](TerrainBuildModuleExtraction.md)

## Related Code

- [`StaticMeshBuild`](../../Engine/Source/Developer/StaticMeshBuild)
- [`SkeletalBuild`](../../Engine/Source/Developer/SkeletalBuild)
- [`AssetForgeBuiltins`](../../Engine/Source/Editor/AssetForgeBuiltins)
- [`Engine project descriptor`](../../Engine/Engine.dproject)
- [`Engine native tests`](../../Engine/Tests/Native/EngineTests)
