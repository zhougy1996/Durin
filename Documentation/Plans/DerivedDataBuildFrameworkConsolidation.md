# Derived Data Build Framework Consolidation Plan

Summary: Consolidate the family-neutral build framework into DerivedDataCache, return Texture2D completion contracts to TextureBuild, and remove AssetBuildCore

Last reviewed: 2026-08-26

Status: Completed
Completed: 2026-08-26

## Current Status

All stages are complete. `DerivedDataCache` now owns the family-neutral Build
Framework under `Durin::DerivedData`; TextureBuild and GeometryBuild consume it
privately while retaining their typed functions and recipes. Texture2D owns its
authoring completion vocabulary, the redundant `AssetBuildCore` module is
deleted, and project, startup, test, closure, and lasting documentation metadata
reflect the consolidated boundary.

Configuration and the full Debug Editor `all` build pass. Focused validation
passes `DerivedDataCacheTests` 10/10, `TextureTests` 87/87, and
`TerrainWorldBuildTests` 15/15. Changed-document validation also passes.

## Goal

Remove the redundant `AssetBuildCore` module while preserving derived-data
behavior, dynamic provider retirement safety, typed recipe ownership, object-aware
asset compilation, runtime-variant closure, and focused test coverage.

## Scope

- Move the family-neutral Build Framework public API and implementation into
  `DerivedDataCache` under the `Durin::DerivedData` namespace.
- Keep Build keys distinct from cache keys and reuse the existing cache facade
  rather than exposing backend details to recipe modules.
- Move the Texture2D asynchronous completion result and callback contract into
  `TextureBuild` with texture-specific names.
- Change `TextureBuild` and `GeometryBuild` to consume `DerivedDataCache`
  privately and remove `AssetBuildCore` from project, editor startup, and tests.
- Update implemented architecture, workspace, and build documentation.

## Non-Goals

- Redesign the synchronous Build Framework as an asynchronous scheduler.
- Add remote execution, build dependency graphs, multi-value outputs, or a new
  cache backend.
- Move typed texture, mesh, skeletal, animation, or terrain recipes out of their
  current provider modules.
- Change `Engine::FAssetCompilingManager` ownership or object publication policy.

## Design Decisions and Invariants

- `DerivedDataCache` owns both opaque cache storage and family-neutral derived
  data construction, matching their shared policy and request boundary.
- Build function implementations remain provider-owned and register through a
  module-owned callback gate; the consolidation must preserve safe DLL retirement.
- `TextureBuild` and `GeometryBuild` public headers do not expose DDC Build
  Framework types, so their DDC dependency is private.
- `Succeeded`, `Failed`, `Canceled`, and `Superseded` describe one Texture2D
  authoring request, not a cache/build-session result.
- `Engine` and `DurinGame` remain independent of Developer DDC/build modules.

## Current Foundations and Gaps

- `DerivedDataCache` already owns the synchronous backend-neutral Get/Put/Trim
  facade and local filesystem backend.
- `AssetBuildCore` contains roughly 740 lines: Build values and keys, immutable
  definitions, policies and results, function registration, and synchronous
  DDC-backed execution.
- `TextureBuild` and `GeometryBuild` publicly depend on `AssetBuildCore`, though
  only Texture2D's public authoring callback currently exposes one of its types.
- `MainFrame` explicitly loads the otherwise empty `AssetBuildCore` module.
- Five focused tests characterize the generic session and policy behavior.

## Implementation Stages

### Stage 0: Define the implementation boundary

- [x] Confirm scope, dependencies, and selected design.

#### Acceptance Gate

- Scope, decisions, and validation requirements are explicit.

### Stage 1: Consolidate the generic framework into DerivedDataCache

- [x] Move and rename Build Framework headers and implementation exports.
- [x] Change generic contracts to `Durin::DerivedData` without changing behavior.
- [x] Move the five focused framework tests to DerivedDataCache ownership.

#### Acceptance Gate

- `DerivedDataCache` exports the complete cache/build contract and its focused
  tests pass without linking `AssetBuildCore`.

### Stage 2: Rewire providers and Texture2D completion

- [x] Update GeometryBuild and TextureBuild function/session consumers.
- [x] Replace the generic async completion surface with Texture2D-owned types.
- [x] Make both recipe modules private consumers of `DerivedDataCache`.

#### Acceptance Gate

- Texture and geometry build targets compile and their focused tests pass with
  unchanged cache hit, local build, validation, cancellation, and publication behavior.

### Stage 3: Remove AssetBuildCore and update lasting contracts

- [x] Remove its source root, project mapping, editor selection and startup load.
- [x] Remove all active source, build metadata, tests, and lasting documentation references.
- [x] Validate documentation and complete the full build/test gate.

#### Acceptance Gate

- No active code or lasting contract references `AssetBuildCore`; the full build
  succeeds and bounded asset/DDC test coverage passes.

## Validation Matrix

| Change | Validation |
| --- | --- |
| Public framework relocation and exports | Build `DerivedDataCache`, `TextureBuild`, and `GeometryBuild` |
| Cache/session behavior | Migrated five-case DerivedData Build Framework test target |
| Texture2D callback and authoring flow | Focused Texture build tests |
| Geometry recipe integration | Focused geometry/terrain build tests selected through the test registry |
| Module graph and deletion | Configure, then full `all` build |
| Lasting documentation | `doc validate --scope changed` and plan validation |

## Definition of Done

- `AssetBuildCore` no longer exists as a module or dependency.
- Generic derived-data Build Framework APIs are exported by `DerivedDataCache`.
- Texture2D owns its asynchronous request completion vocabulary.
- Recipe public APIs do not leak generic DDC framework types.
- Focused tests, documentation validation, configuration, and the full build pass.
- The completed plan and all task changes are committed together with exact plan
  and stage provenance.

## Deferred Follow-ups

- Asynchronous DDC requests, request owners, priorities, remote execution, and
  multi-value records require separate designs if future scale justifies them.
- A shared object-level per-request completion type belongs in `Engine` only if
  more than one compilation domain develops a real public consumer need.

## Related Documentation

- [Asset Data Lifecycle and Storage](../Runtime/Assets/AssetDataLifecycle.md)
- [Asset Compilation](../Runtime/Assets/AssetCompilation.md)
- [Async Asset Operations](../Editor/Architecture/AsyncAssetOperations.md)
- [Code Modules](../Workspace/CodeModules.md)
- [Workspace And Projects](../Workspace/WorkspaceProjects.md)
- [Build System](../Development/Build/BuildSystem.md)
- [Derived Data Cache Module Extraction](DerivedDataCacheModuleExtraction.md)

## Related Code

- [`DerivedDataCache`](../../Engine/Source/Developer/DerivedDataCache)
- [`TextureBuild`](../../Engine/Source/Developer/TextureBuild)
- [`StaticMeshBuild`](../../Engine/Source/Developer/StaticMeshBuild)
- [`SkeletalBuild`](../../Engine/Source/Developer/SkeletalBuild)
- [`DerivedDataBuildTests.cpp`](../../Engine/Tests/Native/EngineTests/Private/DerivedDataBuildTests.cpp)
