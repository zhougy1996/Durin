# Texture2D Owner Async Bridge Plan

Summary: Bridge Texture2D compilation owners to asynchronous derived-data sessions without retaining a waiting worker.

Last reviewed: 2026-10-12

Status: Completed
Completed: 2026-10-12

## Current Status

All stages are complete. Texture2D owner dispatch releases workers during cache
I/O while retaining admission budgets through build callback retirement. Source
validation and shared-output assembly are common to synchronous and asynchronous
consumers. Lasting behavior is documented in
[Asset Compilation](../Runtime/Assets/AssetCompilation.md#initial-compiling-managers)
and [Derived Data Build](../Runtime/Assets/DerivedDataBuild.md#family-boundaries).

Validation on `Win64-Debug-DurinEditor`:

- `test TextureTests --report`: 142 cases passed after the final source change.
- `test AsyncTaskPilotLifecycleTests --report`: passed with two Core workers;
  proves overlapping slow reads, worker availability, cancellation during I/O,
  slow persistence, callback retirement, callback exception delivery, selected
  finish and saturated shutdown with an accepted asynchronous request.
- `test AssetCompilingManagerTests --report`: passed.
- `test "@module=derived-data-cache" --report`: all five targets passed, including
  DerivedDataBuildTests, DerivedDataCacheTests and shader cache/build consumers.
  The lifecycle target was rerun after its final exception regression.
- Final `build --target all` passed; build log:
  `Build/.agent-state/logs/20261012-021117-824332-34272-cmake.log`.
- Changed documentation and all-plan validation passed. Diff whitespace checks passed.

The affected selector expands broadly because of Engine ownership and the test
support header; the bounded CPU suites above cover the changed semantics while
the all build covers project and test compilation. GPU behavior is unchanged.

## Goal

Migrate Texture2D compilation requests, including authored PostLoad and editor
rebuilds, to asynchronous cache I/O and producer continuations. Retain synchronous
detached/Cook APIs and the existing TextureCube platform-cache route for this
bounded migration. Cube requires its own detached asynchronous platform-cache
protocol before migration; StaticMesh, physics and ShaderBuild are outside scope.

## Selected Decisions

- Keep a separate persistent task-scheduled texture session for asynchronous requests.
- Each admitted owner retains a build request owner until dispatch, build callback
  and payload publication have all completed. Never wait for it inside an asset worker.
- Keep concurrency and estimated bytes reserved across I/O, producer execution and
  result assembly. Keep pending capacity reserved until GameThread delivery returns.
- Cancellation combines the asset flag and owner task-scope cancellation; shutdown
  drains asynchronous requests before provider unload. Preserve exactly-once delivery,
  latest-wins serial checks and selected finish semantics.
- Share Texture2D validation and output assembly between synchronous and asynchronous
  consumption; forward owner priority to the build session. No wire/key change.

## Implementation Stages

### Stage 0: Record the bridge contract

- [x] Inspect owner scheduling, mailbox and shared build session boundaries.
- [x] Save and validate the selected plan before implementation.

### Stage 1: Bridge Texture2D owner execution

- [x] Add a detached asynchronous Texture2D build adapter and persistent task session.
- [x] Separate owner dispatch readiness from terminal build readiness and retain budgets.
- [x] Preserve cancellation, rejection, callback lifetime and shutdown drain semantics.
- [x] Document the implemented owner boundary and migration scope.

### Stage 2: Validate and deliver

- [x] Add regressions proving worker release, overlapping owners and terminal lifecycle.
- [x] Pass Texture tests, relevant derived-data tests and the all build according to
  [build](../Agents/BuildAndRun.md) and [testing](../Agents/Testing.md) guidance.
- [x] Validate changed documentation and all plans, record evidence and commit.
