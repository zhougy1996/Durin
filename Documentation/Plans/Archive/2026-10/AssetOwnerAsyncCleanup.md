# Asset Owner Async Cleanup Plan

Summary: Clarify asynchronous owner metrics, remove Texture transition branches, and harden lifecycle test cleanup.

Last reviewed: 2026-10-12

Status: Archived
Completed: 2026-10-12

## Current Status

All stages are complete. Texture reports `BuildElapsedNanoseconds`, including
asynchronous I/O wall time, and StaticMesh reports `AdmittedBuildCount`. All
workspace consumers are migrated. Texture workers require a build owner; queued
cancellation and dropped-dispatch readiness remain supported. Shared held-cache,
bounded-wait and drain helpers replace repeated lifecycle fixtures. Callback-referenced local state
now outlives the drain guards on both normal and early scope exit, including
terminal shutdown. Lasting metric semantics are recorded in
[Asset Compilation](../../../Runtime/Assets/AssetCompilation.md).

Validation on `Win64-Debug-DurinEditor`:

- `test TextureTests --report`: all 142 cases passed.
- `test StaticMeshTests --report`: all 169 cases passed.
- Final `test AsyncTaskPilotLifecycleTests --report`: passed with two Core workers,
  exercising slow I/O, cancellation, callback retirement and combined shutdown.
- `test AssetCompilingManagerTests --report`: passed.
- `build --target all`: passed, including all workspace projects and native targets.
  Log: `Build/.agent-state/logs/20261012-045001-878081-44260-cmake.log`.
- Source/test searches across Engine, Sandbox and RoadWeaver found no remaining
  uses of the renamed fields or the old Texture-specific fake-cache name.
- Changed-document validation, all-plan validation and diff whitespace checks passed.

This batch changes diagnostic source names and private/test structure, preserving
cache identities and asset publication behavior. GPU execution was not needed.
Session consolidation, StaticMesh state-machine refactoring and historical plan
archival remain separate follow-up work.

## Goal

Make elapsed-time and admitted-work diagnostics accurate in name, simplify the
established asynchronous Texture worker route, and guarantee test callbacks drain
before their referenced local state is destroyed.

## Implementation Stages

### Stage 0: Record cleanup boundaries

- [x] Inspect diagnostic consumers and asynchronous worker/test boundaries.
- [x] Save and validate this plan before source changes.

### Stage 1: Clean metrics and owner tests

- [x] Rename Texture worker wall time to build elapsed time and StaticMesh running
  worker count to admitted build count; migrate all workspace consumers.
- [x] Remove worker-only nullable-owner branches while retaining queued cancellation
  and dropped-dispatch readiness paths.
- [x] Share held-cache, bounded wait and drain helpers; order guards after all
  callback state, including terminal shutdown assertions.
- [x] Update lasting metric contracts.

### Stage 2: Validate and deliver

- [x] Pass Texture, StaticMesh, lifecycle and asset-manager tests following
  [testing](../../../Agents/Testing.md) guidance.
- [x] Complete the shared Engine API all-build gate following
  [build](../../../Agents/BuildAndRun.md) guidance.
- [x] Validate documentation and all plans, record evidence and commit.
