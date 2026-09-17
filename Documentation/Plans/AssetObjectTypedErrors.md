# Asset and Object Typed Errors Plan

Summary: Replace text-based asset and object failure propagation with module-owned typed errors and presentation-boundary formatting.

Last reviewed: 2026-09-17

Status: Active
Completed:

## Current Status

Stage 0 is complete and validated. Path factories
and soft-reference setters return `FObjectOperationResult`, retain typed causes
and owned context, and have no legacy error-output overloads. Consumers across
all three workspace projects have been inspected and migrated where needed.
Stages 1 and 2 remain pending; their old framework contracts still receive
explicitly formatted diagnostics at adapters.

Validation on `Win64-Debug-DurinEditor`: the final `all` build passed. The affected
selection ran 99 targets: 98 passed; `EditorAssetWorkflowTests` exposed the
unknown-mount presentation contract. Restoring its existing text in the formatter
made the failing case pass alone and the complete target pass (37 cases). The
other passing results are retained; no semantic producer changed in that fix.
The three added/updated path, formatting, and soft-reference regression cases
also passed independently with serial isolation. The batch report is
`Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml` (records the initial
presentation failure); successful follow-up logs are
`Build/.agent-state/logs/20260917-201825-108406-35536-EditorAssetWorkflowTests.log`
and `Build/.agent-state/logs/20260917-201827-879341-34228-ctest.log`.

Broad test compilation also exposed a pre-existing RHI migration omission in
`SceneViewportResourceTests`: the fixture now uses `IsSuccess()` instead of the
removed `bSucceeded` field. `ViewportTests` passed in the affected run.


## Goal

Keep engine-owned error prose out of validation and lifecycle state, preserve
underlying causes across module boundaries, and assert codes/context in semantic
tests. Preserve dependency direction and existing output-publication, rollback,
recovery, and asynchronous completion contracts.

## Selected Boundaries

Each owning module defines its error domains. Results derive success from their
typed error rather than a second success flag. Data output parameters remain
where appropriate. Formatting belongs at presentation boundaries. During staged
migration, adapters to unmigrated framework contracts format explicitly at those
boundaries; no string overload remains on a migrated API. External provider text
requires an explicit bounded external-diagnostic contract.

## Implementation Stages

### Stage 0: Type path and soft-reference failures

- [x] Return typed path and soft-reference results with owned context, retaining
  mount error classification and exact expected/actual identities.
- [x] Migrate consumers across Engine, Sandbox, and RoadWeaver without legacy
  overloads; preserve failed-output and reference-cache behavior.
- [x] Add semantic regression coverage, document the boundary, and pass affected
  tests and an all build before committing.

### Stage 1: Type package codec and object graph failures

- [ ] Replace Package Reader/Writer, Linker, Capture, property snapshot/copy,
  save overrides, and graph replacement text with typed causes and context.
- [ ] Migrate graph-load and property-edit framework callbacks, including material
  adapters, while preserving publication and rollback contracts.
- [ ] Migrate all consumers and semantic tests; pass affected tests and all build.

### Stage 2: Preserve asset operation causes

- [ ] Migrate Registry, package resources, BulkData, load/save, Cook, reload,
  compilation, and import metadata validation results.
- [ ] Preserve underlying typed causes through Engine asset results and keep
  disposition, transaction identity, affected files, and recovery state separate.
- [ ] Move formatting to editor, log, command, and explicit external boundaries;
  migrate consumers and text-dependent semantic tests.
- [ ] Pass affected tests and all build; document implemented contracts and close
  the plan only after all stages pass.

## Validation

Follow [testing](../Agents/Testing.md) and [build](../Agents/BuildAndRun.md)
guidance. Every stage requires isolated review and commit with its own evidence.
