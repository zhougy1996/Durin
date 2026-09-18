# Asset and Object Typed Errors Plan

Summary: Replace text-based asset and object failure propagation with module-owned typed errors and presentation-boundary formatting.

Last reviewed: 2026-09-18

Status: Active
Completed:

## Current Status

Stage 0 is complete. Stage 1 has typed package codecs, Capture, property
snapshot/copy, save overrides, graph replacement, validation callbacks, and
memory graph save/load and duplication. Decoded soft-reference path causes now
survive Archive, snapshot, property-copy, and graph boundaries. Stage 1 remains
open for a final audit of framework adapters and consumers. Stage 2 has substantial implementation across
asset and Cook subsystems but remains open for end-to-end cause propagation and
removal of pending text adapters, including the outer asset result contract.
Unchecked items below are completion gates, not instructions to redo migrated
APIs. Audit current code before selecting the next batch.

The implementation baseline is `12beed739` (169 consolidated commits), followed
by `6d317f0b5` (Cook dependency discovery) and `d569de006` (memory graph save/load).
Original history remains on
`codex/backup-asset-typed-errors-before-squash-20260918`. Use Git history for
individual changes; this plan records current scope and acceptance only.

The current batch preserves CoreDObject capture, reader and writer causes across
Engine's DAST codec adapters, including ordinary writes and detached linker
mutations. The sole Capture consumer no longer requests an unused text output.
Codec consumers in package operations, relocation and reference rewriting
already propagate complete results; Sandbox and RoadWeaver have no direct
consumers of this private boundary. Callers keep their existing responsibility
to consume success, propagate failure, or own rollback; retaining a cause does
not require each caller to interpret it.

Both focused codec regressions passed. The affected run passed 94 of 95 targets
(`20260918-144928-588756-28636-ctest.log`); the remaining Factory diagnostic test
held a vector element reference across append/reallocation. Retaining a value
copy fixes that test, and all 60 EditorOperationTests cases passed on rerun
(`20260918-145216-454607-17356-EditorOperationTests.log`). Windows validation also
required missing Cook discovery/material transaction test-interface exports and
`/bigobj` for the existing large AssetPackageTests translation unit. The all
build passed (`20260918-145224-260017-35536-cmake.log`). The
outer `FAssetResult`, Engine-owned codec admission errors and linker-application
diagnostics remain explicit Stage 2 migration boundaries. The codec adapters
still format for that pending outer contract while retaining their full typed
causes. Stage 1 callback/property-edit review previously confirmed typed results
at the owning APIs; neither stage is closed by this batch.

Implemented contracts belong in [Serialization](../Runtime/Core/Serialization.md)
and [Asset data lifecycle](../Runtime/Assets/AssetDataLifecycle.md), with their
linked module contracts. Prior validation covers those batches; it does not
establish completion of the remaining stages.

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

## Execution Policy

1. Audit one remaining stage boundary and list its producers, adapters,
   consumers, and semantic tests across the workspace. Distinguish completed
   migration from unresolved work before editing.
2. Migrate a complete interface or cohesive module boundary in one batch,
   including all failure branches and consumers. Split only for an independent
   rollback boundary, a distinct risk, or a failure requiring focused diagnosis.
3. Run focused checks while developing when needed. Once the batch is ready,
   run affected tests, then the required all build, as its acceptance gate;
   reuse passing evidence while relevant inputs remain unchanged.
4. Review and commit the validated batch with the existing Plan and Stage
   trailers. Before starting another batch, update the stage checklist and
   replace the current status/evidence summary rather than appending a journal.

Temporary adapters must retain an explicit remaining-work boundary. Do not
expand the plan into a list of individual error branches or treat a small
commit as completion of a stage. Commit size follows the complete reviewable
change; neither a fixed commit count nor one commit per error is required.

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
- [x] Return typed live reflected Map key validation/token results, migrate
  workspace consumers, and cover owned nested context and failed-output behavior.
- [x] Return typed Package Reader/Writer results, remove diagnostic outputs and
  Message fields, and preserve nested causes through canonical validation.
- [x] Replace Capture, property snapshot/copy,
  save overrides, and graph replacement text with typed causes and context.
- [x] Preserve decoded soft-reference path causes through Archive, snapshot/copy,
  and graph results without expanding serializer caller responsibilities.
- [ ] Close remaining object-graph and property-edit boundaries, beginning with
  an audit of migrated callbacks and material adapters for lost causes while
  preserving publication and rollback contracts.
- [ ] Migrate all consumers and semantic tests; pass affected tests and all build.

### Stage 2: Preserve asset operation causes

- [x] Type graph preparation and reload diagnostics, retaining resource,
  replacement, material and asset causes through asynchronous completion and UI.
- [x] Preserve owned CoreDObject capture, reader and writer causes through the
  Engine DAST codec adapters without expanding caller error-handling duties.

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
guidance. At each completed boundary, verify typed codes and owned context,
underlying cause retention, and applicable publication, rollback, recovery, and
asynchronous completion behavior. Audit shared API consumers across Engine,
Sandbox, and RoadWeaver; shared Engine API changes require an all build.

Close a stage only after its remaining adapters and consumers have been audited
and its acceptance gates pass. Close the plan only after all stages pass and
implemented contracts are documented. Documentation-only maintenance uses
changed-document validation and does not repeat native builds or tests.
