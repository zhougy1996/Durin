# Texture Build Error Boundaries Plan

Summary: Separate recipe diagnostics, Engine recovery, and import-facing texture build outcomes.

Last reviewed: 2026-09-22

Status: Completed
Completed: 2026-09-22

## Current Status

Both stages completed. Cube/Volume operation errors are separate from recipe
failures, Texture2D diagnostic snapshots own build causes, and scene import uses
the detached operation boundary. Pure source preparation returns diagnostics.

Consumer inspection found two existing losses of terminal state: reimport folded
cancellation/supersession into build failure, and batch import discarded compilation
status. The selected implementation therefore preserves these states through
reimport and batch presentation, and prevents superseded batch cleanup from
unloading the newer operation's asset. Existing scheduling and publication effects
remain unchanged.

Validation on Win64-Debug-DurinEditor passed:

- `all` build, including Sandbox, RoadWeaver, and StudioCubeGenerate consumers
  (`20260922-121641-779731-31144-cmake.log`).
- TextureTests: 126; TextureImportWorkflowTests: 23; SceneImportTests: 20;
  AsyncTaskPilotLifecycleTests: 1; EditorAssetWorkflowTests: 40;
  AssetPackageTests: 189. All 399 tests passed.
- Regression coverage verifies minimal public provider failures, retained
  asynchronous causes, actionable invalid input, reimport cancellation and
  supersession, canceled batch accounting, and ownership of superseding builds.
  Existing cache recovery and partial-publication tests remain passing.
- Changed-document validation and all-plan validation passed; diff whitespace
  checks passed. No GPU behavior changed; GPU execution was not selected.

The affected-test resolver selected all tests because the changed program consumer
is outside a registered module. The explicit suite selection above instead covers
texture behavior, import consumers, lifecycle, editor reimport, and publication.
Lasting contracts are recorded in [asset lifecycle](../Runtime/Assets/AssetDataLifecycle.md#optional-asset-operation-boundaries)
and [asset import](../Editor/Architecture/AssetImportFramework.md#standalone-batch-admission).

## Goal

Keep failures at the lowest layer able to handle them. Pure recipes return
products or diagnostic failures without logging or UI policy. Engine owns cache
fallback, provider invocation, cancellation, application, and diagnostic recording.
Import operations translate final outcomes into user-facing explanations.

| Failure | Handling and diagnostic owner | Public outcome |
| --- | --- | --- |
| Invalid authored input | Engine retains a concise actionable reason | Invalid input with reason |
| Recipe/provider failure | Engine logs synchronous failures; retains async diagnostics | Failed without nested recipe cause |
| Source preparation failure | Pure preparation returns its reason; Engine reports it | Failed |
| Cache read/decode failure | Engine rebuilds; retains cache diagnostics | Success if rebuild succeeds |
| Cache store failure | Engine retains persistence diagnostics | Build success |
| Cancellation / supersession | Existing compilation coordinator | Explicit canceled / superseded status |
| Save / partial publication | Existing import publication owner | Existing publication result |

## Implementation Stages

### Stage 0: Define and implement error boundaries

- [x] Audit recipe, orchestration, and import consumers across workspace projects.
- [x] Separate Cube/Volume operation failures from recipe diagnostics and record details at the Engine boundary.
- [x] Separate Texture2D retained build diagnostics from completion errors; preserve cancellation and actionable input reasons.
- [x] Make source preparation return diagnostics without duplicate logging.
- [x] Migrate import consumers to operation-level messages and retain publication semantics.

Completion condition: ordinary recipe diagnostic chains no longer reach import UI.

### Stage 1: Validate and document the contract

Depends on Stage 0.

- [x] Add regression coverage for failure translation, actionable input, and diagnostic retention.
- [x] Run texture and import workflow tests, affected lifecycle consumers, and the required all build.
- [x] Update the runtime contract and validate changed documents and all plans.
- [x] Record evidence and include the completed work in a commit with plan/stage provenance.

Follow [build guidance](../Agents/BuildAndRun.md) and
[test guidance](../Agents/Testing.md). GPU execution is not required because
this work does not change GPU behavior.
