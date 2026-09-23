# Static Mesh Build Module Boundary Plan

Summary: Replace StaticMesh render feature discovery with an explicit module contract and move the algorithm builder into StaticMeshBuild.

Last reviewed: 2026-09-23

Status: Completed
Completed: 2026-09-23

## Current Status

Completed. Engine now owns named orchestration functions and Developer/StaticMeshBuild owns FStaticMeshBuilder behind IStaticMeshBuildModule. All source/test roots declared by Durin.dworkspace were searched and migrated; no old provider, operations class, or BuildRenderRecipe references remain in production code.

Validation on Win64-Debug-DurinEditor: the final `all` build passed (2026-09-23, build log `20260923-110347-485854-18364-cmake.log`). StaticMeshTests (143), SceneImportTests (20), AssetCookTests (39), CookedMeshLoadingTests (4), PhysicsSceneTests (44), SplineTests (48), and ViewportTests (89) passed: 387 cases. The new worker/session lifetime case also passed alone. Geometry algorithm comparison confirmed symbol/header-only changes, apart from a clarified comment; builder version 4 and payload formats remain unchanged. Changed-document and all-plan validation passed.

Initial validation caught one mechanical test rename and two unload fixtures with unrelated pending compilation. The rename was corrected and the fixtures now drain compilation before establishing their unload safe point. Final cleanup after these test runs changed only comments/formatting and documentation; the final all build includes that cleanup. Application-hosted and GPU execution were not run because this change affects CPU construction and module lifetime.

The module manager rejects shutdown while code leases exist. Build sessions acquire on the module-control thread before dispatch and retain the exact implementation through worker completion/publication. Consumers must stop admission, finish or cancel work, and release sessions before unloading. No Core registry or module-manager changes were needed.

## Goal

Engine retains source acquisition, derived-data caching, validation, scheduling, and publication. Developer/StaticMeshBuild owns FStaticMeshBuilder and pure CPU geometry construction. IStaticMeshBuildModule is an Engine-declared module contract with shared value types, avoiding a reverse link dependency. Remove IStaticMeshBuildProvider and FStaticMeshBuildOperations without changing algorithm versions, DDC keys, or payload formats. Physics cooking remains independent.

## Implementation Stages

### Stage 0: Migrate the build boundary

- [x] Split shared build types from the module interface and expose a retained module session.
- [x] Move the algorithm to Developer FStaticMeshBuilder and replace Engine's builder class with named orchestration functions.
- [x] Migrate compilation, cook, import, and test consumers across all workspace projects.
- [x] Remove feature registration and provider-generation checks; retain module sessions through work and publication.

Completion requires no production references to the old provider or operations class and explicit lifetime ownership at worker dispatch sites.

### Stage 1: Validate and document the contract

- [x] Adapt regression tests for module absence, retained sessions, cancellation, CPU products, and cache behavior.
- [x] Run relevant CPU native targets (StaticMeshTests, SceneImportTests, AssetCookTests, CookedMeshLoadingTests, PhysicsSceneTests, SplineTests, and ViewportTests) and the required all build, following [testing](../Agents/Testing.md) and [build guidance](../Agents/BuildAndRun.md).
- [x] Update owning runtime/module documentation and pass changed-document and all-plan validation.
- [x] Review the diff and commit the completed migration with plan provenance.

Affected-test discovery selects nearly every Engine target, including GPU integrations, because the shared Engine API changed. The bounded CPU selections above exercise the changed build/cook/import boundaries; the all build covers remaining consumers. Stage 1 depends on Stage 0. Completion requires passing validation and recorded evidence; application-hosted and GPU execution are outside this CPU/module-boundary change.
