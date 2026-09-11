# Setter Error Boundaries Plan

Summary: Separate infallible state mutation from validation, lookup, and fallible asset preparation.

Last reviewed: 2026-09-11

Status: Archived
Completed: 2026-09-11

## Current Status

Stage 1 is complete: PhysicsSceneTests passed and StaticMeshTests passed all 109 tests.
A separate Renderer prerequisite fix removed a conflicting local Pi constant; Renderer rebuilt successfully.
Stage 2 is complete: AssetImportDataTests (3), AssetImportTests (14), SceneImportTests (4),
and the single-asset/volume import selection in TextureTests (10) passed.
Stage 3 is complete: material/update coverage in StaticMeshTests (14) and SceneImportTests (4)
passed; StaticMeshThumbnailTests compiled after fixture migration.
Stage 4 is complete: StaticMeshTests (110) and PhysicsSceneTests (43) passed, including
invalid-input preservation and failed collision-build rollback.
Stage 5 is complete: AssetCompilingManagerTests passed, including idempotent startup
and recoverable provider startup failure. MaterialTests passed seven selected
lighting/preview cases. The default all build passed. MaterialVulkanTests and
StaticMeshRenderPreparationVulkanTests compiled; GPU execution was not needed
for this API migration.

The requested RoadWeaver follow-up was committed independently: deferred spline
setters now precede explicit UpdateMesh, and the test fixture uses FStaticMeshSource.
RoadWeaverEditor built and RoadSceneIntegrationTests passed all five tests.
The initial checkout was clean.

## Goal

Use void setters for validated internal state. Preserve recoverable diagnostics at
input and build boundaries, including failure-before-publication behavior.

## Implementation Stages

### Stage 1: Simplify BodySetup enum setters

- [x] Make source mode and query policy setters void with explicit enum contracts; remove redundant caller branches.
- [x] Validate collision and static-mesh coverage and commit independently.

### Stage 2: Separate import state validation

Depends on Stage 1.

- [x] Prepare and validate import states before void setters, including derived import data and callers.
- [x] Validate import state and import workflow coverage and commit independently.

### Stage 3: Separate material slot lookup

Depends on Stage 2.

- [x] Resolve imported material slots at the import boundary and use an infallible indexed setter.
- [x] Validate material binding success and failure and commit independently.

### Stage 4: Clarify StaticMesh preparation and publication

Depends on Stage 3.

- [x] Give collision and render-data replacement operations explicit fallible names; keep preparation before publication and document their contracts.
- [x] Validate collision/render replacement coverage and commit independently.

### Stage 5: Enforce compiler manager startup contracts

Depends on Stage 4.

- [x] Convert manager startup to void if callers confirm failures are lifecycle violations; retain compiler-provider startup failures.
- [x] Validate manager lifecycle coverage, complete an all build for the public API migration, and commit independently.

## Validation

Follow [agent testing](../../../Agents/Testing.md) and [build guidance](../../../Agents/BuildAndRun.md).
Reuse passing coverage when its inputs are unchanged. Validate changed documentation
and all plans when adding or completing this plan.
