# Material Command Result Simplification Plan

Summary: Simplify MaterialEditor command outcomes around caller decisions and presentation diagnostics.

Last reviewed: 2026-09-19

Status: Completed
Completed: 2026-09-19

## Current Status

Completed. Commands expose status, owned messages, changed IDs and material graph
diagnostics. Six editor error taxonomies, their context bags and formatter switches,
two forwarding result types, and recursive cleanup-result ownership are removed.
All consumers across the declared workspace projects were searched and migrated.

Validation passed on `windows-msvc-x64` / `Win64-Debug-DurinEditor`:

- `MaterialGraphEditingTests`: 98 tests passed during focused validation.
- `@domain=material-editing`: graph editing, editor interaction and persistence
  targets all passed after the final implementation and regression updates.
- `build --target all`: passed, including remaining editor consumers.
- Changed-document validation, all-plan validation and `git diff --check`: passed.

The affected-test resolver selects the broad Engine project set for these native
test edits. The material-editing domain covers the changed behavior; the all build
checks integration without unrelated runtime/GPU execution. UI application smoke
was not required or run. The isolated change includes this completed plan and
stage provenance in its commit.

## Goal

Expose one command status, owned failure text, separate cleanup failure text,
changed IDs and existing node/port-qualified material diagnostics. Remove the
six public cause slots and recursive command-result ownership. Keep Engine errors
typed and preserve transaction rollback, retry, stale-owner and no-change behavior.
Editor-only rejection formatting is an explicit presentation boundary; private
builders may describe local errors without exposing their implementation taxonomy.
Do not introduce a generic result framework or change compiler lifecycle contracts.

## Implementation Stages

### Stage 0: Simplify command outcomes

- [x] Audit consumers across Engine, Sandbox and RoadWeaver.
- [x] Replace public cause chains and disposition with caller-facing status and diagnostics.
- [x] Flatten cleanup failures and simplify internal forwarding where no information is added.
- [x] Migrate consumers and regression tests to observable command behavior.

Completion requires no consumers of the retired public error slots and preserved
failure messages, material diagnostic locations and recovery behavior.

### Stage 1: Validate and document the contract

Depends on Stage 0.

- [x] Update the material graph operations contract.
- [x] Run material-editing regression coverage and compile affected project targets.
- [x] Validate changed documents and all plan lifecycle metadata.
- [x] Review and commit the isolated change with plan and stage provenance.

Use the [build workflow](../Agents/BuildAndRun.md),
[testing workflow](../Agents/Testing.md) and
[documentation workflow](../Agents/Documentation.md).
