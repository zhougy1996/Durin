# Material Compilation Ownership Plan

Summary: Consolidate material build ownership and cache policy while separating asset publication from detached compilation.

Last reviewed: 2026-10-20

Status: Archived
Completed: 2026-10-20

## Current Status

Both stages are complete. The private compilation service owns subscriptions,
flights share the operation lifetime, and all callers use the operation-owned
program cache policy. Baseline: `3e9c43a5bae0c15e6d36a7688c1ee1201e2117c0`
plus the implementation in this commit. Final tested source fingerprint:
`2f9606b45d7a12b3e6e134f05673aed261af90a8b9af47db9110a8c7ffd8111c`;
the file inventory is retained in `Build/MaterialCompilationOwnership/sources.json`.
Subsequent changes are documentation only.

Validation adjustment: the unchanged Metal round-trip fixture constructs MSL
source for a Game family, while the unchanged codec requires `metal-library-1`.
The failure reproduces in isolation. Update only that fixture to structurally
valid library bytes, keep explicit source-profile rejection, and retain binding
metadata round-trip/corruption checks. Native Metal execution remains outside
this CPU codec test.

Validation on `windows-msvc-x64`, `Win64-Debug-DurinEditor`:

- `test MaterialCompilerTests --report`: 95/95 passed. Full output:
  `Build/.agent-state/logs/20261020-120405-459000-26072-MaterialCompilerTests.log`.
- `test MaterialCompilerTests 'FMaterialProgramOwnershipTests.*' --isolate --test-jobs 1 --report`:
  3/3 passed independently, including both new regression cases. Receipt:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/MaterialCompilerTests.xml`.
- `test MaterialCompileLifecycleTests --report`: 18/18 passed after the fixture
  correction. Receipt: `Build/NativeTestResults/Win64-Debug-DurinEditor/MaterialCompileLifecycleTests.xml`.
- `test '@domain=material-compilation' --report`: shutdown target passed (1 case).
  Its result remains applicable; the lifecycle fixture correction does not alter
  that target or production sources. The initial combined run's lifecycle failure
  is superseded by the passing standalone run above. Raw receipt:
  `Build/.agent-state/logs/20261020-120500-895579-30024-ctest.log`.
- `test MaterialCookTests --report`: 7/7 passed. Receipt:
  `Build/NativeTestResults/Win64-Debug-DurinEditor/MaterialCookTests.xml`.
- `build --preset Win64-Debug-DurinGame Engine`: passed. Receipt:
  `Build/.agent-state/logs/20261020-121022-994619-29176-cmake.log`.

`test affected --explain` expands Engine ownership to unrelated asset, world,
physics and GPU targets. The bounded selections above exercise all changed
caller paths, consumer source mapping, sharing, force, cancellation, fanout,
shutdown and Cook. No shared public API or persistent schema changed. Game
compilation covers the extracted service and non-editor stubs; GPU execution
and performance qualification are unnecessary for this behavior-preserving
ownership refactor. The first sandbox build hit MSVC C1902; all successful
native validation used the authorized unsandboxed environment.

Changed-document validation, all-plan lifecycle validation and final diff review
passed. The lasting ownership contract is recorded in
[Material System](../../../Runtime/Rendering/Materials/MaterialSystem.md#compile-lifecycle-and-cooked-programs).

## Goal

Keep the asset-compilation aggregate and ShaderBuild boundaries. Extract material
request/subscription orchestration into a private compilation service, leaving
live-object capture, edit policy, and publication in the lifecycle implementation.
Use one build object for each flight and one operation-owned program reuse and
retention policy across asset, synchronous, and Cook callers. Weak owner keys in
subscription records remain opaque to worker code. Preserve input/semantic
deduplication, consumer-local diagnostics, force behavior, cancellation, resource
accounting, and the distinction between result readiness and request retirement.

Shader submission order, persistent schemas, pass selection, public APIs, and
renderer behavior are unchanged. Bounded parallel Shader submission and finer
Pass invalidation are separate performance work.

## Implementation Stages

### Stage 0: Consolidate build ownership and isolate compilation orchestration

- [x] Extract the request service from material object lifecycle code.
- [x] Consolidate flight and operation lifetime; remove duplicated cancellation
  and task identity without weakening callback/drain boundaries.
- [x] Route all callers through operation-owned program lookup and retention;
  keep per-consumer rebinding and publication separate.

Completion: asset lifecycle delegates compilation to the private service; inline,
task, and Cook builds use one cache policy and preserve existing public contracts.

### Stage 1: Validate lifecycle behavior and document the ownership contract

Depends on Stage 0.

- [x] Add focused regression coverage for shared cache policy and cancellation
  before start, retaining existing cancellation/fanout/shutdown tests.
- [x] Validate compiler, lifecycle, shutdown, and Cook targets using the
  [test workflow](../../../Agents/Testing.md); these cover changed ownership and all
  production caller paths. Inspect affected coverage and bound any additional
  consumer checks using the [build workflow](../../../Agents/BuildAndRun.md).
- [x] Build the Engine target with the Debug Game preset: the extracted service
  and changed non-editor operation stubs must not introduce authoring/DDC
  dependencies into Game.
- [x] Update the material runtime contract, validate changed documents and all
  plan lifecycles, and review the final diff.

Completion: record exact selections, tested revision/worktree, preset, results,
and any remaining limitations. No timing or GPU behavior claim is required.
