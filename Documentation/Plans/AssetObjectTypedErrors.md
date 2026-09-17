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
Stage 1 is in progress: Linker path resolution and detached canonical Map key
construction now return typed results, including requested/failed indices,
scalar and layout context, and owned nested field/array routes. Their consumers
are migrated without legacy overloads. Reader/Writer public APIs now return
`FPackageReaderResult` and `FPackageWriterResult` directly, with typed reasons,
owned context, and underlying envelope, Linker, Map-key, or Writer causes.
`DecodeValues` preserves a previously recorded inner value error. Engine and
Registry adapters still format explicitly into their pending contracts. The
live reflected Map key API is also pending; it is distinct from the detached
package API.
The remaining Stage 1 boundaries and Stage 2 are not complete.

Validation on `Win64-Debug-DurinEditor`: the final `all` build passed. The affected
selection ran 99 targets: 98 passed; `EditorAssetWorkflowTests` exposed the
unknown-mount presentation contract. Restoring its existing text in the formatter
made the failing case pass alone and the complete target pass (37 cases). The
other passing results are retained; no semantic producer changed in that fix.
The three added/updated path, formatting, and soft-reference regression cases
also passed independently with serial isolation. The original batch log is
`Build/.agent-state/logs/20260917-201524-328270-36276-ctest.log` (records the initial
presentation failure); successful follow-up logs are
`Build/.agent-state/logs/20260917-201825-108406-35536-EditorAssetWorkflowTests.log`
and `Build/.agent-state/logs/20260917-201827-879341-34228-ctest.log`.

Broad test compilation also exposed a pre-existing RHI migration omission in
`SceneViewportResourceTests`: the fixture now uses `IsSuccess()` instead of the
removed `bSucceeded` field. `ViewportTests` passed in the affected run.


Stage 1 Linker/Map-key validation: `PackageLinkerContractTests` passed all
17 cases; the affected selection passed all 56 targets. Five added/updated
error contract cases also passed in serial per-case isolation. Evidence:
`Build/.agent-state/logs/20260917-203718-063159-30180-ctest.log`,
`Build/.agent-state/logs/20260917-203851-479601-35360-ctest.log`.
The final `all` build passed; log:
`Build/.agent-state/logs/20260917-203903-451430-21904-cmake.log`.


Stage 1 Reader/Writer validation: all 20 `PackageWriterContractTests` cases,
57 affected targets, and four new cases in serial per-case isolation passed.
The final `all` build passed. Evidence:
`Build/.agent-state/logs/20260917-205512-417344-36600-ctest.log`,
`Build/.agent-state/logs/20260917-205704-224245-6736-ctest.log`, and
`Build/.agent-state/logs/20260917-205707-016812-5532-cmake.log`.
The current affected report is
`Build/NativeTestResults/Win64-Debug-DurinEditor/affected.xml`.


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

- [x] Return typed Linker path and detached canonical Map key results; retain
  owned failure context and unchanged output on failure.
- [x] Return typed Package Reader/Writer results, remove diagnostic outputs and
  Message fields, and preserve nested causes through canonical validation.
- [ ] Replace Capture, property snapshot/copy,
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
