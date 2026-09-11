# Setter Error Boundaries Plan

Summary: Separate infallible state mutation from validation, lookup, and fallible asset preparation.

Last reviewed: 2026-09-11

Status: Active
Completed:

## Current Status

Stage 1 is complete: PhysicsSceneTests passed and StaticMeshTests passed all 109 tests.
A separate Renderer prerequisite fix removed a conflicting local Pi constant; Renderer rebuilt successfully.
Stage 2 is next. The initial checkout was clean.

## Goal

Use void setters for validated internal state. Preserve recoverable diagnostics at
input and build boundaries, including failure-before-publication behavior.

## Implementation Stages

### Stage 1: Simplify BodySetup enum setters

- [x] Make source mode and query policy setters void with explicit enum contracts; remove redundant caller branches.
- [x] Validate collision and static-mesh coverage and commit independently.

### Stage 2: Separate import state validation

Depends on Stage 1.

- [ ] Prepare and validate import states before void setters, including derived import data and callers.
- [ ] Validate import state and import workflow coverage and commit independently.

### Stage 3: Separate material slot lookup

Depends on Stage 2.

- [ ] Resolve imported material slots at the import boundary and use an infallible indexed setter.
- [ ] Validate material binding success and failure and commit independently.

### Stage 4: Clarify StaticMesh preparation and publication

Depends on Stage 3.

- [ ] Give collision and render-data replacement operations explicit fallible names; keep preparation before publication and document their contracts.
- [ ] Validate collision/render replacement coverage and commit independently.

### Stage 5: Enforce compiler manager startup contracts

Depends on Stage 4.

- [ ] Convert manager startup to void if callers confirm failures are lifecycle violations; retain compiler-provider startup failures.
- [ ] Validate manager lifecycle coverage, complete an all build for the public API migration, and commit independently.

## Validation

Follow [agent testing](../Agents/Testing.md) and [build guidance](../Agents/BuildAndRun.md).
Reuse passing coverage when its inputs are unchanged. Validate changed documentation
and all plans when adding or completing this plan.
