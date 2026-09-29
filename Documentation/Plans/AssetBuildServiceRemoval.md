# Asset Build Service Removal Plan

Summary: Align asset derived-data build ownership with UE by moving the production registry into DDC and sessions into asset-family implementations, then deleting the Engine integration service.

Last reviewed: 2026-09-30

Status: Completed
Completed: 2026-09-30

## Current Status

All implementation stages are complete. DDC owns the production build and per-session registry snapshots; Texture, StaticMesh, and physics own their registration/session boundaries; the integration service and lifecycle calls are gone.

Validation evidence:

- `./DevTool test DerivedDataBuildTests`: 30 tests passed.
- `./DevTool test StaticMeshTests FStaticMeshDerivedDataCacheTests.BuildBoundariesTranslateModuleFailureAndCancellation`: 1 test passed.
- `./DevTool test DerivedDataTextureQualificationTests --mode qualification`: passed.
- `./DevTool test affected --test-jobs 4 --report`: passed; affected analysis resolved to all non-qualification/non-characterization Native targets.
- `./DevTool build --target all --output compact`: passed.
- `./DevTool doc validate --scope changed` and `./DevTool doc plan validate --scope all`: passed before completion; repeated after the final lifecycle update.

## Goal

Make DDC own the process-wide production build registry and execution service. Texture, static-mesh, and physics derived-data code register their own build functions and own the sessions through which they submit work. Engine startup, programs, and tests must no longer initialize or shut down an `AssetBuildService`.

## Selected Boundaries

- `DerivedDataCache` owns the production `IBuild`, its function registry, and session creation.
- A registry snapshot belongs to one session. Registering another function affects later sessions without mutating existing sessions.
- Each asset family registers its build functions and lazily owns its session; no cross-family Engine service coordinates them.
- Build functions resolve the current producer module when executing so module test substitution does not require rebuilding the DDC service.
- `CreateBuild` remains available only as an isolated build-system constructor for tests and specialized callers.
- `AssetBuildService` and its public/private APIs are removed rather than deprecated.

## Implementation Stages

### Stage 1: Move production registry ownership to DDC

- [x] Add a process-wide production `IBuild` accessor owned by `DerivedDataCache`.
- [x] Snapshot the registry independently for each new session and allow registration while the production build remains open.
- [x] Extend native coverage for registration followed by multiple session snapshots.

Completion condition: DDC can accept family registration throughout module startup while existing sessions retain immutable registries.

### Stage 2: Move sessions to family owners

- [x] Register texture and static-mesh build functions from their producer-module startup paths.
- [x] Give texture, static-mesh, and physics code family-local lazy sessions and submit requests directly.
- [x] Resolve producer modules at execution time where tests replace module implementations.

Completion condition: all asset build entry points execute through family-owned sessions without `AssetBuildPrivate::Build`.

### Stage 3: Remove the integration service and validate

- [x] Delete `AssetBuildService` sources and migrate Engine, programs, test fixtures, and all other consumers.
- [x] Update lasting architecture documentation and complete changed-document and plan validation.
- [x] Run focused native tests, affected project builds, and the required shared-Engine `all` build.

Completion condition: repository search finds no `AssetBuildService` API or bridge, validation passes, and the plan records exact evidence before being marked completed.
